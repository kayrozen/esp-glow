# QEMU Boot Test Suite

Boots the real esp-glow firmware image -- bootloader, partition table, app,
the demo `SHW1` bundle -- under [Espressif's QEMU
fork](https://github.com/espressif/qemu) (ESP32-S3 support) with no board
attached, and asserts the same `GLOW-TEST:` telemetry the `tests/hil/` L0
layer asserts against real hardware. This is Wave 0 of the project's
bring-up plan: proving the firmware *starts* before any board exists to
plug in.

## Why this exists

Everything up to this suite was host-green (`make test`, pure C++, no
hardware) but never executed on an actual CPU. QEMU catches the whole first
half of the bring-up ladder without a cable:

- Does it boot at all? Init order, `app_main`, task creation, core pinning.
- The raw "show" partition read: does `loadShow()` find the SHW1 bundle,
  does `GLOW-TEST: bundle fixtures=... matrices=...` print?
- LittleFS mount for `scripts`.
- The Lua VM starting in PSRAM, the Fennel compiler loading -- a large,
  previously-untested surface (a big Lua table graph being built on an
  emulated MCU; if it blows the heap or a task stack, this suite finds out).
- The render task running and a `stats` line appearing.
- Crashes, panics, guru meditation errors -- caught as a hard test failure,
  not silently missed.

## What this suite does NOT catch

Be honest about the limits, not just the wins:

- **DMX break/MAB timing.** `esp_dmx`'s UART bring-up may or may not
  initialize cleanly against QEMU's emulated UART (this harness will tell
  you whether `dmx begin=ok` appears at all -- it says nothing about
  whether the break/MAB timing on a real RS-485 line is correct). That
  remains the least-validated code in the project and only a logic
  analyser against real hardware settles it.
- **Real GC pacing / dropped frames under load.** QEMU is not
  cycle-accurate. This suite's `dropped == 0` check is an idle-boot smoke
  test (proof the render task runs at all), not the soak test's real-time
  claim.
- **PSRAM timing, USB host, MIDI UART, DJ Link** -- anything requiring a
  peripheral this QEMU fork doesn't model.
- **WiFi/Art-Net/the WebSocket console.** This QEMU fork's esp32s3 machine
  has no WiFi/802.11 hardware model at all (only a wired "open_eth" NIC,
  per its `hw/xtensa/esp32s3.c`) -- `esp_wifi_init()`'s RF calibration step
  hangs forever waiting on radio status bits emulated silicon never sets.
  The build this suite runs sets `CONFIG_GLOW_SKIP_WIFI=y`
  (`sdkconfig.qemu.defaults`), which skips WiFi/Art-Net/the web
  console/OSC/DJ Link entirely at boot -- a real, permanent firmware
  option (see `firmware/main/Kconfig.projbuild`), not a QEMU-only patch:
  it's equally useful for a physical rig with no network at all. This is
  also why network bring-up in `app_main()` happens *after* DMX/the render
  task/the Lua VM, not before -- the WiFi-hang this suite first uncovered
  would otherwise have blocked a real board's whole boot too, the moment
  its WiFi hardware or AP ever failed to come up. `artnet tx=ok` never
  appears in this suite's build and is not asserted here.

A green run means the software starts. It does not mean the rig works.

## How it relates to `tests/hil/`

Deliberately the *same* harness, pointed at a different transport:
`tests/shared/telemetry.py` (the `GLOW-TEST:` wire format parser) and
`tests/shared/line_reader.py` (the read/assert-no-crash loop) are shared
verbatim between `tests/hil/conftest.py`'s `SerialReader` (USB serial) and
this suite's `conftest.py`'s `QemuReader` (a QEMU subprocess's stdio). A
test file's assertions read identically either way; only the fixture that
produces the reader differs. `tests/qemu/test_l0_boot.py` mirrors
`tests/hil/test_l0_boot.py` closely, with one deliberate difference: it
does not assert real-time frame cadence (see `test_stats_appear_no_drops`'s
docstring) since QEMU cannot back that claim.

## Prerequisites

1. An ESP-IDF environment, sourced (`. $IDF_PATH/export.sh`) -- provides
   `idf.py` and `esptool.py`, required unless `GLOW_SKIP_BUILD=1`.
2. `qemu-system-xtensa` with ESP32-S3 machine support, on `PATH` (or
   pointed to via `GLOW_QEMU_BIN`). Two ways to get one:
   - **Prebuilt** (fastest, what CI uses): `idf_tools.py install qemu-xtensa`
     if your ESP-IDF version bundles the `qemu-xtensa` tool definition
     (ESP-IDF >= ~5.3.1; check `python $IDF_PATH/tools/idf_tools.py list
     | grep qemu`), or download a release asset directly from
     <https://github.com/espressif/qemu/releases> (asset name
     `qemu-xtensa-softmmu-*-x86_64-linux-gnu.tar.xz` for Linux CI runners).
   - **Build from source** (works with any ESP-IDF version, what this was
     developed against): `git clone --depth 1 --branch esp-develop
     https://github.com/espressif/qemu.git && cd qemu && ./configure
     --target-list=xtensa-softmmu --disable-capstone --disable-docs
     --disable-tools --disable-gtk --disable-sdl --disable-vnc
     --disable-user --enable-slirp && ninja -C build qemu-system-xtensa`.
     Needs `ninja-build pkg-config libglib2.0-dev libpixman-1-dev
     libslirp-dev libgcrypt20-dev` (Debian/Ubuntu package names). This
     path does not depend on `idf.py qemu` existing at all -- see below.
