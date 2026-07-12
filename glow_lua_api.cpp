#include "glow_lua_api.h"

#include <cstring>

#include "effects.h"
#include "fixture_profile.h"
#include "lua_effect.h"
#include "lua_vm.h"
#include "pixel_matrix.h"
#include "pixel_patterns.h"
#include "show_control.h"
#include "vec_math.h"

namespace {

constexpr const char* kEffectHandleMeta = "glow.effect_handle";

struct CapName {
  const char* name;
  Capability cap;
};

// Lowercased Capability enum names (fixture_profile.h). Shared by
// capabilityFromName (glow.set) and glow.CAP's construction, so the two
// can never drift apart.
constexpr CapName kCapNames[] = {
    {"dimmer", Capability::Dimmer},
    {"red", Capability::Red},
    {"green", Capability::Green},
    {"blue", Capability::Blue},
    {"white", Capability::White},
    {"amber", Capability::Amber},
    {"uv", Capability::Uv},
    {"cyan", Capability::Cyan},
    {"magenta", Capability::Magenta},
    {"yellow", Capability::Yellow},
    {"pan", Capability::Pan},
    {"tilt", Capability::Tilt},
    {"shutterstrobe", Capability::ShutterStrobe},
    {"gobo", Capability::Gobo},
    {"focus", Capability::Focus},
    {"zoom", Capability::Zoom},
    {"fog", Capability::Fog},
    {"fan", Capability::Fan},
    {"generic", Capability::Generic},
};

bool capabilityFromName(const char* s, size_t len, Capability& out) {
  for (const auto& e : kCapNames) {
    if (std::strlen(e.name) == len && std::memcmp(e.name, s, len) == 0) {
      out = e.cap;
      return true;
    }
  }
  return false;
}

void registerFn(lua_State* L, int tblIdx, const char* name, lua_CFunction fn, void* ud) {
  tblIdx = lua_absindex(L, tblIdx);
  lua_pushlightuserdata(L, ud);
  lua_pushcclosure(L, fn, 1);
  lua_setfield(L, tblIdx, name);
}

double optNumberField(lua_State* L, int idx, const char* key, double def) {
  lua_getfield(L, idx, key);
  double v = lua_isnil(L, -1) ? def : luaL_checknumber(L, -1);
  lua_pop(L, 1);
  return v;
}

lua_Integer optIntField(lua_State* L, int idx, const char* key, lua_Integer def) {
  lua_getfield(L, idx, key);
  lua_Integer v = lua_isnil(L, -1) ? def : luaL_checkinteger(L, -1);
  lua_pop(L, 1);
  return v;
}

Vec3 checkVec3(lua_State* L, int idx) {
  luaL_checktype(L, idx, LUA_TTABLE);
  Vec3 v{};
  lua_geti(L, idx, 1);
  v.x = static_cast<float>(luaL_checknumber(L, -1));
  lua_pop(L, 1);
  lua_geti(L, idx, 2);
  v.y = static_cast<float>(luaL_checknumber(L, -1));
  lua_pop(L, 1);
  lua_geti(L, idx, 3);
  v.z = static_cast<float>(luaL_checknumber(L, -1));
  lua_pop(L, 1);
  return v;
}

uint16_t checkFixtureId(lua_State* L, int idx) {
  lua_Integer v = luaL_checkinteger(L, idx);
  if (v < 0 || v > 0xFFFF) luaL_error(L, "fixture id out of range: %d", (int)v);
  return static_cast<uint16_t>(v);
}

std::vector<uint16_t> checkFixtureIdList(lua_State* L, int idx) {
  luaL_checktype(L, idx, LUA_TTABLE);
  std::vector<uint16_t> ids;
  lua_Integer n = luaL_len(L, idx);
  ids.reserve(static_cast<size_t>(n));
  for (lua_Integer i = 1; i <= n; ++i) {
    lua_geti(L, idx, i);
    lua_Integer v = luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    if (v < 0 || v > 0xFFFF) luaL_error(L, "fixture id out of range: %d", (int)v);
    ids.push_back(static_cast<uint16_t>(v));
  }
  return ids;
}

}  // namespace

