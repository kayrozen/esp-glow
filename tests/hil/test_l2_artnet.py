"""
L2 — Art-Net Output Tests

Fully automated, no instruments required.

The host listens on UDP 6454 (Art-Net) and validates:
- Packets arrive at ~44 Hz (±15%)
- Art-Net ID "Art-Net\0" present
- OpCode 0x5000 (DMX output)
- Universe field matches expected values
- Payload length is 512
- RGB data changes over time (not static)
"""

import pytest
import socket
import struct
import time
import threading
from typing import Optional, List, Tuple


class ArtNetListener:
    """Listen for Art-Net packets on UDP 6454."""

    def __init__(self, host: str = "0.0.0.0", port: int = 6454):
        self.host = host
        self.port = port
        self.sock: Optional[socket.socket] = None
        self.packets: List[Tuple[bytes, Tuple[str, int]]] = []
        self.running = False
        self.thread: Optional[threading.Thread] = None

    def start(self):
        """Start listening for packets."""
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((self.host, self.port))
        self.sock.settimeout(0.1)

        self.running = True
        self.packets = []

        self.thread = threading.Thread(target=self._listen_loop, daemon=True)
        self.thread.start()

    def _listen_loop(self):
        """Background thread that listens for packets."""
        while self.running:
            try:
                data, addr = self.sock.recvfrom(1024)
                self.packets.append((data, addr))
            except socket.timeout:
                pass
            except Exception:
                pass

    def stop(self):
        """Stop listening and close socket."""
        self.running = False
        if self.thread:
            self.thread.join(timeout=1.0)
        if self.sock:
            self.sock.close()

    def clear(self):
        """Clear packet buffer."""
        self.packets = []

    def get_packets(self) -> List[bytes]:
        """Get all received packet data."""
        return [data for data, _ in self.packets]


def parse_artnet_packet(data: bytes) -> Optional[dict]:
    """
    Parse Art-Net packet structure.

    Returns dict with: id, opcode, universe, length, payload
    Or None if not a valid Art-Net packet.
    """
    if len(data) < 18:
        return None

    # Check ID string "Art-Net\0"
    artnet_id = data[0:8]
    if artnet_id != b"Art-Net\0":
        return None

    # OpCode (little-endian, offset 8-9)
    opcode = struct.unpack("<H", data[8:10])[0]

    # Protocol version (offset 10-11, big-endian)
    # proto_ver = struct.unpack(">H", data[10:12])[0]

    # Sequence (offset 12)
    # sequence = data[12]

    # Physical input (offset 13)
    # physical = data[13]

    # Universe (offset 14-15, big-endian)
    universe = struct.unpack(">H", data[14:16])[0]

    # DMX data length (offset 16-17, big-endian)
    dmx_len = struct.unpack(">H", data[16:18])[0]

    # DMX payload (offset 18+)
    payload = data[18 : 18 + dmx_len]

    return {
        "id": artnet_id,
        "opcode": opcode,
        "universe": universe,
        "length": dmx_len,
        "payload": payload,
    }


@pytest.fixture
def artnet_listener(device_ip: str) -> ArtNetListener:
    """Fixture that provides an Art-Net listener."""
    listener = ArtNetListener(host="0.0.0.0", port=6454)
    listener.start()
    yield listener
    listener.stop()


