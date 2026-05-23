#include "cache.h"
#include "extractor.h"
#include "fetcher.h"
#include "intern.h"
#include "linker.h"
#include "lockfile.h"
#include "pkg.h"
#include "resolver.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <yyjson.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <sys/wait.h>
#endif

struct pkg_context {
  string_pool_t string_pool;
  cache_db_t *cache_db;
  fetcher_t *http;
  pkg_options_t options;
  char *last_error;
  char *cache_dir;
  pkg_install_result_t last_install_result;

  pkg_added_package_t *added_packages;
  uint32_t added_packages_count;
  uint32_t added_packages_capacity;

  pkg_lifecycle_script_t *lifecycle_scripts;
  uint32_t lifecycle_scripts_count;
  uint32_t lifecycle_scripts_capacity;
};

const char *pkg_error_string(const pkg_context_t *ctx) {
  if (ctx && ctx->last_error)
    return ctx->last_error;
  return "Unknown package manager error";
}

static void set_context_error(pkg_context_t *ctx, const char *msg) {
  free(ctx->last_error);
  ctx->last_error = strdup(msg);
}

static char *get_home_dir(void) {
#ifdef _WIN32
  const char *profile = getenv("USERPROFILE");
  if (profile)
    return strdup(profile);
  return NULL;
#else
  const char *home = getenv("HOME");
  if (home)
    return strdup(home);
  return NULL;
#endif
}

static char *get_default_cache_dir(void) {
  char *home = get_home_dir();
  if (!home)
    return NULL;

  char path[4096];
  // Check legacy .ant directory first
  snprintf(path, sizeof(path), "%s/.ant", home);
  struct stat st;
  if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
    snprintf(path, sizeof(path), "%s/.ant/pkg", home);
    free(home);
    return strdup(path);
  }

  const char *xdg = getenv("XDG_CACHE_HOME");
  if (xdg && strlen(xdg) > 0 && xdg[0] == '/') {
    snprintf(path, sizeof(path), "%s/ant/pkg", xdg);
  } else {
    snprintf(path, sizeof(path), "%s/.cache/ant/pkg", home);
  }
  free(home);
  return strdup(path);
}

pkg_context_t *pkg_init(const pkg_options_t *options) {
  pkg_context_t *self = calloc(1, sizeof(pkg_context_t));
  if (!self)
    return NULL;

  if (options) {
    self->options = *options;
  }

  string_pool_init(&self->string_pool);

  const char *cache_path = options ? options->cache_dir : NULL;
  if (cache_path) {
    self->cache_dir = strdup(cache_path);
  } else {
    self->cache_dir = get_default_cache_dir();
  }

  if (!self->cache_dir) {
    pkg_free(self);
    return NULL;
  }

  // Make sure cache dir exists
  char full_path[4096];
  snprintf(full_path, sizeof(full_path), "%s", self->cache_dir);
  char *p = strchr(full_path + 1, '/');
  while (p) {
    *p = '\0';
    mkdir(full_path, 0755);
    *p = '/';
    p = strchr(p + 1, '/');
  }
  mkdir(full_path, 0755);

  self->cache_db = cache_db_open(self->cache_dir);
  if (!self->cache_db) {
    set_context_error(self, "Failed to open cache database");
    pkg_free(self);
    return NULL;
  }

  const char *registry = (options && options->registry_url)
                             ? options->registry_url
                             : "registry.npmjs.org";
  self->http = fetcher_init(registry);
  if (!self->http) {
    set_context_error(self, "Failed to initialize fetcher");
    pkg_free(self);
    return NULL;
  }

  return self;
}

void pkg_free(pkg_context_t *ctx) {
  if (!ctx)
    return;
  if (ctx->http)
    fetcher_deinit(ctx->http);
  if (ctx->cache_db)
    cache_db_close(ctx->cache_db);
  string_pool_deinit(&ctx->string_pool);
  free(ctx->last_error);
  free(ctx->cache_dir);

  for (uint32_t i = 0; i < ctx->added_packages_count; i++) {
    free((char *)ctx->added_packages[i].name);
    free((char *)ctx->added_packages[i].version);
  }
  free(ctx->added_packages);

  for (uint32_t i = 0; i < ctx->lifecycle_scripts_count; i++) {
    free((char *)ctx->lifecycle_scripts[i].name);
    free((char *)ctx->lifecycle_scripts[i].script);
  }
  free(ctx->lifecycle_scripts);

  free(ctx);
}

void pkg_cache_sync(pkg_context_t *ctx) {
  if (ctx && ctx->cache_db) {
    cache_db_sync(ctx->cache_db);
  }
}

pkg_error_t pkg_cache_stats(pkg_context_t *ctx, pkg_cache_stats_t *out) {
  if (!ctx || !ctx->cache_db || !out)
    return PKG_INVALID_ARGUMENT;

  size_t entries = 0, db_size = 0, cache_size = 0;
  if (cache_db_stats(ctx->cache_db, &entries, &db_size, &cache_size)) {
    out->package_count = (uint32_t)entries;
    out->db_size = db_size;
    out->total_size = cache_size;
    return PKG_OK;
  }
  return PKG_CACHE_ERROR;
}

int32_t pkg_cache_prune(pkg_context_t *ctx, uint32_t max_age_days) {
  if (!ctx || !ctx->cache_db)
    return -1;
  return cache_db_prune(ctx->cache_db, max_age_days);
}

// Result storage helpers
static void add_package_to_results(pkg_context_t *self, const char *name,
                                   const char *version, bool direct) {
  if (self->added_packages_count >= self->added_packages_capacity) {
    self->added_packages_capacity = self->added_packages_capacity == 0
                                        ? 8
                                        : self->added_packages_capacity * 2;
    self->added_packages =
        realloc(self->added_packages,
                self->added_packages_capacity * sizeof(pkg_added_package_t));
  }
  self->added_packages[self->added_packages_count].name = strdup(name);
  self->added_packages[self->added_packages_count].version = strdup(version);
  self->added_packages[self->added_packages_count].direct = direct;
  self->added_packages_count++;
}

static void add_lifecycle_script(pkg_context_t *self, const char *name,
                                 const char *script) {
  if (self->lifecycle_scripts_count >= self->lifecycle_scripts_capacity) {
    self->lifecycle_scripts_capacity =
        self->lifecycle_scripts_capacity == 0
            ? 8
            : self->lifecycle_scripts_capacity * 2;
    self->lifecycle_scripts =
        realloc(self->lifecycle_scripts, self->lifecycle_scripts_capacity *
                                             sizeof(pkg_lifecycle_script_t));
  }
  self->lifecycle_scripts[self->lifecycle_scripts_count].name = strdup(name);
  self->lifecycle_scripts[self->lifecycle_scripts_count].script =
      strdup(script);
  self->lifecycle_scripts_count++;
}

