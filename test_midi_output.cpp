// test_midi_output.cpp — host tests for MIDI OUT with change detection and rate limiting.
#include "midi_output.h"

#include <cstdio>
#include <cstring>

static int g_fail = 0;

#define CHECK(cond) do { \
  if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } \
} while (0)

#define TEST(name) printf("Test: %s\n", name)

using namespace glow;

static void test_send_note_on() {
  TEST("sendNoteOn sends messages");
  MidiOutput out;
  // Host mode: always succeeds
  CHECK(out.initDin(1, 17));
  
  bool sent = out.sendNoteOn(0, 60, 127);
  CHECK(sent);
  CHECK(out.messagesSent() == 1);
}

static void test_send_cc() {
  TEST("sendCC sends messages");
  MidiOutput out;
  
  bool sent = out.sendCC(0, 48, 64);
  CHECK(sent);
  CHECK(out.messagesSent() == 1);
}

static void test_change_detection_same_value_suppressed() {
  TEST("sending same value twice suppresses second message");
  MidiOutput out;
  
  bool sent1 = out.sendNoteOn(0, 60, 127);
  CHECK(sent1);
  CHECK(out.messagesSent() == 1);
  
  bool sent2 = out.sendNoteOn(0, 60, 127);
  CHECK(!sent2);
  CHECK(out.messagesSent() == 1);
  CHECK(out.messagesSuppressed() == 1);
}

static void test_change_detection_different_value_sent() {
  TEST("sending different value sends new message");
  MidiOutput out;
  
  out.sendNoteOn(0, 60, 127);
  CHECK(out.messagesSent() == 1);
  
  bool sent = out.sendNoteOn(0, 60, 0);
  CHECK(sent);
  CHECK(out.messagesSent() == 2);
}

static void test_separate_notes_tracked_independently() {
  TEST("different notes tracked independently");
  MidiOutput out;
  
  out.sendNoteOn(0, 60, 127);
  out.sendNoteOn(0, 61, 64);
  CHECK(out.messagesSent() == 2);
  
  // Repeating note 60 should be suppressed
  bool sent60 = out.sendNoteOn(0, 60, 127);
  CHECK(!sent60);
  
  // Repeating note 61 should be suppressed
  bool sent61 = out.sendNoteOn(0, 61, 64);
  CHECK(!sent61);
  
  // Changing note 61 should send
  bool sent61new = out.sendNoteOn(0, 61, 127);
  CHECK(sent61new);
  CHECK(out.messagesSent() == 3);
}

static void test_note_and_cc_separate_namespaces() {
  TEST("note-on and CC are tracked separately");
  MidiOutput out;
  
  out.sendNoteOn(0, 60, 127);
  out.sendCC(0, 60, 127);
  CHECK(out.messagesSent() == 2);
  
  // Same ID but different message type should both be suppressed on repeat
  bool sentNote = out.sendNoteOn(0, 60, 127);
  CHECK(!sentNote);
  
  bool sentCC = out.sendCC(0, 60, 127);
  CHECK(!sentCC);
}

static void test_rate_limiting() {
  TEST("rate limiting suppresses excess messages");
  MidiOutput out;
  out.setRateLimit(5);  // 5 msgs/sec
  
  // First 5 should send
  for (int i = 0; i < 5; i++) {
    bool sent = out.sendNoteOn(0, static_cast<uint8_t>(i), 127);
    CHECK(sent);
  }
  CHECK(out.messagesSent() == 5);
  
  // 6th should be suppressed (host mode bypasses time check, so we can't fully test)
  // In host mode, canSendNow always returns true, so this test is limited
}

static void test_reset_clears_state() {
  TEST("reset clears all tracking state");
  MidiOutput out;
  
  out.sendNoteOn(0, 60, 127);
  out.sendCC(0, 48, 64);
  CHECK(out.messagesSent() == 2);
  
  out.reset();
  CHECK(out.messagesSent() == 0);
  CHECK(out.messagesSuppressed() == 0);
  
  // After reset, same values should send again (state cleared)
  bool sent = out.sendNoteOn(0, 60, 127);
  CHECK(sent);
  CHECK(out.messagesSent() == 1);
}

static void test_velocity_zero_means_off() {
  TEST("velocity 0 is treated as LED off");
  MidiOutput out;
  
  out.sendNoteOn(0, 60, 127);  // on
  CHECK(out.messagesSent() == 1);
  
  bool sentOff = out.sendNoteOn(0, 60, 0);  // off
  CHECK(sentOff);
  CHECK(out.messagesSent() == 2);
  
  // Repeating off should be suppressed
  bool sentOffAgain = out.sendNoteOn(0, 60, 0);
  CHECK(!sentOffAgain);
}

static void test_channel_variations() {
  TEST("different channels are distinct messages");
  MidiOutput out;
  
  out.sendNoteOn(0, 60, 127);
  out.sendNoteOn(1, 60, 127);
  out.sendNoteOn(2, 60, 127);
  
  // All different channels, so all should send
  CHECK(out.messagesSent() == 3);
}

int main() {
  test_send_note_on();
  test_send_cc();
  test_change_detection_same_value_suppressed();
  test_change_detection_different_value_sent();
  test_separate_notes_tracked_independently();
  test_note_and_cc_separate_namespaces();
  test_rate_limiting();
  test_reset_clears_state();
  test_velocity_zero_means_off();
  test_channel_variations();
  
  if (g_fail == 0) {
    printf("All midi_output tests passed!\n");
    return 0;
  }
  printf("%d midi_output tests FAILED\n", g_fail);
  return 1;
}
