#ifndef SILVER_GLUE_H
#define SILVER_GLUE_H

#include "silver/engine.h"

int64_t jit_helper_stack_overflow(ant_t *js);
int64_t jit_helper_is_truthy(ant_t *js, ant_value_t v);

void jit_helper_shape_transition(ant_object_t *obj, ant_shape_t *to_shape);
ant_value_t jit_helper_normalize_sloppy_this(ant_t *js, ant_value_t value);

ant_value_t jit_helper_add(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r);
ant_value_t jit_helper_sub(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r);
ant_value_t jit_helper_mul(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r);
ant_value_t jit_helper_div(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r);
ant_value_t jit_helper_mod(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r);

ant_value_t jit_helper_import_default(ant_t *js, ant_value_t ns);
ant_value_t jit_helper_get_length(sv_vm_t *vm, ant_t *js, ant_value_t obj);
ant_value_t jit_helper_get_length_inline(sv_vm_t *vm, ant_t *js, ant_value_t obj);
ant_value_t jit_helper_catch_value(sv_vm_t *vm, ant_t *js, ant_value_t err);
ant_value_t jit_helper_throw(sv_vm_t *vm, ant_t *js, ant_value_t val);

ant_value_t jit_helper_lt(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r);
ant_value_t jit_helper_le(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r);
ant_value_t jit_helper_seq(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r);
ant_value_t jit_helper_eq(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r);
ant_value_t jit_helper_in(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r);
ant_value_t jit_helper_gt(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r);
ant_value_t jit_helper_ge(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r);
ant_value_t jit_helper_ne(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r);
ant_value_t jit_helper_sne(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r);

ant_value_t jit_helper_band(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r);
ant_value_t jit_helper_bor(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r);
ant_value_t jit_helper_bxor(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r);
ant_value_t jit_helper_bnot(sv_vm_t *vm, ant_t *js, ant_value_t v);
ant_value_t jit_helper_shl(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r);
ant_value_t jit_helper_shr(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r);
ant_value_t jit_helper_ushr(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r);
ant_value_t jit_helper_not(sv_vm_t *vm, ant_t *js, ant_value_t v);

ant_value_t jit_helper_to_propkey(sv_vm_t *vm, ant_t *js, ant_value_t v);
ant_value_t jit_helper_stack_overflow_error(sv_vm_t *vm, ant_t *js);

ant_value_t jit_helper_strict_arguments(sv_vm_t *vm, ant_t *js, ant_value_t *args, int argc);
ant_value_t jit_helper_delete(sv_vm_t *vm, ant_t *js, ant_value_t obj, ant_value_t key);
ant_value_t jit_helper_typeof(sv_vm_t *vm, ant_t *js, ant_value_t v);
ant_value_t jit_helper_special_obj(sv_vm_t *vm, ant_t *js, uint32_t which);

ant_value_t jit_helper_get_global(
  ant_t *js, const char *str,
  sv_func_t *func, int32_t bc_off
);

ant_value_t jit_helper_get_eval_global(
  ant_t *js, sv_closure_t *closure,
  const char *str, uint32_t len,
  sv_func_t *func, int32_t bc_off,
  int allow_missing
);

ant_value_t jit_helper_put_eval_global(
  ant_t *js, sv_closure_t *closure, ant_value_t val,
  const char *str, uint32_t len, int is_strict
);

ant_value_t jit_helper_delete_eval_var(
  ant_t *js, sv_closure_t *closure,
  const char *str, uint32_t len
);

ant_value_t jit_helper_instanceof(
  sv_vm_t *vm, ant_t *js,
  ant_value_t l, ant_value_t r,
  sv_func_t *func, int32_t bc_off
);

ant_value_t jit_helper_call_is_proto(
  sv_vm_t *vm, ant_t *js,
  ant_value_t call_this, ant_value_t call_func, ant_value_t arg,
  sv_func_t *func, int32_t bc_off
);

ant_value_t jit_helper_call(
  sv_vm_t *vm, ant_t *js,
  ant_value_t func, ant_value_t this_val,
  ant_value_t *args, int argc
);

ant_value_t jit_helper_call_method(
  sv_vm_t *vm, ant_t *js,
  ant_value_t func, ant_value_t this_val,
  ant_value_t *args, int argc,
  ant_value_t super_val, ant_value_t new_target,
  ant_value_t *out_this
);

ant_value_t jit_helper_call_char_code_at(
  sv_vm_t *vm, ant_t *js, ant_value_t func, 
  ant_value_t receiver, ant_value_t *args, int argc
);

ant_value_t jit_helper_call_array_includes(
  sv_vm_t *vm, ant_t *js,
  ant_value_t call_func, ant_value_t call_this,
  ant_value_t *args, int argc
);

