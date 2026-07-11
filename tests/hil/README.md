# Hardware-in-the-Loop (HIL) Test Suite

Automated testing harness for the esp-glow firmware running on ESP32-S3. The HIL suite validates boot, DMX/Art-Net output, show loading, live control inputs (Web/OSC/MIDI), stress testing, and OTA/fault recovery — all without manual intervention or instrumentation (except where noted).

## Overview

The suite is organized into 6 layers (L0–L6), each testing a major firmware subsystem:

| Layer | Name | Automation | Duration | Purpose |
|-------|------|-----------|----------|---------|
| **L0** | Boot/POST | Fully automated | 10s | Banner, telemetry, frame rate, no crash |
| **L1** | DMX output | Automated or semi | 5s | DMX initialization, channel output via `?dmx0` |
| **L2** | Art-Net output | Fully automated | 5s | UDP 6454 packet inspection, payload changes, frame rate |
| **L3** | Show load | Fully automated | 10s | Bundle load, fixture/matrix counts, persistence |
| **L4** | Inputs (Web/OSC/MIDI) | Mostly automated | 10s | WebSocket round-trip, OSC reception, MIDI init |
| **L5** | Soak/concurrency | Fully automated | **10 min** | 50–100 msg/s mixed load, heap stability, crash-free |
| **L6** | OTA + robustness | Mostly automated | ~30s | Reset resilience, WiFi reconnect, blackout on error |

**Run order:** L0 → L1 → L2 → L3 → L4 → L6 → L5 (soak last, longest).

## Prerequisites

### Firmware Requirements (Phase 0 — must be implemented first)

Before running HIL tests, add a **structured telemetry layer** to the firmware:

1. **Telemetry output** on serial console with prefix `GLOW-TEST:`:
   - `boot core=<0|1> hz=<frame_rate>` — boot event
   - `dmx begin=ok` — DMX initialized
   - `bundle fixtures=<n> matrices=<m>` — show bundle loaded
   - `stats frames=<n> behind=<m> heap=<bytes>` — every ~1 second

2. **Serial query commands**:
   - `?dmx0` — echo universe 0 (first 512 bytes of DMX)
   - `?state` — echo active cue IDs (if available)

3. **Test/inject mode** (build flag `CONFIG_GLOW_SELFTEST`):
   - Load a fixed test show + effect instead of demo
   - Universe 0, channel 0 = 200 (for DMX loopback tests)
   - Deterministic output so assertions are reproducible

### Host Requirements

- **Python 3.8+** with pytest and dependencies:
  ```bash
  pip install pytest pyserial websocket-client
  ```
- **ESP-IDF tools** (esptool.py, idf.py) on PATH
- **Device on USB** at `/dev/ttyUSB0` (or set `GLOW_SERIAL_PORT`)
- **Device on LAN** at `192.168.1.100` (or set `GLOW_DEVICE_IP`)
- For L2 (Art-Net): host must be on same network, UDP port 6454 must be open

### Hardware Setup (Optional, for Full Automation)

- **L1 DMX loopback** (optional, for automated DMX validation):
  - Jumper DMX TX output to a spare UART RX, or use `?dmx0` query
- **L4 MIDI** (optional, semi-automated):
  - USB MIDI device or DIN-MIDI breakout (not required for CI)
- **L6 OTA** (optional):
  - Second firmware image in OTA slot (not required for basic testing)

## Environment Setup

```bash
# Set serial port (if not /dev/ttyUSB0)
export GLOW_SERIAL_PORT=/dev/ttyUSB1

# Set device IP (if not 192.168.1.100)
export GLOW_DEVICE_IP=192.168.1.50

# Install dependencies
pip install pytest pyserial websocket-client

# Navigate to test directory
cd tests/hil
```

## Running Tests

### Quick Start (All Layers)

```bash
# Run all HIL tests (careful: L5 takes ~10 minutes!)
pytest -v

# Run without the 10-minute soak test
pytest -v -m "not slow"

# Run only quick layers (L0–L2)
pytest -v -k "test_l0 or test_l1 or test_l2"
```

### Run Specific Layers

