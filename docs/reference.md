# `glow.*` API reference

Every function the Fennel/Lua `glow` table exposes to a show, one entry each: what it does,
what its arguments mean *for lighting* (not just their C type), a real example, and what it
returns or emits. Grouped by what you're doing (fixtures, heads, cues, scenes, effects,
matrices, musical time, bindings, LED feedback, parameters, WLED), not alphabetically.

New to the system? Read [the author's guide](authoring.html) first — it explains *why* these
are shaped the way they are (patch vs. show, the zero-allocation rule, HTP/LTP blending) with
worked examples. This page is for looking a call up once you already know what you're doing.

**This page's coverage is enforced, not just aspirational**: CI extracts the real `glow.*`
names straight from `glow_lua_api.cpp` and fails the build if this file documents anything
different — see `docs/build/gen-reference.mjs`. If a name is missing here, it doesn't exist
yet in the real API (or someone broke CI); if a name here isn't in the engine, this page is
lying and CI already caught it.

For the text formats (`.fdef`/`.show`/`.mdef`) that declare fixtures, hang, and patch a
controller, see the [grammar reference](grammar.html) instead — this page is only the Fennel
surface.

---

## Fixtures

Set or query one fixture's capabilities. `glow.set`/`glow.slot` are only valid inside an
effect callback (the function a cue's `:effects` list runs each frame) — calling them from a
bare `eval` or `boot.fnl` top level is a Lua error, not a crash, because there is no frame to
emit into.

### `glow.set(fixtureId, capability, value)`
Sets one capability of a fixture to a continuous value. `capability` is a keyword like
`:dimmer`, `:red`, `:pan` (or the matching `glow.CAP.*` integer constant — both are
zero-allocation); `value` is normalized `0..1`. Emits into the current frame — safe, and
normal, to call every frame from an effect. An unresolvable capability *name* (a typo) is a
silent no-op, the same as a fixture that simply lacks that capability; a wrong argument
*type* is a hard Lua error.
```fennel
(glow.set 3 :dimmer 0.8)   ; fixture 3 to 80% intensity
(glow.set 3 :red 1.0)
```
Returns nothing.

### `glow.slot(fixtureId, capability, slot, value?)`
Like `glow.set`, but for a *function-range* capability — a colour wheel, a gobo wheel — whose
`.fdef` declares named positions (`SLOT`/`RANGE` lines) instead of a plain 0..1 sweep. `slot`
is that name as a string (or a numeric index); `value` (default `0`) only matters for a
continuous `RANGE` slot, where it's spread across the slot's span — it's ignored for a
discrete `SLOT`, which always snaps to the slot's centre.
```fennel
(glow.slot 1 :color-wheel "red")     ; a named wheel position
(glow.slot 1 :shutter-strobe "strobe" 0.6)  ; partway through a continuous RANGE
```
Returns nothing.

### `glow.ranges(fixtureId, capability)`
Introspects the slot/range names a fixture's `.fdef` actually declares for one function-range
capability — useful for a script that adapts to whichever colour wheel a fixture has, instead
of hardcoding slot names that only match one gel wheel. Returns an array of tables, each
`{:name <string-or-nil> :index <int> :continuous? <bool>}`; empty if the fixture or capability
is unknown, or the capability has no ranges declared.
```fennel
(glow.ranges 1 :color-wheel)
; => [{:name "open" :index 0 :continuous? false}
;     {:name "red"  :index 1 :continuous? false} ...]
```

## Moving heads

### `glow.aim(fixtureId, point)`
Points a moving head at a world-space point. `point` is a 3-element table `[x y z]` in
metres, in the *same* world frame the fixture's `POS`/`ROT` were declared in the `.show` —
this is why `POS`/`ROT` must match the physical rig: an `aim` that's correct on paper points
at the wrong spot on stage if the fixture's declared position or hang angle is wrong. Only
valid inside an effect callback; emits into the current frame.
```fennel
(glow.aim 2 [0 2 5])   ; point head 2 at a world point 2m up, 5m out
```
Returns nothing.

## Cues

A cue is a named bundle of effects with a fade envelope and a priority — the unit a show is
actually built from. `glow.cue.*` wraps the engine's `ShowController` (fades, priority,
HTP/LTP) unmodified.

### `glow.cue.define(name, opts)`
Defines (or **redefines**) a cue. `opts` is a table:
- `:effects` — a list of plain Fennel functions (each `(fn [t] ...)`) and/or `glow.fx.*`
  handles, run every frame the cue is active.
- `:fade-in`, `:fade-out`, `:hold` — seconds (default `0`; `hold <= 0` means "hold
  indefinitely once fully faded in").
- `:priority` — `0..255`; higher wins position-class ties (pan/tilt/gobo/focus/zoom) against
  other simultaneously-active cues.

Redefining an already-defined name is the normal live-coding loop — tweak, re-eval — and
points the name at a **new** cue, releasing the old one (its own fade-out still applies) if
it was active, so a live edit never leaves two versions of the same named cue running at
once.
```fennel
(glow.cue.define :warm
  {:effects [my-effect (glow.fx.hue-rotate [2 3] {:period 4.0})]
   :fade-in 2.0 :fade-out 1.0 :priority 0})
```
Returns the cue's numeric id (cues are addressed by name everywhere else; the id is rarely
needed directly).

### `glow.cue.go(name)`
Activates a cue by name: starts (or restarts) its fade-in. Error if `name` was never defined.
```fennel
(glow.cue.go :warm)
```
Returns nothing.

### `glow.cue.release(name)`
Begins a cue's fade-out from wherever its weight currently is (or removes it immediately if
`:fade-out` is `0`). Error if `name` was never defined.
```fennel
(glow.cue.release :warm)
```
Returns nothing.

