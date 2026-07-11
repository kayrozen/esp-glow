// web_input_handler.cpp — tiny JSON scanner for the web console protocol.
#include "web_input_handler.h"
#include "web_protocol.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>

// Find a JSON-ish "key":number pair. Returns the parsed number (0 if absent)
// and sets *found = true if the key was present with a numeric value.
static bool findInt(const char* s, const char* key, int32_t& out) {
  // Search for "key"
  size_t klen = std::strlen(key);
  const char* p = s;
  while ((p = std::strstr(p, key)) != nullptr) {
    // Must be preceded by a quote (the key is a JSON string).
    if (p == s || p[-1] != '"') { p += klen; continue; }
    // And the matched text must be a full key (followed by a quote).
    if (p[klen] != '"') { p += klen; continue; }
    const char* after = p + klen + 1;
    // Skip whitespace then ':'
    while (*after == ' ' || *after == '\t') ++after;
    if (*after != ':') { p += klen; continue; }
    ++after;
    while (*after == ' ' || *after == '\t') ++after;
    // Parse an integer (optionally signed).
    char* end = nullptr;
    long v = std::strtol(after, &end, 10);
    if (end == after) { p += klen; continue; }
    out = static_cast<int32_t>(v);
    return true;
  }
  return false;
}

// Find a quoted string value for a key. Copies up to cap-1 chars into out.
static bool findStr(const char* s, const char* key, char* out, size_t cap) {
  size_t klen = std::strlen(key);
  const char* p = s;
  while ((p = std::strstr(p, key)) != nullptr) {
    if (p == s || p[-1] != '"') { p += klen; continue; }
    if (p[klen] != '"') { p += klen; continue; }
    const char* after = p + klen + 1;
    while (*after == ' ' || *after == '\t') ++after;
    if (*after != ':') { p += klen; continue; }
    ++after;
    while (*after == ' ' || *after == '\t') ++after;
    if (*after != '"') { p += klen; continue; }
    ++after;
    size_t i = 0;
    while (*after && *after != '"' && i + 1 < cap) out[i++] = *after++;
    out[i] = 0;
    return true;
  }
  return false;
}

// Find a "type":"..." field. Returns true and copies the type into out.
static bool findType(const char* s, char* out, size_t cap) {
  return findStr(s, "type", out, cap);
}

bool web_input_handle_text_frame(const char* text, LiveControl& lc, float t) {
  if (!text) return false;
  char type[16] = {0};
  if (!findType(text, type, sizeof(type))) return false;

  int32_t n = 0;
  if (std::strcmp(type, WEB_TYPE_GO) == 0) {
    if (!findInt(text, "cue", n)) return false;
    lc.handleWebCueGo(static_cast<uint16_t>(n), t);
    return true;
  }
  if (std::strcmp(type, WEB_TYPE_RELEASE) == 0) {
    if (!findInt(text, "cue", n)) return false;
    lc.handleWebCueRelease(static_cast<uint16_t>(n), t);
    return true;
  }
  if (std::strcmp(type, WEB_TYPE_SCENE) == 0) {
    if (!findInt(text, "id", n)) return false;
    lc.handleWebScene(static_cast<uint16_t>(n), t);
    return true;
  }
  if (std::strcmp(type, WEB_TYPE_BUTTON) == 0) {
    if (!findInt(text, "id", n)) return false;
    lc.handleWebButton(static_cast<uint8_t>(n), t);
    return true;
  }
  return false;
}

size_t web_input_build_config(const LiveControl& lc, char* out, size_t cap) {
  // {"type":"config","buttons":[{"id":N,"cue":M,"label":"..."},...]}
  size_t off = 0;
  auto write = [&](const char* s) -> bool {
    size_t n = std::strlen(s);
    if (off + n + 1 >= cap) return false;
    std::memcpy(out + off, s, n);
    off += n;
    return true;
  };
  if (!write("{\"type\":\"config\",\"buttons\":[")) return 0;
  const auto& btns = lc.buttons();
  for (size_t i = 0; i < btns.size(); ++i) {
    if (i > 0 && !write(",")) return 0;
    char item[96];
    int n = std::snprintf(item, sizeof(item),
                          "{\"id\":%u,\"cue\":%u,\"label\":\"%s\"}",
                          (unsigned)btns[i].id, (unsigned)btns[i].cueId,
                          btns[i].label ? btns[i].label : "");
    if (n <= 0) return 0;
    item[sizeof(item) - 1] = 0;
    if (!write(item)) return 0;
  }
  if (!write("]}")) return 0;
  out[off] = 0;
  return off;
}
