#pragma once

#include <cstddef>
#include <cstdint>

class IControlEventQueue;
namespace glow {
class IBeatEventQueue;
}

//
// MIDI input — device transport (see midi_input.cpp). Reads DIN-MIDI bytes
// off a UART, frames them (via MidiByteReader, midi_realtime.h -- host-
// tested, handles Realtime bytes interleaved mid-message) and parses
// channel messages with parseMidi (live_control.h) into ControlEvents
// pushed onto the control queue. MIDI Clock (24 PPQN) becomes a BeatEvent
// per beat, pushed onto the beat queue. The render task is the only
// consumer of either queue; this transport never touches
// LiveControl/ShowController/BeatClock directly.
//

// Initialize the MIDI input layer: the queues it pushes to (beatQueue may
// be nullptr to disable MIDI clock -> BeatEvent forwarding, e.g. on a
// device build with no musical-time feature enabled), plus which UART
// port and RX GPIO to read from (TX is unused -- MIDI IN only). Must be
// called before midi_uart_task starts.
void midi_input_init(IControlEventQueue& queue, glow::IBeatEventQueue* beatQueue,
                     int uartNum, int rxGpio);

// Frames and parses one incoming MIDI byte, pushing a ControlEvent if a
// complete message is recognized. Exposed separately from the UART task so
// it's callable from a unit test or an alternate transport (e.g. USB-MIDI)
// without pulling in the UART driver.
void midi_input_handle_byte(uint8_t byte);

// FreeRTOS task entry point: installs the UART driver (configured for
// standard MIDI: 31250 baud, 8N1) on the port/pin passed to midi_input_init,
// then loops reading bytes into midi_input_handle_byte. Never returns.
void midi_uart_task(void* ctx);
