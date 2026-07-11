// storage_manager.h — LittleFS mount + SHW1 bundle reader.
//
// F3 device side: mount LittleFS at boot, read /show.shw1 into RAM, and hand
// the bytes to the host-tested loadShow(). This is the GLM half of F3; the
// Haiku-style patch glue (applyLoadedShow) is host-tested and lives in
// apply_loaded_show.{h,cpp}.
//
// On a missing or corrupt bundle we return false; the caller (main.cpp) falls
// back to a safe blackout (F5 makes that robust; F3 just logs + LED error).
#pragma once

#include "show_bundle.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mount the LittleFS partition (label "littlefs", matching partitions.csv).
// Returns true on success. Safe to call once.
bool storage_mount(void);

// Read /show.shw1 (or the given path) into the provided buffer.
//   out_buf     : caller-provided buffer (must be large enough; use
//                 storage_bundle_size() first)
//   buf_cap     : capacity of out_buf
//   out_len     : receives the number of bytes read
// Returns false if the file is missing or larger than buf_cap.
bool storage_read_bundle(const char* path, uint8_t* out_buf, size_t buf_cap, size_t* out_len);

// Convenience: read /show.shw1 and parse it in one call. On success, fills
// `out_show` and returns true. On any failure (mount, read, parse), returns
// false and logs. `buf`/`buf_cap` is scratch space for the raw file bytes
// (4 KB is enough for typical patches; 64 KB is safe for large ones).
bool storage_load_show(const char* path, LoadedShow* out_show,
                       uint8_t* buf, size_t buf_cap);

// Returns the bundle file size in bytes, or 0 if missing/unmounted.
size_t storage_bundle_size(const char* path);

#ifdef __cplusplus
}
#endif
