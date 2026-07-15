#include "wled_manager.h"
#include "wled_effect_map.h"

#include <cstdio>

#ifdef ESP_PLATFORM
#include "esp_log.h"
static const char* kWledTag = "wled_manager";
#endif

namespace {

void logWarn1(const char* fmt, const char* a) {
#ifdef ESP_PLATFORM
  ESP_LOGW(kWledTag, fmt, a);
#else
  std::fprintf(stderr, "wled_manager: ");
  std::fprintf(stderr, fmt, a);
  std::fprintf(stderr, "\n");
#endif
}

void logWarn2(const char* fmt, const char* a, const char* b) {
#ifdef ESP_PLATFORM
  ESP_LOGW(kWledTag, fmt, a, b);
#else
  std::fprintf(stderr, "wled_manager: ");
  std::fprintf(stderr, fmt, a, b);
  std::fprintf(stderr, "\n");
#endif
}

}  // namespace

void MockWledSink::send(const uint8_t* packet, size_t len, const std::string& ip, uint16_t port) {
  sendCount++;
  lastIp = ip;
  lastPort = port;
  size_t n = len < WLED_PACKET_SIZE ? len : WLED_PACKET_SIZE;
  for (size_t i = 0; i < n; ++i) last[i] = packet[i];
}

void WledManager::addTarget(const WledTarget& t) {
  for (auto& existing : targets_) {
    if (existing.name == t.name) {
      existing = t;
      return;
    }
  }
  targets_.push_back(t);
}

const WledTarget* WledManager::target(const std::string& name) const {
  for (const auto& t : targets_) {
    if (t.name == name) return &t;
  }
  return nullptr;
}

void WledManager::sendPacket(const WledPacketParams& params, const std::string& ip, uint16_t port) {
  if (!sink_) return;
  uint8_t packet[WLED_PACKET_SIZE];
  buildWledPacket(params, packet);
  sink_->send(packet, WLED_PACKET_SIZE, ip, port);
}

void WledManager::setEffect(const std::string& name, const std::string& effectName,
                            uint8_t speed, uint8_t intensity, uint8_t brightness,
                            const std::string& paletteName, uint16_t transitionMs) {
  const WledTarget* t = target(name);
  if (!t) {
    logWarn2("unknown target '%s' (glow.wled.fx '%s')", name.c_str(), effectName.c_str());
    return;
  }

  uint8_t fxId = 0;
  if (!wled::effectIdFromName(effectName, fxId) && effectName != "solid") {
    logWarn1("unknown effect '%s', falling back to solid", effectName.c_str());
  }
  uint8_t palId = 0;
  if (!wled::paletteIdFromName(paletteName, palId) && paletteName != "default") {
    logWarn1("unknown palette '%s', falling back to default", paletteName.c_str());
  }

  WledPacketParams p;
  p.effect = fxId;
  p.speed = speed;
  p.intensity = intensity;
  p.brightness = brightness;
  p.palette = palId;
  p.transitionMs = transitionMs;
  p.callMode = WledCallMode::EffectChanged;
  sendPacket(p, t->ip, t->port);
}

void WledManager::setSolidColor(const std::string& name, uint8_t r, uint8_t g, uint8_t b,
                                uint8_t brightness, uint16_t transitionMs) {
  const WledTarget* t = target(name);
  if (!t) {
    logWarn1("unknown target '%s' (glow.wled.color)", name.c_str());
    return;
  }

  WledPacketParams p;
  p.effect = 0;  // solid
  p.brightness = brightness;
  p.r = r;
  p.g = g;
  p.b = b;
  p.transitionMs = transitionMs;
  p.callMode = WledCallMode::DirectChange;
  sendPacket(p, t->ip, t->port);
}

void WledManager::setPower(const std::string& name, bool on) {
  const WledTarget* t = target(name);
  if (!t) {
    logWarn1("unknown target '%s' (glow.wled.on/off)", name.c_str());
    return;
  }

  WledPacketParams p;
  p.brightness = on ? 255 : 0;
  p.callMode = WledCallMode::DirectChange;
  sendPacket(p, t->ip, t->port);
}

void WledManager::broadcastEffect(const std::string& effectName, uint8_t speed, uint8_t intensity,
                                  uint8_t brightness, const std::string& paletteName) {
  uint8_t fxId = 0;
  if (!wled::effectIdFromName(effectName, fxId) && effectName != "solid") {
    logWarn1("unknown effect '%s', falling back to solid", effectName.c_str());
  }
  uint8_t palId = 0;
  if (!wled::paletteIdFromName(paletteName, palId) && paletteName != "default") {
    logWarn1("unknown palette '%s', falling back to default", paletteName.c_str());
  }

  WledPacketParams p;
  p.effect = fxId;
  p.speed = speed;
  p.intensity = intensity;
  p.brightness = brightness;
  p.palette = palId;
  p.callMode = WledCallMode::EffectChanged;
  sendPacket(p, "255.255.255.255", WLED_DEFAULT_PORT);
}
