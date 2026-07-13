// test_lua_effect.cpp — LuaEffect + GlowLuaApi + the vendored Fennel
// compiler, exercised together: the emit pattern, the error-disable
// contract, and the zero-allocation guarantee on the per-frame path.

#include "lua_effect.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "glow_lua_api.h"
#include "lua_vm.h"
#include "show.h"
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

// Bundles a ready-to-use VM + api + show for one test. Not copyable (LuaVM
// isn't); construct one per test.
struct Harness {
  ShowController show;
  glow::LuaVM vm;
  glow::BeatClock beatClock;
  GlowLuaApi api;

  explicit Harness(const std::string& fennelSrc) : vm(), api(vm, show, nullptr, beatClock) {
    api.install();
    char err[256];
    bool ok = vm.loadFennelCompiler(fennelSrc.data(), fennelSrc.size(), err, sizeof(err));
    if (!ok) {
      printf("FATAL: loadFennelCompiler failed: %s\n", err);
      std::abort();
    }
    vm.collectFullyOnce();
  }

  // Compiles+runs `src` in the sandboxed env, aborting the test process on
  // failure (this is setup code, not the behavior under test).
  void bootOrDie(const char* src) {
    lua_State* L = vm.state();
    vm.pushFennelModule();
    lua_getfield(L, -1, "eval");
    lua_remove(L, -2);
    lua_pushlstring(L, src, std::strlen(src));
    lua_newtable(L);
    vm.pushSandboxEnv();
    lua_setfield(L, -2, "env");
    vm.armEvalBudget();
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
      printf("FATAL: boot eval failed: %s\n", lua_tostring(L, -1));
      std::abort();
    }
  }

  // Compiles `src` to a bare function value (no top-level call), leaving
  // it on the stack, and takes a registry ref to it -- for constructing a
  // LuaEffect directly without going through glow.cue.define.
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

  // Evaluates `src` directly (not wrapped in a LuaEffect, so no frame
  // context is ever set up) and returns whether it succeeded, filling
  // errOut on failure. Used to test glow.set/glow.aim's "only valid inside
  // an effect callback" guard.
  bool evalDirect(const char* src, std::string& errOut) {
    lua_State* L = vm.state();
    vm.pushFennelModule();
    lua_getfield(L, -1, "eval");
    lua_remove(L, -2);
    lua_pushlstring(L, src, std::strlen(src));
    lua_newtable(L);
    vm.pushSandboxEnv();
    lua_setfield(L, -2, "env");
    vm.armEvalBudget();
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
      const char* msg = lua_tostring(L, -1);
      errOut = msg ? msg : "(non-string error)";
      lua_pop(L, 1);
      return false;
    }
    return true;
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// Emit pattern
// ---------------------------------------------------------------------------

void test_emit_cap_and_aim() {
  TEST("LuaEffect emits CapIntent/AimIntent matching the design doc's proof");
  std::string fsrc = readFennelSource();
  Harness h(fsrc);

  const char* src =
      "(fn my-effect [t]\n"
      "  (glow.set 1 :dimmer (* 0.5 (+ 1 (math.sin (* t 2)))))\n"
      "  (glow.set 1 :red 1.0)\n"
      "  (glow.aim 2 [0 2 5]))\n";
  int ref = h.compileToRef(src);
  LuaEffect fx(h.api, ref, "my-effect");

  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;
  fx.evaluate(0.0f, caps, aims);

  CHECK(!fx.disabled());
  CHECK(caps.size() == 2);
  CHECK(caps[0].fixtureId == 1);
  CHECK(caps[0].cap == Capability::Dimmer);
  CHECK(std::fabs(caps[0].norm01 - 0.5f) < 1e-4f);
  CHECK(caps[1].fixtureId == 1);
  CHECK(caps[1].cap == Capability::Red);
  CHECK(std::fabs(caps[1].norm01 - 1.0f) < 1e-4f);

  CHECK(aims.size() == 1);
  CHECK(aims[0].fixtureId == 2);
  CHECK(aims[0].isPoint == true);
  CHECK(std::fabs(aims[0].target.x - 0.0f) < 1e-4f);
  CHECK(std::fabs(aims[0].target.y - 2.0f) < 1e-4f);
  CHECK(std::fabs(aims[0].target.z - 5.0f) < 1e-4f);
}

