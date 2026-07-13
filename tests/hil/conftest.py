"""
HIL Test Harness Configuration and Fixtures.

Talks to a real ESP32-S3 running a CONFIG_GLOW_SELFTEST build (see the
firmware's Kconfig.projbuild and firmware/main/main.cpp's "Phase 0: HIL
selftest observability" section) over USB serial + LAN.

Environment variables:
    GLOW_SERIAL_PORT     Serial port the board enumerates as. Default /dev/ttyUSB0.
    GLOW_DEVICE_IP       Device's LAN IP (WS/OSC/Art-Net tests). No default --
                         tests that need it fail loudly if unset.
    GLOW_FIRMWARE_DIR    Path to the firmware/ ESP-IDF project. Default
                         "<repo>/firmware".
    GLOW_IDF_BUILD_DIR   Build directory for the selftest build. Default
                         "<firmware>/build-hil-selftest" (kept separate from
                         a developer's ordinary "build/" so this suite never
                         clobbers -- or is clobbered by -- a release build).
    GLOW_SKIP_FLASH      If set (any value), skip the build+flash step and
                         assume the board already has a selftest build on
                         it. Useful for iterating on the test suite itself
                         without a multi-minute reflash every run.

Requires `idf.py` on PATH (i.e. an ESP-IDF environment already sourced --
`. $IDF_PATH/export.sh`) unless GLOW_SKIP_FLASH is set.
"""

import json
import logging
import os
import shutil
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterator, Optional

import pytest
import serial

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

REPO_ROOT = Path(__file__).resolve().parents[2]
HIL_DIR = Path(__file__).resolve().parent

# The "show" raw data partition's flash offset + size (see
# firmware/partitions.csv). Not derived at runtime -- kept in sync with
# partitions.csv by hand, same as firmware/main/CMakeLists.txt's own
# esptool_py_flash_to_partition() call. If partitions.csv's "show" line
# moves or is resized, update these too.
SHOW_PARTITION_OFFSET = 0x620000
SHOW_PARTITION_SIZE = 0x100000

CRASH_MARKERS = ("panic", "abort", "guru meditation", "rst:", "backtrace:")


def line_is_crash(line: str) -> Optional[str]:
    """Returns the matched crash marker if `line` looks like a firmware crash, else None."""
    low = line.lower()
    for marker in CRASH_MARKERS:
        if marker in low:
            return marker
    return None


@dataclass
class TelemetryLine:
    """
    A parsed `GLOW-TEST: <event> key=value...` line.

    `event` is the first token after the prefix ("boot", "dmx", "artnet",
    "bundle", "scripts", "stats", "fx_disabled", "dmx0", "state", "lua").
    Everything after "err=" (if present) is taken verbatim to end-of-line as
    the "err" value -- Lua error messages contain spaces and can't be
    whitespace-tokenized like the other key=value pairs.
    """

    raw: str
    event: str
    key_values: dict = field(default_factory=dict)

    @classmethod
    def parse(cls, line: str) -> Optional["TelemetryLine"]:
        idx = line.find("GLOW-TEST:")
        if idx == -1:
            return None
        content = line[idx + len("GLOW-TEST:"):].strip()
        if not content:
            return None

        parts = content.split(None, 1)
        event = parts[0]
        rest = parts[1] if len(parts) > 1 else ""

        kv: dict = {}
        err_marker = "err="
        err_idx = rest.find(err_marker)
        if err_idx != -1:
            kv["err"] = rest[err_idx + len(err_marker):]
            rest = rest[:err_idx]

        for tok in rest.split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                kv[k] = v

        return cls(raw=line, event=event, key_values=kv)

    def int_field(self, key: str) -> int:
        return int(self.key_values[key])

    def list_field(self, key: str) -> list:
        """Parse a comma-separated field (e.g. dmx0's "bytes", state's "cues"). Empty/"none" -> []."""
        v = self.key_values.get(key, "")
        if v in ("", "none"):
            return []
        return v.split(",")


