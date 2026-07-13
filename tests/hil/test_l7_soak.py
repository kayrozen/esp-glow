"""
L7 — Soak (the one that matters most)

10 minutes, everything at once: Art-Net streaming (always on), a cue
toggling via WS + OSC + repeated Fennel evals (50-100 msg/s mixed).

Assert throughout:
- No panic/abort/Guru Meditation/rst: reboot. Any reset = FAIL.
- stats never gaps.
- dropped stays 0 -- this is the GC test. A GC pause that overruns the
  frame slack shows up here and nowhere else (see render_pacing.h's
  PaceResult::droppedFrames). Never weaken this to "mostly 0" or average
  it away: dropped=0 is the guarantee.
- heap and lua_mem show no upward trend (leak detection).

The eval half of the mixed load intentionally re-triggers ONE already-
defined cue (glow.cue.go / glow.cue.release on :hilsoak) rather than
calling glow.cue.define repeatedly. Every glow.cue.define call allocates a
new ShowController cue slot that nothing ever frees (see show_control.h --
there is no cue-removal API), so hammering `define` at 50-100 msg/s for 10
minutes would itself manufacture the exact upward heap/lua_mem trend this
test is trying to catch, as a false positive. go()/release() on an
existing cue is idempotent and allocates nothing per call, which is what
actually stresses the eval queue + render loop + WS/OSC plumbing at
sustained rate without contaminating the leak signal.

Run this LAST (after L0-L6, L8): it is the longest test in the suite by a
wide margin, and every earlier layer is a faster, more precise way to
catch its own failure mode. See run_hil_tests.sh for enforced run order.
"""

import json
import socket
import struct
import threading
import time
from collections import deque

import pytest
import websocket

from conftest import TelemetryLine, line_is_crash

SOAK_DURATION_S = 600  # 10 minutes
HEALTH_CHECK_INTERVAL_S = 5
STATS_GAP_FAIL_S = 5.0  # >5s with no stats line == the render/telemetry task hung

# Heuristic leak thresholds, not exact-zero -- normal operation has some
# noise (fragmentation, one-time allocations settling after boot). These
# are deliberately generous starting points; if a real run trips them
# without an obvious cause, that is itself a signal worth reporting, not a
# reason to loosen the threshold further (see the agent guardrails).
HEAP_LEAK_THRESHOLD_BYTES = 32 * 1024
LUA_MEM_LEAK_THRESHOLD_BYTES = 32 * 1024


def _osc_packet(address: str, value: int) -> bytes:
    def pad4(b: bytes) -> bytes:
        return b + b"\x00" * (4 - (len(b) % 4))

    addr = pad4(address.encode() + b"\x00")
    type_tag = pad4(b",i\x00")
    return addr + type_tag + struct.pack(">i", value)


