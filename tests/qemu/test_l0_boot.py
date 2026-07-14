"""
QEMU L0 — Boot / POST

Boots a CONFIG_GLOW_SELFTEST image under Espressif's QEMU fork (see
conftest.py) and asserts the telemetry lines the project's bring-up plan
calls out by name: banner, `dmx begin=ok`, `bundle fixtures=... matrices=...`,
`scripts mount=ok`, and a `stats` tick -- with no panic anywhere in the boot
window. This is tests/hil/test_l0_boot.py's assertions, minus the one claim
this transport cannot make (real-time frame cadence -- see
test_stats_appear_no_drops below and README.md's "what QEMU will NOT catch").

Each test gets its own fresh `qemu_boot` (a new QEMU process is this
suite's equivalent of tests/hil/'s RTS device_reset -- see conftest.py's
`qemu_boot` fixture).
"""

import time

from conftest import line_is_crash


class TestQemuBootPOST:
    def test_boot_banner_present(self, qemu_boot):
        line = qemu_boot.read_until("esp-glow firmware", timeout_s=20)
        assert line is not None, "Boot banner not found in first 20s"

    def test_boot_telemetry_core_and_hz(self, qemu_boot):
        telem = qemu_boot.read_telemetry(event="boot", timeout_s=20)
        assert telem is not None, (
            f"GLOW-TEST: boot telemetry not found within 20s. "
            f"Captured log:\n{qemu_boot.flush_logs()[-3000:]}"
        )
        assert "core" in telem.key_values and "hz" in telem.key_values

        core = telem.int_field("core")
        hz = telem.int_field("hz")
        assert core in (0, 1), f"core should be 0 or 1, got {core}"
        # `hz` is main.cpp's configured render target (RenderTaskConfig.targetHz
        # = 44, app_main), not a measured rate -- the same fixed constant on
        # real hardware and under QEMU, so an exact check (not a tolerance
        # band) is correct here.
        assert hz == 44, f"hz should be the configured render target (44), got {hz}"

    def test_dmx_begin_ok(self, qemu_boot):
        telem = qemu_boot.read_telemetry(event="dmx", timeout_s=20)
        assert telem is not None, (
            f"GLOW-TEST: dmx telemetry not found within 20s. "
            f"Captured log:\n{qemu_boot.flush_logs()[-3000:]}"
        )
        assert telem.key_values.get("begin") == "ok"

    def test_bundle_loaded(self, qemu_boot):
        """
        The demo SHW1 bundle (samples/demo.show, built into the image the
        same way firmware.yml's CI build does -- see
        firmware/main/CMakeLists.txt) should load. If this instead times
        out, either the raw "show" partition merge is wrong (see
        conftest.py's build_qemu_flash_image) or loadShow() itself failed
        against the partition layout QEMU sees.
        """
        telem = qemu_boot.read_telemetry(event="bundle", timeout_s=20)
        assert telem is not None, (
            f"GLOW-TEST: bundle telemetry not found within 20s. "
            f"Captured log:\n{qemu_boot.flush_logs()[-3000:]}"
        )
        assert telem.int_field("fixtures") > 0, "bundle reported 0 fixtures"

    def test_scripts_mount_ok(self, qemu_boot):
        telem = qemu_boot.read_telemetry(event="scripts", timeout_s=20)
        assert telem is not None, (
            f"GLOW-TEST: scripts telemetry not found within 20s. "
            f"Captured log:\n{qemu_boot.flush_logs()[-3000:]}"
        )
        assert telem.key_values.get("mount") == "ok"

    def test_stats_appear_no_drops(self, qemu_booted):
        """
        At least one `stats` tick, with dropped==0 -- proof the render task
        is actually running, not proof of real-time cadence. Deliberately
        does NOT assert `frames` is within +/-10% of 44/s the way
        tests/hil/test_l0_boot.py does: QEMU is not cycle-accurate, so wall-
        clock frame rate under emulation is not a claim this suite can make
        (see README.md's "what QEMU will NOT catch"). `dropped` (a whole
        frame period with no render call at all -- see
        tests/hil/IMPLEMENTATION.md) is still meaningful: it catches the
        render task never running, not just running at the wrong speed.
        """
        telem = qemu_booted.read_telemetry(event="stats", timeout_s=30)
        assert telem is not None, (
            f"No GLOW-TEST: stats telemetry within 30s. "
            f"Captured log:\n{qemu_booted.flush_logs()[-3000:]}"
        )
        dropped = telem.int_field("dropped")
        assert dropped == 0, f"dropped={dropped}, expected 0 on an idle boot"

    def test_heap_and_lua_mem_reported(self, qemu_booted):
        telem = qemu_booted.read_telemetry(event="stats", timeout_s=30)
        assert telem is not None, (
            f"No GLOW-TEST: stats telemetry within 30s. "
            f"Captured log:\n{qemu_booted.flush_logs()[-3000:]}"
        )
        assert telem.int_field("heap") > 0
        # lua_mem is 0 until glowLuaInit succeeds (see main.cpp's setup_lua) --
        # by the time any stats line prints, it should be nonzero. This is
        # the harness's coverage of the plan's "295 KB Lua table graph being
        # built on an emulated MCU" concern: if Fennel init blows the heap
        # under QEMU, lua_mem never goes nonzero (or the boot never reaches
        # this stats tick at all -- see test_no_crash_in_boot_window).
        assert telem.int_field("lua_mem") > 0, "lua_mem stayed 0 -- Lua/Fennel init may have failed"

    def test_no_crash_in_boot_window(self, qemu_boot):
        # The ROM bootloader's own reset-reason line ("rst:0x1 (POWERON),
        # boot:0x4 (SPI_FLASH_BOOT)") precedes every boot, including a
        # totally normal one, and legitimately contains the "rst:" crash
        # marker (see tests/shared/telemetry.py's CRASH_MARKERS -- it's
        # there to catch an *unexpected* reboot appearing mid-window, e.g.
        # test_l7_soak.py's use after its own initial device_reset() has
        # already consumed this same line). Consume it here too before
        # starting the strict window, so this test asserts "no crash once
        # boot starts", not "no reset-reason line exists anywhere" (which
        # would fail on every single boot, crash or not).
        banner = qemu_boot.read_until("esp-glow firmware", timeout_s=20)
        assert banner is not None, "Boot banner not found in first 20s"

        start = time.time()
        while time.time() - start < 20:
            line = qemu_boot.readline(timeout_s=0.5)
            if line is None:
                if not qemu_boot.is_running():
                    returncode = qemu_boot.proc.returncode if qemu_boot.proc else None
                    raise AssertionError(
                        f"QEMU exited unexpectedly during boot (returncode={returncode}). "
                        f"Captured log:\n{qemu_boot.flush_logs()[-4000:]}"
                    )
                continue
            marker = line_is_crash(line)
            assert marker is None, f"Crash detected during boot: '{marker}' in {line!r}"
