// apply_loaded_show.cpp — the pure patch-routing logic, host-tested.
#include "apply_loaded_show.h"

uint8_t matrixUniverseCount(const MatrixMap& m) {
  uint32_t totalChannels = static_cast<uint32_t>(m.startChannel) +
                           static_cast<uint32_t>(m.width) *
                           static_cast<uint32_t>(m.height) * 3u;
  if (totalChannels == 0) return 0;
  return static_cast<uint8_t>((totalChannels - 1) / 512 + 1);
}

ApplyResult applyLoadedShow(const LoadedShow& ls, Show& show, ISinkFactory& factory) {
  ApplyResult r;

  // 1. Mark which universes are matrix (Raw) universes. A universe is Raw if
  //    any matrix claims it. Everything else defaults to Fixture.
  bool isMatrixUniverse[8] = {false};
  for (const MatrixMap& m : ls.matrices) {
    uint8_t count = matrixUniverseCount(m);
    for (uint8_t i = 0; i < count; ++i) {
      uint8_t u = m.startUniverse + i;
      if (u < 8) {
        isMatrixUniverse[u] = true;
        r.matrixUniverses++;
      }
    }
  }

  // 2. Set universe count and configure each universe.
  show.setUniverseCount(ls.universeCount);
  for (uint8_t u = 0; u < ls.universeCount; ++u) {
    UniverseTransport t = (u < 8) ? ls.transport[u] : UniverseTransport::Unused;
    IUniverseSink* sink = factory.sinkFor(u, t);
    if (!sink) {
      r.universesSkipped++;
      continue;
    }
    UniverseMode mode = isMatrixUniverse[u] ? UniverseMode::Raw : UniverseMode::Fixture;
    show.configureUniverse(u, mode, sink);
    r.universesConfigured++;
  }

  // 3. Patch every fixture. patchHead vs patch is decided by the entry's
  //    isHead flag; head geometry is already built by loadShow.
  for (const PatchEntry& e : ls.fixtures) {
    if (e.universe >= ls.universeCount) continue;  // safety: skip OOB
    if (e.isHead) {
      show.patchHead(e.profile, e.universe, e.base, e.head);
      r.headsPatched++;
    } else {
      show.patch(e.profile, e.universe, e.base);
    }
    r.fixturesPatched++;
  }

  return r;
}
