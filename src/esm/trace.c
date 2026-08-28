#include <compat.h> // IWYU pragma: keep

#include "esm/trace.h"
#include "esm/loader.h"
#include "esm/library.h"
#include "esm/remote.h"
#include "esm/builtin_bundle.h"
#include "loader_internal.h"

#include "silver/ast.h"
#include "internal.h"
#include "runtime.h"
#include "utils.h"
#include "vfs_bundle.h"
#include "tokens.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <uv.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
  char *spec;
  bool is_require;
  bool lenient;
  bool optional;
  bool excluded;
  uint32_t line;
} trace_spec_t;

typedef struct {
  const char *key;
  char *owned_key;
  uint32_t idx;
  UT_hash_handle hh;
} trace_index_entry_t;

typedef struct {
  char *name;
  uint32_t name_len;
  char *value;
  bool ambiguous;
} trace_const_t;

typedef struct {
  ant_trace_result_t *result;
  const char *root;
  size_t root_len;

  trace_index_entry_t *module_map;
  trace_index_entry_t *edge_set;

  uint32_t try_depth;

  trace_spec_t *specs;
  uint32_t spec_count, spec_cap;

  trace_const_t *consts;
  uint32_t const_count, const_cap;
  
  uint32_t materialize_root_count, materialize_root_cap;
  uint32_t native_fallback_warning_count, native_fallback_warning_cap;

  char **materialize_roots;
  char **native_fallback_warnings;
  bool native_fallback_available;
} trace_ctx_t;

static void trace_set_error(ant_trace_result_t *r, const char *fmt, ...);
static int trace_find_module(trace_ctx_t *ctx, const char *abs_path);
static int trace_add_module(trace_ctx_t *ctx, const char *abs_path, bool lenient);

static void trace_index_free(trace_index_entry_t **map) {
  trace_index_entry_t *e, *tmp;
  HASH_ITER(hh, *map, e, tmp) {
    HASH_DEL(*map, e);
    free(e->owned_key);
    free(e);
  }
}

static bool trace_index_add(trace_index_entry_t **map, const char *key, char *owned_key, uint32_t idx) {
  trace_index_entry_t *e = calloc(1, sizeof(*e));
  if (!e) return false;
  e->key = key;
  e->owned_key = owned_key;
  e->idx = idx;
  HASH_ADD_KEYPTR(hh, *map, e->key, strlen(e->key), e);
  return true;
}

static bool trace_grow(void **items, uint32_t count, uint32_t *cap, size_t item_size) {
  if (count < *cap) return true;
  uint32_t next = *cap ? *cap * 2 : 16;
  void *grown = realloc(*items, (size_t)next * item_size);
  if (!grown) return false;
  *items = grown;
  *cap = next;
  return true;
}

static bool trace_path_under(const char *path, const char *root) {
  size_t root_len = strlen(root);
  return strncmp(path, root, root_len) == 0 &&
    (root_len == 1 || path[root_len] == '/' || path[root_len] == '\0');
}

static bool trace_path_is_materialized(trace_ctx_t *ctx, const char *path) {
  for (uint32_t i = 0; i < ctx->materialize_root_count; i++) {
    if (trace_path_under(path, ctx->materialize_roots[i])) return true;
  }
  return false;
}

__attribute__((format(printf, 2, 3)))
static void trace_set_error(ant_trace_result_t *r, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(r->error, sizeof(r->error), fmt, ap);
  va_end(ap);
}

__attribute__((format(printf, 2, 3)))
static void trace_warn(trace_ctx_t *ctx, const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  ant_trace_result_t *r = ctx->result;
  if (!trace_grow((void **)&r->warnings, r->warning_count, &r->warning_cap, sizeof(*r->warnings))) return;
  char *msg = strdup(buf);
  if (msg) r->warnings[r->warning_count++] = msg;
}

__attribute__((format(printf, 2, 3)))
static void trace_warn_native_fallback(trace_ctx_t *ctx, const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  if (!trace_grow(
        (void **)&ctx->native_fallback_warnings,
        ctx->native_fallback_warning_count,
        &ctx->native_fallback_warning_cap,
        sizeof(*ctx->native_fallback_warnings))) return;
  char *msg = strdup(buf);
  if (msg) ctx->native_fallback_warnings[ctx->native_fallback_warning_count++] = msg;
}

static void trace_native_fallback_warnings_finish(trace_ctx_t *ctx) {
  ant_trace_result_t *r = ctx->result;
  for (uint32_t i = 0; i < ctx->native_fallback_warning_count; i++) {
    char *msg = ctx->native_fallback_warnings[i];
    if (!ctx->native_fallback_available &&
        trace_grow((void **)&r->warnings, r->warning_count, &r->warning_cap, sizeof(*r->warnings))) {
      r->warnings[r->warning_count++] = msg;
    } else {
      free(msg);
    }
  }
  ctx->native_fallback_warning_count = 0;
  ctx->native_fallback_available = false;
}

static void trace_native_fallback_warnings_free(trace_ctx_t *ctx) {
  for (uint32_t i = 0; i < ctx->native_fallback_warning_count; i++)
    free(ctx->native_fallback_warnings[i]);
  free(ctx->native_fallback_warnings);
}

