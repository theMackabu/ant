#include "utils.h"
#include "esm/remote.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <uv.h>
#include <tlsuv/tlsuv.h>
#include <tlsuv/http.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define mkdir_p(path) _mkdir(path)
#define PATH_SEP '\\'
#else
#include <sys/stat.h>
#include <sys/types.h>
#define mkdir_p(path) mkdir(path, 0755)
#define PATH_SEP '/'
#endif

typedef struct {
  char *data;
  size_t size;
  size_t capacity;
  int status_code;
  int completed;
  int failed;
  char *error_msg;
  tlsuv_http_t http_client;
  tlsuv_http_req_t *http_req;
} esm_url_fetch_t;

bool esm_is_url(const char *spec) {
  return (strncmp(spec, "http://", 7) == 0 || strncmp(spec, "https://", 8) == 0);
}

static int is_path_sep(char c) {
  return c == '/' || c == '\\';
}

static void esm_url_fetch_close_cb(tlsuv_http_t *client) {
  esm_url_fetch_t *ctx = (esm_url_fetch_t *)client->data;
  ctx->completed = 1;
}

static const char *esm_get_home_dir(void) {
#ifdef _WIN32
  const char *home = getenv("USERPROFILE");
  if (home) return home;
  
  const char *drive = getenv("HOMEDRIVE");
  const char *path = getenv("HOMEPATH");
  if (drive && path) {
    static char win_home[MAX_PATH];
    snprintf(win_home, sizeof(win_home), "%s%s", drive, path);
    return win_home;
  }
#else
  const char *home = getenv("HOME");
  if (home) return home;
#endif
  return NULL;
}

static void esm_url_fetch_body_cb(tlsuv_http_req_t *http_req, char *body, ssize_t len) {
  esm_url_fetch_t *ctx = (esm_url_fetch_t *)http_req->data;

  if (len == UV_EOF) {
    tlsuv_http_close(&ctx->http_client, esm_url_fetch_close_cb);
    return;
  }

  if (len < 0) {
    ctx->failed = 1;
    ctx->error_msg = strdup(uv_strerror((int)len));
    tlsuv_http_close(&ctx->http_client, esm_url_fetch_close_cb);
    return;
  }

  if (ctx->size + (size_t)len > ctx->capacity) {
    size_t new_cap = ctx->capacity * 2;
    while (new_cap < ctx->size + (size_t)len) new_cap *= 2;
    char *new_data = realloc(ctx->data, new_cap);
    if (!new_data) {
      ctx->failed = 1;
      ctx->error_msg = strdup("Out of memory");
      tlsuv_http_close(&ctx->http_client, esm_url_fetch_close_cb);
      return;
    }
    ctx->data = new_data;
    ctx->capacity = new_cap;
  }

  memcpy(ctx->data + ctx->size, body, (size_t)len);
  ctx->size += (size_t)len;
}

static void esm_url_fetch_resp_cb(tlsuv_http_resp_t *resp, void *data) {
  (void)data;
  esm_url_fetch_t *ctx = (esm_url_fetch_t *)resp->req->data;

  if (resp->code < 0) {
    ctx->failed = 1;
    char err_buf[256];
    snprintf(err_buf, sizeof(err_buf), "%s (code: %d)", uv_strerror(resp->code), resp->code);
    ctx->error_msg = strdup(err_buf);
    tlsuv_http_close(&ctx->http_client, esm_url_fetch_close_cb);
    return;
  }

  ctx->status_code = resp->code;
  resp->body_cb = esm_url_fetch_body_cb;
}

static char *esm_get_cache_path(const char *url) {
  const char *home = esm_get_home_dir();
  if (!home) home = ".";

  uint64_t hash = hash_key(url, strlen(url));

  size_t len = strlen(home) + 48;
  char *cache_path = malloc(len);
  if (!cache_path) return NULL;

#ifdef _WIN32
  snprintf(cache_path, len, "%s\\.ant\\esm\\%016llx", home, (unsigned long long)hash);
#else
  snprintf(cache_path, len, "%s/.ant/esm/%016llx", home, (unsigned long long)hash);
#endif
  return cache_path;
}

