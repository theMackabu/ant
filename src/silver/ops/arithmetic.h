#ifndef SV_ARITHMETIC_H
#define SV_ARITHMETIC_H

#include <math.h>
#include "tokens.h"
#include "errors.h"

#include "gc/roots.h"
#include "silver/feedback.h"
#include "modules/bigint.h"

static inline ant_value_t sv_add_to_primitive(ant_t *js, ant_value_t value) {
  return is_object_type(value) 
    ? js_to_primitive(js, value, 0) 
    : value;
}

static inline ant_value_t sv_op_add_bigints(
  sv_vm_t *vm, ant_t *js, ant_value_t l, ant_value_t r
) {
  ant_value_t res = bigint_add(js, l, r);
  vm->sp -= 2;
  vm->stack[vm->sp++] = res;
  return res;
}

static inline ant_value_t sv_op_add(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[vm->sp - 1];
  ant_value_t l = vm->stack[vm->sp - 2];

  if (vtype(l) == kTypeNumber && vtype(r) == kTypeNumber) {
    vm->sp -= 2;
    vm->stack[vm->sp++] = tov(tod(l) + tod(r));
    return tov(0);
  }

  if (vtype(l) == kTypeBigInt && vtype(r) == kTypeBigInt)
    return sv_op_add_bigints(vm, js, l, r);

  ant_value_t lu = sv_add_to_primitive(js, l);
  if (is_err(lu)) { vm->sp -= 2; return lu; }
  vm->stack[vm->sp - 2] = lu;

  ant_value_t ru = sv_add_to_primitive(js, r);
  if (is_err(ru)) { vm->sp -= 2; return ru; }
  vm->stack[vm->sp - 1] = ru;

  if (vtype(lu) == kTypeBigInt && vtype(ru) == kTypeBigInt)
    return sv_op_add_bigints(vm, js, lu, ru);

  if (vtype(lu) == kTypeSymbol || vtype(ru) == kTypeSymbol) {
    vm->sp -= 2;
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert a Symbol value");
  }

  if (is_non_numeric(lu) || is_non_numeric(ru)) {
    if (vtype(lu) == kTypeString && vtype(ru) == kTypeNumber) {
      ant_value_t res = do_string_num_concat(js, lu, ru, false);
      vm->sp -= 2;
      vm->stack[vm->sp++] = res;
      return res;
    }

    if (vtype(lu) == kTypeNumber && vtype(ru) == kTypeString) {
      ant_value_t res = do_string_num_concat(js, ru, lu, true);
      vm->sp -= 2;
      vm->stack[vm->sp++] = res;
      return res;
    }

    ant_value_t l_str = coerce_to_str_concat(js, lu);
    if (is_err(l_str)) { vm->sp -= 2; return l_str; }
    vm->stack[vm->sp - 2] = l_str;

    ant_value_t r_str = coerce_to_str_concat(js, ru);
    if (is_err(r_str)) { vm->sp -= 2; return r_str; }
    vm->stack[vm->sp - 1] = r_str;

    ant_value_t res = do_string_op(js, TOK_PLUS, l_str, r_str);
    vm->sp -= 2;
    vm->stack[vm->sp++] = res;

    return res;
  }

  if (vtype(lu) == kTypeBigInt || vtype(ru) == kTypeBigInt) {
    vm->sp -= 2;
    return js_mkerr(js, "Cannot mix BigInt value and other types");
  }

  double sum = js_to_number(js, lu) + js_to_number(js, ru);
  vm->sp -= 2;
  vm->stack[vm->sp++] = tov(sum);
  
  return tov(0);
}

static inline ant_value_t sv_op_sub(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[vm->sp - 1];
  ant_value_t l = vm->stack[vm->sp - 2];
  
  if (vtype(l) == kTypeNumber && vtype(r) == kTypeNumber) {
    vm->sp -= 2;
    vm->stack[vm->sp++] = tov(tod(l) - tod(r));
    return tov(0);
  }
  
  ant_value_t lu = unwrap_primitive(js, l);
  ant_value_t ru = unwrap_primitive(js, r);
  
  if (vtype(lu) == kTypeBigInt && vtype(ru) == kTypeBigInt) {
    ant_value_t res = bigint_sub(js, lu, ru);
    vm->sp -= 2;
    vm->stack[vm->sp++] = res;
    return res;
  }
  
  if (vtype(lu) == kTypeBigInt || vtype(ru) == kTypeBigInt) {
    vm->sp -= 2;
    return js_mkerr(js, "Cannot mix BigInt value and other types");
  }
  
  double num = js_to_number(js, lu) - js_to_number(js, ru);
  vm->sp -= 2;
  vm->stack[vm->sp++] = tov(num);
  
  return tov(0);
}

