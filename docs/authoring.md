# Writing a show

A concept-first guide to writing an esp-glow show in Fennel — the mental model first, then
every major piece with a real, runnable example. If you want to look up one `glow.*` call's
exact arguments, see [the API reference](reference.html) instead; this page is for learning
the system, not looking things up.

The worked examples below assume a small patch like this one (trimmed from
`samples/demo.show`):

```
SHOW 2
UNIVERSE 1 DMX

FIXTURE samples/dimmer.fdef 1 2      # fixture 0: a 4-channel RGB dimmer
FIXTURE samples/dimmer.fdef 1 12     # fixture 1: a second one

FIXTURE samples/head.fdef 1 22       # fixture 2: a 9-channel moving head
POS 2.0 1.0 0.0
ROT 0 0 0

MATRIX 2 1 10 16 SERP H RGB          # matrix 0: a 10x16 pixel panel

CONTROLLER samples/apc40.mdef        # an Akai APC40 mkII for live control
```

Fixtures are numbered in patch order, starting at `0` — `FIXTURE samples/dimmer.fdef 1 2` is
fixture `0`, the next `FIXTURE` line is fixture `1`, and so on. Matrices are numbered the same
way, separately, starting from their own `0`.

## The mental model

Two things exist, and they're deliberately kept apart:

- **The patch** — which fixtures exist, what DMX channels they occupy, where they physically
  are, which controller is wired — is the `.fdef`/`.show`/`.mdef` text above, compiled by the
  browser provisioner into a binary bundle and flashed to the device. It changes rarely, and a
  live-coded script can't redefine it: a script that could resize a fixture's channel count
  mid-show is a script that can corrupt the render loop's fixed-size buffers. See
  [the grammar reference](grammar.html) for the full text-format grammar.
- **The show** — cues, scenes, effects, controller bindings — is Fennel, written and
  hot-swapped while the rig is running. This page is about writing that half.

An **effect** is just a function of time that emits capability values:

```fennel
(fn my-effect [t]
  (glow.set 0 :dimmer 0.8))
```

`t` is the current time in seconds; the function's job is to call `glow.set`/`glow.aim` (or
similar) for whatever it wants to happen *this frame*. It does not return anything — there is
no "resolved intents" table handed back, because building and returning one would allocate a
table every frame (see "The zero-allocation rule" at the end of this page). An effect just
emits, straight into the current frame, as a side effect of being
called.

Underneath, the split is: **C++ runs the tight loops** (the DMX/Art-Net render loop at 44 Hz,
the pixel-matrix engine, aim geometry) and **Lua composes and parameterises** — it never sits
in a per-pixel loop, never blocks the render task, and is budgeted and sandboxed so a broken
script can't take the rig down. That split is *why* the API looks the way it does throughout
this guide: `glow.matrix.pattern` picks a pattern instead of taking a per-pixel callback;
`glow.aim` takes a point instead of raw pan/tilt DMX values; effects emit instead of
returning.

**The show goes on.** If one effect throws a Lua error, it's disabled — logged once, never
retried — while the rest of its cue, and every other cue, keeps running. A live-coding session
where you're actively breaking things while iterating doesn't take down the lights; it just
mutes the one effect you're mid-edit on.

## Your first cue

The smallest complete, useful unit: define an effect, wrap it in a cue, activate the cue.

```fennel
(fn breathe [t]
  (glow.set 0 :dimmer (* 0.5 (+ 1 (math.sin (* t 2))))))

(glow.cue.define :warm {:effects [breathe] :fade-in 2.0})
(glow.cue.go :warm)
```

Line by line:

1. `breathe` is an effect: a function of `t` that sets fixture `0`'s `:dimmer` capability to a
   value oscillating between `0` and `1` (`math.sin` swings -1..1; `(* 0.5 (+ 1 ...))` rescales
   that to 0..1). It does this every frame it's asked to run — there's no "start" or "loop";
   the whole shape of the breathe comes from evaluating the same formula against a
   continuously advancing `t`.
2. `(glow.cue.define :warm {...})` bundles that effect into a cue named `:warm`, with a 2-second
   fade-in when the cue activates. `:effects` is a list — you can put several effects in one
   cue, and each function in the list runs every frame the cue is active.
