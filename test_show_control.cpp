#include "show_control.h"
#include "fixture_profile.h"
#include "profile_encoder.h"
#include "vec_math.h"

#include <cstdio>
#include <cmath>

static int g_failCount = 0;
static constexpr float EPSILON = 1e-4f;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      g_failCount++; \
    } \
  } while (0)

#define CHECK_NEAR(a, b) \
  do { \
    if (std::fabs((a) - (b)) > EPSILON) { \
      printf("FAIL: %s:%d: %f != %f (diff %f)\n", __FILE__, __LINE__, (a), (b), \
             std::fabs((a) - (b))); \
      g_failCount++; \
    } \
  } while (0)

#define TEST(name) printf("Test: %s\n", name)

// Test effects that emit constant intents
struct ConstCapEffect : IEffect {
  uint16_t id = 0;
  Capability cap = Capability::Dimmer;
  float v = 0.0f;

  ConstCapEffect() = default;
  ConstCapEffect(uint16_t i, Capability c, float val) : id(i), cap(c), v(val) {}

  void evaluate(float, std::vector<CapIntent>& c, std::vector<AimIntent>&) override {
    c.push_back({id, cap, v});
  }
};

struct ConstAimEffect : IEffect {
  uint16_t id = 0;
  Vec3 dir = {0.0f, 0.0f, 0.0f};

  ConstAimEffect() = default;
  ConstAimEffect(uint16_t i, Vec3 d) : id(i), dir(d) {}

  void evaluate(float, std::vector<CapIntent>&, std::vector<AimIntent>& a) override {
    a.push_back({id, dir, false});
  }
};

// Envelope tests
void test_fade_in() {
  TEST("Fade-in: fadeIn 2, hold 0 (infinite), fadeOut 1");

  ShowController ctrl;
  std::vector<IEffect*> effects;
  effects.push_back(new ConstCapEffect{7, Capability::Dimmer, 1.0f});

  uint16_t cueId = ctrl.addCue(effects, 2.0f, 1.0f, 0, 0.0f);

  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;

  // t=0: should be at weight 0 (no intent emitted since weight <= 0)
  ctrl.go(cueId, 0.0f);
  caps.clear();
  aims.clear();
  ctrl.evaluate(0.0f, caps, aims);
  CHECK(caps.empty());  // weight=0 means no intent

  // t=1: should be at weight 0.5 (halfway through fade-in)
  caps.clear();
  aims.clear();
  ctrl.evaluate(1.0f, caps, aims);
  CHECK_NEAR(caps[0].norm01, 0.5f);

  // t=2: should be at weight 1.0 (fade-in complete, now holding)
  caps.clear();
  aims.clear();
  ctrl.evaluate(2.0f, caps, aims);
  CHECK_NEAR(caps[0].norm01, 1.0f);

  // t=5: should still be at weight 1.0 (holding, hold is infinite)
  caps.clear();
  aims.clear();
  ctrl.evaluate(5.0f, caps, aims);
  CHECK_NEAR(caps[0].norm01, 1.0f);

  // Cleanup
  delete effects[0];
}

void test_manual_release() {
  TEST("Manual release: from held weight 1, release(t=5)");

  ShowController ctrl;
  std::vector<IEffect*> effects;
  effects.push_back(new ConstCapEffect{7, Capability::Dimmer, 1.0f});

  uint16_t cueId = ctrl.addCue(effects, 2.0f, 1.0f, 0, 0.0f);

  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;

  ctrl.go(cueId, 0.0f);

  // At t=5, we're held at weight 1.0
  caps.clear();
  aims.clear();
  ctrl.evaluate(5.0f, caps, aims);
  CHECK_NEAR(caps[0].norm01, 1.0f);
  CHECK(ctrl.isActive(cueId));

  // Release at t=5
  ctrl.release(cueId, 5.0f);

  // t=5: should still be at 1.0 (just released)
  caps.clear();
  aims.clear();
  ctrl.evaluate(5.0f, caps, aims);
  CHECK_NEAR(caps[0].norm01, 1.0f);

  // t=5.5: fading out, should be at 0.5 (halfway through 1-sec fade)
  caps.clear();
  aims.clear();
  ctrl.evaluate(5.5f, caps, aims);
  CHECK_NEAR(caps[0].norm01, 0.5f);

  // t=6: fade-out complete, should be inactive
  caps.clear();
  aims.clear();
  ctrl.evaluate(6.0f, caps, aims);
  CHECK(caps.empty());
  CHECK(!ctrl.isActive(cueId));

  delete effects[0];
}

