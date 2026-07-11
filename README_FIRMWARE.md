# esp-glow Firmware (ESP32-S3)

This is the ESP-IDF application that runs the host-tested `esp-glow` core on an
ESP32-S3 board and drives real DMX / Art-Net fixtures, pixel matrices, and
web/MIDI/OSC inputs.

The host-tested core (`make test` at the repo root) is unchanged: every pure
piece of firmware logic is extracted into a host-tested helper so the green
`make test` gate keeps meaning something even after the firmware exists.

> **Validation is on hardware.** No agent can close the hardware loop for you.
> Each phase below states what you should observe on the board to call it done.

## Project layout

```
firmware/
  CMakeLists.txt                 top-level ESP-IDF project
  partitions.csv                 nvs / ota_0 / ota_1 / littlefs / coredump
  sdkconfig.defaults             octal PSRAM, -fno-exceptions/-fno-rtti, WiFi, lwIP
  main/
    CMakeLists.txt               app_main + per-phase modules
    main.cpp                     app_main() — grown phase by phase
    led_status.{h,cpp}           status LED blinker (the one feedback pre-serial)
    console/                     Preact web console (F4), served from LittleFS
  components/
    glow_core/CMakeLists.txt     wraps the repo-root runtime modules as a component
```

## Build & flash (after every phase)

```bash
cd firmware
idf.py set-target esp32s3      # first time only
idf.py build
idf.py flash monitor
```

`idf.py menuconfig` lets you override the status LED GPIO and board-specific
options. The defaults target an ESP32-S3 with 8 MB flash + octal PSRAM; for
16 MB boards, enlarge `ota_0`/`ota_1`/`littlefs` in `partitions.csv`.

## Phases

### Phase F0 — Project scaffold + bring-up  (GLM)

Stand up the ESP-IDF project: `CMakeLists.txt`, partition table, `sdkconfig`
defaults (PSRAM, C++ flags, WebSocket support), the runtime modules as a
component. Get it building, flashing, and printing over serial. Blink an LED.

Files added:
- `firmware/CMakeLists.txt`, `firmware/partitions.csv`, `firmware/sdkconfig.defaults`
- `firmware/main/{CMakeLists.txt,main.cpp,led_status.h,led_status.cpp}`
- `firmware/components/glow_core/CMakeLists.txt` (existing runtime modules only)

**You observe:** `idf.py flash monitor` prints the esp-glow banner (chip rev,
flash size, PSRAM enabled, GPIO); the status LED blinks slowly at ~1 Hz.

If you see that, the toolchain, PSRAM, partition table and `-fno-exceptions
-fno-rtti` C++ flags are all good, and every later phase has a foundation.

---

### Phase F1 — DMX output path  (GLM)  — highest-value bring-up

Fill `dmx_sink.cpp` against `esp_dmx` (UART + RS485 DE GPIO via RTS pin);
implement `send()` to write a 512-byte universe and transmit. Build the
**render task**: a FreeRTOS task pinned to core 1, running at ~44 Hz, calling
`show.renderFrame(t)` with a real monotonic clock (`esp_timer_get_time`),
flushing universe 0 to the DMX sink. Patch one fixture and run a hardcoded
`DimmerEffect` at 50%.

**Extracted testable:** the render-loop timing (tick->seconds, frame pacing,
drift correction) lives in `render_pacing.{h,cpp}` as a pure function
`glow::paceNextFrame()`, covered by `test_render_pacing.cpp` under `make test`.
The device render task is now a thin shell around it — it just reads
`esp_timer_get_time()`, calls `paceNextFrame()`, calls `show.renderFrame()`,
and `vTaskDelay()`s the returned sleep. The drift rule: a frame that overruns
does NOT try to catch up; it rebases the next deadline off `now` so a single
slow frame cannot cascade into a burst of zero-sleep frames.

Files added/changed:
- `render_pacing.h` / `render_pacing.cpp` — host-tested pacing math
- `test_render_pacing.cpp` — 9 tests (steady state, drift, u64 wrap, zero period)
- `dmx_sink.h` / `dmx_sink.cpp` — filled: `begin()` installs the driver, `send()`
  writes slot 0 (start code 0x00) + the universe, pads to 512, `dmx_send()` +
  `dmx_wait_send()`.
- `firmware/main/render_task.{h,cpp}` — FreeRTOS task pinned to core 1, uses
  `glow::paceNextFrame`, logs per-5s frame/behind stats.
- `firmware/main/main.cpp` — F1: hardcoded 1-channel dimmer fixture on u0/base0,
  50% `DimmerEffect`, render task at 44 Hz.
- `firmware/main/idf_component.yml` — depends on `espressif/esp_dmx`.
- `Makefile` — `test_render_pacing` target wired into `make test`.