void test_unknown_capability_name_is_noop() {
  TEST("glow.set with an unresolvable capability name is a no-op, not an error");
  std::string fsrc = readFennelSource();
  Harness h(fsrc);
  int ref = h.compileToRef("(fn f [t] (glow.set 1 :not-a-real-cap 1.0) (glow.set 1 :red 0.5))");
  LuaEffect fx(h.api, ref, "f");

  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;
  fx.evaluate(0.0f, caps, aims);

  CHECK(!fx.disabled());
  CHECK(caps.size() == 1);  // only the valid :red made it through
  CHECK(caps[0].cap == Capability::Red);
}

void test_bad_args_error_not_crash() {
  TEST("glow.set/glow.aim with wrong arg types errors (disables), never crashes");
  std::string fsrc = readFennelSource();
  {
    Harness h(fsrc);
    int ref = h.compileToRef("(fn f [t] (glow.set \"not-a-number\" :red 0.5))");
    LuaEffect fx(h.api, ref, "f");
    std::vector<CapIntent> caps;
    std::vector<AimIntent> aims;
    fx.evaluate(0.0f, caps, aims);
    CHECK(fx.disabled());
    CHECK(caps.empty());
  }
  {
    Harness h(fsrc);
    int ref = h.compileToRef("(fn f [t] (glow.aim 1 \"not-a-table\"))");
    LuaEffect fx(h.api, ref, "f");
    std::vector<CapIntent> caps;
    std::vector<AimIntent> aims;
    fx.evaluate(0.0f, caps, aims);
    CHECK(fx.disabled());
    CHECK(aims.empty());
  }
}

void test_glow_set_outside_frame_context_errors() {
  TEST("glow.set/glow.aim outside an effect callback (no frame context) errors");
  std::string fsrc = readFennelSource();
  Harness h(fsrc);

  std::string err;
  CHECK(!h.evalDirect("(glow.set 1 :dimmer 0.5)", err));
  CHECK(err.find("effect callback") != std::string::npos);

  err.clear();
  CHECK(!h.evalDirect("(glow.aim 1 [0 0 1])", err));
  CHECK(err.find("effect callback") != std::string::npos);

  // cue.go/release are fine outside a frame context (noteEvalTime covers
  // them) -- only glow.set/glow.aim need an active effect callback.
  h.api.noteEvalTime(0.0f);
  err.clear();
  CHECK(h.evalDirect("(glow.cue.define :x {:effects [] :priority 0})", err));
}

// ---------------------------------------------------------------------------
// Error-disable contract
// ---------------------------------------------------------------------------

void test_erroring_effect_disables_permanently_not_retried() {
  TEST("an erroring effect: disabled_=true, not retried on later frames");
  std::string fsrc = readFennelSource();
  Harness h(fsrc);
  int ref = h.compileToRef("(fn f [t] (error \"boom\"))");
  LuaEffect fx(h.api, ref, "f");

  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;
  fx.evaluate(0.0f, caps, aims);
  CHECK(fx.disabled());
  CHECK(fx.lastError().find("boom") != std::string::npos);

  // Frame 2: must not re-invoke the Lua function at all (evaluate()
  // returns immediately when disabled_). Nothing to observe directly
  // except that it doesn't crash and stays disabled with the same error.
  std::string errBefore = fx.lastError();
  fx.evaluate(1.0f, caps, aims);
  CHECK(fx.disabled());
  CHECK(fx.lastError() == errBefore);
  CHECK(caps.empty());
}

