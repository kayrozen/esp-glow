"""
L1 — DMX Output

Automated via the ?dmx0 serial query (no loopback hardware needed).

Selftest fixture (see main.cpp's setup_selftest_fixture, active when no
"show" bundle is flashed) patches universe 0 channel 0 to a Dimmer effect
at 200/255, goes it immediately -> ?dmx0's first byte reads 200.

Without a physical DMX loopback, actually confirming light-level output on
the wire is a human visual check -- noted, not attempted here.
"""

from conftest import line_is_crash


class TestDMXOutput:
    def test_dmx_begin_ok(self, device_reset, serial_reader):
        """device_reset() blocks until this line arrives; confirm it actually landed in the log."""
        device_reset()
        assert any("GLOW-TEST: dmx begin=ok" in line for line in serial_reader.logs)

    def test_dmx0_query_selftest_channel0(self, device_reset, serial_reader):
        """?dmx0 -> first 8 bytes of universe 0; selftest fixture drives ch0 to 200."""
        device_reset()
        telem = serial_reader.query("?dmx0", timeout_s=3)
        assert telem is not None, "No response to ?dmx0 query"

        bytes8 = telem.list_field("bytes")
        assert len(bytes8) == 8, f"Expected 8 bytes from ?dmx0, got {len(bytes8)}: {bytes8}"

        values = [int(b) for b in bytes8]
        assert all(0 <= v <= 255 for v in values), f"Byte out of range: {values}"
        assert values[0] == 200, f"Selftest fixture expects channel 0 == 200, got {values[0]}"

    def test_dmx0_query_repeatable(self, device_reset, serial_reader):
        """Querying twice in a row gives a consistent, stable reading."""
        device_reset()
        first = serial_reader.query("?dmx0", timeout_s=3)
        second = serial_reader.query("?dmx0", timeout_s=3)
        assert first is not None and second is not None
        assert first.key_values["bytes"] == second.key_values["bytes"]

    def test_dmx_stats_persistent(self, device_reset, serial_reader):
        """stats telemetry keeps flowing (the DMX output task hasn't crashed)."""
        device_reset()
        telem = serial_reader.read_telemetry(event="stats", timeout_s=5)
        assert telem is not None, "No stats telemetry within 5s (DMX output task may have crashed)"

    def test_dmx_no_crash_marker(self, device_reset, serial_reader):
        device_reset()
        start_logs_len = len(serial_reader.logs)
        serial_reader.read_for_duration(5.0)
        for line in serial_reader.logs[start_logs_len:]:
            marker = line_is_crash(line)
            assert marker is None, f"Crash detected: '{marker}' in {line!r}"
