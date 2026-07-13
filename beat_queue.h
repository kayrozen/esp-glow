#pragma once

#include "beat_clock.h"  // glow::BeatEvent, glow::BeatClock

#include <cstddef>
#include <mutex>
#include <vector>

//
// Beat-event queue — decouples clock-source transports (MIDI clock,
// DJ-Link, tap, internal) from the render task, exactly mirroring
// control_queue.h's rationale and shape: producers (transports, each on
// their own task) push glow::BeatEvents; the render task is the only
// consumer, draining via pumpBeatEvents() at the top of each frame
// (alongside pumpControlEvents — see render_tick_hooks in main.cpp) and
// feeding glow::BeatClock, which is itself only ever touched from the
// render task.
//
// Host: std::mutex-guarded ring (RingBeatEventQueue). Device: FreeRTOS
// queue (FreeRtosBeatEventQueue, beat_queue_freertos.cpp). No hand-rolled
// lock-free ring on either side — see control_queue.h for why.
//

namespace glow {

class IBeatEventQueue {
public:
  virtual ~IBeatEventQueue() = default;
  // Producer side (any task). Returns false if the queue is full (event dropped).
  virtual bool push(const BeatEvent& ev) = 0;
  // Consumer side (render task). Returns false if empty.
  virtual bool pop(BeatEvent& ev) = 0;
};

// Host implementation: mutex-guarded ring buffer. Same overflow policy as
// RingControlEventQueue -- reject the newest, count the drops.
class RingBeatEventQueue : public IBeatEventQueue {
public:
  explicit RingBeatEventQueue(size_t capacity);
  bool push(const BeatEvent& ev) override;
  bool pop(BeatEvent& ev) override;
  size_t dropped() const;
  size_t size() const;  // approximate, for tests

private:
  mutable std::mutex mutex_;
  std::vector<BeatEvent> buffer_;
  size_t head_ = 0;
  size_t tail_ = 0;
  size_t count_ = 0;
  size_t dropped_ = 0;
};

#ifdef ESP_PLATFORM
// Device-only factory: FreeRTOS-queue-backed IBeatEventQueue. Owned by the
// caller (delete when done).
IBeatEventQueue* createDeviceBeatEventQueue(size_t capacity);
#endif

// Drain pending beat events and feed each to clock.onBeat(). Bounds work
// per frame so a flood cannot stall the render loop; leftovers stay
// queued for the next frame. Returns the number dispatched.
int pumpBeatEvents(IBeatEventQueue& q, BeatClock& clock, int maxPerFrame = 32);

}  // namespace glow
