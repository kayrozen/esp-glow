#pragma once

#include <cstdint>

class IControlEventQueue;
struct MidiControllerProfile;

//
// USB-MIDI host input -- device transport (see usb_midi_input.cpp).
//
// Structurally identical to midi_input.cpp for its INPUT direction: parses
// MIDI bytes into ControlEvents with the same host-tested parseMidi
// (live_control.h) and pushes them onto the same control queue. The
// render task is the only consumer of that queue; this transport never
// touches LiveControl/ShowController/the Lua VM directly -- same
// invariant as every other input (see control_queue.h's rationale).
//
// P1.1 added one OUTPUT capability, deliberately narrow: INIT SYSEX
// send-on-connect (usb_midi_input_init's controllerProfile param, below).
// This is NOT general MIDI OUT -- there is no LED-feedback IMidiOutput
// wired to a USB-MIDI OUT endpoint, and there won't be until a real need
// justifies claiming an OUT endpoint continuously instead of once at
// connect time (a real APC40 is USB-only hardware, and Mode 0 must be set
// before its pads/LEDs behave as the .mdef assumes -- FORMAT.md's
// "INIT SYSEX" -- which is exactly why this one exception exists).
//
// WHERE THIS DIFFERS FROM midi_input.cpp
//   DIN-MIDI is a raw byte stream over UART that this project frames
//   itself (MidiByteReader, midi_realtime.h). USB-MIDI is already framed
//   by the USB Audio Class MIDIStreaming spec into 4-byte "USB-MIDI event
//   packets" (1 cable/CIN byte + 3 MIDI bytes, zero-padded for messages
//   shorter than 3 bytes) -- there is no running status and no realtime-
//   byte-interleaving concern to solve here; USB framing makes that moot.
//   So there is no framing state machine: each packet is handled
//   independently by usb_midi_input_handle_packet.
//
// HARDWARE, READ FIRST (see README_LIVE_CONTROL.md's "Out of Scope" and
// this project's B2 cost/benefit note)
//   USB host means the ESP32 must SUPPLY 5V VBUS to the controller -- a
//   USB-A receptacle, a power path able to source a few hundred mA, and
//   ideally over-current protection. That is a board change, not a
//   software change; this transport is gated behind CONFIG_GLOW_USB_MIDI_HOST
//   (Kconfig.projbuild), OFF by default, so a board without that VBUS path
//   never spins up a USB host stack it can't power. A USB-host-to-DIN
//   adapter (~$20) gets the same result today with no hardware change --
//   this transport is for boards that have made the VBUS change.
//
// LIMITATIONS (inherited from ESP-IDF's usb_host_lib, not this file)
//   - One device at a time (same as every other input transport here).
//   - ESP32-S3's USB-OTG peripheral is full-speed only.
//   - The host task runs on core 0 (with WiFi) -- watch `dropped` frames
//     (render_task.h) for coexistence regressions when USB comes up, same
//     risk class as WiFi.
//

// Initialize the USB-MIDI input layer: the queue it pushes parsed
// ControlEvents to. Must be called before usb_midi_host_task starts.
//
// P1.1: `controllerProfile` (nullptr by default) is the loaded .mdef's
// init blobs (controller_init.h) -- when non-null, every time a
// class-compliant device hot-plugs AND the driver finds a bulk OUT
// endpoint on its MIDIStreaming interface, this transport packs each
// INIT SYSEX blob into USB-MIDI event packets (usb_midi_packetizer.h,
// per the USB-MIDI 1.0 SysEx CIN convention) and submits one OUT
// transfer, once. A device with no OUT endpoint (input-only hardware) or
// a profile with no init blobs is a silent no-op -- same graceful-
// degradation contract as DIN MIDI OUT's txGpio < 0 (midi_input.h).
void usb_midi_input_init(IControlEventQueue& queue, const MidiControllerProfile* controllerProfile = nullptr);

// Parses one 4-byte USB-MIDI event packet (byte 0: cable number + Code
// Index Number: bytes 1..3: the raw MIDI message, zero-padded if shorter
// than 3 bytes) and, if it resolves to a channel message parseMidi
// recognizes (Note On/Off, CC -- see live_control.h), pushes a
// ControlEvent onto the queue passed to usb_midi_input_init. Anything else
// (System Common/Realtime, SysEx, Program Change, Channel Pressure) is
// silently ignored, same as parseMidi already does for the DIN transport
// -- "strip to raw MIDI bytes, call parseMidi, push. Nothing else."
// Exposed separately from the USB transfer callback so it's callable from
// a unit test without a real device (mirrors midi_input_handle_byte's
// contract).
void usb_midi_input_handle_packet(const uint8_t packet[4]);

// FreeRTOS task entry point: installs the USB Host Library, waits for a
// class-compliant USB-MIDI device (USB Audio Class, MIDIStreaming
// subclass, a bulk IN endpoint) to enumerate, claims it, and streams its
// 4-byte event packets into usb_midi_input_handle_packet. Handles
// hot-plug: a disconnect tears down the claimed interface/transfer and
// returns to waiting for the next device -- it never stalls or crashes the
// task. Every state transition (waiting / connected / unsupported device /
// disconnected / error) is reported via ESP_LOG on the serial console.
// Never returns.
void usb_midi_host_task(void* ctx);
