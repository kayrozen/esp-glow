#include "control_queue.h"
#include "live_control.h"
#include "show_control.h"
#include "fixture_profile.h"
#include "profile_encoder.h"

#include <cstdio>
#include <cmath>
#include <thread>
#include <vector>
#include <string>

static int g_failCount = 0;
static constexpr float EPSILON = 1e-4f;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      g_failCount++; \
    } \
  } while (0)

#define CHECK_NEAR(a, b) \
  do { \
    if (std::fabs((a) - (b)) > EPSILON) { \
      printf("FAIL: %s:%d: %f != %f\n", __FILE__, __LINE__, (float)(a), (float)(b)); \
      g_failCount++; \
    } \
  } while (0)

#define TEST(name) printf("Test: %s\n", name)

// Reusable effect for ShowController setup (same pattern as test_live_control).
struct ConstCapEffect : IEffect {
  uint16_t id = 0;
  Capability cap = Capability::Dimmer;
  float v = 0.0f;
  ConstCapEffect() = default;
  ConstCapEffect(uint16_t i, Capability c, float val) : id(i), cap(c), v(val) {}
  void evaluate(float, std::vector<CapIntent>& c, std::vector<AimIntent>&) override {
    c.push_back({id, cap, v});
  }
};

// ---------------------------------------------------------------------------
// Test 1: FIFO ordering
// ---------------------------------------------------------------------------

void test_fifo() {
  TEST("FIFO: push A,B,C -> pop returns A,B,C in order; then pop -> false");
  RingControlEventQueue q(8);

  q.push({ControlType::Button, 1, true,  0.0f});
  q.push({ControlType::Button, 2, true,  0.0f});
  q.push({ControlType::Button, 3, true,  0.0f});

  ControlEvent ev;
  CHECK(q.pop(ev) && ev.id == 1);
  CHECK(q.pop(ev) && ev.id == 2);
  CHECK(q.pop(ev) && ev.id == 3);
  CHECK(!q.pop(ev));  // empty
}

// ---------------------------------------------------------------------------
// Test 2: Full queue rejects newest, increments dropped()
// ---------------------------------------------------------------------------

void test_full() {
  TEST("Full: capacity 2, push 3 -> third returns false, dropped()==1");
  RingControlEventQueue q(2);

  CHECK(q.push({ControlType::Button, 1, true, 0.0f}));
  CHECK(q.push({ControlType::Button, 2, true, 0.0f}));
  CHECK(!q.push({ControlType::Button, 3, true, 0.0f}));  // full
  CHECK(q.dropped() == 1);

  ControlEvent ev;
  CHECK(q.pop(ev) && ev.id == 1);  // oldest two survive
  CHECK(q.pop(ev) && ev.id == 2);
  CHECK(!q.pop(ev));
}

// ---------------------------------------------------------------------------
// Test 3: Empty queue pop returns false
// ---------------------------------------------------------------------------

void test_empty() {
  TEST("Empty: fresh queue, pop -> false");
  RingControlEventQueue q(8);
  ControlEvent ev;
  CHECK(!q.pop(ev));
  CHECK(q.size() == 0);
}

// ---------------------------------------------------------------------------
// Test 4: pump dispatches in order (CueFlash press/release)
// ---------------------------------------------------------------------------

void test_pump_dispatch() {
  TEST("pump dispatches in order: CueFlash press -> active, release -> inactive");

  ShowController ctrl;
  std::vector<IEffect*> effects;
  effects.push_back(new ConstCapEffect{1, Capability::Dimmer, 1.0f});
  uint16_t cueId = ctrl.addCue(effects, 0.0f, 0.0f, 0, 0.0f);

  LiveControl live(ctrl);
  live.bindButton(60, ActionKind::CueFlash, cueId);

  RingControlEventQueue q(8);

  // Enqueue press, pump -> cue active
  q.push({ControlType::Button, 60, true, 0.0f});
  int n = pumpControlEvents(q, live, 0.0f);
  CHECK(n == 1);
  std::vector<CapIntent> caps;
  std::vector<AimIntent> aims;
  ctrl.evaluate(0.0f, caps, aims);
  CHECK(ctrl.isActive(cueId));

  // Enqueue release, pump -> cue inactive
  q.push({ControlType::Button, 60, false, 0.0f});
  n = pumpControlEvents(q, live, 1.0f);
  CHECK(n == 1);
  caps.clear();
  aims.clear();
  ctrl.evaluate(1.0f, caps, aims);
  CHECK(!ctrl.isActive(cueId));

  delete effects[0];
}

// ---------------------------------------------------------------------------
// Test 5: maxPerFrame bounds dispatch, leftovers stay queued
// ---------------------------------------------------------------------------

