#include "pixel_matrix.h"

#include <cmath>
#include <algorithm>

// Canvas implementation
Canvas::Canvas(uint16_t w, uint16_t h) : w_(w), h_(h) {
  px_.resize(static_cast<size_t>(w) * h, {0, 0, 0});
}

uint16_t Canvas::width() const { return w_; }
uint16_t Canvas::height() const { return h_; }

void Canvas::set(uint16_t x, uint16_t y, Rgb c) {
  if (x >= w_ || y >= h_) return;
  px_[static_cast<size_t>(y) * w_ + x] = c;
}

Rgb Canvas::get(uint16_t x, uint16_t y) const {
  if (x >= w_ || y >= h_) return {0, 0, 0};
  return px_[static_cast<size_t>(y) * w_ + x];
}

void Canvas::clear(Rgb c) {
  for (auto& p : px_) p = c;
}

// PixelMatrix implementation
PixelMatrix::PixelMatrix(const MatrixMap& map)
    : map_(map), canvas_(map.width, map.height) {
  uint8_t count = universeCount();
  universeBuffers_.resize(count);
  for (auto& buf : universeBuffers_) {
    buf.resize(512, 0);
  }
}

void PixelMatrix::setPattern(IPixelPattern* p) { pattern_ = p; }

void PixelMatrix::setMasterBrightness(float b01) {
  masterBrightness_ = b01 < 0.0f ? 0.0f : (b01 > 1.0f ? 1.0f : b01);
}

uint8_t PixelMatrix::universeCount() const {
  uint32_t totalChannels = static_cast<uint32_t>(map_.startChannel) +
                           static_cast<uint32_t>(map_.width) *
                           static_cast<uint32_t>(map_.height) * 3U;
  if (totalChannels == 0) return 0;
  return static_cast<uint8_t>((totalChannels - 1) / 512 + 1);
}

uint8_t PixelMatrix::universeIndex(uint8_t i) const {
  return map_.startUniverse + i;
}

const uint8_t* PixelMatrix::universeData(uint8_t i) const {
  if (i >= universeCount()) return nullptr;
  return universeBuffers_[i].data();
}

Canvas& PixelMatrix::canvas() { return canvas_; }

void PixelMatrix::render(float t) {
  // Render pattern if set
  if (pattern_) {
    pattern_->render(t, canvas_);
  }

  // Zero all universe buffers
  for (auto& buf : universeBuffers_) {
    for (auto& byte : buf) {
      byte = 0;
    }
  }

  // Pack pixels into universe buffers
  uint16_t W = map_.width;
  uint16_t H = map_.height;

  for (uint16_t y = 0; y < H; ++y) {
    for (uint16_t x = 0; x < W; ++x) {
      // Compute wiring index based on serpentine and vertical settings
      uint16_t idx;
      if (map_.vertical) {
        if (map_.serpentine) {
          idx = x * H + ((x % 2 == 0) ? y : (H - 1 - y));
        } else {
          idx = x * H + y;
        }
      } else {
        if (map_.serpentine) {
          idx = y * W + ((y % 2 == 0) ? x : (W - 1 - x));
        } else {
          idx = y * W + x;
        }
      }

      Rgb color = canvas_.get(x, y);

      // Apply master brightness
      float r = color.r * masterBrightness_;
      float g = color.g * masterBrightness_;
      float b = color.b * masterBrightness_;

      // Convert to bytes
      uint8_t rb = static_cast<uint8_t>(std::clamp(roundf(r * 255.0f), 0.0f, 255.0f));
      uint8_t gb = static_cast<uint8_t>(std::clamp(roundf(g * 255.0f), 0.0f, 255.0f));
      uint8_t bb = static_cast<uint8_t>(std::clamp(roundf(b * 255.0f), 0.0f, 255.0f));

      // Get component bytes in color order
      uint8_t comp[3];
      switch (map_.order) {
        case ColorOrder::RGB: comp[0] = rb; comp[1] = gb; comp[2] = bb; break;
        case ColorOrder::GRB: comp[0] = gb; comp[1] = rb; comp[2] = bb; break;
        case ColorOrder::BRG: comp[0] = bb; comp[1] = rb; comp[2] = gb; break;
        case ColorOrder::RBG: comp[0] = rb; comp[1] = bb; comp[2] = gb; break;
        case ColorOrder::GBR: comp[0] = gb; comp[1] = bb; comp[2] = rb; break;
        case ColorOrder::BGR: comp[0] = bb; comp[1] = gb; comp[2] = rb; break;
      }

      // Pack each component independently
      for (uint8_t k = 0; k < 3; ++k) {
        uint32_t globalCh = idx * 3U + k;
        uint32_t abs = static_cast<uint32_t>(map_.startUniverse) * 512U +
                       static_cast<uint32_t>(map_.startChannel) + globalCh;
        uint32_t uIdx = abs / 512U;
        uint16_t chInU = abs % 512U;

        if (uIdx < universeBuffers_.size()) {
          universeBuffers_[uIdx][chInU] = comp[k];
        }
      }
    }
  }
}
