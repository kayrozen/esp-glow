#include "effects.h"

#include "color.h"
#include "vec_math.h"

DimmerEffect::DimmerEffect(const std::vector<uint16_t>& ids, float level)
    : ids_(ids), level_(level) {}

void DimmerEffect::evaluate(float, std::vector<CapIntent>& caps,
                             std::vector<AimIntent>&) {
  for (uint16_t id : ids_) caps.push_back({id, Capability::Dimmer, level_});
}

OscillatedDimmerEffect::OscillatedDimmerEffect(const std::vector<uint16_t>& ids,
                                                OscillatedParam p)
    : ids_(ids), p_(p) {}

void OscillatedDimmerEffect::evaluate(float t, std::vector<CapIntent>& caps,
                                       std::vector<AimIntent>&) {
  float v = p_.value(t);
  for (uint16_t id : ids_) caps.push_back({id, Capability::Dimmer, v});
}

ColorEffect::ColorEffect(const std::vector<uint16_t>& ids, float r, float g, float b)
    : ids_(ids), r_(r), g_(g), b_(b) {}

void ColorEffect::evaluate(float, std::vector<CapIntent>& caps,
                            std::vector<AimIntent>&) {
  for (uint16_t id : ids_) {
    caps.push_back({id, Capability::Red, r_});
    caps.push_back({id, Capability::Green, g_});
    caps.push_back({id, Capability::Blue, b_});
  }
}

HueRotateEffect::HueRotateEffect(const std::vector<uint16_t>& ids, float periodSec,
                                  float sat, float val)
    : ids_(ids), periodSec_(periodSec), sat_(sat), val_(val) {}

void HueRotateEffect::evaluate(float t, std::vector<CapIntent>& caps,
                                std::vector<AimIntent>&) {
  float hue = phaseFromTime(t, periodSec_, 0.0f);
  Rgb rgb = hsvToRgb(hue, sat_, val_);
  for (uint16_t id : ids_) {
    caps.push_back({id, Capability::Red, rgb.r});
    caps.push_back({id, Capability::Green, rgb.g});
    caps.push_back({id, Capability::Blue, rgb.b});
  }
}

ChaseEffect::ChaseEffect(const std::vector<uint16_t>& ids, float periodSec)
    : ids_(ids), periodSec_(periodSec) {}

void ChaseEffect::evaluate(float t, std::vector<CapIntent>& caps,
                            std::vector<AimIntent>&) {
  if (ids_.empty()) return;
  int n = static_cast<int>(ids_.size());
  float phase = phaseFromTime(t, periodSec_, 0.0f);
  int active = static_cast<int>(phase * static_cast<float>(n));
  if (active >= n) active = n - 1;
  if (active < 0) active = 0;
  for (int i = 0; i < n; ++i) {
    caps.push_back({ids_[i], Capability::Dimmer, i == active ? 1.0f : 0.0f});
  }
}

StrobeEffect::StrobeEffect(const std::vector<uint16_t>& ids, float hz)
    : ids_(ids), hz_(hz) {}

void StrobeEffect::evaluate(float t, std::vector<CapIntent>& caps,
                             std::vector<AimIntent>&) {
  float on;
  if (hz_ <= 0.0f) {
    on = 1.0f;
  } else {
    on = oscillator(Waveform::Square, phaseFromTime(t, 1.0f / hz_, 0.0f));
  }
  for (uint16_t id : ids_) caps.push_back({id, Capability::Dimmer, on});
}

SweepEffect::SweepEffect(uint16_t id, Vec3 dirA, Vec3 dirB, float periodSec)
    : id_(id), dirA_(dirA), dirB_(dirB), periodSec_(periodSec) {}

void SweepEffect::evaluate(float t, std::vector<CapIntent>&,
                            std::vector<AimIntent>& aims) {
  float k = oscillator(Waveform::Triangle, phaseFromTime(t, periodSec_, 0.0f));
  Vec3 dir = add(dirA_, scale(sub(dirB_, dirA_), k));
  aims.push_back({id_, dir, false});
}
