# ESP-IDF project for esp-glow on ESP32-S3.
#
# Build:   idf.py build
# Flash:   idf.py flash monitor
#
# The runtime lighting modules live at the repo root and are built as the
# `glow_core` component (see components/glow_core/CMakeLists.txt). Device-only
# transports (dmx_sink / artnet_sink / midi_input / osc_input / web_input) are
# guarded by `#ifdef ESP_PLATFORM` so the same sources compile clean on host
# (`make test`) and on the device.
#
# Target: ESP32-S3 with octal PSRAM. See sdkconfig.defaults for the pin/PSRAM
# assumptions; adjust sdkconfig for your specific board.

# Minimum ESP-IDF that supports the component manager + octal PSRAM.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp-glow)
