// midi_output.cpp — MIDI OUT transport for LED feedback.
//
// See midi_output.h for design rationale and rate-limiting policy.

#include "midi_output.h"

#ifdef ESP_PLATFORM
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char* TAG = "midi_output";
#endif

namespace glow {

MidiOutput::MidiOutput() {}

bool MidiOutput::initDin(int uartNum, int txGpio) {
#ifdef ESP_PLATFORM
  uartNum_ = uartNum;
  txGpio_ = txGpio;
  
  uart_port_t port = static_cast<uart_port_t>(uartNum);
  
  uart_config_t cfg = {};
  cfg.baud_rate = 31250;  // standard MIDI baud rate
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity    = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;
  
  if (uart_param_config(port, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "uart_param_config(port=%d) failed", uartNum);
    return false;
  }
  
  // RX unused: this is MIDI OUT only.
  if (uart_set_pin(port, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                   txGpio, UART_PIN_NO_CHANGE) != ESP_OK) {
    ESP_LOGE(TAG, "uart_set_pin(port=%d, tx=%d) failed", uartNum, txGpio);
    return false;
  }
  
  // TX buffer only (no RX).
  constexpr int kTxBufSize = 256;
  if (uart_driver_install(port, 0, kTxBufSize, 0, nullptr, 0) != ESP_OK) {
    ESP_LOGE(TAG, "uart_driver_install(port=%d) failed", uartNum);
    return false;
  }
  
  ESP_LOGI(TAG, "MIDI OUT UART %d ready (tx=%d, 31250 baud)", uartNum, txGpio);
  return true;
#else
  (void)uartNum;
  (void)txGpio;
  return true;  // Host test: always succeed
#endif
}

bool MidiOutput::shouldSend(bool isNote, uint8_t channel, uint8_t id, uint8_t value) {
  // Linear search through tracked LEDs. For typical controllers (40-100 LEDs),
  // this is fast enough. If needed, could use a fixed-size array indexed by
  // (isNote << 7) | id for O(1) lookup.
  for (auto& state : ledStates_) {
    if (state.isNote == isNote && state.channel == channel && state.id == id) {
      if (state.lastValue == value) {
        messagesSuppressed_++;
        return false;  // No change
      }
      state.lastValue = value;
      return true;  // Changed
    }
  }
  // New LED: add to tracking
  ledStates_.push_back({value, isNote, id, channel});
  return true;
}

bool MidiOutput::canSendNow() {
#ifdef ESP_PLATFORM
  uint64_t nowUs = static_cast<uint64_t>(esp_timer_get_time());
  
  // Reset counter at start of new second
  if (nowUs - currentSecondStartUs_ >= 1000000ULL) {
    currentSecondStartUs_ = nowUs;
    sendsThisSecond_ = 0;
  }
  
  if (sendsThisSecond_ >= rateLimit_) {
    return false;
  }
  
  sendsThisSecond_++;
  lastSendTimeUs_ = nowUs;
#else
  // Host test: always allow
  (void)0;
#endif
  return true;
}

void MidiOutput::writeDin(const uint8_t* data, size_t len) {
#ifdef ESP_PLATFORM
  if (uartNum_ < 0) return;
  uart_port_t port = static_cast<uart_port_t>(uartNum_);
  uart_write_bytes(port, data, len);
#else
  (void)data;
  (void)len;
#endif
}

bool MidiOutput::sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
  if (!shouldSend(true, channel, note, velocity)) {
    return false;
  }
  
  if (!canSendNow()) {
    messagesSuppressed_++;
    return false;
  }
  
  uint8_t msg[3] = {
    static_cast<uint8_t>(0x90 | (channel & 0x0F)),
    note,
    velocity
  };
  
  writeDin(msg, 3);
  messagesSent_++;
  return true;
}

bool MidiOutput::sendCC(uint8_t channel, uint8_t cc, uint8_t value) {
  if (!shouldSend(false, channel, cc, value)) {
    return false;
  }
  
  if (!canSendNow()) {
    messagesSuppressed_++;
    return false;
  }
  
  uint8_t msg[3] = {
    static_cast<uint8_t>(0xB0 | (channel & 0x0F)),
    cc,
    value
  };
  
  writeDin(msg, 3);
  messagesSent_++;
  return true;
}

void MidiOutput::setRateLimit(int msgsPerSec) {
  rateLimit_ = msgsPerSec > 0 ? msgsPerSec : 100;
}

void MidiOutput::reset() {
  ledStates_.clear();
  messagesSent_ = 0;
  messagesSuppressed_ = 0;
  sendsThisSecond_ = 0;
  currentSecondStartUs_ = 0;
  lastSendTimeUs_ = 0;
}

}  // namespace glow
