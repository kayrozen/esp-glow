// test_beat_clock.cpp — host tests for the PLL-smoothed musical-time clock.
//
// This is where the value of the whole musical-time feature lives: a beat
// clock that jitters makes every synced effect look wrong, and that is
// not something you can debug on a dance floor. Test it like a
// signal-processing component, not a state machine.
#include "beat_clock.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <random>

static int g_fail = 0;

#define CHECK(cond) do { \
  if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } \
} while (0)

#define TEST(name) printf("Test: %s\n", name)

using glow::BeatClock;
using glow::BeatEvent;

static constexpr double kUsPerBeat120 = 500000.0;  // 120 BPM

// --- 1. Steady 120 BPM -------------------------------------------------

static void test_steady_120bpm() {
  TEST("steady 120 BPM: bpm(), phase ramp, beatNumber incrementing by 1.0/beat");
  BeatClock c;
  uint64_t t = 0;
  // Settle for a bunch of beats first.
  for (int i = 0; i < 20; ++i) {
    c.onBeat(BeatEvent{t, 120.0f, 0, false});
    t += static_cast<uint64_t>(kUsPerBeat120);
  }
  CHECK(std::fabs(c.bpm() - 120.0f) < 0.5f);

  double prevBeatNum = c.beatNumber(t);
  for (int i = 0; i < 10; ++i) {
    uint64_t beatStart = t;
    c.onBeat(BeatEvent{beatStart, 120.0f, 0, false});
    double bnAtStart = c.beatNumber(beatStart);
    // beatNumber increments by ~1.0 per beat (settled clock, so the
    // correction at this instant is negligible).
    CHECK(std::fabs(bnAtStart - std::round(bnAtStart)) < 0.01);
    if (i > 0) {
      CHECK(std::fabs((bnAtStart - prevBeatNum) - 1.0) < 0.02);
    }
    prevBeatNum = bnAtStart;

    // Phase ramps 0 -> 1 across the beat and wraps exactly at the boundary.
    float phaseAtStart = c.beatPhase(beatStart);
    CHECK(phaseAtStart < 0.02f);
    uint64_t mid = beatStart + static_cast<uint64_t>(kUsPerBeat120 / 2.0);
    float phaseAtMid = c.beatPhase(mid);
    CHECK(phaseAtMid > 0.45f && phaseAtMid < 0.55f);

    t += static_cast<uint64_t>(kUsPerBeat120);
  }
}

// --- 2. Interpolation ----------------------------------------------------

static void test_interpolation_continuous() {
  TEST("interpolation: 100 points between two beats -> monotonic, continuous phase");
  BeatClock c;
  uint64_t t = 0;
  for (int i = 0; i < 10; ++i) {
    c.onBeat(BeatEvent{t, 120.0f, 0, false});
    t += static_cast<uint64_t>(kUsPerBeat120);
  }
  uint64_t beatStart = t - static_cast<uint64_t>(kUsPerBeat120);
  double prevPhase = -1.0;
  for (int i = 0; i < 100; ++i) {
    uint64_t q = beatStart + static_cast<uint64_t>(kUsPerBeat120 * i / 100.0);
    double phase = c.beatPhase(q);
    if (i > 0) {
      CHECK(phase > prevPhase);            // strictly increasing within the beat
      CHECK(phase - prevPhase < 0.02);      // no jump between adjacent samples
    }
    prevPhase = phase;
  }
}

// --- 3. Jitter rejection (the test that proves the PLL works) -----------

static void test_jitter_rejection() {
  TEST("jitter: +-10ms noise on 120 BPM beats -> bpm within +-2, no phase snap");
  BeatClock c;
  std::mt19937 rng(12345);
  std::uniform_real_distribution<double> jitterUs(-10000.0, 10000.0);

  double nominal = 1'000'000.0;  // offset well clear of 0 so "- jitter" never underflows
  double maxCorrectionJump = 0.0;
  double maxInterpStep = 0.0;
  double prevInterp = -1.0;

  for (int i = 0; i < 200; ++i) {
    double jitter = jitterUs(rng);
    uint64_t tUs = static_cast<uint64_t>(nominal + jitter);

    // The precise thing "no visible snapping" means: beatNumber at this
    // exact instant, evaluated an instant before vs. an instant after the
    // correction, must not jump by more than a small amount. This isolates
    // the correction itself from ordinary continuous advance of time (a
    // dense stitched-together sample of the whole timeline conflates the
    // two -- gaps between sample windows look like "jumps" that are just
    // elapsed time).
    double before = c.beatNumber(tUs);
    c.onBeat(BeatEvent{tUs, 0.0f, 0, false});  // bpm unknown -- derive from interval, like MIDI clock
    double after = c.beatNumber(tUs);
    if (i > 5) {  // let the PLL settle first
      double jump = after - before;
      CHECK(jump >= -1e-9);  // never backward
      if (jump > maxCorrectionJump) maxCorrectionJump = jump;

      // Also sanity-check plain interpolation stays continuous within the
      // interval that follows (no separate snap between beat events).
      // Reset per-event: the boundary jump is already measured above by
      // maxCorrectionJump, so this only covers samples strictly after it.
      prevInterp = after;
      for (int s = 1; s <= 10; ++s) {
        uint64_t q = tUs + static_cast<uint64_t>(kUsPerBeat120 * s / 20.0);
        double bn = c.beatNumber(q);
        double step = bn - prevInterp;
        CHECK(step >= -1e-9);
        if (step > maxInterpStep) maxInterpStep = step;
        prevInterp = bn;
      }
    }
    nominal += kUsPerBeat120;
  }

  CHECK(std::fabs(c.bpm() - 120.0f) < 2.0f);
  // The correction itself must be a small nudge, not a snap: bounded by
  // the PLL's forward-only phase gain (kPhaseGain * 0.5 beats worst case).
  CHECK(maxCorrectionJump < 0.1);
  // Interpolation steps between consecutive dense samples (1/20th of a
  // beat apart) should track that spacing, not spike.
  CHECK(maxInterpStep < 0.1);
}

