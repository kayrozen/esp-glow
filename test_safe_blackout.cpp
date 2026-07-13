// test_safe_blackout.cpp — F5: safeBlackoutCore proves the two halves of
// "safe blackout" the header promises:
//   1. Fixture-mode universes fall back to profile defaults (shutter held
//      open, etc.), not just "whatever the last active cue left behind".
//   2. Raw-mode universes (pixel matrices bridged over Art-Net) are
//      explicitly zeroed — renderFrame never touches their contents on its
//      own, so without this, a matrix pattern's last frame would keep
//      streaming forever.
#include "safe_blackout.h"
#include "show.h"
#include "show_control.h"
#include "fixture_profile.h"
#include "profile_encoder.h"

#include <cstdio>
#include <cstring>

static int g_failCount = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      g_failCount++; \
    } \
  } while (0)

#define TEST(name) printf("Test: %s\n", name)

namespace {

struct ConstCapEffect : IEffect {
  uint16_t id;
  ConstCapEffect(uint16_t i) : id(i) {}
  void evaluate(float, std::vector<CapIntent>& caps, std::vector<AimIntent>&) override {
    caps.push_back({id, Capability::Dimmer, 1.0f});
    caps.push_back({id, Capability::ShutterStrobe, 1.0f});
  }
};

}  // namespace

void test_blackout_zeroes_and_holds_defaults() {
  TEST("safeBlackoutCore: stops cues (fixture universe falls back to "
       "profile defaults) and zeroes Raw universes");

  ProfileBuilder builder;
  builder.setFootprint(2)
      .add(Capability::Dimmer, 0, 0xFF, 0, false)
      .add(Capability::ShutterStrobe, 1, 0xFF, 200, false);  // "shutter open" default
  std::vector<uint8_t> blob = builder.encode();
  FixtureProfile p;
  CHECK(parseProfile(blob.data(), blob.size(), p));

  Show show;
  MockSink fixtureSink;
  MockSink rawSink;
  show.setUniverseCount(2);
  show.configureUniverse(0, UniverseMode::Fixture, &fixtureSink);
  show.configureUniverse(1, UniverseMode::Raw, &rawSink);
  uint16_t id = show.patch(p, 0, 0);

  ShowController controller;
  show.addEffect(&controller);

  ConstCapEffect fx(id);
  uint16_t cueId = controller.addCue({&fx}, 0.0f, 0.0f, 0, 0.0f);
  controller.go(cueId, 0.0f);

  // Simulate a live pixel-matrix pattern already having written non-zero
  // data into the Raw universe (main.cpp's on_pre_render does this every
  // frame via PixelMatrix::render + writeRawUniverse).
  uint8_t rawFrame[DMX_UNIVERSE_SIZE];
  std::memset(rawFrame, 0x7F, sizeof(rawFrame));
  show.writeRawUniverse(1, rawFrame, DMX_UNIVERSE_SIZE);

  show.renderFrame(0.0f);
  CHECK(controller.isActive(cueId));
  CHECK(fixtureSink.last[0] == 255);  // dimmer full
  CHECK(fixtureSink.last[1] == 255);  // shutter driven away from default
  CHECK(rawSink.last[0] == 0x7F);
  CHECK(rawSink.last[DMX_UNIVERSE_SIZE - 1] == 0x7F);

  safeBlackoutCore(show, controller);
  CHECK(!controller.isActive(cueId));
  CHECK(!controller.anyActive());

  // Render again with NO further writeRawUniverse call for universe 1 (the
  // caller — main.cpp — is responsible for not re-driving the pattern; see
  // safe_blackout.h). Fixture universe falls back to profile defaults on
  // its own since no cue is active; Raw universe stays at the zero
  // safeBlackoutCore wrote.
  show.renderFrame(1.0f);
  CHECK(fixtureSink.last[0] == 0);    // dimmer default
  CHECK(fixtureSink.last[1] == 200);  // shutter held at its profile default
  for (uint16_t i = 0; i < DMX_UNIVERSE_SIZE; ++i) {
    CHECK(rawSink.last[i] == 0);
  }
}

void test_blackout_idempotent_with_nothing_active() {
  TEST("safeBlackoutCore: safe to call with no cues ever active");

  Show show;
  MockSink sink;
  show.setUniverseCount(1);
  show.configureUniverse(0, UniverseMode::Fixture, &sink);

  ShowController controller;
  show.addEffect(&controller);

  safeBlackoutCore(show, controller);
  CHECK(!controller.anyActive());
  show.renderFrame(0.0f);
  CHECK(sink.sendCount == 1);
}

int main() {
  printf("=== safe_blackout Tests ===\n\n");

  test_blackout_zeroes_and_holds_defaults();
  test_blackout_idempotent_with_nothing_active();

  printf("\n=== Results ===\n");
  if (g_failCount == 0) {
    printf("All tests passed!\n");
    return 0;
  } else {
    printf("%d test(s) failed\n", g_failCount);
    return 1;
  }
}