// Progress reporting helper
static void report_progress(pkg_context_t *self, pkg_phase_t phase,
                            uint32_t current, uint32_t total,
                            const char *message) {
  if (self->options.progress_callback) {
    self->options.progress_callback(self->options.user_data, phase, current,
                                    total, message);
  }
}

// Compare function for qsort of cache hits (depth-first ordering)
static lockfile_t *global_lf = NULL;

static int link_path_depth(const char *path) {
  if (!path || strlen(path) == 0)
    return 0;
  int count = 0;
  for (const char *c = path; *c; c++) {
    if (*c == '/')
      count++;
  }
  return count + 1;
}

static int compare_batch_hits(const void *a_ptr, const void *b_ptr) {
  const cache_batch_hit_t *a = a_ptr;
  const cache_batch_hit_t *b = b_ptr;

  const lockfile_package_t *pkg_a = &global_lf->packages[a->index];
  const lockfile_package_t *pkg_b = &global_lf->packages[b->index];

  size_t len_a, len_b;
  const char *parent_a =
      lockfile_string_ref_slice(global_lf, pkg_a->parent_path, &len_a);
  const char *parent_b =
      lockfile_string_ref_slice(global_lf, pkg_b->parent_path, &len_b);

  char parent_a_buf[1024] = {0};
  char parent_b_buf[1024] = {0};
  memcpy(parent_a_buf, parent_a, len_a < 1023 ? len_a : 1023);
  memcpy(parent_b_buf, parent_b, len_b < 1023 ? len_b : 1023);

  int depth_a = link_path_depth(parent_a_buf);
  int depth_b = link_path_depth(parent_b_buf);
  if (depth_a != depth_b)
    return depth_a - depth_b;

  int cmp = strcmp(parent_a_buf, parent_b_buf);
  if (cmp != 0)
    return cmp;

  const char *name_a =
      lockfile_string_ref_slice(global_lf, pkg_a->name, &len_a);
  const char *name_b =
      lockfile_string_ref_slice(global_lf, pkg_b->name, &len_b);
  char name_a_buf[1024] = {0};
  char name_b_buf[1024] = {0};
  memcpy(name_a_buf, name_a, len_a < 1023 ? len_a : 1023);
  memcpy(name_b_buf, name_b, len_b < 1023 ? len_b : 1023);

  return strcmp(name_a_buf, name_b_buf);
}

typedef struct {
  extractor_t *ext;
  uint32_t pkg_idx;
  uint8_t integrity[64];
  char *cache_path;
  char *pkg_name;
  char *version_str;
  bool direct;
  char *parent_path;
  bool has_bin;
  bool completed;
  bool has_error;
} tar_extract_ctx_t;

static void on_tarball_chunk(const uint8_t *data, size_t len, void *user_data) {
  tar_extract_ctx_t *ctx = user_data;
  if (extractor_feed_compressed(ctx->ext, data, len) != EXTRACT_OK) {
    ctx->has_error = true;
  }
}

static void on_tarball_complete(uint16_t status_code, void *user_data) {
  tar_extract_ctx_t *ctx = user_data;
  if (status_code != 200)
    ctx->has_error = true;
  ctx->completed = true;
}

static void on_tarball_error(fetch_error_t err, void *user_data) {
  (void)err;
  tar_extract_ctx_t *ctx = user_data;
  ctx->has_error = true;
  ctx->completed = true;
}

static int compare_extract_ctxs(const void *a_ptr, const void *b_ptr) {
  const tar_extract_ctx_t *a = a_ptr;
  const tar_extract_ctx_t *b = b_ptr;

  int depth_a = link_path_depth(a->parent_path);
  int depth_b = link_path_depth(b->parent_path);
  if (depth_a != depth_b)
    return depth_a - depth_b;

  const char *parent_a = a->parent_path ? a->parent_path : "";
  const char *parent_b = b->parent_path ? b->parent_path : "";
  int cmp = strcmp(parent_a, parent_b);
  if (cmp != 0)
    return cmp;

  return strcmp(a->pkg_name, b->pkg_name);
}

// Run postinstall scripts
typedef struct {
  char *pkg_name;
  char *pkg_dir;
  char *script;
  bool failed;
} postinstall_job_t;

static void run_trusted_postinstall(pkg_context_t *ctx,
                                    const char *node_modules_path,
                                    const char **trusted_names,
                                    uint32_t trusted_count) {
  char abs_nm_path[4096];
  if (!realpath(node_modules_path, abs_nm_path))
    return;

  char bin_path[4096];
  snprintf(bin_path, sizeof(bin_path), "%s/.bin", abs_nm_path);

  const char *path_env = getenv("PATH");
  char new_path[8192];
  if (path_env) {
#ifdef _WIN32
    snprintf(new_path, sizeof(new_path), "%s;%s", bin_path, path_env);
#else
    snprintf(new_path, sizeof(new_path), "%s:%s", bin_path, path_env);
#endif
  } else {
    snprintf(new_path, sizeof(new_path), "%s", bin_path);
  }

#ifdef _WIN32
  _putenv_s("PATH", new_path);
#else
  setenv("PATH", new_path, 1);
#endif

  postinstall_job_t *jobs = malloc(trusted_count * sizeof(postinstall_job_t));
  uint32_t job_count = 0;

  for (uint32_t i = 0; i < trusted_count; i++) {
    const char *pkg_name = trusted_names[i];

    char json_path[4096];
    snprintf(json_path, sizeof(json_path), "%s/%s/package.json",
             node_modules_path, pkg_name);

    yyjson_read_err err;
    yyjson_doc *doc = yyjson_read_file(json_path, 0, NULL, &err);
    if (!doc)
      continue;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *scripts = yyjson_obj_get(root, "scripts");
    if (yyjson_is_obj(scripts)) {
      yyjson_val *post_val = yyjson_obj_get(scripts, "postinstall");
      if (!post_val)
        post_val = yyjson_obj_get(scripts, "install");

      if (post_val && yyjson_is_str(post_val)) {
        if (strcmp(pkg_name, "esbuild") == 0) {
          yyjson_doc_free(doc);
          continue;
        }

        char pkg_dir[4096];
        snprintf(pkg_dir, sizeof(pkg_dir), "%s/%s", node_modules_path,
                 pkg_name);

        char marker[4096];
        snprintf(marker, sizeof(marker), "%s/.postinstall", pkg_dir);
        struct stat st;
        if (stat(marker, &st) == 0) {
          yyjson_doc_free(doc);
          continue;
        }

        jobs[job_count].pkg_name = strdup(pkg_name);
        jobs[job_count].pkg_dir = strdup(pkg_dir);
        jobs[job_count].script = strdup(yyjson_get_str(post_val));
        jobs[job_count].failed = false;
        job_count++;
      }
    }
    yyjson_doc_free(doc);
  }

  for (uint32_t i = 0; i < job_count; i++) {
    report_progress(ctx, PKG_PHASE_POSTINSTALL, i, job_count, jobs[i].pkg_name);

    char cmd[8192];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "cd %s && %s", jobs[i].pkg_dir, jobs[i].script);
#else
    snprintf(cmd, sizeof(cmd), "cd \"%s\" && %s", jobs[i].pkg_dir,
             jobs[i].script);
#endif

    int ret = system(cmd);
    if (ret == 0) {
      char marker[4096];
      snprintf(marker, sizeof(marker), "%s/.postinstall", jobs[i].pkg_dir);
      FILE *f = fopen(marker, "w");
      if (f)
        fclose(f);
    } else {
      jobs[i].failed = true;
    }
  }

  // Cleanup jobs
  for (uint32_t i = 0; i < job_count; i++) {
    free(jobs[i].pkg_name);
    free(jobs[i].pkg_dir);
    free(jobs[i].script);
  }
  free(jobs);
}

