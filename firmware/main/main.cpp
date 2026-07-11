// main.cpp — esp-glow firmware entry point (ESP32-S3).
//
// Phase F4: inputs. On top of F3's data-driven show, we add:
//   - a ShowController (registered as a Show effect) with demo cues
//   - a LiveControl binding layer (MIDI notes, OSC addresses, web buttons)
//   - midi_input (UART DIN-MIDI -> parseMidi -> LiveControl)
//   - osc_input  (UDP -> parseOsc -> LiveControl)
//   - web_input  (HTTP + WS server serving the Preact console -> LiveControl)
//   - a state broadcaster that pushes active-cue ids to the console every 500ms
//
// The parsers (parseMidi, parseOsc, web_input_handle_text_frame) are all
// host-tested; the device code here only owns transport wiring.
//
// What to observe (F4):
//   - A browser loads http://<ip>/  and the cue buttons render (config frame).
//     Tapping "Full" lights the patched fixtures; releasing dims them back.
//   - A MIDI pad sending Note On (ch0, note 60) triggers cue 1 ("Full");
//     Note Off releases it.
//   - An OSC app sending /esp-glow/full with a float arg triggers cue 1.
//   - Serial logs each input transport coming up.
//
// Cue/bindings config: F4 hardcodes a small demo set (blackout + full). A
// live.json loader that maps cues to effects and inputs is a natural future
// extension; the bundle itself doesn't encode bindings.

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
#include "show_control.h"
#include "live_control.h"
#include "midi_input.h"
#include "osc_input.h"
#include "web_input.h"

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
#ifndef CONFIG_GLOW_MIDI_UART
#define GLOW_MIDI_UART 1
#endif
#ifndef CONFIG_GLOW_MIDI_RX_GPIO
#define GLOW_MIDI_RX_GPIO 16
#endif
#ifndef CONFIG_GLOW_MIDI_TX_GPIO
#define GLOW_MIDI_TX_GPIO (-1)
#endif
#ifndef CONFIG_GLOW_OSC_PORT
#define GLOW_OSC_PORT 8000
#endif
#ifndef CONFIG_GLOW_WEB_PORT
#define GLOW_WEB_PORT 80
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
#define BUNDLE_BUF_CAP (64 * 1024)

// Globals: must outlive the render task.
static Show         g_show;
static DmxSink*      g_dmx = nullptr;
static ArtNetSink*   g_artnet = nullptr;

// Show controller + cues + live control (F4).
static ShowController*   g_controller = nullptr;
static LiveControl*      g_live = nullptr;
// Cue effect objects (must outlive the controller; static scope).
static DimmerEffect*     g_fx_black = nullptr;
static DimmerEffect*     g_fx_full  = nullptr;

struct MatrixDriver {
  std::vector<PixelMatrix*>    matrices;
  std::vector<IPixelPattern*> patterns;
  ~MatrixDriver() {
    for (auto* p : patterns) delete p;
    for (auto* m : matrices) delete m;
  }
};
static MatrixDriver g_md;

