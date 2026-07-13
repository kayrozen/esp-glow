#ifdef ESP_PLATFORM

//
// Pro DJ Link input — device transport. Passive only: two UDP sockets,
// no packets ever sent (see djlink_parser.h's header for the scope this
// deliberately stays inside of).
//

#include "djlink_input.h"

#include "beat_queue.h"          // glow::IBeatEventQueue, glow::BeatEvent
#include "djlink_master_tracker.h"
#include "djlink_parser.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include <cerrno>
#include <cstdint>

static const char* TAG = "djlink_input";

static constexpr uint16_t kBeatPort = 50001;
static constexpr uint16_t kStatusPort = 50002;

static glow::IBeatEventQueue* g_queue = nullptr;
static glow::DjLinkMasterTracker g_masterTracker;

void djlink_input_init(glow::IBeatEventQueue& queue) {
  g_queue = &queue;
}

void djlink_input_handle_beat_packet(const uint8_t* packet, int len, uint64_t tUs) {
  if (g_queue == nullptr || len <= 0) return;

  glow::DjLinkBeatPacket parsed;
  if (!glow::parseDjLinkBeatPacket(packet, static_cast<size_t>(len), tUs, parsed)) return;

  // "Prefer the one flagged as tempo master; ignore the rest" (falls back
  // to accepting anyone if no master has been identified yet -- see
  // djlink_master_tracker.h).
  if (!g_masterTracker.shouldAccept(parsed.deviceNumber)) return;

  g_queue->push(parsed.event);
}

void djlink_input_handle_status_packet(const uint8_t* packet, int len) {
  if (len <= 0) return;

  uint8_t deviceNumber = 0;
  bool isMaster = false;
  if (!glow::parseDjLinkMasterFlag(packet, static_cast<size_t>(len), deviceNumber, isMaster)) return;

  g_masterTracker.update(deviceNumber, isMaster);
}

namespace {

int bindUdpBroadcastSocket(uint16_t port) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(TAG, "socket(port=%u): errno %d", static_cast<unsigned>(port), errno);
    return -1;
  }
  int reuse = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    ESP_LOGE(TAG, "bind(port=%u): errno %d", static_cast<unsigned>(port), errno);
    close(sock);
    return -1;
  }
  return sock;
}

}  // namespace

void djlink_beat_task(void* /*ctx*/) {
  int sock = bindUdpBroadcastSocket(kBeatPort);
  if (sock < 0) {
    vTaskDelete(nullptr);
    return;
  }
  ESP_LOGI(TAG, "DJ Link beat listener on UDP port %u", static_cast<unsigned>(kBeatPort));

  uint8_t buf[256];  // Beat packets are 0x60 (96) bytes; generous headroom
  while (true) {
    int n = recvfrom(sock, buf, sizeof(buf), 0, nullptr, nullptr);
    if (n > 0) {
      uint64_t nowUs = static_cast<uint64_t>(esp_timer_get_time());
      djlink_input_handle_beat_packet(buf, n, nowUs);
    }
  }
}

void djlink_status_task(void* /*ctx*/) {
  int sock = bindUdpBroadcastSocket(kStatusPort);
  if (sock < 0) {
    vTaskDelete(nullptr);
    return;
  }
  ESP_LOGI(TAG, "DJ Link status listener on UDP port %u", static_cast<unsigned>(kStatusPort));

  // CDJ status packets range up to 0x200 (512) bytes across firmware
  // versions; this parser only ever reads through byte 0x89 regardless.
  uint8_t buf[600];
  while (true) {
    int n = recvfrom(sock, buf, sizeof(buf), 0, nullptr, nullptr);
    if (n > 0) {
      djlink_input_handle_status_packet(buf, n);
    }
  }
}

#endif  // ESP_PLATFORM