```bash
# L0: Boot/POST (fastest)
pytest test_l0_boot.py -v

# L1: DMX output
pytest test_l1_dmx.py -v

# L2: Art-Net (listen on UDP 6454)
pytest test_l2_artnet.py -v

# L3: Show load
pytest test_l3_show_load.py -v

# L4: Inputs (Web/OSC/MIDI)
pytest test_l4_inputs.py -v

# L5: Soak (10-minute stress test)
pytest test_l5_soak.py -v -s

# L6: OTA/robustness
pytest test_l6_ota_robustness.py -v
```

### Run with Serial Log Capture

```bash
# Capture full serial output to a file
pytest -v --log-file=/tmp/hil_run.log --log-file-level=DEBUG test_l0_boot.py
```

### Debug a Failing Test

```bash
# Run a specific test with verbose output and no timeout
pytest -v -s test_l0_boot.py::TestBootPOST::test_boot_telemetry --timeout=0
```

## Test Output and Logs

- **Serial logs**: Captured in `conftest.py` via `serial_reader.flush_logs()`
- **Pytest output**: Default to console; use `--log-file` to capture
- **Artifacts**: Store panic backtraces and serial captures in test output for debugging

## What Each Test Does

### L0: Boot/POST (`test_l0_boot.py`)

| Test | Validates |
|------|-----------|
| `test_boot_banner_present` | Boot message appears within 10s |
| `test_boot_telemetry` | `GLOW-TEST: boot core=<n> hz=<rate>` |
| `test_dmx_begin_ok` | `GLOW-TEST: dmx begin=ok` telemetry |
| `test_stats_frame_rate` | Frame rate ≈ 44 Hz ±10% |
| `test_behind_zero_initially` | No frame drops at boot (behind=0) |
| `test_no_panic_in_boot_window` | No crash markers in first 10s |
| `test_heap_reported` | Heap size available in stats |

**Duration:** ~10 seconds | **Automation:** Fully automated

---

### L1: DMX Output (`test_l1_dmx.py`)

| Test | Validates |
|------|-----------|
| `test_dmx_begin_ok` | DMX driver initialized |
| `test_dmx_serial_query_ch0` | `?dmx0` returns valid channel data |
| `test_dmx_universe_structure` | 512 channels per universe |
| `test_dmx_stats_persistent` | Render loop still running |
| `test_dmx_no_crash_marker` | No crash during DMX operation |

**Duration:** ~5 seconds | **Automation:** Fully automated (via `?dmx0` query) or semi (with hardware loopback)

**Note:** Requires `CONFIG_GLOW_SELFTEST` to set ch0=200 for validation. Without selftest, ch0 should be 0 (boot default).

---

### L2: Art-Net Output (`test_l2_artnet.py`)

| Test | Validates |
|------|-----------|
| `test_artnet_packets_received` | Packets arrive at port 6454 |
| `test_artnet_id_valid` | Packet header = `"Art-Net\0"` |
| `test_artnet_opcode_dmx` | OpCode = 0x5000 (DMX output) |
| `test_artnet_universe_valid` | Universe field in [0, 15] |
| `test_artnet_payload_length` | Each packet has 512-byte payload |
| `test_artnet_frame_rate_nominal` | ~44 Hz ±15% packet rate |
| `test_artnet_payload_changes_over_time` | RGB data not static (render loop updating) |
| `test_artnet_no_crash_during_stream` | No crash while streaming |

**Duration:** ~7 seconds | **Automation:** Fully automated (no instruments, UDP only)

**Note:** Host must be on same LAN as device and have UDP 6454 open. Listener binds to `0.0.0.0:6454`.

---

### L3: Show Load (`test_l3_show_load.py`)

| Test | Validates |
|------|-----------|
| `test_bundle_telemetry_present` | `GLOW-TEST: bundle fixtures=<n> matrices=<m>` |
| `test_fixture_count_valid` | Fixture count is positive integer [1, 256] |
| `test_matrix_count_valid` | Matrix count is positive integer [1, 16] |
| `test_patched_fixture_dmx_response` | `?dmx0` returns valid channels (fixtures loaded) |
| `test_bundle_persistence_across_queries` | Counts remain stable |
| `test_no_crash_during_bundle_load` | No panic during load |
| `test_stats_continue_after_bundle_load` | Render loop resumed |

