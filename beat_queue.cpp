#include "beat_queue.h"

namespace glow {

// --- RingBeatEventQueue (host implementation) --------------------------
//
// Same mutex-guarded ring as RingControlEventQueue (control_queue.cpp) —
// see that file's header comment for the concurrency rationale.

RingBeatEventQueue::RingBeatEventQueue(size_t capacity)
  : buffer_(capacity > 0 ? capacity : 1) {}

bool RingBeatEventQueue::push(const BeatEvent& ev) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (count_ >= buffer_.size()) {
    ++dropped_;
    return false;
  }
  buffer_[tail_] = ev;
  tail_ = (tail_ + 1) % buffer_.size();
  ++count_;
  return true;
}

bool RingBeatEventQueue::pop(BeatEvent& ev) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (count_ == 0) return false;
  ev = buffer_[head_];
  head_ = (head_ + 1) % buffer_.size();
  --count_;
  return true;
}

size_t RingBeatEventQueue::dropped() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dropped_;
}

size_t RingBeatEventQueue::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return count_;
}

// --- pumpBeatEvents ------------------------------------------------------

int pumpBeatEvents(IBeatEventQueue& q, BeatClock& clock, int maxPerFrame) {
  int dispatched = 0;
  BeatEvent ev;
  while (dispatched < maxPerFrame && q.pop(ev)) {
    clock.onBeat(ev);
    ++dispatched;
  }
  return dispatched;
}

}  // namespace glow
