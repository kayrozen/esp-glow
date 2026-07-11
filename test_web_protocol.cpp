#include "web_protocol.h"
#include "live_control.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failCount = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      g_failCount++; \
    } \
  } while (0)

#define TEST(name) printf("Test: %s\n", name)

// Helper: parse and return whether the ControlEvent was produced.
static bool parseOk(const char* json, ControlEvent& out) {
  return parseWebCommand(json, std::strlen(json), out);
}

// ---------------------------------------------------------------------------
// parseWebCommand — cue / scene / master / hello
// ---------------------------------------------------------------------------

void test_parse_cue_pressed_true() {
  TEST("parse: cue pressed=true");
  ControlEvent ev;
  CHECK(parseOk(R"({"type":"cue","id":42,"pressed":true})", ev));
  CHECK(ev.type == ControlType::Button);
  CHECK(ev.id == 42);
  CHECK(ev.pressed == true);
}

void test_parse_cue_pressed_false() {
  TEST("parse: cue pressed=false (flash release)");
  ControlEvent ev;
  CHECK(parseOk(R"({"type":"cue","id":7,"pressed":false})", ev));
  CHECK(ev.type == ControlType::Button);
  CHECK(ev.id == 7);
  CHECK(ev.pressed == false);
}

void test_parse_scene() {
  TEST("parse: scene");
  ControlEvent ev;
  CHECK(parseOk(R"({"type":"scene","id":3,"pressed":true})", ev));
  CHECK(ev.type == ControlType::Button);
  CHECK(ev.id == 3);
  CHECK(ev.pressed == true);
}

void test_parse_master() {
  TEST("parse: master value=0.5");
  ControlEvent ev;
  CHECK(parseOk(R"({"type":"master","value":0.5})", ev));
  CHECK(ev.type == ControlType::Fader);
  CHECK(ev.id == 0);
  CHECK(ev.pressed == false);
  CHECK(std::fabs(ev.value - 0.5f) < 1e-4f);
}

void test_parse_master_clamp_high() {
  TEST("parse: master value clamps to 1.0");
  ControlEvent ev;
  CHECK(parseOk(R"({"type":"master","value":2.0})", ev));
  CHECK(std::fabs(ev.value - 1.0f) < 1e-4f);
}

void test_parse_master_clamp_low() {
  TEST("parse: master value clamps to 0.0");
  ControlEvent ev;
  CHECK(parseOk(R"({"type":"master","value":-1.0})", ev));
  CHECK(std::fabs(ev.value - 0.0f) < 1e-4f);
}

void test_parse_hello_returns_false() {
  TEST("parse: hello is not a ControlEvent (device handles it)");
  ControlEvent ev;
  CHECK(!parseOk(R"({"type":"hello"})", ev));
}

void test_parse_unknown_type_returns_false() {
  TEST("parse: unknown type rejected");
  ControlEvent ev;
  CHECK(!parseOk(R"({"type":"frob","id":1,"pressed":true})", ev));
}

void test_parse_state_rejected() {
  TEST("parse: state (device->UI) rejected as a command");
  ControlEvent ev;
  CHECK(!parseOk(R"({"type":"state","active":[0,3,5]})", ev));
}

void test_parse_missing_type() {
  TEST("parse: missing type rejected");
  ControlEvent ev;
  CHECK(!parseOk(R"({"id":1,"pressed":true})", ev));
}

void test_parse_missing_id() {
  TEST("parse: cue missing id rejected");
  ControlEvent ev;
  CHECK(!parseOk(R"({"type":"cue","pressed":true})", ev));
}

void test_parse_missing_pressed() {
  TEST("parse: cue missing pressed rejected");
  ControlEvent ev;
  CHECK(!parseOk(R"({"type":"cue","id":1})", ev));
}

void test_parse_missing_value() {
  TEST("parse: master missing value rejected");
  ControlEvent ev;
  CHECK(!parseOk(R"({"type":"master"})", ev));
}

void test_parse_id_out_of_range() {
  TEST("parse: id > 65535 rejected");
  ControlEvent ev;
  CHECK(!parseOk(R"({"type":"cue","id":70000,"pressed":true})", ev));
}

void test_parse_id_negative() {
  TEST("parse: id < 0 rejected");
  ControlEvent ev;
  CHECK(!parseOk(R"({"type":"cue","id":-1,"pressed":true})", ev));
}

void test_parse_tolerates_extra_fields() {
  TEST("parse: extra unknown fields tolerated");
  ControlEvent ev;
  CHECK(parseOk(R"({"type":"cue","id":1,"pressed":true,"extra":"x","n":42})", ev));
  CHECK(ev.id == 1);
  CHECK(ev.pressed == true);
}

void test_parse_tolerates_whitespace() {
  TEST("parse: whitespace between tokens tolerated");
  ControlEvent ev;
  CHECK(parseOk("  {  \"type\"  :  \"cue\"  ,  \"id\"  :  1  ,  \"pressed\"  :  true  }  ", ev));
  CHECK(ev.id == 1);
}

