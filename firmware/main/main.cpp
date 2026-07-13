// main.cpp — esp-glow firmware entry point (ESP32-S3).
//
// Phase F3: load the show from storage. At boot we read the raw "show"
// partition (see partitions.csv), call the host-tested loadShow(), then
// applyLoadedShow() to patch every fixture and route each universe's sink
// (Dmx->DmxSink, ArtNet->ArtNetSink). The patch is now data-driven —
// reflashing the show partition changes the show with no code change.
//
// The show partition is raw (no filesystem): the browser-based web flasher
// (esptool-js) writes the compiled SHW1 bundle directly at the partition's
// flash offset, and the device just reads it back with esp_partition_read().
//
// F5: if the bundle is missing or corrupt, the board no longer falls back
// to a hardcoded demo patch -- it calls glow_safe_blackout() instead (see
// safe_blackout.h). A rig with no valid show should go dark and say why,
// not surprise the room with an unrequested rainbow. The same call is used
// for every other F5 failure path: a scripts partition that won't mount, a
// boot.fnl that errors, and an OTA image that fails self-validation (see
// ota_manager.h).
//
// What to observe (F3):
//   - Serial: "show loaded from partition 'show': U universes, F fixtures,
//     M matrices", then "applied: U configured, F patched, H heads, R
//     matrix universes".
//   - Reflashing the show partition (idf.py or the web flasher) and
//     rebooting changes the patch with no code change.
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
#include "safe_blackout.h"
#include "ota_manager.h"
#include "esp_task_wdt.h"

#include "show.h"
#include "show_control.h"
#include "live_control.h"
#include "control_queue.h"
#include "dmx_sink.h"
#include "artnet_sink.h"
#include "pixel_matrix.h"
#include "pixel_patterns.h"
#include "show_bundle.h"
#include "apply_loaded_show.h"

#include "glow_fennel.h"
#include "glow_lua_api.h"
#include "lua_vm.h"  // complete LuaVM type -- glow_fennel.h only forward-declares it (glowLuaVM().gcStepSlack(...) needs the full definition)
#include "scripts_storage.h"
#include "web_input.h"
#include "web_protocol.h"  // buildEvalResultJson (send_eval_result_to_ws)
#include "midi_input.h"
#include "osc_input.h"
#include "freertos/task.h"  // xTaskCreatePinnedToCore (midi_uart_task/osc_server_task)

// The vendored Fennel compiler, embedded via EMBED_FILES (see
// firmware/main/CMakeLists.txt). ESP-IDF's convention: a file's basename
// with non-alphanumeric characters replaced by '_', wrapped in
// _binary_..._start/_end.
extern "C" const uint8_t fennel_lua_start[] asm("_binary_fennel_lua_start");
extern "C" const uint8_t fennel_lua_end[] asm("_binary_fennel_lua_end");

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

#define BUNDLE_BUF_CAP (64 * 1024)  // 64 KB scratch in PSRAM

// Globals: must outlive the render task.
static Show         g_show;
static DmxSink*      g_dmx = nullptr;
static ArtNetSink*   g_artnet = nullptr;

// F6: ShowController is itself an IEffect (show_control.h) -- it does not
// sit "beside" Show, it plugs INTO it as an effect, the same way any other
// IEffect does. It is the ONE effect g_show ever gets (see app_main's
// g_show.addEffect(&g_controller)): everything downstream of it -- cues,
// scenes, fades, HTP/LTP -- funnels through this one instance, and it in
// turn is what emits the resolved intents Show::renderFrame consumes. Any
// effect added to g_show directly instead would run permanently, outside
// every cue, invisible and unstoppable from Lua/the web console/MIDI/OSC.
//
// Three things converge on this one instance: glow.cue.*/glow.scene.*
// (glow_lua_api.cpp, via glowLuaInit below), the web console, and MIDI/OSC
// (via g_liveControl below) -- all of F4/F6's trigger sources share the
// same ShowController, not separate ones.
static ShowController g_controller;

