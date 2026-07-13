"""
L8 — F5 robustness (when F5 lands)

Target behavior per the task spec: OTA swaps slots and self-validates; a
deliberately broken image rolls back. Pull the AP -> the rig keeps
rendering DMX, reconnects with backoff. Hang the render task -> WDT
reboot.

Honest boundary (see the agent guardrails: report a wrong/premature
assertion rather than write a fake-passing one): as of this branch, F5
has NOT landed --

- No esp_ota_* usage anywhere in firmware/ -- there is no OTA update
  handler, no dual-slot self-validate, no rollback. Those tests are
  skipped outright; there is nothing to flash or assert against yet.
- WiFi reconnect-with-backoff IS implemented (wifi_manager.cpp's
  reconnect_task, capped at 15s) -- but "pull the AP" is a physical
  action this Python harness has no lever for (no test-controlled AP in
  this suite's scope), so it's marked semi-automated/skipped rather than
  faked with a fixed sleep the harness can't actually correlate to an AP
  outage. A human (or a router-API-integrated harness) can verify it by
  power-cycling the AP and watching for "reconnect attempt (backoff=Xs)"
  followed by "got ip: ..." on the serial log.
- No esp_task_wdt_add() call anywhere ties the render task to the task
  watchdog (CONFIG_ESP_TASK_WDT is on, but only for idle-task starvation,
  not a hung render task specifically) -- "hang the render task -> WDT
  reboot" is skipped; there's no wiring yet to reboot from.

What IS tested below is real, already-implemented robustness: clean boot,
repeated resets, and that boot telemetry survives a bundle-load failure
(covered more precisely by L3's own corrupt-bundle test, but this module
adds repeated-reset coverage on top).
"""

import time

import pytest


class TestBootRobustness:
    def test_boot_into_valid_state(self, device_reset, serial_reader):
        device_reset()  # raises if "GLOW-TEST: dmx begin=ok" doesn't arrive
        assert any("GLOW-TEST: boot" in line for line in serial_reader.logs)

    def test_no_crash_on_repeated_resets(self, device_reset, serial_reader):
        """5 consecutive resets, each reaching a clean boot -- brownout/reset-sequence regression guard."""
        for i in range(5):
            device_reset()
            assert any("GLOW-TEST: dmx begin=ok" in line for line in serial_reader.logs), (
                f"Reset {i + 1}/5: did not reach a clean boot"
            )
            telem = serial_reader.read_telemetry(event="stats", timeout_s=5)
            assert telem is not None, f"Reset {i + 1}/5: no stats after boot"
            time.sleep(0.2)

    def test_wifi_connects_after_boot(self, device_reset, serial_reader):
        device_reset(wait_for_boot=False)
        line = serial_reader.read_until("got ip:", timeout_s=20)
        assert line is not None, "WiFi did not report an IP within 20s of reset"


class TestNoCascadeFailure:
    def test_no_crash_in_extended_boot_window(self, device_reset, serial_reader):
        """A longer post-boot window than L0's -- catches a delayed cascade, not just an immediate one."""
        device_reset(wait_for_boot=False)
        serial_reader.assert_no_crash(duration_s=20.0)


class TestOTA:
    @pytest.mark.skip(reason="F5 not yet implemented: no esp_ota_* usage in firmware/ to exercise")
    def test_ota_image_swap_and_self_validate(self, device_reset, serial_reader):
        pass

    @pytest.mark.skip(reason="F5 not yet implemented: no OTA rollback path exists to test against")
    def test_ota_bad_image_rolls_back(self, device_reset, serial_reader):
        pass


class TestWiFiLoss:
    @pytest.mark.skip(
        reason="semi-automated: requires physically pulling/power-cycling the AP, which this "
        "harness has no lever for. wifi_manager.cpp's reconnect_task (bounded backoff, capped "
        "15s) is implemented -- verify manually by watching for 'reconnect attempt (backoff=Xs)' "
        "then 'got ip: ...' in the serial log after an AP outage."
    )
    def test_ap_pull_reconnect_with_backoff(self, device_reset, serial_reader):
        pass


class TestRenderTaskHang:
    @pytest.mark.skip(
        reason="F5 not yet implemented: no esp_task_wdt_add() ties the render task to the "
        "watchdog, so there is nothing to reboot from yet"
    )
    def test_render_task_hang_triggers_wdt_reboot(self, device_reset, serial_reader):
        pass