void test_cue_keeps_running_other_effects_after_one_disables() {
  TEST("ShowController cue: a broken effect doesn't take down its cue-mates");
  std::string fsrc = readFennelSource();
  Harness h(fsrc);

  int refBroken = h.compileToRef("(fn broken [t] (error \"boom\"))");
  int refGood = h.compileToRef("(fn ok-fx [t] (glow.set 9 :dimmer 1.0))");
  LuaEffect broken(h.api, refBroken, "broken");
  LuaEffect good(h.api, refGood, "ok-fx");

  std::vector<IEffect*> effects{&broken, &good};
  uint16_t cueId = h.show.addCue(effects, 0.0f, 0.0f, 0);
  h.show.go(cueId, 0.0f);

  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;
  h.show.evaluate(0.0f, caps, aims);
  CHECK(broken.disabled());
  CHECK(!good.disabled());
  CHECK(caps.size() == 1);
  CHECK(caps[0].fixtureId == 9);

  // Next frame: still just the good effect's contribution.
  caps.clear();
  aims.clear();
  h.show.evaluate(1.0f, caps, aims);
  CHECK(caps.size() == 1);
}

// ---------------------------------------------------------------------------
// Zero allocation on the per-frame path
// ---------------------------------------------------------------------------

void test_zero_allocation_wellwritten_effect() {
  TEST("zero-allocation: literal-capability effect allocates nothing after warm-up");
  std::string fsrc = readFennelSource();
  Harness h(fsrc);
  int ref = h.compileToRef(
      "(fn f [t] (glow.set 1 :dimmer 0.5) (glow.set 1 :red 1.0))");
  LuaEffect fx(h.api, ref, "f");

  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;
  caps.reserve(128);
  aims.reserve(64);

  // Warm-up: first call may allocate (stack growth, etc).
  fx.evaluate(0.0f, caps, aims);
  CHECK(!fx.disabled());

  h.vm.resetAllocCallCount();
  for (int i = 1; i <= 10; ++i) {
    caps.clear();
    aims.clear();
    fx.evaluate(static_cast<float>(i), caps, aims);
  }
  CHECK(h.vm.allocCallCount() == 0);
}

void test_nonzero_allocation_constructed_string_effect() {
  TEST("zero-allocation check proves itself: a constructed string DOES allocate");
  std::string fsrc = readFennelSource();
  Harness h(fsrc);
  // ".." concatenation builds a new string every call -- the documented
  // hazard (design doc 5.2) as opposed to a literal capability name. The
  // suffix must vary per call: Lua 5.4 interns short strings, so
  // concatenating the same two literals every frame would dedupe to the
  // same already-interned string after the first call and allocate
  // nothing -- exactly the failure mode this test needs to rule out.
  // Mixing in `t` (which differs every frame in the loop below) forces a
  // genuinely new string each time.
  int ref = h.compileToRef("(fn f [t] (glow.set 1 (.. \"cap\" (tostring t)) 0.5))");
  LuaEffect fx(h.api, ref, "f");

  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;
  caps.reserve(128);
  aims.reserve(64);

  fx.evaluate(0.0f, caps, aims);
  CHECK(!fx.disabled());

  h.vm.resetAllocCallCount();
  for (int i = 1; i <= 10; ++i) {
    caps.clear();
    aims.clear();
    fx.evaluate(static_cast<float>(i), caps, aims);
  }
  CHECK(h.vm.allocCallCount() > 0);
}

int main() {
  test_emit_cap_and_aim();
  test_unknown_capability_name_is_noop();
  test_bad_args_error_not_crash();
  test_glow_set_outside_frame_context_errors();
  test_erroring_effect_disables_permanently_not_retried();
  test_cue_keeps_running_other_effects_after_one_disables();
  test_zero_allocation_wellwritten_effect();
  test_nonzero_allocation_constructed_string_effect();

  if (g_failCount == 0) {
    printf("\nAll tests passed.\n");
    return 0;
  }
  printf("\n%d test(s) failed.\n", g_failCount);
  return 1;
}
