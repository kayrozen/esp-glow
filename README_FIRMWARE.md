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
