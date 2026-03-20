#ifndef SV_ARITHMETIC_H
#define SV_ARITHMETIC_H

#include <math.h>
#include "tokens.h"
#include "errors.h"

#include "silver/engine.h"
#include "modules/bigint.h"

static inline ant_value_t sv_op_add(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[--vm->sp];
  if (vtype(l) == T_NUM && vtype(r) == T_NUM) {
    vm->stack[vm->sp++] = tov(tod(l) + tod(r));
    return tov(0);
  }
  ant_value_t lu = unwrap_primitive(js, l);
  ant_value_t ru = unwrap_primitive(js, r);
  if (vtype(lu) == T_BIGINT && vtype(ru) == T_BIGINT) {
    ant_value_t res = bigint_add(js, lu, ru);
    vm->stack[vm->sp++] = res;
    return res;
  }
  if (vtype(lu) == T_BIGINT || vtype(ru) == T_BIGINT) {
    return js_mkerr(js, "Cannot mix BigInt value and other types");
  }
  if (is_non_numeric(lu) || is_non_numeric(ru)) {
    ant_value_t l_str = coerce_to_str_concat(js, l);
    if (is_err(l_str)) return l_str;
    ant_value_t r_str = coerce_to_str_concat(js, r);
    if (is_err(r_str)) return r_str;
    ant_value_t res = do_string_op(js, TOK_PLUS, l_str, r_str);
    vm->stack[vm->sp++] = res;
    return res;
  }
  vm->stack[vm->sp++] = tov(js_to_number(js, lu) + js_to_number(js, ru));
  return tov(0);
}

static inline ant_value_t sv_op_sub(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[--vm->sp];
  if (vtype(l) == T_NUM && vtype(r) == T_NUM) {
    vm->stack[vm->sp++] = tov(tod(l) - tod(r));
    return tov(0);
  }
  ant_value_t lu = unwrap_primitive(js, l);
  ant_value_t ru = unwrap_primitive(js, r);
  if (vtype(lu) == T_BIGINT && vtype(ru) == T_BIGINT) {
    ant_value_t res = bigint_sub(js, lu, ru);
    vm->stack[vm->sp++] = res;
    return res;
  }
  if (vtype(lu) == T_BIGINT || vtype(ru) == T_BIGINT)
    return js_mkerr(js, "Cannot mix BigInt value and other types");
  vm->stack[vm->sp++] = tov(js_to_number(js, lu) - js_to_number(js, ru));
  return tov(0);
}

static inline ant_value_t sv_op_mul(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[--vm->sp];
  if (vtype(l) == T_NUM && vtype(r) == T_NUM) {
    vm->stack[vm->sp++] = tov(tod(l) * tod(r));
    return tov(0);
  }
  ant_value_t lu = unwrap_primitive(js, l);
  ant_value_t ru = unwrap_primitive(js, r);
  if (vtype(lu) == T_BIGINT && vtype(ru) == T_BIGINT) {
    ant_value_t res = bigint_mul(js, lu, ru);
    vm->stack[vm->sp++] = res;
    return res;
  }
  if (vtype(lu) == T_BIGINT || vtype(ru) == T_BIGINT)
    return js_mkerr(js, "Cannot mix BigInt value and other types");
  vm->stack[vm->sp++] = tov(js_to_number(js, lu) * js_to_number(js, ru));
  return tov(0);
}

static inline ant_value_t sv_op_div(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[--vm->sp];
  if (vtype(l) == T_NUM && vtype(r) == T_NUM) {
    vm->stack[vm->sp++] = tov(tod(l) / tod(r));
    return tov(0);
  }
  ant_value_t lu = unwrap_primitive(js, l);
  ant_value_t ru = unwrap_primitive(js, r);
  if (vtype(lu) == T_BIGINT && vtype(ru) == T_BIGINT) {
    ant_value_t res = bigint_div(js, lu, ru);
    vm->stack[vm->sp++] = res;
    return res;
  }
  if (vtype(lu) == T_BIGINT || vtype(ru) == T_BIGINT)
    return js_mkerr(js, "Cannot mix BigInt value and other types");
  vm->stack[vm->sp++] = tov(js_to_number(js, lu) / js_to_number(js, ru));
  return tov(0);
}

static inline ant_value_t sv_op_mod(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[--vm->sp];
  if (vtype(l) == T_NUM && vtype(r) == T_NUM) {
    vm->stack[vm->sp++] = tov(fmod(tod(l), tod(r)));
    return tov(0);
  }
  ant_value_t lu = unwrap_primitive(js, l);
  ant_value_t ru = unwrap_primitive(js, r);
  if (vtype(lu) == T_BIGINT && vtype(ru) == T_BIGINT) {
    ant_value_t res = bigint_mod(js, lu, ru);
    vm->stack[vm->sp++] = res;
    return res;
  }
  if (vtype(lu) == T_BIGINT || vtype(ru) == T_BIGINT)
    return js_mkerr(js, "Cannot mix BigInt value and other types");
  vm->stack[vm->sp++] = tov(fmod(js_to_number(js, lu), js_to_number(js, ru)));
  return tov(0);
}

