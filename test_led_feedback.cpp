// test_led_feedback.cpp — LED feedback (A5/A6): change-detection (a static
// show emits zero MIDI messages after the first refresh) and rate-limiting
// (many simultaneous LED changes are spread across refresh() calls, never
// dropped). Links a real ShowController so glow.led.auto's cue-tracking
// exercises the actual go()/stopAll() state machine, not a fake.

#include "led_feedback.h"
#include "controller_encoder.h"
#include "show_control.h"

#include <cstdio>
#include <cstdlib>
#include <string>
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

namespace {

struct SentMsg {
  uint8_t status, data1, data2;
};

class FakeMidiOutput : public IMidiOutput {
public:
  void send3(uint8_t status, uint8_t data1, uint8_t data2) override {
    sent.push_back({status, data1, data2});
  }
  std::vector<SentMsg> sent;
};

// One LED range covering notes [from, to], colours "on"=1 / "off"=0, plus a
// matching PAD declaration (not load-bearing for LedFeedback itself, but
// matches how a real .mdef would declare it).
MidiControllerProfile buildProfile(uint8_t from, uint8_t to) {
  ControllerBuilder b;
  b.name = "Test Pads";
  b.pads.push_back({from, to});

  ControllerLedSpec led;
  led.msgType = LedMsgType::Note;
  led.addrFrom = from;
  led.addrTo = to;
  led.semantic = LedSemantic::Velocity;
  led.colors.push_back({"off", 0});
  led.colors.push_back({"on", 1});
  b.leds.push_back(led);

  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  MidiControllerProfile p;
  if (!err.empty() || !parseMidiController(blob.data(), blob.size(), p)) {
    printf("FATAL: test controller failed to build/parse: %s\n", err.c_str());
    std::abort();
  }
  return p;
}

}  // namespace

void test_change_detection_static_show_emits_nothing() {
  TEST("A static show emits zero MIDI messages after the first refresh");

  MidiControllerProfile profile = buildProfile(50, 50);
  FakeMidiOutput out;
  LedFeedback lf(profile, &out, /*maxMsgsPerSec=*/1000.0f);

  ShowController sc;
  uint16_t cueId = sc.addCue({}, 0.0f, 0.0f, 0, 0.0f);
  lf.setAuto(50, cueId, "on", "off");

  // Cue inactive: first refresh paints "off" (the initial state).
  CHECK(out.sent.empty());
  lf.refresh(sc, 0.0f);
  CHECK(out.sent.size() == 1);
  CHECK(out.sent[0].data1 == 50 && out.sent[0].data2 == 0);
  out.sent.clear();

  // Still inactive, nothing changed: zero further sends, any number of times.
  lf.refresh(sc, 0.1f);
  lf.refresh(sc, 0.2f);
  lf.refresh(sc, 10.0f);
  CHECK(out.sent.empty());

  // Activate the cue: exactly one send (off -> on).
  sc.go(cueId, 10.5f);
  lf.refresh(sc, 10.5f);
  CHECK(out.sent.size() == 1);
  CHECK(out.sent[0].data2 == 1);
  out.sent.clear();

  // Held active, unchanged: zero sends again.
  lf.refresh(sc, 11.0f);
  lf.refresh(sc, 20.0f);
  CHECK(out.sent.empty());
  CHECK(lf.pendingCount() == 0);

  // stopAll() flips the cue inactive immediately (no evaluate() needed --
  // ShowController::isActive reads the flag stopAll sets directly): one
  // more send (on -> off), then quiet again.
  sc.stopAll();
  lf.refresh(sc, 20.5f);
  CHECK(out.sent.size() == 1);
  CHECK(out.sent[0].data2 == 0);
  out.sent.clear();
  lf.refresh(sc, 21.0f);
  CHECK(out.sent.empty());
}

