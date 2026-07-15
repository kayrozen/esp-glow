// artnet_sink.cpp — device-only Art-Net DMX output via one unconnected UDP
// socket + sendto() per packet.
//
// Wave 3: routing (which IP, which wire universe), packet building, and
// per-universe sequence numbers all moved into ArtNetRouter (portable,
// host-tested -- see artnet_router.h/.cpp and its test). This file is now
// just the device-only half: own the socket, implement IArtNetTransport's
// sendTo() via ::sendto(), and forward setDest/send/frameEnd to the router.
//
// One socket for every destination (not one per node): sendto() picks the
// destination per packet, so adding a second/third Art-Net node costs
// nothing here -- see ArtNetRouter for the actual per-node routing.
//
// Coexistence: lwIP lives on core 0 (sdkconfig). send()/frameEnd() from
// core 1 (the render task) are thread-safe under ESP-IDF's sockets layer.
// A short SO_SNDTIMEO bounds any back-pressure so a flood does not stall
// DMX timing on core 1.
#ifdef ESP_PLATFORM

#include "artnet_sink.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"

static const char* TAG = "artnet_sink";

ArtNetSink::ArtNetSink(uint16_t port, uint32_t fallbackIp)
    : router_(fallbackIp, port) {}

ArtNetSink::~ArtNetSink() {
  if (sock_ >= 0) {
    close(sock_);
    sock_ = -1;
  }
}

bool ArtNetSink::begin() {
  sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock_ < 0) {
    ESP_LOGE(TAG, "socket(): errno %d", errno);
    return false;
  }

  // Bound send timeout so a stalled node cannot stall the render loop.
  // 5 ms is generous for a LAN UDP send; if it fires we log and drop.
  struct timeval tv = {};
  tv.tv_usec = 5 * 1000;
  setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  // Always enabled: any universe can resolve to broadcast (an unrouted
  // universe with fallbackIp==0), and ArtSync itself is always broadcast
  // (see frameEnd/sendTo) -- there is no destination this socket sends to
  // that isn't potentially broadcast.
  int bcast = 1;
  setsockopt(sock_, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

  ESP_LOGI(TAG, "Art-Net socket ready (unconnected, routes per universe)");
  return true;
}

void ArtNetSink::setDest(uint8_t universeIndex, const ArtNetDest& d) {
  router_.setDest(universeIndex, d);
}

void ArtNetSink::send(uint8_t universeIndex, const uint8_t* data, uint16_t len) {
  if (sock_ < 0) return;
  router_.send(universeIndex, data, len, *this);
}

void ArtNetSink::frameEnd() {
  if (sock_ < 0) return;
  router_.frameEnd(*this);
}

void ArtNetSink::sendTo(uint32_t ip, uint16_t port, const uint8_t* data, uint16_t len) {
  struct sockaddr_in dst = {};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(port);
  dst.sin_addr.s_addr = htonl(ip);

  int n = ::sendto(sock_, data, len, 0, (struct sockaddr*)&dst, sizeof(dst));
  if (n < 0) {
    // EAGAIN on a bounded-timeout send just means this node/broadcast was
    // slow this frame; log at debug to avoid flooding. Other errors are
    // real (e.g. a genuinely unreachable unicast destination).
    int e = errno;
    if (e != EAGAIN && e != EWOULDBLOCK) {
      ip4_addr_t addr = {htonl(ip)};
      ESP_LOGW(TAG, "sendto(%s:%u): errno %d", ip4addr_ntoa(&addr), (unsigned)port, e);
    }
  }
}

#endif  // ESP_PLATFORM
