#!/usr/bin/env python3
"""flash_devcfg.py — write a CFG1 device config blob straight to a board's
"devcfg" partition over USB, with esptool.py. No WiFi, no running firmware,
no browser required.

Why this exists: the ONLY other way to set artnetFallbackIp/
artnetSyncBroadcast/etc. is POST /devcfg over the web console -- which
requires the device to already be reachable on the network. A device stuck
in the network-wide ENOMEM/ARP-death bug this tool's sibling PRs fix (or
one that simply never joined WiFi in the first place -- wrong SSID/password,
no AP in range) can never be reached that way. This script is the serial/USB
escape hatch: build a CFG1 blob exactly like device_config.h specifies, and
esptool.py write_flash it directly at the "devcfg" partition's offset, no
network involved at any point.

This is the Python twin of tests/shared/devcfg.py's encoder (imported
directly below, not reimplemented) and web/shared/devcfg.js's -- see
device_config.h for the authoritative byte layout both mirror.

Usage:
    python3 scripts/flash_devcfg.py --port /dev/ttyACM0 \\
        --wifi-ssid MyNetwork --wifi-pass hunter2 \\
        --artnet-fallback-ip 192.168.1.50

    # See exactly what would be written/run, without touching the board:
    python3 scripts/flash_devcfg.py --dry-run --wifi-ssid MyNetwork

    # Read the current devcfg blob back off the board and decode it:
    python3 scripts/flash_devcfg.py --port /dev/ttyACM0 --read-only

Requires esptool.py on PATH (the same tool idf.py itself uses to flash;
`pip install esptool` if you don't have an ESP-IDF environment set up).
"""

import argparse
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))
from shared.devcfg import (  # noqa: E402
    DEVCFG_BLOB_SIZE,
    DEVCFG_CRC_OFFSET,
    DEVCFG_FLAG_ARTNET_SYNC_BROADCAST,
    DEVCFG_FLAG_SKIP_WIFI,
    DEVCFG_FLAG_USB_MIDI_HOST,
    encode_devcfg,
)

# Must match firmware/partitions.csv's "devcfg" row exactly -- this table is
# committed and hand-maintained (not auto-generated), so hardcoding here is
# the same assumption tests/qemu/conftest.py's resolve_partition() avoids
# only because it has a build directory's partition-table.bin to parse; a
# field recovery script talking to a bare board over USB has no such build
# directory to reach for. If firmware/partitions.csv's devcfg row ever
# moves, update DEVCFG_PARTITION_OFFSET/DEVCFG_PARTITION_SIZE here too.
DEVCFG_PARTITION_OFFSET = 0x7E0000
DEVCFG_PARTITION_SIZE = 0x1000


def parse_ipv4(text: str) -> int:
    """Parses "192.168.1.50" into the packed host-byte-order uint32
    device_config.h expects. Empty string, "0", or "broadcast" -> 0 (no
    destination / dropped -- see FORMAT.md's artnetFallbackIp writeup;
    only an explicit "255.255.255.255" still broadcasts)."""
    s = (text or "").strip()
    if s == "" or s == "0" or s.lower() == "broadcast":
        return 0
    parts = s.split(".")
    if len(parts) != 4:
        raise ValueError(f"invalid IPv4 address: {text!r}")
    packed = 0
    for p in parts:
        if not p.isdigit():
            raise ValueError(f"invalid IPv4 address: {text!r}")
        n = int(p)
        if n > 255:
            raise ValueError(f"invalid IPv4 address: {text!r}")
        packed = (packed << 8) | n
    return packed & 0xFFFFFFFF