void test_direct_set_no_op_on_unknown_led_or_color() {
  TEST("glow.led.set-equivalent: unknown address / unknown colour name -> no-op");

  MidiControllerProfile profile = buildProfile(53, 53);
  FakeMidiOutput out;
  LedFeedback lf(profile, &out, 1000.0f);
  ShowController sc;

  lf.set(99, "on");     // no LED declared for note 99
  lf.set(53, "purple"); // not in this LED's palette
  lf.refresh(sc, 0.0f);
  CHECK(out.sent.empty());
  CHECK(lf.pendingCount() == 0);

  // A valid set still works, proving the no-ops above weren't silently
  // swallowing everything.
  lf.set(53, "on");
  lf.refresh(sc, 0.1f);
  CHECK(out.sent.size() == 1);
  CHECK(out.sent[0].data1 == 53 && out.sent[0].data2 == 1);
}

void test_null_output_is_a_no_op_not_a_crash() {
  TEST("LedFeedback with output=nullptr: bookkeeping runs, nothing sent, no crash");

  MidiControllerProfile profile = buildProfile(53, 53);
  LedFeedback lf(profile, /*output=*/nullptr, 1000.0f);
  ShowController sc;
  uint16_t cueId = sc.addCue({}, 0.0f, 0.0f, 0, 0.0f);
  lf.setAuto(53, cueId, "on", "off");
  sc.go(cueId, 0.0f);
  lf.refresh(sc, 0.0f);  // must not crash
  lf.refresh(sc, 1.0f);
}

void test_rate_limit_spreads_a_burst_across_refreshes() {
  TEST("Many simultaneous LED changes are rate-limited, not dropped");

  const uint8_t kFirst = 40;
  const uint8_t kCount = 20;
  MidiControllerProfile profile = buildProfile(kFirst, kFirst + kCount - 1);
  FakeMidiOutput out;
  const float kRate = 5.0f;  // low cap so the 20-address burst spans several refreshes
  LedFeedback lf(profile, &out, kRate);

  ShowController sc;
  uint16_t cueId = sc.addCue({}, 0.0f, 0.0f, 0, 0.0f);
  for (uint8_t i = 0; i < kCount; ++i) {
    lf.setAuto(static_cast<uint8_t>(kFirst + i), cueId, "on", "off");
  }
  sc.go(cueId, 0.0f);

  // First refresh: full initial burst allowance (kRate), the rest pending.
  lf.refresh(sc, 0.0f);
  CHECK(out.sent.size() == static_cast<size_t>(kRate));
  CHECK(lf.pendingCount() == kCount - static_cast<size_t>(kRate));

  // Nothing has actually changed since; a refresh at the same instant (no
  // elapsed time -> no new tokens) sends nothing more.
  size_t sentBefore = out.sent.size();
  lf.refresh(sc, 0.0f);
  CHECK(out.sent.size() == sentBefore);

  // Time passes -- tokens refill and the rest drain out, never dropped.
  size_t totalSent = out.sent.size();
  float t = 0.0f;
  int guard = 0;
  while (lf.pendingCount() > 0 && guard < 100) {
    t += 1.0f;
    lf.refresh(sc, t);
    totalSent = out.sent.size();
    ++guard;
  }
  CHECK(lf.pendingCount() == 0);
  CHECK(totalSent == kCount);
  CHECK(guard < 100);  // sanity: actually converged, not stuck

  // And now it's quiet again -- change-detection holds even after a
  // rate-limited flood.
  size_t before = out.sent.size();
  lf.refresh(sc, t + 5.0f);
  CHECK(out.sent.size() == before);
}