// F4 (pulled forward, see glow_core's CMakeLists.txt comment): the web
// console and MIDI/OSC bind cue/scene triggers here; it mutates
// g_controller only from the render task, via pumpControlEvents below --
// never directly from a transport task (see control_queue.h's rationale).
static LiveControl g_liveControl(g_controller);

// F4 (pulled forward): the control-event queue itself. Real transports
// (web httpd, MIDI UART, OSC UDP -- web_input.cpp, midi_input.cpp,
// osc_input.cpp) push into this via the FreeRTOS-backed queue this
// factory returns (control_queue_freertos.cpp); the render task drains it
// via pumpControlEvents in on_pre_render.
static IControlEventQueue* g_controlQueue = nullptr;

// F4: the eval submission queue, mirroring g_controlQueue exactly --
// web_input.cpp's WS handler pushes eval requests here; the render task
// drains it (glow::pumpEvalSubmissions in on_pre_render) and broadcasts
// each result (send_eval_result_to_ws).
static IEvalSubmissionQueue* g_evalQueue = nullptr;

// F4: demo cue metadata for the web console + the MIDI/OSC binding table
// -- intentionally minimal (one togglable cue, one master fader), matching
// README_WEB_CONSOLE.md's documented setup pattern. Currently never
// populated (F5 removed the hardcoded no-bundle demo patch that used to be
// the one thing that filled this in -- see main.cpp's header comment): a
// bundle-loaded show has no cue metadata to expose yet either --
// applyLoadedShow patches fixtures directly, it doesn't wrap them in cues
// -- so richer per-bundle cue/scene metadata for the console remains a
// natural follow-up, not solved here. web_input_init/send_state_to_ws
// below stay written against this general (N cues, not just 0 or 1) shape
// for whenever that follow-up lands.
static WebCueInfo g_wsCues[1];
static size_t     g_nWsCues = 0;
static uint16_t   g_demoShowCueId = 0;  // ShowController cue id (isActive() takes this)
static bool       g_hasDemoCue = false;

// Same controlId (0) is bound across web/MIDI/OSC -- one LiveControl
// binding serves all three transports, since parseWebCommand/parseMidi/
// parseOsc all just produce a ControlEvent{type, id, ...} fed through the
// same LiveControl::handle regardless of which transport produced it.
static const OscBinding g_oscBindings[] = {
  {"/cue/0",   ControlType::Button, 0},
  {"/fader/0", ControlType::Fader,  0},
};
static const OscAddressMap g_oscMap{g_oscBindings, 2};

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

// F6: exposes g_md's matrices to glow.matrix.* by index (see glow_lua_api.h).
class MainMatrixRegistry : public IMatrixRegistry {
public:
  PixelMatrix* matrix(int index) override {
    if (index < 0 || static_cast<size_t>(index) >= g_md.matrices.size()) return nullptr;
    return g_md.matrices[static_cast<size_t>(index)];
  }
};
static MainMatrixRegistry g_matrixRegistry;

// F6: whether the Lua VM ever initialized -- gates render_tick_hooks' Lua-
// dependent work below (fx_error poll, gcStepSlack). Skipping that work
// when g_luaReady is false is the correct behavior, not an oversight:
// there is nothing Lua-related to poll or collect.
static bool g_luaReady = false;

// F5: true once glow_safe_blackout() has fired this boot. Checked by
// on_pre_render (gates re-driving Raw universes -- see safe_blackout.h) and
// by the ~1Hz periodic re-announce in render_tick_hooks. Deliberately
// sticky for the rest of this boot: blackout is a terminal safety state,
// not something later code paths clear on their own. It does NOT lock out
// manual control -- an operator can still go() a cue from the console/
// MIDI/OSC/REPL afterward (see glow_safe_blackout's comment); it only
// stops main.cpp's own matrix-pattern loop from fighting the zeros.
static bool g_blackout = false;
static char g_blackoutReason[160] = {0};

