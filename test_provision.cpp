#include "provision.h"
#include "show_bundle.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <cmath>

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
  printf("Running test_" #name "... "); \
  test_##name(); \
  printf("PASS\n"); \
  testsPassed++; \
} while(0)
#define CHECK(cond) do { \
  if (!(cond)) { \
    printf("FAIL: %s at line %d\n", #cond, __LINE__); \
    testsFailed++; \
    return; \
  } \
} while(0)

// ============================================================================
// Fixture Definition Tests
// ============================================================================

TEST(parse_fdef_basic) {
  std::string fdefText = R"(
    FIXTURE Torrent F1
    FOOTPRINT 16
    HEAD
    PANRANGE 540
    TILTRANGE 270
    CAP Dimmer 0
    CAP Red 1
    CAP Pan 5 6
    CAP ShutterStrobe 10 - 8 inv
  )";

  FixtureDef def;
  std::string err;
  CHECK(parseFixtureDef(fdefText, def, err));
  CHECK(def.footprint == 16);
  CHECK(def.isHead);
  CHECK(std::abs(def.panRangeDeg - 540.0f) < 0.01f);
  CHECK(std::abs(def.tiltRangeDeg - 270.0f) < 0.01f);

  // Check capabilities
  CHECK(def.caps.size() == 4);
  CHECK(def.caps[0].cap == Capability::Dimmer);
  CHECK(def.caps[0].coarse == 0);
  CHECK(def.caps[0].fine == 0xFF);

  CHECK(def.caps[1].cap == Capability::Red);
  CHECK(def.caps[1].coarse == 1);

  CHECK(def.caps[2].cap == Capability::Pan);
  CHECK(def.caps[2].coarse == 5);
  CHECK(def.caps[2].fine == 6);

  CHECK(def.caps[3].cap == Capability::ShutterStrobe);
  CHECK(def.caps[3].coarse == 10);
  CHECK(def.caps[3].fine == 0xFF);  // "-" means 8-bit only
  CHECK(def.caps[3].defaultValue == 8);
  CHECK((def.caps[3].flags & 1) == 1);  // inverted
}

TEST(capFromName_valid) {
  Capability cap;
  CHECK(capFromName("Dimmer", cap) && cap == Capability::Dimmer);
  CHECK(capFromName("ShutterStrobe", cap) && cap == Capability::ShutterStrobe);
  CHECK(capFromName("Pan", cap) && cap == Capability::Pan);
  CHECK(capFromName("Generic", cap) && cap == Capability::Generic);
}

TEST(capFromName_invalid) {
  Capability cap;
  CHECK(!capFromName("InvalidCap", cap));
  CHECK(!capFromName("", cap));
}

TEST(parse_fdef_missing_footprint) {
  std::string fdefText = R"(
    FIXTURE Simple
    CAP Dimmer 0
  )";

  FixtureDef def;
  std::string err;
  CHECK(!parseFixtureDef(fdefText, def, err));
  CHECK(!err.empty());
}

TEST(parse_fdef_bad_footprint) {
  std::string fdefText = R"(
    FIXTURE Test
    FOOTPRINT 256
    CAP Dimmer 0
  )";

  FixtureDef def;
  std::string err;
  CHECK(!parseFixtureDef(fdefText, def, err));
  CHECK(!err.empty());
}

// Regression test: a genuinely non-numeric token (a typo, or an importer's
// malformed QLC+/GDTF translation) -- not just a numerically valid but
// out-of-range value like parse_fdef_bad_footprint's 256 above. This is
// exactly the case that used to reach std::stoi's exception path: fine
// with exceptions enabled (host), but an uncatchable abort under
// Emscripten's default (no -fexceptions) WASM build, since a compiler
// whose entire job is turning user typos into clean error messages cannot
// depend on exceptions anywhere text gets parsed. Also exercises PANRANGE
// (float) and CAP's three integer fields for the same reason.
TEST(parse_fdef_footprint_non_numeric) {
  std::string fdefText = R"(
    FIXTURE Test
    FOOTPRINT abc
    CAP Dimmer 0
  )";

  FixtureDef def;
  std::string err;
  CHECK(!parseFixtureDef(fdefText, def, err));
  CHECK(!err.empty());
}

TEST(parse_fdef_panrange_non_numeric) {
  std::string fdefText = R"(
    FIXTURE Test
    FOOTPRINT 16
    HEAD
    PANRANGE not-a-number
    CAP Dimmer 0
  )";

  FixtureDef def;
  std::string err;
  CHECK(!parseFixtureDef(fdefText, def, err));
  CHECK(!err.empty());
}

TEST(parse_fdef_cap_coarse_non_numeric) {
  std::string fdefText = R"(
    FIXTURE Test
    FOOTPRINT 16
    CAP Dimmer xyz
  )";

  FixtureDef def;
  std::string err;
  CHECK(!parseFixtureDef(fdefText, def, err));
  CHECK(!err.empty());
}

TEST(parse_fdef_cap_fine_non_numeric) {
  std::string fdefText = R"(
    FIXTURE Test
    FOOTPRINT 16
    CAP Pan 5 notanumber
  )";

  FixtureDef def;
  std::string err;
  CHECK(!parseFixtureDef(fdefText, def, err));
  CHECK(!err.empty());
}

TEST(parse_fdef_cap_default_non_numeric) {
  std::string fdefText = R"(
    FIXTURE Test
    FOOTPRINT 16
    CAP ShutterStrobe 10 - notanumber
  )";

  FixtureDef def;
  std::string err;
  CHECK(!parseFixtureDef(fdefText, def, err));
  CHECK(!err.empty());
}

// Same class of bug, in .show's compileShow -- POS/ROT/CENTER/INVERT/MATRIX
// all parse user-typed numeric fields the same way FOOTPRINT/CAP did.
TEST(show_pos_non_numeric_coordinate) {
  std::map<std::string, std::string> fileMap;
  fileMap["head.fdef"] = R"(
    FIXTURE Head
    FOOTPRINT 8
    HEAD
    CAP Pan 0 1
  )";
  std::string showText = R"(
    SHOW 2
    UNIVERSE 1 DMX
    FIXTURE head.fdef 1 1
    POS 1 not-a-number 3
  )";
  auto readFile = [&](const std::string& name) {
    auto it = fileMap.find(name);
    return it != fileMap.end() ? it->second : std::string();
  };

  CompileResult result = compileShow(showText, readFile);
  CHECK(!result.ok);
  CHECK(!result.err.empty());
}

