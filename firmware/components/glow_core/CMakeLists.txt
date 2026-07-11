# `glow_core` ESP-IDF component.
#
# Wraps the host-tested runtime modules (living at the repo root) so they build
# inside the ESP-IDF firmware project. The same .cpp files also build under
# `make test` on the host; device-only transports are guarded by
# `#ifdef ESP_PLATFORM` and compile to nothing on host.
#
# Modules NOT included here (host-only):
#   provision.cpp / profile_encoder.cpp (PC compiler toolchain, provision_main)
#   test_*.cpp                      (host test harness)
#
# -fno-exceptions -fno-rtti are enforced here to match the host build.
#
# F0 scope: register the EXISTING runtime modules only, so the toolchain
# (C++ flags, PSRAM, component wrapping) is de-risked before any device driver
# is written. Later phases append new sources to this list:
#   F1: render_pacing.cpp, dmx_sink.cpp (already listed? no — added in F1)
#   F3: apply_loaded_show.cpp
#   F4: midi_parser.cpp, osc_parser.cpp, web_protocol.cpp, web_input_handler.cpp,
#       live_control.cpp, midi_input.cpp, osc_input.cpp, web_input.cpp

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
        # F1: host-tested render-pacing math
        ${GLOW_ROOT}/render_pacing.cpp
        # device-only DMX sink (#ifdef ESP_PLATFORM guarded; filled in F1)
        ${GLOW_ROOT}/dmx_sink.cpp
        # device-only Art-Net sink (#ifdef ESP_PLATFORM guarded; filled in F2)
        ${GLOW_ROOT}/artnet_sink.cpp
    INCLUDE_DIRS
        ${GLOW_ROOT}
)

# Match the host build flags: no exceptions, no RTTI, full warnings.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -fno-exceptions
    -fno-rtti
    -Wall
    -Wextra
    -Werror
    -std=c++17
)
