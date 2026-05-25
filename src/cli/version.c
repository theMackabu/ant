#include "cli/version.h"

#include "modules/http.h"
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
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <strings.h>
#else
#include <io.h>
#include <process.h>
#define getpid _getpid
#define strcasecmp _stricmp
#endif

#define ANT_MANIFEST_URL "https://manifest.antjs.org/v1/latest"

static char ant_semver_buf[32];
static pthread_once_t ant_semver_once = PTHREAD_ONCE_INIT;

typedef struct {
  char *data;
  size_t len;
  size_t cap;
  FILE *file;
  int status;
  int rc;
  uint64_t content_length;
  uint64_t received;
  const char *label;
  progress_t *progress;
  ant_http_request_t *request;
  bool completed;
  char error[256];
} version_http_ctx_t;

typedef struct {
  char target[64];
  char version[96];
  char download_url[2048];
  uint64_t size;
} ant_latest_info_t;

typedef struct {
  unsigned major;
  unsigned minor;
  unsigned patch;
  uint64_t build;
  bool ok;
} ant_version_parts_t;

static void ant_semver_init(void) {
  const char *s = ANT_VERSION;
  int d = 0, i = 0;
  while (s[i] && d < 3 && i < 31) {
    if (s[i] == '.') d++;
    ant_semver_buf[i] = s[i]; i++;
  }
  ant_semver_buf[i - (d == 3)] = '\0';
}

const char *ant_semver(void) {
  pthread_once(&ant_semver_once, ant_semver_init);
  return ant_semver_buf;
}

static void version_format_bytes(char *out, size_t out_len, uint64_t bytes) {
  static const char *units[] = {"B", "KiB", "MiB", "GiB"};
  double value = (double)bytes;
  size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
    value /= 1024.0;
    unit++;
  }
  if (unit == 0) snprintf(out, out_len, "%llu%s", (unsigned long long)bytes, units[unit]);
  else snprintf(out, out_len, "%.2f%s", value, units[unit]);
}

static void version_progress_message(char *out, size_t out_len, const char *label, uint64_t received, uint64_t total, bool color) {
  if (!out || out_len == 0) return;
  char got[32];
  char want[32];
  version_format_bytes(got, sizeof(got), received);
  version_format_bytes(want, sizeof(want), total);
  if (total == 0) {
    snprintf(out, out_len, "%s [%s]", label, got);
    return;
  }

  unsigned pct = (unsigned)((received * 100u) / total);
  if (pct > 100u) pct = 100u;
  unsigned filled = pct / 5u;
  char bar[128];
  size_t pos = 0;
  if (color) pos += (size_t)snprintf(bar + pos, sizeof(bar) - pos, "\x1b[32m");
  for (unsigned i = 0; i < filled && pos + 1 < sizeof(bar); i++) bar[pos++] = '#';
  if (color) pos += (size_t)snprintf(bar + pos, sizeof(bar) - pos, "\x1b[90m");
  for (unsigned i = filled; i < 20 && pos + 1 < sizeof(bar); i++) bar[pos++] = '-';
  if (color) snprintf(bar + pos, sizeof(bar) - pos, "\x1b[0m");
  else bar[pos] = '\0';

  snprintf(out, out_len, "%s [%s] %s/%s", label, bar, got, want);
}

static uint64_t version_parse_u64(const char *value) {
  if (!value || !value[0]) return 0;
  char *end = NULL;
  unsigned long long parsed = strtoull(value, &end, 10);
  return end && *end == '\0' ? (uint64_t)parsed : 0;
}

static const char *version_http_header(const ant_http_header_t *headers, const char *name) {
  for (const ant_http_header_t *hdr = headers; hdr; hdr = hdr->next) {
    if (hdr->name && hdr->value && strcasecmp(hdr->name, name) == 0) return hdr->value;
  }
  return NULL;
}

static void version_http_fail(version_http_ctx_t *ctx, int rc, const char *message) {
  if (!ctx || ctx->rc != 0) return;
  ctx->rc = rc;
  snprintf(ctx->error, sizeof(ctx->error), "%s", message ? message : "network error");
  if (ctx->request) ant_http_request_cancel(ctx->request);
}

static void version_http_update_progress(version_http_ctx_t *ctx) {
  if (!ctx || !ctx->progress || !ctx->label) return;
  char msg[160];
  version_progress_message(msg, sizeof(msg), ctx->label, ctx->received, ctx->content_length, ctx->progress->supports_ansi);
  progress_update(ctx->progress, msg);
}

static void version_http_response_cb(ant_http_request_t *req, const ant_http_response_t *resp, void *user_data) {
  (void)req;
  version_http_ctx_t *ctx = user_data;
  ctx->status = resp ? resp->status : 0;
  const char *len = resp ? version_http_header(resp->headers, "content-length") : NULL;
  uint64_t content_length = version_parse_u64(len);
  if (content_length > 0) ctx->content_length = content_length;
  version_http_update_progress(ctx);
}

