#ifdef ESP_PLATFORM

//
// USB-MIDI host input — device transport. See usb_midi_input.h for the
// scope/hardware note; this file is the ESP-IDF usb_host_lib glue that
// header describes.
//
// ESP-IDF ships usb_host_lib (the generic USB host stack) but no MIDI
// class driver -- everything below the "USB Audio Class parsing" comment
// is that driver, written directly against usb_host.h/usb_helpers.h.
//

#include "usb_midi_input.h"

#include "control_queue.h"        // IControlEventQueue, ControlEvent (transitively)
#include "live_control.h"         // parseMidi
#include "mdef.h"                 // MidiControllerProfile
#include "controller_init.h"      // IRawMidiOutput, sendControllerInit (P1.1)
#include "usb_midi_packetizer.h"  // packUsbMidiEventPackets (P1.1)

#include "usb/usb_host.h"
#include "usb/usb_helpers.h"

#include "esp_intr_alloc.h"  // ESP_INTR_FLAG_LEVEL1
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdint>
#include <cstring>
#include <vector>

static const char* TAG = "usb_midi";

namespace {

// USB Audio Class (bInterfaceClass) / MIDIStreaming subclass
// (bInterfaceSubClass) -- USB Device Class Definition for MIDI Devices
// 1.0, section 2. Not exposed as named constants anywhere in ESP-IDF's USB
// headers (only a handful of classes -- HID, MSC, hub -- get names there).
constexpr uint8_t kUsbClassAudio = 0x01;
constexpr uint8_t kUsbSubclassMidiStreaming = 0x03;

// bEndpointAddress direction bit and bmAttributes transfer-type field
// (USB 2.0 spec table 9-13).
constexpr uint8_t kUsbEndpointDirInMask = 0x80;
constexpr uint8_t kUsbEndpointXferTypeMask = 0x03;
constexpr uint8_t kUsbEndpointXferBulk = 0x02;

// Full-speed bulk endpoints max out at 64 bytes/transaction (USB 2.0 table
// 5-13); ESP32-S3's USB-OTG peripheral is full-speed only (see
// usb_midi_input.h's Limitations note), so this is also the largest single
// IN transfer this driver will ever see completed in one shot. 64 bytes =
// 16 USB-MIDI event packets per transfer.
constexpr size_t kInBufferSize = 64;

enum class State { WaitingForDevice, Connected, Error };

IControlEventQueue* g_queue = nullptr;
const MidiControllerProfile* g_controllerProfile = nullptr;  // P1.1: INIT SYSEX source
usb_host_client_handle_t g_clientHdl = nullptr;
usb_device_handle_t g_devHdl = nullptr;
usb_transfer_t* g_inTransfer = nullptr;
uint8_t g_intfNum = 0;
uint8_t g_inEpAddr = 0;
volatile State g_state = State::WaitingForDevice;

void teardown_device() {
  if (g_inTransfer != nullptr) {
    usb_host_transfer_free(g_inTransfer);
    g_inTransfer = nullptr;
  }
  if (g_devHdl != nullptr) {
    usb_host_interface_release(g_clientHdl, g_devHdl, g_intfNum);
    usb_host_device_close(g_clientHdl, g_devHdl);
    g_devHdl = nullptr;
  }
  g_state = State::WaitingForDevice;
}

// P1.1: fire-and-forget OUT transfer completion -- INIT SYSEX is a one-shot
// send, nothing downstream is waiting on it, so the callback's only job is
// to report a failure and free the transfer (the IN transfer above is
// reused across the device's whole session; this one is not).
void init_out_transfer_cb(usb_transfer_t* transfer) {
  if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
    ESP_LOGW(TAG, "INIT SYSEX OUT transfer finished with status=%d", (int)transfer->status);
  }
  usb_host_transfer_free(transfer);
}

