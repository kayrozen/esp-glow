#include "provision.h"
#include "profile_encoder.h"
#include "show_bundle.h"
#include <sstream>
#include <map>
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
      if (tokens.size() >= 3 && !parseIntToken(tokens[2], to)) { err = "PAD: invalid note number"; return false; }
      if (from < 0 || from > 127 || to < 0 || to > 127) { err = "PAD: note out of range (0..127)"; return false; }
      if (from > to) { err = "PAD: from must be <= to"; return false; }
      out.pads.push_back({static_cast<uint8_t>(from), static_cast<uint8_t>(to)});
    } else if (cmd == "FADER") {
      if (tokens.size() < 3 || tokens[1] != "CC") {
        err = "FADER: expected 'CC <from> [<to>] [name]'";
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
      std::string fname;
      for (; idx < tokens.size(); ++idx) {
        if (!fname.empty()) fname += " ";
        fname += tokens[idx];
      }
      out.faders.push_back({static_cast<uint8_t>(from), static_cast<uint8_t>(to), fname});
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
        err = "LED: expected 'NOTE|CC <from> <to> velocity|value'";
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

      ControllerLedSpec spec;
      spec.msgType = msgType;
      spec.addrFrom = static_cast<uint8_t>(from);
      spec.addrTo = static_cast<uint8_t>(to);
      spec.semantic = semantic;
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

struct UniverseTransportEntry {
  UniverseTransport transport = UniverseTransport::Unused;
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

  uint8_t maxUniverse = 0;
  FixtureInstance* lastFixture = nullptr;

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
      uint8_t idx = static_cast<uint8_t>(idxInt);

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

      if (idx >= 8) {
        result.err = "UNIVERSE: index out of range (0..7)";
        return result;
      }

      universes[idx].transport = transport;
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
      uint8_t univ = static_cast<uint8_t>(univInt);
      uint16_t baseAddr = static_cast<uint16_t>(baseInt);

      if (univ >= 8) {
        result.err = "FIXTURE: universe out of range (0..7)";
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

      FixtureInstance fi;
      fi.defFile = defFile;
      fi.universe = univ;
      fi.base = baseAddr;

      fixtures.push_back(fi);
      lastFixture = &fixtures.back();

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
      mi.startUniverse = static_cast<uint8_t>(startUniverseInt);
      mi.startChannel = static_cast<uint16_t>(startChannelInt);
      mi.width = static_cast<uint16_t>(widthInt);
      mi.height = static_cast<uint16_t>(heightInt);

      mi.serpentine = (tokens[5] == "SERP");
      mi.vertical = (tokens[6] == "V");
      mi.orderStr = tokens[7];

      if (mi.startUniverse >= 8) {
        result.err = "MATRIX: startUniverse out of range";
        return result;
      }
      if (maxUniverse < mi.startUniverse) {
        maxUniverse = mi.startUniverse;
      }

      matrices.push_back(mi);

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
  // only when at least one CONTROLLER was compiled -- byte-identical to the
  // original v1 layout otherwise, same "only bump the version once the new
  // feature is actually used" convention as ProfileBuilder::encode's PFX2.
  std::vector<uint8_t> bundle;
  uint8_t version = controllerBlobs.empty() ? 1 : 2;

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

  writeU16(profileCount);
  writeU16(fixtureCount);
  writeU16(matrixCount);
  if (version == 2) {
    writeU16(mdefCount);
  }

  // Universe table
  for (int i = 0; i < universeCount; i++) {
    UniverseTransport transport = UniverseTransport::Unused;
    if (universes.find(i) != universes.end()) {
      transport = universes[i].transport;
    }
    writeU8(static_cast<uint8_t>(transport));
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

  // Controller (mdef) table -- v2 only.
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
