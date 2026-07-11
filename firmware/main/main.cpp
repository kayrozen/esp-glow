// main.cpp — esp-glow firmware entry point (ESP32-S3).
//
// Phase F0: toolchain de-risk. Print a banner over serial and blink the status
// LED. Every later phase extends app_main() — DMX render task (F1), WiFi +
// Art-Net (F2), show-from-storage (F3), inputs (F4), OTA + robustness (F5).
//
// The host-tested core (Show, effects, geometry) lives in the `glow_core`
// component; this file is the only place that touches ESP-IDF drivers.
//
// What to observe (F0):
//   idf.py flash monitor  ->  the banner prints, the LED blinks slowly.
//   If you see that, the toolchain, PSRAM, partition table and C++ flags are
//   all good, and every later phase has a foundation to build on.

#include <cstdio>
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "sdkconfig.h"

#include "led_status.h"

static const char* TAG = "esp-glow";

// Board pin for the status LED. Override via sdkconfig if your board differs.
#ifndef CONFIG_GLOW_STATUS_LED_GPIO
#define GLOW_STATUS_LED_GPIO 2
#else
#define GLOW_STATUS_LED_GPIO CONFIG_GLOW_STATUS_LED_GPIO
#endif

extern "C" void app_main(void) {
  // --- Banner: proves serial + chip introspection are alive. ---
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  uint32_t flash_size = 0;
  esp_flash_get_size(nullptr, &flash_size);

  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "  esp-glow firmware  (ESP32-S3)");
  ESP_LOGI(TAG, "  %s %s", __DATE__, __TIME__);
  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "chip: rev %d, cores %d, %s%s",
           chip.revision, chip.cores,
           (chip.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi " : "",
           (chip.features & CHIP_FEATURE_BLE) ? "BLE " : "");
  ESP_LOGI(TAG, "flash: %lu KB", (unsigned long)(flash_size / 1024));
#ifdef CONFIG_SPIRAM
  ESP_LOGI(TAG, "psram:  enabled (octal=%d)", CONFIG_SPIRAM_MODE_OCT ? 1 : 0);
#else
  ESP_LOGI(TAG, "psram:  disabled");
#endif
  ESP_LOGI(TAG, "status LED on GPIO %d", GLOW_STATUS_LED_GPIO);
  ESP_LOGI(TAG, "");

  // --- LED blinker: the one piece of feedback that needs no peripherals. ---
  led_status_init(GLOW_STATUS_LED_GPIO);
  led_status_set(LED_BLINK_SLOW);

  ESP_LOGI(TAG, "F0 bring-up complete. Waiting (later phases add DMX/WiFi/show).");

  // app_main returns; FreeRTOS idle tasks take over. Later phases spawn their
  // own tasks here before returning.
}
