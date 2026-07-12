// dump_fixed_image.c — vendoring-time-only helper used by
// scripts/vendor_littlefs_image_wasm.sh to cross-check the wasm32 build
// against a native build: builds an image from a fixed test vector and
// writes the raw disk bytes to argv[1]. Not part of any shipped artifact
// and not a test in its own right (see test_littlefs_image.c for that).
#include <stdio.h>
#include <string.h>

extern char *lfs_image_name_buf_ptr(void);
extern unsigned char *lfs_image_content_buf_ptr(void);
extern unsigned char *lfs_image_disk_ptr(void);
extern unsigned int lfs_image_disk_size(void);
extern int lfs_image_build(unsigned int nameLen, unsigned int contentLen);

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <output-path>\n", argv[0]);
    return 2;
  }
  static const char name[] = "boot.fnl";
  static const char content[] =
      "(fn breathe [t]\n"
      "  (glow.set 1 :dimmer (* 0.5 (+ 1 (math.sin (* t 2))))))\n"
      "(glow.cue.define :breathe {:effects [breathe]})\n"
      "(glow.cue.go :breathe)\n";

  memcpy(lfs_image_name_buf_ptr(), name, sizeof(name) - 1);
  memcpy(lfs_image_content_buf_ptr(), content, sizeof(content) - 1);

  int rc = lfs_image_build((unsigned int)(sizeof(name) - 1), (unsigned int)(sizeof(content) - 1));
  if (rc != 0) {
    fprintf(stderr, "lfs_image_build failed: %d\n", rc);
    return 1;
  }

  FILE *f = fopen(argv[1], "wb");
  if (!f) { fprintf(stderr, "fopen %s failed\n", argv[1]); return 1; }
  size_t n = fwrite(lfs_image_disk_ptr(), 1, lfs_image_disk_size(), f);
  fclose(f);
  if (n != lfs_image_disk_size()) { fprintf(stderr, "short write\n"); return 1; }
  return 0;
}
