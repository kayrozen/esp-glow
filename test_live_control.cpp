#include "live_control.h"
#include "show_control.h"
#include "fixture_profile.h"
#include "profile_encoder.h"
#include "mdef.h"
#include "controller_encoder.h"

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
  live.handle({ControlType::Button, 60, 0, true, 0.0f}, 0.0f);
  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;
  ctrl.evaluate(0.0f, caps, aims);
  CHECK(ctrl.isActive(cueId));

  // Release at t=1 (with fadeOut 0, becomes inactive immediately)
  live.handle({ControlType::Button, 60, 0, false, 0.0f}, 1.0f);
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
  live.handle({ControlType::Button, 61, 0, true, 0.0f}, 0.0f);
  ctrl.evaluate(0.0f, caps, aims);
  CHECK(ctrl.isActive(cueId));

  // Release: ignored
  live.handle({ControlType::Button, 61, 0, false, 0.0f}, 1.0f);
  caps.clear();
  aims.clear();
  ctrl.evaluate(1.0f, caps, aims);
  CHECK(ctrl.isActive(cueId));

  // Second press: release
  live.handle({ControlType::Button, 61, 0, true, 0.0f}, 2.0f);
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
  live.handle({ControlType::Button, 62, 0, true, 0.0f}, 0.0f);
  ctrl.evaluate(0.0f, caps, aims);
  CHECK(ctrl.isActive(cue1));
  CHECK(ctrl.isActive(cue2));

  // Release at t=1
  live.handle({ControlType::Button, 62, 0, false, 0.0f}, 1.0f);
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
  live.handle({ControlType::Button, 63, 0, true, 0.0f}, 0.0f);
  ctrl.evaluate(0.0f, caps, aims);
  CHECK(ctrl.isActive(cue1));
  CHECK(ctrl.isActive(cue2));

  // Release: ignored
  live.handle({ControlType::Button, 63, 0, false, 0.0f}, 1.0f);
  caps.clear();
  aims.clear();
  ctrl.evaluate(1.0f, caps, aims);
  CHECK(ctrl.isActive(cue1));
  CHECK(ctrl.isActive(cue2));

  // Second press: release
  live.handle({ControlType::Button, 63, 0, true, 0.0f}, 2.0f);
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
  live.handle({ControlType::Fader, 200, 0, false, 0.5f}, 0.0f);
  CHECK_NEAR(live.masterLevel(), 0.5f);

  // Clamp high
  live.handle({ControlType::Fader, 200, 0, false, 2.0f}, 0.0f);
  CHECK_NEAR(live.masterLevel(), 1.0f);

  // Clamp low
  live.handle({ControlType::Fader, 200, 0, false, -1.0f}, 0.0f);
  CHECK_NEAR(live.masterLevel(), 0.0f);

  // Back to normal
  live.handle({ControlType::Fader, 200, 0, false, 0.75f}, 0.0f);
  CHECK_NEAR(live.masterLevel(), 0.75f);
}


void test_profile_resolved_fader_bindings() {
  TEST("Fader binding: channel-significant ranges bind on resolved base channel");

  ControllerBuilder b;
  b.name = "Channel Fader Controller";
  b.faders.push_back({21, 21, "bank", 2, 4});  // one CC multiplexed across channels 2..4
  b.faders.push_back({14, 14, "master"});      // channel-agnostic fader
  std::string encErr;
  std::vector<uint8_t> blob = b.encode(encErr);
  CHECK(encErr.empty());
  MidiControllerProfile profile;
  CHECK(parseMidiController(blob.data(), blob.size(), profile));

  ShowController ctrl;
  LiveControl live(ctrl);
  live.setControllerProfile(&profile);

  uint16_t channelFaderId = resolveFaderBindingId(&profile, 21);
  CHECK(channelFaderId == packChannelControlId(2, static_cast<uint16_t>(128 + 21)));
  live.bindFader(channelFaderId, ActionKind::Master);

  // The one-argument API resolves to channelFrom, so other channels in the
  // same .mdef range do not hit this binding.
  live.handle({ControlType::Fader, static_cast<uint16_t>(128 + 21), 3, false, 0.25f}, 0.0f);
  CHECK_NEAR(live.masterLevel(), 1.0f);

  live.handle({ControlType::Fader, static_cast<uint16_t>(128 + 21), 2, false, 0.5f}, 0.0f);
  CHECK_NEAR(live.masterLevel(), 0.5f);

  uint16_t agnosticFaderId = resolveFaderBindingId(&profile, 14);
  CHECK(agnosticFaderId == static_cast<uint16_t>(128 + 14));
  live.bindFader(agnosticFaderId, ActionKind::Master);
  live.handle({ControlType::Fader, static_cast<uint16_t>(128 + 14), 7, false, 0.75f}, 0.0f);
  CHECK_NEAR(live.masterLevel(), 0.75f);
}

