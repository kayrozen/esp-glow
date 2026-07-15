#include "show_control.h"

#include <algorithm>
#include <cmath>

ShowController::ShowController() {
  // Pre-allocate reasonable working set sizes
  workingCaps_.reserve(128);
  workingAims_.reserve(64);
  activeCues_.reserve(64);
  tempCaps_.reserve(128);
  tempAims_.reserve(64);
}

uint16_t ShowController::addCue(const std::vector<IEffect*>& effects,
                                float fadeInSec, float fadeOutSec, uint8_t priority,
                                float holdSec) {
  uint16_t id = static_cast<uint16_t>(cues_.size());
  cues_.push_back({effects, fadeInSec, fadeOutSec, priority, holdSec});
  return id;
}

uint16_t ShowController::addScene(const std::vector<uint16_t>& cueIds) {
  uint16_t id = static_cast<uint16_t>(scenes_.size());
  scenes_.push_back({cueIds});
  return id;
}

void ShowController::go(uint16_t cueId, float t) {
  Cue* cue = findCue(cueId);
  if (!cue) return;

  cue->active = true;
  cue->goTime = t;
  cue->released = false;
  cue->activationSeq = nextActivationSeq_++;
}

void ShowController::release(uint16_t cueId, float t) {
  Cue* cue = findCue(cueId);
  if (!cue || !cue->active || cue->released) return;

  auto [w, alive] = weightAndAlive(*cue, t);
  (void)alive;  // suppress unused warning
  cue->released = true;
  cue->releaseTime = t;
  cue->releaseWeight = w;
}

void ShowController::goScene(uint16_t sceneId, float t) {
  if (sceneId >= scenes_.size()) return;

  const Scene& scene = scenes_[sceneId];
  for (uint16_t cueId : scene.cueIds) {
    go(cueId, t);
  }
}

void ShowController::releaseScene(uint16_t sceneId, float t) {
  if (sceneId >= scenes_.size()) return;

  const Scene& scene = scenes_[sceneId];
  for (uint16_t cueId : scene.cueIds) {
    release(cueId, t);
  }
}

void ShowController::setManualLevel(uint16_t cueId, float level) {
  Cue* cue = findCue(cueId);
  if (!cue) return;

  if (level < 0.0f) level = 0.0f;
  if (level > 1.0f) level = 1.0f;

  if (level > 0.0f) {
    if (!cue->active) cue->activationSeq = nextActivationSeq_++;
    cue->active = true;
    cue->released = false;
    cue->manualOverride = true;
    cue->manualLevel = level;
  } else {
    cue->manualOverride = false;
    cue->manualLevel = 0.0f;
    cue->active = false;
  }
}

void ShowController::stopAll() {
  for (auto& cue : cues_) {
    cue.active = false;
    cue.released = false;
    cue.manualOverride = false;
  }
}

bool ShowController::isActive(uint16_t cueId) const {
  const Cue* cue = findCue(cueId);
  return cue && cue->active;
}

bool ShowController::anyActive() const {
  for (const auto& cue : cues_) {
    if (cue.active) return true;
  }
  return false;
}

size_t ShowController::activeCueIds(uint16_t* out, size_t cap) const {
  size_t n = 0;
  for (size_t i = 0; i < cues_.size() && n < cap; ++i) {
    if (cues_[i].active) out[n++] = static_cast<uint16_t>(i);
  }
  return n;
}

ShowController::Cue* ShowController::findCue(uint16_t cueId) {
  if (cueId >= cues_.size()) return nullptr;
  return &cues_[cueId];
}

const ShowController::Cue* ShowController::findCue(uint16_t cueId) const {
  if (cueId >= cues_.size()) return nullptr;
  return &cues_[cueId];
}

std::pair<float, bool> ShowController::weightAndAlive(const Cue& cue, float t) const {
  // P1.2: a manual level override pins the weight, bypassing the
  // fade-in/hold/fade-out state machine below entirely (setManualLevel's
  // own comment explains why this wins over an in-flight release()).
  if (cue.manualOverride) {
    return {cue.manualLevel, true};
  }

  if (!cue.active) {
    return {0.0f, false};
  }

  if (cue.released) {
    if (cue.fadeOutSec <= 0.0f) {
      return {0.0f, false};
    }
    float fo = (t - cue.releaseTime) / cue.fadeOutSec;
    if (fo >= 1.0f) {
      return {0.0f, false};
    }
    return {cue.releaseWeight * (1.0f - fo), true};
  }

  float e = t - cue.goTime;
  if (e < 0.0f) {
    return {0.0f, true};  // not started yet
  }

  if (cue.fadeInSec > 0.0f && e < cue.fadeInSec) {
    return {e / cue.fadeInSec, true};  // fading in
  }

  float heldEnd = (cue.fadeInSec > 0.0f ? cue.fadeInSec : 0.0f) + cue.holdSec;

  if (cue.holdSec <= 0.0f) {
    return {1.0f, true};  // hold until manual release
  }

  if (e < heldEnd) {
    return {1.0f, true};  // holding
  }

  if (cue.fadeOutSec <= 0.0f) {
    return {0.0f, false};
  }

  float fo = (e - heldEnd) / cue.fadeOutSec;
  if (fo >= 1.0f) {
    return {0.0f, false};
  }

  return {1.0f - fo, true};
}