GlowLuaApi::GlowLuaApi(glow::LuaVM& vm, ShowController& show, IMatrixRegistry* matrices)
    : vm_(vm), show_(show), matrices_(matrices) {}

GlowLuaApi::~GlowLuaApi() = default;

GlowLuaApi& GlowLuaApi::self(lua_State* L) {
  void* p = lua_touserdata(L, lua_upvalueindex(1));
  return *static_cast<GlowLuaApi*>(p);
}

void GlowLuaApi::beginFrame(float t, std::vector<CapIntent>* caps, std::vector<AimIntent>* aims) {
  currentT_ = t;
  frameCaps_ = caps;
  frameAims_ = aims;
}

void GlowLuaApi::endFrame() {
  frameCaps_ = nullptr;
  frameAims_ = nullptr;
}

void GlowLuaApi::noteEvalTime(float t) { currentT_ = t; }

bool GlowLuaApi::cueIdForName(const std::string& name, uint16_t& idOut) const {
  auto it = cueIdByName_.find(name);
  if (it == cueIdByName_.end()) return false;
  idOut = it->second;
  return true;
}

bool GlowLuaApi::sceneIdForName(const std::string& name, uint16_t& idOut) const {
  auto it = sceneIdByName_.find(name);
  if (it == sceneIdByName_.end()) return false;
  idOut = it->second;
  return true;
}

void GlowLuaApi::pushEffectHandle(std::unique_ptr<IEffect> effect) {
  lua_State* L = vm_.state();
  size_t index = ownedEffects_.size();
  ownedEffects_.push_back(std::move(effect));
  auto* ud = static_cast<size_t*>(lua_newuserdatauv(L, sizeof(size_t), 0));
  *ud = index;
  luaL_getmetatable(L, kEffectHandleMeta);
  lua_setmetatable(L, -2);
}

void GlowLuaApi::resolveEffectsList(lua_State* L, int idx, const char* cueName, std::vector<IEffect*>& out) {
  idx = lua_absindex(L, idx);
  lua_Integer n = luaL_len(L, idx);
  for (lua_Integer i = 1; i <= n; ++i) {
    lua_geti(L, idx, i);
    if (lua_isfunction(L, -1)) {
      int ref = luaL_ref(L, LUA_REGISTRYINDEX);  // pops the function value
      std::string effectName = std::string(cueName) + "#" + std::to_string(i - 1);
      auto fx = std::make_unique<LuaEffect>(*this, ref, std::move(effectName));
      out.push_back(fx.get());
      luaEffects_.push_back(std::move(fx));
      luaEffectReported_.push_back(false);
      continue;
    }
    void* ud = luaL_testudata(L, -1, kEffectHandleMeta);
    if (ud != nullptr) {
      size_t index = *static_cast<size_t*>(ud);
      lua_pop(L, 1);
      if (index >= ownedEffects_.size()) {
        luaL_error(L, "effects[%d]: stale glow.fx.* handle", (int)i);
      }
      out.push_back(ownedEffects_[index].get());
      continue;
    }
    lua_pop(L, 1);
    luaL_error(L, "effects[%d] must be a function or a glow.fx.* handle", (int)i);
  }
}

// --- glow.set / glow.aim ----------------------------------------------------

int GlowLuaApi::l_set(lua_State* L) {
  GlowLuaApi& api = self(L);
  if (api.frameCaps_ == nullptr) {
    return luaL_error(L, "glow.set is only valid inside an effect callback");
  }

  uint16_t fid = checkFixtureId(L, 1);

  Capability cap = Capability::Generic;
  bool known = false;
  int capType = lua_type(L, 2);
  if (capType == LUA_TSTRING) {
    size_t clen = 0;
    const char* cname = lua_tolstring(L, 2, &clen);
    known = capabilityFromName(cname, clen, cap);
    // An unresolvable string name is a no-op, not an error: it is
    // indistinguishable from a fixture that simply lacks that capability
    // (see applyCapability in fixture_profile.h), and a typo shouldn't take
    // down the whole effect.
  } else if (capType == LUA_TNUMBER) {
    lua_Integer raw = luaL_checkinteger(L, 2);
    if (raw < 0 || raw > 255) return luaL_error(L, "glow.set: capability constant out of range");
    cap = static_cast<Capability>(raw);
    known = true;
  } else {
    return luaL_error(L, "glow.set: capability must be a string or a glow.CAP.* constant");
  }

  double v = luaL_checknumber(L, 3);

  if (known) {
    api.frameCaps_->push_back({fid, cap, static_cast<float>(v)});
  }
  return 0;
}

