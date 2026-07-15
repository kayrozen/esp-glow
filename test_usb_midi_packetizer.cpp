// test_usb_midi_packetizer.cpp — USB-MIDI 1.0 event-packet framing for
// outbound SysEx (P1.1's USB-MIDI OUT path, usb_midi_input.cpp). Pure
// packing logic, host-tested; the actual OUT-transfer submission has no
// host equivalent (needs a real/emulated USB host controller).

#include "usb_midi_packetizer.h"

#include <cstdio>
#include <vector>

static int g_failCount = 0;

#define CHECK(cond)                                           \
  do {                                                        \
    if (!(cond)) {                                            \
      printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      g_failCount++;                                          \
    }                                                          \
  } while (0)

#define TEST(name) printf("Test: %s\n", name)

void test_empty_input_appends_nothing() {
  TEST("len==0 appends nothing");
  std::vector<uint8_t> out;
  packUsbMidiEventPackets(nullptr, 0, out);
  CHECK(out.empty());
}

void test_one_byte_cin5() {
  TEST("1-byte message -> one packet, CIN 0x5, zero-padded");
  uint8_t msg[] = {0xF7};
  std::vector<uint8_t> out;
  packUsbMidiEventPackets(msg, sizeof(msg), out);
  std::vector<uint8_t> expected = {0x05, 0xF7, 0x00, 0x00};
  CHECK(out == expected);
}

void test_two_bytes_cin6() {
  TEST("2-byte message -> one packet, CIN 0x6, zero-padded");
  uint8_t msg[] = {0xF0, 0xF7};
  std::vector<uint8_t> out;
  packUsbMidiEventPackets(msg, sizeof(msg), out);
  std::vector<uint8_t> expected = {0x06, 0xF0, 0xF7, 0x00};
  CHECK(out == expected);
}

void test_three_bytes_cin7() {
  TEST("3-byte message -> one packet, CIN 0x7, no padding needed");
  uint8_t msg[] = {0xF0, 0x47, 0xF7};
  std::vector<uint8_t> out;
  packUsbMidiEventPackets(msg, sizeof(msg), out);
  std::vector<uint8_t> expected = {0x07, 0xF0, 0x47, 0xF7};
  CHECK(out == expected);
}

void test_twelve_bytes_three_continue_one_final() {
  TEST("12-byte APC40 mkII intro message -> 4 packets: CIN 4,4,4,7");
  uint8_t msg[] = {0xF0, 0x47, 0x7F, 0x29, 0x60, 0x00, 0x04, 0x40, 0x00, 0x00, 0x00, 0xF7};
  std::vector<uint8_t> out;
  packUsbMidiEventPackets(msg, sizeof(msg), out);
  CHECK(out.size() == 16);  // 4 packets * 4 bytes
  std::vector<uint8_t> expected = {
    0x04, 0xF0, 0x47, 0x7F,
    0x04, 0x29, 0x60, 0x00,
    0x04, 0x04, 0x40, 0x00,
    0x07, 0x00, 0x00, 0xF7,
  };
  CHECK(out == expected);
}

void test_six_bytes_one_continue_one_final() {
  TEST("6-byte message -> 2 packets: CIN 4, 7");
  uint8_t msg[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
  std::vector<uint8_t> out;
  packUsbMidiEventPackets(msg, sizeof(msg), out);
  std::vector<uint8_t> expected = {
    0x04, 0xF0, 0x7E, 0x7F,
    0x07, 0x06, 0x01, 0xF7,
  };
  CHECK(out == expected);
}

void test_appends_without_clearing() {
  TEST("Packing a second blob appends after the first, doesn't clear `out`");
  uint8_t a[] = {0xF7};
  uint8_t b[] = {0xF0, 0xF7};
  std::vector<uint8_t> out;
  packUsbMidiEventPackets(a, sizeof(a), out);
  packUsbMidiEventPackets(b, sizeof(b), out);
  CHECK(out.size() == 8);
  std::vector<uint8_t> expected = {
    0x05, 0xF7, 0x00, 0x00,
    0x06, 0xF0, 0xF7, 0x00,
  };
  CHECK(out == expected);
}

int main() {
  test_empty_input_appends_nothing();
  test_one_byte_cin5();
  test_two_bytes_cin6();
  test_three_bytes_cin7();
  test_twelve_bytes_three_continue_one_final();
  test_six_bytes_one_continue_one_final();
  test_appends_without_clearing();

  if (g_failCount == 0) {
    printf("\nAll tests passed!\n");
    return 0;
  }
  printf("\n%d test(s) FAILED\n", g_failCount);
  return 1;
}
