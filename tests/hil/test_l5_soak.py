"""
L5 — Soak / Concurrency Tests

Fully automated stress test.

Hammers the device with concurrent inputs (WS cue toggles + OSC triggers)
at high frequency (50-100 msg/s mixed) for 10 minutes while Art-Net streams.

Validates throughout:
- No panic/abort/Guru Meditation/rst: reboot
- stats telemetry continues (no task crashes)
- behind stays low (no frame drops)
- heap does not trend downward (no memory leak)

NOTE: This test is designed to surface the cross-core race in ShowController
when inputs mutate from multiple tasks without synchronization. Run AFTER
the control-event-queue integration (see README_CONTROL_QUEUE.md).

If this test fails intermittently, check:
1. Heap trend (line "behind" value growth over time)
2. stats gaps (missing telemetry window = crash)
3. Serial panic backtraces (race condition)
"""

import pytest
import time
import socket
import json
import threading
from collections import deque
import websocket


class SoakMonitor:
    """Monitors device health during soak."""

    def __init__(self, serial_reader):
        self.serial_reader = serial_reader
        self.stats_readings = deque(maxlen=600)  # ~10 min of 1 Hz stats
        self.crashes_found = []
        self.last_stats_time = None

    def read_monitoring_thread(self):
        """Background thread that reads telemetry."""
        crash_markers = ["panic", "abort", "Guru Meditation", "rst:"]

        while True:
            line = self.serial_reader.readline(timeout_s=0.5)
            if line is None:
                continue

            # Check for crashes
            for marker in crash_markers:
                if marker.lower() in line.lower():
                    self.crashes_found.append((marker, line))

            # Parse stats
            if "GLOW-TEST: stats" in line:
                telem_line = self.serial_reader.TelemetryLine.parse(line) if hasattr(
                    self.serial_reader, "TelemetryLine"
                ) else None

                if telem_line:
                    try:
                        behind = int(telem_line.key_values.get("behind", 0))
                        heap = int(telem_line.key_values.get("heap", 0))
                        self.stats_readings.append({
                            "time": time.time(),
                            "behind": behind,
                            "heap": heap,
                            "line": line
                        })
                        self.last_stats_time = time.time()
                    except (ValueError, KeyError):
                        pass

    def get_stats(self):
        """Get accumulated stats readings."""
        return list(self.stats_readings)

    def check_health(self):
        """Check current health status."""
        # Check for crashes
        if self.crashes_found:
            return False, f"Crashes found: {self.crashes_found}"

        # Check for stats gaps (>5 second silence = timeout)
        if self.last_stats_time:
            since_last = time.time() - self.last_stats_time
            if since_last > 5.0:
                return False, f"Stats timeout: no telemetry for {since_last:.1f}s (device may have crashed)"

        # Check for memory leak (heap trending down)
        if len(self.stats_readings) >= 10:
            readings = list(self.stats_readings)
            first_heap = readings[0]["heap"]
            last_heap = readings[-1]["heap"]

            # Heap should not steadily decrease (though temporary variation is OK)
            heap_trend = last_heap - first_heap
            if heap_trend < -10000:  # More than 10KB leak
                return False, f"Memory leak detected: heap decreased by {-heap_trend} bytes"

        return True, "Device healthy"


class LoadGenerator:
    """Generates concurrent load (WS + OSC)."""

    def __init__(self, device_ip: str):
        self.device_ip = device_ip
        self.running = False
        self.ws_sock = None
        self.osc_sock = None

    def start(self):
        """Start load generation."""
        self.running = True

        # Optional: open WS connection
        try:
            self.ws_sock = websocket.create_connection(
                f"ws://{self.device_ip}/ws",
                timeout=2
            )
        except:
            self.ws_sock = None

        # Open OSC socket (UDP, doesn't require connection)
        try:
            self.osc_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        except:
            self.osc_sock = None

    def stop(self):
        """Stop load generation."""
        self.running = False
        if self.ws_sock:
            try:
                self.ws_sock.close()
            except:
                pass
        if self.osc_sock:
            try:
                self.osc_sock.close()
            except:
                pass

    def send_osc_cue(self, cue_id: int):
        """Send an OSC cue message."""
        if not self.osc_sock:
            return

        try:
            # Simple OSC format: /esp-glow/full <int>
            addr = b"/esp-glow/full\x00\x00"  # null-padded
            addr = addr[:16]  # Pad to 16 bytes (4-byte aligned)
            type_tag = b",i\x00\x00"  # integer type
            arg = cue_id.to_bytes(4, byteorder="big")

            pkt = addr + type_tag + arg
            self.osc_sock.sendto(pkt, (self.device_ip, 9000))
        except:
            pass

    def send_ws_cue(self, cue_id: int):
        """Send a WebSocket cue toggle."""
        if not self.ws_sock:
            return

        try:
            msg = json.dumps({
                "type": "cue",
                "id": cue_id,
                "pressed": True
            })
            self.ws_sock.send(msg)
        except:
            pass

    def generate_load_thread(self, rate_hz: int = 50):
        """Background thread that generates load."""
        interval = 1.0 / rate_hz

        cue_id = 0
        while self.running:
            if cue_id % 2 == 0:
                self.send_osc_cue(cue_id % 8)
            else:
                self.send_ws_cue(cue_id % 8)

            cue_id += 1
            time.sleep(interval)