static const char *path_basename(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

int ant_trace_default_root(const char *entry_abs_path, char *out, size_t out_len) {
  char dir[4096];
  if ((size_t)snprintf(dir, sizeof(dir), "%s", entry_abs_path) >= sizeof(dir)) return -1;

  char *slash = strrchr(dir, '/');
  if (!slash) return -1;
  if (slash == dir) slash[1] = '\0';
  else slash[0] = '\0';

  char probe[4096];
  char best[4096];
  snprintf(best, sizeof(best), "%s", dir);

  for (;;) {
    if ((size_t)snprintf(probe, sizeof(probe), "%s/package.json", dir) < sizeof(probe) && access(probe, F_OK) == 0) {
      snprintf(best, sizeof(best), "%s", dir);
      break;
    }
    char *up = strrchr(dir, '/');
    if (!up || up == dir) break;
    *up = '\0';
  }

  if ((size_t)snprintf(out, out_len, "%s", best) >= out_len) return -1;
  return 0;
}

static char *trace_virtual_key(trace_ctx_t *ctx, const char *abs_path) {
  char key[4096];
  size_t rl = ctx->root_len;

  bool under_root = strncmp(abs_path, ctx->root, rl) == 0 && (rl == 1 || abs_path[rl] == '/');
  if (under_root) {
    const char *rel = abs_path + (rl == 1 ? 1 : rl + 1);
    if ((size_t)snprintf(key, sizeof(key), ANT_BUNDLE_KEY_PREFIX "%s", rel) >= sizeof(key)) return NULL;
  } else {
    if ((size_t)snprintf(key, sizeof(key), ANT_BUNDLE_KEY_PREFIX "__external__/%u/%s",
                         ctx->result->external_count++, path_basename(abs_path)) >= sizeof(key)) return NULL;
  }
  return strdup(key);
}

static int trace_find_module(trace_ctx_t *ctx, const char *abs_path) {
  trace_index_entry_t *e = NULL;
  HASH_FIND_STR(ctx->module_map, abs_path, e);
  return e ? (int)e->idx : -1;
}

static int trace_add_module(trace_ctx_t *ctx, const char *abs_path, bool lenient) {
  int existing = trace_find_module(ctx, abs_path);
  if (existing >= 0) {
    if (!lenient) ctx->result->modules[existing].lenient = false;
    return existing;
  }

  ant_trace_result_t *r = ctx->result;
  if (!trace_grow((void **)&r->modules, r->module_count, &r->module_cap, sizeof(*r->modules))) return -1;

  char *key = trace_virtual_key(ctx, abs_path);
  char *abs_copy = strdup(abs_path);
  if (!key || !abs_copy) {
    free(key);
    free(abs_copy);
    return -1;
  }

  r->modules[r->module_count] = (ant_trace_module_t){
    .abs_path = abs_copy,
    .key = key,
    .format = MODULE_EVAL_FORMAT_UNKNOWN,
    .kind = (uint8_t)ESM_MODULE_KIND_CODE,
    .bundle_flags = trace_path_is_materialized(ctx, abs_path)
      ? ANT_BUNDLE_MODULE_MATERIALIZE : 0,
    .lenient = lenient,
    .data = NULL,
    .data_len = 0,
  };

  if (!trace_index_add(&ctx->module_map, abs_copy, NULL, r->module_count)) {
    free(abs_copy);
    free(key);
    return -1;
  }
  return (int)r->module_count++;
}

static bool trace_has_suffix(const char *path, const char *suffix) {
  size_t path_len = strlen(path);
  size_t suffix_len = strlen(suffix);
  return path_len >= suffix_len &&
    strcmp(path + path_len - suffix_len, suffix) == 0;
}

static bool trace_is_shared_library(const char *path) {
  if (trace_has_suffix(path, ".dylib") || trace_has_suffix(path, ".dll")) return true;
  const char *so = strstr(path, ".so");
  return so && (so[3] == '\0' || so[3] == '.');
}

static bool trace_file_exists(const char *path) {
  uv_fs_t req;
  int rc = uv_fs_stat(NULL, &req, path, NULL);
  uv_fs_req_cleanup(&req);
  return rc == 0;
}

static bool trace_find_package_root(
  const char *file, bool fallback_to_dir, char *out, size_t out_len
) {
  char dir[PATH_MAX];
  if ((size_t)snprintf(dir, sizeof(dir), "%s", file) >= sizeof(dir)) return false;
  char *slash = strrchr(dir, '/');
  if (!slash) return false;
  *slash = '\0';

  char fallback[PATH_MAX];
  if ((size_t)snprintf(fallback, sizeof(fallback), "%s", dir) >= sizeof(fallback)) return false;

  for (;;) {
    char package_json[PATH_MAX];
    if ((size_t)snprintf(package_json, sizeof(package_json), "%s/package.json", dir) < sizeof(package_json) &&
        trace_file_exists(package_json)) {
      return (size_t)snprintf(out, out_len, "%s", dir) < out_len;
    }

    char *parent = strrchr(dir, '/');
    if (!parent || parent == dir) break;
    *parent = '\0';
  }

  return fallback_to_dir &&
    (size_t)snprintf(out, out_len, "%s", fallback) < out_len;
}

static int trace_tree_has_native(trace_ctx_t *ctx, const char *dir) {
  uv_fs_t req;
  int rc = uv_fs_scandir(NULL, &req, dir, 0, NULL);
  if (rc < 0) {
    trace_set_error(ctx->result, "cannot scan native package %s: %s", dir, uv_strerror(rc));
    uv_fs_req_cleanup(&req);
    return -1;
  }

  uv_dirent_t entry;
  while (uv_fs_scandir_next(&req, &entry) != UV_EOF) {
    if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0 ||
        strcmp(entry.name, "node_modules") == 0 || strcmp(entry.name, ".git") == 0)
      continue;

    char path[PATH_MAX];
    if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, entry.name) >= sizeof(path)) {
      trace_set_error(ctx->result, "native package path is too long: %s/%s", dir, entry.name);
      uv_fs_req_cleanup(&req);
      return -1;
    }

    if (entry.type == UV_DIRENT_DIR) {
      int found = trace_tree_has_native(ctx, path);
      if (found != 0) {
        uv_fs_req_cleanup(&req);
        return found;
      }
    } else if (entry.type == UV_DIRENT_FILE && trace_has_suffix(path, ".node")) {
      uv_fs_req_cleanup(&req);
      return 1;
    }
  }

  uv_fs_req_cleanup(&req);
  return 0;
}

