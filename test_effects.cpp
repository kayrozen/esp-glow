#include "oscillator.h"
#include "color.h"
#include "effects.h"
#include "show.h"
#include "fixture_profile.h"
#include "profile_encoder.h"

#include <cmath>
#include <cstdio>

static int g_failCount = 0;
static constexpr float kEps = 1e-4f;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      g_failCount++; \
    } \
  } while (0)

#define CHECK_NEAR(a, b) CHECK(fabsf((a) - (b)) < kEps)

#define TEST(name) printf("Test: %s\n", name)

static void test_oscillator_sine() {
  TEST("oscillator sine");
  CHECK_NEAR(oscillator(Waveform::Sine, 0.0f), 0.5f);
  CHECK_NEAR(oscillator(Waveform::Sine, 0.25f), 1.0f);
  CHECK_NEAR(oscillator(Waveform::Sine, 0.5f), 0.5f);
  CHECK_NEAR(oscillator(Waveform::Sine, 0.75f), 0.0f);
}

static void test_oscillator_sawtooth() {
  TEST("oscillator sawtooth");
  CHECK_NEAR(oscillator(Waveform::Sawtooth, 0.0f), 0.0f);
  CHECK_NEAR(oscillator(Waveform::Sawtooth, 0.5f), 0.5f);
}

static void test_oscillator_triangle() {
  TEST("oscillator triangle");
  CHECK_NEAR(oscillator(Waveform::Triangle, 0.0f), 0.0f);
  CHECK_NEAR(oscillator(Waveform::Triangle, 0.25f), 0.5f);
  CHECK_NEAR(oscillator(Waveform::Triangle, 0.5f), 1.0f);
  CHECK_NEAR(oscillator(Waveform::Triangle, 0.75f), 0.5f);
}

static void test_oscillator_square() {
  TEST("oscillator square");
  CHECK_NEAR(oscillator(Waveform::Square, 0.25f), 0.0f);
  CHECK_NEAR(oscillator(Waveform::Square, 0.75f), 1.0f);
}

static void test_phase_from_time() {
  TEST("phaseFromTime");
  CHECK_NEAR(phaseFromTime(0.0f, 2.0f, 0.0f), 0.0f);
  CHECK_NEAR(phaseFromTime(1.0f, 2.0f, 0.0f), 0.5f);
  CHECK_NEAR(phaseFromTime(2.0f, 2.0f, 0.0f), 0.0f);
  CHECK_NEAR(phaseFromTime(1.0f, 2.0f, 0.5f), 0.0f);
}

static void test_oscillated_param() {
  TEST("OscillatedParam value");
  OscillatedParam p1{Waveform::Sawtooth, 2.0f, 0.0f, 0.0f, 100.0f};
  CHECK_NEAR(p1.value(1.0f), 50.0f);

  OscillatedParam p2{Waveform::Sine, 4.0f, 0.0f, 0.0f, 1.0f};
  CHECK_NEAR(p2.value(1.0f), 1.0f);
}

static void test_hsv_to_rgb_primaries() {
  TEST("hsvToRgb primaries");
  Rgb r = hsvToRgb(0.0f, 1.0f, 1.0f);
  CHECK_NEAR(r.r, 1.0f); CHECK_NEAR(r.g, 0.0f); CHECK_NEAR(r.b, 0.0f);

  Rgb g = hsvToRgb(1.0f / 3.0f, 1.0f, 1.0f);
  CHECK_NEAR(g.r, 0.0f); CHECK_NEAR(g.g, 1.0f); CHECK_NEAR(g.b, 0.0f);

  Rgb b = hsvToRgb(2.0f / 3.0f, 1.0f, 1.0f);
  CHECK_NEAR(b.r, 0.0f); CHECK_NEAR(b.g, 0.0f); CHECK_NEAR(b.b, 1.0f);
}

static void test_hsv_to_rgb_gray_and_wrap() {
  TEST("hsvToRgb gray and hue wrap");
  Rgb gray = hsvToRgb(0.0f, 0.0f, 0.5f);
  CHECK_NEAR(gray.r, 0.5f); CHECK_NEAR(gray.g, 0.5f); CHECK_NEAR(gray.b, 0.5f);

  Rgb wrapped = hsvToRgb(1.0f, 1.0f, 1.0f);
  CHECK_NEAR(wrapped.r, 1.0f); CHECK_NEAR(wrapped.g, 0.0f); CHECK_NEAR(wrapped.b, 0.0f);
}

static void test_dimmer_effect() {
  TEST("DimmerEffect");
  std::vector<uint16_t> ids{7};
  DimmerEffect fx(ids, 0.4f);
  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;
  fx.evaluate(0.0f, caps, aims);
  CHECK(caps.size() == 1);
  CHECK(caps[0].fixtureId == 7);
  CHECK(caps[0].cap == Capability::Dimmer);
  CHECK_NEAR(caps[0].norm01, 0.4f);
}

static void test_oscillated_dimmer_effect() {
  TEST("OscillatedDimmerEffect");
  std::vector<uint16_t> ids{7};
  OscillatedParam p{Waveform::Sawtooth, 2.0f, 0.0f, 0.0f, 1.0f};
  OscillatedDimmerEffect fx(ids, p);
  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;
  fx.evaluate(1.0f, caps, aims);
  CHECK(caps.size() == 1);
  CHECK(caps[0].fixtureId == 7);
  CHECK(caps[0].cap == Capability::Dimmer);
  CHECK_NEAR(caps[0].norm01, 0.5f);
}

