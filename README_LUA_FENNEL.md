# Lua + Fennel Live-Coding Layer

This is the layer that makes esp-glow an *Afterglow*, not just a DMX
player: the show (effects, cues, scenes) is written in Fennel, compiled
and evaluated on the device, and hot-swapped while the rig is running.
The C++ engine keeps doing hard real-time; Lua composes and parameterises
it.

**The `glow.*` function list below is illustrative, not authoritative** --
see `docs/reference.md` for the full, hand-written entry on every function
(and `docs/authoring.md` for a concept-first guide to using them). CI
checks `docs/reference.md` against the real names registered in
`glow_lua_api.cpp` and fails the build if they ever disagree
(`docs/build/gen-reference.mjs`), so that page can't silently go stale.
See `docs/architecture.md` for this same "why" narrative. This file's
real-time-safety and sandboxing sections are the canonical hand-written
explanation of *why* the layer is shaped this way.

```
C++ / FreeRTOS   — DMX + Art-Net out, render loop @44 Hz, pixel engine, aim geometry
     ↑             (hard real-time; never blocked by scripts)
Lua VM           — effects, cues, scenes, config  (composes; does NOT do tight loops)
     ↑
Fennel           — Lisp surface, self-hosted compiler running ON the device
```

The load-bearing rule: **Lua composes and parameterises; C++ runs the
tight loops.** Every design decision below exists to enforce that rule,
not just to make the VM work.

## Vendoring

Neither Lua nor Fennel ships as something you can just drop in.

- **Lua 5.4.6** (`third_party/lua/`): upstream C sources at the `v5.4.6`
  tag, with one patch — `luaconf.h`'s `LUA_32BITS` flipped from `0` to
  `1`, making `lua_Number`/`lua_Integer` `float`/`int32` instead of
  `double`/`int64`. The ESP32-S3's FPU is single-precision; running Lua's
  default doubles would fall back to ~10x-slower software emulation for
  every arithmetic op. This must be done by editing the macro in
  `luaconf.h`, not `-DLUA_32BITS=1` on the command line — the latter
  leaves the compiled library on doubles. Verified: `string.pack("n",
  1.0)` is 4 bytes with the patch applied.
