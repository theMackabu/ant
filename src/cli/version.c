#include "cli/version.h"
#include "download.h"

#include "progress.h"
#include "utils.h"

#include <argtable3.h>
#include <crprintf.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uv.h>
#include <yyjson.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#include <strings.h>
#else
#include <io.h>
#include <process.h>
#include <windows.h>
#define getpid _getpid
#define strcasecmp _stricmp
#endif

#define ANT_VERSION_CHECK_INTERVAL_SECONDS (72u * 60u * 60u)
#define ANT_VERSION_CHECK_CACHE_FILE "version-check.json"

typedef struct {
  char target[64];
  char version[96];
  char download_url[2048];
  char release_notes_url[2048];
  uint64_t build_timestamp;
  uint64_t size;
} ant_latest_info_t;

typedef struct {
  unsigned major;
  unsigned minor;
  unsigned patch;
  bool ok;
} ant_version_parts_t;

typedef struct {
  uint64_t checked_at;
  char manifest_url[2048];
  char target[64];
  ant_latest_info_t latest;
  bool has_latest;
} ant_version_cache_t;

static const char *version_json_string(yyjson_val *obj, const char *key) {
  yyjson_val *val = obj && yyjson_is_obj(obj) ? yyjson_obj_get(obj, key) : NULL;
  return val && yyjson_is_str(val) ? yyjson_get_str(val) : NULL;
}

static uint64_t version_json_uint(yyjson_val *obj, const char *key) {
  yyjson_val *val = obj && yyjson_is_obj(obj) ? yyjson_obj_get(obj, key) : NULL;
  return val && yyjson_is_uint(val) ? yyjson_get_uint(val) : 0;
}

static bool version_json_copy_string(yyjson_val *obj, const char *key, char *out, size_t out_len) {
  yyjson_val *val = obj && yyjson_is_obj(obj) ? yyjson_obj_get(obj, key) : NULL;
  if (!val || !yyjson_is_str(val) || !out || out_len == 0) return false;
  size_t len = yyjson_get_len(val);
  if (len >= out_len) return false;
  memcpy(out, yyjson_get_str(val), len + 1);
  return true;
}

static const char *ant_platform_target(void) {
#if defined(__APPLE__)
  #if defined(__aarch64__) || defined(_M_ARM64)
  return "darwin-aarch64";
  #else
  return "darwin-x64";
  #endif
#elif defined(_WIN32)
  return "windows-x64";
#elif defined(__linux__)
  #if defined(__aarch64__) || defined(_M_ARM64)
    #if defined(__MUSL__) || defined(ANT_TARGET_TRIPLE)
  return strstr(ANT_TARGET_TRIPLE, "musl") ? "linux-aarch64-musl" : "linux-aarch64";
    #else
  return "linux-aarch64";
    #endif
  #else
    #if defined(__MUSL__) || defined(ANT_TARGET_TRIPLE)
  return strstr(ANT_TARGET_TRIPLE, "musl") ? "linux-x64-musl" : "linux-x64";
    #else
  return "linux-x64";
    #endif
  #endif
#else
  return "unknown";
#endif
}

static ant_version_parts_t ant_parse_version(const char *s) {
  ant_version_parts_t out = {0};
  if (!s) return out;
  char *end = NULL;
  unsigned long major = strtoul(s, &end, 10);
  if (!end || *end != '.') return out;
  unsigned long minor = strtoul(end + 1, &end, 10);
  if (!end || *end != '.') return out;
  const char *build = end + 1;
  const char *patch_start = strchr(build, '.');
  if (!patch_start) return out;
  if (patch_start == build) return out;
  unsigned long patch = strtoul(patch_start + 1, &end, 10);
  if (!end || *end != '\0') return out;
  if (major > UINT_MAX || minor > UINT_MAX || patch > UINT_MAX) return out;
  out.major = (unsigned)major;
  out.minor = (unsigned)minor;
  out.patch = (unsigned)patch;
  out.ok = true;
  return out;
}

static int ant_version_compare(const char *a, const char *b) {
  ant_version_parts_t av = ant_parse_version(a);
  ant_version_parts_t bv = ant_parse_version(b);
  if (!av.ok || !bv.ok) return strcmp(a ? a : "", b ? b : "");
  if (av.major != bv.major) return av.major < bv.major ? -1 : 1;
  if (av.minor != bv.minor) return av.minor < bv.minor ? -1 : 1;
  if (av.patch != bv.patch) return av.patch < bv.patch ? -1 : 1;
  return 0;
}

