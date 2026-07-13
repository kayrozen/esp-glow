#include "fixture_profile.h"
#include "profile_encoder.h"
#include <cstdio>
#include <cstring>
#include <cmath>

// Simple assert-style test macro
static int g_failCount = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      g_failCount++; \
    } \
  } while (0)

#define TEST(name) printf("Test: %s\n", name)

// Helper to compare floating-point with tolerance
bool floatEq(float a, float b, float tol = 0.5f) {
  return std::abs(a - b) <= tol;
}

void test_roundtrip() {
  TEST("Round-trip: encode, parse, verify");

  ProfileBuilder builder;
  builder
    .setFootprint(16)
    .add(Capability::Dimmer, 0, 0xFF, 0, false)
    .add(Capability::Red, 1, 0xFF, 0, false)
    .add(Capability::Green, 2, 0xFF, 0, false)
    .add(Capability::Blue, 3, 0xFF, 0, false)
    .add(Capability::Pan, 5, 6, 0, false)
    .add(Capability::Tilt, 7, 8, 0, false)
    .add(Capability::ShutterStrobe, 10, 0xFF, 8, false);
  builder.name = "Torrent";

  std::vector<uint8_t> blob = builder.encode();
  FixtureProfile p;
  CHECK(parseProfile(blob.data(), blob.size(), p));

  CHECK(p.footprint == 16);
  CHECK(p.channelCount == 7);

  // Verify each channel
  const ChannelMap* dimmer = findCapability(p, Capability::Dimmer);
  CHECK(dimmer != nullptr);
  CHECK(dimmer->coarse == 0);
  CHECK(dimmer->fine == 0xFF);
  CHECK(dimmer->defaultValue == 0);
  CHECK(dimmer->flags == 0);

  const ChannelMap* red = findCapability(p, Capability::Red);
  CHECK(red != nullptr);
  CHECK(red->coarse == 1);

  const ChannelMap* pan = findCapability(p, Capability::Pan);
  CHECK(pan != nullptr);
  CHECK(pan->coarse == 5);
  CHECK(pan->fine == 6);

  const ChannelMap* shutter = findCapability(p, Capability::ShutterStrobe);
  CHECK(shutter != nullptr);
  CHECK(shutter->defaultValue == 8);
}

void test_bad_magic() {
  TEST("Reject bad magic");

  ProfileBuilder builder;
  builder.setFootprint(1).add(Capability::Dimmer, 0);
  std::vector<uint8_t> blob = builder.encode();

  // Corrupt magic
  blob[0] = 'X';

  FixtureProfile p;
  CHECK(!parseProfile(blob.data(), blob.size(), p));
}

void test_bad_version() {
  TEST("Reject unsupported version (not 1 or 2)");

  ProfileBuilder builder;
  builder.setFootprint(1).add(Capability::Dimmer, 0);
  std::vector<uint8_t> blob = builder.encode();

  // Version 2 is now a supported version (see test_v2_roundtrip) -- use 3,
  // which is not.
  blob[4] = 3;

  FixtureProfile p;
  CHECK(!parseProfile(blob.data(), blob.size(), p));
}

void test_bad_flags() {
  TEST("Reject flags != 0");

  ProfileBuilder builder;
  builder.setFootprint(1).add(Capability::Dimmer, 0);
  std::vector<uint8_t> blob = builder.encode();

  // Set flags to non-zero
  blob[5] = 1;

  FixtureProfile p;
  CHECK(!parseProfile(blob.data(), blob.size(), p));
}

void test_cap_count_overflow() {
  TEST("Reject capCount > MAX_CAPS");

  // Manually construct a bad blob
  std::vector<uint8_t> blob;
  blob.push_back('P');
  blob.push_back('F');
  blob.push_back('X');
  blob.push_back('1');
  blob.push_back(1);   // version
  blob.push_back(0);   // flags
  blob.push_back(10);  // footprint
  blob.push_back(MAX_CAPS + 1);  // capCount > MAX_CAPS
  blob.push_back(0);   // nameLen

  FixtureProfile p;
  CHECK(!parseProfile(blob.data(), blob.size(), p));
}

