# esp-glow

A live-codable DMX lighting engine for the ESP32-S3 — an Afterglow-style show system with no
JVM, running standalone on a microcontroller. Native C++ does the hard real-time work; your
show is written in **Fennel** (a Lisp), compiled **on the device**, and hot-swapped while the
rig is running.

Point a moving head at a spot in the room, scroll a plasma across an LED matrix, lock a strobe
to a CDJ's beat — then change any of it from a REPL, mid-song, without reflashing. A MIDI
controller's pads light up to mirror the show.

> **Status:** every subsystem is built, merged, and green on host (34 test suites,
> `-Wall -Wextra -Werror` + ASan/UBSan/TSan). The firmware **boots and runs in QEMU** (render
> loop at 44 Hz, Lua/Fennel VM up, bundle + scripts loaded). **It has not yet run on real
> silicon** — hardware bring-up is the next step (§9).

---

## 1. Architecture

```
Fennel         — Lisp surface. Self-hosted compiler runs ON the device.
    ↓
Lua VM         — effects, cues, scenes, bindings. Composes and parameterises.
    ↓
C++ / FreeRTOS — render loop @44 Hz, DMX + Art-Net/sACN out, pixel engine, aim geometry,
                 PLL beat clock. Hard real-time, pinned to core 1. Never blocked by scripts.
```

**Load-bearing rule: Lua composes, C runs the tight loops.** **Concurrency invariant:** the
render task is the *only* thing that mutates engine state; every input — WebSocket, MIDI, OSC,
DJ-Link/beat packets, script submissions — is parsed on its own task and **pushed onto a
queue**, drained on the render core each frame. Core 1 runs the render loop and nothing else;
all other tasks are pinned to core 0.

