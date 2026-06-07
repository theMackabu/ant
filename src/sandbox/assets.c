#include <compat.h> // IWYU pragma: keep

#include "assets.h"
#include "modules/http.h"
#include "progress.h"
#include "sandbox/host.h"

#include <yyjson.h>
#include <uv.h>
#include <zlib.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#define sandbox_strcasecmp _stricmp
#else
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>
#define sandbox_strcasecmp strcasecmp
#endif

#define ANT_SANDBOX_MANIFEST_URL "https://manifest.antjs.org/v1/latest"

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
  struct sandbox_multi_progress_s *multi_progress;
  size_t multi_index;
  ant_http_request_t *request;
  bool gzip;
  bool gzip_done;
  bool zlib_init;
  bool validate_uncompressed_size;
  bool validate_gzip_size;
  z_stream zstrm;
  uint64_t compressed_received;
  uint64_t expected_gzip_size;
  uint64_t expected_uncompressed_size;
  bool completed;
  char error[256];
} sandbox_http_ctx_t;

typedef struct {
  const char *kind;
  const char *path;
  const char *url;
  bool gzip;
  uint64_t expected_size;
  uint64_t expected_gzip_size;
} sandbox_download_t;

typedef struct {
  char sandbox_url[2048];
  char kernel_url[2048];
  char sandbox_gzip_url[2048];
  char kernel_gzip_url[2048];
  uint64_t sandbox_size;
  uint64_t kernel_size;
  uint64_t sandbox_gzip_size;
  uint64_t kernel_gzip_size;
} sandbox_manifest_selection_t;

typedef struct sandbox_multi_progress_s {
  FILE *terminal;
  bool enabled;
  bool rendered;
  size_t rows;
  uint64_t prev_refresh_ns;
  uint64_t refresh_rate_ns;
  char lines[2][PROGRESS_MSG_SIZE];
} sandbox_multi_progress_t;

typedef struct {
  int rc;
  char error[512];
} sandbox_download_child_result_t;

static void sandbox_asset_error(char *err, size_t err_len, const char *fmt, ...)
  __attribute__((format(printf, 3, 4)));

static void sandbox_asset_error(char *err, size_t err_len, const char *fmt, ...) {
  if (!err || err_len == 0) return;

  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, err_len, fmt, ap);
  va_end(ap);
}

static bool sandbox_file_exists(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static const char *sandbox_http_header(const ant_http_header_t *headers, const char *name) {
  for (const ant_http_header_t *hdr = headers; hdr; hdr = hdr->next) {
    if (hdr->name && hdr->value && sandbox_strcasecmp(hdr->name, name) == 0) return hdr->value;
  }
  return NULL;
}

static uint64_t sandbox_parse_u64(const char *value) {
  if (!value || !value[0]) return 0;
  char *end = NULL;
  unsigned long long parsed = strtoull(value, &end, 10);
  return end && *end == '\0' ? (uint64_t)parsed : 0;
}

#ifndef _WIN32
static int sandbox_write_full(int fd, const void *buf, size_t len) {
  const unsigned char *p = buf;
  while (len > 0) {
    ssize_t n = write(fd, p, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -errno;
    }
    p += (size_t)n;
    len -= (size_t)n;
  }
  return 0;
}

static int sandbox_read_full(int fd, void *buf, size_t len) {
  unsigned char *p = buf;
  while (len > 0) {
    ssize_t n = read(fd, p, len);
    if (n == 0) return -EPIPE;
    if (n < 0) {
      if (errno == EINTR) continue;
      return -errno;
    }
    p += (size_t)n;
    len -= (size_t)n;
  }
  return 0;
}
#endif

static void sandbox_http_fail(sandbox_http_ctx_t *ctx, int rc, const char *fmt, ...)
  __attribute__((format(printf, 3, 4)));

static void sandbox_http_fail(sandbox_http_ctx_t *ctx, int rc, const char *fmt, ...) {
  if (!ctx || ctx->rc != 0) return;

  ctx->rc = rc;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ctx->error, sizeof(ctx->error), fmt, ap);
  va_end(ap);

  if (ctx->request) ant_http_request_cancel(ctx->request);
}

static void sandbox_progress_message(
  char *out,
  size_t out_len,
  const char *label,
  uint64_t received,
  uint64_t total
) {
  if (!out || out_len == 0) return;
  if (total > 0) {
    unsigned pct = (unsigned)((received * 100u) / total);
    if (pct > 100u) pct = 100u;
    unsigned filled = pct / 5u;
    char bar[21];
    for (unsigned i = 0; i < 20; i++) bar[i] = i < filled ? '#' : '-';
    bar[20] = '\0';
    snprintf(out, out_len, "Downloading %s [%s] %u%%", label, bar, pct);
  } else {
    snprintf(out, out_len, "Downloading %s (%llu bytes)", label, (unsigned long long)received);
  }
}

