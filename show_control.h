#pragma once

#include <cstdint>
#include <vector>

#include "show.h"

class ShowController : public IEffect {
public:
  ShowController();

  // Register a cue; returns its id. effects pointers are borrowed, not owned.
  uint16_t addCue(const std::vector<IEffect*>& effects,
                  float fadeInSec, float fadeOutSec, uint8_t priority,
                  float holdSec = 0.0f);

  // Register a scene (a set of cue ids); returns its id.
  uint16_t addScene(const std::vector<uint16_t>& cueIds);

  void go(uint16_t cueId, float t);
  void release(uint16_t cueId, float t);
  void goScene(uint16_t sceneId, float t);
  void releaseScene(uint16_t sceneId, float t);

  // P1.2 (live_control.h's ActionKind::CueLevel): hold `cueId` at a manual
  // weight in [0,1], bypassing the fade-in/hold/fade-out state machine
  // entirely while held -- a continuous MIDI source (a fader, the pitch
  // wheel, channel pressure) "bumping" a cue the way go()/release() can't.
  // level clamped to [0,1]. level > 0 activates the cue (same
  // isActive()/priority-ordering participation as go(), including a fresh
  // activationSeq on the 0->active edge) and pins its weight at `level`
  // every frame, regardless of any go()/release() call in flight. level <=
  // 0 releases manual control immediately (no fade) and deactivates the
  // cue -- "fader all the way down" means "off", not "back to whatever
  // go()/release() last established". Calling release() on a cue currently
  // under manual override is a no-op in effect: weightAndAlive() answers
  // from the override, not the release fade, until setManualLevel brings
  // it back to 0 (or stopAll() clears it) -- manual control wins.
  void setManualLevel(uint16_t cueId, float level);

  // F5 safe blackout: deactivate every cue immediately, with no fade-out.
  // Unlike release() (which starts a fadeOutSec-paced ramp down), this is
  // the "the show is over right now" primitive a safety path needs -- a
  // corrupt bundle or a failing OTA image can't wait out a fade. The next
  // evaluate() call emits no intents for any of these cues, so
  // Show::renderFrame's per-frame zero+applyDefaults pass is all that's
  // left to run (see safe_blackout.h).
  void stopAll();

  bool isActive(uint16_t cueId) const;

  // True if any cue is currently active (regardless of fade phase). Used to
  // refuse OTA while a show is running (see T4: "refuse OTA while cues are
  // running") -- a reboot mid-set is exactly what this exists to prevent.
  bool anyActive() const;

  // Fills up to `cap` currently-active cue ids into `out` (cues with
  // active==true, regardless of fade/release state -- same definition as
  // isActive()). Returns the number written. Used by the HIL selftest
  // serial query (?state) to assert on live cue state without a WS client.
  size_t activeCueIds(uint16_t* out, size_t cap) const;

  // IEffect
  void evaluate(float t, std::vector<CapIntent>& caps,
                std::vector<AimIntent>& aims) override;

private:
  struct Cue {
    std::vector<IEffect*> effects;
    float fadeInSec;
    float fadeOutSec;
    uint8_t priority;
    float holdSec;

    bool active = false;
    float goTime = 0.0f;
    uint32_t activationSeq = 0;
    bool released = false;
    float releaseTime = 0.0f;
    float releaseWeight = 0.0f;

    // P1.2: manual weight override (setManualLevel) -- see its own comment.
    bool manualOverride = false;
    float manualLevel = 0.0f;
  };

  struct Scene {
    std::vector<uint16_t> cueIds;
  };

  struct WorkingCap {
    uint16_t fixtureId;
    Capability cap;
    float value;
    uint8_t ownerPrio;
    uint32_t ownerSeq;
    // v2: carried through so a glow.slot range selection survives priority
    // resolution instead of being silently flattened into a linear write
    // (see evaluate()'s merge loop). nullptr/-1 = linear, same as CapIntent.
    const char* rangeName = nullptr;
    int16_t rangeIndex = -1;
  };

  struct WorkingAim {
    uint16_t fixtureId;
    Vec3 target;
    bool isPoint;
    uint8_t ownerPrio;
    uint32_t ownerSeq;
  };

  Cue* findCue(uint16_t cueId);
  const Cue* findCue(uint16_t cueId) const;

  // Returns (weight in [0,1], alive bool).
  std::pair<float, bool> weightAndAlive(const Cue& cue, float t) const;

  static bool isIntensityClass(Capability cap);

  std::vector<Cue> cues_;
  std::vector<Scene> scenes_;
  uint32_t nextActivationSeq_ = 0;

  // Working vectors (reused per frame, cleared and reserved once)
  std::vector<WorkingCap> workingCaps_;
  std::vector<WorkingAim> workingAims_;
  std::vector<Cue*> activeCues_;  // sorted by (priority asc, activationSeq asc)
  std::vector<CapIntent> tempCaps_;
  std::vector<AimIntent> tempAims_;
};
