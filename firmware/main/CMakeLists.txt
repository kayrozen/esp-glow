# firmware/main — ESP-IDF application entry.
#
# app_main() lives in main.cpp. It is grown phase by phase (see README_FIRMWARE):
#   F0: banner + LED blink
#   F1: + DMX sink + render task (hardcoded patch)
#   F2: + WiFi + Art-Net sink (matrices)
#   F3: + raw "show" partition + load SHW1 bundle via applyLoadedShow
#   F4: + web/MIDI/OSC inputs + console serving
#   F5: + OTA + watchdog + safe-blackout fallback

idf_component_register(
    SRCS
        main.cpp
        led_status.cpp
        render_task.cpp
        wifi_manager.cpp
        storage_manager.cpp
    INCLUDE_DIRS
        "."
    REQUIRES glow_core
    EMBED_FILES
        "data/console/index.html"
)

# Write the demo SHW1 bundle straight into the raw "show" partition (see
# partitions.csv) at build time. This is a plain esptool_py flash target,
# not a filesystem image: the same partition is what the browser-based web
# flasher (esptool-js) writes a user-compiled bundle into, so there is no
# filesystem step in the loop on either side.
#
# data/show.shw1 is the demo SHW1 bundle (replace with your own; see
# samples/). It's optional at configure time — if absent, the "show"
# partition is left blank and the device falls back to its hardcoded patch
# at boot (see main.cpp). esptool_py_flash_to_partition() records the
# binary's path relative to the build dir, so it's copied into the build
# dir first: that keeps `firmware/build/*.shw1` a valid glob for CI
# artifact upload, and keeps flasher_args.json's path flat (no "../") so
# the web flasher's flat `firmware/<file>` fetch works unmodified.
set(SHOW_BUNDLE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/data/show.shw1")
if(EXISTS "${SHOW_BUNDLE_SRC}")
    set(SHOW_BUNDLE_BUILD_COPY "${CMAKE_BINARY_DIR}/show.shw1")
    configure_file("${SHOW_BUNDLE_SRC}" "${SHOW_BUNDLE_BUILD_COPY}" COPYONLY)
    esptool_py_flash_to_partition(flash "show" "${SHOW_BUNDLE_BUILD_COPY}")
endif()