// Fx-error broadcast: every connected client sees it (any of them may be
// running the live REPL waiting to know an effect just died).
static void send_fx_error_to_ws(void* /*ctx*/, const char* json, size_t len) {
  web_ws_broadcast(json, len);
}

// F5: the ONE path every failure mode funnels into -- corrupt/missing SHW1
// bundle, a scripts partition that won't mount, a boot.fnl that errors, an
// OTA image that fails self-validation (see ota_manager.h). Safe to call
// more than once (each call re-stops every cue and re-zeros every Raw
// universe; idempotent) -- but see g_blackout's comment: an operator
// manually starting a cue afterward is a deliberate override, not
// something this function or its periodic re-announce should fight.
//
// Always reports why, on both channels: ESP_LOGE for serial (reliable even
// with nobody connected to the console yet) and a `blackout` WS broadcast
// (best-effort -- see send_blackout_status_to_ws for the periodic re-send
// that catches clients connecting after the fact). A blackout with no
// reason is a debugging nightmare at 2am.
static void glow_safe_blackout(const char* reason) {
  bool wasAlready = g_blackout;
  g_blackout = true;
  std::snprintf(g_blackoutReason, sizeof(g_blackoutReason), "%s", reason ? reason : "unknown");

  safeBlackoutCore(g_show, g_controller);
  led_status_set(LED_ERROR);

  if (wasAlready) {
    ESP_LOGW(TAG, "SAFE BLACKOUT (re-triggered): %s", g_blackoutReason);
  } else {
    ESP_LOGE(TAG, "SAFE BLACKOUT: %s", g_blackoutReason);
  }

  char buf[256];
  size_t n = buildBlackoutJson(g_blackoutReason, buf, sizeof(buf));
  web_ws_broadcast(buf, n);
}

// Re-announces the current blackout state once/sec (see render_tick_hooks)
// so a console that connects AFTER the triggering event still learns about
// it -- web_ws_broadcast only reaches clients connected at call time, and
// the initial glow_safe_blackout() call very often happens before
// web_server_task has any clients at all (most triggers fire during boot).
// Deliberately does NOT touch g_controller/safeBlackoutCore -- only the
// one-shot glow_safe_blackout() call sites do that (see g_blackout's
// comment on why re-stopping cues here would defeat manual recovery).
static void send_blackout_status_to_ws() {
  if (!g_blackout) return;
  char buf[256];
  size_t n = buildBlackoutJson(g_blackoutReason, buf, sizeof(buf));
  web_ws_broadcast(buf, n);
}

// F5 OTA callbacks (ota_manager.h) -- kept here, not in ota_manager.cpp,
// because only main.cpp has visibility into g_controller/glow_safe_blackout/
// web_ws_broadcast (see OtaCallbacks' own header comment for the
// decoupling rationale).
static void ota_cb_rollback_imminent(const char* reason) {
  glow_safe_blackout(reason);
}
static bool ota_cb_cues_active() {
  return g_controller.anyActive();
}
static void ota_cb_broadcast(const char* json, size_t len) {
  web_ws_broadcast(json, len);
}

// Eval-result broadcast: EvalSubmission carries no client fd (see
// eval_queue.h), so every connected client gets the result and matches it
// against its own pending request by `seq` -- the same broadcast-then-
// client-side-match pattern as fx_error.
static void send_eval_result_to_ws(void* /*ctx*/, uint32_t requestId, bool ok, const char* err) {
  static char buf[512];
  size_t n = buildEvalResultJson(requestId, ok, err, buf, sizeof(buf));
  web_ws_broadcast(buf, n);
}