// --- 4. Tempo change -----------------------------------------------------

static void test_tempo_change_converges() {
  TEST("tempo change 120 -> 128 BPM: converges within a few beats, no snap");
  BeatClock c;
  uint64_t t = 0;
  for (int i = 0; i < 15; ++i) {
    c.onBeat(BeatEvent{t, 120.0f, 0, false});
    t += static_cast<uint64_t>(kUsPerBeat120);
  }
  CHECK(std::fabs(c.bpm() - 120.0f) < 1.0f);

  const double kUsPerBeat128 = 60.0e6 / 128.0;
  double prevBeatNumber = c.beatNumber(t);
  double maxStep = 0.0;
  int beatsToConverge = -1;
  for (int i = 0; i < 30; ++i) {
    c.onBeat(BeatEvent{t, 128.0f, 0, false});
    double bnNow = c.beatNumber(t);
    double step = bnNow - prevBeatNumber;
    CHECK(step >= -1e-9);
    if (step > maxStep) maxStep = step;
    prevBeatNumber = bnNow;

    if (beatsToConverge < 0 && std::fabs(c.bpm() - 128.0f) < 0.5f) beatsToConverge = i;
    t += static_cast<uint64_t>(kUsPerBeat128);
  }
  CHECK(std::fabs(c.bpm() - 128.0f) < 0.5f);
  CHECK(beatsToConverge >= 0 && beatsToConverge < 20);
  // No single beat-to-beat step should look like a snap (a settled 1
  // beat/beat cadence plus a small correction, not a multi-beat jump).
  CHECK(maxStep < 1.3);
}

// --- 5. Garbage rejection --------------------------------------------------

static void test_garbage_rejection() {
  TEST("garbage: a beat implying 900 BPM is ignored; estimate unchanged");
  BeatClock c;
  uint64_t t = 0;
  for (int i = 0; i < 10; ++i) {
    c.onBeat(BeatEvent{t, 120.0f, 0, false});
    t += static_cast<uint64_t>(kUsPerBeat120);
  }
  float bpmBefore = c.bpm();
  double beatNumBefore = c.beatNumber(t);

  c.onBeat(BeatEvent{t, 900.0f, 0, false});  // garbage

  CHECK(c.bpm() == bpmBefore);
  CHECK(c.beatNumber(t) == beatNumBefore);

  // The clock keeps working normally afterward -- the garbage event left
  // no residue (e.g. it did not corrupt the "last real beat" timestamp
  // used to compute the next interval).
  t += static_cast<uint64_t>(kUsPerBeat120);
  c.onBeat(BeatEvent{t, 120.0f, 0, false});
  CHECK(std::fabs(c.bpm() - 120.0f) < 1.0f);
}

static void test_garbage_period_jump_rejected() {
  TEST("garbage: a reported tempo within sane bounds but wildly off the current estimate is ignored");
  BeatClock c;
  uint64_t t = 0;
  for (int i = 0; i < 10; ++i) {
    c.onBeat(BeatEvent{t, 120.0f, 0, false});
    t += static_cast<uint64_t>(kUsPerBeat120);
  }
  float bpmBefore = c.bpm();
  // 50 BPM is within [kMinBpm,kMaxBpm] (passes the absolute sanity gate)
  // but implies a period 2.4x the locked-in ~500ms estimate -- a single
  // packet like this must not be allowed to yank the tempo around.
  c.onBeat(BeatEvent{t, 50.0f, 0, false});
  CHECK(c.bpm() == bpmBefore);
}

// --- 6. Dropout ------------------------------------------------------------

