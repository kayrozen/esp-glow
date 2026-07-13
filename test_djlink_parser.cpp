// test_djlink_parser.cpp — host tests for the passive Pro DJ Link
// packet parsers. Byte offsets are checked against Deep Symmetry's
// dysentery protocol analysis -- see djlink_parser.h's header.
#include "djlink_parser.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_failCount = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      g_failCount++; \
    } \
  } while (0)

#define TEST(name) printf("Test: %s\n", name)

using glow::DjLinkBeatPacket;
using glow::parseDjLinkBeatPacket;
using glow::parseDjLinkMasterFlag;

// --- packet builders ---------------------------------------------------

// Builds a well-formed 0x60-byte Beat packet (port 50001, kind 0x28).
// bpmHundredths: the raw 2-byte BPM field (track BPM * 100).
// pitchRaw: the raw 3 "active" bytes of the Pitch field (0x100000 = no
// adjustment); byte 0x54 (the unused high byte) is always 0.
static std::vector<uint8_t> buildBeatPacket(uint8_t deviceNumber, uint16_t bpmHundredths,
                                            uint32_t pitchRaw, uint8_t beatInBar) {
  std::vector<uint8_t> pkt(0x60, 0);
  static const uint8_t magic[10] = {0x51, 0x73, 0x70, 0x74, 0x31, 0x57, 0x6d, 0x4a, 0x4f, 0x4c};
  std::memcpy(pkt.data(), magic, 10);
  pkt[0x0a] = 0x28;
  pkt[0x20] = 0x00;
  pkt[0x21] = deviceNumber;
  pkt[0x22] = 0x00;
  pkt[0x23] = 0x3c;
  pkt[0x54] = static_cast<uint8_t>((pitchRaw >> 24) & 0xFF);  // documented as always 0 in practice
  pkt[0x55] = static_cast<uint8_t>((pitchRaw >> 16) & 0xFF);
  pkt[0x56] = static_cast<uint8_t>((pitchRaw >> 8) & 0xFF);
  pkt[0x57] = static_cast<uint8_t>(pitchRaw & 0xFF);
  pkt[0x5a] = static_cast<uint8_t>((bpmHundredths >> 8) & 0xFF);
  pkt[0x5b] = static_cast<uint8_t>(bpmHundredths & 0xFF);
  pkt[0x5c] = beatInBar;
  pkt[0x5f] = deviceNumber;  // redundant copy, per beats.adoc
  return pkt;
}

// Builds a minimal CDJ Status packet (port 50002, kind 0x0a), long enough
// to contain the F status-flags byte at 0x89 -- nothing else in this
// large packet is populated (out of scope; see djlink_parser.h).
static std::vector<uint8_t> buildStatusPacket(uint8_t deviceNumber, bool isMaster, size_t totalLen = 0xd0) {
  std::vector<uint8_t> pkt(totalLen, 0);
  static const uint8_t magic[10] = {0x51, 0x73, 0x70, 0x74, 0x31, 0x57, 0x6d, 0x4a, 0x4f, 0x4c};
  std::memcpy(pkt.data(), magic, 10);
  pkt[0x0a] = 0x0a;
  pkt[0x21] = deviceNumber;
  pkt[0x89] = isMaster ? 0x20 : 0x00;  // bit 5 == Master
  return pkt;
}

// --- Beat packet parsing -----------------------------------------------

static void test_beat_packet_basic() {
  TEST("parseDjLinkBeatPacket: 120.00 BPM, no pitch adjustment, beat 1 of bar -> downbeat");
  auto pkt = buildBeatPacket(/*device=*/1, /*bpmHundredths=*/12000, /*pitchRaw=*/0x100000, /*beatInBar=*/1);
  DjLinkBeatPacket out{};
  CHECK(parseDjLinkBeatPacket(pkt.data(), pkt.size(), /*tUs=*/123456, out));
  CHECK(out.deviceNumber == 1);
  CHECK(std::fabs(out.event.bpm - 120.0f) < 0.01f);
  CHECK(out.event.beatInBar == 1);
  CHECK(out.event.isDownbeat == true);
  CHECK(out.event.tUs == 123456);
}

