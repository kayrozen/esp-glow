// artnet_sink.h — device-only Art-Net DMX output sink.
//
// Declares ArtNetSink, a concrete IUniverseSink that owns one unconnected
// UDP socket and routes each universe to its own Art-Net destination (IP +
// wire universe) via ArtNetRouter (artnet_router.h, fully host-tested) --
// see FORMAT.md's "Art-Net Wire Universe & Destination Routing". One
// instance serves every Art-Net universe: configureUniverse(1, Raw,
// &artnet) and (2, Raw, &artnet) both work, each free to go to a different
// node.
#pragma once

#include "show.h"
#include "artnet_router.h"

#ifdef ESP_PLATFORM
#include "lwip/ip4_addr.h"
#include <cstdint>

class ArtNetSink : public IUniverseSink, private IArtNetTransport {
public:
  // port: Art-Net UDP port, typically 6454 (ARTNET_PORT).
  // fallbackIp: destination for any universe whose ArtNetDest.ip is 0 (no
  //   explicit .show route, nothing discovered yet) -- CFG1's
  //   artnetFallbackIp. 0 here means broadcast (255.255.255.255), same
  //   convention as that field.
  ArtNetSink(uint16_t port, uint32_t fallbackIp);

  ~ArtNetSink() override;

  // Create the UDP socket. Call once. Returns false on failure. The socket
  // is unconnected -- every send is a sendto() to that packet's own
  // resolved destination, so one socket serves every node (no per-node
  // sockets to open/track).
  bool begin();

  // Route universeIndex to d (from the loaded bundle; later, from ArtPoll
  // discovery -- see IMPORTANT ordering note below). Safe to call before
  // begin(), and safe to never call for a given universe: it then defaults
  // to {ip=0, wireUniverse=universeIndex} (fallback/broadcast, today's
  // implicit behavior) -- never a crash.
  //
  // IMPORTANT: the discovery task may update destinations while the render
  // task is sending frames; ArtNetRouter synchronizes those updates with a
  // tiny critical section around its per-universe routing table.
  void setDest(uint8_t universeIndex, const ArtNetDest& d);

  void send(uint8_t universeIndex, const uint8_t* data, uint16_t len) override;

  // Broadcasts one ArtSync (OpCode 0x5200) so every node latches the
  // universes just sent simultaneously, instead of each one updating
  // whenever its own packet happens to arrive (the source of visible
  // tearing on a matrix spanning multiple universes). Call exactly once
  // per frame, from the render loop, after every Art-Net send() for that
  // frame has gone out.
  void frameEnd();

private:
  void sendTo(uint32_t ip, uint16_t port, const uint8_t* data, uint16_t len) override;

  ArtNetRouter router_;
  int sock_ = -1;
};

#endif  // ESP_PLATFORM