class TelemetryMonitor:
    """Background thread: parses GLOW-TEST: stats lines, watches for crashes."""

    def __init__(self, serial_reader):
        self.serial_reader = serial_reader
        self.readings = deque(maxlen=SOAK_DURATION_S + 60)
        self.crash = None
        self.last_stats_time = time.time()
        self._stop = threading.Event()
        self._thread = None

    def run(self):
        while not self._stop.is_set():
            line = self.serial_reader.readline(timeout_s=0.5)
            if line is None:
                continue
            marker = line_is_crash(line)
            if marker and self.crash is None:
                self.crash = (marker, line)
                continue
            parsed = TelemetryLine.parse(line)
            if parsed and parsed.event == "stats":
                try:
                    self.readings.append({
                        "t": time.time(),
                        "frames": parsed.int_field("frames"),
                        "behind": parsed.int_field("behind"),
                        "dropped": parsed.int_field("dropped"),
                        "heap": parsed.int_field("heap"),
                        "lua_mem": parsed.int_field("lua_mem"),
                    })
                    self.last_stats_time = time.time()
                except (KeyError, ValueError):
                    pass

    def start(self):
        self._thread = threading.Thread(target=self.run, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()

    def check_health(self) -> str:
        """Returns "" if healthy, else a failure description."""
        if self.crash:
            marker, line = self.crash
            return f"crash marker '{marker}' seen on serial: {line!r}"

        gap = time.time() - self.last_stats_time
        if gap > STATS_GAP_FAIL_S:
            return f"stats gap of {gap:.1f}s (>{STATS_GAP_FAIL_S}s) -- render/telemetry task may have hung"

        total_dropped = sum(r["dropped"] for r in self.readings)
        if total_dropped > 0:
            return (
                f"dropped={total_dropped} across the soak so far -- GC (or something else) "
                "is overrunning the frame slack"
            )

        if len(self.readings) >= 20:
            readings = list(self.readings)
            q = max(len(readings) // 4, 1)
            first_quarter = readings[:q]
            last_quarter = readings[-q:]
            first_heap = sum(r["heap"] for r in first_quarter) / len(first_quarter)
            last_heap = sum(r["heap"] for r in last_quarter) / len(last_quarter)
            if first_heap - last_heap > HEAP_LEAK_THRESHOLD_BYTES:
                return f"heap trending down: {first_heap:.0f} -> {last_heap:.0f} bytes"

            first_lua = sum(r["lua_mem"] for r in first_quarter) / len(first_quarter)
            last_lua = sum(r["lua_mem"] for r in last_quarter) / len(last_quarter)
            if last_lua - first_lua > LUA_MEM_LEAK_THRESHOLD_BYTES:
                return f"lua_mem trending up: {first_lua:.0f} -> {last_lua:.0f} bytes"

        return ""


class MixedLoadGenerator:
    """WS cue toggles + OSC + repeated (idempotent) Fennel evals, mixed."""

    def __init__(self, device_ip: str):
        self.device_ip = device_ip
        self._stop = threading.Event()
        self.ws = None
        self.osc_sock = None
        self.seq = 0

    def setup_cue(self):
        """One-time glow.cue.define -- the load loop only go()s/release()s it after this."""
        self.ws = websocket.create_connection(f"ws://{self.device_ip}/ws", timeout=5)
        self.ws.settimeout(0.3)
        try:
            while True:
                self.ws.recv()
        except Exception:
            pass
        self.seq += 1
        self.ws.send(json.dumps({
            "type": "eval", "seq": self.seq,
            "src": "(glow.cue.define :hilsoak {:effects [(fn [t] (glow.set 0 :dimmer 1.0))] :priority 0})",
        }))
        self.osc_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def run(self, rate_hz: float):
        interval = 1.0 / rate_hz
        toggle = False
        i = 0
        while not self._stop.is_set():
            toggle = not toggle
            kind = i % 3
            try:
                if kind == 0:
                    self.ws.send(json.dumps({"type": "cue", "id": 0, "pressed": toggle}))
                elif kind == 1:
                    self.osc_sock.sendto(_osc_packet("/cue/0", 1 if toggle else 0), (self.device_ip, 9000))
                else:
                    self.seq += 1
                    src = "(glow.cue.go :hilsoak)" if toggle else "(glow.cue.release :hilsoak)"
                    self.ws.send(json.dumps({"type": "eval", "seq": self.seq, "src": src}))
                # Drain (and discard) whatever's queued so the WS recv
                # buffer doesn't grow unbounded over 10 minutes.
                self.ws.settimeout(0.001)
                try:
                    while True:
                        self.ws.recv()
                except Exception:
                    pass
            except Exception:
                pass  # a dropped send here is load-generator noise, not a device failure
            i += 1
            time.sleep(interval)

    def stop(self):
        self._stop.set()
        if self.ws:
            try:
                self.ws.close()
            except Exception:
                pass
        if self.osc_sock:
            try:
                self.osc_sock.close()
            except Exception:
                pass


class TestSoak:
    @pytest.mark.timeout(SOAK_DURATION_S + 120)
    def test_10min_mixed_load(self, device_reset, serial_reader, device_ip: str):
        device_reset()

        monitor = TelemetryMonitor(serial_reader)
        monitor.start()

        load = MixedLoadGenerator(device_ip)
        load.setup_cue()
        # 60 Hz -> ~60 msg/s across WS cue/eval + OSC, within the spec's 50-100 msg/s mixed load.
        load_thread = threading.Thread(target=load.run, args=(60,), daemon=True)
        load_thread.start()

        try:
            start = time.time()
            while time.time() - start < SOAK_DURATION_S:
                time.sleep(HEALTH_CHECK_INTERVAL_S)
                failure = monitor.check_health()
                assert not failure, f"Soak health check failed at {time.time() - start:.0f}s: {failure}"

            failure = monitor.check_health()
            assert not failure, f"Soak test failed: {failure}"
        finally:
            load.stop()
            monitor.stop()