static void test_hue_rotate_effect() {
  TEST("HueRotateEffect");
  std::vector<uint16_t> ids{7};
  HueRotateEffect fx(ids, 3.0f);
  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;
  fx.evaluate(0.0f, caps, aims);
  CHECK(caps.size() == 3);
  float r = 0, g = 0, b = 0;
  for (auto& c : caps) {
    if (c.cap == Capability::Red) r = c.norm01;
    if (c.cap == Capability::Green) g = c.norm01;
    if (c.cap == Capability::Blue) b = c.norm01;
  }
  CHECK_NEAR(r, 1.0f);
  CHECK_NEAR(g, 0.0f);
  CHECK_NEAR(b, 0.0f);
}

static void test_chase_effect() {
  TEST("ChaseEffect");
  std::vector<uint16_t> ids{10, 11, 12, 13};
  ChaseEffect fx(ids, 4.0f);

  {
    std::vector<CapIntent> caps;
    std::vector<AimIntent> aims;
    fx.evaluate(0.0f, caps, aims);
    CHECK(caps.size() == 4);
    for (auto& c : caps) {
      CHECK_NEAR(c.norm01, c.fixtureId == 10 ? 1.0f : 0.0f);
    }
  }
  {
    std::vector<CapIntent> caps;
    std::vector<AimIntent> aims;
    fx.evaluate(1.0f, caps, aims);
    for (auto& c : caps) {
      CHECK_NEAR(c.norm01, c.fixtureId == 11 ? 1.0f : 0.0f);
    }
  }
  {
    std::vector<CapIntent> caps;
    std::vector<AimIntent> aims;
    fx.evaluate(3.9f, caps, aims);
    for (auto& c : caps) {
      CHECK_NEAR(c.norm01, c.fixtureId == 13 ? 1.0f : 0.0f);
    }
  }
}

static void test_strobe_effect() {
  TEST("StrobeEffect");
  std::vector<uint16_t> ids{7};
  StrobeEffect fx(ids, 1.0f);
  {
    std::vector<CapIntent> caps;
    std::vector<AimIntent> aims;
    fx.evaluate(0.0f, caps, aims);
    CHECK(caps.size() == 1);
    CHECK_NEAR(caps[0].norm01, 0.0f);
  }
  {
    std::vector<CapIntent> caps;
    std::vector<AimIntent> aims;
    fx.evaluate(0.75f, caps, aims);
    CHECK_NEAR(caps[0].norm01, 1.0f);
  }
}

static void test_sweep_effect() {
  TEST("SweepEffect");
  SweepEffect fx(7, Vec3{0, 0, 1}, Vec3{0, 0, -1}, 4.0f);

  {
    std::vector<CapIntent> caps;
    std::vector<AimIntent> aims;
    fx.evaluate(0.0f, caps, aims);
    CHECK(aims.size() == 1);
    CHECK_NEAR(aims[0].target.x, 0.0f);
    CHECK_NEAR(aims[0].target.y, 0.0f);
    CHECK_NEAR(aims[0].target.z, 1.0f);
    CHECK(!aims[0].isPoint);
  }
  {
    std::vector<CapIntent> caps;
    std::vector<AimIntent> aims;
    fx.evaluate(1.0f, caps, aims);
    CHECK_NEAR(aims[0].target.z, 0.0f);
  }
  {
    std::vector<CapIntent> caps;
    std::vector<AimIntent> aims;
    fx.evaluate(2.0f, caps, aims);
    CHECK_NEAR(aims[0].target.z, -1.0f);
  }
}

static void test_show_integration() {
  TEST("OscillatedDimmerEffect through Show + MockSink");

  ProfileBuilder builder;
  builder.setFootprint(4)
      .add(Capability::Dimmer, 0, 0xFF, 0, false)
      .add(Capability::Red, 1, 0xFF, 0, false)
      .add(Capability::Green, 2, 0xFF, 0, false)
      .add(Capability::Blue, 3, 0xFF, 0, false);
  std::vector<uint8_t> blob = builder.encode();
  FixtureProfile p;
  bool ok = parseProfile(blob.data(), blob.size(), p);
  CHECK(ok);

  Show show;
  MockSink sink;
  show.setUniverseCount(1);
  show.configureUniverse(0, UniverseMode::Fixture, &sink);
  uint16_t id = show.patch(p, 0, 0);

  std::vector<uint16_t> ids{id};
  OscillatedParam op{Waveform::Sawtooth, 2.0f, 0.0f, 0.0f, 1.0f};
  OscillatedDimmerEffect fx(ids, op);
  show.addEffect(&fx);
  show.renderFrame(1.0f);

  CHECK(sink.last[0] == 127 || sink.last[0] == 128);
}

int main() {
  test_oscillator_sine();
  test_oscillator_sawtooth();
  test_oscillator_triangle();
  test_oscillator_square();
  test_phase_from_time();
  test_oscillated_param();
  test_hsv_to_rgb_primaries();
  test_hsv_to_rgb_gray_and_wrap();
  test_dimmer_effect();
  test_oscillated_dimmer_effect();
  test_hue_rotate_effect();
  test_chase_effect();
  test_strobe_effect();
  test_sweep_effect();
  test_show_integration();

  if (g_failCount == 0) {
    printf("All tests passed.\n");
    return 0;
  }
  printf("%d check(s) failed.\n", g_failCount);
  return 1;
}
