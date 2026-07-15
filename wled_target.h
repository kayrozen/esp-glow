// wled_target.h — a named WLED UDP Notifier target.
//
// Declared by the .show file's WLED directive (provision.h's grammar
// comment) and carried through the SHW1 bundle's v3 WLED table
// (show_bundle.h) into the runtime WledManager (wled_manager.h), which
// resolves glow.wled.* calls (glow_lua_api.h) by name.
#pragma once

#include <cstdint>
#include <string>

inline constexpr uint16_t WLED_DEFAULT_PORT = 21324;  // WLED UDP notifier port

struct WledTarget {
  std::string name;                        // logical name, unique within the show
  std::string ip;                          // IPv4 dotted-quad, mDNS hostname, or 255.255.255.255
  uint16_t    port = WLED_DEFAULT_PORT;
  uint8_t     syncGroup = 1;               // WLED sync group (1..8); filtering happens on the
                                            // receiver -- esp-glow addresses targets by name only.
};