pkg_error_t pkg_install(pkg_context_t *ctx, const char *package_json_path,
                        const char *lockfile_path,
                        const char *node_modules_path) {
  if (!ctx || !lockfile_path || !node_modules_path)
    return PKG_INVALID_ARGUMENT;

  ctx->added_packages_count = 0;

  clock_t start_time = clock();

  lockfile_t lf;
  if (!lockfile_open(lockfile_path, &lf)) {
    set_context_error(ctx, "Failed to open lockfile");
    return PKG_INVALID_LOCKFILE;
  }

  uint32_t pkg_count = lf.header->package_count;

  uint8_t (*integrities)[64] = malloc(pkg_count * 64);
  for (uint32_t i = 0; i < pkg_count; i++) {
    memcpy(integrities[i], lf.packages[i].integrity, 64);
  }

  cache_batch_hit_t *hits = malloc(pkg_count * sizeof(cache_batch_hit_t));
  size_t hit_count = cache_db_batch_lookup(
      ctx->cache_db, (const uint8_t (*)[64])integrities, pkg_count, hits);

  bool *hit_map = calloc(pkg_count, sizeof(bool));
  for (size_t i = 0; i < hit_count; i++) {
    hit_map[hits[i].index] = true;
  }

  uint32_t miss_count = 0;
  uint32_t *misses = malloc(pkg_count * sizeof(uint32_t));
  for (uint32_t i = 0; i < pkg_count; i++) {
    if (!hit_map[i]) {
      misses[miss_count++] = i;
    }
  }
  free(hit_map);

  // Sort hits depth-first
  global_lf = &lf;
  qsort(hits, hit_count, sizeof(cache_batch_hit_t), compare_batch_hits);
  global_lf = NULL;

  linker_t *linker = linker_init();
  if (!linker) {
    lockfile_close(&lf);
    free(integrities);
    free(hits);
    free(misses);
    return PKG_OUT_OF_MEMORY;
  }
  linker_set_node_modules_path(linker, node_modules_path);

  // Link cache hits
  for (size_t i = 0; i < hit_count; i++) {
    const lockfile_package_t *pkg = &lf.packages[hits[i].index];
    size_t name_len, parent_len;
    const char *name = lockfile_string_ref_slice(&lf, pkg->name, &name_len);
    const char *parent =
        lockfile_string_ref_slice(&lf, pkg->parent_path, &parent_len);

    char name_buf[512] = {0};
    char parent_buf[1024] = {0};
    memcpy(name_buf, name, name_len < 511 ? name_len : 511);
    memcpy(parent_buf, parent, parent_len < 1023 ? parent_len : 1023);

    report_progress(ctx, PKG_PHASE_LINKING, (uint32_t)i, (uint32_t)hit_count,
                    name_buf);

    char *cache_path = cache_db_get_package_path(ctx->cache_db, pkg->integrity);
    package_link_t link = {.cache_path = cache_path,
                           .node_modules_path = node_modules_path,
                           .name = name_buf,
                           .parent_path = parent_len > 0 ? parent_buf : NULL,
                           .file_count = hits[i].file_count,
                           .has_bin = pkg->flags.has_bin};
    linker_link_package(linker, &link);
    free(cache_path);

    if (pkg->flags.direct) {
      size_t ver_len;
      const char *ver_str =
          lockfile_string_ref_slice(&lf, pkg->prerelease, &ver_len);
      char ver_buf[64];
      if (ver_len > 0) {
        char pre_buf[64] = {0};
        memcpy(pre_buf, ver_str, ver_len < 63 ? ver_len : 63);
        snprintf(ver_buf, sizeof(ver_buf), "%llu.%llu.%llu-%s",
                 pkg->version_major, pkg->version_minor, pkg->version_patch,
                 pre_buf);
      } else {
        snprintf(ver_buf, sizeof(ver_buf), "%llu.%llu.%llu", pkg->version_major,
                 pkg->version_minor, pkg->version_patch);
      }
      add_package_to_results(ctx, name_buf, ver_buf, true);
    }
  }

  // Link cache misses
  if (miss_count > 0) {
    tar_extract_ctx_t *extracts = calloc(miss_count, sizeof(tar_extract_ctx_t));
    uint32_t active_count = 0;

    for (uint32_t i = 0; i < miss_count; i++) {
      uint32_t pkg_idx = misses[i];
      const lockfile_package_t *pkg = &lf.packages[pkg_idx];

      size_t name_len, url_len, parent_len, pre_len;
      const char *name = lockfile_string_ref_slice(&lf, pkg->name, &name_len);
      const char *url =
          lockfile_string_ref_slice(&lf, pkg->tarball_url, &url_len);
      const char *parent =
          lockfile_string_ref_slice(&lf, pkg->parent_path, &parent_len);
      const char *pre =
          lockfile_string_ref_slice(&lf, pkg->prerelease, &pre_len);

      char name_buf[512] = {0};
      char url_buf[2048] = {0};
      char parent_buf[1024] = {0};
      char ver_buf[64];

      memcpy(name_buf, name, name_len < 511 ? name_len : 511);
      memcpy(url_buf, url, url_len < 2047 ? url_len : 2047);
      memcpy(parent_buf, parent, parent_len < 1023 ? parent_len : 1023);

      if (pre_len > 0) {
        char pre_buf[64] = {0};
        memcpy(pre_buf, pre, pre_len < 63 ? pre_len : 63);
        snprintf(ver_buf, sizeof(ver_buf), "%llu.%llu.%llu-%s",
                 pkg->version_major, pkg->version_minor, pkg->version_patch,
                 pre_buf);
      } else {
        snprintf(ver_buf, sizeof(ver_buf), "%llu.%llu.%llu", pkg->version_major,
                 pkg->version_minor, pkg->version_patch);
      }

      report_progress(ctx, PKG_PHASE_FETCHING, i, miss_count, name_buf);

      char *cache_path =
          cache_db_get_package_path(ctx->cache_db, pkg->integrity);
      extractor_t *ext = extractor_init(cache_path);
      if (!ext) {
        free(cache_path);
        continue;
      }

      tar_extract_ctx_t *ectx = &extracts[active_count++];
      ectx->ext = ext;
      ectx->pkg_idx = pkg_idx;
      memcpy(ectx->integrity, pkg->integrity, 64);
      ectx->cache_path = cache_path;
      ectx->pkg_name = strdup(name_buf);
      ectx->version_str = strdup(ver_buf);
      ectx->direct = pkg->flags.direct;
      ectx->parent_path = parent_len > 0 ? strdup(parent_buf) : NULL;
      ectx->has_bin = pkg->flags.has_bin;

      fetch_stream_handler_t handler = {.on_data = on_tarball_chunk,
                                        .on_complete = on_tarball_complete,
                                        .on_error = on_tarball_error,
                                        .user_data = ectx};

      fetcher_fetch_tarball(ctx->http, url_buf, handler);
    }

    fetcher_finish_tarballs(ctx->http);

    // Sort extracts depth-first
    qsort(extracts, active_count, sizeof(tar_extract_ctx_t),
          compare_extract_ctxs);

    for (uint32_t i = 0; i < active_count; i++) {
      tar_extract_ctx_t *ectx = &extracts[i];
      extractor_stats_t stats = extractor_stats(ectx->ext);
      extractor_deinit(ectx->ext);

      if (!ectx->has_error) {
        cache_entry_t entry = {.unpacked_size = stats.bytes,
                               .file_count = stats.files,
                               .cached_at = (int64_t)time(NULL)};
        memcpy(entry.integrity, ectx->integrity, 64);
        entry.path = ectx->cache_path;
        cache_db_insert(ctx->cache_db, &entry, ectx->pkg_name,
                        ectx->version_str);

        add_package_to_results(ctx, ectx->pkg_name, ectx->version_str,
                               ectx->direct);

        report_progress(ctx, PKG_PHASE_LINKING, i, active_count,
                        ectx->pkg_name);

        package_link_t link = {.cache_path = ectx->cache_path,
                               .node_modules_path = node_modules_path,
                               .name = ectx->pkg_name,
                               .parent_path = ectx->parent_path,
                               .file_count = stats.files,
                               .has_bin = ectx->has_bin};
        linker_link_package(linker, &link);
      }

      free(ectx->cache_path);
      free(ectx->pkg_name);
      free(ectx->version_str);
      free(ectx->parent_path);
    }
    free(extracts);
  }

  cache_db_sync(ctx->cache_db);

  linker_stats_t stats = linker_get_stats(linker);
  linker_deinit(linker);
  lockfile_close(&lf);
  free(integrities);
  free(hits);
  free(misses);

  ctx->last_install_result = (pkg_install_result_t){
      .package_count = pkg_count,
      .cache_hits = (uint32_t)hit_count,
      .cache_misses = miss_count,
      .files_linked = stats.files_linked + stats.files_cloned,
      .files_copied = stats.files_copied,
      .packages_installed = stats.packages_installed,
      .packages_skipped = stats.packages_skipped,
      .elapsed_ms = (clock() - start_time) * 1000 / CLOCKS_PER_SEC};

  // Discover and run trusted postinstall scripts if package.json has them
  yyjson_read_err err;
  yyjson_doc *doc = yyjson_read_file(package_json_path, 0, NULL, &err);
  if (doc) {
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *trusted = yyjson_obj_get(root, "trustedDependencies");
    if (yyjson_is_arr(trusted)) {
      size_t sz = yyjson_arr_size(trusted);
      const char **trusted_names = malloc(sz * sizeof(char *));
      size_t actual_trusted = 0;
      for (size_t i = 0; i < sz; i++) {
        const char *name = yyjson_get_str(yyjson_arr_get(trusted, i));
        if (name)
          trusted_names[actual_trusted++] = name;
      }
      if (actual_trusted > 0) {
        run_trusted_postinstall(ctx, node_modules_path, trusted_names,
                                (uint32_t)actual_trusted);
      }
      free(trusted_names);
    }
    yyjson_doc_free(doc);
  }

  return PKG_OK;
}

