# Per-Pixel Matrix Engine

## Overview

The per-pixel matrix engine renders a 2D pixel canvas over time and packs it into raw DMX universe buffers for LED matrices driven over DMX (via an Art-Net→DMX bridge).

This engine:
- Renders patterns into a 2D canvas
- Maps canvas pixels to DMX channels based on wiring configuration
- Packs color data into 512-byte DMX universe buffers
- Does not transmit data (output is only raw universe buffers)

## Caller Pattern

Each frame, call:

```cpp
matrix.render(t);
for (uint8_t u = 0; u < matrix.universeCount(); ++u)
    show.writeRawUniverse(matrix.universeIndex(u), matrix.universeData(u), 512);
show.renderFrame(t);   // flushes the Raw universes to their sinks
```

The caller is responsible for:
1. Configuring each universe with `show.configureUniverse(idx, UniverseMode::Raw, sink)` before rendering
2. Calling `matrix.render(t)` to update the canvas and pack into buffers
3. Writing each universe buffer via `show.writeRawUniverse()`
4. Calling `show.renderFrame(t)` to flush to sinks

## Wiring Index Formulas

The wiring index determines the order in which pixels map to DMX channels. Given width `W` and height `H`:

### Horizontal Wiring (vertical = false)

**Progressive** (no reversal):
```
idx = y * W + x
```

**Serpentine** (odd rows/columns reversed):
```
idx = y * W + ((y % 2 == 0) ? x : (W - 1 - x))
```

### Vertical Wiring (vertical = true)

**Progressive** (no reversal):
```
idx = x * H + y
```

**Serpentine** (odd rows/columns reversed):
```
idx = x * H + ((x % 2 == 0) ? y : (H - 1 - y))
```

## Per-Component Channel Placement

Each pixel occupies 3 consecutive DMX channels (one per color component). The channels for pixel `idx` are calculated as:

```cpp
for component k in 0..2:
    globalCh = idx * 3 + k
    abs = startUniverse * 512 + startChannel + globalCh
    universeIdx = abs / 512
    channelInUniverse = abs % 512
    buffer[universeIdx][channelInUniverse] = componentByte[k]
```

This correctly handles pixels that straddle a 512-channel universe boundary.

## Color Order Table

The `ColorOrder` enum specifies which color component is emitted first (in byte order):

| ColorOrder | Byte Order |
|-----------|-----------|
| RGB | [R, G, B] |
| GRB | [G, R, B] |
| BRG | [B, R, G] |
| RBG | [R, B, G] |
| GBR | [G, B, R] |
| BGR | [B, G, R] |

## Byte Conversion

After applying master brightness, each color component (in [0, 1]) is converted to a byte:

```cpp
byte = (uint8_t)clamp(roundf(component * 255.0f), 0, 255)
```

Master brightness is applied as a multiplier before conversion.

## MatrixMap Configuration

```cpp
struct MatrixMap {
  uint16_t width, height;        // Canvas dimensions in pixels
  bool     serpentine;           // true: reverse odd rows/cols; false: progressive
  bool     vertical;             // true: wiring runs down columns; false: along rows
  ColorOrder order;              // Byte order for RGB components
  uint8_t  startUniverse;        // Show universe index of pixel 0's first channel
  uint16_t startChannel;         // 0-based channel within startUniverse for pixel 0
};
```

## PixelMatrix API

```cpp
class PixelMatrix {
public:
  explicit PixelMatrix(const MatrixMap& map);

  void setPattern(IPixelPattern* p);     // Set the active pattern (borrowed pointer)
  void setMasterBrightness(float b01);   // Set brightness [0, 1], default 1.0

  void render(float t);                  // Render pattern to canvas, pack to buffers

  uint8_t        universeCount() const;  // Number of universes spanned
  uint8_t        universeIndex(uint8_t i) const;  // Universe index for span i
  const uint8_t* universeData(uint8_t i) const;   // 512-byte buffer for span i

  Canvas& canvas();                      // Access canvas directly (for tests)
};
```

### universeCount()

Returns the number of DMX universes needed to hold all pixel data:

```cpp
uint8_t count = floor((startChannel + width * height * 3 - 1) / 512) + 1
```

## Pattern Implementations

### SolidPattern

Sets every pixel to a constant color.

```cpp
SolidPattern({1.0f, 0.0f, 0.0f});  // red
```

### HGradientPattern

Horizontal linear interpolation from left to right. Interpolation factor `f = (W <= 1) ? 0 : x / (W - 1)`.

```cpp
HGradientPattern({0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});  // black to white
```

### RainbowScrollPattern

Rainbow with hue scroll over time. For each pixel:

```
hue = frac(x / max(W, 1) * cyclesAcross + t / periodSec)
pixel = hsvToRgb(hue, 1, 1)
```

If `periodSec <= 0`, the time term is omitted (static rainbow).

```cpp
RainbowScrollPattern(2.0f, 1.0f);  // 2-second period, 1 cycle across width
```

### PlasmaPattern

Deterministic sine-based plasma. For each (x, y):

```
v = sinf(x * scale + t * speed) + sinf(y * scale + t * speed * 1.3) + sinf((x+y) * scale + t * speed * 0.7)
hue = frac((v + 3.0) / 6.0)
pixel = hsvToRgb(hue, 1, 1)
```

The formula is deterministic and independent of animation speed control.

```cpp
PlasmaPattern(1.0f, 0.1f);  // speed=1.0, scale=0.1
```

## Example: 2×2 Red Matrix

```cpp
// Configure Show
Show show;
show.setUniverseCount(1);
MockSink sink;
show.configureUniverse(0, UniverseMode::Raw, &sink);

// Create matrix: 2×2 pixels, progressive horizontal, RGB order
MatrixMap map{
    .width = 2, .height = 2,
    .serpentine = false, .vertical = false,
    .order = ColorOrder::RGB,
    .startUniverse = 0, .startChannel = 0
};
PixelMatrix matrix(map);

// Apply red pattern
SolidPattern pattern({1.0f, 0.0f, 0.0f});
matrix.setPattern(&pattern);

// Render and send
matrix.render(0);
show.writeRawUniverse(0, matrix.universeData(0), 512);
show.renderFrame(0);

// Result: channels 0-11 contain:
// [255,0,0, 255,0,0, 255,0,0, 255,0,0]
// (4 red pixels × 3 bytes each)
```

## Implementation Notes

- No dynamic allocation per frame: canvas and universe buffers are allocated once at construction.
- `render()` zeroes buffers before packing; it does not blend or accumulate.
- If no pattern is set, `render()` packs whatever is currently in the canvas.
- Master brightness is clamped to [0, 1] and applied as a multiplier.
- Byte conversion uses `roundf()` for correct rounding to nearest integer.
- The engine is host-testable C++17, no hardware or networking dependencies.
