#include "provision.h"
#include "profile_encoder.h"
#include "show_bundle.h"
#include <sstream>
#include <map>
#include <utility>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cerrno>

// ============================================================================
// .fdef Parsing
// ============================================================================

bool capFromName(const std::string& name, Capability& out) {
  static const std::map<std::string, Capability> nameMap = {
    {"Dimmer", Capability::Dimmer},
    {"Red", Capability::Red},
    {"Green", Capability::Green},
    {"Blue", Capability::Blue},
    {"White", Capability::White},
    {"Amber", Capability::Amber},
    {"Uv", Capability::Uv},
    {"Cyan", Capability::Cyan},
    {"Magenta", Capability::Magenta},
    {"Yellow", Capability::Yellow},
    {"Pan", Capability::Pan},
    {"Tilt", Capability::Tilt},
    {"ShutterStrobe", Capability::ShutterStrobe},
    {"Gobo", Capability::Gobo},
    {"Focus", Capability::Focus},
    {"Zoom", Capability::Zoom},
    {"Fog", Capability::Fog},
    {"Fan", Capability::Fan},
    {"ColorWheel", Capability::ColorWheel},
    {"GoboRotation", Capability::GoboRotation},
    {"Prism", Capability::Prism},
    {"PrismRotation", Capability::PrismRotation},
    {"Frost", Capability::Frost},
    {"Iris", Capability::Iris},
    {"CTO", Capability::CTO},
    {"AnimationWheel", Capability::AnimationWheel},
    {"Macro", Capability::Macro},
    {"Generic", Capability::Generic},
  };

  auto it = nameMap.find(name);
  if (it == nameMap.end()) {
    return false;
  }
  out = it->second;
  return true;
}

static std::vector<std::string> tokenize(const std::string& line) {
  std::vector<std::string> tokens;
  std::istringstream iss(line);
  std::string token;
  while (iss >> token) {
    tokens.push_back(token);
  }
  return tokens;
}

static std::string& stripComments(std::string& line) {
  size_t pos = line.find('#');
  if (pos != std::string::npos) {
    line.erase(pos);
  }
  return line;
}

// Parses `s` as a base-10 integer without ever throwing. This compiler's
// contract (like the rest of the .fdef/.show/.mdef grammar) is "no
// exceptions -- report errors via a return value + an err string": every
// text field a user (or an importer translating a QLC+/GDTF file of
// unknown origin) can type ends up here. On the host, where exceptions are
// enabled, a stoi-in-a-try/catch "works" -- but under Emscripten's default
// WASM build (no -fexceptions), a throw aborts the whole module instead of
// unwinding to the catch, so a plain typo (`FOOTPRINT abc`) that should
// produce a clean error message instead kills the provisioner with an
// opaque Aborted(). This helper -- and parseFloatToken below -- replace
// every stoi/stof call in this file so that guarantee actually holds
// everywhere text gets parsed, not just where it happened to be tested.
// Returns false (out untouched) if `s` isn't a valid integer (empty,
// non-digit characters after an optional leading '-'/'+', or an overflow
// past what fits in a long); every caller range-checks/truncates the
// result itself, matching the pre-existing (pre-fix) behavior exactly.
static bool parseIntToken(const std::string& s, int& out) {
  if (s.empty()) return false;
  size_t i = 0;
  bool neg = false;
  if (s[0] == '-' || s[0] == '+') {
    neg = (s[0] == '-');
    i = 1;
  }
  if (i >= s.size()) return false;
  long v = 0;
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (s[i] - '0');
    if (v > 1000000000L) return false;  // overflow guard; every real field fits in 0..65535
  }
  out = static_cast<int>(neg ? -v : v);
  return true;
}

// Parses `s` as a float without ever throwing -- the parseIntToken of
// std::stof. strtod (plain C, never throws) does the real work; the only
// job here is rejecting what stof's exception used to reject: an empty
// string, or trailing garbage after the number (strtod itself would
// silently accept "12abc" as 12.0 and just stop early, unlike stof/strtod's
// own error signaling via a NULL endptr advance -- checking `end` against
// the full string length is what makes this behave like stof, not silently
// accept partial matches).
static bool parseFloatToken(const std::string& s, float& out) {
  if (s.empty()) return false;
  errno = 0;
  char* end = nullptr;
  double v = std::strtod(s.c_str(), &end);
  if (end != s.c_str() + s.size()) return false;  // empty parse or trailing garbage
  if (errno == ERANGE) return false;               // overflow/underflow
  out = static_cast<float>(v);
  return true;
}