void test_truncated_buffer() {
  TEST("Reject truncated buffer");

  ProfileBuilder builder;
  builder.setFootprint(1).add(Capability::Dimmer, 0);
  std::vector<uint8_t> blob = builder.encode();

  // Remove one byte
  blob.pop_back();

  FixtureProfile p;
  CHECK(!parseProfile(blob.data(), blob.size(), p));
}

void test_offset_at_footprint() {
  TEST("Reject offset == footprint");

  ProfileBuilder builder;
  builder.setFootprint(10);
  builder.add(Capability::Dimmer, 10);  // coarse at footprint boundary
  std::vector<uint8_t> blob = builder.encode();

  FixtureProfile p;
  CHECK(!parseProfile(blob.data(), blob.size(), p));
}

void test_apply_8bit() {
  TEST("applyCapability 8-bit");

  ProfileBuilder builder;
  builder.setFootprint(1).add(Capability::Dimmer, 0, 0xFF, 0);
  std::vector<uint8_t> blob = builder.encode();

  FixtureProfile p;
  CHECK(parseProfile(blob.data(), blob.size(), p));

  uint8_t buf[1] = {0};

  // norm 0.0 -> 0
  applyCapability(p, Capability::Dimmer, 0.0f, buf, 0);
  CHECK(buf[0] == 0);

  // norm 1.0 -> 255
  applyCapability(p, Capability::Dimmer, 1.0f, buf, 0);
  CHECK(buf[0] == 255);

  // norm 0.5 -> ~127
  applyCapability(p, Capability::Dimmer, 0.5f, buf, 0);
  CHECK(buf[0] == 128 || buf[0] == 127);
}

void test_apply_16bit() {
  TEST("applyCapability 16-bit (Pan)");

  ProfileBuilder builder;
  builder.setFootprint(10).add(Capability::Pan, 5, 6);
  std::vector<uint8_t> blob = builder.encode();

  FixtureProfile p;
  CHECK(parseProfile(blob.data(), blob.size(), p));

  uint8_t buf[10];
  std::memset(buf, 0, sizeof(buf));

  // norm 1.0 -> coarse 255, fine 255
  applyCapability(p, Capability::Pan, 1.0f, buf, 0);
  CHECK(buf[5] == 255);
  CHECK(buf[6] == 255);

  // Verify nothing else was written
  for (int i = 0; i < 10; ++i) {
    if (i != 5 && i != 6) {
      CHECK(buf[i] == 0);
    }
  }

  std::memset(buf, 0, sizeof(buf));

  // norm 0.5 -> coarse 127, fine 255 (32767 = 0x7FFF)
  applyCapability(p, Capability::Pan, 0.5f, buf, 0);
  CHECK(buf[5] == 127);
  CHECK(buf[6] == 255);

  std::memset(buf, 0, sizeof(buf));

  // norm 0.0 -> 0, 0
  applyCapability(p, Capability::Pan, 0.0f, buf, 0);
  CHECK(buf[5] == 0);
  CHECK(buf[6] == 0);
}

void test_inverted_8bit() {
  TEST("Inverted 8-bit flag");

  ProfileBuilder builder;
  builder.setFootprint(1).add(Capability::Dimmer, 0, 0xFF, 0, true);
  std::vector<uint8_t> blob = builder.encode();

  FixtureProfile p;
  CHECK(parseProfile(blob.data(), blob.size(), p));

  uint8_t buf[1] = {0};

  // norm 0.0 -> 255
  applyCapability(p, Capability::Dimmer, 0.0f, buf, 0);
  CHECK(buf[0] == 255);

  // norm 1.0 -> 0
  applyCapability(p, Capability::Dimmer, 1.0f, buf, 0);
  CHECK(buf[0] == 0);
}

void test_apply_defaults() {
  TEST("applyDefaults");

  ProfileBuilder builder;
  builder.setFootprint(16);
  builder.add(Capability::Dimmer, 0, 0xFF, 0);
  builder.add(Capability::ShutterStrobe, 10, 0xFF, 8);
  builder.add(Capability::Pan, 5, 6, 0);

  std::vector<uint8_t> blob = builder.encode();
  FixtureProfile p;
  CHECK(parseProfile(blob.data(), blob.size(), p));

  uint8_t buf[16];
  std::memset(buf, 0xFF, sizeof(buf));  // Pre-fill with 0xFF

  applyDefaults(p, buf, 0);

  // Dimmer default is 0
  CHECK(buf[0] == 0);

  // ShutterStrobe default is 8
  CHECK(buf[10] == 8);

  // Pan 16-bit: coarse = 0, fine = 0
  CHECK(buf[5] == 0);
  CHECK(buf[6] == 0);
}

