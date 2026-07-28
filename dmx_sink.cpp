// dmx_sink.cpp — device-only DMX output via the esp_dmx driver.
//
// F1 filled the original TODO scaffold: install the driver once, and on
// send() write a full 512-byte universe and transmit, synchronously, from
// whatever task called send() (the render task). B2
// (SPEC-esp-glow-spiram-dmx) moved the actual transmission off that task:
// send() now only copies into a latest-wins DmxFrameBuffer (see
// dmx_frame_buffer.h) and returns in a few us; pumpTx() -- called in a loop
// by the dedicated dmx_tx task (dmx_tx_task.h, core 0) -- does the real
// driver work. DmxSink is a concrete IUniverseSink that Show::configureUniverse
// references by pointer (Show does not take ownership).
//
// Why this moved: 513 slots at 250 kbaud is ~22.7ms of wire time -- the same
// as the render loop's own ~22.7ms period at 44 Hz. The two cadences are in
// lockstep, not one a clean multiple of the other, so by the time send()
// was called again the previous transmission was routinely still in flight,
// and dmx_wait_sent() (necessarily called before touching the driver's
// buffer again) blocked the render task for most of a frame period --
// measured on hardware at ~18.7ms out of a 22.7ms budget (see the SPEC's
// `flush by universe` telemetry). That stalled everything downstream in the
// same frame: the other universes' Art-Net flush, and the next
// renderCompute(). Decoupling means DMX's own physical ceiling (~40-44 Hz
// for 512 slots -- it cannot go faster, full stop) no longer throttles the
// render loop's cadence; the render loop always produces a fresh frame on
// its own schedule, and dmx_tx transmits whatever the latest one was,
// dropping intermediate frames when it's the slower side (correct for DMX:
// only the most recent slot values ever matter).
//
// Hardware: the RS485 transceiver's DE (driver-enable) and /RE (receiver
// enable) lines are wired together and driven by the UART's RTS pin, which
// esp_dmx toggles automatically for the break/MAB + slot transmission. You
// only need to tell dmx_set_pin() which GPIO is RTS.
//
// Driver: the `someweisguy/esp_dmx` v3.1.0 managed component. API used:
//   dmx_driver_install(dmx_num, &config, intr_flags) -> bool
//   dmx_set_pin(dmx_num, tx, rx, rts) -> bool
//   dmx_write_offset(dmx_num, offset, data, size) -> size_t
//   dmx_send(dmx_num, size) -> size_t
//   dmx_wait_sent(dmx_num, wait_ticks) -> bool
//
// dmx_send() is asynchronous -- it starts the break/MAB + slot transmission
// (double-buffered by the driver: it allocates 2x the buffer you configure)
// and returns immediately. dmx_wait_sent() is the synchronous half, and
// esp_dmx's own documented pattern is dmx_write -> dmx_send -> (do other
// work) -> dmx_wait_sent, called in that order every frame -- wait_sent
// guards against overwriting a frame still in flight, not against starting
// the next one too soon. pumpTx() places the PREVIOUS frame's
// dmx_wait_sent() before writing/sending the new one, same ordering the old
// synchronous send() used -- only the task paying for that wait changed.
#ifdef ESP_PLATFORM

#include "dmx_sink.h"
#include "esp_log.h"

#include <cstring>

static const char* TAG = "dmx_sink";

DmxSink::DmxSink(dmx_port_t dmxPort, int tx, int rx, int rts)
    : port_(dmxPort), tx_(tx), rx_(rx), rts_(rts) {}

bool DmxSink::begin() {
  dmx_config_t cfg = DMX_CONFIG_DEFAULT;
  if (!dmx_driver_install(port_, &cfg, 0)) {
    ESP_LOGE(TAG, "dmx_driver_install(port=%d) failed", port_);
    return false;
  }
  if (!dmx_set_pin(port_, tx_, rx_, rts_)) {
    ESP_LOGE(TAG, "dmx_set_pin(port=%d, tx=%d, rx=%d, rts=%d) failed",
             port_, tx_, rx_, rts_);
    return false;
  }
  ESP_LOGI(TAG, "DMX port %d ready (tx=%d rx=%d rts=%d)", port_, tx_, rx_, rts_);
  ready_ = true;
  return true;
}

void DmxSink::send(uint8_t /*universeIndex*/, const uint8_t* data, uint16_t len) {
  if (!ready_) return;
  // Just a memcpy under a short spinlock (see DmxFrameBuffer::write) -- no
  // driver call, no wait. This is what makes send() cheap enough to call
  // every render frame without stalling core 1; see the file header.
  frame_.write(data, len);
}

bool DmxSink::pumpTx(TickType_t waitTicks) {
  if (!ready_) return false;

  uint8_t buf[DMX_UNIVERSE_SIZE];
  uint16_t len = 0;
  if (!frame_.takeIfDirty(buf, &len)) {
    return false;  // latest-wins: nothing new since the last pump
  }

  // Wait for the PREVIOUS transmission to finish before touching the
  // driver's buffer -- see the file header for why this goes right before
  // the write/send, not after. If this ever measures close to the full
  // ~23ms instead of near-zero, that means dmx_tx is being scheduled less
  // often than DMX's own physical wire time allows -- expected under heavy
  // core-0 network load, not a bug (this task never blocks the render
  // loop either way; see render_task.cpp).
  dmx_wait_sent(port_, waitTicks);

  // DMX start code (slot 0) is 0x00 for DMX data. Padding to the full 512
  // slots (when the render side wrote fewer bytes) is handled here, not by
  // the render task -- see dmx_sink.h's B2 note.
  uint8_t startCode = 0x00;
  dmx_write_offset(port_, 0, &startCode, 1);
  if (len > 0) {
    dmx_write_offset(port_, 1, buf, len);
  }
  if (len < DMX_UNIVERSE_SIZE) {
    static uint8_t zeros[DMX_UNIVERSE_SIZE] = {0};
    dmx_write_offset(port_, 1 + len, zeros, DMX_UNIVERSE_SIZE - len);
  }

  // Asynchronous: starts transmission and returns immediately. dmx_send()
  // returns the number of bytes sent; size=0 is clamped to 512. THIS
  // frame's dmx_wait_sent() happens at the top of the next pumpTx() call,
  // not here.
  size_t sent = dmx_send(port_, 0);
  if (sent == 0) {
    ESP_LOGW(TAG, "dmx_send(port=%d) failed", port_);
  }
  return true;
}

void DmxSink::sendBlackoutNow() {
  if (!ready_) return;

  // Block for any transmission already in flight, same as pumpTx(), before
  // writing/sending the final zero frame -- see dmx_tx_task.cpp's shutdown
  // path, the only caller.
  dmx_wait_sent(port_, pdMS_TO_TICKS(30));

  uint8_t startCode = 0x00;
  dmx_write_offset(port_, 0, &startCode, 1);
  static uint8_t zeros[DMX_UNIVERSE_SIZE] = {0};
  dmx_write_offset(port_, 1, zeros, DMX_UNIVERSE_SIZE);

  size_t sent = dmx_send(port_, 0);
  if (sent == 0) {
    ESP_LOGW(TAG, "dmx_send(port=%d) failed (blackout frame)", port_);
  }
  // Block until this deterministic frame has actually finished transmitting
  // -- the whole point of this function is that the zero frame is
  // guaranteed on the wire before the caller (dmx_tx_task's shutdown path)
  // considers the DMX line quiesced, not merely queued.
  dmx_wait_sent(port_, pdMS_TO_TICKS(30));
}

#endif  // ESP_PLATFORM
