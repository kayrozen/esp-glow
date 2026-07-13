// render_task.cpp — the render loop.
//
// Each iteration:
//   1. read esp_timer_get_time() (monotonic us)
//   2. glow::paceNextFrame() -> (nowSec, sleepUs, nextDeadline)  [host-tested]
//   3. show->renderFrame(nowSec)
//   4. post_render(remaining slack) — F6: the Lua VM's only chance to run
//      bounded GC steps (see render_task.h). The clock is re-read after
//      this call, specifically because step 4 can take a variable, non-
//      negligible slice of the slack: computing the sleep from the STALE
//      pre-hook `sleepUs` would double-count that time on top of the
//      hook's own work and slow the render rate whenever the GC actually
//      has work to do. Re-measuring keeps the frame's deadline authoritative
//      regardless of how much of step 4 actually ran.
//   5. vTaskDelay(remaining) — 0 if behind (or no slack left), so we
//      immediately re-loop
//
// Pinned to core 1; WiFi/lwIP live on core 0 (sdkconfig) so DMX timing on
// core 1 is never preempted by network work.
#include "render_task.h"
#include "render_pacing.h"

#include "esp_timer.h"
#include "esp_log.h"
#include "esp_task_wdt.h"

static const char* TAG = "render_task";

static TaskHandle_t s_task = nullptr;
static Show*        s_show = nullptr;
static uint32_t     s_periodUs = 0;
static volatile bool s_behind = false;
static render_pre_render_fn s_pre_render = nullptr;
static void*               s_pre_render_ctx = nullptr;
static render_post_render_fn s_post_render = nullptr;
static void*                 s_post_render_ctx = nullptr;

// Cumulative counters for render_task_get_and_reset_stats(). Written only
// from render_loop (this task); read/reset only from a post_render hook,
// which also runs on this task (see main.cpp's render_tick_hooks) -- so
// this is same-task, not cross-task, and needs no synchronization.
static uint32_t s_statFrames = 0;
static uint32_t s_statBehind = 0;
static uint32_t s_statDropped = 0;

