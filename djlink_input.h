#pragma once

#include <cstdint>

namespace glow {
class IBeatEventQueue;
}

//
// Pro DJ Link input — device transport (see djlink_input.cpp). Passively
// listens for Beat packets (UDP port 50001) and CDJ Status packets' tempo-
// master flag (UDP port 50002), parses both with djlink_parser.h (host-
// tested), gates acceptance through djlink_master_tracker.h ("prefer the
// tempo master; ignore the rest"), and pushes accepted beats onto the
// beat queue. The render task is the only consumer (pumpBeatEvents); this
// transport never touches BeatClock directly. See beat_queue.h for the
// rationale.
//
// This is passive listening ONLY -- see djlink_parser.h's header for the
// scope boundary (not a Virtual CDJ; no packets are ever sent).
//

// Initialize the DJ Link input layer: the beat queue it pushes accepted
// beats to. Must be called before djlink_beat_task/djlink_status_task start.
void djlink_input_init(glow::IBeatEventQueue& queue);

// Parses one inbound packet from the beat port (50001) and, if it's a
// well-formed Beat packet from the currently-accepted device (tempo
// master, or "no master known yet"), pushes a BeatEvent. `tUs` is the
// caller's monotonic arrival timestamp. Exposed separately from the UDP
// task so it's callable without a real socket.
void djlink_input_handle_beat_packet(const uint8_t* packet, int len, uint64_t tUs);

// Parses one inbound packet from the status port (50002) and, if it's a
// well-formed CDJ Status packet, updates the master-tracking table.
void djlink_input_handle_status_packet(const uint8_t* packet, int len);

// FreeRTOS task entry points -- one blocking recvfrom loop per port (two
// tasks is simpler than multiplexing one select() loop over two sockets).
// Never return.
void djlink_beat_task(void* ctx);
void djlink_status_task(void* ctx);
