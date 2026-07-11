// artnet_sink.cpp — device-only Art-Net DMX output via a UDP socket.
//
// F2 fills the former TODO scaffold. The 18-byte Art-Net DMX header layout was
// already correct in the scaffold; this adds: a connected UDP socket, real
// sequence numbering (per-sink, wraps at 255 per the spec), and a non-blocking
// send so the core-1 render loop never stalls on lwIP.
//
// Coexistence: lwIP lives on core 0 (sdkconfig). send() from core 1 is
// thread-safe under ESP-IDF's sockets layer. A short SO_SNDTIMEO bounds any
// back-pressure so a flood does not stall DMX timing on core 1.
#ifdef ESP_PLATFORM

#include "artnet_sink.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include <cstring>

static const char* TAG = "artnet_sink";

ArtNetSink::ArtNetSink(uint32_t bridgeIp, uint16_t bridgePort)
    : ip_(bridgeIp), port_(bridgePort) {}

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

  // Bound send timeout so a stalled bridge cannot stall the render loop.
  // 5 ms is generous for a LAN UDP send; if it fires we log and drop.
  struct timeval tv = {};
  tv.tv_usec = 5 * 1000;
  setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  // Broadcast support if the caller passed 255.255.255.255.
  if (ip_ == 0xFFFFFFFFu) {
    int bcast = 1;
    setsockopt(sock_, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));
  }

  struct sockaddr_in dst = {};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(port_);
  dst.sin_addr.s_addr = htonl(ip_);
  if (connect(sock_, (struct sockaddr*)&dst, sizeof(dst)) != 0) {
    ESP_LOGE(TAG, "connect(%s:%u): errno %d",
             ipaddr_ntoa((const ip4_addr_t*)&ip_), (unsigned)port_, errno);
    close(sock_);
    sock_ = -1;
    return false;
  }

  ESP_LOGI(TAG, "Art-Net -> %s:%u (ready)",
           ipaddr_ntoa((const ip4_addr_t*)&ip_), (unsigned)port_);
  return true;
}

void ArtNetSink::send(uint8_t universeIndex, const uint8_t* data, uint16_t len) {
  if (sock_ < 0) return;
  if (len > DMX_UNIVERSE_SIZE) len = DMX_UNIVERSE_SIZE;

  uint8_t pkt[18 + DMX_UNIVERSE_SIZE];
  // 8-byte ID "Art-Net\0"
  static const char kId[8] = {'A', 'r', 't', '-', 'N', 'e', 't', 0};
  memcpy(pkt, kId, 8);
  pkt[8]  = 0x00; pkt[9]  = 0x50;            // OpCode 0x5000 (OpDmx) LE
  pkt[10] = 0;     pkt[11] = 14;             // ProtVer 14
  pkt[12] = seq_++;                          // Sequence (wraps 0..255)
  pkt[13] = 0;                               // Physical
  pkt[14] = universeIndex & 0xFF;            // SubUni (low byte)
  pkt[15] = (universeIndex >> 8) & 0x7F;     // Net (high byte)
  // Art-Net length is the DMX payload size, big-endian, and must be even.
  uint16_t payload = len;
  if (payload & 1) payload += 1;             // pad to even
  pkt[16] = (payload >> 8) & 0xFF;
  pkt[17] = payload & 0xFF;
  memcpy(pkt + 18, data, len);
  if (payload > len) pkt[18 + len] = 0;      // pad byte

  int n = ::send(sock_, pkt, 18 + payload, 0);
  if (n < 0) {
    // EAGAIN on a non-blocking send just means the bridge is slow this frame;
    // log at debug to avoid flooding. Other errors are real.
    int e = errno;
    if (e != EAGAIN && e != EWOULDBLOCK) {
      ESP_LOGW(TAG, "send(u=%u): errno %d", universeIndex, e);
    }
  }
}

#endif  // ESP_PLATFORM
