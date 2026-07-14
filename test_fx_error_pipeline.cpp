// test_fx_error_pipeline.cpp — proves the FULL fx_error path, end to end,
// in one test: a real LuaEffect throws and gets disabled -> GlowLuaApi
// notices via pollNewlyDisabledEffects -> the notification is serialized
// by buildFxErrorJson (web_protocol.h) into the exact wire-format message
// the console expects.
//
// This exists as its own small binary (rather than folding into
// test_glow_lua_api.cpp or test_web_protocol.cpp) because those two only
// ever prove their own half: test_glow_lua_api proves LuaEffect ->
// pollNewlyDisabledEffects with real Fennel/ShowController but never calls
// buildFxErrorJson; test_web_protocol proves buildFxErrorJson's JSON
// shape but only ever feeds it hand-written strings, never a real
// disabled effect's name/error. Neither on its own rules out the seam
// between them breaking. This binary links glow_lua_api.cpp (GLOW_LUA_SOURCES)
// and web_protocol.cpp together specifically to close that gap.
//
// What this does NOT cover (needs real hardware, see web_input.cpp /
// README_LUA_FENNEL.md's HIL-only list): the render task's frame-slack
// hook actually calling web_input_poll_fx_error() every frame, and the
// httpd layer actually writing the resulting bytes to a WebSocket. Both
// are thin, un-branching wrappers around exactly the loop this test
// exercises (see web_input.cpp's web_input_poll_fx_error: it calls
// pollNewlyDisabledEffects once, then buildFxErrorJson once per
// notification found -- an earlier version called buildFxErrorJson only
// once per poll and silently dropped any second effect that broke in the
// same frame; pollFxErrorMessages below mirrors the CURRENT, fixed
// behavior, and this file's two-effects-in-one-frame test is what caught
// the original bug).

#include "glow_lua_api.h"
#include "web_protocol.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "live_control.h"
#include "lua_vm.h"
#include "show_control.h"

static int g_failCount = 0;

#define CHECK(cond)                                           \
  do {                                                        \
    if (!(cond)) {                                            \
      printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      g_failCount++;                                          \
    }                                                          \
  } while (0)

#define TEST(name) printf("Test: %s\n", name)

