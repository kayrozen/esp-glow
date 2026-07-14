// mdef_parser.cpp — parse .mdef controller definition files.
//
// See mdef_parser.h for the format spec and rationale.

#include "mdef_parser.h"

#include <cctype>
#include <cstring>
#include <sstream>

namespace glow {
namespace mdef {

namespace {

// Trim whitespace from both ends of a string view.
std::string trim(const std::string& s) {
  size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
    start++;
  }
  if (start == s.size()) return "";
  size_t end = s.size() - 1;
  while (end > start && std::isspace(static_cast<unsigned char>(s[end]))) {
    end--;
  }
  return s.substr(start, end - start + 1);
}

// Split a line into tokens by whitespace.
std::vector<std::string> tokenize(const std::string& line) {
  std::vector<std::string> tokens;
  std::istringstream iss(line);
  std::string token;
  while (iss >> token) {
    tokens.push_back(token);
  }
  return tokens;
}

// Strip comments from a line (everything after '#').
std::string stripComment(const std::string& line) {
  size_t pos = line.find('#');
  if (pos == std::string::npos) return line;
  return line.substr(0, pos);
}

// Parse an integer from a token, with bounds checking.
bool parseInt(const std::string& token, int minVal, int maxVal, int& out, 
              char* errOut, size_t errCap) {
  char* endptr = nullptr;
  long val = std::strtol(token.c_str(), &endptr, 10);
  if (endptr == token.c_str() || *endptr != '\0') {
    if (errOut && errCap > 0) {
      std::snprintf(errOut, errCap, "invalid integer: %s", token.c_str());
    }
    return false;
  }
  if (val < minVal || val > maxVal) {
    if (errOut && errCap > 0) {
      std::snprintf(errOut, errCap, "value %ld out of range [%d, %d]", val, minVal, maxVal);
    }
    return false;
  }
  out = static_cast<int>(val);
  return true;
}

// Parse encoder mode from a string token.
bool parseEncoderMode(const std::string& token, EncoderMode& out,
                      char* errOut, size_t errCap) {
  if (token == "absolute") {
    out = EncoderMode::Absolute;
    return true;
  } else if (token == "relative-2c") {
    out = EncoderMode::Relative2C;
    return true;
  } else if (token == "relative-signmag") {
    out = EncoderMode::RelativeSignMag;
    return true;
  } else if (token == "relative-6365") {
    out = EncoderMode::Relative6365;
    return true;
  }
  if (errOut && errCap > 0) {
    std::snprintf(errOut, errCap, "unknown encoder mode: %s", token.c_str());
  }
  return false;
}

// Parse LED semantic from a string token.
bool parseLedSemantic(const std::string& token, LedSemantic& out,
                      char* errOut, size_t errCap) {
  if (token == "velocity") {
    out = LedSemantic::Velocity;
    return true;
  } else if (token == "value") {
    out = LedSemantic::Value;
    return true;
  }
  if (errOut && errCap > 0) {
    std::snprintf(errOut, errCap, "unknown LED semantic: %s", token.c_str());
  }
  return false;
}

}  // namespace

bool parseMdef(const char* src, size_t len, ControllerDef& out,
               char* errOut, size_t errCap) {
  out = ControllerDef();
  
  if (!src || len == 0) {
    if (errOut && errCap > 0) {
      std::snprintf(errOut, errCap, "empty or null source");
    }
    return false;
  }
  
  std::istringstream iss(std::string(src, len));
  std::string line;
  int lineNum = 0;
  
  LedDefinition* currentLed = nullptr;  // for multi-line LED blocks
  
  while (std::getline(iss, line)) {
    lineNum++;
    line = stripComment(line);
    line = trim(line);
    if (line.empty()) continue;
    
    std::vector<std::string> tokens = tokenize(line);
    if (tokens.empty()) continue;
    
    const std::string& cmd = tokens[0];
    
    // CONTROLLER <name...>
    if (cmd == "CONTROLLER") {
      if (tokens.size() < 2) {
        if (errOut && errCap > 0) {
          std::snprintf(errOut, errCap, "line %d: CONTROLLER requires a name", lineNum);
        }
        return false;
      }
      // Join remaining tokens as the controller name
      std::ostringstream nameStream;
      for (size_t i = 1; i < tokens.size(); i++) {
        if (i > 1) nameStream << " ";
        nameStream << tokens[i];
      }
      out.name = nameStream.str();
      currentLed = nullptr;
      continue;
    }
    
    // MIDI_CHANNEL <n>
    if (cmd == "MIDI_CHANNEL") {
      if (tokens.size() != 2) {
        if (errOut && errCap > 0) {
          std::snprintf(errOut, errCap, "line %d: MIDI_CHANNEL requires one value", lineNum);
        }
        return false;
      }
      int channel;
      if (!parseInt(tokens[1], 0, 15, channel, errOut, errCap)) {
        return false;
      }
      out.midiChannel = static_cast<uint8_t>(channel);
      currentLed = nullptr;
      continue;
    }
    
    // PAD <start> [end]
    if (cmd == "PAD") {
      if (tokens.size() < 2 || tokens.size() > 3) {
        if (errOut && errCap > 0) {
          std::snprintf(errOut, errCap, "line %d: PAD requires 1 or 2 values", lineNum);
        }
        return false;
      }
      int startId, endId;
      if (!parseInt(tokens[1], 0, 127, startId, errOut, errCap)) {
        return false;
      }
      if (tokens.size() == 3) {
        if (!parseInt(tokens[2], 0, 127, endId, errOut, errCap)) {
          return false;
        }
        if (endId < startId) {
          if (errOut && errCap > 0) {
            std::snprintf(errOut, errCap, "line %d: PAD end (%d) < start (%d)", lineNum, endId, startId);
          }
          return false;
        }
      } else {
        endId = startId;
      }
      out.controls.push_back(ControlElement{ControlType::Pad, 
                                            static_cast<uint16_t>(startId),
                                            static_cast<uint16_t>(endId),
                                            "",
                                            EncoderMode::Absolute});
      currentLed = nullptr;
      continue;
    }
    
    // FADER CC <start> [end] [name]
    if (cmd == "FADER") {
      if (tokens.size() < 3 || tokens.size() > 4) {
        if (errOut && errCap > 0) {
          std::snprintf(errOut, errCap, "line %d: FADER CC requires start/end and optional name", lineNum);
        }
        return false;
      }
      if (tokens[1] != "CC") {
        if (errOut && errCap > 0) {
          std::snprintf(errOut, errCap, "line %d: FADER requires CC keyword", lineNum);
        }
        return false;
      }
      int startId, endId;
      if (!parseInt(tokens[2], 0, 127, startId, errOut, errCap)) {
        return false;
      }
      if (tokens.size() >= 4) {
        // Check if tokens[3] is a number (end ID) or a name
        bool isNumber = true;
        for (char c : tokens[3]) {
          if (!std::isdigit(static_cast<unsigned char>(c))) {
            isNumber = false;
            break;
          }
        }
        if (isNumber) {
          if (!parseInt(tokens[3], 0, 127, endId, errOut, errCap)) {
            return false;
          }
          if (endId < startId) {
            if (errOut && errCap > 0) {
              std::snprintf(errOut, errCap, "line %d: FADER end (%d) < start (%d)", lineNum, endId, startId);
            }
            return false;
          }
        } else {
          endId = startId;
          // tokens[3] is the name
          out.controls.push_back(ControlElement{ControlType::Fader,
                                                static_cast<uint16_t>(startId),
                                                static_cast<uint16_t>(endId),
                                                tokens[3],
                                                EncoderMode::Absolute});
          currentLed = nullptr;
          continue;
        }
      } else {
        endId = startId;
      }
      out.controls.push_back(ControlElement{ControlType::Fader,
                                            static_cast<uint16_t>(startId),
                                            static_cast<uint16_t>(endId),
                                            "",
                                            EncoderMode::Absolute});
      currentLed = nullptr;
      continue;
    }
    
    // ENCODER CC <start> <end> <mode>
    if (cmd == "ENCODER") {
      if (tokens.size() != 5) {
        if (errOut && errCap > 0) {
          std::snprintf(errOut, errCap, "line %d: ENCODER CC requires start, end, and mode", lineNum);
        }
        return false;
      }
      if (tokens[1] != "CC") {
        if (errOut && errCap > 0) {
          std::snprintf(errOut, errCap, "line %d: ENCODER requires CC keyword", lineNum);
        }
        return false;
      }
      int startId, endId;
      if (!parseInt(tokens[2], 0, 127, startId, errOut, errCap)) {
        return false;
      }
      if (!parseInt(tokens[3], 0, 127, endId, errOut, errCap)) {
        return false;
      }
      if (endId < startId) {
        if (errOut && errCap > 0) {
          std::snprintf(errOut, errCap, "line %d: ENCODER end (%d) < start (%d)", lineNum, endId, startId);
        }
        return false;
      }
      EncoderMode mode;
      if (!parseEncoderMode(tokens[4], mode, errOut, errCap)) {
        return false;
      }
      out.controls.push_back(ControlElement{ControlType::Encoder,
                                            static_cast<uint16_t>(startId),
                                            static_cast<uint16_t>(endId),
                                            "",
                                            mode});
      currentLed = nullptr;
      continue;
    }
    
    // LED NOTE/CC <start> <end> <semantic>
    if (cmd == "LED") {
      if (tokens.size() != 5) {
        if (errOut && errCap > 0) {
          std::snprintf(errOut, errCap, "line %d: LED requires NOTE/CC, start, end, semantic", lineNum);
        }
        return false;
      }
      
      bool isNote;
      if (tokens[1] == "NOTE") {
        isNote = true;
      } else if (tokens[1] == "CC") {
        isNote = false;
      } else {
        if (errOut && errCap > 0) {
          std::snprintf(errOut, errCap, "line %d: LED requires NOTE or CC keyword", lineNum);
        }
        return false;
      }
      
      int startId, endId;
      if (!parseInt(tokens[2], 0, 127, startId, errOut, errCap)) {
        return false;
      }
      if (!parseInt(tokens[3], 0, 127, endId, errOut, errCap)) {
        return false;
      }
      if (endId < startId) {
        if (errOut && errCap > 0) {
          std::snprintf(errOut, errCap, "line %d: LED end (%d) < start (%d)", lineNum, endId, startId);
        }
        return false;
      }
      
      LedSemantic semantic;
      if (!parseLedSemantic(tokens[4], semantic, errOut, errCap)) {
        return false;
      }
      
      LedDefinition led;
      led.isNote = isNote;
      led.startId = static_cast<uint16_t>(startId);
      led.endId = static_cast<uint16_t>(endId);
      led.semantic = semantic;
      out.leds.push_back(led);
      currentLed = &out.leds.back();
      continue;
    }
    
    // COLOR <name> <value> (must follow an LED declaration)
    if (cmd == "COLOR") {
      if (!currentLed) {
        if (errOut && errCap > 0) {
          std::snprintf(errOut, errCap, "line %d: COLOR must follow an LED declaration", lineNum);
        }
        return false;
      }
      if (tokens.size() != 3) {
        if (errOut && errCap > 0) {
          std::snprintf(errOut, errCap, "line %d: COLOR requires name and value", lineNum);
        }
        return false;
      }
      int colorVal;
      if (!parseInt(tokens[2], 0, 127, colorVal, errOut, errCap)) {
        return false;
      }
      currentLed->colorPalette[tokens[1]] = static_cast<uint8_t>(colorVal);
      continue;
    }
    
    // Unknown command
    if (errOut && errCap > 0) {
      std::snprintf(errOut, errCap, "line %d: unknown command: %s", lineNum, cmd.c_str());
    }
    return false;
  }
  
  // Validate: must have a CONTROLLER name
  if (out.name.empty()) {
    if (errOut && errCap > 0) {
      std::snprintf(errOut, errCap, "missing CONTROLLER declaration");
    }
    return false;
  }
  
  return true;
}

const ControlElement* findControlById(const ControllerDef& def, uint16_t id) {
  for (const auto& ctrl : def.controls) {
    if (id >= ctrl.startId && id <= ctrl.endId) {
      return &ctrl;
    }
  }
  return nullptr;
}

const LedDefinition* findLedById(const ControllerDef& def, uint16_t id) {
  for (const auto& led : def.leds) {
    if (id >= led.startId && id <= led.endId) {
      return &led;
    }
  }
  return nullptr;
}

bool resolveColor(const LedDefinition& led, const std::string& name, uint8_t& valueOut) {
  auto it = led.colorPalette.find(name);
  if (it == led.colorPalette.end()) {
    return false;
  }
  valueOut = it->second;
  return true;
}

}  // namespace mdef
}  // namespace glow
