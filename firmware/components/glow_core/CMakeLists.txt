# `glow_core` ESP-IDF component.
#
# Wraps the host-tested runtime modules (living at the repo root) so they build
# inside the ESP-IDF firmware project. The same .cpp files also build under
# `make test` on the host; device-only transports are guarded by
# `#ifdef ESP_PLATFORM` and compile to nothing on host.
#
# Modules NOT included here (host-only):
#   provision.cpp / profile_encoder.cpp / controller_encoder.cpp
#                                    (PC compiler toolchain, provision_main --
#                                     .fdef/.mdef text parsing + encoding;
#                                     the device only ever sees the compiled
#                                     PFX1/MDF1 binary blobs, via
#                                     fixture_profile.cpp/mdef.cpp)
#   test_*.cpp                      (host test harness)
#
# -fno-exceptions -fno-rtti are enforced here to match the host build.
#
# F0 scope: register the EXISTING runtime modules only, so the toolchain
# (C++ flags, PSRAM, component wrapping) is de-risked before any device driver
# is written. Later phases append new sources to this list:
#   F1: render_pacing.cpp, dmx_sink.cpp (already listed? no — added in F1)
#   F3: apply_loaded_show.cpp
#   F4: web_protocol.cpp, osc_parser.cpp (host-tested parsers/protocol) +
#       web_input.cpp, midi_input.cpp, osc_input.cpp (the device transports
#       that call them -- real esp_http_server WS endpoint, MIDI UART,
#       OSC UDP; untestable without hardware, same status as every other
#       device-only file here, but no longer TODO stubs)
#   F6: the Lua/Fennel live-coding layer (see README_LUA_FENNEL.md), plus
#       live_control.cpp/control_queue*.cpp pulled forward from F4: cues
#       need a real ShowController, and ShowController needs a real
#       LiveControl + control-event queue feeding it for Lua, the web
#       console, and MIDI/OSC to converge on the same controller (see
#       main.cpp).

set(GLOW_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)

