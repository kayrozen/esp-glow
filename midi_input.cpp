#ifdef ESP_PLATFORM

#include "live_control.h"
#include <cstdint>

static LiveControl* g_liveControl = nullptr;

void midi_input_init(LiveControl& live) {
  g_liveControl = &live;
}

void midi_input_handle_byte(uint8_t byte) {
  if (g_liveControl == nullptr) return;

  static uint8_t buffer[3];
  static size_t bufferIdx = 0;

  if (byte & 0x80) {
    bufferIdx = 0;
  }

  buffer[bufferIdx++] = byte;

  if (bufferIdx >= 3) {
    ControlEvent ev;
    if (parseMidi(buffer, 3, ev)) {
      float now = 0.0f;
      g_liveControl->handle(ev, now);
    }
    bufferIdx = 0;
  }
}

void midi_uart_task() {
  // TODO: read MIDI bytes from UART/USB (hardware-specific)
  // for each byte received:
  //   midi_input_handle_byte(byte);
}

#endif
