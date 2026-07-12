#ifdef ESP_PLATFORM

//
// MIDI input — device transport.
//
// Parses MIDI bytes into ControlEvents and pushes them to the
// control-event queue. The render task drains the queue via
// pumpControlEvents() and dispatches to LiveControl — the transports
// no longer touch LiveControl/ShowController directly, eliminating the
// cross-core data race. See control_queue.h for the rationale.
//
// DIN-MIDI over UART, standard 31250-8N1, RX only (no MIDI OUT/THRU on
// this device). Running-status is NOT handled -- a real DIN-MIDI keyboard
// that leans on it (repeated Note On without resending the status byte)
// will drop messages here; adding that is a straightforward extension to
// midi_input_handle_byte's framing (track the last status byte across
// calls) if a real controller needs it. Byte framing/UART wiring lives in
// this file; message parsing itself is parseMidi (live_control.h,
// host-tested).
//

#include "midi_input.h"

#include "control_queue.h"  // IControlEventQueue, ControlEvent (transitively)
#include "live_control.h"   // ControlType, parseMidi

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdint>

static const char* TAG = "midi_input";

static IControlEventQueue* g_queue = nullptr;
static int g_uartNum = UART_NUM_2;
static int g_rxGpio = -1;

void midi_input_init(IControlEventQueue& queue, int uartNum, int rxGpio) {
  g_queue = &queue;
  g_uartNum = uartNum;
  g_rxGpio = rxGpio;
}

void midi_input_handle_byte(uint8_t byte) {
  if (g_queue == nullptr) return;

  static uint8_t buffer[3];
  static size_t bufferIdx = 0;

  if (byte & 0x80) {
    bufferIdx = 0;
  }

  buffer[bufferIdx++] = byte;

  if (bufferIdx >= 3) {
    ControlEvent ev;
    if (parseMidi(buffer, 3, ev)) {
      g_queue->push(ev);
    }
    bufferIdx = 0;
  }
}

void midi_uart_task(void* /*ctx*/) {
  uart_port_t port = static_cast<uart_port_t>(g_uartNum);

  uart_config_t cfg = {};
  cfg.baud_rate = 31250;  // standard MIDI baud rate
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity    = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;

  if (uart_param_config(port, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "uart_param_config(port=%d) failed", g_uartNum);
    vTaskDelete(nullptr);
    return;
  }
  // TX/CTS unused: this device only ever receives MIDI (no OUT/THRU).
  if (uart_set_pin(port, UART_PIN_NO_CHANGE, g_rxGpio,
                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
    ESP_LOGE(TAG, "uart_set_pin(port=%d, rx=%d) failed", g_uartNum, g_rxGpio);
    vTaskDelete(nullptr);
    return;
  }
  constexpr int kRxBufSize = 256;
  if (uart_driver_install(port, kRxBufSize, 0, 0, nullptr, 0) != ESP_OK) {
    ESP_LOGE(TAG, "uart_driver_install(port=%d) failed", g_uartNum);
    vTaskDelete(nullptr);
    return;
  }

  ESP_LOGI(TAG, "MIDI UART %d ready (rx=%d, 31250 baud)", g_uartNum, g_rxGpio);

  uint8_t buf[32];
  while (true) {
    int n = uart_read_bytes(port, buf, sizeof(buf), pdMS_TO_TICKS(50));
    for (int i = 0; i < n; ++i) {
      midi_input_handle_byte(buf[i]);
    }
  }
}

#endif