void test_find_capability() {
  TEST("findCapability and hasCapability");

  ProfileBuilder builder;
  builder.setFootprint(16);
  builder.add(Capability::Dimmer, 0);
  builder.add(Capability::Pan, 5, 6);

  std::vector<uint8_t> blob = builder.encode();
  FixtureProfile p;
  CHECK(parseProfile(blob.data(), blob.size(), p));

  // Present capability
  const ChannelMap* dimmer = findCapability(p, Capability::Dimmer);
  CHECK(dimmer != nullptr);
  CHECK(dimmer->coarse == 0);

  // Present 16-bit capability
  const ChannelMap* pan = findCapability(p, Capability::Pan);
  CHECK(pan != nullptr);
  CHECK(pan->fine == 6);

  // Absent capability
  const ChannelMap* gobo = findCapability(p, Capability::Gobo);
  CHECK(gobo == nullptr);

  // hasCapability checks
  CHECK(hasCapability(p, Capability::Dimmer));
  CHECK(hasCapability(p, Capability::Pan));
  CHECK(!hasCapability(p, Capability::Gobo));
}

void test_clamp() {
  TEST("Clamp norm values");

  ProfileBuilder builder;
  builder.setFootprint(1).add(Capability::Dimmer, 0, 0xFF);
  std::vector<uint8_t> blob = builder.encode();

  FixtureProfile p;
  CHECK(parseProfile(blob.data(), blob.size(), p));

  uint8_t buf[1] = {0};

  // Clamp -0.5 -> 0
  applyCapability(p, Capability::Dimmer, -0.5f, buf, 0);
  CHECK(buf[0] == 0);

  // Clamp 2.0 -> 255
  applyCapability(p, Capability::Dimmer, 2.0f, buf, 0);
  CHECK(buf[0] == 255);
}

void test_header_too_short() {
  TEST("Reject buffer shorter than header");

  uint8_t shortBuf[8] = {0};

  FixtureProfile p;
  CHECK(!parseProfile(shortBuf, 8, p));
}

void test_apply_with_base_offset() {
  TEST("applyCapability with base offset");

  ProfileBuilder builder;
  builder.setFootprint(5).add(Capability::Red, 2, 0xFF);
  std::vector<uint8_t> blob = builder.encode();

  FixtureProfile p;
  CHECK(parseProfile(blob.data(), blob.size(), p));

  uint8_t buf[20];
  std::memset(buf, 0, sizeof(buf));

  uint16_t base = 10;

  applyCapability(p, Capability::Red, 0.5f, buf, base);

  // Should write at base+2 = 12
  CHECK(buf[12] == 128 || buf[12] == 127);

  // Everything else should be 0
  for (int i = 0; i < 20; ++i) {
    if (i != 12) {
      CHECK(buf[i] == 0);
    }
  }
}

// ============================================================================
// v2: function ranges
// ============================================================================

void test_v1_still_parses_no_ranges() {
  TEST("v1 blob still parses; rangeCount == 0, capabilities behave linearly");

  ProfileBuilder builder;  // no addRange calls -> encoder still emits v1 bytes
  builder.setFootprint(4).add(Capability::Dimmer, 0).add(Capability::Red, 1);
  std::vector<uint8_t> blob = builder.encode();
  CHECK(blob[4] == 1);

  FixtureProfile p;
  CHECK(parseProfile(blob.data(), blob.size(), p));
  CHECK(p.rangeCount == 0);
  CHECK(rangeCount(p, Capability::Dimmer) == 0);

  uint8_t buf[4] = {0};
  applyCapability(p, Capability::Dimmer, 1.0f, buf, 0);
  CHECK(buf[0] == 255);
}

