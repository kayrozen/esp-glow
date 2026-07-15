#pragma once

#include "mdef.h"

#include <cstdint>
#include <cstddef>

// P1.1: controller init handshake (INIT SYSEX, MDF1 v3 -- see FORMAT.md).
// This is deliberately its own tiny seam, not a reuse of led_feedback.h's
// IMidiOutput: LED feedback sends fixed 3-byte note/CC messages, but an
// init blob is one complete, variable-length raw MIDI message (typically a
// full SysEx frame). Pure send -- never touches ShowController/LiveControl/
// any engine state, so calling this from a transport's connect/bring-up
// path (midi_uart_task, usb_midi_input.cpp's device-connect handler) has no
// concurrency implications for the render task (FORMAT.md's boot-order
// note on why this lives on the transport side).

// Sends one complete raw MIDI message (e.g. a whole F0...F7 SysEx frame)
// verbatim. Device wiring implements this over DIN MIDI OUT (a UART write)
// or a USB-MIDI OUT endpoint; host tests use a recording fake.
class IRawMidiOutput {
public:
  virtual ~IRawMidiOutput() = default;
  virtual void sendRaw(const uint8_t* bytes, size_t len) = 0;
};

// Walks `profile.initBlobs` in declaration order and sends each verbatim
// via `output`, once. No-op if `profile.initCount` is 0 (no INIT line in
// the .mdef, or a v1/v2 blob that pre-dates the feature entirely -- the
// same no-op either way, see mdef.h's MidiControllerProfile::initCount).
void sendControllerInit(const MidiControllerProfile& profile, IRawMidiOutput& output);
