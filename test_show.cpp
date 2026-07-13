#include "show.h"
#include "fixture_profile.h"
#include "profile_encoder.h"
#include "vec_math.h"
#include "aim.h"

#include <cstdio>

static int g_failCount = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      g_failCount++; \
    } \
  } while (0)

#define TEST(name) printf("Test: %s\n", name)

static FixtureProfile buildProfile(const ProfileBuilder& builder) {
  std::vector<uint8_t> blob = builder.encode();
  FixtureProfile p;
  bool ok = parseProfile(blob.data(), blob.size(), p);
  CHECK(ok);
  return p;
}

static MovingHeadConfig identityHead() {
  MovingHeadConfig cfg{};
  cfg.position = {0.0f, 0.0f, 0.0f};
  cfg.orientation = identity3();
  cfg.panRangeDeg = 540.0f;
  cfg.tiltRangeDeg = 270.0f;
  cfg.panCenterNorm = 0.5f;
  cfg.tiltCenterNorm = 0.5f;
  cfg.invertPan = false;
  cfg.invertTilt = false;
  return cfg;
}

void test_static_color() {
  TEST("Static color resolves to channels");

  ProfileBuilder builder;
  builder.setFootprint(4)
      .add(Capability::Dimmer, 0, 0xFF, 0, false)
      .add(Capability::Red, 1, 0xFF, 0, false)
      .add(Capability::Green, 2, 0xFF, 0, false)
      .add(Capability::Blue, 3, 0xFF, 0, false);
  FixtureProfile p = buildProfile(builder);

  Show show;
  MockSink sink;
  show.setUniverseCount(1);
  show.configureUniverse(0, UniverseMode::Fixture, &sink);
  uint16_t id = show.patch(p, 0, 0);

  StaticColorEffect fx(id, 1.0f, 0.0f, 0.0f);
  show.addEffect(&fx);
  show.renderFrame(0.0f);

  CHECK(sink.last[1] == 255);
  CHECK(sink.last[2] == 0);
  CHECK(sink.last[3] == 0);
  CHECK(sink.sendCount == 1);
  CHECK(sink.lastIndex == 0);
}

void test_defaults_reset_each_frame() {
  TEST("Defaults reset each frame");

  ProfileBuilder builder;
  builder.setFootprint(1)
      .add(Capability::ShutterStrobe, 0, 0xFF, 8, false);
  FixtureProfile p = buildProfile(builder);

  Show show;
  MockSink sink;
  show.setUniverseCount(1);
  show.configureUniverse(0, UniverseMode::Fixture, &sink);
  uint16_t id = show.patch(p, 0, 0);

  struct ShutterEffect : IEffect {
    uint16_t fixtureId;
    explicit ShutterEffect(uint16_t id) : fixtureId(id) {}
    void evaluate(float, std::vector<CapIntent>& caps, std::vector<AimIntent>&) override {
      caps.push_back({fixtureId, Capability::ShutterStrobe, 1.0f});
    }
  } fx(id);

  show.addEffect(&fx);
  show.renderFrame(0.0f);
  CHECK(sink.last[0] == 255);

  show.removeAllEffects();
  show.renderFrame(1.0f);
  CHECK(sink.last[0] == 8);
}

void test_moving_head_aim() {
  TEST("Moving head aim resolves pan/tilt");

  ProfileBuilder builder;
  builder.setFootprint(4)
      .add(Capability::Pan, 0, 1, 0, false)
      .add(Capability::Tilt, 2, 3, 0, false);
  FixtureProfile p = buildProfile(builder);

  Show show;
  MockSink sink;
  show.setUniverseCount(1);
  show.configureUniverse(0, UniverseMode::Fixture, &sink);
  MovingHeadConfig cfg = identityHead();
  uint16_t id = show.patchHead(p, 0, 0, cfg);

  AimPointEffect fx(id, Vec3{0.0f, 0.0f, 10.0f});
  show.addEffect(&fx);
  show.renderFrame(0.0f);

  CHECK(sink.last[0] == 0x7F);
  CHECK(sink.last[1] == 0xFF);
  CHECK(sink.last[2] == 0x7F);
  CHECK(sink.last[3] == 0xFF);
}

void test_multi_universe_routing() {
  TEST("Multi-universe routing");

  ProfileBuilder builder;
  builder.setFootprint(1).add(Capability::Dimmer, 0, 0xFF, 0, false);
  FixtureProfile p = buildProfile(builder);

  Show show;
  MockSink sink0, sink1;
  show.setUniverseCount(2);
  show.configureUniverse(0, UniverseMode::Fixture, &sink0);
  show.configureUniverse(1, UniverseMode::Fixture, &sink1);
  show.patch(p, 0, 0);
  show.patch(p, 1, 0);

  show.renderFrame(0.0f);

  CHECK(sink0.sendCount == 1);
  CHECK(sink0.lastIndex == 0);
  CHECK(sink1.sendCount == 1);
  CHECK(sink1.lastIndex == 1);
}

