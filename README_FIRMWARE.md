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
