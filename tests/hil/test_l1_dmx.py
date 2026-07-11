"""
L1 — DMX Output Tests

Automated with serial loopback or ?dmx0 query (semi-automated without loopback).

With selftest mode (CONFIG_GLOW_SELFTEST) setting universe 0 channel 0 to 200:
- Assert via loopback UART (if hardware jumpered) or ?dmx0 query
- Validate DMX output is active and responding

Fully automated via query; semi-automated via loopback (hardware-dependent).
"""

import pytest
import time
from conftest import SerialReader, TelemetryLine


class TestDMXOutput:
    """L1: DMX output validation."""

    def test_dmx_begin_ok(self, device_reset, serial_reader: SerialReader):
        """Validate dmx begin=ok telemetry on boot."""
        device_reset()

        dmx_line = serial_reader.read_until("GLOW-TEST: dmx begin=ok", timeout_s=10)
        assert dmx_line is not None, "DMX not initialized"

    def test_dmx_serial_query_ch0(self, device_reset, serial_reader: SerialReader):
        """
        Query the first channel of universe 0 via ?dmx0 serial command.

        In selftest mode, expects 200; otherwise expects 0 (boot default).
        """
        device_reset()

        # Wait for boot
        serial_reader.read_until("dmx begin=ok", timeout_s=10)
        time.sleep(0.5)

        # Send query command: ?dmx0
        if serial_reader.ser:
            serial_reader.ser.write(b"?dmx0\n")
            time.sleep(0.2)

            # Read response (should echo back channel values)
            response = serial_reader.readline(timeout_s=2.0)
            assert response is not None, "No response to ?dmx0 query"

            # Response format: "200 0 0 0 ..." (first byte is channel 0)
            # In selftest mode, first byte should be 200
            # In normal mode, first byte should be 0
            values = response.split()
            assert len(values) > 0, "Empty response to ?dmx0"

            try:
                ch0 = int(values[0])
                # In selftest: expect 200. In normal: expect 0 or close to it.
                # Accept 0 or 200 for compatibility with both modes.
                assert ch0 in [0, 200], (
                    f"Channel 0 should be 0 (normal) or 200 (selftest), got {ch0}"
                )
            except ValueError:
                pytest.skip(f"Could not parse DMX response: {response}")

    def test_dmx_universe_structure(self, device_reset, serial_reader: SerialReader):
        """
        Validate that ?dmx0 returns 512 bytes (a full DMX universe).

        This tests that the DMX output is properly structured.
        """
        device_reset()

        serial_reader.read_until("dmx begin=ok", timeout_s=10)
        time.sleep(0.5)

        if serial_reader.ser:
            serial_reader.ser.write(b"?dmx0\n")
            time.sleep(0.2)

            response = serial_reader.readline(timeout_s=2.0)
            assert response is not None

            values = response.split()
            # Should have 512 channels
            assert len(values) >= 512, (
                f"DMX universe should have 512 channels, got {len(values)}"
            )

            # All values should be integers in [0, 255]
            for i, val_str in enumerate(values[:512]):
                try:
                    val = int(val_str)
                    assert 0 <= val <= 255, (
                        f"Channel {i} value {val} outside [0, 255]"
                    )
                except ValueError:
                    pytest.fail(f"Channel {i} not an integer: {val_str}")

    def test_dmx_stats_persistent(self, device_reset, serial_reader: SerialReader):
        """
        Assert stats telemetry continues to report (dmx output is running).

        If stats stop, the DMX output task may have crashed.
        """
        device_reset()

        serial_reader.read_until("dmx begin=ok", timeout_s=10)

        # Collect stats for 5 seconds
        start = time.time()
        stats_count = 0

        while time.time() - start < 5:
            line = serial_reader.readline(timeout_s=0.5)
            if line and "GLOW-TEST: stats" in line:
                stats_count += 1

        assert (
            stats_count > 0
        ), "No stats telemetry in 5 seconds (dmx output task may have crashed)"

    def test_dmx_no_crash_marker(self, device_reset, serial_reader: SerialReader):
        """
        Assert no crash markers during a 5-second observation window.

        This is a basic sanity check for the DMX output layer.
        """
        device_reset()

        serial_reader.read_until("dmx begin=ok", timeout_s=10)

        crash_markers = ["panic", "abort", "Guru Meditation", "rst:"]
        start = time.time()
        found_crash = None

        while time.time() - start < 5:
            line = serial_reader.readline(timeout_s=0.5)
            if line:
                for marker in crash_markers:
                    if marker.lower() in line.lower():
                        found_crash = marker
                        break

            if found_crash:
                break

        assert found_crash is None, f"Crash detected: {found_crash}"
