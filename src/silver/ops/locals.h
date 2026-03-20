#ifndef SV_LOCALS_H
#define SV_LOCALS_H

#include "silver/engine.h"
#include "errors.h"

static inline void sv_op_get_local(sv_vm_t *vm, ant_value_t *lp, uint8_t *ip) {
  uint16_t idx = sv_get_u16(ip + 1);
  vm->stack[vm->sp++] = lp[idx];
}

static inline void sv_op_put_local(sv_vm_t *vm, ant_value_t *lp, sv_func_t *func, uint8_t *ip) {
  uint16_t idx = sv_get_u16(ip + 1);
  lp[idx] = vm->stack[--vm->sp];
  sv_tfb_record_local(func, (int)idx, lp[idx]);
}

static inline void sv_op_set_local(sv_vm_t *vm, ant_value_t *lp, sv_func_t *func, uint8_t *ip) {
  uint16_t idx = sv_get_u16(ip + 1);
  lp[idx] = vm->stack[vm->sp - 1];
  sv_tfb_record_local(func, (int)idx, lp[idx]);
}

static inline void sv_op_get_local8(sv_vm_t *vm, ant_value_t *lp, uint8_t *ip) {
  uint8_t idx = sv_get_u8(ip + 1);
  vm->stack[vm->sp++] = lp[idx];
}

static inline void sv_op_put_local8(sv_vm_t *vm, ant_value_t *lp, sv_func_t *func, uint8_t *ip) {
  uint8_t idx = sv_get_u8(ip + 1);
  lp[idx] = vm->stack[--vm->sp];
  sv_tfb_record_local(func, (int)idx, lp[idx]);
}

static inline void sv_op_set_local8(sv_vm_t *vm, ant_value_t *lp, sv_func_t *func, uint8_t *ip) {
  uint8_t idx = sv_get_u8(ip + 1);
  lp[idx] = vm->stack[vm->sp - 1];
  sv_tfb_record_local(func, (int)idx, lp[idx]);
}

static inline void sv_op_set_local_undef(ant_value_t *lp, uint8_t *ip) {
  uint16_t idx = sv_get_u16(ip + 1);
  lp[idx] = SV_TDZ;
}

static inline ant_value_t sv_op_get_local_chk(
  sv_vm_t *vm, ant_value_t *lp,
  ant_t *js, sv_func_t *func, uint8_t *ip
) {
  uint16_t idx = sv_get_u16(ip + 1);
  ant_value_t val = lp[idx];
  if (val == SV_TDZ) {
    uint32_t ai = sv_get_u32(ip + 3);
    if (ai < (uint32_t)func->atom_count) {
      sv_atom_t *a = &func->atoms[ai];
      return js_mkerr_typed(
        js, JS_ERR_REFERENCE,
        "Cannot access '%.*s' before initialization", (int)a->len, a->str);
    }
    return js_mkerr_typed(
      js, JS_ERR_REFERENCE,
      "Cannot access variable before initialization");
  }
  vm->stack[vm->sp++] = val;
  return val;
}

static inline ant_value_t sv_op_put_local_chk(
  sv_vm_t *vm, ant_value_t *lp,
  ant_t *js, sv_func_t *func, uint8_t *ip
) {
  uint16_t idx = sv_get_u16(ip + 1);
  ant_value_t *slot = &lp[idx];
  if (*slot == SV_TDZ) {
    uint32_t ai = sv_get_u32(ip + 3);
    if (ai < (uint32_t)func->atom_count) {
      sv_atom_t *a = &func->atoms[ai];
      return js_mkerr_typed(
        js, JS_ERR_REFERENCE,
        "Cannot access '%.*s' before initialization", (int)a->len, a->str);
    }
    return js_mkerr_typed(
      js, JS_ERR_REFERENCE,
      "Cannot access variable before initialization");
  }
  *slot = vm->stack[--vm->sp];
  return *slot;
}

static inline void sv_op_get_arg(sv_vm_t *vm, sv_frame_t *frame, uint8_t *ip) {
  uint16_t idx = sv_get_u16(ip + 1);
  vm->stack[vm->sp++] = sv_frame_get_arg_value(frame, idx);
}

static inline void sv_op_put_arg(sv_vm_t *vm, sv_frame_t *frame, uint8_t *ip) {
  uint16_t idx = sv_get_u16(ip + 1);
  sv_frame_set_arg_value(frame, idx, vm->stack[--vm->sp]);
}

static inline void sv_op_set_arg(sv_vm_t *vm, sv_frame_t *frame, uint8_t *ip) {
  uint16_t idx = sv_get_u16(ip + 1);
  sv_frame_set_arg_value(frame, idx, vm->stack[vm->sp - 1]);
}

static inline void sv_op_rest(
  sv_vm_t *vm, sv_frame_t *frame,
  ant_t *js, uint8_t *ip
) {
  uint16_t start = sv_get_u16(ip + 1);
  ant_value_t arr = js_mkarr(js);
  if (frame->bp) {
    for (int i = (int)start; i < frame->argc; i++)
      js_arr_push(js, arr, frame->bp[i]);
  }
  vm->stack[vm->sp++] = arr;
}

#endif
