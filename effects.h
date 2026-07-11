#pragma once

#include <cstdint>
#include <vector>

#include "oscillator.h"
#include "show.h"

// Time-driven effects. Each holds its fixture ids and config from
// construction. evaluate() only appends to the caller's vectors and
// allocates nothing itself. Ranges are normalized [0,1].

class DimmerEffect : public IEffect {
public:
  DimmerEffect(const std::vector<uint16_t>& ids, float level);
  void evaluate(float t, std::vector<CapIntent>& caps,
                std::vector<AimIntent>& aims) override;

private:
  std::vector<uint16_t> ids_;
  float level_;
};

class OscillatedDimmerEffect : public IEffect {
public:
  OscillatedDimmerEffect(const std::vector<uint16_t>& ids, OscillatedParam p);
  void evaluate(float t, std::vector<CapIntent>& caps,
                std::vector<AimIntent>& aims) override;

private:
  std::vector<uint16_t> ids_;
  OscillatedParam p_;
};

class ColorEffect : public IEffect {
public:
  ColorEffect(const std::vector<uint16_t>& ids, float r, float g, float b);
  void evaluate(float t, std::vector<CapIntent>& caps,
                std::vector<AimIntent>& aims) override;

private:
  std::vector<uint16_t> ids_;
  float r_, g_, b_;
};

class HueRotateEffect : public IEffect {
public:
  HueRotateEffect(const std::vector<uint16_t>& ids, float periodSec,
                   float sat = 1.0f, float val = 1.0f);
  void evaluate(float t, std::vector<CapIntent>& caps,
                std::vector<AimIntent>& aims) override;

private:
  std::vector<uint16_t> ids_;
  float periodSec_;
  float sat_;
  float val_;
};

class ChaseEffect : public IEffect {
public:
  ChaseEffect(const std::vector<uint16_t>& ids, float periodSec);
  void evaluate(float t, std::vector<CapIntent>& caps,
                std::vector<AimIntent>& aims) override;

private:
  std::vector<uint16_t> ids_;
  float periodSec_;
};

class StrobeEffect : public IEffect {
public:
  StrobeEffect(const std::vector<uint16_t>& ids, float hz);
  void evaluate(float t, std::vector<CapIntent>& caps,
                std::vector<AimIntent>& aims) override;

private:
  std::vector<uint16_t> ids_;
  float hz_;
};

class SweepEffect : public IEffect {
public:
  SweepEffect(uint16_t id, Vec3 dirA, Vec3 dirB, float periodSec);
  void evaluate(float t, std::vector<CapIntent>& caps,
                std::vector<AimIntent>& aims) override;

private:
  uint16_t id_;
  Vec3 dirA_;
  Vec3 dirB_;
  float periodSec_;
};
