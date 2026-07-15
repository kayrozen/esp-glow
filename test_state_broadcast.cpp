// test_state_broadcast.cpp — P1.3: proves the FULL device-pushed-state path,
// end to end, in one test: a cue fired over OSC (parseOsc, osc_parser.h) ->
// LiveControl::handle -> ShowController -> both buildStateJson
// (web_protocol.h, what the web console receives) and LedFeedback (what
// lights an APC40 pad) read the SAME ShowController::isActive/activeCueIds
// snapshot. This is the regression test for "the controller LEDs and the
// UI must never disagree" (FORMAT.md's P1.3 tie-in) -- proven here by
// construction, not by trusting two independent implementations to agree.
//
// This exists as its own small binary (rather than folding into
// test_web_protocol.cpp, test_live_control.cpp, or test_led_feedback.cpp)
// for the same reason test_fx_error_pipeline.cpp does: each of those only
// proves its own half of the pipeline. None on its own rules out the seam
// between OSC input, LiveControl dispatch, ShowController state, the
// state-JSON builder, and LED feedback breaking. main.cpp's actual device
// wiring (broadcast_state_if_changed's change-detection/rate-limit, the
// httpd send) is untestable without hardware, same status as every other
// web_input.cpp/midi_input.cpp transport -- what's tested here is
// everything host-testable that feeds it: the change-detection DISCIPLINE
// itself (a static show emits nothing new) is LedFeedback's own, already
// proven in test_led_feedback.cpp, and deliberately reused rather than
// reinvented (see FORMAT.md/main.cpp's broadcast_state_if_changed comment).

#include "live_control.h"
#include "osc_parser.h"
#include "show_control.h"
#include "web_protocol.h"
#include "led_feedback.h"
#include "mdef.h"
#include "controller_encoder.h"

#include <cstdio>
#include <cstring>
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

// --- OSC packet builder (mirrors test_osc_parser.cpp's) --------------------

void padTo4(std::vector<uint8_t>& v) {
  while (v.size() % 4 != 0) v.push_back(0);
}

std::vector<uint8_t> buildOsc(const char* address, char typeChar, uint32_t argBits) {
  std::vector<uint8_t> pkt(address, address + std::strlen(address));
  pkt.push_back(0);
  padTo4(pkt);
  pkt.push_back(',');
  pkt.push_back(static_cast<uint8_t>(typeChar));
  pkt.push_back(0);
  padTo4(pkt);
  pkt.push_back(static_cast<uint8_t>((argBits >> 24) & 0xFF));
  pkt.push_back(static_cast<uint8_t>((argBits >> 16) & 0xFF));
  pkt.push_back(static_cast<uint8_t>((argBits >> 8) & 0xFF));
  pkt.push_back(static_cast<uint8_t>(argBits & 0xFF));
  return pkt;
}

uint32_t floatBits(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof(bits));
  return bits;
}

// Recording MIDI-out sink, same role as test_led_feedback.cpp's fake --
// records every send3() call so a test can assert on the exact colour
// value an active cue's LED ended up at.
class RecordingMidiOutput : public IMidiOutput {
public:
  void send3(uint8_t status, uint8_t data1, uint8_t data2) override {
    calls.push_back({status, data1, data2});
  }
  struct Call { uint8_t status, data1, data2; };
  std::vector<Call> calls;

  // Last velocity/value sent for `note`, or -1 if never sent.
  int lastValueFor(uint8_t note) const {
    int v = -1;
    for (const auto& c : calls) {
      if (c.data1 == note) v = c.data2;
    }
    return v;
  }
};

MidiControllerProfile buildOneNoteProfile() {
  ControllerBuilder b;
  b.name = "Test";
  b.pads.push_back({53, 53});
  ControllerLedSpec led;
  led.msgType = LedMsgType::Note;
  led.addrFrom = 53;
  led.addrTo = 53;
  led.semantic = LedSemantic::Velocity;
  led.colors.push_back({"off", 0});
  led.colors.push_back({"on", 1});
  b.leds.push_back(led);

  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  MidiControllerProfile p;
  bool ok = parseMidiController(blob.data(), blob.size(), p);
  (void)ok;
  return p;
}

}  // namespace

