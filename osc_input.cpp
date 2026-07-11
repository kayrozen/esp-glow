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
#include "osc_parser.h"     // parseOsc, OscAddressMap, OscAddressBinding

#include <cstdint>
#include <cstring>
#include <cstdio>

static IControlEventQueue* g_queue = nullptr;
static const OscAddressMap* g_map = nullptr;

void osc_input_init(IControlEventQueue& queue) {
  g_queue = &queue;
}

void osc_input_set_address_map(const OscAddressMap* map) {
  g_map = map;
}

void osc_input_handle_packet(const uint8_t* packet, size_t len) {
  if (g_queue == nullptr || g_map == nullptr) return;

  ControlEvent ev;
  if (parseOsc(packet, len, *g_map, ev)) {
    g_queue->push(ev);
  }
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
