// midi_realtime.h — the pure byte-framing state machine that recognises
// MIDI System Realtime bytes (0xF8-0xFF) interleaved mid-message.
//
// WHY THIS EXISTS SEPARATELY FROM parseMidi
//   parseMidi (live_control.h) parses a complete, already-framed 3-byte
//   channel message. It has no opinion on framing. Framing used to live
//   entirely in midi_input.cpp's midi_input_handle_byte -- device-only
//   (#ifdef ESP_PLATFORM), so untestable -- and got it wrong for realtime
//   bytes: "any byte with the high bit set resets framing" is correct for
//   channel/status bytes, but MIDI System Realtime messages (Clock 0xF8,
//   Start 0xFA, Continue 0xFB, Stop 0xFC, plus 0xF9/0xFE/0xFF) are legally
//   sent by real hardware in the MIDDLE of another message's bytes -- a
//   clock byte between a Note On's status and velocity byte must not
//   reset the Note On's framing, or the note is dropped.
//   This class is that fix, pulled out as pure logic (no queue, no UART,
//   no clock) so it can be host-tested against exactly that scenario --
//   see test_midi_realtime.cpp.
//
// MIDI Clock is 24 pulses per quarter note; this class turns every 24th
// pulse into a BeatPulse result. It does NOT compute a tempo -- BeatClock's
// PLL (beat_clock.h) is the only estimator in this codebase; feeding it a
// raw beat-boundary timestamp every 24 pulses is exactly its job, not this
// class's (see beat_clock.h's header).
#pragma once

#include <cstddef>
#include <cstdint>

class MidiByteReader {
public:
  enum class Result {
    None,               // byte consumed, nothing ready yet (or an ignored realtime byte)
    ChannelMessage,      // a complete 3-byte channel message is in outMsg
    BeatPulse,           // the 24th MIDI Clock pulse since the last one -- a beat boundary
    TransportStart,      // 0xFA
    TransportStop,       // 0xFC
    TransportContinue,   // 0xFB
  };

  // Feed one incoming byte. On Result::ChannelMessage, outMsg[0..2] holds
  // the completed message (pass straight to parseMidi). outMsg is
  // untouched for every other result.
  Result feed(uint8_t byte, uint8_t outMsg[3]);

private:
  uint8_t buffer_[3] = {0, 0, 0};
  size_t  bufferIdx_ = 0;
  int     pulseCount_ = 0;
};
