# esp-glow - it's like wled dated afterglow

A live-codable DMX lighting engine for the ESP32-S3 — an Afterglow-style show system with
no JVM, running standalone on a microcontroller. Native C++ does the hard real-time work;
your show is written in **Fennel** (a Lisp), compiled **on the device**, and hot-swapped
while the rig is running.

Point a moving head at a spot in the room, scroll a plasma across an LED matrix, define a
cue with a two-second fade — then change any of it from a REPL, mid-song, without
reflashing.

---

## 1. Architecture

```
Fennel         — Lisp surface. Self-hosted compiler runs ON the device.
    ↓
Lua VM         — effects, cues, scenes, config. Composes and parameterises.
    ↓
C++ / FreeRTOS — render loop @44 Hz, DMX + Art-Net out, pixel engine, aim geometry.
                 Hard real-time. Never blocked by scripts.
```

**The load-bearing rule: Lua composes, C runs the tight loops.** Scripts choose *what*
happens and with *which parameters*; the C engine executes the per-pixel and per-channel
work. This is the same lesson Pixelblaze learned — a general-purpose language cannot drive
15,000 pixel updates a second on an MCU, but it is perfect for describing a show.

The engine turns *time* into *DMX frames*. Effects (Lua or built-in C) emit abstract
intents — "this fixture is blue at 80%", "this head points at that spot" — a resolve layer
maps them to channel values via per-fixture profiles, and cues blend overlapping intents
(HTP for intensity, LTP for position) before the result is flushed to the wire.

Two data paths, by design:

- **Per-fixture** — moving heads, PARs, wash, smoke. One resolved value per capability.
  Heads add pan/tilt inverse kinematics: aim at a point, not an angle.
- **Per-pixel** — RGB matrices. A 2D canvas rendered by a C pattern engine, packed into
  DMX universes and sent over Art-Net to a bridge.

### The two halves of a show

| | **Patch** | **Show** |
|---|---|---|
| What | Which fixtures exist, their addresses, geometry, matrices | What the fixtures *do* over time |
| Written in | `.fdef` / `.show` text → compiled to a binary `SHW1` bundle | **Fennel** |
| Changes when | You change the rig | Every song |
| Lives in | A raw flash partition (browser-flashable) | LittleFS (writable, hot-swappable) |

Scripts never define the patch; the patch never defines the show.

---

## 2. Scripting: Fennel on the device

### A first effect

An effect is a function of time that **emits** intents. It does not return them —
returning tables would allocate on every frame and feed the garbage collector.

```fennel
(fn breathe [t]
  (glow.set 1 :dimmer (* 0.5 (+ 1 (math.sin (* t 2)))))
  (glow.set 1 :red 1.0))

(glow.cue.define :warm-breath {:effects [breathe] :fade-in 2.0 :fade-out 1.0})
(glow.cue.go :warm-breath)
```

Send that over the WebSocket REPL and the light responds immediately. No reflash.

### The `glow.*` API

```fennel
;; Emit (valid only inside an effect callback)
(glow.set fixture-id :dimmer 0.8)        ; capability → normalized [0,1]
(glow.aim  head-id [0 2 5])              ; point a moving head at a world point

;; Built-in C effects — fast, use them freely
(glow.fx.hue-rotate [1 2 3] {:period 4.0})
(glow.fx.chase      [1 2 3 4] {:period 2.0})
(glow.fx.sweep      5 [0 0 1] [1 0 1] {:period 6.0})

;; Cues and scenes (wrap the C++ ShowController: fades, HTP/LTP blending)
(glow.cue.define :verse {:effects [breathe] :fade-in 2.0 :priority 0})
(glow.cue.go :verse)
(glow.cue.release :verse)
(glow.scene.define :chorus [:verse :strobe-hit])

;; Matrices — Lua picks the pattern, C runs the per-pixel loop
(glow.matrix.pattern 0 :plasma {:speed 0.5 :scale 0.2})
(glow.matrix.brightness 0 0.8)

;; Persist — boot.fnl is evaluated at startup, so a show survives reboot
(glow.save :boot "(glow.cue.go :verse)")
```