static bool ant_latest_is_newer(const ant_latest_info_t *latest) {
  if (!latest || !latest->version[0]) return false;
  int cmp = ant_version_compare(ANT_VERSION, latest->version);
  if (cmp != 0) return cmp < 0;
  if (strcmp(ANT_VERSION, latest->version) == 0) return false;
  return latest->build_timestamp > (uint64_t)ANT_BUILD_TIMESTAMP;
}

static int ant_manifest_select_latest(const char *json, size_t json_len, ant_latest_info_t *latest, char *err, size_t err_len) {
  if (!latest) return -EINVAL;
  memset(latest, 0, sizeof(*latest));
  const char *target = ant_platform_target();
  snprintf(latest->target, sizeof(latest->target), "%s", target);

  yyjson_doc *doc = yyjson_read(json, json_len, 0);
  if (!doc) {
    snprintf(err, err_len, "manifest response was not valid JSON");
    return -EINVAL;
  }

  int rc = -ENOENT;
  yyjson_val *root = yyjson_doc_get_root(doc);
  yyjson_val *items = root && yyjson_is_obj(root) ? yyjson_obj_get(root, "ant") : NULL;
  if (items && yyjson_is_arr(items)) {
    size_t idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(items, idx, max, item) {
      const char *item_target = version_json_string(item, "target");
      yyjson_val *available = yyjson_obj_get(item, "available");
      if (!item_target || strcmp(item_target, target) != 0) continue;
      if (available && yyjson_is_bool(available) && !yyjson_get_bool(available)) continue;

      const char *version = version_json_string(item, "version");
      const char *download_url = version_json_string(item, "download_url");
      if (!version || !download_url) break;
      snprintf(latest->version, sizeof(latest->version), "%s", version);
      snprintf(latest->download_url, sizeof(latest->download_url), "%s", download_url);

      const char *release_notes_url = version_json_string(item, "release_notes_url");
      if (release_notes_url)
        snprintf(latest->release_notes_url, sizeof(latest->release_notes_url), "%s", release_notes_url);
      
      latest->build_timestamp = version_json_uint(item, "build_timestamp");
      yyjson_val *artifact = yyjson_obj_get(item, "artifact");
      latest->size = version_json_uint(artifact, "size_in_bytes");
      
      rc = 0;
      break;
    }
  }

  if (rc != 0) snprintf(err, err_len, "manifest is missing Ant target %s", target);
  yyjson_doc_free(doc);
  return rc;
}

static int ant_fetch_latest(ant_latest_info_t *latest, progress_t *progress, char *err, size_t err_len) {
  const char *url = ant_manifest_url();
  char *manifest = NULL;
  size_t manifest_len = 0;
  
  int rc = ant_download_get(
    url, NULL, progress ? "Checking latest version" : NULL, 
    progress, &manifest, &manifest_len, err, err_len
  );
  
  if (rc != 0) return rc;
  rc = ant_manifest_select_latest(manifest, manifest_len, latest, err, err_len);
  free(manifest);
  
  return rc;
}

static bool ant_version_cache_paths(
  char *dir, size_t dir_len,
  char *path, size_t path_len
) {
  if (ant_xdg_cache_path(dir, dir_len, NULL) != 0) return false;
  int written = snprintf(path, path_len, "%s/%s", dir, ANT_VERSION_CHECK_CACHE_FILE);
  return written >= 0 && (size_t)written < path_len;
}

static bool ant_version_cache_read(ant_version_cache_t *cache) {
  if (!cache) return false;
  memset(cache, 0, sizeof(*cache));

  char dir[4096];
  char path[4096];
  if (!ant_version_cache_paths(dir, sizeof(dir), path, sizeof(path))) return false;

  yyjson_doc *doc = yyjson_read_file(path, 0, NULL, NULL);
  if (!doc) return false;

  bool ok = false;
  yyjson_val *root = yyjson_doc_get_root(doc);
  if (
    root && yyjson_is_obj(root) &&
    version_json_copy_string(root, "manifest_url", cache->manifest_url, sizeof(cache->manifest_url)) &&
    version_json_copy_string(root, "target", cache->target, sizeof(cache->target))
  ) {
    cache->checked_at = version_json_uint(root, "checked_at");
    yyjson_val *latest = yyjson_obj_get(root, "latest");
    if (latest && yyjson_is_obj(latest)) {
      cache->has_latest = version_json_copy_string(
        latest, "version", cache->latest.version, sizeof(cache->latest.version)
      );
      if (cache->has_latest) {
        snprintf(cache->latest.target, sizeof(cache->latest.target), "%s", cache->target);
        cache->latest.build_timestamp = version_json_uint(latest, "build_timestamp");
      }
    }
    ok = cache->checked_at != 0;
  }

  yyjson_doc_free(doc);
  if (!ok) memset(cache, 0, sizeof(*cache));
  return ok;
}

