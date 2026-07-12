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
  partitions.csv                 nvs / ota_0 / ota_1 / show / coredump
  sdkconfig.defaults             octal PSRAM, -fno-exceptions/-fno-rtti, WiFi, lwIP
  main/
    CMakeLists.txt               app_main + per-phase modules
    main.cpp                     app_main() — grown phase by phase
    led_status.{h,cpp}           status LED blinker (the one feedback pre-serial)
    console/                     Preact web console (F4), embedded via EMBED_FILES
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
16 MB boards, enlarge `ota_0`/`ota_1`/`show` in `partitions.csv`.

You can also flash from a browser with no local toolchain: the deployed
provisioner page (`web/provisioner-static/`) includes a USB flasher built on
`esptool-js` + Web Serial — see "Web flasher" below.

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

### Phase F3 — Load the show from storage  (GLM read + Haiku-style patch glue)

At boot, read the raw `show` data partition (see `partitions.csv`) with
`esp_partition_read` and call the host-tested `loadShow`. Then iterate the
`LoadedShow` and configure the running `Show` via the host-tested
`applyLoadedShow` — `patch`/`patchHead` each fixture, register matrices (as
Raw universes), and map each universe's `transport` to the right sink
(Dmx -> DmxSink, ArtNet -> ArtNetSink). Now the show is data-driven, not
hardcoded.

The `show` partition is raw, not a filesystem: the SHW1 bundle is a single
opaque blob, so a filesystem buys nothing — and it lets the browser-based web
flasher write a freshly-compiled bundle directly at the partition's flash
offset with no filesystem-image step on either side.

**Split (per the plan):** partition read + bundle parse = GLM (init is
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
- `firmware/main/storage_manager.{h,cpp}` — raw `show` partition read +
  `loadShow` call.
- `firmware/main/main.cpp` — F3: `setup_show_from_bundle()` reads the bundle,
  calls `applyLoadedShow` with a `DeviceSinkFactory`, builds `PixelMatrix`
  objects from `ls.matrices`. Falls back to the hardcoded patch if no bundle.
- `firmware/main/data/show.shw1` — a pre-built demo bundle (2 dimmers + 1
  moving head on DMX u0, 1 dimmer on Art-Net u1, 16x8 matrix on Art-Net u2-3).
- `samples/{demo.show,dimmer.fdef,head.fdef}` — source for the demo bundle.
- `scripts/build_sample_bundle.sh` — rebuilds the bundle with the provision
  compiler.
- `firmware/main/CMakeLists.txt` — `esptool_py_flash_to_partition()` writes
  `data/show.shw1` straight into the `show` partition at build time; the
  console placeholder is embedded into the app binary via `EMBED_FILES`.

**You observe:**
- Serial: `show loaded from partition 'show': 4 universes, 4 fixtures, 1
  matrices`, then `applied: 4 universes configured, 4 fixtures (1 heads), 1
  matrix universes`.
- Reflashing the `show` partition (idf.py, or the browser web flasher — see
  below) and rebooting changes the patch with no firmware code change.
- DMX fixtures respond per the bundle; the matrix lights per its MatrixMap.
- Status LED: double-pulse (WiFi) + fast blink (render).

---

## Web flasher (USB, no local toolchain)

`web/provisioner-static/flash.js` flashes a blank ESP32-S3 straight from the
browser, using [`esptool-js`](https://github.com/espressif/esptool-js) over
the Web Serial API — not ESP Web Tools, because ESP Web Tools needs one
merged binary, and a merged image is static: it can't carry a bundle the user
just compiled in the same page. `esptool-js` flashes an arbitrary list of
`{data, address}` parts instead, so the page can append the freshly-compiled
`SHW1` bytes at the `show` partition's offset alongside the CI-built
bootloader/partition-table/otadata/app parts.

Requirements, surfaced in the UI:
- **Chromium only** (Chrome/Edge 89+ desktop, or Chrome on Android). Web
  Serial doesn't exist in Safari or on iOS.
- **Secure context** (HTTPS or localhost) — GitHub Pages satisfies this.
- A USB **data** cable and a USB-UART bridge (CP210x/CH340) or native USB.

Offsets are never hardcoded in JS: `firmware/flasher_args.json`, emitted by
the ESP-IDF build, is the source of truth for every part's flash address,
including the show partition. `.github/workflows/firmware.yml` uploads it
(plus the bootloader, partition table, otadata, app binary, and the demo
`.shw1`) as the `firmware-esp32s3` artifact; `.github/workflows/provisioner.yml`
downloads the latest one and publishes it to `web/provisioner-static/firmware/`
on the same GitHub Pages deploy as the provisioner, so the page can `fetch()`
it same-origin.

The existing "Download compiled .shw1" button still works — command-line
flashing, and Safari/Firefox users, aren't going away.

---