void test_clear() {
  TEST("clear(): wipes bindings, leaves masterLevel untouched (glow.bind.clear)");

  ShowController ctrl;
  uint16_t cueId = ctrl.addCue({}, 0.0f, 0.0f, 0, 0.0f);

  LiveControl live(ctrl);
  live.bindButton(60, ActionKind::CueFlash, cueId);
  live.bindFader(200, ActionKind::Master);
  live.handle({ControlType::Fader, 200, 0, false, 0.5f}, 0.0f);
  CHECK_NEAR(live.masterLevel(), 0.5f);

  live.clear();

  // The pad binding is gone: a press that used to trigger the cue is now a
  // silent no-op, same as any other unbound id.
  live.handle({ControlType::Button, 60, 0, true, 0.0f}, 0.0f);
  CHECK(!ctrl.isActive(cueId));

  // masterLevel is a fader *value*, not a binding -- clear() must not reset
  // it (see live_control.h's comment on clear()).
  CHECK_NEAR(live.masterLevel(), 0.5f);

  // Rebinding after clear() works normally.
  live.bindButton(60, ActionKind::CueFlash, cueId);
  live.handle({ControlType::Button, 60, 0, true, 0.0f}, 0.0f);
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
  live.handle({ControlType::Button, 99, 0, true, 0.0f}, 0.0f);
  CHECK(!ctrl.isActive(cueId));

  delete effects[0];
}