static inline ant_value_t sv_op_mul(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[vm->sp - 1];
  ant_value_t l = vm->stack[vm->sp - 2];
  
  if (vtype(l) == kTypeNumber && vtype(r) == kTypeNumber) {
    vm->sp -= 2;
    vm->stack[vm->sp++] = tov(tod(l) * tod(r));
    return tov(0);
  }
  
  ant_value_t lu = unwrap_primitive(js, l);
  ant_value_t ru = unwrap_primitive(js, r);
  
  if (vtype(lu) == kTypeBigInt && vtype(ru) == kTypeBigInt) {
    ant_value_t res = bigint_mul(js, lu, ru);
    vm->sp -= 2;
    vm->stack[vm->sp++] = res;
    return res;
  }
  
  if (vtype(lu) == kTypeBigInt || vtype(ru) == kTypeBigInt) {
    vm->sp -= 2;
    return js_mkerr(js, "Cannot mix BigInt value and other types");
  }
  
  double num = js_to_number(js, lu) * js_to_number(js, ru);
  vm->sp -= 2;
  vm->stack[vm->sp++] = tov(num);
  
  return tov(0);
}

static inline ant_value_t sv_op_div(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[vm->sp - 1];
  ant_value_t l = vm->stack[vm->sp - 2];
  
  if (vtype(l) == kTypeNumber && vtype(r) == kTypeNumber) {
    vm->sp -= 2;
    vm->stack[vm->sp++] = tov(tod(l) / tod(r));
    return tov(0);
  }
  
  ant_value_t lu = unwrap_primitive(js, l);
  ant_value_t ru = unwrap_primitive(js, r);
  
  if (vtype(lu) == kTypeBigInt && vtype(ru) == kTypeBigInt) {
    ant_value_t res = bigint_div(js, lu, ru);
    vm->sp -= 2;
    vm->stack[vm->sp++] = res;
    return res;
  }
  
  if (vtype(lu) == kTypeBigInt || vtype(ru) == kTypeBigInt) {
    vm->sp -= 2;
    return js_mkerr(js, "Cannot mix BigInt value and other types");
  }
  
  double num = js_to_number(js, lu) / js_to_number(js, ru);
  vm->sp -= 2;
  vm->stack[vm->sp++] = tov(num);
  
  return tov(0);
}

static inline ant_value_t sv_op_mod(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[vm->sp - 1];
  ant_value_t l = vm->stack[vm->sp - 2];
  
  if (vtype(l) == kTypeNumber && vtype(r) == kTypeNumber) {
    vm->sp -= 2;
    vm->stack[vm->sp++] = tov(fmod(tod(l), tod(r)));
    return tov(0);
  }
  
  ant_value_t lu = unwrap_primitive(js, l);
  ant_value_t ru = unwrap_primitive(js, r);
  
  if (vtype(lu) == kTypeBigInt && vtype(ru) == kTypeBigInt) {
    ant_value_t res = bigint_mod(js, lu, ru);
    vm->sp -= 2;
    vm->stack[vm->sp++] = res;
    return res;
  }
  
  if (vtype(lu) == kTypeBigInt || vtype(ru) == kTypeBigInt) {
    vm->sp -= 2;
    return js_mkerr(js, "Cannot mix BigInt value and other types");
  }
  
  double num = fmod(js_to_number(js, lu), js_to_number(js, ru));
  vm->sp -= 2;
  vm->stack[vm->sp++] = tov(num);
  
  return tov(0);
}

static inline ant_value_t sv_op_exp(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[vm->sp - 1];
  ant_value_t l = vm->stack[vm->sp - 2];
  
  if (vtype(l) == kTypeNumber && vtype(r) == kTypeNumber) {
    vm->sp -= 2;
    vm->stack[vm->sp++] = tov(pow(tod(l), tod(r)));
    return tov(0);
  }
  
  ant_value_t lu = unwrap_primitive(js, l);
  ant_value_t ru = unwrap_primitive(js, r);
  
  if (vtype(lu) == kTypeBigInt && vtype(ru) == kTypeBigInt) {
    ant_value_t res = bigint_exp(js, lu, ru);
    vm->sp -= 2;
    vm->stack[vm->sp++] = res;
    return res;
  }
  
  if (vtype(lu) == kTypeBigInt || vtype(ru) == kTypeBigInt) {
    vm->sp -= 2;
    return js_mkerr(js, "Cannot mix BigInt value and other types");
  }
  
  double num = pow(js_to_number(js, lu), js_to_number(js, ru));
  vm->sp -= 2;
  vm->stack[vm->sp++] = tov(num);
  
  return tov(0);
}

