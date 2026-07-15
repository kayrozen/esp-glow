#pragma once

#include <cstdint>
#include <vector>

#include "aim.h"
#include "fixture_profile.h"
#include "vec_math.h"

static constexpr uint16_t DMX_UNIVERSE_SIZE = 512;
static constexpr uint8_t  MAX_UNIVERSES     = 8;
static constexpr uint16_t MAX_FIXTURES      = 128;

enum class UniverseMode : uint8_t {
  Fixture,  // zeroed each frame, then patched fixtures resolved into it
  Raw       // left untouched by renderFrame; filled via writeRawUniverse (pixel path)
};

struct PatchedFixture {
  uint16_t id;
  FixtureProfile profile;
  uint8_t  universe;      // index into the universe array
  uint16_t base;          // 0-based start channel within that universe
  bool     isHead;
  MovingHeadConfig head;   // valid only if isHead
};

class IUniverseSink {
public:
  virtual ~IUniverseSink() = default;
  // Send `len` bytes (always DMX_UNIVERSE_SIZE) for the given universe index.
  virtual void send(uint8_t universeIndex, const uint8_t* data, uint16_t len) = 0;
};

// An Art-Net destination: which node (IP) and which wire universe a
// universe's packets go out as. The wire universe is Art-Net's 15-bit
// Port-Address (Net:SubNet:Universe, 0..32767) collapsed into one number --
// see FORMAT.md's "Art-Net Wire Universe & Destination Routing" section for
// the byte-level Net/SubNet/Universe decomposition used when a packet is
// actually built.
//
// ip == 0 is a sentinel meaning "no explicit .show route": the sink
// resolves it to its own configured fallback (CFG1's artnetFallbackIp), or
// broadcast if that is also 0. wireUniverse has no equivalent sentinel --
// a destination with ip == 0 still carries a real wireUniverse (defaulted
// to the internal universe index if the .show didn't say otherwise; see
// provision.cpp's compileShow).
struct ArtNetDest {
  uint32_t ip = 0;
  uint16_t wireUniverse = 0;
};

class MockSink : public IUniverseSink {
public:
  void send(uint8_t idx, const uint8_t* data, uint16_t len) override;
  int sendCount = 0;
  uint8_t last[DMX_UNIVERSE_SIZE] = {0};
  uint8_t lastIndex = 0xFF;
};

// rangeName/rangeIndex select a v2 function range instead of applying norm01
// linearly (see fixture_profile.h's applyRangeByName/applyRangeByIndex).
// rangeName == nullptr && rangeIndex < 0 (the default) means "linear",
// i.e. exactly the pre-v2 behavior via applyCapability. rangeName must
// outlive the frame it's gathered in -- glow.slot only ever stores an
// interned Lua string literal here, never a constructed one (same
// zero-allocation rule as glow.set's capability name).
struct CapIntent {
  uint16_t fixtureId;
  Capability cap;
  float norm01;
  const char* rangeName = nullptr;
  int16_t rangeIndex = -1;
};
struct AimIntent { uint16_t fixtureId; Vec3 target; bool isPoint; };  // isPoint=false => target is a direction

class IEffect {
public:
  virtual ~IEffect() = default;
  // Append this effect's intents for time t (seconds). Do not clear the vectors.
  virtual void evaluate(float t, std::vector<CapIntent>& caps,
                        std::vector<AimIntent>& aims) = 0;
};

class StaticColorEffect : public IEffect {
public:
  StaticColorEffect(uint16_t fixtureId, float r, float g, float b);
  void evaluate(float t, std::vector<CapIntent>& caps,
                std::vector<AimIntent>& aims) override;

private:
  uint16_t fixtureId_;
  float r_, g_, b_;
};

class AimPointEffect : public IEffect {
public:
  AimPointEffect(uint16_t fixtureId, Vec3 point);
  void evaluate(float t, std::vector<CapIntent>& caps,
                std::vector<AimIntent>& aims) override;

private:
  uint16_t fixtureId_;
  Vec3 point_;
};

class Show {
public:
  Show();

  void    setUniverseCount(uint8_t n);              // <= MAX_UNIVERSES
  uint8_t universeCount() const;

  void configureUniverse(uint8_t idx, UniverseMode mode, IUniverseSink* sink);

  uint16_t patch(const FixtureProfile& p, uint8_t universe, uint16_t base);
  uint16_t patchHead(const FixtureProfile& p, uint8_t universe, uint16_t base,
                     const MovingHeadConfig& cfg);
  const PatchedFixture* fixture(uint16_t id) const;   // nullptr if unknown

  void addEffect(IEffect* fx);       // Show does not own the pointer
  void removeAllEffects();

  // Pixel-path seam: blit a raw buffer straight into a Raw universe. Must be
  // called before renderFrame for the frame it should appear in. No-op / ignored
  // for Fixture-mode universes.
  void writeRawUniverse(uint8_t idx, const uint8_t* data, uint16_t len);

  // One render pass (see algorithm). Deterministic given t.
  void renderFrame(float t);

  // Read-only access to a universe's current 512-byte DMX buffer (the
  // state as of the last renderFrame/writeRawUniverse). nullptr if idx is
  // out of range. Mirrors PixelMatrix::universeData's naming (pixel_matrix.h)
  // -- "give me the raw buffer for universe i". Used by the HIL selftest
  // serial query (?dmx0) to assert on DMX output without a loopback.
  const uint8_t* universeData(uint8_t idx) const;

private:
  struct Universe {
    UniverseMode mode = UniverseMode::Fixture;
    IUniverseSink* sink = nullptr;
    bool configured = false;
    uint8_t data[DMX_UNIVERSE_SIZE] = {0};
  };

  PatchedFixture* findFixture(uint16_t id);
  uint16_t patchCommon(const FixtureProfile& p, uint8_t universe, uint16_t base,
                       bool isHead, const MovingHeadConfig& cfg);

  uint8_t universeCount_ = 0;
  Universe universes_[MAX_UNIVERSES];

  std::vector<PatchedFixture> fixtures_;
  uint16_t nextFixtureId_ = 0;

  std::vector<IEffect*> effects_;

  std::vector<CapIntent> caps_;
  std::vector<AimIntent> aims_;
};
