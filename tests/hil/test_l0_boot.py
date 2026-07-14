"""
L0 — Boot / POST

Fully automated. Flash a selftest build, reset, read 10s.

Assert: banner; boot core=1; dmx begin=ok; stats appears with frames ~44/s
(+/-10%), behind=0, dropped=0; no panic/Guru Meditation/rst: in the window.
"""

import time

from conftest import line_is_crash


class TestBootPOST:
    def test_boot_banner_present(self, device_reset, serial_reader):
        device_reset(wait_for_boot=False)
        line = serial_reader.read_until("esp-glow firmware", timeout_s=10)
        assert line is not None, "Boot banner not found in first 10s"

    def test_boot_telemetry_core_and_hz(self, device_reset, serial_reader):
        device_reset(wait_for_boot=False)
        telem = serial_reader.read_telemetry(event="boot", timeout_s=10)
        assert telem is not None, "GLOW-TEST: boot telemetry not found within 10s"
        assert "core" in telem.key_values and "hz" in telem.key_values

        core = telem.int_field("core")
        hz = telem.int_field("hz")
        assert core in (0, 1), f"core should be 0 or 1, got {core}"
        assert 40 <= hz <= 50, f"hz should be ~44, got {hz}"

    def test_dmx_begin_ok(self, device_reset, serial_reader):
        device_reset(wait_for_boot=False)
        telem = serial_reader.read_telemetry(event="dmx", timeout_s=10)
        assert telem is not None, "GLOW-TEST: dmx telemetry not found within 10s"
        assert telem.key_values.get("begin") == "ok"

    def test_stats_frame_rate_and_no_drops(self, device_reset, serial_reader):
        """stats frames ~44/s (+/-10%), behind=0, dropped=0 on an idle boot."""
        device_reset()  # waits for dmx begin=ok

        readings = []
        deadline = time.time() + 12
        while time.time() < deadline and len(readings) < 3:
            telem = serial_reader.read_telemetry(event="stats", timeout_s=5)
            assert telem is not None, "No GLOW-TEST: stats telemetry within 5s"
            readings.append(telem)

        assert len(readings) >= 2, "Need at least 2 stats readings to check cadence"

        for telem in readings:
            frames = telem.int_field("frames")
            behind = telem.int_field("behind")
            dropped = telem.int_field("dropped")
            assert 0.9 * 44 <= frames <= 1.1 * 44, f"frames={frames}, expected ~44 (+/-10%)"
            assert behind == 0, f"behind={behind}, expected 0 on an idle boot"
            assert dropped == 0, f"dropped={dropped}, expected 0 on an idle boot"

    def test_heap_and_lua_mem_reported(self, device_reset, serial_reader):
        device_reset()
        telem = serial_reader.read_telemetry(event="stats", timeout_s=5)
        assert telem is not None
        assert telem.int_field("heap") > 0
        # lua_mem is 0 until glowLuaInit succeeds, which happens before the
        # render task starts (see main.cpp's setup_lua/app_main ordering) --
        # by the time any stats line prints, it should be nonzero.
        assert telem.int_field("lua_mem") > 0, "lua_mem stayed 0 -- Lua/Fennel init may have failed"

    def test_no_crash_in_boot_window(self, device_reset, serial_reader):
        device_reset(wait_for_boot=False)
        # The ROM bootloader's own reset-reason line ("rst:0x1 (POWERON),
        # boot:0x4 (SPI_FLASH_BOOT)") precedes every boot, including a
        # totally normal one, and legitimately contains the "rst:" crash
        # marker (see conftest.py's CRASH_MARKERS -- it's there to catch an
        # *unexpected* reboot appearing mid-window, e.g. test_l7_soak.py's
        # use after its own initial device_reset() has already consumed
        # this same line). Consume it here too before starting the strict
        # window, so this test asserts "no crash once boot starts", not "no
        # reset-reason line exists anywhere" (which would fail on every
        # single boot, crash or not).
        banner = serial_reader.read_until("esp-glow firmware", timeout_s=10)
        assert banner is not None, "Boot banner not found in first 10s"

        start = time.time()
        while time.time() - start < 10:
            line = serial_reader.readline(timeout_s=0.5)
            if line is None:
                continue
            marker = line_is_crash(line)
            assert marker is None, f"Crash detected during boot: '{marker}' in {line!r}"