**You observe:**
- A real fixture patched at base 0 holds at ~50% intensity (slot 1 == 128).
- Or a DMX tester / logic analyzer shows slot 1 == 128 at ~44 Hz with correct
  break/MAB timing on the DMX line.
- Serial: `render loop started on core 1` and per-5s `stats: N frames, M behind`.
- Status LED: fast blink (render loop running).

This proves the entire engine -> DMX chain on hardware. Do this before any
networked phase so DMX timing bugs are not hidden by WiFi work.

---

### Phase F2 — WiFi + Art-Net output (matrices)  (GLM)

Bring up WiFi (STA with auto-reconnect and bounded backoff). Fill
`artnet_sink.cpp`: a connected UDP socket to the bridge IP:6454, real Art-Net
DMX packets with per-sink sequence numbering (wraps at 255 per spec), even-
length padding, and a 5 ms `SO_SNDTIMEO` so a slow bridge cannot stall the
core-1 render loop. One `ArtNetSink` instance serves multiple universes —
`send()` stamps the universe index into each packet.

Drive `pixel_matrix`: a 16x8 RGB matrix on universes 1+2 (Raw mode). The
render task gained a `pre_render` hook that, each frame, renders the
`RainbowScrollPattern` into the canvas and `writeRawUniverse`s each matrix
universe into the Show before `renderFrame` flushes them via Art-Net.

**Coexistence** is the load-bearing detail here: WiFi/lwIP run on core 0
(`CONFIG_ESP_WIFI_TASK_CORE_AFFINITY_0` in sdkconfig), the render task is
pinned to core 1, and the Art-Net send is non-blocking with a short timeout.
DMX timing on core 1 is never preempted by network work.

Files added/changed:
- `artnet_sink.h` / `artnet_sink.cpp` — filled: connected UDP socket, sequence
  numbering, even-length padding, broadcast support, 5 ms send timeout.
- `firmware/main/wifi_manager.{h,cpp}` — STA bring-up + reconnect task with
  bounded backoff (1s -> 15s).
- `firmware/main/render_task.h` — added `pre_render` hook + `pre_render_ctx`.
- `firmware/main/main.cpp` — F2: NVS init, WiFi STA, ArtNetSink for the matrix
  universes, 16x8 RainbowScrollPattern, render task with the pre_render hook.
- `firmware/main/Kconfig.projbuild` — menuconfig for GPIO/WiFi/bridge-IP.

**You observe:**
- Serial: `got ip: ...` then `Art-Net -> ...:6454 (ready)`.
- Your existing Art-Net bridge lights the 16x8 matrix with a scrolling rainbow.
- Wireshark: Art-Net OpDmx for universes 1 and 2 at ~44 Hz (sequence numbers
  incrementing); DMX on universe 0 still holds the F1 dimmer at 50%.
- Status LED: double-pulse when WiFi is up, fast blink while connecting.

---

### Phase F3 — Load the show from storage  (GLM mount + Haiku-style patch glue)

Mount LittleFS. At boot, read `/littlefs/show.shw1` and call the host-tested
`loadShow`. Then iterate the `LoadedShow` and configure the running `Show` via
the host-tested `applyLoadedShow` — `patch`/`patchHead` each fixture, register
matrices (as Raw universes), and map each universe's `transport` to the right
sink (Dmx -> DmxSink, ArtNet -> ArtNetSink). Now the show is data-driven, not
hardcoded.

**Split (per the plan):** filesystem mount + bundle read = GLM (init is
finicky). The "iterate LoadedShow -> Show::patch + sink routing" is a
mechanical transcription against `loadShow`'s already-tested output ->
Haiku-style, with F0-F2 as the working example. It is extracted into
`apply_loaded_show.{h,cpp}` with a host test using MockSinks, so even the
"glue" half keeps a `make test` gate.

**Extracted testable:** `applyLoadedShow(LoadedShow&, Show&, ISinkFactory&)` —
a pure function that decides each universe's mode (Raw for matrix universes,
Fixture otherwise), routes sinks via a factory, and patches every fixture
including moving-head geometry. `test_apply_loaded_show.cpp` covers 6 cases
including a large matrix spanning multiple universes, unsupported transports,
and out-of-range fixtures. ASAN caught a dangling-pointer bug in the test's
MockSinkFactory (a `std::vector` reallocation) — exactly why we test.

Files added/changed:
- `apply_loaded_show.h` / `apply_loaded_show.cpp` — host-tested patch routing.
- `test_apply_loaded_show.cpp` — 6 tests, wired into `make test`.
- `firmware/main/storage_manager.{h,cpp}` — LittleFS mount + SHW1 read +
  `loadShow` call.