// P1.1: pack every INIT SYSEX blob in `g_controllerProfile` (mdef.h) into
// USB-MIDI event packets and submit one OUT transfer to `outEpAddr`.
// No-op if there's no controller profile, it declared no init blobs, or
// `outEpAddr` is 0 (the device's MIDIStreaming interface has no bulk OUT
// endpoint -- input-only hardware, same no-op as DIN's txGpio < 0).
class UsbInitPacketSink : public IRawMidiOutput {
public:
  void sendRaw(const uint8_t* bytes, size_t len) override {
    packUsbMidiEventPackets(bytes, len, packets);
  }
  std::vector<uint8_t> packets;
};

void send_controller_init_over_usb(usb_device_handle_t dev, uint8_t outEpAddr) {
  if (g_controllerProfile == nullptr || outEpAddr == 0) return;

  UsbInitPacketSink sink;
  sendControllerInit(*g_controllerProfile, sink);  // no-op loop if initCount == 0
  if (sink.packets.empty()) return;

  usb_transfer_t* outTransfer;
  if (usb_host_transfer_alloc(sink.packets.size(), 0, &outTransfer) != ESP_OK) {
    ESP_LOGW(TAG, "INIT SYSEX: OUT transfer_alloc failed; controller init skipped");
    return;
  }
  std::memcpy(outTransfer->data_buffer, sink.packets.data(), sink.packets.size());
  outTransfer->device_handle = dev;
  outTransfer->bEndpointAddress = outEpAddr;
  outTransfer->callback = init_out_transfer_cb;
  outTransfer->context = nullptr;
  outTransfer->num_bytes = sink.packets.size();

  esp_err_t err = usb_host_transfer_submit(outTransfer);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "INIT SYSEX: OUT transfer_submit failed (%d)", (int)err);
    usb_host_transfer_free(outTransfer);
    return;
  }
  ESP_LOGI(TAG, "INIT SYSEX: sent %u blob(s) (%u USB-MIDI packet bytes) to ep=0x%02x",
           (unsigned)g_controllerProfile->initCount, (unsigned)sink.packets.size(), (unsigned)outEpAddr);
}

// Called from usb_host_client_handle_events() (our own task's context, not
// an ISR) whenever the IN transfer completes, errors, or the device goes
// away mid-transfer.
void in_transfer_cb(usb_transfer_t* transfer) {
  if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
    int n = transfer->actual_num_bytes;
    for (int off = 0; off + 4 <= n; off += 4) {
      usb_midi_input_handle_packet(transfer->data_buffer + off);
    }
    esp_err_t err = usb_host_transfer_submit(transfer);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "resubmit IN transfer failed (%d); waiting for next device event", (int)err);
    }
    return;
  }
  if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) {
    // Disconnect: USB_HOST_CLIENT_EVENT_DEV_GONE (below) owns cleanup.
    return;
  }
  ESP_LOGW(TAG, "IN transfer error (status=%d); resubmitting", (int)transfer->status);
  esp_err_t err = usb_host_transfer_submit(transfer);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "resubmit after transfer error failed (%d)", (int)err);
  }
}

