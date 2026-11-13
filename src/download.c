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
#include <yyjson.h>
#include <zlib.h>
#include <limits.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#define ANT_MANIFEST_URL "https://manifest.antjs.org/v1/latest"
#define ANT_RELEASE_MIRROR_URL "https://github.com/theMackabu/ant/releases/latest/download/latest.json"
#define DOWNLOAD_MAX_REDIRECTS 5

typedef struct {
  FILE *terminal;
  char (*lines)[PROGRESS_MSG_SIZE];
  size_t rows;
  uint64_t prev_refresh_ns;
  bool enabled;
  bool rendered;
} download_board_t;

typedef struct {
  char *data;
  size_t len;
  size_t cap;
  FILE *file;
  uint64_t content_length;
  uint64_t received;
  uint64_t compressed_received;
  uint64_t expected_size;
  uint64_t expected_gzip_size;
  const char *label;
  progress_t *progress;
  download_board_t *board;
  size_t row;
  ant_http_request_t *request;
  const char *url;
  char *redirect;
  z_stream zstrm;
  int status;
  int rc;
  bool gzip;
  bool gzip_done;
  bool zlib_init;
  bool completed;
  char error[256];
} download_ctx_t;

static constexpr uint64_t DOWNLOAD_BOARD_REFRESH_NS = 50 * 1000000ull;

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

static void download_board_render(download_board_t *board, bool force) {
  if (!board->enabled) return;
  uint64_t now = progress_now_ns();
  bool recent = now != 0 && board->prev_refresh_ns != 0 && now - board->prev_refresh_ns < DOWNLOAD_BOARD_REFRESH_NS;
  if (!force && recent) return;

  if (board->rendered) fprintf(board->terminal, "\x1b[%zuF", board->rows);
  for (size_t i = 0; i < board->rows; i++) fprintf(board->terminal, "\x1b[2K  %s\n", board->lines[i]);
  fflush(board->terminal);
  board->rendered = true;
  board->prev_refresh_ns = now;
}

static void download_board_clear(download_board_t *board) {
  if (!board->enabled || !board->rendered) return;
  fprintf(board->terminal, "\x1b[%zuF\x1b[0J", board->rows);
  fflush(board->terminal);
  board->rendered = false;
}

static void download_board_update(download_ctx_t *ctx) {
  download_board_t *board = ctx->board;
  if (!board->enabled || ctx->row >= board->rows) return;

  bool failed = ctx->rc != 0;
  bool done = ctx->completed && !failed && !ctx->redirect;
  char msg[PROGRESS_MSG_SIZE - 32];
  download_progress_message(msg, sizeof(msg), ctx->label, ctx->received, ctx->content_length, true);
  snprintf(
    board->lines[ctx->row], sizeof(board->lines[ctx->row]), "%s%s", msg,
    failed ? " \x1b[31mfailed\x1b[0m" : done ? " \x1b[32mdone\x1b[0m" : ""
  );
  download_board_render(board, done || failed);
}

static void download_update_progress(download_ctx_t *ctx) {
  if (!ctx || !ctx->label) return;
  if (ctx->board) {
    download_board_update(ctx);
    return;
  }
  if (!ctx->progress) return;
  char msg[160];
  download_progress_message(msg, sizeof(msg), ctx->label, ctx->received, ctx->content_length, ctx->progress->supports_ansi);
  progress_update(ctx->progress, msg);
}

static bool download_is_redirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static char *download_resolve_redirect(const char *base, const char *location) {
  url_state_t next;
  char *href = NULL;
  if (parse_url_to_state(location, base, &next) == 0 && next.protocol && (
    strcmp(next.protocol, "https:") == 0 ||
    (strcmp(next.protocol, "http:") == 0 && strncmp(base, "http:", 5) == 0)
  )) href = strdup(next.href);
  url_state_clear(&next);
  return href;
}

static void download_response_cb(ant_http_request_t *, const ant_http_response_t *resp, void *user_data) {
  download_ctx_t *ctx = user_data;
  ctx->status = resp ? resp->status : 0;
  if (resp && download_is_redirect(ctx->status)) {
    const char *location = download_header(resp->headers, "location");
    ctx->redirect = location ? download_resolve_redirect(ctx->url, location) : NULL;
    if (!ctx->redirect) download_fail(ctx, -EPROTO, "invalid redirect");
    return;
  }

  const char *length_header = ctx->gzip ? "x-ant-uncompressed-size" : "content-length";
  uint64_t length = download_parse_u64(resp ? download_header(resp->headers, length_header) : NULL);
  
  if (length > 0) ctx->content_length = length;
  if (length > 0 && ctx->gzip) ctx->expected_size = length;
  
  download_update_progress(ctx);
}

