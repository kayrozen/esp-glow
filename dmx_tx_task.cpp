#ifdef ESP_PLATFORM

#include "dmx_tx_task.h"
#include "dmx_sink.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char* TAG = "dmx_tx";

static TaskHandle_t s_task = nullptr;
static DmxSink*     s_sink = nullptr;
static volatile bool s_stopRequested = false;
static volatile bool s_stopped = true;

static void dmx_tx_loop(void* /*ctx*/) {
  ESP_LOGI(TAG, "dmx_tx task started on core %d", xPortGetCoreID());
  s_stopped = false;

  while (!s_stopRequested) {
    // 30ms comfortably covers DMX's own ~22.7ms wire time for 512 slots
    // with margin (matches the old synchronous send()'s wait, dmx_sink.cpp)
    // -- the bound pumpTx() is allowed to block waiting for the PREVIOUS
    // transmission when it has a new frame to send.
    if (!s_sink->pumpTx(pdMS_TO_TICKS(30))) {
      // Nothing new to send: back off briefly instead of busy-spinning
      // core 0 re-checking the dirty flag. Short enough that a fresh frame
      // (render task writes at ~44 Hz, one every ~22.7ms) is picked up
      // promptly once it arrives.
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }

  // B4: deterministic final frame, blocking until it's actually on the
  // wire -- see DmxSink::sendBlackoutNow's doc.
  s_sink->sendBlackoutNow();
  ESP_LOGI(TAG, "dmx_tx task stopping (final blackout frame sent)");

  s_stopped = true;
  s_task = nullptr;
  vTaskDelete(nullptr);
}

bool dmx_tx_task_start(DmxSink* sink) {
  if (!sink || s_task != nullptr) return false;

  s_sink = sink;
  s_stopRequested = false;

  // Core 0 (never core 1 -- see this file's header comment), priority
  // tskIDLE_PRIORITY + 1: same tier as the other non-render background
  // tasks in this project (e.g. wifi_manager.cpp's reconnect_task).
  // xTaskCreatePinnedToCore's stack-depth argument is in BYTES on
  // ESP-IDF, not words -- pass the byte count directly (see the CI guard
  // in .github/workflows/qemu-boot.yml and main.cpp's own note on this).
  BaseType_t ok = xTaskCreatePinnedToCore(dmx_tx_loop, "dmx_tx", 4096,
                                          nullptr, tskIDLE_PRIORITY + 1,
                                          &s_task, 0);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed");
    s_task = nullptr;
    return false;
  }
  return true;
}

void dmx_tx_task_stop(void) {
  if (s_task == nullptr) return;
  s_stopRequested = true;
  while (!s_stopped) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

#endif  // ESP_PLATFORM
