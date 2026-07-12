// lua_effect.h — bridges a Lua/Fennel function into IEffect (show.h).
#pragma once

#include <string>
#include <vector>

#include "show.h"  // IEffect, CapIntent, AimIntent

class GlowLuaApi;

// Wraps a registry ref to a Lua function so it can sit in a ShowController
// cue's effects list alongside plain C++ effects. Owns the ref (unrefs it
// in the destructor).
//
// evaluate() sets up the "current frame" context glow.set/glow.aim read
// (via GlowLuaApi::beginFrame/endFrame), arms the per-frame instruction
// budget, then lua_pcall's the wrapped function with a single argument: t.
// The function's return value (if any) is discarded — effects emit via
// glow.set/glow.aim, they never return intents (see the design doc's 5.1).
//
// On error, disabled_ becomes true PERMANENTLY: the effect is never
// retried on a later frame (a broken effect would otherwise fail 44x/sec,
// paying a failed lua_pcall every frame and flooding the log). The error
// is logged once and kept for lastError(). The owning cue keeps running
// its other effects; a partially-emitted frame is fine (HTP/LTP blending
// absorbs it — the effect's contribution simply vanishes from the next
// frame on).
class LuaEffect : public IEffect {
public:
  // api: the GlowLuaApi/VM this effect runs against (not owned).
  // registryRef: a LUA_REGISTRYINDEX ref to a Lua function value, already
  // taken (e.g. via luaL_ref) by the caller.
  LuaEffect(GlowLuaApi& api, int registryRef, std::string name);
  ~LuaEffect() override;

  LuaEffect(const LuaEffect&) = delete;
  LuaEffect& operator=(const LuaEffect&) = delete;

  void evaluate(float t, std::vector<CapIntent>& caps,
                std::vector<AimIntent>& aims) override;

  bool disabled() const { return disabled_; }
  const std::string& name() const { return name_; }
  const std::string& lastError() const { return lastError_; }

private:
  GlowLuaApi& api_;
  int registryRef_;
  std::string name_;
  bool disabled_ = false;
  std::string lastError_;
};