ant_value_t sv_map_template_try_fast(
  ant_t *js, ant_value_t call_func, ant_value_t call_this,
  const ant_value_t *substitutions,
  const sv_map_template_desc_t *desc
);

ant_value_t sv_map_template_build_key(
  ant_t *js, const ant_value_t *substitutions,
  const sv_map_template_desc_t *desc
);

ant_value_t sv_op_call_map_template(
  sv_vm_t *vm, ant_t *js,
  ant_value_t call_func, ant_value_t call_this,
  const ant_value_t *substitutions,
  const sv_map_template_desc_t *desc
);

ant_value_t jit_helper_call_map_template(
  sv_vm_t *vm, ant_t *js, ant_value_t call_func, ant_value_t call_this,
  ant_value_t value0, ant_value_t value1, ant_value_t value2,
  const sv_map_template_desc_t *desc
);

ant_value_t jit_helper_map_template_fast(
  ant_t *js, ant_value_t call_func, ant_value_t call_this,
  ant_value_t value0, ant_value_t value1, ant_value_t value2,
  const sv_map_template_desc_t *desc
);

ant_value_t jit_helper_map_numeric_pair_fast(
  ant_t *js,
  ant_value_t call_func, ant_value_t call_this,
  ant_value_t left, ant_value_t right,
  const char *separator, uint32_t separator_len
);

ant_value_t jit_helper_regexp_exec_truthy(
  sv_vm_t *vm, ant_t *js,
  ant_value_t call_func, ant_value_t call_this, ant_value_t arg
);

ant_value_t jit_helper_call_stable_builtin(
  sv_vm_t *vm, ant_t *js, int kind,
  ant_value_t call_func, ant_value_t call_this,
  ant_value_t *args, int argc
);

ant_value_t jit_helper_load_stable_builtin(
  ant_t *js, int kind, ant_value_t *receiver_out
);

ant_value_t jit_helper_apply(
  sv_vm_t *vm, ant_t *js,
  ant_value_t func, ant_value_t this_val,
  ant_value_t *args, int argc
);

ant_value_t jit_helper_object(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, sv_obj_site_cache_t *site
);

ant_value_t jit_helper_object_template(
  sv_vm_t *vm, ant_t *js, 
  sv_func_t *func, sv_obj_site_cache_t *site
);

ant_value_t jit_helper_regexp(
  sv_vm_t *vm, ant_t *js, 
  ant_value_t pattern, ant_value_t flags
);

ant_value_t jit_helper_call_call(
  sv_vm_t *vm, ant_t *js,
  ant_value_t *base, int32_t n1, int32_t n2
);

ant_value_t jit_helper_call_call_slot(
  sv_vm_t *vm, ant_t *js,
  ant_value_t func, ant_value_t arg1, ant_value_t *slot
);

ant_value_t jit_helper_rest(
  sv_vm_t *vm, ant_t *js,
  ant_value_t *args, int argc, int start
);

ant_value_t jit_helper_get_field(
  sv_vm_t *vm, ant_t *js, ant_value_t obj,
  const char *str, uint32_t len,
  sv_func_t *func, int32_t bc_off
);

ant_value_t jit_helper_get_field_inline(
  sv_vm_t *vm, ant_t *js, ant_value_t obj,
  const char *str, uint32_t len,
  sv_func_t *func, int32_t bc_off
);

ant_value_t jit_helper_import_named(
  ant_t *js, ant_value_t ns,
  const char *str, uint32_t len,
  sv_func_t *func, int32_t bc_off
);

ant_value_t jit_helper_export(
  ant_t *js, sv_closure_t *closure,
  const char *str, uint32_t len, ant_value_t value
);

ant_value_t jit_helper_closure(
  sv_vm_t *vm, ant_t *js, sv_closure_t *parent_closure,
  ant_value_t this_val, ant_value_t *slots,
  int slot_base, int slot_count, uint32_t const_idx,
  const char *name, uint32_t name_len,
  sv_upvalue_t **open_upvalues
);

ant_value_t jit_helper_bailout_resume(
  sv_vm_t *vm, sv_closure_t *closure,
  ant_value_t this_val, ant_value_t *args, int argc,
  ant_value_t *vstack, int64_t vstack_sp,
  ant_value_t *params, int64_t n_params,
  ant_value_t *locals, int64_t n_locals,
  int64_t bc_offset
);

void jit_helper_close_upval(
  sv_vm_t *vm, int32_t slot_idx,
  ant_value_t *locals, int n_locals,
  sv_upvalue_t **open_upvalues
);

void jit_helper_take_open_upvalues(
  sv_vm_t *vm, sv_upvalue_t **open_upvalues,
  ant_value_t *slots, int slot_count
);

