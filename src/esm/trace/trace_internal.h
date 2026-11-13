#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uthash.h>

#include "types.h"
#include "esm/trace.h"
#include "silver/ast.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define TRACE_HOLE '\x01'
#define TRACE_MAX_ALTS 8

typedef enum {
#define TRACE_MODULE(id) TRACE_MODULE_##id,
#include "builtins.h"
  TRACE_MODULE_COUNT,
} trace_module_id_t;

typedef enum {
  TRACE_SINK_NONE,
  TRACE_SINK_STATIC,
  TRACE_SINK_IMPORT,
  TRACE_SINK_REQUIRE,
  TRACE_SINK_RESOLVE,
  TRACE_SINK_WORKER,
  TRACE_SINK_FORK,
  TRACE_SINK_ASSET,
  TRACE_SINK_EXPANDED,
} trace_sink_t;

typedef enum {
#define TRACE_FUNC(id, sink) TRACE_FUNC_##id,
#include "builtins.h"
  TRACE_FUNC_COUNT,
} trace_func_id_t;

typedef enum {
  TRACE_VALUE_UNKNOWN,
  TRACE_VALUE_STRING,
  TRACE_VALUE_LIST,
  TRACE_VALUE_BOOL,
  TRACE_VALUE_URL,
  TRACE_VALUE_MODULE,
  TRACE_VALUE_FUNC,
  TRACE_VALUE_META,
} trace_value_kind_t;

typedef struct {
  trace_value_kind_t kind;
  uint8_t count;
  bool partial;
  bool located;
  bool truth;
  uint16_t id;
  const char *alts[TRACE_MAX_ALTS];
} trace_value_t;

typedef struct trace_arena_chunk trace_arena_chunk_t;

typedef struct {
  char *spec;
  uint32_t offset;
  trace_sink_t sink;
  bool is_require;
  bool lenient;
  bool optional;
  bool edge;
  bool excluded;
} trace_dep_t;

typedef struct trace_binding {
  const char *name;
  uint32_t len;
  trace_value_t value;
} trace_binding_t;

typedef struct trace_scope {
  struct trace_scope *parent;
  trace_binding_t *items;
  uint32_t count, cap;
  bool function;
} trace_scope_t;

typedef struct {
  const char *name;
  uint32_t len;
  UT_hash_handle hh;
} trace_name_t;

typedef struct {
  const char *key;
  char *owned_key;
  uint32_t idx;
  UT_hash_handle hh;
} trace_index_entry_t;

typedef struct {
  ant_t *js;
  ant_trace_result_t *result;
  const char *root;
  size_t root_len;

  trace_index_entry_t *module_map;
  trace_index_entry_t *edge_set;

  trace_arena_chunk_t *arena;
  bool oom;

  const char *file;
  const char *dir;
  const char *source;
  size_t source_len;
  uint32_t source_skip;
  uint32_t try_depth;
  trace_scope_t *scope;
  trace_name_t *assigned;

  trace_dep_t *deps;
  uint32_t dep_count, dep_cap;

  char **materialize_roots;
  uint32_t materialize_root_count, materialize_root_cap;

  char **deferred_warnings;
  uint32_t deferred_warning_count, deferred_warning_cap;
  bool native_fallback_available;
} trace_ctx_t;

void *trace_alloc(trace_ctx_t *ctx, size_t size);
char *trace_strndup(trace_ctx_t *ctx, const char *s, size_t len);
char *trace_join_path(trace_ctx_t *ctx, const char *dir, const char *rel);
char *trace_normalize_path(trace_ctx_t *ctx, const char *path);
void trace_arena_free(trace_ctx_t *ctx);
bool trace_grow(void **items, uint32_t count, uint32_t *cap, size_t item_size);
bool trace_has_suffix(const char *path, const char *suffix);
bool trace_path_under(const char *path, const char *root);

void trace_set_error(ant_trace_result_t *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void trace_warn(trace_ctx_t *ctx, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void trace_warn_deferred(trace_ctx_t *ctx, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void trace_warnings_settle(trace_ctx_t *ctx);
void trace_warnings_free(trace_ctx_t *ctx);

uint32_t trace_source_line(trace_ctx_t *ctx, uint32_t offset);
int trace_find_module(trace_ctx_t *ctx, const char *abs_path);
int trace_add_module(trace_ctx_t *ctx, const char *abs_path, bool lenient);

trace_value_t trace_unknown(void);
trace_value_t trace_bool(bool truth);
trace_value_t trace_ref(trace_value_kind_t kind, uint16_t id);
trace_value_t trace_string(trace_ctx_t *ctx, const char *s, size_t len);
trace_value_t trace_concat(trace_ctx_t *ctx, trace_value_t a, trace_value_t b);
trace_value_t trace_union(trace_ctx_t *ctx, trace_value_t a, trace_value_t b);
trace_value_t trace_map_alts(trace_ctx_t *ctx, trace_value_t v, trace_value_kind_t kind, char *(*fn)(trace_ctx_t *, const char *, void *), void *user);
bool trace_value_exact(const trace_value_t *v, const char **out);
bool trace_value_truthy(const trace_value_t *v, bool *out);
bool trace_value_equal(const trace_value_t *a, const trace_value_t *b, bool *out);
bool trace_alt_has_hole(const char *alt);

const char *trace_target_platform(void);
const char *trace_target_arch(void);

void trace_scope_push(trace_ctx_t *ctx, bool function);
void trace_scope_pop(trace_ctx_t *ctx);
void trace_declare(trace_ctx_t *ctx, trace_scope_t *scope, const char *name, uint32_t len, trace_value_t value);
trace_scope_t *trace_function_scope(trace_ctx_t *ctx);
void trace_bind_pattern(trace_ctx_t *ctx, trace_scope_t *scope, const sv_ast_t *pattern, trace_value_t value);
bool trace_name_assigned(trace_ctx_t *ctx, const char *name, uint32_t len);
trace_value_t trace_eval(trace_ctx_t *ctx, const sv_ast_t *node);
trace_value_t trace_import_value(trace_ctx_t *ctx, const char *spec, size_t len);
trace_value_t trace_member(trace_ctx_t *ctx, trace_value_t base, const char *name, size_t len);
trace_sink_t trace_func_sink(uint16_t id);

bool trace_scan_module(trace_ctx_t *ctx, const sv_ast_t *program, bool cjs_wrapped);

typedef bool (*trace_expand_fn)(trace_ctx_t *ctx, const char *spec, bool relative, void *user);
int trace_expand_pattern(trace_ctx_t *ctx, const char *pattern, trace_expand_fn emit, void *user);

int trace_register_native_package(trace_ctx_t *ctx, const char *file, bool fallback_to_dir);
bool trace_path_is_materialized(trace_ctx_t *ctx, const char *path);
void trace_exclude_redundant_universal_native(trace_ctx_t *ctx, uint32_t parent_idx);
void trace_native_free(trace_ctx_t *ctx);