static void render_loop(void*) {
  ESP_LOGI(TAG, "render loop started on core %d (period=%lu us%s%s)",
           xPortGetCoreID(), (unsigned long)s_periodUs,
           s_pre_render ? ", pre_render=on" : "",
           s_post_render ? ", post_render=on" : "");

  // F5: subscribe THIS task to the Task WDT (see sdkconfig.defaults'
  // CONFIG_ESP_TASK_WDT*). If the render loop ever hangs -- a runaway
  // native (C++) effect, a driver stall inside show->renderFrame -- the
  // watchdog reboots the board instead of leaving a frozen rig on stage.
  // This is a DIFFERENT backstop from the Lua instruction-count hook
  // (lua_vm.h's LuaVM instruction budget): that one bounds *scripted*
  // runtime and disables the offending effect without ever reaching this
  // watchdog; this one exists for hangs the Lua layer has no way to see,
  // so removing either thinking the other covers it would reopen a gap.
  // Fed once per frame from render_tick_hooks' post phase (main.cpp), not
  // from here -- the reset has to happen after a full frame actually
  // completes, which this function has no visibility into.
  //
  // Deliberately NOT done for midi_uart_task/osc_server_task/the httpd
  // worker (see main.cpp): a blocked socket waiting on a peer is normal,
  // not a fatal condition, and subscribing a task that can legitimately
  // block for a while would turn ordinary network stalls into reboots.
  esp_err_t wdtErr = esp_task_wdt_add(nullptr);
  if (wdtErr != ESP_OK) {
    ESP_LOGW(TAG, "esp_task_wdt_add failed (%s); render task will not be "
                  "watchdog-protected this boot", esp_err_to_name(wdtErr));
  }

  uint64_t prevDeadline = 0;  // 0 => paceNextFrame seeds from nowUs
  uint32_t frames = 0;
  uint32_t behindCount = 0;
  uint64_t lastReport = esp_timer_get_time();

  while (true) {
    uint64_t now = esp_timer_get_time();
    glow::PaceResult r = glow::paceNextFrame(s_periodUs, now, prevDeadline);
    s_behind = r.behind;
    prevDeadline = r.nextDeadlineUs;

    if (s_show) {
      // F2: let the caller populate Raw universes before the render+flush.
      if (s_pre_render) s_pre_render(s_pre_render_ctx, r.nowSec, s_show);
      s_show->renderFrame(r.nowSec);
    }

    // F6: spend whatever slack is actually left (not the pre-render
    // estimate) on bounded GC work, then re-measure before deciding how
    // long to sleep -- see the file header comment for why.
    uint64_t afterRender = esp_timer_get_time();
    int64_t slackUs = r.behind ? 0 : (int64_t)r.nextDeadlineUs - (int64_t)afterRender;
    if (slackUs < 0) slackUs = 0;
    if (s_post_render) s_post_render(s_post_render_ctx, (uint32_t)slackUs);

    uint64_t afterPostRender = esp_timer_get_time();
    int64_t sleepUs = (int64_t)r.nextDeadlineUs - (int64_t)afterPostRender;

    if (r.behind || sleepUs <= 0) {
      if (r.behind) behindCount++;
      // No sleep: go again immediately. paceNextFrame already rebased the
      // deadline so we won't cascade.
      taskYIELD();
    } else {
      // vTaskDelay expects ticks; use pdMS_TO_TICKS but round up to >=1 tick.
      uint32_t ms = (uint32_t)((sleepUs + 999) / 1000);
      if (ms == 0) ms = 1;
      vTaskDelay(pdMS_TO_TICKS(ms));
    }

    frames++;
    s_statFrames++;
    if (r.behind) s_statBehind++;
    s_statDropped += r.droppedFrames;

    if (now - lastReport >= 5'000'000u) {
      ESP_LOGI(TAG, "stats: %u frames, %u behind in last 5s",
               (unsigned)frames, (unsigned)behindCount);
      frames = 0;
      behindCount = 0;
      lastReport = now;
    }
  }
}

bool render_task_start(const RenderTaskConfig* cfg) {
  if (!cfg || !cfg->show) return false;
  if (s_task) return true;  // already running

  s_show = cfg->show;
  s_pre_render = cfg->pre_render;
  s_pre_render_ctx = cfg->pre_render_ctx;
  s_post_render = cfg->post_render;
  s_post_render_ctx = cfg->post_render_ctx;
  uint32_t hz = cfg->targetHz ? cfg->targetHz : glow::DEFAULT_RENDER_HZ;
  s_periodUs = (hz == 0) ? glow::DEFAULT_RENDER_PERIOD_US : (1'000'000u / hz);

  uint8_t core = cfg->core ? cfg->core : glow::RENDER_CORE;
  uint32_t stack = cfg->stackBytes ? cfg->stackBytes : glow::RENDER_TASK_STACK;
  UBaseType_t prio = cfg->priority ? cfg->priority : glow::RENDER_TASK_PRIORITY;

  BaseType_t ok = xTaskCreatePinnedToCore(render_loop, "render", stack / sizeof(StackType_t),
                                          nullptr, prio, &s_task, core);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed");
    s_task = nullptr;
    return false;
  }
  return true;
}

void render_task_stop(void) {
  if (s_task) {
    vTaskDelete(s_task);
    s_task = nullptr;
  }
}

bool render_task_running(void) { return s_task != nullptr; }

bool render_task_last_frame_behind(void) { return s_behind; }

void render_task_get_and_reset_stats(uint32_t* frames, uint32_t* behind, uint32_t* dropped) {
  if (frames) *frames = s_statFrames;
  if (behind) *behind = s_statBehind;
  if (dropped) *dropped = s_statDropped;
  s_statFrames = 0;
  s_statBehind = 0;
  s_statDropped = 0;
}
