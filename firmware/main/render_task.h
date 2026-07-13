// render_task.h — the FreeRTOS task that drives Show::renderFrame at ~44 Hz.
//
// Pinned to core 1 by default so DMX timing is never starved by WiFi/lwIP
// (which lives on core 0 via sdkconfig). The only real logic — tick->seconds
// and frame pacing — is delegated to render_pacing.h (host-tested).
//
// F2 adds an optional pre_render hook: a callback invoked each frame BEFORE
// renderFrame, so callers can fill Raw universes (e.g. render a PixelMatrix
// pattern and writeRawUniverse it into the Show). This keeps matrix driving
// inside the same paced loop instead of a second racing task.
//
// F6 adds an optional post_render hook: a callback invoked each frame AFTER
// renderFrame, given however much slack is left before the next deadline
// (0 if the frame ran behind and there is none). This is where the Lua VM's
// GC gets its only chance to run (see lua_vm.h's LuaVM::gcStepSlack): the
// GC is created stopped and never runs automatically, specifically so it
// can be confined to this bounded, measured window instead of causing an
// uncontrolled pause on the render path. The loop re-measures the clock
// after this hook returns before computing how long to sleep, so time the
// hook spends is never double-counted on top of the frame's own sleep.
#pragma once

#include "show.h"
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

// Optional per-frame callback. Called on the render task with the current time
// (seconds) and the Show pointer, before renderFrame. Use it to populate Raw
// universes (writeRawUniverse) from a PixelMatrix or similar.
typedef void (*render_pre_render_fn)(void* ctx, float t_sec, Show* show);

// Optional per-frame callback, called AFTER renderFrame with the
// microseconds of slack remaining before the next deadline (see the F6
// comment above). slack_us is 0 on a frame that ran behind — the callback
// must treat that as "do nothing", not "do a full unbounded pass".
typedef void (*render_post_render_fn)(void* ctx, uint32_t slack_us);

struct RenderTaskConfig {
  Show*    show;            // borrowed, must outlive the task
  uint32_t targetHz;        // 0 => use glow::DEFAULT_RENDER_HZ (44)
  uint8_t  core;            // pin to this core (default 1)
  uint32_t stackBytes;      // default 4096
  UBaseType_t priority;     // default 20 (above WiFi, below ISR)
  render_pre_render_fn pre_render;  // optional, may be nullptr
  void*               pre_render_ctx;
  render_post_render_fn post_render;  // optional, may be nullptr
  void*                post_render_ctx;
};

// Start the render task. Returns true on success. The task runs until
// render_task_stop() is called or the Show pointer is invalidated by the
// caller (don't do that while the task is running).
bool render_task_start(const RenderTaskConfig* cfg);

// Stop and delete the render task. Safe to call from any task.
void render_task_stop(void);

// True if the render task is currently running.
bool render_task_running(void);

// Expose the last frame's "behind" flag for diagnostics (e.g. LED pattern).
bool render_task_last_frame_behind(void);

// Snapshot cumulative frame/behind/dropped counts accumulated since the
// last call, then reset them to zero. Only meaningful called from the
// render task itself (e.g. from a post_render hook) -- it is not
// synchronized against concurrent callers on another task. See
// render_pacing.h's PaceResult::droppedFrames for what "dropped" counts
// (whole frame periods with no render call, e.g. a GC pause) as distinct
// from "behind" (this frame missed its deadline, but still ran).
void render_task_get_and_reset_stats(uint32_t* frames, uint32_t* behind, uint32_t* dropped);

#ifdef __cplusplus
}
#endif
