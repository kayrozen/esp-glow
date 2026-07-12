# esp-glow - it's like wled dated afterglow

A native C++ DMX lighting control system for the ESP32-S3 — a from-scratch
reimplementation of an Afterglow-style show engine, with no JVM, running standalone on
the microcontroller. It drives moving heads, wash/PARs, smoke machines, and RGB pixel
matrices; it is scripted with a small text patch language; and it is piloted live from
a MIDI controller, OSC, or a built-in web console.

---

## 1. What it is

The engine turns *time* into *DMX frames*. Effects and cues produce abstract intents
("this fixture is blue at 80%", "this head points at that spot"), an assigner layer
resolves them to channel values using per-fixture profiles, and the result is flushed
to the wire — local DMX-512 for fixtures, Art-Net over WiFi for matrices.

There are two data paths, by design:

- **Per-fixture path** — moving heads, PARs, wash, smoke. One resolved value per
  capability per fixture. Moving heads add pan/tilt geometry (aim a beam at a point in
  the room).
- **Per-pixel path** — RGB matrices. A 2D canvas is rendered by a pattern engine and
  packed into DMX universes (Art-Net → bridge → the matrix's DMX input).

### Module map (all host-testable under `make test`)

| Module | Responsibility |
|---|---|
| `fixture_profile` | `Capability` enum, `FixtureProfile`, `applyCapability`/`applyDefaults`, the `PFX1` binary profile format |
| `vec_math` / `aim` | 3D math + moving-head pan/tilt aim (`aimAtPoint`/`aimDirection`) |
| `show` | `Show` model, universes (Fixture/Raw), `IUniverseSink`, `renderFrame`, the intent→channel resolve |
| `effects` / `oscillator` / `color` | oscillators (sine/saw/triangle/square), HSV→RGB, effects (dimmer, hue-rotate, chase, strobe, sweep, …) |
| `show_control` | cues, scenes, fade envelopes, HTP/LTP blending (`ShowController`) |
| `pixel_matrix` | `Canvas`, pixel mapping (serpentine/order/multi-universe), patterns (solid/gradient/rainbow/plasma) |
| `provision` / `show_bundle` | `.fdef`/`.show` text → `PFX1` profiles + `SHW1` bundle; device-side loader |
| `live_control` / `control_queue` | input→cue bindings, `ControlEvent` queue, `pumpControlEvents` (concurrency-safe dispatch) |
| `firmware/` | the ESP-IDF application (DMX driver, render task, WiFi/Art-Net, raw show partition, inputs, OTA) |

---

## 2. Hardware

- **MCU**: ESP32-S3 (dual-core, hardware FPU, PSRAM for pixel buffers).
- **DMX out**: an RS-485 transceiver (e.g. MAX3485) on the DMX UART. Default GPIOs
  (set in `menuconfig`): TX = 17, RX = 18, DE/RTS = 8. Wire the transceiver to a 3- or
  5-pin XLR.
- **Matrices**: driven over **Art-Net** on WiFi to an existing Art-Net→DMX bridge; the
  matrices take DMX input (each pixel = 3 channels).
- **Inputs**: MIDI (DIN over UART, or native USB-MIDI), OSC (UDP), and a web console
  (HTTP + WebSocket served from the device).
- **Status LED**: default GPIO 2 (reflects boot / WiFi / error state).

The render task is pinned to core 1; WiFi/lwIP run on core 0 so network work never
jitters DMX timing.

---

## 3. Build & flash

Firmware lives in `firmware/` and builds with **ESP-IDF v5.1+**.

### 3.1 One-time setup

```sh
. $IDF_PATH/export.sh            # activate the ESP-IDF environment
cd firmware
idf.py set-target esp32s3
idf.py menuconfig                # Component config → esp-glow:
                                 #   DMX TX/RX/DE GPIOs, status LED GPIO,
                                 #   WiFi SSID/pass, Art-Net bridge IP
                                 #   (bridge IP 0 = LAN broadcast)
```

PSRAM (octal) and the C++ flags are already set in `sdkconfig.defaults`.

### 3.2 Build the show bundle (raw partition)

The device loads its patch from a `SHW1` bundle in the raw `show` data partition — a
single opaque blob, not a filesystem. Compile the demo patch before building the
firmware:

```sh
./scripts/build_sample_bundle.sh      # → firmware/main/data/show.shw1
```

> Note: fix the hard-coded `cd` line at the top of that script to point at your repo
> root (e.g. `cd "$(git rev-parse --show-toplevel)"`). It compiles the `provision`
> tool and runs `./provision samples/demo.show firmware/main/data/show.shw1`. CMake
> then writes `data/show.shw1` straight into the `show` partition at build time via
> `esptool_py_flash_to_partition()`.

### 3.3 Flash & monitor

```sh
idf.py -p /dev/ttyUSB0 flash monitor      # replace with your serial port
```

On boot the serial console prints a banner, `render loop started on core 1`, the DMX
init result, the loaded fixture/matrix counts, and a per-5s frame-rate stat line. Exit
the monitor with `Ctrl-]`. If a flash gets wedged: `idf.py -p <port> erase-flash` then
reflash.

No local toolchain? The deployed provisioner page can flash a blank ESP32-S3 straight
from a Chrome/Edge tab over USB (`esptool-js` + Web Serial) — including a show bundle
you just compiled in the same page. See "Web flasher" in `README_FIRMWARE.md`.

### 3.4 Partition layout

`ota_0` / `ota_1` (3 MB each, A/B OTA), `show` (1 MB raw data partition, the SHW1 show
bundle), `nvs`, and a `coredump` partition for post-mortem backtraces. Web console
assets are embedded into the app binary (`EMBED_FILES`), not stored on a filesystem.

---

## 4. The script language (`.fdef` / `.show`)

Fixtures and patches are authored in two small line-oriented text formats, compiled
off-device by the `provision` tool into the compact binary the firmware loads. `#`
starts a comment; blank lines are ignored.

### 4.1 Fixture definitions — `.fdef`

One fixture *type* per file: its DMX footprint and what each channel does.

```
FIXTURE <name...>
FOOTPRINT <n>                 # number of DMX channels this fixture occupies
HEAD                          # optional: this is a moving head
PANRANGE  <deg>               # head only, e.g. 540
TILTRANGE <deg>               # head only, e.g. 270
CAP <Capability> <coarse> [<fine>|-] [<default>] [inv]
```

`CAP` maps a capability to channel offset(s) within the footprint: `coarse` is
required; `fine` is a second offset for 16-bit channels (or `-` for 8-bit); `default`
is an idle value (e.g. shutter-open); `inv` inverts the output.

Capabilities: `Dimmer Red Green Blue White Amber Uv Cyan Magenta Yellow Pan Tilt
ShutterStrobe Gobo Focus Zoom Fog Fan Generic`.

Example — a 9-channel moving head with 16-bit pan/tilt:

```
FIXTURE Moving Head
FOOTPRINT 9
HEAD
PANRANGE 540
TILTRANGE 270
CAP Dimmer 0
CAP Pan    1 2          # 16-bit: coarse=1, fine=2
CAP Tilt   3 4
CAP Red    5
CAP Green  6
CAP Blue   7
CAP ShutterStrobe 8 - 8 # 8-bit, idle value 8 (shutter open)
```

### 4.2 Show / patch — `.show`

Where fixture *instances* live: their universe, DMX base address, and (for heads)
position/orientation; plus matrices and universe transports.

```
UNIVERSE <idx> <DMX|ARTNET|SACN>          # transport for that universe
FIXTURE  <deffile> <universe> <base>       # patch an instance
POS      <x> <y> <z>                        # head only: metres, modifies the last FIXTURE
ROT      <yaw> <pitch> <roll>              # head only: degrees
CENTER   <panNorm> <tiltNorm>              # head only, optional (default 0.5 0.5)
INVERT   <0|1> <0|1>                        # head only, optional
MATRIX   <startUniverse> <startChannel> <w> <h> <SERP|PROG> <H|V> <ORDER>
```

`ORDER` ∈ `RGB GRB BRG RBG GBR BGR`. Example:

```
UNIVERSE 0 DMX
UNIVERSE 1 ARTNET
UNIVERSE 2 ARTNET

FIXTURE samples/dimmer.fdef 0 1      # a PAR at universe 0, base ch 1
FIXTURE samples/head.fdef   0 21     # a head at universe 0, base ch 21
POS 2.0 1.0 0.0
ROT 0 0 0

MATRIX 2 0 16 8 SERP H RGB           # 16×8 matrix, universe 2, serpentine, RGB
```

### 4.3 Compile

```sh
./provision samples/demo.show firmware/main/data/show.shw1
```

`.fdef` references resolve by path from the working directory, so run from the repo
root. Heavy fixture libraries (GDTF / QLC+ / Open Fixture Library) are **not** parsed
here — importers that emit `.fdef`/`.show` are a separate tool; these text formats are
the stable seam. The output `SHW1` bundle carries the profiles, the patch table (with
each head's baked position/orientation), the matrix maps, and the per-universe
transport routing.

---

## 5. Fixtures

The engine treats four archetypes, each with a different cost and code path:

- **Wash / PARs** — trivial: RGB(W/A/UV) + dimmer + strobe, a few channels, no
  geometry. Pure per-fixture path.
- **Moving heads** — the expensive, expressive case. Pan/tilt are usually 16-bit
  (coarse+fine). With a head's `POS`/`ROT` in the patch, the aim engine computes
  pan/tilt from a target point via inverse kinematics, so cues can say "point here"
  instead of raw angles. `PANRANGE`/`TILTRANGE` and `CENTER` map the angles back to DMX.
- **Smoke / haze** — one or two channels, but with timing semantics (warm-up,
  duty-cycle); treat as a fixture with constraints, not a plain dimmer.
- **RGB pixel matrices** — the per-pixel path. A `MATRIX` line declares geometry,
  wiring (serpentine/progressive, row/column), color order, and start universe/channel.
  The pixel engine renders a pattern into a 2D canvas and packs it — component by
  component, so a pixel straddling the 512-channel boundary is still correct — into the
  Raw universes, which flush over Art-Net. Budget: ~170 RGB pixels per universe.

Under the hood every fixture is just a `FixtureProfile` (a channel map + capability
tags); effects and cues never touch raw channels.

---

## 6. Development & testing

Host-side (no hardware): the entire engine is unit-tested.

```sh
make test        # builds every suite under -Wall -Wextra -Werror + ASan/UBSan
                 # (the control-queue suite additionally runs under ThreadSanitizer)
```

Firmware behaviour that can't be host-tested (DMX timing, Art-Net on the wire, WS/OSC
round-trips, crash-safety under load) is covered by an automated **hardware-in-the-loop
(HIL)** suite that flashes the board and asserts on serial telemetry + network traffic.
Two things HIL cannot check stay human/instrumented: whether a head physically aims
right and whether colors look right.

**Hygiene guardrail** (add to your pre-push): fail on committed conflict markers or
`.rej`/`.orig` files, and require a green `make test`, before any push.

---

## 7. Roadmap

### Done — host-tested and green
- Full engine: `fixture_profile`, `aim`/`vec_math`, `show`, `effects`/`color`/
  `oscillator`, `show_control`, `pixel_matrix`.
- Provisioning: `.fdef`/`.show` compiler, `PFX1`/`SHW1` formats, device-side loader.
- Live-control core: `live_control`, `control_queue` + `pumpControlEvents` (the
  concurrency-safe input dispatch), MIDI/web parsers.
- Web console (Preact + WebSocket protocol) and a browser provisioner (WASM).

### In progress — firmware
- **F0–F3** (scaffold, DMX output + render task, WiFi + Art-Net, show-load from the raw
  `show` partition): clean, host seams green. **Next hardware step: F1 bring-up** —
  flash it and watch a real fixture respond to the engine. Do this before any
  networked work.
- **F4 (inputs)**: spec'd to build on the existing `control_queue` (transports enqueue
  `ControlEvent`s, the render task drains via `pumpControlEvents` — the *only* place the
  controller is mutated). Adds an OSC parser (host-tested), the three transport tasks,
  and the WS server serving the console.

### Next
- **HIL suite** — boot/POST, DMX loopback, Art-Net capture, WS/OSC round-trips, and a
  10-minute soak that proves the concurrency holds under load.
- **F5** — OTA (A/B partitions), watchdog, WiFi reconnect, safe-blackout on a missing
  bundle.

### Later
- GDTF / QLC+ / OFL importers that emit `.fdef`/`.show`.
- Gamma correction and audio-reactive patterns for matrices.
- Integrating matrices into the cue/blend engine (a cue that fades a matrix pattern).
- Timeline / auto-advancing cue-list playback; external clock (MIDI/DJ-Link) sync.

---

## 8. Repository layout

```
*.cpp / *.h              engine + provisioning + live-control (host-buildable)
test_*.cpp               host unit tests            (make test)
samples/                 example .fdef / .show      (+ build_sample_bundle.sh)
firmware/                ESP-IDF application
  main/                  main.cpp, render_task, wifi/storage/ota managers, data/
  components/glow_core/  the engine, wrapped as an IDF component
  partitions.csv, sdkconfig.defaults
web/                     Preact console + browser provisioner
```