// Periodic `state` broadcast (Phase-4 feedback, web_protocol.h): lets every
// connected console reflect the demo cue's actual on/off state even when
// it was toggled from a different client, MIDI, or OSC. This is a UI
// indicator, not a control path, so a once-a-second cadence (see
// render_tick_hooks' frame counter) is plenty.
static void send_state_to_ws() {
  if (!g_hasDemoCue) return;
  uint16_t activeId = g_wsCues[0].id;
  bool active = g_controller.isActive(g_demoShowCueId);
  static char buf[128];
  size_t n = web_input_build_state(active ? &activeId : nullptr, active ? 1 : 0, buf, sizeof(buf));
  web_ws_broadcast(buf, n);
}

// The render task's two hook points, and what must run at each. This is
// the ONE place that lists every per-frame queue/notification drain --
// deliberately, after the fx_error drain shipped once in this project with
// nothing calling it (no render-loop wiring, no header to call it from).
// Splitting by phase isn't cosmetic: Pre must run BEFORE
// show->renderFrame(t) (this frame's input must apply to THIS frame, not
// arrive a frame late), Post must run AFTER (fx_error/state reflect this
// frame's result; gcStepSlack spends time only known free once the frame
// is actually done -- see render_task.cpp). Adding a fifth drain means
// adding one call in the right case below, not remembering to also wire a
// new call-site into on_pre_render/on_post_render separately.
enum class RenderTickPhase { Pre, Post };

static void render_tick_hooks(RenderTickPhase phase, float t, uint32_t slack_us) {
  switch (phase) {
    case RenderTickPhase::Pre:
      // Drain queued web-console/MIDI/OSC events into g_liveControl ->
      // g_controller -- exactly control_queue.h's contract.
      if (g_controlQueue) {
        pumpControlEvents(*g_controlQueue, g_liveControl, t);
      }
      // Same discipline for the eval channel -- bounded work per frame,
      // each result broadcast to every connected console (see
      // send_eval_result_to_ws; no submission carries a client fd to
      // reply to directly, so broadcast + client-side seq matching is
      // the only option).
      if (g_evalQueue) {
        glow::pumpEvalSubmissions(*g_evalQueue, /*maxPerFrame=*/4, send_eval_result_to_ws, nullptr);
      }
      break;

    case RenderTickPhase::Post: {
      // F3/T3: proof of life for the render loop, once per frame,
      // unconditionally -- see render_task.cpp's esp_task_wdt_add. Kept
      // first in this phase so nothing below it (Lua, OTA, blackout) can
      // ever cause a frame to skip feeding the watchdog.
      esp_task_wdt_reset();

      // F5: OTA self-validation criteria + tick. Cheap (a couple of bool
      // checks once validated) and independent of g_luaReady/g_blackout --
      // a Lua-less or already-blacked-out boot must still be able to
      // prove itself and cancel its own pending rollback (see
      // ota_manager.h).
      ota_manager_note_frame_rendered();
      if (wifi_is_connected()) ota_manager_note_wifi_connected();
      ota_manager_tick();

      // State doesn't touch the Lua VM, so this runs regardless of
      // g_luaReady -- a broken boot.fnl shouldn't also take down
      // cue-state feedback.
      static uint32_t frameCounter = 0;
      if (++frameCounter >= 44) {  // ~once/sec at the render task's 44 Hz
        frameCounter = 0;
        send_state_to_ws();
        send_blackout_status_to_ws();
      }

      if (!g_luaReady) break;

      // Poll every frame, independent of slack: an effect that just
      // threw needs to be reported promptly, not only when there's GC
      // time to spare.
      web_input_poll_fx_error(glow::glowLuaApi(), send_fx_error_to_ws, nullptr);

      // This is the ONLY place the Lua VM's GC ever runs (see lua_vm.h)
      // -- it is created stopped specifically so it can be confined to
      // this bounded, measured window instead of causing an uncontrolled
      // pause on the render path. Must stay last in this phase: it's the
      // one hook that consumes the frame's remaining slack rather than a
      // fixed amount of work.
      if (slack_us != 0) {
        glow::glowLuaVM().gcStepSlack(slack_us);
      }
      break;
    }
  }
}

