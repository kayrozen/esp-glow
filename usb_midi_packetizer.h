#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

// P1.1: packs a raw MIDI message (an INIT SYSEX blob, mdef.h -- typically a
// complete F0...F7 SysEx frame, but this function never inspects the
// bytes' meaning) into USB-MIDI 1.0 "USB-MIDI Event Packets": 4 bytes each
// (cable number 0 in the high nibble of byte 0, Code Index Number in the
// low nibble, then up to 3 raw MIDI data bytes, zero-padded if the group
// is shorter than 3), per the USB Device Class Definition for MIDI Devices
// 1.0 section 4's SysEx CIN convention:
//   CIN 0x4 -- SysEx starts or continues (a full 3-byte group, more to come)
//   CIN 0x5 -- SysEx ends with the following single byte
//   CIN 0x6 -- SysEx ends with the following two bytes
//   CIN 0x7 -- SysEx ends with the following three bytes
// This is the one place raw INIT bytes become USB-MIDI wire packets --
// pure and host-testable, unlike the actual OUT-transfer submission
// (usb_midi_input.cpp, device-only, no host equivalent to test against).
//
// Appends onto `out` (does not clear it first), so several blobs can be
// packed back-to-back into one contiguous packet stream ahead of a single
// OUT transfer submission. A `len == 0` call appends nothing.
void packUsbMidiEventPackets(const uint8_t* bytes, size_t len, std::vector<uint8_t>& out);
