// test_wled.cpp — WLED UDP Notifier packet layout, effect/palette maps, and
// WledManager tests (README_WLED.md). Mirrors test_mdef.cpp's shape: a new
// binary wire format (the 24-byte packet) plus a small runtime manager,
// both host-tested via a mock transport (MockWledSink, like show.h's
// MockSink).

#include "wled_packet.h"
#include "wled_effect_map.h"
#include "wled_manager.h"

#include <cstdio>
#include <set>

static int g_failCount = 0;

#define CHECK(cond)                                           \
  do {                                                        \
    if (!(cond)) {                                            \
      printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      g_failCount++;                                          \
    }                                                          \
  } while (0)

#define TEST(name) printf("Test: %s\n", name)

// ============================================================================
// Packet layout
// ============================================================================

void test_build_packet_effect_changed() {
  TEST("buildWledPacket: effect-changed packet matches the wire layout");

  WledPacketParams p;
  p.effect = 50;
  p.speed = 180;
  p.intensity = 220;
  p.brightness = 200;
  p.palette = 12;
  p.transitionMs = 0x0102;
  p.callMode = WledCallMode::EffectChanged;

  uint8_t packet[WLED_PACKET_SIZE];
  buildWledPacket(p, packet);

  CHECK(packet[0] == 0x00);   // notifier protocol
  CHECK(packet[1] == 0x06);  // callMode: effect changed
  CHECK(packet[2] == 200);   // brightness
  CHECK(packet[3] == 0);     // primary R unset for an effect packet
  CHECK(packet[4] == 0);
  CHECK(packet[5] == 0);
  CHECK(packet[6] == 0);     // nightlightActive
  CHECK(packet[7] == 0);     // nightlightDelayMins
  CHECK(packet[8] == 50);    // effectCurrent
  CHECK(packet[9] == 180);   // effectSpeed
  CHECK(packet[10] == 0);    // white
  CHECK(packet[11] == 0x05); // protocol version 5
  CHECK(packet[12] == 0 && packet[13] == 0 && packet[14] == 0);  // colSec
  CHECK(packet[15] == 0);    // whiteSec
  CHECK(packet[16] == 220);  // effectIntensity
  CHECK(packet[17] == 0x02); // transitionDelay LSB
  CHECK(packet[18] == 0x01); // transitionDelay MSB
  CHECK(packet[19] == 12);   // effectPalette
  for (int i = 20; i < 24; i++) CHECK(packet[i] == 0);  // reserved
}

void test_build_packet_direct_change() {
  TEST("buildWledPacket: direct-change (color) packet");

  WledPacketParams p;
  p.effect = 0;
  p.brightness = 255;
  p.r = 255;
  p.g = 10;
  p.b = 20;
  p.callMode = WledCallMode::DirectChange;

  uint8_t packet[WLED_PACKET_SIZE];
  buildWledPacket(p, packet);

  CHECK(packet[1] == 0x01);  // callMode: direct change
  CHECK(packet[3] == 255 && packet[4] == 10 && packet[5] == 20);
  CHECK(packet[8] == 0);     // solid
}

void test_build_packet_zeroes_stale_bytes() {
  TEST("buildWledPacket: a second call fully overwrites the buffer (no stale bytes)");

  uint8_t packet[WLED_PACKET_SIZE];
  for (auto& b : packet) b = 0xAA;  // poison

  // Defaults that are non-zero: callMode (EffectChanged = 0x06), brightness
  // (255), and the always-0x05 protocol version byte.
  WledPacketParams p;
  buildWledPacket(p, packet);

  for (int i = 0; i < 24; i++) {
    if (i == 1 || i == 2 || i == 11) continue;
    CHECK(packet[i] == 0);
  }
  CHECK(packet[1] == 0x06);
  CHECK(packet[2] == 255);
  CHECK(packet[11] == 0x05);
}

// ============================================================================
// Effect / palette maps
// ============================================================================

void test_effect_map_size_and_unique_ids() {
  TEST("EFFECT_MAP: every id 0..186 appears exactly once");
  CHECK(wled::EFFECT_MAP.size() == wled::EFFECT_COUNT);
  std::set<uint8_t> ids;
  for (const auto& [name, id] : wled::EFFECT_MAP) {
    CHECK(ids.insert(id).second);
  }
  CHECK(ids.size() == wled::EFFECT_COUNT);
}

void test_palette_map_size_and_unique_ids() {
  TEST("PALETTE_MAP: every id 0..70 appears exactly once");
  CHECK(wled::PALETTE_MAP.size() == wled::PALETTE_COUNT);
  std::set<uint8_t> ids;
  for (const auto& [name, id] : wled::PALETTE_MAP) {
    CHECK(ids.insert(id).second);
  }
  CHECK(ids.size() == wled::PALETTE_COUNT);
}

void test_effect_id_from_name() {
  TEST("effectIdFromName: known and unknown names");
  uint8_t id = 0xFF;
  CHECK(wled::effectIdFromName("solid", id) && id == 0);
  CHECK(wled::effectIdFromName("fire-2012", id) && id == 66);
  CHECK(wled::effectIdFromName("akemi", id) && id == 186);
  CHECK(!wled::effectIdFromName("not-a-real-effect", id));
}

void test_palette_id_from_name() {
  TEST("paletteIdFromName: known and unknown names");
  uint8_t id = 0xFF;
  CHECK(wled::paletteIdFromName("default", id) && id == 0);
  CHECK(wled::paletteIdFromName("fairy-reef", id) && id == 59);
  CHECK(!wled::paletteIdFromName("not-a-real-palette", id));
}

// ============================================================================
// WledManager
// ============================================================================

