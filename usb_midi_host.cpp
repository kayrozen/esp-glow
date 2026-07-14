#ifdef ESP_PLATFORM

//
// USB-MIDI host — device transport.
//
// Implements a USB MIDI class driver over ESP-IDF's usb_host_lib (generic
// USB host stack). Reads USB MIDI Event Packets (4 bytes), strips them to
// raw MIDI bytes, and feeds midi_input_handle_byte() for framing/parsing
// -- identical pipeline to DIN-MIDI. Hot-plug supported: connect/disconnect
// events logged via console; disconnect does not crash the rig.
//
// References:
// - ESP-IDF usb_host_lib: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/usb_host.html
// - USB MIDI Class Spec: https://www.usb.org/sites/default/files/midi10.pdf
// - ESP-IDF example PR (not yet merged as of v5.x): search for "usb/host MIDI class example"
// - Community implementations: various GitHub repos for ESP32-S3 USB-MIDI host
//
// Limitations:
// - One device at a time (ESP-IDF usb_host_lib limitation)
// - No MIDI 2.0 / MPE (out of scope per spec)
// - Task runs on core 0 with WiFi; may cause dropped frames under load
//
// Hardware requirement: board must supply 5V VBUS to USB-A receptacle
// with adequate current (few hundred mA) and ideally over-current protection.
//

#include "usb_midi_host.h"

#include "midi_input.h"    // midi_input_handle_byte()
#include "control_queue.h" // IControlEventQueue
#include "beat_queue.h"    // glow::IBeatEventQueue, glow::BeatEvent

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "usb/usb_host.h"

#include <cstdint>
#include <atomic>

static const char* TAG = "usb_midi_host";

// USB-MIDI Event Packet structure (USB spec section 3.2)
// Cable Number (4 bits) | CIN (4 bits) | Data (3 bytes)
// CIN values: 0x8 = Note-On, 0x9 = Note-Off, 0xB = CC, 0xF = Clock, etc.
struct UsbMidiEventPacket {
  uint8_t header;   // [7:4] Cable Number, [3:0] Code Index Number (CIN)
  uint8_t data[3];  // MIDI bytes (status + data1 + data2)
};

static IControlEventQueue* g_queue = nullptr;
static glow::IBeatEventQueue* g_beatQueue = nullptr;
static std::atomic<bool> g_connected{false};
static std::atomic<uint16_t> g_vid{0};
static std::atomic<uint16_t> g_pid{0};

static usb_host_client_handle_t g_clientHandle = nullptr;
static usb_dev_handle_t g_devHandle = nullptr;
static usb_interface_claim_info_t g_interfaceClaim = {};
static bool g_interfaceClaimed = false;

// Event group bits
static constexpr int USB_EVENT_CONNECTED = BIT0;
static constexpr int USB_EVENT_DISCONNECTED = BIT1;
static constexpr int USB_EVENT_STOP = BIT2;

static EventGroupHandle_t g_eventGroup = nullptr;

// Forward declarations
static void usb_midi_release_interface(void);

// Callback: USB host client event (device connect/disconnect)
static bool usb_midi_client_event_callback(const usb_host_client_event_msg_t* event_msg, void* arg) {
  (void)arg;
  
  switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
      ESP_LOGI(TAG, "USB device connected (address=%d)", event_msg->new_dev.address);
      // Signal the task to enumerate
      if (g_eventGroup) {
        xEventGroupSetBits(g_eventGroup, USB_EVENT_CONNECTED);
      }
      break;
      
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      ESP_LOGI(TAG, "USB device disconnected");
      g_connected.store(false);
      g_vid.store(0);
      g_pid.store(0);
      if (g_eventGroup) {
        xEventGroupSetBits(g_eventGroup, USB_EVENT_DISCONNECTED);
      }
      break;
      
    default:
      break;
  }
  
  return false; // Don't need wake-up from ISR
}

// Check if an interface is a MIDI class interface
static bool is_midi_interface(const usb_intf_desc_t* intf) {
  // USB Audio/MIDI class: bInterfaceClass = 0x01 (Audio), bInterfaceSubClass = 0x03 (MIDI Streaming)
  // Or some devices use bInterfaceClass = 0xFF (Vendor-specific) with MIDI descriptors
  if (intf->bInterfaceClass == 0x01 && intf->bInterfaceSubClass == 0x03) {
    return true;
  }
  // Some MIDI devices report class 0xFF, check endpoints
  // For now, accept Audio class with MIDI subclass
  return false;
}