static void on_pre_render(void* /*ctx*/, float t, Show* show) {
  if (!show) return;

  render_tick_hooks(RenderTickPhase::Pre, t, 0);

  // F5: once blacked out, stop re-driving Raw universes -- safeBlackoutCore
  // already zeroed them; a still-running matrix pattern would immediately
  // overwrite that zero on this very frame otherwise (see safe_blackout.h).
  // Control-event/eval draining above still ran, so a manual cue trigger
  // from the console/MIDI/OSC/REPL after blackout is not blocked by this.
  if (g_blackout) return;

  for (PixelMatrix* m : g_md.matrices) {
    m->render(t);
    for (uint8_t i = 0; i < m->universeCount(); ++i) {
      uint8_t uidx = m->universeIndex(i);
      if (uidx >= show->universeCount()) continue;
      show->writeRawUniverse(uidx, m->universeData(i), DMX_UNIVERSE_SIZE);
    }
  }
}

static void on_post_render(void* /*ctx*/, uint32_t slack_us) {
  render_tick_hooks(RenderTickPhase::Post, 0.0f, slack_us);
}

// F6/F5: initialize the single process-wide Lua VM, install glow.*, load
// the vendored Fennel compiler, and evaluate boot.fnl if the "scripts"
// partition has one. boot.fnl only ever ADDS Lua-defined cues on top of
// whatever setup_show_from_bundle already patched -- but a bundle's
// matrices (if any) run their default pattern independent of Lua entirely
// (see setup_show_from_bundle), so a VM that never comes up, a scripts
// partition that won't mount, or a boot.fnl that errors all mean nobody
// ever gets a chance to configure or quiet that pattern. Each of those
// three is therefore a real failure, not a shrug -- see glow_safe_blackout
// below. A missing boot.fnl (nothing saved yet) is the one truly benign
// case and does not blackout.
static void setup_lua() {
  const char* fennelSrc = reinterpret_cast<const char*>(fennel_lua_start);
  size_t fennelLen = static_cast<size_t>(fennel_lua_end - fennel_lua_start);

  char err[256];
  g_luaReady = glow::glowLuaInit(g_controller, &g_matrixRegistry, fennelSrc, fennelLen, err, sizeof(err));
  if (!g_luaReady) {
    ESP_LOGE(TAG, "Lua/Fennel init failed (scripts disabled this boot): %s", err);
    // F5: whatever a bundle's matrices are doing (e.g. the default rainbow
    // pattern setup_show_from_bundle attaches) runs unconditionally,
    // independent of Lua -- without boot.fnl ever getting a chance to
    // configure/quiet it, that pattern would otherwise keep painting the
    // venue with unrequested content. Safe blackout, loudly reported,
    // beats a surprise demo animation.
    // Sized to always fit the longest prefix + all of `err` (never actually
    // truncates) -- a merely-large-enough guess would still make GCC's
    // -Wformat-truncation (an error under this project's -Werror) flag the
    // theoretical worst case as unfittable, since `err`'s declared size
    // (not its runtime content) is what the compiler reasons about.
    char reason[sizeof(err) + 40];
    std::snprintf(reason, sizeof(reason), "Lua/Fennel VM init failed: %s", err);
    glow_safe_blackout(reason);
    return;
  }
  ESP_LOGI(TAG, "Lua/Fennel VM ready (Fennel compiler loaded, %u bytes source)",
           (unsigned)fennelLen);

  if (!scripts_storage_mount()) {
    ESP_LOGW(TAG, "scripts partition not mounted; no boot.fnl this boot.");
    glow_safe_blackout("scripts partition failed to mount; no boot.fnl this boot");
    return;
  }

  static char bootBuf[16 * 1024];  // generous for a hand-written boot script
  size_t bootLen = 0;
  if (!scripts_storage_read_boot(bootBuf, sizeof(bootBuf), &bootLen)) {
    // A missing boot.fnl is normal (nothing saved yet), not a failure --
    // does NOT blackout. Contrast with the mount failure above and the
    // eval failure below, both of which are real errors.
    ESP_LOGI(TAG, "no boot.fnl found; nothing to evaluate this boot.");
    return;
  }

  if (!glow_lua_eval_fennel(bootBuf, bootLen, err, sizeof(err))) {
    ESP_LOGE(TAG, "boot.fnl failed, base show still renders: %s", err);
    // See the sizing comment on the Lua/Fennel init failure branch above --
    // same reasoning, different (shorter) prefix.
    char reason[sizeof(err) + 40];
    std::snprintf(reason, sizeof(reason), "boot.fnl failed: %s", err);
    glow_safe_blackout(reason);
    return;
  }
  ESP_LOGI(TAG, "boot.fnl evaluated successfully.");
}