static void test_beat_packet_non_downbeat() {
  TEST("parseDjLinkBeatPacket: beat 3 of bar -> not a downbeat");
  auto pkt = buildBeatPacket(2, 12800, 0x100000, 3);
  DjLinkBeatPacket out{};
  CHECK(parseDjLinkBeatPacket(pkt.data(), pkt.size(), 0, out));
  CHECK(out.event.beatInBar == 3);
  CHECK(out.event.isDownbeat == false);
  CHECK(std::fabs(out.event.bpm - 128.0f) < 0.01f);
}

static void test_beat_packet_pitch_adjustment() {
  TEST("parseDjLinkBeatPacket: +6% pitch adjustment raises the effective BPM");
  // Pitch = 0x100000 * 1.06 = 1111490.56 -> round to 1111491 (0x10F983).
  uint32_t pitchRaw = static_cast<uint32_t>(0x100000 * 1.06);
  auto pkt = buildBeatPacket(1, 12000, pitchRaw, 2);
  DjLinkBeatPacket out{};
  CHECK(parseDjLinkBeatPacket(pkt.data(), pkt.size(), 0, out));
  CHECK(std::fabs(out.event.bpm - 127.2f) < 0.05f);  // 120 * 1.06
}

static void test_beat_packet_pitch_at_complete_stop() {
  TEST("parseDjLinkBeatPacket: pitch 0x000000 (Wide -100%, complete stop) -> effective BPM 0");
  auto pkt = buildBeatPacket(1, 12000, 0x000000, 1);
  DjLinkBeatPacket out{};
  CHECK(parseDjLinkBeatPacket(pkt.data(), pkt.size(), 0, out));
  CHECK(std::fabs(out.event.bpm - 0.0f) < 0.01f);
}

static void test_beat_packet_out_of_range_beat_in_bar_is_unknown() {
  TEST("parseDjLinkBeatPacket: a beat-in-bar value outside 1-4 is reported as unknown (0), not rejected");
  auto pkt = buildBeatPacket(1, 12000, 0x100000, 7);  // malformed field, rest of packet fine
  DjLinkBeatPacket out{};
  CHECK(parseDjLinkBeatPacket(pkt.data(), pkt.size(), 0, out));
  CHECK(out.event.beatInBar == 0);
  CHECK(out.event.isDownbeat == false);
}

// --- rejections ----------------------------------------------------------

static void test_beat_packet_wrong_magic_rejected() {
  TEST("parseDjLinkBeatPacket: wrong magic bytes -> rejected");
  auto pkt = buildBeatPacket(1, 12000, 0x100000, 1);
  pkt[3] = 0xFF;
  DjLinkBeatPacket out{};
  CHECK(!parseDjLinkBeatPacket(pkt.data(), pkt.size(), 0, out));
}

static void test_beat_packet_wrong_kind_rejected() {
  TEST("parseDjLinkBeatPacket: kind byte other than 0x28 -> rejected (e.g. a status packet)");
  auto pkt = buildStatusPacket(1, true);
  DjLinkBeatPacket out{};
  CHECK(!parseDjLinkBeatPacket(pkt.data(), pkt.size(), 0, out));
}

static void test_beat_packet_truncated_no_overread() {
  TEST("parseDjLinkBeatPacket: every truncation of a well-formed packet fails, no overread (ASan-checked)");
  auto full = buildBeatPacket(1, 12000, 0x100000, 1);
  for (size_t n = 0; n < full.size(); ++n) {
    uint8_t* buf = new uint8_t[n];
    for (size_t i = 0; i < n; ++i) buf[i] = full[i];
    DjLinkBeatPacket out{};
    bool ok = parseDjLinkBeatPacket(buf, n, 0, out);
    CHECK(ok == false);
    delete[] buf;
  }
}

static void test_beat_packet_null_and_empty() {
  TEST("parseDjLinkBeatPacket: null pointer / zero length -> false, never crashes");
  DjLinkBeatPacket out{};
  CHECK(!parseDjLinkBeatPacket(nullptr, 0, 0, out));
  uint8_t dummy = 0;
  CHECK(!parseDjLinkBeatPacket(&dummy, 0, 0, out));
}

