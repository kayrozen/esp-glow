#include "live_control.h"
#include "show_control.h"
#include "fixture_profile.h"
#include "profile_encoder.h"

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

void test_cue_flash() {
  TEST("CueFlash: press → active, release → inactive");

  ShowController ctrl;
  std::vector<IEffect*> effects;
  effects.push_back(new ConstCapEffect{1, Capability::Dimmer, 1.0f});

  uint16_t cueId = ctrl.addCue(effects, 0.0f, 0.0f, 0, 0.0f);

  LiveControl live(ctrl);
  live.bindButton(60, ActionKind::CueFlash, cueId);

  // Press at t=0
  live.handle({ControlType::Button, 60, true, 0.0f}, 0.0f);
  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;
  ctrl.evaluate(0.0f, caps, aims);
  CHECK(ctrl.isActive(cueId));

  // Release at t=1 (with fadeOut 0, becomes inactive immediately)
  live.handle({ControlType::Button, 60, false, 0.0f}, 1.0f);
  caps.clear();
  aims.clear();
  ctrl.evaluate(1.0f, caps, aims);
  CHECK(!ctrl.isActive(cueId));

  delete effects[0];
}

void test_cue_toggle() {
  TEST("CueToggle: press → latched active, press → latched inactive, release ignored");

  ShowController ctrl;
  std::vector<IEffect*> effects;
  effects.push_back(new ConstCapEffect{1, Capability::Dimmer, 1.0f});

  uint16_t cueId = ctrl.addCue(effects, 0.0f, 0.0f, 0, 0.0f);

  LiveControl live(ctrl);
  live.bindButton(61, ActionKind::CueToggle, cueId);

  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;

  // First press: go
  live.handle({ControlType::Button, 61, true, 0.0f}, 0.0f);
  ctrl.evaluate(0.0f, caps, aims);
  CHECK(ctrl.isActive(cueId));

  // Release: ignored
  live.handle({ControlType::Button, 61, false, 0.0f}, 1.0f);
  caps.clear();
  aims.clear();
  ctrl.evaluate(1.0f, caps, aims);
  CHECK(ctrl.isActive(cueId));

  // Second press: release
  live.handle({ControlType::Button, 61, true, 0.0f}, 2.0f);
  caps.clear();
  aims.clear();
  ctrl.evaluate(2.0f, caps, aims);
  CHECK(!ctrl.isActive(cueId));

  delete effects[0];
}

void test_scene_go() {
  TEST("SceneGo: press → all cues active, release → all inactive");

  ShowController ctrl;
  std::vector<IEffect*> eff1, eff2;
  eff1.push_back(new ConstCapEffect{1, Capability::Dimmer, 1.0f});
  eff2.push_back(new ConstCapEffect{2, Capability::Dimmer, 1.0f});

  uint16_t cue1 = ctrl.addCue(eff1, 0.0f, 0.0f, 0, 0.0f);
  uint16_t cue2 = ctrl.addCue(eff2, 0.0f, 0.0f, 0, 0.0f);

  std::vector<uint16_t> cueIds{cue1, cue2};
  uint16_t sceneId = ctrl.addScene(cueIds);

  LiveControl live(ctrl);
  live.bindButton(62, ActionKind::SceneGo, sceneId);

  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;

  // Press at t=0
  live.handle({ControlType::Button, 62, true, 0.0f}, 0.0f);
  ctrl.evaluate(0.0f, caps, aims);
  CHECK(ctrl.isActive(cue1));
  CHECK(ctrl.isActive(cue2));

  // Release at t=1
  live.handle({ControlType::Button, 62, false, 0.0f}, 1.0f);
  caps.clear();
  aims.clear();
  ctrl.evaluate(1.0f, caps, aims);
  CHECK(!ctrl.isActive(cue1));
  CHECK(!ctrl.isActive(cue2));

  delete eff1[0];
  delete eff2[0];
}

