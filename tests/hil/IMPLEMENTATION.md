# HIL Test Suite — Implementation Guide

This document describes the automated hardware-in-the-loop (HIL) test harness structure, design patterns, and how to adapt it to your firmware.

## Directory Structure

```
tests/hil/
├── conftest.py              # Shared fixtures: SerialReader, device_reset, etc.
├── test_l0_boot.py          # L0: Boot/POST tests
├── test_l1_dmx.py           # L1: DMX output tests
├── test_l2_artnet.py        # L2: Art-Net output tests
├── test_l3_show_load.py     # L3: Show bundle loading
├── test_l4_inputs.py        # L4: Web/OSC/MIDI inputs
├── test_l5_soak.py          # L5: Stress test (10 min concurrent load)
├── test_l6_ota_robustness.py # L6: OTA and fault recovery
├── pytest.ini               # Pytest configuration
├── requirements.txt         # Python dependencies
├── run_hil_tests.sh        # Convenient test runner script
├── README.md                # User guide and reference
├── IMPLEMENTATION.md        # This file
└── __init__.py              # Package marker
```

## Core Design Patterns

### 1. Serial Communication (`SerialReader` in conftest.py)

```python
reader = SerialReader(port="/dev/ttyUSB0", baudrate=115200)
reader.open()

# Read one line with timeout
line = reader.readline(timeout_s=2.0)

# Read until pattern found
line = reader.read_until("pattern", timeout_s=5.0)

# Read telemetry (GLOW-TEST: prefix)
telem = reader.read_telemetry(timeout_s=2.0)
# Returns: TelemetryLine(raw=..., key_values={'key': 'value', ...})
```

**Key features:**
- Non-blocking reads with configurable timeout
- Automatic `GLOW-TEST:` telemetry parsing
- Full serial log capture for debugging
- Buffer management (handles partial lines)

### 2. Telemetry Parsing (`TelemetryLine`)

Expected firmware format:
```
[other junk] GLOW-TEST: key1=value1 key2=value2 key3=value3
```

Parsed into:
```python
{
  'raw': '[full line]',
  'key_values': {
    'key1': 'value1',
    'key2': 'value2',
    'key3': 'value3'
  }
}
```

**Firmware responsibility:**
Add a tiny struct-to-serial function:
```cpp
// On boot:
printf("GLOW-TEST: boot core=%d hz=%d\n", core_id, frame_rate_hz);

// After DMX init:
printf("GLOW-TEST: dmx begin=ok\n");

// After bundle load:
printf("GLOW-TEST: bundle fixtures=%u matrices=%u\n", fixtures, matrices);

// Every render frame (1 Hz output):
printf("GLOW-TEST: stats frames=%u behind=%u heap=%u\n", 
       frame_count, dropped_frames, free_heap);
```

### 3. Device Reset via Serial RTS

```python
@pytest.fixture
def device_reset(serial_reader: SerialReader) -> callable:
    def reset():
        if serial_reader.ser:
            serial_reader.ser.reset_input_buffer()
            serial_reader.ser.reset_output_buffer()
            
            # Toggle RTS to trigger ESP32 reset
            serial_reader.ser.rts = True
            time.sleep(0.1)
            serial_reader.ser.rts = False
            time.sleep(0.5)
```

This is ESP32-specific: RTS control line connects to EN (reset) on DevKit boards.

### 4. Pytest Fixtures (Dependency Injection)

```python
@pytest.fixture
def serial_reader(serial_port: str) -> Iterator[SerialReader]:
    """Yields a SerialReader, open and ready."""
    reader = SerialReader(serial_port)
    reader.open()
    time.sleep(0.5)
    # Clear stale data
    if reader.ser and reader.ser.in_waiting > 0:
        reader.ser.read(1024)
    reader.buffer = ""
    
    yield reader  # Test runs here
    
    # Cleanup and log
    logger.info(f"Device logs:\n{reader.flush_logs()}")
    reader.close()

# Usage in test:
def test_something(device_reset, serial_reader):
    device_reset()
    line = serial_reader.readline()
```

