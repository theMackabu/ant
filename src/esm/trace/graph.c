#include "gc/roots.h"
#include <compat.h> // IWYU pragma: keep

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esm/builtin_bundle.h"
#include "esm/loader.h"
#include "esm/remote.h"
#include "../loader_internal.h"

#include "internal.h"
#include "runtime.h"
#include "utils.h"
#include "vfs_bundle.h"

#include "trace_internal.h"

bool trace_grow(void **items, uint32_t count, uint32_t *cap, size_t item_size) {
  if (count < *cap) return true;
  uint32_t next = *cap ? *cap * 2 : 16;
  void *grown = realloc(*items, (size_t)next * item_size);
  if (!grown) return false;
  *items = grown;
  *cap = next;
  return true;
}

bool trace_has_suffix(const char *path, const char *suffix) {
  size_t path_len = strlen(path), suffix_len = strlen(suffix);
  return path_len >= suffix_len && strcmp(path + path_len - suffix_len, suffix) == 0;
}

bool trace_path_under(const char *path, const char *root) {
  size_t root_len = strlen(root);
  return strncmp(path, root, root_len) == 0 &&
    (root_len == 1 || path[root_len] == '/' || path[root_len] == '\0');
}

void trace_set_error(ant_trace_result_t *r, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(r->error, sizeof(r->error), fmt, ap);
  va_end(ap);
}

static void trace_push_warning(trace_ctx_t *ctx, char *msg) {
  ant_trace_result_t *r = ctx->result;
  if (msg && trace_grow((void **)&r->warnings, r->warning_count, &r->warning_cap, sizeof(*r->warnings)))
    r->warnings[r->warning_count++] = msg;
  else free(msg);
}

__attribute__((format(printf, 1, 0)))
static char *trace_format(const char *fmt, va_list ap) {
  char buf[1024];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  return strdup(buf);
}

void trace_warn(trace_ctx_t *ctx, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  trace_push_warning(ctx, trace_format(fmt, ap));
  va_end(ap);
}

void trace_warn_deferred(trace_ctx_t *ctx, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *msg = trace_format(fmt, ap);
  va_end(ap);

  if (msg && trace_grow((void **)&ctx->deferred_warnings, ctx->deferred_warning_count,
        &ctx->deferred_warning_cap, sizeof(*ctx->deferred_warnings)))
    ctx->deferred_warnings[ctx->deferred_warning_count++] = msg;
  else free(msg);
}

void trace_warnings_settle(trace_ctx_t *ctx) {
  for (uint32_t i = 0; i < ctx->deferred_warning_count; i++) {
    if (ctx->native_fallback_available) free(ctx->deferred_warnings[i]);
    else trace_push_warning(ctx, ctx->deferred_warnings[i]);
  }
  ctx->deferred_warning_count = 0;
  ctx->native_fallback_available = false;
}

void trace_warnings_free(trace_ctx_t *ctx) {
  for (uint32_t i = 0; i < ctx->deferred_warning_count; i++) free(ctx->deferred_warnings[i]);
  free(ctx->deferred_warnings);
  ctx->deferred_warnings = NULL;
  ctx->deferred_warning_count = ctx->deferred_warning_cap = 0;
}

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
  *e = (trace_index_entry_t){ .key = key, .owned_key = owned_key, .idx = idx };
  HASH_ADD_KEYPTR(hh, *map, e->key, strlen(e->key), e);
  return true;
}

