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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  char *spec;
  bool is_require;
  bool lenient;
  bool optional;
  uint32_t line;
} trace_spec_t;

typedef struct {
  const char *key;
  char *owned_key;
  uint32_t idx;
  UT_hash_handle hh;
} trace_index_entry_t;

typedef struct {
  ant_trace_result_t *result;
  const char *root;
  size_t root_len;

  trace_index_entry_t *module_map;
  trace_index_entry_t *edge_set;

  uint32_t try_depth;

  trace_spec_t *specs;
  uint32_t spec_count, spec_cap;
} trace_ctx_t;

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
  ctx->specs[ctx->spec_count++] = (trace_spec_t){ copy, is_require, lenient, ctx->try_depth > 0, line };
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
      if (node->right && node->right->type == N_STRING && node->right->str) {
        if (!trace_push_spec(ctx, node->right->str, node->right->len, false, false, node->line)) return false;
      } else if (ctx->try_depth == 0) {
        trace_warn(ctx, "%s:%u: import() with a non-constant specifier is not traced; it will fail at runtime unless the target is bundled", file, node->line);
      }
      break;

    case N_CALL:
      if (node_is_require_ident(node->left) && node->args.count >= 1) {
        const sv_ast_t *arg = node->args.items[0];
        if (arg && arg->type == N_STRING && arg->str) {
          if (!trace_push_spec(ctx, arg->str, arg->len, true, false, node->line)) return false;
        } else if (ctx->try_depth == 0) {
          trace_warn(ctx, "%s:%u: require() with a non-constant specifier is not traced; it will fail at runtime unless the target is bundled", file, node->line);
        }
      }
      break;

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
      trace_warn(ctx, "%s:%u: optional module \"%s\" was not found; the require/import inside try/catch will throw at runtime", parent_path, sp->line, spec);
      return 0;
    }
    trace_set_error(r, "%s:%u: cannot resolve module \"%s\"", parent_path, sp->line, spec);
    return -1;
  }

  size_t rlen = strlen(resolved);
  if (rlen > 5 && strcmp(resolved + rlen - 5, ".node") == 0) {
    if (sp->lenient || sp->optional) {
      if (sp->optional) trace_warn(ctx, "%s:%u: native addon \"%s\" inside try/catch was not bundled; it will throw at runtime", parent_path, sp->line, spec);
      free(resolved);
      return 0;
    }
    trace_set_error(r, "%s:%u: native addon \"%s\" cannot be embedded in a compiled executable", parent_path, sp->line, spec);
    free(resolved);
    return -1;
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

  esm_module_kind_t kind = esm_classify_kind_for_path(abs_path);
  if (kind == ESM_MODULE_KIND_NATIVE) {
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
    if (original) {
      memcpy(original, content, size + 1);
    }
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
    if (original) {
      free(content);
      r->modules[idx].data = original;
      r->modules[idx].data_len = size;
    }
    r->modules[idx].kind = (uint8_t)ESM_MODULE_KIND_TEXT;
    r->modules[idx].format = MODULE_EVAL_FORMAT_UNKNOWN;
    r->modules[idx].lenient_text = true;
    return 0;
  }
  free(original);
  original = NULL;

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

  bool scan_ok = trace_scan_ast(ctx, abs_path, program);
  parse_arena_rewind(mark);

  if (!scan_ok) {
    trace_set_error(r, "out of memory while tracing module graph");
    return -1;
  }

  for (uint32_t i = 0; i < ctx->spec_count; i++) {
    if (trace_resolve_spec(js, ctx, idx, &ctx->specs[i]) != 0) return -1;
  }
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
  trace_index_free(&ctx.module_map);
  trace_index_free(&ctx.edge_set);
  return 0;

fail:
  for (uint32_t i = 0; i < ctx.spec_count; i++) free(ctx.specs[i].spec);
  free(ctx.specs);
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
