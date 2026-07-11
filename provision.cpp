#include "provision.h"
#include "profile_encoder.h"
#include "show_bundle.h"
#include <sstream>
#include <map>
#include <algorithm>
#include <cstring>

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

bool parseFixtureDef(const std::string& text, FixtureDef& out, std::string& err) {
  out = FixtureDef();
  err.clear();

  std::istringstream iss(text);
  std::string line;
  int lineNum = 0;

  while (std::getline(iss, line)) {
    lineNum++;
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
      try {
        int fp = std::stoi(tokens[1]);
        if (fp < 1 || fp > 255) {
          err = "FOOTPRINT: out of range (1..255)";
          return false;
        }
        out.footprint = static_cast<uint8_t>(fp);
      } catch (...) {
        err = "FOOTPRINT: invalid integer";
        return false;
      }
    } else if (cmd == "HEAD") {
      out.isHead = true;
    } else if (cmd == "PANRANGE") {
      if (tokens.size() < 2) {
        err = "PANRANGE: missing value";
        return false;
      }
      try {
        out.panRangeDeg = std::stof(tokens[1]);
      } catch (...) {
        err = "PANRANGE: invalid float";
        return false;
      }
    } else if (cmd == "TILTRANGE") {
      if (tokens.size() < 2) {
        err = "TILTRANGE: missing value";
        return false;
      }
      try {
        out.tiltRangeDeg = std::stof(tokens[1]);
      } catch (...) {
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

      try {
        coarse = static_cast<uint8_t>(std::stoi(tokens[2]));
      } catch (...) {
        err = "CAP: invalid coarse channel";
        return false;
      }

      size_t tokenIdx = 3;

      // Fine channel
      if (tokenIdx < tokens.size() && tokens[tokenIdx] != "inv") {
        if (tokens[tokenIdx] != "-") {
          try {
            fine = static_cast<uint8_t>(std::stoi(tokens[tokenIdx]));
          } catch (...) {
            err = "CAP: invalid fine channel";
            return false;
          }
        }
        tokenIdx++;
      }

      // Default value
      if (tokenIdx < tokens.size() && tokens[tokenIdx] != "inv") {
        try {
          def = static_cast<uint8_t>(std::stoi(tokens[tokenIdx]));
        } catch (...) {
          err = "CAP: invalid default value";
          return false;
        }
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
  for (const auto& cm : def.caps) {
    bool inv = (cm.flags & 1) != 0;
    builder.add(cm.cap, cm.coarse, cm.fine, cm.defaultValue, inv);
  }
  return builder.encode();
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
      uint8_t idx;
      try {
        idx = static_cast<uint8_t>(std::stoi(tokens[1]));
      } catch (...) {
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
      uint8_t univ;
      uint16_t baseAddr;

      try {
        univ = static_cast<uint8_t>(std::stoi(tokens[2]));
        baseAddr = static_cast<uint16_t>(std::stoi(tokens[3]));
      } catch (...) {
        result.err = "FIXTURE: invalid universe or base";
        return result;
      }

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

      try {
        lastFixture->posX = std::stof(tokens[1]);
        lastFixture->posY = std::stof(tokens[2]);
        lastFixture->posZ = std::stof(tokens[3]);
      } catch (...) {
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

      try {
        lastFixture->yaw = std::stof(tokens[1]);
        lastFixture->pitch = std::stof(tokens[2]);
        lastFixture->roll = std::stof(tokens[3]);
      } catch (...) {
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

      try {
        lastFixture->panCenterNorm = std::stof(tokens[1]);
        lastFixture->tiltCenterNorm = std::stof(tokens[2]);
      } catch (...) {
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

      try {
        int invPan = std::stoi(tokens[1]);
        int invTilt = std::stoi(tokens[2]);
        lastFixture->invertPan = (invPan != 0);
        lastFixture->invertTilt = (invTilt != 0);
      } catch (...) {
        result.err = "INVERT: invalid flags";
        return result;
      }

    } else if (cmd == "MATRIX") {
      if (tokens.size() < 8) {
        result.err = "MATRIX: missing parameters";
        return result;
      }

      MatrixInstance mi;
      try {
        mi.startUniverse = static_cast<uint8_t>(std::stoi(tokens[1]));
        mi.startChannel = static_cast<uint16_t>(std::stoi(tokens[2]));
        mi.width = static_cast<uint16_t>(std::stoi(tokens[3]));
        mi.height = static_cast<uint16_t>(std::stoi(tokens[4]));
      } catch (...) {
        result.err = "MATRIX: invalid numeric parameters";
        return result;
      }

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

  // Write SHW1 bundle
  std::vector<uint8_t> bundle;

  // Header
  bundle.push_back('S');
  bundle.push_back('H');
  bundle.push_back('W');
  bundle.push_back('1');
  bundle.push_back(1);  // version
  bundle.push_back(universeCount);

  uint16_t profileCount = static_cast<uint16_t>(profileBlobs.size());
  uint16_t fixtureCount = static_cast<uint16_t>(fixtures.size());
  uint16_t matrixCount = static_cast<uint16_t>(matrices.size());

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

  result.ok = true;
  result.bundle = bundle;
  return result;
}
