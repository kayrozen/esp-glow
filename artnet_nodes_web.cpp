#ifdef ESP_PLATFORM

#include "artnet_nodes_web.h"
#include "artnet_discovery_task.h"

#include "esp_http_server.h"
#include "esp_log.h"

#include <cstdio>
#include <cstring>

static const char* TAG = "artnet_nodes_web";

namespace {

// Appends a JSON string literal (quoted, minimally escaped -- node names
// are short ASCII in practice, but an untrusted reply's name field is not
// trusted to stay that way). Same written-so-far/truncation-detection
// idiom as web_protocol.cpp's appendString.
size_t appendJsonString(char* buf, size_t cap, size_t written, const char* s) {
  auto put = [&](char c) {
    if (written + 1 < cap) buf[written] = c;
    written++;
  };
  put('"');
  for (const char* p = s; *p; ++p) {
    unsigned char c = static_cast<unsigned char>(*p);
    if (c == '"' || c == '\\') {
      put('\\');
      put(static_cast<char>(c));
    } else if (c < 0x20) {
      put('?');  // control byte in a node name -- neutralize, don't propagate
    } else {
      put(static_cast<char>(c));
    }
  }
  put('"');
  return written;
}

size_t appendRaw(char* buf, size_t cap, size_t written, const char* s) {
  size_t n = std::strlen(s);
  if (written + n < cap) std::memcpy(buf + written, s, n);
  return written + n;
}

size_t appendUInt(char* buf, size_t cap, size_t written, unsigned v) {
  char tmp[12];
  std::snprintf(tmp, sizeof(tmp), "%u", v);
  return appendRaw(buf, cap, written, tmp);
}

void ipToDotted(uint32_t ip, char* out, size_t cap) {
  std::snprintf(out, cap, "%u.%u.%u.%u", static_cast<unsigned>((ip >> 24) & 0xFF),
                static_cast<unsigned>((ip >> 16) & 0xFF), static_cast<unsigned>((ip >> 8) & 0xFF),
                static_cast<unsigned>(ip & 0xFF));
}

esp_err_t artnet_nodes_get_handler(httpd_req_t* req) {
  static char buf[2048];
  size_t written = 0;

  written = appendRaw(buf, sizeof(buf), written, "{\"nodes\":[");
  size_t n = artnet_discovery_node_count();
  for (size_t i = 0; i < n; ++i) {
    DiscoveredNode node;
    if (!artnet_discovery_node_at(i, node)) continue;

    if (i > 0) written = appendRaw(buf, sizeof(buf), written, ",");
    written = appendRaw(buf, sizeof(buf), written, "{\"ip\":");
    char ipBuf[24];
    ipToDotted(node.ip, ipBuf, sizeof(ipBuf));
    written = appendJsonString(buf, sizeof(buf), written, ipBuf);
    written = appendRaw(buf, sizeof(buf), written, ",\"shortName\":");
    written = appendJsonString(buf, sizeof(buf), written, node.shortName);
    written = appendRaw(buf, sizeof(buf), written, ",\"longName\":");
    written = appendJsonString(buf, sizeof(buf), written, node.longName);
    written = appendRaw(buf, sizeof(buf), written, ",\"universes\":[");
    for (uint8_t p = 0; p < node.portCount; ++p) {
      if (p > 0) written = appendRaw(buf, sizeof(buf), written, ",");
      written = appendUInt(buf, sizeof(buf), written, node.wireUniverse[p]);
    }
    written = appendRaw(buf, sizeof(buf), written, "]}");
  }
  written = appendRaw(buf, sizeof(buf), written, "]}");

  if (written >= sizeof(buf)) {
    ESP_LOGW(TAG, "response truncated (%u nodes)", static_cast<unsigned>(n));
    written = sizeof(buf) - 1;
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, buf, static_cast<ssize_t>(written));
  return ESP_OK;
}

}  // namespace

void artnet_nodes_web_register_handlers(void* server) {
  httpd_handle_t handle = static_cast<httpd_handle_t>(server);
  httpd_uri_t uri = {};
  uri.uri = "/artnet_nodes";
  uri.method = HTTP_GET;
  uri.handler = artnet_nodes_get_handler;
  httpd_register_uri_handler(handle, &uri);
  ESP_LOGI(TAG, "/artnet_nodes (GET) ready");
}

#endif  // ESP_PLATFORM
