"""
L6 — Safety guarantees (NEW, fully automated, proves the promises)

Each of these must leave the rig still rendering (stats keeps flowing, no
reset). Fennel snippets are taken verbatim (or adapted minimally) from the
host-tested guarantees in test_glow_fennel.cpp / test_fx_error_pipeline.cpp
-- this layer proves the same contracts hold on real hardware, under the
real render task and the real WS/eval-queue plumbing.
"""

import json
import time

import websocket

from conftest import ws_drain, ws_eval, ws_recv_json

WS_TIMEOUT = 5.0


class TestRuntimeEffectError:
    def test_single_broken_effect_reports_once_cue_keeps_running(self, device_reset, serial_reader, device_ip):
        """
        A runtime error in an effect: fx_error on WS naming the effect;
        GLOW-TEST: fx_disabled on serial; not retried (exactly one report);
        the cue's OTHER effect keeps running.
        """
        device_reset()
        ws = websocket.create_connection(f"ws://{device_ip}/ws", timeout=WS_TIMEOUT)
        try:
            ws_drain(ws)
            define = (
                '(glow.cue.define :hilbreak '
                '{:effects [(fn [t] (error "boom")) (fn [t] (glow.set 0 :dimmer 1.0))] '
                ':priority 0})'
            )
            r1 = ws_eval(ws, define, seq=1)
            assert r1 is not None and r1["ok"] is True, r1
            r2 = ws_eval(ws, "(glow.cue.go :hilbreak)", seq=2)
            assert r2 is not None and r2["ok"] is True, r2

            # Collect WS traffic and serial telemetry for a few seconds --
            # long enough for many render frames (44/s) to have tried (and
            # given up on) the broken effect.
            fx_errors = []
            deadline = time.time() + 3.0
            while time.time() < deadline:
                msg = ws_recv_json(ws, timeout_s=max(deadline - time.time(), 0.01))
                if msg and msg.get("type") == "fx_error":
                    fx_errors.append(msg)

            assert len(fx_errors) >= 1, "No fx_error broadcast for the broken effect"
            assert all(e.get("effect") == "hilbreak#0" for e in fx_errors), fx_errors
            assert len(fx_errors) == 1, f"Effect was reported more than once (log flood): {fx_errors}"

            fx_disabled_lines = [
                line for line in serial_reader.logs if "GLOW-TEST: fx_disabled" in line and "hilbreak#0" in line
            ]
            assert len(fx_disabled_lines) == 1, (
                f"Expected exactly one GLOW-TEST: fx_disabled line for hilbreak#0, got {fx_disabled_lines}"
            )

            # The cue's other (working) effect kept running.
            telem = serial_reader.query("?dmx0", timeout_s=3)
            assert telem is not None
            values = [int(b) for b in telem.list_field("bytes")]
            assert values[0] == 255, f"Cue-mate effect should still be driving ch0=255, got {values[0]}"

            # Rendering never stalled.
            stats = serial_reader.read_telemetry(event="stats", timeout_s=5)
            assert stats is not None
        finally:
            ws.close()

    def test_two_effects_break_same_frame_both_reported(self, device_reset, serial_reader, device_ip):
        """
        Two effects break in the same frame -> both fx_errors delivered.
        This is the bug already caught once in host tests
        (test_fx_error_pipeline.cpp); assert it on hardware too.
        """
        device_reset()
        ws = websocket.create_connection(f"ws://{device_ip}/ws", timeout=WS_TIMEOUT)
        try:
            ws_drain(ws)
            r1 = ws_eval(ws, '(glow.cue.define :hilbreak1 {:effects [(fn [t] (error "boom1"))] :priority 0})', seq=1)
            assert r1 is not None and r1["ok"] is True, r1
            r2 = ws_eval(ws, '(glow.cue.define :hilbreak2 {:effects [(fn [t] (error "boom2"))] :priority 0})', seq=2)
            assert r2 is not None and r2["ok"] is True, r2

            # go() both before the next render frame renders -- both effects
            # throw on their very first evaluate() call, the same frame.
            r3 = ws_eval(ws, "(glow.cue.go :hilbreak1)", seq=3)
            assert r3 is not None and r3["ok"] is True, r3
            r4 = ws_eval(ws, "(glow.cue.go :hilbreak2)", seq=4)
            assert r4 is not None and r4["ok"] is True, r4

            fx_errors = []
            deadline = time.time() + 3.0
            while time.time() < deadline:
                msg = ws_recv_json(ws, timeout_s=max(deadline - time.time(), 0.01))
                if msg and msg.get("type") == "fx_error":
                    fx_errors.append(msg)

            names = {e.get("effect") for e in fx_errors}
            assert "hilbreak1#0" in names and "hilbreak2#0" in names, (
                f"Expected both broken effects reported, got: {names}"
            )

            stats = serial_reader.read_telemetry(event="stats", timeout_s=5)
            assert stats is not None
        finally:
            ws.close()


