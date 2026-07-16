// test_glow_lua_api.cpp — the glow.* C API surface: cue/scene state via
// ShowController, glow.fx.* effect handles, glow.matrix.* via a fake
// registry. (glow.set/glow.aim's own contract is covered end-to-end in
// test_lua_effect.cpp, since it needs a LuaEffect frame context.)

#include "glow_lua_api.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

#include "controller_encoder.h"
#include "fixture_profile.h"
#include "led_feedback.h"
#include "live_control.h"
#include "lua_effect.h"
#include "lua_vm.h"
#include "mdef.h"
#include "pixel_matrix.h"
#include "profile_encoder.h"
#include "show_control.h"
#include "wled_manager.h"

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

std::string readDemoBootFnl() {
  std::ifstream f("samples/demo-boot.fnl", std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

class FakeMatrixRegistry : public IMatrixRegistry {
public:
  explicit FakeMatrixRegistry(std::vector<PixelMatrix*> mats) : mats_(std::move(mats)) {}
  PixelMatrix* matrix(int index) override {
    if (index < 0 || static_cast<size_t>(index) >= mats_.size()) return nullptr;
    return mats_[index];
  }

private:
  std::vector<PixelMatrix*> mats_;
};

// glow.ranges (v2 introspection) resolves fixture ids through this instead
// of ShowController/Show -- see glow_lua_api.h's IFixtureRegistry.
class FakeFixtureRegistry : public IFixtureRegistry {
public:
  void put(uint16_t fixtureId, const FixtureProfile& p) { profiles_[fixtureId] = p; }
  const FixtureProfile* profile(uint16_t fixtureId) override {
    auto it = profiles_.find(fixtureId);
    return it == profiles_.end() ? nullptr : &it->second;
  }

private:
  std::unordered_map<uint16_t, FixtureProfile> profiles_;
};

struct Harness {
  ShowController show;
  glow::LuaVM vm;
  IMatrixRegistry* matrices;
  glow::BeatClock beatClock;
  LiveControl live;
  GlowLuaApi api;

  Harness(const std::string& fennelSrc, IMatrixRegistry* mats = nullptr,
          IFixtureRegistry* fixtures = nullptr, LedFeedback* ledFeedback = nullptr,
          WledManager* wled = nullptr)
      : vm(), matrices(mats), live(show),
        api(vm, show, mats, beatClock, live, fixtures, ledFeedback, wled) {
    api.install();
    char err[256];
    if (!vm.loadFennelCompiler(fennelSrc.data(), fennelSrc.size(), err, sizeof(err))) {
      printf("FATAL: loadFennelCompiler failed: %s\n", err);
      std::abort();
    }
    vm.collectFullyOnce();
  }

  // Evaluates `src`; returns true/false, filling errOut on failure.
  bool eval(const char* src, std::string* errOut = nullptr) {
    lua_State* L = vm.state();
    vm.pushFennelModule();
    lua_getfield(L, -1, "eval");
    lua_remove(L, -2);
    lua_pushlstring(L, src, std::strlen(src));
    lua_newtable(L);
    vm.pushSandboxEnv();
    lua_setfield(L, -2, "env");
    api.noteEvalTime(evalTime);
    vm.armEvalBudget();
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
      const char* msg = lua_tostring(L, -1);
      if (errOut) *errOut = msg ? msg : "(non-string error)";
      lua_pop(L, 1);
      return false;
    }
    return true;
  }

  void evalOrDie(const char* src) {
    std::string err;
    if (!eval(src, &err)) {
      printf("FATAL: eval failed: %s\n", err.c_str());
      std::abort();
    }
  }

  // Evaluates `src` and returns its value directly (Fennel's eval, like a
  // REPL, returns the last expression's value) -- unlike eval()/global,
  // this doesn't depend on the sandboxed env's globals ever being visible
  // to the real _G (they aren't: `(global x v)` inside a sandboxed eval
  // sets x in the sandbox table, not real Lua globals).
  double evalNumber(const char* src) {
    lua_State* L = vm.state();
    vm.pushFennelModule();
    lua_getfield(L, -1, "eval");
    lua_remove(L, -2);
    lua_pushlstring(L, src, std::strlen(src));
    lua_newtable(L);
    vm.pushSandboxEnv();
    lua_setfield(L, -2, "env");
    api.noteEvalTime(evalTime);
    vm.armEvalBudget();
    if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
      printf("FATAL: evalNumber failed: %s\n", lua_tostring(L, -1));
      std::abort();
    }
    double v = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
  }

  bool evalBool(const char* src) {
    lua_State* L = vm.state();
    vm.pushFennelModule();
    lua_getfield(L, -1, "eval");
    lua_remove(L, -2);
    lua_pushlstring(L, src, std::strlen(src));
    lua_newtable(L);
    vm.pushSandboxEnv();
    lua_setfield(L, -2, "env");
    api.noteEvalTime(evalTime);
    vm.armEvalBudget();
    if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
      printf("FATAL: evalBool failed: %s\n", lua_tostring(L, -1));
      std::abort();
    }
    bool v = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return v;
  }

  // Compiles `src` to a bare function value (no top-level call) and takes
  // a registry ref to it, for constructing a LuaEffect directly -- mirrors
  // test_lua_effect.cpp's Harness::compileToRef.
  int compileToRef(const char* src) {
    lua_State* L = vm.state();
    vm.pushFennelModule();
    lua_getfield(L, -1, "eval");
    lua_remove(L, -2);
    lua_pushlstring(L, src, std::strlen(src));
    lua_newtable(L);
    vm.pushSandboxEnv();
    lua_setfield(L, -2, "env");
    vm.armEvalBudget();
    if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
      printf("FATAL: compileToRef failed: %s\n", lua_tostring(L, -1));
      std::abort();
    }
    return luaL_ref(L, LUA_REGISTRYINDEX);
  }

  float evalTime = 0.0f;
};

}  // namespace

// ---------------------------------------------------------------------------
// cue.define / go / release
// ---------------------------------------------------------------------------

void test_cue_define_and_go_activates_showcontroller_cue() {
  TEST("glow.cue.define + glow.cue.go activates the underlying ShowController cue");
  std::string fsrc = readFennelSource();
  Harness h(fsrc);

  h.evalOrDie(
      "(fn fx [t] (glow.set 1 :dimmer 1.0))\n"
      "(glow.cue.define :wash {:effects [fx] :priority 0})\n");

  uint16_t id;
  CHECK(h.api.cueIdForName("wash", id));
  CHECK(!h.show.isActive(id));

  h.evalOrDie("(glow.cue.go :wash)");
  CHECK(h.show.isActive(id));
}

