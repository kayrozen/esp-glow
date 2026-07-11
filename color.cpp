#include "color.h"

#include <cmath>

static float clamp01(float x) {
  if (x < 0.0f) return 0.0f;
  if (x > 1.0f) return 1.0f;
  return x;
}

Rgb hsvToRgb(float h01, float s01, float v01) {
  float h = h01 - floorf(h01);  // wrap into [0,1)
  float s = clamp01(s01);
  float v = clamp01(v01);

  float hh = h * 6.0f;
  int i = static_cast<int>(hh);
  float f = hh - static_cast<float>(i);

  float p = v * (1.0f - s);
  float q = v * (1.0f - s * f);
  float t = v * (1.0f - s * (1.0f - f));

  switch (i % 6) {
    case 0: return Rgb{v, t, p};
    case 1: return Rgb{q, v, p};
    case 2: return Rgb{p, v, t};
    case 3: return Rgb{p, q, v};
    case 4: return Rgb{t, p, v};
    default: return Rgb{v, p, q};
  }
}
