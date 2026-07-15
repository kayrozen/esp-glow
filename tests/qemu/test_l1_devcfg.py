"""
QEMU L1 — CFG1 flash-time device config (Wave 2)

Extends the L0 boot suite to cover the three boot-time claims device_config.h
and main.cpp make about the "devcfg" partition:

  1. No devcfg written -> `cfg source=defaults`, and the board still boots
     fine (the compiled-in Kconfig defaults take over).
  2. A devcfg written into the flash image -> `cfg source=devcfg` with the
     expected values (proves the partition is actually read and parsed,
     not just that the fallback path works).
  3. skipWifi=1 (whether via the Kconfig default in (1), or explicitly via
     devcfg in (2)) -> boot completes with no network stall -- this is the
     same property that makes QEMU able to boot this firmware at all (see
     tests/qemu/README.md: this fork has no WiFi/802.11 model, so
     esp_wifi_init() hangs forever against it).

Uses `qemu_boot_factory` (conftest.py) to patch a CFG1 blob into a fresh
copy of the session-built flash image at the real "devcfg" partition
offset -- no firmware rebuild needed per devcfg variant, which is the
entire point of moving this out of Kconfig.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests"))
from shared.devcfg import encode_devcfg  # noqa: E402


class TestQemuDevcfg:
    def test_no_devcfg_source_is_defaults(self, qemu_boot_factory):
        """
        The session-built image's "devcfg" partition was never written by
        anyone (esptool's merge_bin pads unlisted regions with 0xFF) --
        an erased partition is exactly the "blank board" case
        parseDeviceConfig must reject on bad magic, so main.cpp should
        fall back to the compiled-in Kconfig defaults and say so.
        """
        reader = qemu_boot_factory(None)
        telem = reader.read_telemetry(event="cfg", timeout_s=20)
        assert telem is not None, (
            f"GLOW-TEST: cfg telemetry not found within 20s. "
            f"Captured log:\n{reader.flush_logs()[-3000:]}"
        )
        assert telem.key_values.get("source") == "defaults", (
            f"expected source=defaults with no devcfg written, got {telem.key_values}"
        )

        # And the board still boots fine on those defaults -- reaching the
        # dmx begin=ok marker proves this isn't just a telemetry fluke.
        assert reader.read_until("GLOW-TEST: dmx begin=ok", timeout_s=20) is not None

    def test_devcfg_written_source_and_fields(self, qemu_boot_factory):
        """
        A valid CFG1 blob patched into the "devcfg" partition should be
        read and parsed at boot: source=devcfg, and the telemetry's
        dmx_tx/usb_midi fields should reflect what was actually written,
        not the Kconfig defaults (which use different values -- see
        firmware/main/Kconfig.projbuild's GLOW_DMX_TX_GPIO default of 17;
        this test deliberately picks a different GPIO to prove the
        partition, not the fallback, is what took effect).

        skipWifi=1 here too -- QEMU still has no WiFi model regardless of
        which path (Kconfig default or explicit devcfg) set it; this test
        is about proving devcfg's OTHER fields were read, not re-proving
        the skip-wifi boot-completes claim (see test_skip_wifi_no_stall
        below for that).
        """
        blob = encode_devcfg(
            wifi_ssid="qemu-test",
            wifi_pass="doesnotmatter",
            artnet_fallback_ip=(192 << 24) | (168 << 16) | (1 << 8) | 42,
            artnet_port=6454,
            dmx_tx_gpio=21,  # deliberately not the Kconfig default (17)
            dmx_rx_gpio=22,
            dmx_rts_gpio=23,
            led_gpio=9,
            usb_midi_host=True,
            skip_wifi=True,
        )
        reader = qemu_boot_factory(blob)
        telem = reader.read_telemetry(event="cfg", timeout_s=20)
        assert telem is not None, (
            f"GLOW-TEST: cfg telemetry not found within 20s. "
            f"Captured log:\n{reader.flush_logs()[-3000:]}"
        )
        assert telem.key_values.get("source") == "devcfg", (
            f"expected source=devcfg with a valid blob written, got {telem.key_values}"
        )
        assert telem.int_field("dmx_tx") == 21, f"dmx_tx should reflect the written devcfg, got {telem.key_values}"
        assert telem.int_field("usb_midi") == 1
        assert telem.int_field("skip_wifi") == 1

    def test_skip_wifi_no_stall(self, qemu_boot_factory):
        """
        skipWifi=1, set explicitly via devcfg (not the Kconfig default) --
        boot must still reach the render task's first `stats` tick with no
        hang. This is the same property tests/qemu's whole L0 suite
        depends on (see BOOT_READY_MARKER/read_until usage throughout) --
        here it's asserted directly against the RUNTIME cfg.skipWifi path
        (main.cpp's `if (!cfg.skipWifi)` branch), not just the Kconfig
        fallback default that test_no_devcfg_source_is_defaults exercises.
        """
        blob = encode_devcfg(
            wifi_ssid="",  # irrelevant when skip_wifi -- never read
            wifi_pass="",
            dmx_tx_gpio=17,
            dmx_rx_gpio=18,
            dmx_rts_gpio=8,
            led_gpio=2,
            usb_midi_host=False,
            skip_wifi=True,
        )
        reader = qemu_boot_factory(blob)

        telem = reader.read_telemetry(event="cfg", timeout_s=20)
        assert telem is not None
        assert telem.key_values.get("source") == "devcfg"
        assert telem.int_field("skip_wifi") == 1

        # No network stall: the render task should still reach its first
        # stats tick well within the same window L0 uses (30s) -- if
        # cfg.skipWifi were somehow not honored, wifi_start_sta() would
        # hang against QEMU's absent WiFi model and this would time out.
        stats = reader.read_telemetry(event="stats", timeout_s=30)
        assert stats is not None, (
            f"No GLOW-TEST: stats telemetry within 30s -- boot may have stalled on WiFi bring-up "
            f"despite skip_wifi=1. Captured log:\n{reader.flush_logs()[-3000:]}"
        )
        assert stats.int_field("dropped") == 0