TEST(show_matrix_non_numeric_param) {
  std::string showText = R"(
    SHOW 2
    UNIVERSE 1 DMX
    MATRIX 1 abc 16 8 SERP H RGB
  )";
  auto readFile = [](const std::string&) { return std::string(); };

  CompileResult result = compileShow(showText, readFile);
  CHECK(!result.ok);
  CHECK(!result.err.empty());
}

TEST(capFromName_v2_valid) {
  Capability cap;
  CHECK(capFromName("ColorWheel", cap) && cap == Capability::ColorWheel);
  CHECK(capFromName("GoboRotation", cap) && cap == Capability::GoboRotation);
  CHECK(capFromName("Prism", cap) && cap == Capability::Prism);
  CHECK(capFromName("PrismRotation", cap) && cap == Capability::PrismRotation);
  CHECK(capFromName("Frost", cap) && cap == Capability::Frost);
  CHECK(capFromName("Iris", cap) && cap == Capability::Iris);
  CHECK(capFromName("CTO", cap) && cap == Capability::CTO);
  CHECK(capFromName("AnimationWheel", cap) && cap == Capability::AnimationWheel);
  CHECK(capFromName("Macro", cap) && cap == Capability::Macro);
}

TEST(parse_fdef_with_ranges) {
  std::string fdefText = R"(
    FIXTURE Lyre
    FOOTPRINT 2
    CAP ColorWheel 0
      SLOT 0 9    open
      SLOT 10 19  red
      SLOT 20 29  blue
    CAP ShutterStrobe 1
      SLOT 0 31   closed
      RANGE 32 63 strobe
  )";

  FixtureDef def;
  std::string err;
  CHECK(parseFixtureDef(fdefText, def, err));
  CHECK(def.caps.size() == 2);
  CHECK(def.ranges.size() == 5);

  CHECK(def.ranges[0].capIndex == 0);
  CHECK(def.ranges[0].dmxFrom == 0);
  CHECK(def.ranges[0].dmxTo == 9);
  CHECK(!def.ranges[0].continuous);
  CHECK(def.ranges[0].name == "open");
  CHECK(def.ranges[1].name == "red");
  CHECK(def.ranges[2].name == "blue");

  CHECK(def.ranges[3].capIndex == 1);
  CHECK(def.ranges[3].name == "closed");
  CHECK(!def.ranges[3].continuous);

  CHECK(def.ranges[4].capIndex == 1);
  CHECK(def.ranges[4].dmxFrom == 32);
  CHECK(def.ranges[4].dmxTo == 63);
  CHECK(def.ranges[4].continuous);
  CHECK(def.ranges[4].name == "strobe");
}

TEST(parse_fdef_cap_without_ranges_stays_linear) {
  std::string fdefText = R"(
    FIXTURE Par
    FOOTPRINT 4
    CAP Dimmer 0
    CAP Red 1
  )";

  FixtureDef def;
  std::string err;
  CHECK(parseFixtureDef(fdefText, def, err));
  CHECK(def.caps.size() == 2);
  CHECK(def.ranges.empty());
}

TEST(parse_fdef_slot_before_cap_error) {
  std::string fdefText = R"(
    FIXTURE Bad
    FOOTPRINT 1
    SLOT 0 9 open
    CAP Dimmer 0
  )";

  FixtureDef def;
  std::string err;
  CHECK(!parseFixtureDef(fdefText, def, err));
  CHECK(!err.empty());
}

TEST(parse_fdef_slot_bad_range) {
  std::string fdefText = R"(
    FIXTURE Bad
    FOOTPRINT 1
    CAP Dimmer 0
      SLOT 50 10 backwards
  )";

  FixtureDef def;
  std::string err;
  CHECK(!parseFixtureDef(fdefText, def, err));
  CHECK(!err.empty());
}

TEST(encode_profile_with_ranges) {
  std::string fdefText = R"(
    FIXTURE Lyre
    FOOTPRINT 2
    CAP ColorWheel 0
      SLOT 0 9   open
      SLOT 10 19 red
    CAP ShutterStrobe 1
      RANGE 32 63 strobe
  )";

  FixtureDef def;
  std::string err;
  CHECK(parseFixtureDef(fdefText, def, err));

  auto blob = encodeProfile(def);
  CHECK(blob[4] == 2);  // ranges present -> v2

  FixtureProfile parsed;
  CHECK(parseProfile(blob.data(), blob.size(), parsed));
  CHECK(parsed.rangeCount == 3);
  CHECK(rangeCount(parsed, Capability::ColorWheel) == 2);
  CHECK(std::strcmp(rangeName(parsed, Capability::ColorWheel, 1), "red") == 0);
  CHECK(rangeIsContinuous(parsed, Capability::ShutterStrobe, 0));
}

TEST(parse_fdef_unknown_cap) {
  std::string fdefText = R"(
    FIXTURE Test
    FOOTPRINT 10
    CAP UnknownCap 0
  )";

  FixtureDef def;
  std::string err;
  CHECK(!parseFixtureDef(fdefText, def, err));
  CHECK(!err.empty());
}

TEST(encode_profile) {
  std::string fdefText = R"(
    FIXTURE Par
    FOOTPRINT 5
    CAP Dimmer 0
    CAP Red 1
    CAP Green 2
    CAP Blue 3
  )";

  FixtureDef def;
  std::string err;
  CHECK(parseFixtureDef(fdefText, def, err));

  auto blob = encodeProfile(def);
  CHECK(!blob.empty());

  // Parse the blob back and verify
  FixtureProfile parsed;
  CHECK(parseProfile(blob.data(), blob.size(), parsed));
  CHECK(parsed.footprint == 5);
  CHECK(parsed.channelCount >= 4);
}

// ============================================================================
// Show Compilation and Load Round-Trip Tests
// ============================================================================

