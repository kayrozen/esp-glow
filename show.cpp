#include "show.h"

void MockSink::send(uint8_t idx, const uint8_t* data, uint16_t len) {
  sendCount++;
  lastIndex = idx;
  uint16_t n = len < DMX_UNIVERSE_SIZE ? len : DMX_UNIVERSE_SIZE;
  for (uint16_t i = 0; i < n; ++i) last[i] = data[i];
}

StaticColorEffect::StaticColorEffect(uint16_t fixtureId, float r, float g, float b)
    : fixtureId_(fixtureId), r_(r), g_(g), b_(b) {}

void StaticColorEffect::evaluate(float /*t*/, std::vector<CapIntent>& caps,
                                 std::vector<AimIntent>& /*aims*/) {
  caps.push_back({fixtureId_, Capability::Red, r_});
  caps.push_back({fixtureId_, Capability::Green, g_});
  caps.push_back({fixtureId_, Capability::Blue, b_});
}

AimPointEffect::AimPointEffect(uint16_t fixtureId, Vec3 point)
    : fixtureId_(fixtureId), point_(point) {}

void AimPointEffect::evaluate(float /*t*/, std::vector<CapIntent>& /*caps*/,
                              std::vector<AimIntent>& aims) {
  aims.push_back({fixtureId_, point_, true});
}

Show::Show() {
  caps_.reserve(256);
  aims_.reserve(256);
  fixtures_.reserve(MAX_FIXTURES);
}

void Show::setUniverseCount(uint8_t n) {
  universeCount_ = n > MAX_UNIVERSES ? MAX_UNIVERSES : n;
}

uint8_t Show::universeCount() const { return universeCount_; }

void Show::configureUniverse(uint8_t idx, UniverseMode mode, IUniverseSink* sink) {
  if (idx >= MAX_UNIVERSES) return;
  universes_[idx].mode = mode;
  universes_[idx].sink = sink;
  universes_[idx].configured = true;
}

uint16_t Show::patchCommon(const FixtureProfile& p, uint8_t universe, uint16_t base,
                           bool isHead, const MovingHeadConfig& cfg) {
  if (fixtures_.size() >= MAX_FIXTURES) return 0xFFFF;
  PatchedFixture f;
  f.id = nextFixtureId_++;
  f.profile = p;
  f.universe = universe;
  f.base = base;
  f.isHead = isHead;
  f.head = cfg;
  fixtures_.push_back(f);
  return f.id;
}

uint16_t Show::patch(const FixtureProfile& p, uint8_t universe, uint16_t base) {
  MovingHeadConfig empty{};
  return patchCommon(p, universe, base, false, empty);
}

uint16_t Show::patchHead(const FixtureProfile& p, uint8_t universe, uint16_t base,
                         const MovingHeadConfig& cfg) {
  return patchCommon(p, universe, base, true, cfg);
}

PatchedFixture* Show::findFixture(uint16_t id) {
  for (auto& f : fixtures_) {
    if (f.id == id) return &f;
  }
  return nullptr;
}

const PatchedFixture* Show::fixture(uint16_t id) const {
  for (const auto& f : fixtures_) {
    if (f.id == id) return &f;
  }
  return nullptr;
}

void Show::addEffect(IEffect* fx) { effects_.push_back(fx); }

void Show::removeAllEffects() { effects_.clear(); }

void Show::writeRawUniverse(uint8_t idx, const uint8_t* data, uint16_t len) {
  if (idx >= MAX_UNIVERSES) return;
  Universe& u = universes_[idx];
  if (u.mode != UniverseMode::Raw) return;
  uint16_t n = len < DMX_UNIVERSE_SIZE ? len : DMX_UNIVERSE_SIZE;
  for (uint16_t i = 0; i < n; ++i) u.data[i] = data[i];
}

const uint8_t* Show::universeData(uint8_t idx) const {
  if (idx >= MAX_UNIVERSES) return nullptr;
  return universes_[idx].data;
}

void Show::renderFrame(float t) {
  // 1. Reset Fixture-mode universes.
  for (uint8_t u = 0; u < universeCount_; ++u) {
    if (universes_[u].mode == UniverseMode::Fixture) {
      for (uint16_t i = 0; i < DMX_UNIVERSE_SIZE; ++i) universes_[u].data[i] = 0;
    }
  }

  // 2. Apply defaults for every patched fixture.
  for (const auto& f : fixtures_) {
    if (f.universe >= universeCount_) continue;
    if (universes_[f.universe].mode != UniverseMode::Fixture) continue;
    applyDefaults(f.profile, universes_[f.universe].data, f.base);
  }

  // 3. Gather intents.
  caps_.clear();
  aims_.clear();
  for (IEffect* fx : effects_) {
    fx->evaluate(t, caps_, aims_);
  }

  // 4. Resolve aim intents first, then cap intents.
  for (const auto& ai : aims_) {
    PatchedFixture* f = findFixture(ai.fixtureId);
    if (!f || !f->isHead) continue;
    if (f->universe >= universeCount_) continue;
    if (universes_[f->universe].mode != UniverseMode::Fixture) continue;
    AimResult r = ai.isPoint ? aimAtPoint(f->head, ai.target)
                             : aimDirection(f->head, ai.target);
    uint8_t* buf = universes_[f->universe].data;
    applyCapability(f->profile, Capability::Pan, r.panNorm, buf, f->base);
    applyCapability(f->profile, Capability::Tilt, r.tiltNorm, buf, f->base);
  }

  for (const auto& ci : caps_) {
    PatchedFixture* f = findFixture(ci.fixtureId);
    if (!f) continue;
    if (f->universe >= universeCount_) continue;
    if (universes_[f->universe].mode != UniverseMode::Fixture) continue;
    applyCapability(f->profile, ci.cap, ci.norm01, universes_[f->universe].data, f->base);
  }

  // 5. Flush.
  for (uint8_t u = 0; u < universeCount_; ++u) {
    Universe& uni = universes_[u];
    if (!uni.configured || !uni.sink) continue;
    uni.sink->send(u, uni.data, DMX_UNIVERSE_SIZE);
  }
}