static int ant_version_cache_replace(const char *tmp_path, const char *path) {
#ifdef _WIN32
  return MoveFileExA(
    tmp_path, path,
    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
  ) ? 0 : -1;
#else
  return rename(tmp_path, path);
#endif
}

static void ant_version_cache_write(const ant_version_cache_t *cache) {
  if (!cache || cache->checked_at == 0 || !cache->manifest_url[0] || !cache->target[0]) return;

  char dir[4096];
  char path[4096];
  if (!ant_version_cache_paths(dir, sizeof(dir), path, sizeof(path))) return;
  if (ant_mkdir_p(dir) != 0) return;

  char tmp_path[4096];
  int written = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", path, (long)getpid());
  if (written < 0 || (size_t)written >= sizeof(tmp_path)) return;

  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  if (!doc) return;
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  if (!root) {
    yyjson_mut_doc_free(doc);
    return;
  }
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_obj_add_uint(doc, root, "checked_at", cache->checked_at);
  yyjson_mut_obj_add_strcpy(doc, root, "manifest_url", cache->manifest_url);
  yyjson_mut_obj_add_strcpy(doc, root, "target", cache->target);
  if (cache->has_latest) {
    yyjson_mut_val *latest = yyjson_mut_obj_add_obj(doc, root, "latest");
    if (latest) {
      yyjson_mut_obj_add_strcpy(doc, latest, "version", cache->latest.version);
      yyjson_mut_obj_add_uint(doc, latest, "build_timestamp", cache->latest.build_timestamp);
    }
  }

  bool saved = yyjson_mut_write_file(tmp_path, doc, 0, NULL, NULL);
  yyjson_mut_doc_free(doc);
  if (!saved || ant_version_cache_replace(tmp_path, path) != 0) remove(tmp_path);
}

static bool ant_version_cache_scope_matches(
  const ant_version_cache_t *cache,
  const char *manifest_url,
  const char *target
) {
  return cache && manifest_url && target &&
    strcmp(cache->manifest_url, manifest_url) == 0 &&
    strcmp(cache->target, target) == 0;
}

static bool ant_version_cache_is_fresh(const ant_version_cache_t *cache, uint64_t now) {
  return cache && cache->checked_at <= now &&
    now - cache->checked_at < ANT_VERSION_CHECK_INTERVAL_SECONDS;
}

static void ant_version_cache_store_latest(const ant_latest_info_t *latest) {
  if (!latest) return;
  time_t now = time(NULL);
  if (now <= 0) return;

  ant_version_cache_t cache = {0};
  cache.checked_at = (uint64_t)now;
  snprintf(cache.manifest_url, sizeof(cache.manifest_url), "%s", ant_manifest_url());
  snprintf(cache.target, sizeof(cache.target), "%s", ant_platform_target());
  cache.latest = *latest;
  cache.has_latest = latest->version[0] != '\0';
  ant_version_cache_write(&cache);
}

const char *ant_release_platform_target(void) {
  return ant_platform_target();
}

bool ant_version_print_update_hint(FILE *out) {
  if (!out || getenv("ANT_NO_VERSION_CHECK")) return false;

  const char *manifest_url = ant_manifest_url();
  const char *target = ant_platform_target();
  time_t now_time = time(NULL);
  uint64_t now = now_time > 0 ? (uint64_t)now_time : 0;

  ant_version_cache_t cache;
  bool cache_matches = ant_version_cache_read(&cache) &&
    ant_version_cache_scope_matches(&cache, manifest_url, target);

  if (!cache_matches) memset(&cache, 0, sizeof(cache));

  if (!cache_matches || !ant_version_cache_is_fresh(&cache, now)) {
    char err[256] = {0};
    ant_latest_info_t latest;
    int rc = ant_fetch_latest(&latest, NULL, err, sizeof(err));

    cache.checked_at = now;
    snprintf(cache.manifest_url, sizeof(cache.manifest_url), "%s", manifest_url);
    snprintf(cache.target, sizeof(cache.target), "%s", target);
    if (rc == 0) {
      cache.latest = latest;
      cache.has_latest = true;
    }
    ant_version_cache_write(&cache);
  }

  if (!cache.has_latest || !ant_latest_is_newer(&cache.latest)) return false;
  crfprintf(out, "<yellow>update available</>: %s <green>(ant upgrade)</>\n", cache.latest.version);
  return true;
}

