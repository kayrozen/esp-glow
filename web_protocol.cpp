#include "web_protocol.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>

// ---------------------------------------------------------------------------
// Minimal JSON parsing for the pinned web-console protocol.
//
// The protocol has exactly four UI->device message shapes and three
// device->UI message shapes; we hand-roll a tiny recursive-descent parser
// rather than pull in a JSON library. The parser is intentionally strict:
// it accepts only the fields listed in the protocol and rejects anything
// else, so a malformed or hostile message is silently dropped instead of
// dispatching a half-built ControlEvent.
//
// All strings we care about are ASCII; we treat input as UTF-8 bytes but
// only match ASCII keys/values.
// ---------------------------------------------------------------------------

namespace {

// --- small string view over the input ------------------------------------

struct View {
  const char* p;
  size_t len;
};

bool operator==(View a, const char* b) {
  size_t blen = std::strlen(b);
  return a.len == blen && std::memcmp(a.p, b, blen) == 0;
}

// --- tiny parser ----------------------------------------------------------

struct Parser {
  const char* p;
  const char* end;

  bool eof() const { return p >= end; }

  void skipWs() {
    while (!eof() && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
  }

  bool peek(char c) {
    skipWs();
    return !eof() && *p == c;
  }

  bool consume(char c) {
    if (!peek(c)) return false;
    ++p;
    return true;
  }

  // Parse a JSON string literal. On success `out` points into the input
  // buffer (no copy, no unescape — protocol strings have no escapes).
  bool parseStringRaw(View& out) {
    skipWs();
    if (eof() || *p != '"') return false;
    ++p;
    const char* start = p;
    while (!eof() && *p != '"') {
      // Reject control chars and backslash escapes — protocol strings are
      // plain printable ASCII.
      if ((unsigned char)*p < 0x20 || *p == '\\') return false;
      ++p;
    }
    if (eof()) return false;  // unterminated
    out = {start, (size_t)(p - start)};
    ++p;  // closing quote
    return true;
  }

  // Parse a number into a double. Accepts optional leading '-', digits,
  // optional '.', optional exponent. Used for master `value` and for `id`.
  bool parseNumber(double& out) {
    skipWs();
    const char* start = p;
    if (!eof() && (*p == '-' || *p == '+')) ++p;
    bool any = false;
    while (!eof() && *p >= '0' && *p <= '9') { ++p; any = true; }
    if (!eof() && *p == '.') {
      ++p;
      while (!eof() && *p >= '0' && *p <= '9') { ++p; any = true; }
    }
    if (!eof() && (*p == 'e' || *p == 'E')) {
      ++p;
      if (!eof() && (*p == '+' || *p == '-')) ++p;
      while (!eof() && *p >= '0' && *p <= '9') { ++p; }
    }
    if (!any) return false;
    char tmp[32];
    size_t n = (size_t)(p - start);
    if (n >= sizeof(tmp)) return false;
    std::memcpy(tmp, start, n);
    tmp[n] = '\0';
    char* endp = nullptr;
    double v = std::strtod(tmp, &endp);
    if (endp != tmp + n) return false;
    out = v;
    return true;
  }

  // Parse `true` / `false` only.
  bool parseBool(bool& out) {
    skipWs();
    if (end - p >= 4 && std::memcmp(p, "true", 4) == 0) {
      out = true; p += 4; return true;
    }
    if (end - p >= 5 && std::memcmp(p, "false", 5) == 0) {
      out = false; p += 5; return true;
    }
    return false;
  }

  // Skip a single JSON value (object/array/string/number/keyword). Used to
  // tolerate unknown fields without failing the whole parse.
  bool skipValue(int depth) {
    if (depth > 32) return false;
    skipWs();
    if (eof()) return false;
    char c = *p;
    if (c == '"') {
      View dummy;
      return parseStringRaw(dummy);
    }
    if (c == '{') {
      ++p;
      skipWs();
      if (consume('}')) return true;
      while (true) {
        View key;
        if (!parseStringRaw(key)) return false;
        if (!consume(':')) return false;
        if (!skipValue(depth + 1)) return false;
        if (consume(',')) continue;
        if (consume('}')) return true;
        return false;
      }
    }
    if (c == '[') {
      ++p;
      skipWs();
      if (consume(']')) return true;
      while (true) {
        if (!skipValue(depth + 1)) return false;
        if (consume(',')) continue;
        if (consume(']')) return true;
        return false;
      }
    }
    if (c == 't' || c == 'f') {
      bool dummy;
      return parseBool(dummy);
    }
    if (c == 'n' && end - p >= 4 && std::memcmp(p, "null", 4) == 0) {
      p += 4; return true;
    }
    double dummy;
    return parseNumber(dummy);
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// parseWebCommand
// ---------------------------------------------------------------------------

bool parseWebCommand(const char* json, size_t len, ControlEvent& out) {
  if (json == nullptr || len == 0) return false;

  View typeStr{nullptr, 0};
  double idVal = -1.0;
  bool pressedVal = false;
  double valueVal = 0.0;
  bool hasType = false, hasId = false, hasPressed = false, hasValue = false;

  // Single pass: collect whichever fields are present.
  Parser ps{json, json + len};
  ps.skipWs();
  if (!ps.consume('{')) return false;
  ps.skipWs();
  if (ps.consume('}')) return false;  // empty object is not a valid command
  while (true) {
    View k;
    if (!ps.parseStringRaw(k)) return false;
    if (!ps.consume(':')) return false;

    if (k == "type") {
      if (!ps.parseStringRaw(typeStr)) return false;
      hasType = true;
    } else if (k == "id") {
      if (!ps.parseNumber(idVal)) return false;
      hasId = true;
    } else if (k == "pressed") {
      if (!ps.parseBool(pressedVal)) return false;
      hasPressed = true;
    } else if (k == "value") {
      if (!ps.parseNumber(valueVal)) return false;
      hasValue = true;
    } else {
      if (!ps.skipValue(1)) return false;
    }

    if (ps.consume(',')) continue;
    if (ps.consume('}')) break;
    return false;
  }

  if (!hasType) return false;

  if (typeStr == "hello") {
    // Handled by the device scaffold (it triggers a config push). Not a
    // ControlEvent.
    return false;
  }

  if (typeStr == "cue" || typeStr == "scene") {
    if (!hasId) return false;
    if (idVal < 0.0 || idVal > 65535.0) return false;
    // `pressed` is required for cue/scene; the protocol says toggle cues
    // "send true only" but we still accept false and let LiveControl's
    // binding decide what to do with it.
    if (!hasPressed) return false;

    out.type = ControlType::Button;
    out.id = (uint16_t)idVal;
    out.pressed = pressedVal;
    out.value = 0.0f;
    return true;
  }

  if (typeStr == "master") {
    if (!hasValue) return false;
    out.type = ControlType::Fader;
    out.id = 0;  // single grandmaster fader; device binds it to controlId 0
    out.pressed = false;
    // Clamp here so a hostile UI cannot drive master outside [0,1]. The
    // LiveControl layer also clamps, but defense in depth.
    if (valueVal < 0.0) valueVal = 0.0;
    if (valueVal > 1.0) valueVal = 1.0;
    out.value = (float)valueVal;
    return true;
  }

  // Unknown type — including "state" (device->UI only) — is rejected.
  return false;
}

// ---------------------------------------------------------------------------
// buildConfigJson / buildStateJson
// ---------------------------------------------------------------------------

namespace {

// Append a literal C string to `buf`, respecting capacity. Returns the
// number of chars written (may exceed remaining capacity; caller detects
// truncation by comparing against bufLen).
size_t appendRaw(char* buf, size_t bufLen, size_t written, const char* s) {
  size_t n = std::strlen(s);
  if (buf != nullptr && written + 1 < bufLen) {
    size_t avail = bufLen - 1 - written;
    size_t copy = n < avail ? n : avail;
    std::memcpy(buf + written, s, copy);
  }
  return written + n;
}

size_t appendChar(char* buf, size_t bufLen, size_t written, char c) {
  if (buf != nullptr && written + 1 < bufLen) {
    buf[written] = c;
  }
  return written + 1;
}

// Append a JSON string literal, escaping the few characters the protocol
// actually uses. Labels in practice are short ASCII like "Blue wash".
size_t appendString(char* buf, size_t bufLen, size_t written, const char* s) {
  written = appendChar(buf, bufLen, written, '"');
  if (s != nullptr) {
    for (const char* q = s; *q; ++q) {
      char c = *q;
      if (c == '"' || c == '\\') {
        written = appendChar(buf, bufLen, written, '\\');
        written = appendChar(buf, bufLen, written, c);
      } else if ((unsigned char)c < 0x20) {
        // Control char — emit \uXXXX.
        char tmp[8];
        std::snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned)c);
        written = appendRaw(buf, bufLen, written, tmp);
      } else {
        written = appendChar(buf, bufLen, written, c);
      }
    }
  }
  written = appendChar(buf, bufLen, written, '"');
  return written;
}

const char* modeFromAction(ActionKind a) {
  switch (a) {
    case ActionKind::CueFlash:
    case ActionKind::SceneGo:
      return "flash";
    case ActionKind::CueToggle:
    case ActionKind::SceneToggle:
      return "toggle";
    case ActionKind::Master:
      return "flash";  // unreachable: Master is fader-only, never a cue
  }
  return "flash";
}

size_t appendUInt(char* buf, size_t bufLen, size_t written, uint32_t v) {
  char tmp[12];
  int n = std::snprintf(tmp, sizeof(tmp), "%u", v);
  if (n <= 0) return written;
  return appendRaw(buf, bufLen, written, tmp);
}

size_t appendBool(char* buf, size_t bufLen, size_t written, bool v) {
  return appendRaw(buf, bufLen, written, v ? "true" : "false");
}

}  // namespace

size_t buildConfigJson(const WebCueInfo* cues, size_t nCues,
                       const WebSceneInfo* scenes, size_t nScenes,
                       bool hasMaster,
                       char* buf, size_t bufLen) {
  size_t w = 0;
  w = appendRaw(buf, bufLen, w, "{\"type\":\"config\",\"cues\":[");
  for (size_t i = 0; i < nCues; ++i) {
    if (i > 0) w = appendChar(buf, bufLen, w, ',');
    w = appendRaw(buf, bufLen, w, "{\"id\":");
    w = appendUInt(buf, bufLen, w, cues[i].id);
    w = appendRaw(buf, bufLen, w, ",\"label\":");
    const char* label = cues[i].label;
    if (label == nullptr) label = "Cue";
    w = appendString(buf, bufLen, w, label);
    w = appendRaw(buf, bufLen, w, ",\"color\":");
    const char* color = cues[i].color;
    if (color == nullptr) color = "#3060ff";
    w = appendString(buf, bufLen, w, color);
    w = appendRaw(buf, bufLen, w, ",\"mode\":\"");
    w = appendRaw(buf, bufLen, w, modeFromAction(cues[i].action));
    w = appendChar(buf, bufLen, w, '"');
    w = appendChar(buf, bufLen, w, '}');
  }
  w = appendRaw(buf, bufLen, w, "],\"scenes\":[");
  for (size_t i = 0; i < nScenes; ++i) {
    if (i > 0) w = appendChar(buf, bufLen, w, ',');
    w = appendRaw(buf, bufLen, w, "{\"id\":");
    w = appendUInt(buf, bufLen, w, scenes[i].id);
    w = appendRaw(buf, bufLen, w, ",\"label\":");
    const char* label = scenes[i].label;
    if (label == nullptr) label = "Scene";
    w = appendString(buf, bufLen, w, label);
    w = appendChar(buf, bufLen, w, '}');
  }
  w = appendRaw(buf, bufLen, w, "],\"hasMaster\":");
  w = appendBool(buf, bufLen, w, hasMaster);
  w = appendChar(buf, bufLen, w, '}');

  // NUL-terminate if we have any room at all.
  if (buf != nullptr && bufLen > 0) {
    size_t termPos = w < bufLen ? w : bufLen - 1;
    buf[termPos] = '\0';
  }
  return w;
}

size_t buildStateJson(const uint16_t* activeIds, size_t nActive,
                      char* buf, size_t bufLen) {
  size_t w = 0;
  w = appendRaw(buf, bufLen, w, "{\"type\":\"state\",\"active\":[");
  for (size_t i = 0; i < nActive; ++i) {
    if (i > 0) w = appendChar(buf, bufLen, w, ',');
    w = appendUInt(buf, bufLen, w, activeIds[i]);
  }
  w = appendRaw(buf, bufLen, w, "]}");

  if (buf != nullptr && bufLen > 0) {
    size_t termPos = w < bufLen ? w : bufLen - 1;
    buf[termPos] = '\0';
  }
  return w;
}
