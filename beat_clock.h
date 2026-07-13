// beat_clock.h — musical time: a second, discontinuous, externally-driven
// clock alongside render_pacing's wall-clock `t`.
//
// Clock sources (MIDI clock, DJ-Link, tap, internal) are transports: they
// parse device bytes/packets and push timestamped BeatEvents onto a queue
// (see beat_queue.h), exactly like every other input transport in this
// project (control_queue.h's rationale — producers on their own tasks,
// one consumer, the render task). BeatClock is that consumer: it owns no
// hardware, does no I/O, and is only ever touched from the render task,
// same invariant as ShowController/LiveControl/the Lua VM.
//
// THE HARD PART
//   Beats arrive as jittery discrete events (WiFi UDP, MIDI UART); effects
//   need a smooth phase every frame. BeatClock is a first-order PLL:
//     - It maintains a continuous model (anchorTUs_, anchorBeat_,
//       periodUs_) and interpolates between beats:
//         beatNumber(t) = anchorBeat_ + (t - anchorTUs_) / periodUs_
//     - Each incoming beat nudges periodUs_ (frequency) toward the
//       source's reported/measured tempo, plus a proportional term driven
//       by the phase error (predicted vs. this beat's true zero-phase
//       instant) — a classic PLL: correct frequency in response to phase
//       error, don't step phase. See beat_clock.cpp's onBeat for why:
//       stepping phase backward, even slightly, would violate
//       beatNumber's monotonicity guarantee (below).
//     - A bounded *forward-only* phase nudge is also applied (never
//       negative) so a source running slow gets pulled into alignment
//       faster than frequency correction alone would manage, without
//       ever risking a backward step.
//   Garbage (an implied BPM outside [kMinBpm,kMaxBpm], or a period wildly
//   different from the current estimate) is rejected outright: the event
//   is ignored and the clock keeps its prior estimate.
//
// MONOTONICITY
//   beatNumber() never decreases, even across corrections, dropouts, or
//   tempo changes. Effects trigger once-per-bar events off it; a backward
//   step would double-fire. This is the load-bearing invariant of the
//   whole class and is what the PLL design above is built to preserve.
//
// DROPOUTS
//   No special-cased "free-run" state is needed: the continuous model
//   keeps extrapolating forever, event or no event (the show does not
//   stop because a packet was lost). `locked()` goes false once too long
//   has passed since the last real beat (or on stop()/never-started), so
//   effects can choose to fall back to something else, but beatPhase/
//   barPhase/beatNumber keep producing sane output regardless.
//
// A default-constructed BeatClock is already free-running at 120 BPM from
// t=0 — T4's "internal clock with no external gear" is this class's
// default state, not a separate code path.
#pragma once

#include <cstdint>

namespace glow {

// One timestamped beat/pulse, pushed by a transport (MIDI clock, DJ-Link,
// tap, ...) after it has parsed/derived it. Fixed-size POD so it fits a
// FreeRTOS queue item (see beat_queue.h), same discipline as ControlEvent
// and EvalSubmission.
struct BeatEvent {
  uint64_t tUs;        // monotonic microseconds when the beat/pulse arrived
  float    bpm;        // source's reported tempo, or 0 if unknown (derive it)
  uint8_t  beatInBar;  // 1..4, or 0 if unknown
  bool     isDownbeat;
};

class BeatClock {
public:
  BeatClock();

  // A source's timestamped beat (a phase=0 instant). bpm 0 means "derive
  // it from the interval since the last beat". Garbage (bpm outside
  // [kMinBpm,kMaxBpm], or an implied period wildly off the current
  // estimate) is rejected: the whole event is ignored, state unchanged.
  void onBeat(const BeatEvent& e);

  // A source that reports tempo without a beat instant (e.g. a DJ-Link
  // pitch update between beat packets). Nudges the period estimate only;
  // never touches phase. Garbage bpm is rejected the same way as onBeat.
  void onTempo(float bpm);

  // Transport start: phase resets to 0 at tUs (matches MIDI Start/DJ-Link
  // "beat 1" semantics). The tempo estimate carries over unchanged.
  void start(uint64_t tUs);

  // Transport stop: freeze phase at its current value (computed from the
  // live model at the most recent tUs this clock has observed, from
  // either a query or an event — stop() itself takes no timestamp).
  // bpm() keeps reporting the last known tempo; locked() goes false.
  void stop();

  // Tap tempo: averages the last up to 4 taps' intervals (discarding
  // outliers relative to the median) into an implied BPM, then feeds it
  // through onBeat exactly like any other source. The very first tap (no
  // interval yet) just anchors phase.
  void tap(uint64_t tUs);

  // Queried every frame by effects. All are safe to call on a freshly
  // constructed, never-fed clock (it free-runs at the default BPM).
  float  beatPhase(uint64_t tUs) const;   // [0,1) within the current beat — CONTINUOUS
  float  barPhase(uint64_t tUs) const;    // [0,1) within the current bar (assumes 4/4)
  double beatNumber(uint64_t tUs) const;  // monotonically non-decreasing, fractional
  float  bpm() const;                     // 0 if no tempo has ever been established
  bool   locked() const;                  // false: never started / stopped / dropped out

  // Sane tempo bounds; also used for garbage rejection.
  static constexpr float kMinBpm = 40.0f;
  static constexpr float kMaxBpm = 220.0f;

private:
  static constexpr int kBeatsPerBar = 4;

  // PLL gains (fraction of the observed error corrected per beat event).
  static constexpr double kPeriodGain = 0.15;          // period -> target tempo blend
  static constexpr double kPhaseToPeriodGain = 0.10;   // phase-error -> frequency feedback
  static constexpr double kPhaseGain = 0.15;            // forward-only phase nudge

  // A beat arriving more than this many periods late than expected is
  // rejected as a wild period estimate, not gradually corrected.
  static constexpr double kMaxPeriodJumpFactor = 2.0;

  // locked() goes false once this many beat-periods have passed with no
  // real beat event.
  static constexpr double kLockTimeoutBeats = 8.0;

  static constexpr double kTapOutlierFactor = 0.3;  // fraction-of-median deviation

  double continuousBeatAt(double tUsD) const;
  void   noteTUs(uint64_t tUs) const;

  double periodUs_;       // current beat-period estimate (us), always > 0
  double anchorTUs_;      // model anchor time (us, as double)
  double anchorBeat_;     // beatNumber value at anchorTUs_
  double lastEventTUs_;   // time of the last accepted real beat event; -1 = none yet
  mutable double lastKnownTUs_;  // latest tUs seen from ANY tUs-bearing call

  bool   running_;        // false after stop(); true otherwise (default free-running)
  bool   tempoKnown_;     // true once periodUs_ reflects real info, not just the default
  double frozenBeat_;     // beatNumber captured at stop()

  double barAnchorBeat_;  // beatNumber value where bar-phase == 0
  bool   haveBar_;

  double tapTimes_[4];
  int    tapCount_;
};

}  // namespace glow
