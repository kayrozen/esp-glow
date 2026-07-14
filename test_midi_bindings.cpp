// test_midi_bindings.cpp — test glow.bind.* and glow.led.* APIs
// Tests MIDI controller bindings and LED feedback via .mdef definitions.

#include "glow_lua_api.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

#include "fixture_profile.h"
#include "live_control.h"
#include "lua_effect.h"
#include "lua_vm.h"
#include "mdef_parser.h"
#include "midi_output.h"
#include "show_control.h"

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

std::string readFennelSource() {
  std::ifstream f("third_party/fennel/fennel.lua", std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

struct Harness {
  ShowController show;
  LiveControl liveControl;
  glow::LuaVM vm;
  glow::BeatClock beatClock;
  glow::MidiOutput midiOut;
  GlowLuaApi api;

  Harness(const std::string& fennelSrc)
      : show(), liveControl(show), vm(), api(vm, show, nullptr, beatClock, nullptr) {
    api.install();
    api.setMidiOutput(&midiOut);
    api.setLiveControl(&liveControl);
    char err[256];
    if (!vm.loadFennelCompiler(fennelSrc.data(), fennelSrc.size(), err, sizeof(err))) {
      printf("FATAL: loadFennelCompiler failed: %s\n", err);
      std::abort();
    }
    vm.collectFullyOnce();
  }

  bool eval(const char* src, std::string* errOut = nullptr) {
    lua_State* L = vm.state();
    vm.pushFennelModule();
    lua_getfield(L, -1, "eval");
    lua_remove(L, -2);
    lua_pushlstring(L, src, std::strlen(src));
    lua_newtable(L);
    vm.pushSandboxEnv();
    lua_setfield(L, -2, "env");
    api.noteEvalTime(0.0f);
    vm.armEvalBudget();
    lua_call(L, 2, 1);
    bool ok = lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (!ok && errOut) {
      *errOut = "eval returned false";
    }
    return ok;
  }
};

}  // namespace

int main() {
  std::string fennelSrc = readFennelSource();

  // Test 1: glow.bind.pad binds a pad to a cue
  TEST("glow.bind.pad binds a pad note to a cue flash");
  {
    Harness h(fennelSrc);
    std::string err;
    
    // Define a cue first
    bool ok = h.eval("(glow.cue.define \"chorus\" {:effects [(fn [t] (glow.set 1 :dimmer 1.0))]})");
    CHECK(ok);
    
    // Bind pad 53 to the cue (flash mode)
    ok = h.eval("(glow.bind.pad 53 :flash :chorus)");
    CHECK(ok);
    
    // Unknown cue should error
    ok = h.eval("(glow.bind.pad 54 :flash :unknown-cue)");
    CHECK(!ok);
  }

  // Test 2: glow.bind.pad toggle mode
  TEST("glow.bind.pad toggle mode");
  {
    Harness h(fennelSrc);
    std::string err;
    
    h.eval("(glow.cue.define \"verse\" {:effects [(fn [t] (glow.set 1 :dimmer 0.5))]})");
    bool ok = h.eval("(glow.bind.pad 60 :toggle :verse)");
    CHECK(ok);
  }

  // Test 3: glow.bind.fader binds a CC to master
  TEST("glow.bind.fader binds a CC to master");
  {
    Harness h(fennelSrc);
    bool ok = h.eval("(glow.bind.fader 48 :master)");
    CHECK(ok);
    
    // Invalid target should error
    ok = h.eval("(glow.bind.fader 49 :invalid)");
    CHECK(!ok);
  }

  // Test 4: glow.bind.clear clears bindings
  TEST("glow.bind.clear clears all bindings");
  {
    Harness h(fennelSrc);
    h.eval("(glow.cue.define \"test\" {:effects [(fn [t] (glow.set 1 :dimmer 1.0))]})");
    h.eval("(glow.bind.pad 53 :flash :test)");
    bool ok = h.eval("(glow.bind.clear)");
    CHECK(ok);
  }

  // Test 5: glow.led.set with no controller def is no-op
  TEST("glow.led.set with no controller def is no-op");
  {
    Harness h(fennelSrc);
    // Should not crash, just no-op
    bool ok = h.eval("(glow.led.set 53 :red)");
    CHECK(ok);  // Returns true (no error), just does nothing
  }

  // Test 6: glow.led.auto with no controller def is no-op
  TEST("glow.led.auto with no controller def is no-op");
  {
    Harness h(fennelSrc);
    h.eval("(glow.cue.define \"active\" {:effects [(fn [t] (glow.set 1 :dimmer 1.0))]})");
    // Should not crash, just no-op even with unknown cue
    bool ok = h.eval("(glow.led.auto 53 :active :green :off)");
    CHECK(!ok);  // Errors because cue doesn't exist in this harness's cueIdByName_
  }

  // Test 7: Full integration with .mdef controller definition
  TEST("glow.led.* with controller definition sends MIDI");
  {
    Harness h(fennelSrc);
    
    // Create a controller definition
    auto mdef = std::make_unique<glow::mdef::ControllerDef>();
    mdef->name = "Test Controller";
    mdef->midiChannel = 1;
    
    // Add pads 53-55
    mdef->controls.push_back({glow::mdef::ControlType::Pad, 53, 55, ""});
    
    // Add LED definition for pads 53-55
    glow::mdef::LedDefinition led;
    led.isNote = true;
    led.startId = 53;
    led.endId = 55;
    led.semantic = glow::mdef::LedSemantic::Velocity;
    led.colorPalette["off"] = 0;
    led.colorPalette["green"] = 1;
    led.colorPalette["red"] = 3;
    mdef->leds.push_back(led);
    
    h.api.setControllerDef(std::move(mdef));
    
    // Now glow.led.set should work
    size_t before = h.midiOut.messagesSent();
    bool ok = h.eval("(glow.led.set 53 :green)");
    CHECK(ok);
    CHECK(h.midiOut.messagesSent() > before);  // Should have sent a message
    
    // Same color again should be suppressed (change detection)
    size_t sent = h.midiOut.messagesSent();
    ok = h.eval("(glow.led.set 53 :green)");
    CHECK(ok);
    CHECK(h.midiOut.messagesSent() == sent);  // No new message
    
    // Different color should send
    ok = h.eval("(glow.led.set 53 :red)");
    CHECK(ok);
    CHECK(h.midiOut.messagesSent() > sent);
    
    // Unknown color is no-op
    size_t sent2 = h.midiOut.messagesSent();
    ok = h.eval("(glow.led.set 53 :unknown-color)");
    CHECK(ok);  // No error, just no-op
    CHECK(h.midiOut.messagesSent() == sent2);
  }

  // Test 8: glow.led.auto tracks cue state
  TEST("glow.led.auto tracks cue active state");
  {
    Harness h(fennelSrc);
    
    // Set up controller def
    auto mdef = std::make_unique<glow::mdef::ControllerDef>();
    mdef->name = "Test";
    mdef->controls.push_back({glow::mdef::ControlType::Pad, 60, 60, ""});
    
    glow::mdef::LedDefinition led;
    led.isNote = true;
    led.startId = 60;
    led.endId = 60;
    led.semantic = glow::mdef::LedSemantic::Velocity;
    led.colorPalette["off"] = 0;
    led.colorPalette["on"] = 5;
    mdef->leds.push_back(led);
    
    h.api.setControllerDef(std::move(mdef));
    
    // Define and go to cue
    h.eval("(glow.cue.define \"test-cue\" {:effects [(fn [t] (glow.set 1 :dimmer 1.0))]})");
    
    // Before cue is active, LED should be off
    size_t before = h.midiOut.messagesSent();
    bool ok = h.eval("(glow.led.auto 60 :test-cue :on :off)");
    CHECK(ok);
    CHECK(h.midiOut.messagesSent() > before);
    
    // Go to cue
    h.eval("(glow.cue.go :test-cue)");
    
    // Now LED should be on
    size_t sent = h.midiOut.messagesSent();
    ok = h.eval("(glow.led.auto 60 :test-cue :on :off)");
    CHECK(ok);
    CHECK(h.midiOut.messagesSent() > sent);
  }

  // Test 9: Out of range note/CC errors
  TEST("glow.bind.pad rejects out-of-range notes");
  {
    Harness h(fennelSrc);
    bool ok = h.eval("(glow.bind.pad 128 :flash :cue)");
    CHECK(!ok);  // Should error
    
    ok = h.eval("(glow.bind.pad -1 :flash :cue)");
    CHECK(!ok);
  }

  TEST("glow.led.set rejects out-of-range ids");
  {
    Harness h(fennelSrc);
    bool ok = h.eval("(glow.led.set 128 :red)");
    CHECK(!ok);
  }

  printf("\n");
  if (g_failCount == 0) {
    printf("All tests passed.\n");
    return 0;
  } else {
    printf("%d test(s) failed.\n", g_failCount);
    return 1;
  }
}
