#ifndef SILVER_GLUE_H
#define SILVER_GLUE_H

#ifdef ANT_JIT
#include "silver/engine.h"

int64_t jit_helper_stack_overflow(ant_t *js);
int64_t jit_helper_is_truthy(ant_t *js, ant_value_t v);

ant_value_t jit_helper_add(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r);
ant_value_t jit_helper_sub(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r);
ant_value_t jit_helper_mul(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r);
ant_value_t jit_helper_div(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r);
ant_value_t jit_helper_mod(sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r);

ant_value_t jit_helper_object(sv_vm_t *vm, ant_t *js);
ant_value_t jit_helper_get_length(sv_vm_t *vm, ant_t *js, ant_value_t obj);
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

ant_value_t jit_helper_delete(sv_vm_t *vm, ant_t *js, ant_value_t obj, ant_value_t key);
ant_value_t jit_helper_typeof(sv_vm_t *vm, ant_t *js, ant_value_t v);
ant_value_t jit_helper_special_obj(sv_vm_t *vm, ant_t *js, uint32_t which);

ant_value_t jit_helper_get_global(
  ant_t *js, const char *str,
  sv_func_t *func, int32_t bc_off
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

ant_value_t jit_helper_call_array_includes(
  sv_vm_t *vm, ant_t *js,
  ant_value_t call_func, ant_value_t call_this,
  ant_value_t *args, int argc
);

ant_value_t jit_helper_apply(
  sv_vm_t *vm, ant_t *js,
  ant_value_t func, ant_value_t this_val,
  ant_value_t *args, int argc
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
  sv_vm_t *vm, uint16_t slot_idx,
  ant_value_t *locals, int n_locals,
  sv_upvalue_t **open_upvalues
);

void jit_helper_take_open_upvalues(
  sv_vm_t *vm, sv_upvalue_t **open_upvalues,
  ant_value_t *slots, int slot_count
);

void jit_helper_adopt_open_upvalues(
  sv_vm_t *vm,
  sv_upvalue_t **open_upvalues
);

void jit_helper_define_field(
  sv_vm_t *vm, ant_t *js, ant_value_t obj,
  ant_value_t val, const char *str, uint32_t len
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

ant_value_t jit_helper_get_elem(
  sv_vm_t *vm, ant_t *js,
  ant_value_t obj, ant_value_t key, sv_func_t *func, int32_t bc_off
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

ant_value_t jit_helper_str_flush_local(
  sv_vm_t *vm, ant_t *js, sv_func_t *func,
  ant_value_t *args, int argc,
  ant_value_t *locals, uint16_t slot_idx
);

#endif
#endif
