#ifdef ESP_PLATFORM

//
// FreeRTOS-backed beat-event queue (device only). Mirrors
// control_queue_freertos.cpp exactly -- see that file's header comment.
//
// Excluded from the host build -- never in the Makefile's SOURCE lists.
// The host uses RingBeatEventQueue (beat_queue.cpp).
//

#include "beat_queue.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace glow {

class FreeRtosBeatEventQueue : public IBeatEventQueue {
public:
  explicit FreeRtosBeatEventQueue(size_t capacity) {
    handle_ = xQueueCreate(capacity, sizeof(BeatEvent));
  }

  ~FreeRtosBeatEventQueue() override {
    if (handle_) vQueueDelete(handle_);
  }

  bool push(const BeatEvent& ev) override {
    return xQueueSend(handle_, &ev, 0) == pdTRUE;
  }

  // For ISR producers, same rationale as FreeRtosControlEventQueue's.
  bool pushFromISR(const BeatEvent& ev) {
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    bool ok = xQueueSendFromISR(handle_, &ev, &higherPriorityTaskWoken) == pdTRUE;
    portYIELD_FROM_ISR(higherPriorityTaskWoken);
    return ok;
  }

  bool pop(BeatEvent& ev) override {
    return xQueueReceive(handle_, &ev, 0) == pdTRUE;
  }

private:
  QueueHandle_t handle_ = nullptr;
};

IBeatEventQueue* createDeviceBeatEventQueue(size_t capacity) {
  return new FreeRtosBeatEventQueue(capacity);
}

}  // namespace glow

#endif  // ESP_PLATFORM
