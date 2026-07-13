#include "midi_realtime.h"

namespace {
constexpr int kPulsesPerQuarterNote = 24;
bool isRealtimeByte(uint8_t b) { return b >= 0xF8; }
}  // namespace

MidiByteReader::Result MidiByteReader::feed(uint8_t byte, uint8_t outMsg[3]) {
  // Realtime bytes are single-byte messages that legally appear between
  // the bytes of another in-flight message. Handle them here, WITHOUT
  // touching buffer_/bufferIdx_ -- that's the whole fix.
  if (isRealtimeByte(byte)) {
    switch (byte) {
      case 0xF8:  // Clock
        if (++pulseCount_ >= kPulsesPerQuarterNote) {
          pulseCount_ = 0;
          return Result::BeatPulse;
        }
        return Result::None;
      case 0xFA:  // Start
        pulseCount_ = 0;
        return Result::TransportStart;
      case 0xFC:  // Stop
        pulseCount_ = 0;
        return Result::TransportStop;
      case 0xFB:  // Continue -- resumes without a fresh downbeat, but this
                  // class doesn't track fractional beat position across a
                  // pause, so the pulse count still restarts cleanly.
        pulseCount_ = 0;
        return Result::TransportContinue;
      default:    // 0xF9 (undefined), 0xFE (Active Sensing), 0xFF (Reset)
        return Result::None;
    }
  }

  if (byte & 0x80) bufferIdx_ = 0;  // a real status byte restarts framing
  buffer_[bufferIdx_++] = byte;
  if (bufferIdx_ >= 3) {
    outMsg[0] = buffer_[0];
    outMsg[1] = buffer_[1];
    outMsg[2] = buffer_[2];
    bufferIdx_ = 0;
    return Result::ChannelMessage;
  }
  return Result::None;
}