static inline ant_value_t sv_op_exp(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[--vm->sp];
  if (vtype(l) == T_NUM && vtype(r) == T_NUM) {
    vm->stack[vm->sp++] = tov(pow(tod(l), tod(r)));
    return tov(0);
  }
  ant_value_t lu = unwrap_primitive(js, l);
  ant_value_t ru = unwrap_primitive(js, r);
  if (vtype(lu) == T_BIGINT && vtype(ru) == T_BIGINT) {
    ant_value_t res = bigint_exp(js, lu, ru);
    vm->stack[vm->sp++] = res;
    return res;
  }
  if (vtype(lu) == T_BIGINT || vtype(ru) == T_BIGINT)
    return js_mkerr(js, "Cannot mix BigInt value and other types");
  vm->stack[vm->sp++] = tov(pow(js_to_number(js, lu), js_to_number(js, ru)));
  return tov(0);
}

static inline ant_value_t sv_op_neg(sv_vm_t *vm, ant_t *js) {
  ant_value_t a = vm->stack[--vm->sp];
  if (vtype(a) == T_BIGINT) {
    ant_value_t res = bigint_neg(js, a);
    vm->stack[vm->sp++] = res;
    return res;
  }
  if (is_object_type(a)) {
    ant_value_t prim = js_to_primitive(js, a, 2);
    if (is_err(prim)) return prim;
    vm->stack[vm->sp++] = tov(-js_to_number(js, prim));
    return tov(0);
  }
  vm->stack[vm->sp++] = tov(-js_to_number(js, a));
  return tov(0);
}

static inline ant_value_t sv_op_uplus(sv_vm_t *vm, ant_t *js) {
  ant_value_t a = vm->stack[--vm->sp];
  if (vtype(a) == T_BIGINT)
    return js_mkerr(js, "Cannot convert a BigInt value to a number");
  if (is_object_type(a)) {
    ant_value_t prim = js_to_primitive(js, a, 2);
    if (is_err(prim)) return prim;
    vm->stack[vm->sp++] = tov(js_to_number(js, prim));
    return tov(0);
  }
  vm->stack[vm->sp++] = tov(js_to_number(js, a));
  return tov(0);
}

static inline void sv_op_inc(sv_vm_t *vm) {
  vm->stack[vm->sp - 1] = tov(tod(vm->stack[vm->sp - 1]) + 1.0);
}

static inline void sv_op_dec(sv_vm_t *vm) {
  vm->stack[vm->sp - 1] = tov(tod(vm->stack[vm->sp - 1]) - 1.0);
}

static inline void sv_op_post_inc(sv_vm_t *vm) {
  ant_value_t old = vm->stack[vm->sp - 1];
  vm->stack[vm->sp++] = tov(tod(old) + 1.0);
}

static inline void sv_op_post_dec(sv_vm_t *vm) {
  ant_value_t old = vm->stack[vm->sp - 1];
  vm->stack[vm->sp++] = tov(tod(old) - 1.0);
}

static inline void sv_op_inc_local(sv_vm_t *vm, ant_value_t *lp, sv_func_t *func, uint8_t *ip) {
  uint8_t idx = sv_get_u8(ip + 1);
  ant_value_t *slot = &lp[idx];
  *slot = tov(tod(*slot) + 1.0);
  sv_tfb_record_local(func, (int)idx, *slot);
}

static inline void sv_op_dec_local(sv_vm_t *vm, ant_value_t *lp, sv_func_t *func, uint8_t *ip) {
  uint8_t idx = sv_get_u8(ip + 1);
  ant_value_t *slot = &lp[idx];
  *slot = tov(tod(*slot) - 1.0);
  sv_tfb_record_local(func, (int)idx, *slot);
  (void)vm;
}

static inline ant_value_t sv_op_add_local(sv_vm_t *vm, ant_value_t *lp, ant_t *js, sv_func_t *func, uint8_t *ip) {
  uint8_t idx = sv_get_u8(ip + 1);
  ant_value_t *slot = &lp[idx];
  ant_value_t val = vm->stack[--vm->sp];
  if (vtype(*slot) == T_NUM && vtype(val) == T_NUM) {
    *slot = tov(tod(*slot) + tod(val));
    sv_tfb_record_local(func, (int)idx, *slot);
    return tov(0);
  }
  vm->stack[vm->sp++] = *slot;
  vm->stack[vm->sp++] = val;
  ant_value_t err = sv_op_add(vm, js);
  if (is_err(err)) return err;
  *slot = vm->stack[--vm->sp];
  sv_tfb_record_local(func, (int)idx, *slot);
  return tov(0);
}

#endif
