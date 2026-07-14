"""
QEMU Boot Test Harness Configuration and Fixtures.

Boots a CONFIG_GLOW_SELFTEST build (the same build the tests/hil/ suite
flashes to a real board -- see firmware/main/main.cpp's "Phase 0: HIL
selftest observability" section) under Espressif's QEMU fork instead of
real hardware, and reads the identical `GLOW-TEST:` telemetry protocol off
QEMU's emulated UART0 (wired to this process's stdio by `-nographic`)
instead of a USB serial port. This is deliberately the same harness as
tests/hil/, minus the transport: TelemetryLine/line_is_crash
(tests/shared/telemetry.py) and the read/assert loop (LineReader,
tests/shared/line_reader.py) are shared verbatim with tests/hil/conftest.py.

What QEMU boots here catches (per the project's bring-up plan): whether the
firmware boots at all, the raw "show" partition load, LittleFS mount for
"scripts", the Lua/Fennel VM coming up, and the render task's first stats
tick -- all without a board. What it does NOT catch: DMX break/MAB timing,
real GC pacing/dropped-frame behavior under load, PSRAM timing, or anything
requiring a peripheral this QEMU fork doesn't model (see README.md).

Environment variables:
    GLOW_FIRMWARE_DIR    Path to the firmware/ ESP-IDF project. Default
                          "<repo>/firmware".
    GLOW_IDF_BUILD_DIR   Build directory for the selftest build. Default
                          "<firmware>/build-qemu-selftest" (kept separate
                          from both a developer's ordinary "build/" and the
                          HIL suite's "build-hil-selftest" so none of the
                          three ever clobber each other).
    GLOW_SKIP_BUILD      If set (any value), skip the `idf.py build` step
                          and assume GLOW_IDF_BUILD_DIR already holds a
                          selftest build. Useful for iterating on the test
                          suite itself without a multi-minute rebuild.
    GLOW_QEMU_BIN        Path to the qemu-system-xtensa binary. Default
                          "qemu-system-xtensa" (resolved via PATH).
    GLOW_QEMU_MACHINE    QEMU `-machine` name. Default "esp32s3".
    GLOW_QEMU_PSRAM_MB   QEMU `-m` size in MB -- this is what attaches a
                          virtual PSRAM chip at all (see QemuReader.start's
                          comment); one of 2/4/8/16/32 (this QEMU fork's
                          esp32s3.c rounds up to the nearest supported
                          size). Default 8.

Requires `idf.py` + `esptool.py` on PATH (i.e. an ESP-IDF environment
already sourced -- `. $IDF_PATH/export.sh`) unless GLOW_SKIP_BUILD is set,
and a `qemu-system-xtensa` with esp32s3 machine support on PATH (or
GLOW_QEMU_BIN) always -- see README.md for how to get one.
"""

import fcntl
import json
import logging
import os
import select
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Iterator, Optional

import pytest

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

REPO_ROOT = Path(__file__).resolve().parents[2]
QEMU_DIR = Path(__file__).resolve().parent

# TelemetryLine/line_is_crash and the read/assert loop are shared with
# tests/hil/ (same wire format, different transport) -- see
# tests/shared/telemetry.py and tests/shared/line_reader.py.
sys.path.insert(0, str(REPO_ROOT / "tests"))
from shared.telemetry import CRASH_MARKERS, TelemetryLine, line_is_crash  # noqa: E402
from shared.line_reader import LineReader  # noqa: E402

# esp32s3 8 MB flash, matching firmware/sdkconfig.defaults'
# CONFIG_ESPTOOLPY_FLASHSIZE. QEMU's `-drive if=mtd` backing file must be
# padded to exactly this size (esptool merge_bin's --fill-flash-size does
# the padding); a short file makes QEMU refuse to boot, not just warn.
FLASH_SIZE_BYTES = 8 * 1024 * 1024
FLASH_SIZE_STR = "8MB"

BOOT_READY_MARKER = "GLOW-TEST: dmx begin=ok"


def run_cmd(cmd: list, cwd: Optional[Path] = None, timeout: Optional[float] = None) -> subprocess.CompletedProcess:
    logger.info("$ %s", " ".join(str(c) for c in cmd))
    return subprocess.run(cmd, cwd=cwd, timeout=timeout, capture_output=True, text=True)


