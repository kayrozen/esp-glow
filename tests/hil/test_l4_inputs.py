"""
L4 — Inputs (Web/OSC/MIDI)

- Web: fully automated round-trip. Connect to ws://<ip>/ws -> `config`
  arrives immediately (web_input.cpp's ws_handler sends it on the initial
  GET handshake, no `hello` needed). Send `{"type":"cue","id":0,"pressed":
  true}` -> the `state` broadcast lists cue 0 active.
- OSC: fully automated. UDP packet to /cue/0 (see main.cpp's g_oscBindings)
  activates the same cue; state broadcast confirms it over WS.
- MIDI: needs a USB-MIDI gadget or a second board to inject notes. Marked
  semi-automated (skipped) here -- the parser itself is host-tested
  (test_... none yet for MIDI parsing directly, but midi_input.cpp's
  contract mirrors osc_parser.cpp's, which is).
"""

import json
import socket
import struct
import time

import pytest
import websocket

WS_TIMEOUT = 5.0


def _osc_packet(address: str, value: int) -> bytes:
    def pad4(b: bytes) -> bytes:
        return b + b"\x00" * (4 - (len(b) % 4))

    addr = pad4(address.encode() + b"\x00")
    type_tag = pad4(b",i\x00")
    return addr + type_tag + struct.pack(">i", value)


def _drain(ws) -> list:
    ws.settimeout(0.3)
    msgs = []
    try:
        while True:
            msgs.append(json.loads(ws.recv()))
    except Exception:
        pass
    return msgs


class TestWebInput:
    def test_websocket_connect_receives_config(self, device_reset, serial_reader, device_ip: str):
        device_reset()
        ws = websocket.create_connection(f"ws://{device_ip}/ws", timeout=WS_TIMEOUT)
        try:
            ws.settimeout(WS_TIMEOUT)
            msg = json.loads(ws.recv())
            assert msg.get("type") == "config", f"Expected config on connect, got {msg}"
            assert "cues" in msg and "hasMaster" in msg
        finally:
            ws.close()

    def test_websocket_cue_toggle_state_broadcast(self, device_reset, serial_reader, device_ip: str):
        device_reset()
        ws = websocket.create_connection(f"ws://{device_ip}/ws", timeout=WS_TIMEOUT)
        try:
            _drain(ws)  # the initial config message
            ws.send(json.dumps({"type": "cue", "id": 0, "pressed": True}))

            ws.settimeout(WS_TIMEOUT)
            deadline = time.time() + WS_TIMEOUT
            state = None
            while time.time() < deadline:
                try:
                    msg = json.loads(ws.recv())
                except Exception:
                    break
                if msg.get("type") == "state":
                    state = msg
                    break
            assert state is not None, "No state broadcast after cue press"
            assert 0 in state.get("active", []), f"Cue 0 not listed active: {state}"
        finally:
            ws.close()

    def test_web_no_crash_on_connect_disconnect(self, device_reset, serial_reader, device_ip: str):
        device_reset()
        for _ in range(3):
            ws = websocket.create_connection(f"ws://{device_ip}/ws", timeout=WS_TIMEOUT)
            time.sleep(0.1)
            ws.close()
            time.sleep(0.3)
        serial_reader.assert_no_crash(duration_s=3.0)


class TestOSCInput:
    def test_osc_cue_trigger_reflected_in_ws_state(self, device_reset, serial_reader, device_ip: str):
        """OSC /cue/0 (see main.cpp's g_oscBindings) activates the same cue as WS id 0."""
        device_reset()
        ws = websocket.create_connection(f"ws://{device_ip}/ws", timeout=WS_TIMEOUT)
        try:
            _drain(ws)
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.sendto(_osc_packet("/cue/0", 1), (device_ip, 9000))
            sock.close()

            ws.settimeout(WS_TIMEOUT)
            deadline = time.time() + WS_TIMEOUT
            state = None
            while time.time() < deadline:
                try:
                    msg = json.loads(ws.recv())
                except Exception:
                    break
                if msg.get("type") == "state":
                    state = msg
                    break
            assert state is not None, "No state broadcast after OSC cue trigger"
            assert 0 in state.get("active", []), f"Cue 0 not listed active after OSC trigger: {state}"
        finally:
            ws.close()

    def test_osc_no_crash_on_packet_flood(self, device_reset, serial_reader, device_ip: str):
        device_reset()
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for i in range(20):
            sock.sendto(_osc_packet("/cue/0", i % 2), (device_ip, 9000))
            time.sleep(0.02)
        sock.close()
        serial_reader.assert_no_crash(duration_s=2.0)


class TestMIDIInput:
    """MIDI needs a USB-MIDI gadget or a second board to inject notes -- semi-automated."""

    def test_midi_input_no_crash_on_boot(self, device_reset, serial_reader):
        device_reset()
        serial_reader.assert_no_crash(duration_s=3.0)

    @pytest.mark.skip(reason="semi-automated: requires a USB-MIDI gadget or a second board to inject notes")
    def test_midi_note_triggers_cue(self, device_reset, serial_reader, device_ip: str):
        pass
