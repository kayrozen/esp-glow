#ifdef ESP_PLATFORM

#include "artnet_discovery_task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include <cerrno>

static const char* TAG = "artnet_discovery";

static ArtNetSink* g_sink = nullptr;
static ArtNetDest g_showDest[MAX_UNIVERSES];
static uint8_t g_universeCount = 0;

// Owned entirely by artnet_discovery_task -- only ever touched from that
// one task, so it needs no locking of its own. The snapshot below is the
// only piece other tasks (the web console's httpd handler) ever read.
static ArtNetDiscovery g_discovery(/*timeoutSec=*/10.0f);

static portMUX_TYPE s_snapshotLock = portMUX_INITIALIZER_UNLOCKED;
static constexpr size_t kSnapshotCap = 16;
static DiscoveredNode s_snapshot[kSnapshotCap];
static size_t s_snapshotCount = 0;

void artnet_discovery_task_init(ArtNetSink* artnetSink, const ArtNetDest showDest[MAX_UNIVERSES],
                                 uint8_t universeCount) {
  g_sink = artnetSink;
  if (universeCount > MAX_UNIVERSES) universeCount = MAX_UNIVERSES;
  g_universeCount = universeCount;
  for (uint8_t i = 0; i < universeCount; ++i) g_showDest[i] = showDest[i];
}

static float nowSeconds() {
  return static_cast<float>(esp_timer_get_time()) / 1000000.0f;
}

static void publishSnapshot() {
  size_t n = g_discovery.nodeCount();
  if (n > kSnapshotCap) n = kSnapshotCap;

  portENTER_CRITICAL(&s_snapshotLock);
  for (size_t i = 0; i < n; ++i) {
    const DiscoveredNode* node = g_discovery.node(i);
    if (node) s_snapshot[i] = *node;
  }
  s_snapshotCount = n;
  portEXIT_CRITICAL(&s_snapshotLock);
}

void artnet_discovery_task(void* /*ctx*/) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(TAG, "socket(): errno %d", errno);
    vTaskDelete(nullptr);
    return;
  }

  int reuse = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  int bcast = 1;
  setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(ARTNET_PORT);
  if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    ESP_LOGE(TAG, "bind(port=%u): errno %d", static_cast<unsigned>(ARTNET_PORT), errno);
    close(sock);
    vTaskDelete(nullptr);
    return;
  }

  // Bounded recv so this task wakes up periodically to re-poll/re-resolve
  // (expire stale nodes, retry setDest) even with zero inbound traffic.
  struct timeval tv = {};
  tv.tv_sec = 1;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  ESP_LOGI(TAG, "ArtPoll discovery listening on port %u", static_cast<unsigned>(ARTNET_PORT));

  uint8_t pollPkt[ARTNET_POLL_PACKET_SIZE];
  uint16_t pollLen = buildArtPollPacket(pollPkt);

  // Generous for one ArtPollReply (fixed 239 bytes) with headroom for a
  // future Art-Net 4 node sending a longer one; still bounds-checked by
  // parseArtPollReply regardless of what actually arrives.
  uint8_t rxBuf[512];

  uint64_t lastPollUs = 0;
  const uint64_t kPollIntervalUs = 3 * 1000000;  // frequent enough to notice
                                                  // a node appearing quickly

  while (true) {
    uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
    if (now - lastPollUs >= kPollIntervalUs) {
      struct sockaddr_in dst = {};
      dst.sin_family = AF_INET;
      dst.sin_port = htons(ARTNET_PORT);
      dst.sin_addr.s_addr = htonl(0xFFFFFFFFu);  // ArtPoll is always broadcast
      sendto(sock, pollPkt, pollLen, 0, reinterpret_cast<struct sockaddr*>(&dst), sizeof(dst));
      lastPollUs = now;
    }

    int n = recvfrom(sock, rxBuf, sizeof(rxBuf), 0, nullptr, nullptr);
    if (n > 0) {
      ArtNetPollReply reply;
      if (parseArtPollReply(rxBuf, static_cast<size_t>(n), reply)) {
        g_discovery.onReply(reply, nowSeconds());
      }
    }

    g_discovery.expire(nowSeconds());

    if (g_sink) {
      ArtNetDest resolved[MAX_UNIVERSES];
      resolveDiscoveredDests(g_showDest, g_universeCount, g_discovery, resolved);
      for (uint8_t u = 0; u < g_universeCount; ++u) {
        // resolveDiscoveredDests already leaves a .show-explicit route
        // (ip != 0) untouched; only re-apply what it actually resolves
        // for the universes the .show left unspecified.
        if (g_showDest[u].ip == 0) {
          g_sink->setDest(u, resolved[u]);
        }
      }
    }

    publishSnapshot();
  }
}

size_t artnet_discovery_node_count() {
  portENTER_CRITICAL(&s_snapshotLock);
  size_t n = s_snapshotCount;
  portEXIT_CRITICAL(&s_snapshotLock);
  return n;
}

bool artnet_discovery_node_at(size_t i, DiscoveredNode& out) {
  portENTER_CRITICAL(&s_snapshotLock);
  bool ok = i < s_snapshotCount;
  if (ok) out = s_snapshot[i];
  portEXIT_CRITICAL(&s_snapshotLock);
  return ok;
}

#endif  // ESP_PLATFORM
