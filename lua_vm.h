// lua_vm.h — lifecycle of a single sandboxed Lua VM running the vendored
// Fennel compiler (see README_LUA_FENNEL.md).
//
// One VM per process (see control_queue.h's rationale for "one VM, owned by
// the render task" — the same single-owner discipline applies here). This
// class does not enforce that by itself; glow_fennel.cpp's singleton does.
#pragma once

#include <cstddef>
#include <cstdint>

#include "lua_glow_include.h"

namespace glow {

// Fennel's compiler is a large table graph: loading it alone (measured
// against the vendored fennel.lua, GC stopped as it runs at boot) already
// uses ~775 KB. A cap anywhere near the bottom of the doc's suggested
// 512 KB - 1 MB range would leave scripts almost no headroom, so this
// picks the top of that range.
constexpr size_t LUA_DEFAULT_MEM_CAP_BYTES = 1024 * 1024;

// Two different budgets, because "run an already-compiled effect function"
// and "compile a Fennel snippet, then run it" are very different workloads:
//   - Invoking an already-compiled effect closure once per frame is meant
//     to be a couple dozen VM instructions; 20000 gives a `while true do
//     end` nowhere to hide while never troubling a well-written effect.
//   - Compiling even a two-line Fennel form through the (interpreted, not
//     JITed) Fennel compiler costs ~30000 VM instructions by itself, before
//     the compiled code has run a single instruction of its own — measured
//     directly against the vendored fennel.lua. A generous ceiling here
//     still bounds a pathological compile (or a top-level infinite loop
//     submitted directly at the REPL) without false-positiving on normal
//     live-coding-sized snippets.
constexpr int LUA_DEFAULT_FRAME_INSTR_BUDGET = 20000;
constexpr int LUA_DEFAULT_EVAL_INSTR_BUDGET = 2'000'000;

// Wraps one lua_State with:
//  - a byte-capped allocator (PSRAM-backed on device, malloc-backed on
//    host) that returns NULL past the cap instead of growing unbounded,
//    plus a high-water mark for telemetry;
//  - only base/math/string/table opened, so scripts never see io/os/debug;
//  - a tiny require()/package.preload shim installed into the trusted _G
//    (needed only to bootstrap the vendored fennel.lua, which is itself
//    built from many internal require()'d submodules — see
//    scripts/vendor_fennel.sh). This shim is NEVER copied into the
//    sandboxed script environment (see pushSandboxEnv);
//  - a per-call instruction-count budget (LUA_MASKCOUNT hook) that aborts
//    a runaway script in bounded time;
//  - a generational GC held stopped except for explicit bounded steps
//    (gcStepSlack), so a script never causes an uncontrolled GC pause on
//    the render path.
class LuaVM {
public:
  explicit LuaVM(size_t capBytes = LUA_DEFAULT_MEM_CAP_BYTES,
                 int frameInstrBudget = LUA_DEFAULT_FRAME_INSTR_BUDGET,
                 int evalInstrBudget = LUA_DEFAULT_EVAL_INSTR_BUDGET);
  ~LuaVM();

  LuaVM(const LuaVM&) = delete;
  LuaVM& operator=(const LuaVM&) = delete;

  // nullptr if lua_newstate itself failed (e.g. capBytes too small to boot
  // a VM at all). Callers must check before doing anything else.
  lua_State* state() const { return L_; }

  // Compile + execute the vendored fennel.lua source into this VM's
  // trusted _G, capturing its returned module table for pushFennelModule().
  // Call once, after GlowLuaApi::install() has set _G.glow (see below) and
  // before the first pushSandboxEnv()/eval. Returns false (fills errOut,
  // capacity errCap) on failure; never throws.
  bool loadFennelCompiler(const char* src, size_t len, char* errOut, size_t errCap);

  // Push the fennel module table (the value loadFennelCompiler captured)
  // onto the stack. Only valid after loadFennelCompiler() has succeeded.
  void pushFennelModule();

  // Push the restricted global-environment table used for every script
  // eval / effect callback. Built lazily on first call as a shallow copy
  // of _G with dofile/loadfile/load/require/package/collectgarbage removed
  // and `_G` self-referencing the copy; cached after that. Because it is a
  // snapshot, `_G.glow` (see GlowLuaApi::install) must already be set the
  // first time this is called.
  void pushSandboxEnv();

  // Re-arm the per-call instruction budget immediately before a lua_pcall
  // that runs script/effect code. Use armFrameBudget() around invoking an
  // already-compiled effect function (LuaEffect::evaluate); use
  // armEvalBudget() around compiling+running a Fennel source string
  // (glow_lua_eval_fennel). Trusted-only calls (e.g. loading the Fennel
  // compiler itself) don't need either.
  void armFrameBudget();
  void armEvalBudget();

  // Run bounded LUA_GCSTEP work for up to budgetUs microseconds of wall
  // time. The GC is generational and created stopped (see ctor); this is
  // the only place it ever runs on the render path. Call from the render
  // task's frame slack, never from the frame path itself.
  void gcStepSlack(uint32_t budgetUs);

  // Force one full, unbounded collection. Loading the Fennel compiler
  // alone leaves ~200 KB of parser/compiler garbage behind (measured:
  // ~910 KB used right after loadFennelCompiler, ~680 KB after this call)
  // because the GC is stopped while it runs. Call this exactly once during
  // startup, after loadFennelCompiler and before the render loop begins —
  // never during real-time operation, since a full collection is
  // unbounded and would drop frames.
  void collectFullyOnce();

  size_t memUsed() const { return alloc_.used; }
  size_t memHighWater() const { return alloc_.highWater; }
  size_t memCap() const { return alloc_.capBytes; }

  // Diagnostics for the zero-allocation host tests (see test_lua_vm.cpp):
  // every allocFn invocation (malloc/realloc/free) increments this counter,
  // regardless of whether it grew, shrank, or freed.
  uint32_t allocCallCount() const { return alloc_.callCount; }
  void resetAllocCallCount() { alloc_.callCount = 0; }

private:
  struct AllocState {
    size_t capBytes;
    size_t used = 0;
    size_t highWater = 0;
    uint32_t callCount = 0;
  };

  static void* allocFn(void* ud, void* ptr, size_t osize, size_t nsize);
  static void countHook(lua_State* L, lua_Debug* ar);

  AllocState alloc_;
  lua_State* L_ = nullptr;
  int frameInstrBudget_;
  int evalInstrBudget_;
  int fennelModuleRef_;
  int sandboxEnvRef_;
};

}  // namespace glow
