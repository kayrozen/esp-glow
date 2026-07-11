// main.cpp — esp-glow firmware entry point (ESP32-S3).
//
// Phase F2: WiFi + Art-Net output (matrices). On top of F1's DMX path, we now:
//   - bring up WiFi (STA) so the Art-Net sink has a network
//   - add a second IUniverseSink: ArtNetSink -> bridge IP:6454
//   - patch a 16x8 RGB pixel matrix on universes 1+2 (Raw mode, Art-Net)
//   - drive the matrix from the render loop via the pre_render hook
//
// What to observe (F2):
//   - Serial: "got ip: ..." then "Art-Net -> ...:6454 (ready)".
//   - Your existing Art-Net bridge lights the matrix with a scrolling rainbow.
//   - Or: Wireshark on the LAN shows Art-Net OpDmx packets for universes 1 and
//     2 at ~44 Hz (sequence numbers incrementing), and DMX on universe 0 still
//     holds the F1 dimmer at 50%.
//   - Status LED: fast blink (render), double-pulse when WiFi is up.
//
// Config: set CONFIG_GLOW_WIFI_SSID / CONFIG_GLOW_WIFI_PASS and
// CONFIG_GLOW_ARTNET_BRIDGE_IP via menuconfig (Kconfig below) or edit defaults
// in sdkconfig. Bridge IP "0.0.0.0" => broadcast (255.255.255.255).

#include <cstdio>
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "sdkconfig.h"
#include "nvs_flash.h"

#include "led_status.h"
#include "render_task.h"
#include "wifi_manager.h"

#include "show.h"
#include "effects.h"
#include "fixture_profile.h"
#include "dmx_sink.h"
#include "artnet_sink.h"
#include "pixel_matrix.h"
#include "pixel_patterns.h"

#ifdef ESP_PLATFORM
#include "esp_dmx.h"
#endif

static const char* TAG = "esp-glow";

// --- Board / config defaults (override in menuconfig) ---
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

#ifndef CONFIG_GLOW_WIFI_SSID
#define GLOW_WIFI_SSID "esp-glow"
#endif
#ifndef CONFIG_GLOW_WIFI_PASS
#define GLOW_WIFI_PASS "esp-glow"
#endif

// Art-Net bridge IP as a packed u32, host-byte order, e.g. 192.168.1.50 =>
// (192<<24)|(168<<16)|(1<<8)|50. 0 => broadcast.
#ifndef CONFIG_GLOW_ARTNET_BRIDGE_IP
#define GLOW_ARTNET_BRIDGE_IP 0u
#endif

// Globals: must outlive the render task.
static Show            g_show;
static DmxSink*         g_dmx = nullptr;
static ArtNetSink*      g_artnet = nullptr;
static DimmerEffect*    g_fx = nullptr;

// Pixel matrix: 16x8 RGB, serpentine horizontal, on universes 1..2.
static PixelMatrix*     g_matrix = nullptr;
static RainbowScrollPattern* g_pattern = nullptr;

static const uint16_t MX_W = 16;
static const uint16_t MX_H = 8;

// Pre-render hook: render the pixel pattern into the canvas, then blit each
// matrix universe into the Show's Raw universes. Show::renderFrame flushes
// them via the ArtNetSink.
static void on_pre_render(void* /*ctx*/, float t, Show* show) {
  if (!g_matrix || !show) return;
  g_matrix->render(t);
  for (uint8_t i = 0; i < g_matrix->universeCount(); ++i) {
    uint8_t  uidx = g_matrix->universeIndex(i);
    const uint8_t* data = g_matrix->universeData(i);
    show->writeRawUniverse(uidx, data, DMX_UNIVERSE_SIZE);
  }
}

static FixtureProfile makeDimmerProfile() {
  FixtureProfile p{};
  p.footprint = 1;
  p.channelCount = 1;
  p.channels[0] = { Capability::Dimmer, 0, 0xFF, 0, 0 };
  return p;
}

extern "C" void app_main(void) {
  // --- Banner (F0) ---
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  uint32_t flash_size = 0;
  esp_flash_get_size(nullptr, &flash_size);
  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "  esp-glow firmware  (ESP32-S3)  F2");
  ESP_LOGI(TAG, "  %s %s", __DATE__, __TIME__);
  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "chip: rev %d, cores %d", chip.revision, chip.cores);
  ESP_LOGI(TAG, "flash: %lu KB", (unsigned long)(flash_size / 1024));
