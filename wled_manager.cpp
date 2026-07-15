#include "wled_manager.h"
#include "wled_effect_map.h"

#include <cstdio>

#ifdef ESP_PLATFORM
#include "esp_log.h"
static const char* kWledTag = "wled_manager";
#endif

namespace {

// ESP-IDF's ESP_LOGx macros splice color codes and a tag/timestamp prefix
// onto the format string via adjacent string-literal concatenation (see
// esp_log.h's LOG_FORMAT), so the format argument must be a literal at each
// macro invocation site -- a helper taking `fmt` as a runtime parameter (as
// an earlier version of this file did) does not work: ESP-IDF's format
// checker only sees the fixed prefix/suffix literals, not the (invisible to
// it) runtime string, and flags every real argument as "too many arguments
// for format" under -Werror=format-extra-args. Each distinct message
// therefore gets its own tiny function with the literal baked in directly,
// same pattern as lua_effect.cpp's logDisabled.

void logUnknownTargetFx(const char* name, const char* effectName) {
#ifdef ESP_PLATFORM
  ESP_LOGW(kWledTag, "unknown target '%s' (glow.wled.fx '%s')", name, effectName);
#else
  std::fprintf(stderr, "wled_manager: unknown target '%s' (glow.wled.fx '%s')\n", name, effectName);
#endif
}

void logUnknownTargetColor(const char* name) {
#ifdef ESP_PLATFORM
  ESP_LOGW(kWledTag, "unknown target '%s' (glow.wled.color)", name);
#else
  std::fprintf(stderr, "wled_manager: unknown target '%s' (glow.wled.color)\n", name);
#endif
}

void logUnknownTargetPower(const char* name) {
#ifdef ESP_PLATFORM
  ESP_LOGW(kWledTag, "unknown target '%s' (glow.wled.on/off)", name);
#else
  std::fprintf(stderr, "wled_manager: unknown target '%s' (glow.wled.on/off)\n", name);
#endif
}

void logUnknownEffect(const char* effectName) {
#ifdef ESP_PLATFORM
  ESP_LOGW(kWledTag, "unknown effect '%s', falling back to solid", effectName);
#else
  std::fprintf(stderr, "wled_manager: unknown effect '%s', falling back to solid\n", effectName);
#endif
}

void logUnknownPalette(const char* paletteName) {
#ifdef ESP_PLATFORM
  ESP_LOGW(kWledTag, "unknown palette '%s', falling back to default", paletteName);
#else
  std::fprintf(stderr, "wled_manager: unknown palette '%s', falling back to default\n", paletteName);
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
    logUnknownTargetFx(name.c_str(), effectName.c_str());
    return;
  }

  uint8_t fxId = 0;
  if (!wled::effectIdFromName(effectName, fxId) && effectName != "solid") {
    logUnknownEffect(effectName.c_str());
  }
  uint8_t palId = 0;
  if (!wled::paletteIdFromName(paletteName, palId) && paletteName != "default") {
    logUnknownPalette(paletteName.c_str());
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
    logUnknownTargetColor(name.c_str());
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
    logUnknownTargetPower(name.c_str());
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
    logUnknownEffect(effectName.c_str());
  }
  uint8_t palId = 0;
  if (!wled::paletteIdFromName(paletteName, palId) && paletteName != "default") {
    logUnknownPalette(paletteName.c_str());
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
