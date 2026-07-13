#pragma once

#include <cstdint>

// Time-driven oscillators. Pure computation: given a time or phase, produce
// a normalized value. No hardware, no allocation.

enum class Waveform : uint8_t { Sine, Sawtooth, Triangle, Square };

// phase01 in [0,1). Returns a value in [0,1].
float oscillator(Waveform w, float phase01);

// Map absolute time to a wrapped phase in [0,1).
// periodSec must be > 0; if <= 0, returns 0.
float phaseFromTime(float t, float periodSec, float phaseOffset01);

// Map a (fractional, monotonic) beat number to a wrapped phase in [0,1),
// with the period expressed in beats rather than seconds. Mirrors
// phaseFromTime's shape exactly, substituting beats for periodSec -- see
// beatsToSeconds below for the naive alternative this replaces for
// anything that needs to track a live, jittery, externally-driven tempo
// rather than a single beatsToSeconds() snapshot taken once at
// construction. periodBeats must be > 0; if <= 0, returns 0.
float phaseFromBeat(double beatNumber, float periodBeats, float phaseOffset01);

// A parameter that swings between min and max on a waveform.
struct OscillatedParam {
  Waveform w = Waveform::Sine;
  float periodSec = 1.0f;
  float phaseOffset01 = 0.0f;
  float min = 0.0f;
  float max = 1.0f;

  // When true, this param is driven by valueAtBeat() (period expressed in
  // beats, phase from a live glow::BeatClock::beatNumber()) instead of
  // value()/t. Informational only -- C++ has no way to enforce which
  // accessor a caller uses, so it's the caller's job to check it (see
  // effects.cpp/glow_lua_api.cpp for where that dispatch happens).
  bool  syncToBeat = false;
  float periodBeats = 1.0f;

  float value(float t) const;

  // The syncToBeat counterpart to value(t): takes the CURRENT beat number
  // (from glow::BeatClock::beatNumber(tUs), continuous and PLL-smoothed --
  // see beat_clock.h) rather than wall-clock t, so the oscillator tracks
  // a live, possibly-changing tempo instead of a fixed beatsToSeconds()
  // conversion taken once at construction.
  float valueAtBeat(double beatNumber) const;
};

// Convert musical beats to seconds so an effect can be built beat-synced.
// bpm <= 0 -> 0. NOTE: this is a fixed snapshot at a given bpm, not a live
// tracking of BeatClock -- prefer OscillatedParam::valueAtBeat for an
// effect that should follow a tempo that can change or drift. This stays
// useful for one-shot conversions (e.g. sizing a fixed-tempo animation).
float beatsToSeconds(float beats, float bpm);