**Duration:** ~15 seconds | **Automation:** Fully automated

**Note:** Requires a provisioned LittleFS image with a valid `show.shw1` bundle.

---

### L4: Inputs (Web/OSC/MIDI) (`test_l4_inputs.py`)

| Test | Validates |
|------|-----------|
| `test_websocket_connect` | WS connection accepted, config message received |
| `test_websocket_cue_toggle` | Send cue toggle JSON, receive state broadcast |
| `test_web_no_crash_on_connect` | Multiple WS connect/close cycles safe |
| `test_osc_packet_reception` | OSC UDP packets accepted |
| `test_osc_cue_trigger` | OSC cue message triggers state update |
| `test_osc_no_crash_on_packet` | High-frequency OSC messages safe |
| `test_midi_input_available` | MIDI initialized (optional, semi-automated) |
| `test_midi_no_crash_on_boot` | No crash during MIDI init |

**Duration:** ~15 seconds | **Automation:** Mostly automated (MIDI semi)

**Note:** 
- Web/OSC tests skip gracefully if device doesn't have those inputs configured
- MIDI test requires USB MIDI device or DIN-MIDI breakout; skips if not present
- Device IP must be set (`GLOW_DEVICE_IP` env var or default 192.168.1.100)

---

### L5: Soak/Concurrency (`test_l5_soak.py`)

| Test | Validates |
|------|-----------|
| `test_10min_concurrent_load` | 50 msg/s mixed Web + OSC input for 10 minutes |
| `test_concurrent_load_no_hang` | 100 msg/s for 30s doesn't hang |
| `test_stats_continuity_during_load` | Stats telemetry never silent >2s |

**Duration:** **~10 minutes** (main test) | **Automation:** Fully automated

**What it checks during load:**
- ✓ No `panic`/`abort`/`Guru Meditation`/`rst:` crashes
- ✓ Stats telemetry arrives at least every 5 seconds (no hang)
- ✓ Heap does not leak >10 KB (memory stability)
- ✓ Device handles high concurrent input load without degradation

**Critical note:** This test is designed to surface the **cross-core race condition** where `ShowController` is mutated from multiple tasks (inputs, render loop) without synchronization. It **will fail intermittently if the control-event-queue drain is not wired** into the render loop (see `README_CONTROL_QUEUE.md`).

**Expected behavior:**
- **After queue integration**: All cycles pass, heap stable, no stats gaps
- **Before queue integration**: Intermittent panics or stats timeouts (the race is real)

---

### L6: OTA + Robustness (`test_l6_ota_robustness.py`)

| Test | Validates |
|------|-----------|
| `test_boot_into_valid_state` | Device boots cleanly |
| `test_no_crash_on_repeated_resets` | 5 consecutive resets all succeed |
| `test_wifi_reconnect_telemetry` | WiFi events logged (semi-automated) |
| `test_bundle_missing_safe_blackout` | Missing bundle → DMX all-zero, no crash |
| `test_stats_report_on_boot_failure` | Stats reported even on error |
| `test_no_cascade_crash_on_bad_config` | Only 1 panic on bad config (not cascading) |
| `test_heap_stable_after_boot_error` | Heap doesn't leak on error recovery |
| `test_device_recoverable_state_on_boot` | Can query `?dmx0` even after errors |

**Duration:** ~30 seconds | **Automation:** Mostly automated

**Note:** Full OTA slot testing (push image, verify new boot) requires a second firmware binary in the OTA partition; those tests skip gracefully if not available.

---

## Troubleshooting

### "Failed to open /dev/ttyUSB0"

```bash
# Check device is connected
ls -la /dev/tty*

# Set correct port
export GLOW_SERIAL_PORT=/dev/ttyUSB1

# Verify permissions
sudo usermod -a -G dialout $USER
# (then log out and back in)
```

### "No Art-Net packets received"

