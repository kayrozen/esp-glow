# ShowController: Fade Envelopes and Effect Blending

The `ShowController` is the show-control layer of the DMX lighting engine. It manages cues (bundles of effects with fade envelopes), scenes (named groups of cues), and blends overlapping effects from multiple cues into a single resolved set of intents for the lighting engine.

## Cue Weight Algorithm

Each cue has four fade parameters: `fadeInSec`, `holdSec`, `fadeOutSec`, and `priority`. A cue's lifecycle:

1. **Go**: triggered via `go(cueId, t)`, marking the cue as active, recording the go-time, and assigning an activation sequence number (used for tie-breaking).
2. **Fade-In**: weight ramps from 0 to 1 over `fadeInSec` seconds.
3. **Hold**: weight stays at 1 for `holdSec` seconds (infinite hold if `holdSec <= 0`).
4. **Auto Fade-Out**: if the hold ends and `fadeOutSec > 0`, weight ramps from 1 to 0 over `fadeOutSec` seconds.
5. **Manual Release**: triggered via `release(cueId, t)`, capturing the current weight and fading out from there.

### Weight Calculation

The function `weightAndAlive(cue, t)` returns a pair `(weight in [0,1], alive: bool)`:

```
if !cue.active:
  return (0, false)

if cue.released:
  if fadeOutSec <= 0: return (0, false)
  fo = (t - releaseTime) / fadeOutSec
  if fo >= 1: return (0, false)
  return (releaseWeight * (1 - fo), true)

e = t - goTime
if e < 0: return (0, true)              // not yet started
if fadeInSec > 0 && e < fadeInSec:
  return (e / fadeInSec, true)          // fading in
heldEnd = fadeInSec + holdSec           // (fadeInSec counted as 0 if <= 0)
if holdSec <= 0: return (1, true)       // hold indefinitely
if e < heldEnd: return (1, true)        // in hold phase
if fadeOutSec <= 0: return (0, false)   // auto-out not enabled
fo = (e - heldEnd) / fadeOutSec         // progress through auto fade-out
if fo >= 1: return (0, false)
return (1 - fo, true)
```

When `weightAndAlive` returns `alive == false`, the controller sets the cue `active = false` and removes it from the active set.

## Capability Classification and Blending

All capabilities fall into two classes with different merge rules:

### Intensity-Class Capabilities
**Dimmer, Red, Green, Blue, White, Amber, UV, Cyan, Magenta, Yellow, ShutterStrobe, Fog, Fan**

- **Scaling**: Each cue's intensity value is multiplied by its weight.
- **Merge Rule**: HTP (Highest Takes Priority). The highest scaled value wins.
- **Intuition**: Multiple overlapping dimmers or color channels should blend additively (the brightest wins).

### Position-Class Capabilities
**Pan, Tilt, Gobo, Focus, Zoom, Generic**

- **Scaling**: NOT scaled by weight. Position is unscaled.
- **Merge Rule**: LTP by priority. Highest priority wins; ties go to the cue with the later activation sequence.
- **Intuition**: A moving head's position should snap to whichever cue has the highest priority, regardless of that cue's fade state. This allows coarse control (dimmer fades in/out) independent of position handoff.

### Aim Intents
**AimIntent** (fixture position and direction) is treated like position-class: LTP by priority per fixture, unscaled.

## Evaluation Algorithm

Each frame, `controller.evaluate(t, outCaps, outAims)` processes all active cues in priority order and produces a single resolved set of intents:

```
clear working sets: rcaps[] (with ownerPrio and ownerSeq), raims[]
activeCues = all active cues sorted by (priority asc, activationSeq asc)

for each cue in activeCues:
  (w, alive) = weightAndAlive(cue, t)
  if !alive: cue.active = false; continue
  if w <= 0: continue                    // skip zero-weight cues

  for each effect in cue.effects:
    effect.evaluate(t, tempCaps, tempAims)

  for each cap intent ci in tempCaps:
    if isIntensityClass(ci.cap):
      val = w * ci.norm01
      find e in rcaps with (fixtureId==ci.fixtureId && cap==ci.cap):
        if found: e.value = max(e.value, val)
        else: append with (ownerPrio=cue.priority, ownerSeq=cue.activationSeq)
    else:                                 // position-class
      find e in rcaps with (fixtureId==ci.fixtureId && cap==ci.cap):
        if found:
          if (cue.priority > e.ownerPrio) ||
             (cue.priority == e.ownerPrio && cue.activationSeq >= e.ownerSeq):
            e.value = ci.norm01; update owner info
        else: append with owner info

  for each aim intent ai in tempAims:
    find e in raims with fixtureId==ai.fixtureId:
      if found:
        if (cue.priority > e.ownerPrio) || (priority==priority && seq >= seq):
          overwrite with ai
      else: append with owner info

emit: for each e in rcaps: outCaps.push_back({fixtureId, cap, value})
      for each e in raims: outAims.push_back({fixtureId, target, isPoint})
```

**Linear search is fine at typical scene sizes** (128 fixtures, 64 cues). Working vectors are pre-allocated and reused per frame; no dynamic allocation occurs during `evaluate`.

## Example: Crossfade Between Two Color States

### Setup
- Fixture 7 (simple RGB: Dimmer, Red, Green, Blue)
- **Cue A** (priority 0): Fade-in 2s, hold 5s, fade-out 2s. Emits `{Dimmer=1.0, Red=1.0, Green=0, Blue=0}` (pure red).
- **Cue B** (priority 0): Fade-in 2s, hold 5s, fade-out 2s. Emits `{Dimmer=1.0, Red=0, Green=1.0, Blue=0}` (pure green).

