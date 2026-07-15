#include "wled_manager.h"
#include <cstring>
#include <cstdio>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#else
// Host stub logging macros
#define ESP_LOGI(tag, ...) printf("[%s] " __VA_ARGS__ "\n", tag)
#define ESP_LOGW(tag, ...) printf("[%s] " __VA_ARGS__ "\n", tag)
#define ESP_LOGE(tag, ...) printf("[%s] " __VA_ARGS__ "\n", tag)
#define ESP_LOGD(tag, ...) // Debug disabled by default on host
#define ESP_LOGV(tag, ...) // Verbose disabled by default on host
#endif

static const char* TAG = "WLED";

void WledManager::init() {
#ifdef ESP_PLATFORM
    udp_socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_socket_ < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket: errno %d", errno);
        return;
    }

    int broadcast = 1;
    if (setsockopt(udp_socket_, SOL_SOCKET, SO_BROADCAST, 
                   &broadcast, sizeof(broadcast)) < 0) {
        ESP_LOGW(TAG, "Failed to enable broadcast: errno %d", errno);
    }

    // Set non-blocking (optional but recommended)
    int flags = fcntl(udp_socket_, F_GETFL, 0);
    fcntl(udp_socket_, F_SETFL, flags | O_NONBLOCK);

    ESP_LOGI(TAG, "WLED UDP manager initialized (socket fd=%d)", udp_socket_);
#else
    // Host stub - no actual socket
    udp_socket_ = -1;
    ESP_LOGI(TAG, "WLED UDP manager initialized (host mode, no socket)");
#endif
}

void WledManager::shutdown() {
#ifdef ESP_PLATFORM
    if (udp_socket_ >= 0) {
        close(udp_socket_);
        udp_socket_ = -1;
    }
#endif
}

void WledManager::addTarget(const std::string& name, const std::string& ip,
                            uint16_t port, uint8_t sync_group) {
    targets_.push_back({name, ip, port, sync_group});
    ESP_LOGI(TAG, "Added target '%s' -> %s:%d (group %d)", 
             name.c_str(), ip.c_str(), port, sync_group);
}

const WledTarget* WledManager::getTarget(const std::string& name) const {
    for (const auto& t : targets_) {
        if (t.name == name) return &t;
    }
    return nullptr;
}

void WledManager::buildPacket(uint8_t effect, uint8_t speed, uint8_t intensity,
                              uint8_t brightness, uint8_t r, uint8_t g, uint8_t b,
                              uint8_t palette, uint16_t transition_ms, uint8_t callMode) {
    // Zero entire packet first
    memset(packet_, 0, sizeof(packet_));

    packet_[0]  = 0x00;                    // Notifier protocol
    packet_[1]  = callMode;                // Why we are sending
    packet_[2]  = brightness;              // Master brightness
    packet_[3]  = r;                       // Primary R
    packet_[4]  = g;                       // Primary G
    packet_[5]  = b;                       // Primary B
    packet_[6]  = 0;                       // nightlightActive
    packet_[7]  = 0;                       // nightlightDelayMins
    packet_[8]  = effect;                  // effectCurrent
    packet_[9]  = speed;                   // effectSpeed
    packet_[10] = 0;                       // white (RGBW strips)
    packet_[11] = 0x05;                    // Protocol version 5
    packet_[12] = 0;                       // colSec[0]
    packet_[13] = 0;                       // colSec[1]
    packet_[14] = 0;                       // colSec[2]
    packet_[15] = 0;                       // whiteSec
    packet_[16] = intensity;               // effectIntensity
    packet_[17] = (transition_ms >> 8) & 0xFF;  // Big-endian MSB
    packet_[18] = transition_ms & 0xFF;         // Big-endian LSB
    packet_[19] = palette;                 // effectPalette
    // bytes 20-23 already zeroed
}

void WledManager::sendTo(const std::string& ip, uint16_t port) {
#ifdef ESP_PLATFORM
    if (udp_socket_ < 0) return;

    struct sockaddr_in dest_addr{};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);

    if (inet_aton(ip.c_str(), &dest_addr.sin_addr) == 0) {
        ESP_LOGW(TAG, "Invalid IP address: %s", ip.c_str());
        return;
    }

    ssize_t sent = sendto(udp_socket_, packet_, sizeof(packet_), 0,
                          (struct sockaddr*)&dest_addr, sizeof(dest_addr));

    if (sent < 0) {
        // Non-fatal: UDP is best-effort. Log and continue.
        ESP_LOGD(TAG, "UDP send failed to %s:%d (errno %d)", 
                 ip.c_str(), port, errno);
    } else {
        ESP_LOGV(TAG, "Sent %zd bytes to %s:%d", sent, ip.c_str(), port);
    }
#else
    // Host stub - just log what would be sent
    ESP_LOGI(TAG, "[HOST MODE] Would send 24 bytes to %s:%d", ip.c_str(), port);
#endif
}

void WledManager::setEffect(const std::string& name,
                            const std::string& effect_name,
                            uint8_t speed, uint8_t intensity,
                            uint8_t brightness,
                            const std::string& palette_name,
                            uint16_t transition_ms) {
    const auto* target = getTarget(name);
    if (!target) {
        ESP_LOGW(TAG, "Unknown WLED target: '%s'", name.c_str());
        return;
    }

    uint8_t fx_id = wled::effectId(effect_name);
    uint8_t pal_id = wled::paletteId(palette_name);

    if (fx_id == 0 && effect_name != "solid") {
        ESP_LOGW(TAG, "Unknown effect '%s', using solid (0)", effect_name.c_str());
    }
    if (pal_id == 0 && palette_name != "default") {
        ESP_LOGW(TAG, "Unknown palette '%s', using default (0)", palette_name.c_str());
    }

    buildPacket(fx_id, speed, intensity, brightness, 0, 0, 0, pal_id, transition_ms, 0x06);
    sendTo(target->ip, target->port);
}

void WledManager::setSolidColor(const std::string& name,
                                uint8_t r, uint8_t g, uint8_t b,
                                uint8_t brightness,
                                uint16_t transition_ms) {
    const auto* target = getTarget(name);
    if (!target) {
        ESP_LOGW(TAG, "Unknown WLED target: '%s'", name.c_str());
        return;
    }

    // Effect 0 = Solid, callMode 0x01 = direct color change
    buildPacket(0, 0, 0, brightness, r, g, b, 0, transition_ms, 0x01);
    sendTo(target->ip, target->port);
}

void WledManager::setPower(const std::string& name, bool on) {
    const auto* target = getTarget(name);
    if (!target) {
        ESP_LOGW(TAG, "Unknown WLED target: '%s'", name.c_str());
        return;
    }

    // Brightness 0 = off, 255 = on (effect solid, no color change)
    buildPacket(0, 0, 0, on ? 255 : 0, 0, 0, 0, 0, 0, 0x01);
    sendTo(target->ip, target->port);
}

void WledManager::broadcastEffect(const std::string& effect_name,
                                  uint8_t speed, uint8_t intensity,
                                  uint8_t brightness,
                                  const std::string& palette_name) {
    uint8_t fx_id = wled::effectId(effect_name);
    uint8_t pal_id = wled::paletteId(palette_name);

    buildPacket(fx_id, speed, intensity, brightness, 0, 0, 0, pal_id, 0, 0x06);
    sendTo("255.255.255.255", 21324);

    ESP_LOGI(TAG, "Broadcast effect '%s' palette '%s' to all WLED devices",
             effect_name.c_str(), palette_name.c_str());
}
