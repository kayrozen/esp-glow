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
#pragma once

#include "show.h"
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations for C++ types used by the C interface.
#ifdef __cplusplus
class IControlEventQueue;
class LiveControl;
#else
typedef void IControlEventQueue;
typedef void LiveControl;
#endif

// Optional per-frame callback. Called on the render task with the current time
// (seconds) and the Show pointer, before renderFrame. Use it to populate Raw
// universes (writeRawUniverse) from a PixelMatrix or similar.
typedef void (*render_pre_render_fn)(void* ctx, float t_sec, Show* show);

// Optional per-frame callback. Called on the render task after renderFrame,
// before the frame pacing sleep. Use it for state broadcasts (F5) or diagnostics.
typedef void (*render_post_render_fn)(void* ctx, float t_sec, Show* show);

struct RenderTaskConfig {
  Show*    show;            // borrowed, must outlive the task
  uint32_t targetHz;        // 0 => use glow::DEFAULT_RENDER_HZ (44)
  uint8_t  core;            // pin to this core (default 1)
  uint32_t stackBytes;      // default 4096
  UBaseType_t priority;     // default 20 (above WiFi, below ISR)
  render_pre_render_fn pre_render;  // optional, may be nullptr
  void*               pre_render_ctx;
  render_post_render_fn post_render;  // F5: optional, may be nullptr
  void*               post_render_ctx;

  // F4: Control input queue and live control handler (optional, may be nullptr).
  IControlEventQueue* queue;        // optional, used if non-nullptr
  LiveControl*        live_control; // optional, used if non-nullptr
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

#ifdef __cplusplus
}
#endif
