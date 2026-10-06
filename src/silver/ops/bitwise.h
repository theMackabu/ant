#ifndef SV_BITWISE_H
#define SV_BITWISE_H

#include "errors.h"
#include "silver/engine.h"
#include "modules/bigint.h"
#include "gc/roots.h"
#include "internal.h"

static inline ant_value_t sv_bitwise_numbers(ant_t *js, sv_op_t op, ant_value_t l, ant_value_t r) {
  bool lb = vtype(l) == kTypeBigInt, rb = vtype(r) == kTypeBigInt;
  if (lb != rb) return js_mkerr(js, "Cannot mix BigInt value and other types");
  
  if (lb) {
    if (op == OP_BAND) return bigint_bitand(js, l, r);
    if (op == OP_BOR) return bigint_bitor(js, l, r);
    if (op == OP_BXOR) return bigint_bitxor(js, l, r);
    
    uint64_t shift = 0;
    ant_value_t err = bigint_asint_bits(js, r, &shift);
    if (is_err(err)) return err;
    if (op == OP_SHL) return bigint_shift_left(js, l, shift);
    if (op == OP_SHR) return bigint_shift_right(js, l, shift);
    return bigint_shift_right_logical(js, l, shift);
  }
  
  double a = tod(l), b = tod(r);
  uint32_t shift = js_to_uint32(b) & 0x1f;
  switch (op) {
    case OP_BAND: return tov((double)(js_to_int32(a) & js_to_int32(b)));
    case OP_BOR:  return tov((double)(js_to_int32(a) | js_to_int32(b)));
    case OP_BXOR: return tov((double)(js_to_int32(a) ^ js_to_int32(b)));
    case OP_SHL:  return tov((double)(int32_t)((uint32_t)js_to_int32(a) << shift));
    case OP_SHR:  return tov((double)(js_to_int32(a) >> shift));
    default:      return tov((double)(js_to_uint32(a) >> shift));
  }
}

static inline ant_value_t sv_bitwise_values(ant_t *js, sv_op_t op, ant_value_t l, ant_value_t r) {
  if (vtype(l) == kTypeNumber && vtype(r) == kTypeNumber) return sv_bitwise_numbers(js, op, l, r);
  
  GC_ROOT_SAVE(mark, js);
  ant_value_t ln = js_to_numeric(js, l);
  
  if (is_err(ln)) {
    GC_ROOT_RESTORE(js, mark);
    return ln;
  }
  
  GC_ROOT_PIN(js, ln);
  ant_value_t rn = js_to_numeric(js, r);
  
  if (is_err(rn)) {
    GC_ROOT_RESTORE(js, mark);
    return rn;
  }
  
  GC_ROOT_PIN(js, rn);
  ant_value_t res = sv_bitwise_numbers(js, op, ln, rn);
  GC_ROOT_RESTORE(js, mark);
  
  return res;
}

static inline ant_value_t sv_bitnot_value(ant_t *js, ant_value_t v) {
  ant_value_t n = js_to_numeric(js, v);
  if (is_err(n)) return n;
  if (vtype(n) == kTypeBigInt) return bigint_bitnot(js, n);
  return tov((double)(~js_to_int32(tod(n))));
}

static inline ant_value_t sv_op_bitwise(sv_vm_t *vm, ant_t *js, sv_op_t op) {
  ant_value_t res = sv_bitwise_values(js, op, vm->stack[vm->sp - 2], vm->stack[vm->sp - 1]);
  vm->sp -= 2;
  if (is_err(res)) return res;
  vm->stack[vm->sp++] = res;
  return tov(0);
}

static inline ant_value_t sv_op_band(sv_vm_t *vm, ant_t *js) { return sv_op_bitwise(vm, js, OP_BAND); }
static inline ant_value_t sv_op_bor(sv_vm_t *vm, ant_t *js)  { return sv_op_bitwise(vm, js, OP_BOR); }
static inline ant_value_t sv_op_bxor(sv_vm_t *vm, ant_t *js) { return sv_op_bitwise(vm, js, OP_BXOR); }
static inline ant_value_t sv_op_shl(sv_vm_t *vm, ant_t *js)  { return sv_op_bitwise(vm, js, OP_SHL); }
static inline ant_value_t sv_op_shr(sv_vm_t *vm, ant_t *js)  { return sv_op_bitwise(vm, js, OP_SHR); }
static inline ant_value_t sv_op_ushr(sv_vm_t *vm, ant_t *js) { return sv_op_bitwise(vm, js, OP_USHR); }

static inline ant_value_t sv_op_bnot(sv_vm_t *vm, ant_t *js) {
  ant_value_t res = sv_bitnot_value(js, vm->stack[vm->sp - 1]);
  vm->sp--;
  if (is_err(res)) return res;
  vm->stack[vm->sp++] = res;
  return tov(0);
}

#endif
