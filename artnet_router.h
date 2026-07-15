// artnet_router.h — portable Art-Net packet building + per-universe routing.
//
// Everything ArtNetSink needs except the actual socket I/O lives here,
// fully host-testable (no ESP_PLATFORM dependency): building ArtDMX/ArtSync
// packet bytes, resolving a universe's (IP, wire-universe) destination, and
// driving per-universe sequence numbers. The device (artnet_sink.h) plugs
// in a real UDP socket via IArtNetTransport; host tests plug in a
// recording mock and assert on the exact bytes sent (see FORMAT.md's
// "Art-Net Wire Universe & Destination Routing" for the wire format this
// implements, and Wave 3's "Host tests" list for what's asserted).
#pragma once

#include "show.h"
#include <cstdint>
#include <cstddef>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#else
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#endif

constexpr uint16_t ARTNET_OP_DMX = 0x5000;
constexpr uint16_t ARTNET_OP_SYNC = 0x5200;
constexpr uint16_t ARTNET_PROTOCOL_VERSION = 14;
constexpr uint16_t ARTNET_PORT = 6454;

constexpr size_t ARTNET_DMX_HEADER_SIZE = 18;
constexpr size_t ARTNET_SYNC_PACKET_SIZE = 14;
constexpr size_t ARTNET_DMX_PACKET_MAX = ARTNET_DMX_HEADER_SIZE + DMX_UNIVERSE_SIZE;

// Anything that can put one UDP datagram on the wire to ip:port.
// ArtNetRouter is transport-agnostic specifically so it's host-testable:
// the device wraps a real socket (artnet_sink.h); a test wraps a recording
// mock, asserting on exactly what would have gone out.
class IArtNetTransport {
public:
  virtual ~IArtNetTransport() = default;
  virtual void sendTo(uint32_t ip, uint16_t port, const uint8_t* data, uint16_t len) = 0;
};

// Builds an ArtDMX packet (OpCode 0x5000) into `out` (must have room for at
// least ARTNET_DMX_PACKET_MAX bytes). `len` is the DMX payload length
// (clamped to DMX_UNIVERSE_SIZE, padded to even per the spec). Stamps
// `wireUniverse` (the Art-Net wire number -- NOT any internal universe
// index) as SubUni/Net, and `sequence` as-is (callers own incrementing it).
// Returns the packet length actually written.
uint16_t buildArtDmxPacket(uint8_t* out, uint16_t wireUniverse, uint8_t sequence,
                           const uint8_t* data, uint16_t len);

// Builds an ArtSync packet (OpCode 0x5200) into `out` (must have room for
// at least ARTNET_SYNC_PACKET_SIZE bytes). Returns the packet length.
uint16_t buildArtSyncPacket(uint8_t* out);

// Routes each universe index to an Art-Net destination, builds packets,
// and tracks one sequence counter per universe (Art-Net sequence numbers
// are per-destination-universe, not global -- getting this wrong makes
// nodes drop frames intermittently, indistinguishable from a flaky
// network; see send()).
class ArtNetRouter {
public:
  // fallbackIp: destination used when a universe's ArtNetDest.ip == 0 (no
  // explicit .show route, and nothing discovered yet -- see Wave 3 Phase
  // 3). 0 here means broadcast, same convention as CFG1's artnetFallbackIp.
  explicit ArtNetRouter(uint32_t fallbackIp, uint16_t port = ARTNET_PORT);

  // Route universeIndex to d. d.ip == 0 means "use the fallback" (see
  // ctor). Safe to call before any send() for that universe, and safe to
  // never call at all -- an unrouted universe defaults to
  // {ip=0, wireUniverse=universeIndex}, i.e. today's implicit
  // fallback/broadcast behavior, never a crash. universeIndex >=
  // MAX_UNIVERSES is a no-op.
  void setDest(uint8_t universeIndex, const ArtNetDest& d);
  ArtNetDest destFor(uint8_t universeIndex) const;

  // Builds and sends one ArtDMX packet for universeIndex via `transport`,
  // stamping the routed wire universe (not universeIndex) and incrementing
  // that universe's own sequence counter (cycles 1..255, never 0).
  // universeIndex >= MAX_UNIVERSES is a no-op.
  void send(uint8_t universeIndex, const uint8_t* data, uint16_t len, IArtNetTransport& transport);

  // Broadcasts one ArtSync so every node latches simultaneously. Call
  // exactly once per frame, after every send() for that frame -- this is
  // what turns "each node updates whenever its own packet arrives"
  // (invisible for independent matrices, visible tearing for one matrix
  // spanning several universes) into "every node updates in lockstep."
  void frameEnd(IArtNetTransport& transport);

private:
  uint32_t resolveIp(const ArtNetDest& d) const;

  uint32_t fallbackIp_;
  uint16_t port_;
  mutable portMUX_TYPE destMux_ = portMUX_INITIALIZER_UNLOCKED;
  ArtNetDest dest_[MAX_UNIVERSES];
  uint8_t seq_[MAX_UNIVERSES];
};
