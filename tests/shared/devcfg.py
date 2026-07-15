"""
CFG1 device config encoder -- the Python twin of device_config.h/.cpp and
web/shared/devcfg.js, used only by tests/qemu/ to build a devcfg blob to
patch into a QEMU flash image (see conftest.py's patch_devcfg_into_image).

No decoder here: the QEMU tests never need to read a CFG1 blob back out of
Python, only build one and let the firmware's own GLOW-TEST: cfg telemetry
prove what it parsed (see test_l1_devcfg.py) -- that's the actual
assertion surface, not a third independent parser implementation.

See device_config.h for the authoritative format spec; keep this file
byte-identical to it.
"""

import struct
import zlib

DEVCFG_BLOB_SIZE = 150
DEVCFG_CRC_OFFSET = 146
DEVCFG_SSID_MAX = 32
DEVCFG_PASS_MAX = 64

DEVCFG_FLAG_USB_MIDI_HOST = 0x01
DEVCFG_FLAG_SKIP_WIFI = 0x02


def _padded(s: str, width: int) -> bytes:
    b = s.encode("utf-8")[:width]
    return b + b"\x00" * (width - len(b))


def encode_devcfg(
    *,
    wifi_ssid: str = "",
    wifi_pass: str = "",
    artnet_fallback_ip: int = 0,
    artnet_port: int = 6454,
    dmx_tx_gpio: int = 17,
    dmx_rx_gpio: int = 18,
    dmx_rts_gpio: int = 8,
    led_gpio: int = 2,
    usb_midi_host: bool = False,
    skip_wifi: bool = False,
) -> bytes:
    """Builds a valid DEVCFG_BLOB_SIZE-byte CFG1 blob, CRC included."""
    flags = 0
    if usb_midi_host:
        flags |= DEVCFG_FLAG_USB_MIDI_HOST
    if skip_wifi:
        flags |= DEVCFG_FLAG_SKIP_WIFI

    body = bytearray()
    body += b"CFG1"
    body += bytes([1])  # version
    body += bytes([flags])
    body += struct.pack("<H", 0)  # reserved
    body += _padded(wifi_ssid, DEVCFG_SSID_MAX)
    body += _padded(wifi_pass, DEVCFG_PASS_MAX)
    body += struct.pack("<I", artnet_fallback_ip & 0xFFFFFFFF)
    body += struct.pack("<H", artnet_port & 0xFFFF)
    body += bytes([dmx_tx_gpio & 0xFF, dmx_rx_gpio & 0xFF, dmx_rts_gpio & 0xFF, led_gpio & 0xFF])
    body += b"\x00" * 32  # reserved2

    assert len(body) == DEVCFG_CRC_OFFSET, f"header/body length drifted: {len(body)} != {DEVCFG_CRC_OFFSET}"

    # Standard CRC-32 (IEEE 802.3 / zlib / PNG) -- zlib.crc32 implements
    # exactly this (poly 0xEDB88320, init/final XOR 0xFFFFFFFF), matching
    # devcfgCrc32 in device_config.cpp/devcfg.js bit-for-bit.
    crc = zlib.crc32(bytes(body)) & 0xFFFFFFFF
    body += struct.pack("<I", crc)

    assert len(body) == DEVCFG_BLOB_SIZE
    return bytes(body)
