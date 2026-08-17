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
#define getpid _getpid
#define strcasecmp _stricmp
#endif

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

static const char *version_json_string(yyjson_val *obj, const char *key) {
  yyjson_val *val = obj && yyjson_is_obj(obj) ? yyjson_obj_get(obj, key) : NULL;
  return val && yyjson_is_str(val) ? yyjson_get_str(val) : NULL;
}

static uint64_t version_json_uint(yyjson_val *obj, const char *key) {
  yyjson_val *val = obj && yyjson_is_obj(obj) ? yyjson_obj_get(obj, key) : NULL;
  return val && yyjson_is_uint(val) ? yyjson_get_uint(val) : 0;
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

const char *ant_release_platform_target(void) {
  return ant_platform_target();
}

bool ant_version_print_update_hint(FILE *out) {
  if (!out || getenv("ANT_NO_VERSION_CHECK")) return false;
  char err[256] = {0};
  ant_latest_info_t latest;
  if (ant_fetch_latest(&latest, NULL, err, sizeof(err)) != 0) return false;
  if (!ant_latest_is_newer(&latest)) return false;
  crfprintf(out, "<yellow>update available</>: %s <green>(ant upgrade)</>\n", latest.version);
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

  if (rename(tmp_path, install_path) != 0) {
    fprintf(stderr, "ant upgrade: failed to install %s: %s\n", install_path, strerror(errno));
    remove(tmp_path);
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