void test_cue_release_deactivates() {
  TEST("glow.cue.release deactivates a cue with no fade-out");
  std::string fsrc = readFennelSource();
  Harness h(fsrc);

  h.evalOrDie("(glow.cue.define :wash {:effects [] :priority 0})");
  h.evalOrDie("(glow.cue.go :wash)");
  uint16_t id;
  h.api.cueIdForName("wash", id);
  CHECK(h.show.isActive(id));

  h.evalOrDie("(glow.cue.release :wash)");
  // release() alone doesn't clear `active` -- ShowController::evaluate()
  // notices the fade-out completed and clears it. fade-out defaults to 0,
  // so it's gone on the very next evaluate().
  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;
  h.show.evaluate(0.0f, caps, aims);
  CHECK(!h.show.isActive(id));
}

void test_cue_go_unknown_name_errors() {
  TEST("glow.cue.go on an unknown name errors, not crashes");
  std::string fsrc = readFennelSource();
  Harness h(fsrc);
  std::string err;
  CHECK(!h.eval("(glow.cue.go :nope)", &err));
  CHECK(err.find("unknown cue") != std::string::npos);
}

void test_cue_redefine_by_name_orphans_old_active_cue() {
  TEST("redefining a cue name releases the old cue if it was active, points to a new one");
  std::string fsrc = readFennelSource();
  Harness h(fsrc);

  h.evalOrDie("(glow.cue.define :wash {:effects [] :priority 0})");
  uint16_t oldId;
  h.api.cueIdForName("wash", oldId);
  h.evalOrDie("(glow.cue.go :wash)");
  CHECK(h.show.isActive(oldId));

  // Redefine while active.
  h.evalOrDie("(glow.cue.define :wash {:effects [] :priority 0})");
  uint16_t newId;
  h.api.cueIdForName("wash", newId);
  CHECK(newId != oldId);

  // The old cue was release()d as part of redefinition; with fade-out=0 it
  // clears on the next evaluate().
  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;
  h.show.evaluate(0.0f, caps, aims);
  CHECK(!h.show.isActive(oldId));
  CHECK(!h.show.isActive(newId));  // new cue was defined, not go()'d
}

void test_cue_glow_slot_range_selection_survives_priority_resolution() {
  TEST("A glow.slot range selection survives ShowController's HTP/LTP resolution");
  std::string fsrc = readFennelSource();
  Harness h(fsrc);

  h.evalOrDie(
      "(fn fx [t] (glow.slot 1 :gobo \"dots\") (glow.slot 1 :color-wheel 2))\n"
      "(glow.cue.define :look {:effects [fx] :priority 0})\n"
      "(glow.cue.go :look)\n");

  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;
  h.show.evaluate(0.0f, caps, aims);

  CHECK(caps.size() == 2);
  bool sawGoboByName = false, sawColorWheelByIndex = false;
  for (auto& c : caps) {
    if (c.fixtureId == 1 && c.cap == Capability::Gobo) {
      CHECK(c.rangeName != nullptr);
      CHECK(std::strcmp(c.rangeName, "dots") == 0);
      CHECK(c.rangeIndex == -1);
      sawGoboByName = true;
    }
    if (c.fixtureId == 1 && c.cap == Capability::ColorWheel) {
      CHECK(c.rangeName == nullptr);
      CHECK(c.rangeIndex == 2);
      sawColorWheelByIndex = true;
    }
  }
  CHECK(sawGoboByName);
  CHECK(sawColorWheelByIndex);
}

void test_cue_glow_slot_on_intensity_class_resolves_by_priority_not_htp() {
  TEST("A glow.slot on an HTP-class capability (ShutterStrobe) resolves by priority, not max()");
  std::string fsrc = readFennelSource();
  Harness h(fsrc);

  h.evalOrDie(
      "(fn low-fx [t] (glow.set 9 :shutter-strobe 1.0))\n"           // plain HTP write, high value
      "(fn high-fx [t] (glow.slot 9 :shutter-strobe \"strobe\" 0.2))\n"  // range-select, lower value
      "(glow.cue.define :low {:effects [low-fx] :priority 0})\n"
      "(glow.cue.define :high {:effects [high-fx] :priority 1})\n"
      "(glow.cue.go :low)\n"
      "(glow.cue.go :high)\n");

  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;
  h.show.evaluate(0.0f, caps, aims);

  bool found = false;
  for (auto& c : caps) {
    if (c.fixtureId == 9 && c.cap == Capability::ShutterStrobe) {
      found = true;
      // The higher-priority cue's range selection wins outright, even
      // though its norm01 (0.2) is lower than the low-priority plain
      // write's (1.0) -- a range selection is a discrete choice, not a
      // blendable intensity.
      CHECK(c.rangeName != nullptr);
      CHECK(std::strcmp(c.rangeName, "strobe") == 0);
      CHECK(std::fabs(c.norm01 - 0.2f) < 1e-4f);
    }
  }
  CHECK(found);
}

// ---------------------------------------------------------------------------
// scene.define / go / release
// ---------------------------------------------------------------------------

void test_scene_go_activates_all_member_cues() {
  TEST("glow.scene.go activates every cue in the scene");
  std::string fsrc = readFennelSource();
  Harness h(fsrc);

  h.evalOrDie(
      "(glow.cue.define :a {:effects [] :priority 0})\n"
      "(glow.cue.define :b {:effects [] :priority 0})\n"
      "(glow.scene.define :both [:a :b])\n");
  uint16_t idA, idB;
  h.api.cueIdForName("a", idA);
  h.api.cueIdForName("b", idB);
  CHECK(!h.show.isActive(idA));
  CHECK(!h.show.isActive(idB));

  h.evalOrDie("(glow.scene.go :both)");
  CHECK(h.show.isActive(idA));
  CHECK(h.show.isActive(idB));
}

void test_scene_define_unknown_cue_errors() {
  TEST("glow.scene.define referencing an unknown cue errors");
  std::string fsrc = readFennelSource();
  Harness h(fsrc);
  std::string err;
  CHECK(!h.eval("(glow.scene.define :s [:nope])", &err));
  CHECK(err.find("unknown cue") != std::string::npos);
}

// ---------------------------------------------------------------------------
// fx.* handles
// ---------------------------------------------------------------------------

void test_fx_hue_rotate_handle_usable_in_cue_and_emits() {
  TEST("glow.fx.hue-rotate returns a handle that emits real color intents in a cue");
  std::string fsrc = readFennelSource();
  Harness h(fsrc);

  h.evalOrDie(
      "(glow.cue.define :hr {:effects [(glow.fx.hue-rotate [1 2] {:period 4.0})] :priority 0})\n"
      "(glow.cue.go :hr)\n");
  CHECK(h.api.ownedEffectCount() == 1);

  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;
  h.show.evaluate(0.0f, caps, aims);
  bool sawFixture1Red = false;
  for (auto& c : caps) {
    if (c.fixtureId == 1 && c.cap == Capability::Red) sawFixture1Red = true;
  }
  CHECK(sawFixture1Red);
}

