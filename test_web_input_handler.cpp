// test_web_input_handler.cpp — host test for the JSON->LiveControl dispatch.
//
// Uses a real ShowController with a MockSink-backed Show so we can verify a web
// frame actually triggers a cue (the cue's effects reach the sink). Also tests
// the MIDI and OSC paths through LiveControl so the binding dispatch is gated
// by `make test`.
#include "web_input_handler.h"
#include "live_control.h"
#include "show_control.h"
#include "show.h"
#include "effects.h"
#include "fixture_profile.h"
#include "midi_parser.h"
#include "osc_parser.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0;
#define CHECK(cond) do { \
  if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } \
} while (0)

static FixtureProfile makeDimmerProfile() {
  FixtureProfile p{};
  p.footprint = 1;
  p.channelCount = 1;
  p.channels[0] = { Capability::Dimmer, 0, 0xFF, 0, 0 };
  return p;
}

// Shared fixture: a Show with one dimmer fixture, a ShowController wired as an
// effect, and two cues (cue 0 = dimmer 0, cue 1 = dimmer full).
struct Harness {
  Show show;
  MockSink sink;
  ShowController controller;
  LiveControl lc;
  DimmerEffect fx0;
  DimmerEffect fx1;
  Harness()
    : lc(controller),
      fx0({0}, 0.0f),
      fx1({0}, 1.0f) {
    show.setUniverseCount(1);
    show.configureUniverse(0, UniverseMode::Fixture, &sink);
    show.patch(makeDimmerProfile(), 0, 0);
    show.addEffect(&controller);
    // Cues hold effects by pointer; the Harness owns them at static scope.
    controller.addCue({&fx0}, /*fadeIn*/ 0.0f, /*fadeOut*/ 0.0f, /*prio*/ 0, /*hold*/ 0.0f);
    controller.addCue({&fx1}, 0.0f, 0.0f, 0, 0.0f);
  }
};

static void test_web_go_frame() {
  printf("Test: {\"type\":\"go\",\"cue\":1} triggers cue 1\n");
  Harness h;
  // Bind button 5 -> cue 1 so we can verify both paths.
  h.lc.bindWebButton(5, 1, "Full");
  h.show.renderFrame(0.0f);  // nothing active -> dimmer default 0
  CHECK(h.sink.last[0] == 0);

  bool ok = web_input_handle_text_frame("{\"type\":\"go\",\"cue\":1}", h.lc, 1.0f);
  CHECK(ok);
  h.show.renderFrame(1.0f);
  CHECK(h.sink.last[0] == 255);  // cue 1 = full dimmer
}

static void test_web_button_frame() {
  printf("Test: {\"type\":\"button\",\"id\":5} fires bound cue\n");
  Harness h;
  h.lc.bindWebButton(5, 1, "Full");
  web_input_handle_text_frame("{\"type\":\"button\",\"id\":5}", h.lc, 1.0f);
  h.show.renderFrame(1.0f);
  CHECK(h.sink.last[0] == 255);
}

static void test_web_release_frame() {
  printf("Test: {\"type\":\"release\",\"cue\":1} releases\n");
  Harness h;
  web_input_handle_text_frame("{\"type\":\"go\",\"cue\":1}", h.lc, 1.0f);
  h.show.renderFrame(1.0f);
  CHECK(h.sink.last[0] == 255);
  web_input_handle_text_frame("{\"type\":\"release\",\"cue\":1}", h.lc, 2.0f);
  h.show.renderFrame(2.0f);
  CHECK(h.sink.last[0] == 0);  // released
}

static void test_web_scene_frame() {
  printf("Test: {\"type\":\"scene\",\"id\":0} triggers scene\n");
  Harness h;
  // Build a scene 0 = {cue 0, cue 1}; HTP blend -> 255 wins.
  h.controller.addScene({0, 1});
  web_input_handle_text_frame("{\"type\":\"scene\",\"id\":0}", h.lc, 1.0f);
  h.show.renderFrame(1.0f);
  CHECK(h.sink.last[0] == 255);
}

static void test_web_malformed_rejected() {
  printf("Test: malformed/unknown frames rejected\n");
  Harness h;
  CHECK(!web_input_handle_text_frame("not json", h.lc, 1.0f));
  CHECK(!web_input_handle_text_frame("{\"type\":\"bogus\"}", h.lc, 1.0f));
  CHECK(!web_input_handle_text_frame("{\"type\":\"go\"}", h.lc, 1.0f));  // no cue
  // Whitespace-tolerant:
  CHECK(web_input_handle_text_frame("{ \"type\" : \"go\", \"cue\" : 1 }", h.lc, 1.0f));
}