TEST(show_compile_and_load_roundtrip) {
  // Create a fixture file map
  std::map<std::string, std::string> fileMap;

  fileMap["torrent.fdef"] = R"(
    FIXTURE Torrent F1
    FOOTPRINT 16
    HEAD
    PANRANGE 540
    TILTRANGE 270
    CAP Dimmer 0
    CAP Red 1
  )";

  fileMap["par.fdef"] = R"(
    FIXTURE Par
    FOOTPRINT 5
    CAP Dimmer 0
    CAP Red 1
  )";

  std::string showText = R"(
    SHOW 2
    UNIVERSE 1 DMX
    UNIVERSE 2 ARTNET
    FIXTURE torrent.fdef 1 2
    POS 1 2 3
    ROT 0 0 0
    CENTER 0.5 0.5
    INVERT 0 0
    FIXTURE par.fdef 1 21
    MATRIX 2 1 16 16 SERP H GRB
  )";

  auto readFile = [&](const std::string& name) {
    auto it = fileMap.find(name);
    if (it != fileMap.end()) return it->second;
    return std::string();
  };

  CompileResult result = compileShow(showText, readFile);
  CHECK(result.ok);
  CHECK(!result.bundle.empty());

  // Load the bundle
  LoadedShow loaded;
  CHECK(loadShow(result.bundle.data(), result.bundle.size(), loaded));

  // Verify universe setup
  CHECK(loaded.universeCount == 2);
  CHECK(loaded.transport[0] == UniverseTransport::Dmx);
  CHECK(loaded.transport[1] == UniverseTransport::ArtNet);

  // Verify fixtures
  CHECK(loaded.fixtures.size() == 2);

  // First fixture (head)
  CHECK(loaded.fixtures[0].isHead);
  CHECK(loaded.fixtures[0].universe == 0);
  CHECK(loaded.fixtures[0].base == 1);
  CHECK(loaded.fixtures[0].profile.footprint == 16);
  CHECK(std::abs(loaded.fixtures[0].head.position.x - 1.0f) < 0.01f);
  CHECK(std::abs(loaded.fixtures[0].head.position.y - 2.0f) < 0.01f);
  CHECK(std::abs(loaded.fixtures[0].head.position.z - 3.0f) < 0.01f);
  CHECK(std::abs(loaded.fixtures[0].head.panRangeDeg - 540.0f) < 0.01f);
  CHECK(std::abs(loaded.fixtures[0].head.tiltRangeDeg - 270.0f) < 0.01f);

  // Verify orientation is ~identity for ROT 0 0 0
  // mat3FromEuler(0, 0, 0) should give identity matrix
  CHECK(std::abs(loaded.fixtures[0].head.orientation.m[0] - 1.0f) < 0.01f);
  CHECK(std::abs(loaded.fixtures[0].head.orientation.m[4] - 1.0f) < 0.01f);
  CHECK(std::abs(loaded.fixtures[0].head.orientation.m[8] - 1.0f) < 0.01f);

  // Second fixture (not head)
  CHECK(!loaded.fixtures[1].isHead);
  CHECK(loaded.fixtures[1].universe == 0);
  CHECK(loaded.fixtures[1].base == 20);

  // Verify matrix
  CHECK(loaded.matrices.size() == 1);
  CHECK(loaded.matrices[0].width == 16);
  CHECK(loaded.matrices[0].height == 16);
  CHECK(loaded.matrices[0].serpentine);
  CHECK(!loaded.matrices[0].vertical);
  CHECK(loaded.matrices[0].order == ColorOrder::GRB);
  CHECK(loaded.matrices[0].startUniverse == 1);
  CHECK(loaded.matrices[0].startChannel == 0);
}

TEST(show_profile_deduplication) {
  std::map<std::string, std::string> fileMap;

  fileMap["par.fdef"] = R"(
    FIXTURE Par
    FOOTPRINT 5
    CAP Dimmer 0
  )";

  std::string showText = R"(
    SHOW 2
    UNIVERSE 1 DMX
    FIXTURE par.fdef 1 1
    FIXTURE par.fdef 1 11
  )";

  auto readFile = [&](const std::string& name) {
    auto it = fileMap.find(name);
    if (it != fileMap.end()) return it->second;
    return std::string();
  };

  CompileResult result = compileShow(showText, readFile);
  CHECK(result.ok);

  LoadedShow loaded;
  CHECK(loadShow(result.bundle.data(), result.bundle.size(), loaded));

  // Both fixtures should reference the same profile in the bundle
  // Verify they have the same profile (by checking footprint)
  CHECK(loaded.fixtures[0].profile.footprint == loaded.fixtures[1].profile.footprint);
}

TEST(show_pos_without_head_error) {
  std::map<std::string, std::string> fileMap;

  fileMap["par.fdef"] = R"(
    FIXTURE Par
    FOOTPRINT 5
    CAP Dimmer 0
  )";

  std::string showText = R"(
    SHOW 2
    UNIVERSE 1 DMX
    FIXTURE par.fdef 1 1
    POS 1 2 3
  )";

  auto readFile = [&](const std::string& name) {
    auto it = fileMap.find(name);
    if (it != fileMap.end()) return it->second;
    return std::string();
  };

  CompileResult result = compileShow(showText, readFile);
  CHECK(!result.ok);
  CHECK(!result.err.empty());
}

// ============================================================================
// .mdef Parsing Tests
// ============================================================================

TEST(parse_mdef_basic) {
  std::string text = R"(
    CONTROLLER Akai APC40 mkII
    MIDI_CHANNEL 1
    PAD  53 60
    PAD  0
    FADER CC 48 55
    FADER CC 7   master
    ENCODER CC 16 23 relative-2c
    LED NOTE 53 60 velocity
      COLOR off    0
      COLOR green  1
      COLOR red    3
    LED CC 48 55 value
  )";

  ControllerBuilder def;
  std::string err;
  CHECK(parseControllerDef(text, def, err));
  CHECK(err.empty());

  CHECK(def.name == "Akai APC40 mkII");
  CHECK(def.midiChannel == 1);

  CHECK(def.pads.size() == 2);
  CHECK(def.pads[0].noteFrom == 53 && def.pads[0].noteTo == 60);
  CHECK(def.pads[1].noteFrom == 0 && def.pads[1].noteTo == 0);

  CHECK(def.faders.size() == 2);
  CHECK(def.faders[0].ccFrom == 48 && def.faders[0].ccTo == 55);
  CHECK(def.faders[0].name.empty());
  CHECK(def.faders[1].ccFrom == 7 && def.faders[1].ccTo == 7);
  CHECK(def.faders[1].name == "master");

  CHECK(def.encoders.size() == 1);
  CHECK(def.encoders[0].ccFrom == 16 && def.encoders[0].ccTo == 23);
  CHECK(def.encoders[0].mode == EncoderMode::Relative2C);

  CHECK(def.leds.size() == 2);
  CHECK(def.leds[0].msgType == LedMsgType::Note);
  CHECK(def.leds[0].addrFrom == 53 && def.leds[0].addrTo == 60);
  CHECK(def.leds[0].semantic == LedSemantic::Velocity);
  CHECK(def.leds[0].colors.size() == 3);
  CHECK(def.leds[0].colors[0].name == "off" && def.leds[0].colors[0].value == 0);
  CHECK(def.leds[0].colors[1].name == "green" && def.leds[0].colors[1].value == 1);
  CHECK(def.leds[0].colors[2].name == "red" && def.leds[0].colors[2].value == 3);

  CHECK(def.leds[1].msgType == LedMsgType::Cc);
  CHECK(def.leds[1].semantic == LedSemantic::Value);
  CHECK(def.leds[1].colors.empty());

  // Encode + parse round-trip through the runtime (device-side) type too.
  std::string encErr;
  std::vector<uint8_t> blob = encodeController(def, encErr);
  CHECK(encErr.empty());
  MidiControllerProfile p;
  CHECK(parseMidiController(blob.data(), blob.size(), p));
  CHECK(p.padCount == 2);
  CHECK(p.ledCount == 2);
  CHECK(p.colorCount == 3);
}