class TestArtNetOutput:
    """L2: Art-Net output validation."""

    def test_artnet_packets_received(
        self, device_reset, serial_reader, artnet_listener: ArtNetListener
    ):
        """Assert Art-Net packets arrive within 10 seconds of boot."""
        device_reset()

        # Give device time to send first packet
        time.sleep(1.0)

        packets = artnet_listener.get_packets()
        assert len(packets) > 0, "No Art-Net packets received within 10 seconds of boot"

    def test_artnet_id_valid(
        self, device_reset, serial_reader, artnet_listener: ArtNetListener
    ):
        """Assert all Art-Net packets have the correct ID string."""
        device_reset()

        time.sleep(1.0)

        packets = artnet_listener.get_packets()
        assert len(packets) > 0, "No packets to validate"

        for i, pkt in enumerate(packets):
            parsed = parse_artnet_packet(pkt)
            assert parsed is not None, f"Packet {i} failed to parse"
            assert parsed["id"] == b"Art-Net\0", f"Packet {i} has wrong ID"

    def test_artnet_opcode_dmx(
        self, device_reset, serial_reader, artnet_listener: ArtNetListener
    ):
        """Assert all Art-Net packets have OpCode 0x5000 (DMX output)."""
        device_reset()

        time.sleep(1.0)

        packets = artnet_listener.get_packets()

        for i, pkt in enumerate(packets):
            parsed = parse_artnet_packet(pkt)
            assert parsed is not None, f"Packet {i} failed to parse"
            assert parsed["opcode"] == 0x5000, (
                f"Packet {i} has opcode {hex(parsed['opcode'])}, expected 0x5000"
            )

    def test_artnet_universe_valid(
        self, device_reset, serial_reader, artnet_listener: ArtNetListener
    ):
        """
        Assert Art-Net packets have valid universe fields.

        For a 2-matrix system, expect universes 0 and 1 (or similar).
        """
        device_reset()

        time.sleep(1.0)

        packets = artnet_listener.get_packets()

        universes = set()
        for pkt in packets:
            parsed = parse_artnet_packet(pkt)
            if parsed:
                universes.add(parsed["universe"])

        # Should have at least 1 universe
        assert len(universes) >= 1, "No valid universes found"

        # All universes should be reasonable (0-15 is standard DMX)
        for u in universes:
            assert 0 <= u <= 15, f"Universe {u} outside valid range [0, 15]"

    def test_artnet_payload_length(
        self, device_reset, serial_reader, artnet_listener: ArtNetListener
    ):
        """Assert all Art-Net packets have 512-byte DMX payload."""
        device_reset()

        time.sleep(1.0)

        packets = artnet_listener.get_packets()

        for i, pkt in enumerate(packets):
            parsed = parse_artnet_packet(pkt)
            assert parsed is not None
            assert parsed["length"] == 512, (
                f"Packet {i} has length {parsed['length']}, expected 512"
            )
            assert len(parsed["payload"]) == 512, (
                f"Packet {i} payload is {len(parsed['payload'])} bytes, expected 512"
            )

    def test_artnet_frame_rate_nominal(
        self, device_reset, serial_reader, artnet_listener: ArtNetListener
    ):
        """
        Assert Art-Net packets arrive at ~44 Hz (±15%).

        Collect for 5 seconds and compute packet rate.
        """
        device_reset()

        time.sleep(0.5)

        # Clear any boot-time packets
        artnet_listener.clear()

        # Collect for 5 seconds
        time.sleep(5.0)

        packets = artnet_listener.get_packets()
        assert len(packets) > 0, "No packets in 5-second collection window"

        frame_rate = len(packets) / 5.0

        # 44 Hz ±15% = [37.4, 50.6]
        expected_min = 44 * 0.85
        expected_max = 44 * 1.15

        assert (
            expected_min <= frame_rate <= expected_max
        ), f"Frame rate {frame_rate:.1f} Hz outside [{expected_min:.1f}, {expected_max:.1f}]"

    def test_artnet_payload_changes_over_time(
        self, device_reset, serial_reader, artnet_listener: ArtNetListener
    ):
        """
        Assert DMX payload changes over time (not static).

        Collects packets for 2 seconds and verifies at least 2 different payloads.
        This proves the render loop is updating the output, not just repeating.
        """
        device_reset()

        time.sleep(0.5)

        artnet_listener.clear()

        # Collect for 2 seconds
        time.sleep(2.0)

        packets = artnet_listener.get_packets()
        assert len(packets) > 5, "Need at least 5 packets to test payload changes"

        # Extract payloads
        payloads = []
        for pkt in packets:
            parsed = parse_artnet_packet(pkt)
            if parsed:
                payloads.append(bytes(parsed["payload"]))

        # Check for at least 2 different payloads
        unique_payloads = len(set(payloads))
        assert unique_payloads >= 2, (
            f"Expected at least 2 unique payloads, got {unique_payloads} "
            "(render loop may not be updating output)"
        )

    def test_artnet_no_crash_during_stream(
        self, device_reset, serial_reader, artnet_listener: ArtNetListener
    ):
        """
        Assert no crash markers while Art-Net is streaming (5 seconds).
        """
        device_reset()
        serial_reader.assert_no_crash(duration_s=5.0)
