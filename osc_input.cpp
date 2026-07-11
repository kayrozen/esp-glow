<<<<<<< ours
#ifdef ESP_PLATFORM

//
// OSC input — device scaffold.
//
// Parses OSC packets into ControlEvents and pushes them to the
// control-event queue. The render task drains the queue via
// pumpControlEvents() and dispatches to LiveControl — the transports
// no longer touch LiveControl/ShowController directly, eliminating the
// cross-core data race. See control_queue.h for the rationale.
//

#include "control_queue.h"  // IControlEventQueue, ControlEvent (transitively)
#include "live_control.h"   // ControlType

#include <cstdint>
#include <cstring>
#include <cstdio>

static IControlEventQueue* g_queue = nullptr;

void osc_input_init(IControlEventQueue& queue) {
  g_queue = &queue;
}

void osc_input_handle_packet(const uint8_t* packet, size_t len) {
  if (g_queue == nullptr) return;

  // Parse OSC packet: address string (null-terminated) + padding + type tag + padding + args
  // This is a minimal parser for address + one float/int argument only.
  // Full OSC spec (bundles, all type tags, timetags) is out of scope.

  const char* address = reinterpret_cast<const char*>(packet);
  size_t addressLen = std::strlen(address);

  if (addressLen >= len) return;

  size_t padLen = 4 - (addressLen + 1) % 4;
  if (padLen == 4) padLen = 0;
  size_t typeTagOffset = addressLen + 1 + padLen;

  if (typeTagOffset >= len) return;

  const char* typeTag = reinterpret_cast<const char*>(packet + typeTagOffset);
  if (typeTag[0] != ',') return;

  size_t typeTagLen = std::strlen(typeTag);
  padLen = 4 - (typeTagLen + 1) % 4;
  if (padLen == 4) padLen = 0;
  size_t argOffset = typeTagOffset + typeTagLen + 1 + padLen;

  if (argOffset + 4 > len) return;

  ControlEvent ev;
  float value = 0.0f;

  if (typeTag[1] == 'f') {
    uint32_t bits = 0;
    std::memcpy(&bits, packet + argOffset, 4);
    bits = ((bits & 0xFF) << 24) | ((bits & 0xFF00) << 8) |
           ((bits >> 8) & 0xFF00) | ((bits >> 24) & 0xFF);
    std::memcpy(&value, &bits, 4);
  } else if (typeTag[1] == 'i') {
    uint32_t intVal = 0;
    std::memcpy(&intVal, packet + argOffset, 4);
    intVal = ((intVal & 0xFF) << 24) | ((intVal & 0xFF00) << 8) |
             ((intVal >> 8) & 0xFF00) | ((intVal >> 24) & 0xFF);
    value = static_cast<float>(intVal) / 127.0f;
  } else {
    return;
  }

  // TODO: Map address to logical controlId via firmware-provided table
  // For now, extract controlId from address (e.g., "/cue/1" -> id=1)
  // This is application-specific and out of scope for this module.

  uint16_t controlId = 0;
  if (std::sscanf(address, "/cue/%hu", &controlId) == 1) {
    ev.type = ControlType::Button;
    ev.id = controlId;
    ev.pressed = (value > 0.5f);
    ev.value = 0.0f;
  } else if (std::sscanf(address, "/fader/%hu", &controlId) == 1) {
    ev.type = ControlType::Fader;
    ev.id = controlId;
    ev.pressed = false;
    ev.value = value;
  } else {
    return;
  }

  g_queue->push(ev);
}

void osc_server_task() {
  // TODO: receive UDP OSC packets (hardware-specific)
  // for each packet received:
  //   osc_input_handle_packet(buffer, len);
  //
  // The render task calls pumpControlEvents(queue, live, t) at the top
  // of each frame; the OSC transport just pushes events here.
}

#endif
=======
// osc_input.cpp — device-only UDP OSC receiver.
#ifdef ESP_PLATFORM

#include "osc_input.h"
#include "osc_parser.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>

static const char* TAG = "osc_input";
static int s_sock = -1;
static TaskHandle_t s_task = nullptr;
static LiveControl* s_live = nullptr;

static float now_sec() { return (float)(esp_timer_get_time() / 1000000.0); }

static void osc_task(void*) {
  static uint8_t buf[1024];
  while (true) {
    struct sockaddr_in src = {};
    socklen_t sl = sizeof(src);
    int n = recvfrom(s_sock, buf, sizeof(buf), 0,
                     (struct sockaddr*)&src, &sl);
    if (n <= 0) {
      // EINTR / socket closed: small delay then retry.
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (!s_live) continue;
    OscMessage m;
    if (!parseOsc(buf, (size_t)n, m)) continue;
    if (!m.valid || !m.address) continue;
    float arg = 0.0f;
    bool hasArg = false;
    if (m.arg.type == OscArg::Float32) { arg = m.arg.f; hasArg = true; }
    else if (m.arg.type == OscArg::Int32) { arg = (float)m.arg.i; hasArg = true; }
    s_live->handleOsc(m.address, arg, hasArg, now_sec());
  }
}

bool osc_input_start(const OscInputConfig* cfg) {
  if (!cfg || !cfg->live) return false;
  s_live = cfg->live;

  s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s_sock < 0) {
    ESP_LOGE(TAG, "socket(): errno %d", errno);
    return false;
  }
  struct sockaddr_in bind = {};
  bind.sin_family = AF_INET;
  bind.sin_port = htons(cfg->port);
  bind.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s_sock, (struct sockaddr*)&bind, sizeof(bind)) != 0) {
    ESP_LOGE(TAG, "bind(%u): errno %d", (unsigned)cfg->port, errno);
    close(s_sock);
    s_sock = -1;
    return false;
  }
  BaseType_t ok = xTaskCreate(osc_task, "osc", 3072, nullptr,
                              tskIDLE_PRIORITY + 2, &s_task);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "task create failed");
    close(s_sock);
    s_sock = -1;
    return false;
  }
  ESP_LOGI(TAG, "OSC listening on UDP %u", (unsigned)cfg->port);
  return true;
}

void osc_input_stop(void) {
  if (s_task) { vTaskDelete(s_task); s_task = nullptr; }
  if (s_sock >= 0) { close(s_sock); s_sock = -1; }
}

#endif  // ESP_PLATFORM
>>>>>>> theirs