```bash
# Check device IP
ping 192.168.1.100

# Check firewall (UDP 6454 must be open)
sudo ufw allow 6454/udp

# Run tcpdump to verify packets are sent
sudo tcpdump -i any -n "udp port 6454"
```

### L5 Soak Test Fails with "Stats timeout" or "Behind growing"

This is **expected before the control-event-queue integration**. The race condition is real:
1. Inputs task mutates `ShowController`
2. Render task reads/updates it concurrently
3. Result: data corruption, missed frames (behind growing), eventually panic

**Solution:** Land the queue-drain fix from the PR review first, then re-run L5.

### WebSocket Connection Refused

```bash
# Device may not have web input enabled
# Try:
curl http://192.168.1.100/ws

# If it fails, the firmware doesn't have web support compiled in
# Build with CONFIG_GLOW_WEB_ENABLED or similar
```

### Tests Hang

```bash
# Force timeout and kill hanging test
pytest test_l5_soak.py --timeout=120 -v

# Or Ctrl+C and check device serial for hangs/panics
```

## Test Markers

Use pytest markers to filter test runs:

```bash
# Only automated tests (skip semi-automated)
pytest -m "not hardware_dependent" -v

# Only quick tests (skip soak)
pytest -m "not slow" -v

# Run by layer
pytest -m "l0" -v    # L0 only
pytest -m "l1 or l2" -v    # L1 and L2
```

## Integration with CI/CD

For GitHub Actions or similar:

```yaml
- name: HIL Tests (Quick)
  run: |
    cd tests/hil
    pytest -v -m "not slow" --tb=short

- name: HIL Soak Test (Long)
  run: |
    cd tests/hil
    pytest test_l5_soak.py -v -s --tb=short
  timeout-minutes: 15
```

## Architecture Notes

### conftest.py

Provides:
- `SerialReader` — manages serial port, reads lines with timeout, parses `GLOW-TEST:` telemetry
- `TelemetryLine` — parses `key=value` telemetry into a dict
- `device_reset` — resets device via RTS (DTR/RTS toggle)
- `serial_reader` — pytest fixture yielding a connected `SerialReader`
- `serial_port` / `device_ip` — env var fixtures

### Test Layers (modules)

- `test_l0_boot.py` — 7 tests, ~10s
- `test_l1_dmx.py` — 5 tests, ~5s
- `test_l2_artnet.py` — 8 tests, ~7s (UDP listener)
- `test_l3_show_load.py` — 7 tests, ~15s
- `test_l4_inputs.py` — 8 tests, ~15s (WS + OSC client)
- `test_l5_soak.py` — 3 tests, ~10+ minutes (concurrent load generator)
- `test_l6_ota_robustness.py` — 8 tests, ~30s

### Error Handling

- **Timeouts**: Tests use short timeouts (2–5s) per read; fail cleanly if device silent
- **Skips**: Tests gracefully skip if optional hardware (MIDI, OTA) is not configured
- **Captures**: On failure, serial logs are dumped for debugging
- **No retries**: If a test fails, it's real; harness doesn't retry flaky reads

## Future Enhancements

- [ ] Automated OTA image generation and push
- [ ] DMX loopback validation (second UART capture)
- [ ] MIDI gadget injection (automated MIDI note generation)
- [ ] Power-cycle validation (brownout, watchdog reset)
- [ ] Heap fragmentation tracking
- [ ] Art-Net universe coverage map (which universes are in use)
- [ ] Frame-by-frame payload capture and diff

## References

- Main design: Firmware HIL test plan (in parent README)
- Control queue integration: `README_CONTROL_QUEUE.md`
- ESP-IDF docs: https://docs.espressif.com/projects/esp-idf/
- Art-Net spec: https://art-net.org.uk/
- OSC spec: http://opensoundcontrol.org/

## Support

For issues:
1. Check device is connected: `ls -la /dev/tty*`
2. Check device IP: `ping $GLOW_DEVICE_IP`
3. Run a quick test: `pytest test_l0_boot.py -v`
4. Capture logs: `pytest test_l0_boot.py -v --log-file=/tmp/hil.log --log-file-level=DEBUG`
5. Check device serial directly: `screen /dev/ttyUSB0 115200`
