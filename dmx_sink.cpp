// dmx_sink.cpp — device-only DMX output via the esp_dmx driver.
//
// F1 fills the former TODO scaffold: install the driver once, and on send()
// write a full 512-byte universe and transmit. DmxSink is a concrete
// IUniverseSink that Show::configureUniverse references by pointer (Show does
// not take ownership).
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
// the next one too soon. send() below places the PREVIOUS frame's
// dmx_wait_sent() at the top of THIS call, before this frame's write/send,
// so DMX's ~23ms wire time overlaps with the rest of the render loop
// (Art-Net flush + the next renderCompute()) instead of stalling it: by the
// time this runs again, a full ~22ms frame period later, the previous
// transmission has normally already finished, so the wait is near-instant.
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

void DmxSink::send(uint8_t universeIndex, const uint8_t* data, uint16_t len) {
  if (!ready_) return;
  if (len > DMX_UNIVERSE_SIZE) len = DMX_UNIVERSE_SIZE;

  // Wait for the PREVIOUS call's transmission to finish before touching the
  // driver's buffer -- see the file header for why this goes FIRST, not
  // after this frame's own dmx_send() (that placement is what used to make
  // every send() pay DMX's full ~23ms wire time synchronously). On the very
  // first call this returns immediately (nothing in flight yet). If this
  // ever measures close to the full ~23ms instead of near-zero, that means
  // the render loop isn't leaving DMX enough of the frame period to finish
  // transmitting -- the driver correctly throttling to DMX's physical
  // ~43Hz limit for 512 slots, not a bug -- see render_task.cpp's
  // per-universe flush timing.
  dmx_wait_sent(port_, pdMS_TO_TICKS(30));

  // DMX start code (slot 0) is 0x00 for DMX data.
  uint8_t startCode = 0x00;
  dmx_write_offset(port_, 0, &startCode, 1);
  if (len > 0) {
    dmx_write_offset(port_, 1, data, len);
  }
  // Pad the remainder of the 512 slots to 0 if the caller sent fewer bytes.
  if (len < DMX_UNIVERSE_SIZE) {
    static uint8_t zeros[DMX_UNIVERSE_SIZE] = {0};
    dmx_write_offset(port_, 1 + len, zeros, DMX_UNIVERSE_SIZE - len);
  }

  // Asynchronous: starts transmission and returns immediately. dmx_send()
  // returns the number of bytes sent; size=0 is clamped to 512. THIS
  // frame's dmx_wait_sent() happens at the top of the next send() call, not
  // here.
  size_t sent = dmx_send(port_, 0);
  if (sent == 0) {
    ESP_LOGW(TAG, "dmx_send(port=%d, u=%u) failed", port_, universeIndex);
  }
}

#endif  // ESP_PLATFORM