bool parseFixtureDef(const std::string& text, FixtureDef& out, std::string& err) {
  out = FixtureDef();
  err.clear();

  std::istringstream iss(text);
  std::string line;
  int lastCapIndex = -1;  // out.caps index the next SLOT/RANGE line attaches to

  while (std::getline(iss, line)) {
    stripComments(line);

    auto tokens = tokenize(line);
    if (tokens.empty()) {
      continue;
    }

    const std::string& cmd = tokens[0];

    if (cmd == "FIXTURE") {
      if (tokens.size() < 2) {
        err = "FIXTURE: missing name";
        return false;
      }
      // Join the rest as the name
      out.name = tokens[1];
      for (size_t i = 2; i < tokens.size(); i++) {
        out.name += " " + tokens[i];
      }
    } else if (cmd == "FOOTPRINT") {
      if (tokens.size() < 2) {
        err = "FOOTPRINT: missing value";
        return false;
      }
      int fp;
      if (!parseIntToken(tokens[1], fp)) {
        err = "FOOTPRINT: invalid integer";
        return false;
      }
      if (fp < 1 || fp > 255) {
        err = "FOOTPRINT: out of range (1..255)";
        return false;
      }
      out.footprint = static_cast<uint8_t>(fp);
    } else if (cmd == "HEAD") {
      out.isHead = true;
    } else if (cmd == "PANRANGE") {
      if (tokens.size() < 2) {
        err = "PANRANGE: missing value";
        return false;
      }
      if (!parseFloatToken(tokens[1], out.panRangeDeg)) {
        err = "PANRANGE: invalid float";
        return false;
      }
    } else if (cmd == "TILTRANGE") {
      if (tokens.size() < 2) {
        err = "TILTRANGE: missing value";
        return false;
      }
      if (!parseFloatToken(tokens[1], out.tiltRangeDeg)) {
        err = "TILTRANGE: invalid float";
        return false;
      }
    } else if (cmd == "CAP") {
      if (tokens.size() < 3) {
        err = "CAP: missing name or coarse";
        return false;
      }
      Capability cap;
      if (!capFromName(tokens[1], cap)) {
        err = "CAP: unknown capability name: " + tokens[1];
        return false;
      }

      uint8_t coarse, fine = 0xFF, def = 0;
      bool inverted = false;

      {
        int coarseInt;
        if (!parseIntToken(tokens[2], coarseInt)) {
          err = "CAP: invalid coarse channel";
          return false;
        }
        coarse = static_cast<uint8_t>(coarseInt);
      }

      size_t tokenIdx = 3;

      // Fine channel
      if (tokenIdx < tokens.size() && tokens[tokenIdx] != "inv") {
        if (tokens[tokenIdx] != "-") {
          int fineInt;
          if (!parseIntToken(tokens[tokenIdx], fineInt)) {
            err = "CAP: invalid fine channel";
            return false;
          }
          fine = static_cast<uint8_t>(fineInt);
        }
        tokenIdx++;
      }

      // Default value
      if (tokenIdx < tokens.size() && tokens[tokenIdx] != "inv") {
        int defInt;
        if (!parseIntToken(tokens[tokenIdx], defInt)) {
          err = "CAP: invalid default value";
          return false;
        }
        def = static_cast<uint8_t>(defInt);
        tokenIdx++;
      }

      // Inverted flag
      if (tokenIdx < tokens.size() && tokens[tokenIdx] == "inv") {
        inverted = true;
      }

      ChannelMap cm;
      cm.cap = cap;
      cm.coarse = coarse;
      cm.fine = fine;
      cm.defaultValue = def;
      cm.flags = inverted ? 1 : 0;

      out.caps.push_back(cm);
      lastCapIndex = static_cast<int>(out.caps.size()) - 1;
    } else if (cmd == "SLOT" || cmd == "RANGE") {
      if (lastCapIndex < 0) {
        err = cmd + ": no preceding CAP";
        return false;
      }
      if (tokens.size() < 3) {
        err = cmd + ": missing from or to";
        return false;
      }

      int from, to;
      if (!parseIntToken(tokens[1], from) || !parseIntToken(tokens[2], to)) {
        err = cmd + ": invalid from/to";
        return false;
      }
      if (from < 0 || from > 255 || to < 0 || to > 255) {
        err = cmd + ": from/to out of range (0..255)";
        return false;
      }
      if (from > to) {
        err = cmd + ": from must be <= to";
        return false;
      }

      // Name is the rest of the line (spaces allowed), i.e. everything
      // after the <to> token -- re-scan `line` (not `tokens`, which
      // collapses whitespace) to preserve it verbatim.
      std::istringstream lineIss(line);
      std::string skip1, skip2, skip3;
      lineIss >> skip1 >> skip2 >> skip3;
      std::string rest;
      std::getline(lineIss, rest);
      size_t nameStart = rest.find_first_not_of(" \t");
      std::string rangeName = (nameStart == std::string::npos) ? "" : rest.substr(nameStart);
      while (!rangeName.empty() &&
             (rangeName.back() == ' ' || rangeName.back() == '\t' || rangeName.back() == '\r')) {
        rangeName.pop_back();
      }

      FdefRange fr;
      fr.capIndex = static_cast<uint8_t>(lastCapIndex);
      fr.dmxFrom = static_cast<uint8_t>(from);
      fr.dmxTo = static_cast<uint8_t>(to);
      fr.continuous = (cmd == "RANGE");
      fr.name = rangeName;
      out.ranges.push_back(fr);
    } else if (!cmd.empty()) {
      err = "Unknown command: " + cmd;
      return false;
    }
  }

  if (out.footprint == 0) {
    err = "FOOTPRINT is required";
    return false;
  }

  return true;
}

std::vector<uint8_t> encodeProfile(const FixtureDef& def) {
  ProfileBuilder builder;
  builder.setFootprint(def.footprint);
  builder.name = def.name;
  for (const auto& cm : def.caps) {
    bool inv = (cm.flags & 1) != 0;
    builder.add(cm.cap, cm.coarse, cm.fine, cm.defaultValue, inv);
  }
  for (const auto& r : def.ranges) {
    builder.addRange(r.capIndex, r.dmxFrom, r.dmxTo, r.continuous, r.name);
  }
  return builder.encode();
}

// ============================================================================
// .mdef Parsing
// ============================================================================

static bool parseEncoderMode(const std::string& s, EncoderMode& out) {
  if (s == "absolute") { out = EncoderMode::Absolute; return true; }
  if (s == "relative-2c") { out = EncoderMode::Relative2C; return true; }
  if (s == "relative-signmag") { out = EncoderMode::RelativeSignMag; return true; }
  return false;
}

