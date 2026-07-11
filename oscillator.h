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

// A parameter that swings between min and max on a waveform.
struct OscillatedParam {
  Waveform w = Waveform::Sine;
  float periodSec = 1.0f;
  float phaseOffset01 = 0.0f;
  float min = 0.0f;
  float max = 1.0f;

  float value(float t) const;
};

// Convert musical beats to seconds so an effect can be built beat-synced.
// bpm <= 0 -> 0.
float beatsToSeconds(float beats, float bpm);
