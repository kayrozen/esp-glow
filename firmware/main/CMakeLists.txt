# firmware/main — ESP-IDF application entry.
#
# app_main() lives in main.cpp. It is grown phase by phase (see README_FIRMWARE):
#   F0: banner + LED blink
#   F1: + DMX sink + render task (hardcoded patch)
#   F2: + WiFi + Art-Net sink (matrices)
#   F3: + LittleFS + load SHW1 bundle via applyLoadedShow
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
)

# Pull in the host-tested lighting core as a component dependency.
idf_component_requires(PRIVATE glow_core)

# Pack the LittleFS partition image at build time. The data/ directory ships:
#   - show.shw1     the demo SHW1 bundle (replace with your own; see samples/)
#   - console/      the Preact web console (F4)
# FLASH_IN_PROJECT makes `idf.py flash` write the image alongside the firmware.
littlefs_create_partition_image(littlefs data FLASH_IN_PROJECT)
