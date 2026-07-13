"""
L5 — Fennel REPL / live-coding (NEW, fully automated, the headline feature)

Over WS (ws://<ip>/ws), exercising the exact protocol in web_protocol.h:

1. eval round-trip: ok:true with a matching seq.
2. Define an effect that sets a known channel -> ?dmx0 shows the new value.
3. A syntax error -> eval_result ok:false with a compile message; the rig
   keeps rendering (stats uninterrupted).
4. script_save -> script_list shows it -> reboot -> boot.fnl restored the
   show.
5. A broken boot.fnl -> the base show still renders (see this module's
   docstring on test_broken_boot_fnl_does_not_brick for why "blackout" is
   not quite what happens today) and the device still accepts evals so you
   can fix it live.

Fixture id 0 is universe 0 / channel 0, the selftest fixture's own Dimmer
fixture (see main.cpp's setup_selftest_fixture) -- glow.set 0 :dimmer 1.0
drives it to byte 255, unambiguously different from the selftest baseline
of 200 and immune to which cue's intent HTP-merge picks first (HTP takes
the max, and 255 > 200 either way).
"""

import json
import time

import pytest
import websocket

from conftest import ws_drain, ws_eval, ws_recv_json

WS_TIMEOUT = 5.0


def _delete_boot_fnl(device_ip: str) -> None:
    """
    Best-effort cleanup over WS -- deliberately independent of the
    function-scoped device_reset/serial_reader fixtures (this runs at
    module setup/teardown, a broader scope pytest won't let depend on
    them). By the time this module's tests run the device is already up
    (an earlier layer's device_reset has run at least once this session),
    so a plain WS connect is enough; retry briefly in case the device is
    mid-reset from the previous test.
    """
    last_err = None
    for _ in range(30):
        try:
            ws = websocket.create_connection(f"ws://{device_ip}/ws", timeout=WS_TIMEOUT)
            break
        except Exception as e:  # noqa: BLE001 - retrying on any connect failure
            last_err = e
            time.sleep(1.0)
    else:
        raise RuntimeError(f"Could not connect to ws://{device_ip}/ws for boot.fnl cleanup: {last_err}")

    try:
        ws_drain(ws)
        ws.send(json.dumps({"type": "script_delete", "name": "boot.fnl"}))
        ws_recv_json(ws, timeout_s=WS_TIMEOUT)  # the refreshed `scripts` reply
    finally:
        ws.close()


@pytest.fixture(scope="module", autouse=True)
def _clean_boot_fnl(flashed_selftest_firmware, device_ip):
    """Defensive cleanup: a boot.fnl left by a previous failed run must not leak into or out of this module."""
    _delete_boot_fnl(device_ip)
    yield
    _delete_boot_fnl(device_ip)


@pytest.fixture
def ws(device_reset, serial_reader, device_ip):
    conn = websocket.create_connection(f"ws://{device_ip}/ws", timeout=WS_TIMEOUT)
    ws_drain(conn)
    yield conn
    conn.close()


class TestFennelEvalRoundTrip:
    def test_eval_ok_true_matching_seq(self, ws):
        result = ws_eval(ws, "(glow.cue.define :hiltest {:effects [] :priority 0})", seq=1)
        assert result is not None, "No eval_result within timeout"
        assert result["seq"] == 1
        assert result["ok"] is True, result

    def test_defined_effect_sets_known_channel(self, ws, serial_reader):
        r1 = ws_eval(ws, "(glow.cue.define :hilset {:effects [(fn [t] (glow.set 0 :dimmer 1.0))] :priority 0})", seq=2)
        assert r1 is not None and r1["ok"] is True, r1
        r2 = ws_eval(ws, "(glow.cue.go :hilset)", seq=3)
        assert r2 is not None and r2["ok"] is True, r2

        time.sleep(0.2)  # let a render frame apply the newly-active cue
        telem = serial_reader.query("?dmx0", timeout_s=3)
        assert telem is not None
        values = [int(b) for b in telem.list_field("bytes")]
        assert values[0] == 255, f"Expected ch0=255 after live-coded glow.set, got {values[0]}"

    def test_syntax_error_ok_false_rig_keeps_rendering(self, ws, serial_reader):
        result = ws_eval(ws, "(glow.cue.go :unterminated", seq=4)
        assert result is not None, "No eval_result for a syntax error"
        assert result["ok"] is False, result
        assert result.get("err"), "ok:false eval_result must carry a compile message"

        # Rendering must not have hiccuped: stats keeps flowing right after.
        telem = serial_reader.read_telemetry(event="stats", timeout_s=5)
        assert telem is not None, "No stats after a syntax error -- render loop stalled"