Capabilities: `:dimmer :red :green :blue :white :amber :uv :cyan :magenta :yellow
:pan :tilt :shutter-strobe :gobo :focus :zoom :fog :fan`. Emitting one a fixture doesn't
have is a harmless no-op, so effects can be written generically.

### Writing effects that don't stutter

At 44 Hz you have 22.7 ms per frame. A garbage-collector pause drops a DMX frame, and a
dropped frame is *visible on stage*. Two rules:

- **Allocate nothing in an effect body.** String *literals* (`:dimmer`) are interned at
  compile time and cost nothing. String *concatenation* (`(.. "dim" n)`), table
  construction, and closures created per call all allocate. Build them once, outside.
- **Don't write per-pixel loops in Lua.** Use `glow.matrix.pattern`. If you need a matrix
  behaviour that doesn't exist, add a C pattern — that's the right layer.

### Safety: the show goes on

Live-coding means you *will* ship a bug mid-set. The engine is built so a bad script cannot
take the rig down:

- A **syntax error** → reported to the console; nothing changes.
- A **runtime error** in an effect → that effect is disabled and reported; **its cue keeps
  running with its other effects**; the rig keeps rendering.
- An **infinite loop** → an instruction-count hook aborts it.
- **Out of memory** → the offending script is aborted; rendering continues.
- A broken **`boot.fnl`** → the device falls back to a safe blackout and reports, rather
  than booting into a broken show.

Every script call is protected. Nothing a script does unwinds the render loop.

---

## 3. Hardware

- **MCU**: ESP32-S3 (dual-core, hardware FPU, PSRAM — the Lua VM lives in PSRAM).
- **DMX out**: RS-485 transceiver (e.g. MAX3485). Default GPIOs (in `menuconfig`):
  TX 17, RX 18, DE/RTS 8. Wire to a 3- or 5-pin XLR.
- **Matrices**: Art-Net over WiFi → an Art-Net→DMX bridge → DMX-input matrices
  (3 channels per pixel, ~170 pixels per universe).
- **Inputs**: MIDI (DIN over UART or native USB), OSC (UDP), and a web console
  (HTTP + WebSocket) served from the device. All triggers funnel into the same cue engine.
- **Status LED**: GPIO 2.

The render task is pinned to core 1; WiFi/lwIP live on core 0, so network work never
jitters DMX timing. The Lua VM is owned by the render task and touched from nowhere else —
input events and script submissions arrive via queues drained on that task.

---

## 4. Build & flash

Firmware builds with **ESP-IDF v5.1+**.

### 4.1 Vendored dependencies

- **Lua 5.4.6** at `firmware/components/lua/`. `luaconf.h` **must** have
  `#define LUA_32BITS 1` — float/int32 numbers, matching the S3's single-precision FPU
  (doubles are software-emulated, roughly 10× slower). This cannot be set with `-D`: the
  macro is already defined in the file, so edit it.
- **Fennel 1.6.1**, vendored as a single generated `fennel.lua` (~295 KB, MIT), embedded in
  the firmware. Regenerate with `scripts/vendor_fennel.sh` — it clones the tag and runs
  `make fennel.lua`, because the repo does **not** ship a prebuilt single file.
- `esp_dmx` and LittleFS are resolved by the IDF Component Manager.

### 4.2 Flash from the browser (easiest)

Open the web provisioner, plug the board in over USB, hit **Flash**. It writes the
bootloader, partition table, app, and — if you compiled a patch in the same page — your
`SHW1` bundle. Requires Chrome or Edge (Web Serial).

### 4.3 Flash from the command line

```sh
. $IDF_PATH/export.sh
cd firmware
idf.py set-target esp32s3
idf.py menuconfig          # DMX GPIOs, status LED, WiFi SSID/pass, Art-Net bridge IP
./scripts/build_sample_bundle.sh    # compiles samples/demo.show → the SHW1 bundle
idf.py -p /dev/ttyUSB0 flash monitor
```

The firmware is **not one binary**. A full flash writes bootloader (`0x0` — note: 0x0 on
the S3, not 0x1000), partition table (`0x8000`), otadata (`0x10000`), app (`0x20000`), and
the show bundle. The build emits `flasher_args.json` with the exact mapping; from
`firmware/build/`:

