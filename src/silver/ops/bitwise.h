#ifndef SV_BITWISE_H
#define SV_BITWISE_H

#include "errors.h"
#include "silver/engine.h"

static inline ant_value_t sv_op_band(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[--vm->sp];
  uint8_t lt = vtype(l), rt = vtype(r);
  if (lt == T_BIGINT || rt == T_BIGINT) {
    if (lt != T_BIGINT || rt != T_BIGINT)
      return js_mkerr(js, "Cannot mix BigInt value and other types");
    return js_mkerr_typed(js, JS_ERR_TYPE, "BigInt does not support bitwise ops");
  }
  int32_t ai = (lt == T_NUM) ? js_to_int32(tod(l)) : js_to_int32(js_to_number(js, l));
  int32_t bi = (rt == T_NUM) ? js_to_int32(tod(r)) : js_to_int32(js_to_number(js, r));
  vm->stack[vm->sp++] = tov((double)(ai & bi));
  return tov(0);
}

static inline ant_value_t sv_op_bor(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[--vm->sp];
  uint8_t lt = vtype(l), rt = vtype(r);
  if (lt == T_BIGINT || rt == T_BIGINT) {
    if (lt != T_BIGINT || rt != T_BIGINT)
      return js_mkerr(js, "Cannot mix BigInt value and other types");
    return js_mkerr_typed(js, JS_ERR_TYPE, "BigInt does not support bitwise ops");
  }
  int32_t ai = (lt == T_NUM) ? js_to_int32(tod(l)) : js_to_int32(js_to_number(js, l));
  int32_t bi = (rt == T_NUM) ? js_to_int32(tod(r)) : js_to_int32(js_to_number(js, r));
  vm->stack[vm->sp++] = tov((double)(ai | bi));
  return tov(0);
}

static inline ant_value_t sv_op_bxor(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[--vm->sp];
  uint8_t lt = vtype(l), rt = vtype(r);
  if (lt == T_BIGINT || rt == T_BIGINT) {
    if (lt != T_BIGINT || rt != T_BIGINT)
      return js_mkerr(js, "Cannot mix BigInt value and other types");
    return js_mkerr_typed(js, JS_ERR_TYPE, "BigInt does not support bitwise ops");
  }
  int32_t ai = (lt == T_NUM) ? js_to_int32(tod(l)) : js_to_int32(js_to_number(js, l));
  int32_t bi = (rt == T_NUM) ? js_to_int32(tod(r)) : js_to_int32(js_to_number(js, r));
  vm->stack[vm->sp++] = tov((double)(ai ^ bi));
  return tov(0);
}

static inline void sv_op_bnot(sv_vm_t *vm, ant_t *js) {
  ant_value_t a = vm->stack[--vm->sp];
  vm->stack[vm->sp++] = tov((double)(~js_to_int32(js_to_number(js, a))));
}

static inline ant_value_t sv_op_shl(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[--vm->sp];
  uint8_t lt = vtype(l), rt = vtype(r);
  if (lt == T_BIGINT || rt == T_BIGINT) {
    if (lt != T_BIGINT || rt != T_BIGINT)
      return js_mkerr(js, "Cannot mix BigInt value and other types");
    uint64_t shift = 0;
    ant_value_t err = bigint_asint_bits(js, r, &shift);
    if (is_err(err)) return err;
    ant_value_t res = bigint_shift_left(js, l, shift);
    if (is_err(res)) return res;
    vm->stack[vm->sp++] = res;
    return tov(0);
  }
  int32_t ai = (lt == T_NUM) ? js_to_int32(tod(l)) : js_to_int32(js_to_number(js, l));
  uint32_t bi = (rt == T_NUM) ? js_to_uint32(tod(r)) : js_to_uint32(js_to_number(js, r));
  vm->stack[vm->sp++] = tov((double)(ai << (bi & 0x1f)));
  return tov(0);
}

static inline ant_value_t sv_op_shr(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[--vm->sp];
  uint8_t lt = vtype(l), rt = vtype(r);
  if (lt == T_BIGINT || rt == T_BIGINT) {
    if (lt != T_BIGINT || rt != T_BIGINT)
      return js_mkerr(js, "Cannot mix BigInt value and other types");
    uint64_t shift = 0;
    ant_value_t err = bigint_asint_bits(js, r, &shift);
    if (is_err(err)) return err;
    ant_value_t res = bigint_shift_right(js, l, shift);
    if (is_err(res)) return res;
    vm->stack[vm->sp++] = res;
    return tov(0);
  }
  int32_t ai = (lt == T_NUM) ? js_to_int32(tod(l)) : js_to_int32(js_to_number(js, l));
  uint32_t bi = (rt == T_NUM) ? js_to_uint32(tod(r)) : js_to_uint32(js_to_number(js, r));
  vm->stack[vm->sp++] = tov((double)(ai >> (bi & 0x1f)));
  return tov(0);
}

static inline ant_value_t sv_op_ushr(sv_vm_t *vm, ant_t *js) {
  ant_value_t r = vm->stack[--vm->sp];
  ant_value_t l = vm->stack[--vm->sp];
  uint8_t lt = vtype(l), rt = vtype(r);
  if (lt == T_BIGINT || rt == T_BIGINT) {
    if (lt != T_BIGINT || rt != T_BIGINT)
      return js_mkerr(js, "Cannot mix BigInt value and other types");
    uint64_t shift = 0;
    ant_value_t err = bigint_asint_bits(js, r, &shift);
    if (is_err(err)) return err;
    ant_value_t res = bigint_shift_right_logical(js, l, shift);
    if (is_err(res)) return res;
    vm->stack[vm->sp++] = res;
    return tov(0);
  }
  uint32_t ai = (lt == T_NUM) ? js_to_uint32(tod(l)) : js_to_uint32(js_to_number(js, l));
  uint32_t bi = (rt == T_NUM) ? js_to_uint32(tod(r)) : js_to_uint32(js_to_number(js, r));
  vm->stack[vm->sp++] = tov((double)(ai >> (bi & 0x1f)));
  return tov(0);
}

#endif