void test_fx_sweep_handle() {
  TEST("glow.fx.sweep returns a usable handle");
  std::string fsrc = readFennelSource();
  Harness h(fsrc);
  h.evalOrDie(
      "(glow.cue.define :sw {:effects [(glow.fx.sweep 5 [0 0 1] [1 0 1] {:period 6.0})] "
      ":priority 0})\n"
      "(glow.cue.go :sw)\n");
  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;
  h.show.evaluate(0.0f, caps, aims);
  CHECK(aims.size() == 1);
  CHECK(aims[0].fixtureId == 5);
}

void test_cue_define_rejects_invalid_effects_entry() {
  TEST("cue.define rejects an effects[] entry that isn't a function or fx handle");
  std::string fsrc = readFennelSource();
  Harness h(fsrc);
  std::string err;
  CHECK(!h.eval("(glow.cue.define :bad {:effects [42] :priority 0})", &err));
  CHECK(err.find("function") != std::string::npos || err.find("handle") != std::string::npos);
}

// ---------------------------------------------------------------------------
// pollNewlyDisabledEffects -- the fx_error notification source
// ---------------------------------------------------------------------------

void test_poll_newly_disabled_effects_reports_once() {
  TEST("pollNewlyDisabledEffects: reports a freshly-broken effect exactly once, named <cue>#<index>");
  std::string fsrc = readFennelSource();
  Harness h(fsrc);

  h.evalOrDie(
      "(fn ok [t] (glow.set 1 :dimmer 1.0))\n"
      "(fn breathe [t] (glow.set \"not-a-number\" :dimmer 1.0))\n"
      "(glow.cue.define :verse {:effects [ok breathe] :priority 0})\n"
      "(glow.cue.go :verse)\n");

  std::vector<std::pair<std::string, std::string>> notifications;
  h.api.pollNewlyDisabledEffects(notifications);
  CHECK(notifications.empty());  // nothing has run yet

  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;
  h.show.evaluate(0.0f, caps, aims);  // drives both effects once

  notifications.clear();
  h.api.pollNewlyDisabledEffects(notifications);
  CHECK(notifications.size() == 1);
  if (notifications.size() == 1) {
    CHECK(notifications[0].first == "verse#1");  // "breathe" is the 2nd (index 1) effect
    CHECK(notifications[0].second.find("number expected") != std::string::npos);
  }

  // A second frame doesn't re-report the same effect.
  h.show.evaluate(1.0f / 44.0f, caps, aims);
  notifications.clear();
  h.api.pollNewlyDisabledEffects(notifications);
  CHECK(notifications.empty());
}

// ---------------------------------------------------------------------------
// matrix.*
// ---------------------------------------------------------------------------

void test_matrix_pattern_and_brightness_apply_to_the_registered_matrix() {
  TEST("glow.matrix.pattern/brightness reach the matrix via IMatrixRegistry");
  MatrixMap mm{};
  mm.width = 4;
  mm.height = 4;
  mm.serpentine = false;
  mm.vertical = false;
  mm.order = ColorOrder::RGB;
  mm.startUniverse = 0;
  mm.startChannel = 0;
  PixelMatrix pm(mm);
  FakeMatrixRegistry reg({&pm});

  std::string fsrc = readFennelSource();
  Harness h(fsrc, &reg);

  h.evalOrDie(R"((glow.matrix.pattern 0 :solid {}))");
  h.evalOrDie("(glow.matrix.brightness 0 0.5)");

  pm.render(0.0f);
  // Brightness is applied when packing into the DMX universe buffer, not
  // into the Canvas itself (see PixelMatrix::render) -- solid white at 0.5
  // brightness, RGB order, channel 0 -> ~127-128.
  const uint8_t* u = pm.universeData(0);
  CHECK(u != nullptr);
  CHECK(u[0] >= 120 && u[0] <= 135);
}

void test_matrix_pattern_unknown_index_errors() {
  TEST("glow.matrix.pattern with an out-of-range index errors, not crashes");
  MatrixMap mm{};
  mm.width = 2;
  mm.height = 2;
  mm.order = ColorOrder::RGB;
  PixelMatrix pm(mm);
  FakeMatrixRegistry reg({&pm});
  std::string fsrc = readFennelSource();
  Harness h(fsrc, &reg);
  std::string err;
  CHECK(!h.eval(R"((glow.matrix.pattern 9 :solid {}))", &err));
  CHECK(err.find("no matrix") != std::string::npos);
}

void test_matrix_without_registry_errors_cleanly() {
  TEST("glow.matrix.* with no registry configured errors instead of crashing");
  std::string fsrc = readFennelSource();
  Harness h(fsrc, nullptr);
  std::string err;
  CHECK(!h.eval(R"((glow.matrix.pattern 0 :solid {}))", &err));
  CHECK(err.find("no matrices") != std::string::npos);
}

void test_matrix_unknown_pattern_name_errors() {
  TEST("glow.matrix.pattern with an unknown pattern name errors");
  MatrixMap mm{};
  mm.width = 2;
  mm.height = 2;
  mm.order = ColorOrder::RGB;
  PixelMatrix pm(mm);
  FakeMatrixRegistry reg({&pm});
  std::string fsrc = readFennelSource();
  Harness h(fsrc, &reg);
  std::string err;
  CHECK(!h.eval(R"((glow.matrix.pattern 0 :not-a-pattern {}))", &err));
  CHECK(err.find("unknown pattern") != std::string::npos);
}

// ---------------------------------------------------------------------------
// glow.wled.*
// ---------------------------------------------------------------------------

void test_wled_fx_sends_a_packet_to_the_named_target() {
  TEST("glow.wled.fx resolves a target by name and sends one packet");
  MockWledSink sink;
  WledManager wled(&sink);
  wled.addTarget({"main_matrix", "192.168.1.100", WLED_DEFAULT_PORT, 1});

  std::string fsrc = readFennelSource();
  Harness h(fsrc, nullptr, nullptr, nullptr, &wled);

  h.evalOrDie(R"((glow.wled.fx "main_matrix" :fire-2012
                  {:speed 180 :intensity 220 :brightness 200 :palette :fire}))");

  CHECK(sink.sendCount == 1);
  CHECK(sink.lastIp == "192.168.1.100");
  CHECK(sink.last[8] == 66);   // fire-2012
  CHECK(sink.last[9] == 180);  // speed
  CHECK(sink.last[16] == 220); // intensity
  CHECK(sink.last[2] == 200);  // brightness
  CHECK(sink.last[19] == 35);  // fire palette
}

void test_wled_color_sends_a_direct_change_packet() {
  TEST("glow.wled.color sends a solid-color direct-change packet");
  MockWledSink sink;
  WledManager wled(&sink);
  wled.addTarget({"tree", "192.168.1.101", WLED_DEFAULT_PORT, 1});

  std::string fsrc = readFennelSource();
  Harness h(fsrc, nullptr, nullptr, nullptr, &wled);
  h.evalOrDie(R"((glow.wled.color "tree" 255 0 0 {:brightness 255 :transition 500}))");

  CHECK(sink.sendCount == 1);
  CHECK(sink.last[1] == 0x01);  // direct change
  CHECK(sink.last[3] == 255 && sink.last[4] == 0 && sink.last[5] == 0);
}

