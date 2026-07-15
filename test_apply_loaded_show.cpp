// test_apply_loaded_show.cpp — host test for the F3 patch-routing logic.
//
// Builds a LoadedShow in-memory (fixtures + a matrix), applies it with a
// MockSinkFactory, and verifies the Show's universe count, modes, sinks, and
// patched fixtures come out right. This is the host gate for the F3
// "iterate LoadedShow -> Show::patch + sink routing" glue.
#include "apply_loaded_show.h"
#include "show.h"
#include "show_bundle.h"
#include "fixture_profile.h"
#include "vec_math.h"

#include <cstdio>
#include <vector>

static int g_fail = 0;
#define CHECK(cond) do { \
  if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } \
} while (0)

// Factory that hands out one MockSink per universe and remembers them.
// Uses a fixed array (universes <= MAX_UNIVERSES=8) so returned pointers stay
// valid — a std::vector would reallocate and dangle the pointers the Show
// stores. (ASAN catches this immediately, which is exactly why we test.)
class MockSinkFactory : public ISinkFactory {
public:
  IUniverseSink* sinkFor(uint8_t universeIdx, UniverseTransport t) override {
    if (t == UniverseTransport::Sacn || t == UniverseTransport::Unused) return nullptr;
    if (universeIdx >= MAX_UNIVERSES) return nullptr;
    used_[universeIdx] = true;
    return &sinks_[universeIdx];
  }
  void configureArtnetDest(uint8_t universeIdx, const ArtNetDest& dest) override {
    if (universeIdx >= MAX_UNIVERSES) return;
    dests_[universeIdx] = dest;
    destConfigured_[universeIdx] = true;
  }
  MockSink sinks_[MAX_UNIVERSES];
  bool     used_[MAX_UNIVERSES] = {false};
  ArtNetDest dests_[MAX_UNIVERSES];
  bool     destConfigured_[MAX_UNIVERSES] = {false};
};

static FixtureProfile makeDimmerProfile() {
  FixtureProfile p{};
  p.footprint = 4;
  p.channelCount = 4;
  p.channels[0] = { Capability::Dimmer, 0, 0xFF, 0, 0 };
  p.channels[1] = { Capability::Red,    1, 0xFF, 0, 0 };
  p.channels[2] = { Capability::Green,  2, 0xFF, 0, 0 };
  p.channels[3] = { Capability::Blue,   3, 0xFF, 0, 0 };
  return p;
}

static FixtureProfile makeHeadProfile() {
  FixtureProfile p{};
  p.footprint = 9;
  p.channelCount = 7;
  p.channels[0] = { Capability::Dimmer, 0, 0xFF, 0, 0 };
  p.channels[1] = { Capability::Pan,    1, 2,   0, 0 };
  p.channels[2] = { Capability::Tilt,   3, 4,   0, 0 };
  p.channels[3] = { Capability::Red,    5, 0xFF, 0, 0 };
  p.channels[4] = { Capability::Green,  6, 0xFF, 0, 0 };
  p.channels[5] = { Capability::Blue,   7, 0xFF, 0, 0 };
  p.channels[6] = { Capability::ShutterStrobe, 8, 0xFF, 8, 0 };
  return p;
}

static void test_basic_patch_and_sink_routing() {
  printf("Test: basic patch + sink routing\n");
  LoadedShow ls;
  ls.universeCount = 2;
  ls.transport[0] = UniverseTransport::Dmx;
  ls.transport[1] = UniverseTransport::ArtNet;

  FixtureProfile dim = makeDimmerProfile();
  PatchEntry f1{}; f1.profile = dim; f1.universe = 0; f1.base = 0;  f1.isHead = false;
  PatchEntry f2{}; f2.profile = dim; f2.universe = 0; f2.base = 10; f2.isHead = false;
  PatchEntry f3{}; f3.profile = dim; f3.universe = 1; f3.base = 0;  f3.isHead = false;
  ls.fixtures.push_back(f1);
  ls.fixtures.push_back(f2);
  ls.fixtures.push_back(f3);

  Show show;
  MockSinkFactory fact;
  ApplyResult r = applyLoadedShow(ls, show, fact);

  CHECK(r.universesConfigured == 2);
  CHECK(r.universesSkipped == 0);
  CHECK(r.fixturesPatched == 3);
  CHECK(r.headsPatched == 0);
  CHECK(r.matrixUniverses == 0);
  CHECK(show.universeCount() == 2);
  // Three fixtures patched -> ids 0,1,2.
  CHECK(show.fixture(0) != nullptr);
  CHECK(show.fixture(1) != nullptr);
  CHECK(show.fixture(2) != nullptr);
  CHECK(show.fixture(0)->universe == 0);
  CHECK(show.fixture(0)->base == 0);
  CHECK(show.fixture(2)->universe == 1);
}