static int trace_add_asset(
  trace_ctx_t *ctx, const char *abs_path, bool executable
) {
  int idx = trace_find_module(ctx, abs_path);
  bool existing = idx >= 0;
  if (!existing) idx = trace_add_module(ctx, abs_path, false);
  if (idx < 0) return -1;

  ant_trace_module_t *module = &ctx->result->modules[idx];
  module->bundle_flags |= ANT_BUNDLE_MODULE_MATERIALIZE;
  if (executable) module->bundle_flags |= ANT_BUNDLE_MODULE_EXECUTABLE;
  if (!existing) module->kind = (uint8_t)ESM_MODULE_KIND_ASSET;
  return idx;
}

static int trace_add_native_tree(trace_ctx_t *ctx, const char *dir) {
  uv_fs_t req;
  int rc = uv_fs_scandir(NULL, &req, dir, 0, NULL);
  if (rc < 0) {
    trace_set_error(ctx->result, "cannot scan native package %s: %s", dir, uv_strerror(rc));
    uv_fs_req_cleanup(&req);
    return -1;
  }

  uv_dirent_t entry;
  while (uv_fs_scandir_next(&req, &entry) != UV_EOF) {
    if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0 ||
        strcmp(entry.name, "node_modules") == 0 || strcmp(entry.name, ".git") == 0)
      continue;

    char path[PATH_MAX];
    if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, entry.name) >= sizeof(path)) {
      trace_set_error(ctx->result, "native package path is too long: %s/%s", dir, entry.name);
      uv_fs_req_cleanup(&req);
      return -1;
    }

    if (entry.type == UV_DIRENT_DIR) {
      if (trace_add_native_tree(ctx, path) != 0) {
        uv_fs_req_cleanup(&req);
        return -1;
      }
      continue;
    }
    if (entry.type != UV_DIRENT_FILE) continue;

    if (trace_has_suffix(path, ".node")) {
      int idx = trace_add_module(ctx, path, false);
      if (idx < 0) {
        uv_fs_req_cleanup(&req);
        return -1;
      }
      ctx->result->modules[idx].kind = (uint8_t)ESM_MODULE_KIND_NATIVE;
      ctx->result->modules[idx].bundle_flags |= ANT_BUNDLE_MODULE_MATERIALIZE;
      continue;
    }

    uv_fs_t stat_req;
    int stat_rc = uv_fs_stat(NULL, &stat_req, path, NULL);
    bool executable = stat_rc == 0 && (stat_req.statbuf.st_mode & 0111u) != 0;
    uv_fs_req_cleanup(&stat_req);
    if ((executable || trace_is_shared_library(path)) &&
        trace_add_asset(ctx, path, executable) < 0) {
      uv_fs_req_cleanup(&req);
      return -1;
    }
  }

  uv_fs_req_cleanup(&req);
  return 0;
}

static int trace_register_native_package(
  trace_ctx_t *ctx, const char *file, bool fallback_to_dir
) {
  char root[PATH_MAX];
  if (!trace_find_package_root(file, fallback_to_dir, root, sizeof(root))) return 0;

  for (uint32_t i = 0; i < ctx->materialize_root_count; i++) {
    if (strcmp(ctx->materialize_roots[i], root) == 0) return 1;
  }

  int has_native = trace_tree_has_native(ctx, root);
  if (has_native <= 0) return has_native;

  if (!trace_grow(
        (void **)&ctx->materialize_roots, ctx->materialize_root_count,
        &ctx->materialize_root_cap, sizeof(*ctx->materialize_roots))) return -1;
  char *root_copy = strdup(root);
  if (!root_copy) return -1;
  ctx->materialize_roots[ctx->materialize_root_count++] = root_copy;

  for (uint32_t i = 0; i < ctx->result->module_count; i++) {
    if (trace_path_under(ctx->result->modules[i].abs_path, root))
      ctx->result->modules[i].bundle_flags |= ANT_BUNDLE_MODULE_MATERIALIZE;
  }

  if (trace_add_native_tree(ctx, root) != 0) return -1;
  return 1;
}

static bool trace_add_edge(trace_ctx_t *ctx, uint32_t parent, const char *spec, uint32_t child, bool is_require) {
  ant_trace_result_t *r = ctx->result;

  size_t edge_key_len = strlen(spec) + 48;
  char *edge_key = malloc(edge_key_len);
  if (!edge_key) return false;
  snprintf(edge_key, edge_key_len, "%u|%u|%d|%s", parent, child, is_require ? 1 : 0, spec);

  trace_index_entry_t *existing = NULL;
  HASH_FIND_STR(ctx->edge_set, edge_key, existing);
  if (existing) {
    free(edge_key);
    return true;
  }

  if (!trace_grow((void **)&r->edges, r->edge_count, &r->edge_cap, sizeof(*r->edges))) {
    free(edge_key);
    return false;
  }
  char *spec_copy = strdup(spec);
  if (!spec_copy) {
    free(edge_key);
    return false;
  }

  if (!trace_index_add(&ctx->edge_set, edge_key, edge_key, r->edge_count)) {
    free(edge_key);
    free(spec_copy);
    return false;
  }

  r->edges[r->edge_count++] = (ant_trace_edge_t){ parent, spec_copy, child, is_require };
  return true;
}