static void version_http_body_cb(ant_http_request_t *req, const uint8_t *chunk, size_t len, void *user_data) {
  (void)req;
  version_http_ctx_t *ctx = user_data;
  if (ctx->rc != 0 || len == 0) return;

  if (ctx->file) {
    if (fwrite(chunk, 1, len, ctx->file) != len) {
      version_http_fail(ctx, -EIO, "failed writing download");
      return;
    }
  } else {
    if (len > SIZE_MAX - ctx->len - 1) {
      version_http_fail(ctx, -EOVERFLOW, "response too large");
      return;
    }
    if (ctx->len + len + 1 > ctx->cap) {
      size_t next = ctx->cap ? ctx->cap * 2u : 16384u;
      while (next < ctx->len + len + 1) {
        if (next > SIZE_MAX / 2u) {
          next = ctx->len + len + 1;
          break;
        }
        next *= 2u;
      }
      char *data = realloc(ctx->data, next);
      if (!data) {
        version_http_fail(ctx, -ENOMEM, "out of memory");
        return;
      }
      ctx->data = data;
      ctx->cap = next;
    }
    memcpy(ctx->data + ctx->len, chunk, len);
    ctx->data[ctx->len + len] = '\0';
  }

  ctx->len += len;
  ctx->received += len;
  version_http_update_progress(ctx);
}

static void version_http_complete_cb(ant_http_request_t *req, ant_http_result_t result, int error_code, const char *error_message, void *user_data) {
  (void)req;
  version_http_ctx_t *ctx = user_data;
  ctx->completed = true;
  if (ctx->rc == 0 && result != ANT_HTTP_RESULT_OK) {
    ctx->rc = error_code < 0 ? error_code : -EIO;
    snprintf(ctx->error, sizeof(ctx->error), "%s", error_message ? error_message : "network error");
  }
  if (ctx->rc == 0 && (ctx->status < 200 || ctx->status >= 300)) {
    ctx->rc = -EIO;
    snprintf(ctx->error, sizeof(ctx->error), "HTTP %d", ctx->status);
  }
}

static int version_http_get(const char *url, FILE *file, const char *label, progress_t *progress, char **body_out, size_t *body_len_out, char *err, size_t err_len) {
  uv_loop_t loop;
  version_http_ctx_t ctx = {.file = file, .label = label, .progress = progress};
  ant_http_request_options_t options = {.method = "GET", .url = url};
  ant_http_request_t *req = NULL;
  int rc = uv_loop_init(&loop);
  if (body_out) *body_out = NULL;
  if (body_len_out) *body_len_out = 0;
  if (rc != 0) {
    snprintf(err, err_len, "failed to initialize network loop: %s", uv_strerror(rc));
    return rc;
  }

  rc = ant_http_request_start(&loop, &options, version_http_response_cb, version_http_body_cb, version_http_complete_cb, &ctx, &req);
  ctx.request = req;
  if (rc == 0) uv_run(&loop, UV_RUN_DEFAULT);
  (void)uv_loop_close(&loop);

  if (rc != 0) {
    free(ctx.data);
    snprintf(err, err_len, "failed to request %s: %s", url, uv_strerror(rc));
    return rc;
  }
  if (ctx.rc != 0) {
    free(ctx.data);
    snprintf(err, err_len, "failed to download %s: %s", url, ctx.error[0] ? ctx.error : "network error");
    return ctx.rc;
  }

  if (body_out) {
    *body_out = ctx.data;
    if (body_len_out) *body_len_out = ctx.len;
  } else free(ctx.data);
  return 0;
}

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
  unsigned long patch = strtoul(end + 1, &end, 10);
  uint64_t build = 0;
  if (end && *end == '.') build = strtoull(end + 1, NULL, 10);
  if (major > UINT_MAX || minor > UINT_MAX || patch > UINT_MAX) return out;
  out.major = (unsigned)major;
  out.minor = (unsigned)minor;
  out.patch = (unsigned)patch;
  out.build = build;
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
  if (av.build != bv.build) return av.build < bv.build ? -1 : 1;
  return 0;
}

static bool ant_latest_is_newer(const ant_latest_info_t *latest) {
  return latest && latest->version[0] && ant_version_compare(ANT_VERSION, latest->version) < 0;
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
  const char *url = getenv("ANT_MANIFEST_URL");
  if (!url || !url[0]) url = ANT_MANIFEST_URL;
  char *manifest = NULL;
  size_t manifest_len = 0;
  int rc = version_http_get(url, NULL, progress ? "Checking latest version" : NULL, progress, &manifest, &manifest_len, err, err_len);
  if (rc != 0) return rc;
  rc = ant_manifest_select_latest(manifest, manifest_len, latest, err, err_len);
  free(manifest);
  return rc;
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
  progress_start(&progress, "Checking latest version");
  int rc = ant_fetch_latest(&latest, &progress, err, sizeof(err));
  progress_stop(&progress);
  if (rc != 0) {
    fprintf(stderr, "ant upgrade: %s\n", err[0] ? err : "failed to check latest version");
    return EXIT_FAILURE;
  }

  if (!ant_latest_is_newer(&latest)) {
    printf("Ant %s is already the latest version for %s.\n", ANT_VERSION, latest.target);
    return EXIT_SUCCESS;
  }

  printf("Ant %s is out! You're on %s\n", latest.version, ANT_VERSION);

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
  rc = version_http_get(latest.download_url, file, "Downloading Ant", &progress, NULL, NULL, err, sizeof(err));
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

  printf("Upgraded Ant to %s at %s\n", latest.version, install_path);
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
