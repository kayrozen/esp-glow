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
#include <cstring>
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
#include "device_config.h"
#include "device_config_web.h"
#include "safe_blackout.h"
#include "ota_manager.h"
#include "esp_task_wdt.h"

#include "show.h"
#include "show_control.h"
#include "live_control.h"
#include "control_queue.h"
#include "effects.h"  // DimmerEffect -- used by setup_selftest_fixture() (CONFIG_GLOW_SELFTEST)
#include "dmx_sink.h"
#include "artnet_sink.h"
#include "artnet_discovery_task.h"
#include "pixel_matrix.h"
#include "pixel_patterns.h"
#include "show_bundle.h"
#include "apply_loaded_show.h"

#include "beat_clock.h"
#include "beat_queue.h"
#include "glow_fennel.h"
#include "glow_lua_api.h"
#include "lua_vm.h"  // complete LuaVM type -- glow_fennel.h only forward-declares it (glowLuaVM().gcStepSlack(...) needs the full definition)
#include "scripts_storage.h"
#include "web_input.h"
#include "web_protocol.h"  // buildEvalResultJson (send_eval_result_to_ws)
#include "midi_input.h"
#include "led_feedback.h"
#include "osc_input.h"
#include "djlink_input.h"
#include "usb_midi_input.h"
#include "freertos/task.h"  // xTaskCreatePinnedToCore (midi_uart_task/osc_server_task/djlink_*_task)

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

#define BUNDLE_BUF_CAP (64 * 1024)  // 64 KB scratch in PSRAM

// CFG1 (device_config.h): builds the DeviceConfig used when the "devcfg"
// partition is absent or fails to parse (missing, blank/erased, corrupt --
// see storage_load_devcfg). Every field here is exactly what menuconfig
// already controlled before CFG1 existed, so a dev build with no devcfg
// ever flashed behaves exactly as it did before this feature landed. This
// is the ONE place CONFIG_GLOW_* (besides GLOW_SELFTEST/GLOW_USB_MIDI_HOST,
// which gate compilation, not runtime values) is read -- everywhere else
// in this file reads the resolved `cfg` instead.
static DeviceConfig defaultDeviceConfigFromKconfig() {
  DeviceConfig cfg;
  std::snprintf(cfg.wifiSsid, sizeof(cfg.wifiSsid), "%s", CONFIG_GLOW_WIFI_SSID);
  std::snprintf(cfg.wifiPass, sizeof(cfg.wifiPass), "%s", CONFIG_GLOW_WIFI_PASS);
  cfg.artnetFallbackIp = static_cast<uint32_t>(CONFIG_GLOW_ARTNET_BRIDGE_IP);
  cfg.artnetPort = 6454;
  cfg.dmxTxGpio = CONFIG_GLOW_DMX_TX_GPIO;
  cfg.dmxRxGpio = CONFIG_GLOW_DMX_RX_GPIO;
  cfg.dmxRtsGpio = CONFIG_GLOW_DMX_RTS_GPIO;
  cfg.ledGpio = CONFIG_GLOW_STATUS_LED_GPIO;
#ifdef CONFIG_GLOW_USB_MIDI_HOST_DEFAULT_ON
  cfg.usbMidiHost = true;
#else
  cfg.usbMidiHost = false;
#endif
#ifdef CONFIG_GLOW_SKIP_WIFI
  cfg.skipWifi = true;
#else
  cfg.skipWifi = false;
#endif
  return cfg;
}

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

// A5/.mdef: the first CONTROLLER embedded in the loaded SHW1 bundle (B1 --
// the transports below support one controller at a time). Copied out of
// LoadedShow (a boot-only local, see setup_show_from_bundle) into a
// process-lifetime global so LedFeedback's borrowed reference stays valid;
// g_ledFeedback is constructed only if g_hasController ends up true.
// g_midiOutput adapts DIN MIDI OUT (midi_input.cpp) to LedFeedback's
// IMidiOutput seam -- see led_feedback.h.
static MidiControllerProfile g_controllerProfile;
static bool                  g_hasController = false;
static DeviceMidiOutput      g_midiOutput;
static LedFeedback*          g_ledFeedback = nullptr;

// Wave 3 Phase 3: the bundle's per-universe Art-Net routes, copied out of
// LoadedShow the same way g_controllerProfile is (ls is a boot-only local;
// see setup_show_from_bundle) so artnet_discovery_task_init has something
// process-lifetime to read. This is exactly the signal discovery needs to
// respect the precedence rule: artnetDest[u].ip != 0 means the .show
// already routed universe u explicitly, so discovery must never touch it.
static ArtNetDest g_artnetShowDest[MAX_UNIVERSES];
static uint8_t    g_artnetUniverseCount = 0;

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