def decode_devcfg(blob: bytes) -> dict:
    """Minimal read-back decoder for --read-only/--verify -- mirrors
    parseDeviceConfig (device_config.cpp) closely enough to report what's
    actually on the board, not a full strict re-implementation (the C++
    parser and its host tests are the real authority)."""
    if len(blob) < DEVCFG_BLOB_SIZE:
        return {"ok": False, "error": f"blob too short ({len(blob)} < {DEVCFG_BLOB_SIZE})"}
    if blob[0:4] != b"CFG1":
        return {"ok": False, "error": "bad magic (expected CFG1 -- erased/blank or non-CFG1 flash)"}
    version = blob[4]
    if version < 1 or version > 2:
        return {"ok": False, "error": f"unsupported version {version}"}
    stored_crc = struct.unpack_from("<I", blob, DEVCFG_CRC_OFFSET)[0]
    computed_crc = zlib.crc32(blob[:DEVCFG_CRC_OFFSET]) & 0xFFFFFFFF
    if stored_crc != computed_crc:
        return {"ok": False, "error": f"CRC mismatch (stored 0x{stored_crc:08x}, computed 0x{computed_crc:08x})"}

    flags = blob[5]
    off = 8
    wifi_ssid = blob[off:off + 32].split(b"\x00", 1)[0].decode("utf-8", "replace")
    off += 32
    wifi_pass_len = len(blob[off:off + 64].split(b"\x00", 1)[0])
    off += 64
    artnet_fallback_ip = struct.unpack_from("<I", blob, off)[0]
    off += 4
    artnet_port = struct.unpack_from("<H", blob, off)[0]
    off += 2
    dmx_tx_gpio, dmx_rx_gpio, dmx_rts_gpio, led_gpio = blob[off], blob[off + 1], blob[off + 2], blob[off + 3]

    return {
        "ok": True,
        "version": version,
        "usbMidiHost": bool(flags & DEVCFG_FLAG_USB_MIDI_HOST),
        "skipWifi": bool(flags & DEVCFG_FLAG_SKIP_WIFI),
        "artnetSyncBroadcast": version >= 2 and bool(flags & DEVCFG_FLAG_ARTNET_SYNC_BROADCAST),
        "wifiSsid": wifi_ssid,
        "wifiPassLen": wifi_pass_len,  # never print the password itself -- see FORMAT.md's Security section
        "artnetFallbackIp": artnet_fallback_ip,
        "artnetPort": artnet_port,
        "dmxTxGpio": dmx_tx_gpio,
        "dmxRxGpio": dmx_rx_gpio,
        "dmxRtsGpio": dmx_rts_gpio,
        "ledGpio": led_gpio,
    }


def format_ipv4(packed: int) -> str:
    if not packed:
        return "(none/dropped)"
    return ".".join(str((packed >> shift) & 0xFF) for shift in (24, 16, 8, 0))