// Parses a trailing "CH <lo> <hi>" channel-significance modifier (PAD/FADER/
// LED lines -- FORMAT.md's "Per-range channel significance"). `idx` must
// point at the "CH" token itself; `lo`/`hi` are 0-indexed MIDI channels
// (0..15), NOT the 1-indexed MIDI_CHANNEL directive's 0..16 scheme -- CH's
// 0 means "channel 1" the way parseMidi's ControlEvent::channel does, not
// MIDI_CHANNEL's "any".
static bool parseChannelRange(const std::vector<std::string>& tokens, size_t idx,
                              const char* context, uint8_t& loOut, uint8_t& hiOut,
                              std::string& err) {
  if (tokens[idx] != "CH") {
    err = std::string(context) + ": unexpected extra token '" + tokens[idx] + "'";
    return false;
  }
  if (idx + 2 >= tokens.size()) {
    err = std::string(context) + ": CH expects '<lo> <hi>'";
    return false;
  }
  int lo, hi;
  if (!parseIntToken(tokens[idx + 1], lo) || !parseIntToken(tokens[idx + 2], hi)) {
    err = std::string(context) + ": CH channel numbers must be integers";
    return false;
  }
  if (lo < 0 || lo > 15 || hi < 0 || hi > 15) {
    err = std::string(context) + ": CH channel out of range (0..15)";
    return false;
  }
  if (lo > hi) {
    err = std::string(context) + ": CH lo must be <= hi";
    return false;
  }
  if (idx + 3 != tokens.size()) {
    err = std::string(context) + ": unexpected extra token '" + tokens[idx + 3] + "'";
    return false;
  }
  loOut = static_cast<uint8_t>(lo);
  hiOut = static_cast<uint8_t>(hi);
  return true;
}

// FADER/ENCODER below use parseIntToken (defined above, near tokenize) not
// just for genuinely malformed input but as *routine* control flow: peeking
// at the next token to decide "is this a <to> number, or does the name/mode
// start here?". A named fader (`FADER CC 7 master`) or a moded encoder
// (`ENCODER CC 16 23 relative-2c`) hits the "not a number" branch on nearly
// every real .mdef file -- this is exactly the path that used to throw on
// the *common* case, not just the error case, before parseIntToken existed.

bool parseControllerDef(const std::string& text, ControllerBuilder& out, std::string& err) {
  out = ControllerBuilder();
  err.clear();

  std::istringstream iss(text);
  std::string line;
  int lastLedIndex = -1;  // out.leds index the next COLOR line attaches to

  while (std::getline(iss, line)) {
    stripComments(line);
    auto tokens = tokenize(line);
    if (tokens.empty()) continue;

    const std::string& cmd = tokens[0];

    if (cmd == "CONTROLLER") {
      if (tokens.size() < 2) { err = "CONTROLLER: missing name"; return false; }
      out.name = tokens[1];
      for (size_t i = 2; i < tokens.size(); i++) out.name += " " + tokens[i];
    } else if (cmd == "MIDI_CHANNEL") {
      if (tokens.size() < 2) { err = "MIDI_CHANNEL: missing value"; return false; }
      int ch;
      if (!parseIntToken(tokens[1], ch)) { err = "MIDI_CHANNEL: invalid integer"; return false; }
      if (ch < 0 || ch > 16) { err = "MIDI_CHANNEL: out of range (0..16)"; return false; }
      out.midiChannel = static_cast<uint8_t>(ch);
    } else if (cmd == "PAD") {
      if (tokens.size() < 2) { err = "PAD: missing note"; return false; }
      int from, to;
      if (!parseIntToken(tokens[1], from)) { err = "PAD: invalid note number"; return false; }
      to = from;
      size_t idx = 2;
      if (idx < tokens.size() && tokens[idx] != "CH") {
        if (!parseIntToken(tokens[idx], to)) { err = "PAD: invalid note number"; return false; }
        idx++;
      }
      if (from < 0 || from > 127 || to < 0 || to > 127) { err = "PAD: note out of range (0..127)"; return false; }
      if (from > to) { err = "PAD: from must be <= to"; return false; }

      uint8_t chFrom = kChannelAgnostic, chTo = kChannelAgnostic;
      if (idx < tokens.size()) {
        if (!parseChannelRange(tokens, idx, "PAD", chFrom, chTo, err)) return false;
      }
      out.pads.push_back({static_cast<uint8_t>(from), static_cast<uint8_t>(to), chFrom, chTo});
    } else if (cmd == "FADER") {
      if (tokens.size() < 3 || tokens[1] != "CC") {
        err = "FADER: expected 'CC <from> [<to>] [name] [CH <lo> <hi>]'";
        return false;
      }
      int from;
      if (!parseIntToken(tokens[2], from)) { err = "FADER: invalid CC number"; return false; }
      size_t idx = 3;
      int to = from;
      if (idx < tokens.size() && parseIntToken(tokens[idx], to)) {
        idx++;
      } else {
        to = from;  // not a number: this token starts the fader's name instead
      }
      if (from < 0 || from > 127 || to < 0 || to > 127) { err = "FADER: CC out of range (0..127)"; return false; }
      if (from > to) { err = "FADER: from must be <= to"; return false; }

      // The name runs up to (not including) a "CH" token, if any -- CH's
      // <lo> <hi> come after it, same "reserved keyword ends the free-text
      // field" convention used nowhere else in this grammar yet, but the
      // only way to let a name and a trailing CH modifier coexist.
      std::string fname;
      for (; idx < tokens.size() && tokens[idx] != "CH"; ++idx) {
        if (!fname.empty()) fname += " ";
        fname += tokens[idx];
      }
      uint8_t chFrom = kChannelAgnostic, chTo = kChannelAgnostic;
      if (idx < tokens.size()) {
        if (!parseChannelRange(tokens, idx, "FADER", chFrom, chTo, err)) return false;
      }
      out.faders.push_back({static_cast<uint8_t>(from), static_cast<uint8_t>(to), fname, chFrom, chTo});
    } else if (cmd == "ENCODER") {
      if (tokens.size() < 3 || tokens[1] != "CC") {
        err = "ENCODER: expected 'CC <from> [<to>] [mode]'";
        return false;
      }
      int from;
      if (!parseIntToken(tokens[2], from)) { err = "ENCODER: invalid CC number"; return false; }
      size_t idx = 3;
      int to = from;
      if (idx < tokens.size() && parseIntToken(tokens[idx], to)) {
        idx++;
      } else {
        to = from;  // not a number: this token is the mode instead
      }
      if (from < 0 || from > 127 || to < 0 || to > 127) { err = "ENCODER: CC out of range (0..127)"; return false; }
      if (from > to) { err = "ENCODER: from must be <= to"; return false; }
      EncoderMode mode = EncoderMode::Absolute;
      if (idx < tokens.size()) {
        if (!parseEncoderMode(tokens[idx], mode)) {
          err = "ENCODER: unknown mode '" + tokens[idx] + "'";
          return false;
        }
        idx++;
      }
      if (idx < tokens.size()) {
        err = "ENCODER: unexpected extra token '" + tokens[idx] + "'";
        return false;
      }
      out.encoders.push_back({static_cast<uint8_t>(from), static_cast<uint8_t>(to), mode});
    } else if (cmd == "LED") {
      if (tokens.size() < 5) {
        err = "LED: expected 'NOTE|CC <from> <to> velocity|value [CH <lo> <hi>]'";
        return false;
      }
      LedMsgType msgType;
      if (tokens[1] == "NOTE") msgType = LedMsgType::Note;
      else if (tokens[1] == "CC") msgType = LedMsgType::Cc;
      else { err = "LED: expected NOTE or CC, got '" + tokens[1] + "'"; return false; }

      int from, to;
      if (!parseIntToken(tokens[2], from) || !parseIntToken(tokens[3], to)) {
        err = "LED: invalid address range";
        return false;
      }
      if (from < 0 || from > 127 || to < 0 || to > 127) { err = "LED: address out of range (0..127)"; return false; }
      if (from > to) { err = "LED: from must be <= to"; return false; }

      LedSemantic semantic;
      if (tokens[4] == "velocity") semantic = LedSemantic::Velocity;
      else if (tokens[4] == "value") semantic = LedSemantic::Value;
      else { err = "LED: expected velocity or value, got '" + tokens[4] + "'"; return false; }

      uint8_t chFrom = kChannelAgnostic, chTo = kChannelAgnostic;
      if (tokens.size() > 5) {
        if (!parseChannelRange(tokens, 5, "LED", chFrom, chTo, err)) return false;
      }

      ControllerLedSpec spec;
      spec.msgType = msgType;
      spec.addrFrom = static_cast<uint8_t>(from);
      spec.addrTo = static_cast<uint8_t>(to);
      spec.semantic = semantic;
      spec.channelFrom = chFrom;
      spec.channelTo = chTo;
      out.leds.push_back(std::move(spec));
      lastLedIndex = static_cast<int>(out.leds.size()) - 1;
    } else if (cmd == "COLOR") {
      if (lastLedIndex < 0) { err = "COLOR: no preceding LED"; return false; }
      if (tokens.size() < 3) { err = "COLOR: missing name or value"; return false; }
      int value;
      if (!parseIntToken(tokens[2], value)) { err = "COLOR: invalid value"; return false; }
      if (value < 0 || value > 127) { err = "COLOR: value out of range (0..127)"; return false; }
      out.leds[static_cast<size_t>(lastLedIndex)].colors.push_back({tokens[1], static_cast<uint8_t>(value)});
    } else if (!cmd.empty()) {
      err = "Unknown command: " + cmd;
      return false;
    }
  }

  if (out.name.empty()) {
    err = "CONTROLLER is required";
    return false;
  }

  return true;
}