void test_wled_on_off_toggle_brightness() {
  TEST("glow.wled.on/off toggle brightness on the named target");
  MockWledSink sink;
  WledManager wled(&sink);
  wled.addTarget({"tree", "192.168.1.101", WLED_DEFAULT_PORT, 1});

  std::string fsrc = readFennelSource();
  Harness h(fsrc, nullptr, nullptr, nullptr, &wled);

  h.evalOrDie(R"((glow.wled.off "tree"))");
  CHECK(sink.sendCount == 1);
  CHECK(sink.last[2] == 0);

  h.evalOrDie(R"((glow.wled.on "tree"))");
  CHECK(sink.sendCount == 2);
  CHECK(sink.last[2] == 255);
}

void test_wled_fx_broadcast_sends_to_broadcast_address() {
  TEST("glow.wled.fx-broadcast sends to 255.255.255.255 without a named target");
  MockWledSink sink;
  WledManager wled(&sink);

  std::string fsrc = readFennelSource();
  Harness h(fsrc, nullptr, nullptr, nullptr, &wled);
  h.evalOrDie(R"((glow.wled.fx-broadcast :pacifica {:speed 100 :intensity 150 :palette :ocean}))");

  CHECK(sink.sendCount == 1);
  CHECK(sink.lastIp == "255.255.255.255");
  CHECK(sink.last[8] == 101);  // pacifica
}

void test_wled_fx_unknown_target_errors_cleanly() {
  TEST("glow.wled.fx on an unknown target name errors, not crashes");
  MockWledSink sink;
  WledManager wled(&sink);
  wled.addTarget({"tree", "192.168.1.101", WLED_DEFAULT_PORT, 1});

  std::string fsrc = readFennelSource();
  Harness h(fsrc, nullptr, nullptr, nullptr, &wled);
  std::string err;
  CHECK(!h.eval(R"((glow.wled.fx "no_such_target" :solid))", &err));
  CHECK(err.find("unknown WLED target") != std::string::npos);
  CHECK(sink.sendCount == 0);
}

void test_wled_without_manager_errors_cleanly() {
  TEST("glow.wled.* with no WledManager configured errors instead of crashing");
  std::string fsrc = readFennelSource();
  Harness h(fsrc);  // wled defaults to nullptr
  std::string err;
  CHECK(!h.eval(R"((glow.wled.fx "tree" :solid))", &err));
  CHECK(err.find("no WLED targets") != std::string::npos);
}

// ---------------------------------------------------------------------------
// glow.beat / glow.bar / glow.beat-number / glow.bpm / glow.locked? / glow.tap
// ---------------------------------------------------------------------------

void test_glow_bpm_locked_default_free_running() {
  TEST("glow.bpm/glow.locked?: a fresh clock is unlocked, glow.beat still returns a sensible phase");
  std::string fsrc = readFennelSource();
  Harness h(fsrc);
  CHECK(h.evalBool("(glow.locked?)") == false);
  CHECK(h.evalNumber("(glow.bpm)") == 0.0);  // no tempo established yet -- "unknown", not garbage
  double beat = h.evalNumber("(glow.beat)");
  CHECK(beat >= 0.0 && beat < 1.0);  // still a real, in-range number, never nil/NaN
}

void test_glow_tap_drives_the_underlying_beat_clock() {
  TEST("glow.tap: four taps from Fennel converge the underlying BeatClock to ~120 BPM");
  std::string fsrc = readFennelSource();
  Harness h(fsrc);

  double tSec = 0.0;
  for (int i = 0; i < 4; ++i) {
    h.evalTime = static_cast<float>(tSec);
    h.evalOrDie("(glow.tap)");
    tSec += 0.5;  // 500ms between taps -> 120 BPM
  }

  CHECK(std::fabs(h.beatClock.bpm() - 120.0f) < 1.0f);

  // The Lua-side glow.bpm() reads the SAME clock (not a copy).
  CHECK(std::fabs(h.evalNumber("(glow.bpm)") - 120.0) < 1.0);
}

void test_glow_beat_pulses_with_tapped_tempo_no_snap() {
  TEST("a Fennel effect using (glow.beat) pulses in time with a tapped tempo, no visible snapping");
  std::string fsrc = readFennelSource();
  Harness h(fsrc);

  // Establish a steady 120 BPM via glow.tap, exactly like a console user
  // tapping along (T4's whole point: develop/test beat-synced effects with
  // no external gear).
  double tSec = 0.0;
  for (int i = 0; i < 4; ++i) {
    h.evalTime = static_cast<float>(tSec);
    h.evalOrDie("(glow.tap)");
    tSec += 0.5;
  }

  // "pulse decaying every beat" -- exactly the DoD's example effect.
  const char* src = "(fn pulse [t] (glow.set 1 :dimmer (- 1 (glow.beat))))";
  int ref = h.compileToRef(src);
  LuaEffect fx(h.api, ref, "pulse");

  // Sample densely across a couple of beats; the dimmer value must ramp
  // down smoothly (1 -> 0) within each beat and never jump back up except
  // exactly at a beat boundary (where it wraps 0 -> ~1), and even that
  // wrap must not overshoot past 1.0.
  double prevDimmer = -1.0;
  int wraps = 0;
  for (int i = 0; i < 400; ++i) {
    float t = static_cast<float>(tSec + i * (0.5 / 40.0));  // 40 samples/beat
    std::vector<CapIntent> caps;
    std::vector<AimIntent> aims;
    fx.evaluate(t, caps, aims);
    CHECK(!fx.disabled());
    CHECK(caps.size() == 1);
    double dimmer = caps[0].norm01;
    CHECK(dimmer >= -1e-4 && dimmer <= 1.0 + 1e-4);  // never overshoots
    if (prevDimmer >= 0.0 && dimmer > prevDimmer + 0.05) {
      ++wraps;  // a legitimate beat-boundary wrap (0 -> ~1)
    } else if (prevDimmer >= 0.0) {
      CHECK(dimmer <= prevDimmer + 0.03);  // otherwise: smooth decay, no snap
    }
    prevDimmer = dimmer;
  }
  CHECK(wraps >= 4);  // ~5 beats sampled -> should see several wraps
}

// ---------------------------------------------------------------------------
// glow.ranges (v2 introspection)
// ---------------------------------------------------------------------------