static inline ant_value_t sv_op_neg(sv_vm_t *vm, ant_t *js) {
  ant_value_t a = vm->stack[--vm->sp];
  if (vtype(a) == kTypeBigInt) {
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
  if (vtype(a) == kTypeBigInt)
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

static __attribute__((noinline)) ant_value_t sv_op_post_update_slow(sv_vm_t *vm, ant_t *js, bool increment) {
  ant_value_t old = vm->stack[vm->sp - 1];
  if (is_object_type(old) || vtype(old) == kTypeBuiltin) {
    old = js_to_primitive(js, old, 2);
    if (is_err(old)) return old;
  }
  
  if (vtype(old) == kTypeSymbol)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert a Symbol value to a number");
  
  ant_value_t next;
  if (vtype(old) == kTypeBigInt) {
    vm->stack[vm->sp - 1] = old;
    ant_value_t step = bigint_from_int64(js, increment ? 1 : -1);
    if (is_err(step)) return step;
    
    GC_ROOT_SAVE(mark, js);
    GC_ROOT_PIN(js, step);
    next = bigint_add(js, old, step);
    
    GC_ROOT_RESTORE(js, mark);
    if (is_err(next)) return next;
  } else {
    double number = js_to_number(js, old);
    old = tov(number);
    next = tov(increment ? number + 1.0 : number - 1.0);
  }
  
  vm->stack[vm->sp - 1] = old;
  vm->stack[vm->sp++] = next;
  
  return js_mkundef();
}

static inline __attribute__((always_inline)) ant_value_t sv_op_post_update(sv_vm_t *vm, ant_t *js, bool increment) {
  ant_value_t old = vm->stack[vm->sp - 1];
  if (vtype(old) != kTypeNumber) return sv_op_post_update_slow(vm, js, increment);
  double number = tod(old);
  vm->stack[vm->sp++] = tov(increment ? number + 1.0 : number - 1.0);
  return js_mkundef();
}

static inline ant_value_t sv_op_inc_local(ant_value_t *lp, ant_t *js, sv_func_t *func, uint8_t *ip) {
  uint8_t idx = sv_get_u8(ip + 1);
  ant_value_t *slot = &lp[idx];
  
  if (vtype(*slot) == kTypeString && str_is_heap_builder(*slot)) {
    ant_value_t out = str_materialize(js, *slot);
    if (is_err(out)) return out;
    *slot = out;
  }
  
  *slot = tov(tod(*slot) + 1.0);
  sv_tfb_record_local(func, (int)idx, *slot);
  
  return js_mkundef();
}

static inline ant_value_t sv_op_dec_local(ant_value_t *lp, ant_t *js, sv_func_t *func, uint8_t *ip) {
  uint8_t idx = sv_get_u8(ip + 1);
  ant_value_t *slot = &lp[idx];
  
  if (vtype(*slot) == kTypeString && str_is_heap_builder(*slot)) {
    ant_value_t out = str_materialize(js, *slot);
    if (is_err(out)) return out;
    *slot = out;
  }
  
  *slot = tov(tod(*slot) - 1.0);
  sv_tfb_record_local(func, (int)idx, *slot);
  
  return js_mkundef();
}

static inline ant_value_t sv_op_add_local(sv_vm_t *vm, ant_value_t *lp, ant_t *js, sv_func_t *func, uint8_t *ip) {
  uint8_t idx = sv_get_u8(ip + 1);
  ant_value_t *slot = &lp[idx];
  
  if (vtype(*slot) == kTypeString && str_is_heap_builder(*slot)) {
    ant_value_t out = str_materialize(js, *slot);
    if (is_err(out)) return out;
    *slot = out;
  }
  
  ant_value_t val = vm->stack[--vm->sp];
  if (vtype(*slot) == kTypeNumber && vtype(val) == kTypeNumber) {
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
