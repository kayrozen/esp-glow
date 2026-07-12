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
    // TODO: handle_ = xQueueCreate(capacity, sizeof(EvalSubmission));
    (void)capacity;
  }

  ~FreeRtosEvalSubmissionQueue() override {
    // TODO: vQueueDelete(handle_);
  }

  bool push(const EvalSubmission& sub) override {
    // return xQueueSend(handle_, &sub, 0) == pdTRUE;
    (void)sub;
    return false;  // TODO
  }

  bool pop(EvalSubmission& sub) override {
    // return xQueueReceive(handle_, &sub, 0) == pdTRUE;
    (void)sub;
    return false;  // TODO
  }

private:
  // QueueHandle_t handle_ = nullptr;
};

#endif  // ESP_PLATFORM
