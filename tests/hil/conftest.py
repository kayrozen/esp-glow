"""
HIL Test Harness Configuration and Fixtures

Provides serial port connection, firmware flashing, and telemetry capture.
"""

import pytest
import serial
import subprocess
import time
import logging
from typing import Iterator, Optional, Tuple
from dataclasses import dataclass
from pathlib import Path

logging.basicConfig(level=logging.DEBUG)
logger = logging.getLogger(__name__)


@dataclass
class TelemetryLine:
    """Parsed GLOW-TEST: telemetry line."""
    raw: str
    key_values: dict[str, str]

    @classmethod
    def parse(cls, line: str) -> Optional["TelemetryLine"]:
        """Parse a GLOW-TEST: <key>=<value> line."""
        if "GLOW-TEST:" not in line:
            return None

        prefix_idx = line.find("GLOW-TEST:")
        if prefix_idx == -1:
            return None

        content = line[prefix_idx + len("GLOW-TEST:"):].strip()
        kv = {}

        for pair in content.split():
            if "=" in pair:
                k, v = pair.split("=", 1)
                kv[k.strip()] = v.strip()

        return cls(raw=line, key_values=kv)


class SerialReader:
    """Manages serial port communication with device."""

    def __init__(self, port: str, baudrate: int = 115200):
        self.port = port
        self.baudrate = baudrate
        self.ser: Optional[serial.Serial] = None
        self.buffer = ""
        self.logs = []

    def open(self) -> None:
        """Open serial connection."""
        try:
            self.ser = serial.Serial(self.port, self.baudrate, timeout=1)
            logger.info(f"Opened serial port {self.port}")
        except serial.SerialException as e:
            raise RuntimeError(f"Failed to open {self.port}: {e}")

    def close(self) -> None:
        """Close serial connection."""
        if self.ser:
            self.ser.close()
            logger.info(f"Closed serial port {self.port}")

    def readline(self, timeout_s: float = 5.0) -> Optional[str]:
        """
        Read one line from serial with timeout.

        Returns the line without trailing newline, or None on timeout.
        """
        if not self.ser:
            raise RuntimeError("Serial port not open")

        start = time.time()
        while time.time() - start < timeout_s:
            if "\n" in self.buffer:
                line, self.buffer = self.buffer.split("\n", 1)
                self.logs.append(line.rstrip("\r"))
                return line.rstrip("\r")

            try:
                chunk = self.ser.read(1024)
                if chunk:
                    self.buffer += chunk.decode("utf-8", errors="replace")
            except Exception as e:
                logger.error(f"Serial read error: {e}")

        return None

    def read_until(self, pattern: str, timeout_s: float = 5.0) -> Optional[str]:
        """
        Read lines until one contains the pattern.

        Returns the matching line or None on timeout.
        """
        start = time.time()
        while time.time() - start < timeout_s:
            line = self.readline(timeout_s=0.5)
            if line is None:
                continue
            if pattern in line:
                return line

        return None

    def read_telemetry(self, timeout_s: float = 5.0) -> Optional[TelemetryLine]:
        """
        Read the next GLOW-TEST: telemetry line.

        Returns parsed telemetry or None on timeout.
        """
        start = time.time()
        while time.time() - start < timeout_s:
            line = self.readline(timeout_s=0.5)
            if line is None:
                continue

            telem = TelemetryLine.parse(line)
            if telem:
                return telem

        return None

    def read_for_duration(self, duration_s: float) -> list[str]:
        """Read all lines for the given duration."""
        start = time.time()
        lines = []
        while time.time() - start < duration_s:
            line = self.readline(timeout_s=0.1)
            if line:
                lines.append(line)

        return lines

    def flush_logs(self) -> str:
        """Get all accumulated logs as a string."""
        return "\n".join(self.logs)


def flash_firmware(
    build_dir: str = "build",
    port: str = "/dev/ttyUSB0",
    selftest: bool = False
) -> None:
    """
    Flash the ESP32-S3 with the latest build.

    Args:
        build_dir: ESP-IDF build directory
        port: Serial port
        selftest: If True, enable CONFIG_GLOW_SELFTEST

    Raises RuntimeError on flash failure.
    """
    logger.info(f"Flashing firmware from {build_dir} to {port}")

    # This is a placeholder; actual implementation depends on ESP-IDF setup.
    # For now, we assume esptool.py is available and the build directory is ready.
    #
    # Real command would be:
    #   esptool.py --chip esp32s3 --port /dev/ttyUSB0 --baud 460800 \
    #     write_flash 0x0 build/esp-glow.bin

    try:
        # Placeholder: user will need to set up proper esptool integration
        logger.warning("Flash command not yet configured; assuming firmware is already loaded")
    except Exception as e:
        raise RuntimeError(f"Flash failed: {e}")


@pytest.fixture(scope="session")
def serial_port() -> str:
    """Get the serial port from environment or use default."""
    import os
    return os.getenv("GLOW_SERIAL_PORT", "/dev/ttyUSB0")


@pytest.fixture(scope="session")
def device_ip() -> str:
    """Get the device IP address from environment."""
    import os
    return os.getenv("GLOW_DEVICE_IP", "192.168.1.100")


@pytest.fixture
def serial_reader(serial_port: str) -> Iterator[SerialReader]:
    """
    Fixture that provides a serial reader for the device.

    Opens the port before the test and closes after.
    """
    reader = SerialReader(serial_port)
    reader.open()

    # Clear any stale data in the buffer
    time.sleep(0.5)
    try:
        while reader.ser and reader.ser.in_waiting > 0:
            reader.ser.read(1024)
    except:
        pass

    reader.buffer = ""

    yield reader

    # Capture logs for debugging
    if reader.logs:
        logger.info(f"Device logs:\n{reader.flush_logs()}")

    reader.close()


@pytest.fixture
def device_reset(serial_reader: SerialReader) -> callable:
    """
    Fixture that provides a function to reset the device via RTS.

    The serial reader will be cleared after reset.
    """
    def reset():
        if serial_reader.ser:
            logger.info("Resetting device via RTS")
            serial_reader.ser.reset_input_buffer()
            serial_reader.ser.reset_output_buffer()

            # Toggle RTS to trigger reset
            serial_reader.ser.dtr = False
            serial_reader.ser.rts = True
            time.sleep(0.1)
            serial_reader.ser.rts = False
            time.sleep(0.5)

            serial_reader.buffer = ""
            serial_reader.logs = []

    return reset