3. Python: `pip install -r requirements.txt`.

No board required, ever.

## Running

```sh
cd tests/qemu
pip install -r requirements.txt
. $IDF_PATH/export.sh

pytest                       # builds once, boots fresh per test
GLOW_SKIP_BUILD=1 pytest     # iterate on the harness without rebuilding
```

## Environment variables

| Variable | Required | Default | Purpose |
|---|---|---|---|
| `GLOW_FIRMWARE_DIR` | no | `<repo>/firmware` | ESP-IDF project path |
| `GLOW_IDF_BUILD_DIR` | no | `<firmware>/build-qemu-selftest` | Out-of-tree build dir (separate from a developer's `build/` and the HIL suite's `build-hil-selftest`) |
| `GLOW_SKIP_BUILD` | no | unset | Skip `idf.py build`; assume `GLOW_IDF_BUILD_DIR` already holds a selftest build |
| `GLOW_QEMU_BIN` | no | `qemu-system-xtensa` | Path to the QEMU binary |
| `GLOW_QEMU_MACHINE` | no | `esp32s3` | QEMU `-machine` name |

## How it works

1. `idf.py build` with `sdkconfig.defaults` + `sdkconfig.selftest.defaults`
   (`CONFIG_GLOW_SELFTEST=y`, the same layered config `tests/hil/` flashes
   to real hardware) + `sdkconfig.qemu.defaults` (`CONFIG_GLOW_SKIP_WIFI=y`,
   required only because this QEMU fork has no WiFi hardware model -- see
   "What this suite does NOT catch" above) layered on top.
2. `conftest.py`'s `build_qemu_flash_image()` reads the build's
   `flasher_args.json` and merges bootloader + partition table + app +
   the `show` partition's SHW1 bundle into one flat, flash-size-padded
   `qemu_flash.bin` via `esptool.py --chip esp32s3 merge_bin`. This is
   deliberately what `idf.py qemu` does internally (per the project's
   bring-up plan: "QEMU builds a `qemu_flash.bin` from your `flash_args`")
   -- reimplemented directly against `esptool.py` rather than depended on
   as an `idf.py` subcommand, so this suite works against any ESP-IDF
   version that can produce a `flasher_args.json` (including the v5.1 this
   project's CI currently pins -- see `.github/workflows/firmware.yml` --
   which predates `idf.py qemu` entirely).
3. `QemuReader` (a `LineReader` subclass -- see
   `tests/shared/line_reader.py`) launches `qemu-system-xtensa -nographic
   -no-reboot -machine esp32s3 -drive file=qemu_flash.bin,if=mtd,format=raw`
   as a subprocess and reads the guest's UART0 off the subprocess's stdio
   (`-nographic` muxes the emulated serial console to stdin/stdout -- no
   PTY, no network socket). A fresh QEMU process per test is this suite's
   equivalent of `tests/hil/`'s RTS-toggle device reset.
4. Test files parse `GLOW-TEST:` telemetry exactly like `tests/hil/` does,
   via the shared `TelemetryLine`/`line_is_crash` (`tests/shared/telemetry.py`).

## Sandbox note (this harness's own development)

This suite's Python harness (`build_qemu_flash_image`'s `esptool.py
merge_bin` invocation and `QemuReader`'s subprocess/line-reading plumbing)
was verified end-to-end against a **real, from-source-built**
`qemu-system-xtensa` -- including booting the actual ESP32-S3 ROM
bootloader against a blank flash image and observing its real
`invalid header` retry behavior, proving the QEMU/xtensa/esp32s3 emulation
path itself works. What could not be verified in that sandbox was booting
the *actual esp-glow firmware*: outbound downloads of GitHub release
assets (the prebuilt `xtensa-esp32s3-elf` GCC toolchain `idf.py
install.sh` needs) were blocked by that environment's network policy, so
`idf.py build` could not run there. CI (which has full internet access) is
where this suite runs against the real firmware for the first time -- see
`.github/workflows/qemu-boot.yml`.
