#pragma once

#include "live_control.h"  // ControlEvent, LiveControl

#include <cstddef>
#include <mutex>
#include <vector>

//
// Control-event queue — decouples input transports from the render task.
//
// PROBLEM
//   The input transports (web_input, midi_input, osc_input) call
//   LiveControl::handle directly, which mutates ShowController from the
//   transport's FreeRTOS task — while the render task reads the same
//   ShowController at 44 Hz on the OTHER core of the ESP32-S3. That is a
//   data race on real hardware (weak memory model, tasks on separate
//   cores) that host tests cannot catch.
//
// SOLUTION
//   Insert a command queue. Producers (transports) push ControlEvents;
//   the render task drains the queue via pumpControlEvents() at the top
//   of each frame, before show.renderFrame(t). After this change, every
//   ShowController mutation happens on the render task — the cross-core
//   race is gone by construction, with no lock on the 44 Hz path.
//
// WHY NOT HAND-ROLLED LOCK-FREE
//   A hand-rolled lock-free ring with manual acquire/release fences would
//   pass on the x86 host (strong TSO ordering) and still race on the
//   Xtensa target (weaker ordering) — an untestable bug. Therefore:
//     - Device: FreeRTOS queue (xQueueSend / xQueueReceive). FreeRTOS
//       handles the memory ordering and multi-producer safety correctly.
//     - Host:   std::mutex-guarded ring. Correct and deterministic; the
//       concurrency test validates it under -fsanitize=thread.
//   No std::atomic-based lock-free ring, no custom fences.
//

class IControlEventQueue {
public:
  virtual ~IControlEventQueue() = default;
  // Producer side (any task). Returns false if the queue is full (event dropped).
  virtual bool push(const ControlEvent& ev) = 0;
  // Consumer side (render task). Returns false if empty.
  virtual bool pop(ControlEvent& ev) = 0;
};

//
// Host implementation: mutex-guarded ring buffer.
//
// Fixed capacity, allocated once at construction. push() rejects (returns
// false) when full and increments an internal drop counter; pop() returns
// false when empty.
//
// Overflow policy: reject the newest. Dropping an already-queued event
// risks stranding a matched press/release pair (e.g. a stuck light).
// Size the device queue generously (>= 64) so overflow never happens at
// human event rates; dropped() exists purely to surface a misconfiguration.
//
class RingControlEventQueue : public IControlEventQueue {
public:
  explicit RingControlEventQueue(size_t capacity);
  bool push(const ControlEvent& ev) override;
  bool pop(ControlEvent& ev) override;
  size_t dropped() const;
  size_t size() const;  // approximate, for tests

private:
  mutable std::mutex mutex_;
  std::vector<ControlEvent> buffer_;
  size_t head_ = 0;    // next pop index
  size_t tail_ = 0;    // next push index
  size_t count_ = 0;
  size_t dropped_ = 0;
};

//
// Drain pending events and dispatch each via live.handle(ev, t).
// Bounds work per frame so a flood cannot stall the 44 Hz render loop;
// leftover events stay queued for the next frame. Returns the number
// dispatched.
//
// All dispatched events are applied at the frame's `t`, not their arrival
// time. This adds at most one frame (~22 ms at 44 Hz) of latency —
// imperceptible for lighting, and makes cue timing deterministic (aligned
// to frame boundaries).
//
int pumpControlEvents(IControlEventQueue& q, LiveControl& live, float t,
                      int maxPerFrame = 64);