3. `(glow.cue.go :warm)` activates it. The fade-in starts counting from *now*.

Send this over the WebSocket REPL (or put it in `boot.fnl`) and the fixture starts breathing,
ramping up over its first 2 seconds. Re-evaluating the `glow.cue.define` form with a tweaked
`breathe` — different period, different capability — is the whole live-coding loop: it points
`:warm` at a new cue and fades out whatever was running before, so you never get two
overlapping copies of the same named cue.

## Fixtures & capabilities

`glow.set` is how almost everything actually happens: it sets one **capability** of one
fixture to a normalized value.

```fennel
(glow.set 0 :dimmer 0.8)   ; fixture 0 to 80% intensity
(glow.set 0 :red 1.0)
(glow.set 0 :green 0.2)
(glow.set 0 :blue 0.2)
```

A capability is a keyword naming one thing a fixture's `.fdef` declares it can do —
`:dimmer`, `:red`, `:green`, `:blue`, `:pan`, `:tilt`, `:colorwheel`, and so on (the full list
is in [the grammar reference](grammar.html)'s `CAP` entry). `value` is always normalized
`0..1`, regardless of whether the fixture's real DMX channel is 8-bit or a 16-bit coarse/fine
pair — the `.fdef` encoding is the engine's problem, not the script's. Calling `glow.set` with
a capability the patched fixture doesn't have is a silent no-op, not an error — the same as a
typo in the capability name — so a script written against a richer fixture degrades quietly
on a simpler one instead of crashing the show.

Some capabilities aren't a smooth `0..1` sweep — a colour wheel or gobo wheel has fixed
positions, declared as named `SLOT`s (or continuous `RANGE`s) in the `.fdef`. Address those
with `glow.slot` instead of `glow.set`:

```fennel
(glow.slot 2 :colorwheel "red")     ; snap the wheel to its named "red" slot
```

`glow.slot`'s third argument is the slot's name (a string, matching the `.fdef`'s `SLOT`
line) — not a `0..1` value, because a slot is a discrete position, not a continuous range. If
you don't know a fixture's wheel names offhand, `(glow.ranges fixtureId :colorwheel)` reads
them straight back from the loaded patch.

`glow.set` and `glow.slot` are both only valid **inside an effect callback** — calling either
from a bare `eval` or from `boot.fnl`'s top level is a Lua error, because there's no frame for
the call to emit into. Everything in this guide that calls `glow.set`/`glow.aim` is written
inside a `(fn [t] ...)` for exactly that reason.

## Moving heads

A `HEAD` fixture (declared with the `HEAD` keyword in its `.fdef`, like `samples/head.fdef`)
can be pointed at a world-space point instead of driven by raw pan/tilt values:

```fennel
(fn spot-on-singer [t]
  (glow.aim 2 [0 2 1.5]))   ; fixture 2, a point 2m up and 1.5m out from the origin
```

`glow.aim`'s second argument is a 3-element table `[x y z]`, in metres, in the **same world
frame** the fixture's `POS`/`ROT` were declared in back in the `.show` (see the patch at the
top of this page: fixture `2` was patched with `POS 2.0 1.0 0.0` / `ROT 0 0 0`). This is the
entire reason `POS`/`ROT` exist and must match the rig: `glow.aim` computes the pan/tilt angle
needed to reach a point using the fixture's declared position and mounting orientation. If the
`.show` says the head is at `(2, 1, 0)` but it's actually hung two metres to the left, every
`glow.aim` call in every script is now aiming at the wrong physical spot — a script bug you
can't fix by editing Fennel, because the bug is in the patch, not the show.

`glow.aim`, like `glow.set`, is only valid inside an effect callback.

## Cues, scenes, blending

A cue is a bundle of effects with a fade envelope and a priority — you've already seen the
basic form. The envelope fields:

```fennel
(glow.cue.define :chorus
  {:effects [my-effect]
   :fade-in 2.0    ; seconds to ramp 0 -> full weight after go
   :hold 8.0        ; seconds at full weight before auto fade-out (0/omitted = hold forever)
   :fade-out 1.0    ; seconds to ramp full weight -> 0, either automatically or on release
   :priority 0})     ; 0..255, breaks ties between simultaneously-active cues
```

