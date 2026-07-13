// test_render_pacing.cpp — host test for the frame-pacing math.
//
// The render loop is the one place the firmware invents real timing logic, so
// it is extracted here and covered by `make test`. The device just calls these
// functions with esp_timer ticks.
#include "render_pacing.h"

#include <cstdio>
#include <cstdint>

static int g_fail = 0;

#define CHECK(cond) do { \
  if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } \
} while (0)

static void test_tick_to_seconds() {
  printf("Test: tickToSeconds basic conversions\n");
  CHECK(glow::tickToSeconds(0) == 0.0f);
  CHECK(glow::tickToSeconds(1'000'000u) == 1.0f);
  CHECK(glow::tickToSeconds(500'000u) == 0.5f);
  // ~44 Hz period in seconds
  float period = glow::tickToSeconds(glow::DEFAULT_RENDER_PERIOD_US);
  CHECK(period > 0.022f && period < 0.023f);
}

static void test_first_frame_seeds_from_now() {
  printf("Test: first frame seeds deadline from nowUs\n");
  uint64_t now = 1'000'000u;  // 1 s
  auto r = glow::paceNextFrame(glow::DEFAULT_RENDER_PERIOD_US, now, 0);
  CHECK(!r.behind);
  CHECK(r.sleepUs == static_cast<int32_t>(glow::DEFAULT_RENDER_PERIOD_US));
  CHECK(r.nextDeadlineUs == now + glow::DEFAULT_RENDER_PERIOD_US);
  CHECK(r.nowSec == 1.0f);
}

static void test_steady_state_pacing() {
  printf("Test: steady-state pacing holds the period\n");
  uint32_t period = glow::DEFAULT_RENDER_PERIOD_US;
  uint64_t now = 0;
  uint64_t deadline = 0;  // first call seeds
  // Simulate 200 frames at exactly the deadline (ideal steady state).
  for (int i = 0; i < 200; ++i) {
    auto r = glow::paceNextFrame(period, now, deadline);
    CHECK(!r.behind);
    CHECK(r.sleepUs == static_cast<int32_t>(period));
    deadline = r.nextDeadlineUs;
    now = deadline;  // woke exactly on time
  }
  // After 200 frames the deadline advanced exactly 200 periods from the seed.
  // Seed deadline was (now0 + period); after 200 frames deadline == now0 + 200*period.
  // (now0 was the first now passed, which we set to 0 on the very first call.)
  uint64_t firstNow = 0;
  uint64_t expected = firstNow + 200ULL * period;
  CHECK(deadline == expected);
}

static void test_behind_frame_does_not_catch_up() {
  printf("Test: behind frame rebases deadline (no catch-up burst)\n");
  uint32_t period = glow::DEFAULT_RENDER_PERIOD_US;
  // Pretend we were supposed to fire at 1,000,000 us but we are firing at
  // 1,000,000 + 2.5 periods (well behind).
  uint64_t prevDeadline = 1'000'000u;
  uint64_t now = prevDeadline + (period * 5u) / 2u;  // 2.5 periods late
  auto r = glow::paceNextFrame(period, now, prevDeadline);
  CHECK(r.behind);
  CHECK(r.sleepUs == 0);
  // Next deadline is one period ahead of NOW, not of the old deadline.
  CHECK(r.nextDeadlineUs == now + period);
  // 2.5 periods elapsed: one of those is ordinary lateness (behind=true
  // already captures it); the other whole period rendered nothing at all.
  CHECK(r.droppedFrames == 1);
}

static void test_slightly_behind_frame_drops_nothing() {
  printf("Test: barely-behind frame (< 2 periods late) drops nothing\n");
  uint32_t period = glow::DEFAULT_RENDER_PERIOD_US;
  uint64_t prevDeadline = 1'000'000u;
  uint64_t now = prevDeadline + period + 1;  // just over one period late
  auto r = glow::paceNextFrame(period, now, prevDeadline);
  CHECK(r.behind);
  CHECK(r.droppedFrames == 0);
}

static void test_gc_pause_sized_gap_drops_many_frames() {
  printf("Test: a GC-pause-sized gap (many periods) reports them as dropped\n");
  uint32_t period = glow::DEFAULT_RENDER_PERIOD_US;
  uint64_t prevDeadline = 1'000'000u;
  uint64_t now = prevDeadline + (period * 20u);  // ~20 periods of silence
  auto r = glow::paceNextFrame(period, now, prevDeadline);
  CHECK(r.behind);
  CHECK(r.droppedFrames == 19);
}

static void test_slight_overrun_recover() {
  printf("Test: slight overrun still targets next deadline (not behind)\n");
  uint32_t period = glow::DEFAULT_RENDER_PERIOD_US;  // ~22727 us
  uint64_t prevDeadline = 5'000'000u;
  // Fire 1 ms past prevDeadline, but still ~21 ms BEFORE the next deadline
  // (prevDeadline + period). This is an overrun of the previous slot but NOT
  // a behind-frame for the next slot.
  uint64_t now = prevDeadline + 1000u;  // 5,001,000
  auto r = glow::paceNextFrame(period, now, prevDeadline);
  uint64_t next = prevDeadline + period;  // 5,022,727
  CHECK(!r.behind);
  CHECK(r.droppedFrames == 0);
  CHECK(r.sleepUs == static_cast<int32_t>(next - now));
  CHECK(r.nextDeadlineUs == next);
}

static void test_cross_deadline_becomes_behind() {
  printf("Test: firing at/after the next deadline is behind\n");
  uint32_t period = glow::DEFAULT_RENDER_PERIOD_US;
  uint64_t prevDeadline = 5'000'000u;
  uint64_t next = prevDeadline + period;
  // Fire exactly at the next deadline -> behind, sleep 0, rebase off now.
  auto r = glow::paceNextFrame(period, next, prevDeadline);
  CHECK(r.behind);
  CHECK(r.sleepUs == 0);
  CHECK(r.nextDeadlineUs == next + period);
}

static void test_zero_period_safe() {
  printf("Test: zero period does not divide by zero / does not crash\n");
  auto r = glow::paceNextFrame(0, 1'000'000u, 500'000u);
  // With period 0, next = base + 0 = base = prevDeadline = 500,000.
  // now (1,000,000) >= 500,000 -> behind, sleep 0, next = now + 0 = now.
  CHECK(r.behind);
  CHECK(r.sleepUs == 0);
  CHECK(r.nextDeadlineUs == 1'000'000u);
}

static void test_64bit_large_tick_safe() {
  printf("Test: large tick value (no overflow) paces normally\n");
  // Pick a huge value such that huge + period still fits in u64 (no wrap).
  // 0xFFFFFFFFFFFF0000 + 22727 < 0xFFFFFFFFFFFFFFFF.
  uint64_t huge = 0xFFFFFFFFFFFF0000ULL;
  auto r = glow::paceNextFrame(glow::DEFAULT_RENDER_PERIOD_US, huge, 0);
  CHECK(!r.behind);
  CHECK(r.nextDeadlineUs == huge + glow::DEFAULT_RENDER_PERIOD_US);
  CHECK(r.sleepUs == static_cast<int32_t>(glow::DEFAULT_RENDER_PERIOD_US));
}

static void test_64bit_wrap_is_defined_not_crash() {
  printf("Test: tick so large that deadline wraps is defined behaviour\n");
  // huge + period DOES overflow u64 here. Unsigned wrap is well-defined; the
  // helper must not crash or invoke UB. The wrapped nextDeadline is tiny, so
  // now >= next is true -> behind, sleep 0, rebase off now.
  uint64_t huge = 0xFFFFFFFFFFFFFF00ULL;
  auto r = glow::paceNextFrame(glow::DEFAULT_RENDER_PERIOD_US, huge, 0);
  CHECK(r.behind);
  CHECK(r.sleepUs == 0);
  // No UB assertion: reaching here is the contract.
}

int main() {
  test_tick_to_seconds();
  test_first_frame_seeds_from_now();
  test_steady_state_pacing();
  test_behind_frame_does_not_catch_up();
  test_slightly_behind_frame_drops_nothing();
  test_gc_pause_sized_gap_drops_many_frames();
  test_slight_overrun_recover();
  test_cross_deadline_becomes_behind();
  test_zero_period_safe();
  test_64bit_large_tick_safe();
  test_64bit_wrap_is_defined_not_crash();
  if (g_fail == 0) {
    printf("All render_pacing tests passed!\n");
    return 0;
  }
  printf("%d render_pacing tests FAILED\n", g_fail);
  return 1;
}