void test_v1_legacy_hardcoded_blob_still_parses() {
  TEST("A hand-built (pre-v2) v1 byte sequence still parses identically");

  // FORMAT.md's worked "Torrent" example, verbatim -- a genuine legacy
  // blob, not something the current encoder produced.
  std::vector<uint8_t> blob = {
    'P', 'F', 'X', '1', 1, 0, 16, 7, 7,
    'T', 'o', 'r', 'r', 'e', 'n', 't',
    0x00, 0x00, 0xff, 0x00, 0x00,
    0x01, 0x01, 0xff, 0x00, 0x00,
    0x02, 0x02, 0xff, 0x00, 0x00,
    0x03, 0x03, 0xff, 0x00, 0x00,
    0x0a, 0x05, 0x06, 0x00, 0x00,
    0x0b, 0x07, 0x08, 0x00, 0x00,
    0x0c, 0x0a, 0xff, 0x08, 0x00,
  };
  CHECK(blob.size() == 51);

  FixtureProfile p;
  CHECK(parseProfile(blob.data(), blob.size(), p));
  CHECK(p.footprint == 16);
  CHECK(p.channelCount == 7);
  CHECK(p.rangeCount == 0);
  const ChannelMap* shutter = findCapability(p, Capability::ShutterStrobe);
  CHECK(shutter != nullptr && shutter->defaultValue == 8);
}

void test_v2_roundtrip() {
  TEST("v2 round-trip: encode with ranges, parse, verify every field");

  ProfileBuilder builder;
  builder.setFootprint(2).add(Capability::ColorWheel, 0).add(Capability::ShutterStrobe, 1);
  builder.addRange(0, 0, 9, false, "open");
  builder.addRange(0, 10, 19, false, "red");
  builder.addRange(0, 20, 29, false, "blue");
  builder.addRange(1, 0, 31, false, "closed");
  builder.addRange(1, 32, 63, true, "strobe");

  std::vector<uint8_t> blob = builder.encode();
  CHECK(blob[4] == 2);  // version bumped once ranges are present

  FixtureProfile p;
  CHECK(parseProfile(blob.data(), blob.size(), p));
  CHECK(p.footprint == 2);
  CHECK(p.channelCount == 2);
  CHECK(p.rangeCount == 5);

  CHECK(rangeCount(p, Capability::ColorWheel) == 3);
  CHECK(rangeCount(p, Capability::ShutterStrobe) == 2);
  CHECK(rangeCount(p, Capability::Gobo) == 0);  // absent capability

  CHECK(std::strcmp(rangeName(p, Capability::ColorWheel, 0), "open") == 0);
  CHECK(std::strcmp(rangeName(p, Capability::ColorWheel, 1), "red") == 0);
  CHECK(std::strcmp(rangeName(p, Capability::ColorWheel, 2), "blue") == 0);
  CHECK(!rangeIsContinuous(p, Capability::ColorWheel, 1));
  CHECK(rangeName(p, Capability::ColorWheel, 3) == nullptr);  // only 3 ranges

  CHECK(std::strcmp(rangeName(p, Capability::ShutterStrobe, 0), "closed") == 0);
  CHECK(!rangeIsContinuous(p, Capability::ShutterStrobe, 0));
  CHECK(std::strcmp(rangeName(p, Capability::ShutterStrobe, 1), "strobe") == 0);
  CHECK(rangeIsContinuous(p, Capability::ShutterStrobe, 1));
}

void test_range_discrete_centre() {
  TEST("applyRangeByName: discrete slot writes the centre, floor((from+to)/2)");

  ProfileBuilder builder;
  builder.setFootprint(1).add(Capability::ShutterStrobe, 0);
  builder.addRange(0, 32, 63, false, "open");
  std::vector<uint8_t> blob = builder.encode();
  FixtureProfile p;
  CHECK(parseProfile(blob.data(), blob.size(), p));

  uint8_t buf[1] = {0};
  CHECK(applyRangeByName(p, Capability::ShutterStrobe, "open", 0.0f, buf, 0));
  CHECK(buf[0] == 47);  // (32+63)/2 = 47 via integer division, not 48

  buf[0] = 0;
  CHECK(applyRangeByIndex(p, Capability::ShutterStrobe, 0, 0.0f, buf, 0));
  CHECK(buf[0] == 47);
}

