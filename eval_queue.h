#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

//
// Eval submission queue — decouples the web input transport from the
// render task, for the live-coding eval channel (see design doc section 8,
// README_LUA_FENNEL.md).
//
// Same rationale as control_queue.h: the WS transport runs on its own
// FreeRTOS task, while the single Lua VM is owned by the render task (see
// glow_fennel.h). Submissions cross that boundary through this queue,
// drained on the render task in its frame slack (pumpEvalSubmissions),
// exactly like pumpControlEvents. See control_queue.h for why this is a
// mutex-guarded ring on host / FreeRTOS queue on device rather than a
// hand-rolled lock-free ring.
//
// EvalSubmission is a fixed-size POD (not a std::string) so it fits a
// FreeRTOS queue item, which needs a fixed size. EVAL_SRC_MAX is generous
// for a live-coding snippet; a submission that doesn't fit is truncated
// rather than dropped (see makeEvalSubmission) — the compile will simply
// fail with a visible syntax error, same as if the author's paste got cut
// off, and the request id is still echoed back either way.
//

constexpr size_t EVAL_SRC_MAX = 4096;

struct EvalSubmission {
  char source[EVAL_SRC_MAX];
  uint32_t len;        // bytes of `source` actually used (not NUL-terminated-length dependent)
  uint32_t requestId;  // opaque; echoed back in eval_result so the client can correlate
};

class IEvalSubmissionQueue {
public:
  virtual ~IEvalSubmissionQueue() = default;
  virtual bool push(const EvalSubmission& sub) = 0;
  virtual bool pop(EvalSubmission& sub) = 0;
};

// Host implementation: mutex-guarded ring, same design (and the same
// reasoning) as RingControlEventQueue in control_queue.h.
class RingEvalSubmissionQueue : public IEvalSubmissionQueue {
public:
  explicit RingEvalSubmissionQueue(size_t capacity);
  bool push(const EvalSubmission& sub) override;
  bool pop(EvalSubmission& sub) override;
  size_t dropped() const;
  size_t size() const;

private:
  mutable std::mutex mutex_;
  std::vector<EvalSubmission> buffer_;
  size_t head_ = 0;
  size_t tail_ = 0;
  size_t count_ = 0;
  size_t dropped_ = 0;
};

// Build an EvalSubmission from a source buffer. Truncates (never overflows)
// if `len` exceeds EVAL_SRC_MAX; still returns true in that case (see
// header rationale above) — returns false only if src is null or len is 0.
bool makeEvalSubmission(const char* src, size_t len, uint32_t requestId, EvalSubmission& out);

#ifdef ESP_PLATFORM
// Device-only factory: returns a FreeRTOS-queue-backed IEvalSubmissionQueue
// (FreeRtosEvalSubmissionQueue, defined in eval_queue_freertos.cpp). Owned
// by the caller (delete when done). Mirrors control_queue.h's
// createDeviceControlEventQueue exactly.
IEvalSubmissionQueue* createDeviceEvalSubmissionQueue(size_t capacity);
#endif