The engine turns *time* into *DMX frames*: effects emit abstract **intents** ("this fixture is
blue at 80%", "this head points there"), a resolve layer maps them to channels via per-fixture
profiles, and cues blend overlapping intents (HTP intensity, LTP position) before the frame is
flushed.

Two data paths: **per-fixture** (heads/PARs — one value per capability; heads add aim
inverse-kinematics) and **per-pixel** (RGB matrices — a C pattern engine over Art-Net/sACN).

### Two halves of a show

| | **Patch** | **Show** |
|---|---|---|
| What | Fixtures, addresses, geometry, matrices, controller | What the fixtures *do* over time |
| Format | `.fdef`/`.show`/`.mdef` → binary `PFX2`/`SHW1`/`MDF1` | **Fennel** |
| Changes when | You change the rig | Every song |
| Lives in | A raw flash partition (browser-flashable) | LittleFS (writable, hot-swappable) |

---

## 2. Scripting: Fennel on the device

An effect is a function of time that **emits** intents (never returns them — that would
allocate every frame and feed the GC):

```fennel
(fn breathe [t]
  (glow.set 1 :dimmer (* 0.5 (+ 1 (math.sin (* t 2)))))
  (glow.slot 1 :color-wheel "red"))          ; named function-range slot (PFX2)

(glow.cue.define :warm {:effects [breathe] :fade-in 2.0})
(glow.cue.go :warm)
```
Send it over the WebSocket REPL; the light responds immediately. See `GRAMMAR_REFERENCE.md`
for the full `glow.*` API (set/aim/slot/cue/scene/fx/matrix/beat/bind/led/save).

**Effects must not stutter:** allocate nothing in an effect body (string *literals* are
interned and free; concatenation and table construction allocate), and never loop per-pixel in
Lua — use `glow.matrix.pattern`.

**The show goes on.** A syntax error, a runtime error, an infinite loop, or out-of-memory each
leaves the rig still rendering: the offending effect is disabled and reported (`fx_error`), its
cue keeps running its other effects. A broken `boot.fnl` falls back to safe blackout, never a
brick.

---

## 3. Musical time

A **PLL `BeatClock`** turns jittery clock sources into a smooth phase (gradual correction, no
snapping, monotonic beat number, free-run on dropout). Sources: **MIDI clock**, **Pro DJ Link**
(passive CDJ beat packets — gives true downbeats), **tap tempo**, internal. `(glow.beat)`,
`(glow.bar)`, `(glow.bpm)`, `(glow.locked?)` drive beat-synced effects; when no source is live
they free-run rather than freeze.

---

## 4. Controllers (MIDI) — described, not hardcoded

A controller is a **`.mdef` file**, not C++. It declares pads, faders, encoders, LED palette,
per-range MIDI-channel handling, and an opaque `INIT SYSEX` sent on connect (e.g. the APC40's
Mode-0 message). Bindings live in Fennel and are named by **source shape**, so one show runs on
any controller:

```fennel
(glow.bind.pad-xy 0 0 :toggle :chorus)   ; grid (col,row), resolved via the .mdef
(glow.bind.pitchbend :param :hue)        ; any pitch wheel drives a parameter
(glow.led.auto 53 :chorus :green :off)   ; the pad lights when its cue is active
```
Every binding is a no-op if the controller lacks that control. LED feedback and the web console
read the **same active-cue state**, so a cue fired anywhere updates the pads and every console
together. Adding a new controller is a new `.mdef` — zero code.

---

## 5. Fixtures & import

Four archetypes: **wash/PARs** (RGBWAUV + dimmer + strobe), **moving heads** (16-bit pan/tilt +
aim geometry; **PFX2 named ranges** make colour wheels, gobos, prisms, strobe modes addressable
by name), **smoke/haze**, **RGB matrices** (serpentine/progressive, colour order,
multi-universe). Don't hand-write profiles: the browser provisioner **imports QLC+, Open Fixture
Library, and GDTF**, mapping channels *and* named ranges onto PFX2.

---

## 6. Hardware & flashing

ESP32-S3 (dual-core, FPU, PSRAM — the Lua VM lives there). DMX out via an RS-485 transceiver
(MAX3485; default GPIOs TX 17 / RX 18 / DE-RTS 8). Matrices via Art-Net or sACN over WiFi.
Inputs: MIDI (DIN/UART or native USB host), OSC (UDP), DJ-Link, and a device-served web console.

**Flash-time config (no toolchain):** the browser flasher writes a `CFG1` blob — WiFi
credentials, DMX GPIOs, Art-Net fallback, and a **USB-MIDI checkbox** — so a stranger can flash a
working rig without ESP-IDF. Config is also editable from the device console. A missing/corrupt
config boots on compiled-in defaults and says so.

**Build** (ESP-IDF v5.1+): Lua 5.4.6 (`luaconf.h` **must** set `LUA_32BITS 1` — edit the file,
not `-D`) + Fennel 1.6.1 (generated single file via `scripts/vendor_fennel.sh`). See
`BENCH_RUNBOOK.md` for the full bring-up ladder.

---

## 7. Formats

See `GRAMMAR_REFERENCE.md` for `.fdef` / `.show` / `.mdef`. Key points: `.show` is **1-indexed**
and requires a `SHOW 2` header (un-migrated files are rejected loudly); the compiler **errors on
address collisions**; a `CAP` with no `SLOT`/`RANGE` is linear; a control with no `CH` clause is
channel-agnostic. Binary bundles are versioned and backward-compatible (`SHW1` v4, `PFX2` v2,
`MDF1` v3, `CFG1` v1 — older versions still load).

---

## 8. Testing

```sh
make test    # 34 suites, -Wall -Wextra -Werror + ASan/UBSan (TSan on the queue suite)
```
Host-tested: the engine, the Lua layer (emission, error policy, infinite-loop hook, memory cap,
a zero-allocation check), the PLL beat clock (jitter rejection, monotonicity), every binary
format (fuzzed), MIDI parsing, and the importers. **QEMU** boots the real image in CI (asserts
boot telemetry, a 5× anti-flake loop, addr2line-symbolicated crashes). **HIL** (`tests/hil/`)
covers what neither can: DMX timing, Art-Net on the wire, the Fennel REPL end-to-end, and a
10-minute soak. Two things stay human: whether a head physically aims right, and colour.

---

## 9. Status & next steps

**Done, merged, host-green, boots in QEMU:** engine · aim geometry · cues/scenes with HTP/LTP
blending · pixel matrices · PFX2 named ranges · the Lua/Fennel live-coding layer with real-time
guards · concurrency-safe input dispatch · firmware F0–F5 (DMX, WiFi/Art-Net/sACN, show load,
WS/MIDI/OSC/DJ-Link transports, OTA+rollback, watchdog, safe blackout) · musical time · web
console with the Fennel REPL and device-pushed state · controller-agnostic MIDI with LED
feedback and `INIT SYSEX` · CFG1 flash-time config · multi-node Art-Net + ArtSync + WLED · the
browser provisioner, fixture importers, and USB flasher · the QEMU + HIL harnesses.

**Not done: it has never run on real hardware.** That is the next and only real step. The
bring-up ladder (`BENCH_RUNBOOK.md`): boot → **DMX out** (the milestone) → WiFi/Art-Net → console
→ live-coding → HIL → **the soak** — the one unvalidated risk in the whole stack: whether the Lua
GC, paced in the frame slack, ever drops a DMX frame under load. No host or QEMU test can answer
that.

**Later:** semantic colour mapping (`:red` → nearest wheel slot or RGB — profile v3, the format
byte is reserved) · gamma correction · audio-reactive matrix patterns · matrices inside the
cue/blend engine · Ableton Link · a richer web file/asset workflow (multiple shows & scripts,
export).

**Known limitations (for a shipping product):** WiFi password in plaintext in flash, no console
auth, unsigned OTA images. Documented choices, not accidents — revisit as a block if productized.