void test_type_mismatch() {
  TEST("Type mismatch: Fader event at Button binding ignored");

  ShowController ctrl;
  LiveControl live(ctrl);
  live.bindFader(200, ActionKind::Master);

  // Try to send Button event at id 200 (which is bound as Fader)
  live.handle({ControlType::Button, 200, 0, true, 0.0f}, 0.0f);
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

void test_midi_parse_channel_reported() {
  TEST("MIDI parse: channel nibble ALWAYS reported (the bug this rewrite fixes)");

  ControlEvent ev;
  // 0x95 = Note On channel 6 (0x90 | 0x05)
  uint8_t msg[] = {0x95, 60, 100};
  CHECK(parseMidi(msg, 3, ev));
  CHECK(ev.type == ControlType::Button);
  CHECK(ev.id == 60);
  CHECK(ev.channel == 5);
  CHECK(ev.pressed == true);
}

void test_midi_parse_same_note_different_channel_same_id() {
  TEST("MIDI parse: same note, different channel -> same id, different channel");

  ControlEvent evA, evB;
  uint8_t msgA[] = {0x90, 53, 100};  // channel 0
  uint8_t msgB[] = {0x93, 53, 100};  // channel 3
  CHECK(parseMidi(msgA, 3, evA));
  CHECK(parseMidi(msgB, 3, evB));
  CHECK(evA.id == evB.id);
  CHECK(evA.id == 53);
  CHECK(evA.channel == 0);
  CHECK(evB.channel == 3);
}

void test_midi_parse_poly_aftertouch() {
  TEST("MIDI parse: Polyphonic Aftertouch (0xA0, note, pressure)");

  ControlEvent ev;
  uint8_t msg[] = {0xA3, 64, 96};  // channel 3
  CHECK(parseMidi(msg, 3, ev));
  CHECK(ev.type == ControlType::Aftertouch);
  CHECK(ev.id == 64);
  CHECK(ev.channel == 3);
  CHECK_NEAR(ev.value, 96.0f / 127.0f);
}

void test_midi_parse_channel_pressure_2_bytes() {
  TEST("MIDI parse: Channel Pressure is 2 bytes (0xD0, pressure) -- not rejected for len<3");

  ControlEvent ev;
  uint8_t msg[] = {0xD5, 80};  // channel 5, exactly 2 bytes -- ASan-sized-exact
  CHECK(parseMidi(msg, 2, ev));
  CHECK(ev.type == ControlType::Aftertouch);
  CHECK(ev.channel == 5);
  CHECK_NEAR(ev.value, 80.0f / 127.0f);

  // Also accepted when the buffer is longer (e.g. USB-MIDI's zero-padded
  // 3-byte packets) -- the 3rd byte is simply unused.
  ControlEvent ev2;
  uint8_t msg3[] = {0xD5, 80, 0};
  CHECK(parseMidi(msg3, 3, ev2));
  CHECK(ev2.type == ControlType::Aftertouch);
  CHECK_NEAR(ev2.value, 80.0f / 127.0f);
}

void test_midi_parse_program_change_2_bytes() {
  TEST("MIDI parse: Program Change is 2 bytes (0xC0, program) -- not rejected for len<3");

  ControlEvent ev;
  uint8_t msg[] = {0xC2, 41};  // channel 2, exactly 2 bytes
  CHECK(parseMidi(msg, 2, ev));
  CHECK(ev.type == ControlType::Program);
  CHECK(ev.id == 41);
  CHECK(ev.channel == 2);
}

void test_midi_parse_pitch_bend() {
  TEST("MIDI parse: Pitch Bend -- center 0x2000 -> 0.5, extremes -> 0/1");

  ControlEvent ev;

  // Minimum: LSB=0, MSB=0 -> 0.0
  uint8_t msgMin[] = {0xE0, 0, 0};
  CHECK(parseMidi(msgMin, 3, ev));
  CHECK(ev.type == ControlType::PitchBend);
  CHECK_NEAR(ev.value, 0.0f);

  // Center: 0x2000 -> LSB=0x00, MSB=0x40 -> ~0.5
  uint8_t msgMid[] = {0xE0, 0x00, 0x40};
  CHECK(parseMidi(msgMid, 3, ev));
  CHECK_NEAR(ev.value, 0.5f);

  // Maximum: LSB=127, MSB=127 -> 1.0
  uint8_t msgMax[] = {0xE0, 127, 127};
  CHECK(parseMidi(msgMax, 3, ev));
  CHECK_NEAR(ev.value, 1.0f);
}

void test_midi_parse_status_ge_0xf0_rejected() {
  TEST("MIDI parse: status >= 0xF0 (System/Realtime) always rejected");

  ControlEvent ev;
  for (int status = 0xF0; status <= 0xFF; ++status) {
    uint8_t msg[] = {static_cast<uint8_t>(status), 0, 0};
    CHECK(!parseMidi(msg, 3, ev));
  }
}

void test_midi_parse_truncated_no_oob() {
  TEST("MIDI parse: truncated message -> false, no OOB read (sized-exact buffers)");

  ControlEvent ev;

  // 3-byte messages given only 1 byte (just the status).
  uint8_t noteOn1[] = {0x90};
  CHECK(!parseMidi(noteOn1, 1, ev));
  uint8_t cc1[] = {0xB0};
  CHECK(!parseMidi(cc1, 1, ev));
  uint8_t pitchBend1[] = {0xE0};
  CHECK(!parseMidi(pitchBend1, 1, ev));

  // 3-byte messages given only 2 bytes (status + data1, missing data2).
  uint8_t noteOn2[] = {0x90, 60};
  CHECK(!parseMidi(noteOn2, 2, ev));
  uint8_t polyAt2[] = {0xA0, 60};
  CHECK(!parseMidi(polyAt2, 2, ev));

  // 2-byte messages given only the status byte.
  uint8_t pc0[] = {0xC0};
  CHECK(!parseMidi(pc0, 1, ev));
  uint8_t chPressure0[] = {0xD0};
  CHECK(!parseMidi(chPressure0, 1, ev));

  // Zero-length buffer.
  CHECK(!parseMidi(noteOn1, 0, ev));
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

// The regression test for the actual bug this whole rewrite exists to fix:
// on a channel-multiplexed controller (the APC40's clip grid), two DIFFERENT
// physical pads share the same note number and are told apart only by MIDI
// channel. Before this rewrite, parseMidi discarded the channel, so both
// pads collapsed onto the same ControlEvent id and could only ever be bound
// to the same cue. With a channel-significant .mdef wired into LiveControl,
// they must fire independently.
void test_channel_significant_pads_fire_independently() {
  TEST("Regression: two APC40 pads, same note, different channel -> two different cues");

  ControllerBuilder b;
  b.name = "Test APC40";
  b.pads.push_back({53, 53, 0, 7});  // note 53, channel-significant across 0..7
  std::string encErr;
  std::vector<uint8_t> blob = b.encode(encErr);
  CHECK(encErr.empty());
  MidiControllerProfile profile;
  CHECK(parseMidiController(blob.data(), blob.size(), profile));

  ShowController ctrl;
  std::vector<IEffect*> effA, effB;
  effA.push_back(new ConstCapEffect{1, Capability::Dimmer, 1.0f});
  effB.push_back(new ConstCapEffect{2, Capability::Dimmer, 1.0f});
  uint16_t cueA = ctrl.addCue(effA, 0.0f, 0.0f, 0, 0.0f);
  uint16_t cueB = ctrl.addCue(effB, 0.0f, 0.0f, 0, 0.0f);

  LiveControl live(ctrl);
  live.setControllerProfile(&profile);
  // Same note (53), different channel (0 vs 3) -> packed ids (0<<8|53) and
  // (3<<8|53), bound to two different cues.
  live.bindButton(static_cast<uint16_t>((0 << 8) | 53), ActionKind::CueFlash, cueA);
  live.bindButton(static_cast<uint16_t>((3 << 8) | 53), ActionKind::CueFlash, cueB);

  ControlEvent evChan0, evChan3;
  uint8_t msgChan0[] = {0x90, 53, 100};  // channel 0
  uint8_t msgChan3[] = {0x93, 53, 100};  // channel 3
  CHECK(parseMidi(msgChan0, 3, evChan0));
  CHECK(parseMidi(msgChan3, 3, evChan3));
  CHECK(evChan0.id == evChan3.id);  // parseMidi's id is unpacked, unchanged
  CHECK(evChan0.channel == 0);
  CHECK(evChan3.channel == 3);

  // Channel 0's press fires cueA only.
  live.handle(evChan0, 0.0f);
  CHECK(ctrl.isActive(cueA));
  CHECK(!ctrl.isActive(cueB));

  // Channel 3's press fires cueB too -- both now independently active,
  // proving they were never the same binding.
  live.handle(evChan3, 0.0f);
  CHECK(ctrl.isActive(cueA));
  CHECK(ctrl.isActive(cueB));

  delete effA[0];
  delete effB[0];
}

void test_channel_agnostic_binding_unaffected_by_profile() {
  TEST("A profile with no channel-significant ranges: effectiveId is unpacked, unchanged");

  ControllerBuilder b;
  b.name = "Ordinary Controller";
  b.pads.push_back({60, 60});  // no CH -- channel-agnostic
  std::string encErr;
  std::vector<uint8_t> blob = b.encode(encErr);
  MidiControllerProfile profile;
  CHECK(parseMidiController(blob.data(), blob.size(), profile));

  ShowController ctrl;
  std::vector<IEffect*> effects;
  effects.push_back(new ConstCapEffect{1, Capability::Dimmer, 1.0f});
  uint16_t cueId = ctrl.addCue(effects, 0.0f, 0.0f, 0, 0.0f);

  LiveControl live(ctrl);
  live.setControllerProfile(&profile);
  live.bindButton(60, ActionKind::CueFlash, cueId);  // plain, unpacked id

  // Any channel on note 60 hits the same binding -- the profile has no
  // channel-significant range covering it.
  ControlEvent ev;
  uint8_t msg[] = {0x95, 60, 100};  // channel 5
  CHECK(parseMidi(msg, 3, ev));
  live.handle(ev, 0.0f);
  CHECK(ctrl.isActive(cueId));

  delete effects[0];
}

int main() {
  test_cue_flash();
  test_cue_toggle();
  test_scene_go();
  test_scene_toggle();
  test_master_fader();
  test_profile_resolved_fader_bindings();
  test_clear();
  test_unbound_event();
  test_type_mismatch();
  test_midi_parse_note_on();
  test_midi_parse_note_off();
  test_midi_parse_cc();
  test_midi_parse_channel_reported();
  test_midi_parse_same_note_different_channel_same_id();
  test_midi_parse_poly_aftertouch();
  test_midi_parse_channel_pressure_2_bytes();
  test_midi_parse_program_change_2_bytes();
  test_midi_parse_pitch_bend();
  test_midi_parse_status_ge_0xf0_rejected();
  test_midi_parse_truncated_no_oob();
  test_midi_parse_short_msg();
  test_midi_parse_unsupported();
  test_midi_to_dispatch();
  test_midi_button_value_zeroed();
  test_channel_significant_pads_fire_independently();
  test_channel_agnostic_binding_unaffected_by_profile();

  if (g_failCount == 0) {
    printf("\nAll tests passed.\n");
    return 0;
  } else {
    printf("\n%d test(s) failed.\n", g_failCount);
    return 1;
  }
}