### 5. Network Tests (Art-Net, OSC, WebSocket)

#### Art-Net Listener (L2)
```python
class ArtNetListener:
    def start(self):
        self.sock = socket.socket(AF_INET, SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", 6454))
        self.thread = threading.Thread(self._listen_loop, daemon=True)
        self.thread.start()
    
    def _listen_loop(self):
        while self.running:
            data, addr = self.sock.recvfrom(1024)
            self.packets.append(data)
    
    def get_packets(self) -> List[bytes]:
        return [data for data, _ in self.packets]

# Usage:
listener = ArtNetListener()
listener.start()
time.sleep(2.0)
packets = listener.get_packets()  # All packets received in 2s
listener.stop()
```

#### WebSocket Client (L4)
```python
import websocket
import json

ws = websocket.create_connection("ws://192.168.1.100/ws", timeout=5)

# Send JSON command
ws.send(json.dumps({"type": "cue", "id": 1, "pressed": True}))

# Receive response
response = ws.recv()
data = json.loads(response)

ws.close()
```

#### OSC Sender (L4)
```python
def send_osc_cue(device_ip: str, cue_id: int):
    sock = socket.socket(AF_INET, SOCK_DGRAM)
    
    # OSC packet: /esp-glow/full <int>
    addr = b"/esp-glow/full\x00\x00"[:16]  # Null-padded to 4 bytes
    type_tag = b",i\x00\x00"               # Integer argument
    arg = cue_id.to_bytes(4, "big")
    
    pkt = addr + type_tag + arg
    sock.sendto(pkt, (device_ip, 9000))
    sock.close()
```

### 6. Stress Testing (`test_l5_soak.py`)

Concurrent load generator:
```python
class LoadGenerator:
    def generate_load_thread(self, rate_hz=50):
        interval = 1.0 / rate_hz
        cue_id = 0
        while self.running:
            if cue_id % 2 == 0:
                self.send_osc_cue(cue_id % 8)
            else:
                self.send_ws_cue(cue_id % 8)
            cue_id += 1
            time.sleep(interval)

# Run in background while monitoring device health
loader = LoadGenerator(device_ip)
loader.start()
load_thread = threading.Thread(target=loader.generate_load_thread, args=(50,))
load_thread.start()

# Monitor for 10 minutes
while time.time() - start < 600:
    # Check stats, heap, crashes
    time.sleep(5)

loader.stop()
```

Health checks during soak:
```python
class SoakMonitor:
    def check_health(self):
        # 1. Any crashes?
        if self.crashes_found:
            return False, "Crashes found"
        
        # 2. Stats timeout? (>5s silence = device hung)
        if time.time() - self.last_stats_time > 5.0:
            return False, "Stats timeout (device may have crashed)"
        
        # 3. Memory leak? (heap trending down)
        if readings[-1]["heap"] - readings[0]["heap"] < -10000:
            return False, "Memory leak detected"
        
        return True, "Device healthy"
```

## Adding New Layers

To add a new test layer (e.g., L7: Custom feature):

1. **Create test file** `tests/hil/test_l7_custom.py`

2. **Import fixtures**:
```python
from conftest import SerialReader, TelemetryLine, pytest
```

3. **Define test class**:
```python
class TestCustomFeature:
    """L7: Custom feature validation."""
    
    def test_something(self, device_reset, serial_reader: SerialReader):
        device_reset()
        
        # Wait for boot
        line = serial_reader.read_until("boot core=", timeout_s=10)
        assert line is not None
        
        # Test custom feature
        line = serial_reader.read_until("GLOW-TEST: custom_event", timeout_s=5)
        assert line is not None
        
        telem = TelemetryLine.parse(line)
        assert telem["custom_key"] == expected_value
```

