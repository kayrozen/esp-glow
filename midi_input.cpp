<<<<<<< ours
#ifdef ESP_PLATFORM

//
// MIDI input — device scaffold.
//
// Parses MIDI bytes into ControlEvents and pushes them to the
// control-event queue. The render task drains the queue via
// pumpControlEvents() and dispatches to LiveControl — the transports
// no longer touch LiveControl/ShowController directly, eliminating the
// cross-core data race. See control_queue.h for the rationale.
//
// If MIDI bytes arrive from a UART ISR (not a task), use a
// FreeRtosControlEventQueue and call pushFromISR() instead of push().
//

#include "control_queue.h"  // IControlEventQueue, ControlEvent (transitively)
#include "live_control.h"   // ControlType, parseMidi

#include <cstdint>

static IControlEventQueue* g_queue = nullptr;

void midi_input_init(IControlEventQueue& queue) {
  g_queue = &queue;
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

void midi_uart_task() {
  // TODO: read MIDI bytes from UART/USB (hardware-specific)
  // for each byte received:
  //   midi_input_handle_byte(byte);
  //
  // If wired as a UART RX ISR instead of a task, call
  // ((FreeRtosControlEventQueue*)g_queue)->pushFromISR(ev) directly
  // from the ISR, bypassing midi_input_handle_byte.
}

#endif
=======
// midi_input.cpp — device-only MIDI byte source -> MidiParser -> LiveControl.
#ifdef ESP_PLATFORM

#include "midi_input.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>

static const char* TAG = "midi_input";

static MidiParser s_parser;
static LiveControl* s_live = nullptr;
static int s_uart = -1;
static TaskHandle_t s_task = nullptr;

static float now_sec() { return (float)(esp_timer_get_time() / 1000000.0); }

void midi_input_feed_bytes(LiveControl* live, const uint8_t* data, size_t len) {
  if (!live || !data) return;
  MidiEvent e;
  for (size_t i = 0; i < len; ++i) {
    if (s_parser.feed(data[i], e)) {
      live->handleMidi(e, now_sec());
    }
  }
}

static void uart_reader_task(void*) {
  static uint8_t buf[128];
  while (true) {
    int n = uart_read_bytes(s_uart, buf, sizeof(buf), pdMS_TO_TICKS(20));
    if (n > 0 && s_live) {
      midi_input_feed_bytes(s_live, buf, (size_t)n);
    }
  }
}

bool midi_input_start(const MidiInputConfig* cfg) {
  if (!cfg || !cfg->live) return false;
  s_live = cfg->live;
  s_uart = cfg->uartNum;

  uart_config_t uc = {};
  uc.baud_rate = 31250;          // MIDI baud rate
  uc.data_bits = UART_DATA_8_BITS;
  uc.parity = UART_PARITY_DISABLE;
  uc.stop_bits = UART_STOP_BITS_1;
  uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uc.source_clk = UART_SCLK_DEFAULT;

  esp_err_t e = uart_driver_install(s_uart, 256, 0, 0, nullptr, 0);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "uart_driver_install(%d): %s", s_uart, esp_err_to_name(e));
    return false;
  }
  e = uart_param_config(s_uart, &uc);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "uart_param_config: %s", esp_err_to_name(e));
    return false;
  }
  e = uart_set_pin(s_uart, cfg->txGpio >= 0 ? cfg->txGpio : -1,
                   cfg->rxGpio, -1, -1);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "uart_set_pin(rx=%d): %s", cfg->rxGpio, esp_err_to_name(e));
    return false;
  }

  BaseType_t ok = xTaskCreate(uart_reader_task, "midi", 3072, nullptr,
                              tskIDLE_PRIORITY + 2, &s_task);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "task create failed");
    return false;
  }
  ESP_LOGI(TAG, "DIN-MIDI on UART%d rx=%d", s_uart, cfg->rxGpio);
  return true;
}

void midi_input_stop(void) {
  if (s_task) { vTaskDelete(s_task); s_task = nullptr; }
  if (s_uart >= 0) { uart_driver_delete(s_uart); s_uart = -1; }
}

#endif  // ESP_PLATFORM
>>>>>>> theirs
