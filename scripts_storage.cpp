#include "scripts_storage.h"

#include <cstring>

// LittleFS's own filename limit (see esp_littlefs's LFS_NAME_MAX / the
// upstream littlefs default); reject anything that couldn't be created
// rather than let a truncated name silently collide with another script.
static constexpr size_t kMaxNameLen = 255;

bool scriptNameIsValid(const char* name, size_t len) {
  if (name == nullptr || len == 0 || len > kMaxNameLen) return false;
  if (len == 1 && name[0] == '.') return false;
  if (len == 2 && name[0] == '.' && name[1] == '.') return false;
  for (size_t i = 0; i < len; ++i) {
    if (name[i] == '/') return false;
    if (name[i] == '\0') return false;  // embedded NUL: not a valid filename
  }
  return true;
}

#ifdef ESP_PLATFORM

//
// Device-only mount/read/save, against esp_littlefs (joltwallet/littlefs,
// see glow_core's idf_component.yml). Untestable without hardware -- same
// status as web_input.cpp / midi_input.cpp / osc_input.cpp's transports.
//

#include "esp_littlefs.h"
#include "esp_log.h"

#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>

static const char* TAG = "scripts_storage";
static bool s_mounted = false;

// Guard rails: names arrive off the network (glow.save / the console's
// script-save message), so cap both the size of any one script and how
// many can pile up on the partition -- neither is enforced by LittleFS
// itself.
static constexpr size_t kMaxScriptBytes = 32 * 1024;
static constexpr size_t kMaxScriptCount = 64;

// scripts_storage_save writes to "/scripts/.<name>.tmp" then rename()s
// over the final path (never partially overwrites on failure -- see the
// header's contract). That temp file must not show up as a script in
// listings or count against kMaxScriptCount; scriptNameIsValid otherwise
// allows leading dots (see its own tests), so a suffix check is what
// actually distinguishes our own staging file from a real script.
static bool isTempFile(const char* name) {
  size_t n = std::strlen(name);
  constexpr char kSuffix[] = ".tmp";
  size_t suffixLen = sizeof(kSuffix) - 1;
  return name[0] == '.' && n > suffixLen &&
         std::strcmp(name + n - suffixLen, kSuffix) == 0;
}

// Counts real (non-temp-file) entries directly under "/scripts". Returns
// 0 if the directory can't be opened -- callers only use this to guard
// kMaxScriptCount, and an unreadable directory has bigger problems that
// scripts_storage_mount already reported.
static size_t scriptCount() {
  DIR* d = opendir("/scripts");
  if (!d) return 0;
  size_t count = 0;
  struct dirent* ent;
  while ((ent = readdir(d)) != nullptr) {
    if (ent->d_type != DT_REG) continue;
    if (isTempFile(ent->d_name)) continue;
    ++count;
  }
  closedir(d);
  return count;
}

bool scripts_storage_mount(void) {
  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = "/scripts";
  conf.partition_label = SCRIPTS_PARTITION_LABEL;
  conf.format_if_mount_failed = true;
  conf.dont_mount = false;

  esp_err_t err = esp_vfs_littlefs_register(&conf);
  s_mounted = (err == ESP_OK);
  if (!s_mounted) {
    ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
  }
  return s_mounted;
}

bool scripts_storage_read_boot(char* buf, size_t bufCap, size_t* outLen) {
  if (!s_mounted) return false;
  FILE* f = fopen("/scripts/" SCRIPTS_BOOT_FILENAME, "rb");
  if (!f) return false;
  size_t n = fread(buf, 1, bufCap, f);
  bool truncated = (feof(f) == 0);
  fclose(f);
  if (truncated) return false;  // never hand back a silently-truncated boot script
  *outLen = n;
  return true;
}

bool scripts_storage_save(const char* name, const char* src, size_t len) {
  if (!s_mounted) return false;
  if (!scriptNameIsValid(name, std::strlen(name))) return false;
  if (len > kMaxScriptBytes) return false;

  char finalPath[320];
  std::snprintf(finalPath, sizeof(finalPath), "/scripts/%s", name);

  // Only a NEW file counts against kMaxScriptCount -- overwriting an
  // existing script doesn't grow the partition's file count.
  struct stat st {};
  bool isNewFile = (stat(finalPath, &st) != 0);
  if (isNewFile && scriptCount() >= kMaxScriptCount) return false;

  char tmpPath[320];
  std::snprintf(tmpPath, sizeof(tmpPath), "/scripts/.%s.tmp", name);

  FILE* f = fopen(tmpPath, "wb");
  if (!f) return false;
  size_t written = fwrite(src, 1, len, f);
  bool closedOk = (fclose(f) == 0);  // always close, even if the write was short
  bool ok = (written == len) && closedOk;
  if (ok) {
    ok = (rename(tmpPath, finalPath) == 0);
  }
  if (!ok) {
    remove(tmpPath);
  }
  return ok;
}

bool scripts_storage_list(ScriptListCallback cb, void* ctx) {
  if (!s_mounted) return false;
  DIR* d = opendir("/scripts");
  if (!d) return false;
  struct dirent* ent;
  while ((ent = readdir(d)) != nullptr) {
    if (ent->d_type != DT_REG) continue;  // flat root only, no subdirs
    if (isTempFile(ent->d_name)) continue;
    if (!cb(ent->d_name, ctx)) break;
  }
  closedir(d);
  return true;
}

bool scripts_storage_load(const char* name, char* buf, size_t bufCap, size_t* outLen) {
  if (!s_mounted) return false;
  if (!scriptNameIsValid(name, std::strlen(name))) return false;

  char path[320];
  std::snprintf(path, sizeof(path), "/scripts/%s", name);
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  size_t n = fread(buf, 1, bufCap, f);
  bool truncated = (feof(f) == 0);
  fclose(f);
  if (truncated) return false;
  *outLen = n;
  return true;
}

bool scripts_storage_delete(const char* name) {
  if (!s_mounted) return false;
  if (!scriptNameIsValid(name, std::strlen(name))) return false;

  char path[320];
  std::snprintf(path, sizeof(path), "/scripts/%s", name);
  return remove(path) == 0;
}

#endif  // ESP_PLATFORM