static bool sandbox_progress_enabled(void) {
  return PROGRESS_ISATTY(PROGRESS_FILENO(stderr));
}

static void sandbox_format_bytes(char *out, size_t out_len, uint64_t bytes) {
  static const char *units[] = {"B", "KiB", "MiB", "GiB"};
  double value = (double)bytes;
  size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
    value /= 1024.0;
    unit++;
  }

  if (unit == 0) snprintf(out, out_len, "%llu %s", (unsigned long long)bytes, units[unit]);
  else snprintf(out, out_len, "%.1f %s", value, units[unit]);
}

static void sandbox_multi_progress_line(
  char *out,
  size_t out_len,
  const char *label,
  uint64_t received,
  uint64_t total,
  bool done,
  bool failed
) {
  char received_buf[32];
  char total_buf[32];
  sandbox_format_bytes(received_buf, sizeof(received_buf), received);
  sandbox_format_bytes(total_buf, sizeof(total_buf), total);

  if (total > 0) {
    unsigned pct = (unsigned)((received * 100u) / total);
    if (pct > 100u) pct = 100u;
    unsigned filled = pct / 5u;
    char bar[256];
    size_t pos = 0;
    pos += (size_t)snprintf(bar + pos, sizeof(bar) - pos, "\x1b[32m");
    for (unsigned i = 0; i < filled && pos + 1 < sizeof(bar); i++) bar[pos++] = '#';
    pos += (size_t)snprintf(bar + pos, sizeof(bar) - pos, "\x1b[90m");
    for (unsigned i = filled; i < 20 && pos + 1 < sizeof(bar); i++) bar[pos++] = '-';
    snprintf(bar + pos, sizeof(bar) - pos, "\x1b[0m");

    snprintf(
      out, out_len,
      "\x1b[36m%s\x1b[0m [%s] %3u%% %s/%s%s",
      label, bar, pct, received_buf, total_buf,
      failed ? " \x1b[31mfailed\x1b[0m" : done ? " \x1b[32mdone\x1b[0m" : ""
    );
  } else snprintf(
    out, out_len,
    "\x1b[36m%s\x1b[0m %s%s",
    label, received_buf,
    failed ? " \x1b[31mfailed\x1b[0m" : done ? " \x1b[32mdone\x1b[0m" : ""
  );
}

static void sandbox_multi_progress_render(sandbox_multi_progress_t *progress, bool force) {
  if (!progress || !progress->enabled || progress->rows == 0) return;
  uint64_t now = progress_now_ns();
  if (!force && now != 0 && progress->prev_refresh_ns != 0 && now - progress->prev_refresh_ns < progress->refresh_rate_ns) return;

  if (progress->rendered) fprintf(progress->terminal, "\x1b[%zuF", progress->rows);
  for (size_t i = 0; i < progress->rows; i++) {
    fprintf(progress->terminal, "\x1b[2K  %s\n", progress->lines[i]);
  }
  fflush(progress->terminal);
  progress->rendered = true;
  progress->prev_refresh_ns = now;
}

static void sandbox_multi_progress_clear(sandbox_multi_progress_t *progress) {
  if (!progress || !progress->enabled || !progress->rendered || progress->rows == 0) return;
  fprintf(progress->terminal, "\x1b[%zuF\x1b[0J", progress->rows);
  fflush(progress->terminal);
  progress->rendered = false;
}

static void sandbox_multi_progress_start(sandbox_multi_progress_t *progress, size_t rows) {
  memset(progress, 0, sizeof(*progress));
  progress->terminal = stderr;
  progress->rows = rows > 2 ? 2 : rows;
  progress->refresh_rate_ns = 50 * 1000000ULL;
  progress->enabled = sandbox_progress_enabled() && progress_detect_ansi(progress->terminal);
}

static void sandbox_multi_progress_update(
  sandbox_multi_progress_t *progress,
  size_t row,
  const char *label,
  uint64_t received,
  uint64_t total,
  bool done,
  bool failed
) {
  if (!progress || !progress->enabled || row >= progress->rows) return;
  sandbox_multi_progress_line(progress->lines[row], sizeof(progress->lines[row]), label, received, total, done, failed);
  sandbox_multi_progress_render(progress, done || failed);
}