int GlowLuaApi::l_aim(lua_State* L) {
  GlowLuaApi& api = self(L);
  if (api.frameAims_ == nullptr) {
    return luaL_error(L, "glow.aim is only valid inside an effect callback");
  }
  uint16_t fid = checkFixtureId(L, 1);
  Vec3 point = checkVec3(L, 2);
  api.frameAims_->push_back({fid, point, true});
  return 0;
}

// --- glow.cue.* / glow.scene.* ----------------------------------------------

int GlowLuaApi::l_cue_define(lua_State* L) {
  GlowLuaApi& api = self(L);
  const char* name = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);

  std::vector<IEffect*> effects;
  lua_getfield(L, 2, "effects");
  if (!lua_isnil(L, -1)) {
    luaL_checktype(L, -1, LUA_TTABLE);
    api.resolveEffectsList(L, -1, name, effects);
  }
  lua_pop(L, 1);

  float fadeIn = static_cast<float>(optNumberField(L, 2, "fade-in", 0.0));
  float fadeOut = static_cast<float>(optNumberField(L, 2, "fade-out", 0.0));
  float hold = static_cast<float>(optNumberField(L, 2, "hold", 0.0));
  lua_Integer priority = optIntField(L, 2, "priority", 0);
  if (priority < 0 || priority > 255) return luaL_error(L, "glow.cue.define: priority out of range");

  uint16_t id = api.show_.addCue(effects, fadeIn, fadeOut, static_cast<uint8_t>(priority), hold);

  // Redefine-by-name: point the name at the new cue. If the old cue this
  // name pointed to is still active, release it (its own fade-out still
  // applies) so a live-coded redefinition doesn't leave two versions of
  // the same named cue running at once. ShowController itself is not
  // touched -- the old cue just becomes unreachable by name and ages out.
  std::string nameStr(name);
  auto it = api.cueIdByName_.find(nameStr);
  if (it != api.cueIdByName_.end()) {
    if (api.show_.isActive(it->second)) {
      api.show_.release(it->second, api.currentT_);
    }
    it->second = id;
  } else {
    api.cueIdByName_.emplace(std::move(nameStr), id);
  }

  lua_pushinteger(L, id);
  return 1;
}

int GlowLuaApi::l_cue_go(lua_State* L) {
  GlowLuaApi& api = self(L);
  const char* name = luaL_checkstring(L, 1);
  uint16_t id;
  if (!api.cueIdForName(name, id)) return luaL_error(L, "unknown cue '%s'", name);
  api.show_.go(id, api.currentT_);
  return 0;
}

int GlowLuaApi::l_cue_release(lua_State* L) {
  GlowLuaApi& api = self(L);
  const char* name = luaL_checkstring(L, 1);
  uint16_t id;
  if (!api.cueIdForName(name, id)) return luaL_error(L, "unknown cue '%s'", name);
  api.show_.release(id, api.currentT_);
  return 0;
}

int GlowLuaApi::l_scene_define(lua_State* L) {
  GlowLuaApi& api = self(L);
  const char* name = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);

  std::vector<uint16_t> cueIds;
  lua_Integer n = luaL_len(L, 2);
  for (lua_Integer i = 1; i <= n; ++i) {
    lua_geti(L, 2, i);
    const char* cueName = luaL_checkstring(L, -1);
    uint16_t id;
    bool ok = api.cueIdForName(cueName, id);
    lua_pop(L, 1);
    if (!ok) return luaL_error(L, "scene '%s' references unknown cue '%s'", name, cueName);
    cueIds.push_back(id);
  }

  uint16_t id = api.show_.addScene(cueIds);
  std::string nameStr(name);
  auto it = api.sceneIdByName_.find(nameStr);
  if (it != api.sceneIdByName_.end()) {
    it->second = id;
  } else {
    api.sceneIdByName_.emplace(std::move(nameStr), id);
  }
  lua_pushinteger(L, id);
  return 1;
}