// USB Audio Class parsing: walk the device's active configuration for an
// Audio/MIDIStreaming interface with a bulk IN endpoint (required -- this
// transport is input-first) and, on the same interface, a bulk OUT
// endpoint (optional -- P1.1's INIT SYSEX send-on-connect only; see
// usb_midi_input.h's header note on why this never grows into general
// MIDI OUT). Returns true and fills outIntfNum/outInEpAddr on success;
// outOutEpAddr is 0 if the interface has no bulk OUT endpoint at all.
bool find_midistreaming_endpoints(const usb_config_desc_t* cfg, uint8_t& outIntfNum,
                                  uint8_t& outInEpAddr, uint8_t& outOutEpAddr) {
  for (int i = 0; i < cfg->bNumInterfaces; ++i) {
    int offset = 0;
    const usb_intf_desc_t* intf = usb_parse_interface_descriptor(cfg, i, 0, &offset);
    if (intf == nullptr) continue;
    if (intf->bInterfaceClass != kUsbClassAudio || intf->bInterfaceSubClass != kUsbSubclassMidiStreaming) {
      continue;
    }
    uint8_t inEp = 0, outEp = 0;
    for (int e = 0; e < intf->bNumEndpoints; ++e) {
      int epOffset = offset;
      const usb_ep_desc_t* ep = usb_parse_endpoint_descriptor_by_index(intf, e, cfg->wTotalLength, &epOffset);
      if (ep == nullptr) break;
      bool isIn = (ep->bEndpointAddress & kUsbEndpointDirInMask) != 0;
      bool isBulk = (ep->bmAttributes & kUsbEndpointXferTypeMask) == kUsbEndpointXferBulk;
      if (!isBulk) continue;
      if (isIn && inEp == 0) inEp = ep->bEndpointAddress;
      else if (!isIn && outEp == 0) outEp = ep->bEndpointAddress;
    }
    if (inEp != 0) {
      outIntfNum = intf->bInterfaceNumber;
      outInEpAddr = inEp;
      outOutEpAddr = outEp;
      return true;
    }
  }
  return false;
}

void handle_new_device(uint8_t addr) {
  // B1: the host stack (and this driver) supports one device at a time --
  // ignore additional enumerations while one is already claimed rather
  // than silently dropping the first controller's stream.
  if (g_devHdl != nullptr) {
    ESP_LOGW(TAG, "a device is already active; ignoring new device at addr=%d", (int)addr);
    return;
  }

  usb_device_handle_t dev;
  if (usb_host_device_open(g_clientHdl, addr, &dev) != ESP_OK) {
    ESP_LOGE(TAG, "device_open(addr=%d) failed", (int)addr);
    return;
  }

  const usb_config_desc_t* cfg;
  if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "get_active_config_descriptor(addr=%d) failed", (int)addr);
    usb_host_device_close(g_clientHdl, dev);
    return;
  }

  uint8_t intfNum = 0, epAddr = 0, outEpAddr = 0;
  if (!find_midistreaming_endpoints(cfg, intfNum, epAddr, outEpAddr)) {
    ESP_LOGW(TAG, "device addr=%d has no MIDIStreaming bulk-IN interface; ignoring (not a class-compliant USB-MIDI device)", (int)addr);
    usb_host_device_close(g_clientHdl, dev);
    return;
  }

  if (usb_host_interface_claim(g_clientHdl, dev, intfNum, 0) != ESP_OK) {
    ESP_LOGE(TAG, "interface_claim(intf=%d) failed on addr=%d", (int)intfNum, (int)addr);
    usb_host_device_close(g_clientHdl, dev);
    return;
  }

  usb_transfer_t* transfer;
  if (usb_host_transfer_alloc(kInBufferSize, 0, &transfer) != ESP_OK) {
    ESP_LOGE(TAG, "transfer_alloc failed on addr=%d", (int)addr);
    usb_host_interface_release(g_clientHdl, dev, intfNum);
    usb_host_device_close(g_clientHdl, dev);
    return;
  }
  transfer->device_handle = dev;
  transfer->bEndpointAddress = epAddr;
  transfer->callback = in_transfer_cb;
  transfer->context = nullptr;
  transfer->num_bytes = kInBufferSize;

  g_devHdl = dev;
  g_intfNum = intfNum;
  g_inEpAddr = epAddr;
  g_inTransfer = transfer;

  esp_err_t err = usb_host_transfer_submit(transfer);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "initial transfer_submit failed (%d) on addr=%d", (int)err, (int)addr);
    teardown_device();
    return;
  }

  g_state = State::Connected;
  ESP_LOGI(TAG, "device connected: addr=%d intf=%d ep=0x%02x -- streaming USB-MIDI", (int)addr, (int)intfNum, (int)epAddr);

  // P1.1: controller init handshake -- IN streaming is already submitted
  // above, so this can't delay it; a failed/skipped init send never blocks
  // ordinary input. outEpAddr == 0 (no bulk OUT endpoint on this device)
  // or no controller profile wired is a silent no-op, same contract as
  // DIN MIDI OUT's txGpio < 0 (see send_controller_init_over_usb).
  send_controller_init_over_usb(dev, outEpAddr);
}