TEST(parse_mdef_encoder_default_mode_absolute) {
  std::string text = R"(
    CONTROLLER Generic
    ENCODER CC 16 23
  )";
  ControllerBuilder def;
  std::string err;
  CHECK(parseControllerDef(text, def, err));
  CHECK(def.encoders.size() == 1);
  CHECK(def.encoders[0].mode == EncoderMode::Absolute);
}

TEST(parse_mdef_color_before_led_error) {
  std::string text = R"(
    CONTROLLER Generic
    COLOR red 3
  )";
  ControllerBuilder def;
  std::string err;
  CHECK(!parseControllerDef(text, def, err));
  CHECK(!err.empty());
}

TEST(parse_mdef_missing_name_error) {
  std::string text = R"(
    PAD 0
  )";
  ControllerBuilder def;
  std::string err;
  CHECK(!parseControllerDef(text, def, err));
  CHECK(!err.empty());
}

TEST(parse_mdef_unknown_encoder_mode_error) {
  std::string text = R"(
    CONTROLLER Generic
    ENCODER CC 16 23 sideways
  )";
  ControllerBuilder def;
  std::string err;
  CHECK(!parseControllerDef(text, def, err));
}

// ============================================================================
// .show CONTROLLER / SHW1 v2 Tests
// ============================================================================

TEST(show_compile_and_load_roundtrip_with_controller) {
  std::map<std::string, std::string> fileMap;

  fileMap["par.fdef"] = R"(
    FIXTURE Par
    FOOTPRINT 5
    CAP Dimmer 0
  )";

  fileMap["apc.mdef"] = R"(
    CONTROLLER Akai APC40 mkII
    PAD 53 60
    LED NOTE 53 60 velocity
      COLOR off   0
      COLOR green 1
  )";

  std::string showText = R"(
    SHOW 2
    UNIVERSE 1 DMX
    FIXTURE par.fdef 1 1
    CONTROLLER apc.mdef
  )";

  auto readFile = [&](const std::string& name) {
    auto it = fileMap.find(name);
    if (it != fileMap.end()) return it->second;
    return std::string();
  };

  CompileResult result = compileShow(showText, readFile);
  CHECK(result.ok);
  CHECK(!result.bundle.empty());
  CHECK(result.bundle[4] == 2);  // version bumped once a CONTROLLER is present

  LoadedShow loaded;
  CHECK(loadShow(result.bundle.data(), result.bundle.size(), loaded));

  CHECK(loaded.fixtures.size() == 1);  // fixture table untouched by the new section
  CHECK(loaded.controllers.size() == 1);

  const MidiControllerProfile& ctrl = loaded.controllers[0];
  CHECK(ctrl.padCount == 1);
  CHECK(ctrl.pads[0].noteFrom == 53 && ctrl.pads[0].noteTo == 60);
  CHECK(ctrl.ledCount == 1);
  CHECK(ctrl.leds[0].colorCount == 2);

  uint8_t value = 255;
  CHECK(ledColorValueByName(ctrl, ctrl.leds[0], "green", value));
  CHECK(value == 1);
}

TEST(show_compile_no_controller_stays_v1) {
  std::map<std::string, std::string> fileMap;
  fileMap["par.fdef"] = R"(
    FIXTURE Par
    FOOTPRINT 5
    CAP Dimmer 0
  )";
  std::string showText = R"(
    SHOW 2
    UNIVERSE 1 DMX
    FIXTURE par.fdef 1 1
  )";
  auto readFile = [&](const std::string& name) {
    auto it = fileMap.find(name);
    if (it != fileMap.end()) return it->second;
    return std::string();
  };

  CompileResult result = compileShow(showText, readFile);
  CHECK(result.ok);
  CHECK(result.bundle[4] == 1);  // no CONTROLLER -> byte-identical v1 layout

  LoadedShow loaded;
  CHECK(loadShow(result.bundle.data(), result.bundle.size(), loaded));
  CHECK(loaded.controllers.empty());
}

TEST(show_controller_missing_deffile_error) {
  std::string showText = R"(
    SHOW 2
    UNIVERSE 1 DMX
    CONTROLLER missing.mdef
  )";
  auto readFile = [&](const std::string&) { return std::string(); };

  CompileResult result = compileShow(showText, readFile);
  CHECK(!result.ok);
  CHECK(!result.err.empty());
}

// ============================================================================
// .show WLED / SHW1 v3 Tests
// ============================================================================

TEST(show_compile_and_load_roundtrip_with_wled) {
  std::string showText = R"(
    SHOW 2
    UNIVERSE 1 DMX
    WLED "main_matrix" "192.168.1.100" 1
    WLED "christmas_tree" "192.168.1.101" 3
    WLED broadcast_zone 255.255.255.255
  )";
  auto readFile = [&](const std::string&) { return std::string(); };

  CompileResult result = compileShow(showText, readFile);
  CHECK(result.ok);
  CHECK(!result.bundle.empty());
  CHECK(result.bundle[4] == 3);  // version bumped once a WLED target is present

  LoadedShow loaded;
  CHECK(loadShow(result.bundle.data(), result.bundle.size(), loaded));
  CHECK(loaded.wledTargets.size() == 3);

  CHECK(loaded.wledTargets[0].name == "main_matrix");
  CHECK(loaded.wledTargets[0].ip == "192.168.1.100");
  CHECK(loaded.wledTargets[0].port == WLED_DEFAULT_PORT);
  CHECK(loaded.wledTargets[0].syncGroup == 1);

  CHECK(loaded.wledTargets[1].name == "christmas_tree");
  CHECK(loaded.wledTargets[1].syncGroup == 3);

  // Unquoted tokens work exactly like quoted ones (stripQuotes is a no-op
  // when there are no surrounding quotes -- see provision.cpp).
  CHECK(loaded.wledTargets[2].name == "broadcast_zone");
  CHECK(loaded.wledTargets[2].ip == "255.255.255.255");
  CHECK(loaded.wledTargets[2].syncGroup == 1);  // default
}