void test_release_mid_fade_in() {
  TEST("Release mid-fade-in: fadeIn 4, fadeOut 2, release at t=2 (weight 0.5)");

  ShowController ctrl;
  std::vector<IEffect*> effects;
  effects.push_back(new ConstCapEffect{7, Capability::Dimmer, 1.0f});

  uint16_t cueId = ctrl.addCue(effects, 4.0f, 2.0f, 0, 0.0f);

  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;

  ctrl.go(cueId, 0.0f);

  // t=2: weight should be 0.5 (halfway through 4-sec fade-in)
  caps.clear();
  aims.clear();
  ctrl.evaluate(2.0f, caps, aims);
  CHECK_NEAR(caps[0].norm01, 0.5f);

  // Release at t=2 with weight 0.5
  ctrl.release(cueId, 2.0f);

  // t=2: should still be 0.5
  caps.clear();
  aims.clear();
  ctrl.evaluate(2.0f, caps, aims);
  CHECK_NEAR(caps[0].norm01, 0.5f);

  // t=3: fading out from 0.5, halfway through 2-sec fade should be 0.25
  caps.clear();
  aims.clear();
  ctrl.evaluate(3.0f, caps, aims);
  CHECK_NEAR(caps[0].norm01, 0.25f);

  // t=4: fade-out complete, should be inactive
  caps.clear();
  aims.clear();
  ctrl.evaluate(4.0f, caps, aims);
  CHECK(caps.empty());
  CHECK(!ctrl.isActive(cueId));

  delete effects[0];
}

void test_auto_hold() {
  TEST("Auto hold: fadeIn 0, hold 2, fadeOut 1");

  ShowController ctrl;
  std::vector<IEffect*> effects;
  effects.push_back(new ConstCapEffect{7, Capability::Dimmer, 1.0f});

  uint16_t cueId = ctrl.addCue(effects, 0.0f, 1.0f, 0, 2.0f);

  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;

  ctrl.go(cueId, 0.0f);

  // t=0: instant in (fadeIn=0), should be at 1.0
  caps.clear();
  aims.clear();
  ctrl.evaluate(0.0f, caps, aims);
  CHECK_NEAR(caps[0].norm01, 1.0f);

  // t=2: still held at 1.0
  caps.clear();
  aims.clear();
  ctrl.evaluate(2.0f, caps, aims);
  CHECK_NEAR(caps[0].norm01, 1.0f);

  // t=2.5: auto fade-out started, halfway through 1-sec fade, should be 0.5
  caps.clear();
  aims.clear();
  ctrl.evaluate(2.5f, caps, aims);
  CHECK_NEAR(caps[0].norm01, 0.5f);

  // t=3: fade-out complete, should be inactive
  caps.clear();
  aims.clear();
  ctrl.evaluate(3.0f, caps, aims);
  CHECK(caps.empty());
  CHECK(!ctrl.isActive(cueId));

  delete effects[0];
}

// Blending tests
void test_intensity_scaling() {
  TEST("Intensity scaling: single cue fadeIn 2 with Dimmer 1.0, at t=1 should be 0.5");

  ShowController ctrl;
  std::vector<IEffect*> effects;
  effects.push_back(new ConstCapEffect{7, Capability::Dimmer, 1.0f});

  uint16_t cueId = ctrl.addCue(effects, 2.0f, 1.0f, 0, 0.0f);

  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;

  ctrl.go(cueId, 0.0f);
  caps.clear();
  aims.clear();
  ctrl.evaluate(1.0f, caps, aims);

  CHECK_NEAR(caps[0].norm01, 0.5f);

  delete effects[0];
}

void test_intensity_htp() {
  TEST("Intensity HTP: two cues both at weight 1, one emits 0.6, other 0.4 -> resolved 0.6");

  ShowController ctrl;
  std::vector<IEffect*> effects1;
  effects1.push_back(new ConstCapEffect{7, Capability::Dimmer, 0.6f});

  std::vector<IEffect*> effects2;
  effects2.push_back(new ConstCapEffect{7, Capability::Dimmer, 0.4f});

  uint16_t cue1 = ctrl.addCue(effects1, 0.0f, 1.0f, 0, 0.0f);
  uint16_t cue2 = ctrl.addCue(effects2, 0.0f, 1.0f, 0, 0.0f);

  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;

  ctrl.go(cue1, 0.0f);
  ctrl.go(cue2, 0.0f);

  caps.clear();
  aims.clear();
  ctrl.evaluate(0.0f, caps, aims);

  // Should have exactly one Dimmer entry with value 0.6
  uint32_t dimmerCount = 0;
  float dimmerValue = 0.0f;
  for (const auto& c : caps) {
    if (c.fixtureId == 7 && c.cap == Capability::Dimmer) {
      dimmerCount++;
      dimmerValue = c.norm01;
    }
  }

  CHECK(dimmerCount == 1);
  CHECK_NEAR(dimmerValue, 0.6f);

  delete effects1[0];
  delete effects2[0];
}