void test_max_per_frame() {
  TEST("maxPerFrame bound: 5 events, maxPerFrame=2 -> 2 dispatched, 3 left");

  ShowController ctrl;
  LiveControl live(ctrl);  // no bindings — events are no-ops, that's fine
  RingControlEventQueue q(8);

  for (int i = 0; i < 5; i++) {
    q.push({ControlType::Button, (uint16_t)i, true, 0.0f});
  }

  int n = pumpControlEvents(q, live, 0.0f, 2);
  CHECK(n == 2);
  CHECK(q.size() == 3);

  n = pumpControlEvents(q, live, 0.0f, 64);
  CHECK(n == 3);
  CHECK(q.size() == 0);
}

// ---------------------------------------------------------------------------
// Test 6: Equivalence — queue+pump produces same state as direct handle
// ---------------------------------------------------------------------------

void test_equivalence() {
  TEST("Equivalence: same event stream via queue+pump == direct handle");

  // Two identical setups. Cues with fadeIn/out 0 for deterministic state.
  ShowController ctrlA, ctrlB;
  std::vector<IEffect*> effA, effB;
  effA.push_back(new ConstCapEffect{1, Capability::Dimmer, 1.0f});
  effB.push_back(new ConstCapEffect{1, Capability::Dimmer, 1.0f});
  uint16_t cueA = ctrlA.addCue(effA, 0.0f, 0.0f, 0, 0.0f);
  uint16_t cueB = ctrlB.addCue(effB, 0.0f, 0.0f, 0, 0.0f);

  LiveControl liveA(ctrlA);
  LiveControl liveB(ctrlB);
  liveA.bindButton(60, ActionKind::CueToggle, cueA);
  liveB.bindButton(60, ActionKind::CueToggle, cueB);

  // Event stream: press (latch on), press (latch off), press (latch on)
  std::vector<ControlEvent> events = {
    {ControlType::Button, 60, true, 0.0f},
    {ControlType::Button, 60, true, 0.0f},
    {ControlType::Button, 60, true, 0.0f},
  };

  // Path A: direct handle, one event at a time at t=0,1,2
  for (size_t i = 0; i < events.size(); i++) {
    liveA.handle(events[i], (float)i);
  }

  // Path B: enqueue all, pump at the same ts
  RingControlEventQueue q(8);
  for (auto& ev : events) q.push(ev);
  for (size_t i = 0; i < events.size(); i++) {
    pumpControlEvents(q, liveB, (float)i);
  }

  // Compare final state
  CHECK(ctrlA.isActive(cueA) == ctrlB.isActive(cueB));

  delete effA[0];
  delete effB[0];
}

// ---------------------------------------------------------------------------
// Test 7: Concurrency — must pass under -fsanitize=thread
// ---------------------------------------------------------------------------

void test_concurrency() {
  TEST("Concurrency: 1 producer + 1 consumer, 10000 events, FIFO, TSan clean");

  const int N = 10000;
  // Capacity < N so the ring wraps multiple times, exercising the
  // modulo arithmetic under concurrent access. The producer retries on
  // push failure (queue full) to guarantee every event is eventually
  // enqueued.
  RingControlEventQueue q(N / 4);

  std::thread producer([&]() {
    for (int i = 0; i < N; i++) {
      ControlEvent ev{ControlType::Button, (uint16_t)(i & 0xFFFF), true, 0.0f};
      while (!q.push(ev)) {
        // Queue full; yield and retry. The yield is important: without
        // it, a tight spin on the mutex would starve the consumer.
        std::this_thread::yield();
      }
    }
  });

  std::vector<uint16_t> received;
  received.reserve(N);
  ControlEvent ev;
  while (received.size() < N) {
    if (q.pop(ev)) {
      received.push_back(ev.id);
    } else {
      std::this_thread::yield();
    }
  }
  producer.join();

  // Drain any stragglers (shouldn't be any, but be safe).
  while (q.pop(ev)) {
    received.push_back(ev.id);
  }

  // Every event received exactly once, in FIFO order.
  CHECK(received.size() == (size_t)N);
  for (int i = 0; i < N; i++) {
    if (received[i] != (uint16_t)(i & 0xFFFF)) {
      printf("FAIL: %s:%d: received[%d]=%u, expected %u\n",
             __FILE__, __LINE__, i, received[i], (uint16_t)(i & 0xFFFF));
      g_failCount++;
      break;  // report first mismatch only
    }
  }

  // No events dropped (the producer retried until every push succeeded).
}

// ---------------------------------------------------------------------------

int main() {
  test_fifo();
  test_full();
  test_empty();
  test_pump_dispatch();
  test_max_per_frame();
  test_equivalence();
  test_concurrency();

  if (g_failCount == 0) {
    printf("\nAll tests passed.\n");
    return 0;
  } else {
    printf("\n%d test(s) failed.\n", g_failCount);
    return 1;
  }
}
