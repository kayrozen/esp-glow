#pragma once

#include "pixel_matrix.h"

class SolidPattern : public IPixelPattern {
public:
  explicit SolidPattern(Rgb color);
  void render(float t, Canvas& c) override;

private:
  Rgb color_;
};

class HGradientPattern : public IPixelPattern {
public:
  HGradientPattern(Rgb a, Rgb b);
  void render(float t, Canvas& c) override;

private:
  Rgb a_, b_;
};

class RainbowScrollPattern : public IPixelPattern {
public:
  RainbowScrollPattern(float periodSec, float cyclesAcross);
  void render(float t, Canvas& c) override;

private:
  float periodSec_;
  float cyclesAcross_;
};

class PlasmaPattern : public IPixelPattern {
public:
  PlasmaPattern(float speed, float scale);
  void render(float t, Canvas& c) override;

private:
  float speed_;
  float scale_;
};
