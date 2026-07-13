// fdef_check.cpp -- host CLI used by the importer test suite
// (web/shared/importers/test-importers.mjs) as the round-trip oracle for
// imported .fdef files.
//
// Runs the exact same three steps the provisioner WASM build runs on a
// .fdef (parseFixtureDef -> encodeProfile -> parseProfile), against the
// same C++ source (provision.cpp / fixture_profile.cpp) -- this binary is
// just compiled with g++ instead of emcc, since this sandbox has no
// emsdk. Prints the resulting FixtureProfile as JSON so a Node test can
// assert on it without needing a JSON library on the C++ side beyond hand
// -rolled string escaping (the shape is small and fixed).
//
// Usage: fdef_check <fixture.fdef>

#include "provision.h"
#include "fixture_profile.h"
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static std::string readFileFromDisk(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) return "";
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

static const char* capToName(Capability c) {
  switch (c) {
    case Capability::Dimmer: return "Dimmer";
    case Capability::Red: return "Red";
    case Capability::Green: return "Green";
    case Capability::Blue: return "Blue";
    case Capability::White: return "White";
    case Capability::Amber: return "Amber";
    case Capability::Uv: return "Uv";
    case Capability::Cyan: return "Cyan";
    case Capability::Magenta: return "Magenta";
    case Capability::Yellow: return "Yellow";
    case Capability::Pan: return "Pan";
    case Capability::Tilt: return "Tilt";
    case Capability::ShutterStrobe: return "ShutterStrobe";
    case Capability::Gobo: return "Gobo";
    case Capability::Focus: return "Focus";
    case Capability::Zoom: return "Zoom";
    case Capability::Fog: return "Fog";
    case Capability::Fan: return "Fan";
    case Capability::ColorWheel: return "ColorWheel";
    case Capability::GoboRotation: return "GoboRotation";
    case Capability::Prism: return "Prism";
    case Capability::PrismRotation: return "PrismRotation";
    case Capability::Frost: return "Frost";
    case Capability::Iris: return "Iris";
    case Capability::CTO: return "CTO";
    case Capability::AnimationWheel: return "AnimationWheel";
    case Capability::Macro: return "Macro";
    case Capability::Generic: return "Generic";
  }
  return "Unknown";
}

static void printJsonString(const std::string& s) {
  putchar('"');
  for (char c : s) {
    if (c == '"' || c == '\\') { putchar('\\'); putchar(c); }
    else if (c == '\n') { fputs("\\n", stdout); }
    else if (static_cast<unsigned char>(c) < 0x20) { printf("\\u%04x", c); }
    else putchar(c);
  }
  putchar('"');
}

static void printErr(const char* stage, const std::string& err) {
  printf("{\"ok\":false,\"stage\":\"%s\",\"err\":", stage);
  printJsonString(err);
  printf("}\n");
}

int main(int argc, char* argv[]) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <fixture.fdef>\n", argv[0]);
    return 1;
  }

  std::string text = readFileFromDisk(argv[1]);
  if (text.empty()) {
    printErr("read", std::string("cannot read ") + argv[1]);
    return 1;
  }

  FixtureDef def;
  std::string err;
  if (!parseFixtureDef(text, def, err)) {
    printErr("parseFixtureDef", err);
    return 1;
  }

  std::vector<uint8_t> blob = encodeProfile(def);

  FixtureProfile prof;
  if (!parseProfile(blob.data(), blob.size(), prof)) {
    printErr("parseProfile", "parseProfile rejected the blob encodeProfile just produced");
    return 1;
  }

  printf("{\"ok\":true,\"footprint\":%d,\"blobLen\":%zu,\"isHead\":%s,\"panRangeDeg\":%g,\"tiltRangeDeg\":%g,\"caps\":[",
         prof.footprint, blob.size(), def.isHead ? "true" : "false", def.panRangeDeg, def.tiltRangeDeg);
  for (int i = 0; i < prof.channelCount; i++) {
    const ChannelMap& c = prof.channels[i];
    if (i) putchar(',');
    printf("{\"cap\":\"%s\",\"coarse\":%d,\"fine\":%d,\"default\":%d,\"inverted\":%s,\"ranges\":[",
           capToName(c.cap), c.coarse, c.fine == 0xFF ? -1 : c.fine, c.defaultValue,
           (c.flags & 1) ? "true" : "false");
    bool firstRange = true;
    for (int r = 0; r < prof.rangeCount; r++) {
      const FunctionRange& fr = prof.ranges[r];
      if (fr.capIndex != i) continue;
      if (!firstRange) putchar(',');
      firstRange = false;
      printf("{\"from\":%d,\"to\":%d,\"continuous\":%s,\"name\":",
             fr.dmxFrom, fr.dmxTo, fr.continuous ? "true" : "false");
      if (fr.nameOff == 0xFFFF) {
        printf("null");
      } else {
        printJsonString(reinterpret_cast<const char*>(prof.rangeNameBlob) + fr.nameOff);
      }
      putchar('}');
    }
    printf("]}");
  }
  printf("]}\n");
  return 0;
}