class DeviceSinkFactory : public ISinkFactory {
public:
  IUniverseSink* sinkFor(uint8_t /*u*/, UniverseTransport t) override {
    switch (t) {
      case UniverseTransport::Dmx:    return g_dmx;
      case UniverseTransport::ArtNet: return g_artnet;
      default: return nullptr;
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

// F4: build the ShowController, demo cues, and LiveControl bindings. Fixture 0
// is guaranteed to exist (the bundle patches fixtures starting at id 0; the
// fallback patches one at id 0).
static void setup_live_control() {
  g_controller = new ShowController();
  g_show.addEffect(g_controller);

  // Two cues driving fixture 0's dimmer. Cue 0 = blackout, cue 1 = full.
  static uint16_t ids[] = { 0 };
  g_fx_black = new DimmerEffect({ids, 1}, 0.0f);
  g_fx_full  = new DimmerEffect({ids, 1}, 1.0f);
  g_controller->addCue({g_fx_black}, /*fadeIn*/ 0.05f, /*fadeOut*/ 0.2f, /*prio*/ 10, /*hold*/ 0.0f);
  g_controller->addCue({g_fx_full},  0.05f, 0.2f, 10, 0.0f);

  g_live = new LiveControl(*g_controller);
  // Web buttons (rendered in the Preact console).
  g_live->bindWebButton(0, /*cue*/ 0, "Blackout");
  g_live->bindWebButton(1, /*cue*/ 1, "Full");
  // MIDI: note 48 (C2) = blackout, note 60 (C3) = full, on channel 0.
  g_live->bindMidiNote(0, 48, 0);
  g_live->bindMidiNote(0, 60, 1);
  // OSC: /esp-glow/blackout and /esp-glow/full
  g_live->bindOsc("/esp-glow/blackout", 0);
  g_live->bindOsc("/esp-glow/full", 1);
}

static void setup_hardcoded_fallback() {
  ESP_LOGW(TAG, "using hardcoded fallback patch (no bundle found).");
  g_show.setUniverseCount(3);
  g_show.configureUniverse(0, UniverseMode::Fixture, g_dmx);
  g_show.configureUniverse(1, UniverseMode::Raw, g_artnet);
  g_show.configureUniverse(2, UniverseMode::Raw, g_artnet);
  g_show.patch(makeDimmerProfile(), 0, 0);

  MatrixMap mm = {};
  mm.width = 16; mm.height = 8; mm.serpentine = true; mm.vertical = false;
  mm.order = ColorOrder::RGB; mm.startUniverse = 1; mm.startChannel = 0;
  PixelMatrix* m = new PixelMatrix(mm);
  IPixelPattern* p = new RainbowScrollPattern(4.0f, 2.0f);
  m->setPattern(p);
  m->setMasterBrightness(0.8f);
  g_md.matrices.push_back(m);
  g_md.patterns.push_back(p);
}

static bool setup_show_from_bundle() {
  uint8_t* buf = (uint8_t*)heap_caps_malloc(BUNDLE_BUF_CAP, MALLOC_CAP_SPIRAM);
  if (!buf) { ESP_LOGE(TAG, "no PSRAM for bundle buffer"); return false; }

  LoadedShow ls;
  if (!storage_load_show(BUNDLE_PATH, &ls, buf, BUNDLE_BUF_CAP)) { free(buf); return false; }
  free(buf);

  DeviceSinkFactory factory;
  ApplyResult r = applyLoadedShow(ls, g_show, factory);
  ESP_LOGI(TAG, "applied: %u universes, %u fixtures (%u heads), %u matrix universes",
           r.universesConfigured, r.fixturesPatched, r.headsPatched, r.matrixUniverses);

  for (const MatrixMap& mm : ls.matrices) {
    PixelMatrix* m = new PixelMatrix(mm);
    IPixelPattern* p = new RainbowScrollPattern(4.0f, 2.0f);
    m->setPattern(p);
    m->setMasterBrightness(0.8f);
    g_md.matrices.push_back(m);
    g_md.patterns.push_back(p);
  }
  return true;
}

// State broadcaster: pushes {"type":"state","active":[...]} every 500ms.
static void state_task(void*) {
  char buf[128];
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(500));
    if (!g_controller || !g_live) continue;
    int off = snprintf(buf, sizeof(buf), "{\"type\":\"state\",\"active\":[");
    bool first = true;
    for (uint16_t id = 0; id < 8; ++id) {
      if (g_controller->isActive(id)) {
        off += snprintf(buf + off, sizeof(buf) - off, "%s%u", first ? "" : ",", id);
        first = false;
      }
    }
    off += snprintf(buf + off, sizeof(buf) - off, "]}");
    web_input_broadcast_state(buf);
  }
}

extern "C" void app_main(void) {
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  uint32_t flash_size = 0;
  esp_flash_get_size(nullptr, &flash_size);
  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "  esp-glow firmware  (ESP32-S3)  F4");
  ESP_LOGI(TAG, "  %s %s", __DATE__, __TIME__);
  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "chip: rev %d, cores %d", chip.revision, chip.cores);
  ESP_LOGI(TAG, "flash: %lu KB", (unsigned long)(flash_size / 1024));