void test_range_continuous() {
  TEST("applyRangeByName: continuous sub-range maps [0,1] across [dmxFrom,dmxTo]");

  ProfileBuilder builder;
  builder.setFootprint(1).add(Capability::ShutterStrobe, 0);
  builder.addRange(0, 64, 95, true, "strobe");
  std::vector<uint8_t> blob = builder.encode();
  FixtureProfile p;
  CHECK(parseProfile(blob.data(), blob.size(), p));

  uint8_t buf[1] = {0};
  CHECK(applyRangeByName(p, Capability::ShutterStrobe, "strobe", 0.0f, buf, 0));
  CHECK(buf[0] == 64);
  CHECK(applyRangeByName(p, Capability::ShutterStrobe, "strobe", 1.0f, buf, 0));
  CHECK(buf[0] == 95);
  CHECK(applyRangeByName(p, Capability::ShutterStrobe, "strobe", 0.5f, buf, 0));
  CHECK(buf[0] == 80);  // 64 + round(0.5*31) = 64 + 16
}

void test_range_unknown_noop() {
  TEST("applyRangeByName/Index: unknown name/index/capability -> false, buffer untouched");

  ProfileBuilder builder;
  builder.setFootprint(1).add(Capability::ShutterStrobe, 0);
  builder.addRange(0, 0, 31, false, "closed");
  std::vector<uint8_t> blob = builder.encode();
  FixtureProfile p;
  CHECK(parseProfile(blob.data(), blob.size(), p));

  uint8_t buf[1] = {77};
  CHECK(!applyRangeByName(p, Capability::ShutterStrobe, "nope", 0.0f, buf, 0));
  CHECK(buf[0] == 77);
  CHECK(!applyRangeByIndex(p, Capability::ShutterStrobe, 5, 0.0f, buf, 0));
  CHECK(buf[0] == 77);

  // Capability entirely absent from the fixture.
  CHECK(!applyRangeByName(p, Capability::Gobo, "closed", 0.0f, buf, 0));
  CHECK(!applyRangeByIndex(p, Capability::Gobo, 0, 0.0f, buf, 0));
  CHECK(buf[0] == 77);
}

void test_capability_without_ranges() {
  TEST("Capability with no ranges: applyCapability linear as always, applyRangeBy* false");

  ProfileBuilder builder;
  builder.setFootprint(1).add(Capability::Dimmer, 0);
  std::vector<uint8_t> blob = builder.encode();
  CHECK(blob[4] == 1);  // no ranges anywhere in this profile -> still v1 wire format

  FixtureProfile p;
  CHECK(parseProfile(blob.data(), blob.size(), p));

  uint8_t buf[1] = {0};
  applyCapability(p, Capability::Dimmer, 1.0f, buf, 0);
  CHECK(buf[0] == 255);

  CHECK(!applyRangeByName(p, Capability::Dimmer, "anything", 0.5f, buf, 0));
  CHECK(!applyRangeByIndex(p, Capability::Dimmer, 0, 0.5f, buf, 0));
}

void test_range_16bit_coarse_only() {
  TEST("Ranges on a 16-bit capability touch only the coarse channel");

  ProfileBuilder builder;
  builder.setFootprint(10).add(Capability::Pan, 5, 6);
  builder.addRange(0, 32, 63, false, "centre-ish");
  std::vector<uint8_t> blob = builder.encode();
  FixtureProfile p;
  CHECK(parseProfile(blob.data(), blob.size(), p));

  uint8_t buf[10];
  std::memset(buf, 0xAB, sizeof(buf));
  CHECK(applyRangeByName(p, Capability::Pan, "centre-ish", 0.0f, buf, 0));
  CHECK(buf[5] == 47);    // coarse written (centre of [32,63])
  CHECK(buf[6] == 0xAB);  // fine untouched
}

void test_v2_rangecount_overflow() {
  TEST("Reject rangeCount > MAX_RANGES");

  std::vector<uint8_t> blob = {'P', 'F', 'X', '1', 2, 0, 1, 0, 0,
                                static_cast<uint8_t>((MAX_RANGES + 1) & 0xFF),
                                static_cast<uint8_t>(((MAX_RANGES + 1) >> 8) & 0xFF)};
  FixtureProfile p;
  CHECK(!parseProfile(blob.data(), blob.size(), p));
}