class SerialReader:
    """Manages serial port communication with the device."""

    def __init__(self, port: str, baudrate: int = 115200):
        self.port = port
        self.baudrate = baudrate
        self.ser: Optional[serial.Serial] = None
        self.buffer = ""
        self.logs: list = []

    def open(self) -> None:
        try:
            self.ser = serial.Serial(self.port, self.baudrate, timeout=0.2)
            logger.info("Opened serial port %s", self.port)
        except serial.SerialException as e:
            raise RuntimeError(f"Failed to open {self.port}: {e}") from e

    def close(self) -> None:
        if self.ser:
            self.ser.close()
            logger.info("Closed serial port %s", self.port)

    def readline(self, timeout_s: float = 5.0) -> Optional[str]:
        """Read one line from serial with timeout. Returns the line (no trailing newline) or None."""
        if not self.ser:
            raise RuntimeError("Serial port not open")

        start = time.time()
        while time.time() - start < timeout_s:
            if "\n" in self.buffer:
                line, self.buffer = self.buffer.split("\n", 1)
                line = line.rstrip("\r")
                self.logs.append(line)
                return line
            try:
                chunk = self.ser.read(1024)
                if chunk:
                    self.buffer += chunk.decode("utf-8", errors="replace")
            except Exception as e:  # pragma: no cover - hardware I/O error path
                logger.error("Serial read error: %s", e)
        return None

    def readline_retry_once(self, timeout_s: float = 5.0) -> Optional[str]:
        """
        One retry for a flaky (not a real failure) missed line -- see the
        HIL agent guardrails: retry a flaky read once, then report. Do not
        retry indefinitely.
        """
        line = self.readline(timeout_s)
        if line is not None:
            return line
        return self.readline(timeout_s)

    def read_until(self, pattern: str, timeout_s: float = 5.0) -> Optional[str]:
        """Read lines until one contains `pattern` (plain substring match). Returns the line or None."""
        start = time.time()
        while time.time() - start < timeout_s:
            remaining = timeout_s - (time.time() - start)
            line = self.readline(timeout_s=min(0.5, max(remaining, 0.01)))
            if line is None:
                continue
            if pattern in line:
                return line
        return None

    def read_telemetry(self, event: Optional[str] = None, timeout_s: float = 5.0) -> Optional[TelemetryLine]:
        """Read the next GLOW-TEST: line, optionally filtered to a specific `event`."""
        start = time.time()
        while time.time() - start < timeout_s:
            remaining = timeout_s - (time.time() - start)
            line = self.readline(timeout_s=min(0.5, max(remaining, 0.01)))
            if line is None:
                continue
            telem = TelemetryLine.parse(line)
            if telem is None:
                continue
            if event is not None and telem.event != event:
                continue
            return telem
        return None

    def read_for_duration(self, duration_s: float) -> list:
        """Read all lines for the given duration."""
        start = time.time()
        lines = []
        while time.time() - start < duration_s:
            line = self.readline(timeout_s=0.1)
            if line:
                lines.append(line)
        return lines

    def query(self, cmd: str, timeout_s: float = 3.0) -> Optional[TelemetryLine]:
        """
        Send a serial query command (e.g. "?dmx0", "?state", "?lua") and
        return the matching GLOW-TEST: reply. The reply's event name is the
        command with its leading '?' stripped (?dmx0 -> event "dmx0").
        """
        if not self.ser:
            raise RuntimeError("Serial port not open")
        expect_event = cmd.lstrip("?")
        self.ser.write((cmd + "\n").encode("ascii"))
        return self.read_telemetry(event=expect_event, timeout_s=timeout_s)

    def assert_no_crash(self, duration_s: float) -> None:
        """Read for `duration_s`, failing loudly (not skipping) on any crash marker."""
        start = time.time()
        while time.time() - start < duration_s:
            line = self.readline(timeout_s=min(0.5, duration_s))
            if line is None:
                continue
            marker = line_is_crash(line)
            if marker:
                raise AssertionError(f"Crash marker '{marker}' seen on serial: {line!r}")

    def flush_logs(self) -> str:
        return "\n".join(self.logs)


def run_cmd(cmd: list, cwd: Optional[Path] = None, timeout: Optional[float] = None) -> subprocess.CompletedProcess:
    logger.info("$ %s", " ".join(str(c) for c in cmd))
    return subprocess.run(
        cmd, cwd=cwd, timeout=timeout, capture_output=True, text=True,
    )


