// test_glow_fennel.cpp — the process-wide VM singleton, glow_lua_eval_fennel,
// and the eval submission queue drain. This is the "definition of done"
// module: a syntax error, a runtime error, an infinite loop, and an
// out-of-memory must each leave the rig still rendering.

#include "glow_fennel.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "beat_clock.h"
#include "eval_queue.h"
#include "glow_lua_api.h"
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

std::string g_fennelSrc;

const std::string& fennelSrc() {
  if (g_fennelSrc.empty()) {
    std::ifstream f("third_party/fennel/fennel.lua", std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    g_fennelSrc = ss.str();
  }
  return g_fennelSrc;
}

// Every test starts from a clean singleton (host-tests-only capability;
// see glow_fennel.h). ShowController must outlive the VM, so it's a static
// here too, recreated per test via placement into this holder. Same for
// the BeatClock glowLuaInit borrows.
ShowController* g_show = nullptr;
glow::BeatClock* g_beatClock = nullptr;

void freshInit(size_t capBytes = 0, int frameBudget = 0, int evalBudget = 0) {
  glow::glowLuaShutdown();
  delete g_show;
  g_show = new ShowController();
  delete g_beatClock;
  g_beatClock = new glow::BeatClock();
  char err[256];
  bool ok = glow::glowLuaInit(*g_show, nullptr, *g_beatClock, fennelSrc().data(), fennelSrc().size(), err,
                              sizeof(err), capBytes, frameBudget, evalBudget);
  if (!ok) {
    printf("FATAL: glowLuaInit failed: %s\n", err);
    std::abort();
  }
}

}  // namespace

void test_init_and_eval_basic() {
  TEST("glowLuaInit + glow_lua_eval_fennel: basic cue define/go round-trip");
  freshInit();

  const char* src =
      "(fn f [t] (glow.set 3 :dimmer 0.25)) "
      "(glow.cue.define :c {:effects [f] :priority 0}) "
      "(glow.cue.go :c)";
  char err[256];
  bool ok = glow_lua_eval_fennel(src, std::strlen(src), err, sizeof(err));
  CHECK(ok);

  uint16_t id;
  CHECK(glow::glowLuaApi().cueIdForName("c", id));
  CHECK(g_show->isActive(id));

  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;
  g_show->evaluate(0.0f, caps, aims);
  CHECK(caps.size() == 1);
  CHECK(caps[0].fixtureId == 3);
}

void test_syntax_error_leaves_vm_usable() {
  TEST("a syntax error is reported and the VM keeps working afterward");
  freshInit();
  char err[256];
  const char* bad = "(this is not valid fennel";
  bool ok = glow_lua_eval_fennel(bad, std::strlen(bad), err, sizeof(err));
  CHECK(!ok);
  CHECK(std::strlen(err) > 0);

  const char* good = "(glow.cue.define :ok {:effects [] :priority 0})";
  ok = glow_lua_eval_fennel(good, std::strlen(good), err, sizeof(err));
  CHECK(ok);
}

void test_runtime_error_leaves_vm_usable() {
  TEST("a runtime error is reported and the VM keeps working afterward");
  freshInit();
  char err[256];
  const char* bad = "(error \"boom at top level\")";
  bool ok = glow_lua_eval_fennel(bad, std::strlen(bad), err, sizeof(err));
  CHECK(!ok);
  CHECK(std::string(err).find("boom") != std::string::npos);

  const char* good = "(glow.cue.define :ok {:effects [] :priority 0})";
  ok = glow_lua_eval_fennel(good, std::strlen(good), err, sizeof(err));
  CHECK(ok);
}

void test_infinite_loop_aborted_leaves_vm_usable() {
  TEST("a top-level infinite loop is aborted in bounded time; VM keeps working");
  // Small eval budget so the test doesn't burn real time.
  freshInit(0, 0, /*evalInstrBudget=*/50000);
  char err[256];
  const char* loop = "(fn f [] (while true nil)) (f)";
  bool ok = glow_lua_eval_fennel(loop, std::strlen(loop), err, sizeof(err));
  CHECK(!ok);
  CHECK(std::string(err).find("instruction budget") != std::string::npos);

  const char* good = "(glow.cue.define :ok {:effects [] :priority 0})";
  ok = glow_lua_eval_fennel(good, std::strlen(good), err, sizeof(err));
  CHECK(ok);
}

