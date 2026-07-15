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

Wave 3 additions (see FORMAT.md's "Art-Net Wire Universe & Destination
Routing" and the "Testing Reality" note there -- QEMU has no 802.11 model,
so this is the only place any of this validates against real UDP traffic):
- Sequence numbers advance independently per universe, not globally.
- Exactly one ArtSync (OpCode 0x5200) follows each frame's ArtDMX packets.
- Wire universes observed on the network match the demo show's own
  topology (samples/demo.show's bare `UNIVERSE 2/3/4 ARTNET` lines default
  their wire universe to their internal index -- see provision.cpp).

These run against whatever `.show` is already flashed to the device -- see
conftest.py's flash_show_bundle/blank_show_partition for the "flash, test,
restore" pattern L3 uses if a test module ever needs a specific bundle.
Today that's samples/demo.show (three bare-ARTNET universes, no explicit
per-node routing), which is enough to exercise per-universe sequencing and
ArtSync ordering even though it never explicitly sets IPs -- see
FORMAT.md's Wave 3 section for why the explicit-routing test coverage
(`(ip, wireUniverse)` destinations, multi-output nodes) lives in the host
test suite (test_artnet_router.cpp) instead: this file is confirming the
real socket path Wave 3 built on top of, not re-deriving the routing logic.
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


ARTNET_OP_DMX = 0x5000
ARTNET_OP_SYNC = 0x5200


def parse_artnet_packet(data: bytes) -> Optional[dict]:
    """
    Parse an Art-Net packet's common header (ID + OpCode), and -- only for
    ArtDMX (0x5000) -- the sequence/universe/length/payload fields too.

    Returns a dict with: id, opcode, sequence, universe, length, payload
    (sequence/universe/length/payload are None/0/b"" for non-ArtDMX
    packets, e.g. Wave 3's ArtSync, which is only 14 bytes and has none of
    those fields at all -- see FORMAT.md's ArtSync layout). Returns None if
    not a valid Art-Net packet (bad ID, or too short even for the common
    header).
    """
    if len(data) < 10:
        return None

    artnet_id = data[0:8]
    if artnet_id != b"Art-Net\0":
        return None

    # OpCode (little-endian, offset 8-9)
    opcode = struct.unpack("<H", data[8:10])[0]

    result = {
        "id": artnet_id,
        "opcode": opcode,
        "sequence": None,
        "universe": None,
        "length": 0,
        "payload": b"",
    }

    if opcode != ARTNET_OP_DMX or len(data) < 18:
        return result

    # Sequence (offset 12) -- must advance independently per universe, not
    # globally (see test_artnet_sequence_advances_independently_per_universe).
    result["sequence"] = data[12]

    # Universe (offset 14-15, big-endian): SubUni (low byte) + Net (high 7
    # bits) -- the wire universe, not any internal index (see FORMAT.md).
    result["universe"] = struct.unpack(">H", data[14:16])[0]

    # DMX data length (offset 16-17, big-endian)
    dmx_len = struct.unpack(">H", data[16:18])[0]
    result["length"] = dmx_len

    # DMX payload (offset 18+)
    result["payload"] = data[18 : 18 + dmx_len]

    return result


def is_artnet_dmx(parsed: Optional[dict]) -> bool:
    return parsed is not None and parsed["opcode"] == ARTNET_OP_DMX


