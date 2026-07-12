#pragma once

#include <cstddef>
#include <cstdint>

#include "osc_parser.h"  // OscAddressMap

class IControlEventQueue;

//
// OSC input — device transport (see osc_input.cpp). Listens on a UDP
// socket, parses each packet with parseOsc (osc_parser.h, host-tested)
// against a caller-supplied address map, and pushes matches onto the
// control queue. The render task is the only consumer (pumpControlEvents);
// this transport never touches LiveControl/ShowController directly.
//

// Initialize the OSC input layer: the queue it pushes to, the address ->
// logical-control binding table (borrowed -- must outlive the server, same
// contract as OscBinding/OscAddressMap in osc_parser.h), and which UDP port
// to listen on. Must be called before osc_server_task starts.
void osc_input_init(IControlEventQueue& queue, const OscAddressMap& map, uint16_t udpPort);

// Parses one inbound OSC packet and pushes a ControlEvent if it matches a
// bound address. Exposed separately from the UDP task so it's testable/
// callable without a real socket.
void osc_input_handle_packet(const uint8_t* packet, size_t len);

// FreeRTOS task entry point: opens a UDP socket bound to the port passed
// to osc_input_init and loops receiving packets into
// osc_input_handle_packet. Never returns.
void osc_server_task(void* ctx);
