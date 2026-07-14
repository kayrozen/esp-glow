# esp-glow - it's like wled dated afterglow

A live-codable DMX lighting engine for the ESP32-S3 — an Afterglow-style show system with
no JVM, running standalone on a microcontroller. Native C++ does the hard real-time work;
your show is written in **Fennel** (a Lisp), compiled **on the device**, and hot-swapped
while the rig is running.

Point a moving head at a spot in the room, scroll a plasma across an LED matrix, lock a
strobe to the beat of a track playing on a CDJ — then change any of it from a REPL,
mid-song, without reflashing.

> **Status:** every subsystem is built, merged, and green on host (25 test suites,
> `-Wall -Wextra -Werror` + ASan/UBSan/TSan). **It has never run on silicon.** Bring-up is
> the next step — see §9.

---

## 1. Architecture

```
Fennel         — Lisp surface. Self-hosted compiler runs ON the device.
    ↓
Lua VM         — effects, cues, scenes, config. Composes and parameterises.
    ↓
C++ / FreeRTOS — render loop @44 Hz, DMX + Art-Net out, pixel engine, aim geometry,
                 beat clock. Hard real-time. Never blocked by scripts.
```

**The load-bearing rule: Lua composes, C runs the tight loops.** Scripts choose *what*
happens and with *which parameters*; the C engine executes the per-pixel and per-channel
work. (The same lesson Pixelblaze learned — a general-purpose language cannot drive 15,000
pixel updates a second on an MCU, but it is perfect for describing a show.)

The engine turns *time* into *DMX frames*. Effects emit abstract **intents** — "this fixture
is blue at 80%", "this head points at that spot" — a resolve layer maps them to channel
values via per-fixture profiles, and cues blend overlapping intents (HTP for intensity, LTP
for position) before the result is flushed to the wire.

**Concurrency, in one sentence:** the render task is the *only* thing that mutates engine
state. Every input — WebSocket, MIDI, OSC, beat packets, script submissions — is parsed on
its own task and **pushed onto a queue**, drained on the render core each frame. This
invariant is the backbone of the whole system.

### Two data paths

- **Per-fixture** — moving heads, PARs, wash, smoke. One resolved value per capability.
  Heads add pan/tilt inverse kinematics: aim at a *point*, not an angle.
- **Per-pixel** — RGB matrices. A C pattern engine renders a 2D canvas, packed into DMX
  universes and sent over Art-Net.

### Two halves of a show

| | **Patch** | **Show** |
|---|---|---|
| What | Which fixtures exist, addresses, geometry, matrices | What they *do* over time |
| Format | `.fdef` / `.show` → binary `PFX2`/`SHW1` bundle | **Fennel** |
| Changes when | You change the rig | Every song |
| Lives in | A raw flash partition (browser-flashable) | LittleFS (writable, hot-swappable) |

---

## 2. Scripting: Fennel on the device

An effect is a function of time that **emits** intents — it never returns them (returning
tables would allocate every frame and feed the GC).

```fennel
(fn breathe [t]
  (glow.set 1 :dimmer (* 0.5 (+ 1 (math.sin (* t 2)))))
  (glow.set 1 :red 1.0))

(glow.cue.define :warm-breath {:effects [breathe] :fade-in 2.0 :fade-out 1.0})
(glow.cue.go :warm-breath)
```
Send that over the WebSocket REPL; the light responds immediately.

### The `glow.*` API

```fennel
;; Emit (inside an effect callback only)
(glow.set 1 :dimmer 0.8)                 ; capability → normalized [0,1]
(glow.aim  2 [0 2 5])                    ; point a head at a world point

;; Named function ranges (PFX2) — how you actually drive a moving head
(glow.slot 1 :color-wheel "red")         ; discrete slot → centre of its DMX range
(glow.slot 1 :gobo "dots")
(glow.slot 1 :gobo 3)                    ; …or by index
(glow.slot 1 :shutter-strobe "strobe" 0.8)  ; continuous sub-range → 80% across it
(glow.ranges 1 :gobo)                    ; discover what this fixture can do

;; Musical time
(glow.beat)  (glow.bar)  (glow.bpm)  (glow.beat-number)  (glow.locked?)

;; Built-in C effects — fast, use them freely
(glow.fx.hue-rotate [1 2 3] {:period 4.0})
(glow.fx.chase [1 2 3 4] {:period 2.0})
(glow.fx.sweep 5 [0 0 1] [1 0 1] {:period 6.0})

;; Cues, scenes, matrices, persistence
(glow.cue.define :verse {:effects [breathe] :fade-in 2.0 :priority 0})
(glow.scene.define :chorus [:verse :strobe-hit])
(glow.matrix.pattern 0 :plasma {:speed 0.5 :scale 0.2})
(glow.save :boot "(glow.cue.go :verse)")
```

