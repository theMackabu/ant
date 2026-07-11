#include <compat.h> // IWYU pragma: keep

#include "cli/registry.h"

#include "base64.h"
#include "modules/http.h"
#include "modules/io.h"

#include <argtable3.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/evp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <uv.h>
#include <yyjson.h>
#include <zlib.h>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#define ANT_LAND_SITE_URL "https://ants.land"
#define ANT_LAND_REGISTRY_URL "https://npm.ants.land"
#define NPM_REGISTRY_URL "https://registry.npmjs.org"

typedef struct {
  uint8_t *data;
  size_t len;
  size_t cap;
} byte_buf_t;

typedef struct {
  char *data;
  size_t len;
  size_t cap;
  int status;
  int rc;
  ant_http_request_t *request;
  char error[256];
} registry_http_ctx_t;

typedef struct {
  char *json;
  size_t json_len;
  uint8_t *tarball;
  char *name;
  char *version;
  char *filename;
  size_t tarball_len;
  size_t file_count;
  char shasum[41];
  char integrity[104];
} publish_payload_t;

static void byte_buf_free(byte_buf_t *buf) {
  if (!buf) return;
  free(buf->data);
  buf->data = NULL;
  buf->len = 0;
  buf->cap = 0;
}

static int byte_buf_reserve(byte_buf_t *buf, size_t extra) {
  if (extra > SIZE_MAX - buf->len) return -1;
  size_t need = buf->len + extra;
  if (need <= buf->cap) return 0;
  size_t cap = buf->cap ? buf->cap : 4096;
  while (cap < need) {
    if (cap > SIZE_MAX / 2) {
      cap = need;
      break;
    }
    cap *= 2;
  }
  uint8_t *next = realloc(buf->data, cap);
  if (!next) return -1;
  buf->data = next;
  buf->cap = cap;
  return 0;
}

static int byte_buf_append(byte_buf_t *buf, const void *data, size_t len) {
  if (byte_buf_reserve(buf, len) != 0) return -1;
  memcpy(buf->data + buf->len, data, len);
  buf->len += len;
  return 0;
}

static int byte_buf_append_zeros(byte_buf_t *buf, size_t len) {
  if (byte_buf_reserve(buf, len) != 0) return -1;
  memset(buf->data + buf->len, 0, len);
  buf->len += len;
  return 0;
}

static char *str_dup_range(const char *s, size_t len) {
  char *out = malloc(len + 1);
  if (!out) return NULL;
  memcpy(out, s, len);
  out[len] = '\0';
  return out;
}

static char *str_dup(const char *s) {
  return s ? str_dup_range(s, strlen(s)) : NULL;
}

static int registry_lstat(const char *path, struct stat *st) {
#ifdef _WIN32
  return stat(path, st);
#else
  return lstat(path, st);
#endif
}

static char *path_join2(const char *a, const char *b) {
  size_t alen = strlen(a);
  size_t blen = strlen(b);
  bool slash = alen > 0 && a[alen - 1] == '/';
  char *out = malloc(alen + (slash ? 0 : 1) + blen + 1);
  if (!out) return NULL;
  memcpy(out, a, alen);
  size_t pos = alen;
  if (!slash) out[pos++] = '/';
  memcpy(out + pos, b, blen);
  out[pos + blen] = '\0';
  return out;
}

static int read_file(const char *path, char **out, size_t *out_len) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return -1;
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return -1;
  }
  long n = ftell(fp);
  if (n < 0 || fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    return -1;
  }
  char *buf = malloc((size_t)n + 1);
  if (!buf) {
    fclose(fp);
    errno = ENOMEM;
    return -1;
  }
  size_t got = fread(buf, 1, (size_t)n, fp);
  fclose(fp);
  if (got != (size_t)n) {
    free(buf);
    errno = EIO;
    return -1;
  }
  buf[got] = '\0';
  *out = buf;
  if (out_len) *out_len = got;
  return 0;
}

static char *home_path(const char *leaf) {
  const char *home = getenv("HOME");
  if (!home || !home[0]) return NULL;
  return path_join2(home, leaf);
}

static void registry_http_response_cb(ant_http_request_t *req, const ant_http_response_t *resp, void *user_data) {
  (void)req;
  registry_http_ctx_t *ctx = user_data;
  ctx->status = resp ? resp->status : 0;
}

static void registry_http_body_cb(ant_http_request_t *req, const uint8_t *chunk, size_t len, void *user_data) {
  (void)req;
  registry_http_ctx_t *ctx = user_data;
  if (ctx->rc != 0) return;
  if (len > SIZE_MAX - ctx->len - 1) {
    ctx->rc = -ENOMEM;
    snprintf(ctx->error, sizeof(ctx->error), "response is too large");
    if (ctx->request) ant_http_request_cancel(ctx->request);
    return;
  }
  size_t need = ctx->len + len + 1;
  if (need > ctx->cap) {
    size_t cap = ctx->cap ? ctx->cap : 4096;
    while (cap < need) cap *= 2;
    char *next = realloc(ctx->data, cap);
    if (!next) {
      ctx->rc = -ENOMEM;
      snprintf(ctx->error, sizeof(ctx->error), "out of memory");
      if (ctx->request) ant_http_request_cancel(ctx->request);
      return;
    }
    ctx->data = next;
    ctx->cap = cap;
  }
  memcpy(ctx->data + ctx->len, chunk, len);
  ctx->len += len;
  ctx->data[ctx->len] = '\0';
}