static int ant_install_path(char *out, size_t out_len) {
  char dir[4096];
  if (ant_user_bin_path(dir, sizeof(dir)) != 0) return -EINVAL;
  int rc = ant_mkdir_p(dir);
  if (rc != 0) return -errno;
#ifdef _WIN32
  int written = snprintf(out, out_len, "%s/ant.exe", dir);
#else
  int written = snprintf(out, out_len, "%s/ant", dir);
#endif
  return written < 0 || (size_t)written >= out_len ? -ENAMETOOLONG : 0;
}

#ifdef _WIN32
static void ant_windows_error_message(
  DWORD code, char *out, size_t out_len
) {
  if (!out || out_len == 0) return;

  char message[256] = {0};
  DWORD written = FormatMessageA(
    FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
    NULL, code, 0, message, (DWORD)sizeof(message), NULL
  );
  while (written > 0 && (message[written - 1] == '\r' || message[written - 1] == '\n'))
    message[--written] = '\0';

  if (written > 0)
    snprintf(out, out_len, "%s (Windows error %lu)", message, (unsigned long)code);
  else
    snprintf(out, out_len, "Windows error %lu", (unsigned long)code);
}
#endif

static int ant_install_downloaded(
  const char *tmp_path,
  const char *install_path,
  char *err,
  size_t err_len
) {
#ifdef _WIN32
  char backup_path[4096];
  int written = snprintf(backup_path, sizeof(backup_path), "%s.old", install_path);
  if (written < 0 || (size_t)written >= sizeof(backup_path)) {
    snprintf(err, err_len, "backup path is too long");
    remove(tmp_path);
    return -1;
  }

  DWORD attrs = GetFileAttributesA(install_path);
  bool had_existing = attrs != INVALID_FILE_ATTRIBUTES;
  if (!had_existing) {
    DWORD code = GetLastError();
    if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
      ant_windows_error_message(code, err, err_len);
      remove(tmp_path);
      return -1;
    }
  } else if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
    snprintf(err, err_len, "install path is a directory");
    remove(tmp_path);
    return -1;
  }

  const DWORD move_flags = MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH;
  if (had_existing && !MoveFileExA(install_path, backup_path, move_flags)) {
    DWORD code = GetLastError();
    ant_windows_error_message(code, err, err_len);
    remove(tmp_path);
    return -1;
  }

  if (MoveFileExA(tmp_path, install_path, move_flags)) return 0;

  DWORD install_error = GetLastError();
  char install_message[256];
  ant_windows_error_message(install_error, install_message, sizeof(install_message));
  if (!had_existing) {
    snprintf(err, err_len, "%s", install_message);
    remove(tmp_path);
    return -1;
  }

  if (MoveFileExA(backup_path, install_path, move_flags)) {
    snprintf(err, err_len, "%s; restored the previous executable", install_message);
    remove(tmp_path);
    return -1;
  }

  DWORD rollback_error = GetLastError();
  char rollback_message[256];
  ant_windows_error_message(rollback_error, rollback_message, sizeof(rollback_message));
  snprintf(
    err, err_len,
    "%s; also failed to restore %s: %s; downloaded executable remains at %s",
    install_message, backup_path, rollback_message, tmp_path
  );
  return -1;
#else
  if (rename(tmp_path, install_path) == 0) return 0;
  snprintf(err, err_len, "%s", strerror(errno));
  remove(tmp_path);
  return -1;
#endif
}

