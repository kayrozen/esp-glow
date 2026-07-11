// artnet_sink.h — device-only Art-Net DMX output sink.
//
// Declares ArtNetSink, a concrete IUniverseSink that builds an Art-Net DMX
// packet (OpCode 0x5000) and UDP-sends it to a bridge IP:port. One instance
// can serve multiple universes: send() stamps the universeIndex into each
// packet, so configureUniverse(1, Raw, &artnet) and (2, Raw, &artnet) both
// work with a single socket.
#pragma once

#include "show.h"

#ifdef ESP_PLATFORM
#include "lwip/ip4_addr.h"
#include <cstdint>

class ArtNetSink : public IUniverseSink {
public:
  // bridgeIp: IPv4 of the Art-Net node/bridge (e.g. your ESP832/PixLite).
  //   Use IPADDR_BROADCAST (255.255.255.255) for LAN broadcast Art-Net.
  // bridgePort: typically 6454 (ART_NET_PORT).
  ArtNetSink(uint32_t bridgeIp, uint16_t bridgePort);

  ~ArtNetSink() override;

  // Create and connect the UDP socket. Call once. Returns false on failure.
  // Uses a connected UDP socket (connect() then send()) so per-packet routing
  // is cheap and the render loop on core 1 never blocks for long.
  bool begin();

  void send(uint8_t universeIndex, const uint8_t* data, uint16_t len) override;

private:
  uint32_t ip_;
  uint16_t port_;
  int      sock_ = -1;
  uint8_t  seq_  = 0;   // Art-Net sequence number, wraps at 255.
};

#endif  // ESP_PLATFORM