`(glow.cue.go :chorus)` starts the fade-in; `(glow.cue.release :chorus)` starts the fade-out
early, from wherever the cue's current weight is (releasing a cue that's only 40% faded in
fades out from 40%, not from 100%). `glow.bind.pad`'s `:flash` mode calls exactly these two
functions on press/release, which is how a momentary cue works.

When two cues target the *same* fixture capability at the *same* time, they blend, and the
blend rule depends on what kind of capability it is:

- **Intensity-class** (dimmer, colour channels, shutter, fog, fan): each cue's value is
  scaled by its own current fade weight, then the **highest** scaled value across all active
  cues wins (HTP — Highest Takes Priority). This is why overlapping colour washes look like
  colours *adding*, and why a cue fading out doesn't visibly cut lights the moment a second
  cue takes over — the max just gradually favours the growing one.
- **Position-class** (pan, tilt, gobo, focus, zoom, and `glow.aim` targets): **not** scaled by
  weight at all. The **highest-priority** active cue wins outright (LTP — Latest/Last Takes
  Priority by priority, ties broken by which cue went most recently). This is deliberate: a
  moving head's aim shouldn't blend halfway between two targets while a colour cue crossfades
  underneath it — you want a clean handoff of position, independent of whatever's fading.

That split is why `:priority` exists as a separate knob from fade timing: a low-priority
"ambient wash" cue can keep a fixture's dimmer gently breathing forever while a high-priority
"hit" cue snaps its position around on top, without either fighting the other for control of
the wrong kind of capability.

A **scene** is a named group of cue names, triggered/released together:

```fennel
(glow.scene.define :verse [:warm :chorus])
(glow.scene.go :verse)       ; go's every cue in the list
(glow.scene.release :verse)  ; release's every cue in the list
```

Use a scene when a "look" is really several cues moving together — a colour wash plus a
moving-head position plus a matrix pattern — so a performer triggers one thing, not three.

## Matrices

A pixel matrix (an LED panel or strip wired as a grid, declared with `MATRIX` in the `.show`)
doesn't run per-pixel Lua — there's no budget for a scripting VM to touch every pixel at
frame rate. Instead, Lua **picks and parameterises** a pattern that runs in C:

```fennel
(glow.matrix.pattern 0 :plasma {:speed 0.5 :scale 0.2})
(glow.matrix.brightness 0 0.8)
```

The pattern name is one of a fixed, small set the engine implements — `:plasma`, `:rainbow`,
`:solid` — not something you can define from Fennel. What you *can* do from Fennel is decide
**which** pattern runs and **when**, the same way any other cue would: call
`glow.matrix.pattern` from inside an effect to switch patterns on a cue change, a beat, or a
button press. The pixel math itself — the sine waves, the hue scroll, packing into DMX
universes — is C, running every frame regardless of what Lua is doing, which is what makes a
340-pixel matrix at 44 fps possible at all.

## Musical time

`glow.beat`/`glow.bar`/`glow.bpm` read the render task's one beat clock — always, without
blocking or erroring, even if nothing has ever set a tempo:

```fennel
(fn pulse [t]
  (glow.set 0 :dimmer (- 1 (glow.beat))))   ; snap to 1 on the beat, fade out toward the next
```

`(glow.beat)` returns `0..1`, the phase within the current beat (`0` = exactly on the beat).
`(glow.bar)` is the same idea one level up (phase within a 4-beat bar). If nothing has locked
the clock to an external source — no tap tempo, no MIDI/OSC clock — it **free-runs** on its
last known (or default) tempo rather than refusing to answer; `(glow.locked?)` tells you
whether the current phase/tempo is actually locked to something, if an effect wants to behave
differently in that case (e.g. dim down instead of pulsing confidently to an unlocked guess).
`(glow.tap)` registers one tap-tempo beat — bind it to a pad (see below) so a performer can
set the tempo by hand.

Beat-synced effects are exactly this shape: read `(glow.beat)`/`(glow.bar)`/`(glow.beat-number)`
inside the effect body every frame, same as reading `t` — there's no separate "on beat"
callback to register.

