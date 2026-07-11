// render_pacing.h — pure frame-pacing math extracted from the firmware render
// loop, so the one piece of real logic the firmware invents stays host-tested.
//
// The firmware render task (see firmware/main/render_task.cpp) does, every
// iteration:
//   1. read a monotonic tick (esp_timer_get_time, in microseconds)
//   2. convert to seconds
//   3. compute the next deadline and how long to sleep
//   4. call show.renderFrame(t)
//
// Steps 2 and 3 are pure arithmetic with real edge cases (tick rollover, frame
// budget exceeded, drift correction). They live here and are covered by
// test_render_pacing.cpp under `make test`. The device side just calls these
// functions with real timer values.
#pragma once

#include <cstdint>

namespace glow {

// Render-loop timing constants. Centralised so device and host agree.
constexpr uint32_t DEFAULT_RENDER_HZ      = 44;          // ~44 Hz, DMX-friendly
constexpr uint32_t DEFAULT_RENDER_PERIOD_US = 1'000'000u / DEFAULT_RENDER_HZ;  // ~22727 us
constexpr uint32_t RENDER_CORE             = 1;          // pin render to core 1
constexpr uint32_t RENDER_TASK_STACK       = 4096;
constexpr uint32_t RENDER_TASK_PRIORITY    = 20;         // above WiFi, below ISR

// Convert a monotonic microsecond tick to seconds (float, for renderFrame).
// Pure. Handles the (unrealistic on ESP32) u64 wrap cleanly because the
// division is exact in double precision over the 64-bit range.
inline float tickToSeconds(uint64_t tickUs) {
  return static_cast<float>(static_cast<double>(tickUs) / 1.0e6);
}

// Result of pacing a single frame.
struct PaceResult {
  float    nowSec;       // tickToSeconds(nowUs) — pass straight to renderFrame
  int32_t  sleepUs;      // how long to vTaskDelay before the next frame.
                         // <= 0 means we are behind budget (do not sleep).
  uint64_t nextDeadlineUs;  // the target absolute tick for the next frame
  bool     behind;      // true if this frame overran its budget
};

// Compute pacing for the next frame.
//
//   targetPeriodUs  : nominal frame period (e.g. DEFAULT_RENDER_PERIOD_US)
//   nowUs           : current monotonic tick (microseconds)
//   prevDeadlineUs  : the deadline the previous frame was scheduled for
//                     (pass 0 on the very first frame to seed from nowUs)
//
// Drift-correcting rule: if we fell behind, do NOT try to "catch up" by
// sleeping negative — just emit sleepUs <= 0 and re-base the next deadline
// off the current time. This keeps the loop stable under load (a single
// overrunning frame does not cascade into a burst of zero-sleep frames).
PaceResult paceNextFrame(uint32_t targetPeriodUs, uint64_t nowUs,
                         uint64_t prevDeadlineUs);

// Convert a FreeRTOS tick count (xTaskGetTickCount) to microseconds. Useful as
// a coarse fallback timer if esp_timer is unavailable in a test harness.
inline uint64_t ticksToUs(uint32_t ticks, uint32_t tickHz) {
  if (tickHz == 0) return 0;
  return (static_cast<uint64_t>(ticks) * 1'000'000u) / tickHz;
}

}  // namespace glow
