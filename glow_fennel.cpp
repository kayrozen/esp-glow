#include "glow_fennel.h"

#include <cstdio>
#include <memory>

#include "glow_lua_api.h"
#include "lua_glow_include.h"
#include "lua_vm.h"

namespace glow {
namespace {

std::unique_ptr<LuaVM> g_vm;
std::unique_ptr<GlowLuaApi> g_api;
bool g_ready = false;

void copyErr(const char* msg, char* errOut, size_t errCap) {
  if (errOut == nullptr || errCap == 0) return;
  std::snprintf(errOut, errCap, "%s", msg ? msg : "(non-string error)");
}

}  // namespace

bool glowLuaInit(ShowController& show, IMatrixRegistry* matrices, BeatClock& beatClock,
                 const char* fennelSrc, size_t fennelSrcLen,
                 char* errOut, size_t errCap,
                 size_t capBytes, int frameInstrBudget, int evalInstrBudget) {
  if (g_ready) return true;  // already initialized; not re-entrant per design (one VM)

  size_t cap = capBytes != 0 ? capBytes : LUA_DEFAULT_MEM_CAP_BYTES;
  int frameBudget = frameInstrBudget != 0 ? frameInstrBudget : LUA_DEFAULT_FRAME_INSTR_BUDGET;
  int evalBudget = evalInstrBudget != 0 ? evalInstrBudget : LUA_DEFAULT_EVAL_INSTR_BUDGET;

  g_vm = std::make_unique<LuaVM>(cap, frameBudget, evalBudget);
  if (g_vm->state() == nullptr) {
    copyErr("lua_newstate failed (capBytes too small?)", errOut, errCap);
    g_vm.reset();
    return false;
  }

  g_api = std::make_unique<GlowLuaApi>(*g_vm, show, matrices, beatClock);
  g_api->install();

  char loadErr[256];
  if (!g_vm->loadFennelCompiler(fennelSrc, fennelSrcLen, loadErr, sizeof(loadErr))) {
    copyErr(loadErr, errOut, errCap);
    g_api.reset();
    g_vm.reset();
    return false;
  }

  g_vm->collectFullyOnce();
  g_ready = true;
  return true;
}

void glowLuaShutdown() {
  g_ready = false;
  g_api.reset();
  g_vm.reset();
}

bool glowLuaReady() { return g_ready; }

LuaVM& glowLuaVM() { return *g_vm; }

GlowLuaApi& glowLuaApi() { return *g_api; }

int pumpEvalSubmissions(IEvalSubmissionQueue& q, int maxPerFrame, EvalResultFn onResult, void* ctx) {
  int dispatched = 0;
  EvalSubmission sub;
  char err[256];
  while (dispatched < maxPerFrame && q.pop(sub)) {
    err[0] = '\0';
    bool ok = glow_lua_eval_fennel(sub.source, sub.len, err, sizeof(err));
    if (onResult != nullptr) onResult(ctx, sub.requestId, ok, ok ? nullptr : err);
    ++dispatched;
  }
  return dispatched;
}

}  // namespace glow

bool glow_lua_eval_fennel(const char* src, size_t len, char* errOut, size_t errCap) {
  if (!glow::glowLuaReady()) {
    if (errOut != nullptr && errCap != 0) {
      std::snprintf(errOut, errCap, "glow_lua_eval_fennel: VM not initialized");
    }
    return false;
  }

  glow::LuaVM& vm = glow::glowLuaVM();
  lua_State* L = vm.state();

  vm.pushFennelModule();
  lua_getfield(L, -1, "eval");
  lua_remove(L, -2);
  lua_pushlstring(L, src, len);
  lua_newtable(L);
  vm.pushSandboxEnv();
  lua_setfield(L, -2, "env");
  // Errors go into a JSON field for a web console, not a terminal: turn
  // off Fennel's ANSI "pinpoint" highlighting (reverse-video escape codes
  // around the offending token), which is otherwise on by default.
  lua_pushboolean(L, 0);
  lua_setfield(L, -2, "error-pinpoint");

  vm.armEvalBudget();
  int rc = lua_pcall(L, 2, 0, 0);
  if (rc != LUA_OK) {
    const char* msg = lua_tostring(L, -1);
    if (errOut != nullptr && errCap != 0) {
      std::snprintf(errOut, errCap, "%s", msg ? msg : "(non-string error)");
    }
    lua_pop(L, 1);
    return false;
  }
  return true;
}