void handle_device_gone(usb_device_handle_t devHdl) {
  if (devHdl != g_devHdl) return;  // not the device we're tracking; ignore
  ESP_LOGI(TAG, "device disconnected");
  teardown_device();
}

void client_event_cb(const usb_host_client_event_msg_t* eventMsg, void* /*arg*/) {
  switch (eventMsg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
      handle_new_device(eventMsg->new_dev.address);
      break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      handle_device_gone(eventMsg->dev_gone.dev_hdl);
      break;
    default:
      break;
  }
}

// The library's own event-processing loop (control transfers, enumeration
// bookkeeping) needs a task independent of the client task below -- this
// is ESP-IDF's documented two-task usb_host_lib pattern. Pinned to the
// same core as usb_midi_host_task (core 0, with WiFi -- see this file's
// header note on coexistence).
void usb_lib_daemon_task(void* /*ctx*/) {
  while (true) {
    uint32_t eventFlags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &eventFlags);
  }
}

}  // namespace

void usb_midi_input_init(IControlEventQueue& queue, const MidiControllerProfile* controllerProfile) {
  g_queue = &queue;
  g_controllerProfile = controllerProfile;
}

void usb_midi_input_handle_packet(const uint8_t packet[4]) {
  if (g_queue == nullptr) return;
  // USB-MIDI event packets are self-framed: bytes 1..3 are already a
  // complete (zero-padded if shorter) MIDI message -- no running status,
  // no realtime-byte interleaving to resolve (that's midi_realtime.h's job
  // for the DIN byte stream; USB framing makes it moot here). parseMidi
  // handles all seven channel-voice types (including the 2-byte Program
  // Change/Channel Pressure, whose unused 3rd byte here is just the
  // zero-padding) and rejects System/SysEx (byte 0's CIN nibble selects
  // those) -- "strip to raw MIDI bytes, call parseMidi, push. Nothing
  // else" (see usb_midi_input.h).
  ControlEvent ev;
  if (parseMidi(packet + 1, 3, ev)) g_queue->push(ev);
}

void usb_midi_host_task(void* /*ctx*/) {
  usb_host_config_t hostConfig = {};
  hostConfig.intr_flags = ESP_INTR_FLAG_LEVEL1;
  if (usb_host_install(&hostConfig) != ESP_OK) {
    ESP_LOGE(TAG, "usb_host_install failed; USB-MIDI host disabled for this boot");
    g_state = State::Error;
    vTaskDelete(nullptr);
    return;
  }

  usb_host_client_config_t clientConfig = {};
  clientConfig.is_synchronous = false;
  clientConfig.max_num_event_msg = 5;
  clientConfig.async.client_event_callback = client_event_cb;
  clientConfig.async.callback_arg = nullptr;
  if (usb_host_client_register(&clientConfig, &g_clientHdl) != ESP_OK) {
    ESP_LOGE(TAG, "client_register failed; USB-MIDI host disabled for this boot");
    g_state = State::Error;
    vTaskDelete(nullptr);
    return;
  }

  xTaskCreatePinnedToCore(usb_lib_daemon_task, "usb_daemon", 4096 / sizeof(StackType_t),
                          nullptr, 5, nullptr, 0);

  ESP_LOGI(TAG, "host stack up, waiting for a class-compliant USB-MIDI device");

  while (true) {
    // Delivers client_event_cb synchronously on THIS task -- device
    // open/claim/transfer-submit above, and in_transfer_cb's parse+
    // resubmit, all run here, never on the render task (core 1).
    usb_host_client_handle_events(g_clientHdl, portMAX_DELAY);
  }
}

#endif  // ESP_PLATFORM