static bool trace_push_spec(trace_ctx_t *ctx, const char *spec, uint32_t len, bool is_require, bool lenient, uint32_t line) {
  if (!trace_grow((void **)&ctx->specs, ctx->spec_count, &ctx->spec_cap, sizeof(*ctx->specs))) return false;
  char *copy = strndup(spec, len);
  if (!copy) return false;
  ctx->specs[ctx->spec_count++] = (trace_spec_t){
    .spec = copy,
    .is_require = is_require,
    .lenient = lenient,
    .optional = ctx->try_depth > 0,
    .line = line,
  };
  return true;
}

static bool node_is_ident(const sv_ast_t *node, const char *name, uint32_t len) {
  return node && node->type == N_IDENT && node->len == len && node->str && strncmp(node->str, name, len) == 0;
}

static bool node_prop_is(const sv_ast_t *node, const char *name, uint32_t len) {
  return node && node->len == len && node->str && strncmp(node->str, name, len) == 0;
}

static bool node_is_import_meta_url(const sv_ast_t *node) {
  if (!node || node->type != N_MEMBER || !node_prop_is(node->right, "url", 3)) return false;
  const sv_ast_t *meta = node->left;
  if (!meta || meta->type != N_MEMBER || !node_prop_is(meta->right, "meta", 4)) return false;
  return node_is_ident(meta->left, "import", 6);
}

static bool node_is_require_ident(const sv_ast_t *node) {
  return node && node->type == N_IDENT && node->len == 7 && node->str && strncmp(node->str, "require", 7) == 0;
}

static const char *trace_target_platform(void) {
#if defined(__APPLE__)
  return "darwin";
#elif defined(__linux__)
  return "linux";
#elif defined(_WIN32)
  return "win32";
#elif defined(__FreeBSD__)
  return "freebsd";
#else
  return "unknown";
#endif
}

static const char *trace_target_arch(void) {
#if defined(__x86_64__) || defined(_M_X64)
  return "x64";
#elif defined(__i386__) || defined(_M_IX86)
  return "ia32";
#elif defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#elif defined(__arm__) || defined(_M_ARM)
  return "arm";
#else
  return "unknown";
#endif
}

static char *trace_darwin_arch_spec(const char *spec) {
  if (strcmp(trace_target_platform(), "darwin") != 0) return NULL;

  static const char package_marker[] = "-darwin-universal";
  static const char file_suffix[] = ".darwin-universal.node";
  const char *marker = NULL;
  const char *tail = NULL;
  char replacement[64];

  const char *package = strstr(spec, package_marker);
  const char *package_tail = package ? package + strlen(package_marker) : NULL;
  if (package && (*package_tail == '\0' || *package_tail == '/')) {
    marker = package;
    tail = package_tail;
    snprintf(replacement, sizeof(replacement), "-darwin-%s", trace_target_arch());
  } else if (trace_has_suffix(spec, file_suffix)) {
    marker = spec + strlen(spec) - strlen(file_suffix);
    tail = spec + strlen(spec);
    snprintf(replacement, sizeof(replacement), ".darwin-%s.node", trace_target_arch());
  } else {
    return NULL;
  }

  size_t prefix_len = (size_t)(marker - spec);
  size_t replacement_len = strlen(replacement);
  size_t tail_len = strlen(tail);
  char *target = malloc(prefix_len + replacement_len + tail_len + 1);
  if (!target) return NULL;
  memcpy(target, spec, prefix_len);
  memcpy(target + prefix_len, replacement, replacement_len);
  memcpy(target + prefix_len + replacement_len, tail, tail_len + 1);
  return target;
}

static void trace_consts_clear(trace_ctx_t *ctx) {
  for (uint32_t i = 0; i < ctx->const_count; i++) {
    free(ctx->consts[i].name);
    free(ctx->consts[i].value);
  }
  ctx->const_count = 0;
}

static const char *trace_const_lookup(
  trace_ctx_t *ctx, const char *name, uint32_t name_len
) {
  for (uint32_t i = 0; i < ctx->const_count; i++) {
    trace_const_t *entry = &ctx->consts[i];
    if (entry->name_len == name_len && memcmp(entry->name, name, name_len) == 0)
      return entry->ambiguous ? NULL : entry->value;
  }
  return NULL;
}

static bool trace_const_add(
  trace_ctx_t *ctx, const char *name, uint32_t name_len, char *value
) {
  for (uint32_t i = 0; i < ctx->const_count; i++) {
    trace_const_t *entry = &ctx->consts[i];
    if (entry->name_len != name_len || memcmp(entry->name, name, name_len) != 0)
      continue;
    if (!entry->ambiguous && strcmp(entry->value, value) != 0) {
      free(entry->value);
      entry->value = NULL;
      entry->ambiguous = true;
    }
    free(value);
    return true;
  }

  if (!trace_grow(
        (void **)&ctx->consts, ctx->const_count,
        &ctx->const_cap, sizeof(*ctx->consts))) {
    free(value);
    return false;
  }

  char *name_copy = strndup(name, name_len);
  if (!name_copy) {
    free(value);
    return false;
  }
  ctx->consts[ctx->const_count++] = (trace_const_t){
    .name = name_copy,
    .name_len = name_len,
    .value = value,
  };
  return true;
}

static char *trace_join_strings(const char *left, const char *right) {
  size_t left_len = strlen(left);
  size_t right_len = strlen(right);
  if (left_len > SIZE_MAX - right_len - 1) return NULL;
  char *joined = malloc(left_len + right_len + 1);
  if (!joined) return NULL;
  memcpy(joined, left, left_len);
  memcpy(joined + left_len, right, right_len + 1);
  return joined;
}

