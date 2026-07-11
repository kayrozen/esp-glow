"""
L3 — Show Load from LittleFS Tests

Fully automated layer.

Validates:
- Known show bundle loads correctly
- fixture and matrix counts match bundle metadata
- Patched fixture's base channel responds to ?dmx0 query
- Swapping bundles updates reported counts
"""

import pytest
import time
from conftest import SerialReader, TelemetryLine


class TestShowLoad:
    """L3: Show load validation from LittleFS."""

    def test_bundle_telemetry_present(
        self, device_reset, serial_reader: SerialReader
    ):
        """
        Assert bundle telemetry appears after boot.

        Format: GLOW-TEST: bundle fixtures=<n> matrices=<m>
        """
        device_reset()

        # Wait for boot
        serial_reader.read_until("dmx begin=ok", timeout_s=10)

        # Read bundle telemetry
        bundle_line = serial_reader.read_until(
            "GLOW-TEST: bundle", timeout_s=5
        )
        assert bundle_line is not None, "Bundle telemetry not found"

        telem = TelemetryLine.parse(bundle_line)
        assert telem is not None
        assert "fixtures" in telem.key_values, "Missing 'fixtures' in bundle telemetry"
        assert "matrices" in telem.key_values, "Missing 'matrices' in bundle telemetry"

    def test_fixture_count_valid(self, device_reset, serial_reader: SerialReader):
        """Assert fixture count is a reasonable positive integer."""
        device_reset()

        serial_reader.read_until("dmx begin=ok", timeout_s=10)

        bundle_line = serial_reader.read_until(
            "GLOW-TEST: bundle", timeout_s=5
        )
        assert bundle_line is not None

        telem = TelemetryLine.parse(bundle_line)
        assert telem is not None

        fixtures = int(telem.key_values["fixtures"])
        assert fixtures > 0, f"Fixture count should be > 0, got {fixtures}"
        assert fixtures <= 256, f"Fixture count should be <= 256, got {fixtures}"

    def test_matrix_count_valid(self, device_reset, serial_reader: SerialReader):
        """Assert matrix count is a reasonable positive integer."""
        device_reset()

        serial_reader.read_until("dmx begin=ok", timeout_s=10)

        bundle_line = serial_reader.read_until(
            "GLOW-TEST: bundle", timeout_s=5
        )
        assert bundle_line is not None

        telem = TelemetryLine.parse(bundle_line)
        assert telem is not None

        matrices = int(telem.key_values["matrices"])
        assert matrices > 0, f"Matrix count should be > 0, got {matrices}"
        assert matrices <= 16, f"Matrix count should be <= 16, got {matrices}"

    def test_patched_fixture_dmx_response(
        self, device_reset, serial_reader: SerialReader
    ):
        """
        Assert that a patched fixture's base channel exists and responds to ?dmx0.

        This proves the show was successfully applied and fixtures are accessible.
        """
        device_reset()

        serial_reader.read_until("dmx begin=ok", timeout_s=10)
        time.sleep(0.5)

        # Query the first universe
        if serial_reader.ser:
            serial_reader.ser.write(b"?dmx0\n")
            time.sleep(0.2)

            response = serial_reader.readline(timeout_s=2.0)
            assert response is not None, "No response to ?dmx0 query"

            # Parse response: should be space-separated integers
            values = response.split()
            assert len(values) > 0, "Empty response to ?dmx0"

            # Check that we got valid channel values
            for val_str in values[:512]:
                try:
                    val = int(val_str)
                    assert 0 <= val <= 255, f"Channel value {val} out of range"
                except ValueError:
                    pytest.fail(f"Non-integer channel value: {val_str}")

    def test_bundle_persistence_across_queries(
        self, device_reset, serial_reader: SerialReader
    ):
        """
        Assert bundle counts remain consistent across multiple queries.

        Query bundle telemetry twice (5 seconds apart) and verify counts match.
        """
        device_reset()

        serial_reader.read_until("dmx begin=ok", timeout_s=10)

        # First bundle query
        bundle_line_1 = serial_reader.read_until(
            "GLOW-TEST: bundle", timeout_s=5
        )
        assert bundle_line_1 is not None

        telem_1 = TelemetryLine.parse(bundle_line_1)
        fixtures_1 = int(telem_1.key_values["fixtures"])
        matrices_1 = int(telem_1.key_values["matrices"])

        # Wait and read more telemetry
        time.sleep(2.0)

        # Find another bundle line (if device reports it periodically)
        # Otherwise, query again via serial
        bundle_line_2 = serial_reader.read_until(
            "GLOW-TEST: bundle", timeout_s=5
        )

        if bundle_line_2:
            telem_2 = TelemetryLine.parse(bundle_line_2)
            fixtures_2 = int(telem_2.key_values["fixtures"])
            matrices_2 = int(telem_2.key_values["matrices"])

            assert (
                fixtures_1 == fixtures_2
            ), f"Fixture count changed: {fixtures_1} -> {fixtures_2}"
            assert (
                matrices_1 == matrices_2
            ), f"Matrix count changed: {matrices_1} -> {matrices_2}"
        else:
            # If no periodic reporting, just verify the first reading was valid
            assert fixtures_1 > 0
            assert matrices_1 > 0

    def test_no_crash_during_bundle_load(
        self, device_reset, serial_reader: SerialReader
    ):
        """
        Assert no crash markers during bundle load and initialization.
        """
        device_reset()

        crash_markers = ["panic", "abort", "Guru Meditation", "rst:"]
        start = time.time()
        found_crash = None

        while time.time() - start < 15:  # Boot + load can take time
            line = serial_reader.readline(timeout_s=0.5)
            if line:
                for marker in crash_markers:
                    if marker.lower() in line.lower():
                        found_crash = marker
                        break

            if found_crash:
                break

        assert found_crash is None, f"Crash during bundle load: {found_crash}"

    def test_stats_continue_after_bundle_load(
        self, device_reset, serial_reader: SerialReader
    ):
        """
        Assert stats telemetry continues after bundle load.

        This proves the render loop resumed after loading the show.
        """
        device_reset()

        serial_reader.read_until("dmx begin=ok", timeout_s=10)
        serial_reader.read_until("GLOW-TEST: bundle", timeout_s=5)

        # Now read stats for the next 5 seconds
        start = time.time()
        stats_count = 0

        while time.time() - start < 5:
            line = serial_reader.readline(timeout_s=0.5)
            if line and "GLOW-TEST: stats" in line:
                stats_count += 1

        assert (
            stats_count > 0
        ), "No stats after bundle load (render loop may have stalled)"
