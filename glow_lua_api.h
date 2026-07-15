// glow_lua_api.h — the `glow.*` C API surface exposed to sandboxed Fennel
// scripts (see README_LUA_FENNEL.md, design doc section 5).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "beat_clock.h"  // glow::BeatClock
#include "lua_glow_include.h"
#include "live_control.h"  // LiveControl (glow.bind.*)
#include "show.h"  // IEffect, CapIntent, AimIntent

class ShowController;
class PixelMatrix;
class IPixelPattern;
class LuaEffect;
class LedFeedback;
class WledManager;
namespace glow {
class LuaVM;
}

// Injected lookup for glow.matrix.*. nullptr disables matrix control (the
// calls then lua_error with a clear message instead of reaching for a
// registry that doesn't exist). Device wiring supplies a registry backed by
// main.cpp's matrix driver; host tests supply a small fake.
class IMatrixRegistry {
public:
  virtual ~IMatrixRegistry() = default;
  virtual PixelMatrix* matrix(int index) = 0;  // nullptr if index is out of range
};

// Injected lookup for glow.ranges (introspection only -- never on the frame
// path, unlike glow.set/glow.slot which resolve fixtures lazily at render
// time via Show itself). nullptr disables it (glow.ranges then lua_errors
// with a clear message). Device wiring supplies a registry backed by
// main.cpp's Show; host tests supply a small fake.
class IFixtureRegistry {
public:
  virtual ~IFixtureRegistry() = default;
  virtual const FixtureProfile* profile(uint16_t fixtureId) = 0;  // nullptr if unknown
};

// Installs and implements the `glow` global table:
//   glow.set / glow.aim        — frame-context-gated, zero allocation
//   glow.CAP.*                 — integer capability constants
//   glow.cue.define/go/release — wraps ShowController, string name -> id
//   glow.scene.define/go/release
//   glow.fx.hue-rotate/chase/sweep — wraps the existing C++ effect library,
//                                    returns opaque handles usable in
//                                    cue.define's :effects list
//   glow.matrix.pattern/brightness — wraps PixelMatrix via IMatrixRegistry
//   glow.beat/bar/beat-number/bpm/locked?/tap — musical time, reads/drives
//                                    the render task's one BeatClock
//   glow.bind.pad/fader/clear  — wraps LiveControl: MIDI/OSC/web control ids
//                                 -> cue triggers. The *show* (live-editable,
//                                 no file format); see README_LIVE_CONTROL.md.
//   glow.bind.pad-xy           — like glow.bind.pad, but resolves a grid
//                                 (col, row) coordinate to a (note, channel)
//                                 via the wired .mdef's channel-significant
//                                 PAD declarations (mdef.h's resolvePadXY) --
//                                 e.g. the APC40's 8x5 clip-launch grid. A
//                                 silent no-op if there's no LED capability
//                                 (no .mdef) or the coordinate is out of
//                                 range, same graceful-degradation contract
//                                 as glow.led.*.
//   glow.led.set/auto          — wraps LedFeedback (nullable): LED feedback
//                                 for a controller with a .mdef. No-op, not
//                                 an error, when null or the addressed pad/
//                                 fader has no LED (see mdef.h/FORMAT.md).
//   glow.wled.fx/color/on/off/fx-broadcast — wraps WledManager (nullable):
//                                 named WLED UDP Notifier targets declared
//                                 by the .show file's WLED directive (see
//                                 README_WLED.md). Fire-and-forget, not
//                                 frame-context-gated (unlike glow.set/aim) —
//                                 callable from anywhere a cue effect or the
//                                 REPL can run.
//
// One instance per LuaVM (see lua_vm.h — there is exactly one VM, owned by
// the render task; see control_queue.h's rationale, which this reuses).
class GlowLuaApi {
public:
  // matrices/ledFeedback may be nullptr if this device has no pixel
  // matrices / no .mdef-described LED capability, respectively -- both
  // degrade to a clear Lua error (matrices) or a silent no-op (LED
  // feedback; see glow.led.*'s own contract). beatClock and liveControl are
  // never optional -- every device has SOME musical clock (a default-
  // constructed glow::BeatClock free-runs at a sane BPM with zero external
  // input; see beat_clock.h) and SOME control-binding table (an empty
  // LiveControl is a fully valid "nothing bound yet" state), so there is no
  // "not present on this device" case to model with a null pointer for
  // either.
  // wled backs glow.wled.*; nullptr disables it with a clear Lua error, same
  // "device has none of this" convention as matrices/fixtures above (a
  // device whose .show has no WLED directive still runs everything else).
  GlowLuaApi(glow::LuaVM& vm, ShowController& show, IMatrixRegistry* matrices,
            glow::BeatClock& beatClock, LiveControl& liveControl,
            IFixtureRegistry* fixtures = nullptr, LedFeedback* ledFeedback = nullptr,
            WledManager* wled = nullptr);
  ~GlowLuaApi();

  GlowLuaApi(const GlowLuaApi&) = delete;
  GlowLuaApi& operator=(const GlowLuaApi&) = delete;

  // Registers the `glow` table into the VM's trusted _G. Call once, before
  // LuaVM::loadFennelCompiler/pushSandboxEnv (the sandboxed env is a
  // snapshot of _G taken on first use — glow must already be present).
  void install();

