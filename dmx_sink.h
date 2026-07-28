// dmx_sink.h — device-only DMX output sink.
//
// Declares DmxSink, a concrete IUniverseSink that wraps the esp_dmx driver.
// The implementation is in dmx_sink.cpp and is `#ifdef ESP_PLATFORM`-guarded;
// on a host build this header still parses (the class body is hidden) so
// device-facing main.cpp can be written against a stable interface.
//
// B2 (SPEC-esp-glow-spiram-dmx): send() (called from the render task, core 1)
// no longer talks to the esp_dmx driver at all -- it only copies into a
// latest-wins DmxFrameBuffer (dmx_frame_buffer.h) and returns, in a few us.
// The actual dmx_write_offset -> dmx_send -> dmx_wait_sent cycle moved to
// pumpTx(), called in a loop by the dedicated dmx_tx task (core 0, see
// dmx_tx_task.h) -- that's where the ~22.7ms DMX wire time now gets spent,
// off the render loop entirely.
#pragma once

#include "show.h"
#include "dmx_frame_buffer.h"

#ifdef ESP_PLATFORM
#include "esp_dmx.h"
#include "freertos/FreeRTOS.h"
#include <cstdint>

class DmxSink : public IUniverseSink {
public:
  // dmxPort: DMX_NUM_1 / DMX_NUM_2.
  // tx/rx/rts: GPIO numbers. rts drives the RS485 DE+RE pair (tied together).
  DmxSink(dmx_port_t dmxPort, int tx, int rx, int rts);

  // Install the driver. Call exactly once. Returns false on failure. The
  // dmx_tx task (dmx_tx_task_start) must not be started until this returns
  // true -- see its own header comment.
  bool begin();

  // Render-task side (core 1): copies `data`/`len` into the latest-wins
  // frame buffer and returns -- no driver call, no wait. `universeIndex` is
  // accepted only to satisfy IUniverseSink; this device has exactly one
  // physical DMX port, so every universe routed to the same DmxSink shares
  // it -- the last send() called in a given frame is what actually goes out
  // (see B3's "multiple DMX universes" note; there is only one wire).
  void send(uint8_t universeIndex, const uint8_t* data, uint16_t len) override;

  // dmx_tx-task side (core 0): if a newer frame has been written since the
  // last call, waits for the PREVIOUS transmission to finish (bounded by
  // waitTicks), writes the new frame (start code + padding included) to the
  // driver, and starts transmitting it. Returns true if a frame was
  // actually sent this call, false if there was nothing new to send (the
  // caller should back off briefly rather than busy-loop -- see
  // dmx_tx_task.cpp).
  bool pumpTx(TickType_t waitTicks);

  // F5/B4: forces one final all-zero frame out, blocking until it has
  // actually finished transmitting. Used by dmx_tx_task's shutdown path so
  // "the DMX line keeps carrying zero frames" (safe_blackout.h) holds even
  // once the task that normally drives the line has stopped looping. Safe
  // to call whether or not a transmission was already in flight.
  void sendBlackoutNow();

private:
  dmx_port_t port_;
  int tx_, rx_, rts_;
  bool ready_ = false;
  DmxFrameBuffer frame_;
};

#endif  // ESP_PLATFORM