```sh
esptool.py -p /dev/ttyUSB0 write_flash @flash_args
```

On a fresh or wedged board, run `esptool.py -p /dev/ttyUSB0 erase_flash` first. On boot the
serial console prints a banner, `render loop started on core 1`, the DMX init result, the
loaded fixture/matrix counts, and a frame-rate stat line every 5 s.

---

## 5. The patch language (`.fdef` / `.show`)

Compiled off-device by the `provision` tool (or in the browser, via WASM) into the `SHW1`
bundle. `#` starts a comment.

### Fixture types — `.fdef`

```
FIXTURE Moving Head
FOOTPRINT 9
HEAD
PANRANGE 540
TILTRANGE 270
CAP Dimmer 0
CAP Pan    1 2            # 16-bit: coarse=1, fine=2
CAP Tilt   3 4
CAP Red    5
CAP Green  6
CAP Blue   7
CAP ShutterStrobe 8 - 8   # 8-bit, idle value 8 (shutter open)
```
`CAP <Capability> <coarse> [<fine>|-] [<default>] [inv]`.

### The patch — `.show`

```
UNIVERSE 0 DMX
UNIVERSE 1 ARTNET

FIXTURE samples/dimmer.fdef 0 1      # a PAR at universe 0, base ch 1
FIXTURE samples/head.fdef   0 21     # a head at universe 0, base ch 21
POS 2.0 1.0 0.0                       # metres — this is what enables aim-at-a-point
ROT 0 0 0

MATRIX 1 0 16 8 SERP H RGB           # 16×8 matrix, serpentine wiring, RGB order
```

Heavy fixture libraries (GDTF / QLC+ / OFL) aren't parsed here — importers that *emit*
`.fdef`/`.show` are a separate tool; these text formats are the stable seam.

---

## 6. Development & testing

```sh
make test    # every suite, -Wall -Wextra -Werror + ASan/UBSan
             # (the control-queue suite also runs under ThreadSanitizer)
```

The whole engine is host-tested — including the Lua layer, since Lua builds fine on Linux:
effect emission, cue/scene state, the error policy, the infinite-loop hook, the memory cap,
and a **zero-allocation check** (a well-written effect must make zero allocator calls per
frame).

What can't be host-tested — DMX break/MAB timing, Art-Net on the wire, GC pacing under
load, hot-swap while rendering — is covered by an automated **hardware-in-the-loop** suite
(serial telemetry + network capture + a 10-minute soak). Two things stay human: whether a
head physically aims right, and whether the colours look right.

---

## 7. Roadmap

**Done, host-tested:** the engine (profiles, aim geometry, show model, effects, cues with
fades and HTP/LTP blending, pixel matrices), provisioning (`.fdef`/`.show` → `SHW1`), the
concurrency-safe input dispatch, the web console, and a browser provisioner (WASM).

**In progress:** firmware bring-up (F0–F3: scaffold, DMX out + render task, WiFi/Art-Net,
show-load) — the next hardware milestone is a real fixture responding to the engine. Then
inputs (F4), and the **Lua/Fennel live-coding layer** — the heart of the project.

**Next:** hardware-in-the-loop suite · OTA + robustness (F5) · browser firmware flashing.

**Later:** GDTF/QLC+ importers · gamma correction and audio-reactive matrix patterns ·
beat/clock sync (MIDI clock, DJ-Link) · a dedicated expression VM if scripted per-pixel
work is ever wanted.

---

## 8. Repository layout

```
*.cpp / *.h              engine, provisioning, live-control (host-buildable)
test_*.cpp               host unit tests            (make test)
samples/                 example .fdef / .show
scripts/                 vendor_fennel.sh, build_sample_bundle.sh
firmware/                ESP-IDF application
  main/                  main.cpp, render_task, wifi/storage/ota managers
  components/glow_core/  the engine as an IDF component
  components/lua/        Lua 5.4.6 (LUA_32BITS) + embedded fennel.lua
  partitions.csv, sdkconfig.defaults
web/                     console (Preact) + provisioner/flasher (WASM + Web Serial)
```
