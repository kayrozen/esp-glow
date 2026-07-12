// scripts_storage.h — LittleFS-backed script storage (design doc phases 2,
// 6, 8; README_LUA_FENNEL.md).
//
// Unlike storage_manager.h's raw "show" partition (a single opaque blob,
// browser-flashed, read-only at runtime), the "scripts" partition (see
// partitions.csv) is a real filesystem: many small named Fennel/Lua files
// the DEVICE ITSELF creates and overwrites at runtime via glow.save, plus
// a boot.fnl evaluated at startup so a live-coded show survives reboot.
//
// Mounting a filesystem, reading directory entries, and writing files are
// all real hardware/flash-driver behavior this environment cannot
// exercise (same reasoning as web_input.cpp / midi_input.cpp /
// osc_input.cpp's still-TODO transports) — the bodies here are stubs
// documenting the intended shape. The contract each stub must satisfy
// when implemented is written out below precisely so filling them in is
// mechanical.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCRIPTS_PARTITION_LABEL "scripts"
#define SCRIPTS_BOOT_FILENAME "boot.fnl"

// Host-testable core: true iff `name` is safe to use as a script filename
// -- non-empty, fits LittleFS's filename limit, contains no '/' (so a
// script can only ever land directly under the partition's flat root,
// never in a subdirectory), and isn't exactly "." or ".." (which a flat
// root doesn't need and a naive path-join could otherwise misinterpret).
// Saving as SCRIPTS_BOOT_FILENAME is allowed and intentional -- that's how
// a script becomes the boot script. Pure function, no device dependency.
bool scriptNameIsValid(const char* name, size_t len);

// Mount the "scripts" LittleFS partition. Must be called once before any
// other scripts_storage_* function. Returns false if the partition is
// missing/corrupt; the caller's contract (main.cpp) is: proceed without a
// boot script rather than fail the whole boot — an unwritten scripts
// partition on first boot is normal, not an error condition.
//
// Implementation contract: esp_vfs_littlefs_register() with
// {partition_label = SCRIPTS_PARTITION_LABEL, format_if_mount_failed =
// true} — format-on-first-mount-failure is correct here (unlike the
// "show" partition) because this partition has no meaningful un-formatted
// content to preserve; a corrupt scripts filesystem is safely replaced
// with an empty one.
bool scripts_storage_mount(void);

// Read boot.fnl in one call. On success, fills `buf` (NOT
// NUL-terminated -- treat as `*outLen` raw bytes) and returns true. On any
// failure (not mounted, file absent, read error, doesn't fit in bufCap),
// returns false -- the caller's contract (main.cpp) is: skip boot.fnl and
// continue rendering, never brick on a missing/corrupt boot script.
bool scripts_storage_read_boot(char* buf, size_t bufCap, size_t* outLen);

// Write `src` (len bytes) to a script file named `name` (no path
// separators; implementations should reject any containing '/' or ".."
// to stay confined to the scripts partition's root). Used by glow.save
// from the live-coding REPL (design doc phase 8). Returns false on any
// failure (not mounted, invalid name, write/flash error); never partially
// overwrites the previous file's content on failure (write to a temp name
// and rename, so a power loss mid-write can't corrupt an existing script).
bool scripts_storage_save(const char* name, const char* src, size_t len);

// Called once per script file present in the "scripts" partition (flat
// root only -- there are no subdirectories), in whatever order the
// underlying directory listing returns them. `name` is NUL-terminated and
// does not include a path. Return true to keep listing, false to stop
// early. Used by the console's script_list message (web_protocol.h's
// buildScriptsJson) -- the WS layer supplies a callback that appends into
// a fixed-size name buffer and returns false once it's full, so a
// pathological number of files on the partition can't grow the response
// message unboundedly.
typedef bool (*ScriptListCallback)(const char* name, void* ctx);

// Lists every script file. Returns false if not mounted or the underlying
// directory listing fails; returns true (even with zero files found)
// otherwise, including when `cb` returned false to stop early.
bool scripts_storage_list(ScriptListCallback cb, void* ctx);

// Read a script file named `name` in one call (same contract as
// scripts_storage_read_boot, generalized to any name). On success, fills
// `buf` (NOT NUL-terminated -- treat as `*outLen` raw bytes) and returns
// true. On any failure (not mounted, invalid name, file absent, read
// error, doesn't fit in bufCap), returns false.
bool scripts_storage_load(const char* name, char* buf, size_t bufCap, size_t* outLen);

// Delete a script file named `name`. Returns false on any failure (not
// mounted, invalid name, file absent, or a filesystem error); true if the
// file no longer exists afterward.
bool scripts_storage_delete(const char* name);

#ifdef __cplusplus
}
#endif
