#pragma once

#include "decl.h"

size_t literal_num_key(double num, char *buf, size_t cap);
size_t literal_num_name(const sv_ast_t *key, char *buf, size_t cap);

void emit_return_from_stack(sv_compiler_t *c);
void emit_derived_field_inits(sv_compiler_t *c);
void emit_empty_disposal_stack(sv_compiler_t *c);
void emit_using_push(sv_compiler_t *c, bool is_async);
void emit_call_srcpos(sv_compiler_t *c, sv_ast_t *node);
void emit_super_get(sv_compiler_t *c, sv_ast_t *member);
void emit_super_key(sv_compiler_t *c, sv_ast_t *member);
bool emit_private_token(sv_compiler_t *c, sv_ast_t *name);
void emit_dispose_resource(sv_compiler_t *c, bool is_async);
void emit_super_reference(sv_compiler_t *c, sv_ast_t *member);
void emit_private_get(sv_compiler_t *c, sv_ast_t *name, sv_op_t op);
void emit_using_cleanups_to_depth(sv_compiler_t *c, int target_depth);
void emit_loop_exit_jump(sv_compiler_t *c, sv_loop_t *loop, sv_patch_list_t *pl);
void emit_field_inits(sv_compiler_t *c, sv_compiler_t *owner, sv_ast_t **fields, int count);
void emit_using_dispose_call(sv_compiler_t *c, int stack_local, int completion_local, bool is_async, bool suppressed_completion);

bool compile_stack_overflow(sv_compiler_t *c);
void compile_method_function(sv_compiler_t *c, sv_ast_t *m);
bool compile_self_append_stmt(sv_compiler_t *c, sv_ast_t *node);
void compile_push_arg_to_array(sv_compiler_t *c, sv_ast_t *arg);
void compile_truthy_test_expr(sv_compiler_t *c, sv_ast_t *node);
sv_call_kind_t compile_tag_setup(sv_compiler_t *c, sv_ast_t *tag);
void compile_call_args_array(sv_compiler_t *c, sv_ast_t *call_node);
void compile_template_items(sv_compiler_t *c, sv_ast_t *node, int start);
void compile_concat_spread_to_array(sv_compiler_t *c, sv_ast_t *spread_arg);
bool compile_regexp_exec_discard_intrinsic(sv_compiler_t *c, sv_ast_t *node);
sv_call_kind_t compile_call_setup_non_optional(sv_compiler_t *c, sv_ast_t *callee);
void compile_body_block(sv_compiler_t *c, sv_ast_t *body, bool skip_static_declarations);
bool compile_call_map_template_intrinsic(sv_compiler_t *c, sv_ast_t *node, bool has_spread, bool is_tail);

void compile_destructure_pattern(
  sv_compiler_t *c, sv_ast_t *pat,
  bool keep, bool consume_source,
  sv_destructure_mode_t mode,
  sv_var_kind_t kind
);

bool push_with_scope(sv_compiler_t *c, sv_with_scope_t scope);
void push_using_cleanup(sv_compiler_t *c, int stack_local, int stash_local, int scope_depth, bool is_async);

int private_eval_binding_count(const sv_compiler_t *c);
bool private_name_resolvable(sv_compiler_t *c, sv_ast_t *name);
bool private_name_uses_brand(sv_compiler_t *c, sv_ast_t *name);
bool private_eval_bindings_each(sv_compiler_t *c, private_eval_binding_fn fn, void *ctx);

void pop_using_cleanup(sv_compiler_t *c);
bool sv_node_has_optional_base(sv_ast_t *n);
bool call_has_spread_arg(const sv_ast_t *node);
bool is_destructure_pattern_node(sv_ast_t *node);
bool class_method_needs_home(const sv_ast_t *method);
bool derived_return_needs_check(const sv_compiler_t *c);
uint8_t infer_expr_type(sv_compiler_t *c, sv_ast_t *node);
bool template_has_valid_cooked_segments(const sv_ast_t *node);
void mark_char_code_at_binding(sv_compiler_t *c, sv_ast_t *prop);
bool reject_super_private(sv_compiler_t *c, const sv_ast_t *member);
int resolve_local(sv_compiler_t *c, const char *name, uint32_t len);
bool stmt_list_has_using_decl(sv_ast_list_t *list, bool *has_await_using);
int capture_field_local(sv_compiler_t *c, sv_compiler_t *owner, int local);
int find_loop_label(const sv_compiler_t *c, const char *label, uint32_t label_len);

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
    size_t n = literal_num_name(key, buf, sizeof(buf));
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

static inline uint8_t private_method_kind(const sv_ast_t *m) {
  if (m->flags & FN_GETTER) return SV_COMP_PRIVATE_GETTER;
  return (m->flags & FN_SETTER) ? SV_COMP_PRIVATE_SETTER : SV_COMP_PRIVATE_METHOD;
}

static inline const sv_ast_t *field_static_key(const sv_ast_t *m) {
  const sv_ast_t *key = m->left;
  if ((m->flags & FN_COMPUTED) || !key) return NULL;
  return key->type == N_IDENT || key->type == N_STRING || key->type == N_NUMBER ? key : NULL;
}

static inline uint8_t field_define_flags(const sv_ast_t *m) {
  bool named_late = !field_static_key(m) && node_needs_inferred_function_name(m->right);
  return SV_DEFINE_METHOD_FIELD | (named_late ? SV_DEFINE_METHOD_SET_NAME : 0);
}

static inline void compile_field_initializer(sv_compiler_t *c, sv_ast_t *expr, const sv_ast_t *key) {
  char buf[SV_LITERAL_NUM_KEY_BUF_SIZE];
  if (key && key->type == N_NUMBER)
    compile_expr_with_inferred_name(c, expr, buf, (uint32_t)literal_num_key(key->num, buf, sizeof(buf)));
  else if (!key || !key->str) compile_expr(c, expr);
  else if (key->type != N_STRING && is_quoted_ident_key(key)) compile_expr_with_inferred_name(c, expr, key->str + 1, key->len - 2);
  else compile_expr_with_inferred_name(c, expr, key->str, key->len);
}

static inline sv_patch_list_t *optional_chain_take(sv_compiler_t *c) {
  sv_patch_list_t *chain = c->optional_chain;
  c->optional_chain = NULL;
  return chain;
}

static inline void compile_chain_base(sv_compiler_t *c, sv_ast_t *node, sv_patch_list_t *chain) {
  if (sv_node_has_optional_base(node)) c->optional_chain = chain;
  compile_expr(c, node);
  c->optional_chain = NULL;
}

static inline void emit_chain_short_circuit(sv_compiler_t *c, sv_patch_list_t *chain) {
  int present = emit_jump(c, OP_JMP_NOT_NULLISH);
  emit_op(c, OP_POP);
  emit_op(c, OP_UNDEF);
  patch_list_add(c, chain, emit_jump(c, OP_JMP));
  patch_jump(c, present);
}

static inline void compile_stmt_region(sv_compiler_t *c, sv_ast_t *node) {
  int mark = type_region_enter(c);
  compile_stmt(c, node);
  type_region_exit(c, mark);
}
