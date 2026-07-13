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

#include "lua_effect.h"
#include "lua_vm.h"
#include "pixel_matrix.h"
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

struct Harness {
  ShowController show;
  glow::LuaVM vm;
  IMatrixRegistry* matrices;
  glow::BeatClock beatClock;
  GlowLuaApi api;

  Harness(const std::string& fennelSrc, IMatrixRegistry* mats = nullptr)
      : vm(), matrices(mats), api(vm, show, mats, beatClock) {
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

int main() {
  test_cue_define_and_go_activates_showcontroller_cue();
  test_cue_release_deactivates();
  test_cue_go_unknown_name_errors();
  test_cue_redefine_by_name_orphans_old_active_cue();

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

  if (g_failCount == 0) {
    printf("\nAll tests passed.\n");
    return 0;
  }
  printf("\n%d test(s) failed.\n", g_failCount);
  return 1;
}