// Find and claim the MIDI interface on a newly connected device
static esp_err_t usb_midi_enumerate_and_claim(void) {
  if (!g_devHandle) {
    return ESP_ERR_INVALID_STATE;
  }
  
  // Get device info
  usb_device_info_t devInfo;
  esp_err_t err = usb_host_get_device_info(g_devHandle, &devInfo);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get device info: %s", esp_err_to_name(err));
    return err;
  }
  
  g_vid.store(devInfo.descriptor.idVendor);
  g_pid.store(devInfo.descriptor.idProduct);
  ESP_LOGI(TAG, "Device VID=0x%04X PID=0x%04X", g_vid.load(), g_pid.load());
  
  // Find MIDI interface
  usb_config_desc_t* configDesc = nullptr;
  err = usb_host_get_active_config_descriptor(g_devHandle, &configDesc);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get config descriptor: %s", esp_err_to_name(err));
    return err;
  }
  
  int midiInterfaceIdx = -1;
  for (int i = 0; i < configDesc->bNumInterfaces; i++) {
    const usb_intf_desc_t* intf = usb_host_get_interface_desc(configDesc, i, 0);
    if (intf && is_midi_interface(intf)) {
      midiInterfaceIdx = i;
      ESP_LOGI(TAG, "Found MIDI interface at index %d", i);
      break;
    }
  }
  
  if (midiInterfaceIdx < 0) {
    ESP_LOGW(TAG, "No MIDI interface found on this device");
    return ESP_ERR_NOT_FOUND;
  }
  
  // Claim the interface
  g_interfaceClaim.interface_num = configDesc->interface[midiInterfaceIdx].altsetting[0].bInterfaceNumber;
  g_interfaceClaim.alt_setting = 0;
  
  err = usb_host_interface_claim(g_clientHandle, g_devHandle, &g_interfaceClaim, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to claim interface %d: %s", g_interfaceClaim.interface_num, esp_err_to_name(err));
    return err;
  }
  
  g_interfaceClaimed = true;
  g_connected.store(true);
  ESP_LOGI(TAG, "MIDI interface claimed successfully");
  
  return ESP_OK;
}

static void usb_midi_release_interface(void) {
  if (g_interfaceClaimed && g_clientHandle) {
    usb_host_interface_release(g_clientHandle, &g_interfaceClaim);
    g_interfaceClaimed = false;
    ESP_LOGI(TAG, "MIDI interface released");
  }
  
  if (g_devHandle) {
    usb_host_device_close(g_clientHandle, g_devHandle);
    g_devHandle = nullptr;
  }
  
  g_connected.store(false);
  g_vid.store(0);
  g_pid.store(0);
}

// Parse a USB MIDI Event Packet and feed bytes to the MIDI input handler
static void usb_midi_process_packet(const UsbMidiEventPacket* packet) {
  uint8_t cin = packet->header & 0x0F;
  
  // CIN determines how many data bytes are valid
  // 0x8 = Note-On (3 bytes), 0x9 = Note-Off (3 bytes), 0xB = CC (3 bytes)
  // 0xF = Clock (1 byte), etc.
  int byteCount = 0;
  
  switch (cin) {
    case 0x0: // Miscellaneous
    case 0x1: // Cable Event
    case 0x4: // SysEx ends
    case 0x5: // SysEx ends with 1 byte
    case 0x6: // SysEx ends with 2 bytes
    case 0x7: // SysEx ends with 3 bytes
      byteCount = 3;
      break;
    case 0x2: // SysEx starts
    case 0x3: // SysEx continues
    case 0x8: // Note-On
    case 0x9: // Note-Off
    case 0xA: // Poly Aftertouch
    case 0xB: // Control Change
    case 0xE: // Pitch Bend
      byteCount = 3;
      break;
    case 0xC: // Program Change
    case 0xD: // Channel Aftertouch
      byteCount = 2;
      break;
    case 0xF: // Single byte (Realtime messages like Clock 0xF8)
      byteCount = 1;
      break;
    default:
      byteCount = 3;
      break;
  }
  
  // Feed each valid byte to the MIDI input handler
  for (int i = 0; i < byteCount && i < 3; i++) {
    if (packet->data[i] != 0) { // Skip padding zeros
      midi_input_handle_byte(packet->data[i]);
    }
  }
}