### Timeline

**t=0**: `go(cueA, 0)`, `go(cueB, 1)` (B starts 1 second later)
- **Cue A**: fading in, weight = 0/2 = 0
  - No intents (weight <= 0)
- **Cue B**: not yet started, weight = (1-1)/2 = 0, alive=true but not started (e < 0)
  - No intents (not yet started)
- **Result**: Black (no intents)

**t=1**: Both fading in
- **Cue A**: weight = 1/2 = 0.5
  - Emits: Dimmer=0.5, Red=0.5, Green=0, Blue=0
  - After blend (HTP): Dimmer=0.5, Red=0.5, Green=0, Blue=0
- **Cue B**: e=1-1=0, fading in, weight = 0/2 = 0
  - No intents (weight <= 0)
- **Result**: Dark red (0.5 intensity)

**t=2**: A at full, B starting
- **Cue A**: weight = 2/2 = 1.0 (in hold phase: heldEnd=2+5=7, e=2, e < 7)
  - Emits: Dimmer=1.0, Red=1.0, Green=0, Blue=0
- **Cue B**: e=2-1=1, fading in, weight = 1/2 = 0.5
  - Emits: Dimmer=0.5, Red=0, Green=0.5, Blue=0
- **Blend** (HTP for intensity):
  - Dimmer: max(1.0, 0.5) = 1.0 ✓
  - Red: max(1.0, 0) = 1.0 ✓
  - Green: max(0, 0.5) = 0.5 ✓
  - Blue: max(0, 0) = 0
- **Result**: Bright orange (red + half green)

**t=3**: Crossfade halfway
- **Cue A**: e=3, still in hold (e < 7), weight = 1.0
  - Emits: Dimmer=1.0, Red=1.0, Green=0, Blue=0
- **Cue B**: e=2, fading in, weight = 2/2 = 1.0 (now in hold)
  - Emits: Dimmer=1.0, Red=0, Green=1.0, Blue=0
- **Blend**:
  - Dimmer: max(1.0, 1.0) = 1.0 ✓
  - Red: max(1.0, 0) = 1.0 ✓
  - Green: max(0, 1.0) = 1.0 ✓
  - Blue: 0
- **Result**: Bright yellow (red + green)

**t=7**: A starts auto fade-out, B still holding
- **Cue A**: e=7, heldEnd=7, starting auto fade-out, weight = 1.0 (but immediately at the boundary)
  - Actually e >= heldEnd and fadeOutSec=2, so fo = (7-7)/2 = 0, weight = 1 - 0 = 1.0
- **Cue B**: e=6, still in hold (e < 7), weight = 1.0
  - Emits: Dimmer=1.0, Red=0, Green=1.0, Blue=0
- **Blend**:
  - Dimmer: max(1.0, 1.0) = 1.0
  - Red: max(1.0, 0) = 1.0
  - Green: max(0, 1.0) = 1.0
  - Blue: 0
- **Result**: Yellow (no change yet)

**t=8**: A fading out
- **Cue A**: e=8, auto fade-out, fo = (8-7)/2 = 0.5, weight = 1 - 0.5 = 0.5
  - Emits: Dimmer=0.5, Red=0.5, Green=0, Blue=0
- **Cue B**: e=7, at heldEnd, about to auto fade-out, weight = 1.0 (still in hold phase)
  - Emits: Dimmer=1.0, Red=0, Green=1.0, Blue=0
- **Blend**:
  - Dimmer: max(0.5, 1.0) = 1.0 ✓
  - Red: max(0.5, 0) = 0.5
  - Green: max(0, 1.0) = 1.0 ✓
  - Blue: 0
- **Result**: Lime green (high green, low red)

**t=9**: A nearly gone, B fading out
- **Cue A**: e=9, fo = (9-7)/2 = 1.0, weight = 0
  - No intents (weight <= 0)
- **Cue B**: e=8, auto fade-out, fo = (8-7)/2 = 0.5, weight = 0.5
  - Emits: Dimmer=0.5, Red=0, Green=0.5, Blue=0
- **Blend**: Only B's intents remain
  - Dimmer: 0.5
  - Red: 0
  - Green: 0.5
  - Blue: 0
- **Result**: Dark lime

**t=10**: B gone
- **Cue A**: inactive (weight <= 0)
- **Cue B**: e=9, fo = (9-7)/2 = 1.0, weight = 0
  - No intents (weight <= 0), sets active=false
- **Result**: Black (all cues inactive)

## Scenes

A scene is a convenience: a named group of cue IDs. `goScene(sceneId, t)` calls `go` on all cue IDs in the scene; `releaseScene(sceneId, t)` calls `release` on all cues. This allows a single operation to trigger/release a coordinated "look" (e.g., a color wash + a moving head sweep).

## API

```cpp
class ShowController : public IEffect {
public:
  ShowController();

  uint16_t addCue(const std::vector<IEffect*>& effects,
                  float fadeInSec, float fadeOutSec, uint8_t priority,
                  float holdSec = 0.0f);
  uint16_t addScene(const std::vector<uint16_t>& cueIds);

  void go(uint16_t cueId, float t);
  void release(uint16_t cueId, float t);
  void goScene(uint16_t sceneId, float t);
  void releaseScene(uint16_t sceneId, float t);

  bool isActive(uint16_t cueId) const;

  void evaluate(float t, std::vector<CapIntent>& caps,
                std::vector<AimIntent>& aims) override;
};
```

The controller is itself an `IEffect`, so it can be added to a `Show` via `show.addEffect(&controller)`. The `Show` treats it like any other effect during `renderFrame`.
