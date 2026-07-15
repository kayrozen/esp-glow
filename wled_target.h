#pragma once
#include <string>
#include <cstdint>

struct WledTarget {
    std::string name;           // Logical name from .show file
    std::string ip;             // IP address or hostname
    uint16_t    port = 21324;   // WLED UDP notifier port
    uint8_t     sync_group = 1; // WLED sync group (1-8)
};