class TestScriptPersistence:
    def test_save_list_reboot_restores_show(self, ws, device_reset, serial_reader, device_ip):
        src = "(glow.cue.define :hilboot {:effects [(fn [t] (glow.set 0 :dimmer 1.0))] :priority 0}) (glow.cue.go :hilboot)"
        ws.send(json.dumps({"type": "script_save", "name": "boot.fnl", "src": src}))
        saved = ws_recv_json(ws, timeout_s=WS_TIMEOUT)
        assert saved is not None and saved.get("type") == "scripts", saved
        assert "boot.fnl" in saved.get("names", []), saved

        ws.send(json.dumps({"type": "script_list"}))
        listed = ws_recv_json(ws, timeout_s=WS_TIMEOUT)
        assert listed is not None and "boot.fnl" in listed.get("names", []), listed

        device_reset()  # full reboot -- boot.fnl now evaluates at startup
        time.sleep(0.3)
        telem = serial_reader.query("?dmx0", timeout_s=3)
        assert telem is not None
        values = [int(b) for b in telem.list_field("bytes")]
        assert values[0] == 255, (
            f"Expected ch0=255 after boot.fnl restored the live-coded cue, got {values[0]}"
        )


class TestBrokenBootFnl:
    def test_broken_boot_fnl_does_not_brick(self, ws, device_reset, serial_reader, device_ip):
        """
        A broken boot.fnl must not brick the device or stop rendering. In
        the current architecture (see main.cpp's setup_lua comment:
        "boot.fnl only ever ADDS Lua-defined cues") boot.fnl failing simply
        leaves the base show exactly as setup_selftest_fixture left it --
        there is no separate "blackout" state to fall into, because
        nothing was torn down. This asserts what's actually true (no
        crash, base show keeps rendering, the VM stays live for further
        evals) rather than a literal blackout the current code doesn't
        implement.
        """
        ws.send(json.dumps({"type": "script_save", "name": "boot.fnl", "src": '(error "deliberately broken boot.fnl")'}))
        saved = ws_recv_json(ws, timeout_s=WS_TIMEOUT)
        assert saved is not None and "boot.fnl" in saved.get("names", []), saved

        device_reset()  # must still boot cleanly -- no crash loop
        serial_reader.assert_no_crash(duration_s=5.0)

        # Base show (the selftest fixture) is still rendering.
        telem = serial_reader.query("?dmx0", timeout_s=3)
        assert telem is not None
        values = [int(b) for b in telem.list_field("bytes")]
        assert values[0] == 200, f"Base show should still be rendering (ch0=200), got {values[0]}"

        # The VM is still alive and accepts new evals -- "fix it live".
        ws2 = websocket.create_connection(f"ws://{device_ip}/ws", timeout=WS_TIMEOUT)
        try:
            ws_drain(ws2)
            result = ws_eval(ws2, "(glow.cue.define :recover {:effects [] :priority 0})", seq=99)
            assert result is not None and result["ok"] is True, (
                f"VM did not accept a new eval after a broken boot.fnl: {result}"
            )
        finally:
            ws2.close()
