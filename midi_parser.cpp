// midi_parser.cpp — pure MIDI parser implementation.
#include "midi_parser.h"

uint8_t midiDataBytesForStatus(uint8_t status) {
  if (status < 0x80) return 0;          // not a status byte
  uint8_t hi = status & 0xF0;
  switch (hi) {
    case 0x80: return 2;  // Note Off
    case 0x90: return 2;  // Note On
    case 0xA0: return 2;  // Poly Pressure
    case 0xB0: return 2;  // Control Change
    case 0xC0: return 1;  // Program Change
    case 0xD0: return 1;  // Channel Pressure
    case 0xE0: return 2;  // Pitch Bend
    case 0xF0: {          // System
      switch (status) {
        case 0xF0: return 255;  // SysEx start (variable)
        case 0xF1: return 1;    // MTC quarter frame
        case 0xF2: return 2;    // Song position
        case 0xF3: return 1;    // Song select
        case 0xF4:
        case 0xF5: return 0;    // undefined / unused
        case 0xF6: return 0;    // Tune request
        default:   return 0;    // 0xF7..0xFF: real-time / EOX / reset
      }
    }
  }
  return 0;
}

bool MidiParser::feed(uint8_t byte, MidiEvent& out) {
  out = MidiEvent();

  // Real-time bytes (0xF8..0xFF) can appear ANYWHERE — even between a status
  // and its data bytes — and must not disturb running status. Emit immediately.
  if (byte >= 0xF8) {
    out.type = MidiEvent::SystemRealTime;
    out.data1 = byte;
    return true;
  }

  // Inside a SysEx: ignore everything until the EOX (0xF7) or a real-time byte
  // (handled above). A status byte mid-sysex aborts the sysex.
  if (inSysex_) {
    if (byte == 0xF7) {
      inSysex_ = false;
      // We do not emit sysex as an event; lighting doesn't use it.
      return false;
    }
    if (byte & 0x80) {
      // Status byte aborts sysex; fall through to handle it as a new status.
      inSysex_ = false;
    } else {
      return false;  // sysex data byte, ignore
    }
  }

  if (byte & 0x80) {
    // Status byte.
    status_ = byte;
    expect_ = midiDataBytesForStatus(byte);
    dataLen_ = 0;
    if (expect_ == 255) {  // SysEx start
      inSysex_ = true;
      return false;
    }
    if (expect_ == 0) {
      // Zero-data message (e.g. 0xF6 tune request): emit now.
      MidiEvent e;
      if (byte >= 0xF0) {
        e.type = MidiEvent::SystemCommon;
        e.data1 = byte;
      } else {
        // No zero-data channel messages exist; treat as real-time (already handled).
        return false;
      }
      out = e;
      // Do NOT clear status_ here for system-common zero-data; running status
      // only applies to channel-voice messages per the spec. Clear to be safe.
      status_ = 0;
      return true;
    }
    return false;  // wait for data bytes
  }

  // Data byte.
  if (expect_ == 0) {
    // No status pending; ignore stray data bytes (robustness).
    return false;
  }
  data_[dataLen_++] = byte;
  if (dataLen_ < expect_) return false;

  // Complete message.
  MidiEvent e;
  uint8_t hi = status_ & 0xF0;
  if (hi < 0xF0) {
    e.channel = status_ & 0x0F;
    e.type = static_cast<MidiEvent::Type>(hi >> 4);
    e.data1 = data_[0];
    e.data2 = (expect_ >= 2) ? data_[1] : 0;
    // Running status persists for channel-voice messages.
  } else {
    e.type = MidiEvent::SystemCommon;
    e.data1 = status_;
    e.data2 = 0;
    if (expect_ >= 1) e.data2 = data_[0];
    // System Common messages do NOT set running status (spec); clear it.
    status_ = 0;
  }
  dataLen_ = 0;
  out = e;
  return true;
}

void MidiParser::reset() {
  status_ = 0;
  dataLen_ = 0;
  expect_ = 0;
  inSysex_ = false;
}