void test_glow_ranges_lists_named_and_continuous() {
  TEST("glow.ranges lists a capability's ranges: name/index/continuous?");
  ProfileBuilder builder;
  builder.setFootprint(2).add(Capability::ColorWheel, 0).add(Capability::ShutterStrobe, 1);
  builder.addRange(0, 0, 9, false, "open");
  builder.addRange(0, 10, 19, false, "red");
  builder.addRange(1, 32, 63, true, "strobe");
  auto blob = builder.encode();
  FixtureProfile profile;
  CHECK(parseProfile(blob.data(), blob.size(), profile));

  FakeFixtureRegistry reg;
  reg.put(1, profile);

  std::string fsrc = readFennelSource();
  Harness h(fsrc, nullptr, &reg);

  CHECK(h.evalNumber("(length (glow.ranges 1 :color-wheel))") == 2.0);
  CHECK(h.evalBool("(= (. (glow.ranges 1 :color-wheel) 1 \"name\") \"open\")"));
  CHECK(h.evalBool("(= (. (glow.ranges 1 :color-wheel) 1 \"index\") 0)"));
  CHECK(h.evalBool("(= (. (glow.ranges 1 :color-wheel) 1 \"continuous?\") false)"));
  CHECK(h.evalBool("(= (. (glow.ranges 1 :color-wheel) 2 \"name\") \"red\")"));

  CHECK(h.evalNumber("(length (glow.ranges 1 :shutter-strobe))") == 1.0);
  CHECK(h.evalBool("(= (. (glow.ranges 1 :shutter-strobe) 1 \"continuous?\") true)"));

  // Unknown fixture / capability the fixture lacks -> empty table, not an error.
  CHECK(h.evalNumber("(length (glow.ranges 99 :color-wheel))") == 0.0);
  CHECK(h.evalNumber("(length (glow.ranges 1 :gobo))") == 0.0);
}

void test_glow_ranges_without_registry_errors_cleanly() {
  TEST("glow.ranges with no IFixtureRegistry errors with a clear message");
  std::string fsrc = readFennelSource();
  Harness h(fsrc);  // no fixture registry

  std::string err;
  CHECK(!h.eval("(glow.ranges 1 :color-wheel)", &err));
  CHECK(err.find("fixture registry") != std::string::npos);
}

namespace {
// A tiny 2-row x 4-column grid: notes 53/54, each channel-significant 0..3
// -- the same shape as the APC40's clip-launch grid, just smaller.
MidiControllerProfile buildGridProfile() {
  ControllerBuilder b;
  b.name = "Test Grid";
  b.pads.push_back({53, 53, 0, 3});  // row 0
  b.pads.push_back({54, 54, 0, 3});  // row 1
  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  MidiControllerProfile p;
  if (!err.empty() || !parseMidiController(blob.data(), blob.size(), p)) {
    printf("FATAL: buildGridProfile failed: %s\n", err.c_str());
    std::abort();
  }
  return p;
}

// Same grid, but with a channel-significant LED range covering both rows
// (mirrors the APC40's clip grid, where LED output honours the channel) --
// this is the shape glow.led.set-xy/auto-xy need to prove the resolved
// channel actually reaches the emitted MIDI bytes.
MidiControllerProfile buildGridProfileWithLed() {
  ControllerBuilder b;
  b.name = "Test Grid With LED";
  b.pads.push_back({53, 53, 0, 3});  // row 0
  b.pads.push_back({54, 54, 0, 3});  // row 1

  ControllerLedSpec led;
  led.msgType = LedMsgType::Note;
  led.addrFrom = 53;
  led.addrTo = 54;
  led.semantic = LedSemantic::Velocity;
  led.colors.push_back({"off", 0});
  led.colors.push_back({"green", 1});
  led.channelFrom = 0;
  led.channelTo = 3;
  b.leds.push_back(led);

  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  MidiControllerProfile p;
  if (!err.empty() || !parseMidiController(blob.data(), blob.size(), p)) {
    printf("FATAL: buildGridProfileWithLed failed: %s\n", err.c_str());
    std::abort();
  }
  return p;
}

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
}  // namespace

void test_bind_pad_xy_resolves_grid_and_binds_packed_id() {
  TEST("glow.bind.pad-xy: resolves (col,row) via the .mdef grid, fires independently per channel");

  MidiControllerProfile profile = buildGridProfile();
  LedFeedback lf(profile);  // no IMidiOutput needed -- only its profile() is read here
  std::string fsrc = readFennelSource();
  Harness h(fsrc, nullptr, nullptr, &lf);
  h.live.setControllerProfile(&profile);

  h.evalOrDie("(glow.cue.define :a {:effects []})");
  h.evalOrDie("(glow.cue.define :b {:effects []})");
  h.evalOrDie("(glow.bind.pad-xy 0 0 :flash :a)");  // col=0,row=0 -> note 53, channel 0
  h.evalOrDie("(glow.bind.pad-xy 2 0 :flash :b)");  // col=2,row=0 -> note 53, channel 2

  uint16_t cueA, cueB;
  CHECK(h.api.cueIdForName("a", cueA));
  CHECK(h.api.cueIdForName("b", cueB));

  ControlEvent evCol0, evCol2;
  uint8_t msgCol0[] = {0x90, 53, 100};  // channel 0
  uint8_t msgCol2[] = {0x92, 53, 100};  // channel 2
  CHECK(parseMidi(msgCol0, 3, evCol0));
  CHECK(parseMidi(msgCol2, 3, evCol2));

  h.live.handle(evCol0, 0.0f);
  CHECK(h.show.isActive(cueA));
  CHECK(!h.show.isActive(cueB));

  h.live.handle(evCol2, 0.0f);
  CHECK(h.show.isActive(cueB));
}

void test_bind_pad_xy_out_of_range_is_a_no_op() {
  TEST("glow.bind.pad-xy: out-of-range coordinate is a no-op, not an error");

  MidiControllerProfile profile = buildGridProfile();
  LedFeedback lf(profile);
  std::string fsrc = readFennelSource();
  Harness h(fsrc, nullptr, nullptr, &lf);

  h.evalOrDie("(glow.cue.define :a {:effects []})");
  // row 5 doesn't exist (only rows 0/1 declared) -- must not error.
  CHECK(h.eval("(glow.bind.pad-xy 0 5 :flash :a)"));
  // col 9 is past this grid's channel span (0..3) -- must not error.
  CHECK(h.eval("(glow.bind.pad-xy 9 0 :flash :a)"));
}

void test_bind_pad_xy_no_led_feedback_is_a_no_op() {
  TEST("glow.bind.pad-xy: no LedFeedback wired (no .mdef) -- no-op, not an error");

  std::string fsrc = readFennelSource();
  Harness h(fsrc);  // no LedFeedback

  h.evalOrDie("(glow.cue.define :a {:effects []})");
  CHECK(h.eval("(glow.bind.pad-xy 0 0 :flash :a)"));
}

// ============================================================================
// glow.led.set-xy / glow.led.auto-xy: the LED half of pad-addressing parity
// -- resolves (col, row) via the same resolvePadXY as glow.bind.pad-xy, and
// the regression test for the channel bug: the resolved channel must reach
// the emitted MIDI status byte, not just get computed and dropped.
// ============================================================================