// F3: build the show from a loaded bundle via the host-tested applyLoadedShow.
static bool setup_show_from_bundle() {
  uint8_t* buf = (uint8_t*)heap_caps_malloc(BUNDLE_BUF_CAP, MALLOC_CAP_SPIRAM);
  if (!buf) {
    ESP_LOGE(TAG, "no PSRAM for bundle buffer");
    return false;
  }

  LoadedShow ls;
  if (!storage_load_show(&ls, buf, BUNDLE_BUF_CAP)) {
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
  ESP_LOGI(TAG, "  esp-glow firmware  (ESP32-S3)  F5");
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

  // --- F5: OTA self-validation wiring. Set the callbacks and check
  // whether this boot is a pending-verify OTA slot BEFORE anything else --
  // ota_manager_tick starts getting called from the very first rendered
  // frame (render_tick_hooks), so the callbacks must already be in place
  // by then (see ota_manager.h). ---
  OtaCallbacks otaCb = {};
  otaCb.onRollbackImminent = ota_cb_rollback_imminent;
  otaCb.cuesActive = ota_cb_cues_active;
  otaCb.broadcast = ota_cb_broadcast;
  ota_manager_set_callbacks(&otaCb);
  ota_manager_init();

  // --- F1: DMX sink ---
  g_dmx = new DmxSink(DMX_NUM_1, CONFIG_GLOW_DMX_TX_GPIO, CONFIG_GLOW_DMX_RX_GPIO, CONFIG_GLOW_DMX_RTS_GPIO);
  if (!g_dmx->begin()) {
    ESP_LOGE(TAG, "DMX bring-up failed; halting.");
    led_status_set(LED_ERROR);
    return;
  }
  ota_manager_note_dmx_ready();  // F5: one of the three self-validation criteria

  // --- F2/F5: WiFi (STA), with a SoftAP fallback after repeated failed
  // reconnects so the console stays reachable even when the venue's WiFi
  // is gone (see wifi_manager.h's HIL flag on AP+DMX coexistence). ---
  WifiStaConfig wc = {};
  wc.ssid = CONFIG_GLOW_WIFI_SSID;
  wc.password = CONFIG_GLOW_WIFI_PASS;
  wc.ap_fallback = true;
  wifi_start_sta(&wc);

  // --- F2: Art-Net sink ---
  uint32_t bridge = CONFIG_GLOW_ARTNET_BRIDGE_IP;
  if (bridge == 0) bridge = 0xFFFFFFFFu;
  g_artnet = new ArtNetSink(bridge, 6454);
  if (!g_artnet->begin()) {
    ESP_LOGE(TAG, "Art-Net socket failed; matrix output disabled.");
  }

  // --- F5: always keep at least one DMX universe configured and
  // streaming, independent of whether a show bundle loads -- "safe
  // blackout" means the DMX line keeps carrying zero frames, and that only
  // works if a universe exists to send them on. setup_show_from_bundle
  // overwrites this via applyLoadedShow's own setUniverseCount/
  // configureUniverse if a bundle loads successfully (see apply_loaded_show.cpp). ---
  g_show.setUniverseCount(1);
  g_show.configureUniverse(0, UniverseMode::Fixture, g_dmx);

  // --- F3/F5: load the show from the raw "show" partition; a missing or
  // corrupt bundle is now a safe blackout, not a hardcoded demo patch --
  // see glow_safe_blackout and this file's header comment. ---
  bool from_bundle = setup_show_from_bundle();
  if (!from_bundle) {
    glow_safe_blackout("show partition: missing or corrupt SHW1 bundle (no valid show)");
  }

  // --- F6: g_controller is g_show's ONE effect (see its declaration's
  // comment). Must happen before the render task starts, and before
  // anything (Lua, LiveControl) is asked to drive g_controller, so it's
  // already part of every frame from the first one rendered. ---
  g_show.addEffect(&g_controller);

  // --- F4 (pulled forward): the control-event queue LiveControl drains
  // into g_controller from (see on_pre_render). Sized generously (64) so
  // overflow never happens at human event rates, matching
  // control_queue.h's own guidance. ---
  g_controlQueue = createDeviceControlEventQueue(64);

  // --- F4: eval submission queue, same sizing rationale as g_controlQueue ---
  g_evalQueue = createDeviceEvalSubmissionQueue(64);

  // --- F6: Lua/Fennel VM + boot.fnl (see setup_lua's comment) ---
  setup_lua();

  // --- Render task (core 1, 44 Hz, pre_render drives all matrices,
  // post_render paces the Lua VM's GC) ---
  RenderTaskConfig rcfg = {};
  rcfg.show       = &g_show;
  rcfg.targetHz   = 44;
  rcfg.core       = 1;
  rcfg.stackBytes = 4096;
  rcfg.priority   = 20;
  rcfg.pre_render = on_pre_render;
  rcfg.pre_render_ctx = nullptr;
  rcfg.post_render = on_post_render;
  rcfg.post_render_ctx = nullptr;
  if (!render_task_start(&rcfg)) {
    ESP_LOGE(TAG, "render task start failed; halting.");
    led_status_set(LED_ERROR);
    return;
  }
  led_status_set(LED_BLINK_FAST);

  // --- F4/F5: web console (WS httpd + console static files + /ota) + MIDI
  // + OSC --- any g_liveControl bindings would need to already be in place
  // before any of these start -- "configured once, before tasks start, and
  // never mutated" is what makes transport-side binding reads race-free
  // (none are bound yet -- see g_wsCues' comment).
  web_input_init(*g_controlQueue, *g_evalQueue,
                 g_nWsCues ? g_wsCues : nullptr, g_nWsCues,
                 /*scenes=*/nullptr, /*nScenes=*/0,
                 /*hasMaster=*/true);
  web_server_task(nullptr);  // starts httpd; not a FreeRTOS task itself (see web_input.h)

  midi_input_init(*g_controlQueue, CONFIG_GLOW_MIDI_UART_NUM, CONFIG_GLOW_MIDI_RX_GPIO);
  xTaskCreatePinnedToCore(midi_uart_task, "midi", 4096 / sizeof(StackType_t),
                          nullptr, 5, nullptr, 0);

  osc_input_init(*g_controlQueue, g_oscMap, static_cast<uint16_t>(CONFIG_GLOW_OSC_UDP_PORT));
  xTaskCreatePinnedToCore(osc_server_task, "osc", 4096 / sizeof(StackType_t),
                          nullptr, 5, nullptr, 0);

  ESP_LOGI(TAG, "F5 complete: show is %s.",
           from_bundle ? "loaded from the 'show' partition" : "in safe blackout (no valid bundle)");
  ESP_LOGI(TAG, "Reflash the 'show' partition and reboot to change the patch.");

  while (true) {
    led_status_set(wifi_is_connected() ? LED_BLINK_DOUBLE : LED_BLINK_FAST);
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}
