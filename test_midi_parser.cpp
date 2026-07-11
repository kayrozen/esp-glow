// test_midi_parser.cpp — host test for the MIDI byte-stream parser.
#include "midi_parser.h"

#include <cstdio>
#include <cstdint>

static int g_fail = 0;
#define CHECK(cond) do { \
  if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } \
} while (0)

static void test_note_on_basic() {
  printf("Test: Note On basic\n");
  MidiParser p;
  MidiEvent e;
  CHECK(!p.feed(0x90, e));   // status, wait for data
  CHECK(!p.feed(60, e));     // note, wait for velocity
  CHECK(p.feed(100, e));     // velocity -> complete
  CHECK(e.type == MidiEvent::NoteOn);
  CHECK(e.channel == 0);
  CHECK(e.data1 == 60);
  CHECK(e.data2 == 100);
}

static void test_running_status() {
  printf("Test: running status persists across messages\n");
  MidiParser p;
  MidiEvent e;
  p.feed(0x91, e); p.feed(60, e); p.feed(100, e);  // first Note On
  // Second Note On without re-sending status:
  CHECK(p.feed(62, e) == false);  // note
  CHECK(p.feed(110, e) == true);  // velocity -> complete
  CHECK(e.type == MidiEvent::NoteOn);
  CHECK(e.channel == 1);
  CHECK(e.data1 == 62);
  CHECK(e.data2 == 110);
}

static void test_program_change_one_data_byte() {
  printf("Test: Program Change is one data byte\n");
  MidiParser p;
  MidiEvent e;
  CHECK(!p.feed(0xC5, e));
  CHECK(p.feed(42, e));
  CHECK(e.type == MidiEvent::ProgramChange);
  CHECK(e.channel == 5);
  CHECK(e.data1 == 42);
  CHECK(e.data2 == 0);
}

static void test_realtime_does_not_disturb_running_status() {
  printf("Test: real-time byte interleaved mid-message\n");
  MidiParser p;
  MidiEvent e;
  p.feed(0x90, e);            // status
  p.feed(60, e);              // note
  // A timing clock (0xF8) arrives before velocity:
  CHECK(p.feed(0xF8, e) == true);
  CHECK(e.type == MidiEvent::SystemRealTime);
  CHECK(e.data1 == 0xF8);
  // The original Note On should still complete:
  CHECK(p.feed(100, e) == true);
  CHECK(e.type == MidiEvent::NoteOn);
  CHECK(e.data1 == 60);
  CHECK(e.data2 == 100);
}

static void test_note_off_vs_note_on_zero_velocity() {
  printf("Test: Note On with velocity 0 is a Note On event (caller decides)\n");
  MidiParser p;
  MidiEvent e;
  p.feed(0x92, e); p.feed(48, e); p.feed(0, e);
  CHECK(e.type == MidiEvent::NoteOn);  // parser is literal; caller maps v=0 to off
  CHECK(e.data2 == 0);
}

static void test_pitch_bend_two_bytes() {
  printf("Test: Pitch Bend lsb+msb\n");
  MidiParser p;
  MidiEvent e;
  p.feed(0xE0, e); p.feed(0x00, e); p.feed(0x40, e);
  CHECK(e.type == MidiEvent::PitchBend);
  CHECK(e.channel == 0);
  CHECK(e.data1 == 0x00);
  CHECK(e.data2 == 0x40);
}

static void test_control_change() {
  printf("Test: Control Change\n");
  MidiParser p;
  MidiEvent e;
  p.feed(0xB3, e); p.feed(7, e); p.feed(120, e);
  CHECK(e.type == MidiEvent::ControlChange);
  CHECK(e.channel == 3);
  CHECK(e.data1 == 7);     // CC 7 = volume
  CHECK(e.data2 == 120);
}

static void test_sysex_ignored() {
  printf("Test: SysEx is consumed without emitting\n");
  MidiParser p;
  MidiEvent e;
  CHECK(!p.feed(0xF0, e));   // sysex start
  CHECK(!p.feed(0x41, e));   // manufacturer id
  CHECK(!p.feed(0x10, e));   // data
  CHECK(!p.feed(0xF7, e));   // EOX -> no event
  // After sysex, a Note On should parse normally:
  p.feed(0x90, e); p.feed(60, e); CHECK(p.feed(100, e));
  CHECK(e.type == MidiEvent::NoteOn);
}

static void test_sysex_aborted_by_status() {
  printf("Test: status byte aborts an unterminated SysEx\n");
  MidiParser p;
  MidiEvent e;
  p.feed(0xF0, e); p.feed(0x41, e); p.feed(0x10, e);
  // A Note On status arrives without an EOX:
  p.feed(0x90, e); p.feed(60, e); CHECK(p.feed(100, e));
  CHECK(e.type == MidiEvent::NoteOn);
  CHECK(e.data1 == 60);
}

static void test_stray_data_bytes_ignored() {
  printf("Test: stray data bytes with no status are ignored\n");
  MidiParser p;
  MidiEvent e;
  CHECK(!p.feed(60, e));
  CHECK(!p.feed(100, e));
  // Now a real message:
  p.feed(0x90, e); p.feed(60, e); CHECK(p.feed(100, e));
  CHECK(e.type == MidiEvent::NoteOn);
}

static void test_data_bytes_for_status() {
  printf("Test: midiDataBytesForStatus table\n");
  CHECK(midiDataBytesForStatus(0x80) == 2);
  CHECK(midiDataBytesForStatus(0x90) == 2);
  CHECK(midiDataBytesForStatus(0xB0) == 2);
  CHECK(midiDataBytesForStatus(0xC0) == 1);
  CHECK(midiDataBytesForStatus(0xD0) == 1);
  CHECK(midiDataBytesForStatus(0xE0) == 2);
  CHECK(midiDataBytesForStatus(0xF0) == 255);
  CHECK(midiDataBytesForStatus(0xF1) == 1);
  CHECK(midiDataBytesForStatus(0xF2) == 2);
  CHECK(midiDataBytesForStatus(0xF6) == 0);
  CHECK(midiDataBytesForStatus(0xF8) == 0);  // real-time
  CHECK(midiDataBytesForStatus(0x40) == 0);  // not a status
}

int main() {
  test_note_on_basic();
  test_running_status();
  test_program_change_one_data_byte();
  test_realtime_does_not_disturb_running_status();
  test_note_off_vs_note_on_zero_velocity();
  test_pitch_bend_two_bytes();
  test_control_change();
  test_sysex_ignored();
  test_sysex_aborted_by_status();
  test_stray_data_bytes_ignored();
  test_data_bytes_for_status();
  if (g_fail == 0) {
    printf("All midi_parser tests passed!\n");
    return 0;
  }
  printf("%d midi_parser tests FAILED\n", g_fail);
  return 1;
}
