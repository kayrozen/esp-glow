#include "pixel_matrix.h"
#include "pixel_patterns.h"
#include "show.h"
#include "color.h"

#include <cstdio>
#include <cmath>

static int g_failCount = 0;
static constexpr float kEps = 1e-4f;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      g_failCount++; \
    } \
  } while (0)

#define CHECK_NEAR(a, b) CHECK(fabsf((a) - (b)) < kEps)

#define TEST(name) printf("Test: %s\n", name)

// Test 1: Progressive RGB mapping
static void test_progressive_rgb() {
  TEST("progressive RGB 2x2");
  MatrixMap map{
      .width = 2, .height = 2, .serpentine = false, .vertical = false,
      .order = ColorOrder::RGB, .startUniverse = 0, .startChannel = 0};
  PixelMatrix matrix(map);
  Canvas& c = matrix.canvas();

  c.set(0, 0, {1.0f, 0.0f, 0.0f});  // red
  c.set(1, 0, {0.0f, 1.0f, 0.0f});  // green
  c.set(0, 1, {0.0f, 0.0f, 1.0f});  // blue
  c.set(1, 1, {1.0f, 1.0f, 1.0f});  // white

  matrix.render(0);

  const uint8_t* data = matrix.universeData(0);
  CHECK(data != nullptr);

  // (0,0) red at channels 0,1,2
  CHECK(data[0] == 255);
  CHECK(data[1] == 0);
  CHECK(data[2] == 0);

  // (1,0) green at channels 3,4,5
  CHECK(data[3] == 0);
  CHECK(data[4] == 255);
  CHECK(data[5] == 0);

  // (0,1) blue at channels 6,7,8
  CHECK(data[6] == 0);
  CHECK(data[7] == 0);
  CHECK(data[8] == 255);

  // (1,1) white at channels 9,10,11
  CHECK(data[9] == 255);
  CHECK(data[10] == 255);
  CHECK(data[11] == 255);
}

// Test 2: Serpentine mapping
static void test_serpentine() {
  TEST("serpentine 2x2");
  MatrixMap map{
      .width = 2, .height = 2, .serpentine = true, .vertical = false,
      .order = ColorOrder::RGB, .startUniverse = 0, .startChannel = 0};
  PixelMatrix matrix(map);
  Canvas& c = matrix.canvas();

  // Set a known distinct color on (0,1)
  Rgb distinctColor{0.2f, 0.3f, 0.4f};
  c.set(0, 1, distinctColor);

  matrix.render(0);

  const uint8_t* data = matrix.universeData(0);
  CHECK(data != nullptr);

  // (0,1) has wiring idx = 1*2 + (2-1-0) = 3 -> channels 9,10,11
  uint8_t expectedR = static_cast<uint8_t>(roundf(0.2f * 255.0f));
  uint8_t expectedG = static_cast<uint8_t>(roundf(0.3f * 255.0f));
  uint8_t expectedB = static_cast<uint8_t>(roundf(0.4f * 255.0f));

  CHECK(data[9] == expectedR);
  CHECK(data[10] == expectedG);
  CHECK(data[11] == expectedB);

  // Should NOT be at 6,7,8 (which would be non-serpentine)
  CHECK(!(data[6] == expectedR && data[7] == expectedG && data[8] == expectedB));
}

// Test 3: Color order GRB
static void test_color_order_grb() {
  TEST("color order GRB");
  MatrixMap map{
      .width = 1, .height = 1, .serpentine = false, .vertical = false,
      .order = ColorOrder::GRB, .startUniverse = 0, .startChannel = 0};
  PixelMatrix matrix(map);
  Canvas& c = matrix.canvas();

  c.set(0, 0, {0.1f, 0.2f, 0.3f});

  matrix.render(0);

  const uint8_t* data = matrix.universeData(0);
  uint8_t expectedR = static_cast<uint8_t>(roundf(0.1f * 255.0f));
  uint8_t expectedG = static_cast<uint8_t>(roundf(0.2f * 255.0f));
  uint8_t expectedB = static_cast<uint8_t>(roundf(0.3f * 255.0f));

  // GRB order: [g, r, b]
  CHECK(data[0] == expectedG);
  CHECK(data[1] == expectedR);
  CHECK(data[2] == expectedB);
}

