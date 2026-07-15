// wled_packet.h — builds the 24-byte WLED UDP Notifier packet (protocol
// version 5). Pure, host-testable, zero allocation: see README_WLED.md for
// the wire layout and tools/generate_wled_maps.py for the effect/palette
// IDs this packet's effect/palette bytes reference.
#pragma once

#include <cstddef>
#include <cstdint>

inline constexpr size_t WLED_PACKET_SIZE = 24;

enum class WledCallMode : uint8_t {
  DirectChange = 0x01,   // color/brightness/power changes
  EffectChanged = 0x06,  // effect id/speed/intensity/palette changes
};

struct WledPacketParams {
  uint8_t effect = 0;
  uint8_t speed = 0;
  uint8_t intensity = 0;
  uint8_t brightness = 255;
  uint8_t r = 0, g = 0, b = 0;
  uint8_t white = 0;
  uint8_t palette = 0;
  uint16_t transitionMs = 0;
  WledCallMode callMode = WledCallMode::EffectChanged;
};

// Fills packet[0..WLED_PACKET_SIZE-1] per the WLED UDP Notifier wire format.
// `packet` must point at at least WLED_PACKET_SIZE bytes.
void buildWledPacket(const WledPacketParams& p, uint8_t* packet);
