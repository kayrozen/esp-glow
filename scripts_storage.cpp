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
// Device-only mount/read/save. Untestable without hardware -- same status
// as web_input.cpp / midi_input.cpp / osc_input.cpp's transports. See
// scripts_storage.h for the exact contract each stub must satisfy.
//

#include "esp_littlefs.h"
#include "esp_log.h"

static const char* TAG = "scripts_storage";
static bool s_mounted = false;

bool scripts_storage_mount(void) {
  // TODO:
  // esp_vfs_littlefs_conf_t conf = {
  //     .base_path = "/scripts",
  //     .partition_label = SCRIPTS_PARTITION_LABEL,
  //     .format_if_mount_failed = true,
  //     .dont_mount = false,
  // };
  // esp_err_t err = esp_vfs_littlefs_register(&conf);
  // s_mounted = (err == ESP_OK);
  // if (!s_mounted) ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
  // return s_mounted;
  ESP_LOGW(TAG, "scripts_storage_mount: not yet implemented (needs hardware to verify)");
  return false;
}

bool scripts_storage_read_boot(char* buf, size_t bufCap, size_t* outLen) {
  // TODO:
  // if (!s_mounted) return false;
  // FILE* f = fopen("/scripts/" SCRIPTS_BOOT_FILENAME, "rb");
  // if (!f) return false;
  // size_t n = fread(buf, 1, bufCap, f);
  // bool truncated = !feof(f);
  // fclose(f);
  // if (truncated) return false;  // never hand back a silently-truncated boot script
  // *outLen = n;
  // return true;
  (void)buf;
  (void)bufCap;
  (void)outLen;
  return false;
}

bool scripts_storage_save(const char* name, const char* src, size_t len) {
  if (!s_mounted) return false;
  if (!scriptNameIsValid(name, std::strlen(name))) return false;
  // TODO: write to a temp file and rename over the target (see header:
  // never partially overwrite on failure).
  // char tmpPath[320], finalPath[320];
  // snprintf(tmpPath, sizeof(tmpPath), "/scripts/.%s.tmp", name);
  // snprintf(finalPath, sizeof(finalPath), "/scripts/%s", name);
  // FILE* f = fopen(tmpPath, "wb");
  // if (!f) return false;
  // size_t written = fwrite(src, 1, len, f);
  // bool ok = (written == len) && (fclose(f) == 0);
  // if (ok) ok = (rename(tmpPath, finalPath) == 0);
  // if (!ok) remove(tmpPath);
  // return ok;
  (void)src;
  (void)len;
  return false;
}

bool scripts_storage_list(ScriptListCallback cb, void* ctx) {
  if (!s_mounted) return false;
  // TODO:
  // DIR* d = opendir("/scripts");
  // if (!d) return false;
  // struct dirent* ent;
  // while ((ent = readdir(d)) != nullptr) {
  //   if (ent->d_type != DT_REG) continue;  // flat root only, no subdirs
  //   if (!cb(ent->d_name, ctx)) break;
  // }
  // closedir(d);
  // return true;
  (void)cb;
  (void)ctx;
  return true;
}

bool scripts_storage_load(const char* name, char* buf, size_t bufCap, size_t* outLen) {
  if (!s_mounted) return false;
  if (!scriptNameIsValid(name, std::strlen(name))) return false;
  // TODO: same shape as scripts_storage_read_boot, generalized to `name`.
  // char path[320];
  // snprintf(path, sizeof(path), "/scripts/%s", name);
  // FILE* f = fopen(path, "rb");
  // if (!f) return false;
  // size_t n = fread(buf, 1, bufCap, f);
  // bool truncated = !feof(f);
  // fclose(f);
  // if (truncated) return false;
  // *outLen = n;
  // return true;
  (void)buf;
  (void)bufCap;
  (void)outLen;
  return false;
}

bool scripts_storage_delete(const char* name) {
  if (!s_mounted) return false;
  if (!scriptNameIsValid(name, std::strlen(name))) return false;
  // TODO:
  // char path[320];
  // snprintf(path, sizeof(path), "/scripts/%s", name);
  // return remove(path) == 0;
  return false;
}

#endif  // ESP_PLATFORM