static void test_midi_note_on_triggers_cue() {
  printf("Test: MIDI Note On triggers bound cue\n");
  Harness h;
  h.lc.bindMidiNote(/*ch*/ 0, /*note*/ 60, /*cue*/ 1);
  MidiParser p;
  MidiEvent e;
  p.feed(0x90, e); p.feed(60, e); p.feed(100, e);
  h.lc.handleMidi(e, 1.0f);
  h.show.renderFrame(1.0f);
  CHECK(h.sink.last[0] == 255);
}

static void test_midi_note_off_releases() {
  printf("Test: MIDI Note Off releases bound cue\n");
  Harness h;
  h.lc.bindMidiNote(0, 60, 1);
  MidiParser p; MidiEvent e;
  p.feed(0x90, e); p.feed(60, e); p.feed(100, e);  // on
  h.lc.handleMidi(e, 1.0f);
  p.feed(0x80, e); p.feed(60, e); p.feed(0, e);    // off
  h.lc.handleMidi(e, 2.0f);
  h.show.renderFrame(2.0f);
  CHECK(h.sink.last[0] == 0);
}

static void test_midi_cc_go_release() {
  printf("Test: MIDI CC value>0 go, 0 release\n");
  Harness h;
  h.lc.bindMidiCC(0, 7, 1);
  MidiParser p; MidiEvent e;
  p.feed(0xB0, e); p.feed(7, e); p.feed(127, e);  // go
  h.lc.handleMidi(e, 1.0f);
  h.show.renderFrame(1.0f);
  CHECK(h.sink.last[0] == 255);
  p.feed(0xB0, e); p.feed(7, e); p.feed(0, e);    // release
  h.lc.handleMidi(e, 2.0f);
  h.show.renderFrame(2.0f);
  CHECK(h.sink.last[0] == 0);
}

static void test_osc_address_triggers_cue() {
  printf("Test: OSC address triggers bound cue\n");
  Harness h;
  h.lc.bindOsc("/esp-glow/cue/1/go", 1);
  // Build a minimal OSC message with a float 1.0 arg.
  uint8_t buf[64]; size_t off = 0;
  auto putStr = [&](const char* s) {
    size_t n = strlen(s); memcpy(buf+off, s, n); off += n; buf[off++]=0;
    while (off%4) buf[off++]=0;
  };
  auto putBE = [&](uint32_t v) {
    buf[off++]=(v>>24)&0xFF; buf[off++]=(v>>16)&0xFF;
    buf[off++]=(v>>8)&0xFF; buf[off++]=v&0xFF;
  };
  putStr("/esp-glow/cue/1/go");
  putStr(",f");
  putBE(0x3F800000u);  // 1.0f
  OscMessage m;
  CHECK(parseOsc(buf, off, m));
  h.lc.handleOsc(m.address, m.arg.f, m.arg.type != OscArg::None, 1.0f);
  h.show.renderFrame(1.0f);
  CHECK(h.sink.last[0] == 255);
}

static void test_config_json_builds() {
  printf("Test: config JSON snapshot builds\n");
  Harness h;
  h.lc.bindWebButton(1, 0, "Black");
  h.lc.bindWebButton(2, 1, "Full");
  char buf[256];
  size_t n = web_input_build_config(h.lc, buf, sizeof(buf));
  CHECK(n > 0);
  CHECK(strstr(buf, "\"type\":\"config\"") != nullptr);
  CHECK(strstr(buf, "\"id\":1") != nullptr);
  CHECK(strstr(buf, "\"cue\":1") != nullptr);
  CHECK(strstr(buf, "Full") != nullptr);
}

int main() {
  test_web_go_frame();
  test_web_button_frame();
  test_web_release_frame();
  test_web_scene_frame();
  test_web_malformed_rejected();
  test_midi_note_on_triggers_cue();
  test_midi_note_off_releases();
  test_midi_cc_go_release();
  test_osc_address_triggers_cue();
  test_config_json_builds();
  if (g_fail == 0) {
    printf("All web_input_handler tests passed!\n");
    return 0;
  }
  printf("%d web_input_handler tests FAILED\n", g_fail);
  return 1;
}