  glow::LuaVM& vm() { return vm_; }
  ShowController& show() { return show_; }

  // Frame context read by glow.set/glow.aim. beginFrame/endFrame bracket a
  // LuaEffect::evaluate call (or any effect callback in general).
  // noteEvalTime is used by the eval-submission drain (glow_fennel.cpp):
  // there is no effect callback there, so glow.set/glow.aim stay invalid,
  // but cue.go/release/define (which only need "now") are fine.
  void beginFrame(float t, std::vector<CapIntent>* caps, std::vector<AimIntent>* aims);
  void endFrame();
  void noteEvalTime(float t);

  float currentTime() const { return currentT_; }

  // Look up a cue/scene id previously registered by glow.cue.define /
  // glow.scene.define. Returns false if name is unknown. Exposed for
  // wiring the web console / MIDI / OSC bindings (LiveControl/ShowController
  // already address cues/scenes by these numeric ids).
  bool cueIdForName(const std::string& name, uint16_t& idOut) const;
  bool sceneIdForName(const std::string& name, uint16_t& idOut) const;

  size_t ownedEffectCount() const { return ownedEffects_.size(); }
  size_t luaEffectCount() const { return luaEffects_.size(); }

  // Appends (effectName, lastError) for every LuaEffect that has become
  // disabled (see lua_effect.h's error policy) since the last call to this
  // method -- i.e. exactly the unsolicited fx_error notifications the WS
  // layer should push (web_protocol.h's buildFxErrorJson). Pure bookkeeping
  // over LuaEffect's own disabled()/lastError(); no hardware involved, so
  // this is host-tested like the rest of this file.
  void pollNewlyDisabledEffects(std::vector<std::pair<std::string, std::string>>& out);

private:
  static int l_set(lua_State* L);
  static int l_aim(lua_State* L);
  static int l_slot(lua_State* L);
  static int l_ranges(lua_State* L);
  static int l_cue_define(lua_State* L);
  static int l_cue_go(lua_State* L);
  static int l_cue_release(lua_State* L);
  static int l_scene_define(lua_State* L);
  static int l_scene_go(lua_State* L);
  static int l_scene_release(lua_State* L);
  static int l_fx_hue_rotate(lua_State* L);
  static int l_fx_chase(lua_State* L);
  static int l_fx_sweep(lua_State* L);
  static int l_matrix_pattern(lua_State* L);
  static int l_matrix_brightness(lua_State* L);
  static int l_beat_phase(lua_State* L);
  static int l_bar_phase(lua_State* L);
  static int l_beat_number(lua_State* L);
  static int l_bpm(lua_State* L);
  static int l_locked(lua_State* L);
  static int l_tap(lua_State* L);

  static int l_bind_pad(lua_State* L);
  static int l_bind_pad_xy(lua_State* L);
  static int l_bind_fader(lua_State* L);
  static int l_bind_clear(lua_State* L);
  static int l_led_set(lua_State* L);
  static int l_led_auto(lua_State* L);

  static int l_wled_fx(lua_State* L);
  static int l_wled_color(lua_State* L);
  static int l_wled_on(lua_State* L);
  static int l_wled_off(lua_State* L);
  static int l_wled_fx_broadcast(lua_State* L);

  static GlowLuaApi& self(lua_State* L);

  // Resolves each entry of the Lua array-table on top of the stack (either
  // a Lua function, wrapped in a freshly-owned LuaEffect, or a
  // glow.effect_handle userdata from glow.fx.*) into a raw IEffect*,
  // appending to `out`. lua_error()s (never returns) on an invalid entry.
  //
  // `cueName` names any freshly-created LuaEffect as "<cueName>#<index>" --
  // Lua function values have no reliable introspectable name of their own
  // (lua_getinfo's name fields need call-site context we don't have here),
  // so this is the identifier fx_error (web_protocol.h's buildFxErrorJson)
  // reports when this specific effect throws and gets disabled.
  void resolveEffectsList(lua_State* L, int idx, const char* cueName, std::vector<IEffect*>& out);

  // Wraps `effect` in a glow.effect_handle userdata pushed on top of the
  // stack, taking ownership (kept alive in ownedEffects_ for the VM's
  // lifetime).
  void pushEffectHandle(std::unique_ptr<IEffect> effect);

  glow::LuaVM& vm_;
  ShowController& show_;
  IMatrixRegistry* matrices_;
  glow::BeatClock& beatClock_;
  LiveControl& liveControl_;
  IFixtureRegistry* fixtures_;
  LedFeedback* ledFeedback_;
  WledManager* wled_;

  float currentT_ = 0.0f;
  std::vector<CapIntent>* frameCaps_ = nullptr;
  std::vector<AimIntent>* frameAims_ = nullptr;

  std::unordered_map<std::string, uint16_t> cueIdByName_;
  std::unordered_map<std::string, uint16_t> sceneIdByName_;

  std::vector<std::unique_ptr<IEffect>> ownedEffects_;         // glow.fx.* handles
  std::vector<std::unique_ptr<LuaEffect>> luaEffects_;         // wrapped bare Lua fns
  std::vector<bool> luaEffectReported_;                        // parallel to luaEffects_
  std::vector<std::unique_ptr<IPixelPattern>> ownedPatterns_;  // glow.matrix.pattern
};