class TestInfiniteLoop:
    def test_infinite_loop_aborted_render_never_stalls(self, device_reset, serial_reader, device_ip):
        """(while true nil) at the top-level REPL: the instruction hook aborts it in bounded time."""
        device_reset()
        ws = websocket.create_connection(f"ws://{device_ip}/ws", timeout=WS_TIMEOUT)
        try:
            ws_drain(ws)
            start = time.time()
            result = ws_eval(ws, "(fn f [] (while true nil)) (f)", seq=1, timeout_s=10)
            elapsed = time.time() - start

            assert result is not None, "No eval_result for the infinite loop -- did the eval channel stall?"
            assert result["ok"] is False, result
            assert elapsed < 8.0, f"Infinite loop took {elapsed:.1f}s to abort -- not bounded"

            # The render loop never stalled while this ran, and the VM is
            # still usable afterward.
            stats = serial_reader.read_telemetry(event="stats", timeout_s=5)
            assert stats is not None
            recovery = ws_eval(ws, "(glow.cue.define :hilok {:effects [] :priority 0})", seq=2)
            assert recovery is not None and recovery["ok"] is True, recovery
        finally:
            ws.close()


class TestMemoryExhaustion:
    def test_oom_aborted_rendering_continues_mem_returns_to_baseline(self, device_reset, serial_reader, device_ip):
        """A script retaining references in a loop hits the allocator cap; rendering continues; lua_mem recovers."""
        device_reset()

        baseline = serial_reader.query("?lua", timeout_s=3)
        assert baseline is not None
        baseline_mem = baseline.int_field("mem")

        ws = websocket.create_connection(f"ws://{device_ip}/ws", timeout=WS_TIMEOUT)
        try:
            ws_drain(ws)
            hog = '(local t {}) (for [i 1 5000000] (tset t i (.. "x" (tostring i))))'
            result = ws_eval(ws, hog, seq=1, timeout_s=15)
            assert result is not None, "No eval_result for the memory hog -- did the eval channel stall?"
            assert result["ok"] is False, (
                f"Expected the memory hog to be rejected (cap or instruction budget), got {result}"
            )

            # Rendering never stopped.
            stats = serial_reader.read_telemetry(event="stats", timeout_s=5)
            assert stats is not None

            # lua_mem trends back toward baseline as gcStepSlack reclaims
            # the failed allocation over the next several frames (the GC is
            # stepped, not stop-the-world -- see lua_vm.h -- so this is not
            # instantaneous).
            recovered = False
            for _ in range(10):
                time.sleep(1.0)
                reading = serial_reader.query("?lua", timeout_s=3)
                if reading is None:
                    continue
                if reading.int_field("mem") <= baseline_mem * 1.2:
                    recovered = True
                    break
            assert recovered, (
                f"lua_mem did not return near baseline ({baseline_mem}) within 10s after the OOM eval"
            )

            # The VM still accepts new evals.
            recovery = ws_eval(ws, "(glow.cue.define :hilok {:effects [] :priority 0})", seq=2)
            assert recovery is not None and recovery["ok"] is True, recovery
        finally:
            ws.close()
