// midi_parser.h — pure MIDI byte-stream parser (host-tested).
//
// The firmware's MIDI input (UART DIN-MIDI or USB-MIDI via TinyUSB) is just a
// byte source; the parsing — running status, real-time interleaving, variable
// data-byte counts — is pure logic and lives here so `make test` covers it.
//
// Usage: feed bytes one at a time; each time feed() returns true, a complete
// MidiEvent is available. The device UART reader calls feed() per byte in its
// ISR-safe ring buffer drain.
#pragma once

#include <cstdint>
#include <cstddef>

struct MidiEvent {
  enum Type : uint8_t {
    None = 0,
    NoteOff        = 0x8,
    NoteOn         = 0x9,
    PolyPressure   = 0xA,
    ControlChange  = 0xB,
    ProgramChange  = 0xC,
    ChannelPressure= 0xD,
    PitchBend      = 0xE,
    SystemCommon   = 0xF,   // data1 carries the sub-status (0xF1..0xF6)
    SystemRealTime = 0x10,  // data1 carries 0xF8..0xFF (clock/start/stop/etc)
  };
  Type    type = None;
  uint8_t channel = 0;   // 0..15 (0 for system messages)
  uint8_t data1 = 0;     // note / cc / program / sub-status
  uint8_t data2 = 0;     // velocity / pressure / value (0 if N/A)
};

class MidiParser {
public:
  // Feed one byte. Returns true if a complete MidiEvent was assembled; `out`
  // is filled only in that case. Real-time bytes (0xF8..0xFF) return true
  // immediately and do NOT disturb running status.
  bool feed(uint8_t byte, MidiEvent& out);

  // Reset parser state (e.g. on a UART glitch /sysex abort).
  void reset();

private:
  uint8_t status_ = 0;     // running status high nibble | channel (0 = none)
  uint8_t data_[2] = {};
  uint8_t dataLen_ = 0;
  uint8_t expect_  = 0;    // data bytes expected for the current status
  bool    inSysex_ = false;
};

// Convenience: how many data bytes a status byte's message carries.
// status is the full 0x80..0xFF byte. Returns 0 for real-time, 1/2 for others,
// and 255 for SysEx start (variable).
uint8_t midiDataBytesForStatus(uint8_t status);