static void registry_http_complete_cb(ant_http_request_t *req, ant_http_result_t result, int error_code, const char *error_message, void *user_data) {
  (void)req;
  registry_http_ctx_t *ctx = user_data;
  if (ctx->rc == 0 && result != ANT_HTTP_RESULT_OK) {
    ctx->rc = error_code < 0 ? error_code : -EIO;
    snprintf(ctx->error, sizeof(ctx->error), "%s", error_message ? error_message : "network error");
  }
}

static int registry_http_request(const char *method, const char *url, const void *body, size_t body_len, const char *content_type, const char *auth_token, char **body_out, size_t *body_len_out, int *status_out, char *err, size_t err_len) {
  registry_http_ctx_t ctx = {0};
  uv_loop_t loop;
  ant_http_header_t auth_header = {0};
  ant_http_header_t type_header = {0};
  const ant_http_header_t *headers = NULL;

  if (body && content_type) {
    type_header.name = (char *)"content-type";
    type_header.value = (char *)content_type;
    headers = &type_header;
  }
  char auth_value[4096];
  if (auth_token && auth_token[0]) {
    snprintf(auth_value, sizeof(auth_value), "Bearer %s", auth_token);
    auth_header.name = (char *)"authorization";
    auth_header.value = auth_value;
    auth_header.next = (ant_http_header_t *)headers;
    headers = &auth_header;
  }

  ant_http_request_options_t options = {
    .method = method,
    .url = url,
    .headers = headers,
    .body = body,
    .body_len = body ? body_len : 0,
  };
  ant_http_request_t *req = NULL;
  int rc = uv_loop_init(&loop);
  if (body_out) *body_out = NULL;
  if (body_len_out) *body_len_out = 0;
  if (status_out) *status_out = 0;
  if (rc != 0) {
    snprintf(err, err_len, "failed to initialize network loop: %s", uv_strerror(rc));
    return rc;
  }

  rc = ant_http_request_start(&loop, &options, registry_http_response_cb, registry_http_body_cb, registry_http_complete_cb, &ctx, &req);
  ctx.request = req;
  if (rc == 0) uv_run(&loop, UV_RUN_DEFAULT);
  (void)uv_loop_close(&loop);

  if (rc != 0) {
    snprintf(err, err_len, "failed to request %s: %s", url, uv_strerror(rc));
    free(ctx.data);
    return rc;
  }
  if (ctx.rc != 0) {
    snprintf(err, err_len, "failed to request %s: %s", url, ctx.error[0] ? ctx.error : "network error");
    free(ctx.data);
    return ctx.rc;
  }

  if (status_out) *status_out = ctx.status;
  if (body_out) {
    *body_out = ctx.data;
    if (body_len_out) *body_len_out = ctx.len;
  } else free(ctx.data);
  return 0;
}

static int registry_http_json(const char *method, const char *url, const char *body, const char *auth_token, char **body_out, size_t *body_len_out, int *status_out, char *err, size_t err_len) {
  return registry_http_request(method, url, body, body ? strlen(body) : 0, "application/json", auth_token, body_out, body_len_out, status_out, err, err_len);
}

static char *url_join(const char *base, const char *path) {
  size_t blen = strlen(base);
  while (blen > 0 && base[blen - 1] == '/') blen--;
  size_t plen = strlen(path);
  bool path_slash = plen > 0 && path[0] == '/';
  char *out = malloc(blen + (path_slash ? 0 : 1) + plen + 1);
  if (!out) return NULL;
  memcpy(out, base, blen);
  size_t pos = blen;
  if (!path_slash) out[pos++] = '/';
  memcpy(out + pos, path, plen);
  out[pos + plen] = '\0';
  return out;
}

static char *url_host(const char *url) {
  const char *p = strstr(url, "://");
  p = p ? p + 3 : url;
  const char *end = p;
  while (*end && *end != '/' && *end != ':' && *end != '?' && *end != '#') end++;
  return str_dup_range(p, (size_t)(end - p));
}

static char *url_encode_package(const char *name) {
  byte_buf_t out = {0};
  for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
    bool keep = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~' || *p == '@';
    if (keep) {
      if (byte_buf_append(&out, p, 1) != 0) goto fail;
    } else {
      char enc[4];
      snprintf(enc, sizeof(enc), "%%%02x", *p);
      if (byte_buf_append(&out, enc, 3) != 0) goto fail;
    }
  }
  if (byte_buf_append(&out, "", 1) != 0) goto fail;
  return (char *)out.data;
fail:
  byte_buf_free(&out);
  return NULL;
}

static void open_browser(const char *url) {
#ifdef _WIN32
  (void)_spawnlp(_P_NOWAIT, "explorer.exe", "explorer.exe", url, (char *)NULL);
#else
  pid_t pid = fork();
  if (pid != 0) return;
#if defined(__APPLE__)
  execlp("open", "open", url, (char *)NULL);
#else
  execlp("xdg-open", "xdg-open", url, (char *)NULL);
#endif
  _exit(127);
#endif
}

