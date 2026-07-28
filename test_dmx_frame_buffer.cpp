// test_dmx_frame_buffer.cpp — B4: host test for the latest-wins DMX
// mailbox (dmx_frame_buffer.h) the render task and dmx_tx task share.
#include "dmx_frame_buffer.h"

#include <cstdio>
#include <cstring>

static int g_fail = 0;

#define CHECK(cond) do { \
  if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } \
} while (0)

#define TEST(name) printf("Test: %s\n", name)

static void test_write_then_read() {
  TEST("write() then takeIfDirty() returns exactly what was written");

  DmxFrameBuffer buf;
  uint8_t frame[DMX_UNIVERSE_SIZE];
  for (uint16_t i = 0; i < DMX_UNIVERSE_SIZE; ++i) frame[i] = static_cast<uint8_t>(i & 0xFF);

  buf.write(frame, DMX_UNIVERSE_SIZE);

  uint8_t out[DMX_UNIVERSE_SIZE] = {0};
  uint16_t outLen = 0;
  CHECK(buf.takeIfDirty(out, &outLen));
  CHECK(outLen == DMX_UNIVERSE_SIZE);
  CHECK(std::memcmp(out, frame, DMX_UNIVERSE_SIZE) == 0);

  // Consumed: a second call with nothing new written finds nothing dirty.
  CHECK(!buf.takeIfDirty(out, &outLen));
}

static void test_overwrite_unconsumed_frame() {
  TEST("a second write() before takeIfDirty() overwrites the first "
       "(latest-wins, not queued)");

  DmxFrameBuffer buf;
  uint8_t first[DMX_UNIVERSE_SIZE];
  uint8_t second[DMX_UNIVERSE_SIZE];
  std::memset(first, 0x11, sizeof(first));
  std::memset(second, 0x22, sizeof(second));

  buf.write(first, DMX_UNIVERSE_SIZE);
  buf.write(second, DMX_UNIVERSE_SIZE);  // first is discarded, never consumed

  uint8_t out[DMX_UNIVERSE_SIZE] = {0};
  uint16_t outLen = 0;
  CHECK(buf.takeIfDirty(out, &outLen));
  CHECK(outLen == DMX_UNIVERSE_SIZE);
  // Only the second (latest) frame's bytes should ever be observed.
  CHECK(out[0] == 0x22);
  CHECK(out[DMX_UNIVERSE_SIZE - 1] == 0x22);

  // Exactly one frame was ever delivered -- the mailbox is not a queue of 2.
  CHECK(!buf.takeIfDirty(out, &outLen));
}

static void test_read_without_prior_write() {
  TEST("takeIfDirty() with nothing ever written returns false and leaves "
       "the output untouched");

  DmxFrameBuffer buf;
  uint8_t out[DMX_UNIVERSE_SIZE];
  std::memset(out, 0xAA, sizeof(out));  // sentinel -- must survive untouched
  uint16_t outLen = 12345;

  CHECK(!buf.takeIfDirty(out, &outLen));
  CHECK(outLen == 12345);
  for (uint16_t i = 0; i < DMX_UNIVERSE_SIZE; ++i) {
    CHECK(out[i] == 0xAA);
  }
}

static void test_short_write_len_is_preserved() {
  TEST("a write() shorter than the full universe reports its own length, "
       "not DMX_UNIVERSE_SIZE (padding is the dmx_tx task's job, not this "
       "buffer's -- see SPEC B3)");

  DmxFrameBuffer buf;
  uint8_t frame[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  buf.write(frame, 8);

  uint8_t out[DMX_UNIVERSE_SIZE];
  uint16_t outLen = 0;
  CHECK(buf.takeIfDirty(out, &outLen));
  CHECK(outLen == 8);
  CHECK(std::memcmp(out, frame, 8) == 0);
}

int main() {
  printf("=== dmx_frame_buffer Tests ===\n\n");

  test_write_then_read();
  test_overwrite_unconsumed_frame();
  test_read_without_prior_write();
  test_short_write_len_is_preserved();

  printf("\n=== Results ===\n");
  if (g_fail == 0) {
    printf("All tests passed!\n");
    return 0;
  } else {
    printf("%d test(s) failed\n", g_fail);
    return 1;
  }
}
