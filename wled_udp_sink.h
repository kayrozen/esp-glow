// wled_udp_sink.h — device-only UDP transport for WledManager.
//
// One socket serves every WLED target (unlike ArtNetSink, which connects to
// a single bridge IP, a WLED show can point at many different device IPs
// plus the broadcast address) -- so send() uses sendto() per call instead of
// a connected socket. Mirrors artnet_sink.h's shape otherwise: construct,
// begin() once netif/lwIP is up, non-blocking with a bounded send timeout so
// a slow/unreachable WLED device can never stall the render loop.
#pragma once

#include "wled_manager.h"

#ifdef ESP_PLATFORM
#include <cstdint>

class WledUdpSink : public IWledSink {
public:
  WledUdpSink() = default;
  ~WledUdpSink() override;

  // Create the UDP socket (broadcast-enabled) and bind ephemeral. Call once,
  // after netif/lwIP is up (same "constructed early, begun late" pattern as
  // ArtNetSink -- see main.cpp). Returns false on failure.
  bool begin();

  void send(const uint8_t* packet, size_t len, const std::string& ip, uint16_t port) override;

private:
  int sock_ = -1;
};

#endif  // ESP_PLATFORM