Emitting a capability (or a slot) a fixture doesn't have is a harmless **no-op** — so cues
written for one head degrade gracefully on another.

### Writing effects that don't stutter

At 44 Hz you have 22.7 ms per frame. A GC pause drops a DMX frame, and a dropped frame is
*visible on stage*.

- **Allocate nothing in an effect body.** String *literals* (`:dimmer`) are interned at
  compile time and cost nothing. String *concatenation* (`(.. "dim" n)`), table
  construction, and per-call closures all allocate. Build them once, outside.
- **No per-pixel loops in Lua.** Use `glow.matrix.pattern`; add a C pattern if you need a
  new one.

### The show goes on

Live-coding means you *will* ship a bug mid-set. Nothing a script does can take the rig
down:

| Failure | What happens |
|---|---|
| Syntax error | Reported to the console; nothing changes |
| Runtime error in an effect | That effect is disabled and reported (`fx_error`); **its cue keeps running its other effects**; the rig keeps rendering |
| Infinite loop | An instruction-count hook aborts it |
| Out of memory | The script is aborted; rendering continues |
| Broken `boot.fnl` | Safe blackout + report — never a brick |

Every script call is protected. Nothing unwinds the render loop.

---

## 3. Musical time (beat sync)

The feature that made Afterglow *Afterglow*. Clock sources are discrete and jittery; effects
need a smooth phase. A **PLL-based `BeatClock`** reconciles them — gradual phase correction
(never snapping), garbage rejection, free-run on dropout, and a strictly monotonic beat
number.