static char *esm_read_cache(const char *cache_path, size_t *out_len) {
  FILE *fp = fopen(cache_path, "rb");
  if (!fp) return NULL;

  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  char *content = malloc(size + 1);
  if (!content) {
    fclose(fp);
    return NULL;
  }

  fread(content, 1, size, fp);
  fclose(fp);
  content[size] = '\0';

  if (out_len) *out_len = (size_t)size;
  return content;
}

static void esm_mkdir_recursive(char *path) {
  for (char *p = path + 1; *p; p++) {
#ifdef _WIN32
    if (p == path + 2 && path[1] == ':') continue;
#endif
    if (is_path_sep(*p)) {
      *p = '\0';
      mkdir_p(path);
      *p = PATH_SEP;
    }
  }
  mkdir_p(path);
}

static void esm_write_cache(const char *cache_path, const char *url, const char *content, size_t len) {
  char *dir = strdup(cache_path);
  if (!dir) return;

  for (char *p = dir + strlen(dir) - 1; p > dir; p--) {
    if (is_path_sep(*p)) { *p = '\0'; break; }
  }

  esm_mkdir_recursive(dir);

  FILE *fp = fopen(cache_path, "wb");
  if (!fp) { free(dir); return; }
  fwrite(content, 1, len, fp);
  fclose(fp);

  size_t meta_len = strlen(dir) + 16;
  char *meta_path = malloc(meta_len);
  if (meta_path) {
    snprintf(meta_path, meta_len, "%s/metadata.bin", dir);
    FILE *mfp = fopen(meta_path, "ab");
    if (mfp) {
      uint64_t hash = hash_key(url, strlen(url));
      uint16_t url_len = (uint16_t)strlen(url);
      fwrite(&url_len, sizeof(url_len), 1, mfp);
      fwrite(url, 1, url_len, mfp);
      fwrite(&hash, sizeof(hash), 1, mfp);
      fclose(mfp);
    }
    free(meta_path);
  }

  free(dir);
}

char *esm_fetch_url(const char *url, size_t *out_len, char **out_error) {
  char *cache_path = esm_get_cache_path(url);
  if (cache_path) {
    char *cached = esm_read_cache(cache_path, out_len);
    if (cached) { free(cache_path); return cached; }
  }

  uv_loop_t *loop = uv_default_loop();
  esm_url_fetch_t ctx = {0};
  
  ctx.capacity = 16384;
  ctx.data = malloc(ctx.capacity);
  if (!ctx.data) {
    if (out_error) *out_error = strdup("Out of memory");
    return NULL;
  }

  const char *scheme_end = strstr(url, "://");
  if (!scheme_end) {
    free(ctx.data);
    if (out_error) *out_error = strdup("Invalid URL: no scheme");
    return NULL;
  }

  const char *host_start = scheme_end + 3;
  const char *path_start = strchr(host_start, '/');
  const char *at_in_host = NULL;

  for (const char *p = host_start; p < (path_start ? path_start : host_start + strlen(host_start)); p++) {
    if (*p == '@') at_in_host = p;
  }
  if (at_in_host) host_start = at_in_host + 1;

  size_t scheme_len = scheme_end - url;
  size_t host_len = path_start ? (size_t)(path_start - host_start) : strlen(host_start);
  const char *path = path_start ? path_start : "/";

  char *host_url = calloc(1, scheme_len + 3 + host_len + 1);
  snprintf(host_url, scheme_len + 3 + host_len + 1, "%.*s://%.*s", (int)scheme_len, url, (int)host_len, host_start);
  int rc = tlsuv_http_init(loop, &ctx.http_client, host_url);
  free(host_url);

  if (rc != 0) {
    free(ctx.data);
    if (out_error) *out_error = strdup("Failed to initialize HTTP client");
    return NULL;
  }

  ctx.http_client.data = &ctx;
  ctx.http_req = tlsuv_http_req(&ctx.http_client, "GET", path, esm_url_fetch_resp_cb, &ctx);

  if (!ctx.http_req) {
    free(ctx.data);
    tlsuv_http_close(&ctx.http_client, NULL);
    if (out_error) *out_error = strdup("Failed to create HTTP request");
    return NULL;
  }

  ctx.http_req->data = &ctx;
  while (!ctx.completed) uv_run(loop, UV_RUN_ONCE);

  if (ctx.failed || ctx.status_code < 200 || ctx.status_code >= 400) {
    if (out_error) {
      if (ctx.error_msg) {
        *out_error = ctx.error_msg;
        ctx.error_msg = NULL;
      } else {
        char err_buf[64];
        snprintf(err_buf, sizeof(err_buf), "HTTP error: %d", ctx.status_code);
        *out_error = strdup(err_buf);
      }
    }
    free(ctx.data);
    if (ctx.error_msg) free(ctx.error_msg);
    if (cache_path) free(cache_path);
    return NULL;
  }

  ctx.data[ctx.size] = '\0';
  if (out_len) *out_len = ctx.size;
  if (ctx.error_msg) free(ctx.error_msg);

  if (cache_path) {
    esm_write_cache(cache_path, url, ctx.data, ctx.size);
    free(cache_path);
  }

  return ctx.data;
}

