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
  partitions.csv                 nvs / ota_0 / ota_1 / show / coredump / scripts / devcfg
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
options -- but these are now DEFAULTS, not the running truth: a `CFG1` blob in
the raw `devcfg` partition (written by the browser flasher or the device
console's reconfigure page, see FORMAT.md and "Web flasher" below) overrides
WiFi SSID/password, DMX/LED GPIOs, the Art-Net fallback destination, and
whether USB-MIDI host is enabled, at boot. `menuconfig`'s values only take
effect when `devcfg` is absent or fails to parse. The defaults target an
ESP32-S3 with 8 MB flash + octal PSRAM; for 16 MB boards, enlarge
`ota_0`/`ota_1`/`show` in `partitions.csv` (leave `devcfg` at the end of the
table so existing boards' offsets don't move).

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
  objects from `ls.matrices`. F5 replaced the original hardcoded-patch
  fallback with a safe blackout — see below.
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

### Phase F5 — OTA, watchdog, WiFi reconnect, safe blackout  (GLM/Sonnet)

Hardens the firmware for real-world use: update it without a cable, survive
network loss, survive a hung task, and never end up as a black rig with no
way back.

**Safe blackout** (`safe_blackout.{h,cpp}`, host-tested): the fallback state
every other F5 failure path funnels into. `safeBlackoutCore(Show&,
ShowController&)` calls the new `ShowController::stopAll()` (immediate, no
fade — unlike `release()`) and zeros every Raw-mode universe directly
(Fixture-mode universes fall back to their profile defaults on their own,
via `Show::renderFrame`'s existing per-frame zero+`applyDefaults` pass, once
no cue is active). Blackout is **not** "stop rendering" — the render loop,
`DmxSink`, and `ArtNetSink` all keep running and keep sending zeros forever,
because a bridge or fixture that stops receiving frames often holds its last
value instead of going dark. `glow_safe_blackout()` (main.cpp) wraps the
host-tested core with serial (`ESP_LOGE`) + WebSocket (`buildBlackoutJson`,
re-announced once/sec so a client connecting later still sees it) reporting.
Triggered by: a missing/corrupt SHW1 bundle, a scripts partition that won't
mount, a Lua/Fennel VM or `boot.fnl` failure, and an OTA image that fails
self-validation. The hardcoded no-bundle demo patch (F1/F2) is gone — a rig
with no valid show goes dark and says why, instead of lighting an unrequested
rainbow. Blackout does not lock out manual control: an operator can still
`go()` a cue from the console/MIDI/OSC/REPL afterward.

**WiFi reconnect** (`firmware/main/wifi_manager.{h,cpp}`): exponential
backoff on `WIFI_EVENT_STA_DISCONNECTED`, now capped at 30s (was 15s), with
`GLOW-TEST: wifi state=<connected|retrying> attempts=<n>` telemetry. DMX is
local and the render task is pinned to core 1 with WiFi/lwIP confined to
core 0, so a dropped AP structurally cannot stall rendering or drop DMX
frames. After `kApFallbackThreshold` (10) consecutive failed reconnects, an
optional SoftAP (`WifiStaConfig::ap_fallback`, mode APSTA) comes up
alongside the still-retrying STA link so the console stays reachable even
when the venue's WiFi is gone — **flagged, not HIL-verified**: SoftAP + DMX
timing coexistence needs a real soak test before being trusted at a gig.

**Task watchdog**: the render task subscribes itself
(`esp_task_wdt_add`, `render_task.cpp`) and is fed once per frame from
`render_tick_hooks`' post phase (`esp_task_wdt_reset`, `main.cpp`) — a
runaway native effect or driver stall reboots the board instead of leaving a
frozen rig. This is a different backstop from the Lua instruction-count hook
(`lua_vm.h`), which bounds *scripted* runtime and disables the offending
effect without ever reaching the watchdog. Transport tasks (MIDI/OSC/httpd)
are deliberately not subscribed — a blocked socket is not a fatal condition.
`CONFIG_ESP_TASK_WDT_PANIC=y` (sdkconfig.defaults) is what actually turns a
timeout into a reboot rather than a log line.

**OTA** (`ota_manager.{h,cpp}`): `POST /ota` on the *existing*
`esp_http_server` (no second server), streaming into `esp_ota_write` ->
`esp_ota_end` (image validity boundary) -> `esp_ota_set_boot_partition` ->
reboot. Refuses with 409 while any cue is active
(`ShowController::anyActive()`, also host-tested). Progress/result reported
over the WS console (`buildOtaStatusJson`). On first boot of a new slot
(`esp_ota_get_state_partition` == `ESP_OTA_IMG_PENDING_VERIFY`), the image
must self-validate within 20s — concretely: WiFi has come up, the render
loop has produced 100 real frames, and DMX bring-up succeeded — before
`esp_ota_mark_app_valid_cancel_rollback()` cancels the pending rollback. An
image that boots but never clears those criteria calls
`glow_safe_blackout()` (reported before the reboot) then
`esp_ota_mark_app_invalid_rollback_and_reboot()`, landing back on the
previous slot. A image that panics outright is handled for free by
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` alone (bootloader-level, no app
code involved) — `ota_manager` exists specifically for the harder case: an
image that boots fine but doesn't actually work.

**Storage robustness**: `loadShow` was already strict (magic/version/length,
never reads out of bounds); F5's change is that a failure now surfaces as a
reported safe blackout instead of a silent hardcoded fallback. A scripts
partition mount failure is likewise now a reported blackout (not just a
warning) — without it, a bundle's matrix pattern would keep running
un-configurable, independent of Lua, with nobody ever having gotten the
chance to quiet it. `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y` +
`CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y` land a panic's backtrace in the
`coredump` partition, pullable with `espcoredump.py`.

**Out of scope** (flagged, not implemented): signed/encrypted OTA images
(flash encryption + secure boot — a real concern for a commercial product,
a separate hardening pass); OTA of the SHW1 bundle or the scripts partition
(they have their own paths — browser flash and `script_save` respectively);
delta/compressed OTA.

**You observe:**
- Pull the AP: DMX keeps rendering, serial shows `GLOW-TEST: wifi
  state=retrying attempts=N` with growing backoff, the console reconnects
  once the AP returns.
- Push an OTA: the new slot boots, self-validates within ~20s
  (`OTA self-validated...; rollback cancelled` on serial + WS), and stays up.
  A deliberately broken image (panics in `app_main`, or one that boots but
  never brings up WiFi/DMX/a frame) rolls back automatically; the device
  comes up on the old slot, still working.
- Hang the render task artificially: the board reboots within
  `CONFIG_ESP_TASK_WDT_TIMEOUT_S` (10s).
- Corrupt the `show` partition: serial + WS show `SAFE BLACKOUT: show
  partition: missing or corrupt SHW1 bundle`, DMX/Art-Net keep streaming
  zeros, no crash loop.

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

**CFG1 device config (Wave 2):** the flash modal also includes a device
config form (WiFi SSID/password or "no WiFi", DMX/status-LED GPIOs, Art-Net
fallback destination, USB-MIDI on/off with a VBUS-hardware warning). It's
encoded in the browser (`web/shared/devcfg.js`, no WASM needed -- CFG1 is a
plain fixed-field struct, not a compiled text grammar) and written at the
`devcfg` partition's offset, resolved from the real `partition-table.bin`
the same way the `scripts` partition's offset already is. Form values
persist to `localStorage` so reflashing a board isn't retyping the WiFi
password every time. See FORMAT.md's "CFG1 Device Config Format" section
for the wire format, and `device_config_web.h`/the device console's
`/devcfg.html` page for reconfiguring an already-flashed board without a
cable.

---