- `firmware/main/main.cpp` — F3: `setup_show_from_bundle()` reads the bundle,
  calls `applyLoadedShow` with a `DeviceSinkFactory`, builds `PixelMatrix`
  objects from `ls.matrices`. Falls back to the hardcoded patch if no bundle.
- `firmware/main/data/show.shw1` — a pre-built demo bundle (2 dimmers + 1
  moving head on DMX u0, 1 dimmer on Art-Net u1, 16x8 matrix on Art-Net u2-3).
- `samples/{demo.show,dimmer.fdef,head.fdef}` — source for the demo bundle.
- `scripts/build_sample_bundle.sh` — rebuilds the bundle with the provision
  compiler.
- `firmware/main/CMakeLists.txt` — `littlefs_create_partition_image()` packs
  `data/` (bundle + console) into the LittleFS partition at flash time.

**You observe:**
- Serial: `LittleFS mounted ...`, `read /littlefs/show.shw1 (295 bytes)`,
  `show loaded: 4 universes, 4 fixtures, 1 matrices`, then
  `applied: 4 universes configured, 4 fixtures (1 heads), 1 matrix universes`.
- Swapping the bundle file on LittleFS and rebooting changes the patch with no
  code change / no reflash of the firmware.
- DMX fixtures respond per the bundle; the matrix lights per its MatrixMap.
- Status LED: double-pulse (WiFi) + fast blink (render).

---

### Phase F4 — Inputs: web + MIDI + OSC  (GLM)

Fill the three input scaffolds. The parsers underneath are already
host-tested, so the risk is confined to the transport wiring:

- **`web_input.cpp`** — `esp_http_server` with a WebSocket endpoint at `/ws`
  feeding the host-tested `web_input_handle_text_frame` -> `LiveControl`.
  Serves the Preact console bundle from LittleFS (`/littlefs/console/`).
  Sends the `config` snapshot on connect; a state broadcaster pushes
  `{"type":"state","active":[...]}` every 500ms.
- **`midi_input.cpp`** — DIN-MIDI over UART (31250 baud) -> `parseMidi`
  (host-tested) -> `LiveControl::handle`. Native USB-MIDI via TinyUSB is
  stubbed via `midi_input_feed_bytes()` (the descriptor is board-specific).
- **`osc_input.cpp`** — UDP socket -> `parseOsc` (host-tested) ->
  `LiveControl::handle`.

**Host-tested helpers added (the plan's "already-tested" parsers, created
here because they did not yet exist):**
- `midi_parser.{h,cpp}` + `test_midi_parser.cpp` — 11 tests: running status,
  real-time interleaving, SysEx abort, variable data-byte counts.
- `osc_parser.{h,cpp}` + `test_osc_parser.cpp` — 8 tests: float/int args,
  bundles, blob/string skip, malformed rejection, 4-byte alignment.
- `web_input_handler.{h,cpp}` + `test_web_input_handler.cpp` — 10 tests:
  go/release/scene/button frames, malformed rejection, MIDI/OSC paths through
  LiveControl (verified end-to-end against a MockSink), config JSON build.
- `live_control.{h,cpp}` — input->ShowController binding layer (MIDI note/CC,
  OSC address, web button -> cue go/release).
- `web_protocol.h` — JSON message-shape constants.

**Device transports** (`midi_input`, `osc_input`, `web_input`) are
`#ifdef ESP_PLATFORM`-guarded and added to the `glow_core` component.

Files added/changed:
- Host-tested: `midi_parser.{h,cpp}`, `osc_parser.{h,cpp}`,
  `web_input_handler.{h,cpp}`, `live_control.{h,cpp}`, `web_protocol.h`,
  + 3 test files wired into `make test`.
- Device-only: `midi_input.{h,cpp}`, `osc_input.{h,cpp}`, `web_input.{h,cpp}`.
- `firmware/main/data/console/index.html` — minimal Preact-free console
  (plain DOM, < 3 KB) that connects to /ws, renders cue buttons, sends
  go/release on tap.
- `firmware/main/main.cpp` — F4: ShowController + 2 demo cues (blackout/full),
  LiveControl with MIDI/OSC/web bindings, starts all three input transports,
  state broadcaster task.
- `firmware/main/Kconfig.projbuild` — MIDI UART/pins, OSC port, web port.

**You observe:**
- Browser loads `http://<ip>/`; cue buttons render (config frame). Tapping
  "Full" lights the patched fixtures; releasing dims them back.
- MIDI pad: Note On (ch0, note 60) triggers "Full"; Note Off releases it.
- OSC app: `/esp-glow/full` with a float triggers cue 1.
- Serial: each transport logs its bring-up line.

---
