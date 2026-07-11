#ifdef ESP_PLATFORM

//
// MIDI input — device scaffold.
//
// Parses MIDI bytes into ControlEvents and pushes them to the
// control-event queue. The render task drains the queue via
// pumpControlEvents() and dispatches to LiveControl — the transports
// no longer touch LiveControl/ShowController directly, eliminating the
// cross-core data race. See control_queue.h for the rationale.
//
// If MIDI bytes arrive from a UART ISR (not a task), use a
// FreeRtosControlEventQueue and call pushFromISR() instead of push().
//

#include "control_queue.h"  // IControlEventQueue, ControlEvent (transitively)
#include "live_control.h"   // ControlType, parseMidi

#include <cstdint>

static IControlEventQueue* g_queue = nullptr;

void midi_input_init(IControlEventQueue& queue) {
  g_queue = &queue;
}

void midi_input_handle_byte(uint8_t byte) {
  if (g_queue == nullptr) return;

  static uint8_t buffer[3];
  static size_t bufferIdx = 0;

  if (byte & 0x80) {
    bufferIdx = 0;
  }

  buffer[bufferIdx++] = byte;

  if (bufferIdx >= 3) {
    ControlEvent ev;
    if (parseMidi(buffer, 3, ev)) {
      g_queue->push(ev);
    }
    bufferIdx = 0;
  }
}

void midi_uart_task() {
  // TODO: read MIDI bytes from UART/USB (hardware-specific)
  // for each byte received:
  //   midi_input_handle_byte(byte);
  //
  // If wired as a UART RX ISR instead of a task, call
  // ((FreeRtosControlEventQueue*)g_queue)->pushFromISR(ev) directly
  // from the ISR, bypassing midi_input_handle_byte.
}

#endif