def build_selftest_firmware(firmware_dir: Path, build_dir: Path) -> None:
    """
    Build the firmware (CONFIG_GLOW_SELFTEST=y, the exact same layered
    sdkconfig the HIL suite flashes to real hardware -- see its
    conftest.py's build_and_flash_firmware) into `build_dir`. QEMU needs no
    port and no flash step: it boots straight off the merged image built
    from these outputs (see build_qemu_flash_image below), so there is
    nothing here a real board's absence should block.
    """
    if not shutil.which("idf.py"):
        raise RuntimeError(
            "idf.py not found on PATH. Source an ESP-IDF environment "
            "(`. $IDF_PATH/export.sh`) before running the QEMU suite, or "
            "set GLOW_SKIP_BUILD=1 if GLOW_IDF_BUILD_DIR already holds a "
            "selftest build."
        )

    defaults = ["sdkconfig.defaults", "sdkconfig.selftest.defaults"]
    cmd = [
        "idf.py",
        "-C", str(firmware_dir),
        "-B", str(build_dir),
        "-D", f"SDKCONFIG_DEFAULTS={';'.join(defaults)}",
        "build",
    ]
    result = run_cmd(cmd, cwd=firmware_dir, timeout=900)
    if result.returncode != 0:
        raise RuntimeError(
            f"idf.py build failed (exit {result.returncode}):\n"
            f"--- stdout (tail) ---\n{result.stdout[-4000:]}\n"
            f"--- stderr (tail) ---\n{result.stderr[-4000:]}"
        )
    logger.info("Firmware built into %s", build_dir)


def build_qemu_flash_image(build_dir: Path) -> Path:
    """
    Merge the build's bootloader/partition-table/app/show-bundle outputs
    into one flat `qemu_flash.bin`, exactly what `idf.py qemu` does
    internally (it reads flasher_args.json's flash_files and calls
    esptool.py merge_bin) -- the point being the image QEMU boots is
    identical to what would be flashed to a real board, partitions and all,
    not a QEMU-specific rebuild.
    """
    if not shutil.which("esptool.py"):
        raise RuntimeError("esptool.py not found on PATH (ships with ESP-IDF's Python env).")

    flasher_args_path = build_dir / "flasher_args.json"
    if not flasher_args_path.exists():
        raise RuntimeError(
            f"{flasher_args_path} not found -- has the firmware been built "
            f"into this directory? (see GLOW_IDF_BUILD_DIR / GLOW_SKIP_BUILD)"
        )
    flasher_args = json.loads(flasher_args_path.read_text())
    flash_files: dict = flasher_args["flash_files"]
    if not flash_files:
        raise RuntimeError(f"{flasher_args_path} lists no flash_files")

    out_path = build_dir / "qemu_flash.bin"
    cmd = [
        "esptool.py", "--chip", "esp32s3",
        "merge_bin",
        "-o", str(out_path),
        "--flash_size", FLASH_SIZE_STR,
        "--fill-flash-size", FLASH_SIZE_STR,
    ]
    # flasher_args.json's paths are relative to build_dir (see
    # firmware/main/CMakeLists.txt's header comment on why show.shw1 is
    # copied into the build dir rather than referenced from data/).
    for offset, rel_path in flash_files.items():
        cmd += [offset, str(build_dir / rel_path)]

    result = run_cmd(cmd, cwd=build_dir, timeout=120)
    if result.returncode != 0:
        raise RuntimeError(
            f"esptool.py merge_bin failed (exit {result.returncode}):\n"
            f"--- stdout ---\n{result.stdout}\n--- stderr ---\n{result.stderr}"
        )
    if not out_path.exists() or out_path.stat().st_size != FLASH_SIZE_BYTES:
        raise RuntimeError(
            f"merge_bin did not produce a {FLASH_SIZE_BYTES}-byte image at {out_path}"
        )
    logger.info("Merged QEMU flash image: %s (%d bytes)", out_path, out_path.stat().st_size)
    return out_path


