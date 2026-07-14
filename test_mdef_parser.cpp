// test_mdef_parser.cpp — host tests for .mdef controller definition parsing.
#include "mdef_parser.h"

#include <cstdio>
#include <cstring>

static int g_fail = 0;

#define CHECK(cond) do { \
  if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } \
} while (0)

#define TEST(name) printf("Test: %s\n", name)

using namespace glow::mdef;

static void test_parse_minimal_controller() {
  TEST("minimal valid .mdef with CONTROLLER and one PAD");
  const char* src = 
    "CONTROLLER Test Controller\n"
    "PAD 60\n";
  
  ControllerDef def;
  char err[256] = {0};
  bool ok = parseMdef(src, strlen(src), def, err, sizeof(err));
  CHECK(ok);
  CHECK(def.name == "Test Controller");
  CHECK(def.controls.size() == 1);
  CHECK(def.controls[0].type == ControlType::Pad);
  CHECK(def.controls[0].startId == 60);
  CHECK(def.controls[0].endId == 60);
}

static void test_parse_pad_range() {
  TEST("PAD with range 53 92");
  const char* src = 
    "CONTROLLER Akai APC40 mkII\n"
    "PAD 53 92\n";
  
  ControllerDef def;
  char err[256] = {0};
  bool ok = parseMdef(src, strlen(src), def, err, sizeof(err));
  CHECK(ok);
  CHECK(def.controls.size() == 1);
  CHECK(def.controls[0].startId == 53);
  CHECK(def.controls[0].endId == 92);
}

static void test_parse_fader_cc_single() {
  TEST("FADER CC single with name");
  const char* src = 
    "CONTROLLER Test\n"
    "FADER CC 7 master\n";
  
  ControllerDef def;
  char err[256] = {0};
  bool ok = parseMdef(src, strlen(src), def, err, sizeof(err));
  CHECK(ok);
  CHECK(def.controls.size() == 1);
  CHECK(def.controls[0].type == ControlType::Fader);
  CHECK(def.controls[0].startId == 7);
  CHECK(def.controls[0].endId == 7);
  CHECK(def.controls[0].name == "master");
}

static void test_parse_fader_cc_range() {
  TEST("FADER CC range 48 55");
  const char* src = 
    "CONTROLLER Test\n"
    "FADER CC 48 55\n";
  
  ControllerDef def;
  char err[256] = {0};
  bool ok = parseMdef(src, strlen(src), def, err, sizeof(err));
  CHECK(ok);
  CHECK(def.controls.size() == 1);
  CHECK(def.controls[0].type == ControlType::Fader);
  CHECK(def.controls[0].startId == 48);
  CHECK(def.controls[0].endId == 55);
}

static void test_parse_encoder_relative_2c() {
  TEST("ENCODER CC relative-2c mode");
  const char* src = 
    "CONTROLLER Test\n"
    "ENCODER CC 16 23 relative-2c\n";
  
  ControllerDef def;
  char err[256] = {0};
  bool ok = parseMdef(src, strlen(src), def, err, sizeof(err));
  CHECK(ok);
  CHECK(def.controls.size() == 1);
  CHECK(def.controls[0].type == ControlType::Encoder);
  CHECK(def.controls[0].startId == 16);
  CHECK(def.controls[0].endId == 23);
  CHECK(def.controls[0].encoderMode == EncoderMode::Relative2C);
}

static void test_parse_encoder_absolute() {
  TEST("ENCODER CC absolute mode");
  const char* src = 
    "CONTROLLER Test\n"
    "ENCODER CC 0 7 absolute\n";
  
  ControllerDef def;
  char err[256] = {0};
  bool ok = parseMdef(src, strlen(src), def, err, sizeof(err));
  CHECK(ok);
  CHECK(def.controls[0].encoderMode == EncoderMode::Absolute);
}

static void test_parse_led_note_velocity() {
  TEST("LED NOTE velocity with color palette");
  const char* src = 
    "CONTROLLER Test\n"
    "PAD 53 55\n"
    "LED NOTE 53 55 velocity\n"
    "  COLOR off 0\n"
    "  COLOR green 1\n"
    "  COLOR red 3\n";
  
  ControllerDef def;
  char err[256] = {0};
  bool ok = parseMdef(src, strlen(src), def, err, sizeof(err));
  CHECK(ok);
  CHECK(def.leds.size() == 1);
  CHECK(def.leds[0].isNote == true);
  CHECK(def.leds[0].startId == 53);
  CHECK(def.leds[0].endId == 55);
  CHECK(def.leds[0].semantic == LedSemantic::Velocity);
  CHECK(def.leds[0].colorPalette.size() == 3);
  
  uint8_t val;
  CHECK(resolveColor(def.leds[0], "off", val) && val == 0);
  CHECK(resolveColor(def.leds[0], "green", val) && val == 1);
  CHECK(resolveColor(def.leds[0], "red", val) && val == 3);
}

static void test_parse_led_cc_value() {
  TEST("LED CC value for fader LED rings");
  const char* src = 
    "CONTROLLER Test\n"
    "FADER CC 48 55\n"
    "LED CC 48 55 value\n";
  
  ControllerDef def;
  char err[256] = {0};
  bool ok = parseMdef(src, strlen(src), def, err, sizeof(err));
  CHECK(ok);
  CHECK(def.leds.size() == 1);
  CHECK(def.leds[0].isNote == false);
  CHECK(def.leds[0].semantic == LedSemantic::Value);
}