void test_led_set_xy_emits_resolved_channel() {
  TEST("glow.led.set-xy: resolves (col,row) -> (note,channel) and emits on that channel");

  MidiControllerProfile profile = buildGridProfileWithLed();
  FakeMidiOutput out;
  LedFeedback lf(profile, &out, 1000.0f);
  std::string fsrc = readFennelSource();
  Harness h(fsrc, nullptr, nullptr, &lf);

  h.evalOrDie("(glow.led.set-xy 2 0 :green)");  // col=2,row=0 -> note 53, channel 2
  lf.refresh(h.show, 0.0f);

  CHECK(out.sent.size() == 1);
  CHECK(out.sent[0].status == (0x90 | 0x02));  // Note On, channel nibble 2
  CHECK(out.sent[0].data1 == 53 && out.sent[0].data2 == 1);
}

void test_led_auto_xy_emits_resolved_channel() {
  TEST("glow.led.auto-xy: tracks a cue and emits on the resolved channel");

  MidiControllerProfile profile = buildGridProfileWithLed();
  FakeMidiOutput out;
  LedFeedback lf(profile, &out, 1000.0f);
  std::string fsrc = readFennelSource();
  Harness h(fsrc, nullptr, nullptr, &lf);

  h.evalOrDie("(glow.cue.define :chorus {:effects []})");
  h.evalOrDie("(glow.led.auto-xy 1 1 :chorus :green :off)");  // col=1,row=1 -> note 54, channel 1

  // Initial paint: cue inactive -> "off".
  lf.refresh(h.show, 0.0f);
  CHECK(out.sent.size() == 1);
  CHECK(out.sent[0].status == (0x90 | 0x01));  // channel nibble 1
  CHECK(out.sent[0].data1 == 54 && out.sent[0].data2 == 0);
  out.sent.clear();

  uint16_t cueId;
  CHECK(h.api.cueIdForName("chorus", cueId));
  h.show.go(cueId, 0.1f);
  lf.refresh(h.show, 0.1f);
  CHECK(out.sent.size() == 1);
  CHECK(out.sent[0].status == (0x90 | 0x01));
  CHECK(out.sent[0].data1 == 54 && out.sent[0].data2 == 1);  // "green"
}

void test_led_xy_out_of_range_is_a_no_op() {
  TEST("glow.led.set-xy/auto-xy: out-of-range (col,row) -> no-op, not an error");

  MidiControllerProfile profile = buildGridProfileWithLed();
  FakeMidiOutput out;
  LedFeedback lf(profile, &out, 1000.0f);
  std::string fsrc = readFennelSource();
  Harness h(fsrc, nullptr, nullptr, &lf);

  h.evalOrDie("(glow.cue.define :a {:effects []})");
  CHECK(h.eval("(glow.led.set-xy 0 5 :green)"));   // row 5 doesn't exist
  CHECK(h.eval("(glow.led.set-xy 9 0 :green)"));   // col 9 past this grid's channel span
  CHECK(h.eval("(glow.led.auto-xy 0 5 :a :green :off)"));
  CHECK(h.eval("(glow.led.auto-xy 9 0 :a :green :off)"));

  lf.refresh(h.show, 0.0f);
  CHECK(out.sent.empty());
}

void test_led_xy_no_led_feedback_is_a_no_op() {
  TEST("glow.led.set-xy/auto-xy: no LedFeedback wired (no .mdef) -- no-op, not an error");

  std::string fsrc = readFennelSource();
  Harness h(fsrc);  // no LedFeedback

  h.evalOrDie("(glow.cue.define :a {:effects []})");
  CHECK(h.eval("(glow.led.set-xy 0 0 :green)"));
  CHECK(h.eval("(glow.led.auto-xy 0 0 :a :green :off)"));
}

void test_bind_pad_xy_and_led_auto_xy_agree_on_note_and_channel() {
  TEST("glow.bind.pad-xy and glow.led.auto-xy on the same (col,row) resolve to the same (note,channel)");

  MidiControllerProfile profile = buildGridProfileWithLed();
  FakeMidiOutput out;
  LedFeedback lf(profile, &out, 1000.0f);
  std::string fsrc = readFennelSource();
  Harness h(fsrc, nullptr, nullptr, &lf);
  h.live.setControllerProfile(&profile);

  h.evalOrDie("(glow.cue.define :a {:effects []})");
  h.evalOrDie("(glow.bind.pad-xy 2 0 :flash :a)");        // col=2,row=0 -> note 53, channel 2
  h.evalOrDie("(glow.led.auto-xy 2 0 :a :green :off)");    // same coordinate

  // Initial paint: inactive -> off, on channel 2.
  lf.refresh(h.show, 0.0f);
  CHECK(out.sent.size() == 1);
  CHECK(out.sent[0].status == (0x90 | 0x02));
  CHECK(out.sent[0].data1 == 53 && out.sent[0].data2 == 0);
  out.sent.clear();

  // The incoming MIDI event that bind.pad-xy actually resolved to (note 53,
  // channel 2) activates the cue -- proving bind and feedback agree.
  uint16_t cueId;
  CHECK(h.api.cueIdForName("a", cueId));
  ControlEvent ev;
  uint8_t msg[] = {0x92, 53, 100};  // Note On, channel 2, note 53
  CHECK(parseMidi(msg, 3, ev));
  h.live.handle(ev, 0.1f);
  CHECK(h.show.isActive(cueId));

  lf.refresh(h.show, 0.1f);
  CHECK(out.sent.size() == 1);
  CHECK(out.sent[0].status == (0x90 | 0x02));
  CHECK(out.sent[0].data1 == 53 && out.sent[0].data2 == 1);  // "green"
}

void test_led_set_raw_note_unchanged() {
  TEST("glow.led.set: raw-note form unchanged (channel-agnostic) for a simple controller");

  ControllerBuilder b;
  b.name = "Simple";
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
  MidiControllerProfile profile;
  CHECK(parseMidiController(blob.data(), blob.size(), profile));

  FakeMidiOutput out;
  LedFeedback lf(profile, &out, 1000.0f);
  std::string fsrc = readFennelSource();
  Harness h(fsrc, nullptr, nullptr, &lf);

  h.evalOrDie("(glow.led.set 53 :on)");
  lf.refresh(h.show, 0.0f);
  CHECK(out.sent.size() == 1);
  CHECK(out.sent[0].status == 0x90);  // channel nibble 0, unchanged behaviour
  CHECK(out.sent[0].data1 == 53 && out.sent[0].data2 == 1);
}

