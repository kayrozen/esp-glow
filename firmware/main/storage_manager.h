// storage_manager.h — raw-partition SHW1 bundle reader.
//
// F3 device side: read the "show" raw data partition (see partitions.csv)
// into RAM and hand the bytes to the host-tested loadShow(). This is the
// GLM half of F3; the Haiku-style patch glue (applyLoadedShow) is
// host-tested and lives in apply_loaded_show.{h,cpp}.
//
// The show bundle is a raw `data` partition, not a filesystem: it's a
// single opaque SHW1 blob, so LittleFS bought nothing and the web flasher
// (esptool-js, running in the browser) can't construct a filesystem image
// anyway. The browser writes the compiled bundle bytes directly at the
// partition's flash offset; the device just reads them back.
//
// On a missing or corrupt bundle we return false; the caller (main.cpp)
// falls back to a safe blackout (F5 makes that robust; F3 just logs + LED
// error).
#pragma once

#include "show_bundle.h"
#include "device_config.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Label of the raw data partition holding the SHW1 bundle (partitions.csv).
#define STORAGE_SHOW_PARTITION_LABEL "show"

// Read the "show" partition and parse it in one call. On success, fills
// `out_show` and returns true. On any failure (partition missing, read
// error, parse error), returns false and logs. `buf`/`buf_cap` is scratch
// space for the raw partition bytes (64 KB is safe; see BUNDLE_BUF_CAP in
// main.cpp).
bool storage_load_show(LoadedShow* out_show, uint8_t* buf, size_t buf_cap);

// Read the "devcfg" partition and parse it in one call. On success, fills
// `out_cfg` and returns true. On any failure (partition missing/not found,
// read error, or parseDeviceConfig rejecting the blob -- bad magic/
// version/CRC, including an erased/all-0xFF board), returns false and
// logs; the caller (main.cpp) falls back to the compiled-in Kconfig
// defaults -- see device_config.h's header comment.
bool storage_load_devcfg(DeviceConfig* out_cfg);

#ifdef __cplusplus
}
#endif
