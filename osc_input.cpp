#ifdef ESP_PLATFORM

//
// OSC input — device transport.
//
// Listens on a UDP socket and parses each datagram into ControlEvents via
// parseOsc (osc_parser.h, host-tested), pushing them to the control-event
// queue. The render task drains the queue via pumpControlEvents() and
// dispatches to LiveControl — the transport never touches
// LiveControl/ShowController directly, eliminating the cross-core data
// race. See control_queue.h for the rationale.
//
// Bundles, full type-tag support, and matching multiple bound addresses
// per packet are out of scope (see osc_parser.h) — one address, one
// float/int arg, same as a MIDI CC.
//

#include "osc_input.h"

#include "control_queue.h"  // IControlEventQueue, ControlEvent (transitively)

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include <cerrno>
#include <cstdint>

static const char* TAG = "osc_input";

static IControlEventQueue* g_queue = nullptr;
static OscAddressMap g_map = {nullptr, 0};
static uint16_t g_port = 9000;

void osc_input_init(IControlEventQueue& queue, const OscAddressMap& map, uint16_t udpPort) {
  g_queue = &queue;
  g_map = map;
  g_port = udpPort;
}

void osc_input_handle_packet(const uint8_t* packet, size_t len) {
  if (g_queue == nullptr) return;

  ControlEvent ev;
  if (parseOsc(packet, len, g_map, ev)) {
    g_queue->push(ev);
  }
}

void osc_server_task(void* /*ctx*/) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(TAG, "socket(): errno %d", errno);
    vTaskDelete(nullptr);
    return;
  }

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(g_port);
  if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    ESP_LOGE(TAG, "bind(port=%u): errno %d", static_cast<unsigned>(g_port), errno);
    close(sock);
    vTaskDelete(nullptr);
    return;
  }

  ESP_LOGI(TAG, "OSC UDP listening on port %u", static_cast<unsigned>(g_port));

  uint8_t buf[512];
  while (true) {
    int n = recvfrom(sock, buf, sizeof(buf), 0, nullptr, nullptr);
    if (n > 0) {
      osc_input_handle_packet(buf, static_cast<size_t>(n));
    }
  }
}

#endif
