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

// Snapshot of ArtNetSink::sendTo()'s cumulative outcome counters -- see
// txStats()'s doc. Plain old data, cheap to copy, safe to build from a
// non-render-task caller (the web console's /artnet_nodes handler).
struct ArtNetTxStats {
  uint32_t ok = 0;              // sendto() succeeded
  uint32_t dropWouldBlock = 0;  // EAGAIN/EWOULDBLOCK -- WiFi TX path was momentarily busy
  uint32_t dropNoMem = 0;       // ENOMEM -- lwIP/WiFi driver out of TX buffers (the bug this
                                 // PR adds observability for; see FORMAT.md)
  uint32_t dropOther = 0;       // any other errno
  int lastErrno = 0;            // errno of the most recent non-ok sendto(), 0 if none yet
};

class ArtNetSink : public IUniverseSink, private IArtNetTransport {
public:
  // port: Art-Net UDP port, typically 6454 (ARTNET_PORT).
  // fallbackIp: destination for any universe whose ArtNetDest.ip is 0 (no
  //   explicit .show route, nothing discovered yet) -- CFG1's
  //   artnetFallbackIp. 0 here means "no destination" (dropped, not
  //   broadcast) unless explicitly set to 0xFFFFFFFF -- see ArtNetRouter.
  // syncBroadcast: CFG1's artnetSyncBroadcast. false (default) sends
  //   ArtSync unicast to each distinct routed node, none if none are
  //   routed; true keeps the spec-literal unconditional broadcast. See
  //   ArtNetRouter::frameEnd()'s doc.
  ArtNetSink(uint16_t port, uint32_t fallbackIp, bool syncBroadcast = false);

  ~ArtNetSink() override;

  // Create the UDP socket. Call once. Returns false on failure. The socket
  // is unconnected -- every send is a sendto() to that packet's own
  // resolved destination, so one socket serves every node (no per-node
  // sockets to open/track) -- and non-blocking (O_NONBLOCK), so a busy
  // WiFi TX path drops the packet (EWOULDBLOCK) instead of stalling the
  // render loop; see artnet_sink.cpp's begin() for the measurement behind
  // that choice.
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

  // Cumulative TX outcome counters since begin() (never reset) -- see
  // ArtNetTxStats. Reads four uint32_ts and an int with no lock: called
  // from the httpd task while sendTo() writes them from the render task,
  // so a torn read across counters is possible in principle but each
  // individual counter read is a plain aligned word load, and this is a
  // diagnostics endpoint, not a control path -- an off-by-one-frame count
  // is never worth a mutex on sendTo()'s hot path (see this project's
  // "zero allocation / no new locks on the render path" constraint).
  ArtNetTxStats txStats() const { return stats_; }

private:
  void sendTo(uint32_t ip, uint16_t port, const uint8_t* data, uint16_t len) override;

  // Logs stats_ at most once per second, and only if something changed
  // since the last time it logged -- see artnet_sink.cpp's sendTo() for
  // why this replaced a per-packet ESP_LOGW (the log line itself was
  // amplifying the ENOMEM bug this stat exists to diagnose).
  void logStatsIfDue();

  ArtNetRouter router_;
  int sock_ = -1;
  ArtNetTxStats stats_;
  int64_t lastLogUs_ = 0;
  ArtNetTxStats lastLogged_;
};

#endif  // ESP_PLATFORM