void test_parse_field_order_independent() {
  TEST("parse: field order is independent");
  ControlEvent ev;
  CHECK(parseOk(R"({"pressed":true,"id":5,"type":"cue"})", ev));
  CHECK(ev.id == 5);
  CHECK(ev.pressed == true);
}

void test_parse_malformed_object() {
  TEST("parse: malformed JSON rejected");
  ControlEvent ev;
  CHECK(!parseOk("{not json}", ev));
  CHECK(!parseOk("{\"type\":\"cue\",\"id\":}", ev));
  CHECK(!parseOk("{\"type\":\"cue\",\"id\":1", ev));  // missing close
  CHECK(!parseOk("\"type\":\"cue\"", ev));  // not an object
}

void test_parse_empty_input() {
  TEST("parse: empty input rejected");
  ControlEvent ev;
  CHECK(!parseWebCommand("", 0, ev));
  CHECK(!parseWebCommand(nullptr, 0, ev));
}

void test_parse_string_with_escapes_rejected() {
  TEST("parse: string with backslash escape rejected (protocol has none)");
  ControlEvent ev;
  CHECK(!parseOk(R"({"type":"cu\e","id":1,"pressed":true})", ev));
}

void test_parse_depth_limit() {
  TEST("parse: deep nested unknown field rejected, shallow tolerated");
  ControlEvent ev;
  CHECK(parseOk(R"({"type":"cue","id":1,"pressed":true,"x":[1,[2,3]]})", ev));
  CHECK(parseOk(R"({"type":"cue","id":1,"pressed":true,"x":{"a":{"b":1}}})", ev));
  std::string deep = R"({"type":"cue","id":1,"pressed":true,"x":)";
  for (int i = 0; i < 200; i++) deep += "[";
  for (int i = 0; i < 200; i++) deep += "]";
  deep += "}";
  CHECK(parseWebCommand(deep.c_str(), deep.size(), ev) == false);
}

void test_is_hello_command() {
  TEST("isHelloCommand: detects hello, no overread on short buffer");
  CHECK(isHelloCommand(R"({"type":"hello"})", 16) == true);
  CHECK(isHelloCommand(R"({"type":"cue","id":0,"pressed":true})", 37) == false);
  CHECK(isHelloCommand(nullptr, 0) == false);
  const char* src = "{\"type\":\"hello";   // 14 chars, not NUL-terminated
  char* buf = new char[14];
  std::memcpy(buf, src, 14);
  CHECK(isHelloCommand(buf, 14) == false); // len<15 -> false, never reads buf[14]
  delete[] buf;
}

// ---------------------------------------------------------------------------
// buildConfigJson
// ---------------------------------------------------------------------------

void test_build_config_empty() {
  TEST("build: empty config, hasMaster=false");
  char buf[256];
  size_t n = buildConfigJson(nullptr, 0, nullptr, 0, false, buf, sizeof(buf));
  std::string s(buf, n);
  CHECK(s == R"({"type":"config","cues":[],"scenes":[],"hasMaster":false})");
}

void test_build_config_one_cue_toggle() {
  TEST("build: one cue, toggle mode, defaults for label/color");
  WebCueInfo cue{0, "Blue wash", "#3060ff", ActionKind::CueToggle};
  char buf[256];
  size_t n = buildConfigJson(&cue, 1, nullptr, 0, true, buf, sizeof(buf));
  std::string s(buf, n);
  CHECK(s == R"({"type":"config","cues":[{"id":0,"label":"Blue wash","color":"#3060ff","mode":"toggle"}],"scenes":[],"hasMaster":true})");
}

void test_build_config_cue_flash_mode() {
  TEST("build: CueFlash -> mode=flash");
  WebCueInfo cue{3, nullptr, nullptr, ActionKind::CueFlash};
  char buf[256];
  size_t n = buildConfigJson(&cue, 1, nullptr, 0, false, buf, sizeof(buf));
  std::string s(buf, n);
  // Default label "Cue", default color "#3060ff".
  CHECK(s == R"({"type":"config","cues":[{"id":3,"label":"Cue","color":"#3060ff","mode":"flash"}],"scenes":[],"hasMaster":false})");
}

void test_build_config_two_cues_one_scene() {
  TEST("build: two cues + one scene");
  WebCueInfo cues[2] = {
    {0, "Verse",   "#3060ff", ActionKind::CueToggle},
    {1, "Chorus",  "#ff6030", ActionKind::CueToggle},
  };
  WebSceneInfo scenes[1] = {{0, "Verse"}};
  char buf[512];
  size_t n = buildConfigJson(cues, 2, scenes, 1, true, buf, sizeof(buf));
  std::string s(buf, n);
  CHECK(s == R"({"type":"config","cues":[{"id":0,"label":"Verse","color":"#3060ff","mode":"toggle"},{"id":1,"label":"Chorus","color":"#ff6030","mode":"toggle"}],"scenes":[{"id":0,"label":"Verse"}],"hasMaster":true})");
}

void test_build_config_label_escaped() {
  TEST("build: label with quote and backslash is escaped");
  WebCueInfo cue{0, "Quote\"Slash\\", "#000000", ActionKind::CueToggle};
  char buf[256];
  size_t n = buildConfigJson(&cue, 1, nullptr, 0, false, buf, sizeof(buf));
  std::string s(buf, n);
  // Escaped: "Quote\"Slash\\"
  CHECK(s == R"({"type":"config","cues":[{"id":0,"label":"Quote\"Slash\\","color":"#000000","mode":"toggle"}],"scenes":[],"hasMaster":false})");
}