void test_position_ltp_priority() {
  TEST("Position LTP by priority: low-prio emits 0.2, high-prio emits 0.8 -> resolved 0.8");

  ShowController ctrl;
  std::vector<IEffect*> effects1;
  effects1.push_back(new ConstCapEffect{7, Capability::Pan, 0.2f});

  std::vector<IEffect*> effects2;
  effects2.push_back(new ConstCapEffect{7, Capability::Pan, 0.8f});

  uint16_t cue1 = ctrl.addCue(effects1, 0.0f, 1.0f, 1, 0.0f);  // priority 1 (lower)
  uint16_t cue2 = ctrl.addCue(effects2, 0.0f, 1.0f, 5, 0.0f);  // priority 5 (higher)

  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;

  ctrl.go(cue1, 0.0f);
  ctrl.go(cue2, 0.0f);

  caps.clear();
  aims.clear();
  ctrl.evaluate(0.0f, caps, aims);

  // Should have exactly one Pan entry with value 0.8 (from high-priority cue)
  uint32_t panCount = 0;
  float panValue = 0.0f;
  for (const auto& c : caps) {
    if (c.fixtureId == 7 && c.cap == Capability::Pan) {
      panCount++;
      panValue = c.norm01;
    }
  }

  CHECK(panCount == 1);
  CHECK_NEAR(panValue, 0.8f);

  delete effects1[0];
  delete effects2[0];
}

void test_position_unscaled() {
  TEST("Position unscaled: high-prio cue at weight 0.5 still emits full Pan value");

  ShowController ctrl;
  std::vector<IEffect*> effects1;
  effects1.push_back(new ConstCapEffect{7, Capability::Pan, 0.2f});

  std::vector<IEffect*> effects2;
  effects2.push_back(new ConstCapEffect{7, Capability::Pan, 0.8f});

  uint16_t cue1 = ctrl.addCue(effects1, 0.0f, 1.0f, 1, 0.0f);
  uint16_t cue2 = ctrl.addCue(effects2, 2.0f, 1.0f, 5, 0.0f);  // fadeIn 2, so weight will be 0.5 at t=1

  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;

  ctrl.go(cue1, 0.0f);
  ctrl.go(cue2, 0.0f);

  // At t=1, cue2 is fading in (weight 0.5) but should still emit full Pan 0.8
  caps.clear();
  aims.clear();
  ctrl.evaluate(1.0f, caps, aims);

  float panValue = 0.0f;
  for (const auto& c : caps) {
    if (c.fixtureId == 7 && c.cap == Capability::Pan) {
      panValue = c.norm01;
    }
  }

  CHECK_NEAR(panValue, 0.8f);

  delete effects1[0];
  delete effects2[0];
}

void test_aim_ltp() {
  TEST("Aim LTP: higher priority's direction wins");

  ShowController ctrl;
  Vec3 dir1 = {0.2f, 0.0f, 0.0f};
  Vec3 dir2 = {0.8f, 0.0f, 0.0f};

  std::vector<IEffect*> effects1;
  effects1.push_back(new ConstAimEffect{7, dir1});

  std::vector<IEffect*> effects2;
  effects2.push_back(new ConstAimEffect{7, dir2});

  uint16_t cue1 = ctrl.addCue(effects1, 0.0f, 1.0f, 1, 0.0f);
  uint16_t cue2 = ctrl.addCue(effects2, 0.0f, 1.0f, 5, 0.0f);

  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;

  ctrl.go(cue1, 0.0f);
  ctrl.go(cue2, 0.0f);

  caps.clear();
  aims.clear();
  ctrl.evaluate(0.0f, caps, aims);

  // Should have exactly one Aim entry with dir2
  CHECK(aims.size() == 1);
  CHECK_NEAR(aims[0].target.x, 0.8f);

  delete effects1[0];
  delete effects2[0];
}

void test_removal_on_fadeout() {
  TEST("Removal: cue fadeIn 0, hold 1, fadeOut 0. At t=2, should be inactive and no intent");

  ShowController ctrl;
  std::vector<IEffect*> effects;
  effects.push_back(new ConstCapEffect{7, Capability::Dimmer, 1.0f});

  uint16_t cueId = ctrl.addCue(effects, 0.0f, 0.0f, 0, 1.0f);

  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;

  ctrl.go(cueId, 0.0f);

  // t=0: should be active with intent
  caps.clear();
  aims.clear();
  ctrl.evaluate(0.0f, caps, aims);
  CHECK(!caps.empty());
  CHECK(ctrl.isActive(cueId));

  // t=2: past hold time, fadeOut=0 so instant drop
  caps.clear();
  aims.clear();
  ctrl.evaluate(2.0f, caps, aims);
  CHECK(caps.empty());
  CHECK(!ctrl.isActive(cueId));

  delete effects[0];
}

