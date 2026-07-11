// main.cpp — esp-glow firmware entry point (ESP32-S3).
//
// Phase F1: DMX output path. Stand up a hardcoded patch (one dimmer fixture on
// universe 0, base channel 0, 50% dimmer effect) and drive it through the
// render task into a real DmxSink. This proves the entire engine->DMX chain
// on hardware — the biggest single milestone.
//
// What to observe (F1):
//   - A real fixture patched at base 0 dims to ~50%.
//   - Or: a DMX tester / logic analyzer on the DMX line shows slot 1 == 128
//     (round(0.5*255)) at ~44 Hz, with correct break/MAB timing.
//   - Serial: "render loop started on core 1" + per-5s frame stats.
//   - Status LED: fast blink (render loop running).
//
// DMX pin defaults (override in menuconfig):
//   TX  = GPIO 17   (RS485 transceiver DI)
//   RX  = GPIO 18   (RS485 transceiver RO; unused for TX-only but required)
//   RTS = GPIO 8    (RS485 DE/RE — driver-enable + receiver-enable tied)

#include <cstdio>
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "sdkconfig.h"

#include "led_status.h"
#include "render_task.h"

#include "show.h"
#include "effects.h"
#include "fixture_profile.h"
#include "dmx_sink.h"   // device-side: pulls in the DmxSink class

#ifdef ESP_PLATFORM
#include "esp_dmx.h"
#include "driver/gpio.h"
#endif

static const char* TAG = "esp-glow";

#ifndef CONFIG_GLOW_STATUS_LED_GPIO
#define GLOW_STATUS_LED_GPIO 2
#endif
#ifndef CONFIG_GLOW_DMX_TX_GPIO
#define GLOW_DMX_TX_GPIO 17
#endif
#ifndef CONFIG_GLOW_DMX_RX_GPIO
#define GLOW_DMX_RX_GPIO 18
#endif
#ifndef CONFIG_GLOW_DMX_RTS_GPIO
#define GLOW_DMX_RTS_GPIO 8
#endif

// A minimal 1-channel dimmer profile, constructed by hand so we do not depend
// on profile_encoder.cpp (which is host-only per the plan). This is exactly
// the layout a PFX1 blob for "Dimmer at channel 0, 8-bit, default 0" would
// decode to.
static FixtureProfile makeDimmerProfile() {
  FixtureProfile p{};
  p.footprint = 1;
  p.channelCount = 1;
  p.channels[0] = { Capability::Dimmer, /*coarse*/ 0, /*fine*/ 0xFF,
                    /*default*/ 0, /*flags*/ 0 };
  return p;
}

// Globals: the Show, the DMX sink, and the effect. These must outlive the
// render task, so they live at static scope.
static Show         g_show;
static DmxSink*      g_dmx = nullptr;
static DimmerEffect* g_fx  = nullptr;

extern "C" void app_main(void) {
  // --- Banner (F0) ---
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  uint32_t flash_size = 0;
  esp_flash_get_size(nullptr, &flash_size);
  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "  esp-glow firmware  (ESP32-S3)  F1");
  ESP_LOGI(TAG, "  %s %s", __DATE__, __TIME__);
  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "chip: rev %d, cores %d", chip.revision, chip.cores);
  ESP_LOGI(TAG, "flash: %lu KB", (unsigned long)(flash_size / 1024));
#ifdef CONFIG_SPIRAM
  ESP_LOGI(TAG, "psram:  enabled (octal=%d)", CONFIG_SPIRAM_MODE_OCT ? 1 : 0);
#endif
  ESP_LOGI(TAG, "DMX:   tx=%d rx=%d rts=%d  (DMX_NUM_1)",
           GLOW_DMX_TX_GPIO, GLOW_DMX_RX_GPIO, GLOW_DMX_RTS_GPIO);

  led_status_init(GLOW_STATUS_LED_GPIO);
  led_status_set(LED_BLINK_SLOW);

  // --- F1: DMX sink ---
  g_dmx = new DmxSink(DMX_NUM_1, GLOW_DMX_TX_GPIO, GLOW_DMX_RX_GPIO, GLOW_DMX_RTS_GPIO);
  if (!g_dmx->begin()) {
    ESP_LOGE(TAG, "DMX bring-up failed; halting.");
    led_status_set(LED_ERROR);
    return;  // app_main returns; idle tasks run
  }

  // --- F1: hardcoded patch (universe 0, one dimmer fixture at base 0) ---
  g_show.setUniverseCount(1);
  g_show.configureUniverse(0, UniverseMode::Fixture, g_dmx);

  FixtureProfile dimmer = makeDimmerProfile();
  uint16_t fixtureId = g_show.patch(dimmer, /*universe*/ 0, /*base*/ 0);
  ESP_LOGI(TAG, "patched fixture id=%u on u0 base0 (1ch dimmer)", fixtureId);

  // 50% dimmer — a real fixture patched at base 0 will hold at ~half intensity.
  // (DimmerEffect holds ids by value; the vector is moved in.)
  static uint16_t ids[] = { 0 };  // fixtureId 0 (we patched first)
  g_fx = new DimmerEffect({ids, 1}, 0.5f);
  g_show.addEffect(g_fx);

  // --- F1: render task (pinned to core 1, ~44 Hz) ---
  RenderTaskConfig rcfg = {};
  rcfg.show       = &g_show;
  rcfg.targetHz   = 44;
  rcfg.core       = 1;
  rcfg.stackBytes = 4096;
  rcfg.priority   = 20;
  if (!render_task_start(&rcfg)) {
    ESP_LOGE(TAG, "render task start failed; halting.");
    led_status_set(LED_ERROR);
    return;
  }
  led_status_set(LED_BLINK_FAST);  // render loop alive

  ESP_LOGI(TAG, "F1 complete: render loop driving universe 0 to DMX at 44 Hz.");
  ESP_LOGI(TAG, "Patch a fixture at base 0; it should hold ~50%% dimmer.");
}
