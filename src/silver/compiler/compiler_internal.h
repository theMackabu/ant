#ifndef ANT_SILVER_COMPILER_COMPILER_INTERNAL_H
#define ANT_SILVER_COMPILER_COMPILER_INTERNAL_H

#include "silver/jit.h"
#include "silver/compile_ctx.h"
#include "silver/compiler.h"
#include "silver/directives.h"
#include "silver/eval_env.h"
#include "silver/call.h"

#include "utf8.h"
#include "hash.h"
#include "numbers.h"
#include "tokens.h"

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
  SV_COMP_PRIVATE_SETTER = 4
};

enum {
  UNW_TRY_CATCH = 0,
  UNW_TRY_FINALLY = 1,
  UNW_FINALLY_BODY = 2,
};

typedef struct {
  bool is_upval;
  uint16_t idx;
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

size_t literal_num_key(double num, char *buf, size_t cap);
const char *copy_source_text(ant_t *js, const char *source, ant_offset_t source_len);
void emit(sv_compiler_t *c, uint8_t byte);
void emit_u16(sv_compiler_t *c, uint16_t val);
void emit_u32(sv_compiler_t *c, uint32_t val);
void emit_i32(sv_compiler_t *c, int32_t val);
void emit_op(sv_compiler_t *c, sv_op_t op);
void emit_srcpos(sv_compiler_t *c, sv_ast_t *node);
int add_constant(sv_compiler_t *c, ant_value_t val);
bool build_gc_const_tables(ant_t *js, sv_func_t *func);
sv_ic_entry_t *alloc_ic_slots(ant_t *js, uint16_t count);
void emit_constant(sv_compiler_t *c, ant_value_t val);
void emit_number(sv_compiler_t *c, double num);
bool template_has_valid_cooked_segments(const sv_ast_t *node);
const char *copy_name(ant_t *js, const char *name, uint32_t len);
sv_func_t *sv_func_new(ant_t *js);
ant_value_t sv_const_str(sv_compiler_t *c, const char *ptr, size_t len);
ant_value_t sv_const_bigint(sv_compiler_t *c, const char *digits, size_t len, bool negative);
bool emit_private_token(sv_compiler_t *c, sv_ast_t *name);
int add_atom(sv_compiler_t *c, const char *str, uint32_t len);
uint32_t add_map_template_desc(
  sv_compiler_t *c, sv_map_template_operation_t operation,
  const sv_ast_t *tpl, uint8_t substitution_count
);
void sv_func_init_code_and_map_templates(
  const sv_compiler_t *c, sv_func_t *func
);
uint16_t alloc_ic_idx(sv_compiler_t *c);
void sv_func_init_obj_sites(const sv_compiler_t *c, sv_func_t *func);
void emit_atom_idx_op(sv_compiler_t *c, sv_op_t op, uint32_t atom_idx);
void emit_get_length(sv_compiler_t *c);
void emit_atom_op(sv_compiler_t *c, sv_op_t op, const char *str, uint32_t len);
void compile_expr_with_inferred_name(
  sv_compiler_t *c, sv_ast_t *node,
  const char *name, uint32_t len
);
void emit_throw_type_error(sv_compiler_t *c, const char *msg, uint32_t len);
void emit_const_assign_error(sv_compiler_t *c, const char *name, uint32_t len);
void emit_import_binding_resolve(
  sv_compiler_t *c,
  uint8_t import_kind,
  const char *import_name,
  uint32_t import_name_len
);
void mark_import_binding(sv_compiler_t *c, int idx, sv_ast_t *spec);
int resolve_local(sv_compiler_t *c, const char *name, uint32_t len);
int resolve_local_at_depth(sv_compiler_t *c, const char *name, uint32_t len, int depth);
int add_local(
  sv_compiler_t *c, const char *name, uint32_t len, bool is_const, int depth
);
int reserve_hidden_locals(sv_compiler_t *c, int count);
void set_local_inferred_type(sv_compiler_t *c, int local_idx, uint8_t type);
const char *typeof_name_for_type(uint8_t type);
uint8_t iter_hint_for_type(uint8_t type);
int ensure_local_at_depth(
  sv_compiler_t *c, const char *name, uint32_t len,
  bool is_const, int depth
);
int local_to_frame_slot(sv_compiler_t *c, int local_idx);
int resolve_local_slot(sv_compiler_t *c, const char *name, uint32_t len);
int add_upvalue_binding(
  sv_compiler_t *c,
  uint16_t index,
  bool is_local,
  bool is_const,
  bool by_value,
  bool write_once,
  const sv_binding_meta_t *binding
);
int add_upvalue_tdz(sv_compiler_t *c, uint16_t index, bool is_local, bool may_tdz);
void compile_method_function(sv_compiler_t *c, sv_ast_t *m);
bool param_captured_by_value(sv_compiler_t *c, int local);
bool local_written_once(sv_compiler_t *c, int local);
uint8_t ctor_slot_class(const sv_ast_t *node);
int resolve_super_upvalue(sv_compiler_t *c);
int resolve_arguments_upvalue(sv_compiler_t *c);
int resolve_new_target_upvalue(sv_compiler_t *c);
int resolve_upvalue_raw(sv_compiler_t *c, const char *name, uint32_t len);
int resolve_upvalue(sv_compiler_t *c, const char *name, uint32_t len);
bool await_allowed(const sv_compiler_t *c);
bool undefined_shadowed(sv_compiler_t *c);
bool global_this_shadowed(sv_compiler_t *c);
bool capture_dynamic_eval_scope(
  sv_compiler_t *c, uint32_t *scope_index_out
);
void sv_func_finalize_type_data(
  sv_func_t *func, const sv_compiler_t *comp, int local_type_count
);
int emit_jump(sv_compiler_t *c, sv_op_t op);
void patch_jump(sv_compiler_t *c, int offset);
void emit_loop(sv_compiler_t *c, int loop_start);
void patch_list_add(sv_patch_list_t *pl, int offset);
void patch_list_resolve(sv_compiler_t *c, sv_patch_list_t *pl);
void unwind_push(sv_compiler_t *c, uint8_t kind);
void unwind_pop(sv_compiler_t *c);
void push_loop(
  sv_compiler_t *c, int loop_start,
  const char *label, uint32_t label_len,
  bool is_switch
);
void pop_loop(sv_compiler_t *c);
void begin_scope(sv_compiler_t *c);
void end_scope(sv_compiler_t *c);
void emit_close_upvals(sv_compiler_t *c);
void with_shadows(sv_compiler_t *c, const char *name, uint32_t len, sv_with_refs_t *out);
bool local_with_shadowed(sv_compiler_t *c, const char *name, uint32_t len);
void emit_with_tries(
  sv_compiler_t *c, sv_with_refs_t *shadows, sv_op_t op,
  const char *name, uint32_t len, int pops, sv_patch_list_t *out
);
void emit_lexical_new_target(sv_compiler_t *c);
void emit_get_var_plain(sv_compiler_t *c, const char *name, uint32_t len);
void emit_get_var(sv_compiler_t *c, const char *name, uint32_t len);
void emit_export_writes_from_stack(sv_compiler_t *c, const sv_export_name_t *exports);
void emit_local_export_writes(sv_compiler_t *c, int local);
void emit_set_var(sv_compiler_t *c, const char *name, uint32_t len, bool keep);
void emit_put_local_typed(sv_compiler_t *c, int local_idx, uint8_t type);
void emit_put_local(sv_compiler_t *c, int local_idx);
void emit_set_completion_from_stack(sv_compiler_t *c);
void emit_set_completion_undefined(sv_compiler_t *c);
void emit_get_local(sv_compiler_t *c, int local_idx);
void compile_template_items(sv_compiler_t *c, sv_ast_t *node, int start);
bool compile_self_append_stmt(sv_compiler_t *c, sv_ast_t *node);
void mark_char_code_at_binding(sv_compiler_t *c, sv_ast_t *prop);
bool is_sloppy_eval(const sv_compiler_t *c);
void add_eval_var(sv_compiler_t *c, const char *name, uint32_t len, bool annex_b);
void collect_pattern_names(sv_ast_t *pat, sv_decl_names_t *out);
void check_statement_list_declarations(
  sv_compiler_t *c, sv_ast_list_t *stmts, bool functions_lexical,
  const sv_decl_names_t *params, bool function_body
);
void check_switch_declarations(sv_compiler_t *c, sv_ast_t *node);
void check_for_head_declarations(sv_compiler_t *c, sv_ast_t *head, sv_ast_t *body);
void check_catch_declarations(sv_compiler_t *c, sv_ast_t *param, sv_ast_t *body);
void hoist_var_decls(sv_compiler_t *c, sv_ast_t *node);
void hoist_lexical_pattern(sv_compiler_t *c, sv_ast_t *pat,
                                  bool is_const);
void annex_b_collect_funcs(ant_t *js, sv_ast_t *node, sv_ast_list_t *out);
void mark_export_binding(sv_compiler_t *c, const char *name, uint32_t len);
void mark_export_pattern(sv_compiler_t *c, sv_ast_t *pat);
void hoist_lexical_decls(sv_compiler_t *c, sv_ast_list_t *stmts);
void emit_closure(sv_compiler_t *c, int index);
void hoist_one_func(sv_compiler_t *c, sv_ast_t *node, bool annex_b_update_var);
void hoist_func_decls(sv_compiler_t *c, sv_ast_list_t *stmts);
uint8_t infer_expr_type(sv_compiler_t *c, sv_ast_t *node);
bool compile_stack_overflow(sv_compiler_t *c);
bool sv_node_has_optional_base(sv_ast_t *n);
bool call_has_spread_arg(const sv_ast_t *node);
void compile_push_arg_to_array(sv_compiler_t *c, sv_ast_t *arg);
void compile_concat_spread_to_array(sv_compiler_t *c, sv_ast_t *spread_arg);
void emit_field_inits(
  sv_compiler_t *c, sv_compiler_t *owner, sv_ast_t **fields, int count
);
void emit_call_srcpos(sv_compiler_t *c, sv_ast_t *node);
sv_call_kind_t compile_call_setup_non_optional(sv_compiler_t *c, sv_ast_t *callee);
bool compile_call_map_template_intrinsic(
  sv_compiler_t *c, sv_ast_t *node, bool has_spread, bool is_tail
);
bool compile_regexp_exec_discard_intrinsic(
  sv_compiler_t *c, sv_ast_t *node
);
void compile_truthy_test_expr(sv_compiler_t *c, sv_ast_t *node);
bool check_function_param_early_errors(
  ant_t *js,
  sv_ast_t *fn,
  bool inherited_strict,
  bool *out_has_own_use_strict,
  bool *out_has_non_simple_params,
  bool emit_error
);
bool compile_direct_eval_call(
  sv_compiler_t *c, sv_ast_t *node, bool has_spread
);
bool is_destructure_pattern_node(sv_ast_t *node);
void compile_destructure_pattern(
  sv_compiler_t *c, sv_ast_t *pat,
  bool keep, bool consume_source,
  sv_destructure_mode_t mode,
  sv_var_kind_t kind
);
void emit_using_dispose_call(
  sv_compiler_t *c,
  int stack_local,
  int completion_local,
  bool is_async,
  bool suppressed_completion
);
void emit_using_cleanups_to_depth(sv_compiler_t *c, int target_depth);
void emit_close_upvals_to_depth(sv_compiler_t *c, int target_depth);
void emit_loop_exit_jump(sv_compiler_t *c, sv_loop_t *loop, sv_patch_list_t *pl);
void emit_return_from_stack(sv_compiler_t *c);
bool module_stmt_is_static_declaration(sv_ast_t *node);
void compile_module_static_declaration_prologue(sv_compiler_t *c, sv_ast_list_t *list);
bool should_emit_module_static_declarations(sv_compiler_t *c, sv_ast_t *body);
void compile_body_block(
  sv_compiler_t *c,
  sv_ast_t *body,
  bool skip_static_declarations
);
bool stmt_list_has_using_decl(sv_ast_list_t *list, bool *has_await_using);
void emit_empty_disposal_stack(sv_compiler_t *c);
void emit_using_push(sv_compiler_t *c, bool is_async);
void pop_using_cleanup(sv_compiler_t *c);
void emit_dispose_resource(sv_compiler_t *c, bool is_async);
void push_using_cleanup(
  sv_compiler_t *c,
  int stack_local,
  int scope_depth,
  bool is_async
);
void compile_export_emit(sv_compiler_t *c, const char *name, uint32_t len);
bool ast_has_eval_var(const sv_ast_t *node);
bool ast_has_own_eval(sv_compiler_t *c, const sv_ast_t *node);

static inline bool is_quoted_ident_key(const sv_ast_t *node) {
  if (!node || node->type != N_IDENT || !node->str || node->len < 2) return false;
  char open = node->str[0];
  char close = node->str[node->len - 1];
  return ((open == '\'' && close == '\'') || (open == '"' && close == '"'));
}

static inline bool is_template_segment(const sv_ast_t *node) {
  return node && node->type == N_STRING && (node->flags & FN_TEMPLATE_SEGMENT);
}

static inline ant_value_t ast_string_const(sv_compiler_t *c, const sv_ast_t *node) {
  if (!node || !node->str) return sv_const_str(c, "", 0);
  return sv_const_str(c, node->str, node->len);
}

static inline void compile_static_property_key(sv_compiler_t *c, sv_ast_t *key) {
  if (!key) {
    emit_op(c, OP_UNDEF);
    return;
  }

  if (key->type == N_STRING) {
    emit_constant(c, ast_string_const(c, key));
    return;
  }

  if (key->type == N_NUMBER) {
    char buf[SV_LITERAL_NUM_KEY_BUF_SIZE];
    size_t n = literal_num_key(key->num, buf, sizeof(buf));
    emit_constant(c, sv_const_str(c, buf, n));
    return;
  }

  if (key->type == N_IDENT) {
    if (is_quoted_ident_key(key)) emit_constant(c, sv_const_str(c, key->str + 1, key->len - 2));
    else emit_constant(c, sv_const_str(c, key->str, key->len));
    return;
  }

  compile_expr(c, key);
}

static inline bool is_private_name_node(const sv_ast_t *node) {
  return node && node->type == N_IDENT && node->str && node->len > 0 && node->str[0] == '#';
}

static inline void emit_set_function_name(
  sv_compiler_t *c,
  const char *name, uint32_t len
) {
  if (!name) { name = ""; len = 0; }
  emit_atom_op(c, OP_SET_NAME, name, len);
}

static inline bool node_needs_inferred_function_name(const sv_ast_t *node) {
  return node && 
    (node->type == N_FUNC || node->type == N_CLASS) &&
    (!node->str || node->len == 0);
}

static inline bool is_ident_str(
  const char *name, uint32_t len,
  const char *lit, uint32_t lit_len
) {
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

static inline sv_op_t env_resolution_op(
  const sv_compiler_t *c, sv_op_t global_op, sv_op_t eval_op
) {
  return c && c->inherits_eval_env ? eval_op : global_op;
}

static inline bool has_module_import_binding(const sv_compiler_t *c) {
  for (const sv_compiler_t *cur = c; cur; cur = cur->enclosing) {
    if (cur->mode == SV_COMPILE_MODULE) return true;
  }
  return false;
}

static inline bool has_active_with_scope(const sv_compiler_t *c) {
  for (const sv_compiler_t *cur = c; cur; cur = cur->enclosing) {
    if (cur->with_depth > 0) return true;
  }
  return false;
}

static inline bool has_implicit_arguments_obj(const sv_compiler_t *c) {
  return c && !c->is_arrow && c->enclosing && !sv_compile_mode_is_eval(c->mode);
}

static inline int add_hidden_local(sv_compiler_t *c) {
  return add_local(c, "", 0, false, c->scope_depth);
}

static inline bool sv_type_is_known(uint8_t t) {
  return t != SV_TI_UNKNOWN;
}

static inline bool sv_type_is_num(uint8_t t) {
  return t == SV_TI_NUM;
}

static inline uint8_t get_local_inferred_type(sv_compiler_t *c, int local_idx) {
  if (local_idx < 0 || local_idx >= c->local_count) return SV_TI_UNKNOWN;
  if (c->locals[local_idx].depth == -1) return SV_TI_UNKNOWN;
  if (c->locals[local_idx].is_tdz) return SV_TI_UNKNOWN;
  if (c->locals[local_idx].captured) return SV_TI_UNKNOWN;
  return c->locals[local_idx].inferred_type;
}

static inline int add_upvalue(sv_compiler_t *c, uint16_t index, bool is_local, bool is_const) {
  return add_upvalue_binding(c, index, is_local, is_const, false, false, NULL);
}

static inline void emit_get_module_import_binding(sv_compiler_t *c) {
  emit_op(c, OP_SPECIAL_OBJ);
  emit(c, SV_SPECIAL_MODULE_IMPORT);
}

static inline void emit_slot_op(sv_compiler_t *c, sv_op_t op, uint16_t slot) {
  emit_op(c, op);
  emit_u16(c, slot);
}

static inline bool is_ident_name(const sv_ast_t *node, const char *name) {
  size_t n = strlen(name);
  return node 
    && node->type == N_IDENT && node->len == (uint32_t)n 
    && memcmp(node->str, name, n) == 0;
}

static inline void decl_names_init(sv_decl_names_t *names) {
  names->items = names->inline_items;
  names->count = 0;
  names->cap = 16;
}

static inline void decl_names_free(sv_decl_names_t *names) {
  if (names->items != names->inline_items) free(names->items);
}

static inline bool is_class_method_def(const sv_ast_t *m) {
  return
    m && m->right && m->right->type == N_FUNC &&
    (m->right->flags & FN_METHOD);
}

#endif
