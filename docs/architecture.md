# Architecture

## The load-bearing rule

```
C++ / FreeRTOS   — DMX + Art-Net out, render loop @44 Hz, pixel engine, aim geometry
     ↑             (hard real-time; never blocked by scripts)
Lua VM           — effects, cues, scenes, config  (composes; does NOT do tight loops)
     ↑
Fennel           — Lisp surface, self-hosted compiler running ON the device
```

**Lua composes and parameterises; C++ runs the tight loops.** Every design decision in
this file exists to enforce that rule, not just to make the VM work. `glow.*` (the
functions Fennel scripts call) is a thin, sandboxed C API surface over the existing C++
engine — see the [API reference](reference.html) for exactly what it exposes today, and
[Writing a show](authoring.html) for how to use it; this page only covers *why* it's shaped
the way it is.

## Patch vs. show

The **patch** — which fixtures exist, what DMX channels they occupy, where they physically
are, which universes/transports carry them — lives in the binary-encoded `.fdef`/`.show`/
`.mdef` bundle the browser provisioner compiles and flashes (see the
[grammar reference](grammar.html)). It changes rarely and is not
something a live-coded script can redefine; a script that could redefine channel counts
mid-show is a script that can corrupt the render loop's fixed-size intent buffers.

The **show** — cues, scenes, effects, controller bindings — is Fennel, hot-swapped while
the rig runs. `glow.cue.*`/`glow.scene.*` wrap the existing `ShowController` (fades,
priority, HTP/LTP, unmodified); redefining a cue name points it at a *new* controller cue
and releases the old one, rather than leaving two versions of the same named cue running.

Bindings (`glow.bind.*`, `glow.led.*`) are a *third* thing again: live-editable, no file
format, resolved through the `.mdef`'s hardware description (channel-significant PAD
ranges, LED palettes) but never baked into the binary patch. One show `.fnl` runs on any
controller — a binding to a control the connected controller doesn't have is a no-op, not
an error.

## The concurrency invariant: exactly one of everything

There is **exactly one** Lua VM, owned by the render task — the same single-owner
discipline `ShowController` established first (`control_queue.h`), reused rather than
reinvented for the eval-submission path (`eval_queue.h`). Script submissions (a WS `eval`
message, a boot-file load) cross from the web input task to the render task through that
queue, drained once per frame in the frame slack, never blocking the render loop waiting
on a script.

## Real-time safety, four guards

#### 1. Instruction budget

A frame-context call (an already-compiled effect closure, invoked once per frame) gets a
tight budget (20,000 instructions) — nowhere for a runaway loop to hide. Compiling and
running a fresh Fennel source string (the REPL, a boot file) gets a much larger one
(2,000,000) — compiling the Fennel compiler's own two-line effect costs ~30,000
instructions before the compiled code runs a single instruction, so the frame budget
would reject every real compile.

#### 2. GC pacing

Stopped by default; a bounded number of GC steps run in the render task's frame slack,
never a synchronous full collection mid-frame.

#### 3. Everything is `lua_pcall`

No unprotected call anywhere in this layer.

#### 4. A global memory cap

One VM, one ceiling; the custom allocator returns `NULL` past it, Lua raises
`LUA_ERRMEM`, `lua_pcall` catches it, the offending call is disabled — the same error
path as any other script failure, no new machinery.

"The show goes on": a broken effect is disabled, not retried — logged once, and the
owning cue keeps running its *other* effects. A partially-emitted frame is fine; HTP/LTP
blending absorbs it. The same philosophy extends to safe-blackout on a corrupt/missing
bundle or a broken `boot.fnl` — the device falls back to a known-safe state rather than
refusing to boot.

## Sandboxing

Every script eval and effect callback runs against a *separate* sandboxed environment: a
shallow copy of `_G` with `dofile`/`loadfile`/`load`/`require`/`package`/`collectgarbage`
deleted. `load` is stripped alongside `dofile`/`loadfile` because it can run arbitrary,
unverified precompiled bytecode — strictly more dangerous than either.
`collectgarbage("collect")` is stripped for the same real-time reason as guard #2 above:
letting a script force a synchronous full GC pass mid-frame would defeat the pacing
entirely.

## Further reading

- [Writing a show](authoring.html) — concept-first guide to the Fennel authoring layer,
  with real examples.
- [API reference](reference.html) / [Grammar reference](grammar.html) — every `glow.*` call
  and every `.fdef`/`.show`/`.mdef` keyword, hand-written and checked against source in CI
  (`docs/build/gen-reference.mjs`) so neither can silently go stale.
- `README_LIVE_CONTROL.md`, `README_WLED.md`, `README_PIXEL_MATRIX.md` in the repo root —
  deeper hand-written notes on specific subsystems.