static int save_npmrc_token(const char *host, const char *token) {
  char *npmrc = home_path(".npmrc");
  if (!npmrc) {
    fprintf(stderr, "Error: HOME is not set; cannot write ~/.npmrc\n");
    return -1;
  }

  char prefix[512];
  snprintf(prefix, sizeof(prefix), "//%s/:_authToken=", host);
  char *content = NULL;
  size_t content_len = 0;
  (void)read_file(npmrc, &content, &content_len);

  byte_buf_t out = {0};
  const char *p = content ? content : "";
  size_t prefix_len = strlen(prefix);
  while (*p) {
    const char *line = p;
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - line) : strlen(line);
    if (!(len >= prefix_len && strncmp(line, prefix, prefix_len) == 0) && len > 0) {
      if (byte_buf_append(&out, line, len) != 0 || byte_buf_append(&out, "\n", 1) != 0) goto fail;
    }
    p = nl ? nl + 1 : line + len;
  }
  if (byte_buf_append(&out, prefix, prefix_len) != 0 || byte_buf_append(&out, token, strlen(token)) != 0 || byte_buf_append(&out, "\n", 1) != 0) goto fail;

  FILE *fp = fopen(npmrc, "wb");
  if (!fp) goto fail;
  bool ok = fwrite(out.data, 1, out.len, fp) == out.len;
  if (fclose(fp) != 0) ok = false;
  free(content);
  byte_buf_free(&out);
  free(npmrc);
  return ok ? 0 : -1;

fail:
  free(content);
  byte_buf_free(&out);
  free(npmrc);
  return -1;
}

