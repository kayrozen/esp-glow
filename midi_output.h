// midi_output.h — MIDI OUT transport for LED feedback.
//
// Sends MIDI messages (note-on for pad LEDs, CC for fader rings) via:
//   - DIN: UART TX at 31250 baud
//   - USB: USB-MIDI endpoint (when USB host stack is active)
//
// RATE LIMITING: To avoid flooding controllers or DIN links (31250 baud ≈
// 1000 msg/sec max), this class tracks last-sent state per LED and only
// transmits on change. Total rate is capped (e.g. ≤100 msg/sec).
//
// This is the missing half of midi_input.cpp — bidirectional MIDI is
// required for LED feedback. See mdef_parser.h for controller definitions.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace glow {

// MIDI output manager with change detection and rate limiting.
class MidiOutput {
public:
  MidiOutput();
  
  // Initialize DIN MIDI OUT on given UART/TX pin. Returns false on failure.
  bool initDin(int uartNum, int txGpio);
  
  // Send a note-on message (for pad LEDs). velocity=0 means off.
  // Returns true if sent, false if suppressed (no change or rate-limited).
  bool sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
  
  // Send a CC message (for fader LED rings).
  // Returns true if sent, false if suppressed (no change or rate-limited).
  bool sendCC(uint8_t channel, uint8_t cc, uint8_t value);
  
  // Set rate limit (messages per second). Default 100.
  void setRateLimit(int msgsPerSec);
  
  // Reset all tracked state (useful when switching controllers).
  void reset();
  
  // Statistics
  size_t messagesSent() const { return messagesSent_; }
  size_t messagesSuppressed() const { return messagesSuppressed_; }

private:
  struct LedState {
    uint8_t lastValue = 0xFF;  // 0xFF = never sent
    bool isNote;               // true = note-on, false = CC
    uint8_t id;                // note or CC number
    uint8_t channel;           // MIDI channel (0-15)
  };
  
  // Check if this LED state changed; update tracking if so.
  bool shouldSend(bool isNote, uint8_t channel, uint8_t id, uint8_t value);
  
  // Write raw bytes to DIN UART.
  void writeDin(const uint8_t* data, size_t len);
  
  // Rate limiting
  bool canSendNow();
  
  int uartNum_ = -1;
  int txGpio_ = -1;
  int rateLimit_ = 100;  // msgs/sec
  uint64_t lastSendTimeUs_ = 0;
  int sendsThisSecond_ = 0;
  uint64_t currentSecondStartUs_ = 0;
  
  std::vector<LedState> ledStates_;  // dynamically sized as needed
  size_t messagesSent_ = 0;
  size_t messagesSuppressed_ = 0;
};

}  // namespace glow