std::vector<uint8_t> encodeController(const ControllerBuilder& def, std::string& err) {
  return def.encode(err);
}

// ============================================================================
// .show Parsing and Compilation
// ============================================================================

// Parses "a.b.c.d" (each octet 0..255, no extra characters) into a packed
// host-byte-order IPv4, same convention as CFG1's artnetFallbackIp and
// device_config.h. Never throws (parseIntToken doesn't either) -- rejects
// anything that isn't exactly four dot-separated octets.
static bool parseIPv4Token(const std::string& s, uint32_t& out) {
  uint8_t octets[4];
  size_t start = 0;
  for (int i = 0; i < 4; ++i) {
    size_t dot = (i < 3) ? s.find('.', start) : s.size();
    if (dot == std::string::npos) return false;
    std::string part = s.substr(start, dot - start);
    int v;
    if (!parseIntToken(part, v)) return false;
    if (v < 0 || v > 255) return false;
    octets[i] = static_cast<uint8_t>(v);
    start = dot + 1;
  }
  out = (static_cast<uint32_t>(octets[0]) << 24) | (static_cast<uint32_t>(octets[1]) << 16) |
        (static_cast<uint32_t>(octets[2]) << 8) | static_cast<uint32_t>(octets[3]);
  return true;
}

struct UniverseTransportEntry {
  UniverseTransport transport = UniverseTransport::Unused;
  // Wave 3: explicit Art-Net routing (see FORMAT.md's "Art-Net Wire
  // Universe & Destination Routing"). destIp==0 means "no explicit IP" --
  // the fallback/broadcast marker carried straight into the SHW1 bundle.
  // wireUniverse is always resolved by the time this is written to the
  // bundle: either what the .show said, or (if omitted) the universe's own
  // internal 0-indexed number, matching today's implicit behavior.
  uint32_t destIp = 0;
  uint16_t wireUniverse = 0;
  bool hasExplicitRoute = false;  // true if the .show gave ip and/or wireUniverse
};

struct FixtureInstance {
  std::string defFile;
  uint8_t universe = 0;
  uint16_t base = 0;

  // Head-specific fields
  float posX = 0, posY = 0, posZ = 0;
  float yaw = 0, pitch = 0, roll = 0;
  float panCenterNorm = 0.5f, tiltCenterNorm = 0.5f;
  bool invertPan = false, invertTilt = false;
};

struct MatrixInstance {
  uint16_t width = 0, height = 0;
  bool serpentine = false;
  bool vertical = false;
  std::string orderStr;
  uint8_t startUniverse = 0;
  uint16_t startChannel = 0;
};