## Scenes

A scene is a convenience: a named group of cues, triggered and released together — a "look"
made of several coordinated cues (a colour wash plus a moving-head sweep).

### `glow.scene.define(name, cueNames)`
Defines (or redefines) a scene from a list of cue names, each of which must already be
defined via `glow.cue.define`.
```fennel
(glow.scene.define :chorus [:verse-wash :strobe-hit])
```
Returns the scene's numeric id.

### `glow.scene.go(name)`
Activates every cue in the scene (each starts its own fade-in). Error if `name` was never
defined.
```fennel
(glow.scene.go :chorus)
```
Returns nothing.

### `glow.scene.release(name)`
Releases every cue in the scene (each begins its own fade-out).
```fennel
(glow.scene.release :chorus)
```
Returns nothing.

## Effects (`glow.fx.*`)

Fast, hand-tuned C++ effects, constructed from Fennel and dropped straight into a cue's
`:effects` list alongside plain functions — write Lua only for genuinely new behaviour, get
the built-ins for free.

### `glow.fx.hue-rotate(fixtureIds, opts?)`
Cycles hue across the given fixtures' colour capabilities over time. `opts`: `:period`
(seconds per full rotation, default `2.0`), `:sat`, `:val` (`0..1`, default `1.0` each).
```fennel
(glow.fx.hue-rotate [2 3] {:period 4.0})
```
Returns an opaque effect handle — put it directly in a cue's `:effects` list.

### `glow.fx.chase(fixtureIds, opts?)`
Chases a single point of light along the given fixture list over time (fixture order in the
list is chase order). `opts`: `:period` (seconds for one full lap, default `2.0`).
```fennel
(glow.fx.chase [1 2 3 4] {:period 1.5})
```
Returns an effect handle.

### `glow.fx.sweep(fixtureId, dirA, dirB, opts?)`
Sweeps one moving head back and forth between two world-space directions, `dirA`/`dirB`
(3-vectors, same convention as `glow.aim`'s point). `opts`: `:period` (seconds for one full
sweep, default `2.0`).
```fennel
(glow.fx.sweep 2 [-1 0 1] [1 0 1] {:period 3.0})
```
Returns an effect handle.

## Matrices

A pixel matrix (LED panel/strip wired as a grid, declared with `MATRIX` in the `.show`) runs
a *pattern* — Lua picks and parameterises it; the pixel math runs in C at pixel rate, because
per-pixel Lua callbacks aren't a budget a general-purpose scripting VM can hold at 44 fps.

### `glow.matrix.pattern(index, name, opts?)`
Selects the running pattern for one matrix. `index` is the matrix's position among the
`.show`'s `MATRIX` lines (`0` for the first). `name` is one of `:plasma`, `:rainbow`,
`:solid` — the complete, fixed set the engine implements; there is no way to define a new one
from Fennel. `opts` vary by pattern: `:speed`/`:scale` for `:plasma`; `:period`/`:cycles` for
`:rainbow`.
```fennel
(glow.matrix.pattern 0 :plasma {:speed 0.5 :scale 0.2})
```
Returns nothing. Error if there's no matrix at `index`, or `name` isn't a known pattern.