static void test_dropout_freeruns_then_unlocks() {
  TEST("dropout: free-runs on last period, locked() goes false after timeout");
  BeatClock c;
  uint64_t t = 0;
  for (int i = 0; i < 10; ++i) {
    c.onBeat(BeatEvent{t, 120.0f, 0, false});
    t += static_cast<uint64_t>(kUsPerBeat120);
  }
  uint64_t lastBeatT = t - static_cast<uint64_t>(kUsPerBeat120);
  CHECK(c.locked());

  // Shortly after the last beat: still free-running, still locked.
  uint64_t soon = lastBeatT + static_cast<uint64_t>(kUsPerBeat120 * 2.5);
  double bnSoon = c.beatNumber(soon);
  CHECK(bnSoon > 0.0);  // kept advancing with no new events
  CHECK(c.locked());

  // Well past the lock timeout: no longer locked, but still produces a
  // sane, still-advancing phase (the show does not stop).
  uint64_t late = lastBeatT + static_cast<uint64_t>(kUsPerBeat120 * 20.0);
  double bnLate = c.beatNumber(late);
  CHECK(bnLate > bnSoon);
  CHECK(!c.locked());
  CHECK(std::isfinite(c.beatPhase(late)));
}

// --- 7. Monotonicity across everything -------------------------------------

static void test_monotonic_beat_number_long_sequence() {
  TEST("monotonicity: beatNumber never decreases across jitter/tempo/dropout/taps");
  BeatClock c;
  std::mt19937 rng(999);
  std::uniform_real_distribution<double> jitterUs(-15000.0, 15000.0);

  double prev = -1.0;
  uint64_t t = 1'000'000;  // offset well clear of 0 so "+ negative jitter" never underflows
  double period = kUsPerBeat120;

  // Queries must be issued in non-decreasing tUs order to test what the
  // guarantee actually promises (beatNumber(t) monotonic as t advances,
  // exactly how the render task calls it every frame) -- so clamp every
  // requested q to never go backward relative to the last one issued.
  uint64_t qFloor = 0;
  auto sampleAndCheck = [&](uint64_t q) {
    if (q < qFloor) q = qFloor;
    qFloor = q;
    double bn = c.beatNumber(q);
    if (bn < prev - 1e-9) {
      printf("  DEBUG: backward step at q=%llu prev=%.9f bn=%.9f delta=%.9f\n",
             (unsigned long long)q, prev, bn, bn - prev);
    }
    CHECK(bn >= prev - 1e-9);
    prev = bn;
  };

  // Phase 1: steady with jitter.
  for (int i = 0; i < 100; ++i) {
    uint64_t tUs = t + static_cast<uint64_t>(jitterUs(rng));
    c.onBeat(BeatEvent{tUs, 0.0f, 0, false});
    for (int s = 0; s < 5; ++s) sampleAndCheck(tUs + static_cast<uint64_t>(period * s / 5.0));
    t += static_cast<uint64_t>(period);
  }

  // Phase 2: a tempo ramp.
  for (int i = 0; i < 60; ++i) {
    period = kUsPerBeat120 - i * 500.0;  // gradually speeding up
    c.onBeat(BeatEvent{t, static_cast<float>(60.0e6 / period), 0, false});
    for (int s = 0; s < 5; ++s) sampleAndCheck(t + static_cast<uint64_t>(period * s / 5.0));
    t += static_cast<uint64_t>(period);
  }

  // Phase 3: a dropout (no events), just querying.
  uint64_t dropoutEnd = t + static_cast<uint64_t>(period * 30.0);
  for (uint64_t q = t; q < dropoutEnd; q += static_cast<uint64_t>(period / 4.0)) {
    sampleAndCheck(q);
  }
  t = dropoutEnd;

  // Phase 4: taps resuming the clock.
  for (int i = 0; i < 6; ++i) {
    c.tap(t);
    sampleAndCheck(t);
    t += static_cast<uint64_t>(kUsPerBeat120);
  }

  // Phase 5: a garbage event mixed in, must not perturb monotonicity.
  c.onBeat(BeatEvent{t, 900.0f, 0, false});
  sampleAndCheck(t);
}

// --- 8. Tap tempo ------------------------------------------------------------

static void test_tap_tempo_basic() {
  TEST("tap tempo: 4 taps at 500ms -> ~120 BPM");
  BeatClock c;
  uint64_t t = 0;
  for (int i = 0; i < 4; ++i) {
    c.tap(t);
    t += static_cast<uint64_t>(kUsPerBeat120);
  }
  CHECK(std::fabs(c.bpm() - 120.0f) < 1.0f);
}

