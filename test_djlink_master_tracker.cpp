// test_djlink_master_tracker.cpp — host tests for the small, bounded
// "which player is currently tempo master" table.
#include "djlink_master_tracker.h"

#include <cstdio>

static int g_fail = 0;

#define CHECK(cond) do { \
  if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } \
} while (0)

#define TEST(name) printf("Test: %s\n", name)

using glow::DjLinkMasterTracker;

static void test_no_devices_known_is_permissive() {
  TEST("no devices known yet -> accept beats from anyone");
  DjLinkMasterTracker t;
  CHECK(!t.hasKnownMaster());
  CHECK(t.shouldAccept(1));
  CHECK(t.shouldAccept(2));
  CHECK(t.shouldAccept(0x21));  // mixer's device number
}

static void test_known_master_gates_other_devices() {
  TEST("once a master is known, only its beats are accepted");
  DjLinkMasterTracker t;
  t.update(1, false);
  t.update(2, true);
  t.update(3, false);

  CHECK(t.hasKnownMaster());
  CHECK(t.shouldAccept(2));
  CHECK(!t.shouldAccept(1));
  CHECK(!t.shouldAccept(3));
  // An entirely unknown device number is also rejected once a master is known.
  CHECK(!t.shouldAccept(4));
}

static void test_master_handoff_updates_live() {
  TEST("a tempo master handoff (device 2 loses it, device 3 gains it) is reflected immediately");
  DjLinkMasterTracker t;
  t.update(2, true);
  t.update(3, false);
  CHECK(t.shouldAccept(2));
  CHECK(!t.shouldAccept(3));

  t.update(2, false);
  t.update(3, true);
  CHECK(!t.shouldAccept(2));
  CHECK(t.shouldAccept(3));
}

static void test_repeated_updates_same_device_overwrite() {
  TEST("repeated updates for the same device number overwrite, not accumulate");
  DjLinkMasterTracker t;
  t.update(1, true);
  t.update(1, true);
  t.update(1, false);
  CHECK(!t.hasKnownMaster());
  CHECK(t.shouldAccept(1));  // permissive again -- no known master
}

static void test_capacity_headroom_for_a_real_booth() {
  TEST("a realistic booth (4 CDJs + 1 mixer) all fit without eviction surprises");
  DjLinkMasterTracker t;
  t.update(1, false);
  t.update(2, false);
  t.update(3, true);   // mixer handed master to CDJ 3
  t.update(4, false);
  t.update(0x21, false);  // mixer

  CHECK(t.shouldAccept(3));
  CHECK(!t.shouldAccept(1));
  CHECK(!t.shouldAccept(2));
  CHECK(!t.shouldAccept(4));
  CHECK(!t.shouldAccept(0x21));
}

int main() {
  test_no_devices_known_is_permissive();
  test_known_master_gates_other_devices();
  test_master_handoff_updates_live();
  test_repeated_updates_same_device_overwrite();
  test_capacity_headroom_for_a_real_booth();

  if (g_fail == 0) {
    printf("\nAll tests passed.\n");
    return 0;
  }
  printf("\n%d test(s) failed.\n", g_fail);
  return 1;
}