bool ShowController::isIntensityClass(Capability cap) {
  switch (cap) {
    case Capability::Dimmer:
    case Capability::Red:
    case Capability::Green:
    case Capability::Blue:
    case Capability::White:
    case Capability::Amber:
    case Capability::Uv:
    case Capability::Cyan:
    case Capability::Magenta:
    case Capability::Yellow:
    case Capability::ShutterStrobe:
    case Capability::Fog:
    case Capability::Fan:
      return true;
    default:
      return false;
  }
}

void ShowController::evaluate(float t, std::vector<CapIntent>& outCaps,
                              std::vector<AimIntent>& outAims) {
  // Clear working sets
  workingCaps_.clear();
  workingAims_.clear();
  activeCues_.clear();

  // Build active cue list
  for (auto& cue : cues_) {
    if (cue.active) {
      activeCues_.push_back(&cue);
    }
  }

  // Sort by (priority asc, activationSeq asc)
  std::sort(activeCues_.begin(), activeCues_.end(),
            [](const Cue* a, const Cue* b) {
              if (a->priority != b->priority) {
                return a->priority < b->priority;
              }
              return a->activationSeq < b->activationSeq;
            });

  // Process each active cue
  for (Cue* cue : activeCues_) {
    auto [w, alive] = weightAndAlive(*cue, t);

    if (!alive) {
      cue->active = false;
      continue;
    }

    if (w <= 0.0f) {
      continue;
    }

    // Evaluate all effects in this cue
    tempCaps_.clear();
    tempAims_.clear();

    for (IEffect* fx : cue->effects) {
      fx->evaluate(t, tempCaps_, tempAims_);
    }

    // Merge capability intents
    for (const CapIntent& ci : tempCaps_) {
      // v2: a range selection (glow.slot) is a discrete choice, not a
      // blendable intensity -- "highest value wins" doesn't mean anything
      // when two cues pick different named ranges. Always resolve it with
      // priority (like position LTP below), even for capabilities that are
      // normally HTP-blended (e.g. ShutterStrobe).
      bool isRangeSelect = ci.rangeName != nullptr || ci.rangeIndex >= 0;

      if (!isRangeSelect && isIntensityClass(ci.cap)) {
        // HTP: highest value wins, scaled by weight
        float val = w * ci.norm01;

        // Find existing entry
        bool found = false;
        for (auto& e : workingCaps_) {
          if (e.fixtureId == ci.fixtureId && e.cap == ci.cap) {
            if (val >= e.value) {
              // This plain write is (or ties) the new max -- it wins outright,
              // including clearing any range selection a previous entry held.
              e.value = val;
              e.rangeName = nullptr;
              e.rangeIndex = -1;
            }
            found = true;
            break;
          }
        }

        if (!found) {
          workingCaps_.push_back({ci.fixtureId, ci.cap, val, cue->priority, cue->activationSeq, nullptr, -1});
        }
      } else {
        // Position/range-select LTP: highest priority wins, ties go to later activationSeq
        bool found = false;
        for (auto& e : workingCaps_) {
          if (e.fixtureId == ci.fixtureId && e.cap == ci.cap) {
            if ((cue->priority > e.ownerPrio) ||
                (cue->priority == e.ownerPrio && cue->activationSeq >= e.ownerSeq)) {
              e.value = ci.norm01;
              e.ownerPrio = cue->priority;
              e.ownerSeq = cue->activationSeq;
              e.rangeName = ci.rangeName;
              e.rangeIndex = ci.rangeIndex;
            }
            found = true;
            break;
          }
        }

        if (!found) {
          workingCaps_.push_back({ci.fixtureId, ci.cap, ci.norm01, cue->priority, cue->activationSeq,
                                  ci.rangeName, ci.rangeIndex});
        }
      }
    }

    // Merge aim intents
    for (const AimIntent& ai : tempAims_) {
      bool found = false;
      for (auto& e : workingAims_) {
        if (e.fixtureId == ai.fixtureId) {
          if ((cue->priority > e.ownerPrio) ||
              (cue->priority == e.ownerPrio && cue->activationSeq >= e.ownerSeq)) {
            e.target = ai.target;
            e.isPoint = ai.isPoint;
            e.ownerPrio = cue->priority;
            e.ownerSeq = cue->activationSeq;
          }
          found = true;
          break;
        }
      }

      if (!found) {
        workingAims_.push_back({ai.fixtureId, ai.target, ai.isPoint, cue->priority, cue->activationSeq});
      }
    }
  }

  // Emit resolved intents
  for (const auto& e : workingCaps_) {
    outCaps.push_back({e.fixtureId, e.cap, e.value, e.rangeName, e.rangeIndex});
  }

  for (const auto& e : workingAims_) {
    outAims.push_back({e.fixtureId, e.target, e.isPoint});
  }
}