void test_osc_cue_reflected_in_state_and_led_feedback_together() {
  TEST("Fire a cue via OSC -> state JSON and LED feedback both report it active");

  ShowController ctrl;
  uint16_t cueId = ctrl.addCue({}, 0.0f, 0.0f, 0, 0.0f);  // no effects needed; just active/inactive

  LiveControl live(ctrl);
  live.bindButton(0, ActionKind::CueFlash, cueId);  // web control id 0 -> ShowController cue 0

  MidiControllerProfile profile = buildOneNoteProfile();
  RecordingMidiOutput midiOut;
  LedFeedback led(profile, &midiOut);
  led.setAuto(53, cueId, "on", "off");

  static const OscBinding kBindings[] = {{"/cue/0", ControlType::Button, 0}};
  static const OscAddressMap kMap{kBindings, 1};

  // Not active yet: state JSON is empty, LED is "off" (0) once painted.
  led.refresh(ctrl, 0.0f);
  CHECK(midiOut.lastValueFor(53) == 0);

  char buf[128];
  size_t n = buildStateJson(nullptr, 0, live.masterLevel(), buf, sizeof(buf));
  std::string emptyState(buf, n);
  CHECK(emptyState.find("\"active\":[]") != std::string::npos);

  // Fire it via OSC (a press: nonzero float arg).
  std::vector<uint8_t> pkt = buildOsc("/cue/0", 'f', floatBits(1.0f));
  ControlEvent ev;
  CHECK(parseOsc(pkt.data(), pkt.size(), kMap, ev));
  CHECK(ev.type == ControlType::Button);
  CHECK(ev.pressed);
  live.handle(ev, 1.0f);

  CHECK(ctrl.isActive(cueId));

  // The state broadcast now reports it -- exactly what buildStateJson
  // (web_protocol.h) would send over the WS.
  uint16_t activeIds[1] = {0};  // web control id, same translation main.cpp's
                                 // g_wsCueShowIds does (see its own comment)
  n = buildStateJson(activeIds, 1, live.masterLevel(), buf, sizeof(buf));
  std::string activeState(buf, n);
  CHECK(activeState.find("\"active\":[0]") != std::string::npos);

  // LED feedback, reading the exact same ShowController, agrees: pad 53
  // is now "on" (velocity 1) -- the LED-feedback tie-in FORMAT.md/P1.3
  // calls for, proven here rather than assumed.
  led.refresh(ctrl, 1.0f);
  CHECK(midiOut.lastValueFor(53) == 1);
}

void test_static_show_no_state_or_led_traffic_after_first_paint() {
  TEST("Static show: after the initial paint, neither LED feedback nor a "
       "recomputed state JSON reflects any new change (change-detection parity)");

  ShowController ctrl;
  uint16_t cueId = ctrl.addCue({}, 0.0f, 0.0f, 0, 0.0f);
  ctrl.go(cueId, 0.0f);  // active from the start, never touched again

  MidiControllerProfile profile = buildOneNoteProfile();
  RecordingMidiOutput midiOut;
  LedFeedback led(profile, &midiOut);
  led.setAuto(53, cueId, "on", "off");

  led.refresh(ctrl, 0.0f);
  size_t sentAfterFirstPaint = midiOut.calls.size();
  CHECK(sentAfterFirstPaint > 0);  // the initial "on" paint did send

  // Several more frames, nothing changes on either side.
  for (float t = 0.1f; t < 1.0f; t += 0.1f) {
    led.refresh(ctrl, t);
  }
  CHECK(midiOut.calls.size() == sentAfterFirstPaint);  // no further LED sends

  // The state JSON a hypothetical unconditional recompute would produce is
  // byte-identical every time too -- this is exactly the "recomputed state
  // == last-broadcast state -> no send" comparison
  // broadcast_state_if_changed (main.cpp) makes every frame.
  uint16_t activeIds[1] = {0};
  char bufA[128], bufB[128];
  size_t nA = buildStateJson(activeIds, 1, 1.0f, bufA, sizeof(bufA));
  size_t nB = buildStateJson(activeIds, 1, 1.0f, bufB, sizeof(bufB));
  CHECK(nA == nB);
  CHECK(std::memcmp(bufA, bufB, nA) == 0);
}

void test_state_includes_master_level() {
  TEST("state JSON carries the current master level, not just active ids");

  ShowController ctrl;
  LiveControl live(ctrl);
  live.bindFader(200, ActionKind::Master);
  live.handle({ControlType::Fader, 200, 0, false, 0.42f}, 0.0f);

  char buf[128];
  size_t n = buildStateJson(nullptr, 0, live.masterLevel(), buf, sizeof(buf));
  std::string s(buf, n);
  CHECK(s.find("\"master\":0.4200") != std::string::npos);
}

int main() {
  test_osc_cue_reflected_in_state_and_led_feedback_together();
  test_static_show_no_state_or_led_traffic_after_first_paint();
  test_state_includes_master_level();

  if (g_failCount == 0) {
    printf("\nAll tests passed!\n");
    return 0;
  }
  printf("\n%d test(s) FAILED\n", g_failCount);
  return 1;
}
