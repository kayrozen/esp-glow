#ifdef ESP_PLATFORM

//
// FreeRTOS-backed control-event queue (device only).
//
// Backed by a FreeRTOS queue of sizeof(ControlEvent) items. FreeRTOS
// handles the memory ordering and multi-producer safety correctly — no
// hand-rolled atomics or fences are needed.
//
// Both push() and pop() are non-blocking (0 ticks to wait): producers
// never block, and the render task never blocks draining. If the queue
// is full, push() returns false (the event is dropped, same overflow
// policy as the host ring). Size the queue generously (>= 64) so this
// never happens at human event rates.
//
// Excluded from the host build — this file is never in the Makefile's
// SOURCE lists. The host uses RingControlEventQueue (control_queue.cpp).
//

#include "control_queue.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

class FreeRtosControlEventQueue : public IControlEventQueue {
public:
  explicit FreeRtosControlEventQueue(size_t capacity) {
    handle_ = xQueueCreate(capacity, sizeof(ControlEvent));
  }

  ~FreeRtosControlEventQueue() override {
    if (handle_) vQueueDelete(handle_);
  }

  bool push(const ControlEvent& ev) override {
    // Non-blocking send. Called from any task (web httpd, OSC UDP, MIDI
    // UART task — not ISR).
    return xQueueSend(handle_, &ev, 0) == pdTRUE;
  }

  // For ISR producers (e.g. a MIDI UART RX ISR). Not on the base
  // interface because it's device+ISR-specific. Cast the IControlEventQueue*
  // to FreeRtosControlEventQueue* to access this from the MIDI ISR path.
  bool pushFromISR(const ControlEvent& ev) {
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    bool ok = xQueueSendFromISR(handle_, &ev, &higherPriorityTaskWoken) == pdTRUE;
    portYIELD_FROM_ISR(higherPriorityTaskWoken);
    return ok;
  }

  bool pop(ControlEvent& ev) override {
    // Non-blocking receive. Called from the render task at the top of
    // each frame via pumpControlEvents().
    return xQueueReceive(handle_, &ev, 0) == pdTRUE;
  }

private:
  QueueHandle_t handle_ = nullptr;
};

// See control_queue.h: the only way to construct one of these from outside
// this file. Kept as a factory (rather than exposing the class itself in
// the header) so main.cpp doesn't need to pull in FreeRTOS queue headers
// just to hold a pointer.
IControlEventQueue* createDeviceControlEventQueue(size_t capacity) {
  return new FreeRtosControlEventQueue(capacity);
}

#endif  // ESP_PLATFORM