// A note range that's channel-significant for BOTH input (PAD CH) and LED
// output (LED ... CH), like the APC40's 0x30-0x39: a status byte's channel
// nibble must match the specific pad addressed.
MidiControllerProfile buildChannelSignificantProfile(uint8_t note) {
  ControllerBuilder b;
  b.name = "Test Channel-Significant Pad";
  b.pads.push_back({note, note, 0, 7});

  ControllerLedSpec led;
  led.msgType = LedMsgType::Note;
  led.addrFrom = note;
  led.addrTo = note;
  led.semantic = LedSemantic::Velocity;
  led.colors.push_back({"off", 0});
  led.colors.push_back({"on", 1});
  led.channelFrom = 0;
  led.channelTo = 7;
  b.leds.push_back(led);

  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  MidiControllerProfile p;
  if (!err.empty() || !parseMidiController(blob.data(), blob.size(), p)) {
    printf("FATAL: test controller failed to build/parse: %s\n", err.c_str());
    std::abort();
  }
  return p;
}

void test_channel_aware_set_emits_correct_channel_nibble() {
  TEST("LED on a channel-multiplexed pad -> correct channel nibble emitted");

  MidiControllerProfile profile = buildChannelSignificantProfile(53);
  FakeMidiOutput out;
  LedFeedback lf(profile, &out, 1000.0f);
  ShowController sc;

  lf.set(53, /*channel=*/3, "on");
  lf.refresh(sc, 0.0f);
  CHECK(out.sent.size() == 1);
  CHECK(out.sent[0].status == (0x90 | 0x03));  // Note On, channel nibble 3
  CHECK(out.sent[0].data1 == 53 && out.sent[0].data2 == 1);
  out.sent.clear();

  // A different channel on the SAME note is tracked independently: setting
  // channel 5 doesn't touch (and doesn't re-send) channel 3's already-sent
  // state.
  lf.set(53, /*channel=*/5, "on");
  lf.refresh(sc, 0.1f);
  CHECK(out.sent.size() == 1);
  CHECK(out.sent[0].status == (0x90 | 0x05));
  out.sent.clear();

  lf.refresh(sc, 0.2f);
  CHECK(out.sent.empty());  // both channels unchanged -- no re-send
}

void test_channel_ignored_when_led_range_not_channel_significant() {
  TEST("A channel argument on an LED range with no CH modifier is ignored (the LED rule)");

  // PAD is channel-significant, but the LED range is NOT (mirrors the
  // APC40's DEVICE/BANK buttons: input carries a channel, LED output
  // doesn't) -- see mdef.h's MdefLedRange comment.
  ControllerBuilder b;
  b.name = "Input-only channel significance";
  b.pads.push_back({58, 58, 0, 7});
  ControllerLedSpec led;
  led.msgType = LedMsgType::Note;
  led.addrFrom = 58;
  led.addrTo = 58;
  led.semantic = LedSemantic::Velocity;
  led.colors.push_back({"off", 0});
  led.colors.push_back({"on", 1});
  // No CH on the LED line -- channelFrom/channelTo stay kChannelAgnostic.
  b.leds.push_back(led);
  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  MidiControllerProfile profile;
  CHECK(parseMidiController(blob.data(), blob.size(), profile));

  FakeMidiOutput out;
  LedFeedback lf(profile, &out, 1000.0f);
  ShowController sc;

  lf.set(58, /*channel=*/4, "on");  // channel argument should be ignored
  lf.refresh(sc, 0.0f);
  CHECK(out.sent.size() == 1);
  CHECK(out.sent[0].status == 0x90);  // channel nibble 0 (MIDI_CHANNEL default), NOT 4
}

int main() {
  test_change_detection_static_show_emits_nothing();
  test_direct_set_no_op_on_unknown_led_or_color();
  test_null_output_is_a_no_op_not_a_crash();
  test_rate_limit_spreads_a_burst_across_refreshes();
  test_channel_aware_set_emits_correct_channel_nibble();
  test_channel_ignored_when_led_range_not_channel_significant();

  if (g_failCount == 0) {
    printf("\nAll tests passed!\n");
    return 0;
  } else {
    printf("\n%d test(s) failed.\n", g_failCount);
    return 1;
  }
}
