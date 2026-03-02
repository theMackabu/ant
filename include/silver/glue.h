#ifndef SILVER_GLUE_H
#define SILVER_GLUE_H

#ifdef ANT_JIT
#include "silver/engine.h"

double jit_helper_tod(jsval_t v);
uint64_t jit_helper_vtype(jsval_t v);
int64_t jit_helper_is_truthy(ant_t *js, jsval_t v);

jsval_t jit_helper_tov(double d);
jsval_t jit_helper_mkbool(int b);

bool jit_helper_stack_overflow(ant_t *js);
sv_closure_t *jit_helper_get_closure(jsval_t v);

jsval_t jit_helper_add(sv_vm_t *vm, ant_t *js, jsval_t l, jsval_t r);
jsval_t jit_helper_sub(sv_vm_t *vm, ant_t *js, jsval_t l, jsval_t r);
jsval_t jit_helper_mul(sv_vm_t *vm, ant_t *js, jsval_t l, jsval_t r);
jsval_t jit_helper_div(sv_vm_t *vm, ant_t *js, jsval_t l, jsval_t r);
jsval_t jit_helper_mod(sv_vm_t *vm, ant_t *js, jsval_t l, jsval_t r);

jsval_t jit_helper_object(sv_vm_t *vm, ant_t *js);
jsval_t jit_helper_get_length(sv_vm_t *vm, ant_t *js, jsval_t obj);
jsval_t jit_helper_catch_value(sv_vm_t *vm, ant_t *js, jsval_t err);
jsval_t jit_helper_throw(sv_vm_t *vm, ant_t *js, jsval_t val);

jsval_t jit_helper_lt(sv_vm_t *vm, ant_t *js, jsval_t l, jsval_t r);
jsval_t jit_helper_le(sv_vm_t *vm, ant_t *js, jsval_t l, jsval_t r);
jsval_t jit_helper_seq(sv_vm_t *vm, ant_t *js, jsval_t l, jsval_t r);
jsval_t jit_helper_eq(sv_vm_t *vm, ant_t *js, jsval_t l, jsval_t r);
jsval_t jit_helper_in(sv_vm_t *vm, ant_t *js, jsval_t l, jsval_t r);
jsval_t jit_helper_gt(sv_vm_t *vm, ant_t *js, jsval_t l, jsval_t r);
jsval_t jit_helper_ge(sv_vm_t *vm, ant_t *js, jsval_t l, jsval_t r);
jsval_t jit_helper_ne(sv_vm_t *vm, ant_t *js, jsval_t l, jsval_t r);
jsval_t jit_helper_sne(sv_vm_t *vm, ant_t *js, jsval_t l, jsval_t r);

jsval_t jit_helper_band(sv_vm_t *vm, ant_t *js, jsval_t l, jsval_t r);
jsval_t jit_helper_bor(sv_vm_t *vm, ant_t *js, jsval_t l, jsval_t r);
jsval_t jit_helper_bxor(sv_vm_t *vm, ant_t *js, jsval_t l, jsval_t r);
jsval_t jit_helper_bnot(sv_vm_t *vm, ant_t *js, jsval_t v);
jsval_t jit_helper_shl(sv_vm_t *vm, ant_t *js, jsval_t l, jsval_t r);
jsval_t jit_helper_shr(sv_vm_t *vm, ant_t *js, jsval_t l, jsval_t r);
jsval_t jit_helper_ushr(sv_vm_t *vm, ant_t *js, jsval_t l, jsval_t r);
jsval_t jit_helper_not(sv_vm_t *vm, ant_t *js, jsval_t v);

jsval_t jit_helper_get_global(ant_t *js, const char *str, uint32_t len);
jsval_t jit_helper_to_propkey(sv_vm_t *vm, ant_t *js, jsval_t v);
jsval_t jit_helper_stack_overflow_error(sv_vm_t *vm, ant_t *js);
jsval_t jit_helper_instanceof(sv_vm_t *vm, ant_t *js, jsval_t l, jsval_t r);
jsval_t jit_helper_delete(sv_vm_t *vm, ant_t *js, jsval_t obj, jsval_t key);
jsval_t jit_helper_typeof(sv_vm_t *vm, ant_t *js, jsval_t v);

jsval_t jit_helper_call(
  sv_vm_t *vm, ant_t *js,
  jsval_t func, jsval_t this_val,
  jsval_t *args, int argc
);

jsval_t jit_helper_get_field(
  sv_vm_t *vm, ant_t *js, jsval_t obj,
  const char *str, uint32_t len, 
  sv_func_t *func, int32_t bc_off
);

jsval_t jit_helper_closure(
  sv_vm_t *vm, ant_t *js,
  sv_closure_t *parent_closure, jsval_t this_val,
  jsval_t *args, int argc,
  uint32_t const_idx, jsval_t *locals, int n_locals
);

jsval_t jit_helper_bailout_resume(
  sv_vm_t *vm, sv_closure_t *closure,
  jsval_t this_val, jsval_t *args, int argc,
  jsval_t *vstack, int64_t vstack_sp,
  jsval_t *locals, int64_t n_locals,
  int64_t bc_offset
);

void jit_helper_close_upval(
  sv_vm_t *vm, uint16_t slot_idx,
  jsval_t *locals, int n_locals
);

void jit_helper_define_field(
  sv_vm_t *vm, ant_t *js, jsval_t obj,
  jsval_t val, const char *str, uint32_t len
);

void jit_helper_set_name(
  sv_vm_t *vm, ant_t *js, jsval_t fn,
  const char *str, uint32_t len
);

jsval_t jit_helper_put_field(
  sv_vm_t *vm, ant_t *js, jsval_t obj,
  jsval_t val, const char *str, uint32_t len
);

jsval_t jit_helper_get_elem(
  sv_vm_t *vm, ant_t *js,
  jsval_t obj, jsval_t key, sv_func_t *func, int32_t bc_off
);

jsval_t jit_helper_put_elem(
  sv_vm_t *vm, ant_t *js,
  jsval_t obj, jsval_t key, jsval_t val
);

jsval_t jit_helper_put_global(
  sv_vm_t *vm, ant_t *js, jsval_t val,
  const char *str, uint32_t len, int is_strict
);

jsval_t jit_helper_array(
  sv_vm_t *vm, ant_t *js,
  jsval_t *elements, int count
);

jsval_t jit_helper_throw_error(
  sv_vm_t *vm, ant_t *js,
  const char *str, uint32_t len, int err_type
);

jsval_t jit_helper_get_elem2(
  sv_vm_t *vm, ant_t *js,
  jsval_t obj, jsval_t key
);

jsval_t jit_helper_set_proto(
  sv_vm_t *vm, ant_t *js,
  jsval_t obj, jsval_t proto
);

jsval_t jit_helper_new(
  sv_vm_t *vm, ant_t *js,
  jsval_t func, jsval_t new_target,
  jsval_t *args, int argc
);

#endif
#endif
