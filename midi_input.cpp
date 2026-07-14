#ifdef ESP_PLATFORM

//
// MIDI input — device transport.
//
// Bytes go through MidiByteReader (midi_realtime.h, host-tested): it
// frames 3-byte channel messages AND recognizes MIDI Realtime bytes
// (Clock 0xF8 et al.) interleaved mid-message without disturbing that
// framing. Channel messages are parsed with parseMidi (live_control.h)
// into ControlEvents pushed onto the control queue; every 24th Clock
// pulse becomes a BeatEvent pushed onto the beat queue (no tempo
// estimation here -- that's BeatClock's PLL, fed via pumpBeatEvents on
// the render task). The render task drains both queues; this transport
// never touches LiveControl/ShowController/BeatClock directly,
// eliminating the cross-core data race. See control_queue.h/beat_queue.h
// for the rationale.
//
// DIN-MIDI over UART, standard 31250-8N1. Running-status is NOT handled --
// a real DIN-MIDI keyboard that leans on it (repeated Note On without
// resending the status byte) will drop messages here; adding that is a
// straightforward extension to MidiByteReader::feed (track the last status
// byte across calls) if a real controller needs it.
//
// A5: MIDI OUT (TX) shares this UART port on a second GPIO -- see
// midi_output_send3/DeviceMidiOutput below. THRU is still not implemented
// (RX bytes are never echoed to TX).
//

#include "midi_input.h"

#include "control_queue.h"   // IControlEventQueue, ControlEvent (transitively)
#include "live_control.h"    // ControlType, parseMidi
#include "midi_realtime.h"   // MidiByteReader (host-tested byte framing)
#include "beat_queue.h"      // glow::IBeatEventQueue, glow::BeatEvent

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdint>

static const char* TAG = "midi_input";

static IControlEventQueue* g_queue = nullptr;
static glow::IBeatEventQueue* g_beatQueue = nullptr;
static int g_uartNum = UART_NUM_2;
static int g_rxGpio = -1;
static int g_txGpio = -1;
static volatile bool g_uartReady = false;  // true once uart_driver_install succeeds
static MidiByteReader g_reader;

void midi_input_init(IControlEventQueue& queue, glow::IBeatEventQueue* beatQueue,
                     int uartNum, int rxGpio, int txGpio) {
  g_queue = &queue;
  g_beatQueue = beatQueue;
  g_uartNum = uartNum;
  g_rxGpio = rxGpio;
  g_txGpio = txGpio;
}

void midi_input_handle_byte(uint8_t byte) {
  if (g_queue == nullptr) return;

  uint8_t msg[3];
  MidiByteReader::Result r = g_reader.feed(byte, msg);
  switch (r) {
    case MidiByteReader::Result::ChannelMessage: {
      ControlEvent ev;
      if (parseMidi(msg, 3, ev)) g_queue->push(ev);
      break;
    }
    case MidiByteReader::Result::BeatPulse:
      // 24 PPQN summed into one beat boundary; bpm/beatInBar unknown --
      // BeatClock's PLL derives tempo from the interval between these.
      if (g_beatQueue) {
        uint64_t nowUs = static_cast<uint64_t>(esp_timer_get_time());
        g_beatQueue->push(glow::BeatEvent{nowUs, 0.0f, 0, false});
      }
      break;
    case MidiByteReader::Result::TransportStart:
    case MidiByteReader::Result::TransportStop:
    case MidiByteReader::Result::TransportContinue:
    case MidiByteReader::Result::None:
      break;
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
  // CTS unused; TX is UART_PIN_NO_CHANGE (disabled) unless A5's MIDI OUT
  // was configured via midi_input_init's txGpio.
  int txPin = (g_txGpio >= 0) ? g_txGpio : UART_PIN_NO_CHANGE;
  if (uart_set_pin(port, txPin, g_rxGpio,
                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
    ESP_LOGE(TAG, "uart_set_pin(port=%d, rx=%d, tx=%d) failed", g_uartNum, g_rxGpio, g_txGpio);
    vTaskDelete(nullptr);
    return;
  }
  // TX buffering: 0 = blocking writes on the caller's task (render task,
  // via LedFeedback -- see midi_output_send3). Fine at LED feedback's rate
  // (<=100 msg/sec, 3 bytes each, ~1ms at 31250 baud): never enough to
  // meaningfully stall the render loop's frame budget.
  constexpr int kRxBufSize = 256;
  if (uart_driver_install(port, kRxBufSize, 0, 0, nullptr, 0) != ESP_OK) {
    ESP_LOGE(TAG, "uart_driver_install(port=%d) failed", g_uartNum);
    vTaskDelete(nullptr);
    return;
  }
  g_uartReady = true;

  ESP_LOGI(TAG, "MIDI UART %d ready (rx=%d, tx=%d, 31250 baud)", g_uartNum, g_rxGpio, g_txGpio);

  uint8_t buf[32];
  while (true) {
    int n = uart_read_bytes(port, buf, sizeof(buf), pdMS_TO_TICKS(50));
    for (int i = 0; i < n; ++i) {
      midi_input_handle_byte(buf[i]);
    }
  }
}

void midi_output_send3(uint8_t status, uint8_t data1, uint8_t data2) {
  if (g_txGpio < 0 || !g_uartReady) return;
  uint8_t msg[3] = {status, data1, data2};
  uart_write_bytes(static_cast<uart_port_t>(g_uartNum), reinterpret_cast<const char*>(msg), sizeof(msg));
}

void DeviceMidiOutput::send3(uint8_t status, uint8_t data1, uint8_t data2) {
  midi_output_send3(status, data1, data2);
}

#endif
