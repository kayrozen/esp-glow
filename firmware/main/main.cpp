// main.cpp — esp-glow firmware entry point (ESP32-S3).
//
// Phase F3: load the show from storage. At boot we mount LittleFS, read
// /littlefs/show.shw1, call the host-tested loadShow(), then applyLoadedShow()
// to patch every fixture and route each universe's sink (Dmx->DmxSink,
// ArtNet->ArtNetSink). The patch is now data-driven — swapping the bundle
// changes the show with no code change.
//
// If the bundle is missing or corrupt we fall back to the F1/F2 hardcoded
// patch (one dimmer + a 16x8 rainbow matrix) so the board still does
// something. F5 replaces this fallback with a safe blackout.
//
// What to observe (F3):
//   - Serial: "LittleFS mounted ...", "read /littlefs/show.shw1 (N bytes)",
//     "show loaded: U universes, F fixtures, M matrices", then
//     "applied: U configured, F patched, H heads, R matrix universes".
//   - Swapping the bundle file on LittleFS and rebooting changes the patch
//     with no code change / no reflash.
//   - DMX fixtures respond per the bundle; matrices light per the bundle's
//     MatrixMap entries.
//   - Status LED: double-pulse (WiFi up) + fast blink (render).

#include <cstdio>
#include <vector>
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"

#include "led_status.h"
#include "render_task.h"
#include "wifi_manager.h"
#include "storage_manager.h"

#include "show.h"
#include "effects.h"
#include "fixture_profile.h"
#include "dmx_sink.h"
#include "artnet_sink.h"
#include "pixel_matrix.h"
#include "pixel_patterns.h"
#include "show_bundle.h"
#include "apply_loaded_show.h"

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
#ifndef CONFIG_GLOW_ARTNET_BRIDGE_IP
#define GLOW_ARTNET_BRIDGE_IP 0u
#endif

#define BUNDLE_PATH "/littlefs/show.shw1"
#define BUNDLE_BUF_CAP (64 * 1024)  // 64 KB scratch in PSRAM

// Globals: must outlive the render task.
static Show         g_show;
static DmxSink*      g_dmx = nullptr;
static ArtNetSink*   g_artnet = nullptr;

// Matrix driver: owns the PixelMatrix + pattern objects built from the bundle
// (or the hardcoded fallback). The pre_render hook iterates this list.
struct MatrixDriver {
  std::vector<PixelMatrix*>     matrices;
  std::vector<IPixelPattern*>  patterns;  // owned
  ~MatrixDriver() {
    for (auto* p : patterns) delete p;
    for (auto* m : matrices) delete m;
  }
};
static MatrixDriver g_md;

// Device-side sink factory: routes Dmx -> DmxSink, ArtNet -> ArtNetSink,
// everything else -> nullptr (universe skipped).
class DeviceSinkFactory : public ISinkFactory {
public:
  IUniverseSink* sinkFor(uint8_t /*universeIdx*/, UniverseTransport t) override {
    switch (t) {
      case UniverseTransport::Dmx:    return g_dmx;
      case UniverseTransport::ArtNet: return g_artnet;
      default: return nullptr;  // Sacn/Unused not supported
    }
  }
};

static void on_pre_render(void* /*ctx*/, float t, Show* show) {
  if (!show) return;
  for (PixelMatrix* m : g_md.matrices) {
    m->render(t);
    for (uint8_t i = 0; i < m->universeCount(); ++i) {
      uint8_t uidx = m->universeIndex(i);
      if (uidx >= show->universeCount()) continue;
      show->writeRawUniverse(uidx, m->universeData(i), DMX_UNIVERSE_SIZE);
    }
  }
}

static FixtureProfile makeDimmerProfile() {
  FixtureProfile p{};
  p.footprint = 1;
  p.channelCount = 1;
  p.channels[0] = { Capability::Dimmer, 0, 0xFF, 0, 0 };
  return p;
}

// F1/F2 fallback: hardcoded dimmer + 16x8 rainbow matrix. Used when no bundle.
static void setup_hardcoded_fallback() {
  ESP_LOGW(TAG, "using hardcoded fallback patch (no bundle found).");
  g_show.setUniverseCount(3);
  g_show.configureUniverse(0, UniverseMode::Fixture, g_dmx);
  g_show.configureUniverse(1, UniverseMode::Raw, g_artnet);
  g_show.configureUniverse(2, UniverseMode::Raw, g_artnet);

  FixtureProfile dimmer = makeDimmerProfile();
  g_show.patch(dimmer, 0, 0);
  static uint16_t ids[] = { 0 };
  static DimmerEffect fx(std::vector<uint16_t>{ids[0]}, 0.5f);
  g_show.addEffect(&fx);

  MatrixMap mm = {};
  mm.width = 16; mm.height = 8; mm.serpentine = true; mm.vertical = false;
  mm.order = ColorOrder::RGB; mm.startUniverse = 1; mm.startChannel = 0;
  PixelMatrix* m = new PixelMatrix(mm);
  IPixelPattern* p = new RainbowScrollPattern(4.0f, 2.0f);
  m->setPattern(p);
  m->setMasterBrightness(0.8f);
  g_md.matrices.push_back(m);
  g_md.patterns.push_back(p);  // PixelMatrix does not own it; we do.
}