// A span of DMX channels occupied by one fixture or matrix, in human-facing
// 1-indexed addresses -- used only for the post-parse overlap/gap checks
// below, never written to the SHW1 bundle.
struct OccupiedRange {
  uint8_t universe;  // 0-indexed internal universe
  int fromAddr;       // 1-indexed, inclusive
  int toAddr;          // 1-indexed, inclusive
  std::string name;
};

static std::string missingShowHeaderErr() {
  return "this .show has no 'SHOW 2' header. Addressing changed in v2: "
         "universes and DMX addresses are now 1-indexed (write the address "
         "printed on the fixture). Add 'SHOW 2' as the first line and add 1 "
         "to every universe and channel number.";
}

// Converts a 1-indexed .show universe number to the internal 0-indexed
// index, validating range 1..8. `context` names the calling directive
// (e.g. "UNIVERSE", "FIXTURE") for the error message.
static bool convertUniverse(int univOneIndexed, const char* context, uint8_t& out, std::string& err) {
  if (univOneIndexed < 1) {
    err = std::string(context) + ": universes are 1-indexed (write 1 for the first universe, not 0)";
    return false;
  }
  if (univOneIndexed > 8) {
    err = std::string(context) + ": universe out of range (1..8)";
    return false;
  }
  out = static_cast<uint8_t>(univOneIndexed - 1);
  return true;
}

// Converts a 1-indexed DMX address (a FIXTURE base or MATRIX start channel)
// to the internal 0-indexed channel, validating range 1..512.
static bool convertAddress(int addrOneIndexed, const char* context, uint16_t& out, std::string& err) {
  if (addrOneIndexed < 1 || addrOneIndexed > 512) {
    err = std::string(context) + ": DMX addresses are 1..512";
    return false;
  }
  out = static_cast<uint16_t>(addrOneIndexed - 1);
  return true;
}

static ColorOrder colorOrderFromString(const std::string& s) {
  if (s == "RGB") return ColorOrder::RGB;
  if (s == "GRB") return ColorOrder::GRB;
  if (s == "BRG") return ColorOrder::BRG;
  if (s == "RBG") return ColorOrder::RBG;
  if (s == "GBR") return ColorOrder::GBR;
  if (s == "BGR") return ColorOrder::BGR;
  return ColorOrder::RGB;  // Default
}