4. **Add pytest marker** in `pytest.ini`:
```ini
[pytest]
markers =
    l7: Layer 7 - Custom feature tests
```

5. **Document** the test in `README.md`.

## Adapting to Your Firmware

### Step 1: Implement Firmware Observability (Phase 0)

Your firmware must emit telemetry. Add these prints:

```cpp
// Boot event
void app_main() {
    printf("GLOW-TEST: boot core=%d hz=%d\n", core_id, 44);
    
    // ... initialization ...
    
    printf("GLOW-TEST: dmx begin=ok\n");
    printf("GLOW-TEST: bundle fixtures=42 matrices=2\n");
}

// Render loop (1 Hz stats)
void render_task(void *arg) {
    uint32_t frame_count = 0;
    while (true) {
        // ... render one frame ...
        
        if (frame_count % 44 == 0) {  // Once per second at 44 Hz
            printf("GLOW-TEST: stats frames=%u behind=%u heap=%u\n",
                   frame_count, behind_count, esp_get_free_heap_size());
        }
        frame_count++;
    }
}

// Serial command handler
void handle_serial_command(const char *cmd) {
    if (strcmp(cmd, "?dmx0") == 0) {
        printf("Universe 0: ");
        for (int i = 0; i < 512; i++) {
            printf("%u ", dmx_universe[0][i]);
        }
        printf("\n");
    }
    else if (strcmp(cmd, "?state") == 0) {
        printf("Active cues: %u\n", active_cue_id);
    }
}
```

### Step 2: Install Python Dependencies

```bash
pip install -r tests/hil/requirements.txt
```

### Step 3: Configure Environment

```bash
export GLOW_SERIAL_PORT=/dev/ttyUSB0
export GLOW_DEVICE_IP=192.168.1.100
```

### Step 4: Run Tests

```bash
cd tests/hil
pytest -v                    # All quick tests
pytest -v -m "not slow"     # No soak
pytest test_l0_boot.py -v   # Only L0
```

### Step 5: Debug Failures

```bash
# Capture full logs
pytest test_l0_boot.py -v --log-file=/tmp/hil.log --log-file-level=DEBUG

# Watch device serial in real-time
screen /dev/ttyUSB0 115200

# Inspect Art-Net packets with tcpdump
tcpdump -i any -n "udp port 6454" -XX
```

## Common Patterns in Tests

### Pattern 1: Wait for event, assert property

```python
def test_something(device_reset, serial_reader):
    device_reset()
    
    line = serial_reader.read_until("GLOW-TEST: event", timeout_s=5)
    assert line is not None, "Event not received"
    
    telem = TelemetryLine.parse(line)
    value = int(telem.key_values["key"])
    assert value > 0, "Value should be positive"
```

### Pattern 2: Observe for duration, collect data

```python
def test_something(device_reset, serial_reader):
    device_reset()
    
    start = time.time()
    readings = []
    
    while time.time() - start < 10:
        line = serial_reader.readline(timeout_s=0.5)
        if line and "GLOW-TEST: metric" in line:
            telem = TelemetryLine.parse(line)
            readings.append(telem.key_values["value"])
    
    assert len(readings) > 5, "Not enough data points"
    assert all(r > 0 for r in readings), "All values should be positive"
```

### Pattern 3: Send input, observe response

```python
def test_something(device_reset, serial_reader, device_ip):
    device_reset()
    serial_reader.read_until("dmx begin=ok", timeout_s=10)
    
    # Send UDP packet
    sock = socket.socket(AF_INET, SOCK_DGRAM)
    sock.sendto(b"test_data", (device_ip, 9000))
    sock.close()
    
    # Observe response
    line = serial_reader.read_until("GLOW-TEST: response", timeout_s=2)
    assert line is not None, "No response to input"
```

### Pattern 4: Stress test with health monitoring