// Read bulk IN endpoint for MIDI data
static void usb_midi_read_task(void) {
  if (!g_interfaceClaimed || !g_devHandle) {
    return;
  }
  
  // Find the bulk IN endpoint
  usb_config_desc_t* configDesc = nullptr;
  if (usb_host_get_active_config_descriptor(g_devHandle, &configDesc) != ESP_OK) {
    return;
  }
  
  const usb_ep_desc_t* bulkInEp = nullptr;
  for (int epIdx = 0; epIdx < configDesc->interface[g_interfaceClaim.interface_num].altsetting[0].bNumEndpoints; epIdx++) {
    const usb_ep_desc_t* ep = usb_host_get_endpoint_desc(configDesc, g_interfaceClaim.interface_num, 0, epIdx);
    if (ep && (ep->bEndpointAddress & 0x80) && ((ep->bmAttributes & 0x03) == 0x02)) {
      // Bulk IN endpoint
      bulkInEp = ep;
      break;
    }
  }
  
  if (!bulkInEp) {
    ESP_LOGE(TAG, "No bulk IN endpoint found");
    return;
  }
  
  ESP_LOGI(TAG, "Starting MIDI read on endpoint 0x%02X", bulkInEp->bEndpointAddress);
  
  // Allocate transfer
  usb_transfer_t* transfer = nullptr;
  esp_err_t err = usb_host_alloc_transfer(&transfer, 0, bulkInEp->wMaxPacketSize, pdMS_TO_TICKS(1000));
  if (err != ESP_OK || !transfer) {
    ESP_LOGE(TAG, "Failed to allocate transfer: %s", esp_err_to_name(err));
    return;
  }
  
  while (g_connected.load() && g_interfaceClaimed) {
    // Submit bulk IN transfer
    transfer->timeout_ms = 100;
    err = usb_host_endpoint_transfer(g_devHandle, bulkInEp->bEndpointAddress, transfer);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Endpoint transfer failed: %s", esp_err_to_name(err));
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    
    // Wait for completion
    if (usb_host_transfer_wait(transfer, pdMS_TO_TICKS(200)) == ESP_OK) {
      if (transfer->actual_len > 0) {
        // Process USB MIDI Event Packets (4 bytes each)
        for (size_t i = 0; i + 3 < transfer->actual_len; i += 4) {
          UsbMidiEventPacket packet;
          packet.header = transfer->data_buffer[i];
          packet.data[0] = transfer->data_buffer[i + 1];
          packet.data[1] = transfer->data_buffer[i + 2];
          packet.data[2] = transfer->data_buffer[i + 3];
          usb_midi_process_packet(&packet);
        }
      }
    } else {
      // Timeout or error
      usb_host_endpoint_halt(g_devHandle, bulkInEp->bEndpointAddress);
      usb_host_endpoint_clear(g_devHandle, bulkInEp->bEndpointAddress);
    }
  }
  
  usb_host_free_transfer(transfer);
  ESP_LOGI(TAG, "MIDI read task stopped");
}

void usb_midi_host_init(IControlEventQueue& queue, glow::IBeatEventQueue* beatQueue) {
  g_queue = &queue;
  g_beatQueue = beatQueue;
  g_connected.store(false);
  g_vid.store(0);
  g_pid.store(0);
  g_eventGroup = xEventGroupCreate();
  
  ESP_LOGI(TAG, "USB-MIDI host initialized");
}

void usb_midi_host_deinit(void) {
  // Signal stop
  if (g_eventGroup) {
    xEventGroupSetBits(g_eventGroup, USB_EVENT_STOP);
  }
  
  // Release interface
  usb_midi_release_interface();
  
  // Deregister client
  if (g_clientHandle) {
    usb_host_client_deregister(g_clientHandle);
    g_clientHandle = nullptr;
  }
  
  // Free event group
  if (g_eventGroup) {
    vEventGroupDelete(g_eventGroup);
    g_eventGroup = nullptr;
  }
  
  ESP_LOGI(TAG, "USB-MIDI host deinitialized");
}

bool usb_midi_is_connected(void) {
  return g_connected.load();
}

void usb_midi_get_device_ids(uint16_t* vid, uint16_t* pid) {
  if (vid) *vid = g_vid.load();
  if (pid) *pid = g_pid.load();
}

