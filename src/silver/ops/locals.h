#ifndef SV_LOCALS_H
#define SV_LOCALS_H

#include "silver/engine.h"
#include "errors.h"

static inline ant_value_t sv_op_get_local(
  sv_vm_t *vm, ant_value_t *lp,
  ant_t *js, sv_frame_t *frame, uint8_t *ip
) {
  uint16_t idx = sv_get_u16(ip + 1);
  ant_value_t value = lp[idx];
  if (vtype(value) == T_STR && str_is_heap_builder(value)) {
    value = str_materialize(js, value);
    if (is_err(value)) return value;
  }
  vm->stack[vm->sp++] = value;
  return js_mkundef();
}

static inline void sv_op_put_local(
  sv_vm_t *vm, ant_value_t *lp,
  sv_frame_t *frame, sv_func_t *func, uint8_t *ip
) {
  uint16_t idx = sv_get_u16(ip + 1);
  lp[idx] = vm->stack[--vm->sp];
  sv_tfb_record_local(func, (int)idx, lp[idx]);
}

static inline void sv_op_set_local(
  sv_vm_t *vm, ant_value_t *lp,
  sv_frame_t *frame, sv_func_t *func, uint8_t *ip
) {
  uint16_t idx = sv_get_u16(ip + 1);
  lp[idx] = vm->stack[vm->sp - 1];
  sv_tfb_record_local(func, (int)idx, lp[idx]);
}

static inline ant_value_t sv_op_get_local8(
  sv_vm_t *vm, ant_value_t *lp,
  ant_t *js, sv_frame_t *frame, uint8_t *ip
) {
  uint8_t idx = sv_get_u8(ip + 1);
  ant_value_t value = lp[idx];
  if (vtype(value) == T_STR && str_is_heap_builder(value)) {
    value = str_materialize(js, value);
    if (is_err(value)) return value;
  }
  vm->stack[vm->sp++] = value;
  return js_mkundef();
}

static inline void sv_op_put_local8(
  sv_vm_t *vm, ant_value_t *lp,
  sv_frame_t *frame, sv_func_t *func, uint8_t *ip
) {
  uint8_t idx = sv_get_u8(ip + 1);
  lp[idx] = vm->stack[--vm->sp];
  sv_tfb_record_local(func, (int)idx, lp[idx]);
}

static inline void sv_op_set_local8(
  sv_vm_t *vm, ant_value_t *lp,
  sv_frame_t *frame, sv_func_t *func, uint8_t *ip
) {
  uint8_t idx = sv_get_u8(ip + 1);
  lp[idx] = vm->stack[vm->sp - 1];
  sv_tfb_record_local(func, (int)idx, lp[idx]);
}

static inline void sv_op_set_local_undef(sv_frame_t *frame, ant_value_t *lp, uint8_t *ip) {
  uint16_t idx = sv_get_u16(ip + 1);
  lp[idx] = SV_TDZ;
}

static inline ant_value_t sv_op_get_local_chk(
  sv_vm_t *vm, ant_value_t *lp,
  ant_t *js, sv_frame_t *frame, sv_func_t *func, uint8_t *ip
) {
  uint16_t idx = sv_get_u16(ip + 1);
  ant_value_t val = lp[idx];
  if (val == SV_TDZ) {
    uint32_t ai = sv_get_u32(ip + 3);
    if (ai < (uint32_t)func->atom_count) {
      sv_atom_t *a = &func->atoms[ai];
      return js_mkerr_typed(
        js, JS_ERR_REFERENCE,
        "Cannot access '%.*s' before initialization", 
        (int)a->len, a->str
      );
    }
    return js_mkerr_typed(
      js, JS_ERR_REFERENCE,
      "Cannot access variable before initialization"
    );
  }
  if (vtype(val) == T_STR && str_is_heap_builder(val)) {
    val = str_materialize(js, val);
    if (is_err(val)) return val;
  }
  vm->stack[vm->sp++] = val;
  return val;
}