pkg_error_t pkg_resolve_and_install(pkg_context_t *ctx,
                                    const char *package_json_path,
                                    const char *lockfile_path,
                                    const char *node_modules_path) {
  if (!ctx || !package_json_path || !lockfile_path || !node_modules_path)
    return PKG_INVALID_ARGUMENT;

  resolver_t *r = resolver_init(&ctx->string_pool, ctx->cache_db, ctx->http);
  if (!r)
    return PKG_OUT_OF_MEMORY;

  if (!resolver_resolve_from_package_json(r, package_json_path)) {
    resolver_deinit(r);
    set_context_error(ctx, "Failed to resolve dependencies");
    return PKG_RESOLVE_ERROR;
  }

  if (!resolver_write_lockfile(r, lockfile_path)) {
    resolver_deinit(r);
    set_context_error(ctx, "Failed to write lockfile");
    return PKG_IO_ERROR;
  }

  resolver_deinit(r);

  return pkg_install(ctx, package_json_path, lockfile_path, node_modules_path);
}

typedef struct {
  resolver_t *resolver;
  const char *name;
  const semver_constraint_t *constraint;
  const version_info_t *best;
} add_package_ctx_t;

static void add_meta_cb(const char *n, const uint8_t *data, size_t len,
                        bool has_err, void *ud) {
  if (has_err || !data)
    return;
  add_package_ctx_t *ctx = ud;
  package_metadata_t meta = parse_metadata_json(n, (const char *)data, len);

  // Add to cache
  resolver_add_metadata_to_cache(ctx->resolver, n, meta);

  ctx->best = select_best_version(&meta, ctx->constraint);
}