// Musical time: MIDI clock/DJ-Link/tap all push glow::BeatEvents onto this
// queue (same transport-pushes/render-task-drains discipline as
// g_controlQueue -- see beat_queue.h). g_beatClock is owned by, and only
// ever touched from, the render task (drained in on_pre_render, alongside
// pumpControlEvents), and is what glow.beat/bar/bpm/locked? (glow_lua_api)
// read from every frame.
static glow::BeatClock g_beatClock;
static glow::IBeatEventQueue* g_beatQueue = nullptr;

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

  // Wave 3: the bundle's per-universe (IP, wire-universe) route, applied
  // straight onto g_artnet before the render task ever calls send() for
  // this universe -- see setup_show_from_bundle's call order and
  // ArtNetSink::setDest's ordering note.
  void configureArtnetDest(uint8_t universeIdx, const ArtNetDest& dest) override {
    if (g_artnet) g_artnet->setDest(universeIdx, dest);
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

// v2: exposes g_show's patched fixtures to glow.ranges by fixture id (see
// glow_lua_api.h's IFixtureRegistry).
class MainFixtureRegistry : public IFixtureRegistry {
public:
  const FixtureProfile* profile(uint16_t fixtureId) override {
    const PatchedFixture* f = g_show.fixture(fixtureId);
    return f ? &f->profile : nullptr;
  }
};
static MainFixtureRegistry g_fixtureRegistry;

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

// --- Phase 0: HIL selftest observability (tests/hil/) -------------------
//
// Everything in this section is compiled in only under CONFIG_GLOW_SELFTEST
// (Kconfig.projbuild) and MUST NEVER be on in a release build: it's a
// chatty serial protocol plus a UART command-reader task that a real show
// has no use for. It gives the tests/hil/ pytest suite two things a real
// show has no need to expose:
//   - structured `GLOW-TEST: <event> key=value...` telemetry lines, parsed
//     by tests/hil/conftest.py's TelemetryLine
//   - serial query commands (?dmx0 / ?state / ?lua), answered synchronously
//     from a dedicated low-priority task, so hardware state is assertable
//     without a WS client or a physical DMX loopback
//
// See the design doc / task description's "Phase 0 — Firmware
// observability" section for the exact wire format each line/command must
// produce; the HIL suite's tests are written against that format verbatim.
#ifdef CONFIG_GLOW_SELFTEST

// The selftest fixture's exact target byte: round(200.0f/255.0f * 255.0f)
// == 200 with no rounding slop (see fixture_profile.cpp's applyCapability
// and test_show.cpp's regression check for this same constant).
static constexpr float kSelftestDimmerLevel = 200.0f / 255.0f;

// The "main" task's handle (the one that runs app_main and, since
// app_main never returns, keeps existing for the device's whole life) --
// captured once at the top of app_main so selftest_print_stack_hwm below
// (called from the render task) can read ITS high water mark too, not
// just its own. See CONFIG_ESP_MAIN_TASK_STACK_SIZE's sdkconfig.defaults
// comment for why this task's stack budget is not a formality here.
static TaskHandle_t g_mainTaskHandle = nullptr;

static void selftest_print_stats(uint32_t frames, uint32_t behind, uint32_t dropped) {
  size_t heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  size_t luaMem = g_luaReady ? glow::glowLuaVM().memUsed() : 0;
  printf("GLOW-TEST: stats frames=%u behind=%u dropped=%u heap=%u lua_mem=%u\n",
         (unsigned)frames, (unsigned)behind, (unsigned)dropped,
         (unsigned)heap, (unsigned)luaMem);
}

// Stack high-water marks (uxTaskGetStackHighWaterMark returns bytes on
// ESP-IDF's FreeRTOS port, not words -- see its header comment): the
// smallest amount of free stack space either task has had since it
// started. Both matter for different reasons -- main: one-time boot cost
// (driver bring-up, FixtureProfile-by-value show patching, Lua/Fennel VM
// init) that only ever gets worse if boot.fnl or the bundle grows; render:
// an ONGOING cost every frame the live-coding REPL is used, since
// glow_lua_eval_fennel (a recursive-descent Fennel compiler) and every
// effect's lua_pcall both run on THIS task's stack, not a dedicated one
// (see glow_fennel.h/lua_effect.cpp). A high-water mark trending toward 0
// over a long soak is the signal to size up, not a guess.
static void selftest_print_stack_hwm() {
  UBaseType_t mainHwm = g_mainTaskHandle ? uxTaskGetStackHighWaterMark(g_mainTaskHandle) : 0;
  UBaseType_t renderHwm = uxTaskGetStackHighWaterMark(nullptr);  // NULL = calling (render) task
  printf("GLOW-TEST: stack main=%u render=%u\n", (unsigned)mainHwm, (unsigned)renderHwm);
}

// Answers one query line (already trimmed of its trailing newline). Runs on
// the selftest query task, never the render task -- see
// selftest_query_task's header comment for why that's safe despite reading
// g_show/g_controller/the Lua VM, all of which the render task also touches.
static void selftest_handle_query(const char* cmd) {
  if (std::strcmp(cmd, "?dmx0") == 0) {
    const uint8_t* data = g_show.universeData(0);
    if (!data) {
      printf("GLOW-TEST: dmx0 bytes=none\n");
      return;
    }
    printf("GLOW-TEST: dmx0 bytes=%u,%u,%u,%u,%u,%u,%u,%u\n",
           (unsigned)data[0], (unsigned)data[1], (unsigned)data[2], (unsigned)data[3],
           (unsigned)data[4], (unsigned)data[5], (unsigned)data[6], (unsigned)data[7]);
    return;
  }

  if (std::strcmp(cmd, "?state") == 0) {
    uint16_t ids[32];
    size_t n = g_controller.activeCueIds(ids, 32);
    if (n == 0) {
      printf("GLOW-TEST: state cues=none\n");
      return;
    }
    char buf[256];
    size_t off = 0;
    for (size_t i = 0; i < n && off < sizeof(buf); ++i) {
      off += static_cast<size_t>(snprintf(buf + off, sizeof(buf) - off, i ? ",%u" : "%u", (unsigned)ids[i]));
    }
    printf("GLOW-TEST: state cues=%s\n", buf);
    return;
  }

  if (std::strcmp(cmd, "?lua") == 0) {
    if (!g_luaReady) {
      printf("GLOW-TEST: lua mem=0 highwater=0\n");
      return;
    }
    printf("GLOW-TEST: lua mem=%u highwater=%u\n",
           (unsigned)glow::glowLuaVM().memUsed(), (unsigned)glow::glowLuaVM().memHighWater());
    return;
  }

  // Unrecognized query: silently ignored (not every line typed at the
  // console is meant for us -- e.g. IDF's own monitor keystrokes).
}

// Blocks reading stdin (the same UART0 the ESP-IDF console/log already
// uses) one byte at a time, accumulating a line, and dispatches it to
// selftest_handle_query on '\n'. This is its own low-priority FreeRTOS
// task specifically so it can block on fgetc(stdin) -- render_tick_hooks'
// own comment is explicit that it is "the ONE place" for per-frame,
// non-blocking work, and a blocking stdin read has no place there. Reading
// g_show/g_controller/the Lua VM from a different task than the render
// task IS a cross-core data race in general (see control_queue.h's
// rationale) -- acceptable here only because this is test-only
// instrumentation behind a flag that must never ship, answering a human
// (or a test harness acting like one) one query at a time, not a
// real-time control path.
static void selftest_query_task(void*) {
  char line[32];
  size_t len = 0;
  while (true) {
    int c = fgetc(stdin);
    if (c == EOF) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (c == '\r') continue;
    if (c == '\n') {
      line[len] = '\0';
      if (len > 0) selftest_handle_query(line);
      len = 0;
      continue;
    }
    if (len + 1 < sizeof(line)) {
      line[len++] = static_cast<char>(c);
    } else {
      len = 0;  // overlong line: drop it rather than mis-parse a truncation
    }
  }
}

#endif  // CONFIG_GLOW_SELFTEST

// Fx-error broadcast: every connected client sees it (any of them may be
// running the live REPL waiting to know an effect just died).
static void send_fx_error_to_ws(void* /*ctx*/, const char* effectName, const char* err,
                                const char* json, size_t len) {
#ifdef CONFIG_GLOW_SELFTEST
  printf("GLOW-TEST: fx_disabled name=%s err=%s\n", effectName, err);
#else
  (void)effectName;
  (void)err;
#endif
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
      // Musical time: drain MIDI-clock/DJ-Link/tap BeatEvents into
      // g_beatClock -- same contract as g_controlQueue above, and must
      // also run before renderFrame so this frame's beat-synced effects
      // see this frame's phase, not last frame's.
      if (g_beatQueue) {
        glow::pumpBeatEvents(*g_beatQueue, g_beatClock);
      }
      // A5/.mdef: LED feedback. Runs after pumpControlEvents above so a
      // button press dispatched this frame (go()/release() already applied
      // to g_controller) is reflected in this same frame's LED refresh
      // instead of lagging a frame behind. Change-detection + rate-limiting
      // live in LedFeedback itself -- this call is unconditional every
      // frame, but is a no-op send-wise once nothing has changed.
      if (g_ledFeedback) {
        g_ledFeedback->refresh(g_controller, t);
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
#ifdef CONFIG_GLOW_SELFTEST
        uint32_t frames = 0, behind = 0, dropped = 0;
        render_task_get_and_reset_stats(&frames, &behind, &dropped);
        selftest_print_stats(frames, behind, dropped);
        selftest_print_stack_hwm();
#endif
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
  // Wave 3: exactly one ArtSync broadcast per frame, right after every
  // Art-Net send() this frame's renderFrame() just did (render_task.cpp
  // calls s_post_render immediately after show->renderFrame) -- this is
  // what makes every node latch its outputs simultaneously instead of
  // whenever its own packet happens to arrive (visible tearing on a
  // matrix spanning multiple universes otherwise). A safe no-op before
  // g_artnet->begin() (socket not open yet).
  if (g_artnet) g_artnet->frameEnd();

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
  g_luaReady = glow::glowLuaInit(g_controller, &g_matrixRegistry, g_beatClock, g_liveControl,
                                 fennelSrc, fennelLen, err, sizeof(err),
                                 0, 0, 0, &g_fixtureRegistry, g_ledFeedback);
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
#ifdef CONFIG_GLOW_SELFTEST
  printf("GLOW-TEST: scripts mount=ok\n");
#endif

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

static FixtureProfile makeDimmerProfile() {
  FixtureProfile p{};
  p.footprint = 1;
  p.channelCount = 1;
  p.channels[0] = { Capability::Dimmer, 0, 0xFF, 0, 0 };
  return p;
}

#ifdef CONFIG_GLOW_SELFTEST
// Phase 0 selftest fixture: used in place of the normal no-bundle response
// when CONFIG_GLOW_SELFTEST is on (see app_main). A real show (bundle-
// loaded, or F5's safe blackout otherwise) isn't deterministic enough to
// assert exact byte values against -- this patches exactly one fixture
// (universe 0, channel 0) to a known Dimmer effect at kSelftestDimmerLevel
// (200/255) and goes it immediately, so `?dmx0` always reads back 200 in
// the first byte on a selftest build with no bundle flashed (see L0/L1 in
// tests/hil/). Deliberately minimal -- no matrix, no Art-Net universes --
// there is nothing here for a test to assert on except the one known
// channel. This is the ONLY caller of makeDimmerProfile() left: the old
// F1/F2 hardcoded fallback it used to share this with was removed by F5
// (missing/corrupt bundle is a safe blackout now, not a demo patch -- see
// glow_safe_blackout and this file's header comment).
static void setup_selftest_fixture() {
  ESP_LOGW(TAG, "CONFIG_GLOW_SELFTEST: using the deterministic selftest fixture (no bundle found).");
  g_show.setUniverseCount(1);
  g_show.configureUniverse(0, UniverseMode::Fixture, g_dmx);

  FixtureProfile dimmer = makeDimmerProfile();
  uint16_t fixtureId = g_show.patch(dimmer, 0, 0);
  static DimmerEffect fx(std::vector<uint16_t>{fixtureId}, kSelftestDimmerLevel);
  uint16_t cueId = g_controller.addCue({&fx}, /*fadeInSec=*/0.0f, /*fadeOutSec=*/0.0f, /*priority=*/0);
  g_controller.go(cueId, 0.0f);

  g_liveControl.bindButton(0, ActionKind::CueToggle, cueId);
  g_wsCues[0] = { /*id=*/0, "Selftest", "#ff0000", ActionKind::CueToggle };
  g_nWsCues = 1;
  g_demoShowCueId = cueId;
  g_hasDemoCue = true;
}
#endif  // CONFIG_GLOW_SELFTEST

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
#ifdef CONFIG_GLOW_SELFTEST
  printf("GLOW-TEST: bundle fixtures=%u matrices=%u\n",
         (unsigned)ls.fixtures.size(), (unsigned)ls.matrices.size());
#endif

  DeviceSinkFactory factory;
  ApplyResult r = applyLoadedShow(ls, g_show, factory);
  ESP_LOGI(TAG, "applied: %u universes configured, %u skipped, %u fixtures (%u heads), %u matrix universes",
           r.universesConfigured, r.universesSkipped, r.fixturesPatched,
           r.headsPatched, r.matrixUniverses);

  // Wave 3 Phase 3: copy the bundle's per-universe routes out before `ls`
  // goes out of scope (same reasoning as g_controllerProfile below) --
  // artnet_discovery_task_init reads this once WiFi/Art-Net come up.
  g_artnetUniverseCount = ls.universeCount;
  for (uint8_t u = 0; u < ls.universeCount && u < MAX_UNIVERSES; ++u) {
    g_artnetShowDest[u] = ls.artnetDest[u];
  }

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

  // A5/.mdef: adopt the first embedded controller definition, if any (B1:
  // one controller at a time). Copied out of `ls` since it's about to go
  // out of scope -- see g_controllerProfile's own comment.
  if (!ls.controllers.empty()) {
    g_controllerProfile = ls.controllers[0];
    g_hasController = true;
    ESP_LOGI(TAG, "controller: mdef loaded (%u pads, %u faders, %u encoders, %u LED ranges)",
             (unsigned)g_controllerProfile.padCount, (unsigned)g_controllerProfile.faderCount,
             (unsigned)g_controllerProfile.encoderCount, (unsigned)g_controllerProfile.ledCount);
  }

  return true;
}

extern "C" void app_main(void) {
#ifdef CONFIG_GLOW_SELFTEST
  // Captured first, before anything else can fail/return early -- see
  // g_mainTaskHandle's own comment and selftest_print_stack_hwm.
  g_mainTaskHandle = xTaskGetCurrentTaskHandle();
#endif

  // --- CFG1: read the "devcfg" partition FIRST, before anything else --
  // even the status LED init a few lines down needs cfg.ledGpio, and DMX
  // bring-up (F1, below) needs cfg.dmxTxGpio/RxGpio/RtsGpio. This is a
  // local flash read only (esp_partition_read) -- it must never depend on,
  // or wait for, the network (see FORMAT.md's boot-ordering note): nothing
  // here touches WiFi, and WiFi bring-up itself stays all the way at the
  // bottom of this function, unchanged. A missing or corrupt devcfg falls
  // back to the compiled-in Kconfig defaults -- see
  // defaultDeviceConfigFromKconfig's header comment -- and is reported
  // loudly below, once the banner (and serial) is up. ---
  DeviceConfig cfg;
  const char* cfgSource = "devcfg";
  if (!storage_load_devcfg(&cfg)) {
    cfg = defaultDeviceConfigFromKconfig();
    cfgSource = "defaults";
  }

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

  // CFG1: report what actually took effect -- both a human-readable log
  // line (never the WiFi password) and, under CONFIG_GLOW_SELFTEST, the
  // structured GLOW-TEST: line the QEMU/HIL harnesses assert on. A corrupt
  // or missing devcfg falling back silently would be a debugging nightmare
  // the first time someone's WiFi doesn't come up -- this is that report.
  ESP_LOGI(TAG, "cfg: source=%s dmx_tx=%u dmx_rx=%u dmx_rts=%u led=%u usb_midi=%d skip_wifi=%d "
                "artnet_fallback=%s:%u ssid=\"%s\"",
           cfgSource, (unsigned)cfg.dmxTxGpio, (unsigned)cfg.dmxRxGpio, (unsigned)cfg.dmxRtsGpio,
           (unsigned)cfg.ledGpio, (int)cfg.usbMidiHost, (int)cfg.skipWifi,
           cfg.artnetFallbackIp == 0 ? "broadcast" : "unicast", (unsigned)cfg.artnetPort, cfg.wifiSsid);
#ifdef CONFIG_GLOW_SELFTEST
  printf("GLOW-TEST: cfg source=%s dmx_tx=%u usb_midi=%d skip_wifi=%d\n",
         cfgSource, (unsigned)cfg.dmxTxGpio, (int)cfg.usbMidiHost, (int)cfg.skipWifi);
#endif

  led_status_init(cfg.ledGpio);
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

  // CFG1 §6: same "refuse while cues are active" gate as OTA (a reconfigure
  // reboot mid-show helps nobody either); GET /devcfg serializes `cfg` as
  // resolved above (devcfg or Kconfig defaults), so the reconfigure page
  // always has a real starting point to edit. Both wired well before the
  // web server can possibly start (see the network bring-up block below).
  DeviceConfigWebCallbacks devcfgCb = {};
  devcfgCb.cuesActive = ota_cb_cues_active;
  device_config_web_set_callbacks(&devcfgCb);
  device_config_web_set_effective_config(cfg);

  // --- F1: DMX sink ---
  g_dmx = new DmxSink(DMX_NUM_1, cfg.dmxTxGpio, cfg.dmxRxGpio, cfg.dmxRtsGpio);
  if (!g_dmx->begin()) {
    ESP_LOGE(TAG, "DMX bring-up failed; halting.");
    led_status_set(LED_ERROR);
    return;
  }
  ota_manager_note_dmx_ready();  // F5: one of the three self-validation criteria
#ifdef CONFIG_GLOW_SELFTEST
  printf("GLOW-TEST: dmx begin=ok\n");
#endif

  // --- F2/Wave 3: Art-Net sink -- constructed now (not begun). WiFi/netif
  // bring-up moves to the very end of this function (see the "network
  // bring-up" block below), but DeviceSinkFactory::sinkFor() still needs a
  // non-null g_artnet right now, before setup_show_from_bundle patches any
  // ArtNet-transport universe below (and calls configureArtnetDest for
  // each one -- see that function and ArtNetSink::setDest's ordering
  // note). g_artnet->send()/frameEnd() are safe no-ops until begin()
  // actually opens the UDP socket (sock_ stays -1 until then; see
  // artnet_sink.cpp), so constructing early costs nothing. ---
  // CFG1: artnetFallbackIp is the destination for Art-Net universes the
  // .show does not route explicitly (0 = broadcast) -- see FORMAT.md's
  // "artnetFallbackIp" precedence note. Precedence, per FORMAT.md: an
  // explicit per-universe (IP, wire-universe) route in the .show wins over
  // this fallback, which wins over broadcast. ArtNetSink resolves that
  // precedence itself (via ArtNetRouter) every send -- ip=0 on a
  // per-universe route means "use this fallback."
  g_artnet = new ArtNetSink(cfg.artnetPort, cfg.artnetFallbackIp);

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

  // A5/.mdef: only now (after setup_show_from_bundle populated
  // g_controllerProfile, if the bundle had a CONTROLLER) is there anything
  // for LedFeedback to address. g_midiOutput's actual sends stay a no-op
  // until midi_input_init/midi_uart_task finish bringing up MIDI OUT below
  // -- constructing this early just means glow.led.* is live from the
  // first boot.fnl eval instead of racing the UART task's startup.
  if (g_hasController) {
    g_ledFeedback = new LedFeedback(g_controllerProfile, &g_midiOutput);
  }

  if (!from_bundle) {
#ifdef CONFIG_GLOW_SELFTEST
    // HIL builds get a deterministic fixture instead of blackout so the
    // pytest suite has known bytes to assert on (see setup_selftest_fixture's
    // header comment) -- CONFIG_GLOW_SELFTEST must never be on in a release
    // build, so this never trades away F5's real-world guarantee below.
    setup_selftest_fixture();
#else
    glow_safe_blackout("show partition: missing or corrupt SHW1 bundle (no valid show)");
#endif
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

  // --- F4: eval submission queue. NOT the same sizing as g_controlQueue --
  // EvalSubmission carries a fixed EVAL_SRC_MAX=4096-byte source buffer per
  // slot (eval_queue.h), unlike ControlEvent/BeatEvent's few bytes, and
  // FreeRTOS queue storage is internal-RAM-only, always (ESP-IDF maps it
  // to MALLOC_CAP_INTERNAL unconditionally -- see
  // components/freertos/heap_idf.c -- so CONFIG_SPIRAM_USE_MALLOC does not
  // help here). Capacity 64 here was ~256 KB of required internal RAM --
  // most of the whole internal heap on an ESP32-S3 -- and xQueueCreate
  // failed outright on first real boot (QEMU found this; see
  // eval_queue_freertos.cpp's constructor for the resulting safe-no-op
  // fallback if this ever happens again). A human at a REPL submits one
  // eval at a time and waits for its result (web_protocol.h's eval/
  // eval_result round trip) -- 8 in flight is already generous headroom
  // for multiple simultaneous console clients, at a sane ~32 KB. ---
  g_evalQueue = createDeviceEvalSubmissionQueue(8);

  // --- Musical time: the beat-event queue MIDI clock/DJ-Link/tap push
  // into, drained into g_beatClock every frame (see on_pre_render). Beats
  // arrive far less often than control events (at most a few Hz even at
  // fast tempos), so a smaller capacity than g_controlQueue is plenty. ---
  g_beatQueue = glow::createDeviceBeatEventQueue(16);

  // --- F6: Lua/Fennel VM + boot.fnl (see setup_lua's comment) ---
  setup_lua();

  // --- Render task (core 1, 44 Hz, pre_render drives all matrices,
  // post_render paces the Lua VM's GC) ---
  RenderTaskConfig rcfg = {};
  rcfg.show       = &g_show;
  rcfg.targetHz   = 44;
  rcfg.core       = 1;
  // 4096 was sized for what this task used to do (pixel-matrix rendering,
  // fixed per-frame work). It's no longer just that: glow_lua_eval_fennel
  // (live-coded Fennel -- a recursive-descent compiler) is drained here on
  // eval, and every effect's lua_pcall runs here every frame too (see
  // glow_fennel.h/lua_effect.cpp) -- both on THIS stack, not a dedicated
  // one. 20 KB is a generous starting point, not a measured minimum; see
  // GLOW-TEST: stack telemetry (CONFIG_GLOW_SELFTEST,
  // selftest_print_stack_hwm) for the real high-water mark under a full
  // boot + Fennel eval + effects load.
  rcfg.stackBytes = 20480;
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
#ifdef CONFIG_GLOW_SELFTEST
  printf("GLOW-TEST: boot core=%u hz=%u\n", (unsigned)rcfg.core, (unsigned)rcfg.targetHz);
  // Low priority (5, same tier as the MIDI/OSC transport tasks): answering
  // a serial query is never more urgent than real-time rendering or
  // network input, and this task spends nearly all its time blocked on
  // fgetc(stdin) waiting for a byte.
  xTaskCreatePinnedToCore(selftest_query_task, "selftest_query", 4096 / sizeof(StackType_t),
                          nullptr, 5, nullptr, 0);
#endif

  // --- Network bring-up (WiFi, Art-Net, web console, OSC, DJ Link) is
  // deliberately LAST, not first: DMX output, the render task, and the
  // Lua/Fennel VM (boot.fnl, live-coded cues) are all fully up before
  // anything here is even attempted, so a rig with no network at all --
  // dead AP, no WiFi at the venue, cfg.skipWifi set outright -- still
  // plays its show instead of never getting past app_main(). This is the
  // same guarantee F5's reconnect-with-backoff already gives for a network
  // that drops AFTER boot (wifi_manager.cpp) -- extended to cover a
  // network that was never there to begin with, or hardware/emulation
  // that can't bring WiFi up at all (see Kconfig.projbuild's
  // GLOW_SKIP_WIFI help: QEMU has no WiFi/802.11 model, so
  // esp_wifi_init()'s RF calibration step hangs forever against it --
  // tests/qemu/README.md). wifi_start_sta() itself does not block on
  // association (see wifi_manager.cpp) -- moving it here means even
  // esp_wifi_init()/esp_wifi_start() themselves hanging or failing can no
  // longer take the rig's actual light output down with them, which is
  // the point.
  //
  // CFG1: this is now a RUNTIME branch on cfg.skipWifi, not a compile-time
  // #ifdef -- the flasher's "no WiFi" checkbox and the device console's
  // reconfigure page both need this to be a flash-time/runtime choice, not
  // a rebuild. Kconfig's GLOW_SKIP_WIFI only seeds cfg.skipWifi's fallback
  // value when no devcfg is present (defaultDeviceConfigFromKconfig) --
  // see Kconfig.projbuild's updated help text. ---
  if (!cfg.skipWifi) {
    // --- F2/F5: WiFi (STA), with a SoftAP fallback after repeated failed
    // reconnects so the console stays reachable even when the venue's WiFi
    // is gone (see wifi_manager.h's HIL flag on AP+DMX coexistence). ---
    WifiStaConfig wc = {};
    wc.ssid = cfg.wifiSsid;
    wc.password = cfg.wifiPass;
    wc.ap_fallback = true;
    wifi_start_sta(&wc);

    // --- F2: Art-Net sink begin() -- opens the UDP socket now that
    // netif/lwIP is up (wifi_start_sta() brings that up regardless of
    // whether STA ever associates). g_artnet itself was already constructed
    // earlier so bundle patching above could route ArtNet universes to it. ---
    if (!g_artnet->begin()) {
      ESP_LOGE(TAG, "Art-Net socket failed; matrix output disabled.");
    } else {
#ifdef CONFIG_GLOW_SELFTEST
      printf("GLOW-TEST: artnet tx=ok\n");
#endif
      // Wave 3 Phase 3: ArtPoll discovery, own socket + own task -- fills
      // in only the universes the .show left unspecified (see
      // g_artnetShowDest's comment); a node vanishing reverts its
      // universes to fallback/broadcast, never darkness.
      artnet_discovery_task_init(g_artnet, g_artnetShowDest, g_artnetUniverseCount);
      xTaskCreatePinnedToCore(artnet_discovery_task, "artnet_disc", 4096 / sizeof(StackType_t),
                              nullptr, 5, nullptr, 0);
    }

    // --- F4/F5: web console (WS httpd + console static files + /ota +
    // /devcfg) + OSC -- any g_liveControl bindings would need to already
    // be in place before either of these start -- "configured once,
    // before tasks start, and never mutated" is what makes transport-side
    // binding reads race-free (none are bound yet -- see g_wsCues'
    // comment).
    web_input_init(*g_controlQueue, *g_evalQueue,
                   g_nWsCues ? g_wsCues : nullptr, g_nWsCues,
                   /*scenes=*/nullptr, /*nScenes=*/0,
                   /*hasMaster=*/true);
    web_server_task(nullptr);  // starts httpd; not a FreeRTOS task itself (see web_input.h)

    osc_input_init(*g_controlQueue, g_oscMap, static_cast<uint16_t>(CONFIG_GLOW_OSC_UDP_PORT));
    xTaskCreatePinnedToCore(osc_server_task, "osc", 4096 / sizeof(StackType_t),
                            nullptr, 5, nullptr, 0);

    // Musical time: passive Pro DJ Link listener (Afterglow's signature
    // feature -- see djlink_parser.h's header). Two tasks, one per UDP
    // port: beat packets (50001, the actual sync source) and CDJ status's
    // tempo-master flag (50002, gates which player's beats get accepted).
    djlink_input_init(*g_beatQueue);
    xTaskCreatePinnedToCore(djlink_beat_task, "djlink_beat", 4096 / sizeof(StackType_t),
                            nullptr, 5, nullptr, 0);
    xTaskCreatePinnedToCore(djlink_status_task, "djlink_status", 4096 / sizeof(StackType_t),
                            nullptr, 5, nullptr, 0);
  } else {
    ESP_LOGW(TAG, "cfg.skipWifi: WiFi/Art-Net/web console/OSC/DJ Link all skipped -- "
                  "DMX output and Lua/Fennel cues run standalone.");
  }

  // MIDI DIN is a UART, not a network transport -- always up, independent
  // of cfg.skipWifi. Needs the same g_liveControl-bindings-already-
  // in-place guarantee as the transports above (see their comment).
  midi_input_init(*g_controlQueue, g_beatQueue, CONFIG_GLOW_MIDI_UART_NUM, CONFIG_GLOW_MIDI_RX_GPIO,
                  CONFIG_GLOW_MIDI_TX_GPIO);
  xTaskCreatePinnedToCore(midi_uart_task, "midi", 4096 / sizeof(StackType_t),
                          nullptr, 5, nullptr, 0);

#ifdef CONFIG_GLOW_USB_MIDI_HOST
  // The driver is compiled in unconditionally (GLOW_USB_MIDI_HOST default
  // y -- see Kconfig.projbuild): nothing here costs RAM or touches
  // hardware unless cfg.usbMidiHost is actually true, so this compiles to
  // "check a bool, usually skip" when the board has no VBUS path. This is
  // what makes USB-MIDI a flash-time checkbox instead of a rebuild -- B2:
  // this is still a BOARD change (see usb_midi_input.h's hardware note),
  // just no longer also a firmware one.
  if (cfg.usbMidiHost) {
    // Core 0 (with WiFi), same as every other network/USB transport task;
    // watch render_task's `dropped` stat for coexistence regressions.
    usb_midi_input_init(*g_controlQueue);
    xTaskCreatePinnedToCore(usb_midi_host_task, "usb_midi", 4096 / sizeof(StackType_t),
                            nullptr, 5, nullptr, 0);
  } else {
    ESP_LOGI(TAG, "USB-MIDI host compiled in but disabled (cfg.usbMidiHost=false) -- "
                  "no USB host stack, no VBUS.");
  }
#endif

  const char* showStatus;
  if (from_bundle) {
    showStatus = "loaded from the 'show' partition";
#ifdef CONFIG_GLOW_SELFTEST
  } else {
    showStatus = "the CONFIG_GLOW_SELFTEST deterministic fixture (no valid bundle)";
#else
  } else {
    showStatus = "in safe blackout (no valid bundle)";
#endif
  }
  ESP_LOGI(TAG, "F5 complete: show is %s.", showStatus);
  ESP_LOGI(TAG, "Reflash the 'show' partition and reboot to change the patch.");

  while (true) {
    led_status_set(wifi_is_connected() ? LED_BLINK_DOUBLE : LED_BLINK_FAST);
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}