CompileResult compileShow(const std::string& showText,
                          const std::function<std::string(const std::string&)>& readFile) {
  CompileResult result;

  std::map<uint8_t, UniverseTransportEntry> universes;
  std::vector<FixtureInstance> fixtures;
  std::vector<MatrixInstance> matrices;
  std::map<std::string, FixtureDef> defCache;
  std::map<std::string, int> profileIndexMap;  // defFile -> profileIndex
  std::vector<std::vector<uint8_t>> profileBlobs;
  std::map<std::string, int> controllerIndexMap;  // defFile -> mdef blob index
  std::vector<std::vector<uint8_t>> controllerBlobs;

  // Wave 3: (destIp, wireUniverse) -> the internal universe index that
  // already claimed it. Two ARTNET universes resolving to the same pair is
  // never intentional (two streams fighting over one node output) -- same
  // IP with different wire universes is fine (a multi-output node).
  std::map<std::pair<uint32_t, uint16_t>, uint8_t> artnetDestOwner;
  bool anyExplicitArtnetRoute = false;  // gates the v3 bundle-version bump

  uint8_t maxUniverse = 0;
  FixtureInstance* lastFixture = nullptr;
  bool sawHeader = false;
  std::vector<OccupiedRange> occupied;

  // Parse .show
  std::istringstream iss(showText);
  std::string line;

  while (std::getline(iss, line)) {
    stripComments(line);
    auto tokens = tokenize(line);
    if (tokens.empty()) {
      continue;
    }

    const std::string& cmd = tokens[0];

    // SHOW 2 must be the first non-comment, non-blank line. No v1 fallback:
    // a pre-migration .show that just happens to parse (no SHOW line) would
    // otherwise silently mean every address shifted by one channel.
    if (!sawHeader) {
      if (cmd != "SHOW") {
        result.err = missingShowHeaderErr();
        return result;
      }
      if (tokens.size() < 2 || tokens[1] != "2") {
        result.err = "SHOW: unsupported version (this compiler only supports 'SHOW 2')";
        return result;
      }
      sawHeader = true;
      continue;
    }

    if (cmd == "UNIVERSE") {
      if (tokens.size() < 3) {
        result.err = "UNIVERSE: missing index or transport";
        return result;
      }
      int idxInt;
      if (!parseIntToken(tokens[1], idxInt)) {
        result.err = "UNIVERSE: invalid index";
        return result;
      }

      UniverseTransport transport;
      const std::string& tName = tokens[2];
      if (tName == "DMX") {
        transport = UniverseTransport::Dmx;
      } else if (tName == "ARTNET") {
        transport = UniverseTransport::ArtNet;
      } else if (tName == "SACN") {
        transport = UniverseTransport::Sacn;
      } else {
        result.err = "UNIVERSE: unknown transport: " + tName;
        return result;
      }

      uint8_t idx;
      if (!convertUniverse(idxInt, "UNIVERSE", idx, result.err)) {
        return result;
      }

      UniverseTransportEntry entry;
      entry.transport = transport;

      if (transport == UniverseTransport::ArtNet) {
        // UNIVERSE <idx> ARTNET [<ip>] [<wireUniverse>] -- see FORMAT.md's
        // "Art-Net Wire Universe & Destination Routing" section. Bare
        // ARTNET (no args) keeps today's behavior exactly: fallback/
        // broadcast destination, wire universe defaults to the internal
        // index -- and does NOT bump the bundle to v3.
        entry.wireUniverse = idx;
        if (tokens.size() >= 4) {
          entry.hasExplicitRoute = true;
          if (!parseIPv4Token(tokens[3], entry.destIp)) {
            result.err = "UNIVERSE " + std::to_string(idxInt) + ": invalid Art-Net IP: " + tokens[3];
            return result;
          }
          if (tokens.size() >= 5) {
            int wireInt;
            if (!parseIntToken(tokens[4], wireInt) || wireInt < 0 || wireInt > 32767) {
              result.err = "UNIVERSE " + std::to_string(idxInt) + ": wire universe must be 0..32767";
              return result;
            }
            entry.wireUniverse = static_cast<uint16_t>(wireInt);
          } else {
            // An explicit IP with no wire universe is almost always a
            // mistake (it silently falls back to the internal index) --
            // non-fatal, but worth surfacing.
            result.warnings.push_back(
                "UNIVERSE " + std::to_string(idxInt) + " ARTNET " + tokens[3] +
                ": no wire universe given; defaulting to " + std::to_string(idx) +
                " (the internal index) -- write it explicitly if that isn't what you mean");
          }
        }

        auto key = std::make_pair(entry.destIp, entry.wireUniverse);
        auto collideIt = artnetDestOwner.find(key);
        if (collideIt != artnetDestOwner.end() && collideIt->second != idx) {
          result.err = "UNIVERSE " + std::to_string(idxInt) +
                       ": routes to the same Art-Net destination (ip, wire universe) as "
                       "UNIVERSE " + std::to_string(collideIt->second + 1) +
                       " -- two universes cannot share one node output "
                       "(same IP with a different wire universe is fine)";
          return result;
        }
        artnetDestOwner[key] = idx;
        if (entry.hasExplicitRoute) anyExplicitArtnetRoute = true;
      }

      universes[idx] = entry;
      if (idx > maxUniverse) {
        maxUniverse = idx;
      }

    } else if (cmd == "FIXTURE") {
      if (tokens.size() < 4) {
        result.err = "FIXTURE: missing deffile, universe, or base";
        return result;
      }

      const std::string& defFile = tokens[1];
      int univInt, baseInt;
      if (!parseIntToken(tokens[2], univInt) || !parseIntToken(tokens[3], baseInt)) {
        result.err = "FIXTURE: invalid universe or base";
        return result;
      }

      uint8_t univ;
      if (!convertUniverse(univInt, "FIXTURE", univ, result.err)) {
        return result;
      }
      uint16_t baseAddr;
      if (!convertAddress(baseInt, "FIXTURE", baseAddr, result.err)) {
        return result;
      }
      if (maxUniverse < univ) {
        maxUniverse = univ;
      }

      // Load and cache the fixture def
      if (defCache.find(defFile) == defCache.end()) {
        std::string defText = readFile(defFile);
        if (defText.empty()) {
          result.err = "FIXTURE: cannot load " + defFile;
          return result;
        }
        FixtureDef def;
        if (!parseFixtureDef(defText, def, result.err)) {
          return result;
        }
        defCache[defFile] = def;
      }

      const FixtureDef& def = defCache[defFile];
      int footprintEnd = baseInt + static_cast<int>(def.footprint) - 1;
      if (footprintEnd > 512) {
        result.err = "FIXTURE: fixture at address " + std::to_string(baseInt) +
                      " with a " + std::to_string(static_cast<int>(def.footprint)) +
                      "-channel footprint runs past the end of universe " +
                      std::to_string(univInt);
        return result;
      }

      FixtureInstance fi;
      fi.defFile = defFile;
      fi.universe = univ;
      fi.base = baseAddr;

      fixtures.push_back(fi);
      lastFixture = &fixtures.back();

      occupied.push_back({univ, baseInt, footprintEnd, def.name.empty() ? defFile : def.name});

    } else if (cmd == "POS") {
      if (!lastFixture) {
        result.err = "POS: no FIXTURE before POS";
        return result;
      }
      if (tokens.size() < 4) {
        result.err = "POS: missing x, y, or z";
        return result;
      }

      const auto& def = defCache[lastFixture->defFile];
      if (!def.isHead) {
        result.err = "POS: fixture is not a head";
        return result;
      }

      if (!parseFloatToken(tokens[1], lastFixture->posX) ||
          !parseFloatToken(tokens[2], lastFixture->posY) ||
          !parseFloatToken(tokens[3], lastFixture->posZ)) {
        result.err = "POS: invalid coordinates";
        return result;
      }

    } else if (cmd == "ROT") {
      if (!lastFixture) {
        result.err = "ROT: no FIXTURE before ROT";
        return result;
      }
      if (tokens.size() < 4) {
        result.err = "ROT: missing yaw, pitch, or roll";
        return result;
      }

      const auto& def = defCache[lastFixture->defFile];
      if (!def.isHead) {
        result.err = "ROT: fixture is not a head";
        return result;
      }

      if (!parseFloatToken(tokens[1], lastFixture->yaw) ||
          !parseFloatToken(tokens[2], lastFixture->pitch) ||
          !parseFloatToken(tokens[3], lastFixture->roll)) {
        result.err = "ROT: invalid angles";
        return result;
      }

    } else if (cmd == "CENTER") {
      if (!lastFixture) {
        result.err = "CENTER: no FIXTURE before CENTER";
        return result;
      }
      if (tokens.size() < 3) {
        result.err = "CENTER: missing panNorm or tiltNorm";
        return result;
      }

      const auto& def = defCache[lastFixture->defFile];
      if (!def.isHead) {
        result.err = "CENTER: fixture is not a head";
        return result;
      }

      if (!parseFloatToken(tokens[1], lastFixture->panCenterNorm) ||
          !parseFloatToken(tokens[2], lastFixture->tiltCenterNorm)) {
        result.err = "CENTER: invalid values";
        return result;
      }

    } else if (cmd == "INVERT") {
      if (!lastFixture) {
        result.err = "INVERT: no FIXTURE before INVERT";
        return result;
      }
      if (tokens.size() < 3) {
        result.err = "INVERT: missing pan or tilt invert flag";
        return result;
      }

      const auto& def = defCache[lastFixture->defFile];
      if (!def.isHead) {
        result.err = "INVERT: fixture is not a head";
        return result;
      }

      int invPan, invTilt;
      if (!parseIntToken(tokens[1], invPan) || !parseIntToken(tokens[2], invTilt)) {
        result.err = "INVERT: invalid flags";
        return result;
      }
      lastFixture->invertPan = (invPan != 0);
      lastFixture->invertTilt = (invTilt != 0);

    } else if (cmd == "MATRIX") {
      if (tokens.size() < 8) {
        result.err = "MATRIX: missing parameters";
        return result;
      }

      MatrixInstance mi;
      int startUniverseInt, startChannelInt, widthInt, heightInt;
      if (!parseIntToken(tokens[1], startUniverseInt) ||
          !parseIntToken(tokens[2], startChannelInt) ||
          !parseIntToken(tokens[3], widthInt) ||
          !parseIntToken(tokens[4], heightInt)) {
        result.err = "MATRIX: invalid numeric parameters";
        return result;
      }

      if (!convertUniverse(startUniverseInt, "MATRIX", mi.startUniverse, result.err)) {
        return result;
      }
      if (!convertAddress(startChannelInt, "MATRIX", mi.startChannel, result.err)) {
        return result;
      }
      mi.width = static_cast<uint16_t>(widthInt);
      mi.height = static_cast<uint16_t>(heightInt);

      mi.serpentine = (tokens[5] == "SERP");
      mi.vertical = (tokens[6] == "V");
      mi.orderStr = tokens[7];

      if (maxUniverse < mi.startUniverse) {
        maxUniverse = mi.startUniverse;
      }

      matrices.push_back(mi);

      // Matrices are allowed to span multiple universes -- PixelMatrix
      // (pixel_matrix.cpp) packs w*h*3 channels starting at
      // startUniverse:startChannel and rolls over into subsequent
      // universes once a universe's 512 channels fill up. Split the
      // occupied range across every universe it touches (in 1-indexed,
      // per-universe channel numbers) so overlap detection below sees the
      // real footprint in each one, instead of wrongly flagging a large
      // matrix as "running past the end".
      {
        long long totalChannels = static_cast<long long>(mi.width) * static_cast<long long>(mi.height) * 3;
        std::string name = std::to_string(mi.width) + "x" + std::to_string(mi.height) + " Matrix";
        long long globalStart = static_cast<long long>(mi.startUniverse) * 512 + (startChannelInt - 1);
        long long globalEnd = globalStart + totalChannels - 1;
        for (long long g = globalStart; g <= globalEnd;) {
          long long u = g / 512;
          long long universeEnd = u * 512 + 511;
          long long spanEnd = std::min(globalEnd, universeEnd);
          if (u > 255) break;  // out of representable universe range; give up extending
          occupied.push_back({static_cast<uint8_t>(u), static_cast<int>(g % 512) + 1,
                               static_cast<int>(spanEnd % 512) + 1, name});
          g = spanEnd + 1;
        }
      }

    } else if (cmd == "CONTROLLER") {
      if (tokens.size() < 2) {
        result.err = "CONTROLLER: missing deffile";
        return result;
      }
      const std::string& defFile = tokens[1];
      if (controllerIndexMap.find(defFile) == controllerIndexMap.end()) {
        std::string defText = readFile(defFile);
        if (defText.empty()) {
          result.err = "CONTROLLER: cannot load " + defFile;
          return result;
        }
        ControllerBuilder def;
        if (!parseControllerDef(defText, def, result.err)) {
          return result;
        }
        std::string encErr;
        auto blob = encodeController(def, encErr);
        if (!encErr.empty()) {
          result.err = "CONTROLLER: " + encErr;
          return result;
        }
        controllerIndexMap[defFile] = static_cast<int>(controllerBlobs.size());
        controllerBlobs.push_back(std::move(blob));
      }

    } else if (!cmd.empty()) {
      result.err = "Unknown command: " + cmd;
      return result;
    }
  }

  if (!sawHeader) {
    // A file that was entirely blank/comments never hit the header check
    // inside the loop -- still needs one.
    result.err = missingShowHeaderErr();
    return result;
  }

  // Overlap detection: group every fixture's and matrix's occupied-channel
  // range by universe and report every colliding pair, not just the first
  // -- a botched patch usually has several. This is the single most common
  // real-world patching mistake, and it doesn't error at runtime; it just
  // makes two fixtures fight over channels.
  {
    std::map<uint8_t, std::vector<size_t>> byUniverse;
    for (size_t i = 0; i < occupied.size(); i++) {
      byUniverse[occupied[i].universe].push_back(i);
    }

    std::vector<std::string> collisions;
    for (const auto& entry : byUniverse) {
      const auto& idxs = entry.second;
      for (size_t a = 0; a < idxs.size(); a++) {
        for (size_t b = a + 1; b < idxs.size(); b++) {
          const OccupiedRange& ra = occupied[idxs[a]];
          const OccupiedRange& rb = occupied[idxs[b]];
          if (ra.fromAddr <= rb.toAddr && rb.fromAddr <= ra.toAddr) {
            int overlapFrom = std::max(ra.fromAddr, rb.fromAddr);
            int overlapTo = std::min(ra.toAddr, rb.toAddr);
            collisions.push_back(
                "address collision in universe " + std::to_string(entry.first + 1) + ":\n" +
                "  '" + ra.name + "' at " + std::to_string(ra.fromAddr) + " occupies channels " +
                std::to_string(ra.fromAddr) + ".." + std::to_string(ra.toAddr) + "\n" +
                "  '" + rb.name + "' at " + std::to_string(rb.fromAddr) + " occupies channels " +
                std::to_string(rb.fromAddr) + ".." + std::to_string(rb.toAddr) + "\n" +
                "  \xe2\x86\x92 they overlap on channels " + std::to_string(overlapFrom) + ".." +
                std::to_string(overlapTo));
          }
        }
      }
    }

    if (!collisions.empty()) {
      std::string joined;
      for (size_t i = 0; i < collisions.size(); i++) {
        if (i > 0) joined += "\n\n";
        joined += collisions[i];
      }
      result.err = joined;
      return result;
    }

    // Gap warning (informational, not an error): unused trailing channels
    // in a universe that's actually patched.
    for (const auto& entry : byUniverse) {
      int maxEnd = 0;
      for (size_t idx : entry.second) {
        maxEnd = std::max(maxEnd, occupied[idx].toAddr);
      }
      if (maxEnd > 0 && maxEnd < 512) {
        result.warnings.push_back("universe " + std::to_string(entry.first + 1) + ": channels " +
                                   std::to_string(maxEnd + 1) + "..512 unused");
      }
    }
  }

  // Build profile table with deduplication
  for (const auto& fi : fixtures) {
    if (profileIndexMap.find(fi.defFile) == profileIndexMap.end()) {
      auto blob = encodeProfile(defCache[fi.defFile]);
      profileIndexMap[fi.defFile] = static_cast<int>(profileBlobs.size());
      profileBlobs.push_back(blob);
    }
  }

  // Build universe count
  uint8_t universeCount = maxUniverse + 1;

  // Write SHW1 bundle. Version 2 (mdefCount + a trailing controller table)
  // when at least one CONTROLLER was compiled; version 3 (v2's header plus
  // a 7-byte-per-entry universe table carrying destIp/wireUniverse) when at
  // least one UNIVERSE ARTNET line gave an explicit ip/wireUniverse -- byte-
  // identical to v1/v2 otherwise, same "only bump the version once the new
  // feature is actually used" convention as ProfileBuilder::encode's PFX2.
  std::vector<uint8_t> bundle;
  uint8_t version = 1;
  if (anyExplicitArtnetRoute) {
    version = 3;
  } else if (!controllerBlobs.empty()) {
    version = 2;
  }

  // Header
  bundle.push_back('S');
  bundle.push_back('H');
  bundle.push_back('W');
  bundle.push_back('1');
  bundle.push_back(version);
  bundle.push_back(universeCount);

  uint16_t profileCount = static_cast<uint16_t>(profileBlobs.size());
  uint16_t fixtureCount = static_cast<uint16_t>(fixtures.size());
  uint16_t matrixCount = static_cast<uint16_t>(matrices.size());
  uint16_t mdefCount = static_cast<uint16_t>(controllerBlobs.size());

  // Write counts (little-endian u16)
  auto writeU16 = [&](uint16_t v) {
    bundle.push_back(v & 0xFF);
    bundle.push_back((v >> 8) & 0xFF);
  };

  auto writeU8 = [&](uint8_t v) {
    bundle.push_back(v);
  };

  auto writeF32 = [&](float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(float));
    bundle.push_back(bits & 0xFF);
    bundle.push_back((bits >> 8) & 0xFF);
    bundle.push_back((bits >> 16) & 0xFF);
    bundle.push_back((bits >> 24) & 0xFF);
  };

  auto writeU32 = [&](uint32_t v) {
    bundle.push_back(v & 0xFF);
    bundle.push_back((v >> 8) & 0xFF);
    bundle.push_back((v >> 16) & 0xFF);
    bundle.push_back((v >> 24) & 0xFF);
  };

  writeU16(profileCount);
  writeU16(fixtureCount);
  writeU16(matrixCount);
  if (version >= 2) {
    writeU16(mdefCount);
  }

  // Universe table: 1 byte/entry (transport only) for v1/v2, 7 bytes/entry
  // (transport + destIp + wireUniverse) for v3 -- see show_bundle.h.
  for (int i = 0; i < universeCount; i++) {
    UniverseTransportEntry entry;
    if (universes.find(i) != universes.end()) {
      entry = universes[i];
    }
    writeU8(static_cast<uint8_t>(entry.transport));
    if (version == 3) {
      writeU32(entry.destIp);
      writeU16(entry.wireUniverse);
    }
  }

  // Profile table
  for (const auto& blob : profileBlobs) {
    writeU16(static_cast<uint16_t>(blob.size()));
    for (uint8_t b : blob) {
      writeU8(b);
    }
  }

  // Fixture table (46 bytes per entry)
  for (const auto& fi : fixtures) {
    uint16_t profileIndex = profileIndexMap[fi.defFile];
    const auto& def = defCache[fi.defFile];

    writeU16(profileIndex);
    writeU8(fi.universe);
    writeU16(fi.base);
    writeU8(def.isHead ? 1 : 0);

    writeF32(fi.posX);
    writeF32(fi.posY);
    writeF32(fi.posZ);
    writeF32(fi.yaw);
    writeF32(fi.pitch);
    writeF32(fi.roll);
    writeF32(def.panRangeDeg);
    writeF32(def.tiltRangeDeg);
    writeF32(fi.panCenterNorm);
    writeF32(fi.tiltCenterNorm);
    writeU8(fi.invertPan ? 1 : 0);
    writeU8(fi.invertTilt ? 1 : 0);
  }

  // Matrix table
  for (const auto& mi : matrices) {
    writeU16(mi.width);
    writeU16(mi.height);
    writeU8(mi.serpentine ? 1 : 0);
    writeU8(mi.vertical ? 1 : 0);
    writeU8(static_cast<uint8_t>(colorOrderFromString(mi.orderStr)));
    writeU8(mi.startUniverse);
    writeU16(mi.startChannel);
  }

  // Controller (mdef) table -- present (possibly empty) whenever version >= 2.
  for (const auto& blob : controllerBlobs) {
    writeU16(static_cast<uint16_t>(blob.size()));
    for (uint8_t b : blob) {
      writeU8(b);
    }
  }

  result.ok = true;
  result.bundle = bundle;
  return result;
}
