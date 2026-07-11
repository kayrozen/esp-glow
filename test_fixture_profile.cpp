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
  TEST("Reject version != 1");

  ProfileBuilder builder;
  builder.setFootprint(1).add(Capability::Dimmer, 0);
  std::vector<uint8_t> blob = builder.encode();

  // Change version to 2
  blob[4] = 2;

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

  if (g_failCount == 0) {
    printf("\nAll tests passed!\n");
    return 0;
  } else {
    printf("\n%d test(s) failed.\n", g_failCount);
    return 1;
  }
}
