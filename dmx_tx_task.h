// dmx_tx_task.h — B2 (SPEC-esp-glow-spiram-dmx): the dedicated DMX transmit
// task.
//
// Owns the real dmx_write_offset -> dmx_send -> dmx_wait_sent cycle that
// used to run synchronously inside DmxSink::send() on the render task (core
// 1). The render task now only ever writes into DmxSink's latest-wins frame
// buffer (dmx_frame_buffer.h) via send(); this task, pinned to core 0 like
// every other non-render task in this project, drains it at DMX's own
// physical cadence (~40-44 Hz for 512 slots -- it cannot go faster) via
// DmxSink::pumpTx(). Core 1 stays reserved for rendering only -- the
// project's own invariant, enforced by the CI guard on bare xTaskCreate().
#pragma once

#ifdef ESP_PLATFORM

#include <stdbool.h>
#include "dmx_sink.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts the dmx_tx task. `sink` must already have had begin() called
// successfully (the driver must be installed before this task's first
// pumpTx() call) -- see DmxSink::begin()'s doc. Borrowed pointer, must
// outlive the task. Returns false if the task could not be created; safe to
// call again after a false return (e.g. once more DRAM is free).
bool dmx_tx_task_start(DmxSink* sink);

// B3/B4: signals the task to stop looping. It finishes any transmission
// already in flight, sends one final deterministic all-zero (blackout)
// frame via DmxSink::sendBlackoutNow() -- so "the DMX line keeps carrying
// zero frames" (safe_blackout.h) holds even once nothing is left to drive
// it -- then deletes itself. Blocks until the task has actually stopped
// (bounded: at most two DMX wire times, ~46ms). Safe to call even if the
// task was never started.
void dmx_tx_task_stop(void);

#ifdef __cplusplus
}
#endif

#endif  // ESP_PLATFORM
