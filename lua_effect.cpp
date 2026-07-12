#include "lua_effect.h"

#include <cstdio>

#include "glow_lua_api.h"
#include "lua_glow_include.h"
#include "lua_vm.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
static const char* kLuaEffectTag = "lua_effect";
#endif

namespace {
void logDisabled(const std::string& name, const std::string& err) {
#ifdef ESP_PLATFORM
  ESP_LOGE(kLuaEffectTag, "effect '%s' disabled: %s", name.c_str(), err.c_str());
#else
  std::fprintf(stderr, "lua_effect: effect '%s' disabled: %s\n", name.c_str(), err.c_str());
#endif
}
}  // namespace

LuaEffect::LuaEffect(GlowLuaApi& api, int registryRef, std::string name)
    : api_(api), registryRef_(registryRef), name_(std::move(name)) {}

LuaEffect::~LuaEffect() {
  if (registryRef_ != LUA_NOREF && registryRef_ != LUA_REFNIL) {
    luaL_unref(api_.vm().state(), LUA_REGISTRYINDEX, registryRef_);
  }
}

void LuaEffect::evaluate(float t, std::vector<CapIntent>& caps, std::vector<AimIntent>& aims) {
  if (disabled_) return;

  lua_State* L = api_.vm().state();
  api_.beginFrame(t, &caps, &aims);
  api_.vm().armFrameBudget();

  lua_rawgeti(L, LUA_REGISTRYINDEX, registryRef_);
  lua_pushnumber(L, t);
  int rc = lua_pcall(L, 1, 0, 0);

  api_.endFrame();

  if (rc != LUA_OK) {
    const char* msg = lua_tostring(L, -1);
    lastError_ = msg ? msg : "(non-string error)";
    lua_pop(L, 1);
    disabled_ = true;
    logDisabled(name_, lastError_);
  }
}