int GlowLuaApi::l_scene_go(lua_State* L) {
  GlowLuaApi& api = self(L);
  const char* name = luaL_checkstring(L, 1);
  uint16_t id;
  if (!api.sceneIdForName(name, id)) return luaL_error(L, "unknown scene '%s'", name);
  api.show_.goScene(id, api.currentT_);
  return 0;
}

int GlowLuaApi::l_scene_release(lua_State* L) {
  GlowLuaApi& api = self(L);
  const char* name = luaL_checkstring(L, 1);
  uint16_t id;
  if (!api.sceneIdForName(name, id)) return luaL_error(L, "unknown scene '%s'", name);
  api.show_.releaseScene(id, api.currentT_);
  return 0;
}

// --- glow.fx.* ---------------------------------------------------------------

int GlowLuaApi::l_fx_hue_rotate(lua_State* L) {
  GlowLuaApi& api = self(L);
  std::vector<uint16_t> ids = checkFixtureIdList(L, 1);
  double period = 2.0, sat = 1.0, val = 1.0;
  if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
    luaL_checktype(L, 2, LUA_TTABLE);
    period = optNumberField(L, 2, "period", period);
    sat = optNumberField(L, 2, "sat", sat);
    val = optNumberField(L, 2, "val", val);
  }
  api.pushEffectHandle(std::make_unique<HueRotateEffect>(
      ids, static_cast<float>(period), static_cast<float>(sat), static_cast<float>(val)));
  return 1;
}

int GlowLuaApi::l_fx_chase(lua_State* L) {
  GlowLuaApi& api = self(L);
  std::vector<uint16_t> ids = checkFixtureIdList(L, 1);
  double period = 2.0;
  if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
    luaL_checktype(L, 2, LUA_TTABLE);
    period = optNumberField(L, 2, "period", period);
  }
  api.pushEffectHandle(std::make_unique<ChaseEffect>(ids, static_cast<float>(period)));
  return 1;
}

int GlowLuaApi::l_fx_sweep(lua_State* L) {
  GlowLuaApi& api = self(L);
  uint16_t id = checkFixtureId(L, 1);
  Vec3 dirA = checkVec3(L, 2);
  Vec3 dirB = checkVec3(L, 3);
  double period = 2.0;
  if (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) {
    luaL_checktype(L, 4, LUA_TTABLE);
    period = optNumberField(L, 4, "period", period);
  }
  api.pushEffectHandle(std::make_unique<SweepEffect>(id, dirA, dirB, static_cast<float>(period)));
  return 1;
}

// --- glow.matrix.* -----------------------------------------------------------

int GlowLuaApi::l_matrix_pattern(lua_State* L) {
  GlowLuaApi& api = self(L);
  if (api.matrices_ == nullptr) return luaL_error(L, "glow.matrix: no matrices on this device");

  lua_Integer idx = luaL_checkinteger(L, 1);
  const char* patName = luaL_checkstring(L, 2);
  PixelMatrix* m = api.matrices_->matrix(static_cast<int>(idx));
  if (m == nullptr) return luaL_error(L, "glow.matrix.pattern: no matrix at index %d", (int)idx);

  double speed = 0.5, scale = 0.2, period = 4.0, cycles = 2.0;
  if (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) {
    luaL_checktype(L, 3, LUA_TTABLE);
    speed = optNumberField(L, 3, "speed", speed);
    scale = optNumberField(L, 3, "scale", scale);
    period = optNumberField(L, 3, "period", period);
    cycles = optNumberField(L, 3, "cycles", cycles);
  }

  std::unique_ptr<IPixelPattern> pattern;
  if (std::strcmp(patName, "plasma") == 0) {
    pattern = std::make_unique<PlasmaPattern>(static_cast<float>(speed), static_cast<float>(scale));
  } else if (std::strcmp(patName, "rainbow") == 0) {
    pattern =
        std::make_unique<RainbowScrollPattern>(static_cast<float>(period), static_cast<float>(cycles));
  } else if (std::strcmp(patName, "solid") == 0) {
    pattern = std::make_unique<SolidPattern>(Rgb{1.0f, 1.0f, 1.0f});
  } else {
    return luaL_error(L, "glow.matrix.pattern: unknown pattern '%s'", patName);
  }

  IPixelPattern* raw = pattern.get();
  m->setPattern(raw);                                   // PixelMatrix borrows it
  api.ownedPatterns_.push_back(std::move(pattern));      // we keep it alive
  return 0;
}

