#pragma once

// Pure color model. No hardware, no allocation.

struct Rgb { float r, g, b; };  // each in [0,1]

// Standard HSV -> RGB. h wraps (h and h+1 are equal). s,v clamped to [0,1].
Rgb hsvToRgb(float h01, float s01, float v01);