void jit_helper_take_open_upvalues_rebase(
  sv_vm_t *vm, sv_upvalue_t **open_upvalues,
  ant_value_t *src_slots, ant_value_t *dst_slots, int slot_count
);

void jit_helper_upval_barrier(
  ant_t *js,
  sv_upvalue_t *uv, ant_value_t val
);

void jit_helper_adopt_open_upvalues(
  sv_vm_t *vm,
  sv_upvalue_t **open_upvalues
);

void jit_helper_define_field(
  sv_vm_t *vm, ant_t *js, ant_value_t obj,
  ant_value_t val, const char *str, uint32_t len
);

void jit_helper_define_slot(
  sv_vm_t *vm, ant_t *js, ant_value_t obj, ant_value_t val,
  const char *str, uint32_t len, uint32_t slot
);

void jit_helper_define_method_comp(
  ant_t *js,
  ant_value_t obj, ant_value_t key, ant_value_t fn, uint8_t flags
);

void jit_helper_set_name(
  ant_t *js, ant_value_t fn,
  const char *str, uint32_t len
);

ant_value_t jit_helper_put_field(
  sv_vm_t *vm, ant_t *js, ant_value_t obj,
  ant_value_t val, const char *str, uint32_t len
);

ant_value_t jit_helper_put_field_ic(
  sv_vm_t *vm, ant_t *js, ant_value_t obj,
  ant_value_t val, const sv_atom_t *atom, sv_ic_entry_t *ic
);

ant_value_t jit_helper_get_elem(
  sv_vm_t *vm, ant_t *js,
  ant_value_t obj, ant_value_t key, sv_func_t *func, int32_t bc_off
);

ant_value_t jit_helper_get_private(
  sv_vm_t *vm, ant_t *js, ant_value_t obj, ant_value_t token
);

ant_value_t jit_helper_put_private(
  sv_vm_t *vm, ant_t *js,
  ant_value_t obj, ant_value_t val, ant_value_t token
);

ant_value_t jit_helper_put_elem(
  sv_vm_t *vm, ant_t *js,
  ant_value_t obj, ant_value_t key, ant_value_t val
);

ant_value_t jit_helper_put_global(
  sv_vm_t *vm, ant_t *js, ant_value_t val,
  const char *str, uint32_t len, int is_strict
);

ant_value_t jit_helper_array(
  sv_vm_t *vm, ant_t *js,
  ant_value_t *elements, int count
);

ant_value_t jit_helper_for_of(
  sv_vm_t *vm, ant_t *js,
  ant_value_t iterable, ant_value_t *iter_buf
);

ant_value_t jit_helper_iter_next(
  sv_vm_t *vm, ant_t *js,
  ant_value_t *iter_buf, int hint
);

void jit_helper_destructure_close(
  sv_vm_t *vm, ant_t *js,
  ant_value_t *iter_buf
);

ant_value_t jit_helper_destructure_next(
  sv_vm_t *vm, ant_t *js,
  ant_value_t *iter_buf
);

ant_value_t jit_helper_throw_error(
  sv_vm_t *vm, ant_t *js,
  const char *str, uint32_t len, int err_type
);

ant_value_t jit_helper_get_elem2(
  sv_vm_t *vm, ant_t *js,
  ant_value_t obj, ant_value_t key
);
ant_value_t jit_helper_get_elem_inline(
  sv_vm_t *vm, ant_t *js,
  ant_value_t obj, ant_value_t key
);

ant_value_t jit_helper_set_proto(
  sv_vm_t *vm, ant_t *js,
  ant_value_t obj, ant_value_t proto
);

ant_value_t jit_helper_new(
  sv_vm_t *vm, ant_t *js,
  ant_value_t func, ant_value_t new_target,
  ant_value_t *args, int argc
);

ant_value_t jit_helper_str_append_local(
  sv_vm_t *vm, ant_t *js, sv_func_t *func,
  ant_value_t *args, int argc,
  ant_value_t *locals, uint16_t slot_idx,
  ant_value_t rhs
);

ant_value_t jit_helper_str_append_local_snapshot(
  sv_vm_t *vm, ant_t *js, sv_func_t *func,
  ant_value_t *args, int argc,
  ant_value_t *locals, uint16_t slot_idx,
  ant_value_t lhs, ant_value_t rhs
);

ant_value_t jit_helper_str_read_value(
  sv_vm_t *vm, ant_t *js, 
  ant_value_t value
);

ant_value_t jit_helper_str_flush_local(
  sv_vm_t *vm, ant_t *js, sv_func_t *func,
  ant_value_t *args, int argc,
  ant_value_t *locals, uint16_t slot_idx
);

#endif