def run_esptool(args, port, dry_run: bool) -> None:
    cmd = ["esptool.py", "--port", port or "<port>"] + args
    print(f"$ {' '.join(cmd)}")
    if dry_run:
        print("(--dry-run: not actually executed)")
        return
    subprocess.run(cmd, check=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=None, help="Serial port (e.g. /dev/ttyACM0, COM5). Required unless --dry-run.")
    ap.add_argument("--dry-run", action="store_true", help="Print the esptool.py command(s) and the encoded blob's summary; touch nothing.")
    ap.add_argument("--read-only", action="store_true", help="Read the board's current devcfg blob back and decode it; write nothing.")
    ap.add_argument("--verify", action="store_true", help="After writing, read the blob back and confirm it matches byte-for-byte.")

    ap.add_argument("--wifi-ssid", default="")
    ap.add_argument("--wifi-pass", default="")
    ap.add_argument("--artnet-fallback-ip", default="", help='Dotted IPv4, "broadcast" for 255.255.255.255, or blank/0 for "dropped, no destination" (the safe default -- see FORMAT.md).')
    ap.add_argument("--artnet-port", type=int, default=6454)
    ap.add_argument("--artnet-sync-broadcast", action="store_true", help="Set artnetSyncBroadcast (spec-literal broadcast ArtSync instead of the default targeted-unicast behavior). See FORMAT.md.")
    ap.add_argument("--dmx-tx-gpio", type=int, default=17)
    ap.add_argument("--dmx-rx-gpio", type=int, default=18)
    ap.add_argument("--dmx-rts-gpio", type=int, default=8)
    ap.add_argument("--led-gpio", type=int, default=2)
    ap.add_argument("--usb-midi-host", action="store_true")
    ap.add_argument("--skip-wifi", action="store_true")
    args = ap.parse_args()

    if not args.port and not args.dry_run:
        ap.error("--port is required (unless --dry-run)")

    if args.read_only:
        with tempfile.NamedTemporaryFile(suffix=".bin") as tmp:
            run_esptool(
                ["read_flash", hex(DEVCFG_PARTITION_OFFSET), hex(DEVCFG_PARTITION_SIZE), tmp.name],
                args.port, args.dry_run,
            )
            if args.dry_run:
                return 0
            blob = Path(tmp.name).read_bytes()
        result = decode_devcfg(blob[:DEVCFG_BLOB_SIZE])
        if not result["ok"]:
            print(f"devcfg partition does not hold a valid CFG1 blob: {result['error']}")
            return 1
        for key, val in result.items():
            if key == "artnetFallbackIp":
                print(f"  artnetFallbackIp: {format_ipv4(val)} (0x{val:08x})")
            elif key == "wifiPassLen":
                print(f"  wifiPass: <{val} bytes, not shown>")
            else:
                print(f"  {key}: {val}")
        return 0

    try:
        fallback_ip = parse_ipv4(args.artnet_fallback_ip)
    except ValueError as e:
        ap.error(str(e))

    blob = encode_devcfg(
        wifi_ssid=args.wifi_ssid,
        wifi_pass=args.wifi_pass,
        artnet_fallback_ip=fallback_ip,
        artnet_port=args.artnet_port,
        dmx_tx_gpio=args.dmx_tx_gpio,
        dmx_rx_gpio=args.dmx_rx_gpio,
        dmx_rts_gpio=args.dmx_rts_gpio,
        led_gpio=args.led_gpio,
        usb_midi_host=args.usb_midi_host,
        skip_wifi=args.skip_wifi,
        artnet_sync_broadcast=args.artnet_sync_broadcast,
    )
    assert len(blob) == DEVCFG_BLOB_SIZE

    print("Encoded CFG1 blob:")
    print(f"  wifiSsid: {args.wifi_ssid!r}  (password not printed)")
    print(f"  artnetFallbackIp: {format_ipv4(fallback_ip)} (0x{fallback_ip:08x})")
    print(f"  artnetPort: {args.artnet_port}")
    print(f"  artnetSyncBroadcast: {args.artnet_sync_broadcast}")
    print(f"  dmxTxGpio/RxGpio/RtsGpio/ledGpio: {args.dmx_tx_gpio}/{args.dmx_rx_gpio}/{args.dmx_rts_gpio}/{args.led_gpio}")
    print(f"  usbMidiHost: {args.usb_midi_host}  skipWifi: {args.skip_wifi}")

    with tempfile.NamedTemporaryFile(suffix=".cfg1", delete=False) as tmp:
        tmp.write(blob)
        tmp_path = tmp.name

    try:
        run_esptool(
            ["write_flash", hex(DEVCFG_PARTITION_OFFSET), tmp_path],
            args.port, args.dry_run,
        )
        if args.dry_run:
            return 0

        if args.verify:
            with tempfile.NamedTemporaryFile(suffix=".bin") as readback:
                run_esptool(
                    ["read_flash", hex(DEVCFG_PARTITION_OFFSET), hex(DEVCFG_BLOB_SIZE), readback.name],
                    args.port, dry_run=False,
                )
                actual = Path(readback.name).read_bytes()
            if actual != blob:
                print("VERIFY FAILED: what's on the board does not match what was written.")
                return 1
            print("Verified: the board's devcfg partition matches the blob just written.")
    finally:
        Path(tmp_path).unlink(missing_ok=True)

    print("Done. Power-cycle or reset the board to apply the new config.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
