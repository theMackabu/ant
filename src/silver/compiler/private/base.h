#pragma once

#include "hash.h"    // IWYU pragma: export
#include "numbers.h" // IWYU pragma: export
#include "tokens.h"  // IWYU pragma: export

#include "silver/jit.h"  // IWYU pragma: export
#include "silver/call.h" // IWYU pragma: export

#include "silver/compile_ctx.h" // IWYU pragma: export
#include "silver/compiler.h"    // IWYU pragma: export
#include "silver/directives.h"  // IWYU pragma: export
#include "silver/eval_env.h"    // IWYU pragma: export

static constexpr int SV_TYPE_INFER_DEPTH = 48;
static constexpr int SV_SPILL_THRESHOLD = 32768;
static constexpr char SV_FIELD_THUNK_NAME[] = "%fields";
static constexpr uint32_t SV_FIELD_THUNK_LEN = sizeof(SV_FIELD_THUNK_NAME) - 1;
static constexpr size_t SV_LITERAL_NUM_KEY_BUF_SIZE = 32;

enum {
  SV_ITER_HINT_GENERIC = 0,
  SV_ITER_HINT_ARRAY = 1,
  SV_ITER_HINT_STRING = 4,
};

enum {
  SV_COMP_PRIVATE_FIELD = 0,
  SV_COMP_PRIVATE_METHOD = 1,
  SV_COMP_PRIVATE_GETTER = 3,
  SV_COMP_PRIVATE_SETTER = 4,
  SV_COMP_PRIVATE_METHODS = 5
};

enum {
  UNW_TRY_CATCH = 0,
  UNW_TRY_FINALLY = 1,
  UNW_FINALLY_BODY = 2,
};

typedef enum {
  SV_WITH_REF_LOCAL,
  SV_WITH_REF_UPVALUE,
  SV_WITH_REF_ENV,
} sv_with_ref_kind_t;

typedef struct {
  sv_with_ref_kind_t kind;
  bool implicit;
  uint16_t idx;
  const char *marker;
  uint32_t marker_len;
} sv_with_ref_t;

typedef struct {
  sv_with_ref_t *refs;
  int count;
  int cap;
} sv_with_refs_t;

typedef struct {
  const char *str;
  uint32_t len;
  bool is_function;
} sv_decl_name_t;

typedef struct {
  sv_decl_name_t *items;
  int count;
  int cap;
  sv_decl_name_t inline_items[16];
} sv_decl_names_t;

typedef enum {
  SV_CALL_DIRECT = 0,
  SV_CALL_METHOD = 1,
  SV_CALL_SUPER = 2,
} sv_call_kind_t;

typedef enum {
  DESTRUCTURE_ASSIGN = 0,
  DESTRUCTURE_BIND   = 1,
} sv_destructure_mode_t;

typedef bool (*private_eval_binding_fn)(
  void *ctx, const char *name, uint32_t len, uint8_t kind, uint16_t index
);

typedef struct {
  bool needs_this_binding;
  bool has_eval;
  bool returns_through_finally;
  const sv_ast_t *sole_super_stmt;
} derived_ctor_plan_t;

static inline bool is_quoted_ident_key(const sv_ast_t *node) {
  if (!node || node->type != N_IDENT || !node->str || node->len < 2) return false;
  char open = node->str[0];
  char close = node->str[node->len - 1];
  return ((open == '\'' && close == '\'') || (open == '"' && close == '"'));
}

static inline bool is_template_segment(const sv_ast_t *node) {
  return node && node->type == N_STRING && (node->flags & FN_TEMPLATE_SEGMENT);
}

static inline bool node_needs_inferred_function_name(const sv_ast_t *node) {
  return node && 
    (node->type == N_FUNC || node->type == N_CLASS) &&
    (!node->str || node->len == 0);
}

static inline bool is_ident_str(const char *name, uint32_t len, const char *lit, uint32_t lit_len) {
  return len == lit_len && memcmp(name, lit, lit_len) == 0;
}

static inline bool is_strict_restricted_ident(const char *name, uint32_t len) {
  return 
    is_ident_str(name, len, "eval", 4) ||
    is_ident_str(name, len, "arguments", 9);
}

static inline bool is_repl_root(const sv_compiler_t *c) {
  return c->mode == SV_COMPILE_REPL && c->enclosing && !c->enclosing->enclosing;
}

static inline bool is_repl_top_level(const sv_compiler_t *c) {
  return is_repl_root(c) && c->scope_depth == 0;
}

static inline bool has_completion_value(const sv_compiler_t *c) {
  return c && (sv_compile_mode_is_eval(c->mode) || c->mode == SV_COMPILE_REPL);
}

static inline bool is_completion_top_level(const sv_compiler_t *c) {
  return has_completion_value(c) && c->enclosing && !c->enclosing->enclosing;
}

static inline bool has_completion_accumulator(const sv_compiler_t *c) {
  return c && c->completion_local >= 0;
}

static inline sv_op_t env_resolution_op(const sv_compiler_t *c, sv_op_t global_op, sv_op_t eval_op) {
  return c && c->inherits_eval_env ? eval_op : global_op;
}

static inline bool has_module_import_binding(const sv_compiler_t *c) {
  for (const sv_compiler_t *cur = c; cur; cur = cur->enclosing) {
    if (cur->mode == SV_COMPILE_MODULE) return true;
  }
  return false;
}

static inline bool has_active_with_scope(const sv_compiler_t *c) {
  return c->with_depth > 0 || c->with_inherited;
}

static inline bool has_implicit_arguments_obj(const sv_compiler_t *c) {
  return c && !c->is_arrow && c->enclosing && !sv_compile_mode_is_eval(c->mode);
}

static inline void note_eval_reads_arguments(sv_compiler_t *c) {
  while (c->enclosing) c = c->enclosing;
  c->eval_reads_arguments = true;
}

static inline void note_eval_writes_names(sv_compiler_t *c) {
  while (c->enclosing) c = c->enclosing;
  c->eval_writes_names = true;
}

static inline bool sv_type_is_known(uint8_t t) {
  return t != SV_TI_UNKNOWN;
}

static inline bool sv_type_is_num(uint8_t t) {
  return t == SV_TI_NUM;
}

static inline bool is_ident_name(const sv_ast_t *node, const char *name) {
  size_t n = strlen(name);
  return node 
    && node->type == N_IDENT && node->len == (uint32_t)n 
    && memcmp(node->str, name, n) == 0;
}

static inline bool is_class_method_def(const sv_ast_t *m) {
  return
    m && m->right && m->right->type == N_FUNC &&
    (m->right->flags & FN_METHOD);
}
