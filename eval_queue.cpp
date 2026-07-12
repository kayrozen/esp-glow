#include "eval_queue.h"

#include <algorithm>
#include <cstring>

RingEvalSubmissionQueue::RingEvalSubmissionQueue(size_t capacity)
    : buffer_(capacity > 0 ? capacity : 1) {}

bool RingEvalSubmissionQueue::push(const EvalSubmission& sub) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (count_ >= buffer_.size()) {
    ++dropped_;
    return false;
  }
  buffer_[tail_] = sub;
  tail_ = (tail_ + 1) % buffer_.size();
  ++count_;
  return true;
}

bool RingEvalSubmissionQueue::pop(EvalSubmission& sub) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (count_ == 0) return false;
  sub = buffer_[head_];
  head_ = (head_ + 1) % buffer_.size();
  --count_;
  return true;
}

size_t RingEvalSubmissionQueue::dropped() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dropped_;
}

size_t RingEvalSubmissionQueue::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return count_;
}

bool makeEvalSubmission(const char* src, size_t len, uint32_t requestId, EvalSubmission& out) {
  if (src == nullptr || len == 0) return false;
  size_t copyLen = std::min(len, EVAL_SRC_MAX);
  std::memcpy(out.source, src, copyLen);
  out.len = static_cast<uint32_t>(copyLen);
  out.requestId = requestId;
  return true;
}