TEST(show_compile_and_load_roundtrip_with_wled_and_controller) {
  std::map<std::string, std::string> fileMap;
  fileMap["apc.mdef"] = R"(
    CONTROLLER Akai APC40 mkII
    PAD 53 60
  )";
  std::string showText = R"(
    SHOW 2
    UNIVERSE 1 DMX
    CONTROLLER apc.mdef
    WLED "tree" "192.168.1.101" 1
  )";
  auto readFile = [&](const std::string& name) {
    auto it = fileMap.find(name);
    if (it != fileMap.end()) return it->second;
    return std::string();
  };

  CompileResult result = compileShow(showText, readFile);
  CHECK(result.ok);
  CHECK(result.bundle[4] == 3);  // WLED present -> v3, which still carries mdefCount

  LoadedShow loaded;
  CHECK(loadShow(result.bundle.data(), result.bundle.size(), loaded));
  CHECK(loaded.controllers.size() == 1);
  CHECK(loaded.wledTargets.size() == 1);
  CHECK(loaded.wledTargets[0].name == "tree");
}

TEST(show_compile_no_wled_stays_v1) {
  std::string showText = R"(
    SHOW 2
    UNIVERSE 1 DMX
  )";
  auto readFile = [&](const std::string&) { return std::string(); };

  CompileResult result = compileShow(showText, readFile);
  CHECK(result.ok);
  CHECK(result.bundle[4] == 1);  // no CONTROLLER/WLED -> byte-identical v1 layout

  LoadedShow loaded;
  CHECK(loadShow(result.bundle.data(), result.bundle.size(), loaded));
  CHECK(loaded.wledTargets.empty());
}

TEST(show_wled_duplicate_name_error) {
  std::string showText = R"(
    SHOW 2
    UNIVERSE 1 DMX
    WLED "tree" "192.168.1.101"
    WLED "tree" "192.168.1.102"
  )";
  auto readFile = [&](const std::string&) { return std::string(); };

  CompileResult result = compileShow(showText, readFile);
  CHECK(!result.ok);
  CHECK(!result.err.empty());
}

TEST(show_wled_missing_ip_error) {
  std::string showText = R"(
    SHOW 2
    UNIVERSE 1 DMX
    WLED "tree"
  )";
  auto readFile = [&](const std::string&) { return std::string(); };

  CompileResult result = compileShow(showText, readFile);
  CHECK(!result.ok);
  CHECK(!result.err.empty());
}

TEST(show_wled_syncgroup_out_of_range_error) {
  std::string showText = R"(
    SHOW 2
    UNIVERSE 1 DMX
    WLED "tree" "192.168.1.101" 9
  )";
  auto readFile = [&](const std::string&) { return std::string(); };

  CompileResult result = compileShow(showText, readFile);
  CHECK(!result.ok);
  CHECK(!result.err.empty());
}

// ============================================================================
// SHOW 2: 1-indexed addressing + migration + validation
// ============================================================================

// The conversion, proven: a 1-indexed address/universe in the text becomes
// the internal 0-indexed base/universe (PatchEntry.base = address - 1,
// universe index = universe - 1).
TEST(show_v2_address_and_universe_convert_to_0_indexed) {
  std::map<std::string, std::string> fileMap;
  fileMap["par.fdef"] = R"(
    FIXTURE Par
    FOOTPRINT 5
    CAP Dimmer 0
  )";
  std::string showText = R"(
    SHOW 2
    UNIVERSE 1 DMX
    FIXTURE par.fdef 1 17
  )";
  auto readFile = [&](const std::string& name) {
    auto it = fileMap.find(name);
    return it != fileMap.end() ? it->second : std::string();
  };

  CompileResult result = compileShow(showText, readFile);
  CHECK(result.ok);

  LoadedShow loaded;
  CHECK(loadShow(result.bundle.data(), result.bundle.size(), loaded));
  CHECK(loaded.fixtures.size() == 1);
  CHECK(loaded.fixtures[0].universe == 0);
  CHECK(loaded.fixtures[0].base == 16);
}

// A .show with no 'SHOW 2' header must fail loudly -- no silent
// reinterpretation of what used to be a valid 0-indexed file.
TEST(show_missing_header_error_names_migration) {
  std::string showText = R"(
    UNIVERSE 0 DMX
    FIXTURE par.fdef 0 0
  )";
  auto readFile = [](const std::string&) { return std::string(); };

  CompileResult result = compileShow(showText, readFile);
  CHECK(!result.ok);
  CHECK(result.err.find("SHOW 2") != std::string::npos);
  CHECK(result.err.find("1-indexed") != std::string::npos);
}

// A .show that's entirely blank/comments (never reaches a content line)
// still needs the header.
TEST(show_empty_file_missing_header_error) {
  std::string showText = "# just a comment\n\n";
  auto readFile = [](const std::string&) { return std::string(); };

  CompileResult result = compileShow(showText, readFile);
  CHECK(!result.ok);
  CHECK(result.err.find("SHOW 2") != std::string::npos);
}

// An unsupported SHOW version is also a hard error (not silently ignored).
TEST(show_unsupported_version_error) {
  std::string showText = R"(
    SHOW 3
    UNIVERSE 1 DMX
  )";
  auto readFile = [](const std::string&) { return std::string(); };

  CompileResult result = compileShow(showText, readFile);
  CHECK(!result.ok);
  CHECK(!result.err.empty());
}

TEST(show_universe_zero_error) {
  std::string showText = R"(
    SHOW 2
    UNIVERSE 0 DMX
  )";
  auto readFile = [](const std::string&) { return std::string(); };

  CompileResult result = compileShow(showText, readFile);
  CHECK(!result.ok);
  CHECK(result.err.find("1-indexed") != std::string::npos);
}

TEST(show_address_zero_error) {
  std::map<std::string, std::string> fileMap;
  fileMap["par.fdef"] = R"(
    FIXTURE Par
    FOOTPRINT 5
    CAP Dimmer 0
  )";
  std::string showText = R"(
    SHOW 2
    UNIVERSE 1 DMX
    FIXTURE par.fdef 1 0
  )";
  auto readFile = [&](const std::string& name) {
    auto it = fileMap.find(name);
    return it != fileMap.end() ? it->second : std::string();
  };

  CompileResult result = compileShow(showText, readFile);
  CHECK(!result.ok);
  CHECK(result.err.find("1..512") != std::string::npos);
}