pkg_error_t pkg_add(pkg_context_t *ctx, const char *package_json_path,
                    const char *package_spec, bool dev) {
  if (!ctx || !package_json_path || !package_spec)
    return PKG_INVALID_ARGUMENT;

  char pkg_name[512];
  char version_constraint[512];
  strcpy(version_constraint, "latest");

  const char *at = strchr(package_spec, '@');
  if (at) {
    if (at == package_spec) {
      const char *second_at = strchr(package_spec + 1, '@');
      if (second_at) {
        snprintf(pkg_name, sizeof(pkg_name), "%.*s",
                 (int)(second_at - package_spec), package_spec);
        snprintf(version_constraint, sizeof(version_constraint), "%s",
                 second_at + 1);
      } else {
        snprintf(pkg_name, sizeof(pkg_name), "%s", package_spec);
      }
    } else {
      snprintf(pkg_name, sizeof(pkg_name), "%.*s", (int)(at - package_spec),
               package_spec);
      snprintf(version_constraint, sizeof(version_constraint), "%s", at + 1);
    }
  } else {
    snprintf(pkg_name, sizeof(pkg_name), "%s", package_spec);
  }

  resolver_t *r = resolver_init(&ctx->string_pool, ctx->cache_db, ctx->http);
  if (!r)
    return PKG_OUT_OF_MEMORY;

  semver_constraint_t con;
  semver_constraint_parse(version_constraint, &con);

  // Fetch metadata to find latest version
  char *names[] = {pkg_name};
  add_package_ctx_t add_ctx = {
      .resolver = r, .name = pkg_name, .constraint = &con, .best = NULL};

  fetcher_fetch_metadata_streaming(ctx->http, (const char *const *)names, 1,
                                   add_meta_cb, &add_ctx);
  semver_constraint_free(&con);

  if (!add_ctx.best) {
    resolver_deinit(r);
    set_context_error(ctx, "Failed to resolve package");
    return PKG_RESOLVE_ERROR;
  }

  char *ver_str = semver_format(&add_ctx.best->version);
  char caret_ver[256];
  snprintf(caret_ver, sizeof(caret_ver), "^%s", ver_str);
  free(ver_str);

  resolver_deinit(r);

  // Read and modify package.json
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_read_err err;
  yyjson_doc *read_doc = yyjson_read_file(package_json_path, 0, NULL, &err);
  yyjson_mut_val *root;
  if (read_doc) {
    root = yyjson_val_mut_copy(doc, yyjson_doc_get_root(read_doc));
  } else {
    root = yyjson_mut_obj(doc);
  }

  const char *target = dev ? "devDependencies" : "dependencies";
  yyjson_mut_val *deps = yyjson_mut_obj_get(root, target);
  if (!deps || !yyjson_mut_is_obj(deps)) {
    deps = yyjson_mut_obj(doc);
    yyjson_mut_obj_add(root, yyjson_mut_str(doc, target), deps);
  }

  yyjson_mut_obj_add(deps, yyjson_mut_str(doc, pkg_name),
                     yyjson_mut_str(doc, caret_ver));

  yyjson_mut_write_file(package_json_path, doc, 0, NULL, NULL);
  yyjson_mut_doc_free(doc);
  if (read_doc)
    yyjson_doc_free(read_doc);

  return PKG_OK;
}

pkg_error_t pkg_add_many(pkg_context_t *ctx, const char *package_json_path,
                         const char *const *package_specs, uint32_t count,
                         bool dev) {
  for (uint32_t i = 0; i < count; i++) {
    pkg_error_t err = pkg_add(ctx, package_json_path, package_specs[i], dev);
    if (err != PKG_OK)
      return err;
  }
  return PKG_OK;
}

pkg_error_t pkg_remove(pkg_context_t *ctx, const char *package_json_path,
                       const char *package_name) {
  if (!ctx || !package_json_path || !package_name)
    return PKG_INVALID_ARGUMENT;

  yyjson_read_err err;
  yyjson_doc *read_doc = yyjson_read_file(package_json_path, 0, NULL, &err);
  if (!read_doc)
    return PKG_IO_ERROR;

  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root =
      yyjson_val_mut_copy(doc, yyjson_doc_get_root(read_doc));

  yyjson_mut_val *deps = yyjson_mut_obj_get(root, "dependencies");
  if (deps && yyjson_mut_is_obj(deps)) {
    yyjson_mut_obj_remove(deps, yyjson_mut_str(doc, package_name));
  }

  yyjson_mut_val *dev_deps = yyjson_mut_obj_get(root, "devDependencies");
  if (dev_deps && yyjson_mut_is_obj(dev_deps)) {
    yyjson_mut_obj_remove(dev_deps, yyjson_mut_str(doc, package_name));
  }

  yyjson_mut_write_file(package_json_path, doc, 0, NULL, NULL);
  yyjson_mut_doc_free(doc);
  yyjson_doc_free(read_doc);

  return PKG_OK;
}

uint32_t pkg_get_added_count(const pkg_context_t *ctx) {
  return ctx ? ctx->added_packages_count : 0;
}

uint32_t pkg_count_installed(const char *node_modules_path) {
  DIR *dir = opendir(node_modules_path);
  if (!dir)
    return 0;

  uint32_t count = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
        strcmp(entry->d_name, ".bin") == 0) {
      continue;
    }

    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", node_modules_path, entry->d_name);
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
      if (entry->d_name[0] == '@') {
        // Scoped folder - count contents
        DIR *sdir = opendir(path);
        if (sdir) {
          struct dirent *sentry;
          while ((sentry = readdir(sdir)) != NULL) {
            if (strcmp(sentry->d_name, ".") == 0 ||
                strcmp(sentry->d_name, "..") == 0)
              continue;
            count++;
          }
          closedir(sdir);
        }
      } else {
        count++;
      }
    }
  }
  closedir(dir);
  return count;
}

uint32_t pkg_get_lifecycle_script_count(const pkg_context_t *ctx) {
  return ctx ? ctx->lifecycle_scripts_count : 0;
}