### `glow.matrix.brightness(index, value)`
Sets a matrix's master brightness multiplier, `0..1`, applied after the pattern renders.
```fennel
(glow.matrix.brightness 0 0.8)
```
Returns nothing.

## Musical time

Read-only queries against the render task's one beat clock (plus one write, `glow.tap`).
These never error and never block: a freshly-booted clock with no external sync input still
has a valid free-running phase/tempo, so an effect can always beat-sync — it just free-runs
until something locks it.

### `glow.beat()`
Phase within the current beat, `0..1` (`0` = on the beat).
```fennel
(glow.set 1 :dimmer (* 0.5 (+ 1 (math.sin (* (glow.beat) (* 2 math.pi))))))
```
Returns a number.

### `glow.bar()`
Phase within the current bar (4 beats), `0..1`.
```fennel
(glow.bar)
```
Returns a number.

### `glow.beat-number()`
Monotonically increasing beat count since the clock started, fractional (the integer part is
the beat index; the fractional part is `glow.beat()`).
```fennel
(glow.beat-number)
```
Returns a number.

### `glow.bpm()`
Current tempo estimate, beats per minute.
```fennel
(glow.bpm)
```
Returns a number.

### `glow.locked?()`
Whether the clock is currently locked to an external source (tap tempo, MIDI/OSC clock)
rather than free-running on its own last estimate.
```fennel
(glow.locked?)
```
Returns a boolean.

### `glow.tap()`
Registers one tap-tempo beat at the current time. Bind this to a pad so a performer can set
tempo by hand.
```fennel
(glow.tap)
```
Returns nothing.

## Controllers & bindings (`glow.bind.*`)