void test_scene_toggle() {
  TEST("SceneToggle: press → latched, press → unlatched, release ignored");

  ShowController ctrl;
  std::vector<IEffect*> eff1, eff2;
  eff1.push_back(new ConstCapEffect{1, Capability::Dimmer, 1.0f});
  eff2.push_back(new ConstCapEffect{2, Capability::Dimmer, 1.0f});

  uint16_t cue1 = ctrl.addCue(eff1, 0.0f, 0.0f, 0, 0.0f);
  uint16_t cue2 = ctrl.addCue(eff2, 0.0f, 0.0f, 0, 0.0f);

  std::vector<uint16_t> cueIds{cue1, cue2};
  uint16_t sceneId = ctrl.addScene(cueIds);

  LiveControl live(ctrl);
  live.bindButton(63, ActionKind::SceneToggle, sceneId);

  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;

  // First press: go
  live.handle({ControlType::Button, 63, true, 0.0f}, 0.0f);
  ctrl.evaluate(0.0f, caps, aims);
  CHECK(ctrl.isActive(cue1));
  CHECK(ctrl.isActive(cue2));

  // Release: ignored
  live.handle({ControlType::Button, 63, false, 0.0f}, 1.0f);
  caps.clear();
  aims.clear();
  ctrl.evaluate(1.0f, caps, aims);
  CHECK(ctrl.isActive(cue1));
  CHECK(ctrl.isActive(cue2));

  // Second press: release
  live.handle({ControlType::Button, 63, true, 0.0f}, 2.0f);
  caps.clear();
  aims.clear();
  ctrl.evaluate(2.0f, caps, aims);
  CHECK(!ctrl.isActive(cue1));
  CHECK(!ctrl.isActive(cue2));

  delete eff1[0];
  delete eff2[0];
}

void test_master_fader() {
  TEST("Master fader: value clamped to [0,1]");

  ShowController ctrl;
  LiveControl live(ctrl);
  live.bindFader(200, ActionKind::Master);

  // Normal value
  live.handle({ControlType::Fader, 200, false, 0.5f}, 0.0f);
  CHECK_NEAR(live.masterLevel(), 0.5f);

  // Clamp high
  live.handle({ControlType::Fader, 200, false, 2.0f}, 0.0f);
  CHECK_NEAR(live.masterLevel(), 1.0f);

  // Clamp low
  live.handle({ControlType::Fader, 200, false, -1.0f}, 0.0f);
  CHECK_NEAR(live.masterLevel(), 0.0f);

  // Back to normal
  live.handle({ControlType::Fader, 200, false, 0.75f}, 0.0f);
  CHECK_NEAR(live.masterLevel(), 0.75f);
}

void test_clear() {
  TEST("clear(): wipes bindings, leaves masterLevel untouched (glow.bind.clear)");

  ShowController ctrl;
  uint16_t cueId = ctrl.addCue({}, 0.0f, 0.0f, 0, 0.0f);

  LiveControl live(ctrl);
  live.bindButton(60, ActionKind::CueFlash, cueId);
  live.bindFader(200, ActionKind::Master);
  live.handle({ControlType::Fader, 200, false, 0.5f}, 0.0f);
  CHECK_NEAR(live.masterLevel(), 0.5f);

  live.clear();

  // The pad binding is gone: a press that used to trigger the cue is now a
  // silent no-op, same as any other unbound id.
  live.handle({ControlType::Button, 60, true, 0.0f}, 0.0f);
  CHECK(!ctrl.isActive(cueId));

  // masterLevel is a fader *value*, not a binding -- clear() must not reset
  // it (see live_control.h's comment on clear()).
  CHECK_NEAR(live.masterLevel(), 0.5f);

  // Rebinding after clear() works normally.
  live.bindButton(60, ActionKind::CueFlash, cueId);
  live.handle({ControlType::Button, 60, true, 0.0f}, 0.0f);
  CHECK(ctrl.isActive(cueId));
}

void test_unbound_event() {
  TEST("Unbound event: no-op, no crash");

  ShowController ctrl;
  std::vector<IEffect*> effects;
  effects.push_back(new ConstCapEffect{1, Capability::Dimmer, 1.0f});

  uint16_t cueId = ctrl.addCue(effects, 0.0f, 0.0f, 0, 0.0f);

  LiveControl live(ctrl);
  live.bindButton(60, ActionKind::CueFlash, cueId);

  // Send unbound id
  live.handle({ControlType::Button, 99, true, 0.0f}, 0.0f);
  CHECK(!ctrl.isActive(cueId));

  delete effects[0];
}

void test_type_mismatch() {
  TEST("Type mismatch: Fader event at Button binding ignored");

  ShowController ctrl;
  LiveControl live(ctrl);
  live.bindFader(200, ActionKind::Master);

  // Try to send Button event at id 200 (which is bound as Fader)
  live.handle({ControlType::Button, 200, true, 0.0f}, 0.0f);
  CHECK_NEAR(live.masterLevel(), 1.0f);  // unchanged
}

