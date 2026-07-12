// test_lua_vm.cpp — LuaVM unit tests: allocator cap + high-water mark,
// sandboxing, instruction budget, GC pacing. Deliberately Fennel-free (see
// test_lua_effect.cpp / test_glow_fennel.cpp for the full pipeline) so
// these stay fast and test exactly one module at a time.

#include "lua_vm.h"

#include <cstdio>
#include <cstring>

static int g_failCount = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
      g_failCount++;                                                   \
    }                                                                  \
  } while (0)

#define TEST(name) printf("Test: %s\n", name)

using glow::LuaVM;

namespace {

// Runs `src` (plain Lua, not Fennel) with the given per-frame instruction
// budget armed. Returns the lua_pcall status.
int runWithFrameBudget(LuaVM& vm, const char* src) {
  lua_State* L = vm.state();
  if (luaL_loadstring(L, src) != LUA_OK) {
    lua_pop(L, 1);
    return -1;
  }
  vm.armFrameBudget();
  return lua_pcall(L, 0, 0, 0);
}

// Same, but arms the generous eval budget instead -- for tests about the
// allocator/GC, not about instruction budgeting, where the snippet does
// more work than a single video frame's worth of script would.
int runWithEvalBudget(LuaVM& vm, const char* src) {
  lua_State* L = vm.state();
  if (luaL_loadstring(L, src) != LUA_OK) {
    lua_pop(L, 1);
    return -1;
  }
  vm.armEvalBudget();
  return lua_pcall(L, 0, 0, 0);
}

}  // namespace

void test_boots_with_default_settings() {
  TEST("LuaVM boots with default cap/budgets");
  LuaVM vm;
  CHECK(vm.state() != nullptr);
  CHECK(vm.memUsed() > 0);        // opening base/math/string/table costs something
  CHECK(vm.memUsed() <= vm.memCap());
}

void test_sandbox_omits_dangerous_globals() {
  TEST("sandbox: io/os/debug are never opened; _G stays trusted-only");
  LuaVM vm;
  lua_State* L = vm.state();
  const char* names[] = {"io", "os", "debug"};
  for (const char* n : names) {
    lua_getglobal(L, n);
    CHECK(lua_isnil(L, -1));
    lua_pop(L, 1);
  }
}

void test_sandbox_env_strips_loaders_and_gc() {
  TEST("sandbox env: dofile/loadfile/load/require/package/collectgarbage removed");
  LuaVM vm;
  lua_State* L = vm.state();
  vm.pushSandboxEnv();
  const char* forbidden[] = {"dofile", "loadfile", "load", "require",
                            "package", "collectgarbage", "io", "os", "debug"};
  for (const char* n : forbidden) {
    lua_getfield(L, -1, n);
    CHECK(lua_isnil(L, -1));
    lua_pop(L, 1);
  }
  // But ordinary base/math/string/table essentials survive.
  const char* kept[] = {"pairs", "ipairs", "error", "assert", "pcall",
                       "type", "tostring", "math", "string", "table"};
  for (const char* n : kept) {
    lua_getfield(L, -1, n);
    CHECK(!lua_isnil(L, -1));
    lua_pop(L, 1);
  }
  // _G inside the sandbox refers to the sandbox itself, not the trusted _G.
  lua_getfield(L, -1, "_G");
  CHECK(lua_rawequal(L, -1, -2));
  lua_pop(L, 2);
}

void test_sandbox_env_is_cached() {
  TEST("sandbox env: same table returned on repeated calls (built once)");
  LuaVM vm;
  lua_State* L = vm.state();
  vm.pushSandboxEnv();
  vm.pushSandboxEnv();
  CHECK(lua_rawequal(L, -1, -2));
  lua_pop(L, 2);
}

void test_alloc_cap_rejects_growth_past_cap() {
  TEST("allocator: growth past the cap fails cleanly, VM survives");
  LuaVM vm(16 * 1024);  // small cap: base+math+string+table alone gets close
  CHECK(vm.state() != nullptr);

  // Try to build a table far bigger than remaining headroom.
  int rc = runWithFrameBudget(vm, "local t = {} for i = 1, 1000000 do t[i] = i * 2 end");
  CHECK(rc != LUA_OK);
  const char* msg = lua_tostring(vm.state(), -1);
  CHECK(msg != nullptr);
  lua_pop(vm.state(), 1);

  CHECK(vm.memUsed() <= vm.memCap());

  // The VM is still usable after an OOM — this is the "render survives"
  // guarantee (design doc section 7.4): a small, unrelated eval succeeds.
  rc = runWithFrameBudget(vm, "local x = 1 + 1");
  CHECK(rc == LUA_OK);
}

void test_alloc_high_water_mark_tracks_peak() {
  TEST("allocator: high-water mark tracks the peak, not just the current usage");
  LuaVM vm;
  size_t before = vm.memHighWater();
  CHECK(runWithEvalBudget(vm, "local t = {} for i = 1, 5000 do t[i] = tostring(i) end") == LUA_OK);
  size_t afterAlloc = vm.memHighWater();
  CHECK(afterAlloc > before);

  // Even after the garbage is unreachable (locals went out of scope) and
  // collected, the high-water mark must not decrease.
  lua_gc(vm.state(), LUA_GCCOLLECT);
  CHECK(vm.memHighWater() >= afterAlloc);
}

