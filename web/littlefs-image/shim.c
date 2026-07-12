// littlefs image builder — builds a scripts-partition LittleFS image
// in-memory, writing a single boot.fnl file into it, matching the exact
// on-disk geometry esp_littlefs (joltwallet/littlefs, pinned via
// firmware/components/glow_core/idf_component.yml) mounts on-device.
//
// Geometry mirrors esp_littlefs's compiled-in Kconfig defaults (this repo's
// sdkconfig.defaults does not override any LITTLEFS_* option) and
// firmware/partitions.csv's "scripts" partition (0x40000 bytes):
//   block_size=4096, block_count=64 (0x40000/4096), read/prog_size=128,
//   cache_size=512, lookahead_size=128, block_cycles=512. esp_littlefs never
//   sets cfg.name_max (confirmed by reading esp_littlefs.c), so it stays at
//   littlefs's compile-time LFS_NAME_MAX default (255) -- NOT
//   CONFIG_LITTLEFS_OBJ_NAME_LEN (64), which is an unrelated internal
//   scratch-buffer size in esp_littlefs.c, not the on-disk name limit.
// LFS_NO_MALLOC also means file opens must go through lfs_file_opencfg()
// with a static per-file buffer, not the plain lfs_file_open() -- see
// lfs.h's own comment where it's declared.
//
// Compiled freestanding to wasm32 (see scripts/vendor_littlefs_image_wasm.sh)
// and, for the host round-trip test, natively — same source both times, so
// the host test is a real proof the image this produces mounts and reads
// back correctly, not just that it "looks like" a filesystem.

#include "lfs.h"

#define BLOCK_SIZE 4096u
#define BLOCK_COUNT 64u
#define READ_SIZE 128u
#define PROG_SIZE 128u
#define CACHE_SIZE 512u
#define LOOKAHEAD_SIZE 128u
#define BLOCK_CYCLES 512
// esp_littlefs never sets cfg.name_max (see scripts/vendor_littlefs_image_wasm.sh
// for how this was confirmed against its source) -- it stays 0, so littlefs
// falls back to the compile-time LFS_NAME_MAX default (255). Passing 0 here
// reproduces that rather than hardcoding a guess.

#define DISK_SIZE (BLOCK_SIZE * BLOCK_COUNT)
#define NAME_BUF_CAP 64u
#define CONTENT_BUF_CAP (64u * 1024u)

static unsigned char g_disk[DISK_SIZE];
static unsigned char g_read_buf[CACHE_SIZE];
static unsigned char g_prog_buf[CACHE_SIZE];
static unsigned char g_lookahead_buf[LOOKAHEAD_SIZE];
static unsigned char g_file_buf[CACHE_SIZE];

static char g_name_buf[NAME_BUF_CAP];
static unsigned char g_content_buf[CONTENT_BUF_CAP];

static int bd_read(const struct lfs_config *c, lfs_block_t block,
                   lfs_off_t off, void *buffer, lfs_size_t size) {
  (void)c;
  unsigned char *dst = (unsigned char *)buffer;
  const unsigned char *src = g_disk + (block * BLOCK_SIZE) + off;
  for (lfs_size_t i = 0; i < size; i++) dst[i] = src[i];
  return 0;
}

static int bd_prog(const struct lfs_config *c, lfs_block_t block,
                   lfs_off_t off, const void *buffer, lfs_size_t size) {
  (void)c;
  unsigned char *dst = g_disk + (block * BLOCK_SIZE) + off;
  const unsigned char *src = (const unsigned char *)buffer;
  for (lfs_size_t i = 0; i < size; i++) dst[i] = src[i];
  return 0;
}

static int bd_erase(const struct lfs_config *c, lfs_block_t block) {
  (void)c;
  unsigned char *dst = g_disk + (block * BLOCK_SIZE);
  for (lfs_size_t i = 0; i < BLOCK_SIZE; i++) dst[i] = 0xFF;  // erased flash reads as 0xFF
  return 0;
}

static int bd_sync(const struct lfs_config *c) {
  (void)c;
  return 0;
}

static const struct lfs_config g_cfg = {
    .context = 0,
    .read = bd_read,
    .prog = bd_prog,
    .erase = bd_erase,
    .sync = bd_sync,
    .read_size = READ_SIZE,
    .prog_size = PROG_SIZE,
    .block_size = BLOCK_SIZE,
    .block_count = BLOCK_COUNT,
    .block_cycles = BLOCK_CYCLES,
    .cache_size = CACHE_SIZE,
    .lookahead_size = LOOKAHEAD_SIZE,
    .read_buffer = g_read_buf,
    .prog_buffer = g_prog_buf,
    .lookahead_buffer = g_lookahead_buf,
    .name_max = 0,  // 0 = library default (255) -- see comment above
    .file_max = 0,  // 0 = littlefs default
    .attr_max = 0,  // 0 = littlefs default
};

__attribute__((visibility("default")))
char *lfs_image_name_buf_ptr(void) { return g_name_buf; }

__attribute__((visibility("default")))
unsigned int lfs_image_name_buf_cap(void) { return NAME_BUF_CAP; }

__attribute__((visibility("default")))
unsigned char *lfs_image_content_buf_ptr(void) { return g_content_buf; }

__attribute__((visibility("default")))
unsigned int lfs_image_content_buf_cap(void) { return CONTENT_BUF_CAP; }

__attribute__((visibility("default")))
unsigned char *lfs_image_disk_ptr(void) { return g_disk; }

__attribute__((visibility("default")))
unsigned int lfs_image_disk_size(void) { return DISK_SIZE; }

// Formats a fresh image, writes g_content_buf[0:contentLen] as a file named
// g_name_buf[0:nameLen] (NOT NUL-terminated by the caller -- this function
// NUL-terminates internally), and unmounts. Returns 0 on success, or a
// negative lfs_error code (see lfs.h) on failure. g_disk holds the result
// either way; only trust it when this returns 0.
__attribute__((visibility("default")))
int lfs_image_build(unsigned int nameLen, unsigned int contentLen) {
  if (nameLen == 0 || nameLen >= NAME_BUF_CAP) return LFS_ERR_INVAL;
  if (contentLen > CONTENT_BUF_CAP) return LFS_ERR_INVAL;
  g_name_buf[nameLen] = '\0';

  static lfs_t lfs;
  int err = lfs_format(&lfs, &g_cfg);
  if (err) return err;

  err = lfs_mount(&lfs, &g_cfg);
  if (err) return err;

  lfs_file_t file;
  static struct lfs_file_config fcfg;
  fcfg.buffer = g_file_buf;
  fcfg.attrs = 0;
  fcfg.attr_count = 0;
  err = lfs_file_opencfg(&lfs, &file, g_name_buf, LFS_O_WRONLY | LFS_O_CREAT, &fcfg);
  if (err) { lfs_unmount(&lfs); return err; }

  if (contentLen > 0) {
    lfs_ssize_t written = lfs_file_write(&lfs, &file, g_content_buf, contentLen);
    if (written < 0 || (unsigned int)written != contentLen) {
      lfs_file_close(&lfs, &file);
      lfs_unmount(&lfs);
      return (written < 0) ? (int)written : LFS_ERR_IO;
    }
  }

  err = lfs_file_close(&lfs, &file);
  if (err) { lfs_unmount(&lfs); return err; }

  return lfs_unmount(&lfs);
}