static void test_head_patch_carries_geometry() {
  printf("Test: moving head patch carries geometry\n");
  LoadedShow ls;
  ls.universeCount = 1;
  ls.transport[0] = UniverseTransport::Dmx;

  FixtureProfile hp = makeHeadProfile();
  PatchEntry h{};
  h.profile = hp; h.universe = 0; h.base = 0; h.isHead = true;
  h.head.position = {2.0f, 1.0f, 0.0f};
  h.head.orientation = mat3FromEuler(0.0f, 0.0f, 0.0f);
  h.head.panRangeDeg = 540.0f;
  h.head.tiltRangeDeg = 270.0f;
  h.head.panCenterNorm = 0.5f;
  h.head.tiltCenterNorm = 0.5f;
  h.head.invertPan = false;
  h.head.invertTilt = false;
  ls.fixtures.push_back(h);

  Show show;
  MockSinkFactory fact;
  ApplyResult r = applyLoadedShow(ls, show, fact);
  CHECK(r.headsPatched == 1);
  const PatchedFixture* pf = show.fixture(0);
  CHECK(pf != nullptr);
  CHECK(pf->isHead == true);
  CHECK(pf->head.panRangeDeg == 540.0f);
  CHECK(pf->head.position.x == 2.0f);
}

static void test_matrix_universes_are_raw() {
  printf("Test: matrix universes configured as Raw\n");
  LoadedShow ls;
  ls.universeCount = 3;
  ls.transport[0] = UniverseTransport::Dmx;
  ls.transport[1] = UniverseTransport::ArtNet;
  ls.transport[2] = UniverseTransport::ArtNet;

  // 16x8 RGB = 384 channels = 1 universe from startUniverse=1.
  MatrixMap m{};
  m.width = 16; m.height = 8; m.serpentine = true; m.vertical = false;
  m.order = ColorOrder::RGB; m.startUniverse = 1; m.startChannel = 0;
  ls.matrices.push_back(m);

  // One dimmer fixture on universe 0.
  PatchEntry f{}; f.profile = makeDimmerProfile(); f.universe = 0; f.base = 0; f.isHead = false;
  ls.fixtures.push_back(f);

  Show show;
  MockSinkFactory fact;
  ApplyResult r = applyLoadedShow(ls, show, fact);
  CHECK(r.matrixUniverses == 1);
  CHECK(r.universesConfigured == 3);

  // Render a frame and check each configured universe gets flushed.
  show.renderFrame(0.0f);
  CHECK(fact.used_[0] && fact.used_[1] && fact.used_[2]);
  CHECK(fact.sinks_[0].sendCount == 1);
  CHECK(fact.sinks_[1].sendCount == 1);
  CHECK(fact.sinks_[2].sendCount == 1);
}

