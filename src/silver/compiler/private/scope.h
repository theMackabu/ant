#pragma once

#include "emit.h"

void emit_this(sv_compiler_t *c);
void emit_super(sv_compiler_t *c);
void emit_new_target(sv_compiler_t *c);
void emit_this_store(sv_compiler_t *c);
void emit_close_upvals(sv_compiler_t *c);
void emit_super_receiver(sv_compiler_t *c);
void emit_lexical_new_target(sv_compiler_t *c);
void emit_derived_return_check(sv_compiler_t *c);
void emit_spill_define(sv_compiler_t *c, int spill);
void emit_get_local(sv_compiler_t *c, int local_idx);
void emit_put_local(sv_compiler_t *c, int local_idx);
void emit_set_completion_undefined(sv_compiler_t *c);
void emit_break_to_loop(sv_compiler_t *c, int target);
void emit_set_completion_from_stack(sv_compiler_t *c);
void emit_local_export_writes(sv_compiler_t *c, int local);
void emit_get_var(sv_compiler_t *c, const char *name, uint32_t len);
void emit_get_var_plain(sv_compiler_t *c, const char *name, uint32_t len);
void emit_get_var_typeof(sv_compiler_t *c, const char *name, uint32_t len);
void emit_set_var(sv_compiler_t *c, const char *name, uint32_t len, bool keep);
void emit_export_writes_from_stack(sv_compiler_t *c, const sv_export_name_t *exports);
void emit_put_local_typed(sv_compiler_t *c, int local_idx, uint8_t type, int dep_base);
int emit_with_try(sv_compiler_t *c, sv_op_t op, const sv_with_ref_t *ref, const char *name, uint32_t len);
void emit_import_binding_resolve(sv_compiler_t *c, uint8_t import_kind, const char *import_name, uint32_t import_name_len);

void emit_with_tries(
  sv_compiler_t *c, sv_with_refs_t *shadows, sv_op_t op,
  const char *name, uint32_t len, int pops, sv_patch_list_t *out
);

int resolve_this_upvalue(sv_compiler_t *c);
int resolve_super_upvalue(sv_compiler_t *c);
int resolve_new_target_upvalue(sv_compiler_t *c);
int resolve_upvalue(sv_compiler_t *c, const char *name, uint32_t len);
int resolve_local_slot(sv_compiler_t *c, const char *name, uint32_t len);
int resolve_upvalue_raw(sv_compiler_t *c, const char *name, uint32_t len);
int resolve_upvalue_to(sv_compiler_t *c, sv_compiler_t *owner, int local);
int resolve_spill_slow(const sv_compiler_t *c, const char *name, uint32_t len);
int resolve_local_at_depth(sv_compiler_t *c, const char *name, uint32_t len, int depth);

void add_eval_var(sv_compiler_t *c, const char *name, uint32_t len, bool annex_b);
int add_local(sv_compiler_t *c, const char *name, uint32_t len, bool is_const, int depth);

int add_upvalue_binding(
  sv_compiler_t *c,
  uint16_t index,
  bool is_local,
  bool is_const,
  bool by_value,
  bool write_once,
  const sv_binding_meta_t *binding
);

void set_local_inferred_type(sv_compiler_t *c, int local_idx, uint8_t type);
void set_local_stored_type(sv_compiler_t *c, int local_idx, uint8_t type, int dep_base);
void set_local_declared_type(sv_compiler_t *c, int local_idx, uint8_t type, int dep_base);

bool local_written_once(sv_compiler_t *c, int local);
int local_to_frame_slot(sv_compiler_t *c, int local_idx);
bool local_with_shadowed(sv_compiler_t *c, const char *name, uint32_t len);

void eval_scope_locals_popped(sv_compiler_t *c);
uint64_t eval_written_params(sv_compiler_t *c, const sv_ast_t *fn);
void eval_source_visit(sv_compiler_t *c, const sv_ast_t *source, void (*visit)(void *data, const sv_ast_t *program), void *data);

void type_loop_leave(sv_compiler_t *c);
void type_dep_add(sv_compiler_t *c, int local);
void type_region_leave(sv_compiler_t *c, int mark);
bool type_use_op(sv_compiler_t *c, int base, int pc);
bool type_deps_live(const sv_compiler_t *c, int base);
void type_assign_local_slow(sv_compiler_t *c, int local_idx, uint8_t type, int dep_base);

void unwind_pop(sv_compiler_t *c);
void unwind_push(sv_compiler_t *c, uint8_t kind);

bool spilled_binding_visible(sv_compiler_t *c, const char *name, uint32_t len);
bool spilled_const_assignment(sv_compiler_t *c, const char *name, uint32_t len);