pkg_error_t pkg_discover_lifecycle_scripts(pkg_context_t *ctx,
                                           const char *node_modules_path) {
  if (!ctx || !node_modules_path)
    return PKG_INVALID_ARGUMENT;

  // Clear existing
  for (uint32_t i = 0; i < ctx->lifecycle_scripts_count; i++) {
    free((char *)ctx->lifecycle_scripts[i].name);
    free((char *)ctx->lifecycle_scripts[i].script);
  }
  ctx->lifecycle_scripts_count = 0;

  DIR *dir = opendir(node_modules_path);
  if (!dir)
    return PKG_IO_ERROR;

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
        strcmp(entry->d_name, ".bin") == 0) {
      continue;
    }

    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", node_modules_path, entry->d_name);
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
      if (entry->d_name[0] == '@') {
        DIR *sdir = opendir(path);
        if (sdir) {
          struct dirent *sentry;
          while ((sentry = readdir(sdir)) != NULL) {
            if (strcmp(sentry->d_name, ".") == 0 ||
                strcmp(sentry->d_name, "..") == 0)
              continue;

            char full_name[512];
            snprintf(full_name, sizeof(full_name), "%s/%s", entry->d_name,
                     sentry->d_name);

            char pkg_dir[4096];
            snprintf(pkg_dir, sizeof(pkg_dir), "%s/%s", path, sentry->d_name);

            char marker[4096];
            snprintf(marker, sizeof(marker), "%s/.postinstall", pkg_dir);
            struct stat mst;
            if (stat(marker, &mst) == 0)
              continue;

            char json_path[4096];
            snprintf(json_path, sizeof(json_path), "%s/package.json", pkg_dir);
            yyjson_read_err jerr;
            yyjson_doc *doc = yyjson_read_file(json_path, 0, NULL, &jerr);
            if (doc) {
              yyjson_val *scripts =
                  yyjson_obj_get(yyjson_doc_get_root(doc), "scripts");
              if (yyjson_is_obj(scripts)) {
                yyjson_val *post = yyjson_obj_get(scripts, "postinstall");
                if (!post)
                  post = yyjson_obj_get(scripts, "install");
                if (post && yyjson_is_str(post) &&
                    strcmp(full_name, "esbuild") != 0) {
                  add_lifecycle_script(ctx, full_name, yyjson_get_str(post));
                }
              }
              yyjson_doc_free(doc);
            }
          }
          closedir(sdir);
        }
      } else {
        char marker[4096];
        snprintf(marker, sizeof(marker), "%s/.postinstall", path);
        struct stat mst;
        if (stat(marker, &mst) == 0)
          continue;

        char json_path[4096];
        snprintf(json_path, sizeof(json_path), "%s/package.json", path);
        yyjson_read_err jerr;
        yyjson_doc *doc = yyjson_read_file(json_path, 0, NULL, &jerr);
        if (doc) {
          yyjson_val *scripts =
              yyjson_obj_get(yyjson_doc_get_root(doc), "scripts");
          if (yyjson_is_obj(scripts)) {
            yyjson_val *post = yyjson_obj_get(scripts, "postinstall");
            if (!post)
              post = yyjson_obj_get(scripts, "install");
            if (post && yyjson_is_str(post) &&
                strcmp(entry->d_name, "esbuild") != 0) {
              add_lifecycle_script(ctx, entry->d_name, yyjson_get_str(post));
            }
          }
          yyjson_doc_free(doc);
        }
      }
    }
  }
  closedir(dir);
  return PKG_OK;
}

pkg_error_t pkg_get_lifecycle_script(const pkg_context_t *ctx, uint32_t index,
                                     pkg_lifecycle_script_t *out) {
  if (!ctx || !out || index >= ctx->lifecycle_scripts_count)
    return PKG_INVALID_ARGUMENT;
  *out = ctx->lifecycle_scripts[index];
  return PKG_OK;
}

pkg_error_t pkg_run_postinstall(pkg_context_t *ctx,
                                const char *node_modules_path,
                                const char **package_names, uint32_t count) {
  if (!ctx || !node_modules_path)
    return PKG_INVALID_ARGUMENT;
  run_trusted_postinstall(ctx, node_modules_path, package_names, count);
  return PKG_OK;
}

pkg_error_t pkg_add_trusted_dependencies(const char *package_json_path,
                                         const char **package_names,
                                         uint32_t count) {
  yyjson_read_err err;
  yyjson_doc *read_doc = yyjson_read_file(package_json_path, 0, NULL, &err);

  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root;
  if (read_doc) {
    root = yyjson_val_mut_copy(doc, yyjson_doc_get_root(read_doc));
  } else {
    root = yyjson_mut_obj(doc);
  }

  yyjson_mut_val *trusted = yyjson_mut_obj_get(root, "trustedDependencies");
  if (!trusted || !yyjson_mut_is_arr(trusted)) {
    trusted = yyjson_mut_arr(doc);
    yyjson_mut_obj_add(root, yyjson_mut_str(doc, "trustedDependencies"),
                       trusted);
  }

  for (uint32_t i = 0; i < count; i++) {
    // Check if already in array
    bool exists = false;
    size_t sz = yyjson_mut_arr_size(trusted);
    for (size_t j = 0; j < sz; j++) {
      yyjson_mut_val *item = yyjson_mut_arr_get(trusted, j);
      if (yyjson_mut_is_str(item) &&
          strcmp(yyjson_mut_get_str(item), package_names[i]) == 0) {
        exists = true;
        break;
      }
    }
    if (!exists) {
      yyjson_mut_arr_add_val(trusted, yyjson_mut_str(doc, package_names[i]));
    }
  }

  yyjson_mut_write_file(package_json_path, doc, 0, NULL, NULL);
  yyjson_mut_doc_free(doc);
  if (read_doc)
    yyjson_doc_free(read_doc);
  return PKG_OK;
}

pkg_error_t pkg_get_install_result(pkg_context_t *ctx,
                                   pkg_install_result_t *out) {
  if (!ctx || !out)
    return PKG_INVALID_ARGUMENT;
  *out = ctx->last_install_result;
  return PKG_OK;
}

pkg_error_t pkg_get_added_package(const pkg_context_t *ctx, uint32_t index,
                                  pkg_added_package_t *out) {
  if (!ctx || !out || index >= ctx->added_packages_count)
    return PKG_INVALID_ARGUMENT;
  *out = ctx->added_packages[index];
  return PKG_OK;
}

int pkg_get_latest_available_version(pkg_context_t *ctx,
                                     const char *package_name,
                                     const char *installed_version,
                                     char *out_version,
                                     size_t out_version_len) {
  (void)ctx;
  (void)package_name;
  (void)installed_version;
  (void)out_version;
  (void)out_version_len;
  return 0; // stub
}

