#ifdef ESP_PLATFORM

#include "wled_udp_sink.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <fcntl.h>

static const char* TAG = "wled_udp_sink";

WledUdpSink::~WledUdpSink() {
  if (sock_ >= 0) {
    close(sock_);
    sock_ = -1;
  }
}

bool WledUdpSink::begin() {
  sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock_ < 0) {
    ESP_LOGE(TAG, "socket(): errno %d", errno);
    return false;
  }

  // Non-blocking, same rationale and measurement as ArtNetSink::begin(): a
  // blocking sendto() waits for the WiFi driver to actually get the packet
  // on the air (measured in the hundreds of us, not the sub-us cost of a
  // buffer copy), and the render loop pays that cost per target, per
  // frame. Non-blocking turns a busy TX path into an immediate
  // EWOULDBLOCK (dropped packet, logged at debug below) instead of a
  // stall; the next frame is only ~23 ms away.
  int flags = fcntl(sock_, F_GETFL, 0);
  fcntl(sock_, F_SETFL, flags | O_NONBLOCK);

  // WLED targets are addressed by name/IP at send time (unlike ArtNetSink's
  // single connect()ed bridge), so broadcast is enabled unconditionally --
  // any target whose .show IP is 255.255.255.255 needs it, and it's a no-op
  // for unicast sends.
  int bcast = 1;
  setsockopt(sock_, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

  ESP_LOGI(TAG, "WLED UDP sink ready (socket fd=%d)", sock_);
  return true;
}

void WledUdpSink::send(const uint8_t* packet, size_t len, const std::string& ip, uint16_t port) {
  if (sock_ < 0) return;

  struct sockaddr_in dest = {};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(port);
  if (inet_aton(ip.c_str(), &dest.sin_addr) == 0) {
    ESP_LOGW(TAG, "invalid IP/hostname: %s (mDNS hostnames are not resolved here -- "
                  "use a static IP or resolve it into the .show ahead of time)", ip.c_str());
    return;
  }

  int n = sendto(sock_, packet, len, 0, (struct sockaddr*)&dest, sizeof(dest));
  if (n < 0) {
    int e = errno;
    if (e != EAGAIN && e != EWOULDBLOCK) {
      ESP_LOGW(TAG, "sendto(%s:%u): errno %d", ip.c_str(), (unsigned)port, e);
    }
  }
}

#endif  // ESP_PLATFORM