static inline ant_value_t sv_op_get_slot_raw(
  sv_vm_t *vm, ant_t *js, sv_frame_t *frame, uint8_t *ip
) {
  uint16_t slot_idx = sv_get_u16(ip + 1);
  ant_value_t *slot = sv_frame_slot_ptr(frame, slot_idx);
  if (!slot) return js_mkerr(js, "invalid frame slot");
  if (*slot == SV_TDZ) return js_mkerr_typed(js, JS_ERR_REFERENCE,
    "Cannot access variable before initialization"
  );
  vm->stack[vm->sp++] = *slot;
  return js_mkundef();
}

static inline ant_value_t sv_op_put_local_chk(
  sv_vm_t *vm, ant_value_t *lp,
  ant_t *js, sv_frame_t *frame, sv_func_t *func, uint8_t *ip
) {
  uint16_t idx = sv_get_u16(ip + 1);
  ant_value_t *slot = &lp[idx];
  if (*slot == SV_TDZ) {
    uint32_t ai = sv_get_u32(ip + 3);
    if (ai < (uint32_t)func->atom_count) {
      sv_atom_t *a = &func->atoms[ai];
      return js_mkerr_typed(
        js, JS_ERR_REFERENCE,
        "Cannot access '%.*s' before initialization", 
        (int)a->len, a->str
      );
    }
    return js_mkerr_typed(
      js, JS_ERR_REFERENCE,
      "Cannot access variable before initialization"
    );
  }
  *slot = vm->stack[--vm->sp];
  return *slot;
}

static inline ant_value_t sv_op_get_arg(sv_vm_t *vm, ant_t *js, sv_frame_t *frame, uint8_t *ip) {
  uint16_t idx = sv_get_u16(ip + 1);
  ant_value_t value = sv_frame_get_arg_value(frame, idx);
  if (vtype(value) == T_STR && str_is_heap_builder(value)) {
    value = str_materialize(js, value);
    if (is_err(value)) return value;
  }
  vm->stack[vm->sp++] = value;
  return js_mkundef();
}

static inline void sv_op_put_arg(sv_vm_t *vm, ant_t *js, sv_frame_t *frame, uint8_t *ip) {
  uint16_t idx = sv_get_u16(ip + 1);
  sv_frame_set_arg_value(js, frame, idx, vm->stack[--vm->sp]);
}

static inline void sv_op_set_arg(sv_vm_t *vm, ant_t *js, sv_frame_t *frame, uint8_t *ip) {
  uint16_t idx = sv_get_u16(ip + 1);
  sv_frame_set_arg_value(js, frame, idx, vm->stack[vm->sp - 1]);
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

static inline ant_value_t sv_op_str_append_local(
  sv_vm_t *vm, ant_t *js,
  sv_frame_t *frame, sv_func_t *func, uint8_t *ip
) {
  uint16_t idx = sv_get_u16(ip + 1);
  ant_value_t rhs = vm->stack[--vm->sp];
  return sv_string_builder_append_slot(vm, js, frame, func, idx, rhs);
}

static inline ant_value_t sv_op_str_append_local_snapshot(
  sv_vm_t *vm, ant_t *js,
  sv_frame_t *frame, sv_func_t *func, uint8_t *ip
) {
  uint16_t idx = sv_get_u16(ip + 1);
  ant_value_t rhs = vm->stack[--vm->sp];
  ant_value_t lhs = vm->stack[--vm->sp];
  return sv_string_builder_append_snapshot_slot(vm, js, frame, func, idx, lhs, rhs);
}

static inline ant_value_t sv_op_str_flush_local(
  sv_vm_t *vm, ant_t *js,
  sv_frame_t *frame, uint8_t *ip
) {
  uint16_t idx = sv_get_u16(ip + 1);
  ant_value_t flush = sv_string_builder_flush_slot(vm, js, frame, idx);
  if (is_err(flush)) return flush;
  return js_mkundef();
}

#endif