def build_and_flash_firmware(firmware_dir: Path, build_dir: Path, port: str, selftest: bool = True) -> None:
    """
    Build the firmware (CONFIG_GLOW_SELFTEST=y when `selftest`) and flash it.

    Uses idf.py's -B (out-of-tree build dir) and -D SDKCONFIG_DEFAULTS to
    layer firmware/sdkconfig.selftest.defaults on top of the project's
    ordinary sdkconfig.defaults, without touching the developer's own
    "build/" directory or checked-in sdkconfig.

    Raises RuntimeError on any build/flash failure -- this is a real
    firmware bring-up step, not something to silently degrade past (an
    agent guardrail: this suite does not modify firmware to make a test
    pass, and it does not pretend a broken flash succeeded).
    """
    if not shutil.which("idf.py"):
        raise RuntimeError(
            "idf.py not found on PATH. Source an ESP-IDF environment "
            "(`. $IDF_PATH/export.sh`) before running the HIL suite, or set "
            "GLOW_SKIP_FLASH=1 if the board already has a selftest build."
        )

    defaults = ["sdkconfig.defaults"]
    if selftest:
        defaults.append("sdkconfig.selftest.defaults")

    cmd = [
        "idf.py",
        "-C", str(firmware_dir),
        "-B", str(build_dir),
        "-D", f"SDKCONFIG_DEFAULTS={';'.join(defaults)}",
        "-p", port,
        "build", "flash",
    ]
    result = run_cmd(cmd, cwd=firmware_dir, timeout=900)
    if result.returncode != 0:
        raise RuntimeError(
            f"idf.py build/flash failed (exit {result.returncode}):\n"
            f"--- stdout (tail) ---\n{result.stdout[-4000:]}\n"
            f"--- stderr (tail) ---\n{result.stderr[-4000:]}"
        )
    logger.info("Firmware built and flashed from %s to %s", firmware_dir, port)


def flash_show_bundle(bundle_path: Path, port: str) -> None:
    """
    Write a compiled .shw1 bundle directly to the "show" raw partition via
    esptool.py, mirroring exactly what the browser-based web flasher does
    (see storage_manager.h's header comment) and what
    firmware/main/CMakeLists.txt's esptool_py_flash_to_partition() does at
    build time. Does not rebuild or reflash the application itself.
    """
    if not shutil.which("esptool.py"):
        raise RuntimeError("esptool.py not found on PATH (ships with ESP-IDF's Python env).")
    cmd = [
        "esptool.py", "--chip", "esp32s3", "--port", port,
        "write_flash", hex(SHOW_PARTITION_OFFSET), str(bundle_path),
    ]
    result = run_cmd(cmd, timeout=120)
    if result.returncode != 0:
        raise RuntimeError(
            f"esptool.py write_flash to 'show' partition failed:\n{result.stdout}\n{result.stderr}"
        )


def blank_show_partition(port: str) -> None:
    """
    Erase the "show" partition back to blank flash. loadShow() then fails
    (no valid SHW1 magic), so main.cpp falls back to
    setup_selftest_fixture() again -- the deterministic ch0=200 patch that
    L0/L1/L5/L6/L7 depend on. Any test module that flashes a real bundle
    (L3) MUST restore this afterward (see its module-scoped autouse
    fixture) so later layers don't inherit a non-deterministic "show".
    """
    if not shutil.which("esptool.py"):
        raise RuntimeError("esptool.py not found on PATH (ships with ESP-IDF's Python env).")
    cmd = [
        "esptool.py", "--chip", "esp32s3", "--port", port,
        "erase_region", hex(SHOW_PARTITION_OFFSET), hex(SHOW_PARTITION_SIZE),
    ]
    result = run_cmd(cmd, timeout=120)
    if result.returncode != 0:
        raise RuntimeError(f"esptool.py erase_region on 'show' partition failed:\n{result.stdout}\n{result.stderr}")


def compile_show_bundle(show_path: Path, output_path: Path) -> None:
    """
    Compile a .show/.fdef text fixture into a .shw1 binary bundle using the
    `provision` host tool (provision_main.cpp + provision.cpp, the same
    compiler build_sample_bundle.sh uses for the demo bundle). Built once
    into HIL_DIR/.provision_tool and reused across calls.
    """
    tool = HIL_DIR / ".provision_tool"
    if not tool.exists():
        cxx = os.environ.get("CXX", "g++")
        cmd = [
            cxx, "-std=c++17", "-O2",
            "provision_main.cpp", "provision.cpp", "profile_encoder.cpp",
            "fixture_profile.cpp", "pixel_matrix.cpp", "color.cpp", "vec_math.cpp",
            "aim.cpp", "show_bundle.cpp",
            "-o", str(tool), "-lm",
        ]
        result = run_cmd(cmd, cwd=REPO_ROOT, timeout=120)
        if result.returncode != 0:
            raise RuntimeError(f"Failed to build the provision tool:\n{result.stdout}\n{result.stderr}")

    # provision resolves FIXTURE deffile paths relative to the current
    # working directory (see samples/demo.show's own comment) -- run it
    # from the repo root so "samples/dimmer.fdef" resolves.
    cmd = [str(tool), str(show_path.relative_to(REPO_ROOT)), str(output_path)]
    result = run_cmd(cmd, cwd=REPO_ROOT, timeout=30)
    if result.returncode != 0:
        raise RuntimeError(f"provision compile failed for {show_path}:\n{result.stdout}\n{result.stderr}")