TEST(show_address_513_error) {
  std::map<std::string, std::string> fileMap;
  fileMap["par.fdef"] = R"(
    FIXTURE Par
    FOOTPRINT 5
    CAP Dimmer 0
  )";
  std::string showText = R"(
    SHOW 2
    UNIVERSE 1 DMX
    FIXTURE par.fdef 1 513
  )";
  auto readFile = [&](const std::string& name) {
    auto it = fileMap.find(name);
    return it != fileMap.end() ? it->second : std::string();
  };

  CompileResult result = compileShow(showText, readFile);
  CHECK(!result.ok);
  CHECK(result.err.find("1..512") != std::string::npos);
}

// The classic "off the end of the universe" mistake: a fixture whose
// address + footprint runs past channel 512.
TEST(show_footprint_runs_past_universe_end_error) {
  std::map<std::string, std::string> fileMap;
  fileMap["torrent.fdef"] = R"(
    FIXTURE Torrent
    FOOTPRINT 16
    CAP Dimmer 0
  )";
  std::string showText = R"(
    SHOW 2
    UNIVERSE 1 DMX
    FIXTURE torrent.fdef 1 500
  )";
  auto readFile = [&](const std::string& name) {
    auto it = fileMap.find(name);
    return it != fileMap.end() ? it->second : std::string();
  };

  CompileResult result = compileShow(showText, readFile);
  CHECK(!result.ok);
  CHECK(result.err.find("runs past the end") != std::string::npos);
}

// ============================================================================
// SHOW 2: address collision (overlap) detection
// ============================================================================

TEST(show_two_fixture_overlap_error) {
  std::map<std::string, std::string> fileMap;
  fileMap["torrent.fdef"] = R"(
    FIXTURE Torrent
    FOOTPRINT 16
    CAP Dimmer 0
  )";
  fileMap["par.fdef"] = R"(
    FIXTURE RGBWAUV PAR
    FOOTPRINT 8
    CAP Dimmer 0
  )";
  std::string showText = R"(
    SHOW 2
    UNIVERSE 1 DMX
    FIXTURE torrent.fdef 1 17
    FIXTURE par.fdef 1 30
  )";
  auto readFile = [&](const std::string& name) {
    auto it = fileMap.find(name);
    return it != fileMap.end() ? it->second : std::string();
  };

  CompileResult result = compileShow(showText, readFile);
  CHECK(!result.ok);
  CHECK(result.err.find("address collision in universe 1") != std::string::npos);
  CHECK(result.err.find("Torrent") != std::string::npos);
  CHECK(result.err.find("RGBWAUV PAR") != std::string::npos);
  CHECK(result.err.find("17..32") != std::string::npos);  // torrent's full range
  CHECK(result.err.find("30..37") != std::string::npos);  // par's full range
  CHECK(result.err.find("30..32") != std::string::npos);  // the overlap itself
}

// A botched patch usually has several collisions -- every pair must be
// reported, not just the first.
TEST(show_three_fixture_overlap_all_reported) {
  std::map<std::string, std::string> fileMap;
  fileMap["f.fdef"] = R"(
    FIXTURE Gen
    FOOTPRINT 10
    CAP Dimmer 0
  )";
  std::string showText = R"(
    SHOW 2
    UNIVERSE 1 DMX
    FIXTURE f.fdef 1 1
    FIXTURE f.fdef 1 5
    FIXTURE f.fdef 1 9
  )";
  auto readFile = [&](const std::string& name) {
    auto it = fileMap.find(name);
    return it != fileMap.end() ? it->second : std::string();
  };

  CompileResult result = compileShow(showText, readFile);
  CHECK(!result.ok);

  size_t count = 0;
  size_t pos = 0;
  while ((pos = result.err.find("address collision in universe 1", pos)) != std::string::npos) {
    count++;
    pos++;
  }
  CHECK(count == 3);  // (1,5) (1,9) (5,9): every pairwise overlap reported
}

// Matrices occupy channels too (w*h*3), and must be checked against
// fixtures for overlap, not just other fixtures.
TEST(show_matrix_overlaps_fixture_error) {
  std::map<std::string, std::string> fileMap;
  fileMap["par.fdef"] = R"(
    FIXTURE Par
    FOOTPRINT 5
    CAP Dimmer 0
  )";
  std::string showText = R"(
    SHOW 2
    UNIVERSE 1 DMX
    FIXTURE par.fdef 1 1
    MATRIX 1 3 2 2 SERP H RGB
  )";
  auto readFile = [&](const std::string& name) {
    auto it = fileMap.find(name);
    return it != fileMap.end() ? it->second : std::string();
  };

  CompileResult result = compileShow(showText, readFile);
  CHECK(!result.ok);
  CHECK(result.err.find("address collision in universe 1") != std::string::npos);
  CHECK(result.err.find("Par") != std::string::npos);
  CHECK(result.err.find("Matrix") != std::string::npos);
}

// ============================================================================
// Golden test: the migrated samples/demo.show compiles to the exact same
// SHW1 bytes it produced before the 1-indexing migration -- proves the text
// semantics moved and the bytes didn't. kDemoShowGoldenBytes was captured
// from the pre-migration provision compiler run against the pre-migration
// (0-indexed, no SHOW header) samples/demo.show.
// ============================================================================

