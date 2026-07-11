#include "pixel_patterns.h"

#include <cmath>

// SolidPattern
SolidPattern::SolidPattern(Rgb color) : color_(color) {}

void SolidPattern::render(float /*t*/, Canvas& c) {
  c.clear(color_);
}

// HGradientPattern
HGradientPattern::HGradientPattern(Rgb a, Rgb b) : a_(a), b_(b) {}

void HGradientPattern::render(float /*t*/, Canvas& c) {
  uint16_t w = c.width();
  uint16_t h = c.height();

  for (uint16_t y = 0; y < h; ++y) {
    for (uint16_t x = 0; x < w; ++x) {
      float f = (w <= 1) ? 0.0f : static_cast<float>(x) / static_cast<float>(w - 1);
      Rgb color;
      color.r = a_.r + (b_.r - a_.r) * f;
      color.g = a_.g + (b_.g - a_.g) * f;
      color.b = a_.b + (b_.b - a_.b) * f;
      c.set(x, y, color);
    }
  }
}

// RainbowScrollPattern
RainbowScrollPattern::RainbowScrollPattern(float periodSec, float cyclesAcross)
    : periodSec_(periodSec), cyclesAcross_(cyclesAcross) {}

void RainbowScrollPattern::render(float t, Canvas& c) {
  uint16_t w = c.width();
  uint16_t h = c.height();

  for (uint16_t y = 0; y < h; ++y) {
    for (uint16_t x = 0; x < w; ++x) {
      uint32_t w_safe = (w > 0) ? w : 1;
      float hue = static_cast<float>(x) / static_cast<float>(w_safe) *
                  cyclesAcross_;

      if (periodSec_ > 0.0f) {
        hue += t / periodSec_;
      }

      hue -= floorf(hue);

      Rgb color = hsvToRgb(hue, 1.0f, 1.0f);
      c.set(x, y, color);
    }
  }
}

// PlasmaPattern
PlasmaPattern::PlasmaPattern(float speed, float scale)
    : speed_(speed), scale_(scale) {}

void PlasmaPattern::render(float t, Canvas& c) {
  uint16_t w = c.width();
  uint16_t h = c.height();

  for (uint16_t y = 0; y < h; ++y) {
    for (uint16_t x = 0; x < w; ++x) {
      float fx = static_cast<float>(x);
      float fy = static_cast<float>(y);

      float v = sinf(fx * scale_ + t * speed_) +
                sinf(fy * scale_ + t * speed_ * 1.3f) +
                sinf((fx + fy) * scale_ + t * speed_ * 0.7f);

      float hue = (v + 3.0f) / 6.0f;
      hue -= floorf(hue);

      Rgb color = hsvToRgb(hue, 1.0f, 1.0f);
      c.set(x, y, color);
    }
  }
}