static char *read_npmrc_token_for_host(const char *host) {
  char *npmrc = home_path(".npmrc");
  if (!npmrc) return NULL;
  char *content = NULL;
  if (read_file(npmrc, &content, NULL) != 0) {
    free(npmrc);
    return NULL;
  }
  free(npmrc);

  char prefix[512];
  snprintf(prefix, sizeof(prefix), "//%s/:_authToken=", host);
  size_t prefix_len = strlen(prefix);
  char *result = NULL;
  char *save = NULL;
  for (char *line = strtok_r(content, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    while (*line == ' ' || *line == '\t') line++;
    if (strncmp(line, prefix, prefix_len) == 0) {
      char *value = line + prefix_len;
      char *end = value + strlen(value);
      while (end > value && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
      result = str_dup(value);
      break;
    }
  }
  free(content);
  return result;
}

static const char *json_string(yyjson_val *obj, const char *key) {
  yyjson_val *val = obj && yyjson_is_obj(obj) ? yyjson_obj_get(obj, key) : NULL;
  return val && yyjson_is_str(val) ? yyjson_get_str(val) : NULL;
}

int pkg_cmd_login(int argc, char **argv) {
  struct arg_lit *help = arg_lit0("h", "help", "display help");
  struct arg_end *end = arg_end(5);
  void *argtable[] = { help, end };
  int nerrors = arg_parse(argc, argv, argtable);

  int exitcode = EXIT_SUCCESS;
  if (help->count > 0) {
    printf("Usage: ant login\n\n");
    printf("Authenticate this device with ants.land.\n");
    printf("Environment:\n");
    printf("  ANTS_URL       Site URL. Defaults to https://ants.land.\n");
    arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
    return EXIT_SUCCESS;
  }
  if (nerrors > 0) {
    arg_print_errors(stdout, end, "ant login");
    arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
    return EXIT_FAILURE;
  }

  const char *site = getenv("ANTS_URL");
  if (!site || !site[0]) site = ANT_LAND_SITE_URL;
  char *start_url = url_join(site, "/api/cli/start");
  char *poll_url = url_join(site, "/api/cli/poll");
  char *start_body = NULL;
  if (!start_url || !poll_url) {
    fprintf(stderr, "Error: out of memory\n");
    exitcode = EXIT_FAILURE;
    goto done;
  }

  char err[256] = {0};
  int status = 0;
  if (registry_http_json("POST", start_url, NULL, NULL, &start_body, NULL, &status, err, sizeof(err)) != 0 || status < 200 || status >= 300) {
    fprintf(stderr, "Error: failed to start login: %s%s%d\n", err[0] ? err : "HTTP ", err[0] ? "" : "", err[0] ? 0 : status);
    exitcode = EXIT_FAILURE;
    goto done;
  }

  yyjson_doc *start_doc = yyjson_read(start_body, strlen(start_body), 0);
  yyjson_val *start_root = start_doc ? yyjson_doc_get_root(start_doc) : NULL;
  const char *code = json_string(start_root, "code");
  const char *verify_url = json_string(start_root, "verifyUrl");
  yyjson_val *interval_val = start_root ? yyjson_obj_get(start_root, "interval") : NULL;
  yyjson_val *expires_val = start_root ? yyjson_obj_get(start_root, "expiresIn") : NULL;
  uint64_t interval = interval_val && yyjson_is_uint(interval_val) ? yyjson_get_uint(interval_val) : 2;
  uint64_t expires = expires_val && yyjson_is_uint(expires_val) ? yyjson_get_uint(expires_val) : 600;
  if (!code || !verify_url) {
    fprintf(stderr, "Error: login endpoint returned an invalid response\n");
    yyjson_doc_free(start_doc);
    exitcode = EXIT_FAILURE;
    goto done;
  }

  printf("To authorize this device, visit:\n");
  printf("  %s%s%s\n\n", C_CYAN, verify_url, C_RESET);
  printf("%sOpening your browser... waiting for approval.%s\n", C_DIM, C_RESET);
  open_browser(verify_url);

  char poll_body[512];
  snprintf(poll_body, sizeof(poll_body), "{\"code\":\"%s\"}", code);
  time_t deadline = time(NULL) + (time_t)expires;
  while (time(NULL) < deadline) {
    sleep(interval > 0 && interval < 60 ? (unsigned)interval : 2);
    char *poll_resp = NULL;
    status = 0;
    if (registry_http_json("POST", poll_url, poll_body, NULL, &poll_resp, NULL, &status, err, sizeof(err)) != 0 || status < 200 || status >= 300) {
      fprintf(stderr, "Error: failed to poll login: %s%s%d\n", err[0] ? err : "HTTP ", err[0] ? "" : "", err[0] ? 0 : status);
      free(poll_resp);
      exitcode = EXIT_FAILURE;
      break;
    }

    yyjson_doc *poll_doc = yyjson_read(poll_resp, strlen(poll_resp), 0);
    yyjson_val *poll_root = poll_doc ? yyjson_doc_get_root(poll_doc) : NULL;
    const char *poll_status = json_string(poll_root, "status");
    if (poll_status && strcmp(poll_status, "done") == 0) {
      const char *token = json_string(poll_root, "token");
      const char *email = json_string(poll_root, "email");
      if (!token) {
        fprintf(stderr, "Error: login completed without a token\n");
        exitcode = EXIT_FAILURE;
      } else if (save_npmrc_token("npm.ants.land", token) != 0) {
        fprintf(stderr, "Error: failed to save token to ~/.npmrc\n");
        exitcode = EXIT_FAILURE;
      } else {
        if (email) printf("%sLogged in%s as %s. Token saved to ~/.npmrc\n", C_GREEN, C_RESET, email);
        else printf("%sLogged in%s. Token saved to ~/.npmrc\n", C_GREEN, C_RESET);
      }
      yyjson_doc_free(poll_doc);
      free(poll_resp);
      break;
    }
    if (poll_status && strcmp(poll_status, "expired") == 0) {
      fprintf(stderr, "Error: login request expired. Run ant login again.\n");
      yyjson_doc_free(poll_doc);
      free(poll_resp);
      exitcode = EXIT_FAILURE;
      break;
    }
    yyjson_doc_free(poll_doc);
    free(poll_resp);
  }
  if (exitcode == EXIT_SUCCESS && time(NULL) >= deadline) {
    fprintf(stderr, "Error: login timed out.\n");
    exitcode = EXIT_FAILURE;
  }
  yyjson_doc_free(start_doc);

done:
  free(start_body);
  free(start_url);
  free(poll_url);
  arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
  return exitcode;
}

static bool should_skip_entry(const char *rel) {
  const char *slash = strchr(rel, '/');
  size_t first_len = slash ? (size_t)(slash - rel) : strlen(rel);
  return (first_len == 4 && strncmp(rel, ".git", 4) == 0) ||
         (first_len == 4 && strncmp(rel, ".ant", 4) == 0) ||
         (first_len == 12 && strncmp(rel, "node_modules", 12) == 0);
}

static int tar_split_name(const char *path, char *name, size_t name_len, char *prefix, size_t prefix_len) {
  size_t len = strlen(path);
  if (len < name_len) {
    strcpy(name, path);
    prefix[0] = '\0';
    return 0;
  }

  const char *split = path + len;
  while (split > path) {
    split--;
    while (split > path && *split != '/') split--;
    if (*split != '/') break;
    size_t plen = (size_t)(split - path);
    size_t nlen = len - plen - 1;
    if (plen < prefix_len && nlen < name_len) {
      memcpy(prefix, path, plen);
      prefix[plen] = '\0';
      memcpy(name, split + 1, nlen);
      name[nlen] = '\0';
      return 0;
    }
  }
  return -1;
}

static void tar_octal(char *dst, size_t len, uint64_t value) {
  snprintf(dst, len, "%0*llo", (int)len - 1, (unsigned long long)value);
}

static int tar_append_file_header(byte_buf_t *tar, const char *path, const struct stat *st) {
  uint8_t h[512];
  memset(h, 0, sizeof(h));
  char name[100];
  char prefix[155];
  if (tar_split_name(path, name, sizeof(name), prefix, sizeof(prefix)) != 0) {
    fprintf(stderr, "Error: package file path is too long for tar: %s\n", path);
    return -1;
  }
  memcpy(h, name, strlen(name));
  tar_octal((char *)h + 100, 8, (uint64_t)(st->st_mode & 0777));
  tar_octal((char *)h + 108, 8, 0);
  tar_octal((char *)h + 116, 8, 0);
  tar_octal((char *)h + 124, 12, (uint64_t)st->st_size);
  tar_octal((char *)h + 136, 12, (uint64_t)st->st_mtime);
  memset(h + 148, ' ', 8);
  h[156] = '0';
  memcpy(h + 257, "ustar", 5);
  memcpy(h + 263, "00", 2);
  if (prefix[0]) memcpy(h + 345, prefix, strlen(prefix));
  unsigned sum = 0;
  for (size_t i = 0; i < sizeof(h); i++) sum += h[i];
  snprintf((char *)h + 148, 8, "%06o", sum);
  h[154] = '\0';
  h[155] = ' ';
  return byte_buf_append(tar, h, sizeof(h));
}

static int tar_append_file(byte_buf_t *tar, const char *full_path, const char *rel_path, size_t *file_count) {
  struct stat st;
  if (registry_lstat(full_path, &st) != 0) return -1;
  if (!S_ISREG(st.st_mode)) return 0;

  char tar_path[PATH_MAX];
  if (snprintf(tar_path, sizeof(tar_path), "package/%s", rel_path) >= (int)sizeof(tar_path)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  if (tar_append_file_header(tar, tar_path, &st) != 0) return -1;

  FILE *fp = fopen(full_path, "rb");
  if (!fp) return -1;
  uint8_t tmp[16384];
  size_t got;
  while ((got = fread(tmp, 1, sizeof(tmp), fp)) > 0) {
    if (byte_buf_append(tar, tmp, got) != 0) {
      fclose(fp);
      errno = ENOMEM;
      return -1;
    }
  }
  if (ferror(fp)) {
    fclose(fp);
    errno = EIO;
    return -1;
  }
  fclose(fp);
  size_t pad = (512 - (tar->len % 512)) % 512;
  if (byte_buf_append_zeros(tar, pad) != 0) return -1;
  (*file_count)++;
  return 0;
}

static int tar_walk(byte_buf_t *tar, const char *base, const char *rel, size_t *file_count) {
  if (should_skip_entry(rel)) return 0;
  char *full = rel[0] ? path_join2(base, rel) : str_dup(base);
  if (!full) return -1;

  struct stat st;
  if (registry_lstat(full, &st) != 0) {
    free(full);
    return -1;
  }
  if (S_ISREG(st.st_mode)) {
    int rc = tar_append_file(tar, full, rel, file_count);
    free(full);
    return rc;
  }
  if (!S_ISDIR(st.st_mode)) {
    free(full);
    return 0;
  }

  DIR *dir = opendir(full);
  if (!dir) {
    free(full);
    return -1;
  }
  struct dirent *ent;
  while ((ent = readdir(dir))) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
    char *child = rel[0] ? path_join2(rel, ent->d_name) : str_dup(ent->d_name);
    if (!child) {
      closedir(dir);
      free(full);
      errno = ENOMEM;
      return -1;
    }
    int rc = tar_walk(tar, base, child, file_count);
    free(child);
    if (rc != 0) {
      closedir(dir);
      free(full);
      return rc;
    }
  }
  closedir(dir);
  free(full);
  return 0;
}

static int gzip_buffer(const uint8_t *data, size_t len, byte_buf_t *out) {
  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  int zr = deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
  if (zr != Z_OK) return -1;
  zs.next_in = (Bytef *)data;
  zs.avail_in = (uInt)len;
  uint8_t tmp[16384];
  do {
    zs.next_out = tmp;
    zs.avail_out = sizeof(tmp);
    zr = deflate(&zs, Z_FINISH);
    if (zr != Z_OK && zr != Z_STREAM_END) {
      deflateEnd(&zs);
      return -1;
    }
    size_t have = sizeof(tmp) - zs.avail_out;
    if (have && byte_buf_append(out, tmp, have) != 0) {
      deflateEnd(&zs);
      return -1;
    }
  } while (zr != Z_STREAM_END);
  deflateEnd(&zs);
  return 0;
}

static char *unscoped_name(const char *name) {
  if (name[0] == '@') {
    const char *slash = strchr(name, '/');
    if (slash && slash[1]) return str_dup(slash + 1);
  }
  return str_dup(name);
}

static char *find_readme(void) {
  const char *names[] = {"README.md", "README", "readme.md", "Readme.md", NULL};
  for (int i = 0; names[i]; i++) {
    char *body = NULL;
    if (read_file(names[i], &body, NULL) == 0) return body;
  }
  return NULL;
}

static int build_publish_payload(publish_payload_t *payload, bool include_attachment) {
  memset(payload, 0, sizeof(*payload));
  char *pkg_json = NULL;
  size_t pkg_json_len = 0;
  if (read_file("package.json", &pkg_json, &pkg_json_len) != 0) {
    fprintf(stderr, "Error: package.json not found in current directory\n");
    return -1;
  }

  yyjson_doc *pkg_doc = yyjson_read(pkg_json, pkg_json_len, 0);
  yyjson_val *pkg_root = pkg_doc ? yyjson_doc_get_root(pkg_doc) : NULL;
  const char *name = json_string(pkg_root, "name");
  const char *version = json_string(pkg_root, "version");
  if (!pkg_root || !yyjson_is_obj(pkg_root) || !name || !version) {
    fprintf(stderr, "Error: package.json must contain string name and version fields\n");
    yyjson_doc_free(pkg_doc);
    free(pkg_json);
    return -1;
  }

  byte_buf_t tar = {0};
  if (tar_walk(&tar, ".", "", &payload->file_count) != 0 || byte_buf_append_zeros(&tar, 1024) != 0) {
    fprintf(stderr, "Error: failed to build package tarball: %s\n", strerror(errno));
    yyjson_doc_free(pkg_doc);
    free(pkg_json);
    byte_buf_free(&tar);
    return -1;
  }

  byte_buf_t gz = {0};
  if (gzip_buffer(tar.data, tar.len, &gz) != 0) {
    fprintf(stderr, "Error: failed to gzip package tarball\n");
    yyjson_doc_free(pkg_doc);
    free(pkg_json);
    byte_buf_free(&tar);
    byte_buf_free(&gz);
    return -1;
  }
  byte_buf_free(&tar);

  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  if (EVP_Digest(gz.data, gz.len, digest, &digest_len, EVP_sha1(), NULL) != 1 || digest_len != 20) {
    fprintf(stderr, "Error: failed to hash package tarball\n");
    yyjson_doc_free(pkg_doc);
    free(pkg_json);
    byte_buf_free(&gz);
    return -1;
  }
  for (size_t i = 0; i < digest_len; i++) snprintf(payload->shasum + i * 2, 3, "%02x", digest[i]);
  if (EVP_Digest(gz.data, gz.len, digest, &digest_len, EVP_sha512(), NULL) != 1 || digest_len != 64) {
    fprintf(stderr, "Error: failed to hash package tarball\n");
    yyjson_doc_free(pkg_doc);
    free(pkg_json);
    byte_buf_free(&gz);
    return -1;
  }
  size_t digest_b64_len = 0;
  char *digest_b64 = ant_base64_encode(digest, digest_len, &digest_b64_len);
  if (!digest_b64 || digest_b64_len + 8 >= sizeof(payload->integrity)) {
    fprintf(stderr, "Error: failed to encode package integrity\n");
    yyjson_doc_free(pkg_doc);
    free(pkg_json);
    byte_buf_free(&gz);
    free(digest_b64);
    return -1;
  }
  snprintf(payload->integrity, sizeof(payload->integrity), "sha512-%s", digest_b64);
  free(digest_b64);

  char *bare = unscoped_name(name);
  if (!bare) {
    fprintf(stderr, "Error: out of memory\n");
    yyjson_doc_free(pkg_doc);
    free(pkg_json);
    byte_buf_free(&gz);
    return -1;
  }
  size_t filename_len = strlen(bare) + 1 + strlen(version) + 4;
  char *filename = malloc(filename_len + 1);
  if (!filename) {
    fprintf(stderr, "Error: out of memory\n");
    yyjson_doc_free(pkg_doc);
    free(pkg_json);
    byte_buf_free(&gz);
    free(bare);
    return -1;
  }
  snprintf(filename, filename_len + 1, "%s-%s.tgz", bare, version);
  free(bare);

  size_t b64_len = 0;
  char *b64 = NULL;
  if (include_attachment) {
    b64 = ant_base64_encode(gz.data, gz.len, &b64_len);
    if (!b64) {
      fprintf(stderr, "Error: failed to encode tarball\n");
      yyjson_doc_free(pkg_doc);
      free(pkg_json);
      byte_buf_free(&gz);
      free(filename);
      return -1;
    }
  }

  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_obj_add_strcpy(doc, root, "_id", name);
  yyjson_mut_obj_add_strcpy(doc, root, "name", name);
  const char *description = json_string(pkg_root, "description");
  if (description) yyjson_mut_obj_add_strcpy(doc, root, "description", description);

  yyjson_mut_val *tags = yyjson_mut_obj_add_obj(doc, root, "dist-tags");
  yyjson_mut_obj_add_strcpy(doc, tags, "latest", version);

  yyjson_mut_val *versions = yyjson_mut_obj_add_obj(doc, root, "versions");
  yyjson_mut_val *manifest = yyjson_val_mut_copy(doc, pkg_root);
  if (!manifest) {
    fprintf(stderr, "Error: failed to build package manifest\n");
    yyjson_mut_doc_free(doc);
    yyjson_doc_free(pkg_doc);
    free(pkg_json);
    byte_buf_free(&gz);
    free(filename);
    free(b64);
    return -1;
  }
  if (!yyjson_mut_obj_get(manifest, "_id")) {
    size_t id_len = strlen(name) + 1 + strlen(version);
    char *id = malloc(id_len + 1);
    if (!id) {
      fprintf(stderr, "Error: out of memory\n");
      yyjson_mut_doc_free(doc);
      yyjson_doc_free(pkg_doc);
      free(pkg_json);
      byte_buf_free(&gz);
      free(filename);
      free(b64);
      return -1;
    }
    snprintf(id, id_len + 1, "%s@%s", name, version);
    yyjson_mut_obj_add_strcpy(doc, manifest, "_id", id);
    free(id);
  }
  if (!yyjson_mut_obj_get(manifest, "dist")) yyjson_mut_obj_add_obj(doc, manifest, "dist");
  yyjson_mut_obj_add_val(doc, versions, version, manifest);

  char *readme = find_readme();
  if (readme) yyjson_mut_obj_add_strcpy(doc, root, "readme", readme);

  if (include_attachment) {
    yyjson_mut_val *attachments = yyjson_mut_obj_add_obj(doc, root, "_attachments");
    yyjson_mut_val *attachment = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, attachment, "content_type", "application/octet-stream");
    yyjson_mut_obj_add_strncpy(doc, attachment, "data", b64, b64_len);
    yyjson_mut_obj_add_uint(doc, attachment, "length", (uint64_t)gz.len);
    yyjson_mut_obj_add_val(doc, attachments, filename, attachment);
  }

  size_t json_len = 0;
  char *json = yyjson_mut_write(doc, 0, &json_len);
  if (!json) {
    fprintf(stderr, "Error: failed to encode publish payload\n");
    yyjson_mut_doc_free(doc);
    yyjson_doc_free(pkg_doc);
    free(pkg_json);
    byte_buf_free(&gz);
    free(filename);
    free(b64);
    free(readme);
    return -1;
  }

  payload->json = json;
  payload->json_len = json_len;
  payload->name = str_dup(name);
  payload->version = str_dup(version);
  payload->filename = filename;
  payload->tarball_len = gz.len;
  if (!include_attachment) {
    payload->tarball = gz.data;
    gz.data = NULL;
    gz.len = gz.cap = 0;
  }

  yyjson_mut_doc_free(doc);
  yyjson_doc_free(pkg_doc);
  free(pkg_json);
  byte_buf_free(&gz);
  free(b64);
  free(readme);
  if (!payload->name || !payload->version) return -1;
  return 0;
}

static void publish_payload_free(publish_payload_t *payload) {
  if (!payload) return;
  free(payload->json);
  free(payload->tarball);
  free(payload->name);
  free(payload->version);
  free(payload->filename);
  memset(payload, 0, sizeof(*payload));
}

static int registry_request_expect(
  const char *method, const char *url, const void *body, size_t body_len,
  const char *content_type, const char *token, char **response,
  char *err, size_t err_len
) {
  int status = 0;
  char request_err[256] = {0};
  char *resp = NULL;
  int rc = registry_http_request(method, url, body, body_len, content_type, token, &resp, NULL, &status, request_err, sizeof(request_err));
  if (rc != 0) {
    snprintf(err, err_len, "%s", request_err[0] ? request_err : "network error");
    free(resp);
    return -1;
  }
  if (status < 200 || status >= 300) {
    snprintf(err, err_len, "HTTP %d%s%s", status, resp && resp[0] ? ": " : "", resp && resp[0] ? resp : "");
    free(resp);
    return -1;
  }
  if (response) *response = resp;
  else free(resp);
  return 0;
}

static int multipart_publish(const char *registry_url, const publish_payload_t *payload, const char *token, char *err, size_t err_len) {
  int result = -1;
  char *create_url = url_join(registry_url, "/-/v1/publish/uploads");
  char *upload_url = NULL;
  char *resp = NULL;
  yyjson_mut_doc *create_doc = NULL;
  yyjson_mut_doc *complete_doc = NULL;
  char *create_body = NULL;
  char *complete_body = NULL;
  if (!create_url) goto oom;

  create_doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *create_root = yyjson_mut_obj(create_doc);
  yyjson_mut_doc_set_root(create_doc, create_root);
  yyjson_mut_obj_add_strcpy(create_doc, create_root, "name", payload->name);
  yyjson_mut_obj_add_strcpy(create_doc, create_root, "version", payload->version);
  yyjson_mut_obj_add_uint(create_doc, create_root, "size", payload->tarball_len);
  yyjson_mut_obj_add_strcpy(create_doc, create_root, "shasum", payload->shasum);
  yyjson_mut_obj_add_strcpy(create_doc, create_root, "integrity", payload->integrity);
  size_t create_len = 0;
  create_body = yyjson_mut_write(create_doc, 0, &create_len);
  if (!create_body) goto oom;
  if (registry_request_expect("POST", create_url, create_body, create_len, "application/json", token, &resp, err, err_len) != 0) goto done;

  yyjson_doc *create_resp_doc = yyjson_read(resp, strlen(resp), 0);
  yyjson_val *create_resp_root = create_resp_doc ? yyjson_doc_get_root(create_resp_doc) : NULL;
  const char *upload_id = json_string(create_resp_root, "id");
  yyjson_val *part_size_val = create_resp_root ? yyjson_obj_get(create_resp_root, "partSize") : NULL;
  uint64_t part_size_u64 = part_size_val && yyjson_is_uint(part_size_val) ? yyjson_get_uint(part_size_val) : 0;
  if (!upload_id || part_size_u64 < 5 * 1024 * 1024 || part_size_u64 > SIZE_MAX) {
    snprintf(err, err_len, "registry returned an invalid multipart upload");
    yyjson_doc_free(create_resp_doc);
    goto done;
  }
  char *upload_path = malloc(strlen("/-/v1/publish/uploads/") + strlen(upload_id) + 1);
  if (!upload_path) {
    yyjson_doc_free(create_resp_doc);
    goto oom;
  }
  sprintf(upload_path, "/-/v1/publish/uploads/%s", upload_id);
  upload_url = url_join(registry_url, upload_path);
  free(upload_path);
  yyjson_doc_free(create_resp_doc);
  free(resp);
  resp = NULL;
  if (!upload_url) goto oom;

  complete_doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *complete_root = yyjson_mut_obj(complete_doc);
  yyjson_mut_doc_set_root(complete_doc, complete_root);
  yyjson_mut_val *parts = yyjson_mut_obj_add_arr(complete_doc, complete_root, "parts");
  size_t part_size = (size_t)part_size_u64;
  size_t offset = 0;
  unsigned part_number = 1;
  while (offset < payload->tarball_len) {
    size_t length = payload->tarball_len - offset;
    if (length > part_size) length = part_size;
    char suffix[64];
    snprintf(suffix, sizeof(suffix), "/parts/%u", part_number);
    char *part_url = url_join(upload_url, suffix);
    if (!part_url) goto oom;
    if (registry_request_expect("PUT", part_url, payload->tarball + offset, length, "application/octet-stream", token, &resp, err, err_len) != 0) {
      free(part_url);
      goto abort_upload;
    }
    free(part_url);
    yyjson_doc *part_doc = yyjson_read(resp, strlen(resp), 0);
    yyjson_val *part_root = part_doc ? yyjson_doc_get_root(part_doc) : NULL;
    const char *etag = json_string(part_root, "etag");
    if (!etag) {
      snprintf(err, err_len, "registry returned an invalid upload part");
      yyjson_doc_free(part_doc);
      goto abort_upload;
    }
    yyjson_mut_val *part = yyjson_mut_obj(complete_doc);
    yyjson_mut_obj_add_uint(complete_doc, part, "partNumber", part_number);
    yyjson_mut_obj_add_strcpy(complete_doc, part, "etag", etag);
    yyjson_mut_arr_append(parts, part);
    yyjson_doc_free(part_doc);
    free(resp);
    resp = NULL;
    offset += length;
    part_number++;
  }

  size_t complete_len = 0;
  complete_body = yyjson_mut_write(complete_doc, 0, &complete_len);
  if (!complete_body) goto oom;
  char *complete_url = url_join(upload_url, "/complete");
  if (!complete_url) goto oom;
  if (registry_request_expect("POST", complete_url, complete_body, complete_len, "application/json", token, NULL, err, err_len) != 0) {
    free(complete_url);
    goto abort_upload;
  }
  free(complete_url);

  char *finalize_url = url_join(upload_url, "/finalize");
  if (!finalize_url) goto oom;
  if (registry_request_expect("POST", finalize_url, payload->json, payload->json_len, "application/json", token, NULL, err, err_len) != 0) {
    free(finalize_url);
    goto abort_upload;
  }
  free(finalize_url);
  result = 0;
  goto done;

oom:
  snprintf(err, err_len, "out of memory");
  goto abort_upload;

abort_upload:
  if (upload_url) {
    char ignored[64] = {0};
    (void)registry_http_request("DELETE", upload_url, NULL, 0, NULL, token, NULL, NULL, NULL, ignored, sizeof(ignored));
  }

done:
  free(resp);
  free(create_url);
  free(upload_url);
  free(create_body);
  free(complete_body);
  yyjson_mut_doc_free(create_doc);
  yyjson_mut_doc_free(complete_doc);
  return result;
}

static void print_publish_help(void) {
  printf("Usage: ant publish [--land|--npm] [--registry <url>] [--dry-run]\n\n");
  printf("Publish the current package with Ant's native registry client.\n\n");
  printf("Options:\n");
  printf("  --land              Publish to the ants.land registry (default).\n");
  printf("  --npm               Publish to npm.\n");
  printf("  --registry <url>    Publish to a custom npm-compatible registry.\n");
  printf("  --dry-run           Build the publish payload without uploading.\n");
}

int pkg_cmd_publish(int argc, char **argv) {
  const char *registry_url = NULL;
  bool dry_run = false;
  bool npm = false;
  bool land = true;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      print_publish_help();
      return EXIT_SUCCESS;
    } else if (strcmp(argv[i], "--dry-run") == 0) {
      dry_run = true;
    } else if (strcmp(argv[i], "--npm") == 0) {
      npm = true;
      land = false;
      registry_url = NPM_REGISTRY_URL;
    } else if (strcmp(argv[i], "--land") == 0) {
      npm = false;
      land = true;
      registry_url = NULL;
    } else if (strcmp(argv[i], "--registry") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "Error: --registry requires a URL\n");
        return EXIT_FAILURE;
      }
      land = false;
      registry_url = argv[++i];
    } else {
      fprintf(stderr, "Error: unknown publish option: %s\n", argv[i]);
      return EXIT_FAILURE;
    }
  }

  if (!registry_url) {
    const char *env = getenv("ANTS_REGISTRY");
    registry_url = env && env[0] ? env : ANT_LAND_REGISTRY_URL;
  }

  publish_payload_t payload;
  if (build_publish_payload(&payload, !land) != 0) return EXIT_FAILURE;

  char *host = url_host(registry_url);
  char *encoded = url_encode_package(payload.name);
  char *path = encoded ? url_join("/", encoded) : NULL;
  char *url = path ? url_join(registry_url, path) : NULL;
  free(encoded);
  free(path);
  if (!host || !url) {
    fprintf(stderr, "Error: out of memory\n");
    free(host);
    free(url);
    publish_payload_free(&payload);
    return EXIT_FAILURE;
  }

  printf("Publishing %s@%s to %s\n", payload.name, payload.version, registry_url);
  printf("Packed %zu files into %s (%zu bytes)\n", payload.file_count, payload.filename, payload.tarball_len);
  if (dry_run) {
    printf("Dry run complete. No upload performed.\n");
    free(host);
    free(url);
    publish_payload_free(&payload);
    return EXIT_SUCCESS;
  }

  char *token = read_npmrc_token_for_host(host);
  if (!token || !token[0]) {
    if (npm) fprintf(stderr, "Error: no npm token found for //%s/:_authToken= in ~/.npmrc\n", host);
    else fprintf(stderr, "Error: no ants.land token found. Run ant login first.\n");
    free(token);
    free(host);
    free(url);
    publish_payload_free(&payload);
    return EXIT_FAILURE;
  }

  char err[1024] = {0};
  if (land) {
    int rc = multipart_publish(registry_url, &payload, token, err, sizeof(err));
    if (rc != 0) {
      fprintf(stderr, "Error: publish failed: %s\n", err[0] ? err : "network error");
      free(token);
      free(host);
      free(url);
      publish_payload_free(&payload);
      return EXIT_FAILURE;
    }
    printf("Published %s@%s\n", payload.name, payload.version);
    free(token);
    free(host);
    free(url);
    publish_payload_free(&payload);
    return EXIT_SUCCESS;
  }

  char *resp = NULL;
  int status = 0;
  int rc = registry_http_json("PUT", url, payload.json, token, &resp, NULL, &status, err, sizeof(err));
  if (rc != 0) {
    fprintf(stderr, "Error: publish failed: %s\n", err[0] ? err : "network error");
    free(resp);
    free(token);
    free(host);
    free(url);
    publish_payload_free(&payload);
    return EXIT_FAILURE;
  }
  if (status < 200 || status >= 300) {
    fprintf(stderr, "Error: publish failed with HTTP %d%s%s\n", status, resp && resp[0] ? ": " : "", resp && resp[0] ? resp : "");
    free(resp);
    free(token);
    free(host);
    free(url);
    publish_payload_free(&payload);
    return EXIT_FAILURE;
  }

  printf("Published %s@%s\n", payload.name, payload.version);
  free(resp);
  free(token);
  free(host);
  free(url);
  publish_payload_free(&payload);
  return EXIT_SUCCESS;
}