```python
def test_soak(device_reset, serial_reader, device_ip):
    device_reset()
    serial_reader.read_until("dmx begin=ok", timeout_s=10)
    
    # Start background load
    loader = LoadGenerator(device_ip)
    loader.start()
    
    # Monitor for 10 minutes
    start = time.time()
    crash_markers = ["panic", "abort"]
    found_crash = None
    
    while time.time() - start < 600:
        line = serial_reader.readline(timeout_s=0.5)
        if line:
            for marker in crash_markers:
                if marker in line:
                    found_crash = marker
        
        # Periodic health check
        if int(time.time() - start) % 30 == 0:
            print(f"Soak running: {int(time.time() - start)}s elapsed")
    
    loader.stop()
    assert found_crash is None, f"Crash detected: {found_crash}"
```

## Troubleshooting Implementation

### "No GLOW-TEST: telemetry lines received"

**Check:**
1. Firmware is actually printing the lines (check device serial directly)
2. Serial baud rate matches (115200 default)
3. Telemetry format exactly matches `GLOW-TEST: key=value`
4. Newline is being sent (`\n`)

**Debug:**
```bash
# Raw serial monitor
screen /dev/ttyUSB0 115200

# Or use pyserial directly
python3 -c "
import serial
s = serial.Serial('/dev/ttyUSB0', 115200)
for i in range(100):
    print(s.readline().decode('utf-8', errors='replace'))
"
```

### "Tests pass locally but fail in CI"

**Common causes:**
1. Device IP hardcoded → set `GLOW_DEVICE_IP` env var
2. Serial port hardcoded → set `GLOW_SERIAL_PORT` env var
3. Timing assumptions → add margins to timeouts
4. Network isolation in CI → Art-Net tests need same network

**Fix:**
```bash
# CI setup
export GLOW_SERIAL_PORT=$CI_SERIAL_PORT
export GLOW_DEVICE_IP=$CI_DEVICE_IP
pytest tests/hil/ -v
```

### "Art-Net test fails: no packets"

**Check:**
1. Device IP is correct (`ping $GLOW_DEVICE_IP`)
2. Device is on same LAN as host
3. Firewall allows UDP 6454 (`sudo ufw allow 6454/udp`)
4. Firmware is configured to send Art-Net (check build flags)

**Debug:**
```bash
# Listen for Art-Net with tcpdump
sudo tcpdump -i any -n "udp port 6454" -XX

# Or in Python
import socket
sock = socket.socket(AF_INET, SOCK_DGRAM)
sock.bind(("0.0.0.0", 6454))
data, addr = sock.recvfrom(1024)
print(f"Got {len(data)} bytes from {addr}")
```

### "L5 soak test fails with 'Stats timeout'"

**Likely cause:** Device crashed or hung.

**Diagnosis:**
1. Check serial logs for panic/Guru Meditation
2. Check heap trend (is it leaking?)
3. Verify stats are being generated on idle device

**If after control-event-queue integration:** This is expected to pass. If it fails, there's still a race.

## Performance Notes

- **L0–L4:** <30 seconds total
- **L5:** 10 minutes (designed to surface races)
- **L6:** <30 seconds
- **Total with soak:** ~11–12 minutes

Optimize for CI:
```bash
# Quick smoke test
pytest -m "not slow" -v                  # ~30s

# Full validation
pytest -v                                 # ~12 min
```

## Maintenance

- **When firmware changes:** Update telemetry format in `conftest.py`
- **When new features added:** Add corresponding L4/L5/L6 test
- **When protocol changes:** Update Art-Net/OSC/WebSocket parsing
- **When moving to new hardware:** Check RTS reset sequence, baud rate, device IP

## References

- Pytest docs: https://docs.pytest.org/
- ESP32 serial pins: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/uart.html
- Art-Net packet format: https://art-net.org.uk/structure/
- OSC spec: http://opensoundcontrol.org/spec-1_0
- WebSocket API: https://en.wikipedia.org/wiki/WebSocket