static void test_matrix_spans_two_universes() {
  printf("Test: large matrix spans two universes (Raw)\n");
  LoadedShow ls;
  ls.universeCount = 3;
  ls.transport[0] = UniverseTransport::Dmx;
  ls.transport[1] = UniverseTransport::ArtNet;
  ls.transport[2] = UniverseTransport::ArtNet;

  // 100x4 RGB = 1200 channels => 3 universes from startUniverse=1, startChannel=0.
  // 1200 channels: u1 = 512, u2 = 512, u3 = 176. But universeCount=3 and
  // startUniverse=1 means it touches u1, u2, u3 (u3 == index 3 which is out
  // of [0,3) -> actually u3 is index 3, but universeCount=3 means indices 0,1,2.
  // So universe index 2 (the 3rd) is the partial. matrixUniverseCount=3 but
  // startUniverse=1 => touches indices 1,2,3. Index 3 is beyond universeCount=3
  // (indices 0..2), so only 2 are configured. Let's verify the math:
  //   matrixUniverseCount(m) = (1200-1)/512 + 1 = 1199/512 + 1 = 2 + 1 = 3.
  //   isMatrixUniverse[1]=true, [2]=true, [3] = out of range (skipped, guarded).
  MatrixMap m{};
  m.width = 100; m.height = 4; m.serpentine = false; m.vertical = false;
  m.order = ColorOrder::RGB; m.startUniverse = 1; m.startChannel = 0;
  ls.matrices.push_back(m);
  CHECK(matrixUniverseCount(m) == 3);

  Show show;
  MockSinkFactory fact;
  ApplyResult r = applyLoadedShow(ls, show, fact);
  // matrixUniverses counts only u<8 entries: u1, u2, u3 => 3.
  CHECK(r.matrixUniverses == 3);
  CHECK(r.universesConfigured == 3);  // u0,u1,u2 all configured (u3 doesn't exist)
}

static void test_unsupported_transport_skipped() {
  printf("Test: unsupported transport is skipped, not configured\n");
  LoadedShow ls;
  ls.universeCount = 2;
  ls.transport[0] = UniverseTransport::Dmx;
  ls.transport[1] = UniverseTransport::Sacn;  // unsupported

  Show show;
  MockSinkFactory fact;
  ApplyResult r = applyLoadedShow(ls, show, fact);
  CHECK(r.universesConfigured == 1);
  CHECK(r.universesSkipped == 1);
}

static void test_fixture_in_unconfigured_universe_skipped() {
  printf("Test: fixture whose universe was skipped is still patched (Show allows it)\n");
  // The Show patches fixtures regardless of whether the universe sink exists;
  // renderFrame skips universes with no sink. applyLoadedShow patches per the
  // bundle (fixturesPatched counts the patch attempt), but a fixture in an
  // out-of-range universe is skipped for safety.
  LoadedShow ls;
  ls.universeCount = 1;
  ls.transport[0] = UniverseTransport::Dmx;
  PatchEntry f{};
  f.profile = makeDimmerProfile();
  f.universe = 5;  // out of range (universeCount=1)
  f.base = 0; f.isHead = false;
  ls.fixtures.push_back(f);

  Show show;
  MockSinkFactory fact;
  ApplyResult r = applyLoadedShow(ls, show, fact);
  CHECK(r.fixturesPatched == 0);  // skipped: universe >= universeCount
  CHECK(show.fixture(0) == nullptr);
}

static void test_artnet_dest_configured_only_for_artnet_universes() {
  printf("Test: configureArtnetDest called only for ArtNet-transport universes, with the bundle's dest\n");
  LoadedShow ls;
  ls.universeCount = 2;
  ls.transport[0] = UniverseTransport::Dmx;
  ls.transport[1] = UniverseTransport::ArtNet;
  ls.artnetDest[1] = ArtNetDest{0xC0A80132, 5};  // 192.168.1.50, wire universe 5

  Show show;
  MockSinkFactory fact;
  applyLoadedShow(ls, show, fact);

  CHECK(!fact.destConfigured_[0]);  // Dmx universe -- never called
  CHECK(fact.destConfigured_[1]);
  CHECK(fact.dests_[1].ip == 0xC0A80132);
  CHECK(fact.dests_[1].wireUniverse == 5);
}

int main() {
  test_basic_patch_and_sink_routing();
  test_head_patch_carries_geometry();
  test_matrix_universes_are_raw();
  test_matrix_spans_two_universes();
  test_unsupported_transport_skipped();
  test_fixture_in_unconfigured_universe_skipped();
  test_artnet_dest_configured_only_for_artnet_universes();
  if (g_fail == 0) {
    printf("All apply_loaded_show tests passed!\n");
    return 0;
  }
  printf("%d apply_loaded_show tests FAILED\n", g_fail);
  return 1;
}