class QemuReader(LineReader):
    """
    Boots qemu-system-xtensa as a subprocess and reads the guest's UART0
    off its stdio (QEMU's `-nographic` muxes the emulated serial console to
    this process's stdin/stdout -- there is no PTY or network socket
    involved, unlike a real board's USB-serial adapter). Implements
    LineReader's `_read_chunk` over a non-blocking read of the subprocess's
    stdout; everything else (readline/read_until/read_telemetry/
    assert_no_crash/...) is inherited unchanged -- see
    tests/shared/line_reader.py.
    """

    def __init__(self, qemu_bin: str, machine: str, flash_image: Path, psram_mb: int = 8):
        super().__init__()
        self.qemu_bin = qemu_bin
        self.machine = machine
        self.flash_image = flash_image
        self.psram_mb = psram_mb
        self.proc: Optional[subprocess.Popen] = None

    def start(self) -> None:
        cmd = [
            self.qemu_bin,
            "-nographic",
            "-no-reboot",
            "-machine", self.machine,
            "-drive", f"file={self.flash_image},if=mtd,format=raw",
            # esp32s3.c's esp32s3_machine_init_psram() only attaches a
            # virtual PSRAM chip to SPI1/CS1 when machine->ram_size > 0
            # (mc->default_ram_size is 0 -- no -m means no PSRAM device on
            # the bus at all, not a smaller one). CONFIG_SPIRAM=y +
            # CONFIG_BOOTLOADER_APP_ROLLBACK... -- app_main aborts in
            # cpu_start if PSRAM init fails, so this is required, not
            # optional, for this firmware to boot at all under QEMU. Valid
            # sizes this machine accepts: 2/4/8/16/32 MB (see
            # esp32s3_fixup_ram_size); 8M matches a typical ESP32-S3-WROOM
            # board and firmware/sdkconfig.defaults' CONFIG_SPIRAM_TYPE_AUTO.
            "-m", f"{self.psram_mb}M",
            # esp32s3_machine_init_psram() never sets the "ssi_psram" QOM
            # type's is_octal property (defaults false -- quad mode), but
            # firmware/sdkconfig.defaults' CONFIG_SPIRAM_MODE_OCT=y means
            # the firmware's octal_psram driver only speaks the octal wire
            # protocol; against a quad-mode model that mismatch reads back
            # as an all-zero ID ("PSRAM chip not found") even with -m set.
            # -global overrides the device property generically, without
            # needing esp32s3.c to set it itself -- confirmed against
            # hw/misc/ssi_psram.c's DEFINE_PROP_BOOL("is_octal", ...).
            "-global", "driver=ssi_psram,property=is_octal,value=true",
        ]
        logger.info("$ %s", " ".join(cmd))
        self.proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=0,
        )
        # Non-blocking stdout so _read_chunk can honor a timeout instead of
        # blocking the whole harness on a guest that never prints another byte.
        fd = self.proc.stdout.fileno()
        flags = fcntl.fcntl(fd, fcntl.F_GETFL)
        fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)

    def _read_chunk(self, timeout_s: float) -> bytes:
        if not self.proc or not self.proc.stdout:
            raise RuntimeError("QEMU not started")
        ready, _, _ = select.select([self.proc.stdout], [], [], timeout_s)
        if not ready:
            return b""
        try:
            return self.proc.stdout.read(4096) or b""
        except (BlockingIOError, OSError):
            return b""

    def write(self, data: bytes) -> None:
        """Write to the guest's UART0 (the selftest query protocol -- see SerialReader.query)."""
        if not self.proc or not self.proc.stdin:
            raise RuntimeError("QEMU not started")
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def query(self, cmd: str, timeout_s: float = 3.0) -> Optional[TelemetryLine]:
        expect_event = cmd.lstrip("?")
        self.write((cmd + "\n").encode("ascii"))
        return self.read_telemetry(event=expect_event, timeout_s=timeout_s)

    def is_running(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def stop(self) -> None:
        if not self.proc:
            return
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.proc = None


# --- pytest fixtures -------------------------------------------------------

@pytest.fixture(scope="session")
def firmware_dir() -> Path:
    return Path(os.getenv("GLOW_FIRMWARE_DIR", str(REPO_ROOT / "firmware")))


@pytest.fixture(scope="session")
def idf_build_dir(firmware_dir: Path) -> Path:
    return Path(os.getenv("GLOW_IDF_BUILD_DIR", str(firmware_dir / "build-qemu-selftest")))


@pytest.fixture(scope="session")
def qemu_flash_image(firmware_dir: Path, idf_build_dir: Path) -> Path:
    """Session-scoped: build once (or reuse, if GLOW_SKIP_BUILD), merge once."""
    if not os.getenv("GLOW_SKIP_BUILD"):
        build_selftest_firmware(firmware_dir, idf_build_dir)
    else:
        logger.warning("GLOW_SKIP_BUILD set: assuming %s already holds a selftest build.", idf_build_dir)
    return build_qemu_flash_image(idf_build_dir)


@pytest.fixture
def qemu_boot(qemu_flash_image: Path) -> Iterator[QemuReader]:
    """
    Per-test fixture: a fresh QEMU process for a clean boot. QEMU has no
    RTS-reset equivalent (see tests/hil/conftest.py's device_reset) -- a new
    process per test IS the clean-boot mechanism here, and it's cheap
    (booting in an emulator is a fraction of a second of wall time vs. a
    board's ROM bootloader + app boot).
    """
    qemu_bin = os.getenv("GLOW_QEMU_BIN", "qemu-system-xtensa")
    machine = os.getenv("GLOW_QEMU_MACHINE", "esp32s3")
    psram_mb = int(os.getenv("GLOW_QEMU_PSRAM_MB", "8"))
    if not shutil.which(qemu_bin):
        pytest.fail(
            f"{qemu_bin!r} not found on PATH. Install Espressif's QEMU fork "
            f"(esp32s3 machine support) or set GLOW_QEMU_BIN -- see README.md."
        )

    reader = QemuReader(qemu_bin, machine, qemu_flash_image, psram_mb)
    reader.start()
    try:
        yield reader
    finally:
        if reader.logs:
            logger.info("QEMU serial log (%d lines) captured for this test", len(reader.logs))
        reader.stop()


@pytest.fixture
def qemu_booted(qemu_boot: QemuReader) -> QemuReader:
    """Like `qemu_boot`, but blocks until the firmware has reached a known-good state."""
    line = qemu_boot.read_until(BOOT_READY_MARKER, timeout_s=30)
    if line is None:
        raise AssertionError(
            f"QEMU guest did not report {BOOT_READY_MARKER!r} within 30s of boot -- "
            f"not booting, or not a CONFIG_GLOW_SELFTEST build. Captured log:\n"
            f"{qemu_boot.flush_logs()[-4000:]}"
        )
    return qemu_boot
