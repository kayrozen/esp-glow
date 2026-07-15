#pragma once

#include <cstddef>
#include <cstdint>

#include "led_feedback.h"     // IMidiOutput (A5: MIDI OUT / LED feedback)
#include "controller_init.h"  // IRawMidiOutput (P1.1: INIT SYSEX send-on-connect)

class IControlEventQueue;
namespace glow {
class IBeatEventQueue;
}
struct MidiControllerProfile;

//
// MIDI input/output — device transport (see midi_input.cpp). Reads DIN-MIDI
// bytes off a UART, frames them (via MidiByteReader, midi_realtime.h -- host-
// tested, handles Realtime bytes interleaved mid-message) and parses
// channel messages with parseMidi (live_control.h) into ControlEvents
// pushed onto the control queue. MIDI Clock (24 PPQN) becomes a BeatEvent
// per beat, pushed onto the beat queue. The render task is the only
// consumer of either queue; this transport never touches
// LiveControl/ShowController/BeatClock directly.
//
// A5: also drives DIN MIDI OUT (TX), a second UART pin at the same 31250
// baud -- no optocoupler needed on the TX side, that's the receiver's job.
// This is the one place LED feedback (led_feedback.h) touches hardware;
// LedFeedback itself only ever calls the IMidiOutput interface.
//

// Initialize the MIDI input layer: the queues it pushes to (beatQueue may
// be nullptr to disable MIDI clock -> BeatEvent forwarding, e.g. on a
// device build with no musical-time feature enabled), plus which UART
// port and RX/TX GPIOs to use. txGpio < 0 (the default) disables MIDI OUT --
// midi_output_send3 becomes a no-op, same graceful-degradation contract as
// every other missing-capability case in this project. Must be called
// before midi_uart_task starts.
//
// P1.1: `controllerProfile` (nullptr by default) is the loaded .mdef's
// init blobs (controller_init.h) -- when non-null and txGpio >= 0,
// midi_uart_task sends each one, in order, once, right after the UART
// finishes installing (see midi_uart_task's header comment). Passing
// nullptr (no controller wired, or DIN MIDI OUT disabled) is the same
// no-op sendControllerInit already gives an empty init table.
void midi_input_init(IControlEventQueue& queue, glow::IBeatEventQueue* beatQueue,
                     int uartNum, int rxGpio, int txGpio = -1,
                     const MidiControllerProfile* controllerProfile = nullptr);

// Frames and parses one incoming MIDI byte, pushing a ControlEvent if a
// complete message is recognized. Exposed separately from the UART task so
// it's callable from a unit test or an alternate transport (e.g. USB-MIDI)
// without pulling in the UART driver.
void midi_input_handle_byte(uint8_t byte);

// FreeRTOS task entry point: installs the UART driver (configured for
// standard MIDI: 31250 baud, 8N1) on the port/pins passed to midi_input_init,
// then loops reading bytes into midi_input_handle_byte. Never returns.
void midi_uart_task(void* ctx);

// A5: send a raw 3-byte MIDI message out the TX pin configured via
// midi_input_init. No-op if txGpio was < 0, or the UART driver hasn't
// finished installing yet (midi_uart_task hasn't run) -- safe to call
// before boot.fnl paints initial LED state.
void midi_output_send3(uint8_t status, uint8_t data1, uint8_t data2);

// P1.1: send one complete raw MIDI message (e.g. a whole SysEx frame) out
// the same TX pin -- same readiness/no-op rules as midi_output_send3.
void midi_output_send_raw(const uint8_t* bytes, size_t len);

#ifdef ESP_PLATFORM
// Adapter handed to LedFeedback's constructor (main.cpp): wraps
// midi_output_send3 as an IMidiOutput so LedFeedback (led_feedback.h)
// never needs to know DIN MIDI is a UART underneath -- same seam
// glow_lua_api.h's IMatrixRegistry draws for glow.matrix.*. Also
// implements IRawMidiOutput (controller_init.h) over midi_output_send_raw,
// so the same instance serves both LED feedback and INIT SYSEX sends --
// one DIN MIDI OUT adapter, not two.
class DeviceMidiOutput : public IMidiOutput, public IRawMidiOutput {
public:
  void send3(uint8_t status, uint8_t data1, uint8_t data2) override;
  void sendRaw(const uint8_t* bytes, size_t len) override;
};
#endif
