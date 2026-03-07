#ifndef SV_COMPARISON_H
#define SV_COMPARISON_H

#include "silver/engine.h"
#include "modules/bigint.h"

static inline void sv_op_seq(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[--vm->sp];
  vm->stack[vm->sp++] = mkval(T_BOOL, strict_eq_values(js, l, r));
}

static inline void sv_op_sne(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[--vm->sp];
  vm->stack[vm->sp++] = mkval(T_BOOL, !strict_eq_values(js, l, r));
}

static inline ant_value_t sv_abstract_eq(ant_t *js, ant_value_t l, ant_value_t r) {
  uint8_t lt = vtype(l), rt = vtype(r);

  if ((lt == T_NULL && rt == T_NULL) || (lt == T_UNDEF && rt == T_UNDEF) ||
      (lt == T_UNDEF && rt == T_NULL) || (lt == T_NULL && rt == T_UNDEF))
    return mkval(T_BOOL, 1);

  if (lt == T_NULL || rt == T_NULL || lt == T_UNDEF || rt == T_UNDEF)
    return mkval(T_BOOL, 0);

  if (lt == rt)
    return mkval(T_BOOL, strict_eq_values(js, l, r));

  if ((lt == T_BIGINT && rt == T_NUM) || (lt == T_NUM && rt == T_BIGINT)) {
    double num_val = lt == T_NUM ? tod(l) : tod(r);
    ant_value_t bigint_val = lt == T_BIGINT ? l : r;
    if (isfinite(num_val) && num_val == trunc(num_val)) {
      bool neg = num_val < 0;
      if (neg) num_val = -num_val;
      char buf[64];
      snprintf(buf, sizeof(buf), "%.0f", num_val);
      return mkval(
        T_BOOL, bigint_compare(js, bigint_val,
        js_mkbigint(js, buf, strlen(buf), neg)) == 0);
    }
    return mkval(T_BOOL, 0);
  }

  if (lt == T_BOOL) return sv_abstract_eq(js, tov(vdata(l) ? 1.0 : 0.0), r);
  if (rt == T_BOOL) return sv_abstract_eq(js, l, tov(vdata(r) ? 1.0 : 0.0));

  if ((lt == T_NUM && rt == T_STR) || (lt == T_STR && rt == T_NUM))
    return mkval(T_BOOL, js_to_number(js, l) == js_to_number(js, r));

  if (is_object_type(l)) {
    ant_value_t lp = js_to_primitive(js, l, 0);
    if (!is_err(lp)) return sv_abstract_eq(js, lp, r);
  }
  if (is_object_type(r)) {
    ant_value_t rp = js_to_primitive(js, r, 0);
    if (!is_err(rp)) return sv_abstract_eq(js, l, rp);
  }
  return mkval(T_BOOL, 0);
}

static inline void sv_op_eq(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[--vm->sp];
  vm->stack[vm->sp++] = sv_abstract_eq(js, l, r);
}

static inline void sv_op_ne(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[--vm->sp];
  ant_value_t eq = sv_abstract_eq(js, l, r);
  vm->stack[vm->sp++] = mkval(T_BOOL, !vdata(eq));
}

static inline int sv_strcmp(ant_t *js, ant_value_t l, ant_value_t r) {
  ant_offset_t n1, off1 = vstr(js, l, &n1);
  ant_offset_t n2, off2 = vstr(js, r, &n2);
  ant_offset_t min_len = n1 < n2 ? n1 : n2;
  int cmp = memcmp(&js->mem[off1], &js->mem[off2], min_len);
  if (cmp == 0) return (n1 < n2) ? -1 : (n1 > n2) ? 1 : 0;
  return cmp < 0 ? -1 : 1;
}

static inline void sv_coerce_relational(ant_t *js, ant_value_t *l, ant_value_t *r) {
  if (is_object_type(*l)) {
    ant_value_t prim = js_to_primitive(js, *l, 2);
    if (!is_err(prim)) *l = prim;
  }
  if (is_object_type(*r)) {
    ant_value_t prim = js_to_primitive(js, *r, 2);
    if (!is_err(prim)) *r = prim;
  }
}

static inline ant_value_t sv_op_lt(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[--vm->sp];
  sv_coerce_relational(js, &l, &r);
  uint8_t lt = vtype(l), rt = vtype(r);
  if (lt == T_NUM && rt == T_NUM) {
    vm->stack[vm->sp++] = mkval(T_BOOL, tod(l) < tod(r));
    return tov(0);
  }
  if (lt == T_BIGINT && rt == T_BIGINT) {
    vm->stack[vm->sp++] = mkval(T_BOOL, bigint_compare(js, l, r) < 0);
    return tov(0);
  }
  if (lt == T_BIGINT || rt == T_BIGINT)
    return js_mkerr(js, "Cannot mix BigInt value and other types");
  if (lt == T_STR && rt == T_STR) {
    vm->stack[vm->sp++] = mkval(T_BOOL, sv_strcmp(js, l, r) < 0);
    return tov(0);
  }
  vm->stack[vm->sp++] = mkval(T_BOOL, js_to_number(js, l) < js_to_number(js, r));
  return tov(0);
}