static char *trace_eval_static_string(trace_ctx_t *ctx, const sv_ast_t *node) {
  if (!node) return NULL;
  if (node->type == N_STRING && node->str) return strndup(node->str, node->len);

  if (node->type == N_IDENT && node->str) {
    const char *value = trace_const_lookup(ctx, node->str, node->len);
    return value ? strdup(value) : NULL;
  }

  if (node->type == N_MEMBER && node_is_ident(node->left, "process", 7)) {
    if (node_prop_is(node->right, "platform", 8)) return strdup(trace_target_platform());
    if (node_prop_is(node->right, "arch", 4)) return strdup(trace_target_arch());
  }

  if (node->type == N_BINARY && node->op == TOK_PLUS) {
    char *left = trace_eval_static_string(ctx, node->left);
    if (!left) return NULL;
    char *right = trace_eval_static_string(ctx, node->right);
    if (!right) {
      free(left);
      return NULL;
    }
    char *joined = trace_join_strings(left, right);
    free(left);
    free(right);
    return joined;
  }

  if (node->type == N_TEMPLATE) {
    char *result = strdup("");
    if (!result) return NULL;
    for (int i = 0; i < node->args.count; i++) {
      char *part = trace_eval_static_string(ctx, node->args.items[i]);
      if (!part) {
        free(result);
        return NULL;
      }
      char *joined = trace_join_strings(result, part);
      free(result);
      free(part);
      if (!joined) return NULL;
      result = joined;
    }
    return result;
  }

  return NULL;
}

static bool trace_eval_static_bool(
  trace_ctx_t *ctx, const sv_ast_t *node, bool *out
) {
  if (!node) return false;
  if (node->type == N_BOOL) {
    *out = node->num != 0;
    return true;
  }
  if (node->type == N_UNARY && node->op == TOK_NOT) {
    bool value;
    if (!trace_eval_static_bool(ctx, node->right ? node->right : node->left, &value))
      return false;
    *out = !value;
    return true;
  }
  if (node->type != N_BINARY ||
      (node->op != TOK_EQ && node->op != TOK_NE &&
       node->op != TOK_SEQ && node->op != TOK_SNE)) return false;

  char *left = trace_eval_static_string(ctx, node->left);
  if (!left) return false;
  char *right = trace_eval_static_string(ctx, node->right);
  if (!right) {
    free(left);
    return false;
  }

  bool equal = strcmp(left, right) == 0;
  free(left);
  free(right);
  *out = (node->op == TOK_EQ || node->op == TOK_SEQ) ? equal : !equal;
  return true;
}

static bool trace_bytes_contain(
  const char *value, uint32_t value_len, const char *needle
) {
  size_t needle_len = strlen(needle);
  if (!value || value_len < needle_len) return false;
  for (uint32_t i = 0; i <= value_len - needle_len; i++) {
    if (memcmp(value + i, needle, needle_len) == 0) return true;
  }
  return false;
}

static bool trace_node_mentions_native_addon(const sv_ast_t *node) {
  if (!node) return false;
  if (node->type == N_STRING &&
      (trace_bytes_contain(node->str, node->len, ".node") ||
       trace_bytes_contain(node->aux, node->aux_len, ".node"))) return true;

  if (trace_node_mentions_native_addon(node->left) ||
      trace_node_mentions_native_addon(node->right) ||
      trace_node_mentions_native_addon(node->cond) ||
      trace_node_mentions_native_addon(node->body) ||
      trace_node_mentions_native_addon(node->catch_param) ||
      trace_node_mentions_native_addon(node->catch_body) ||
      trace_node_mentions_native_addon(node->finally_body) ||
      trace_node_mentions_native_addon(node->init) ||
      trace_node_mentions_native_addon(node->update)) return true;
  for (int i = 0; i < node->args.count; i++) {
    if (trace_node_mentions_native_addon(node->args.items[i])) return true;
  }
  return false;
}

