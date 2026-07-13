# Hardware-in-the-Loop (HIL) Test Suite

Automated testing harness for the esp-glow firmware on a real ESP32-S3,
driven over USB serial + LAN. Validates boot, DMX/Art-Net output, show
loading, live control (Web/OSC/MIDI), the Fennel live-coding REPL, the
scripting layer's safety guarantees, and a 10-minute soak — everything the
host `make test` suite (pure C++, no hardware) cannot reach.

Phase 0 (firmware observability -- the `GLOW-TEST:` telemetry protocol and
serial query commands this suite depends on) is already implemented, behind
the `CONFIG_GLOW_SELFTEST` Kconfig flag. See `firmware/main/main.cpp`'s
"Phase 0: HIL selftest observability" section and `firmware/main/Kconfig.projbuild`.

## Overview

| Layer | Name | Automation | Approx. duration |
|-------|------|-----------|----------|
| **L0** | Boot/POST | Fully automated | ~15s |
| **L1** | DMX output (`?dmx0`) | Fully automated (no loopback needed) | ~15s |
| **L2** | Art-Net output | Fully automated | ~30s |
| **L3** | Show load (raw "show" partition) | Fully automated | ~1-2 min (compiles + flashes bundles) |
| **L4** | Inputs: Web + OSC | Fully automated; MIDI is semi-automated (skipped) | ~30s |
| **L5** | Fennel REPL / live-coding over WS | Fully automated | ~30s |
| **L6** | Safety guarantees (fx_error, infinite loop, OOM) | Fully automated | ~1 min |
| **L7** | Soak: everything at once | Fully automated | **10 min** |
| **L8** | F5 robustness (OTA / AP-pull / WDT) | Mostly skipped -- F5 not implemented yet | ~30s |

**Run order:** L0 → L1 → L2 → L3 → L4 → L5 → L6 → L8, then **L7 soak last**
(it's the longest test by far, and every earlier layer catches its own
failure mode faster). `run_hil_tests.sh --all` enforces this order; a bare
`pytest` invocation uses alphabetical file order instead (L7 before L8).

Not automated, by design: whether a moving head physically aims correctly,
whether colors look right, whether a beat-synced effect feels in time. This
suite proves the plumbing and the safety guarantees. It does not prove the
photons.

## Prerequisites

1. An ESP-IDF environment, sourced (`. $IDF_PATH/export.sh`) -- provides
   `idf.py` and `esptool.py`, both required unless `GLOW_SKIP_FLASH=1`.
2. Python dependencies: `pip install -r requirements.txt`.
3. An ESP32-S3 board, connected over USB, on the same LAN the test host can
   reach (for Art-Net/WS/OSC).

## Environment variables

| Variable | Required | Default | Purpose |
|---|---|---|---|
| `GLOW_SERIAL_PORT` | no | `/dev/ttyUSB0` | Serial port the board enumerates as |
| `GLOW_DEVICE_IP` | yes, for L2/L4/L5/L6/L7 | -- | Device's LAN IP |
| `GLOW_FIRMWARE_DIR` | no | `<repo>/firmware` | ESP-IDF project path |
| `GLOW_IDF_BUILD_DIR` | no | `<firmware>/build-hil-selftest` | Out-of-tree build dir (never touches a developer's own `build/`) |
| `GLOW_SKIP_FLASH` | no | unset | Skip the build+flash step; assume a selftest build is already on the board |

## Running

```sh
cd tests/hil
pip install -r requirements.txt
. $IDF_PATH/export.sh

# Full run in the correct order (builds + flashes once, ~15+ minutes total):
./run_hil_tests.sh --all --device 192.168.1.42

# Quick run, skip the 10-minute soak:
./run_hil_tests.sh --device 192.168.1.42

# One layer:
./run_hil_tests.sh --layer L2 --device 192.168.1.42

# Iterating on the test suite itself, board already flashed:
./run_hil_tests.sh --skip-flash --device 192.168.1.42
```

The firmware build+flash happens once per pytest session (a session-scoped
fixture in `conftest.py`), not once per test -- individual tests get a
clean boot via an RTS reset, not a reflash.

## How it works

- `conftest.py`'s `SerialReader` opens the port, parses `GLOW-TEST: <event>
  key=value...` telemetry lines into `TelemetryLine`, and exposes a
  `query("?dmx0"/"?state"/"?lua")` helper for the serial query protocol.
- `device_reset()` resets the board via RTS and blocks until `GLOW-TEST:
  dmx begin=ok` reappears (a real reboot, not a reflash).
- L3 compiles small `.show`/`.fdef` fixtures (`fixtures/bundle_a.show`,
  `fixtures/bundle_b.show`) with the `provision` host tool and flashes the
  resulting `.shw1` straight to the raw `"show"` partition via
  `esptool.py write_flash` at a fixed offset (kept in sync with
  `firmware/partitions.csv` by hand -- see `conftest.py`'s
  `SHOW_PARTITION_OFFSET`). It blanks the partition again afterward (a
  module-scoped fixture) so every later layer sees the deterministic
  selftest fixture, not whatever bundle L3 last flashed.
- L5/L6 talk the live-coding protocol from `web_protocol.h` directly over
  `ws://<ip>/ws`: `eval`/`eval_result`, `script_save`/`script_list`/`script`,
  and the unsolicited `fx_error` broadcast.

## Agent guardrails (if you're an agent running this suite)

- Report pass/fail with captured serial + any ESP-IDF backtrace. Do not
  modify firmware to make a test pass.
- Do not weaken assertions to get green -- not the frame-rate tolerance,
  not `dropped=0`, not the leak thresholds in L7. If an assertion looks
  wrong, report it for review instead of loosening it.
- Retry a flaky serial read once (`SerialReader.readline_retry_once`), then
  report. Do not retry indefinitely.
- On a hardware failure, capture the coredump (`espcoredump.py` against the
  `coredump` partition) and escalate. Diagnosing whether a soak reset is a
  race, a GC overrun, or a brownout is a human judgment call.

## Definition of done

A green run means: the device boots, outputs DMX and Art-Net, loads shows
from the raw partition, accepts live control over Web/OSC, compiles and
runs Fennel typed over WebSocket, survives every scripting failure mode
still rendering, and holds all of it for 10 minutes with zero dropped
frames and no memory trend. Everything except the photometric check --
which stays a documented human step.
