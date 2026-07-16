<!-- GENERATED FILE -- do not hand-edit.
Produced by docs/build/gen-reference.mjs from glow_lua_api.cpp.
Run `node docs/build/gen-reference.mjs` to regenerate; CI fails the build
if this file doesn't match what the generator produces (drift guard). -->

# `glow.*` API reference

Every function `glow_lua_api.cpp`'s `GlowLuaApi::install()` registers into the Fennel/Lua `glow` global, grouped by subtable. Argument lists are a best-effort static read of each function's own `luaL_check*` calls -- when an argument's type can't be inferred cleanly (or the check calls disagree across branches), it's shown as `value` rather than guessed specifically.

38 functions across 9 groups.

The dotted names alone are also emitted as `glow-api-names.json` (a flat, generated allow-list) for any tool that wants one -- e.g. `web/provisioner-static/fennel-check.js` deliberately does NOT hand-list `glow.*` names (see that file's own header for why a hand-list drifted and rejected valid scripts); this JSON is here if a future consumer wants a generated one instead.

## Top-level (`glow.*`)

- `(glow.set fixture integer number)`
- `(glow.aim fixture point)`
- `(glow.slot fixture integer integer number?)`
- `(glow.ranges fixture integer)`
- `glow.beat` -- signature not cheaply extractable; see `glow_lua_api.cpp`'s `GlowLuaApi::l_beat_phase`.
- `glow.bar` -- signature not cheaply extractable; see `glow_lua_api.cpp`'s `GlowLuaApi::l_bar_phase`.
- `glow.beat-number` -- signature not cheaply extractable; see `glow_lua_api.cpp`'s `GlowLuaApi::l_beat_number`.
- `glow.bpm` -- signature not cheaply extractable; see `glow_lua_api.cpp`'s `GlowLuaApi::l_bpm`.
- `glow.locked?` -- signature not cheaply extractable; see `glow_lua_api.cpp`'s `GlowLuaApi::l_locked`.
- `glow.tap` -- signature not cheaply extractable; see `glow_lua_api.cpp`'s `GlowLuaApi::l_tap`.

## `glow.cue.*`

- `(glow.cue.define string table)`
- `(glow.cue.go string)`
- `(glow.cue.release string)`

## `glow.scene.*`

- `(glow.scene.define string table)`
- `(glow.scene.go string)`
- `(glow.scene.release string)`

## `glow.fx.*`

- `(glow.fx.hue-rotate fixtures[] table?)`
- `(glow.fx.chase fixtures[] table?)`
- `(glow.fx.sweep fixture point point table?)`

## `glow.matrix.*`

- `(glow.matrix.pattern integer string table?)`
- `(glow.matrix.brightness integer number)`

## `glow.bind.*`

- `(glow.bind.pad integer string string)`
- `(glow.bind.pad-xy integer integer string string)`
- `(glow.bind.fader integer)`
- `glow.bind.pitchbend` -- signature not cheaply extractable; see `glow_lua_api.cpp`'s `GlowLuaApi::l_bind_pitchbend`.
- `glow.bind.pressure` -- signature not cheaply extractable; see `glow_lua_api.cpp`'s `GlowLuaApi::l_bind_pressure`.
- `(glow.bind.program string)`
- `glow.bind.clear` -- signature not cheaply extractable; see `glow_lua_api.cpp`'s `GlowLuaApi::l_bind_clear`.

## `glow.led.*`

- `(glow.led.set integer string)`
- `(glow.led.auto integer string string string)`
- `(glow.led.set-xy integer integer string)`
- `(glow.led.auto-xy integer integer string string string)`

## `glow.param.*`

- `(glow.param.get string)`

## `glow.wled.*`

- `(glow.wled.fx string string table?)`
- `(glow.wled.color string integer integer integer table?)`
- `(glow.wled.on string)`
- `(glow.wled.off string)`
- `(glow.wled.fx-broadcast string table?)`

## `glow.CAP.*` capability constants

Also accepted as lowercase/kebab-case strings anywhere a capability is expected (`glow.set`, `glow.slot`, `glow.ranges`).

`dimmer`, `red`, `green`, `blue`, `white`, `amber`, `uv`, `cyan`, `magenta`, `yellow`, `pan`, `tilt`, `shutterstrobe`, `shutter-strobe`, `gobo`, `focus`, `zoom`, `fog`, `fan`, `colorwheel`, `color-wheel`, `gobo-rotation`, `goborotation`, `prism`, `prism-rotation`, `prismrotation`, `frost`, `iris`, `cto`, `animation-wheel`, `animationwheel`, `macro`, `generic`

## `glow.matrix.pattern` pattern names

`plasma`, `rainbow`, `solid`