// F3: build the show from a loaded bundle via the host-tested applyLoadedShow.
static bool setup_show_from_bundle() {
  uint8_t* buf = (uint8_t*)heap_caps_malloc(BUNDLE_BUF_CAP, MALLOC_CAP_SPIRAM);
  if (!buf) {
    ESP_LOGE(TAG, "no PSRAM for bundle buffer");
    return false;
  }

  LoadedShow ls;
  if (!storage_load_show(BUNDLE_PATH, &ls, buf, BUNDLE_BUF_CAP)) {
    free(buf);
    return false;
  }
  free(buf);  // loadShow copied what it needed

  DeviceSinkFactory factory;
  ApplyResult r = applyLoadedShow(ls, g_show, factory);
  ESP_LOGI(TAG, "applied: %u universes configured, %u skipped, %u fixtures (%u heads), %u matrix universes",
           r.universesConfigured, r.universesSkipped, r.fixturesPatched,
           r.headsPatched, r.matrixUniverses);

  // Build PixelMatrix objects for each matrix entry. Each gets a rainbow
  // pattern by default; a richer config (F4) can map patterns per matrix.
  for (const MatrixMap& mm : ls.matrices) {
    PixelMatrix* m = new PixelMatrix(mm);
    IPixelPattern* p = new RainbowScrollPattern(4.0f, 2.0f);
    m->setPattern(p);
    m->setMasterBrightness(0.8f);
    g_md.matrices.push_back(m);
    g_md.patterns.push_back(p);
    ESP_LOGI(TAG, "matrix %ux%u -> universes %u..%u",
             mm.width, mm.height, mm.startUniverse,
             mm.startUniverse + m->universeCount() - 1);
  }
  return true;
}

extern "C" void app_main(void) {
  // --- Banner (F0) ---
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  uint32_t flash_size = 0;
  esp_flash_get_size(nullptr, &flash_size);
  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "  esp-glow firmware  (ESP32-S3)  F3");
  ESP_LOGI(TAG, "  %s %s", __DATE__, __TIME__);
  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "chip: rev %d, cores %d", chip.revision, chip.cores);
  ESP_LOGI(TAG, "flash: %lu KB", (unsigned long)(flash_size / 1024));
#ifdef CONFIG_SPIRAM
  ESP_LOGI(TAG, "psram:  enabled (octal=%d)", CONFIG_SPIRAM_MODE_OCT ? 1 : 0);
#endif

  led_status_init(CONFIG_GLOW_STATUS_LED_GPIO);
  led_status_set(LED_BLINK_SLOW);

  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }

  // --- F1: DMX sink ---
  g_dmx = new DmxSink(DMX_NUM_1, CONFIG_GLOW_DMX_TX_GPIO, CONFIG_GLOW_DMX_RX_GPIO, CONFIG_GLOW_DMX_RTS_GPIO);
  if (!g_dmx->begin()) {
    ESP_LOGE(TAG, "DMX bring-up failed; halting.");
    led_status_set(LED_ERROR);
    return;
  }

  // --- F2: WiFi (STA) ---
  WifiStaConfig wc = {};
  wc.ssid = CONFIG_GLOW_WIFI_SSID;
  wc.password = CONFIG_GLOW_WIFI_PASS;
  wc.ap_fallback = false;
  wifi_start_sta(&wc);

  // --- F2: Art-Net sink ---
  uint32_t bridge = CONFIG_GLOW_ARTNET_BRIDGE_IP;
  if (bridge == 0) bridge = 0xFFFFFFFFu;
  g_artnet = new ArtNetSink(bridge, 6454);
  if (!g_artnet->begin()) {
    ESP_LOGE(TAG, "Art-Net socket failed; matrix output disabled.");
  }

  // --- F3: load the show from LittleFS, else fall back ---
  bool from_bundle = setup_show_from_bundle();
  if (!from_bundle) {
    setup_hardcoded_fallback();
  }

  // --- Render task (core 1, 44 Hz, pre_render drives all matrices) ---
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

  ESP_LOGI(TAG, "F3 complete: show is %s.",
           from_bundle ? "loaded from /littlefs/show.shw1" : "hardcoded fallback");
  ESP_LOGI(TAG, "Swap the bundle file on LittleFS and reboot to change the patch.");

  while (true) {
    led_status_set(wifi_is_connected() ? LED_BLINK_DOUBLE : LED_BLINK_FAST);
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}