## Controllers

A `.mdef` describes a controller's **hardware** — which notes/CCs exist, whether they're
grouped into a channel-multiplexed grid, which ones have LED feedback. It does not describe
*bindings* — what a pad or fader actually does is Fennel, live-editable independently of the
patch, using `glow.bind.*`/`glow.led.*`.

```fennel
(glow.cue.define :warm {:effects []})
(glow.bind.pad-xy 0 0 :flash :warm)       ; grid pad (col=0, row=0) -> momentary :warm
(glow.led.auto-xy 0 0 :warm :red :off)    ; that pad's own LED tracks :warm's active state
```

(This is drawn straight from `samples/demo-boot.fnl`, a small pad-addressing show verified to
compile against `samples/apc40.mdef`.)

`glow.bind.pad-xy` addresses a pad by grid `(column, row)` instead of a raw MIDI note, and
resolves it through the loaded `.mdef`'s hardware description — so the *same* script, unedited,
binds the correct physical pad on any controller that has an 8×5 (or whatever) clip-launch
grid, not just the exact APC40 unit it was written against. `glow.led.auto-xy` closes the loop:
once registered, that pad's LED tracks `:warm`'s active state on its own, every frame, with no
further scripting — green (or whatever colour you pick) while the cue is running, the
"inactive" colour while it isn't.

Both `pad-xy` forms are **no-ops**, not errors, on a controller that lacks that grid entirely,
or lacks that specific coordinate (a smaller grid) — the same graceful-degradation contract
`glow.set` has for a missing capability. That's what makes one `boot.fnl` portable across
different controllers: bind everything you want, and whatever hardware doesn't exist just
silently does nothing.

Continuous controls (a fader, a pitch wheel, channel pressure) all bind through the same three
targets, regardless of which physical control drives them — a script that reads "wheel drives
hue" doesn't care whether the eventual controller even *has* a wheel:

```fennel
(glow.bind.fader 7 :master)              ; CC 7 -> grandmaster level
(glow.bind.pitchbend :param :hue)        ; the pitch wheel -> a named parameter
(glow.bind.pressure :cue-level :chorus)  ; channel pressure -> holds :chorus's weight
(glow.bind.program :scene)               ; program-change N -> scene N, any keyboard
```

`:master`, `:cue-level <cue>`, and `:param <name>` are the same three actions no matter which
of `glow.bind.fader`/`pitchbend`/`pressure` is doing the binding — that's the point: the
*shape* of the source (a source that reports a smooth `0..1`) is what matters, not which MIDI
message type it happens to arrive as. An effect reads a `:param` binding back with
`(glow.param.get :hue)`.

## The zero-allocation rule

Effect bodies run on the render task, once per active effect per frame, inside a hard
instruction budget. The Lua GC is stopped by default and only ever steps in the render loop's
leftover frame slack — never a synchronous collection mid-frame. That means an effect that
allocates a new table (or a new, non-literal string) every frame is quietly generating garbage
the GC can't fully keep up with indefinitely; eventually it catches up in one big pass, and
that pass can blow the frame budget and drop a DMX frame. On a real rig, a dropped frame is a
visible flicker or a glitch on a moving head, not an abstract performance number.

The fix is the same one that applies to any real-time loop: **build tables outside the effect,
reuse them inside it.**

```fennel
;; Bad: a new table is constructed every single frame this effect runs.
(fn flicker-bad [t]
  (glow.aim 2 [(math.sin t) 2 0]))

;; Good: nothing is allocated after the closure captures `target` once.
(local target [0 2 0])
(fn flicker-good [t]
  (tset target 1 (math.sin t))
  (glow.aim 2 target))
```

Capability names are the other half of this rule, and they're easy to get backwards: a
**literal** keyword/string in Fennel source (`:dimmer`, `"red"`) is a compile-time constant —
created once, reused as the same interned pointer on every call, genuinely free. A
**constructed** string (`(.. "dim" suffix)`) allocates a brand-new Lua string every time it
runs. `glow.set`/`glow.slot` accept both, but only the literal form is actually free — write
the capability name as a literal whenever the choice is yours, which in practice is almost
always.