static void sandbox_http_response_cb(
  ant_http_request_t *req,
  const ant_http_response_t *resp,
  void *user_data
) {
  (void)req;
  sandbox_http_ctx_t *ctx = user_data;
  ctx->status = resp ? resp->status : 0;
  const char *len = resp ? sandbox_http_header(resp->headers, "content-length") : NULL;
  uint64_t content_length = sandbox_parse_u64(len);
  if (content_length > 0 && !ctx->gzip) ctx->content_length = content_length;
  if (ctx->gzip && resp) {
    const char *uncompressed = sandbox_http_header(resp->headers, "x-ant-uncompressed-size");
    uint64_t uncompressed_size = sandbox_parse_u64(uncompressed);
    if (uncompressed_size > 0) {
      ctx->validate_uncompressed_size = true;
      ctx->expected_uncompressed_size = uncompressed_size;
      ctx->content_length = uncompressed_size;
    }
  }
  if (ctx->multi_progress && ctx->label) {
    sandbox_multi_progress_update(ctx->multi_progress, ctx->multi_index, ctx->label, ctx->received, ctx->content_length, false, false);
  }
}

static void sandbox_http_write_output(sandbox_http_ctx_t *ctx, const uint8_t *chunk, size_t len) {
  if (len == 0 || ctx->rc != 0) return;
  if (ctx->file) {
    if (fwrite(chunk, 1, len, ctx->file) != len) {
      sandbox_http_fail(ctx, -EIO, "failed writing download");
      return;
    }
  } else {
    if (len > SIZE_MAX - ctx->len - 1) {
      sandbox_http_fail(ctx, -EOVERFLOW, "response too large");
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
        sandbox_http_fail(ctx, -ENOMEM, "out of memory");
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
}

static void sandbox_http_body_gzip(sandbox_http_ctx_t *ctx, const uint8_t *chunk, size_t len) {
  if (ctx->gzip_done) {
    sandbox_http_fail(ctx, -EIO, "gzip data continued after stream end");
    return;
  }
  if (!ctx->zlib_init) {
    memset(&ctx->zstrm, 0, sizeof(ctx->zstrm));
    int ret = inflateInit2(&ctx->zstrm, 15 + 16);
    if (ret != Z_OK) {
      sandbox_http_fail(ctx, -EIO, "gzip inflate init failed");
      return;
    }
    ctx->zlib_init = true;
  }

  unsigned char out[65536];
  while (len > 0 && ctx->rc == 0) {
    uInt avail = len > UINT_MAX ? UINT_MAX : (uInt)len;
    ctx->zstrm.next_in = (Bytef *)chunk;
    ctx->zstrm.avail_in = avail;

    do {
      ctx->zstrm.next_out = out;
      ctx->zstrm.avail_out = (uInt)sizeof(out);
      int ret = inflate(&ctx->zstrm, Z_NO_FLUSH);
      size_t have = sizeof(out) - ctx->zstrm.avail_out;
      sandbox_http_write_output(ctx, out, have);
      if (ctx->rc != 0) return;
      if (ret == Z_STREAM_END) {
        ctx->gzip_done = true;
        break;
      }
      if (ret != Z_OK) {
        sandbox_http_fail(ctx, -EIO, "gzip inflate failed");
        return;
      }
    } while (ctx->zstrm.avail_out == 0);

    size_t consumed = (size_t)avail - ctx->zstrm.avail_in;
    chunk += consumed;
    len -= consumed;
    if (ctx->gzip_done) {
      if (ctx->zstrm.avail_in != 0 || len != 0) {
        sandbox_http_fail(ctx, -EIO, "gzip data continued after stream end");
      }
      return;
    }
    if (consumed == 0) {
      sandbox_http_fail(ctx, -EIO, "gzip inflate made no progress");
      return;
    }
  }
}

static void sandbox_http_body_cb(
  ant_http_request_t *req,
  const uint8_t *chunk,
  size_t len,
  void *user_data
) {
  (void)req;
  sandbox_http_ctx_t *ctx = user_data;
  if (ctx->rc != 0 || len == 0) return;

  if (ctx->gzip) {
    if (len > UINT64_MAX - ctx->compressed_received) {
      sandbox_http_fail(ctx, -EOVERFLOW, "gzip download too large");
      return;
    }
    ctx->compressed_received += (uint64_t)len;
    sandbox_http_body_gzip(ctx, chunk, len);
  } else sandbox_http_write_output(ctx, chunk, len);
  if (ctx->rc != 0) return;

  if (ctx->multi_progress && ctx->label) {
    sandbox_multi_progress_update(ctx->multi_progress, ctx->multi_index, ctx->label, ctx->received, ctx->content_length, false, false);
  } else if (ctx->progress && ctx->label) {
    char msg[128];
    sandbox_progress_message(msg, sizeof(msg), ctx->label, ctx->received, ctx->content_length);
    progress_update(ctx->progress, msg);
  }
}

static void sandbox_http_complete_cb(
  ant_http_request_t *req,
  ant_http_result_t result,
  int error_code,
  const char *error_message,
  void *user_data
) {
  (void)req;
  sandbox_http_ctx_t *ctx = user_data;
  ctx->completed = true;
  if (ctx->rc == 0 && result != ANT_HTTP_RESULT_OK) {
    ctx->rc = error_code < 0 ? error_code : -EIO;
    snprintf(ctx->error, sizeof(ctx->error), "%s", error_message ? error_message : "network error");
  }
  if (ctx->rc == 0 && (ctx->status < 200 || ctx->status >= 300)) {
    ctx->rc = -EIO;
    snprintf(ctx->error, sizeof(ctx->error), "HTTP %d", ctx->status);
  }
  if (ctx->rc == 0 && ctx->gzip && !ctx->gzip_done) {
    ctx->rc = -EIO;
    snprintf(ctx->error, sizeof(ctx->error), "gzip stream ended early");
  }
  if (ctx->rc == 0 && ctx->gzip && ctx->validate_uncompressed_size && ctx->received != ctx->expected_uncompressed_size) {
    ctx->rc = -EIO;
    snprintf(
      ctx->error,
      sizeof(ctx->error),
      "gzip size mismatch: expected %llu bytes, got %llu",
      (unsigned long long)ctx->expected_uncompressed_size,
      (unsigned long long)ctx->received
    );
  }
  if (ctx->rc == 0 && ctx->gzip && ctx->validate_gzip_size && ctx->compressed_received != ctx->expected_gzip_size) {
    ctx->rc = -EIO;
    snprintf(
      ctx->error,
      sizeof(ctx->error),
      "gzip download size mismatch: expected %llu bytes, got %llu",
      (unsigned long long)ctx->expected_gzip_size,
      (unsigned long long)ctx->compressed_received
    );
  }
  if (ctx->zlib_init) {
    inflateEnd(&ctx->zstrm);
    ctx->zlib_init = false;
  }
  if (ctx->multi_progress && ctx->label) sandbox_multi_progress_update(
    ctx->multi_progress,
    ctx->multi_index,
    ctx->label,
    ctx->received,
    ctx->content_length,
    ctx->rc == 0,
    ctx->rc != 0
  );
}

static int sandbox_http_get(
  const char *url,
  FILE *file,
  const char *label,
  uint64_t expected_size,
  progress_t *progress,
  char **body_out,
  size_t *body_len_out,
  char *err,
  size_t err_len
) {
  uv_loop_t loop;
  sandbox_http_ctx_t ctx = {.file = file, .label = label, .progress = progress, .content_length = expected_size};
  ant_http_request_options_t options = {.method = "GET", .url = url};
  ant_http_request_t *req = NULL;
  int rc = uv_loop_init(&loop);
  if (body_out) *body_out = NULL;
  if (body_len_out) *body_len_out = 0;
  if (rc != 0) {
    sandbox_asset_error(err, err_len, "failed to initialize network loop: %s", uv_strerror(rc));
    return rc;
  }

  rc = ant_http_request_start(
    &loop, &options,
    sandbox_http_response_cb,
    sandbox_http_body_cb,
    sandbox_http_complete_cb,
    &ctx, &req
  );
  ctx.request = req;
  if (rc == 0) uv_run(&loop, UV_RUN_DEFAULT);
  (void)uv_loop_close(&loop);

  if (rc != 0) {
    free(ctx.data);
    sandbox_asset_error(err, err_len, "failed to request %s: %s", url, uv_strerror(rc));
    return rc;
  }
  if (ctx.rc != 0) {
    free(ctx.data);
    sandbox_asset_error(err, err_len, "failed to download %s: %s", url, ctx.error[0] ? ctx.error : "network error");
    return ctx.rc;
  }

  if (body_out) {
    *body_out = ctx.data;
    if (body_len_out) *body_len_out = ctx.len;
  } else free(ctx.data);

  return 0;
}

static const char *sandbox_json_string(yyjson_val *obj, const char *key) {
  yyjson_val *val = obj && yyjson_is_obj(obj) ? yyjson_obj_get(obj, key) : NULL;
  return val && yyjson_is_str(val) ? yyjson_get_str(val) : NULL;
}

static bool sandbox_json_bool(yyjson_val *obj, const char *key) {
  yyjson_val *val = obj && yyjson_is_obj(obj) ? yyjson_obj_get(obj, key) : NULL;
  return val && yyjson_is_bool(val) && yyjson_get_bool(val);
}

static uint64_t sandbox_json_uint(yyjson_val *obj, const char *key) {
  yyjson_val *val = obj && yyjson_is_obj(obj) ? yyjson_obj_get(obj, key) : NULL;
  if (!val) return 0;
  if (yyjson_is_uint(val)) return yyjson_get_uint(val);
  if (yyjson_is_sint(val)) {
    int64_t n = yyjson_get_sint(val);
    return n > 0 ? (uint64_t)n : 0;
  }
  return 0;
}

static yyjson_val *sandbox_manifest_find(yyjson_val *root, const char *array_key, const char *field, const char *value) {
  yyjson_val *arr = root && yyjson_is_obj(root) ? yyjson_obj_get(root, array_key) : NULL;
  if (!arr || !yyjson_is_arr(arr)) return NULL;

  size_t idx = 0, max = 0;
  yyjson_val *entry = NULL;
  yyjson_arr_foreach(arr, idx, max, entry) {
    if (!yyjson_is_obj(entry) || !sandbox_json_bool(entry, "available")) continue;
    const char *candidate = sandbox_json_string(entry, field);
    if (candidate && strcmp(candidate, value) == 0) return entry;
  }
  return NULL;
}

static int sandbox_manifest_copy_string(
  char *out,
  size_t out_len,
  yyjson_val *obj,
  const char *key,
  const char *name,
  char *err,
  size_t err_len
) {
  const char *value = sandbox_json_string(obj, key);
  if (!value || !value[0]) {
    sandbox_asset_error(err, err_len, "manifest entry for %s is missing %s", name, key);
    return -EINVAL;
  }

  int written = snprintf(out, out_len, "%s", value);
  return written < 0 || (size_t)written >= out_len ? -ENAMETOOLONG : 0;
}

static int sandbox_manifest_copy_optional_string(
  char *out,
  size_t out_len,
  yyjson_val *obj,
  const char *key
) {
  const char *value = sandbox_json_string(obj, key);
  if (!value || !value[0]) {
    if (out_len > 0) out[0] = '\0';
    return 0;
  }

  int written = snprintf(out, out_len, "%s", value);
  return written < 0 || (size_t)written >= out_len ? -ENAMETOOLONG : 0;
}

static uint64_t sandbox_manifest_artifact_size(yyjson_val *entry) {
  yyjson_val *artifact = entry && yyjson_is_obj(entry) ? yyjson_obj_get(entry, "artifact") : NULL;
  return sandbox_json_uint(artifact, "size_in_bytes");
}

static uint64_t sandbox_manifest_gzip_size(yyjson_val *entry) {
  return sandbox_json_uint(entry, "gzip_size_in_bytes");
}

static int sandbox_manifest_url(char *out, size_t out_len) {
  const char *base = getenv("ANT_SANDBOX_MANIFEST_URL");
  const char *branch = getenv("ANT_SANDBOX_MANIFEST_BRANCH");

  if (!base || !base[0]) base = ANT_SANDBOX_MANIFEST_URL;
  if (!branch || !branch[0]) {
    int written = snprintf(out, out_len, "%s", base);
    return written < 0 || (size_t)written >= out_len ? -ENAMETOOLONG : 0;
  }

  int written = snprintf(out, out_len, "%s%cbranch=", base, strchr(base, '?') ? '&' : '?');
  if (written < 0 || (size_t)written >= out_len) return -ENAMETOOLONG;
  
  size_t pos = (size_t)written;
  static const char hex[] = "0123456789ABCDEF";
  
  for (const unsigned char *p = (const unsigned char *)branch; *p; p++) {
    bool safe = 
      (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
      (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
      *p == '.' || *p == '/';
    if (safe) {
      if (pos + 1 >= out_len) return -ENAMETOOLONG;
      out[pos++] = (char)*p;
    } else {
      if (pos + 3 >= out_len) return -ENAMETOOLONG;
      out[pos++] = '%';
      out[pos++] = hex[*p >> 4];
      out[pos++] = hex[*p & 15u];
    }
  }
  out[pos] = '\0';
  return 0;
}

static int sandbox_manifest_select(
  const char *json,
  size_t json_len,
  sandbox_manifest_selection_t *selection,
  char *err,
  size_t err_len
) {
  yyjson_doc *doc = yyjson_read(json, json_len, 0);
  if (!doc) {
    sandbox_asset_error(err, err_len, "manifest response was not valid JSON");
    return -EINVAL;
  }

  int rc = 0;
  yyjson_val *root = yyjson_doc_get_root(doc);
  char ant_target[64];
  int written = snprintf(ant_target, sizeof(ant_target), "linux-%s-musl", ant_sandbox_cache_arch());
  if (written < 0 || (size_t)written >= sizeof(ant_target)) {
    yyjson_doc_free(doc);
    return -ENAMETOOLONG;
  }

  yyjson_val *ant = sandbox_manifest_find(root, "ant", "target", ant_target);
  yyjson_val *sandbox = sandbox_manifest_find(root, "sandbox", "arch", ant_sandbox_cache_arch());
  yyjson_val *kernel = sandbox_manifest_find(root, "kernel", "arch", ant_sandbox_cache_arch());

  if (!ant) {
    sandbox_asset_error(err, err_len, "manifest is missing Ant target %s", ant_target);
    rc = -ENOENT;
    goto done;
  }
  if (!sandbox) {
    sandbox_asset_error(err, err_len, "manifest is missing sandbox image for %s", ant_sandbox_cache_arch());
    rc = -ENOENT;
    goto done;
  }
  if (!kernel) {
    sandbox_asset_error(err, err_len, "manifest is missing sandbox kernel for %s", ant_sandbox_cache_arch());
    rc = -ENOENT;
    goto done;
  }

  yyjson_val *ant_source = yyjson_obj_get(ant, "source");
  yyjson_val *sandbox_source = yyjson_obj_get(sandbox, "source");
  yyjson_val *kernel_source = yyjson_obj_get(kernel, "source");
  const char *ant_revision = sandbox_json_string(ant, "revision");

  if (!ant_revision || strcmp(ant_revision, ANT_GIT_LONGHASH) != 0) {
    sandbox_asset_error(err, err_len,
      "manifest Ant revision mismatch: current=%s manifest=%s",
      ANT_GIT_LONGHASH, ant_revision ? ant_revision : "(missing)"
    );
    rc = -EINVAL;
    goto done;
  }

  uint64_t ant_run = sandbox_json_uint(ant_source, "run_id");
  uint64_t sandbox_run = sandbox_json_uint(sandbox_source, "run_id");
  uint64_t kernel_run = sandbox_json_uint(kernel_source, "run_id");
  
  if (!ant_run || sandbox_run != ant_run || kernel_run != ant_run) {
    sandbox_asset_error(err, err_len,
      "manifest sandbox build mismatch: ant=%llu sandbox=%llu kernel=%llu",
      (unsigned long long)ant_run,
      (unsigned long long)sandbox_run,
      (unsigned long long)kernel_run
    );
    rc = -EINVAL;
    goto done;
  }

  const char *sandbox_revision = sandbox_json_string(sandbox, "revision");
  const char *kernel_revision = sandbox_json_string(kernel, "revision");

  if (!sandbox_revision || strcmp(sandbox_revision, ant_revision) != 0) {
    sandbox_asset_error(err, err_len, "manifest sandbox commit mismatch");
    rc = -EINVAL;
    goto done;
  }

  if (!kernel_revision || strcmp(kernel_revision, ant_revision) != 0) {
    sandbox_asset_error(err, err_len, "manifest kernel commit mismatch");
    rc = -EINVAL;
    goto done;
  }

  rc = sandbox_manifest_copy_string(selection->sandbox_url, sizeof(selection->sandbox_url), sandbox, "download_url", "sandbox", err, err_len);
  if (rc != 0) goto done;
  
  rc = sandbox_manifest_copy_string(selection->kernel_url, sizeof(selection->kernel_url), kernel, "download_url", "kernel", err, err_len);
  if (rc != 0) goto done;
  
  rc = sandbox_manifest_copy_optional_string(selection->sandbox_gzip_url, sizeof(selection->sandbox_gzip_url), sandbox, "gzip_url");
  if (rc != 0) goto done;
  
  rc = sandbox_manifest_copy_optional_string(selection->kernel_gzip_url, sizeof(selection->kernel_gzip_url), kernel, "gzip_url");
  if (rc != 0) goto done;

  selection->sandbox_size = sandbox_manifest_artifact_size(sandbox);
  selection->kernel_size = sandbox_manifest_artifact_size(kernel);
  selection->sandbox_gzip_size = sandbox_manifest_gzip_size(sandbox);
  selection->kernel_gzip_size = sandbox_manifest_gzip_size(kernel);

done:
  yyjson_doc_free(doc);
  return rc;
}

typedef struct {
  sandbox_http_ctx_t http;
  const sandbox_download_t *download;
  FILE *file;
  char tmp_path[4096];
} sandbox_download_ctx_t;

static int sandbox_download_files(
  const sandbox_download_t *downloads,
  size_t download_count,
  char *err,
  size_t err_len
) {
  sandbox_download_ctx_t contexts[2] = {0};
  size_t count = 0;
  int rc = 0;

  for (size_t i = 0; i < download_count; i++) {
    if (sandbox_file_exists(downloads[i].path)) continue;
    if (count >= sizeof(contexts) / sizeof(contexts[0])) return -EOVERFLOW;

    sandbox_download_ctx_t *ctx = &contexts[count];
    ctx->download = &downloads[i];
    int written = snprintf(ctx->tmp_path, sizeof(ctx->tmp_path), "%s.tmp", downloads[i].path);
    if (written < 0 || (size_t)written >= sizeof(ctx->tmp_path)) return -ENAMETOOLONG;

    ctx->file = fopen(ctx->tmp_path, "wb");
    if (!ctx->file) {
      sandbox_asset_error(err, err_len, "failed to open %s: %s", ctx->tmp_path, strerror(errno));
      rc = -errno;
      goto cleanup_unstarted;
    }

    ctx->http.file = ctx->file;
    ctx->http.label = downloads[i].kind;
    ctx->http.gzip = downloads[i].gzip;
    ctx->http.content_length = downloads[i].expected_size;
    ctx->http.expected_uncompressed_size = downloads[i].expected_size;
    ctx->http.validate_uncompressed_size = downloads[i].gzip && downloads[i].expected_size > 0;
    ctx->http.expected_gzip_size = downloads[i].expected_gzip_size;
    ctx->http.validate_gzip_size = downloads[i].gzip && downloads[i].expected_gzip_size > 0;
    ctx->http.multi_index = count;
    count++;
  }

  if (count == 0) return 0;

  uv_loop_t loop;
  rc = uv_loop_init(&loop);
  if (rc != 0) {
    sandbox_asset_error(err, err_len, "failed to initialize network loop: %s", uv_strerror(rc));
    goto cleanup_unstarted;
  }

  sandbox_multi_progress_t progress;
  sandbox_multi_progress_start(&progress, count);
  if (progress.enabled) {
    for (size_t i = 0; i < count; i++) {
      sandbox_multi_progress_line(
        progress.lines[i],
        sizeof(progress.lines[i]),
        contexts[i].download->kind,
        0,
        contexts[i].download->expected_size,
        false,
        false
      );
      contexts[i].http.multi_progress = &progress;
    }
    sandbox_multi_progress_render(&progress, true);
  }

  size_t started = 0;
  for (size_t i = 0; i < count; i++) {
    sandbox_download_ctx_t *ctx = &contexts[i];
    ant_http_request_options_t options = {.method = "GET", .url = ctx->download->url};
    ant_http_request_t *req = NULL;
    rc = ant_http_request_start(
      &loop, &options,
      sandbox_http_response_cb,
      sandbox_http_body_cb,
      sandbox_http_complete_cb,
      &ctx->http, &req
    );
    ctx->http.request = req;
    if (rc != 0) {
      ctx->http.rc = rc;
      snprintf(ctx->http.error, sizeof(ctx->http.error), "failed to request %s: %s", ctx->download->url, uv_strerror(rc));
      sandbox_multi_progress_update(&progress, i, ctx->download->kind, 0, ctx->download->expected_size, false, true);
      break;
    }
    started++;
  }

  if (rc != 0) {
    for (size_t i = 0; i < started; i++) {
      if (contexts[i].http.request) ant_http_request_cancel(contexts[i].http.request);
    }
  }

  uv_run(&loop, UV_RUN_DEFAULT);
  (void)uv_loop_close(&loop);

  int result = 0;
  for (size_t i = 0; i < count; i++) {
    sandbox_download_ctx_t *ctx = &contexts[i];

    if (ctx->http.rc != 0 && result == 0) {
      result = ctx->http.rc;
      sandbox_asset_error(
        err, err_len,
        "failed to download %s: %s",
        ctx->download->url,
        ctx->http.error[0] ? ctx->http.error : "network error"
      );
    }

    if (fflush(ctx->file) != 0 && ctx->http.rc == 0 && result == 0) {
      sandbox_asset_error(err, err_len, "failed to flush %s: %s", ctx->tmp_path, strerror(errno));
      result = -errno;
    }
    if (fclose(ctx->file) != 0 && ctx->http.rc == 0 && result == 0) {
      sandbox_asset_error(err, err_len, "failed to close %s: %s", ctx->tmp_path, strerror(errno));
      result = -errno;
    }
    ctx->file = NULL;

    if (ctx->http.gzip && ctx->http.validate_uncompressed_size && ctx->http.rc == 0 && result == 0) {
      struct stat st;
      if (stat(ctx->tmp_path, &st) != 0) {
        sandbox_asset_error(err, err_len, "failed to stat %s: %s", ctx->tmp_path, strerror(errno));
        result = -errno;
      } else if (st.st_size < 0 || (uint64_t)st.st_size != ctx->http.expected_uncompressed_size) {
        sandbox_asset_error(
          err,
          err_len,
          "gzip file size mismatch: expected %llu bytes, got %llu",
          (unsigned long long)ctx->http.expected_uncompressed_size,
          (unsigned long long)(st.st_size < 0 ? 0 : (uint64_t)st.st_size)
        );
        result = -EIO;
      }
    }

    if (ctx->http.rc != 0 || result != 0) {
      remove(ctx->tmp_path);
      continue;
    }

    if (rename(ctx->tmp_path, ctx->download->path) != 0) {
      if (sandbox_file_exists(ctx->download->path)) {
        remove(ctx->tmp_path);
        continue;
      }
      sandbox_asset_error(err, err_len, "failed to install %s: %s", ctx->download->path, strerror(errno));
      remove(ctx->tmp_path);
      result = -errno;
    }
  }

  if (result == 0) sandbox_multi_progress_clear(&progress);
  return result;

cleanup_unstarted:
  for (size_t i = 0; i < count; i++) {
    if (contexts[i].file) fclose(contexts[i].file);
    if (contexts[i].tmp_path[0]) remove(contexts[i].tmp_path);
  }
  return rc;
}

static int sandbox_assets_download_missing_direct(
  const char *image_path,
  const char *kernel_path,
  char *err,
  size_t err_len
) {
  char manifest_url[2048];
  int rc = sandbox_manifest_url(manifest_url, sizeof(manifest_url));
  if (rc != 0) return rc;

  progress_t progress;
  progress_t *progress_ptr = NULL;
  if (sandbox_progress_enabled()) {
    progress_start(&progress, "Fetching sandbox manifest");
    progress_ptr = &progress;
  }

  char *manifest = NULL;
  size_t manifest_len = 0;
  rc = sandbox_http_get(manifest_url, NULL, NULL, 0, progress_ptr, &manifest, &manifest_len, err, err_len);
  if (progress_ptr) progress_stop(progress_ptr);
  if (rc != 0) return rc;

  sandbox_manifest_selection_t selection = {0};
  rc = sandbox_manifest_select(manifest, manifest_len, &selection, err, err_len);
  free(manifest);
  if (rc != 0) return rc;

  bool sandbox_gzip = selection.sandbox_gzip_url[0] != '\0';
  bool kernel_gzip = selection.kernel_gzip_url[0] != '\0';
  sandbox_download_t downloads[] = {
    {
      .kind = "sandbox image",
      .path = image_path,
      .url = sandbox_gzip ? selection.sandbox_gzip_url : selection.sandbox_url,
      .gzip = sandbox_gzip,
      .expected_size = selection.sandbox_size,
      .expected_gzip_size = selection.sandbox_gzip_size,
    },
    {
      .kind = "sandbox kernel",
      .path = kernel_path,
      .url = kernel_gzip ? selection.kernel_gzip_url : selection.kernel_url,
      .gzip = kernel_gzip,
      .expected_size = selection.kernel_size,
      .expected_gzip_size = selection.kernel_gzip_size,
    },
  };

  return sandbox_download_files(downloads, sizeof(downloads) / sizeof(downloads[0]), err, err_len);
}

int ant_sandbox_assets_download_missing(
  const char *image_path,
  const char *kernel_path,
  char *err,
  size_t err_len
) {
#ifdef _WIN32
  return sandbox_assets_download_missing_direct(image_path, kernel_path, err, err_len);
#else
  int result_pipe[2] = {-1, -1};
  if (pipe(result_pipe) != 0) {
    sandbox_asset_error(err, err_len, "failed to create sandbox download pipe: %s", strerror(errno));
    return -errno;
  }

  pid_t pid = fork();
  if (pid < 0) {
    int rc = -errno;
    close(result_pipe[0]);
    close(result_pipe[1]);
    sandbox_asset_error(err, err_len, "failed to start sandbox download helper: %s", strerror(errno));
    return rc;
  }

  if (pid == 0) {
    close(result_pipe[0]);
    char child_err[512] = {0};
    sandbox_download_child_result_t result = {0};
    result.rc = sandbox_assets_download_missing_direct(image_path, kernel_path, child_err, sizeof(child_err));
    if (child_err[0]) snprintf(result.error, sizeof(result.error), "%s", child_err);
    (void)sandbox_write_full(result_pipe[1], &result, sizeof(result));
    close(result_pipe[1]);
    _exit(result.rc == 0 ? 0 : 1);
  }

  close(result_pipe[1]);
  sandbox_download_child_result_t result = {0};
  int read_rc = sandbox_read_full(result_pipe[0], &result, sizeof(result));
  close(result_pipe[0]);

  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno == EINTR) continue;
    sandbox_asset_error(err, err_len, "sandbox download helper wait failed: %s", strerror(errno));
    return -errno;
  }

  if (read_rc != 0) {
    sandbox_asset_error(err, err_len, "sandbox download helper exited without a result");
    return read_rc;
  }
  if (!WIFEXITED(status)) {
    sandbox_asset_error(err, err_len, "sandbox download helper crashed");
    return -EIO;
  }
  if (result.rc != 0) {
    sandbox_asset_error(err, err_len, "%s", result.error[0] ? result.error : "sandbox download failed");
    return result.rc;
  }
  if (WEXITSTATUS(status) != 0) {
    sandbox_asset_error(err, err_len, "sandbox download helper failed");
    return -EIO;
  }
  return 0;
#endif
}
