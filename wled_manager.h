#pragma once
#include "wled_target.h"
#include "wled_effect_map.h"
#include <vector>
#include <string>
#include <cstdint>

#ifdef ESP_PLATFORM
#include <lwip/sockets.h>
#include <arpa/inet.h>
#include <fcntl.h>
#endif

class WledManager {
public:
    // Lifecycle
    void init();
    void shutdown();

    // Target management (populated by show_compiler)
    void addTarget(const std::string& name, const std::string& ip, 
                   uint16_t port = 21324, uint8_t sync_group = 1);
    const WledTarget* getTarget(const std::string& name) const;

    // --- Synchronous, non-blocking UDP sends ---
    // All methods return immediately. Packet is on the wire in <1ms.
    // No heap allocation. No background task.

    // Set effect with full parameters
    void setEffect(const std::string& name, 
                   const std::string& effect_name,
                   uint8_t speed = 128,
                   uint8_t intensity = 128,
                   uint8_t brightness = 255,
                   const std::string& palette_name = "default",
                   uint16_t transition_ms = 0);

    // Set solid color (effect = 0, speed/intensity ignored)
    void setSolidColor(const std::string& name,
                       uint8_t r, uint8_t g, uint8_t b,
                       uint8_t brightness = 255,
                       uint16_t transition_ms = 0);

    // Power on/off
    void setPower(const std::string& name, bool on);

    // Broadcast to all WLED devices on network (sync group filtering happens on receiver)
    void broadcastEffect(const std::string& effect_name,
                         uint8_t speed = 128,
                         uint8_t intensity = 128,
                         uint8_t brightness = 255,
                         const std::string& palette_name = "default");

private:
#ifdef ESP_PLATFORM
    int udp_socket_ = -1;
#else
    int udp_socket_ = -1;  // Stub for host testing
#endif
    std::vector<WledTarget> targets_;

    // Pre-allocated, aligned packet buffer. Never allocates at send time.
    alignas(4) uint8_t packet_[24] = {};

    void buildPacket(uint8_t effect, uint8_t speed, uint8_t intensity,
                     uint8_t brightness, uint8_t r, uint8_t g, uint8_t b,
                     uint8_t palette, uint16_t transition_ms, uint8_t callMode);
    void sendTo(const std::string& ip, uint16_t port);
};