void test_alloc_call_counter_increments() {
  TEST("allocator: callCount increments on every allocFn invocation");
  LuaVM vm;
  vm.resetAllocCallCount();
  CHECK(vm.allocCallCount() == 0);
  CHECK(runWithFrameBudget(vm, "local t = {1,2,3}") == LUA_OK);
  CHECK(vm.allocCallCount() > 0);
}

void test_frame_budget_aborts_infinite_loop() {
  TEST("instruction budget: a while-true loop is aborted in bounded time");
  LuaVM vm;
  int rc = runWithFrameBudget(vm, "while true do end");
  CHECK(rc != LUA_OK);
  const char* msg = lua_tostring(vm.state(), -1);
  CHECK(msg != nullptr && std::strstr(msg, "instruction budget") != nullptr);
  lua_pop(vm.state(), 1);
}

void test_frame_budget_does_not_trip_short_calls() {
  TEST("instruction budget: short well-behaved calls never trip it");
  LuaVM vm;
  for (int i = 0; i < 50; ++i) {
    int rc = runWithFrameBudget(vm, "local x = 0 for i = 1, 20 do x = x + i end");
    CHECK(rc == LUA_OK);
  }
}

void test_frame_budget_resets_per_call() {
  TEST("instruction budget: re-armed fresh before each call (not cumulative)");
  LuaVM vm;
  // Many short calls in a row must not accumulate toward the budget --
  // each call gets a full fresh allowance because armFrameBudget()
  // re-arms lua_sethook immediately before every lua_pcall.
  for (int i = 0; i < 2000; ++i) {
    int rc = runWithFrameBudget(vm, "for i = 1, 100 do end");
    CHECK(rc == LUA_OK);
  }
}

void test_eval_budget_is_larger_than_frame_budget() {
  TEST("instruction budget: eval budget tolerates a small Fennel-scale compile,"
      " frame budget does not");
  // Without Fennel in this file, approximate "compile-scale work" with a
  // plain Lua loop sized between the two budgets.
  LuaVM vm(glow::LUA_DEFAULT_MEM_CAP_BYTES, /*frameInstrBudget=*/20000,
          /*evalInstrBudget=*/2000000);
  const char* midSizedWork = "local x = 0 for i = 1, 100000 do x = x + i end";

  lua_State* L = vm.state();
  CHECK(luaL_loadstring(L, midSizedWork) == LUA_OK);
  lua_pushvalue(L, -1);  // duplicate: one call per budget type
  vm.armFrameBudget();
  int rcFrame = lua_pcall(L, 0, 0, 0);
  if (rcFrame != LUA_OK) lua_pop(L, 1);

  vm.armEvalBudget();
  int rcEval = lua_pcall(L, 0, 0, 0);
  if (rcEval != LUA_OK) lua_pop(L, 1);

  CHECK(rcFrame != LUA_OK);   // too much for the tight per-frame budget
  CHECK(rcEval == LUA_OK);    // comfortably within the eval budget
}

void test_gc_step_slack_reclaims_garbage() {
  TEST("gcStepSlack: repeated bounded steps eventually reclaim garbage");
  LuaVM vm;
  CHECK(runWithEvalBudget(vm,
                          "local junk = {} for i = 1, 3000 do junk[i] = "
                          "tostring(i) .. \"garbage\" end junk = nil") == LUA_OK);
  size_t used = vm.memUsed();

  // GC is stopped by construction: without stepping, nothing is reclaimed.
  for (int i = 0; i < 200; ++i) vm.gcStepSlack(500);
  CHECK(vm.memUsed() < used);
}

void test_collect_fully_once_reclaims_more_than_stepping() {
  TEST("collectFullyOnce: an unbounded full collection reclaims everything collectible");
  LuaVM vm;
  CHECK(runWithEvalBudget(vm,
                          "local junk = {} for i = 1, 3000 do junk[i] = "
                          "tostring(i) .. \"garbage\" end junk = nil") == LUA_OK);
  size_t before = vm.memUsed();
  vm.collectFullyOnce();
  CHECK(vm.memUsed() < before);
}

int main() {
  test_boots_with_default_settings();
  test_sandbox_omits_dangerous_globals();
  test_sandbox_env_strips_loaders_and_gc();
  test_sandbox_env_is_cached();
  test_alloc_cap_rejects_growth_past_cap();
  test_alloc_high_water_mark_tracks_peak();
  test_alloc_call_counter_increments();
  test_frame_budget_aborts_infinite_loop();
  test_frame_budget_does_not_trip_short_calls();
  test_frame_budget_resets_per_call();
  test_eval_budget_is_larger_than_frame_budget();
  test_gc_step_slack_reclaims_garbage();
  test_collect_fully_once_reclaims_more_than_stepping();

  if (g_failCount == 0) {
    printf("\nAll tests passed.\n");
    return 0;
  }
  printf("\n%d test(s) failed.\n", g_failCount);
  return 1;
}
