#include "control_queue.h"

// --- RingControlEventQueue (host implementation) -----------------------
//
// A mutex-guarded circular ring. The mutex makes push() and pop() safe
// for multiple producers (any FreeRTOS task on the device, any std::thread
// in tests) and one consumer (the render task). ThreadSanitizer validates
// that the locking is correct — the property the whole module exists to
// guarantee.
//
// No atomics, no fences. The mutex is the only synchronization primitive.

RingControlEventQueue::RingControlEventQueue(size_t capacity)
  : buffer_(capacity > 0 ? capacity : 1) {
  // Guard against zero-capacity construction; a zero-capacity ring would
  // reject every push, which is never the intent.
}

bool RingControlEventQueue::push(const ControlEvent& ev) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (count_ >= buffer_.size()) {
    // Overflow: reject the newest event. See header for rationale.
    ++dropped_;
    return false;
  }
  buffer_[tail_] = ev;
  tail_ = (tail_ + 1) % buffer_.size();
  ++count_;
  return true;
}

bool RingControlEventQueue::pop(ControlEvent& ev) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (count_ == 0) return false;
  ev = buffer_[head_];
  head_ = (head_ + 1) % buffer_.size();
  --count_;
  return true;
}

size_t RingControlEventQueue::dropped() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dropped_;
}

size_t RingControlEventQueue::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return count_;
}

// --- pumpControlEvents --------------------------------------------------

int pumpControlEvents(IControlEventQueue& q, LiveControl& live, float t,
                      int maxPerFrame) {
  int dispatched = 0;
  ControlEvent ev;
  while (dispatched < maxPerFrame && q.pop(ev)) {
    live.handle(ev, t);
    ++dispatched;
  }
  return dispatched;
}