void test_last_write_wins() {
  TEST("Last write wins for duplicate cap intents");

  ProfileBuilder builder;
  builder.setFootprint(4)
      .add(Capability::Dimmer, 0, 0xFF, 0, false)
      .add(Capability::Red, 1, 0xFF, 0, false)
      .add(Capability::Green, 2, 0xFF, 0, false)
      .add(Capability::Blue, 3, 0xFF, 0, false);
  FixtureProfile p = buildProfile(builder);

  Show show;
  MockSink sink;
  show.setUniverseCount(1);
  show.configureUniverse(0, UniverseMode::Fixture, &sink);
  uint16_t id = show.patch(p, 0, 0);

  StaticColorEffect fx1(id, 1.0f, 0.0f, 0.0f);
  StaticColorEffect fx2(id, 0.0f, 1.0f, 0.0f);
  show.addEffect(&fx1);
  show.addEffect(&fx2);
  show.renderFrame(0.0f);

  CHECK(sink.last[1] == 0);
  CHECK(sink.last[2] == 255);
}

void test_raw_universe_passthrough() {
  TEST("Raw universe passthrough alongside fixture universe");

  ProfileBuilder builder;
  builder.setFootprint(4)
      .add(Capability::Dimmer, 0, 0xFF, 0, false)
      .add(Capability::Red, 1, 0xFF, 0, false)
      .add(Capability::Green, 2, 0xFF, 0, false)
      .add(Capability::Blue, 3, 0xFF, 0, false);
  FixtureProfile p = buildProfile(builder);

  Show show;
  MockSink sink0, sink1;
  show.setUniverseCount(2);
  show.configureUniverse(0, UniverseMode::Fixture, &sink0);
  show.configureUniverse(1, UniverseMode::Raw, &sink1);
  uint16_t id = show.patch(p, 0, 0);

  uint8_t raw[DMX_UNIVERSE_SIZE];
  for (int i = 0; i < DMX_UNIVERSE_SIZE; ++i) raw[i] = static_cast<uint8_t>(i & 0xFF);
  show.writeRawUniverse(1, raw, DMX_UNIVERSE_SIZE);

  StaticColorEffect fx(id, 1.0f, 0.0f, 0.0f);
  show.addEffect(&fx);
  show.renderFrame(0.0f);

  bool rawMatches = true;
  for (int i = 0; i < DMX_UNIVERSE_SIZE; ++i) {
    if (sink1.last[i] != raw[i]) { rawMatches = false; break; }
  }
  CHECK(rawMatches);
  CHECK(sink0.last[1] == 255);
}

void test_unknown_fixture_id_safe_noop() {
  TEST("Unknown fixture id is a safe no-op");

  ProfileBuilder builder;
  builder.setFootprint(1).add(Capability::Dimmer, 0, 0xFF, 0, false);
  FixtureProfile p = buildProfile(builder);

  Show show;
  MockSink sink;
  show.setUniverseCount(1);
  show.configureUniverse(0, UniverseMode::Fixture, &sink);
  show.patch(p, 0, 0);

  StaticColorEffect fx(999, 1.0f, 1.0f, 1.0f);
  AimPointEffect afx(999, Vec3{0.0f, 0.0f, 1.0f});
  show.addEffect(&fx);
  show.addEffect(&afx);
  show.renderFrame(0.0f);

  CHECK(sink.sendCount == 1);
  CHECK(sink.last[0] == 0);
}

void test_universe_data_read_accessor() {
  TEST("universeData() reads back a Fixture universe's resolved bytes and a Raw universe's blit");

  ProfileBuilder builder;
  builder.setFootprint(1).add(Capability::Red, 0, 0xFF, 0, false);
  FixtureProfile p = buildProfile(builder);

  Show show;
  MockSink sink0, sink1;
  show.setUniverseCount(2);
  show.configureUniverse(0, UniverseMode::Fixture, &sink0);
  show.configureUniverse(1, UniverseMode::Raw, &sink1);
  uint16_t id = show.patch(p, 0, 0);

  // 200/255 -- the exact value the HIL selftest fixture drives channel 0 to
  // (see main.cpp's setup_selftest_fixture): round(200/255 * 255) == 200
  // with no rounding slop, so this doubles as a regression check for that
  // fixture's math.
  StaticColorEffect fx(id, static_cast<float>(200) / 255.0f, 0.0f, 0.0f);
  show.addEffect(&fx);
  show.renderFrame(0.0f);
  CHECK(show.universeData(0)[0] == 200);

  uint8_t raw[DMX_UNIVERSE_SIZE] = {0};
  raw[5] = 77;
  show.writeRawUniverse(1, raw, DMX_UNIVERSE_SIZE);
  CHECK(show.universeData(1)[5] == 77);

  CHECK(show.universeData(MAX_UNIVERSES) == nullptr);
}

int main() {
  test_static_color();
  test_defaults_reset_each_frame();
  test_moving_head_aim();
  test_multi_universe_routing();
  test_last_write_wins();
  test_raw_universe_passthrough();
  test_unknown_fixture_id_safe_noop();
  test_universe_data_read_accessor();

  if (g_failCount == 0) {
    printf("All tests passed.\n");
    return 0;
  }
  printf("%d check(s) failed.\n", g_failCount);
  return 1;
}