static inline ant_value_t sv_op_le(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[--vm->sp];
  sv_coerce_relational(js, &l, &r);
  uint8_t lt = vtype(l), rt = vtype(r);
  if (lt == T_NUM && rt == T_NUM) {
    vm->stack[vm->sp++] = mkval(T_BOOL, tod(l) <= tod(r));
    return tov(0);
  }
  if (lt == T_BIGINT && rt == T_BIGINT) {
    vm->stack[vm->sp++] = mkval(T_BOOL, bigint_compare(js, l, r) <= 0);
    return tov(0);
  }
  if (lt == T_BIGINT || rt == T_BIGINT)
    return js_mkerr(js, "Cannot mix BigInt value and other types");
  if (lt == T_STR && rt == T_STR) {
    vm->stack[vm->sp++] = mkval(T_BOOL, sv_strcmp(js, l, r) <= 0);
    return tov(0);
  }
  vm->stack[vm->sp++] = mkval(T_BOOL, js_to_number(js, l) <= js_to_number(js, r));
  return tov(0);
}

static inline ant_value_t sv_op_gt(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[--vm->sp];
  sv_coerce_relational(js, &l, &r);
  uint8_t lt = vtype(l), rt = vtype(r);
  if (lt == T_NUM && rt == T_NUM) {
    vm->stack[vm->sp++] = mkval(T_BOOL, tod(l) > tod(r));
    return tov(0);
  }
  if (lt == T_BIGINT && rt == T_BIGINT) {
    vm->stack[vm->sp++] = mkval(T_BOOL, bigint_compare(js, l, r) > 0);
    return tov(0);
  }
  if (lt == T_BIGINT || rt == T_BIGINT)
    return js_mkerr(js, "Cannot mix BigInt value and other types");
  if (lt == T_STR && rt == T_STR) {
    vm->stack[vm->sp++] = mkval(T_BOOL, sv_strcmp(js, l, r) > 0);
    return tov(0);
  }
  vm->stack[vm->sp++] = mkval(T_BOOL, js_to_number(js, l) > js_to_number(js, r));
  return tov(0);
}

static inline ant_value_t sv_op_ge(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[--vm->sp];
  sv_coerce_relational(js, &l, &r);
  uint8_t lt = vtype(l), rt = vtype(r);
  if (lt == T_NUM && rt == T_NUM) {
    vm->stack[vm->sp++] = mkval(T_BOOL, tod(l) >= tod(r));
    return tov(0);
  }
  if (lt == T_BIGINT && rt == T_BIGINT) {
    vm->stack[vm->sp++] = mkval(T_BOOL, bigint_compare(js, l, r) >= 0);
    return tov(0);
  }
  if (lt == T_BIGINT || rt == T_BIGINT)
    return js_mkerr(js, "Cannot mix BigInt value and other types");
  if (lt == T_STR && rt == T_STR) {
    vm->stack[vm->sp++] = mkval(T_BOOL, sv_strcmp(js, l, r) >= 0);
    return tov(0);
  }
  vm->stack[vm->sp++] = mkval(T_BOOL, js_to_number(js, l) >= js_to_number(js, r));
  return tov(0);
}

static inline ant_value_t sv_op_instanceof(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[--vm->sp];
  ant_value_t res = do_instanceof(js, l, r);
  if (!is_err(res)) vm->stack[vm->sp++] = res;
  return res;
}

static inline ant_value_t sv_op_in(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[--vm->sp];
  ant_value_t res = do_in(js, l, r);
  if (!is_err(res)) vm->stack[vm->sp++] = res;
  return res;
}

static inline void sv_op_is_nullish(sv_vm_t *vm) {
  ant_value_t v = vm->stack[--vm->sp];
  uint8_t t = vtype(v);
  vm->stack[vm->sp++] = mkval(T_BOOL, t == T_NULL || t == T_UNDEF);
}

static inline void sv_op_is_undef_or_null(sv_vm_t *vm) {
  ant_value_t v = vm->stack[--vm->sp];
  uint8_t t = vtype(v);
  vm->stack[vm->sp++] = mkval(T_BOOL, t == T_NULL || t == T_UNDEF);
}

#endif