static const uint8_t kDemoShowGoldenBytes[] = {
  83, 72, 87, 49, 2, 4, 2, 0, 4, 0, 1, 0, 1, 0, 0, 1, 1, 1, 39, 0, 80, 70, 88, 49, 1, 0, 4, 4, 10,
  68, 105, 109, 109, 101, 114, 32, 82, 71, 66, 0, 0, 255, 0, 0, 1, 1, 255, 0, 0, 2, 2, 255, 0, 0,
  3, 3, 255, 0, 0, 55, 0, 80, 70, 88, 49, 1, 0, 9, 7, 11, 77, 111, 118, 105, 110, 103, 32, 72,
  101, 97, 100, 0, 0, 255, 0, 0, 10, 1, 2, 0, 0, 11, 3, 4, 0, 0, 1, 5, 255, 0, 0, 2, 6, 255, 0, 0,
  3, 7, 255, 0, 0, 12, 8, 255, 8, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 68, 0, 0, 135, 67, 0, 0, 0, 63, 0, 0, 0, 63, 0, 0, 0, 0,
  0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7,
  68, 0, 0, 135, 67, 0, 0, 0, 63, 0, 0, 0, 63, 0, 0, 1, 0, 0, 21, 0, 1, 0, 0, 0, 64, 0, 0, 128,
  63, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 68, 0, 0, 135, 67, 0, 0, 0, 63, 0,
  0, 0, 63, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 7, 68, 0, 0, 135, 67, 0, 0, 0, 63, 0, 0, 0, 63, 0, 0, 16, 0, 8, 0, 1, 0, 0, 2, 0,
  0, 108, 1, 77, 68, 70, 49, 1, 0, 0, 15, 6, 5, 2, 4, 28, 65, 107, 97, 105, 32, 65, 80, 67, 52,
  48, 32, 109, 107, 73, 73, 0, 39, 48, 52, 58, 66, 80, 81, 82, 86, 87, 103, 7, 7, 0, 0, 14, 14, 6,
  0, 15, 15, 13, 0, 16, 23, 255, 255, 48, 55, 255, 255, 13, 13, 1, 47, 47, 1, 0, 0, 39, 0, 0, 0,
  17, 0, 82, 86, 0, 17, 0, 5, 0, 52, 52, 0, 22, 0, 3, 0, 66, 66, 0, 25, 0, 3, 24, 0, 0, 28, 0, 1,
  37, 0, 2, 42, 0, 3, 48, 0, 5, 52, 0, 9, 59, 0, 13, 66, 0, 17, 71, 0, 21, 77, 0, 29, 84, 0, 33,
  89, 0, 37, 98, 0, 41, 103, 0, 45, 110, 0, 49, 117, 0, 53, 125, 0, 57, 130, 0, 0, 134, 0, 3, 140,
  0, 5, 144, 0, 21, 150, 0, 41, 155, 0, 0, 159, 0, 1, 162, 0, 2, 168, 0, 0, 172, 0, 1, 179, 0, 2,
  116, 114, 97, 99, 107, 0, 109, 97, 115, 116, 101, 114, 0, 99, 114, 111, 115, 115, 102, 97, 100,
  101, 114, 0, 111, 102, 102, 0, 103, 114, 101, 121, 45, 100, 105, 109, 0, 103, 114, 101, 121, 0,
  119, 104, 105, 116, 101, 0, 114, 101, 100, 0, 111, 114, 97, 110, 103, 101, 0, 121, 101, 108,
  108, 111, 119, 0, 108, 105, 109, 101, 0, 103, 114, 101, 101, 110, 0, 115, 112, 114, 105, 110,
  103, 0, 97, 113, 117, 97, 0, 115, 107, 121, 45, 98, 108, 117, 101, 0, 98, 108, 117, 101, 0, 105,
  110, 100, 105, 103, 111, 0, 118, 105, 111, 108, 101, 116, 0, 109, 97, 103, 101, 110, 116, 97, 0,
  112, 105, 110, 107, 0, 111, 102, 102, 0, 119, 104, 105, 116, 101, 0, 114, 101, 100, 0, 103, 114,
  101, 101, 110, 0, 98, 108, 117, 101, 0, 111, 102, 102, 0, 111, 110, 0, 98, 108, 105, 110, 107,
  0, 111, 102, 102, 0, 121, 101, 108, 108, 111, 119, 0, 111, 114, 97, 110, 103, 101, 0,
};

// Mirrors samples/demo.show and the .fdef/.mdef files it references
// (samples/dimmer.fdef, samples/head.fdef, samples/apc40.mdef) as string
// literals, rather than reading them off disk: this test also runs under
// the WASM/Node CI build (build-wasm.sh), whose Emscripten virtual
// filesystem has no access to the real repo tree, only whatever's
// preloaded -- unlike every other compileShow test in this file (which
// already use an in-memory fileMap), a disk read here would silently
// return "" and fail with a confusing "file not found"-shaped error under
// WASM while passing fine on host. Keep this text in sync with the real
// samples/ files by hand if either changes.
TEST(show_demo_golden_bytes_unchanged_by_1_indexing_migration) {
  std::map<std::string, std::string> fileMap;
  fileMap["samples/dimmer.fdef"] = R"(FIXTURE Dimmer RGB
FOOTPRINT 4
CAP Dimmer 0
CAP Red    1
CAP Green  2
CAP Blue   3
)";
  fileMap["samples/head.fdef"] = R"(FIXTURE Moving Head
FOOTPRINT 9
HEAD
PANRANGE 540
TILTRANGE 270
CAP Dimmer 0
CAP Pan    1 2
CAP Tilt   3 4
CAP Red    5
CAP Green  6
CAP Blue   7
CAP ShutterStrobe 8 - 8
)";
  fileMap["samples/apc40.mdef"] = R"(CONTROLLER Akai APC40 mkII
MIDI_CHANNEL 0

PAD 0 39
PAD 48 52
PAD 58 66
PAD 80 81
PAD 82 86
PAD 87 103

FADER CC 7   track      # 9 track faders (0x07, track = MIDI channel)
FADER CC 14  master     # master fader (0x0E)
FADER CC 15  crossfader # crossfader (0x0F)
FADER CC 16 23          # 8 DEVICE CONTROL knobs (0x10..0x17), absolute
FADER CC 48 55          # 8 TRACK CONTROL knobs (0x30..0x37), absolute

ENCODER CC 13 relative-2c   # tempo knob
ENCODER CC 47 relative-2c   # cue level

LED NOTE 0 39 velocity
  COLOR off       0
  COLOR grey-dim  1
  COLOR grey      2
  COLOR white     3
  COLOR red       5
  COLOR orange    9
  COLOR yellow    13
  COLOR lime      17
  COLOR green     21
  COLOR spring    29
  COLOR aqua      33
  COLOR sky-blue  37
  COLOR blue      41
  COLOR indigo    45
  COLOR violet    49
  COLOR magenta   53
  COLOR pink      57
LED NOTE 82 86 velocity
  COLOR off   0
  COLOR white 3
  COLOR red   5
  COLOR green 21
  COLOR blue  41
LED NOTE 52 52 velocity
  COLOR off   0
  COLOR on    1
  COLOR blink 2
LED NOTE 66 66 velocity
  COLOR off    0
  COLOR yellow 1
  COLOR orange 2
)";

  std::string showText = R"(SHOW 2

UNIVERSE 1 DMX
UNIVERSE 2 ARTNET
UNIVERSE 3 ARTNET
UNIVERSE 4 ARTNET

FIXTURE samples/dimmer.fdef 1 2
FIXTURE samples/dimmer.fdef 1 12

FIXTURE samples/head.fdef 1 22
POS 2.0 1.0 0.0
ROT 0 0 0
CENTER 0.5 0.5

FIXTURE samples/dimmer.fdef 2 2

MATRIX 3 1 16 8 SERP H RGB

