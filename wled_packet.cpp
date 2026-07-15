#include "wled_packet.h"

#include <cstring>

void buildWledPacket(const WledPacketParams& p, uint8_t* packet) {
  std::memset(packet, 0, WLED_PACKET_SIZE);

  packet[0]  = 0x00;                                  // packet_purpose: 0 = notifier protocol
  packet[1]  = static_cast<uint8_t>(p.callMode);       // callMode
  packet[2]  = p.brightness;                           // master brightness
  packet[3]  = p.r;                                    // primary R
  packet[4]  = p.g;                                    // primary G
  packet[5]  = p.b;                                    // primary B
  packet[6]  = 0;                                       // nightlightActive
  packet[7]  = 0;                                       // nightlightDelayMins
  packet[8]  = p.effect;                                // effectCurrent
  packet[9]  = p.speed;                                 // effectSpeed
  packet[10] = p.white;                                 // primary white channel
  packet[11] = 0x05;                                    // protocol version 5 (palettes supported)
  packet[12] = 0;                                       // colSec[0]
  packet[13] = 0;                                       // colSec[1]
  packet[14] = 0;                                       // colSec[2]
  packet[15] = 0;                                       // whiteSec
  packet[16] = p.intensity;                             // effectIntensity
  packet[17] = static_cast<uint8_t>((p.transitionMs >> 8) & 0xFF);  // transitionDelay MSB
  packet[18] = static_cast<uint8_t>(p.transitionMs & 0xFF);         // transitionDelay LSB
  packet[19] = p.palette;                               // effectPalette
  // bytes 20-23 (reserved) already zeroed by memset above.
}