Sources: **MIDI clock** (24 PPQN) · **Pro DJ Link** (passive listening to CDJ beat packets —
gives you true *downbeats*, parsed against Deep Symmetry's protocol documentation) ·
**tap tempo** · an internal clock. When no source is live, `(glow.beat)` free-runs rather
than freezing — a show shouldn't die because a DJ unplugged a CDJ.

---

## 4. Fixtures

Four archetypes, four costs:

- **Wash / PARs** — trivial. RGB(W/A/UV) + dimmer + strobe.
- **Moving heads** (*lyres*) — the expressive case. 16-bit pan/tilt, and with `POS`/`ROT` in
  the patch the aim engine computes pan/tilt from a target point. **PFX2 named ranges** make
  colour wheels, gobos, prisms, and strobe modes addressable by name.
- **Smoke / haze** — few channels, but with timing semantics (warm-up, duty cycle).
- **RGB matrices** — the per-pixel path. Wiring (serpentine/progressive), colour order, and
  multi-universe spans are all config. ~170 RGB pixels per universe.

### Importing real fixtures

Don't hand-write profiles from a PDF. The browser provisioner imports **QLC+ (`.qxf`)**,
**Open Fixture Library (JSON)**, and **GDTF (`.gdtf`)** — mapping their channels *and their
named ranges* onto PFX2. Pick the mode, review the channel table, edit if needed, save.

---

## 5. Hardware

- **MCU**: ESP32-S3 (dual-core, hardware FPU, PSRAM — the Lua VM lives there).
- **DMX out**: RS-485 transceiver (e.g. MAX3485). Default GPIOs: TX 17, RX 18, DE/RTS 8.
- **Matrices**: Art-Net over WiFi → an Art-Net→DMX bridge.
- **Inputs**: MIDI (DIN/UART, with MIDI OUT for LED feedback — see `.mdef`,
  FORMAT.md), OSC (UDP), a web console (HTTP + WebSocket) served from the
  device, DJ-Link, and USB-MIDI host (`usb_midi_input.cpp`, opt-in via
  `CONFIG_GLOW_USB_MIDI_HOST`, off by default — see README_LIVE_CONTROL.md's
  "Out of Scope"). USB-MIDI host needs a board respin for VBUS (the ESP32
  must supply 5V to the controller); a USB-host-to-DIN adapter works today
  with no hardware change if you'd rather skip that.
- **Status LED**: GPIO 2.

Render task pinned to core 1; WiFi/lwIP on core 0, so network work never jitters DMX timing.

---

## 6. Build & flash

**ESP-IDF v5.1+.** Vendored: **Lua 5.4.6** (`luaconf.h` **must** have `#define LUA_32BITS 1`
— float/int32 to match the S3's single-precision FPU; it cannot be set with `-D`) and
**Fennel 1.6.1** as a generated single `fennel.lua` (regenerate via
`scripts/vendor_fennel.sh` — the repo ships no prebuilt file). `esp_dmx` and LittleFS come
from the IDF Component Manager.

**From the browser (easiest):** open the provisioner, plug in over USB, hit **Flash** — it
writes bootloader, partition table, app, and your compiled patch. Chrome/Edge (Web Serial).

**From the CLI:**
```sh
. $IDF_PATH/export.sh && cd firmware
idf.py set-target esp32s3
idf.py menuconfig                    # DMX GPIOs, LED, WiFi, Art-Net bridge IP
./scripts/build_sample_bundle.sh     # samples/demo.show → SHW1 bundle
idf.py -p /dev/ttyUSB0 flash monitor
```
The firmware is **not one binary**: bootloader (`0x0` — on the S3, not 0x1000), partition
table (`0x8000`), otadata (`0x10000`), app (`0x20000`), and the show bundle. `flasher_args.json`
carries the exact mapping; from `firmware/build/`: `esptool.py write_flash @flash_args`.
On a fresh or wedged board, `erase_flash` first.

---

## 7. Patch language

```
# fixture type — .fdef
FIXTURE Moving Head
FOOTPRINT 16
HEAD
PANRANGE 540
TILTRANGE 270
CAP Dimmer 0
CAP Pan 1 2                # 16-bit: coarse, fine
CAP Tilt 3 4
CAP ColorWheel 5
  SLOT 0 9    open
  SLOT 10 19  red
  SLOT 20 29  blue
CAP ShutterStrobe 6
  SLOT 0 31   closed
  SLOT 32 63  open
  RANGE 64 95 strobe       # continuous sub-range
```
```
# patch — .show (SHOW 2: universes and DMX addresses are 1-indexed)
SHOW 2
UNIVERSE 1 DMX
UNIVERSE 2 ARTNET
FIXTURE samples/head.fdef 1 22
POS 2.0 1.0 0.0            # metres — this is what enables aim-at-a-point
ROT 0 0 0
MATRIX 2 1 16 8 SERP H RGB
```
A `CAP` with no `SLOT`/`RANGE` lines stays linear — every v1 profile still works.

---

## 8. Development & testing

```sh
make test    # 25 suites, -Wall -Wextra -Werror + ASan/UBSan (TSan on the queue suite)
```
Host-tested: the whole engine, the Lua layer (effect emission, error policy, the
infinite-loop hook, the memory cap, a **zero-allocation check**), the PLL beat clock (jitter
rejection, monotonicity), the binary formats (fuzzed against malformed input), and the
importers.

**Hardware-in-the-loop** (`tests/hil/`) covers what the host cannot: DMX timing, Art-Net on
the wire, WS/OSC/MIDI round-trips, the Fennel REPL end to end, every scripting failure mode,
OTA/rollback, and a **10-minute soak**. Two things stay human: whether a head physically aims
right, and whether the colours look right.

---

## 9. Status & next steps

**Done, merged, host-green:** the engine · aim geometry · cues with fades and HTP/LTP
blending · pixel matrices · PFX2 named ranges · the Lua/Fennel live-coding layer with its
real-time guards · the concurrency-safe input dispatch · firmware F0–F5 (DMX, WiFi/Art-Net,
show load, WebSocket + MIDI + OSC transports, OTA with rollback, watchdog, safe blackout) ·
musical time (PLL beat clock, MIDI clock, DJ-Link, tap tempo) · the web console with the
Fennel REPL · the browser provisioner, fixture importers, and USB flasher · the HIL suite.

**Not done: it has never run on hardware.** That is the next and only real step.

The bring-up ladder (see `BENCH_RUNBOOK.md`): boot → DMX out (**the milestone — a real
fixture responding to the engine**) → WiFi/Art-Net → web console → live-coding → HIL → the
**soak**. The soak is the one unvalidated risk in the whole stack: whether the Lua GC, paced
in the frame slack, ever drops a DMX frame under load. No host test can answer that.

**Later:** semantic colour mapping (`:red` → nearest wheel slot *or* RGB — profile v3, the
format byte is already reserved) · gamma correction and audio-reactive matrix patterns ·
matrices inside the cue/blend engine · Ableton Link.

---

## 10. Repository layout

```
*.cpp / *.h              engine, Lua layer, provisioning, live control (host-buildable)
test_*.cpp               host unit tests            (make test)
samples/                 example .fdef / .mdef / .show
scripts/                 vendor_fennel.sh, vendor_lua.sh, build_sample_bundle.sh
tests/hil/               hardware-in-the-loop suite
firmware/                ESP-IDF application
  main/                  main.cpp, render_task, wifi/storage/ota managers
  components/glow_core/  the engine as an IDF component
  components/lua/        Lua 5.4.6 (LUA_32BITS) + embedded fennel.lua
web/                     console (Preact + Fennel REPL) · provisioner (WASM, importers,
                         Web Serial flasher) · shared editor
```
