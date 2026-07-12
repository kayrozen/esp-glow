#!/usr/bin/env bash
# vendor_littlefs_image_wasm.sh — build web/vendor/littlefs-image.wasm, the
# provisioner's in-browser "scripts" partition image builder (README_PROVISIONER.md
# B3: bake the authored boot.fnl into the flash image).
#
# Unlike the editor/wasmoon bundles, there is no npm package to vendor here:
# this wraps the REAL upstream littlefs C library (web/littlefs-image/shim.c)
# compiled freestanding straight to wasm32 with clang+wasm-ld — no
# Emscripten, no libc, no malloc. Freestanding is not a shortcut here: it's
# what makes the output byte-identical to what esp_littlefs mounts on-device
# (see the geometry note below) provable on a machine that will never touch
# real hardware.
#
# littlefs is pinned to the exact commit that firmware's
# joltwallet/littlefs (^1.14, resolved to v1.22.2 as of writing;
# firmware/components/glow_core/idf_component.yml) vendors as its `littlefs`
# git submodule — not just "some littlefs release" — because the disk image
# has to mount under exactly that library. shim.c's block_size/block_count/
# read_size/prog_size/cache_size/lookahead_size/block_cycles/name_max were
# read directly out of that pin's esp_littlefs.c (its Kconfig defaults,
# since this repo's sdkconfig.defaults doesn't override any LITTLEFS_*
# option) and firmware/partitions.csv's "scripts" partition size (0x40000),
# not guessed — see shim.c's header comment for the specifics. If
# joltwallet/littlefs's pin, this repo's LittleFS Kconfig, or the scripts
# partition size ever changes, this script (the LFS_PIN below) and
# web/littlefs-image/shim.c need to move together.
#
# Verification: scripts_storage_littlefs_image_test (wired into `make test`)
# compiles the identical shim.c + lfs.c + lfs_util.c *natively* and proves a
# round-trip (build an image, mount it fresh, read boot.fnl back). This
# script additionally re-derives that same host binary's output and diffs
# it byte-for-byte against the wasm32 build's output for the same input --
# the littlefs on-disk format has no host-specific encoding, so identical
# geometry + identical input must produce identical bytes on any target;
# a mismatch would mean the freestanding wasm32 build silently diverged
# from the host reference (e.g. a struct-layout or arithmetic difference)
# and must not be shipped.
#
# Re-run this script to reproduce web/vendor/littlefs-image.wasm from scratch.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

LFS_PIN=6cb4e86540eca0d9ba62500a298385c9d863c8be  # littlefs commit esp_littlefs v1.22.2 vendors

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

git clone -q https://github.com/littlefs-project/littlefs.git "$WORK/littlefs"
git -C "$WORK/littlefs" checkout -q "$LFS_PIN"

SRC_DIR="web/littlefs-image"
cp "$WORK/littlefs/lfs.c" "$WORK/littlefs/lfs.h" \
   "$WORK/littlefs/lfs_util.c" "$WORK/littlefs/lfs_util.h" \
   "$SRC_DIR/"

CDEFS="-DLFS_NO_MALLOC -DLFS_NO_ASSERT -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR"

# --- 1. Build the host reference binary (real libc, ASan/UBSan). ----------
gcc -std=c99 -O0 -g -fsanitize=address,undefined -Wall -Wextra $CDEFS \
  -o "$WORK/host_ref" "$SRC_DIR/shim.c" "$SRC_DIR/lfs.c" "$SRC_DIR/lfs_util.c" \
  "$SRC_DIR/dump_fixed_image.c"

# --- 2. Build the wasm32 module (freestanding, no libc, no Emscripten). ---
clang --target=wasm32 -O2 -ffreestanding -nostdlib -fno-builtin $CDEFS \
  -I "$SRC_DIR/freestanding" \
  -Wl,--no-entry \
  -Wl,--export=lfs_image_name_buf_ptr -Wl,--export=lfs_image_name_buf_cap \
  -Wl,--export=lfs_image_content_buf_ptr -Wl,--export=lfs_image_content_buf_cap \
  -Wl,--export=lfs_image_disk_ptr -Wl,--export=lfs_image_disk_size \
  -Wl,--export=lfs_image_build \
  -o "$WORK/littlefs-image.wasm" \
  "$SRC_DIR/shim.c" "$SRC_DIR/lfs.c" "$SRC_DIR/lfs_util.c" "$SRC_DIR/freestanding_impl.c"

# --- 3. Cross-check: host reference vs. wasm32, same fixed test input. ----
"$WORK/host_ref" "$WORK/disk_host.bin"
node "$SRC_DIR/run_wasm_dump.mjs" "$WORK/littlefs-image.wasm" "$WORK/disk_wasm.bin"
if ! cmp -s "$WORK/disk_host.bin" "$WORK/disk_wasm.bin"; then
  echo "FATAL: wasm32 build diverges from the native host reference build byte-for-byte." >&2
  echo "Refusing to vendor a littlefs-image.wasm that hasn't been proven correct." >&2
  exit 1
fi
echo "wasm32 output verified byte-identical to the host reference build."

# --- 4. Commit the wasm module (lfs.{c,h}/lfs_util.{c,h} were already
# refreshed in place above, before the build). --------------------------
cp "$WORK/littlefs/LICENSE.md" "$SRC_DIR/LICENSE.md"

mkdir -p web/vendor
cp "$WORK/littlefs-image.wasm" web/vendor/littlefs-image.wasm

echo "Vendored web/vendor/littlefs-image.wasm ($(wc -c < web/vendor/littlefs-image.wasm) bytes), littlefs @ $LFS_PIN."
echo "web/littlefs-image/{lfs.c,lfs.h,lfs_util.c,lfs_util.h} refreshed from the same pin."