Bindings connect a physical control (a pad, fader, wheel) to a cue or continuous action.
They're a *third* thing next to the patch and the show: live-editable, no file format of
their own, resolved through the loaded `.mdef`'s hardware description but never baked into
the binary patch — see [the author's guide](authoring.html) (Controllers section) for why
that split exists. Every binding is a no-op, not an error, on a controller that lacks the control it
names — one show `.fnl` runs on any controller.

### `glow.bind.pad(note, mode, cueName)`
Binds a raw MIDI note to a cue. `mode` is `:flash` (momentary — active only while held) or
`:toggle` (latches on/off). `cueName` must already be defined.
```fennel
(glow.bind.pad 53 :flash :chorus)
```
Returns nothing.

### `glow.bind.pad-xy(col, row, mode, cueName)`
Same as `glow.bind.pad`, addressed by grid `(column, row)` instead of a raw note — resolved
through the loaded `.mdef`'s channel-significant `PAD` declarations, so the same script binds
the right physical pad on any controller with that grid shape (this is what makes a script
written for one APC40 run correctly on another). No-op if there's no controller wired at all,
or `(col, row)` is out of range for this controller's grid.
```fennel
(glow.bind.pad-xy 0 0 :flash :warm)
```
Returns nothing.

### `glow.bind.fader(cc, action, target?)`
Binds a MIDI CC fader to a continuous action. `action` is one of:
- `:master` — no target; drives the grandmaster level.
- `:cue-level` — `target` is a cue name; holds that cue's weight pinned at the fader's
  position, bypassing its normal fade envelope while held.
- `:param` — `target` is an arbitrary parameter name; read back with `glow.param.get`.
```fennel
(glow.bind.fader 7 :master)
(glow.bind.fader 8 :param :depth)
```
Returns nothing.

### `glow.bind.pitchbend(action, target?)`
Binds the pitch wheel to a continuous action — same `:master`/`:cue-level`/`:param`
vocabulary as `glow.bind.fader`. Works on any controller with a wheel; harmlessly inert on
one without.
```fennel
(glow.bind.pitchbend :param :hue)
```
Returns nothing.

### `glow.bind.pressure(action, target?)`
Binds channel pressure (aftertouch) to a continuous action — same vocabulary as
`glow.bind.fader`.
```fennel
(glow.bind.pressure :cue-level :chorus)
```
Returns nothing.

### `glow.bind.program(mode)`
Binds MIDI Program Change as a preset selector. `mode` must be `:scene` — the incoming
program number *is* the target scene id, so there's no separate target argument.
```fennel
(glow.bind.program :scene)
```
Returns nothing.

### `glow.bind.clear()`
Removes every binding (pads, faders, wheel, pressure, program selector) and every
`glow.led.auto`/`glow.led.auto-xy` tracker.
```fennel
(glow.bind.clear)
```
Returns nothing.

## LED feedback (`glow.led.*`)

Drives a controller's own LEDs so it reflects the show back at the performer — needs a
`.mdef` with `LED` ranges; every call below is a no-op, not an error, if there's no LED
capability wired, the addressed note/CC has no `LED` range in the loaded `.mdef`, or the
named colour isn't in that range's palette.

### `glow.led.set(note, color)`
Sets one LED (by raw MIDI note) to a named colour from that address's `.mdef` palette.
```fennel
(glow.led.set 53 :red)
```
Returns nothing.

### `glow.led.auto(note, cueName, activeColor, inactiveColor)`
Wires an LED to automatically track a cue's active state — register once, no further
scripting. Re-evaluated every render frame; only sends MIDI when the colour actually
changes, so a static show emits zero ongoing LED traffic.
```fennel
(glow.led.auto 53 :chorus :green :off)
```
Returns nothing.

### `glow.led.set-xy(col, row, color)`
Like `glow.led.set`, addressed by grid `(col, row)` — the same resolution
`glow.bind.pad-xy` uses, so a pad bound by `(col, row)` gets its feedback by that same
`(col, row)`, with no note number ever hand-copied between the two. This is the only form
that correctly lights a channel-multiplexed grid (e.g. the APC40's clip-launch pads, which
share note numbers across tracks) — `glow.led.set` has no channel concept and can't
distinguish them.
```fennel
(glow.led.set-xy 0 0 :red)
```
Returns nothing.

### `glow.led.auto-xy(col, row, cueName, activeColor, inactiveColor)`
`glow.led.auto`, addressed by `(col, row)` — the usual form for a grid controller.
```fennel
(glow.led.auto-xy 0 0 :warm :red :off)
```
Returns nothing.

## Parameters (`glow.param.*`)

A named slot a continuous control can drive and an effect can read — the mechanism behind
`glow.bind.*`'s `:param` action.

### `glow.param.get(name)`
Reads back whatever last drove parameter `name` via a `:param` binding (`glow.bind.fader`/
`pitchbend`/`pressure`); `0.0` if it was never bound or nothing has driven it yet.
```fennel
(fn hue-shift [t]
  (glow.set 1 :red (glow.param.get :hue)))
```
Returns a number.

## WLED (`glow.wled.*`)

Drives a named WLED strip/panel target (declared with `WLED` in the `.show`) over its own
protocol, alongside DMX/Art-Net/sACN fixtures in the same show.

### `glow.wled.fx(name, effectName, opts?)`
Runs one of WLED's built-in effects on target `name`. `opts`: `:speed`, `:intensity`,
`:brightness` (each `0..255`, default `128`/`128`/`255`), `:palette` (name, default
`"default"`), `:transition` (milliseconds, default `0`).
```fennel
(glow.wled.fx "strip1" "Rainbow" {:speed 200})
```
Returns nothing. Error if `name` isn't a declared WLED target.

### `glow.wled.color(name, r, g, b, opts?)`
Sets target `name` to a solid RGB colour (`r`/`g`/`b` each `0..255`). `opts`: `:brightness`,
`:transition` (milliseconds).
```fennel
(glow.wled.color "strip1" 255 0 0)
```
Returns nothing.

### `glow.wled.on(name)`
Turns a WLED target's power on.
```fennel
(glow.wled.on "strip1")
```
Returns nothing.

### `glow.wled.off(name)`
Turns a WLED target's power off.
```fennel
(glow.wled.off "strip1")
```
Returns nothing.

### `glow.wled.fx-broadcast(effectName, opts?)`
Like `glow.wled.fx`, but runs the effect on *every* declared WLED target at once — same
`opts` vocabulary, minus a target name.
```fennel
(glow.wled.fx-broadcast "Rainbow" {:speed 200})
```
Returns nothing.