static bool trace_scan_ast(trace_ctx_t *ctx, const char *file, const sv_ast_t *node) {
  if (!node) return true;

  switch (node->type) {
    case N_IMPORT_DECL:
      if (node->right && node->right->type == N_STRING && node->right->str) {
        if (!trace_push_spec(ctx, node->right->str, node->right->len, false, false, node->line)) return false;
      }
      break;

    case N_EXPORT:
      if ((node->flags & EX_FROM) && node->right && node->right->type == N_STRING && node->right->str) {
        if (!trace_push_spec(ctx, node->right->str, node->right->len, false, false, node->line)) return false;
      }
      break;

    case N_IMPORT:
      if (node->right) {
        char *spec = trace_eval_static_string(ctx, node->right);
        if (spec) {
          bool pushed = trace_push_spec(ctx, spec, (uint32_t)strlen(spec), false, false, node->line);
          free(spec);
          if (!pushed) return false;
        } else {
          if (ctx->try_depth > 0) {
            trace_warn_native_fallback(ctx,
              "%s:%u: import() with a non-constant specifier is not traced inside try/catch; it will fail at runtime unless the target is bundled",
              file, node->line);
          } else {
            trace_warn(ctx,
              "%s:%u: import() with a non-constant specifier is not traced; it will fail at runtime unless the target is bundled",
              file, node->line);
          }
        }
      }
      break;

    case N_CALL:
      if (node_is_require_ident(node->left) && node->args.count >= 1) {
        const sv_ast_t *arg = node->args.items[0];
        char *spec = trace_eval_static_string(ctx, arg);
        if (spec) {
          bool pushed = trace_push_spec(ctx, spec, (uint32_t)strlen(spec), true, false, node->line);
          free(spec);
          if (!pushed) return false;
        } else {
          int native_package = trace_node_mentions_native_addon(arg)
            ? trace_register_native_package(ctx, file, false) : 0;
          if (native_package < 0) return false;
          if (native_package > 0) ctx->native_fallback_available = true;
          if (native_package == 0) {
            if (ctx->try_depth > 0) {
              trace_warn_native_fallback(ctx,
                "%s:%u: require() with a non-constant specifier is not traced inside try/catch; it will fail at runtime unless the target is bundled",
                file, node->line);
            } else {
              trace_warn(ctx,
                "%s:%u: require() with a non-constant specifier is not traced; it will fail at runtime unless the target is bundled",
                file, node->line);
            }
          }
        }
      }
      break;

    case N_VAR:
      if (node->var_kind == SV_VAR_CONST) {
        for (int i = 0; i < node->args.count; i++) {
          const sv_ast_t *decl = node->args.items[i];
          if (!decl || decl->type != N_VARDECL || !decl->left ||
              decl->left->type != N_IDENT || !decl->left->str || !decl->right) continue;
          char *value = trace_eval_static_string(ctx, decl->right);
          if (value && !trace_const_add(
                ctx, decl->left->str, decl->left->len, value)) return false;
        }
      }
      break;

    case N_IF: {
      bool condition;
      if (trace_eval_static_bool(ctx, node->cond, &condition)) {
        if (!trace_scan_ast(ctx, file, node->cond)) return false;
        return trace_scan_ast(ctx, file, condition ? node->left : node->right);
      }
      break;
    }

    case N_TRY: {
      ctx->try_depth++;
      bool try_ok = trace_scan_ast(ctx, file, node->body);
      ctx->try_depth--;
      if (!try_ok) return false;
      if (!trace_scan_ast(ctx, file, node->catch_param)) return false;
      if (!trace_scan_ast(ctx, file, node->catch_body)) return false;
      return trace_scan_ast(ctx, file, node->finally_body);
    }

    case N_NEW:
      if (node_is_ident(node->left, "URL", 3) && node->args.count >= 2) {
        const sv_ast_t *spec = node->args.items[0];
        if (spec && spec->type == N_STRING && spec->str && node_is_import_meta_url(node->args.items[1])) {
          if (!trace_push_spec(ctx, spec->str, spec->len, false, true, node->line)) return false;
        }
      }
      break;

    default:
      break;
  }

  if (!trace_scan_ast(ctx, file, node->left)) return false;
  if (!trace_scan_ast(ctx, file, node->right)) return false;
  if (!trace_scan_ast(ctx, file, node->cond)) return false;
  if (!trace_scan_ast(ctx, file, node->body)) return false;
  if (!trace_scan_ast(ctx, file, node->catch_param)) return false;
  if (!trace_scan_ast(ctx, file, node->catch_body)) return false;
  if (!trace_scan_ast(ctx, file, node->finally_body)) return false;
  if (!trace_scan_ast(ctx, file, node->init)) return false;
  if (!trace_scan_ast(ctx, file, node->update)) return false;
  for (int i = 0; i < node->args.count; i++) {
    if (!trace_scan_ast(ctx, file, node->args.items[i])) return false;
  }
  return true;
}

static sv_ast_t *trace_parse(ant_t *js, const char *code, size_t len) {
  bool saved_thrown_exists = js->thrown_exists;
  ant_value_t saved_thrown_value = js->thrown_value;
  ant_value_t saved_thrown_stack = js->thrown_stack;

  sv_ast_t *program = sv_parse(js, code, (ant_offset_t)len, false);
  if (!program) {
    js->thrown_exists = saved_thrown_exists;
    js->thrown_value = saved_thrown_value;
    js->thrown_stack = saved_thrown_stack;
  }
  return program;
}

static sv_ast_t *trace_parse_cjs_wrapped(ant_t *js, const char *code, size_t len) {
  static const char prefix[] = "(function(require,module,exports,__filename,__dirname){";
  static const char suffix[] = "\n})";

  size_t total = sizeof(prefix) - 1 + len + sizeof(suffix) - 1;
  char *wrapped = malloc(total + 1);
  if (!wrapped) return NULL;

  memcpy(wrapped, prefix, sizeof(prefix) - 1);
  memcpy(wrapped + sizeof(prefix) - 1, code, len);
  memcpy(wrapped + sizeof(prefix) - 1 + len, suffix, sizeof(suffix));

  sv_ast_t *program = trace_parse(js, wrapped, total);
  free(wrapped);
  return program;
}

static bool trace_spec_resolves_native(
  ant_t *js, const char *parent_path, const trace_spec_t *sp
) {
  char *resolved = esm_resolve(
    js, sp->spec, parent_path,
    sp->is_require ? esm_resolve_path_require : esm_resolve_path
  );
  if (!resolved) return false;
  bool native = trace_has_suffix(resolved, ".node");
  free(resolved);
  return native;
}

static bool trace_matching_spec_resolves_native(
  ant_t *js, trace_ctx_t *ctx, const char *parent_path,
  const char *spec, bool is_require
) {
  for (uint32_t i = 0; i < ctx->spec_count; i++) {
    trace_spec_t *candidate = &ctx->specs[i];
    if (candidate->is_require == is_require && strcmp(candidate->spec, spec) == 0)
      return trace_spec_resolves_native(js, parent_path, candidate);
  }
  return false;
}