CONTROLLER samples/apc40.mdef
)";

  auto readFile = [&](const std::string& name) {
    auto it = fileMap.find(name);
    return it != fileMap.end() ? it->second : std::string();
  };

  CompileResult result = compileShow(showText, readFile);
  CHECK(result.ok);

  size_t goldenLen = sizeof(kDemoShowGoldenBytes) / sizeof(kDemoShowGoldenBytes[0]);
  CHECK(result.bundle.size() == goldenLen);
  CHECK(std::memcmp(result.bundle.data(), kDemoShowGoldenBytes, goldenLen) == 0);
}

// ============================================================================
// Loader Strictness Tests
// ============================================================================

TEST(loader_bad_magic) {
  uint8_t badBundle[] = {'X', 'X', 'X', 'X', 1, 0, 0, 0, 0, 0, 0, 0};
  LoadedShow loaded;
  CHECK(!loadShow(badBundle, sizeof(badBundle), loaded));
}

TEST(loader_truncated_header) {
  uint8_t truncated[] = {'S', 'H', 'W', '1'};
  LoadedShow loaded;
  CHECK(!loadShow(truncated, sizeof(truncated), loaded));
}

TEST(loader_truncated_fixture_table) {
  // Create a minimal valid bundle header
  std::vector<uint8_t> bundle;
  bundle.push_back('S');
  bundle.push_back('H');
  bundle.push_back('W');
  bundle.push_back('1');
  bundle.push_back(1);  // version
  bundle.push_back(1);  // universeCount
  bundle.push_back(0);  // profileCount (low byte)
  bundle.push_back(0);  // profileCount (high byte)
  bundle.push_back(1);  // fixtureCount (low byte) -- says 1 fixture
  bundle.push_back(0);  // fixtureCount (high byte)
  bundle.push_back(0);  // matrixCount
  bundle.push_back(0);
  bundle.push_back(0);  // universe 0 transport

  // Truncate in the middle of the fixture table
  LoadedShow loaded;
  CHECK(!loadShow(bundle.data(), bundle.size(), loaded));
}

TEST(loader_profileindex_out_of_range) {
  std::vector<uint8_t> bundle;
  bundle.push_back('S');
  bundle.push_back('H');
  bundle.push_back('W');
  bundle.push_back('1');
  bundle.push_back(1);  // version
  bundle.push_back(1);  // universeCount
  bundle.push_back(0);  // profileCount = 0
  bundle.push_back(0);
  bundle.push_back(1);  // fixtureCount = 1
  bundle.push_back(0);
  bundle.push_back(0);  // matrixCount
  bundle.push_back(0);
  bundle.push_back(0);  // universe transport

  // Fixture record with profileIndex = 5 (out of range)
  bundle.push_back(5);  // profileIndex low
  bundle.push_back(0);  // profileIndex high
  // Rest of fixture record (46 - 2 = 44 bytes)
  for (int i = 0; i < 44; i++) {
    bundle.push_back(0);
  }

  LoadedShow loaded;
  CHECK(!loadShow(bundle.data(), bundle.size(), loaded));
}

// ============================================================================
// Main
// ============================================================================

int main() {
  printf("=== Provision Compiler & Loader Tests ===\n\n");

  RUN_TEST(parse_fdef_basic);
  RUN_TEST(capFromName_valid);
  RUN_TEST(capFromName_invalid);
  RUN_TEST(capFromName_v2_valid);
  RUN_TEST(parse_fdef_with_ranges);
  RUN_TEST(parse_fdef_cap_without_ranges_stays_linear);
  RUN_TEST(parse_fdef_slot_before_cap_error);
  RUN_TEST(parse_fdef_slot_bad_range);
  RUN_TEST(encode_profile_with_ranges);
  RUN_TEST(parse_fdef_missing_footprint);
  RUN_TEST(parse_fdef_bad_footprint);
  RUN_TEST(parse_fdef_footprint_non_numeric);
  RUN_TEST(parse_fdef_panrange_non_numeric);
  RUN_TEST(parse_fdef_cap_coarse_non_numeric);
  RUN_TEST(parse_fdef_cap_fine_non_numeric);
  RUN_TEST(parse_fdef_cap_default_non_numeric);
  RUN_TEST(parse_fdef_unknown_cap);
  RUN_TEST(encode_profile);
  RUN_TEST(show_compile_and_load_roundtrip);
  RUN_TEST(show_profile_deduplication);
  RUN_TEST(show_pos_without_head_error);
  RUN_TEST(show_pos_non_numeric_coordinate);
  RUN_TEST(show_matrix_non_numeric_param);
  RUN_TEST(parse_mdef_basic);
  RUN_TEST(parse_mdef_encoder_default_mode_absolute);
  RUN_TEST(parse_mdef_color_before_led_error);
  RUN_TEST(parse_mdef_missing_name_error);
  RUN_TEST(parse_mdef_unknown_encoder_mode_error);
  RUN_TEST(show_compile_and_load_roundtrip_with_controller);
  RUN_TEST(show_compile_no_controller_stays_v1);
  RUN_TEST(show_controller_missing_deffile_error);
  RUN_TEST(show_compile_and_load_roundtrip_with_wled);
  RUN_TEST(show_compile_and_load_roundtrip_with_wled_and_controller);
  RUN_TEST(show_compile_no_wled_stays_v1);
  RUN_TEST(show_wled_duplicate_name_error);
  RUN_TEST(show_wled_missing_ip_error);
  RUN_TEST(show_wled_syncgroup_out_of_range_error);
  RUN_TEST(show_v2_address_and_universe_convert_to_0_indexed);
  RUN_TEST(show_missing_header_error_names_migration);
  RUN_TEST(show_empty_file_missing_header_error);
  RUN_TEST(show_unsupported_version_error);
  RUN_TEST(show_universe_zero_error);
  RUN_TEST(show_address_zero_error);
  RUN_TEST(show_address_513_error);
  RUN_TEST(show_footprint_runs_past_universe_end_error);
  RUN_TEST(show_two_fixture_overlap_error);
  RUN_TEST(show_three_fixture_overlap_all_reported);
  RUN_TEST(show_matrix_overlaps_fixture_error);
  RUN_TEST(show_demo_golden_bytes_unchanged_by_1_indexing_migration);
  RUN_TEST(loader_bad_magic);
  RUN_TEST(loader_truncated_header);
  RUN_TEST(loader_truncated_fixture_table);
  RUN_TEST(loader_profileindex_out_of_range);

  printf("\n=== Test Summary ===\n");
  printf("Passed: %d\n", testsPassed);
  printf("Failed: %d\n", testsFailed);

  return testsFailed > 0 ? 1 : 0;
}
