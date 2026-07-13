// test_midi_realtime.cpp — host tests for MidiByteReader: the fix for
// realtime bytes (0xF8 Clock etc.) interleaved mid-message.
#include "midi_realtime.h"

#include <cstdio>

static int g_fail = 0;

#define CHECK(cond) do { \
  if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } \
} while (0)

#define TEST(name) printf("Test: %s\n", name)

using Result = MidiByteReader::Result;

static void test_plain_channel_message_no_realtime_bytes() {
  TEST("plain Note On, no realtime bytes -> ChannelMessage on the 3rd byte");
  MidiByteReader r;
  uint8_t msg[3];
  CHECK(r.feed(0x90, msg) == Result::None);
  CHECK(r.feed(0x40, msg) == Result::None);
  CHECK(r.feed(0x7F, msg) == Result::ChannelMessage);
  CHECK(msg[0] == 0x90 && msg[1] == 0x40 && msg[2] == 0x7F);
}

static void test_clock_byte_between_status_and_data1_does_not_corrupt_message() {
  TEST("0xF8 injected between status and data1 -> Note On still frames correctly");
  MidiByteReader r;
  uint8_t msg[3];
  CHECK(r.feed(0x90, msg) == Result::None);       // status
  for (int i = 0; i < 23; ++i) {                  // 23 clock pulses, not yet a beat
    CHECK(r.feed(0xF8, msg) == Result::None);
  }
  CHECK(r.feed(0x40, msg) == Result::None);       // data1 -- framing must be untouched
  CHECK(r.feed(0x7F, msg) == Result::ChannelMessage);  // data2 -- completes the SAME message
  CHECK(msg[0] == 0x90 && msg[1] == 0x40 && msg[2] == 0x7F);
}

static void test_clock_byte_between_data1_and_data2_does_not_corrupt_message() {
  TEST("0xF8 injected between data1 and data2 -> message still completes correctly");
  MidiByteReader r;
  uint8_t msg[3];
  CHECK(r.feed(0xB0, msg) == Result::None);   // CC status
  CHECK(r.feed(0x07, msg) == Result::None);   // data1
  CHECK(r.feed(0xF8, msg) == Result::None);   // clock pulse mid-message
  CHECK(r.feed(0x64, msg) == Result::ChannelMessage);  // data2 -- completes the CC, not a new message
  CHECK(msg[0] == 0xB0 && msg[1] == 0x07 && msg[2] == 0x64);
}

static void test_24th_pulse_is_a_beat_the_rest_are_not() {
  TEST("24 PPQN: only the 24th consecutive 0xF8 returns BeatPulse");
  MidiByteReader r;
  uint8_t msg[3];
  for (int i = 0; i < 23; ++i) {
    CHECK(r.feed(0xF8, msg) == Result::None);
  }
  CHECK(r.feed(0xF8, msg) == Result::BeatPulse);
  // Counter wraps: the next 23 are None again, the 24th is a beat again.
  for (int i = 0; i < 23; ++i) {
    CHECK(r.feed(0xF8, msg) == Result::None);
  }
  CHECK(r.feed(0xF8, msg) == Result::BeatPulse);
}

static void test_transport_bytes_recognized_and_reset_pulse_count() {
  TEST("Start/Stop/Continue are recognized and reset the pulse counter");
  MidiByteReader r;
  uint8_t msg[3];
  for (int i = 0; i < 10; ++i) r.feed(0xF8, msg);  // partial count, not yet a beat
  CHECK(r.feed(0xFA, msg) == Result::TransportStart);
  // Counter reset by Start: need a fresh 24 pulses for the next beat, not 14.
  for (int i = 0; i < 23; ++i) {
    CHECK(r.feed(0xF8, msg) == Result::None);
  }
  CHECK(r.feed(0xF8, msg) == Result::BeatPulse);

  CHECK(r.feed(0xFC, msg) == Result::TransportStop);
  CHECK(r.feed(0xFB, msg) == Result::TransportContinue);
}

static void test_transport_byte_mid_message_does_not_corrupt_framing() {
  TEST("0xFC Stop injected mid-message -> the channel message still completes");
  MidiByteReader r;
  uint8_t msg[3];
  CHECK(r.feed(0x80, msg) == Result::None);          // Note Off status
  CHECK(r.feed(0xFC, msg) == Result::TransportStop);  // Stop, interleaved
  CHECK(r.feed(0x40, msg) == Result::None);          // data1 of the Note Off, untouched
  CHECK(r.feed(0x00, msg) == Result::ChannelMessage);
  CHECK(msg[0] == 0x80 && msg[1] == 0x40 && msg[2] == 0x00);
}

static void test_undefined_and_active_sensing_and_reset_are_ignored() {
  TEST("0xF9/0xFE/0xFF -> None, and never disturb in-flight framing");
  MidiByteReader r;
  uint8_t msg[3];
  CHECK(r.feed(0x90, msg) == Result::None);
  CHECK(r.feed(0xF9, msg) == Result::None);
  CHECK(r.feed(0xFE, msg) == Result::None);
  CHECK(r.feed(0xFF, msg) == Result::None);
  CHECK(r.feed(0x50, msg) == Result::None);
  CHECK(r.feed(0x60, msg) == Result::ChannelMessage);
  CHECK(msg[0] == 0x90 && msg[1] == 0x50 && msg[2] == 0x60);
}

static void test_a_real_status_byte_still_resets_framing() {
  TEST("a genuine new status byte mid-message still restarts framing (unrelated to realtime bytes)");
  MidiByteReader r;
  uint8_t msg[3];
  CHECK(r.feed(0x90, msg) == Result::None);
  CHECK(r.feed(0x40, msg) == Result::None);
  // A new status byte arrives before data2 -- the previous partial message
  // is abandoned (this is standard MIDI framing, not a realtime-byte concern).
  CHECK(r.feed(0x80, msg) == Result::None);
  CHECK(r.feed(0x41, msg) == Result::None);
  CHECK(r.feed(0x00, msg) == Result::ChannelMessage);
  CHECK(msg[0] == 0x80 && msg[1] == 0x41 && msg[2] == 0x00);
}

int main() {
  test_plain_channel_message_no_realtime_bytes();
  test_clock_byte_between_status_and_data1_does_not_corrupt_message();
  test_clock_byte_between_data1_and_data2_does_not_corrupt_message();
  test_24th_pulse_is_a_beat_the_rest_are_not();
  test_transport_bytes_recognized_and_reset_pulse_count();
  test_transport_byte_mid_message_does_not_corrupt_framing();
  test_undefined_and_active_sensing_and_reset_are_ignored();
  test_a_real_status_byte_still_resets_framing();

  if (g_fail == 0) {
    printf("All midi_realtime tests passed!\n");
    return 0;
  }
  printf("%d midi_realtime tests FAILED\n", g_fail);
  return 1;
}