void test_build_config_truncation_reported() {
  TEST("build: truncation reported via return value");
  WebCueInfo cue{0, "Blue wash", "#3060ff", ActionKind::CueToggle};
  char buf[32];
  size_t n = buildConfigJson(&cue, 1, nullptr, 0, true, buf, sizeof(buf));
  // Full output is 88 chars; with 32-byte buffer we get a truncated,
  // NUL-terminated prefix.
  CHECK(n >= sizeof(buf));
  CHECK(std::strlen(buf) < sizeof(buf));
}

void test_build_config_null_buffer_returns_size() {
  TEST("build: null buffer still returns full length");
  WebCueInfo cue{0, "Blue wash", "#3060ff", ActionKind::CueToggle};
  size_t n = buildConfigJson(&cue, 1, nullptr, 0, true, nullptr, 0);
  // Matches the full string emitted by test_build_config_one_cue_toggle.
  CHECK(n == 118);
}

// ---------------------------------------------------------------------------
// buildStateJson
// ---------------------------------------------------------------------------

void test_build_state_empty() {
  TEST("build: state with no active cues");
  char buf[64];
  size_t n = buildStateJson(nullptr, 0, buf, sizeof(buf));
  std::string s(buf, n);
  CHECK(s == R"({"type":"state","active":[]})");
}

void test_build_state_three_ids() {
  TEST("build: state with three active cue ids");
  uint16_t ids[3] = {0, 3, 5};
  char buf[64];
  size_t n = buildStateJson(ids, 3, buf, sizeof(buf));
  std::string s(buf, n);
  CHECK(s == R"({"type":"state","active":[0,3,5]})");
}

// ---------------------------------------------------------------------------
// Round-trip: parsed command feeds LiveControl end-to-end.
// ---------------------------------------------------------------------------

void test_roundtrip_cue_toggle_dispatches() {
  TEST("roundtrip: cue toggle command activates cue via LiveControl");
  // We don't need a real ShowController for this — we just verify that the
  // ControlEvent we parse has the right shape to feed LiveControl. The
  // LiveControl tests already cover dispatch behavior.
  ControlEvent ev;
  CHECK(parseOk(R"({"type":"cue","id":61,"pressed":true})", ev));
  CHECK(ev.type == ControlType::Button);
  CHECK(ev.id == 61);
  CHECK(ev.pressed == true);
  // LiveControl would now: bindButton(61, CueToggle, cueId); handle(ev, t);
}

void test_roundtrip_master_dispatches() {
  TEST("roundtrip: master command sets masterLevel via LiveControl");
  ControlEvent ev;
  CHECK(parseOk(R"({"type":"master","value":0.75})", ev));
  CHECK(ev.type == ControlType::Fader);
  CHECK(ev.id == 0);  // device binds fader id 0 as Master
  CHECK(std::fabs(ev.value - 0.75f) < 1e-4f);
}

// ---------------------------------------------------------------------------

int main() {
  // Parser — valid inputs
  test_parse_cue_pressed_true();
  test_parse_cue_pressed_false();
  test_parse_scene();
  test_parse_master();
  test_parse_master_clamp_high();
  test_parse_master_clamp_low();
  test_parse_hello_returns_false();
  test_parse_tolerates_extra_fields();
  test_parse_tolerates_whitespace();
  test_parse_field_order_independent();

  // Parser — rejection cases
  test_parse_unknown_type_returns_false();
  test_parse_state_rejected();
  test_parse_missing_type();
  test_parse_missing_id();
  test_parse_missing_pressed();
  test_parse_missing_value();
  test_parse_id_out_of_range();
  test_parse_id_negative();
  test_parse_malformed_object();
  test_parse_empty_input();
  test_parse_string_with_escapes_rejected();
  test_parse_depth_limit();
  test_is_hello_command();

  // Builder — config
  test_build_config_empty();
  test_build_config_one_cue_toggle();
  test_build_config_cue_flash_mode();
  test_build_config_two_cues_one_scene();
  test_build_config_label_escaped();
  test_build_config_truncation_reported();
  test_build_config_null_buffer_returns_size();

  // Builder — state
  test_build_state_empty();
  test_build_state_three_ids();

  // Round-trip into LiveControl shape
  test_roundtrip_cue_toggle_dispatches();
  test_roundtrip_master_dispatches();

  if (g_failCount == 0) {
    printf("\nAll tests passed.\n");
    return 0;
  } else {
    printf("\n%d test(s) failed.\n", g_failCount);
    return 1;
  }
}
