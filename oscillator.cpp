#include "oscillator.h"

#include <cmath>

static constexpr float kPi = 3.14159265358979f;

float oscillator(Waveform w, float phase01) {
  switch (w) {
    case Waveform::Sine:
      return 0.5f + 0.5f * sinf(2.0f * kPi * phase01);
    case Waveform::Sawtooth:
      return phase01;
    case Waveform::Triangle:
      return phase01 < 0.5f ? 2.0f * phase01 : 2.0f * (1.0f - phase01);
    case Waveform::Square:
      return phase01 < 0.5f ? 0.0f : 1.0f;
  }
  return 0.0f;
}

float phaseFromTime(float t, float periodSec, float phaseOffset01) {
  if (periodSec <= 0.0f) return 0.0f;
  float raw = t / periodSec + phaseOffset01;
  return raw - floorf(raw);
}

float phaseFromBeat(double beatNumber, float periodBeats, float phaseOffset01) {
  if (periodBeats <= 0.0f) return 0.0f;
  double raw = beatNumber / static_cast<double>(periodBeats) + static_cast<double>(phaseOffset01);
  double wrapped = raw - std::floor(raw);
  return static_cast<float>(wrapped);
}

float OscillatedParam::value(float t) const {
  float phase = phaseFromTime(t, periodSec, phaseOffset01);
  return min + (max - min) * oscillator(w, phase);
}

float OscillatedParam::valueAtBeat(double beatNumber) const {
  float phase = phaseFromBeat(beatNumber, periodBeats, phaseOffset01);
  return min + (max - min) * oscillator(w, phase);
}

float beatsToSeconds(float beats, float bpm) {
  if (bpm <= 0.0f) return 0.0f;
  return beats * 60.0f / bpm;
}
