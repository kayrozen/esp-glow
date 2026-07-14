#pragma once

#include <cstddef>
#include <cstdint>

class IControlEventQueue;
namespace glow {
class IBeatEventQueue;
}

//
// USB-MIDI host transport — device-only (ESP32-S3 USB OTG).
//
// Uses ESP-IDF's usb_host_lib (generic USB host stack) to enumerate a
// connected USB-MIDI device, claim its interface, and read MIDI Event
// Packets (4 bytes: Cable Number + CIN + 3 MIDI bytes). Strips to raw
// MIDI bytes and feeds midi_input_handle_byte() (midi_input.h) for
// framing/parsing -- same pipeline as DIN-MIDI. Hot-plug supported:
// connect/disconnect events reported via console; disconnect does not
// crash the rig.
//
// Limitations (ESP-IDF usb_host_lib as of IDF v5.x):
// - One device at a time (fine for one controller)
// - No MIDI 2.0 / MPE (out of scope per spec)
// - Task runs on core 0 with WiFi; watch for dropped frames under load
//
// Hardware requirement: board must supply 5V VBUS to the USB-A receptacle
// with adequate current (few hundred mA) and ideally over-current protection.
//

// Initialize the USB-MIDI host layer: sets up the USB host library,
// registers callbacks for device connect/disconnect, and prepares to
// enumerate a MIDI device. Must be called before usb_midi_host_task starts.
// beatQueue may be nullptr to disable MIDI clock -> BeatEvent forwarding.
void usb_midi_host_init(IControlEventQueue& queue, glow::IBeatEventQueue* beatQueue);

// Deinitialize USB-MIDI host, release all resources. Safe to call even if
// no device is connected or if the task is running (will stop it).
void usb_midi_host_deinit(void);

// FreeRTOS task entry point: initializes the USB host library, installs
// event callbacks, then loops waiting for USB events (connect/disconnect).
// On connect, claims the MIDI interface and reads bulk IN packets, feeding
// each MIDI byte to midi_input_handle_byte(). On disconnect, releases the
// interface and waits for the next device. Never returns unless explicitly
// stopped via usb_midi_host_deinit().
void usb_midi_host_task(void* ctx);

// Check if a USB-MIDI device is currently connected and active.
bool usb_midi_is_connected(void);

// Get the vendor/product ID of the connected device (or 0/0 if none).
void usb_midi_get_device_ids(uint16_t* vid, uint16_t* pid);

#ifdef __cplusplus
extern "C" {
#endif

// Mock functions for host testing (non-ESP32 platforms only)
#ifndef ESP_PLATFORM
void usb_midi_mock_connect(uint16_t vid, uint16_t pid);
void usb_midi_mock_disconnect(void);
#endif

#ifdef __cplusplus
}
#endif
