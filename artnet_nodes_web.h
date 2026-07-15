// artnet_nodes_web.h — Wave 3 Phase 3: GET /artnet_nodes, a JSON list of
// currently-discovered Art-Net nodes (name, IP, wire universes) for the web
// console -- "why is nothing lighting up" -> "ah, the node isn't on the
// network." Reads artnet_discovery_task's thread-safe snapshot; registers
// onto the same httpd server web_input.cpp already runs (same pattern as
// ota_manager.h/device_config_web.h).
#pragma once

#ifdef ESP_PLATFORM

#ifdef __cplusplus
extern "C" {
#endif

// Registers GET /artnet_nodes on the already-running httpd server. `server`
// is actually an httpd_handle_t (see device_config_web.h's header comment
// for why it's typed void* here). Call after httpd_start.
void artnet_nodes_web_register_handlers(void* server);

#ifdef __cplusplus
}
#endif

#endif  // ESP_PLATFORM
