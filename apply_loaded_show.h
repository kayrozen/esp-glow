// apply_loaded_show.h — pure, host-tested patch routing from a LoadedShow.
//
// The firmware invents one piece of real logic at F3: iterating a LoadedShow
// (produced by the already-tested loadShow) and configuring a live Show —
// universe count, universe modes (Fixture vs Raw), sink routing, and
// patching every fixture including moving heads. That logic is extracted here
// so it stays under `make test` instead of only running on hardware.
//
// The caller owns the concrete sinks (DmxSink / ArtNetSink on device,
// MockSink in tests) and supplies them via an ISinkFactory. applyLoadedShow
// never constructs a sink itself, so it has zero device dependencies.
#pragma once

#include "show.h"
#include "show_bundle.h"
#include "wled_manager.h"
#include <cstdint>

// Factory that returns the sink to use for a given universe's transport.
// Return nullptr to leave that universe unconfigured (it will be skipped at
// flush time). This is how the device wires DmxSink/ArtNetSink and how the
// host test wires MockSinks.
class ISinkFactory {
public:
  virtual ~ISinkFactory() = default;
  // universeIdx: 0..ls.universeCount-1. transport: from LoadedShow.transport[].
  virtual IUniverseSink* sinkFor(uint8_t universeIdx, UniverseTransport t) = 0;
};

struct ApplyResult {
  uint16_t universesConfigured = 0;  // universes given a sink + mode
  uint16_t universesSkipped    = 0;  // transport unsupported (no sink)
  uint16_t fixturesPatched     = 0;
  uint16_t headsPatched        = 0;
  uint16_t matrixUniverses     = 0;  // universes marked Raw for a matrix
  uint16_t wledTargetsApplied  = 0;  // WLED targets added to `wled` (0 if wled is nullptr)
};

// Apply a LoadedShow to a live Show:
//   1. show.setUniverseCount(ls.universeCount)
//   2. For each universe, decide mode:
//        - Raw   if any matrix claims it (matrix universes hold pixel data)
//        - Fixture otherwise (patched fixtures write into these)
//   3. Configure each universe with its mode + the factory's sink.
//   4. patch / patchHead every fixture in ls.fixtures.
//   5. addTarget every ls.wledTargets entry into `wled`, if non-null.
//
// Matrices themselves are NOT constructed here (the Show does not own
// PixelMatrix objects); the caller iterates ls.matrices afterwards to build
// PixelMatrix instances and feed them via the render pre_render hook. We only
// ensure their universes are configured Raw with a sink so renderFrame
// flushes them.
//
// wled is nullptr on a device with no WLED transport configured (or in
// tests that don't care about it) -- ls.wledTargets is simply skipped, same
// "device has none of this" convention as ISinkFactory returning nullptr
// for an unsupported transport.
ApplyResult applyLoadedShow(const LoadedShow& ls, Show& show, ISinkFactory& factory,
                            WledManager* wled = nullptr);

// Helper: how many universes a matrix occupies (same formula as
// PixelMatrix::universeCount, kept here so applyLoadedShow is self-contained
// and host-testable without depending on pixel_matrix.cpp).
uint8_t matrixUniverseCount(const MatrixMap& m);