void usb_midi_host_task(void* ctx) {
  (void)ctx;
  
  ESP_LOGI(TAG, "USB-MIDI host task started");
  
  // Initialize USB host library
  usb_host_config_t hostConfig = {
    .skip_phy_setup = false,
    .intr_flags = ESP_INTR_FLAG_LEVEL1,
  };
  
  esp_err_t err = usb_host_install(&hostConfig);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to install USB host: %s", esp_err_to_name(err));
    vTaskDelete(nullptr);
    return;
  }
  
  // Register client
  usb_host_client_handle_t clientHandle = nullptr;
  usb_host_client_config_t clientConfig = {
    .is_synchronous = false,
    .max_num_event_callbacks = 2,
    .async = {
      .client_event_callback = usb_midi_client_event_callback,
      .callback_arg = nullptr,
    },
  };
  
  err = usb_host_client_register(&clientConfig, &clientHandle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register client: %s", esp_err_to_name(err));
    usb_host_uninstall();
    vTaskDelete(nullptr);
    return;
  }
  
  g_clientHandle = clientHandle;
  
  // Main event loop
  while (true) {
    // Wait for events
    EventBits_t bits = xEventGroupWaitBits(g_eventGroup, 
                                           USB_EVENT_CONNECTED | USB_EVENT_DISCONNECTED | USB_EVENT_STOP,
                                           pdTRUE,
                                           pdFALSE,
                                           portMAX_DELAY);
    
    if (bits & USB_EVENT_STOP) {
      ESP_LOGI(TAG, "Stop signal received, shutting down");
      break;
    }
    
    if (bits & USB_EVENT_CONNECTED) {
      // Try to open the new device
      uint8_t devAddr = 0;
      err = usb_host_get_new_device_address(g_clientHandle, &devAddr);
      if (err == ESP_OK && devAddr > 0) {
        err = usb_host_device_open(g_clientHandle, devAddr, &g_devHandle);
        if (err == ESP_OK) {
          ESP_LOGI(TAG, "Device opened, attempting to claim MIDI interface");
          err = usb_midi_enumerate_and_claim();
          if (err == ESP_OK) {
            ESP_LOGI(TAG, "MIDI interface claimed, starting read loop");
            // Run the read loop until disconnect
            usb_midi_read_task();
          } else {
            usb_midi_release_interface();
          }
        } else {
          ESP_LOGE(TAG, "Failed to open device: %s", esp_err_to_name(err));
        }
      }
    }
    
    if (bits & USB_EVENT_DISCONNECTED) {
      ESP_LOGI(TAG, "Device disconnected, releasing interface");
      usb_midi_release_interface();
    }
    
    // Let the USB host library process events
    usb_host_client_handle_events(g_clientHandle, pdMS_TO_TICKS(10));
  }
  
  // Cleanup
  usb_midi_release_interface();
  usb_host_client_deregister(g_clientHandle);
  usb_host_uninstall();
  
  ESP_LOGI(TAG, "USB-MIDI host task stopped");
  vTaskDelete(nullptr);
}

#else // !ESP_PLATFORM - Host stub for testing

// Host stub implementation for unit testing
// In a real host test environment, these would be no-ops or mock implementations

#include "usb_midi_host.h"
#include "control_queue.h"
#include "beat_queue.h"
#include <cstdio>

static IControlEventQueue* g_queue_stub = nullptr;
static glow::IBeatEventQueue* g_beatQueue_stub = nullptr;
static bool g_connected_stub = false;
static uint16_t g_vid_stub = 0;
static uint16_t g_pid_stub = 0;

void usb_midi_host_init(IControlEventQueue& queue, glow::IBeatEventQueue* beatQueue) {
  g_queue_stub = &queue;
  g_beatQueue_stub = beatQueue;
  g_connected_stub = false;
  g_vid_stub = 0;
  g_pid_stub = 0;
  printf("[usb_midi_host] Initialized (host stub)\n");
}

void usb_midi_host_deinit(void) {
  g_connected_stub = false;
  g_vid_stub = 0;
  g_pid_stub = 0;
  printf("[usb_midi_host] Deinitialized (host stub)\n");
}

void usb_midi_host_task(void* ctx) {
  (void)ctx;
  printf("[usb_midi_host] Task started (host stub - would block on ESP32)\n");
  // In host tests, this function should not be called or should be mocked
}

bool usb_midi_is_connected(void) {
  return g_connected_stub;
}

void usb_midi_get_device_ids(uint16_t* vid, uint16_t* pid) {
  if (vid) *vid = g_vid_stub;
  if (pid) *pid = g_pid_stub;
}

// Mock function for tests to simulate connection
void usb_midi_mock_connect(uint16_t vid, uint16_t pid) {
  g_connected_stub = true;
  g_vid_stub = vid;
  g_pid_stub = pid;
  printf("[usb_midi_host] Mock connected: VID=0x%04X PID=0x%04X\n", vid, pid);
}

void usb_midi_mock_disconnect(void) {
  g_connected_stub = false;
  g_vid_stub = 0;
  g_pid_stub = 0;
  printf("[usb_midi_host] Mock disconnected\n");
}

#endif // ESP_PLATFORM