void test_v2_dmxfrom_after_dmxto() {
  TEST("Reject range record with dmxFrom > dmxTo");

  ProfileBuilder builder;
  builder.setFootprint(1).add(Capability::Dimmer, 0);
  builder.addRange(0, 50, 10, false, "bad");  // from > to
  std::vector<uint8_t> blob = builder.encode();
  FixtureProfile p;
  CHECK(!parseProfile(blob.data(), blob.size(), p));
}

void test_v2_capindex_out_of_range() {
  TEST("Reject range record whose capIndex >= capCount");

  ProfileBuilder builder;
  builder.setFootprint(1).add(Capability::Dimmer, 0);
  builder.addRange(5, 0, 9, false, "bad");  // only 1 capability exists (index 0)
  std::vector<uint8_t> blob = builder.encode();
  FixtureProfile p;
  CHECK(!parseProfile(blob.data(), blob.size(), p));
}

void test_v2_nameoff_past_blob() {
  TEST("Reject range record whose nameOff points past the name blob");

  ProfileBuilder builder;
  builder.setFootprint(1).add(Capability::Dimmer, 0);
  builder.addRange(0, 0, 9, false, "ok");  // name blob is "ok\0" = 3 bytes
  std::vector<uint8_t> blob = builder.encode();

  size_t rangeRecOffset = blob.size() - 7 - 3;
  blob[rangeRecOffset + 4] = 0xFE;  // nameOff low byte
  blob[rangeRecOffset + 5] = 0x00;  // nameOff = 254, past the 3-byte blob

  FixtureProfile p;
  CHECK(!parseProfile(blob.data(), blob.size(), p));
}

void test_v2_truncated_header() {
  TEST("Reject v2 buffer shorter than the 11-byte v2 header");

  uint8_t buf[10] = {'P', 'F', 'X', '1', 2, 0, 1, 0, 0, 0};
  FixtureProfile p;
  CHECK(!parseProfile(buf, sizeof(buf), p));
}

void test_v2_truncated_range_table() {
  TEST("Reject v2 buffer truncated inside the range table / name blob");

  ProfileBuilder builder;
  builder.setFootprint(1).add(Capability::Dimmer, 0);
  builder.addRange(0, 0, 9, false, "ok");
  std::vector<uint8_t> blob = builder.encode();
  blob.resize(blob.size() - 5);

  FixtureProfile p;
  CHECK(!parseProfile(blob.data(), blob.size(), p));
}

void test_v2_name_blob_too_large() {
  TEST("Reject v2 name blob larger than MAX_RANGE_NAME_BLOB");

  ProfileBuilder builder;
  builder.setFootprint(1).add(Capability::Dimmer, 0);
  builder.addRange(0, 0, 9, false, "x");
  std::vector<uint8_t> blob = builder.encode();
  // Padding bytes appended here become part of the trailing name blob
  // (defined as "everything remaining to the end of the buffer").
  blob.resize(blob.size() + MAX_RANGE_NAME_BLOB + 1, 0);

  FixtureProfile p;
  CHECK(!parseProfile(blob.data(), blob.size(), p));
}

int main() {
  test_roundtrip();
  test_bad_magic();
  test_bad_version();
  test_bad_flags();
  test_cap_count_overflow();
  test_truncated_buffer();
  test_offset_at_footprint();
  test_apply_8bit();
  test_apply_16bit();
  test_inverted_8bit();
  test_apply_defaults();
  test_find_capability();
  test_clamp();
  test_header_too_short();
  test_apply_with_base_offset();

  test_v1_still_parses_no_ranges();
  test_v1_legacy_hardcoded_blob_still_parses();
  test_v2_roundtrip();
  test_range_discrete_centre();
  test_range_continuous();
  test_range_unknown_noop();
  test_capability_without_ranges();
  test_range_16bit_coarse_only();
  test_v2_rangecount_overflow();
  test_v2_dmxfrom_after_dmxto();
  test_v2_capindex_out_of_range();
  test_v2_nameoff_past_blob();
  test_v2_truncated_header();
  test_v2_truncated_range_table();
  test_v2_name_blob_too_large();

  if (g_failCount == 0) {
    printf("\nAll tests passed!\n");
    return 0;
  } else {
    printf("\n%d test(s) failed.\n", g_failCount);
    return 1;
  }
}
