// render_task.h — the FreeRTOS task that drives Show::renderFrame at ~44 Hz.
//
// Pinned to core 1 by default so DMX timing is never starved by WiFi/lwIP
// (which lives on core 0 via sdkconfig). The only real logic — tick->seconds
// and frame pacing — is delegated to render_pacing.h (host-tested).
#pragma once

#include "show.h"
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

struct RenderTaskConfig {
  Show*    show;            // borrowed, must outlive the task
  uint32_t targetHz;        // 0 => use glow::DEFAULT_RENDER_HZ (44)
  uint8_t  core;            // pin to this core (default 1)
  uint32_t stackBytes;      // default 4096
  UBaseType_t priority;     // default 20 (above WiFi, below ISR)
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
