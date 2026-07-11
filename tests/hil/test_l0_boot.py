"""
L0 — Boot / POST Tests

Fully automated layer.

Validates:
- Banner present on reset
- boot telemetry line with core and hz
- dmx begin=ok
- stats frames at ~44/s (±10%)
- No panic/abort/Guru Meditation/rst: reboot
"""

import pytest
import re
import time
from conftest import SerialReader, TelemetryLine


class TestBootPOST:
    """L0: Boot and POST verification."""

    def test_boot_banner_present(self, device_reset, serial_reader: SerialReader):
        """Assert boot banner appears within 10 seconds of reset."""
        device_reset()

        start = time.time()
        found_banner = False

        while time.time() - start < 10:
            line = serial_reader.readline(timeout_s=0.5)
            if line is None:
                continue

            # Look for common ESP32 boot messages
            if any(marker in line for marker in ["esp-glow", "Glow", "boot", "Starting"]):
                found_banner = True
                break

        assert found_banner, "Boot banner not found in first 10 seconds"

    def test_boot_telemetry(self, device_reset, serial_reader: SerialReader):
        """Assert boot telemetry appears with core and hz."""
        device_reset()

        boot_telem = serial_reader.read_until(
            "GLOW-TEST: boot", timeout_s=10
        )
        assert boot_telem is not None, "Boot telemetry not found within 10 seconds"

        telem = TelemetryLine.parse(boot_telem)
        assert telem is not None
        assert "core" in telem.key_values, "Missing 'core' in boot telemetry"
        assert "hz" in telem.key_values, "Missing 'hz' in boot telemetry"

        # Validate core is a number and hz is reasonable (should be ~44)
        core = int(telem.key_values["core"])
        hz = int(telem.key_values["hz"])

        assert core in [0, 1], f"core should be 0 or 1, got {core}"
        assert 40 <= hz <= 50, f"hz should be ~44, got {hz}"

    def test_dmx_begin_ok(self, device_reset, serial_reader: SerialReader):
        """Assert dmx begin=ok telemetry appears within 10 seconds."""
        device_reset()

        dmx_line = serial_reader.read_until(
            "GLOW-TEST: dmx begin=ok", timeout_s=10
        )
        assert dmx_line is not None, "DMX begin=ok telemetry not found within 10 seconds"

    def test_stats_frame_rate(self, device_reset, serial_reader: SerialReader):
        """
        Assert stats telemetry appears with frames ~44/s (±10%).

        Read for 10 seconds, collect frame counts, validate rate.
        """
        device_reset()

        # Wait for boot to complete
        serial_reader.read_until("dmx begin=ok", timeout_s=10)

        # Read stats lines for 10 seconds
        start = time.time()
        frame_counts = []

        while time.time() - start < 10:
            line = serial_reader.readline(timeout_s=0.5)
            if line is None:
                continue

            telem = TelemetryLine.parse(line)
            if telem and "frames" in telem.key_values:
                try:
                    frames = int(telem.key_values["frames"])
                    frame_counts.append(frames)
                except ValueError:
                    pass

        assert len(frame_counts) > 0, "No frame statistics found in 10 seconds"

        # Compute frame rate: assume first and last frame counts represent ~10s window
        if len(frame_counts) >= 2:
            frame_delta = frame_counts[-1] - frame_counts[0]
            frame_rate = frame_delta / 10.0

            # Validate: 44 Hz ±10% = [39.6, 48.4]
            expected_min = 44 * 0.9
            expected_max = 44 * 1.1

            assert (
                expected_min <= frame_rate <= expected_max
            ), f"Frame rate {frame_rate:.1f} Hz outside expected range [{expected_min:.1f}, {expected_max:.1f}]"

    def test_behind_zero_initially(self, device_reset, serial_reader: SerialReader):
        """Assert behind=0 in early stats telemetry (no frame drops)."""
        device_reset()

        # Wait for boot
        serial_reader.read_until("dmx begin=ok", timeout_s=10)

        # Get the first stats line after boot
        stats_line = serial_reader.read_until("GLOW-TEST: stats", timeout_s=5)
        assert stats_line is not None, "Stats telemetry not found"

        telem = TelemetryLine.parse(stats_line)
        assert telem is not None
        assert "behind" in telem.key_values, "Missing 'behind' in stats"

        behind = int(telem.key_values["behind"])
        assert behind == 0, f"Expected behind=0 at boot, got {behind}"

    def test_no_panic_in_boot_window(self, device_reset, serial_reader: SerialReader):
        """
        Assert no panic/abort/Guru Meditation/rst: lines in first 10 seconds.

        These indicate a crash.
        """
        device_reset()

        crash_markers = [
            "panic",
            "abort",
            "Guru Meditation Error",
            "rst:",
            "FATAL",
            "Backtrace:",
        ]

        start = time.time()
        found_crash = None

        while time.time() - start < 10:
            line = serial_reader.readline(timeout_s=0.5)
            if line is None:
                continue

            for marker in crash_markers:
                if marker.lower() in line.lower():
                    found_crash = marker
                    break

            if found_crash:
                break

        assert (
            found_crash is None
        ), f"Crash detected during boot: '{found_crash}' in output"

    def test_heap_reported(self, device_reset, serial_reader: SerialReader):
        """Assert heap size is reported in stats telemetry."""
        device_reset()

        serial_reader.read_until("dmx begin=ok", timeout_s=10)

        stats_line = serial_reader.read_until("GLOW-TEST: stats", timeout_s=5)
        assert stats_line is not None

        telem = TelemetryLine.parse(stats_line)
        assert telem is not None
        assert "heap" in telem.key_values, "Missing 'heap' in stats"

        heap = int(telem.key_values["heap"])
        assert heap > 0, f"Heap size should be > 0, got {heap}"