bool reject_forbidden_arguments(sv_compiler_t *c);
bool reject_misplaced_super_call(sv_compiler_t *c);
bool reject_super_identifier(sv_compiler_t *c, const sv_ast_t *node);

bool ast_has_eval_var(const sv_ast_t *node);
bool ast_has_own_eval(sv_compiler_t *c, const sv_ast_t *node);
bool ast_literal_eval_mentions_arguments(sv_compiler_t *c, const sv_ast_t *node);

void pop_loop(sv_compiler_t *c);
void end_scope(sv_compiler_t *c);
void begin_scope(sv_compiler_t *c);
uint8_t iter_hint_for_type(uint8_t type);
bool undefined_shadowed(sv_compiler_t *c);
bool await_allowed(const sv_compiler_t *c);
bool global_this_shadowed(sv_compiler_t *c);
bool is_sloppy_eval(const sv_compiler_t *c);
const char *typeof_name_for_type(uint8_t type);
sv_compiler_t *lexical_owner(sv_compiler_t *c);
bool arguments_forbidden(const sv_compiler_t *c);
bool compiler_has_this_binding(const sv_compiler_t *c);
int reserve_hidden_locals(sv_compiler_t *c, int count);
bool param_captured_by_value(sv_compiler_t *c, int local);
derived_ctor_plan_t derived_ctor_plan(const sv_ast_t *ctor);
uint32_t class_public_field_count(sv_ast_t **fields, int count);
uint32_t ctor_prop_count(const sv_ast_t *node, uint32_t fields);
int find_eval_var(const sv_compiler_t *c, const char *name, uint32_t len);
bool capture_dynamic_eval_scope(sv_compiler_t *c, uint32_t *scope_index_out);
bool compile_direct_eval_call(sv_compiler_t *c, sv_ast_t *node, bool has_spread);
void with_shadows(sv_compiler_t *c, const char *name, uint32_t len, sv_with_refs_t *out);
int ensure_local_at_depth(sv_compiler_t *c, const char *name, uint32_t len, bool is_const, int depth);
bool declare_spilled_slow(sv_compiler_t *c, const char *name, uint32_t len, bool is_const, bool define);
void push_loop(sv_compiler_t *c, int loop_start, const char *label, uint32_t label_len, sv_loop_kind_t kind);

static inline sv_loop_t *loop_top(sv_compiler_t *c) {
  return c->loop_lost ? &c->loop_sink : &c->loops[c->loop_count - 1];
}

static inline int resolve_spill(const sv_compiler_t *c, const char *name, uint32_t len) {
  return __builtin_expect(c->spill_count == 0, 1) ? -1 : resolve_spill_slow(c, name, len);
}

static inline bool declare_spilled(sv_compiler_t *c, const char *name, uint32_t len, bool is_const, bool define) {
  if (__builtin_expect(c->spill_count == 0 && c->local_count - c->param_locals < SV_SPILL_THRESHOLD, 1)) return false;
  return declare_spilled_slow(c, name, len, is_const, define);
}

static inline void emit_arguments_eval_env(sv_compiler_t *c, const char *name, uint32_t len) {
  if (!c->lazy_eval_env || !is_ident_str(name, len, "arguments", 9)) return;
  emit_op(c, OP_SPECIAL_OBJ);
  emit(c, SV_SPECIAL_EVAL_ENV);
  emit_op(c, OP_POP);
}

static inline int add_hidden_local(sv_compiler_t *c) {
  return add_local(c, "", 0, false, c->scope_depth);
}

static inline void type_assign_local(sv_compiler_t *c, int local_idx, uint8_t type, int dep_base) {
  sv_local_t *local = &c->locals[local_idx];
  local->str_accumulating = false;
  if (local->inferred_type == type && (type == SV_TI_UNKNOWN || !c->type_loop_depth)) return;
  bool fresh =
    local->depth != -1 && !local->type_fact && !sv_type_is_known(local->inferred_type) && sv_type_is_known(type) &&
    !c->type_region_depth && dep_base == c->types->dep_count;
  if (!fresh) {
    type_assign_local_slow(c, local_idx, type, dep_base);
    return;
  }
  local->inferred_type = type;
  local->type_fact = ++c->types->fact_next;
  local->type_use_head = 0;
  local->type_derived = false;
  c->type_any = true;
}

static inline int type_region_enter(sv_compiler_t *c) {
  c->type_region_depth++;
  return c->types->promo_count;
}

static inline void type_region_exit(sv_compiler_t *c, int mark) {
  if (mark == c->types->promo_count) c->type_region_depth--;
  else type_region_leave(c, mark);
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
