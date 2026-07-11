// render_task.cpp — the render loop.
//
// Each iteration:
//   1. read esp_timer_get_time() (monotonic us)
//   2. glow::paceNextFrame() -> (nowSec, sleepUs, nextDeadline)  [host-tested]
//   3. show->renderFrame(nowSec)
//   4. vTaskDelay(sleepUs) — 0 if behind, so we immediately re-loop
//
// Pinned to core 1; WiFi/lwIP live on core 0 (sdkconfig) so DMX timing on
// core 1 is never preempted by network work.
#include "render_task.h"
#include "render_pacing.h"

#include "esp_timer.h"
#include "esp_log.h"

static const char* TAG = "render_task";

static TaskHandle_t s_task = nullptr;
static Show*        s_show = nullptr;
static uint32_t     s_periodUs = 0;
static volatile bool s_behind = false;
static render_pre_render_fn s_pre_render = nullptr;
static void*               s_pre_render_ctx = nullptr;

static void render_loop(void*) {
  ESP_LOGI(TAG, "render loop started on core %d (period=%lu us%s)",
           xPortGetCoreID(), (unsigned long)s_periodUs,
           s_pre_render ? ", pre_render=on" : "");

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

    if (r.behind) {
      behindCount++;
      // No sleep: go again immediately. paceNextFrame already rebased the
      // deadline so we won't cascade.
      taskYIELD();
    } else if (r.sleepUs > 0) {
      // vTaskDelay expects ticks; use pdMS_TO_TICKS but round up to >=1 tick.
      uint32_t ms = (r.sleepUs + 999) / 1000;
      if (ms == 0) ms = 1;
      vTaskDelay(pdMS_TO_TICKS(ms));
    }

    frames++;
    if (now - lastReport >= 5'000'000u) {
      ESP_LOGI(TAG, "stats: %u frames, %u behind in last 5s", frames, behindCount);
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