static const char *trace_basename(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

int ant_trace_default_root(const char *entry_abs_path, char *out, size_t out_len) {
  char dir[PATH_MAX];
  if ((size_t)snprintf(dir, sizeof(dir), "%s", entry_abs_path) >= sizeof(dir)) return -1;

  char *slash = strrchr(dir, '/');
  if (!slash) return -1;
  if (slash == dir) slash[1] = '\0';
  else slash[0] = '\0';

  char best[PATH_MAX];
  snprintf(best, sizeof(best), "%s", dir);

  for (;;) {
    char probe[PATH_MAX];
    if ((size_t)snprintf(probe, sizeof(probe), "%s/package.json", dir) < sizeof(probe) && access(probe, F_OK) == 0) {
      snprintf(best, sizeof(best), "%s", dir);
      break;
    }
    char *up = strrchr(dir, '/');
    if (!up || up == dir) break;
    *up = '\0';
  }

  return (size_t)snprintf(out, out_len, "%s", best) >= out_len ? -1 : 0;
}

static char *trace_virtual_key(trace_ctx_t *ctx, const char *abs_path) {
  char key[PATH_MAX];
  size_t rl = ctx->root_len;
  bool under_root = strncmp(abs_path, ctx->root, rl) == 0 && (rl == 1 || abs_path[rl] == '/');

  int written = under_root
    ? snprintf(key, sizeof(key), ANT_BUNDLE_KEY_PREFIX "%s", abs_path + (rl == 1 ? 1 : rl + 1))
    : snprintf(key, sizeof(key), ANT_BUNDLE_KEY_PREFIX "__external__/%u/%s", ctx->result->external_count++, trace_basename(abs_path));

  return (size_t)written >= sizeof(key) ? NULL : strdup(key);
}

uint32_t trace_source_line(trace_ctx_t *ctx, uint32_t offset) {
  if (!ctx->source || offset < ctx->source_skip) return 1;
  size_t end = offset - ctx->source_skip;
  if (end > ctx->source_len) end = ctx->source_len;
  
  uint32_t line = 1;
  for (size_t i = 0; i < end; i++) if (ctx->source[i] == '\n') line++;
  
  return line;
}

int trace_find_module(trace_ctx_t *ctx, const char *abs_path) {
  trace_index_entry_t *e = NULL;
  HASH_FIND_STR(ctx->module_map, abs_path, e);
  return e ? (int)e->idx : -1;
}

int trace_add_module(trace_ctx_t *ctx, const char *abs_path, bool lenient) {
  int existing = trace_find_module(ctx, abs_path);
  if (existing >= 0) {
    if (!lenient) ctx->result->modules[existing].lenient = false;
    return existing;
  }

  ant_trace_result_t *r = ctx->result;
  if (!trace_grow((void **)&r->modules, r->module_count, &r->module_cap, sizeof(*r->modules))) return -1;

  char *key = trace_virtual_key(ctx, abs_path);
  char *abs_copy = strdup(abs_path);
  if (!key || !abs_copy || !trace_index_add(&ctx->module_map, abs_copy, NULL, r->module_count)) {
    free(key);
    free(abs_copy);
    return -1;
  }

  r->modules[r->module_count] = (ant_trace_module_t){
    .abs_path = abs_copy,
    .key = key,
    .format = MODULE_EVAL_FORMAT_UNKNOWN,
    .kind = (uint8_t)ESM_MODULE_KIND_CODE,
    .bundle_flags = trace_path_is_materialized(ctx, abs_path) ? ANT_BUNDLE_MODULE_MATERIALIZE : 0,
    .lenient = lenient,
  };

  return (int)r->module_count++;
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

  char *spec_copy = strdup(spec);
  if (!spec_copy ||
      !trace_grow((void **)&r->edges, r->edge_count, &r->edge_cap, sizeof(*r->edges)) ||
      !trace_index_add(&ctx->edge_set, edge_key, edge_key, r->edge_count)) {
    free(edge_key);
    free(spec_copy);
    return false;
  }

  r->edges[r->edge_count++] = (ant_trace_edge_t){ parent, spec_copy, child, is_require };
  return true;
}

static void trace_warn_unbundled(trace_ctx_t *ctx, const char *parent_path, const trace_dep_t *dep) {
  switch (dep->sink) {
  case TRACE_SINK_ASSET:
    trace_warn(ctx, "%s:%u: new URL(\"%s\", import.meta.url) target was not bundled", parent_path, trace_source_line(ctx, dep->offset), dep->spec);
    break;
  case TRACE_SINK_WORKER:
    trace_warn(ctx, "%s:%u: new Worker(\"%s\") target was not bundled", parent_path, trace_source_line(ctx, dep->offset), dep->spec);
    break;
  case TRACE_SINK_FORK:
    trace_warn(ctx, "%s:%u: child_process.fork(\"%s\") target was not bundled", parent_path, trace_source_line(ctx, dep->offset), dep->spec);
    break;
  default:
    break;
  }
}

static int trace_resolve_missing(trace_ctx_t *ctx, const char *parent_path, const trace_dep_t *dep) {
  if (dep->lenient) {
    trace_warn_unbundled(ctx, parent_path, dep);
    return 0;
  }

  if (dep->optional) {
    trace_warn_deferred(ctx,
      "%s:%u: optional module \"%s\" was not found; the require/import inside try/catch will throw at runtime",
      parent_path, trace_source_line(ctx, dep->offset), dep->spec);
    return 0;
  }

  trace_set_error(ctx->result, "%s:%u: cannot resolve module \"%s\"", parent_path, trace_source_line(ctx, dep->offset), dep->spec);
  return -1;
}

static int trace_resolve_native(trace_ctx_t *ctx, const char *parent_path, const trace_dep_t *dep, const char *resolved) {
  int native_package = trace_register_native_package(ctx, resolved, true);
  if (native_package > 0) {
    ctx->native_fallback_available = true;
    return 0;
  }

  if (native_package == 0)
    trace_set_error(ctx->result, "%s:%u: cannot determine native package assets for \"%s\"", parent_path, trace_source_line(ctx, dep->offset), dep->spec);
  else if (!ctx->result->error[0])
    trace_set_error(ctx->result, "out of memory while tracing native package assets");

  return -1;
}

static int trace_resolve_dep(trace_ctx_t *ctx, uint32_t parent_idx, const trace_dep_t *dep) {
  ant_trace_result_t *r = ctx->result;
  const char *parent_path = r->modules[parent_idx].abs_path;
  const char *spec = dep->spec;

  if (esm_find_builtin(ctx->js, spec, strlen(spec)).kind != ANT_BUILTIN_NAME_NONE) return 0;
  if (esm_has_builtin_scheme(spec) || esm_is_data_url(spec)) return 0;

  char *file_url_path = esm_file_url_to_path(ctx->js, spec);
  if (!file_url_path && esm_is_url(spec)) {
    if (dep->lenient) return 0;
    if (dep->optional) {
      trace_warn(ctx, "%s:%u: remote import \"%s\" inside try/catch was not bundled; it will throw at runtime", parent_path, trace_source_line(ctx, dep->offset), spec);
      return 0;
    }
    trace_set_error(r, "%s:%u: cannot compile remote import \"%s\"", parent_path, trace_source_line(ctx, dep->offset), spec);
    return -1;
  }

  char *resolved = esm_resolve(
    ctx->js, file_url_path ? file_url_path : spec, parent_path,
    dep->is_require ? esm_resolve_path_require : esm_resolve_path
  );
  free(file_url_path);
  if (!resolved) return trace_resolve_missing(ctx, parent_path, dep);

  if (trace_has_suffix(resolved, ".node") && trace_resolve_native(ctx, parent_path, dep, resolved) != 0) {
    free(resolved);
    return -1;
  }

  int child = trace_add_module(ctx, resolved, dep->lenient);
  free(resolved);

  if (child < 0 || (dep->edge && !trace_add_edge(ctx, parent_idx, spec, (uint32_t)child, dep->is_require))) {
    trace_set_error(r, "out of memory while tracing module graph");
    return -1;
  }

  return 0;
}

static sv_ast_t *trace_parse(ant_t *js, const char *code, size_t len, sv_parse_goal_t goal) {
  GC_ROOT_SAVE(exception_mark, js);
  ant_value_t saved_exception = Ant_Exception_Peek(js);
  GC_ROOT_PIN(js, saved_exception);

  sv_ast_t *program = sv_parse(js, code, (ant_offset_t)len, goal, false);
  if (!program) Ant_Exception_Set(js, saved_exception);

  GC_ROOT_RESTORE(js, exception_mark);
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

  sv_ast_t *program = trace_parse(js, wrapped, total, SV_PARSE_SCRIPT);
  free(wrapped);

  return program;
}

static int trace_read_module(trace_ctx_t *ctx, uint32_t idx, char **content, size_t *size) {
  ant_trace_module_t *m = &ctx->result->modules[idx];
  esm_module_kind_t kind = m->kind == (uint8_t)ESM_MODULE_KIND_ASSET
    ? ESM_MODULE_KIND_ASSET : esm_classify_kind_for_path(m->abs_path);

  if (kind == ESM_MODULE_KIND_NATIVE && !(m->bundle_flags & ANT_BUNDLE_MODULE_MATERIALIZE)) {
    trace_set_error(ctx->result, "native addon %s cannot be embedded in a compiled executable", m->abs_path);
    return -1;
  }

  if (kind == ESM_MODULE_KIND_NONE || kind == ESM_MODULE_KIND_URL) kind = ESM_MODULE_KIND_CODE;
  m->kind = (uint8_t)kind;

  esm_file_data_t file;
  if (is_err(esm_read_file(ctx->js, m->abs_path, "module", &file))) {
    trace_set_error(ctx->result, "cannot read %s", m->abs_path);
    return -1;
  }

  file.data[file.size] = '\0';
  *content = file.data;
  *size = file.size;

  if (kind == ESM_MODULE_KIND_CODE) return 1;
  m->data = (uint8_t *)file.data;
  m->data_len = file.size;

  return 0;
}

static sv_ast_t *trace_parse_module(trace_ctx_t *ctx, ant_trace_module_t *m, const char *code, size_t len, bool *wrapped) {
  ant_module_format_t format = esm_decide_module_format(ctx->js, m->abs_path);
  sv_ast_t *program = trace_parse(ctx->js, code, len,
    format == MODULE_EVAL_FORMAT_ESM ? SV_PARSE_MODULE :
    format == MODULE_EVAL_FORMAT_CJS ? SV_PARSE_SCRIPT : SV_PARSE_DETECT);

  *wrapped = false;
  if (program && format == MODULE_EVAL_FORMAT_UNKNOWN)
    format = (program->flags & FN_MODULE_SYNTAX) ? MODULE_EVAL_FORMAT_ESM : MODULE_EVAL_FORMAT_CJS;

  if (!program && format != MODULE_EVAL_FORMAT_ESM && !m->lenient) {
    format = MODULE_EVAL_FORMAT_CJS;
    program = trace_parse_cjs_wrapped(ctx->js, code, len);
    *wrapped = program != NULL;
  }

  m->format = (uint8_t)format;
  return program;
}

static void trace_reset_module_scan(trace_ctx_t *ctx, ant_trace_module_t *m, char *dir) {
  ctx->dep_count = 0;
  ctx->file = m->abs_path;
  ctx->dir = dir;
  trace_warnings_settle(ctx);
}

static constexpr uint32_t TRACE_CJS_PREFIX_LEN = sizeof("(function(require,module,exports,__filename,__dirname){") - 1;

static int trace_scan_and_resolve(trace_ctx_t *ctx, uint32_t idx, const sv_ast_t *program, bool wrapped) {
  ant_trace_module_t *m = &ctx->result->modules[idx];
  char *dir = strdup(m->abs_path);
  if (!dir) {
    trace_set_error(ctx->result, "out of memory while tracing module graph");
    return -1;
  }

  char *slash = strrchr(dir, '/');
  if (slash) *(slash == dir ? slash + 1 : slash) = '\0';
  trace_reset_module_scan(ctx, m, dir);
  ctx->source = (const char *)m->data;
  ctx->source_len = m->data_len;
  ctx->source_skip = wrapped ? TRACE_CJS_PREFIX_LEN : 0;

  bool scan_ok = trace_scan_module(ctx, program, wrapped);
  int rc = 0;

  if (!scan_ok) {
    if (!ctx->result->error[0]) trace_set_error(ctx->result, "out of memory while tracing module graph");
    rc = -1;
  } else {
    trace_exclude_redundant_universal_native(ctx, idx);
    for (uint32_t i = 0; rc == 0 && i < ctx->dep_count; i++)
      if (!ctx->deps[i].excluded) rc = trace_resolve_dep(ctx, idx, &ctx->deps[i]);
  }

  if (rc == 0) trace_warnings_settle(ctx);
  trace_arena_free(ctx);
  ctx->dep_count = 0;
  ctx->file = ctx->dir = NULL;
  ctx->source = NULL;
  free(dir);

  return rc;
}

static int trace_process_module(trace_ctx_t *ctx, uint32_t idx) {
  ant_trace_result_t *r = ctx->result;
  char *content = NULL;
  size_t size = 0;

  int read = trace_read_module(ctx, idx, &content, &size);
  if (read <= 0) return read;

  ant_trace_module_t *m = &r->modules[idx];
  uint8_t *original = NULL;
  if (m->lenient) {
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
  if (strip_typescript_inplace(&content, size, m->abs_path, &js_len, &strip_detail) < 0) {
    trace_set_error(r, "TypeScript error in %s: %s", m->abs_path, strip_detail ? strip_detail : "strip failed");
    free(content);
    free(original);
    return -1;
  }
  m->data = (uint8_t *)content;
  m->data_len = js_len;

  code_arena_mark_t mark = parse_arena_mark(ctx->js);
  bool wrapped = false;
  sv_ast_t *program = trace_parse_module(ctx, m, content, js_len, &wrapped);

  if (!program && m->lenient) {
    parse_arena_rewind(ctx->js, mark);
    free(content);
    m->data = original;
    m->data_len = size;
    m->kind = (uint8_t)ESM_MODULE_KIND_TEXT;
    m->format = MODULE_EVAL_FORMAT_UNKNOWN;
    m->lenient_text = true;
    return 0;
  }
  free(original);

  if (!program) {
    trace_set_error(r, "failed to parse module %s", m->abs_path);
    parse_arena_rewind(ctx->js, mark);
    return -1;
  }

  int rc = trace_scan_and_resolve(ctx, idx, program, wrapped);
  parse_arena_rewind(ctx->js, mark);
  return rc;
}

static bool trace_reprocess_strict_text(trace_ctx_t *ctx, uint32_t processed, int *rc) {
  bool reprocessed = false;
  for (uint32_t i = 0; i < processed && *rc == 0; i++) {
    ant_trace_module_t *m = &ctx->result->modules[i];
    if (!m->lenient_text || m->lenient) continue;

    free(m->data);
    m->data = NULL;
    m->data_len = 0;
    m->kind = (uint8_t)ESM_MODULE_KIND_CODE;
    m->format = MODULE_EVAL_FORMAT_UNKNOWN;
    m->lenient_text = false;

    *rc = trace_process_module(ctx, i);
    reprocessed = true;
  }
  return reprocessed;
}

static void trace_ctx_free(trace_ctx_t *ctx) {
  free(ctx->deps);
  trace_arena_free(ctx);
  trace_native_free(ctx);
  trace_warnings_free(ctx);
  trace_index_free(&ctx->module_map);
  trace_index_free(&ctx->edge_set);
}

int ant_esm_trace_graph(ant_t *js, const char *entry_abs_path, const char *root_dir, ant_trace_result_t *out) {
  memset(out, 0, sizeof(*out));

  size_t root_len = strlen(root_dir);
  while (root_len > 1 && root_dir[root_len - 1] == '/') root_len--;

  char root[PATH_MAX];
  if (root_len >= sizeof(root)) {
    trace_set_error(out, "pack root path too long");
    return -1;
  }

  memcpy(root, root_dir, root_len);
  root[root_len] = '\0';
  trace_ctx_t ctx = { .js = js, .result = out, .root = root, .root_len = root_len };

  int entry = trace_add_module(&ctx, entry_abs_path, false);
  if (entry < 0) {
    trace_set_error(out, "out of memory while tracing module graph");
    trace_ctx_free(&ctx);
    return -1;
  }
  out->entry_idx = (uint32_t)entry;

  int rc = 0;
  uint32_t processed = 0;
  for (bool again = true; again && rc == 0;) {
    while (rc == 0 && processed < out->module_count) rc = trace_process_module(&ctx, processed++);
    again = rc == 0 && (trace_reprocess_strict_text(&ctx, processed, &rc) || processed != out->module_count);
  }

  trace_ctx_free(&ctx);
  return rc == 0 ? 0 : -1;
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