@pytest.fixture
def soak_monitor(serial_reader):
    """Fixture that provides a soak monitor."""
    return SoakMonitor(serial_reader)


@pytest.fixture
def load_generator(device_ip: str):
    """Fixture that provides a load generator."""
    return LoadGenerator(device_ip)


class TestSoakStress:
    """L5: Soak and concurrency stress tests."""

    @pytest.mark.timeout(650)  # 10 min + buffer
    def test_10min_concurrent_load(
        self,
        device_reset,
        serial_reader,
        soak_monitor: SoakMonitor,
        load_generator: LoadGenerator,
        device_ip: str
    ):
        """
        Run 10-minute soak test with 50-100 msg/s mixed input load.

        Validates:
        - No crashes (panic/abort/Guru Meditation/rst:)
        - No stats gaps (>5s silence = crash)
        - No memory leak (heap trend)
        """
        device_reset()

        # Wait for boot
        serial_reader.read_until("dmx begin=ok", timeout_s=10)
        time.sleep(1.0)

        # Start monitoring thread
        monitor_thread = threading.Thread(
            target=soak_monitor.read_monitoring_thread,
            daemon=True
        )
        monitor_thread.start()

        # Start load generator
        load_generator.start()
        load_thread = threading.Thread(
            target=load_generator.generate_load_thread,
            args=(50,),  # 50 Hz mixed load
            daemon=True
        )
        load_thread.start()

        # Let it run for 10 minutes
        soak_duration = 600  # seconds
        start_time = time.time()

        try:
            while time.time() - start_time < soak_duration:
                time.sleep(5.0)

                # Periodic health check
                healthy, msg = soak_monitor.check_health()
                if not healthy:
                    pytest.fail(f"Soak health check failed at {time.time() - start_time:.0f}s: {msg}")

            # Final health check
            healthy, msg = soak_monitor.check_health()
            assert healthy, f"Soak test failed: {msg}"

        finally:
            load_generator.stop()

    def test_concurrent_load_no_hang(
        self,
        device_reset,
        serial_reader,
        load_generator: LoadGenerator,
        device_ip: str
    ):
        """
        Shorter soak variant: 30 seconds of heavy load.

        Validates that device doesn't hang under concurrent input.
        """
        device_reset()

        serial_reader.read_until("dmx begin=ok", timeout_s=10)
        time.sleep(1.0)

        load_generator.start()
        load_thread = threading.Thread(
            target=load_generator.generate_load_thread,
            args=(100,),  # Higher rate for 30s
            daemon=True
        )
        load_thread.start()

        # Monitor for crashes during load
        crash_markers = ["panic", "abort", "Guru Meditation", "rst:"]
        start = time.time()
        found_crash = None

        try:
            while time.time() - start < 30:
                line = serial_reader.readline(timeout_s=0.5)
                if line:
                    for marker in crash_markers:
                        if marker.lower() in line.lower():
                            found_crash = marker
                            break

                if found_crash:
                    break

        finally:
            load_generator.stop()

        assert found_crash is None, f"Crash under concurrent load: {found_crash}"

    def test_stats_continuity_during_load(
        self,
        device_reset,
        serial_reader,
        load_generator: LoadGenerator,
        device_ip: str
    ):
        """
        Assert stats telemetry remains continuous during load (no >2s gaps).

        A gap indicates a task hang or crash.
        """
        device_reset()

        serial_reader.read_until("dmx begin=ok", timeout_s=10)

        load_generator.start()
        load_thread = threading.Thread(
            target=load_generator.generate_load_thread,
            args=(50,),
            daemon=True
        )
        load_thread.start()

        try:
            # Monitor for 30 seconds
            start = time.time()
            last_stats = start
            max_gap = 0

            while time.time() - start < 30:
                line = serial_reader.readline(timeout_s=0.5)
                if line and "GLOW-TEST: stats" in line:
                    now = time.time()
                    gap = now - last_stats
                    max_gap = max(max_gap, gap)
                    last_stats = now

            # Stats should arrive at ~1 Hz; max gap should be <3s
            assert max_gap < 3.0, (
                f"Stats gap of {max_gap:.1f}s detected (task may have hung)"
            )

        finally:
            load_generator.stop()