static void trace_exclude_redundant_universal_native(
  ant_t *js, trace_ctx_t *ctx, uint32_t parent_idx
) {
  const char *parent_path = ctx->result->modules[parent_idx].abs_path;

  for (uint32_t i = 0; i < ctx->spec_count; i++) {
    trace_spec_t *universal = &ctx->specs[i];
    if (!universal->optional) continue;

    char *target_spec = trace_darwin_arch_spec(universal->spec);
    if (!target_spec) continue;

    universal->excluded = trace_matching_spec_resolves_native(
      js, ctx, parent_path, target_spec, universal->is_require
    );

    static const char package_json_suffix[] = "/package.json";
    if (!universal->excluded && trace_has_suffix(target_spec, package_json_suffix)) {
      size_t base_len = strlen(target_spec) - strlen(package_json_suffix);
      char *target_base = strndup(target_spec, base_len);
      if (target_base) {
        universal->excluded = trace_matching_spec_resolves_native(
          js, ctx, parent_path, target_base, universal->is_require
        );
        free(target_base);
      }
    }
    free(target_spec);
  }
}

static int trace_resolve_spec(ant_t *js, trace_ctx_t *ctx, uint32_t parent_idx, const trace_spec_t *sp) {
  ant_trace_result_t *r = ctx->result;
  const char *parent_path = r->modules[parent_idx].abs_path;
  const char *spec = sp->spec;
  size_t spec_len = strlen(spec);

  if (esm_lookup_builtin_alias(spec, spec_len)) return 0;
  if (esm_has_builtin_scheme(spec)) return 0;
  if (js_esm_is_registered_library(spec, spec_len)) return 0;
  if (esm_is_data_url(spec)) return 0;

  char *file_url_path = esm_file_url_to_path(js, spec);
  if (!file_url_path && esm_is_url(spec)) {
    if (sp->lenient) return 0;
    if (sp->optional) {
      trace_warn(ctx, "%s:%u: remote import \"%s\" inside try/catch was not bundled; it will throw at runtime", parent_path, sp->line, spec);
      return 0;
    }
    trace_set_error(r, "%s:%u: cannot compile remote import \"%s\"", parent_path, sp->line, spec);
    return -1;
  }

  char *resolved = esm_resolve(
    js, file_url_path ? file_url_path : spec, parent_path,
    sp->is_require ? esm_resolve_path_require : esm_resolve_path
  );
  free(file_url_path);

  if (!resolved) {
    if (sp->lenient) {
      trace_warn(ctx, "%s:%u: new URL(\"%s\", import.meta.url) target was not bundled", parent_path, sp->line, spec);
      return 0;
    }
    if (sp->optional) {
      trace_warn_native_fallback(ctx,
        "%s:%u: optional module \"%s\" was not found; the require/import inside try/catch will throw at runtime",
        parent_path, sp->line, spec);
      return 0;
    }
    trace_set_error(r, "%s:%u: cannot resolve module \"%s\"", parent_path, sp->line, spec);
    return -1;
  }

  size_t rlen = strlen(resolved);
  if (rlen > 5 && strcmp(resolved + rlen - 5, ".node") == 0) {
    int native_package = trace_register_native_package(ctx, resolved, true);
    if (native_package <= 0) {
      if (native_package == 0)
        trace_set_error(r, "%s:%u: cannot determine native package assets for \"%s\"", parent_path, sp->line, spec);
      else if (!r->error[0])
        trace_set_error(r, "out of memory while tracing native package assets");
      free(resolved);
      return -1;
    }
    ctx->native_fallback_available = true;
  }

  int child = trace_add_module(ctx, resolved, sp->lenient);
  free(resolved);
  if (child < 0) {
    trace_set_error(r, "out of memory while tracing module graph");
    return -1;
  }

  if (!trace_add_edge(ctx, parent_idx, spec, (uint32_t)child, sp->is_require)) {
    trace_set_error(r, "out of memory while tracing module graph");
    return -1;
  }
  return 0;
}