void test_out_of_memory_leaves_vm_usable() {
  TEST("compiling/running something past the memory cap fails cleanly; VM keeps working");
  // Cap barely big enough to load Fennel; a big allocation inside eval
  // should be rejected rather than growing unbounded.
  freshInit(/*capBytes=*/900 * 1024, 0, /*evalInstrBudget=*/5'000'000);
  char err[256];
  const char* hog = "(local t {}) (for [i 1 2000000] (tset t i (.. \"x\" (tostring i))))";
  bool ok = glow_lua_eval_fennel(hog, std::strlen(hog), err, sizeof(err));
  CHECK(!ok);

  const char* good = "(glow.cue.define :ok {:effects [] :priority 0})";
  ok = glow_lua_eval_fennel(good, std::strlen(good), err, sizeof(err));
  CHECK(ok);
}

void test_eval_before_init_fails_cleanly() {
  TEST("glow_lua_eval_fennel before glowLuaInit reports an error, doesn't crash");
  glow::glowLuaShutdown();
  char err[256];
  bool ok = glow_lua_eval_fennel("(+ 1 2)", 7, err, sizeof(err));
  CHECK(!ok);
  CHECK(std::strlen(err) > 0);
}

void test_boot_script_blackout_contract() {
  TEST("a broken boot.fnl reports failure so the caller can fall back to blackout");
  freshInit();
  char err[256];
  const char* brokenBoot = "(glow.cue.define :x {:effects [(error \"nope\")] :priority 0})";
  bool ok = glow_lua_eval_fennel(brokenBoot, std::strlen(brokenBoot), err, sizeof(err));
  // The `(error ...)` call happens while building the :effects list itself
  // (evaluating the form (error "nope") to get a value), so the whole
  // define call fails -- exactly the "never boot into a broken show"
  // signal the caller (main.cpp) uses to fall back to blackout.
  CHECK(!ok);
}

// ---------------------------------------------------------------------------
// Eval submission queue drain (the WS path)
// ---------------------------------------------------------------------------

namespace {
struct ResultLog {
  std::vector<uint32_t> ids;
  std::vector<bool> oks;
};
void collectResult(void* ctx, uint32_t id, bool ok, const char* /*err*/) {
  auto* log = static_cast<ResultLog*>(ctx);
  log->ids.push_back(id);
  log->oks.push_back(ok);
}
}  // namespace

void test_pump_eval_submissions_dispatches_and_reports() {
  TEST("pumpEvalSubmissions drains the queue and reports ok/err per request");
  freshInit();

  RingEvalSubmissionQueue q(8);
  EvalSubmission sub;
  const char* good = "(glow.cue.define :ok {:effects [] :priority 0})";
  makeEvalSubmission(good, std::strlen(good), 1, sub);
  q.push(sub);
  const char* bad = "(broken (((";
  makeEvalSubmission(bad, std::strlen(bad), 2, sub);
  q.push(sub);

  ResultLog log;
  int n = glow::pumpEvalSubmissions(q, 10, &collectResult, &log);
  CHECK(n == 2);
  CHECK(log.ids.size() == 2);
  CHECK(log.ids[0] == 1 && log.oks[0] == true);
  CHECK(log.ids[1] == 2 && log.oks[1] == false);
}

void test_pump_eval_submissions_respects_max_per_frame() {
  TEST("pumpEvalSubmissions bounds work per frame, leaving the rest queued");
  freshInit();
  RingEvalSubmissionQueue q(8);
  EvalSubmission sub;
  const char* good = "(glow.cue.define :ok {:effects [] :priority 0})";
  for (int i = 0; i < 5; ++i) {
    makeEvalSubmission(good, std::strlen(good), static_cast<uint32_t>(i), sub);
    q.push(sub);
  }
  ResultLog log;
  int n = glow::pumpEvalSubmissions(q, 2, &collectResult, &log);
  CHECK(n == 2);
  CHECK(q.size() == 3);  // the rest stayed queued for the next frame
}

int main() {
  test_init_and_eval_basic();
  test_syntax_error_leaves_vm_usable();
  test_runtime_error_leaves_vm_usable();
  test_infinite_loop_aborted_leaves_vm_usable();
  test_out_of_memory_leaves_vm_usable();
  test_eval_before_init_fails_cleanly();
  test_boot_script_blackout_contract();
  test_pump_eval_submissions_dispatches_and_reports();
  test_pump_eval_submissions_respects_max_per_frame();

  glow::glowLuaShutdown();
  delete g_show;

  if (g_failCount == 0) {
    printf("\nAll tests passed.\n");
    return 0;
  }
  printf("\n%d test(s) failed.\n", g_failCount);
  return 1;
}