#ifdef CONFIG_SPIRAM
  ESP_LOGI(TAG, "psram:  enabled (octal=%d)", CONFIG_SPIRAM_MODE_OCT ? 1 : 0);
#endif

  led_status_init(GLOW_STATUS_LED_GPIO);
  led_status_set(LED_BLINK_SLOW);

  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }

  // F1: DMX
  g_dmx = new DmxSink(DMX_NUM_1, GLOW_DMX_TX_GPIO, GLOW_DMX_RX_GPIO, GLOW_DMX_RTS_GPIO);
  if (!g_dmx->begin()) { ESP_LOGE(TAG, "DMX failed"); led_status_set(LED_ERROR); return; }

  // F2: WiFi + Art-Net
  WifiStaConfig wc = {};
  wc.ssid = GLOW_WIFI_SSID;
  wc.password = GLOW_WIFI_PASS;
  wifi_start_sta(&wc);

  uint32_t bridge = GLOW_ARTNET_BRIDGE_IP;
  if (bridge == 0) bridge = 0xFFFFFFFFu;
  g_artnet = new ArtNetSink(bridge, 6454);
  g_artnet->begin();

  // F3: load show from storage (or fallback)
  bool from_bundle = setup_show_from_bundle();
  if (!from_bundle) setup_hardcoded_fallback();

  // F4: show controller + cues + live control bindings
  setup_live_control();

  // F4: input transports (each feeds LiveControl with the current show time)
  MidiInputConfig mc = {};
  mc.uartNum = GLOW_MIDI_UART;
  mc.rxGpio  = GLOW_MIDI_RX_GPIO;
  mc.txGpio  = GLOW_MIDI_TX_GPIO;
  mc.live    = g_live;
  midi_input_start(&mc);  // non-fatal if it fails (no MIDI hardware attached)

  OscInputConfig oc = {};
  oc.port = GLOW_OSC_PORT;
  oc.live = g_live;
  osc_input_start(&oc);

  // Web console needs WiFi up; start it after a brief delay so the station
  // has had a chance to associate. (It self-heals: clients retry.)
  WebInputConfig wc2 = {};
  wc2.live = g_live;
  wc2.port = GLOW_WEB_PORT;
  web_input_start(&wc2);

  // State broadcaster (pushes active-cue ids to the console)
  xTaskCreate(state_task, "state", 3072, nullptr, tskIDLE_PRIORITY + 1, nullptr);

  // Render task (core 1, 44 Hz)
  RenderTaskConfig rcfg = {};
  rcfg.show       = &g_show;
  rcfg.targetHz   = 44;
  rcfg.core       = 1;
  rcfg.stackBytes = 4096;
  rcfg.priority   = 20;
  rcfg.pre_render = on_pre_render;
  rcfg.pre_render_ctx = nullptr;
  if (!render_task_start(&rcfg)) { ESP_LOGE(TAG, "render task failed"); led_status_set(LED_ERROR); return; }
  led_status_set(LED_BLINK_FAST);

  ESP_LOGI(TAG, "F4 complete. Inputs:");
  ESP_LOGI(TAG, "  web:  http://<ip>:%u/   (ws at /ws)", GLOW_WEB_PORT);
  ESP_LOGI(TAG, "  osc:  UDP %u  (/esp-glow/full, /esp-glow/blackout)", GLOW_OSC_PORT);
  ESP_LOGI(TAG, "  midi: UART%d rx=%d  (note 60=full, 48=blackout, ch0)",
           GLOW_MIDI_UART, GLOW_MIDI_RX_GPIO);

  while (true) {
    led_status_set(wifi_is_connected() ? LED_BLINK_DOUBLE : LED_BLINK_FAST);
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}
