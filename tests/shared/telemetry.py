"""
The `GLOW-TEST: <event> key=value...` telemetry wire format.

Firmware built with CONFIG_GLOW_SELFTEST (see firmware/main/main.cpp's
"Phase 0: HIL selftest observability" section) prints these lines to UART0
regardless of what's on the other end of that UART -- a real board's USB
serial (tests/hil/) or a QEMU subprocess's stdio (tests/qemu/). This module
has no serial/subprocess dependency itself, so both suites parse the exact
same wire format with the exact same code; only how the bytes are obtained
differs per suite's own conftest.py.
"""

from dataclasses import dataclass, field
from typing import Optional

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
