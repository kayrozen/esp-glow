#ifdef ESP_PLATFORM

//
// FreeRTOS-backed eval submission queue (device only). Mirrors
// control_queue_freertos.cpp exactly, including its TODO status: the real
// httpd WS server this would sit behind (web_input.cpp) is itself still a
// stub, so this is wired the same way and for the same reason — it cannot
// be verified without hardware. See control_queue_freertos.cpp for the
// full rationale (FreeRTOS, not hand-rolled atomics, handles cross-core
// memory ordering correctly).
//
// Excluded from the host build — never in the Makefile's SOURCE lists.
// The host uses RingEvalSubmissionQueue (eval_queue.cpp).
//

#include "eval_queue.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

class FreeRtosEvalSubmissionQueue : public IEvalSubmissionQueue {
public:
  explicit FreeRtosEvalSubmissionQueue(size_t capacity) {
    handle_ = xQueueCreate(capacity, sizeof(EvalSubmission));
  }

  ~FreeRtosEvalSubmissionQueue() override {
    if (handle_) vQueueDelete(handle_);
  }

  bool push(const EvalSubmission& sub) override {
    return xQueueSend(handle_, &sub, 0) == pdTRUE;
  }

  bool pop(EvalSubmission& sub) override {
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
