// midi_input.h — device-only MIDI byte source -> MidiParser -> LiveControl.
//
// Two transports are supported, selected at init:
//   - UART DIN-MIDI  (31250 baud, 8N1) on a GPIO pair. Most lighting uses this.
//   - Native USB-MIDI via TinyUSB on the ESP32-S3 USB-OTG pins. (Stubbed: the
//     TinyUSB descriptor + RX hook is board-specific; wire it to feed_bytes().)
//
// The parser (running status, real-time, sysex) is host-tested in
// midi_parser.cpp; this file only owns the byte source and the LiveControl
// dispatch with the current show time.
#pragma once

#include "live_control.h"
#include "midi_parser.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct MidiInputConfig {
  int      uartNum;     // e.g. UART_NUM_1
  int      rxGpio;      // DIN-MIDI opto-isolator data pin
  int      txGpio;      // -1 if TX not wired (listen-only)
  LiveControl* live;     // borrowed; must outlive the input task
};

// Install the UART driver and start the MIDI reader task. Returns true on
// success. The task feeds bytes into a MidiParser and dispatches complete
// events to live->handleMidi() with the current esp_timer time.
bool midi_input_start(const struct MidiInputConfig* cfg);

// Feed bytes from an external source (e.g. TinyUSB MIDI RX hook). Public so
// the USB-MIDI path can reuse the same parser+dispatch without a UART.
void midi_input_feed_bytes(LiveControl* live, const uint8_t* data, size_t len);

// Stop the UART reader task (USB-MIDI is driven externally, no task to stop).
void midi_input_stop(void);

#ifdef __cplusplus
}
#endif