// Test 4: Universe overflow
static void test_universe_overflow() {
  TEST("universe overflow 513 channels");
  MatrixMap map{
      .width = 171, .height = 1, .serpentine = false, .vertical = false,
      .order = ColorOrder::RGB, .startUniverse = 0, .startChannel = 0};
  PixelMatrix matrix(map);

  CHECK(matrix.universeCount() == 2);
  CHECK(matrix.universeIndex(0) == 0);
  CHECK(matrix.universeIndex(1) == 1);

  Canvas& c = matrix.canvas();
  Rgb distinctColor{0.4f, 0.5f, 0.6f};
  c.set(170, 0, distinctColor);

  matrix.render(0);

  uint8_t expectedR = static_cast<uint8_t>(roundf(0.4f * 255.0f));
  uint8_t expectedG = static_cast<uint8_t>(roundf(0.5f * 255.0f));
  uint8_t expectedB = static_cast<uint8_t>(roundf(0.6f * 255.0f));

  // Pixel 170 -> base channel 510
  // Channels: universe 0 ch 510,511 and universe 1 ch 0
  const uint8_t* data0 = matrix.universeData(0);
  const uint8_t* data1 = matrix.universeData(1);

  CHECK(data0[510] == expectedR);
  CHECK(data0[511] == expectedG);
  CHECK(data1[0] == expectedB);
}

// Test 5: Master brightness
static void test_master_brightness() {
  TEST("master brightness 0.5");
  MatrixMap map{
      .width = 1, .height = 1, .serpentine = false, .vertical = false,
      .order = ColorOrder::RGB, .startUniverse = 0, .startChannel = 0};
  PixelMatrix matrix(map);
  matrix.setMasterBrightness(0.5f);

  Canvas& c = matrix.canvas();
  c.set(0, 0, {1.0f, 1.0f, 1.0f});

  matrix.render(0);

  const uint8_t* data = matrix.universeData(0);
  uint8_t expected = static_cast<uint8_t>(roundf(0.5f * 255.0f));

  CHECK(data[0] == expected);
  CHECK(data[1] == expected);
  CHECK(data[2] == expected);
}

// Test 6: SolidPattern
static void test_solid_pattern() {
  TEST("SolidPattern red");
  MatrixMap map{
      .width = 3, .height = 3, .serpentine = false, .vertical = false,
      .order = ColorOrder::RGB, .startUniverse = 0, .startChannel = 0};
  PixelMatrix matrix(map);

  SolidPattern pattern({1.0f, 0.0f, 0.0f});
  matrix.setPattern(&pattern);
  matrix.render(0);

  Canvas& c = matrix.canvas();
  for (uint16_t y = 0; y < c.height(); ++y) {
    for (uint16_t x = 0; x < c.width(); ++x) {
      Rgb color = c.get(x, y);
      CHECK_NEAR(color.r, 1.0f);
      CHECK_NEAR(color.g, 0.0f);
      CHECK_NEAR(color.b, 0.0f);
    }
  }
}