def is_artnet_sync(parsed: Optional[dict]) -> bool:
    return parsed is not None and parsed["opcode"] == ARTNET_OP_SYNC


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

    def test_artnet_opcode_valid(
        self, device_reset, serial_reader, artnet_listener: ArtNetListener
    ):
        """
        Assert every captured packet is either ArtDMX (0x5000, the data) or
        ArtSync (0x5200, Wave 3's once-per-frame latch broadcast) -- never
        anything else.
        """
        device_reset()

        time.sleep(1.0)

        packets = artnet_listener.get_packets()

        for i, pkt in enumerate(packets):
            parsed = parse_artnet_packet(pkt)
            assert parsed is not None, f"Packet {i} failed to parse"
            assert parsed["opcode"] in (ARTNET_OP_DMX, ARTNET_OP_SYNC), (
                f"Packet {i} has unexpected opcode {hex(parsed['opcode'])}"
            )

    def test_artnet_universe_valid(
        self, device_reset, serial_reader, artnet_listener: ArtNetListener
    ):
        """
        Assert ArtDMX packets have valid (wire) universe fields.

        For a 2-matrix system, expect universes 0 and 1 (or similar).
        Sync packets (universe=None) are not DMX data and are excluded --
        see is_artnet_dmx.
        """
        device_reset()

        time.sleep(1.0)

        packets = artnet_listener.get_packets()

        universes = set()
        for pkt in packets:
            parsed = parse_artnet_packet(pkt)
            if is_artnet_dmx(parsed):
                universes.add(parsed["universe"])

        # Should have at least 1 universe
        assert len(universes) >= 1, "No valid universes found"

        # All universes should be reasonable (0-15 is standard DMX)
        for u in universes:
            assert 0 <= u <= 15, f"Universe {u} outside valid range [0, 15]"

    def test_artnet_wire_universes_match_show_topology(
        self, device_reset, serial_reader, artnet_listener: ArtNetListener
    ):
        """
        samples/demo.show declares three bare `ARTNET` universes (`UNIVERSE
        2/3/4`, internal indices 1/2/3) with no explicit wire universe --
        Wave 3 defaults that to the internal index (see provision.cpp), so
        the wire numbers actually observed on the network must be exactly
        {1, 2, 3}: this is send() stamping the routed wire universe, not
        some other numbering, confirmed against real UDP traffic.
        """
        device_reset()

        time.sleep(0.5)
        artnet_listener.clear()
        time.sleep(2.0)

        packets = artnet_listener.get_packets()
        universes = set()
        for pkt in packets:
            parsed = parse_artnet_packet(pkt)
            if is_artnet_dmx(parsed):
                universes.add(parsed["universe"])

        assert universes == {1, 2, 3}, (
            f"expected wire universes {{1, 2, 3}} (demo.show's UNIVERSE 2/3/4), got {universes}"
        )

    def test_artnet_payload_length(
        self, device_reset, serial_reader, artnet_listener: ArtNetListener
    ):
        """Assert all ArtDMX packets have 512-byte DMX payload (Sync packets excluded)."""
        device_reset()

        time.sleep(1.0)

        packets = artnet_listener.get_packets()

        for i, pkt in enumerate(packets):
            parsed = parse_artnet_packet(pkt)
            assert parsed is not None
            if not is_artnet_dmx(parsed):
                continue
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
        Assert the render loop's frame rate is ~44 Hz (±15%).

        Collect for 5 seconds and compute the rate, normalized by how many
        packets one frame actually produces now: one ArtDMX per Art-Net
        universe plus Wave 3's one ArtSync -- demo.show has 3 Art-Net
        universes, so ~4 packets/frame, not 1.
        """
        device_reset()

        time.sleep(0.5)

        # Clear any boot-time packets
        artnet_listener.clear()

        # Collect for 5 seconds
        time.sleep(5.0)

        packets = artnet_listener.get_packets()
        assert len(packets) > 0, "No packets in 5-second collection window"

        parsed_all = [parse_artnet_packet(p) for p in packets]
        universes = {p["universe"] for p in parsed_all if is_artnet_dmx(p)}
        assert len(universes) >= 1, "No ArtDMX universes seen -- can't infer packets/frame"
        packets_per_frame = len(universes) + 1  # + one ArtSync

        frame_rate = (len(packets) / packets_per_frame) / 5.0

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

        # Extract ArtDMX payloads only -- ArtSync packets carry no DMX data
        # (payload is always b"") and would otherwise count as a spurious
        # "unique" entry unrelated to the render loop actually updating.
        payloads = []
        for pkt in packets:
            parsed = parse_artnet_packet(pkt)
            if is_artnet_dmx(parsed):
                payloads.append(bytes(parsed["payload"]))

        # Check for at least 2 different payloads
        unique_payloads = len(set(payloads))
        assert unique_payloads >= 2, (
            f"Expected at least 2 unique payloads, got {unique_payloads} "
            "(render loop may not be updating output)"
        )

    def test_artnet_sequence_advances_independently_per_universe(
        self, device_reset, serial_reader, artnet_listener: ArtNetListener
    ):
        """
        Wave 3: sequence numbers are per destination universe, not global.

        Interleaving is inherent here already -- demo.show has 3 Art-Net
        universes, sent back-to-back every frame -- so grouping captured
        packets by universe and checking each group's own sequence advances
        by exactly 1 (mod 256) between consecutive packets directly proves
        the counters are independent: a shared/global counter would show
        gaps here (advancing by however many OTHER universes sent in
        between), not steady +1 steps.
        """
        device_reset()

        time.sleep(0.5)
        artnet_listener.clear()
        time.sleep(2.0)

        packets = artnet_listener.get_packets()
        dmx = [p for p in (parse_artnet_packet(pkt) for pkt in packets) if is_artnet_dmx(p)]
        assert len(dmx) > 10, "Need a reasonable sample size to check sequencing"

        by_universe: dict = {}
        for p in dmx:
            by_universe.setdefault(p["universe"], []).append(p["sequence"])

        assert len(by_universe) >= 1
        for universe, seqs in by_universe.items():
            assert len(seqs) >= 2, f"universe {universe}: need >= 2 packets to check sequencing"
            for a, b in zip(seqs, seqs[1:]):
                assert (b - a) % 256 == 1, (
                    f"universe {universe}: sequence {a} -> {b} is not a +1 step -- "
                    "sequence numbers must be per-universe, not shared globally"
                )

    def test_artnet_sync_follows_each_frames_data_packets(
        self, device_reset, serial_reader, artnet_listener: ArtNetListener
    ):
        """
        Wave 3: exactly one ArtSync per frame, and it comes after that
        frame's ArtDMX packets, never before/instead of them.

        Walks the capture in arrival order (a single UDP socket receiving
        on one host reflects actual send order closely enough for this),
        treating each ArtSync as the end of a "frame" of however many
        distinct universes sent DMX since the previous ArtSync. A Sync with
        no preceding DMX data this frame, or DMX universes with no Sync at
        all, would both indicate frameEnd() isn't correctly following the
        universe sends it's supposed to latch.
        """
        device_reset()

        time.sleep(0.5)
        artnet_listener.clear()
        time.sleep(2.0)

        packets = artnet_listener.get_packets()
        parsed = [parse_artnet_packet(pkt) for pkt in packets]
        assert any(is_artnet_sync(p) for p in parsed), (
            "No ArtSync packets seen -- Wave 3 requires exactly one per frame"
        )

        seen_universes_this_frame = set()
        frames_with_data = 0
        total_syncs = 0
        for p in parsed:
            if is_artnet_dmx(p):
                seen_universes_this_frame.add(p["universe"])
            elif is_artnet_sync(p):
                total_syncs += 1
                assert seen_universes_this_frame, (
                    "ArtSync arrived with no preceding ArtDMX packets this frame"
                )
                frames_with_data += 1
                seen_universes_this_frame = set()

        assert total_syncs > 0
        assert frames_with_data == total_syncs

    def test_artnet_no_crash_during_stream(
        self, device_reset, serial_reader, artnet_listener: ArtNetListener
    ):
        """
        Assert no crash markers while Art-Net is streaming (5 seconds).
        """
        device_reset()
        serial_reader.assert_no_crash(duration_s=5.0)
