"""
L3 — Show Load (raw "show" partition, not LittleFS)

Fully automated. Flash a known SHW1 bundle straight to the "show" raw
partition (see storage_manager.h): assert `GLOW-TEST: bundle
fixtures=<n> matrices=<m>` matches; swap bundles -> counts change; corrupt
it -> no crash, no crash loop (see the docstring on
test_corrupt_bundle_no_crash for how "safe blackout" maps onto current,
pre-F5 behavior).
"""

import time

import pytest

from conftest import blank_show_partition, compile_show_bundle, flash_show_bundle


@pytest.fixture(scope="module", autouse=True)
def _restore_blank_show_partition_after_module(serial_port):
    """
    This module is the only one that flashes a real bundle to the "show"
    partition. Every layer after L3 (L5/L6/L7 especially) depends on
    main.cpp's setup_selftest_fixture() fallback -- which only runs when
    the "show" partition has nothing valid on it. Blank it back out once
    this module's tests are done so that invariant holds for the rest of
    the session, regardless of run order.
    """
    yield
    blank_show_partition(serial_port)


class TestShowLoad:
    def test_bundle_a_fixture_and_matrix_counts(
        self, device_reset, serial_reader, firmware_dir, hil_fixtures_dir, serial_port, tmp_path
    ):
        bundle = tmp_path / "bundle_a.shw1"
        compile_show_bundle(hil_fixtures_dir / "bundle_a.show", bundle)
        flash_show_bundle(bundle, serial_port)

        device_reset()
        telem = serial_reader.read_telemetry(event="bundle", timeout_s=5)
        assert telem is not None, "GLOW-TEST: bundle telemetry not found"
        assert telem.int_field("fixtures") == 2, telem.raw
        assert telem.int_field("matrices") == 0, telem.raw

    def test_swap_bundle_changes_counts(
        self, device_reset, serial_reader, hil_fixtures_dir, serial_port, tmp_path
    ):
        bundle_a = tmp_path / "bundle_a.shw1"
        bundle_b = tmp_path / "bundle_b.shw1"
        compile_show_bundle(hil_fixtures_dir / "bundle_a.show", bundle_a)
        compile_show_bundle(hil_fixtures_dir / "bundle_b.show", bundle_b)

        flash_show_bundle(bundle_a, serial_port)
        device_reset()
        telem_a = serial_reader.read_telemetry(event="bundle", timeout_s=5)
        assert telem_a is not None
        assert (telem_a.int_field("fixtures"), telem_a.int_field("matrices")) == (2, 0)

        flash_show_bundle(bundle_b, serial_port)
        device_reset()
        telem_b = serial_reader.read_telemetry(event="bundle", timeout_s=5)
        assert telem_b is not None
        assert (telem_b.int_field("fixtures"), telem_b.int_field("matrices")) == (3, 1)

        assert (telem_a.int_field("fixtures"), telem_a.int_field("matrices")) != (
            telem_b.int_field("fixtures"), telem_b.int_field("matrices"),
        )
        # No manual restore needed here -- the module-scoped
        # _restore_blank_show_partition_after_module fixture blanks the
        # partition once, after every test in this module has run.

    def test_corrupt_bundle_no_crash(self, device_reset, serial_reader, serial_port, tmp_path):
        """
        A corrupt "show" partition must not crash or crash-loop the device.

        NOTE on "safe blackout": the task spec's target behavior for a
        corrupt bundle is a safe blackout. That fallback path is F5's
        responsibility (main.cpp's own header comment: "F5 replaces this
        fallback with a safe blackout") and F5 has not landed yet -- today,
        a failed storage_load_show() falls back to setup_selftest_fixture()
        (this build) or setup_hardcoded_fallback() (a release build), which
        still drives DMX output. This test asserts what's actually true
        right now (no crash, no crash loop, rendering continues) and flags
        the blackout gap rather than asserting something false; re-tighten
        this to assert an all-zero ?dmx0 once F5 lands (see L8).
        """
        corrupt = tmp_path / "corrupt.shw1"
        corrupt.write_bytes(b"NOTASHW1" + bytes(range(256)) * 4)
        flash_show_bundle(corrupt, serial_port)

        device_reset()  # must still reach "dmx begin=ok" -- no crash loop
        serial_reader.assert_no_crash(duration_s=10.0)

        stats = serial_reader.read_telemetry(event="stats", timeout_s=5)
        assert stats is not None, "Render loop did not resume after a corrupt bundle"

    def test_stats_continue_after_bundle_load(self, device_reset, serial_reader):
        """The render loop resumes (stats keeps flowing) after any bundle load."""
        device_reset()
        readings = 0
        deadline = time.time() + 5
        while time.time() < deadline:
            telem = serial_reader.read_telemetry(event="stats", timeout_s=5)
            if telem is not None:
                readings += 1
        assert readings > 0, "No stats after bundle load (render loop may have stalled)"

    def test_no_crash_during_bundle_load(self, device_reset, serial_reader):
        device_reset(wait_for_boot=False)
        serial_reader.assert_no_crash(duration_s=15.0)
