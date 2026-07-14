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
    UNIVERSE 0 DMX
    UNIVERSE 1 ARTNET
    FIXTURE torrent.fdef 0 1
    POS 1 2 3
    ROT 0 0 0
    CENTER 0.5 0.5
    INVERT 0 0
    FIXTURE par.fdef 0 20
    MATRIX 1 0 16 16 SERP H GRB
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
    UNIVERSE 0 DMX
    FIXTURE par.fdef 0 0
    FIXTURE par.fdef 0 10
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
    UNIVERSE 0 DMX
    FIXTURE par.fdef 0 0
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
    UNIVERSE 0 DMX
    FIXTURE par.fdef 0 0
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
    UNIVERSE 0 DMX
    FIXTURE par.fdef 0 0
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
    UNIVERSE 0 DMX
    CONTROLLER missing.mdef
  )";
  auto readFile = [&](const std::string&) { return std::string(); };

  CompileResult result = compileShow(showText, readFile);
  CHECK(!result.ok);
  CHECK(!result.err.empty());
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
  RUN_TEST(parse_fdef_unknown_cap);
  RUN_TEST(encode_profile);
  RUN_TEST(show_compile_and_load_roundtrip);
  RUN_TEST(show_profile_deduplication);
  RUN_TEST(show_pos_without_head_error);
  RUN_TEST(parse_mdef_basic);
  RUN_TEST(parse_mdef_encoder_default_mode_absolute);
  RUN_TEST(parse_mdef_color_before_led_error);
  RUN_TEST(parse_mdef_missing_name_error);
  RUN_TEST(parse_mdef_unknown_encoder_mode_error);
  RUN_TEST(show_compile_and_load_roundtrip_with_controller);
  RUN_TEST(show_compile_no_controller_stays_v1);
  RUN_TEST(show_controller_missing_deffile_error);
  RUN_TEST(loader_bad_magic);
  RUN_TEST(loader_truncated_header);
  RUN_TEST(loader_truncated_fixture_table);
  RUN_TEST(loader_profileindex_out_of_range);

  printf("\n=== Test Summary ===\n");
  printf("Passed: %d\n", testsPassed);
  printf("Failed: %d\n", testsFailed);

  return testsFailed > 0 ? 1 : 0;
}
