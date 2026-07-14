"""
Transport-independent line/telemetry reading.

tests/hil/'s SerialReader (a real board's USB serial) and tests/qemu/'s
QemuReader (a QEMU subprocess's stdio) both accumulate a byte stream into
lines and pick `GLOW-TEST:` telemetry lines out of it identically -- only
how the next chunk of bytes is obtained differs. This base class supplies
the shared half (readline/read_until/read_telemetry/read_for_duration/
assert_no_crash); a subclass supplies only `_read_chunk()`. This is what
lets a test file written against one transport run unmodified against the
other -- see tests/qemu/README.md.
"""

import logging
import time
from typing import Optional

from .telemetry import TelemetryLine, line_is_crash

logger = logging.getLogger(__name__)


class LineReader:
    def __init__(self):
        self.buffer = ""
        self.logs: list = []

    def _read_chunk(self, timeout_s: float) -> bytes:
        """Subclasses: return whatever bytes are available within timeout_s (possibly empty)."""
        raise NotImplementedError

    def readline(self, timeout_s: float = 5.0) -> Optional[str]:
        """Read one line with timeout. Returns the line (no trailing newline) or None."""
        start = time.time()
        while time.time() - start < timeout_s:
            if "\n" in self.buffer:
                line, self.buffer = self.buffer.split("\n", 1)
                line = line.rstrip("\r")
                self.logs.append(line)
                return line
            try:
                remaining = max(0.01, timeout_s - (time.time() - start))
                chunk = self._read_chunk(timeout_s=remaining)
                if chunk:
                    self.buffer += chunk.decode("utf-8", errors="replace")
            except Exception as e:  # pragma: no cover - transport I/O error path
                logger.error("Read error: %s", e)
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
