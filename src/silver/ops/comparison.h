#ifndef SV_COMPARISON_H
#define SV_COMPARISON_H

#include "shapes.h"
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
  uint8_t lt = vtype(l), rty = vtype(r);

  if ((lt == T_NULL && rty == T_NULL) || (lt == T_UNDEF && rty == T_UNDEF) ||
      (lt == T_UNDEF && rty == T_NULL) || (lt == T_NULL && rty == T_UNDEF))
    return mkval(T_BOOL, 1);

  if (lt == T_NULL || rty == T_NULL || lt == T_UNDEF || rty == T_UNDEF)
    return mkval(T_BOOL, 0);

  if (lt == rty)
    return mkval(T_BOOL, strict_eq_values(js, l, r));

  if ((lt == T_BIGINT && rty == T_NUM) || (lt == T_NUM && rty == T_BIGINT)) {
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
  if (rty == T_BOOL) return sv_abstract_eq(js, l, tov(vdata(r) ? 1.0 : 0.0));

  if ((lt == T_NUM && rty == T_STR) || (lt == T_STR && rty == T_NUM))
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
  int cmp = memcmp((const void *)(uintptr_t)off1, (const void *)(uintptr_t)off2, min_len);
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
  uint8_t lt = vtype(l), rty = vtype(r);
  if (lt == T_NUM && rty == T_NUM) {
    vm->stack[vm->sp++] = mkval(T_BOOL, tod(l) < tod(r));
    return tov(0);
  }
  if (lt == T_BIGINT && rty == T_BIGINT) {
    vm->stack[vm->sp++] = mkval(T_BOOL, bigint_compare(js, l, r) < 0);
    return tov(0);
  }
  if (lt == T_BIGINT || rty == T_BIGINT)
    return js_mkerr(js, "Cannot mix BigInt value and other types");
  if (lt == T_STR && rty == T_STR) {
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
  uint8_t lt = vtype(l), rty = vtype(r);
  if (lt == T_NUM && rty == T_NUM) {
    vm->stack[vm->sp++] = mkval(T_BOOL, tod(l) <= tod(r));
    return tov(0);
  }
  if (lt == T_BIGINT && rty == T_BIGINT) {
    vm->stack[vm->sp++] = mkval(T_BOOL, bigint_compare(js, l, r) <= 0);
    return tov(0);
  }
  if (lt == T_BIGINT || rty == T_BIGINT)
    return js_mkerr(js, "Cannot mix BigInt value and other types");
  if (lt == T_STR && rty == T_STR) {
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
  uint8_t lt = vtype(l), rty = vtype(r);
  if (lt == T_NUM && rty == T_NUM) {
    vm->stack[vm->sp++] = mkval(T_BOOL, tod(l) > tod(r));
    return tov(0);
  }
  if (lt == T_BIGINT && rty == T_BIGINT) {
    vm->stack[vm->sp++] = mkval(T_BOOL, bigint_compare(js, l, r) > 0);
    return tov(0);
  }
  if (lt == T_BIGINT || rty == T_BIGINT)
    return js_mkerr(js, "Cannot mix BigInt value and other types");
  if (lt == T_STR && rty == T_STR) {
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
  uint8_t lt = vtype(l), rty = vtype(r);
  if (lt == T_NUM && rty == T_NUM) {
    vm->stack[vm->sp++] = mkval(T_BOOL, tod(l) >= tod(r));
    return tov(0);
  }
  if (lt == T_BIGINT && rty == T_BIGINT) {
    vm->stack[vm->sp++] = mkval(T_BOOL, bigint_compare(js, l, r) >= 0);
    return tov(0);
  }
  if (lt == T_BIGINT || rty == T_BIGINT)
    return js_mkerr(js, "Cannot mix BigInt value and other types");
  if (lt == T_STR && rty == T_STR) {
    vm->stack[vm->sp++] = mkval(T_BOOL, sv_strcmp(js, l, r) >= 0);
    return tov(0);
  }
  vm->stack[vm->sp++] = mkval(T_BOOL, js_to_number(js, l) >= js_to_number(js, r));
  return tov(0);
}

static inline sv_ic_entry_t *sv_instanceof_ic_slot(sv_func_t *func, uint8_t *ip) {
  if (!func || !func->ic_slots || !ip) return NULL;
  uint16_t ic_idx = sv_get_u16(ip + 1);
  if (ic_idx == UINT16_MAX || ic_idx >= func->ic_count) return NULL;
  return &func->ic_slots[ic_idx];
}

static inline sv_ic_entry_t *sv_isproto_ic_slot(sv_func_t *func, uint8_t *ip) {
  if (!func || !func->ic_slots || !ip) return NULL;
  uint16_t ic_idx = sv_get_u16(ip + 1);
  if (ic_idx == UINT16_MAX || ic_idx >= func->ic_count) return NULL;
  return &func->ic_slots[ic_idx];
}

static inline ant_value_t sv_instanceof_ic_eval(
  ant_t *js, ant_value_t l, ant_value_t r,
  sv_func_t *func, uint8_t *ip
) {
  sv_ic_entry_t *ic = sv_instanceof_ic_slot(func, ip);
  if (!ic || !is_object_type(l) || vtype(r) != T_FUNC) goto slow_path;

  uint32_t cur_epoch = ant_ic_epoch_counter;
  uintptr_t rhs_id = (uintptr_t)vdata(r);
  if (ic->epoch != cur_epoch || ic->cached_aux != rhs_id) goto slow_path;

  ant_object_t *lhs_ptr = js_obj_ptr(js_as_obj(l));
  if (lhs_ptr && lhs_ptr->shape == ic->cached_shape)
    return js_bool(ic->cached_index != 0);

slow_path:
  ant_value_t res = do_instanceof(js, l, r);
  lhs_ptr = is_object_type(l) ? js_obj_ptr(js_as_obj(l)) : NULL;
  
  if (!is_err(res) && ic && lhs_ptr && vtype(r) == T_FUNC && vtype(res) == T_BOOL) {
    ic->cached_shape = lhs_ptr->shape;
    ic->cached_holder = NULL;
    ic->cached_index = (uint32_t)(vdata(res) ? 1u : 0u);
    ic->epoch = ant_ic_epoch_counter;
    ic->cached_aux = (uintptr_t)vdata(r);
  }
  
  return res;
}

static inline ant_value_t sv_op_instanceof(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[--vm->sp];
  ant_value_t res = sv_instanceof_ic_eval(js, l, r, func, ip);
  if (!is_err(res)) vm->stack[vm->sp++] = res;
  return res;
}

static inline ant_value_t sv_isproto_ic_eval(
  ant_t *js, ant_value_t proto_obj, ant_value_t obj,
  sv_func_t *func, uint8_t *ip
) {
  sv_ic_entry_t *ic = sv_isproto_ic_slot(func, ip);
  ant_object_t *proto_ptr = is_object_type(proto_obj) ? js_obj_ptr(js_as_obj(proto_obj)) : NULL;
  ant_object_t *obj_ptr = is_object_type(obj) ? js_obj_ptr(js_as_obj(obj)) : NULL;

  if (
    ic && proto_ptr && obj_ptr &&
    ic->epoch == ant_ic_epoch_counter &&
    ic->cached_holder == proto_ptr &&
    (ant_object_t *)(uintptr_t)ic->cached_shape == obj_ptr
  ) {
    return js_bool(ic->cached_index != 0);
  }

  bool found = js_is_prototype_of(js, proto_obj, obj);
  if (ic && proto_ptr && obj_ptr) {
    ic->cached_holder = proto_ptr;
    ic->cached_shape = (ant_shape_t *)(uintptr_t)obj_ptr;
    ic->cached_index = found ? 1u : 0u;
    ic->epoch = ant_ic_epoch_counter;
  }
  return js_bool(found);
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