static void test_find_control_by_id() {
  TEST("findControlById finds controls in ranges");
  const char* src = 
    "CONTROLLER Test\n"
    "PAD 53 92\n"
    "FADER CC 7 master\n";
  
  ControllerDef def;
  char err[256] = {0};
  bool ok = parseMdef(src, strlen(src), def, err, sizeof(err));
  CHECK(ok);
  
  const ControlElement* ctrl;
  ctrl = findControlById(def, 53);
  CHECK(ctrl != nullptr && ctrl->type == ControlType::Pad);
  ctrl = findControlById(def, 72);
  CHECK(ctrl != nullptr && ctrl->type == ControlType::Pad);
  ctrl = findControlById(def, 92);
  CHECK(ctrl != nullptr && ctrl->type == ControlType::Pad);
  ctrl = findControlById(def, 7);
  CHECK(ctrl != nullptr && ctrl->type == ControlType::Fader);
  ctrl = findControlById(def, 100);
  CHECK(ctrl == nullptr);
}

static void test_find_led_by_id() {
  TEST("findLedById finds LEDs covering an ID");
  const char* src = 
    "CONTROLLER Test\n"
    "LED NOTE 53 92 velocity\n"
    "  COLOR off 0\n";
  
  ControllerDef def;
  char err[256] = {0};
  bool ok = parseMdef(src, strlen(src), def, err, sizeof(err));
  CHECK(ok);
  
  const LedDefinition* led;
  led = findLedById(def, 53);
  CHECK(led != nullptr && led->startId == 53);
  led = findLedById(def, 72);
  CHECK(led != nullptr);
  led = findLedById(def, 92);
  CHECK(led != nullptr);
  led = findLedById(def, 100);
  CHECK(led == nullptr);
}

static void test_unknown_color_name_no_op() {
  TEST("resolveColor returns false for unknown color name (no-op, not error)");
  const char* src = 
    "CONTROLLER Test\n"
    "LED NOTE 53 55 velocity\n"
    "  COLOR off 0\n"
    "  COLOR green 1\n";
  
  ControllerDef def;
  char err[256] = {0};
  bool ok = parseMdef(src, strlen(src), def, err, sizeof(err));
  CHECK(ok);
  
  uint8_t val;
  CHECK(!resolveColor(def.leds[0], "unknown-color", val));
}

static void test_missing_controller_name_fails() {
  TEST("missing CONTROLLER declaration fails parse");
  const char* src = "PAD 60\n";
  
  ControllerDef def;
  char err[256] = {0};
  bool ok = parseMdef(src, strlen(src), def, err, sizeof(err));
  CHECK(!ok);
  CHECK(strstr(err, "missing CONTROLLER") != nullptr);
}

static void test_unknown_command_fails() {
  TEST("unknown command fails parse");
  const char* src = 
    "CONTROLLER Test\n"
    "UNKNOWN_CMD foo\n";
  
  ControllerDef def;
  char err[256] = {0};
  bool ok = parseMdef(src, strlen(src), def, err, sizeof(err));
  CHECK(!ok);
  CHECK(strstr(err, "unknown command") != nullptr);
}

static void test_comments_ignored() {
  TEST("comments (# ...) are ignored");
  const char* src = 
    "CONTROLLER Test  # this is a comment\n"
    "# another comment\n"
    "PAD 60  # inline comment\n";
  
  ControllerDef def;
  char err[256] = {0};
  bool ok = parseMdef(src, strlen(src), def, err, sizeof(err));
  CHECK(ok);
  CHECK(def.name == "Test");
  CHECK(def.controls.size() == 1);
}

static void test_midi_channel() {
  TEST("MIDI_CHANNEL parsed correctly");
  const char* src = 
    "CONTROLLER Test\n"
    "MIDI_CHANNEL 1\n"
    "PAD 60\n";
  
  ControllerDef def;
  char err[256] = {0};
  bool ok = parseMdef(src, strlen(src), def, err, sizeof(err));
  CHECK(ok);
  CHECK(def.midiChannel == 1);
}

static void test_all_encoder_modes() {
  TEST("all encoder modes parse correctly");
  
  struct TestCase {
    const char* modeStr;
    EncoderMode expected;
  };
  
  TestCase cases[] = {
    {"absolute", EncoderMode::Absolute},
    {"relative-2c", EncoderMode::Relative2C},
    {"relative-signmag", EncoderMode::RelativeSignMag},
    {"relative-6365", EncoderMode::Relative6365},
  };
  
  for (const auto& tc : cases) {
    char src[256];
    std::snprintf(src, sizeof(src), 
      "CONTROLLER Test\n"
      "ENCODER CC 0 7 %s\n", tc.modeStr);
    
    ControllerDef def;
    char err[256] = {0};
    bool ok = parseMdef(src, strlen(src), def, err, sizeof(err));
    CHECK(ok);
    CHECK(def.controls[0].encoderMode == tc.expected);
  }
}

int main() {
  test_parse_minimal_controller();
  test_parse_pad_range();
  test_parse_fader_cc_single();
  test_parse_fader_cc_range();
  test_parse_encoder_relative_2c();
  test_parse_encoder_absolute();
  test_parse_led_note_velocity();
  test_parse_led_cc_value();
  test_find_control_by_id();
  test_find_led_by_id();
  test_unknown_color_name_no_op();
  test_missing_controller_name_fails();
  test_unknown_command_fails();
  test_comments_ignored();
  test_midi_channel();
  test_all_encoder_modes();
  
  if (g_fail == 0) {
    printf("All mdef_parser tests passed!\n");
    return 0;
  }
  printf("%d mdef_parser tests FAILED\n", g_fail);
  return 1;
}