int pkg_get_bin_path(const char *node_modules_path, const char *bin_name,
                     char *out_path, size_t out_path_len) {
  // Finds binary command in node_modules/.bin/
  char path[4096];
  snprintf(path, sizeof(path), "%s/.bin/%s", node_modules_path, bin_name);
  struct stat st;
  if (stat(path, &st) == 0 && (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode))) {
    snprintf(out_path, out_path_len, "%s", path);
    return 1;
  }
  return 0;
}

int pkg_list_bins(const char *node_modules_path, pkg_bin_callback callback,
                  void *user_data) {
  char path[4096];
  snprintf(path, sizeof(path), "%s/.bin", node_modules_path);
  DIR *dir = opendir(path);
  if (!dir)
    return 0;

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    callback(entry->d_name, user_data);
  }
  closedir(dir);
  return 1;
}

int pkg_list_package_bins(const char *node_modules_path,
                          const char *package_name, pkg_bin_callback callback,
                          void *user_data) {
  char pkg_json_path[4096];
  snprintf(pkg_json_path, sizeof(pkg_json_path), "%s/%s/package.json",
           node_modules_path, package_name);

  yyjson_read_err err;
  yyjson_doc *doc = yyjson_read_file(pkg_json_path, 0, NULL, &err);
  if (!doc)
    return 0;

  yyjson_val *root = yyjson_doc_get_root(doc);
  yyjson_val *bin_val = yyjson_obj_get(root, "bin");
  if (yyjson_is_obj(bin_val)) {
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(bin_val, &iter);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&iter))) {
      callback(yyjson_get_str(key), user_data);
    }
  } else if (yyjson_is_str(bin_val)) {
    const char *slash = strchr(package_name, '/');
    callback(slash ? slash + 1 : package_name, user_data);
  }

  yyjson_doc_free(doc);
  return 1;
}

int pkg_get_script(const char *package_json_path, const char *script_name,
                   char *out_script, size_t out_script_len) {
  yyjson_read_err err;
  yyjson_doc *doc = yyjson_read_file(package_json_path, 0, NULL, &err);
  if (!doc)
    return -1;

  yyjson_val *root = yyjson_doc_get_root(doc);
  yyjson_val *scripts = yyjson_obj_get(root, "scripts");
  int found = -1;
  if (yyjson_is_obj(scripts)) {
    yyjson_val *scr = yyjson_obj_get(scripts, script_name);
    if (scr && yyjson_is_str(scr)) {
      snprintf(out_script, out_script_len, "%s", yyjson_get_str(scr));
      found = 0;
    }
  }
  yyjson_doc_free(doc);
  return found;
}

pkg_error_t pkg_run_script(const char *package_json_path,
                           const char *script_name,
                           const char *node_modules_path,
                           const char *extra_args,
                           pkg_script_result_t *result) {
  (void)node_modules_path;
  (void)extra_args;
  char script_cmd[4096];
  if (pkg_get_script(package_json_path, script_name, script_cmd,
                     sizeof(script_cmd)) != 0) {
    return PKG_NOT_FOUND;
  }

  // CWD of script execution is the folder containing package.json
  char *path_copy = strdup(package_json_path);
  char *slash = strrchr(path_copy, '/');
  if (slash)
    *slash = '\0';

  char final_cmd[8192];
#ifdef _WIN32
  snprintf(final_cmd, sizeof(final_cmd), "cd %s && %s", path_copy, script_cmd);
#else
  snprintf(final_cmd, sizeof(final_cmd), "cd \"%s\" && %s", path_copy,
           script_cmd);
#endif
  free(path_copy);

  int code = system(final_cmd);
  if (result) {
    result->exit_code = WIFEXITED(code) ? WEXITSTATUS(code) : code;
    result->signal = WIFSIGNALED(code) ? WTERMSIG(code) : 0;
  }

  return PKG_OK;
}

int pkg_list_scripts(const char *package_json_path,
                     pkg_script_callback callback, void *user_data) {
  yyjson_read_err err;
  yyjson_doc *doc = yyjson_read_file(package_json_path, 0, NULL, &err);
  if (!doc)
    return -1;

  yyjson_val *root = yyjson_doc_get_root(doc);
  yyjson_val *scripts = yyjson_obj_get(root, "scripts");
  if (yyjson_is_obj(scripts)) {
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(scripts, &iter);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&iter))) {
      yyjson_val *val = yyjson_obj_iter_get_val(key);
      callback(yyjson_get_str(key), yyjson_get_str(val), user_data);
    }
  }
  yyjson_doc_free(doc);
  return 0;
}

int pkg_why_info(const char *lockfile_path, const char *package_name,
                 pkg_why_info_t *out) {
  lockfile_t lf;
  if (!lockfile_open(lockfile_path, &lf))
    return -1;

  out->found = false;
  out->is_peer = false;
  out->is_dev = false;
  out->is_direct = false;
  memset(out->target_version, 0, sizeof(out->target_version));

  for (uint32_t i = 0; i < lf.header->package_count; i++) {
    const lockfile_package_t *pkg = &lf.packages[i];
    size_t len;
    const char *name = lockfile_string_ref_slice(&lf, pkg->name, &len);

    char name_buf[512] = {0};
    memcpy(name_buf, name, len < 511 ? len : 511);

    if (strcmp(name_buf, package_name) == 0) {
      size_t pre_len;
      const char *pre =
          lockfile_string_ref_slice(&lf, pkg->prerelease, &pre_len);
      if (pre_len > 0) {
        char pre_buf[64] = {0};
        memcpy(pre_buf, pre, pre_len < 63 ? pre_len : 63);
        snprintf(out->target_version, sizeof(out->target_version),
                 "%llu.%llu.%llu-%s", pkg->version_major, pkg->version_minor,
                 pkg->version_patch, pre_buf);
      } else {
        snprintf(out->target_version, sizeof(out->target_version),
                 "%llu.%llu.%llu", pkg->version_major, pkg->version_minor,
                 pkg->version_patch);
      }
      out->found = true;
      out->is_dev = pkg->flags.dev;
      out->is_direct = pkg->flags.direct;
    }

    // Check if this package depends on target package as a peer
    for (uint32_t j = 0; j < pkg->deps_count; j++) {
      const lockfile_dependency_t *dep = &lf.dependencies[pkg->deps_start + j];
      const lockfile_package_t *dep_pkg = &lf.packages[dep->package_index];
      size_t dep_len;
      const char *dep_name =
          lockfile_string_ref_slice(&lf, dep_pkg->name, &dep_len);
      char dep_buf[512] = {0};
      memcpy(dep_buf, dep_name, dep_len < 511 ? dep_len : 511);

      if (strcmp(dep_buf, package_name) == 0 && dep->flags.peer) {
        out->is_peer = true;
      }
    }
  }

  lockfile_close(&lf);
  return 0;
}

