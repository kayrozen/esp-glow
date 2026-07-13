#include "beat_clock.h"

#include <algorithm>
#include <cmath>

namespace glow {

namespace {
constexpr double kDefaultPeriodUs = 500000.0;  // 120 BPM
double periodForBpm(double bpm) { return 60.0e6 / bpm; }
constexpr double kAbsMinPeriodUs = 60.0e6 / BeatClock::kMaxBpm;  // fastest sane tempo
constexpr double kAbsMaxPeriodUs = 60.0e6 / BeatClock::kMinBpm;  // slowest sane tempo
}  // namespace

BeatClock::BeatClock()
    : periodUs_(kDefaultPeriodUs),
      anchorTUs_(0.0),
      anchorBeat_(0.0),
      lastEventTUs_(-1.0),
      lastKnownTUs_(-1.0),
      running_(true),
      tempoKnown_(false),
      frozenBeat_(0.0),
      barAnchorBeat_(0.0),
      haveBar_(false),
      tapTimes_{0.0, 0.0, 0.0, 0.0},
      tapCount_(0) {}

double BeatClock::continuousBeatAt(double tUsD) const {
  return anchorBeat_ + (tUsD - anchorTUs_) / periodUs_;
}

void BeatClock::noteTUs(uint64_t tUs) const {
  double t = static_cast<double>(tUs);
  if (t > lastKnownTUs_) lastKnownTUs_ = t;
}

void BeatClock::onBeat(const BeatEvent& e) {
  // Garbage rejection: an explicitly reported tempo outside sane bounds.
  // Reject before touching any state -- the clock keeps its prior estimate.
  if (e.bpm != 0.0f && (e.bpm < kMinBpm || e.bpm > kMaxBpm)) return;

  noteTUs(e.tUs);
  double tUsD = static_cast<double>(e.tUs);

  if (lastEventTUs_ < 0.0) {
    // First-ever beat: nothing to correct against yet. Anchor phase here;
    // tempo becomes "known" only if this event actually reported one (a
    // lone first pulse can't imply an interval).
    anchorTUs_ = tUsD;
    anchorBeat_ = 0.0;
    lastEventTUs_ = tUsD;
    running_ = true;
    if (e.bpm > 0.0f) {
      periodUs_ = periodForBpm(e.bpm);
      tempoKnown_ = true;
    }
    if (e.isDownbeat) {
      barAnchorBeat_ = 0.0;
      haveBar_ = true;
    } else if (e.beatInBar >= 1 && e.beatInBar <= kBeatsPerBar) {
      barAnchorBeat_ = -static_cast<double>(e.beatInBar - 1);
      haveBar_ = true;
    }
    return;
  }

  // Continuous prediction from the CURRENT model at this event's time --
  // this is the value beatNumber(e.tUs) would have returned an instant
  // before this call, and is the baseline every correction below is
  // anchored to (never allowed to decrease from it).
  double predBeat = continuousBeatAt(tUsD);
  double nearestInt = std::round(predBeat);
  double phaseError = predBeat - nearestInt;  // beats, in [-0.5, 0.5]

  // What period does this event imply?
  double dt = tUsD - lastEventTUs_;
  double beatsElapsed = std::round(dt / periodUs_);
  if (beatsElapsed < 1.0) beatsElapsed = 1.0;
  double measuredPeriod = dt / beatsElapsed;
  double targetPeriod = (e.bpm > 0.0f) ? periodForBpm(e.bpm) : measuredPeriod;

  // Garbage rejection: absolute sanity, then (once we have a real
  // estimate to compare against) reject periods wildly off it -- a
  // single bad packet must not send the phase flying.
  if (targetPeriod < kAbsMinPeriodUs || targetPeriod > kAbsMaxPeriodUs) return;
  if (tempoKnown_ && (targetPeriod > periodUs_ * kMaxPeriodJumpFactor ||
                      targetPeriod < periodUs_ / kMaxPeriodJumpFactor)) {
    return;
  }

  // --- PLL correction ---
  // Frequency term: blend toward the target period, plus a proportional
  // phase-error feedback term (positive phaseError == running fast/ahead
  // of the true beat == slow down == increase period; negative == speed
  // up == decrease period). This is what "locks" phase over several
  // beats without ever stepping it.
  double phaseErrorUs = phaseError * periodUs_;
  double newPeriod = periodUs_ + kPeriodGain * (targetPeriod - periodUs_) +
                     kPhaseToPeriodGain * phaseErrorUs;
  newPeriod = std::clamp(newPeriod, kAbsMinPeriodUs, kAbsMaxPeriodUs);

  // Position term: a bounded, FORWARD-ONLY nudge. If phaseError > 0 (model
  // ahead of the true beat), the "correct" direction is backward -- which
  // would violate monotonicity, so it's clamped to zero and left entirely
  // to the frequency term above (slower over the next few beats, no snap).
  // If phaseError < 0 (model behind), nudging forward is always safe.
  double posDelta = -kPhaseGain * phaseError;
  if (posDelta < 0.0) posDelta = 0.0;

  anchorBeat_ = predBeat + posDelta;
  anchorTUs_ = tUsD;
  periodUs_ = newPeriod;
  lastEventTUs_ = tUsD;
  running_ = true;
  tempoKnown_ = true;

  if (e.isDownbeat) {
    barAnchorBeat_ = anchorBeat_;
    haveBar_ = true;
  } else if (e.beatInBar >= 1 && e.beatInBar <= kBeatsPerBar) {
    barAnchorBeat_ = anchorBeat_ - static_cast<double>(e.beatInBar - 1);
    haveBar_ = true;
  }
}

void BeatClock::onTempo(float bpm) {
  if (bpm <= 0.0f || bpm < kMinBpm || bpm > kMaxBpm) return;
  double targetPeriod = periodForBpm(bpm);
  if (tempoKnown_ && (targetPeriod > periodUs_ * kMaxPeriodJumpFactor ||
                      targetPeriod < periodUs_ / kMaxPeriodJumpFactor)) {
    return;
  }
  periodUs_ = std::clamp(periodUs_ + kPeriodGain * (targetPeriod - periodUs_),
                         kAbsMinPeriodUs, kAbsMaxPeriodUs);
  tempoKnown_ = true;
}

void BeatClock::start(uint64_t tUs) {
  noteTUs(tUs);
  double tUsD = static_cast<double>(tUs);
  anchorBeat_ = 0.0;
  anchorTUs_ = tUsD;
  barAnchorBeat_ = 0.0;
  haveBar_ = false;
  lastEventTUs_ = tUsD;
  running_ = true;
  // periodUs_/tempoKnown_ carry over -- a transport restart keeps the
  // tempo estimate, it only resets where beat 0 falls.
}

void BeatClock::stop() {
  double atTUs = (lastKnownTUs_ >= 0.0) ? lastKnownTUs_ : anchorTUs_;
  frozenBeat_ = running_ ? continuousBeatAt(atTUs) : frozenBeat_;
  running_ = false;
}

void BeatClock::tap(uint64_t tUs) {
  noteTUs(tUs);
  for (int i = 0; i + 1 < 4; ++i) tapTimes_[i] = tapTimes_[i + 1];
  tapTimes_[3] = static_cast<double>(tUs);
  if (tapCount_ < 4) ++tapCount_;

  if (tapCount_ < 2) {
    // Not enough data for a tempo yet; register a bare beat pulse so a
    // single tap still produces immediate motion (phase resets to now).
    onBeat(BeatEvent{tUs, 0.0f, 0, false});
    return;
  }

  double intervals[3];
  int n = 0;
  for (int i = 4 - tapCount_; i + 1 < 4; ++i) {
    intervals[n++] = tapTimes_[i + 1] - tapTimes_[i];
  }

  double sorted[3];
  for (int i = 0; i < n; ++i) sorted[i] = intervals[i];
  std::sort(sorted, sorted + n);
  double median = sorted[n / 2];

  double sum = 0.0;
  int kept = 0;
  for (int i = 0; i < n; ++i) {
    if (median > 0.0 && std::fabs(intervals[i] - median) / median > kTapOutlierFactor) continue;
    sum += intervals[i];
    ++kept;
  }
  if (kept == 0) {
    sum = median;
    kept = 1;
  }
  double avgInterval = sum / kept;
  if (avgInterval <= 0.0) return;

  float impliedBpm = static_cast<float>(60.0e6 / avgInterval);
  onBeat(BeatEvent{tUs, impliedBpm, 0, false});
}

double BeatClock::beatNumber(uint64_t tUs) const {
  noteTUs(tUs);
  if (!running_) return frozenBeat_;
  return continuousBeatAt(static_cast<double>(tUs));
}

float BeatClock::beatPhase(uint64_t tUs) const {
  double bn = beatNumber(tUs);
  double f = bn - std::floor(bn);
  return static_cast<float>(f);
}

float BeatClock::barPhase(uint64_t tUs) const {
  double bn = beatNumber(tUs);
  double rel = haveBar_ ? (bn - barAnchorBeat_) : bn;
  double bars = rel / static_cast<double>(kBeatsPerBar);
  double f = bars - std::floor(bars);
  return static_cast<float>(f);
}

float BeatClock::bpm() const {
  if (!tempoKnown_) return 0.0f;
  return static_cast<float>(60.0e6 / periodUs_);
}

bool BeatClock::locked() const {
  if (!running_ || !tempoKnown_ || lastEventTUs_ < 0.0) return false;
  double since = lastKnownTUs_ - lastEventTUs_;
  if (since < 0.0) since = 0.0;
  return since <= kLockTimeoutBeats * periodUs_;
}

}  // namespace glow
