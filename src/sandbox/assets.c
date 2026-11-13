#include <compat.h> // IWYU pragma: keep

#include "assets.h"
#include "debug.h"
#include "download.h"
#include "progress.h"
#include "sandbox/host.h"

#include <yyjson.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define ANT_SANDBOX_MANIFEST_URL "https://manifest.antjs.org/v1/latest"

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

static yyjson_val *sandbox_manifest_find(yyjson_val *root, const char *array_key, const char *field, const char *value) {
  yyjson_val *arr = root && yyjson_is_obj(root) ? yyjson_obj_get(root, array_key) : NULL;
  if (!arr || !yyjson_is_arr(arr)) return NULL;

  size_t idx = 0, max = 0;
  yyjson_val *entry = NULL;
  yyjson_arr_foreach(arr, idx, max, entry) {
    if (!yyjson_is_obj(entry) || !ant_manifest_bool(entry, "available")) continue;
    const char *candidate = ant_manifest_string(entry, field);
    if (candidate && strcmp(candidate, value) == 0) return entry;
  }
  return NULL;
}

static int sandbox_manifest_copy_string(
  char *out, size_t out_len, yyjson_val *obj,
  const char *key, const char *name, char *err, size_t err_len
) {
  const char *value = ant_manifest_string(obj, key);
  if (!value || !value[0]) {
    sandbox_asset_error(err, err_len, "manifest entry for %s is missing %s", name, key);
    return -EINVAL;
  }

  int written = snprintf(out, out_len, "%s", value);
  return written < 0 || (size_t)written >= out_len ? -ENAMETOOLONG : 0;
}

static int sandbox_manifest_copy_optional_string(
  char *out, size_t out_len, yyjson_val *obj, const char *key
) {
  const char *value = ant_manifest_string(obj, key);
  if (!value || !value[0]) {
    if (out_len > 0) out[0] = '\0';
    return 0;
  }

  int written = snprintf(out, out_len, "%s", value);
  return written < 0 || (size_t)written >= out_len ? -ENAMETOOLONG : 0;
}

static uint64_t sandbox_manifest_artifact_size(yyjson_val *entry) {
  yyjson_val *artifact = entry && yyjson_is_obj(entry) ? yyjson_obj_get(entry, "artifact") : NULL;
  return ant_manifest_uint(artifact, "size_in_bytes");
}

static uint64_t sandbox_manifest_gzip_size(yyjson_val *entry) {
  return ant_manifest_uint(entry, "gzip_size_in_bytes");
}

static int sandbox_manifest_url(char *out, size_t out_len) {
  const char *base = getenv("ANT_SANDBOX_MANIFEST_URL");
  const char *branch = getenv("ANT_SANDBOX_MANIFEST_BRANCH");
  
  const char *revision = sv_debug_enabled(SV_DEBUG_SANDBOX_BYPASS_MANIFEST) ? NULL : ANT_GIT_LONGHASH;
  if (!base || !base[0]) base = ANT_SANDBOX_MANIFEST_URL;

  int written = snprintf(out, out_len, "%s", base);
  if (written < 0 || (size_t)written >= out_len) return -ENAMETOOLONG;

  size_t pos = (size_t)written;
  bool query = strchr(base, '?') != NULL;

  if (revision && revision[0]) {
    written = snprintf(out + pos, out_len - pos, "%crevision=%s", query ? '&' : '?', revision);
    if (written < 0 || (size_t)written >= out_len - pos) return -ENAMETOOLONG;
    pos += (size_t)written;
    query = true;
  }

  if (!branch || !branch[0]) return 0;

  written = snprintf(out + pos, out_len - pos, "%cbranch=", query ? '&' : '?');
  if (written < 0 || (size_t)written >= out_len - pos) return -ENAMETOOLONG;

  pos += (size_t)written;
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
  
  const char *ant_revision = ant_manifest_string(ant, "revision");
  bool bypass_manifest = sv_debug_enabled(SV_DEBUG_SANDBOX_BYPASS_MANIFEST);

  if (!bypass_manifest && (!ant_revision || strcmp(ant_revision, ANT_GIT_LONGHASH) != 0)) {
    sandbox_asset_error(err, err_len,
      "manifest Ant revision mismatch: current=%s manifest=%s",
      ANT_GIT_LONGHASH, ant_revision ? ant_revision : "(missing)"
    );
    rc = -EINVAL;
    goto done;
  }

  uint64_t ant_run = ant_manifest_uint(ant_source, "run_id");
  uint64_t sandbox_run = ant_manifest_uint(sandbox_source, "run_id");
  uint64_t kernel_run = ant_manifest_uint(kernel_source, "run_id");
  
  if (!bypass_manifest && (!ant_run || sandbox_run != ant_run || kernel_run != ant_run)) {
    sandbox_asset_error(err, err_len,
      "manifest sandbox build mismatch: ant=%llu sandbox=%llu kernel=%llu",
      (unsigned long long)ant_run,
      (unsigned long long)sandbox_run,
      (unsigned long long)kernel_run
    );
    rc = -EINVAL;
    goto done;
  }

  const char *sandbox_revision = ant_manifest_string(sandbox, "revision");
  const char *kernel_revision = ant_manifest_string(kernel, "revision");

  if (!bypass_manifest && (!sandbox_revision || strcmp(sandbox_revision, ant_revision) != 0)) {
    sandbox_asset_error(err, err_len, "manifest sandbox commit mismatch");
    rc = -EINVAL;
    goto done;
  }

  if (!bypass_manifest && (!kernel_revision || strcmp(kernel_revision, ant_revision) != 0)) {
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
  bool show_progress = PROGRESS_ISATTY(PROGRESS_FILENO(stderr));
  if (show_progress) progress_start(&progress, "Fetching sandbox manifest");

  char *manifest = NULL;
  size_t manifest_len = 0;
  rc = ant_download_get(manifest_url, NULL, NULL, show_progress ? &progress : NULL, &manifest, &manifest_len, err, err_len);
  if (show_progress) progress_stop(&progress);
  if (rc != 0) {
    if (!sv_debug_enabled(SV_DEBUG_SANDBOX_BYPASS_MANIFEST) && err && strstr(err, "HTTP 404"))
      sandbox_asset_error(err, err_len, "%s", ANT_UNPUBLISHED_BUILD_HINT);
    return rc;
  }

  sandbox_manifest_selection_t selection = {0};
  rc = sandbox_manifest_select(manifest, manifest_len, &selection, err, err_len);
  free(manifest);
  if (rc != 0) return rc;

  bool sandbox_gzip = selection.sandbox_gzip_url[0] != '\0';
  bool kernel_gzip = selection.kernel_gzip_url[0] != '\0';
  ant_download_file_t downloads[] = {
    {
      .label = "sandbox image",
      .path = image_path,
      .url = sandbox_gzip ? selection.sandbox_gzip_url : selection.sandbox_url,
      .gzip = sandbox_gzip,
      .expected_size = selection.sandbox_size,
      .expected_gzip_size = selection.sandbox_gzip_size,
    },
    {
      .label = "sandbox kernel",
      .path = kernel_path,
      .url = kernel_gzip ? selection.kernel_gzip_url : selection.kernel_url,
      .gzip = kernel_gzip,
      .expected_size = selection.kernel_size,
      .expected_gzip_size = selection.kernel_gzip_size,
    },
  };

  return ant_download_files(downloads, sizeof(downloads) / sizeof(downloads[0]), err, err_len);
}

int ant_sandbox_assets_download_missing(
  const char *image_path,
  const char *kernel_path,
  char *err,
  size_t err_len
) {
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
}