char *esm_resolve_url(const char *specifier, const char *base_url) {
  if (esm_is_url(specifier)) {
    return strdup(specifier);
  }

  if (specifier[0] == '/') {
    const char *scheme_end = strstr(base_url, "://");
    if (!scheme_end) return NULL;

    const char *host_start = scheme_end + 3;
    const char *path_start = strchr(host_start, '/');

    const char *at_in_host = NULL;
    for (const char *p = host_start; p < (path_start ? path_start : host_start + strlen(host_start)); p++) {
      if (*p == '@') at_in_host = p;
    }
    if (at_in_host) host_start = at_in_host + 1;

    size_t scheme_len = scheme_end - base_url;
    size_t host_len = path_start ? (size_t)(path_start - host_start) : strlen(host_start);

    size_t len = scheme_len + 3 + host_len + strlen(specifier) + 1;
    char *result = malloc(len);
    if (!result) return NULL;

    snprintf(
      result, len, "%.*s://%.*s%s",
      (int)scheme_len, base_url,
      (int)host_len, host_start,
      specifier
   );
   
    return result;
  }

  if (specifier[0] == '.' && (specifier[1] == '/' || (specifier[1] == '.' && specifier[2] == '/'))) {
    char *base_copy = strdup(base_url);
    if (!base_copy) return NULL;

    char *last_slash = strrchr(base_copy, '/');
    char *scheme_end = strstr(base_copy, "://");
    if (scheme_end && last_slash > scheme_end + 2) {
      *last_slash = '\0';
    }

    const char *spec = specifier;
    while (strncmp(spec, "../", 3) == 0) {
      spec += 3;
      char *prev_slash = strrchr(base_copy, '/');
      if (prev_slash && prev_slash > scheme_end + 2) {
        *prev_slash = '\0';
      }
    }
    
    if (strncmp(spec, "./", 2) == 0) spec += 2;
    size_t len = strlen(base_copy) + strlen(spec) + 2;
    char *result = malloc(len);
    if (!result) { free(base_copy); return NULL; }

    snprintf(result, len, "%s/%s", base_copy, spec);
    free(base_copy);
    
    return result;
  }

  return strdup(specifier);
}

char *esm_resolve(const char *specifier, const char *base_path, char FILE_RESOLVER) {
  if (esm_is_url(specifier) || esm_is_url(base_path)) return esm_resolve_url(specifier, base_path);
  return file_resolver(specifier, base_path);
}