idf_component_register(
    SRCS
        # geometry / math
        ${GLOW_ROOT}/vec_math.cpp
        ${GLOW_ROOT}/aim.cpp
        # fixture profiles
        ${GLOW_ROOT}/fixture_profile.cpp
        # show engine + effects
        ${GLOW_ROOT}/show.cpp
        ${GLOW_ROOT}/oscillator.cpp
        ${GLOW_ROOT}/color.cpp
        ${GLOW_ROOT}/effects.cpp
        ${GLOW_ROOT}/show_control.cpp
        # pixel matrices
        ${GLOW_ROOT}/pixel_matrix.cpp
        ${GLOW_ROOT}/pixel_patterns.cpp
        # bundle loader
        ${GLOW_ROOT}/show_bundle.cpp
        # CFG1 flash-time device config: host-tested parser (device_config.h)
        # + encoder (device_config_encoder.h -- unlike PFX1/MDF1's text-
        # grammar encoders, this one is useful on-device too: see
        # device_config_web.cpp's GET /devcfg)
        ${GLOW_ROOT}/device_config.cpp
        ${GLOW_ROOT}/device_config_encoder.cpp
        # CFG1 read/write over the web console's existing httpd server --
        # reconfigure without reflashing (#ifdef ESP_PLATFORM guarded; see
        # device_config_web.h)
        ${GLOW_ROOT}/device_config_web.cpp
        # A5/.mdef: MDF1 controller-definition runtime parser (the MIDI
        # twin of fixture_profile.cpp), consumed by show_bundle.cpp's v2
        # controller table, and the LED feedback engine that reads it
        # (host-tested; see mdef.h/led_feedback.h)
        ${GLOW_ROOT}/mdef.cpp
        ${GLOW_ROOT}/led_feedback.cpp
        # F1: host-tested render-pacing math
        ${GLOW_ROOT}/render_pacing.cpp
        # Musical time: PLL-smoothed beat clock (host-tested; the transports
        # that feed it -- MIDI clock, DJ-Link -- are the #ifdef ESP_PLATFORM
        # device files further down) + the BeatEvent queue they push through.
        ${GLOW_ROOT}/beat_clock.cpp
        ${GLOW_ROOT}/beat_queue.cpp
        ${GLOW_ROOT}/beat_queue_freertos.cpp
        # F3: host-tested patch routing from a LoadedShow
        ${GLOW_ROOT}/apply_loaded_show.cpp
        # F5: host-tested safe-blackout core (stop every cue, zero every
        # Raw universe) -- see safe_blackout.h
        ${GLOW_ROOT}/safe_blackout.cpp
        # device-only DMX sink (#ifdef ESP_PLATFORM guarded; filled in F1)
        ${GLOW_ROOT}/dmx_sink.cpp
        # device-only Art-Net sink (#ifdef ESP_PLATFORM guarded; filled in F2)
        ${GLOW_ROOT}/artnet_sink.cpp
        # F6: the Lua/Fennel live-coding layer
        ${GLOW_ROOT}/lua_vm.cpp
        ${GLOW_ROOT}/glow_lua_api.cpp
        ${GLOW_ROOT}/lua_effect.cpp
        ${GLOW_ROOT}/glow_fennel.cpp
        ${GLOW_ROOT}/eval_queue.cpp
        # device-only eval-queue backend (#ifdef ESP_PLATFORM guarded),
        # same status as control_queue_freertos.cpp — see its header)
        ${GLOW_ROOT}/eval_queue_freertos.cpp
        # scripts partition (LittleFS): host-tested scriptNameIsValid +
        # device-only mount/read/save (#ifdef ESP_PLATFORM guarded)
        ${GLOW_ROOT}/scripts_storage.cpp
        # pulled forward from F4: LiveControl + the control-event queue,
        # so glow.cue.*/the web console/MIDI/OSC all drive one
        # ShowController via one queue (see main.cpp's g_controller)
        ${GLOW_ROOT}/live_control.cpp
        ${GLOW_ROOT}/control_queue.cpp
        # device-only FreeRTOS queue backend (#ifdef ESP_PLATFORM guarded;
        # see control_queue_freertos.cpp's header)
        ${GLOW_ROOT}/control_queue_freertos.cpp
        # F4: the testable protocol/parser cores (no ESP-IDF dependency —
        # these already build and run under `make test`)
        ${GLOW_ROOT}/web_protocol.cpp
        ${GLOW_ROOT}/osc_parser.cpp
        # Musical time: MIDI realtime-byte framing (host-tested) + passive
        # Pro DJ Link packet parsing/master-tracking (host-tested; byte
        # offsets verified against Deep Symmetry's dysentery protocol
        # analysis -- see djlink_parser.h's header).
        ${GLOW_ROOT}/midi_realtime.cpp
        ${GLOW_ROOT}/djlink_parser.cpp
        ${GLOW_ROOT}/djlink_master_tracker.cpp
        # F4: the device-only transports that call the above (#ifdef
        # ESP_PLATFORM guarded; see each file's own header for the real
        # esp_http_server/UART/UDP wiring)
        ${GLOW_ROOT}/web_input.cpp
        ${GLOW_ROOT}/midi_input.cpp
        ${GLOW_ROOT}/osc_input.cpp
        ${GLOW_ROOT}/djlink_input.cpp
        # USB-MIDI host: ESP-IDF ships usb_host_lib but no MIDI class
        # driver -- this is that driver (REQUIRES usb below). Gated at
        # runtime behind CONFIG_GLOW_USB_MIDI_HOST (Kconfig.projbuild,
        # default off -- see usb_midi_input.h's hardware note); always
        # compiled in (like every other #ifdef ESP_PLATFORM transport
        # here) so flipping the Kconfig flag needs no CMakeLists change.
        ${GLOW_ROOT}/usb_midi_input.cpp
        # F5: OTA over the existing web console httpd server, with A/B
        # rollback + self-validation (#ifdef ESP_PLATFORM guarded; see
        # ota_manager.h)
        ${GLOW_ROOT}/ota_manager.cpp
    INCLUDE_DIRS
        ${GLOW_ROOT}
        ${GLOW_ROOT}/third_party/lua
    REQUIRES esp_dmx esp_wifi lwip lua usb
    PRIV_REQUIRES littlefs esp_http_server driver app_update
    EMBED_FILES
        # The console bundle web_input.cpp's static_file_handler serves
        # (see its kStaticFiles table) -- EMBED_FILES has to live in
        # *this* component because the _binary_..._start/_end symbols it
        # generates are extern "C" globals in whichever object file
        # embeds them; web_input.cpp is the one declaring/using them, and
        # putting the EMBED_FILES that produce them in a different
        # component (main) than the one consuming them isn't reliable --
        # main REQUIRES glow_core, not the other way around, so nothing
        # guarantees main's archive gets pulled into the link before
        # glow_core's object file's reference is resolved. Symbol naming:
        # EMBED_FILES foo/ws-client.js -> _binary_ws_client_js_start/_end
        # (path dropped, non-alnum chars in the basename -> '_').
        ${GLOW_ROOT}/firmware/main/data/console/index.html
        ${GLOW_ROOT}/firmware/main/data/console/app.js
        ${GLOW_ROOT}/firmware/main/data/console/styles.css
        ${GLOW_ROOT}/firmware/main/data/console/ws-client.js
        ${GLOW_ROOT}/firmware/main/data/console/script-panel.js
        ${GLOW_ROOT}/firmware/main/data/console/shared/fennel-editor.js
        ${GLOW_ROOT}/firmware/main/data/console/vendor/preact.mjs
        ${GLOW_ROOT}/firmware/main/data/console/vendor/preact-hooks.mjs
        ${GLOW_ROOT}/firmware/main/data/console/vendor/htm.mjs
        ${GLOW_ROOT}/firmware/main/data/console/vendor/editor-bundle.mjs
        # CFG1 §6: the reconfigure page -- a plain static form (not part of
        # the Preact bundle above), plus its own copy of the byte-format
        # encoder/decoder (shared/devcfg.js -- same file as
        # web/shared/devcfg.js; see that file's header comment).
        ${GLOW_ROOT}/firmware/main/data/console/devcfg.html
        ${GLOW_ROOT}/firmware/main/data/console/devcfg-page.js
        ${GLOW_ROOT}/firmware/main/data/console/shared/devcfg.js
)

# Match the host build flags: no exceptions, no RTTI, full warnings.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -fno-exceptions
    -fno-rtti
    -Wall
    -Wextra
    -Werror
    -std=gnu++17
)