- **Fennel 1.6.1** (`third_party/fennel/fennel.lua`): Fennel is a
  self-hosted compiler (Fennel compiling Fennel), so there's no
  pre-built single-file release for a tag — it's bootstrapped: build a
  throwaway host `lua`, then run Fennel's own `make LUA=... fennel.lua`
  (`--require-as-include`) to AOT-compile the whole compiler into one
  ~300 KB file. Pinned to the `1.6.1` release tag, not git HEAD (which
  reports itself as `1.7.0-dev` and isn't a reproducible artifact).

`scripts/vendor_lua.sh` and `scripts/vendor_fennel.sh` record exactly how
each was produced and are re-runnable from scratch; `vendor_fennel.sh`
also self-checks the vendored Fennel produces the design's reference emit
pattern before committing anything.

Both vendored trees are curated at compile time (see the root `Makefile`
and `firmware/components/lua/CMakeLists.txt`) to exactly the Lua
libraries the sandbox opens — core VM + `base`/`math`/`string`/`table` and
their aux lib. `io`/`os`/`debug`/`package` (`loadlib.c`)/`utf8`/
`coroutine`/`ltests`/`onelua`/the standalone `lua.c` CLI are never even
*compiled in*. That's a second line of defense on top of the runtime
sandboxing below: a bug that tried to reopen one of those libraries would
fail to **link**, not silently reopen a hole.

## Architecture

```
glow_fennel.{h,cpp}     process-wide VM singleton + glow_lua_eval_fennel()
    │                   + pumpEvalSubmissions() (the WS eval-channel drain)
    ├── lua_vm.{h,cpp}       LuaVM: allocator (cap + high-water mark),
    │                        sandboxing, GC pacing, instruction budgets
    ├── glow_lua_api.{h,cpp} GlowLuaApi: the `glow.*` table (set/aim,
    │                        cue/scene, fx.*, matrix.*)
    └── lua_effect.{h,cpp}   LuaEffect : IEffect — wraps one Lua function

eval_queue.{h,cpp}      the eval submission queue (WS "eval" → render task)
eval_queue_freertos.cpp device backend (TODO stub, see below)
scripts_storage.{h,cpp} LittleFS "scripts" partition (boot.fnl, glow.save)
lua_glow_include.h      extern "C" wrapper around the vendored Lua headers
```

There is **exactly one VM**, owned by the render task — the same
single-owner discipline `control_queue.h` established for
`ShowController` (see `README_CONTROL_QUEUE.md`), reused here rather than
reinvented. Script submissions (WS "eval" messages) cross from the web
input task to the render task through `eval_queue.h`, drained once per
frame in the frame slack via `glow::pumpEvalSubmissions`, exactly like
`pumpControlEvents`.

## The `glow.*` API

```fennel
(fn my-effect [t]
  (glow.set 1 :dimmer (* 0.5 (+ 1 (math.sin (* t 2)))))
  (glow.set 1 :red 1.0)
  (glow.aim 2 [0 2 5]))                 ; point head 2 at a world point

(glow.cue.define :verse-wash
  {:effects [my-effect (glow.fx.hue-rotate [2 3] {:period 4.0})]
   :fade-in 2.0 :fade-out 1.0 :priority 0})
(glow.cue.go :verse-wash)

(glow.scene.define :chorus [:verse-wash :strobe-hit])
(glow.scene.go :chorus)

(glow.matrix.pattern 0 :plasma {:speed 0.5 :scale 0.2})
(glow.matrix.brightness 0 0.8)
```

- **`glow.set` / `glow.aim`** are only valid inside an effect callback
  (`LuaEffect::evaluate` brackets the call with `GlowLuaApi::beginFrame`/
  `endFrame`); calling them from a bare `eval` (the REPL, boot.fnl) is a
  clear error, not a crash. They emit straight into the render loop's
  `CapIntent`/`AimIntent` vectors — **effects never return intents**,
  because returning a table would allocate every frame.
- Capabilities are strings (`:dimmer`) or `glow.CAP.dimmer` integer
  constants — both zero-allocation on the frame path. A literal string in
  Fennel source is a compile-time constant (created once, reused as a
  pointer every call); resolving it in C is a linear scan over ~19 short
  names. The real hazard is a **constructed** string
  (`(.. "dim" suffix)`) — that allocates a new Lua string every call.
  `test_lua_effect.cpp` proves both directions: a literal-capability
  effect allocates nothing after warm-up, and the same effect rewritten
  with concatenation does allocate.
  - An **unresolvable** capability *name* (a typo) is a silent no-op —
    indistinguishable from a fixture that simply lacks that capability,
    and shouldn't take down the whole effect over a spelling mistake. A
    **wrong argument type**, however, is a hard Lua error (`luaL_check*`),
    caught by the effect's own `lua_pcall` like any other runtime error.
- **`glow.cue.*` / `glow.scene.*`** wrap the existing `ShowController`
  (fades, priority, HTP/LTP — unmodified). Fennel-side names map to
  `ShowController`'s numeric ids via a small name→id table. Redefining a
  cue name (the normal live-coding loop: tweak, re-eval `cue.define`)
  points the name at a **new** `ShowController` cue and releases the old
  one if it was active, rather than leaving two versions of the same
  named cue running — `ShowController` itself is never patched to support
  update-in-place (out of scope; see below).
- **`glow.fx.*`** (`hue-rotate`, `chase`, `sweep`) constructs the
  existing C++ effect classes (`effects.h`) and returns an opaque
  `glow.effect_handle` userdata usable directly in a cue's `:effects`
  list, right alongside plain Fennel functions. Script authors get the
  fast, hand-tuned built-ins for free and write Lua only for genuinely
  new behavior.
- **`glow.matrix.*`** selects/parameterises a named `IPixelPattern`
  (`plasma`, `rainbow`, `solid`) via an injected `IMatrixRegistry` — Lua
  never gets a per-pixel callback (~340 px × 44 fps with trig is not a
  budget a general-purpose scripting VM can hold; that's a dedicated
  expression-VM problem, out of scope here, same as Pixelblaze's).

## Real-time safety

Four guards, all mandatory, all host-tested:

1. **Instruction budget.** `lua_sethook(L, hook, LUA_MASKCOUNT, N)`,
   re-armed immediately before *every* `lua_pcall` — not installed once
   at boot. Re-arming resets the countdown fresh for that call, so the
   budget is genuinely per-call rather than a rolling counter that could
   abort an innocent short call purely because of *previous* calls'
   instruction count. Two different budgets, because the two call sites
   are very different workloads:
   - **`armFrameBudget()`** (20,000 instructions) around invoking an
     *already-compiled* effect closure once per frame — a couple dozen
     instructions in the well-behaved case, nowhere for a
     `while true do end` to hide.
   - **`armEvalBudget()`** (2,000,000 instructions) around
     *compiling-and-running* a Fennel source string. Measured directly
     against the vendored compiler: compiling a trivial two-line effect
     costs ~30,000 VM instructions by itself, before the compiled code
     runs a single instruction — the frame budget would reject every
     real compile.
2. **GC pacing.** Generational (`LUA_GCGEN`), created **stopped**
   (`LUA_GCSTOP`). It never runs automatically. `LuaVM::gcStepSlack(us)`
   spends a bounded budget of `LUA_GCSTEP` calls in the render task's
   frame slack; nothing else ever collects, except one unbounded
   `collectFullyOnce()` called exactly once at startup (loading the
   Fennel compiler alone leaves ~200 KB of parser/compiler garbage behind
   with GC stopped — measured ~910 KB used right after
   `loadFennelCompiler`, ~680 KB after this one call — reclaiming it
   before the render loop starts is free of any real-time cost).
3. **Everything is `lua_pcall`.** No unprotected call anywhere in this
   layer.
4. **A global memory cap.** One VM, so one ceiling
   (`LUA_DEFAULT_MEM_CAP_BYTES` = 1 MB — the top of the design's
   suggested 512 KB–1 MB range, because loading the Fennel compiler alone
   already uses ~910 KB with GC stopped (~680 KB after the one-time
   `collectFullyOnce`, see guard #2), leaving almost no headroom for
   scripts at the bottom of that range). The custom Lua allocator counts
   bytes and returns `NULL` past the cap; Lua raises `LUA_ERRMEM`;
   `lua_pcall` catches it; the offending call is disabled — reusing the
   same error path as any other script failure, no new machinery. A
   subtlety worth flagging for anyone touching `lua_vm.cpp`: per the Lua
   manual, when `ptr == NULL` (a brand-new allocation) the `osize`
   parameter is **not** the previous size — it's an object-kind tag
   (`LUA_TSTRING` etc). Treating it as a size there silently corrupts the
   running byte count.

`LuaEffect`'s error policy implements the "disabled, not retried" half of
this: on any error, `disabled_` becomes permanently `true` — a broken
effect is never retried on a later frame (44×/sec of failed `lua_pcall`
plus log spam otherwise), the error is logged once, and the owning cue
keeps running its *other* effects. A partially-emitted frame is fine —
HTP/LTP blending absorbs it, and the effect's contribution simply
vanishes from the next frame on.

## Sandboxing

The VM's trusted `_G` opens exactly `base`/`math`/`string`/`table`, plus a
~10-line hand-rolled `require()`/`package.preload` shim — needed only
because the vendored `fennel.lua` (built with `--require-as-include`) is
internally a chain of `package.preload[...]` assignments stitched
together with real `require()` calls; it cannot execute at all without
one. This shim never touches the filesystem and is never exposed to
scripts.

Every script eval (and every effect callback) runs against a *separate*
sandboxed environment table — a shallow copy of `_G` with
`dofile`/`loadfile`/`load`/`require`/`package`/`collectgarbage` deleted
and `_G` inside it self-referencing the copy, not the trusted one. `load`
is stripped along with `dofile`/`loadfile`, not just those two — it can
run arbitrary, **unverified precompiled bytecode**, which is strictly
more dangerous. `collectgarbage` is stripped for real-time safety: a
script calling `collectgarbage("collect")` would force an unbounded
synchronous GC pass mid-frame, exactly the failure mode guard #2 above
exists to prevent.

## Testing

Ten host-tested binaries, all under `-fsanitize=address,undefined`,
wired into `make test`:

| Binary | Covers |
|---|---|
| `test_lua_vm` | Allocator cap + high-water mark + call counter, sandbox library/env restrictions, GC pacing, both instruction budgets (including that the eval budget really is more generous than the frame budget) |
| `test_lua_effect` | The emit pattern end-to-end through real Fennel, unknown-capability no-op vs bad-arg error, the disabled/not-retried contract, a cue surviving one broken effect, and the zero-allocation proof (both directions) |
| `test_glow_lua_api` | `cue`/`scene` `define`/`go`/`release` against a real `ShowController`, cue redefinition's release-the-old-cue behavior, `glow.fx.*` handles usable in a cue and actually emitting, `glow.matrix.*` against a fake `IMatrixRegistry` |
| `test_glow_fennel` | The process-wide singleton, and the definition-of-done guarantee itself: a syntax error, a runtime error, an infinite loop, and an out-of-memory each leave the VM usable for the next eval; the eval-submission queue drain and its per-frame bound |
| `test_scripts_storage` | `scriptNameIsValid` (path-traversal/embedded-NUL/length rejection) |
| `test_web_protocol` (extended) | `parseEvalCommand` (JSON escape decoding — the one place in this protocol that needs real escapes, since script source is free text) and `buildEvalResultJson` |

A subtlety the zero-allocation test had to work around: Lua 5.4 interns
short strings, so concatenating the *same* two literals every frame would
dedupe to the one already-interned string after the first call and
allocate nothing — the failure mode the test exists to catch. The
non-allocating-effect test mixes the per-frame `t` into the constructed
string so each call genuinely produces a new one.

Run everything:
```bash
make test
```

## `main.cpp` wiring

Unlike a hardware transport, none of this needed real hardware to reason
about correctly, so it's wired all the way through rather than left as a
dangling capability:

- **`g_controller` is `g_show`'s one and only effect**
  (`g_show.addEffect(&g_controller)`). `ShowController` is itself an
  `IEffect` — it does not sit beside `Show`, it plugs into it exactly like
  any other effect, and it is what emits the resolved (fades, HTP/LTP)
  intents `Show::renderFrame` consumes. `glow.cue.*`/`glow.scene.*`, the
  web console, and MIDI/OSC all converge on this **one** instance via
  `LiveControl` — there is no separate path that bypasses it. The old
  hardcoded-fallback `DimmerEffect` (added straight to `g_show`, pre-F6)
  has been rewired as a cue on `g_controller` for the same reason: an
  effect added directly to `Show` runs permanently, outside every cue,
  invisible and unstoppable from Lua or any console.
- **`pumpControlEvents` runs every frame**, at the top of the existing
  `pre_render` hook, before `renderFrame` — not after, and not
  conditionally. `LiveControl`/the control-event queue were pulled
  forward from F4 into `glow_core`'s `SRCS` for exactly this: a
  `ShowController` with nothing feeding it isn't wired to anything.
- **`gcStepSlack` runs every frame**, via a new `post_render` hook
  (`render_task.h`) invoked after `renderFrame` with however much slack
  is actually left before the next deadline. The render loop re-reads the
  clock after this call before computing how long to sleep — computing
  the sleep from the pre-hook estimate would double-count whatever time
  GC just spent and slow the render rate the moment there's real garbage
  to collect.

What's still a stub, and correctly so — these need real hardware, not
more C++:

- The FreeRTOS backends themselves (`control_queue_freertos.cpp`,
  `eval_queue_freertos.cpp`) — the queue *handles* aren't created yet
  (`xQueueCreate` etc. are commented-out TODOs), so on device `pop()`
  always reports empty today. The consumer side (this section) is real;
  only the innermost `xQueueSend`/`xQueueReceive` calls remain.
- The actual transports that would push into those queues: the web httpd
  endpoint, MIDI UART, OSC UDP (`web_input.cpp`, `midi_input.cpp`,
  `osc_input.cpp`). `web_input.cpp`'s message *dispatch* (parse → queue /
  `scripts_storage_*` call / reply-JSON build, covering the console's
  `eval`/`script_list`/`script_load`/`script_save`/`script_delete`/
  `fx_error` — see README_WEB_CONSOLE.md) is written and documented; only
  the httpd URI handler and its `xQueueSend`/`fopen`/`readdir` innards
  remain hardware-only TODOs.
- LittleFS mount/read/write (`scripts_storage.cpp`'s device half — now
  including `list`/`load`/`delete`, not just `mount`/`read_boot`/`save`),
  for the same reason `storage_manager.cpp` was before F3's hardware
  validation.

## What is HIL-only (cannot be verified in this environment)

Everything else is exercised on the host with the real vendored Lua +
Fennel. What's left needs an actual ESP32-S3:

- GC pacing under real frame-timing load; frame timing with scripts
  actually running.
- Hot-swap while rendering (redefine a cue, watch the lights change,
  live).
- Boot-from-`boot.fnl`, and the blackout fallback on a broken one.
- `lua_mem` high-water-mark trend during a soak (the design's
  `GLOW-TEST: lua_mem=<bytes>` telemetry line).
- Everything in the "still a stub" list above, once hardware is involved.

## Out of scope (see the design doc)

- Per-pixel Lua callbacks — a dedicated expression-VM project, not this
  one.
- Replacing the SHW1 patch with scripts — the patch stays binary and
  browser-flashed; scripts never define fixtures/geometry.
- Beat/clock sync — belongs in C with a Lua accessor, later.
- Coroutine-based effects — plain function effects first.
- Multiple VMs / per-script state — there is exactly one VM, on the
  render task.
- Modifying `ShowController`, `Show`, the C++ effect library, or
  `pixel_matrix`/`pixel_patterns` — all reused as-is, wrapped, not
  rewritten.