def ws_recv_json(ws, timeout_s: float = 5.0) -> Optional[dict]:
    """Receive one WS text frame and parse it as JSON, or None on timeout/non-JSON."""
    ws.settimeout(timeout_s)
    try:
        return json.loads(ws.recv())
    except Exception:
        return None


def ws_drain(ws, timeout_s: float = 0.3) -> list:
    """Drain and discard whatever's already buffered (e.g. the initial `config` message)."""
    ws.settimeout(timeout_s)
    msgs = []
    try:
        while True:
            msgs.append(json.loads(ws.recv()))
    except Exception:
        pass
    return msgs


def ws_eval(ws, src: str, seq: int, timeout_s: float = 5.0) -> Optional[dict]:
    """
    Send `{"type":"eval","src":src,"seq":seq}` and wait (skipping any other
    broadcast traffic, e.g. `state`, `fx_error`) for the matching
    eval_result. Returns the parsed message dict, or None on timeout.
    """
    ws.send(json.dumps({"type": "eval", "src": src, "seq": seq}))
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        msg = ws_recv_json(ws, timeout_s=max(deadline - time.time(), 0.01))
        if msg is None:
            continue
        if msg.get("type") == "eval_result" and msg.get("seq") == seq:
            return msg
    return None


# --- pytest fixtures -------------------------------------------------------

@pytest.fixture(scope="session")
def serial_port() -> str:
    return os.getenv("GLOW_SERIAL_PORT", "/dev/ttyUSB0")


@pytest.fixture(scope="session")
def device_ip() -> str:
    ip = os.getenv("GLOW_DEVICE_IP")
    if not ip:
        pytest.fail(
            "GLOW_DEVICE_IP is not set. WS/OSC/Art-Net tests need the "
            "device's LAN IP (check the board's serial banner or your "
            "router's DHCP leases)."
        )
    return ip


@pytest.fixture(scope="session")
def firmware_dir() -> Path:
    return Path(os.getenv("GLOW_FIRMWARE_DIR", str(REPO_ROOT / "firmware")))


@pytest.fixture(scope="session")
def idf_build_dir(firmware_dir: Path) -> Path:
    return Path(os.getenv("GLOW_IDF_BUILD_DIR", str(firmware_dir / "build-hil-selftest")))


@pytest.fixture(scope="session")
def flashed_selftest_firmware(firmware_dir: Path, idf_build_dir: Path, serial_port: str) -> None:
    """
    Session-scoped: builds (CONFIG_GLOW_SELFTEST=y) and flashes the firmware
    exactly once for the whole test session. Individual tests get a clean
    boot via the `device_reset` fixture's RTS toggle, not a reflash --
    reflashing per test would make the suite take hours.

    Set GLOW_SKIP_FLASH=1 to skip this and test against whatever is already
    on the board (useful when iterating on the test suite itself).
    """
    if os.getenv("GLOW_SKIP_FLASH"):
        logger.warning("GLOW_SKIP_FLASH set: assuming a selftest build is already on the board.")
        return
    build_and_flash_firmware(firmware_dir, idf_build_dir, serial_port, selftest=True)


@pytest.fixture
def serial_reader(flashed_selftest_firmware, serial_port: str) -> Iterator[SerialReader]:
    reader = SerialReader(serial_port)
    reader.open()

    time.sleep(0.5)
    try:
        while reader.ser and reader.ser.in_waiting > 0:
            reader.ser.read(1024)
    except Exception:
        pass
    reader.buffer = ""

    yield reader

    if reader.logs:
        logger.info("Device logs (%d lines) captured for this test", len(reader.logs))
    reader.close()


@pytest.fixture
def device_reset(serial_reader: SerialReader):
    """Returns a function that resets the device via RTS and clears the reader's buffers."""

    def reset(wait_for_boot: bool = True):
        if not serial_reader.ser:
            raise RuntimeError("Serial port not open")
        serial_reader.ser.reset_input_buffer()
        serial_reader.ser.reset_output_buffer()
        serial_reader.ser.dtr = False
        serial_reader.ser.rts = True
        time.sleep(0.1)
        serial_reader.ser.rts = False
        time.sleep(0.5)
        serial_reader.buffer = ""
        serial_reader.logs = []
        if wait_for_boot:
            line = serial_reader.read_until("GLOW-TEST: dmx begin=ok", timeout_s=10)
            if line is None:
                raise AssertionError(
                    "Device did not report 'GLOW-TEST: dmx begin=ok' within 10s of reset -- "
                    "not booting, or not a CONFIG_GLOW_SELFTEST build."
                )

    return reset


@pytest.fixture(scope="session")
def hil_fixtures_dir() -> Path:
    return HIL_DIR / "fixtures"
