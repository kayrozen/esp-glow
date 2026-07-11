#ifdef ESP_PLATFORM

#include "live_control.h"
#include <cstdint>
#include <cstring>

static LiveControl* g_liveControl = nullptr;

void osc_input_init(LiveControl& live) {
  g_liveControl = &live;
}

void osc_input_handle_packet(const uint8_t* packet, size_t len) {
  if (g_liveControl == nullptr) return;

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
  // For now, extract controlId from address (e.g., "/cue/1" → id=1)
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

  float now = 0.0f;
  g_liveControl->handle(ev, now);
}

void osc_server_task() {
  // TODO: receive UDP OSC packets (hardware-specific)
  // for each packet received:
  //   osc_input_handle_packet(buffer, len);
}

#endif