void test_manager_add_and_lookup_target() {
  TEST("WledManager: addTarget + target() lookup");
  MockWledSink sink;
  WledManager mgr(&sink);
  CHECK(mgr.target("main_matrix") == nullptr);

  mgr.addTarget({"main_matrix", "192.168.1.100", 21324, 1});
  CHECK(mgr.targetCount() == 1);
  const WledTarget* t = mgr.target("main_matrix");
  CHECK(t != nullptr);
  CHECK(t->ip == "192.168.1.100");
  CHECK(t->syncGroup == 1);
}

void test_manager_add_target_replaces_by_name() {
  TEST("WledManager: addTarget with an existing name replaces it in place");
  MockWledSink sink;
  WledManager mgr(&sink);
  mgr.addTarget({"tree", "192.168.1.101", 21324, 1});
  mgr.addTarget({"tree", "192.168.1.102", 21324, 2});
  CHECK(mgr.targetCount() == 1);
  CHECK(mgr.target("tree")->ip == "192.168.1.102");
  CHECK(mgr.target("tree")->syncGroup == 2);
}

void test_manager_set_effect_sends_to_target_ip() {
  TEST("WledManager::setEffect sends one packet to the target's ip:port");
  MockWledSink sink;
  WledManager mgr(&sink);
  mgr.addTarget({"tree", "192.168.1.101", 21324, 1});

  mgr.setEffect("tree", "fire-2012", 180, 220, 200, "fire", 500);

  CHECK(sink.sendCount == 1);
  CHECK(sink.lastIp == "192.168.1.101");
  CHECK(sink.lastPort == 21324);
  CHECK(sink.last[8] == 66);   // fire-2012
  CHECK(sink.last[19] == 35);  // fire palette
  CHECK(sink.last[1] == 0x06); // effect-changed
}

void test_manager_set_effect_unknown_target_is_noop() {
  TEST("WledManager::setEffect on an unknown target sends nothing");
  MockWledSink sink;
  WledManager mgr(&sink);
  mgr.setEffect("nope", "solid");
  CHECK(sink.sendCount == 0);
}

void test_manager_set_effect_unknown_name_falls_back_to_solid_default() {
  TEST("WledManager::setEffect falls back to solid(0)/default(0) for unknown names");
  MockWledSink sink;
  WledManager mgr(&sink);
  mgr.addTarget({"tree", "192.168.1.101", 21324, 1});
  mgr.setEffect("tree", "not-a-real-effect", 128, 128, 255, "not-a-real-palette");
  CHECK(sink.sendCount == 1);
  CHECK(sink.last[8] == 0);   // solid
  CHECK(sink.last[19] == 0);  // default
}

void test_manager_set_solid_color_is_direct_change() {
  TEST("WledManager::setSolidColor forces effect 0 and callMode 0x01");
  MockWledSink sink;
  WledManager mgr(&sink);
  mgr.addTarget({"tree", "192.168.1.101", 21324, 1});
  mgr.setSolidColor("tree", 255, 0, 0, 128, 500);

  CHECK(sink.sendCount == 1);
  CHECK(sink.last[1] == 0x01);  // direct change
  CHECK(sink.last[2] == 128);   // brightness
  CHECK(sink.last[3] == 255 && sink.last[4] == 0 && sink.last[5] == 0);
  CHECK(sink.last[8] == 0);     // solid
}

void test_manager_set_power() {
  TEST("WledManager::setPower toggles brightness only");
  MockWledSink sink;
  WledManager mgr(&sink);
  mgr.addTarget({"tree", "192.168.1.101", 21324, 1});

  mgr.setPower("tree", true);
  CHECK(sink.sendCount == 1);
  CHECK(sink.last[2] == 255);
  CHECK(sink.last[1] == 0x01);

  mgr.setPower("tree", false);
  CHECK(sink.sendCount == 2);
  CHECK(sink.last[2] == 0);
}

void test_manager_broadcast_effect() {
  TEST("WledManager::broadcastEffect sends to 255.255.255.255:21324");
  MockWledSink sink;
  WledManager mgr(&sink);
  mgr.broadcastEffect("pacifica", 100, 150, 180, "ocean");

  CHECK(sink.sendCount == 1);
  CHECK(sink.lastIp == "255.255.255.255");
  CHECK(sink.lastPort == 21324);
  CHECK(sink.last[8] == 101);  // pacifica
  CHECK(sink.last[19] == 9);   // ocean
}

void test_manager_null_sink_does_not_crash() {
  TEST("WledManager with a null sink resolves targets but sends nothing");
  WledManager mgr(nullptr);
  mgr.addTarget({"tree", "192.168.1.101", 21324, 1});
  mgr.setEffect("tree", "solid");  // must not crash
  CHECK(mgr.target("tree") != nullptr);
}

int main() {
  test_build_packet_effect_changed();
  test_build_packet_direct_change();
  test_build_packet_zeroes_stale_bytes();
  test_effect_map_size_and_unique_ids();
  test_palette_map_size_and_unique_ids();
  test_effect_id_from_name();
  test_palette_id_from_name();
  test_manager_add_and_lookup_target();
  test_manager_add_target_replaces_by_name();
  test_manager_set_effect_sends_to_target_ip();
  test_manager_set_effect_unknown_target_is_noop();
  test_manager_set_effect_unknown_name_falls_back_to_solid_default();
  test_manager_set_solid_color_is_direct_change();
  test_manager_set_power();
  test_manager_broadcast_effect();
  test_manager_null_sink_does_not_crash();

  if (g_failCount == 0) {
    printf("\nAll tests passed!\n");
    return 0;
  } else {
    printf("\n%d test(s) failed.\n", g_failCount);
    return 1;
  }
}