#ifdef CONFIG_SPIRAM
  ESP_LOGI(TAG, "psram:  enabled (octal=%d)", CONFIG_SPIRAM_MODE_OCT ? 1 : 0);
#endif
  ESP_LOGI(TAG, "DMX:    tx=%d rx=%d rts=%d", GLOW_DMX_TX_GPIO, GLOW_DMX_RX_GPIO, GLOW_DMX_RTS_GPIO);

  led_status_init(GLOW_STATUS_LED_GPIO);
  led_status_set(LED_BLINK_SLOW);

  // NVS (needed by WiFi).
  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }

  // --- F1: DMX sink ---
  g_dmx = new DmxSink(DMX_NUM_1, GLOW_DMX_TX_GPIO, GLOW_DMX_RX_GPIO, GLOW_DMX_RTS_GPIO);
  if (!g_dmx->begin()) {
    ESP_LOGE(TAG, "DMX bring-up failed; halting.");
    led_status_set(LED_ERROR);
    return;
  }

  // --- F2: WiFi (STA) ---
  WifiStaConfig wc = {};
  wc.ssid = GLOW_WIFI_SSID;
  wc.password = GLOW_WIFI_PASS;
  wc.ap_fallback = false;
  wifi_start_sta(&wc);

  // --- F2: Art-Net sink ---
  uint32_t bridge = GLOW_ARTNET_BRIDGE_IP;
  if (bridge == 0) bridge = 0xFFFFFFFFu;  // broadcast default
  g_artnet = new ArtNetSink(bridge, 6454);
  if (!g_artnet->begin()) {
    ESP_LOGE(TAG, "Art-Net socket failed; matrix output disabled.");
    // Non-fatal: DMX still works.
  }

  // --- F1: DMX patch (universe 0, dimmer at base 0, 50%) ---
  g_show.setUniverseCount(3);
  g_show.configureUniverse(0, UniverseMode::Fixture, g_dmx);
  FixtureProfile dimmer = makeDimmerProfile();
  uint16_t fixtureId = g_show.patch(dimmer, 0, 0);
  (void)fixtureId;
  static uint16_t ids[] = { 0 };
  g_fx = new DimmerEffect({ids, 1}, 0.5f);
  g_show.addEffect(g_fx);

  // --- F2: pixel matrix on universes 1+2 (Raw mode, Art-Net) ---
  MatrixMap mm = {};
  mm.width = MX_W;
  mm.height = MX_H;
  mm.serpentine = true;
  mm.vertical = false;
  mm.order = ColorOrder::RGB;
  mm.startUniverse = 1;
  mm.startChannel = 0;
  g_matrix = new PixelMatrix(mm);
  g_pattern = new RainbowScrollPattern(/*periodSec*/ 4.0f, /*cyclesAcross*/ 2.0f);
  g_matrix->setPattern(g_pattern);
  g_matrix->setMasterBrightness(0.8f);

  // One ArtNetSink instance serves both matrix universes (send() stamps the
  // universe index per packet).
  g_show.configureUniverse(1, UniverseMode::Raw, g_artnet);
  g_show.configureUniverse(2, UniverseMode::Raw, g_artnet);

  ESP_LOGI(TAG, "matrix %ux%u -> universes %u..%u (Art-Net)",
           MX_W, MX_H, mm.startUniverse,
           mm.startUniverse + g_matrix->universeCount() - 1);

  // --- Render task (core 1, 44 Hz, with pre_render filling the matrix) ---
  RenderTaskConfig rcfg = {};
  rcfg.show       = &g_show;
  rcfg.targetHz   = 44;
  rcfg.core       = 1;
  rcfg.stackBytes = 4096;
  rcfg.priority   = 20;
  rcfg.pre_render = on_pre_render;
  rcfg.pre_render_ctx = nullptr;
  if (!render_task_start(&rcfg)) {
    ESP_LOGE(TAG, "render task start failed; halting.");
    led_status_set(LED_ERROR);
    return;
  }
  led_status_set(LED_BLINK_FAST);

  // Reflect WiFi state on the LED (best-effort; loop in app_main is fine
  // because everything else runs in its own task).
  while (true) {
    if (wifi_is_connected()) {
      led_status_set(LED_BLINK_DOUBLE);
    } else {
      led_status_set(LED_BLINK_FAST);
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}
