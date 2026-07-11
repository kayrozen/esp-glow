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
// Driver: the `espressif/esp_dmx` managed component (see
// firmware/main/idf_component.yml). API used:
//   dmx_driver_install(dmx_num, &config, intr_flags)
//   dmx_set_pin(dmx_num, tx, rx, rts)
//   dmx_write(dmx_num, start, data, size)
//   dmx_send(dmx_num)
//   dmx_wait_send(dmx_num, timeout)
//
// If your esp_dmx version's API differs slightly, adjust the calls in send()
// and begin() — they are the only driver touchpoints.
#ifdef ESP_PLATFORM

#include "dmx_sink.h"
#include "esp_log.h"

#include <cstring>

static const char* TAG = "dmx_sink";

DmxSink::DmxSink(dmx_port_t dmxPort, int tx, int rx, int rts)
    : port_(dmxPort), tx_(tx), rx_(rx), rts_(rts) {}

bool DmxSink::begin() {
  dmx_config_t cfg = {};
  cfg.baud_rate = DMX_BAUD_RATE;     // 250000
  cfg.break_num = DMX_BREAK_NUM;     // driver default (~176 us break)
  cfg.idle_num  = DMX_IDLE_NUM;      // driver default (~12 us MAB)
  cfg.source_clk = DMX_SCLK_CLK_SEL; // default source clock
  esp_err_t e = dmx_driver_install(port_, &cfg, 0);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "dmx_driver_install(port=%d): %s", port_, esp_err_to_name(e));
    return false;
  }
  e = dmx_set_pin(port_, tx_, rx_, rts_);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "dmx_set_pin(port=%d, tx=%d, rx=%d, rts=%d): %s",
             port_, tx_, rx_, rts_, esp_err_to_name(e));
    return false;
  }
  ESP_LOGI(TAG, "DMX port %d ready (tx=%d rx=%d rts=%d)", port_, tx_, rx_, rts_);
  ready_ = true;
  return true;
}

void DmxSink::send(uint8_t universeIndex, const uint8_t* data, uint16_t len) {
  if (!ready_) return;
  if (len > DMX_UNIVERSE_SIZE) len = DMX_UNIVERSE_SIZE;

  // DMX start code (slot 0) is 0x00 for DMX data. esp_dmx writes slot 0 from
  // the buffer; we force it to 0x00 so a stale byte never leaks onto the wire.
  uint8_t startCode = 0x00;
  dmx_write(port_, 0, &startCode, 1);
  if (len > 0) {
    dmx_write(port_, 1, data, len);
  }
  // Pad the remainder of the 512 slots to 0 if the caller sent fewer bytes.
  // (Show always sends DMX_UNIVERSE_SIZE, so this is a safety net.)
  if (len < DMX_UNIVERSE_SIZE) {
    static uint8_t zeros[DMX_UNIVERSE_SIZE] = {0};
    dmx_write(port_, 1 + len, zeros, DMX_UNIVERSE_SIZE - len);
  }

  esp_err_t e = dmx_send(port_, 0);
  if (e != ESP_OK) {
    ESP_LOGW(TAG, "dmx_send(port=%d, u=%u): %s", port_, universeIndex, esp_err_to_name(e));
    return;
  }
  // Block until the frame is on the wire so the next call doesn't queue.
  // DMX at 250 kbaud takes ~23 ms for 512 slots; allow 30 ms.
  dmx_wait_send(port_, pdMS_TO_TICKS(30));
}

#endif  // ESP_PLATFORM