void test_led_auto_raw_note_unchanged() {
  TEST("glow.led.auto: raw-note form unchanged (channel-agnostic) for a simple controller");

  ControllerBuilder b;
  b.name = "Simple";
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
  MidiControllerProfile profile;
  CHECK(parseMidiController(blob.data(), blob.size(), profile));

  FakeMidiOutput out;
  LedFeedback lf(profile, &out, 1000.0f);
  std::string fsrc = readFennelSource();
  Harness h(fsrc, nullptr, nullptr, &lf);

  h.evalOrDie("(glow.cue.define :chorus {:effects []})");
  h.evalOrDie("(glow.led.auto 53 :chorus :on :off)");
  lf.refresh(h.show, 0.0f);
  CHECK(out.sent.size() == 1);
  CHECK(out.sent[0].status == 0x90);  // channel nibble 0, unchanged behaviour
  CHECK(out.sent[0].data1 == 53 && out.sent[0].data2 == 0);
}

// ============================================================================
// samples/demo-boot.fnl: the acceptance case. Mirrors samples/apc40.mdef's
// 8-track x 5-scene clip-launch grid (notes 53..57, channel = track) --
// the real .mdef text isn't parsed here (parseControllerDef/provision.h
// aren't linked into this test binary; see the Makefile's
// GLOW_LUA_API_SOURCES), so it's built directly via ControllerBuilder,
// same approach as buildGridProfile/buildGridProfileWithLed above.
// ============================================================================

namespace {
MidiControllerProfile buildApc40GridProfile() {
  ControllerBuilder b;
  b.name = "Akai APC40 mkII (grid only)";
  for (uint8_t note = 53; note <= 57; ++note) {
    b.pads.push_back({note, note, 0, 7});
  }

  ControllerLedSpec led;
  led.msgType = LedMsgType::Note;
  led.addrFrom = 53;
  led.addrTo = 57;
  led.semantic = LedSemantic::Velocity;
  led.colors.push_back({"off", 0});
  led.colors.push_back({"red", 5});
  led.colors.push_back({"yellow", 13});
  led.colors.push_back({"green", 21});
  led.colors.push_back({"blue", 41});
  led.channelFrom = 0;
  led.channelTo = 7;
  b.leds.push_back(led);

  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  MidiControllerProfile p;
  if (!err.empty() || !parseMidiController(blob.data(), blob.size(), p)) {
    printf("FATAL: buildApc40GridProfile failed: %s\n", err.c_str());
    std::abort();
  }
  return p;
}
}  // namespace

void test_demo_boot_fnl_loads_clean_and_pads_mirror_cues() {
  TEST("samples/demo-boot.fnl loads clean on the APC40 grid; pads mirror their cues");

  MidiControllerProfile profile = buildApc40GridProfile();
  FakeMidiOutput out;
  LedFeedback lf(profile, &out, 1000.0f);
  std::string fsrc = readFennelSource();
  Harness h(fsrc, nullptr, nullptr, &lf);
  h.live.setControllerProfile(&profile);

  std::string demoBoot = readDemoBootFnl();
  CHECK(!demoBoot.empty());
  std::string err;
  if (!h.eval(demoBoot.c_str(), &err)) {
    printf("FAIL: samples/demo-boot.fnl did not load clean: %s\n", err.c_str());
    g_failCount++;
    return;
  }

  // Initial LED paint: every bound pad's cue starts inactive -> "off".
  lf.refresh(h.show, 0.0f);
  out.sent.clear();

  // col=0,row=0 -> note 53, channel 0, bound to :warm (flash), LED red/off.
  uint16_t warmId;
  CHECK(h.api.cueIdForName("warm", warmId));
  ControlEvent ev;
  uint8_t msg[] = {0x90, 53, 100};  // Note On, channel 0, note 53
  CHECK(parseMidi(msg, 3, ev));
  h.live.handle(ev, 0.1f);
  CHECK(h.show.isActive(warmId));

  lf.refresh(h.show, 0.1f);
  CHECK(out.sent.size() == 1);
  CHECK(out.sent[0].status == 0x90);  // channel nibble 0 -- the bind and the LED agree
  CHECK(out.sent[0].data1 == 53 && out.sent[0].data2 == 5);  // "red"
  out.sent.clear();

  // col=1,row=1 -> note 54, channel 1, bound to :verse (toggle), LED yellow/off.
  uint16_t verseId;
  CHECK(h.api.cueIdForName("verse", verseId));
  ControlEvent ev2;
  uint8_t msg2[] = {0x91, 54, 100};  // Note On, channel 1, note 54
  CHECK(parseMidi(msg2, 3, ev2));
  h.live.handle(ev2, 0.2f);
  CHECK(h.show.isActive(verseId));

  lf.refresh(h.show, 0.2f);
  CHECK(out.sent.size() == 1);
  CHECK(out.sent[0].status == (0x90 | 0x01));  // channel nibble 1
  CHECK(out.sent[0].data1 == 54 && out.sent[0].data2 == 13);  // "yellow"
}

// ============================================================================
// P1.2: glow.bind.pitchbend/pressure/program, glow.param.get
// ============================================================================

void test_bind_pitchbend_param_drives_glow_param_get() {
  TEST("glow.bind.pitchbend :param -- the wheel drives glow.param.get");

  std::string fsrc = readFennelSource();
  Harness h(fsrc);

  h.evalOrDie("(glow.bind.pitchbend :param :hue)");
  CHECK(std::fabs(h.evalNumber("(glow.param.get :hue)")) < 1e-6);  // unset -> 0

  ControlEvent ev;
  uint8_t msg[] = {0xE0, 0x7F, 0x7F};  // Pitch Bend, max value -> 1.0
  CHECK(parseMidi(msg, 3, ev));
  h.live.handle(ev, 0.0f);

  CHECK(std::fabs(h.evalNumber("(glow.param.get :hue)") - 1.0) < 1e-3);
}

void test_bind_pressure_cue_level_holds_cue() {
  TEST("glow.bind.pressure :cue-level -- channel pressure holds a cue at its level");

  std::string fsrc = readFennelSource();
  Harness h(fsrc);

  h.evalOrDie("(glow.cue.define :chorus {:effects []})");
  h.evalOrDie("(glow.bind.pressure :cue-level :chorus)");

  uint16_t chorusId;
  CHECK(h.api.cueIdForName("chorus", chorusId));
  CHECK(!h.show.isActive(chorusId));

  ControlEvent ev;
  uint8_t msg[] = {0xD2, 64};  // Channel Pressure, channel 2, ~0.5
  CHECK(parseMidi(msg, 2, ev));
  h.live.handle(ev, 0.0f);
  CHECK(h.show.isActive(chorusId));
}

void test_bind_fader_cue_level_and_param() {
  TEST("glow.bind.fader :cue-level / :param -- the fader reaches the same continuous targets");

  std::string fsrc = readFennelSource();
  Harness h(fsrc);

  h.evalOrDie("(glow.cue.define :bump {:effects []})");
  h.evalOrDie("(glow.bind.fader 10 :cue-level :bump)");
  h.evalOrDie("(glow.bind.fader 11 :param :depth)");

  uint16_t bumpId;
  CHECK(h.api.cueIdForName("bump", bumpId));

  h.live.handle({ControlType::Fader, static_cast<uint16_t>(128 + 10), 0, false, 0.6f}, 0.0f);
  CHECK(h.show.isActive(bumpId));

  h.live.handle({ControlType::Fader, static_cast<uint16_t>(128 + 11), 0, false, 0.3f}, 0.0f);
  CHECK(std::fabs(h.evalNumber("(glow.param.get :depth)") - 0.3) < 1e-3);
}