static void download_write(download_ctx_t *ctx, const uint8_t *chunk, size_t len) {
  if (len == 0 || ctx->rc != 0) return;

  if (ctx->file) {
    if (fwrite(chunk, 1, len, ctx->file) != len) {
      download_fail(ctx, -EIO, "failed writing download");
      return;
    }
    ctx->len += len;
    ctx->received += len;
    return;
  }

  if (len > SIZE_MAX - ctx->len - 1) {
    download_fail(ctx, -EOVERFLOW, "response too large");
    return;
  }

  size_t need = ctx->len + len + 1;
  if (need > ctx->cap) {
    size_t next = ctx->cap ? ctx->cap : 16384u;
    while (next < need) next = next > SIZE_MAX / 2u ? need : next * 2u;
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
  ctx->len += len;
  ctx->received += len;
}

static void download_inflate(download_ctx_t *ctx, const uint8_t *chunk, size_t len) {
  if (ctx->gzip_done) {
    download_fail(ctx, -EIO, "gzip data continued after stream end");
    return;
  }

  if (!ctx->zlib_init) {
    memset(&ctx->zstrm, 0, sizeof(ctx->zstrm));
    if (inflateInit2(&ctx->zstrm, 15 + 16) != Z_OK) {
      download_fail(ctx, -EIO, "gzip inflate init failed");
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
      download_write(ctx, out, sizeof(out) - ctx->zstrm.avail_out);
      if (ctx->rc != 0) return;
      if (ret == Z_STREAM_END) {
        ctx->gzip_done = true;
        break;
      }
      if (ret != Z_OK) {
        download_fail(ctx, -EIO, "gzip inflate failed");
        return;
      }
    } while (ctx->zstrm.avail_out == 0);

    size_t consumed = (size_t)avail - ctx->zstrm.avail_in;
    chunk += consumed;
    len -= consumed;

    if (ctx->gzip_done) {
      if (len != 0) download_fail(ctx, -EIO, "gzip data continued after stream end");
      return;
    }
    if (consumed == 0) {
      download_fail(ctx, -EIO, "gzip inflate made no progress");
      return;
    }
  }
}

static void download_body_cb(ant_http_request_t *, const uint8_t *chunk, size_t len, void *user_data) {
  download_ctx_t *ctx = user_data;
  if (ctx->rc != 0 || ctx->redirect || len == 0) return;

  if (ctx->gzip) {
    ctx->compressed_received += (uint64_t)len;
    download_inflate(ctx, chunk, len);
  } else download_write(ctx, chunk, len);

  if (ctx->rc == 0) download_update_progress(ctx);
}

static void download_size_mismatch(download_ctx_t *ctx, const char *what, uint64_t expected, uint64_t got) {
  ctx->rc = -EIO;
  snprintf(
    ctx->error, sizeof(ctx->error), "%s size mismatch: expected %llu bytes, got %llu",
    what, (unsigned long long)expected, (unsigned long long)got
  );
}

static void download_check_gzip(download_ctx_t *ctx) {
  if (!ctx->gzip_done) {
    ctx->rc = -EIO;
    snprintf(ctx->error, sizeof(ctx->error), "gzip stream ended early");
    return;
  }
  if (ctx->expected_size > 0 && ctx->received != ctx->expected_size) {
    download_size_mismatch(ctx, "gzip", ctx->expected_size, ctx->received);
    return;
  }
  if (ctx->expected_gzip_size > 0 && ctx->compressed_received != ctx->expected_gzip_size) {
    download_size_mismatch(ctx, "gzip download", ctx->expected_gzip_size, ctx->compressed_received);
  }
}

static void download_complete_cb(
  ant_http_request_t *, ant_http_result_t result,
  int error_code, const char *error_message, void *user_data
) {
  download_ctx_t *ctx = user_data;
  ctx->completed = true;
  ctx->request = NULL;

  if (ctx->zlib_init) {
    inflateEnd(&ctx->zstrm);
    ctx->zlib_init = false;
  }

  if (ctx->redirect) return;
  if (ctx->rc == 0 && result != ANT_HTTP_RESULT_OK) {
    ctx->rc = error_code < 0 ? error_code : -EIO;
    snprintf(ctx->error, sizeof(ctx->error), "%s", error_message ? error_message : "network error");
  }
  if (ctx->rc == 0 && (ctx->status < 200 || ctx->status >= 300)) {
    ctx->rc = -EIO;
    snprintf(ctx->error, sizeof(ctx->error), "HTTP %d", ctx->status);
  }
  if (ctx->rc == 0 && ctx->gzip) download_check_gzip(ctx);
  if (ctx->board) download_board_update(ctx);
}

int ant_download_get(
  const char *url, FILE *file, const char *label, 
  progress_t *progress, char **body_out, 
  size_t *body_len_out, char *err, size_t err_len
) {
  uv_loop_t loop;
  download_ctx_t ctx = {.file = file, .label = label, .progress = progress};
  char *owned_url = NULL;
  int rc = uv_loop_init(&loop);
  if (body_out) *body_out = NULL;
  if (body_len_out) *body_len_out = 0;
  if (rc != 0) {
    snprintf(err, err_len, "failed to initialize network loop: %s", uv_strerror(rc));
    return rc;
  }

  for (int hops = 0;; hops++) {
    ant_http_request_options_t options = {.method = "GET", .url = url};
    ant_http_request_t *req = NULL;
    ctx.url = url;
    
    rc = ant_http_request_start(&loop, &options, download_response_cb, download_body_cb, download_complete_cb, &ctx, &req);
    ctx.request = req;
    
    if (rc == 0) uv_run(&loop, UV_RUN_DEFAULT);
    ctx.request = NULL;
    
    if (rc != 0 || !ctx.redirect) break;
    if (hops == DOWNLOAD_MAX_REDIRECTS) {
      download_fail(&ctx, -ELOOP, "too many redirects");
      break;
    }
    
    free(owned_url);
    url = owned_url = ctx.redirect;
    ctx.redirect = NULL;
    ctx.status = 0;
    ctx.content_length = 0;
  }
  
  (void)uv_loop_close(&loop);
  free(ctx.redirect);

  if (rc != 0) {
    free(ctx.data);
    snprintf(err, err_len, "failed to request %s: %s", url, uv_strerror(rc));
    free(owned_url);
    return rc;
  }
  if (ctx.rc != 0) {
    free(ctx.data);
    snprintf(err, err_len, "failed to download %s: %s", url, ctx.error[0] ? ctx.error : "network error");
    free(owned_url);
    return ctx.rc;
  }
  free(owned_url);

  if (body_out) {
    *body_out = ctx.data;
    if (body_len_out) *body_len_out = ctx.len;
  } else free(ctx.data);
  return 0;
}

typedef struct {
  download_ctx_t http;
  const ant_download_file_t *spec;
  char *tmp_path;
  char *owned_url;
  int hops;
  bool pending;
} download_job_t;

static bool download_file_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int download_job_open(download_job_t *job, const ant_download_file_t *spec, char *err, size_t err_len) {
  size_t tmp_len = strlen(spec->path) + sizeof(".tmp");
  job->tmp_path = try_oom(tmp_len);
  snprintf(job->tmp_path, tmp_len, "%s.tmp", spec->path);

  job->http.file = fopen(job->tmp_path, "wb");
  if (!job->http.file) {
    int rc = -errno;
    snprintf(err, err_len, "failed to open %s: %s", job->tmp_path, strerror(errno));
    return rc;
  }

  job->spec = spec;
  job->pending = true;
  job->http.url = spec->url;
  job->http.label = spec->label;
  job->http.gzip = spec->gzip;
  job->http.content_length = spec->expected_size;
  job->http.expected_size = spec->gzip ? spec->expected_size : 0;
  job->http.expected_gzip_size = spec->gzip ? spec->expected_gzip_size : 0;
  return 0;
}

static void download_job_follow(download_job_t *job) {
  download_ctx_t *ctx = &job->http;
  if (!ctx->redirect || ctx->rc != 0) return;

  if (job->hops++ == DOWNLOAD_MAX_REDIRECTS) {
    download_fail(ctx, -ELOOP, "too many redirects");
    return;
  }

  free(job->owned_url);
  ctx->url = job->owned_url = ctx->redirect;
  ctx->redirect = NULL;
  ctx->status = 0;
  ctx->completed = false;
  job->pending = true;
}

static bool download_jobs_start(uv_loop_t *loop, download_job_t *jobs, size_t count) {
  bool started = false;
  for (size_t i = 0; i < count; i++) {
    download_job_t *job = &jobs[i];
    if (!job->pending) continue;
    job->pending = false;
    if (job->http.rc != 0) continue;

    ant_http_request_options_t options = {.method = "GET", .url = job->http.url};
    int rc = ant_http_request_start(
      loop, &options, download_response_cb, download_body_cb, download_complete_cb,
      &job->http, &job->http.request
    );
    if (rc == 0) {
      started = true;
      continue;
    }

    job->http.rc = rc;
    job->http.completed = true;
    snprintf(job->http.error, sizeof(job->http.error), "failed to request: %s", uv_strerror(rc));
    for (size_t j = 0; j < count; j++) download_fail(&jobs[j].http, -ECANCELED, "canceled");
    download_update_progress(&job->http);
  }
  return started;
}

static int download_job_finish(download_job_t *job, int result, char *err, size_t err_len) {
  download_ctx_t *ctx = &job->http;
  if (!job->tmp_path) return result;

  int rc = ctx->rc;
  if (rc != 0 && result == 0) snprintf(
    err, err_len, "failed to download %s: %s",
    job->spec->url, ctx->error[0] ? ctx->error : "network error"
  );

  if (ctx->file && fclose(ctx->file) != 0 && rc == 0) {
    rc = -errno;
    if (result == 0) snprintf(err, err_len, "failed to write %s: %s", job->tmp_path, strerror(errno));
  }
  ctx->file = NULL;

  bool installed = rc == 0 && result == 0 && rename(job->tmp_path, job->spec->path) == 0;
  if (rc == 0 && result == 0 && !installed && !download_file_exists(job->spec->path)) {
    rc = -errno;
    snprintf(err, err_len, "failed to install %s: %s", job->spec->path, strerror(errno));
  }
  if (!installed) remove(job->tmp_path);

  free(job->tmp_path);
  free(job->owned_url);
  free(ctx->redirect);
  return result != 0 ? result : rc;
}

int ant_download_files(const ant_download_file_t *files, size_t count, char *err, size_t err_len) {
  download_job_t *jobs = try_oom(sizeof(*jobs) * (count ? count : 1));
  memset(jobs, 0, sizeof(*jobs) * (count ? count : 1));

  download_board_t board = {.terminal = stderr};
  uv_loop_t loop;
  size_t active = 0;
  int result = 0;

  for (size_t i = 0; i < count && result == 0; i++) {
    if (download_file_exists(files[i].path)) continue;
    result = download_job_open(&jobs[i], &files[i], err, err_len);
    if (result == 0) jobs[i].http.row = active++;
  }

  bool run = result == 0 && active > 0;
  if (run && (result = uv_loop_init(&loop)) != 0) {
    snprintf(err, err_len, "failed to initialize network loop: %s", uv_strerror(result));
    run = false;
  }

  if (run) {
    board.rows = active;
    board.lines = try_oom(sizeof(*board.lines) * active);
    memset(board.lines, 0, sizeof(*board.lines) * active);
    board.enabled = PROGRESS_ISATTY(PROGRESS_FILENO(stderr)) && progress_detect_ansi(stderr);

    for (size_t i = 0; i < count; i++) {
      if (!jobs[i].tmp_path) continue;
      jobs[i].http.board = &board;
      download_board_update(&jobs[i].http);
    }
    download_board_render(&board, true);

    while (download_jobs_start(&loop, jobs, count)) {
      uv_run(&loop, UV_RUN_DEFAULT);
      for (size_t i = 0; i < count; i++) download_job_follow(&jobs[i]);
    }
    (void)uv_loop_close(&loop);
  }

  for (size_t i = 0; i < count && result == 0; i++) {
    download_ctx_t *ctx = &jobs[i].http;
    if (ctx->rc == 0 || ctx->rc == -ECANCELED) continue;
    result = ctx->rc;
    snprintf(err, err_len, "failed to download %s: %s", jobs[i].spec->url, ctx->error[0] ? ctx->error : "network error");
  }
  for (size_t i = 0; i < count; i++) result = download_job_finish(&jobs[i], result, err, err_len);
  if (result == 0) download_board_clear(&board);

  free(board.lines);
  free(jobs);
  return result;
}

const char *ant_manifest_string(yyjson_val *obj, const char *key) {
  yyjson_val *val = obj && yyjson_is_obj(obj) ? yyjson_obj_get(obj, key) : NULL;
  return val && yyjson_is_str(val) ? yyjson_get_str(val) : NULL;
}

uint64_t ant_manifest_uint(yyjson_val *obj, const char *key) {
  yyjson_val *val = obj && yyjson_is_obj(obj) ? yyjson_obj_get(obj, key) : NULL;
  return val && yyjson_is_uint(val) ? yyjson_get_uint(val) : 0;
}

bool ant_manifest_bool(yyjson_val *obj, const char *key) {
  yyjson_val *val = obj && yyjson_is_obj(obj) ? yyjson_obj_get(obj, key) : NULL;
  return val && yyjson_is_bool(val) && yyjson_get_bool(val);
}

const char *ant_manifest_url(void) {
  const char *url = getenv("ANT_MANIFEST_URL");
  return url && url[0] ? url : ANT_MANIFEST_URL;
}

const char *ant_release_mirror_url(void) {
  const char *url = getenv("ANT_RELEASE_MIRROR_URL");
  if (url) return url[0] ? url : NULL;
  const char *manifest = getenv("ANT_MANIFEST_URL");
  return manifest && manifest[0] ? NULL : ANT_RELEASE_MIRROR_URL;
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
