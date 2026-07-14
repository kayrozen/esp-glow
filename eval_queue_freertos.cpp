#ifdef ESP_PLATFORM

//
// FreeRTOS-backed eval submission queue (device only). Mirrors
// control_queue_freertos.cpp exactly -- same FreeRTOS-queue backing, same
// reasoning (not hand-rolled atomics; FreeRTOS handles cross-core memory
// ordering correctly). See control_queue_freertos.cpp for the full
// rationale. Untestable without real hardware, same status as every other
// device-only file in this component.
//
// Excluded from the host build — never in the Makefile's SOURCE lists.
// The host uses RingEvalSubmissionQueue (eval_queue.cpp).
//

#include "eval_queue.h"

#include "esp_log.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

static const char* kEvalQueueTag = "eval_queue";

class FreeRtosEvalSubmissionQueue : public IEvalSubmissionQueue {
public:
  explicit FreeRtosEvalSubmissionQueue(size_t capacity) {
    handle_ = xQueueCreate(capacity, sizeof(EvalSubmission));
    if (!handle_) {
      // See control_queue_freertos.cpp's FreeRtosControlEventQueue
      // constructor comment -- same internal-RAM-only reasoning, same
      // "loud, no-op, never a panic" policy. EvalSubmission is
      // considerably bigger than ControlEvent/BeatEvent (EVAL_SRC_MAX=4096
      // bytes of source per slot -- see eval_queue.h), so this is the one
      // of the three queues actually likely to hit this path; see the
      // capacity comment at this queue's call site in main.cpp for the
      // sizing fix that should keep it from doing so in practice.
      ESP_LOGE(kEvalQueueTag,
               "xQueueCreate(capacity=%u, item=%u bytes) failed -- the "
               "live-coding REPL (glow.eval over WS) disabled this boot.",
               (unsigned)capacity, (unsigned)sizeof(EvalSubmission));
    }
  }

  ~FreeRtosEvalSubmissionQueue() override {
    if (handle_) vQueueDelete(handle_);
  }

  bool push(const EvalSubmission& sub) override {
    if (!handle_) return false;
    return xQueueSend(handle_, &sub, 0) == pdTRUE;
  }

  bool pop(EvalSubmission& sub) override {
    if (!handle_) return false;
    return xQueueReceive(handle_, &sub, 0) == pdTRUE;
  }

private:
  QueueHandle_t handle_ = nullptr;
};

// See eval_queue.h: the only way to construct one of these from outside
// this file, matching createDeviceControlEventQueue's pattern.
IEvalSubmissionQueue* createDeviceEvalSubmissionQueue(size_t capacity) {
  return new FreeRtosEvalSubmissionQueue(capacity);
}

#endif  // ESP_PLATFORM