int pkg_why(const char *lockfile_path, const char *package_name,
            pkg_why_callback callback, void *user_data) {
  lockfile_t lf;
  if (!lockfile_open(lockfile_path, &lf))
    return -1;

  int count = 0;
  for (uint32_t i = 0; i < lf.header->package_count; i++) {
    const lockfile_package_t *pkg = &lf.packages[i];

    for (uint32_t j = 0; j < pkg->deps_count; j++) {
      const lockfile_dependency_t *dep = &lf.dependencies[pkg->deps_start + j];
      const lockfile_package_t *dep_pkg = &lf.packages[dep->package_index];
      size_t dep_len;
      const char *dep_name =
          lockfile_string_ref_slice(&lf, dep_pkg->name, &dep_len);
      char dep_buf[512] = {0};
      memcpy(dep_buf, dep_name, dep_len < 511 ? dep_len : 511);

      if (strcmp(dep_buf, package_name) == 0) {
        size_t name_len, con_len, pre_len;
        const char *pkg_name =
            lockfile_string_ref_slice(&lf, pkg->name, &name_len);
        const char *constraint =
            lockfile_string_ref_slice(&lf, dep->constraint, &con_len);
        const char *pre =
            lockfile_string_ref_slice(&lf, pkg->prerelease, &pre_len);

        char name_buf[512] = {0};
        char ver_buf[64] = {0};
        char con_buf[128] = {0};

        memcpy(name_buf, pkg_name, name_len < 511 ? name_len : 511);
        memcpy(con_buf, constraint, con_len < 127 ? con_len : 127);

        if (pre_len > 0) {
          char pre_buf[64] = {0};
          memcpy(pre_buf, pre, pre_len < 63 ? pre_len : 63);
          snprintf(ver_buf, sizeof(ver_buf), "%llu.%llu.%llu-%s",
                   pkg->version_major, pkg->version_minor, pkg->version_patch,
                   pre_buf);
        } else {
          snprintf(ver_buf, sizeof(ver_buf), "%llu.%llu.%llu",
                   pkg->version_major, pkg->version_minor, pkg->version_patch);
        }

        pkg_dep_type_t dep_type = {.peer = dep->flags.peer,
                                   .dev = dep->flags.dev || pkg->flags.dev,
                                   .optional = dep->flags.optional,
                                   .direct = pkg->flags.direct};
        callback(name_buf, ver_buf, con_buf, dep_type, user_data);
        count++;
      }
    }
  }

  // Check if direct
  for (uint32_t i = 0; i < lf.header->package_count; i++) {
    const lockfile_package_t *pkg = &lf.packages[i];
    size_t name_len;
    const char *pkg_name = lockfile_string_ref_slice(&lf, pkg->name, &name_len);
    char name_buf[512] = {0};
    memcpy(name_buf, pkg_name, name_len < 511 ? name_len : 511);

    if (strcmp(name_buf, package_name) == 0 && pkg->flags.direct) {
      pkg_dep_type_t dep_type = {.peer = false,
                                 .dev = pkg->flags.dev,
                                 .optional = false,
                                 .direct = true};
      callback("package.json", "", "dependencies", dep_type, user_data);
      count++;
    }
  }

  lockfile_close(&lf);
  return count;
}

// Info query API (not fully utilized by main runtime but implemented for
// conformance)
pkg_error_t pkg_info(pkg_context_t *ctx, const char *package_spec,
                     pkg_info_t *out) {
  (void)ctx;
  (void)package_spec;
  (void)out;
  return PKG_RESOLVE_ERROR;
}

uint32_t pkg_info_dist_tag_count(const pkg_context_t *ctx) {
  (void)ctx;
  return 0;
}

pkg_error_t pkg_info_get_dist_tag(const pkg_context_t *ctx, uint32_t index,
                                  pkg_dist_tag_t *out) {
  (void)ctx;
  (void)index;
  (void)out;
  return PKG_RESOLVE_ERROR;
}

uint32_t pkg_info_maintainer_count(const pkg_context_t *ctx) {
  (void)ctx;
  return 0;
}

pkg_error_t pkg_info_get_maintainer(const pkg_context_t *ctx, uint32_t index,
                                    pkg_maintainer_t *out) {
  (void)ctx;
  (void)index;
  (void)out;
  return PKG_RESOLVE_ERROR;
}

uint32_t pkg_info_dependency_count(const pkg_context_t *ctx) {
  (void)ctx;
  return 0;
}

pkg_error_t pkg_info_get_dependency(const pkg_context_t *ctx, uint32_t index,
                                    pkg_dependency_t *out) {
  (void)ctx;
  (void)index;
  (void)out;
  return PKG_RESOLVE_ERROR;
}

pkg_error_t pkg_exec_temp(pkg_context_t *ctx, const char *package_spec,
                          char *out_bin_path, size_t out_bin_path_len) {
  (void)ctx;
  (void)package_spec;
  (void)out_bin_path;
  (void)out_bin_path_len;
  return PKG_RESOLVE_ERROR;
}

pkg_error_t pkg_add_global(pkg_context_t *ctx, const char *package_spec) {
  (void)ctx;
  (void)package_spec;
  return PKG_RESOLVE_ERROR;
}

pkg_error_t pkg_add_global_many(pkg_context_t *ctx,
                                const char *const *package_specs,
                                uint32_t count) {
  (void)ctx;
  (void)package_specs;
  (void)count;
  return PKG_RESOLVE_ERROR;
}

pkg_error_t pkg_remove_global(pkg_context_t *ctx, const char *package_name) {
  (void)ctx;
  (void)package_name;
  return PKG_RESOLVE_ERROR;
}

uint32_t pkg_count_global(pkg_context_t *ctx) {
  (void)ctx;
  return 0;
}

uint32_t pkg_count_local(pkg_context_t *ctx) {
  (void)ctx;
  return 0;
}

pkg_error_t pkg_list_global(pkg_context_t *ctx,
                            pkg_global_list_callback callback,
                            void *user_data) {
  (void)ctx;
  (void)callback;
  (void)user_data;
  return PKG_RESOLVE_ERROR;
}

pkg_error_t pkg_list_local(pkg_context_t *ctx,
                           pkg_global_list_callback callback, void *user_data) {
  (void)ctx;
  (void)callback;
  (void)user_data;
  return PKG_RESOLVE_ERROR;
}