static void test_tap_tempo_outlier_discarded() {
  TEST("tap tempo: one wild outlier tap is discarded, not allowed to corrupt bpm");
  BeatClock c;
  uint64_t t = 0;
  c.tap(t); t += 500000;
  c.tap(t); t += 500000;
  c.tap(t);                       // three regular taps: 0, 500ms, 1000ms -> ~120 BPM baseline
  CHECK(std::fabs(c.bpm() - 120.0f) < 2.0f);

  t += 30000;                     // wild outlier: 30ms later (glitch double-tap)
  c.tap(t);
  // The outlier must not send the estimate anywhere near what a naive
  // average would produce (30ms implies 2000 BPM) -- it stays in a sane
  // ballpark, i.e. it was filtered out, not blindly averaged in.
  CHECK(c.bpm() > 60.0f && c.bpm() < 200.0f);

  // A couple more good taps at the true tempo re-converge close to 120.
  t += static_cast<uint64_t>(kUsPerBeat120);
  c.tap(t);
  t += static_cast<uint64_t>(kUsPerBeat120);
  c.tap(t);
  CHECK(std::fabs(c.bpm() - 120.0f) < 5.0f);
}

// --- 9. Stop/start -----------------------------------------------------------

static void test_stop_freezes_start_resumes() {
  TEST("stop() freezes phase; start() resumes cleanly");
  BeatClock c;
  uint64_t t = 0;
  for (int i = 0; i < 10; ++i) {
    c.onBeat(BeatEvent{t, 120.0f, 0, false});
    t += static_cast<uint64_t>(kUsPerBeat120);
  }
  double bnAtQuery = c.beatNumber(t);  // registers "now" for stop() to freeze at
  c.stop();
  CHECK(!c.locked());

  double frozen = c.beatNumber(t);
  CHECK(std::fabs(frozen - bnAtQuery) < 1e-6);
  // Querying much later while stopped still returns the frozen value.
  CHECK(c.beatNumber(t + 5'000'000) == frozen);
  CHECK(c.beatPhase(t + 5'000'000) == c.beatPhase(t));

  // bpm() is retained across stop.
  CHECK(std::fabs(c.bpm() - 120.0f) < 1.0f);

  uint64_t restart = t + 2'000'000;
  c.start(restart);
  CHECK(c.beatPhase(restart) < 0.01f);
  CHECK(c.beatNumber(restart) == 0.0);
  // Tempo carried over from before the stop.
  CHECK(std::fabs(c.bpm() - 120.0f) < 1.0f);

  // Resumes cleanly: feeding beats at the (retained) tempo re-locks.
  uint64_t t2 = restart;
  for (int i = 0; i < 10; ++i) {
    c.onBeat(BeatEvent{t2, 120.0f, 0, false});
    t2 += static_cast<uint64_t>(kUsPerBeat120);
  }
  CHECK(c.locked());
}

// --- bar phase / downbeat (DJ-Link's gift) ----------------------------------

static void test_bar_phase_from_downbeat() {
  TEST("barPhase: downbeat events align bar phase to 0, wraps every 4 beats");
  BeatClock c;
  uint64_t t = 0;
  for (int beat = 0; beat < 16; ++beat) {
    bool downbeat = (beat % 4) == 0;
    uint8_t beatInBar = static_cast<uint8_t>((beat % 4) + 1);
    c.onBeat(BeatEvent{t, 120.0f, beatInBar, downbeat});
    if (beat >= 4) {
      float bp = c.barPhase(t);
      if (downbeat) {
        CHECK(bp < 0.05f);
      }
    }
    t += static_cast<uint64_t>(kUsPerBeat120);
  }
}

// --- default free-running clock (T4: no external gear needed) --------------

static void test_default_free_running() {
  TEST("a fresh BeatClock free-runs at a sane default BPM with zero input");
  BeatClock c;
  CHECK(c.beatPhase(0) >= 0.0f);
  float bpmDefault = c.bpm();
  // No tempo has ever been established -- bpm() reports "unknown", but
  // phase still advances sensibly (effects degrade gracefully, they never
  // see a frozen or NaN phase).
  CHECK(bpmDefault == 0.0f);
  double bn0 = c.beatNumber(0);
  double bn1 = c.beatNumber(250000);
  CHECK(bn1 > bn0);
  CHECK(std::isfinite(c.beatPhase(1'000'000)));
}

int main() {
  test_steady_120bpm();
  test_interpolation_continuous();
  test_jitter_rejection();
  test_tempo_change_converges();
  test_garbage_rejection();
  test_garbage_period_jump_rejected();
  test_dropout_freeruns_then_unlocks();
  test_monotonic_beat_number_long_sequence();
  test_tap_tempo_basic();
  test_tap_tempo_outlier_discarded();
  test_stop_freezes_start_resumes();
  test_bar_phase_from_downbeat();
  test_default_free_running();

  if (g_fail == 0) {
    printf("All beat_clock tests passed!\n");
    return 0;
  }
  printf("%d beat_clock tests FAILED\n", g_fail);
  return 1;
}
