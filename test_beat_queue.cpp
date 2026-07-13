#include "beat_queue.h"
#include "beat_clock.h"

#include <cstdio>
#include <cmath>
#include <thread>
#include <vector>

using glow::BeatClock;
using glow::BeatEvent;
using glow::RingBeatEventQueue;
using glow::pumpBeatEvents;

static int g_failCount = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      g_failCount++; \
    } \
  } while (0)

#define TEST(name) printf("Test: %s\n", name)

static BeatEvent mkEvent(uint64_t tUs, float bpm = 120.0f) {
  return BeatEvent{tUs, bpm, 0, false};
}

void test_fifo() {
  TEST("FIFO: push A,B,C -> pop returns A,B,C in order; then pop -> false");
  RingBeatEventQueue q(8);

  q.push(mkEvent(1));
  q.push(mkEvent(2));
  q.push(mkEvent(3));

  BeatEvent ev;
  CHECK(q.pop(ev) && ev.tUs == 1);
  CHECK(q.pop(ev) && ev.tUs == 2);
  CHECK(q.pop(ev) && ev.tUs == 3);
  CHECK(!q.pop(ev));
}

void test_full() {
  TEST("Full: capacity 2, push 3 -> third returns false, dropped()==1");
  RingBeatEventQueue q(2);

  CHECK(q.push(mkEvent(1)));
  CHECK(q.push(mkEvent(2)));
  CHECK(!q.push(mkEvent(3)));
  CHECK(q.dropped() == 1);

  BeatEvent ev;
  CHECK(q.pop(ev) && ev.tUs == 1);
  CHECK(q.pop(ev) && ev.tUs == 2);
  CHECK(!q.pop(ev));
}

void test_empty() {
  TEST("Empty: fresh queue, pop -> false");
  RingBeatEventQueue q(8);
  BeatEvent ev;
  CHECK(!q.pop(ev));
  CHECK(q.size() == 0);
}

void test_pump_feeds_clock() {
  TEST("pumpBeatEvents: drains the queue into BeatClock::onBeat, in order");
  RingBeatEventQueue q(8);
  BeatClock c;

  uint64_t t = 0;
  for (int i = 0; i < 5; ++i) {
    q.push(mkEvent(t, 120.0f));
    t += 500000;
  }
  int n = pumpBeatEvents(q, c);
  CHECK(n == 5);
  CHECK(std::fabs(c.bpm() - 120.0f) < 0.5f);
  CHECK(q.size() == 0);
}

void test_max_per_frame() {
  TEST("maxPerFrame bound: 5 events, maxPerFrame=2 -> 2 dispatched, 3 left");
  RingBeatEventQueue q(8);
  BeatClock c;

  uint64_t t = 0;
  for (int i = 0; i < 5; ++i) {
    q.push(mkEvent(t, 120.0f));
    t += 500000;
  }

  int n = pumpBeatEvents(q, c, 2);
  CHECK(n == 2);
  CHECK(q.size() == 3);

  n = pumpBeatEvents(q, c, 64);
  CHECK(n == 3);
  CHECK(q.size() == 0);
}

void test_concurrency() {
  TEST("Concurrency: 1 producer + 1 consumer, 10000 events, FIFO, TSan clean");

  const int N = 10000;
  RingBeatEventQueue q(N / 4);

  std::thread producer([&]() {
    for (int i = 0; i < N; i++) {
      BeatEvent ev = mkEvent(static_cast<uint64_t>(i), 120.0f);
      while (!q.push(ev)) {
        std::this_thread::yield();
      }
    }
  });

  std::vector<uint64_t> received;
  received.reserve(N);
  BeatEvent ev;
  while (received.size() < static_cast<size_t>(N)) {
    if (q.pop(ev)) {
      received.push_back(ev.tUs);
    } else {
      std::this_thread::yield();
    }
  }
  producer.join();

  while (q.pop(ev)) {
    received.push_back(ev.tUs);
  }

  CHECK(received.size() == static_cast<size_t>(N));
  for (int i = 0; i < N; i++) {
    if (received[i] != static_cast<uint64_t>(i)) {
      printf("FAIL: %s:%d: received[%d]=%llu, expected %llu\n",
             __FILE__, __LINE__, i, (unsigned long long)received[i], (unsigned long long)i);
      g_failCount++;
      break;
    }
  }
}

int main() {
  test_fifo();
  test_full();
  test_empty();
  test_pump_feeds_clock();
  test_max_per_frame();
  test_concurrency();

  if (g_failCount == 0) {
    printf("\nAll tests passed.\n");
    return 0;
  } else {
    printf("\n%d test(s) failed.\n", g_failCount);
    return 1;
  }
}