int GlowLuaApi::l_matrix_brightness(lua_State* L) {
  GlowLuaApi& api = self(L);
  if (api.matrices_ == nullptr) return luaL_error(L, "glow.matrix: no matrices on this device");

  lua_Integer idx = luaL_checkinteger(L, 1);
  double b = luaL_checknumber(L, 2);
  PixelMatrix* m = api.matrices_->matrix(static_cast<int>(idx));
  if (m == nullptr) return luaL_error(L, "glow.matrix.brightness: no matrix at index %d", (int)idx);
  m->setMasterBrightness(static_cast<float>(b));
  return 0;
}

// --- install -----------------------------------------------------------------

void GlowLuaApi::install() {
  lua_State* L = vm_.state();

  luaL_newmetatable(L, kEffectHandleMeta);
  lua_pop(L, 1);

  lua_newtable(L);  // glow
  int glowIdx = lua_gettop(L);

  registerFn(L, glowIdx, "set", &GlowLuaApi::l_set, this);
  registerFn(L, glowIdx, "aim", &GlowLuaApi::l_aim, this);

  lua_newtable(L);  // glow.CAP
  for (const auto& e : kCapNames) {
    lua_pushinteger(L, static_cast<lua_Integer>(e.cap));
    lua_setfield(L, -2, e.name);
  }
  lua_setfield(L, glowIdx, "CAP");

  lua_newtable(L);  // glow.cue
  registerFn(L, -1, "define", &GlowLuaApi::l_cue_define, this);
  registerFn(L, -1, "go", &GlowLuaApi::l_cue_go, this);
  registerFn(L, -1, "release", &GlowLuaApi::l_cue_release, this);
  lua_setfield(L, glowIdx, "cue");

  lua_newtable(L);  // glow.scene
  registerFn(L, -1, "define", &GlowLuaApi::l_scene_define, this);
  registerFn(L, -1, "go", &GlowLuaApi::l_scene_go, this);
  registerFn(L, -1, "release", &GlowLuaApi::l_scene_release, this);
  lua_setfield(L, glowIdx, "scene");

  lua_newtable(L);  // glow.fx
  registerFn(L, -1, "hue-rotate", &GlowLuaApi::l_fx_hue_rotate, this);
  registerFn(L, -1, "chase", &GlowLuaApi::l_fx_chase, this);
  registerFn(L, -1, "sweep", &GlowLuaApi::l_fx_sweep, this);
  lua_setfield(L, glowIdx, "fx");

  lua_newtable(L);  // glow.matrix
  registerFn(L, -1, "pattern", &GlowLuaApi::l_matrix_pattern, this);
  registerFn(L, -1, "brightness", &GlowLuaApi::l_matrix_brightness, this);
  lua_setfield(L, glowIdx, "matrix");

  lua_setglobal(L, "glow");
}

void GlowLuaApi::pollNewlyDisabledEffects(std::vector<std::pair<std::string, std::string>>& out) {
  for (size_t i = 0; i < luaEffects_.size(); ++i) {
    if (luaEffects_[i]->disabled() && !luaEffectReported_[i]) {
      luaEffectReported_[i] = true;
      out.emplace_back(luaEffects_[i]->name(), luaEffects_[i]->lastError());
    }
  }
}