void test_bind_program_scene_selector() {
  TEST("glow.bind.program :scene -- program N selects scene N");

  std::string fsrc = readFennelSource();
  Harness h(fsrc);

  h.evalOrDie("(glow.cue.define :a {:effects []})");
  h.evalOrDie("(glow.scene.define :verse [:a])");
  h.evalOrDie("(glow.bind.program :scene)");

  uint16_t cueA;
  CHECK(h.api.cueIdForName("a", cueA));
  uint16_t sceneVerse;
  CHECK(h.api.sceneIdForName("verse", sceneVerse));

  ControlEvent ev;
  uint8_t msg[] = {0xC0, static_cast<uint8_t>(sceneVerse)};
  CHECK(parseMidi(msg, 2, ev));
  h.live.handle(ev, 0.0f);
  CHECK(h.show.isActive(cueA));
}

void test_bind_program_only_scene_keyword_valid() {
  TEST("glow.bind.program: any keyword other than :scene errors");

  std::string fsrc = readFennelSource();
  Harness h(fsrc);
  std::string err;
  CHECK(!h.eval("(glow.bind.program :cue)", &err));
  CHECK(!err.empty());
}

void test_bind_pitchbend_unknown_action_errors() {
  TEST("glow.bind.pitchbend: an unknown action keyword errors, not crashes");

  std::string fsrc = readFennelSource();
  Harness h(fsrc);
  std::string err;
  CHECK(!h.eval("(glow.bind.pitchbend :sideways)", &err));
  CHECK(!err.empty());
}

void test_bind_pressure_unknown_cue_errors() {
  TEST("glow.bind.pressure :cue-level with an unknown cue name errors");

  std::string fsrc = readFennelSource();
  Harness h(fsrc);
  std::string err;
  CHECK(!h.eval("(glow.bind.pressure :cue-level :nope)", &err));
  CHECK(!err.empty());
}

void test_param_get_never_bound_is_zero() {
  TEST("glow.param.get on a name never bound to -- 0, not an error");

  std::string fsrc = readFennelSource();
  Harness h(fsrc);
  CHECK(std::fabs(h.evalNumber("(glow.param.get :nonexistent)")) < 1e-6);
}

void test_two_controllers_pitchbend_inert_without_wheel() {
  TEST("Two controllers, same boot.fnl: pitchbend binding is inert on the wheel-less one");

  std::string fsrc = readFennelSource();
  Harness h(fsrc);

  h.evalOrDie("(glow.cue.define :a {:effects []})");
  h.evalOrDie("(glow.bind.pad 60 :flash :a)");      // both controllers have this pad
  h.evalOrDie("(glow.bind.pitchbend :param :hue)");  // only a controller with a wheel sends this

  // Controller WITHOUT a wheel: only sends the pad. Works fine; the wheel
  // binding simply never fires because nothing calls handle() with a
  // PitchBend event for it.
  h.live.handle({ControlType::Button, 60, 0, true, 0.0f}, 0.0f);
  uint16_t cueA;
  CHECK(h.api.cueIdForName("a", cueA));
  CHECK(h.show.isActive(cueA));
  CHECK(std::fabs(h.evalNumber("(glow.param.get :hue)")) < 1e-6);  // untouched

  // Controller WITH a wheel: same bindings, wheel event now fires.
  ControlEvent ev;
  uint8_t msg[] = {0xE0, 0x7F, 0x7F};
  CHECK(parseMidi(msg, 3, ev));
  h.live.handle(ev, 1.0f);
  CHECK(std::fabs(h.evalNumber("(glow.param.get :hue)") - 1.0) < 1e-3);
}

int main() {
  test_cue_define_and_go_activates_showcontroller_cue();
  test_cue_release_deactivates();
  test_cue_go_unknown_name_errors();
  test_cue_redefine_by_name_orphans_old_active_cue();

  test_cue_glow_slot_range_selection_survives_priority_resolution();
  test_cue_glow_slot_on_intensity_class_resolves_by_priority_not_htp();

  test_scene_go_activates_all_member_cues();
  test_scene_define_unknown_cue_errors();

  test_fx_hue_rotate_handle_usable_in_cue_and_emits();
  test_fx_sweep_handle();
  test_cue_define_rejects_invalid_effects_entry();

  test_poll_newly_disabled_effects_reports_once();

  test_matrix_pattern_and_brightness_apply_to_the_registered_matrix();
  test_matrix_pattern_unknown_index_errors();
  test_matrix_without_registry_errors_cleanly();
  test_matrix_unknown_pattern_name_errors();

  test_glow_bpm_locked_default_free_running();
  test_glow_tap_drives_the_underlying_beat_clock();
  test_glow_beat_pulses_with_tapped_tempo_no_snap();

  test_glow_ranges_lists_named_and_continuous();
  test_glow_ranges_without_registry_errors_cleanly();

  test_wled_fx_sends_a_packet_to_the_named_target();
  test_wled_color_sends_a_direct_change_packet();
  test_wled_on_off_toggle_brightness();
  test_wled_fx_broadcast_sends_to_broadcast_address();
  test_wled_fx_unknown_target_errors_cleanly();
  test_wled_without_manager_errors_cleanly();

  test_bind_pad_xy_resolves_grid_and_binds_packed_id();
  test_bind_pad_xy_out_of_range_is_a_no_op();
  test_bind_pad_xy_no_led_feedback_is_a_no_op();

  test_led_set_xy_emits_resolved_channel();
  test_led_auto_xy_emits_resolved_channel();
  test_led_xy_out_of_range_is_a_no_op();
  test_led_xy_no_led_feedback_is_a_no_op();
  test_bind_pad_xy_and_led_auto_xy_agree_on_note_and_channel();
  test_led_set_raw_note_unchanged();
  test_led_auto_raw_note_unchanged();
  test_demo_boot_fnl_loads_clean_and_pads_mirror_cues();

  test_bind_pitchbend_param_drives_glow_param_get();
  test_bind_pressure_cue_level_holds_cue();
  test_bind_fader_cue_level_and_param();
  test_bind_program_scene_selector();
  test_bind_program_only_scene_keyword_valid();
  test_bind_pitchbend_unknown_action_errors();
  test_bind_pressure_unknown_cue_errors();
  test_param_get_never_bound_is_zero();
  test_two_controllers_pitchbend_inert_without_wheel();

  if (g_failCount == 0) {
    printf("\nAll tests passed.\n");
    return 0;
  }
  printf("\n%d test(s) failed.\n", g_failCount);
  return 1;
}