namespace {

std::string readFennelSource() {
  std::ifstream f("third_party/fennel/fennel.lua", std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

struct Harness {
  ShowController show;
  glow::LuaVM vm;
  glow::BeatClock beatClock;
  LiveControl live;
  GlowLuaApi api;

  explicit Harness(const std::string& fennelSrc)
      : vm(), live(show), api(vm, show, nullptr, beatClock, live) {
    api.install();
    char err[256];
    if (!vm.loadFennelCompiler(fennelSrc.data(), fennelSrc.size(), err, sizeof(err))) {
      printf("FATAL: loadFennelCompiler failed: %s\n", err);
      std::abort();
    }
    vm.collectFullyOnce();
  }

  void evalOrDie(const char* src) {
    lua_State* L = vm.state();
    vm.pushFennelModule();
    lua_getfield(L, -1, "eval");
    lua_remove(L, -2);
    lua_pushlstring(L, src, std::strlen(src));
    lua_newtable(L);
    vm.pushSandboxEnv();
    lua_setfield(L, -2, "env");
    api.noteEvalTime(0.0f);
    vm.armEvalBudget();
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
      const char* msg = lua_tostring(L, -1);
      printf("FATAL: eval failed: %s\n", msg ? msg : "(non-string error)");
      std::abort();
    }
  }
};

}  // namespace

// The exact composition web_input.cpp's web_input_poll_fx_error performs
// (see its header comment) -- reproduced here so the test doesn't need
// web_input.cpp itself to compile on host (it's ESP_PLATFORM-gated glue;
// see README_LUA_FENNEL.md's HIL-only list). If that function's body ever
// diverges from this, the divergence is the bug, not this test.
//
// Deliberately mirrors web_input_poll_fx_error's real signature (a
// callback invoked once per newly-disabled effect, not a single
// true/false + one buffer) -- an earlier version of both this helper and
// the real function returned only the first notification per call and
// silently dropped the rest when more than one effect broke in the same
// frame, since GlowLuaApi::pollNewlyDisabledEffects marks ALL of them
// reported in one call regardless of how many the caller consumes. This
// test's two-effects-in-one-frame case below exists specifically to catch
// that regression if it comes back.
static std::vector<std::string> pollFxErrorMessages(GlowLuaApi& api) {
  std::vector<std::pair<std::string, std::string>> notifications;
  api.pollNewlyDisabledEffects(notifications);
  std::vector<std::string> messages;
  for (const auto& n : notifications) {
    char buf[512];
    size_t len = buildFxErrorJson(n.first.c_str(), n.second.c_str(), buf, sizeof(buf));
    messages.emplace_back(buf, len);
  }
  return messages;
}

void test_broken_effect_produces_correct_fx_error_wire_message() {
  TEST("full path: LuaEffect throws -> disabled_=true -> pollNewlyDisabledEffects -> buildFxErrorJson");
  std::string fsrc = readFennelSource();
  Harness h(fsrc);

  // A cue with two effects: one fine, one that throws a real Lua runtime
  // error the first time it's driven.
  h.evalOrDie(
      "(fn ok [t] (glow.set 1 :dimmer 1.0))\n"
      "(fn breathe [t] (glow.set \"not-a-number\" :dimmer 1.0))\n"
      "(glow.cue.define :verse {:effects [ok breathe] :priority 0})\n"
      "(glow.cue.go :verse)\n");

  // Before the cue has ever rendered, there is nothing to report.
  CHECK(pollFxErrorMessages(h.api).empty());

  // Drive the show once -- this is what actually calls LuaEffect::evaluate,
  // which is where disabled_ gets set to true on the throwing effect.
  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;
  h.show.evaluate(0.0f, caps, aims);

  // Now the full pipeline should produce exactly one well-formed fx_error
  // message, for the effect that actually broke.
  auto messages = pollFxErrorMessages(h.api);
  CHECK(messages.size() == 1);
  if (messages.size() == 1) {
    const std::string& msg = messages[0];
    CHECK(msg.find("\"type\":\"fx_error\"") != std::string::npos);
    CHECK(msg.find("\"effect\":\"verse#1\"") != std::string::npos);  // "breathe" is effects[1]
    CHECK(msg.find("number expected") != std::string::npos);
  }

  // The engine keeps running: the surviving effect's cue-mate is untouched,
  // and re-driving the show doesn't crash or re-disable anything further.
  h.show.evaluate(1.0f / 44.0f, caps, aims);

  // A second poll must NOT re-emit the same effect -- "reported once" has
  // to hold all the way through to the JSON layer, not just at the
  // GlowLuaApi bookkeeping layer (test_glow_lua_api.cpp already proves the
  // bookkeeping half in isolation; this proves it survives serialization
  // too, i.e. nothing here accidentally clears/reuses state).
  CHECK(pollFxErrorMessages(h.api).empty());
}

void test_multiple_broken_effects_in_one_frame_both_reported_in_one_poll() {
  TEST("two effects break in the same frame: ONE poll call delivers BOTH messages, neither dropped");
  std::string fsrc = readFennelSource();
  Harness h(fsrc);

  h.evalOrDie(
      "(fn boom1 [t] (glow.set \"x\" :dimmer 1.0))\n"
      "(fn boom2 [t] (glow.aim 1 \"not-a-table\"))\n"
      "(glow.cue.define :chorus {:effects [boom1 boom2] :priority 0})\n"
      "(glow.cue.go :chorus)\n");

  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;
  h.show.evaluate(0.0f, caps, aims);  // both effects throw on this frame

  // A single poll call must surface both -- GlowLuaApi::pollNewlyDisabledEffects
  // marks both reported in this one call, so a caller that only consumed
  // one message per poll (the bug this test guards against) would lose
  // boom2's notification forever, not just delay it to "next frame".
  auto messages = pollFxErrorMessages(h.api);
  CHECK(messages.size() == 2);
  if (messages.size() == 2) {
    CHECK(messages[0].find("\"effect\":\"chorus#0\"") != std::string::npos);
    CHECK(messages[1].find("\"effect\":\"chorus#1\"") != std::string::npos);
  }

  CHECK(pollFxErrorMessages(h.api).empty());  // both already reported
}

int main() {
  test_broken_effect_produces_correct_fx_error_wire_message();
  test_multiple_broken_effects_in_one_frame_both_reported_in_one_poll();

  if (g_failCount == 0) {
    printf("\nAll tests passed.\n");
    return 0;
  }
  printf("\n%d test(s) failed.\n", g_failCount);
  return 1;
}
