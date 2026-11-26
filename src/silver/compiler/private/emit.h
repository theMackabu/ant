#pragma once

#include "base.h"

const char *copy_name(ant_t *js, const char *name, uint32_t len);
const char *copy_source_text(ant_t *js, const char *source, ant_offset_t source_len);

void Silver_Compiler_RaiseOutOfMemory(sv_compiler_t *c);
void *Silver_Compiler_Grow(sv_compiler_t *c, void *items, int *cap, int need, int initial, size_t size);

void emit_get_length(sv_compiler_t *c);
void emit(sv_compiler_t *c, uint8_t byte);
void emit_argc(sv_compiler_t *c, int argc);
void emit_op(sv_compiler_t *c, sv_op_t op);
int emit_jump(sv_compiler_t *c, sv_op_t op);
void emit_i32(sv_compiler_t *c, int32_t val);
void emit_u16(sv_compiler_t *c, uint16_t val);
void emit_u32(sv_compiler_t *c, uint32_t val);
void emit_number(sv_compiler_t *c, double num);
void emit_loop(sv_compiler_t *c, int loop_start);
void emit_srcpos(sv_compiler_t *c, sv_ast_t *node);
void emit_constant(sv_compiler_t *c, ant_value_t val);
void emit_atom_idx_op(sv_compiler_t *c, sv_op_t op, uint32_t atom_idx);
void emit_throw_type_error(sv_compiler_t *c, const char *msg, uint32_t len);
void emit_atom_op(sv_compiler_t *c, sv_op_t op, const char *str, uint32_t len);
void emit_const_assign_error(sv_compiler_t *c, const char *name, uint32_t len);
void emit_throw_error(sv_compiler_t *c, js_err_type_t type, const char *msg, uint32_t len);

int add_constant(sv_compiler_t *c, ant_value_t val);
int add_atom(sv_compiler_t *c, const char *str, uint32_t len);
int add_constant_run(sv_compiler_t *c, const ant_value_t *vals, int count);

uint32_t add_map_template_desc(
  sv_compiler_t *c, sv_map_template_operation_t operation,
  const sv_ast_t *tpl, uint8_t substitution_count
);

uint16_t alloc_ic_idx(sv_compiler_t *c);
sv_ic_entry_t *alloc_ic_slots(ant_t *js, uint16_t count);

sv_func_t *sv_func_new(ant_t *js);
void sv_func_init_literal_sites(sv_compiler_t *c, sv_func_t *func);
void sv_func_init_obj_sites(const sv_compiler_t *c, sv_func_t *func);
void sv_func_init_code_and_map_templates(const sv_compiler_t *c, sv_func_t *func);
void sv_func_finalize_type_data(sv_func_t *func, const sv_compiler_t *comp, int local_type_count);

ant_value_t sv_const_str(sv_compiler_t *c, const char *ptr, size_t len);
ant_value_t sv_const_bigint(sv_compiler_t *c, const char *digits, size_t len, bool negative);

void patch_jump(sv_compiler_t *c, int offset);
void patch_list_resolve(sv_compiler_t *c, sv_patch_list_t *pl);
void patch_list_add(sv_compiler_t *c, sv_patch_list_t *pl, int offset);

bool build_gc_const_tables(ant_t *js, sv_func_t *func);
uint32_t source_line_of(const sv_compiler_t *c, const sv_ast_t *node);
void compile_expr_with_inferred_name(sv_compiler_t *c, sv_ast_t *node, const char *name, uint32_t len);

static inline ant_value_t ast_string_const(sv_compiler_t *c, const sv_ast_t *node) {
  if (!node || !node->str) return sv_const_str(c, "", 0);
  return sv_const_str(c, node->str, node->len);
}

static inline void emit_set_function_name(sv_compiler_t *c, const char *name, uint32_t len) {
  if (!name) { name = ""; len = 0; }
  emit_atom_op(c, OP_SET_NAME, name, len);
}

static inline void emit_get_module_import_binding(sv_compiler_t *c) {
  emit_op(c, OP_SPECIAL_OBJ);
  emit(c, SV_SPECIAL_MODULE_IMPORT);
}

static inline void emit_slot_op(sv_compiler_t *c, sv_op_t op, uint16_t slot) {
  emit_op(c, op);
  emit_u16(c, slot);
}
