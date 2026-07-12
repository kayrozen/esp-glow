// test_littlefs_image.c — round-trip proof for web/littlefs-image/shim.c
// (the provisioner's in-browser "scripts" partition image builder; see
// scripts/vendor_littlefs_image_wasm.sh and README_PROVISIONER.md B3).
//
// Compiles the exact same shim.c + lfs.c + lfs_util.c that gets built to
// wasm32 for the browser, natively, under ASan/UBSan. Builds an image,
// then mounts that image *again* from scratch with an independent
// lfs_config/lfs_t (simulating the device mounting an image it never
// built), and reads boot.fnl back -- proving the image this tool produces
// is a real, correctly-formed LittleFS filesystem, not just plausible
// bytes. vendor_littlefs_image_wasm.sh additionally diffs a wasm32 build's
// output against this same host binary's output byte-for-byte.
#include <stdio.h>
#include <string.h>
#include "lfs.h"

extern char *lfs_image_name_buf_ptr(void);
extern unsigned int lfs_image_name_buf_cap(void);
extern unsigned char *lfs_image_content_buf_ptr(void);
extern unsigned int lfs_image_content_buf_cap(void);
extern unsigned char *lfs_image_disk_ptr(void);
extern unsigned int lfs_image_disk_size(void);
extern int lfs_image_build(unsigned int nameLen, unsigned int contentLen);

#define DISK_BLOCK_SIZE 4096u
#define DISK_BLOCK_COUNT 64u

static unsigned char g_remount_disk[DISK_BLOCK_SIZE * DISK_BLOCK_COUNT];
static unsigned char g_read_buf[512], g_prog_buf[512], g_lookahead_buf[128], g_file_buf[512];

static int bd_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size) {
  (void)c;
  memcpy(buffer, g_remount_disk + (size_t)block * DISK_BLOCK_SIZE + off, size);
  return 0;
}
static int bd_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size) {
  (void)c;
  memcpy(g_remount_disk + (size_t)block * DISK_BLOCK_SIZE + off, buffer, size);
  return 0;
}
static int bd_erase(const struct lfs_config *c, lfs_block_t block) {
  (void)c;
  memset(g_remount_disk + (size_t)block * DISK_BLOCK_SIZE, 0xFF, DISK_BLOCK_SIZE);
  return 0;
}
static int bd_sync(const struct lfs_config *c) { (void)c; return 0; }

static int failures = 0;
#define CHECK(cond, msg) do { \
  if (!(cond)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); failures++; } \
} while (0)

int main(void) {
  const char *name = "boot.fnl";
  const char *content =
      "(fn breathe [t]\n"
      "  (glow.set 1 :dimmer (* 0.5 (+ 1 (math.sin (* t 2))))))\n"
      "(glow.cue.define :breathe {:effects [breathe]})\n"
      "(glow.cue.go :breathe)\n";
  size_t nameLen = strlen(name);
  size_t contentLen = strlen(content);

  CHECK(nameLen < lfs_image_name_buf_cap(), "name fits name buffer");
  CHECK(contentLen <= lfs_image_content_buf_cap(), "content fits content buffer");
  memcpy(lfs_image_name_buf_ptr(), name, nameLen);
  memcpy(lfs_image_content_buf_ptr(), content, contentLen);

  int rc = lfs_image_build((unsigned int)nameLen, (unsigned int)contentLen);
  CHECK(rc == 0, "lfs_image_build succeeds");
  CHECK(lfs_image_disk_size() == DISK_BLOCK_SIZE * DISK_BLOCK_COUNT, "disk size matches the scripts partition");

  // Reject bad inputs without touching the disk.
  CHECK(lfs_image_build(0, contentLen) == LFS_ERR_INVAL, "empty name rejected");
  CHECK(lfs_image_build(lfs_image_name_buf_cap(), contentLen) == LFS_ERR_INVAL, "oversized name rejected");
  CHECK(lfs_image_build(nameLen, lfs_image_content_buf_cap() + 1) == LFS_ERR_INVAL, "oversized content rejected");

  // Rebuild the good image (the rejected calls above didn't touch g_disk,
  // but re-run explicitly so this test doesn't depend on that).
  rc = lfs_image_build((unsigned int)nameLen, (unsigned int)contentLen);
  CHECK(rc == 0, "rebuild succeeds");

  memcpy(g_remount_disk, lfs_image_disk_ptr(), lfs_image_disk_size());

  struct lfs_config cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.read = bd_read;
  cfg.prog = bd_prog;
  cfg.erase = bd_erase;
  cfg.sync = bd_sync;
  cfg.read_size = 128;
  cfg.prog_size = 128;
  cfg.block_size = DISK_BLOCK_SIZE;
  cfg.block_count = DISK_BLOCK_COUNT;
  cfg.block_cycles = 512;
  cfg.cache_size = 512;
  cfg.lookahead_size = 128;
  cfg.read_buffer = g_read_buf;
  cfg.prog_buffer = g_prog_buf;
  cfg.lookahead_buffer = g_lookahead_buf;

  lfs_t lfs;
  rc = lfs_mount(&lfs, &cfg);
  CHECK(rc == 0, "independent remount succeeds");

  if (rc == 0) {
    lfs_file_t file;
    struct lfs_file_config fcfg;
    memset(&fcfg, 0, sizeof(fcfg));
    fcfg.buffer = g_file_buf;
    rc = lfs_file_opencfg(&lfs, &file, "boot.fnl", LFS_O_RDONLY, &fcfg);
    CHECK(rc == 0, "boot.fnl opens on remount");

    if (rc == 0) {
      char readback[512] = {0};
      lfs_ssize_t n = lfs_file_read(&lfs, &file, readback, sizeof(readback) - 1);
      lfs_file_close(&lfs, &file);
      CHECK(n == (lfs_ssize_t)contentLen, "read-back length matches");
      CHECK(n >= 0 && memcmp(readback, content, contentLen) == 0, "read-back content matches");
    }

    // A second file (glow.save from the live REPL, post-boot) must coexist
    // with boot.fnl -- this is a real multi-file filesystem, not a
    // single-blob trick.
    lfs_file_t file2;
    struct lfs_file_config fcfg2;
    memset(&fcfg2, 0, sizeof(fcfg2));
    fcfg2.buffer = g_file_buf;
    rc = lfs_file_opencfg(&lfs, &file2, "verse.fnl", LFS_O_WRONLY | LFS_O_CREAT, &fcfg2);
    CHECK(rc == 0, "second script file can be created alongside boot.fnl");
    if (rc == 0) {
      const char *v = "(fn verse [t] t)\n";
      lfs_ssize_t w = lfs_file_write(&lfs, &file2, v, strlen(v));
      CHECK(w == (lfs_ssize_t)strlen(v), "second file writes fully");
      lfs_file_close(&lfs, &file2);
    }

    lfs_unmount(&lfs);
  }

  if (failures == 0) {
    printf("test_littlefs_image: all checks passed\n");
    return 0;
  }
  fprintf(stderr, "test_littlefs_image: %d check(s) failed\n", failures);
  return 1;
}
