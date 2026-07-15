// wled_manager.h — the runtime side of WLED UDP Notifier support: named
// targets (loaded from the SHW1 bundle's WLED table, see
// apply_loaded_show.cpp) addressed by glow.wled.* (glow_lua_api.cpp).
//
// Every setter builds exactly one 24-byte packet (wled_packet.h) and hands
// it to an injected IWledSink -- no queue, no background task, no heap
// allocation at send time, matching artnet_sink.cpp's fire-and-forget
// discipline. This keeps WledManager itself host-testable (like Show is
// testable via MockSink, show.h) without any ESP-IDF dependency; the real
// UDP transport lives in wled_udp_sink.h, #ifdef ESP_PLATFORM guarded.
#pragma once

#include "wled_target.h"
#include "wled_packet.h"

#include <cstdint>
#include <string>
#include <vector>

// Injected UDP transport. send() must not block the caller for long (see
// wled_udp_sink.h's SO_SNDTIMEO) and never allocates.
class IWledSink {
public:
  virtual ~IWledSink() = default;
  virtual void send(const uint8_t* packet, size_t len, const std::string& ip, uint16_t port) = 0;
};

// Test double: records the last packet sent (and a running count), like
// show.h's MockSink.
class MockWledSink : public IWledSink {
public:
  void send(const uint8_t* packet, size_t len, const std::string& ip, uint16_t port) override;

  int sendCount = 0;
  uint8_t last[WLED_PACKET_SIZE] = {0};
  std::string lastIp;
  uint16_t lastPort = 0;
};

class WledManager {
public:
  // sink is borrowed and must outlive this manager; nullptr disables actual
  // sends (every setter still resolves the target and builds the packet, it
  // just has nowhere to send it -- host tests that only care about target
  // bookkeeping can pass nullptr).
  explicit WledManager(IWledSink* sink) : sink_(sink) {}

  // Adds or replaces (by name) a target. Called once per WLED .show
  // directive at load time (apply_loaded_show.cpp).
  void addTarget(const WledTarget& target);

  // nullptr if `name` is unknown. Exposed so glow_lua_api.cpp can validate
  // a target name up front and lua_error with a clear message, the same
  // "no matrix at index" contract as glow.matrix.* (glow_lua_api.h).
  const WledTarget* target(const std::string& name) const;
  size_t targetCount() const { return targets_.size(); }

  // Set effect with full parameters. speed/intensity/brightness are DMX-
  // style bytes (0..255); paletteName defaults to WLED's "default" (0),
  // which lets the effect's own default palette apply. Unknown
  // effectName/paletteName fall back to solid(0)/default(0) with a logged
  // warning (see wled_effect_map.h's effectIdFromName/paletteIdFromName).
  void setEffect(const std::string& name, const std::string& effectName,
                 uint8_t speed = 128, uint8_t intensity = 128, uint8_t brightness = 255,
                 const std::string& paletteName = "default", uint16_t transitionMs = 0);

  // Solid color override: effect forced to 0 (solid), sent as a direct
  // color change (callMode 0x01) rather than an effect change.
  void setSolidColor(const std::string& name, uint8_t r, uint8_t g, uint8_t b,
                     uint8_t brightness = 255, uint16_t transitionMs = 0);

  // Brightness 0 (off) / 255 (on), sent as a direct change. Does not touch
  // color or effect state on the receiver.
  void setPower(const std::string& name, bool on);

  // Same as setEffect but broadcasts to 255.255.255.255:21324 instead of a
  // named target -- syncs every WLED device on the LAN (sync-group
  // filtering, if any, happens on the receivers).
  void broadcastEffect(const std::string& effectName, uint8_t speed = 128,
                       uint8_t intensity = 128, uint8_t brightness = 255,
                       const std::string& paletteName = "default");

private:
  void sendPacket(const WledPacketParams& params, const std::string& ip, uint16_t port);

  IWledSink* sink_;
  std::vector<WledTarget> targets_;
};
