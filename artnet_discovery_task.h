// artnet_discovery_task.h — Wave 3 Phase 3: device-only glue binding the
// portable ArtPoll/ArtPollReply logic (artnet_discovery.h) to a real UDP
// socket and the render loop's ArtNetSink.
//
// Everything with actual protocol logic (parsing, the discovery table, the
// "explicit .show route always wins" precedence) lives in artnet_discovery.h
// and is host-tested there. This file is just: open a socket, broadcast
// ArtPoll periodically, feed replies into that logic, and re-apply the
// result onto ArtNetSink::setDest() -- the same table Phase 1 built.
#pragma once

#include "artnet_discovery.h"
#include "artnet_sink.h"
#include <cstddef>
#include <cstdint>

#ifdef ESP_PLATFORM

// Must be called once, after the bundle has loaded (setup_show_from_bundle
// has already called configureArtnetDest for every ArtNet universe) and
// before artnet_discovery_task starts -- this is what lets discovery know
// which universes the .show already routed explicitly (never touched here)
// versus which it left for discovery to fill in.
void artnet_discovery_task_init(ArtNetSink* artnetSink, const ArtNetDest showDest[MAX_UNIVERSES],
                                 uint8_t universeCount);

// FreeRTOS task entry point. Opens its own UDP socket bound to
// ARTNET_PORT, broadcasts ArtPoll every few seconds, parses every
// ArtPollReply it receives, and after every cycle re-applies
// resolveDiscoveredDests() onto the ArtNetSink for every universe the
// .show left unspecified. Never returns.
void artnet_discovery_task(void* ctx);

// Web console support: a thread-safe snapshot of the currently-known
// nodes, refreshed once per poll cycle. Safe to call from the httpd task.
size_t artnet_discovery_node_count();
bool artnet_discovery_node_at(size_t i, DiscoveredNode& out);

#endif  // ESP_PLATFORM
