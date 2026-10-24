#include <compat.h> // IWYU pragma: keep

#include "download.h"
#include "modules/http.h"
#include "modules/url.h"
#include "utils.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <uv.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#define ANT_MANIFEST_URL "https://manifest.antjs.org/v1/latest"
#define DOWNLOAD_MAX_REDIRECTS 5
#define DOWNLOAD_IDLE_TIMEOUT_MS 30000

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
  const char *url;
  char *location;
  uint64_t max_bytes;
  uv_timer_t idle;
  bool follow_redirects;
  bool redirecting;
  char error[256];
} download_ctx_t;

static void download_format_bytes(char *out, size_t out_len, uint64_t bytes) {
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

static void download_progress_message(char *out, size_t out_len, const char *label, uint64_t received, uint64_t total, bool color) {
  if (!out || out_len == 0) return;
  char got[32];
  char want[32];
  download_format_bytes(got, sizeof(got), received);
  download_format_bytes(want, sizeof(want), total);
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

static uint64_t download_parse_u64(const char *value) {
  if (!value || !value[0]) return 0;
  char *end = NULL;
  unsigned long long parsed = strtoull(value, &end, 10);
  return end && *end == '\0' ? (uint64_t)parsed : 0;
}

static const char *download_header(const ant_http_header_t *headers, const char *name) {
  for (const ant_http_header_t *hdr = headers; hdr; hdr = hdr->next) {
    if (hdr->name && hdr->value && strcasecmp(hdr->name, name) == 0) return hdr->value;
  }
  return NULL;
}

static void download_fail(download_ctx_t *ctx, int rc, const char *message) {
  if (!ctx || ctx->rc != 0) return;
  ctx->rc = rc;
  snprintf(ctx->error, sizeof(ctx->error), "%s", message ? message : "network error");
  if (ctx->request) ant_http_request_cancel(ctx->request);
}

static void download_update_progress(download_ctx_t *ctx) {
  if (!ctx || !ctx->progress || !ctx->label) return;
  char msg[160];
  download_progress_message(msg, sizeof(msg), ctx->label, ctx->received, ctx->content_length, ctx->progress->supports_ansi);
  progress_update(ctx->progress, msg);
}

static void download_idle_cb(uv_timer_t *timer) {
  download_fail(timer->data, -ETIMEDOUT, "timed out waiting for the server");
}

static void download_touch(download_ctx_t *ctx) {
  uv_timer_start(&ctx->idle, download_idle_cb, DOWNLOAD_IDLE_TIMEOUT_MS, 0);
}

static bool download_is_redirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static char *download_redirect_url(const char *base, const char *location) {
  url_state_t next = {0};
  char *href = NULL;
  bool secure = strncasecmp(base, "https://", 8) == 0;
  if (parse_url_to_state(location, base, &next) == 0 && (
    strcmp(next.protocol, "https:") == 0 || (!secure && strcmp(next.protocol, "http:") == 0)
  )) href = strndup(next.href, strcspn(next.href, "#"));
  url_state_clear(&next);
  return href;
}

static void download_response_cb(ant_http_request_t *req, const ant_http_response_t *resp, void *user_data) {
  (void)req;
  download_ctx_t *ctx = user_data;
  download_touch(ctx);
  ctx->status = resp ? resp->status : 0;
  if (ctx->follow_redirects && download_is_redirect(ctx->status)) {
    const char *location = download_header(resp->headers, "location");
    char message[sizeof(ctx->error)];
    ctx->redirecting = true;
    if (!location) download_fail(ctx, -EIO, "redirect without a Location header");
    else if (!(ctx->location = download_redirect_url(ctx->url, location))) {
      snprintf(message, sizeof(message), "refusing to follow redirect to %s", location);
      download_fail(ctx, -EPROTONOSUPPORT, message);
    }
    return;
  }
  const char *len = resp ? download_header(resp->headers, "content-length") : NULL;
  uint64_t content_length = download_parse_u64(len);
  if (content_length > 0) ctx->content_length = content_length;
  download_update_progress(ctx);
}

static void download_body_cb(ant_http_request_t *req, const uint8_t *chunk, size_t len, void *user_data) {
  (void)req;
  download_ctx_t *ctx = user_data;
  download_touch(ctx);
  if (ctx->rc != 0 || ctx->redirecting || len == 0) return;
  if (ctx->max_bytes && ctx->received + len > ctx->max_bytes) {
    download_fail(ctx, -EFBIG, "response is larger than the expected size");
    return;
  }

  if (ctx->file) {
    if (fwrite(chunk, 1, len, ctx->file) != len) {
      download_fail(ctx, -EIO, "failed writing download");
      return;
    }
  } else {
    if (len > SIZE_MAX - ctx->len - 1) {
      download_fail(ctx, -EOVERFLOW, "response too large");
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
        download_fail(ctx, -ENOMEM, "out of memory");
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
  download_update_progress(ctx);
}

static void download_complete_cb(
  ant_http_request_t *req, ant_http_result_t result, 
  int error_code, const char *error_message, void *user_data
) {
  (void)req;
  download_ctx_t *ctx = user_data;
  uv_timer_stop(&ctx->idle);
  if (ctx->rc == 0 && result != ANT_HTTP_RESULT_OK) {
    ctx->rc = error_code < 0 ? error_code : -EIO;
    snprintf(ctx->error, sizeof(ctx->error), "%s", error_message ? error_message : "network error");
  }
  if (ctx->rc == 0 && !ctx->redirecting && (ctx->status < 200 || ctx->status >= 300)) {
    ctx->rc = -EIO;
    snprintf(ctx->error, sizeof(ctx->error), "HTTP %d", ctx->status);
  }
}

int ant_download_request(
  const char *url, const ant_download_options_t *options,
  FILE *file, const char *label, progress_t *progress,
  char **body_out, size_t *body_len_out, char *err, size_t err_len
) {
  uv_loop_t loop;
  download_ctx_t ctx = {.file = file, .label = label, .progress = progress, .url = url};
  const ant_http_header_t *headers = options ? options->headers : NULL;
  char *hop_url = NULL;
  if (options) {
    ctx.follow_redirects = options->follow_redirects;
    ctx.max_bytes = options->max_bytes;
  }
  if (body_out) *body_out = NULL;
  if (body_len_out) *body_len_out = 0;
  if (ctx.follow_redirects && download_header(headers, "authorization")) {
    snprintf(err, err_len, "refusing to send Authorization across redirects");
    return -EINVAL;
  }

  int rc = uv_loop_init(&loop);
  if (rc != 0) {
    snprintf(err, err_len, "failed to initialize network loop: %s", uv_strerror(rc));
    return rc;
  }
  uv_timer_init(&loop, &ctx.idle);
  ctx.idle.data = &ctx;

  for (int hop = 0;; hop++) {
    ant_http_request_options_t req_options = {.method = "GET", .url = ctx.url, .headers = headers};
    ant_http_request_t *req = NULL;
    ctx.redirecting = false;
    rc = ant_http_request_start(&loop, &req_options, download_response_cb, download_body_cb, download_complete_cb, &ctx, &req);
    ctx.request = req;
    if (rc == 0) {
      download_touch(&ctx);
      uv_run(&loop, UV_RUN_DEFAULT);
    }
    ctx.request = NULL;
    if (rc != 0 || ctx.rc != 0 || !ctx.redirecting) break;
    if (hop >= DOWNLOAD_MAX_REDIRECTS) {
      download_fail(&ctx, -ELOOP, "too many redirects");
      break;
    }
    free(hop_url);
    ctx.url = hop_url = ctx.location;
    ctx.location = NULL;
  }
  uv_close((uv_handle_t *)&ctx.idle, NULL);
  uv_run(&loop, UV_RUN_DEFAULT);
  (void)uv_loop_close(&loop);
  free(hop_url);
  free(ctx.location);

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

int ant_download_get(
  const char *url, FILE *file, const char *label, 
  progress_t *progress, char **body_out, 
  size_t *body_len_out, char *err, size_t err_len
) {
  return ant_download_request(url, NULL, file, label, progress, body_out, body_len_out, err, err_len);
}

const char *ant_manifest_url(void) {
  const char *url = getenv("ANT_MANIFEST_URL");
  return url && url[0] ? url : ANT_MANIFEST_URL;
}

int ant_manifest_channel_url(const char *channel, char *out, size_t out_len) {
  if (!out || out_len == 0) return -EINVAL;
  const char *base = ant_manifest_url(); int written;
  if (!channel || !channel[0]) written = snprintf(out, out_len, "%s", base);
  else written = snprintf(out, out_len, "%s%cchannel=%s", base, strchr(base, '?') ? '&' : '?', channel);
  return written < 0 || (size_t)written >= out_len ? -ENAMETOOLONG : 0;
}

int ant_manifest_revision_url(const char *revision, char *out, size_t out_len) {
  if (!out || out_len == 0) return -EINVAL;
  const char *base = ant_manifest_url();
  int written;
  if (!revision || !revision[0]) written = snprintf(out, out_len, "%s", base);
  else written = snprintf(out, out_len, "%s%crevision=%s", base, strchr(base, '?') ? '&' : '?', revision);
  return written < 0 || (size_t)written >= out_len ? -ENAMETOOLONG : 0;
}

int ant_manifest_fetch(
  const char *revision, char **body_out, size_t *body_len_out,
  char *err, size_t err_len
) {
  char url[2048];
  if (ant_manifest_revision_url(revision, url, sizeof(url)) != 0) {
    snprintf(err, err_len, "manifest url is too long");
    return -ENAMETOOLONG;
  }

  int rc = ant_download_get(url, NULL, NULL, NULL, body_out, body_len_out, err, err_len);
  if (rc != 0 && revision && strstr(err, "HTTP 404")) snprintf(err, err_len, ANT_UNPUBLISHED_BUILD_HINT);
  return rc;
}

int ant_http_download_file(const char *url, FILE *file, const char *label, char *err, size_t err_len) {
  progress_t progress;
  progress_start(&progress, label);
  int rc = ant_download_get(url, file, label, &progress, NULL, NULL, err, err_len);
  progress_stop(&progress);
  return rc;
}

int ant_remove_tree(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -errno;
  if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) return unlink(path) == 0 ? 0 : -errno;

  DIR *dir = opendir(path);
  if (!dir) return -errno;
  int rc = 0;
  struct dirent *ent = NULL;

  while ((ent = readdir(dir))) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
    char child[4096];
    int written = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
    if (written < 0 || (size_t)written >= sizeof(child)) {
      rc = -ENAMETOOLONG;
      break;
    }
    int child_rc = ant_remove_tree(child);
    if (child_rc != 0 && rc == 0) rc = child_rc;
  }
  closedir(dir);
  if (rc != 0) return rc;
  return rmdir(path) == 0 ? 0 : -errno;
}

void ant_cache_prune_revisions(const char *kind, const char *keep_dirname) {
  char root[4096];
  if (!kind || !keep_dirname || ant_xdg_cache_path(root, sizeof(root), kind) != 0) return;

  DIR *dir = opendir(root);
  if (!dir) return;

  struct dirent *ent = NULL;
  while ((ent = readdir(dir))) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
    if (strcmp(ent->d_name, keep_dirname) == 0) continue;

    char child[4096];
    int written = snprintf(child, sizeof(child), "%s/%s", root, ent->d_name);
    if (written < 0 || (size_t)written >= sizeof(child)) continue;
    (void)ant_remove_tree(child);
  }

  closedir(dir);
}
