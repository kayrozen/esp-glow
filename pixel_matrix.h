#pragma once

#include "color.h"
#include <cstdint>
#include <vector>

class Canvas {
public:
  Canvas(uint16_t w, uint16_t h);
  uint16_t width() const;
  uint16_t height() const;
  void set(uint16_t x, uint16_t y, Rgb c);
  Rgb  get(uint16_t x, uint16_t y) const;
  void clear(Rgb c = {0, 0, 0});

private:
  uint16_t w_, h_;
  std::vector<Rgb> px_;
};

enum class ColorOrder : uint8_t { RGB, GRB, BRG, RBG, GBR, BGR };

struct MatrixMap {
  uint16_t width, height;
  bool     serpentine;
  bool     vertical;
  ColorOrder order;
  uint8_t  startUniverse;
  uint16_t startChannel;
};

class IPixelPattern {
public:
  virtual ~IPixelPattern() = default;
  virtual void render(float t, Canvas& c) = 0;
};

class PixelMatrix {
public:
  explicit PixelMatrix(const MatrixMap& map);

  void setPattern(IPixelPattern* p);
  void setMasterBrightness(float b01);

  void render(float t);

  uint8_t        universeCount() const;
  uint8_t        universeIndex(uint8_t i) const;
  const uint8_t* universeData(uint8_t i) const;

  Canvas& canvas();

private:
  MatrixMap map_;
  Canvas    canvas_;
  IPixelPattern* pattern_ = nullptr;
  float masterBrightness_ = 1.0f;
  std::vector<std::vector<uint8_t>> universeBuffers_;
};