void test_midi_parse_note_on() {
  TEST("MIDI parse: Note On (0x90, noteId, velocity)");

  ControlEvent ev;

  // Note On with velocity 100
  uint8_t msg1[] = {0x90, 60, 100};
  CHECK(parseMidi(msg1, 3, ev));
  CHECK(ev.type == ControlType::Button);
  CHECK(ev.id == 60);
  CHECK(ev.pressed == true);

  // Note On with velocity 0 (treated as Note Off)
  uint8_t msg2[] = {0x90, 60, 0};
  CHECK(parseMidi(msg2, 3, ev));
  CHECK(ev.type == ControlType::Button);
  CHECK(ev.id == 60);
  CHECK(ev.pressed == false);
}

void test_midi_parse_note_off() {
  TEST("MIDI parse: Note Off (0x80, noteId)");

  ControlEvent ev;
  uint8_t msg[] = {0x80, 60, 0};
  CHECK(parseMidi(msg, 3, ev));
  CHECK(ev.type == ControlType::Button);
  CHECK(ev.id == 60);
  CHECK(ev.pressed == false);
}

void test_midi_parse_cc() {
  TEST("MIDI parse: Control Change (0xB0, ccNumber, value)");

  ControlEvent ev;
  uint8_t msg[] = {0xB0, 7, 64};
  CHECK(parseMidi(msg, 3, ev));
  CHECK(ev.type == ControlType::Fader);
  CHECK(ev.id == 128 + 7);
  CHECK_NEAR(ev.value, 64.0f / 127.0f);
}

void test_midi_parse_channel_ignored() {
  TEST("MIDI parse: channel nibble ignored");

  ControlEvent ev;
  // 0x95 = Note On channel 6 (0x90 | 0x05)
  uint8_t msg[] = {0x95, 60, 100};
  CHECK(parseMidi(msg, 3, ev));
  CHECK(ev.type == ControlType::Button);
  CHECK(ev.id == 60);
  CHECK(ev.pressed == true);
}

void test_midi_parse_short_msg() {
  TEST("MIDI parse: buffer too short returns false");

  ControlEvent ev;
  uint8_t msg[] = {0x90, 60};
  CHECK(!parseMidi(msg, 2, ev));
}

void test_midi_parse_unsupported() {
  TEST("MIDI parse: unsupported status returns false");

  ControlEvent ev;
  // 0xF0 = SysEx (unsupported)
  uint8_t msg[] = {0xF0, 60, 100};
  CHECK(!parseMidi(msg, 3, ev));
}

void test_midi_to_dispatch() {
  TEST("MIDI → dispatch: parse Note On and trigger cue");

  ShowController ctrl;
  std::vector<IEffect*> effects;
  effects.push_back(new ConstCapEffect{1, Capability::Dimmer, 1.0f});

  uint16_t cueId = ctrl.addCue(effects, 0.0f, 0.0f, 0, 0.0f);

  LiveControl live(ctrl);
  live.bindButton(60, ActionKind::CueFlash, cueId);

  ControlEvent ev;
  uint8_t midiMsg[] = {0x90, 60, 100};
  CHECK(parseMidi(midiMsg, 3, ev));
  live.handle(ev, 0.0f);

  CHECK(ctrl.isActive(cueId));

  delete effects[0];
}

void test_midi_button_value_zeroed() {
  TEST("MIDI parse: button events zero out.value");
  ControlEvent ev;
  ev.value = 42.0f;
  uint8_t noteOn[] = {0x90, 60, 100};
  CHECK(parseMidi(noteOn, 3, ev));
  CHECK(ev.value == 0.0f);
  ev.value = 42.0f;
  uint8_t noteOff[] = {0x80, 60, 0};
  CHECK(parseMidi(noteOff, 3, ev));
  CHECK(ev.value == 0.0f);
}

int main() {
  test_cue_flash();
  test_cue_toggle();
  test_scene_go();
  test_scene_toggle();
  test_master_fader();
  test_clear();
  test_unbound_event();
  test_type_mismatch();
  test_midi_parse_note_on();
  test_midi_parse_note_off();
  test_midi_parse_cc();
  test_midi_parse_channel_ignored();
  test_midi_parse_short_msg();
  test_midi_parse_unsupported();
  test_midi_to_dispatch();
  test_midi_button_value_zeroed();

  if (g_failCount == 0) {
    printf("\nAll tests passed.\n");
    return 0;
  } else {
    printf("\n%d test(s) failed.\n", g_failCount);
    return 1;
  }
}