int ant_upgrade(int argc, char **argv) {
  (void)argc;
  (void)argv;
  char err[512] = {0};
  ant_latest_info_t latest;
  progress_t progress;

  crprintf("<bold>Current Ant version:</> <bright_green>%s</>\n", ANT_VERSION);
  crprintf("<dim>Looking up latest version</>\n\n");

  int rc = ant_fetch_latest(&latest, NULL, err, sizeof(err));
  if (rc != 0) {
    fprintf(stderr, "ant upgrade: %s\n", err[0] ? err : "failed to check latest version");
    return EXIT_FAILURE;
  }
  ant_version_cache_store_latest(&latest);

  if (!ant_latest_is_newer(&latest)) {
    crprintf("<bright_green>Ant is already up to date.</> <dim>(%s for %s)</>\n", ANT_VERSION, latest.target);
    return EXIT_SUCCESS;
  }

  crprintf("Found latest version <green>%s</>\n\n", latest.version);
  crprintf("Downloading <bright_green>%s</>\n", latest.download_url);
  crprintf("Ant is upgrading to version <green>%s</>\n\n", latest.version);
  fflush(stdout);

  char install_path[4096];
  rc = ant_install_path(install_path, sizeof(install_path));
  if (rc != 0) {
    fprintf(stderr, "ant upgrade: failed to resolve install path\n");
    return EXIT_FAILURE;
  }

  char tmp_path[4096];
  int written = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", install_path, (long)getpid());
  if (written < 0 || (size_t)written >= sizeof(tmp_path)) {
    fprintf(stderr, "ant upgrade: install path is too long\n");
    return EXIT_FAILURE;
  }

  FILE *file = fopen(tmp_path, "wb");
  if (!file) {
    fprintf(stderr, "ant upgrade: failed to open %s: %s\n", tmp_path, strerror(errno));
    return EXIT_FAILURE;
  }

  progress_start(&progress, "Downloading Ant");
  rc = ant_download_get(latest.download_url, file, "Downloading Ant", &progress, NULL, NULL, err, sizeof(err));
  progress_stop(&progress);
  
  int close_rc = fclose(file);
  if (rc != 0 || close_rc != 0) {
    if (close_rc != 0 && rc == 0) snprintf(err, sizeof(err), "failed to close %s: %s", tmp_path, strerror(errno));
    remove(tmp_path);
    fprintf(stderr, "ant upgrade: %s\n", err[0] ? err : "download failed");
    return EXIT_FAILURE;
  }

#ifndef _WIN32
  if (chmod(tmp_path, 0755) != 0) {
    fprintf(stderr, "ant upgrade: failed to mark %s executable: %s\n", tmp_path, strerror(errno));
    remove(tmp_path);
    return EXIT_FAILURE;
  }
#endif

  if (ant_install_downloaded(tmp_path, install_path, err, sizeof(err)) != 0) {
    fprintf(stderr, "ant upgrade: failed to install %s: %s\n", install_path, err);
    return EXIT_FAILURE;
  }

  crprintf("<bright_green>Upgraded successfully to Ant %s</>\n", latest.version);
  crprintf("<dim>Installed at %s</>\n", install_path);

  if (latest.release_notes_url[0]) {
    crprintf("\n<bold>Release notes:</>\n\n");
    crprintf("  <green>%s</>\n\n", latest.release_notes_url);
  }
  
  return EXIT_SUCCESS;
}

int ant_version(void *argtable[]) {
  time_t build_time = (time_t)ANT_BUILD_TIMESTAMP;
  time_t now = time(NULL);
  long diff = (long)difftime(now, build_time);
  
  struct { long secs; const char *suffix; } units[] = {
    {86400, "d"}, {3600, "h"}, {60, "m"}, {1, "s"}
  };
  
  const char *suffix = "s";
  long value = diff;
  
  for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
    if (diff >= units[i].secs) {
      value = diff / units[i].secs;
      suffix = units[i].suffix; break;
    }
  }
  
  struct tm *tm = gmtime(&build_time);
  char date_buf[32];
  strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", tm);
  
  #define RED "\033[38;5;197m"
  #define RESET "\033[0m"
  
  const char *logo =
    RED
    "    ___          __         __                  _____           _       __\n"
    "   /   |  ____  / /_       / /___ __   ______ _/ ___/__________(_)___  / /_\n"
    "  / /| | / __ \\/ __/  __  / / __ `/ | / / __ `/\\__ \\/ ___/ ___/ / __ \\/ __/\n"
    " / ___ |/ / / / /_   / /_/ / /_/ /| |/ / /_/ /___/ / /__/ /  / / /_/ / /_\n"
    "/_/  |_/_/ /_/\\__/   \\____/\\__,_/ |___/\\__,_//____/\\___/_/  /_/ .___/\\__/\n"
    "                                                             /_/" RESET "   by @themackabu\n"
    RESET;
  
  fputs(logo, stdout);
  if (ant_version_print_update_hint(stdout)) printf("\n");
  
  printf("%s (released %s, %ld%s ago)\n", 
    ANT_VERSION, 
    date_buf, 
    value, suffix
  );
  
  printf("built for %s\n", ANT_TARGET_TRIPLE);
  arg_freetable(argtable, ARGTABLE_COUNT);
  
  return EXIT_SUCCESS;
}