// Test 7: HGradientPattern
static void test_hgradient_pattern() {
  TEST("HGradientPattern");
  MatrixMap map{
      .width = 5, .height = 1, .serpentine = false, .vertical = false,
      .order = ColorOrder::RGB, .startUniverse = 0, .startChannel = 0};
  PixelMatrix matrix(map);

  HGradientPattern pattern({0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
  matrix.setPattern(&pattern);
  matrix.render(0);

  Canvas& c = matrix.canvas();

  // x=0 -> f=0 -> {0,0,0}
  Rgb c0 = c.get(0, 0);
  CHECK_NEAR(c0.r, 0.0f);
  CHECK_NEAR(c0.g, 0.0f);
  CHECK_NEAR(c0.b, 0.0f);

  // x=4 -> f=1 -> {1,1,1}
  Rgb c4 = c.get(4, 0);
  CHECK_NEAR(c4.r, 1.0f);
  CHECK_NEAR(c4.g, 1.0f);
  CHECK_NEAR(c4.b, 1.0f);

  // x=2 -> f=0.5 -> {0.5, 0.5, 0.5}
  Rgb c2 = c.get(2, 0);
  CHECK_NEAR(c2.r, 0.5f);
  CHECK_NEAR(c2.g, 0.5f);
  CHECK_NEAR(c2.b, 0.5f);
}

// Test 8: RainbowScrollPattern at t=0, x=0
static void test_rainbow_scroll_pattern() {
  TEST("RainbowScrollPattern at t=0, x=0");
  MatrixMap map{
      .width = 10, .height = 1, .serpentine = false, .vertical = false,
      .order = ColorOrder::RGB, .startUniverse = 0, .startChannel = 0};
  PixelMatrix matrix(map);

  RainbowScrollPattern pattern(1.0f, 1.0f);
  matrix.setPattern(&pattern);
  matrix.render(0);

  Canvas& c = matrix.canvas();
  Rgb c0 = c.get(0, 0);

  // At t=0, x=0: hue = 0 -> red = hsvToRgb(0, 1, 1)
  Rgb expected = hsvToRgb(0.0f, 1.0f, 1.0f);
  CHECK_NEAR(c0.r, expected.r);
  CHECK_NEAR(c0.g, expected.g);
  CHECK_NEAR(c0.b, expected.b);
}

// Test 9: PlasmaPattern
static void test_plasma_pattern() {
  TEST("PlasmaPattern");
  MatrixMap map{
      .width = 10, .height = 10, .serpentine = false, .vertical = false,
      .order = ColorOrder::RGB, .startUniverse = 0, .startChannel = 0};
  PixelMatrix matrix(map);

  float speed = 1.0f;
  float scale = 0.1f;
  float t = 2.0f;
  uint16_t x = 5;
  uint16_t y = 3;

  PlasmaPattern pattern(speed, scale);
  matrix.setPattern(&pattern);
  matrix.render(t);

  Canvas& c = matrix.canvas();
  Rgb actual = c.get(x, y);

  // Recompute formula
  float fx = static_cast<float>(x);
  float fy = static_cast<float>(y);
  float v = sinf(fx * scale + t * speed) +
            sinf(fy * scale + t * speed * 1.3f) +
            sinf((fx + fy) * scale + t * speed * 0.7f);
  float hue = (v + 3.0f) / 6.0f;
  hue -= floorf(hue);
  Rgb expected = hsvToRgb(hue, 1.0f, 1.0f);

  CHECK_NEAR(actual.r, expected.r);
  CHECK_NEAR(actual.g, expected.g);
  CHECK_NEAR(actual.b, expected.b);
}

// Test 10: End-to-end through Show + MockSink
static void test_end_to_end() {
  TEST("end-to-end with Show and MockSink");

  Show show;
  show.setUniverseCount(1);

  MockSink sink;
  show.configureUniverse(0, UniverseMode::Raw, &sink);

  MatrixMap map{
      .width = 2, .height = 2, .serpentine = false, .vertical = false,
      .order = ColorOrder::RGB, .startUniverse = 0, .startChannel = 0};
  PixelMatrix matrix(map);

  SolidPattern pattern({1.0f, 0.0f, 0.0f});
  matrix.setPattern(&pattern);

  matrix.render(0);
  show.writeRawUniverse(0, matrix.universeData(0), 512);
  show.renderFrame(0);

  // Check that red pixels are in sink
  CHECK(sink.last[0] == 255);   // (0,0) red
  CHECK(sink.last[1] == 0);
  CHECK(sink.last[2] == 0);
  CHECK(sink.last[3] == 255);   // (1,0) red
  CHECK(sink.last[4] == 0);
  CHECK(sink.last[5] == 0);
  CHECK(sink.last[6] == 255);   // (0,1) red
  CHECK(sink.last[7] == 0);
  CHECK(sink.last[8] == 0);
  CHECK(sink.last[9] == 255);   // (1,1) red
  CHECK(sink.last[10] == 0);
  CHECK(sink.last[11] == 0);
}

int main() {
  test_progressive_rgb();
  test_serpentine();
  test_color_order_grb();
  test_universe_overflow();
  test_master_brightness();
  test_solid_pattern();
  test_hgradient_pattern();
  test_rainbow_scroll_pattern();
  test_plasma_pattern();
  test_end_to_end();

  if (g_failCount == 0) {
    printf("All tests passed!\n");
    return 0;
  } else {
    printf("%d test(s) failed\n", g_failCount);
    return 1;
  }
}
