#include "lua_vm.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_timer.h"
#else
#include <chrono>
#endif

namespace glow {

namespace {

uint64_t nowUs() {
#ifdef ESP_PLATFORM
  return static_cast<uint64_t>(esp_timer_get_time());
#else
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
#endif
}

void copyError(lua_State* L, char* errOut, size_t errCap) {
  if (errOut == nullptr || errCap == 0) return;
  const char* msg = lua_tostring(L, -1);
  std::snprintf(errOut, errCap, "%s", msg ? msg : "(non-string error)");
}

// Bootstrap-only require()/package.preload shim. The vendored fennel.lua
// (built with `--require-as-include`, see scripts/vendor_fennel.sh) is a
// chain of package.preload[...] assignments stitched together with
// require() calls — it needs a real require() to execute at all, even
// though it never touches the filesystem (every module it requires is
// already registered in package.preload by an earlier assignment in the
// same file). This shim is installed into the VM's trusted _G only; it is
// never present in the sandboxed script environment built by
// pushSandboxEnv() (see the removal list there). It has no filesystem or
// dynamic-library access, unlike Lua's real `package` library.
constexpr const char* kRequireShim =
    "package = package or {}\n"
    "package.preload = package.preload or {}\n"
    "package.loaded = package.loaded or {}\n"
    "function require(name)\n"
    "  if package.loaded[name] ~= nil then return package.loaded[name] end\n"
    "  local loader = package.preload[name]\n"
    "  if not loader then error(\"module '\" .. name .. \"' not found\", 2) end\n"
    "  local result = loader(name)\n"
    "  if result == nil then result = true end\n"
    "  package.loaded[name] = result\n"
    "  return result\n"
    "end\n";

// Builds the restricted script environment as a shallow copy of _G (at
// this point: base + math + string + table + the require shim above, plus
// whatever GlowLuaApi::install has added as _G.glow) with everything a
// script must not touch removed. `load` is stripped too, not just
// dofile/loadfile: it can run arbitrary — including precompiled, unverified
// — bytecode, which is strictly more dangerous than either.
constexpr const char* kSandboxBuilder =
    "local env = {}\n"
    "for k, v in pairs(_G) do env[k] = v end\n"
    "env.dofile = nil\n"
    "env.loadfile = nil\n"
    "env.load = nil\n"
    "env.require = nil\n"
    "env.package = nil\n"
    "env.collectgarbage = nil\n"
    "env._G = env\n"
    "return env\n";

}  // namespace

void* LuaVM::allocFn(void* ud, void* ptr, size_t osize, size_t nsize) {
  auto* st = static_cast<AllocState*>(ud);
  st->callCount++;

  // Per the Lua manual (lua_Alloc): when ptr is NULL, osize carries an
  // object-kind tag (LUA_TSTRING etc.), NOT the previous block size, which
  // is always 0 for a brand-new allocation. Treating it as a size here
  // would corrupt the running byte count.
  size_t oldSize = (ptr != nullptr) ? osize : 0;

  if (nsize == 0) {
    if (ptr != nullptr) {
#ifdef ESP_PLATFORM
      heap_caps_free(ptr);
#else
      free(ptr);
#endif
    }
    st->used -= oldSize;
    return nullptr;
  }

  if (nsize > oldSize) {
    size_t grow = nsize - oldSize;
    if (st->capBytes > 0 && st->used + grow > st->capBytes) {
      return nullptr;  // Lua treats this as LUA_ERRMEM, caught by lua_pcall.
    }
  }

#ifdef ESP_PLATFORM
  void* np = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM);
#else
  void* np = realloc(ptr, nsize);
#endif
  if (np == nullptr) return nullptr;

  st->used = st->used - oldSize + nsize;
  if (st->used > st->highWater) st->highWater = st->used;
  return np;
}

void LuaVM::countHook(lua_State* L, lua_Debug* /*ar*/) {
  luaL_error(L, "instruction budget exceeded (script aborted)");
}

LuaVM::LuaVM(size_t capBytes, int frameInstrBudget, int evalInstrBudget)
    : frameInstrBudget_(frameInstrBudget),
      evalInstrBudget_(evalInstrBudget),
      fennelModuleRef_(LUA_NOREF),
      sandboxEnvRef_(LUA_NOREF) {
  alloc_.capBytes = capBytes;
  L_ = lua_newstate(&LuaVM::allocFn, &alloc_);
  if (L_ == nullptr) return;  // capBytes too small to even boot; state()==nullptr signals this

  luaL_requiref(L_, LUA_GNAME, luaopen_base, 1);
  lua_pop(L_, 1);
  luaL_requiref(L_, LUA_MATHLIBNAME, luaopen_math, 1);
  lua_pop(L_, 1);
  luaL_requiref(L_, LUA_STRLIBNAME, luaopen_string, 1);
  lua_pop(L_, 1);
  luaL_requiref(L_, LUA_TABLIBNAME, luaopen_table, 1);
  lua_pop(L_, 1);

  // Bootstrap-only; see kRequireShim's comment. Trusted code, not expected
  // to fail — if it somehow does, loadFennelCompiler will fail loudly next.
  luaL_dostring(L_, kRequireShim);

  lua_gc(L_, LUA_GCGEN, 0, 0);
  lua_gc(L_, LUA_GCSTOP, 0);
}

LuaVM::~LuaVM() {
  if (L_ != nullptr) lua_close(L_);
}

bool LuaVM::loadFennelCompiler(const char* src, size_t len, char* errOut, size_t errCap) {
  if (luaL_loadbuffer(L_, src, len, "=fennel.lua") != LUA_OK) {
    copyError(L_, errOut, errCap);
    lua_pop(L_, 1);
    return false;
  }
  if (lua_pcall(L_, 0, 1, 0) != LUA_OK) {
    copyError(L_, errOut, errCap);
    lua_pop(L_, 1);
    return false;
  }
  fennelModuleRef_ = luaL_ref(L_, LUA_REGISTRYINDEX);  // pops the module table
  return true;
}

void LuaVM::pushFennelModule() {
  lua_rawgeti(L_, LUA_REGISTRYINDEX, fennelModuleRef_);
}

void LuaVM::pushSandboxEnv() {
  if (sandboxEnvRef_ == LUA_NOREF) {
    luaL_dostring(L_, kSandboxBuilder);  // leaves the env table on the stack
    sandboxEnvRef_ = luaL_ref(L_, LUA_REGISTRYINDEX);
  }
  lua_rawgeti(L_, LUA_REGISTRYINDEX, sandboxEnvRef_);
}

void LuaVM::armFrameBudget() {
  lua_sethook(L_, &LuaVM::countHook, LUA_MASKCOUNT, frameInstrBudget_);
}

void LuaVM::armEvalBudget() {
  lua_sethook(L_, &LuaVM::countHook, LUA_MASKCOUNT, evalInstrBudget_);
}

void LuaVM::collectFullyOnce() { lua_gc(L_, LUA_GCCOLLECT); }

void LuaVM::gcStepSlack(uint32_t budgetUs) {
  // A generous but finite cap on iterations: protects against a clock that
  // never advances (some host test doubles) turning this into a hang,
  // without meaningfully limiting real GC work under a real clock.
  constexpr int kMaxIterations = 100000;
  uint64_t start = nowUs();
  int iterations = 0;
  while (iterations < kMaxIterations) {
    lua_gc(L_, LUA_GCSTEP, 0);
    ++iterations;
    if (nowUs() - start >= budgetUs) break;
  }
}

}  // namespace glow
