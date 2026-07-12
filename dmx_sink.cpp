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

  // dmx_send() returns the number of bytes sent. size=0 is clamped to 512.
  size_t sent = dmx_send(port_, 0);
  if (sent == 0) {
    ESP_LOGW(TAG, "dmx_send(port=%d, u=%u) failed", port_, universeIndex);
    return;
  }
  // Block until the frame is on the wire so the next call doesn't queue.
  // DMX at 250 kbaud takes ~23 ms for 512 slots; allow 30 ms.
  dmx_wait_sent(port_, pdMS_TO_TICKS(30));
}

#endif  // ESP_PLATFORM