// --- CDJ status master-flag parsing --------------------------------------

static void test_master_flag_true() {
  TEST("parseDjLinkMasterFlag: bit 5 of F set -> isMaster true");
  auto pkt = buildStatusPacket(/*device=*/2, /*isMaster=*/true);
  uint8_t dev = 0;
  bool master = false;
  CHECK(parseDjLinkMasterFlag(pkt.data(), pkt.size(), dev, master));
  CHECK(dev == 2);
  CHECK(master == true);
}

static void test_master_flag_false() {
  TEST("parseDjLinkMasterFlag: bit 5 of F clear -> isMaster false");
  auto pkt = buildStatusPacket(3, false);
  uint8_t dev = 0;
  bool master = true;
  CHECK(parseDjLinkMasterFlag(pkt.data(), pkt.size(), dev, master));
  CHECK(dev == 3);
  CHECK(master == false);
}

static void test_master_flag_other_bits_ignored() {
  TEST("parseDjLinkMasterFlag: other bits of F set alongside bit 5 don't change the reading");
  auto pkt = buildStatusPacket(1, true);
  pkt[0x89] |= 0x40;  // also set "Play" bit -- must not affect Master
  uint8_t dev = 0;
  bool master = false;
  CHECK(parseDjLinkMasterFlag(pkt.data(), pkt.size(), dev, master));
  CHECK(master == true);

  auto pkt2 = buildStatusPacket(1, false);
  pkt2[0x89] |= 0x40;
  master = true;
  CHECK(parseDjLinkMasterFlag(pkt2.data(), pkt2.size(), dev, master));
  CHECK(master == false);
}

static void test_master_flag_wrong_kind_rejected() {
  TEST("parseDjLinkMasterFlag: kind byte other than 0x0a -> rejected (e.g. a beat packet)");
  auto pkt = buildBeatPacket(1, 12000, 0x100000, 1);
  uint8_t dev = 0;
  bool master = false;
  CHECK(!parseDjLinkMasterFlag(pkt.data(), pkt.size(), dev, master));
}

static void test_master_flag_shorter_variant_still_readable() {
  TEST("parseDjLinkMasterFlag: the shortest documented CDJ status length (0xd0) is enough to read F");
  auto pkt = buildStatusPacket(1, true, 0xd0);
  uint8_t dev = 0;
  bool master = false;
  CHECK(parseDjLinkMasterFlag(pkt.data(), pkt.size(), dev, master));
  CHECK(master == true);
}

static void test_master_flag_too_short_rejected() {
  TEST("parseDjLinkMasterFlag: a packet too short to contain byte 0x89 -> rejected, no overread");
  auto full = buildStatusPacket(1, true, 0x8a);
  for (size_t n = 0; n < 0x8a; ++n) {
    uint8_t* buf = new uint8_t[n];
    for (size_t i = 0; i < n; ++i) buf[i] = full[i];
    uint8_t dev = 0;
    bool master = false;
    CHECK(!parseDjLinkMasterFlag(buf, n, dev, master));
    delete[] buf;
  }
}

int main() {
  test_beat_packet_basic();
  test_beat_packet_non_downbeat();
  test_beat_packet_pitch_adjustment();
  test_beat_packet_pitch_at_complete_stop();
  test_beat_packet_out_of_range_beat_in_bar_is_unknown();

  test_beat_packet_wrong_magic_rejected();
  test_beat_packet_wrong_kind_rejected();
  test_beat_packet_truncated_no_overread();
  test_beat_packet_null_and_empty();

  test_master_flag_true();
  test_master_flag_false();
  test_master_flag_other_bits_ignored();
  test_master_flag_wrong_kind_rejected();
  test_master_flag_shorter_variant_still_readable();
  test_master_flag_too_short_rejected();

  if (g_failCount == 0) {
    printf("\nAll tests passed.\n");
    return 0;
  }
  printf("\n%d test(s) failed.\n", g_failCount);
  return 1;
}