static int trace_process_module(ant_t *js, trace_ctx_t *ctx, uint32_t idx) {
  ant_trace_result_t *r = ctx->result;
  const char *abs_path = r->modules[idx].abs_path;

  esm_module_kind_t kind = r->modules[idx].kind == (uint8_t)ESM_MODULE_KIND_ASSET
    ? ESM_MODULE_KIND_ASSET : esm_classify_kind_for_path(abs_path);
  if (kind == ESM_MODULE_KIND_NATIVE &&
      !(r->modules[idx].bundle_flags & ANT_BUNDLE_MODULE_MATERIALIZE)) {
    trace_set_error(r, "native addon %s cannot be embedded in a compiled executable", abs_path);
    return -1;
  }
  if (kind == ESM_MODULE_KIND_NONE || kind == ESM_MODULE_KIND_URL) kind = ESM_MODULE_KIND_CODE;
  r->modules[idx].kind = (uint8_t)kind;

  esm_file_data_t file;
  ant_value_t err = esm_read_file(js, abs_path, "module", &file);
  if (is_err(err)) {
    trace_set_error(r, "cannot read %s", abs_path);
    return -1;
  }
  char *content = file.data;
  size_t size = file.size;
  content[size] = '\0';

  if (kind != ESM_MODULE_KIND_CODE) {
    r->modules[idx].data = (uint8_t *)content;
    r->modules[idx].data_len = size;
    return 0;
  }

  uint8_t *original = NULL;
  if (r->modules[idx].lenient) {
    original = malloc(size + 1);
    if (!original) {
      trace_set_error(r, "out of memory while tracing module graph");
      free(content);
      return -1;
    }
    memcpy(original, content, size + 1);
  }

  size_t js_len = size;
  const char *strip_detail = NULL;
  int strip_result = strip_typescript_inplace(&content, size, abs_path, &js_len, &strip_detail);
  if (strip_result < 0) {
    trace_set_error(r, "TypeScript error in %s: %s", abs_path, strip_detail ? strip_detail : "strip failed");
    free(content);
    free(original);
    return -1;
  }

  r->modules[idx].data = (uint8_t *)content;
  r->modules[idx].data_len = js_len;

  ant_module_format_t format = esm_decide_module_format(js, abs_path);

  code_arena_mark_t mark = parse_arena_mark();
  sv_ast_t *program = trace_parse(js, content, js_len);

  if (program && format == MODULE_EVAL_FORMAT_UNKNOWN) {
    format = (program->flags & FN_MODULE_SYNTAX) ? MODULE_EVAL_FORMAT_ESM : MODULE_EVAL_FORMAT_CJS;
  }

  if (!program && r->modules[idx].lenient) {
    parse_arena_rewind(mark);
    free(content);
    r->modules[idx].data = original;
    r->modules[idx].data_len = size;
    r->modules[idx].kind = (uint8_t)ESM_MODULE_KIND_TEXT;
    r->modules[idx].format = MODULE_EVAL_FORMAT_UNKNOWN;
    r->modules[idx].lenient_text = true;
    return 0;
  }
  free(original);

  if (!program) {
    if (format == MODULE_EVAL_FORMAT_ESM) {
      trace_set_error(r, "failed to parse module %s", abs_path);
      parse_arena_rewind(mark);
      return -1;
    }
    format = MODULE_EVAL_FORMAT_CJS;
    program = trace_parse_cjs_wrapped(js, content, js_len);
    if (!program) {
      trace_set_error(r, "failed to parse module %s", abs_path);
      parse_arena_rewind(mark);
      return -1;
    }
  }

  r->modules[idx].format = (uint8_t)format;

  for (uint32_t i = 0; i < ctx->spec_count; i++) free(ctx->specs[i].spec);
  ctx->spec_count = 0;
  ctx->try_depth = 0;
  trace_native_fallback_warnings_finish(ctx);
  trace_consts_clear(ctx);

  bool scan_ok = trace_scan_ast(ctx, abs_path, program);
  parse_arena_rewind(mark);

  if (!scan_ok) {
    if (!r->error[0]) trace_set_error(r, "out of memory while tracing module graph");
    return -1;
  }

  trace_exclude_redundant_universal_native(js, ctx, idx);
  for (uint32_t i = 0; i < ctx->spec_count; i++) {
    if (ctx->specs[i].excluded) continue;
    if (trace_resolve_spec(js, ctx, idx, &ctx->specs[i]) != 0) return -1;
  }
  trace_native_fallback_warnings_finish(ctx);
  return 0;
}

int ant_esm_trace_graph(ant_t *js, const char *entry_abs_path, const char *root_dir, ant_trace_result_t *out) {
  memset(out, 0, sizeof(*out));

  trace_ctx_t ctx = {
    .result = out,
    .root = root_dir,
    .root_len = strlen(root_dir),
  };
  while (ctx.root_len > 1 && root_dir[ctx.root_len - 1] == '/') ctx.root_len--;

  char root_copy[4096];
  if (ctx.root_len >= sizeof(root_copy)) {
    trace_set_error(out, "pack root path too long");
    return -1;
  }
  memcpy(root_copy, root_dir, ctx.root_len);
  root_copy[ctx.root_len] = '\0';
  ctx.root = root_copy;

  int entry = trace_add_module(&ctx, entry_abs_path, false);
  if (entry < 0) {
    trace_set_error(out, "out of memory while tracing module graph");
    goto fail;
  }
  out->entry_idx = (uint32_t)entry;

  uint32_t processed = 0;
  for (;;) {
    while (processed < out->module_count) {
      if (trace_process_module(js, &ctx, processed) != 0) goto fail;
      processed++;
    }

    bool reprocessed = false;
    for (uint32_t i = 0; i < processed; i++) {
      ant_trace_module_t *m = &out->modules[i];
      if (!m->lenient_text || m->lenient) continue;

      free(m->data);
      m->data = NULL;
      m->data_len = 0;
      m->kind = (uint8_t)ESM_MODULE_KIND_CODE;
      m->format = MODULE_EVAL_FORMAT_UNKNOWN;
      m->lenient_text = false;

      if (trace_process_module(js, &ctx, i) != 0) goto fail;
      reprocessed = true;
    }

    if (!reprocessed && processed == out->module_count) break;
  }

  for (uint32_t i = 0; i < ctx.spec_count; i++) free(ctx.specs[i].spec);
  free(ctx.specs);
  trace_consts_clear(&ctx);
  free(ctx.consts);
  for (uint32_t i = 0; i < ctx.materialize_root_count; i++)
    free(ctx.materialize_roots[i]);
  free(ctx.materialize_roots);
  trace_native_fallback_warnings_free(&ctx);
  trace_index_free(&ctx.module_map);
  trace_index_free(&ctx.edge_set);
  return 0;

fail:
  for (uint32_t i = 0; i < ctx.spec_count; i++) free(ctx.specs[i].spec);
  free(ctx.specs);
  trace_consts_clear(&ctx);
  free(ctx.consts);
  for (uint32_t i = 0; i < ctx.materialize_root_count; i++)
    free(ctx.materialize_roots[i]);
  free(ctx.materialize_roots);
  trace_native_fallback_warnings_free(&ctx);
  trace_index_free(&ctx.module_map);
  trace_index_free(&ctx.edge_set);
  return -1;
}

void ant_esm_trace_free(ant_trace_result_t *result) {
  for (uint32_t i = 0; i < result->module_count; i++) {
    free(result->modules[i].abs_path);
    free(result->modules[i].key);
    free(result->modules[i].data);
  }
  free(result->modules);

  for (uint32_t i = 0; i < result->edge_count; i++) free(result->edges[i].spec);
  free(result->edges);

  for (uint32_t i = 0; i < result->warning_count; i++) free(result->warnings[i]);
  free(result->warnings);

  memset(result, 0, sizeof(*result));
}