void test_scene_fan_out() {
  TEST("Scene fan-out: both cues active and emitting, release both drops them");

  ShowController ctrl;
  std::vector<IEffect*> effects1;
  effects1.push_back(new ConstCapEffect{7, Capability::Dimmer, 0.6f});

  std::vector<IEffect*> effects2;
  effects2.push_back(new ConstCapEffect{8, Capability::Dimmer, 0.8f});

  uint16_t cue1 = ctrl.addCue(effects1, 0.0f, 1.0f, 0, 0.0f);
  uint16_t cue2 = ctrl.addCue(effects2, 0.0f, 1.0f, 0, 0.0f);

  std::vector<uint16_t> sceneIds = {cue1, cue2};
  uint16_t sceneId = ctrl.addScene(sceneIds);

  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;

  ctrl.goScene(sceneId, 0.0f);

  // Both should be active
  CHECK(ctrl.isActive(cue1));
  CHECK(ctrl.isActive(cue2));

  // Both should emit
  caps.clear();
  aims.clear();
  ctrl.evaluate(0.0f, caps, aims);
  CHECK(caps.size() == 2);

  // Release the scene
  ctrl.releaseScene(sceneId, 0.0f);

  // Both should fade and drop out
  caps.clear();
  aims.clear();
  ctrl.evaluate(1.5f, caps, aims);
  CHECK(caps.empty());
  CHECK(!ctrl.isActive(cue1));
  CHECK(!ctrl.isActive(cue2));

  delete effects1[0];
  delete effects2[0];
}

// End-to-end test with Show
void test_end_to_end() {
  TEST("End-to-end through Show: controller emits intents that resolve through show");

  // Build a 4-channel RGB par: Dimmer, R, G, B
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
  uint16_t fixtureId = show.patch(p, 0, 0);

  // Create a controller with one cue
  ShowController ctrl;
  std::vector<IEffect*> effects;
  effects.push_back(new ConstCapEffect{fixtureId, Capability::Dimmer, 1.0f});

  uint16_t cueId = ctrl.addCue(effects, 2.0f, 1.0f, 0, 0.0f);

  // Add controller to show
  show.addEffect(&ctrl);

  // Trigger cue at t=0
  ctrl.go(cueId, 0.0f);

  // Render at t=1 (should be halfway through fade-in, weight=0.5)
  show.renderFrame(1.0f);

  // Dimmer should be approximately 127/128 (0.5 scaled to 8-bit)
  uint8_t expectedDimmer = static_cast<uint8_t>(0.5f * 255.0f + 0.5f);  // round
  CHECK(sink.last[0] == expectedDimmer || sink.last[0] == expectedDimmer - 1 ||
        sink.last[0] == expectedDimmer + 1);  // allow 1-bit error due to rounding

  delete effects[0];
}

void test_active_cue_ids() {
  TEST("activeCueIds() lists only cues with active==true, matching isActive()");

  ShowController ctrl;
  std::vector<IEffect*> effects;
  effects.push_back(new ConstCapEffect{0, Capability::Dimmer, 1.0f});

  uint16_t cue0 = ctrl.addCue(effects, 0.0f, 0.0f, 0, 0.0f);  // no fade -- go()/release() are immediate
  uint16_t cue1 = ctrl.addCue(effects, 0.0f, 0.0f, 0, 0.0f);
  uint16_t cue2 = ctrl.addCue(effects, 0.0f, 0.0f, 0, 0.0f);

  uint16_t ids[8];
  CHECK(ctrl.activeCueIds(ids, 8) == 0);  // nothing active yet

  ctrl.go(cue0, 0.0f);
  ctrl.go(cue2, 0.0f);
  size_t n = ctrl.activeCueIds(ids, 8);
  CHECK(n == 2);
  CHECK(ids[0] == cue0);
  CHECK(ids[1] == cue2);
  CHECK(ctrl.isActive(cue0));
  CHECK(!ctrl.isActive(cue1));
  CHECK(ctrl.isActive(cue2));

  // Cap truncates rather than overflowing the caller's buffer.
  CHECK(ctrl.activeCueIds(ids, 1) == 1);

  delete effects[0];
}

int main() {
  printf("=== ShowController Tests ===\n\n");

  // Envelope tests
  test_fade_in();
  test_manual_release();
  test_release_mid_fade_in();
  test_auto_hold();

  printf("\n");

  // Blending tests
  test_intensity_scaling();
  test_intensity_htp();
  test_position_ltp_priority();
  test_position_unscaled();
  test_aim_ltp();
  test_removal_on_fadeout();
  test_scene_fan_out();

  printf("\n");

  // End-to-end
  test_end_to_end();

  printf("\n");

  test_active_cue_ids();

  printf("\n=== Results ===\n");
  if (g_failCount == 0) {
    printf("All tests passed!\n");
    return 0;
  } else {
    printf("%d test(s) failed\n", g_failCount);
    return 1;
  }
}
