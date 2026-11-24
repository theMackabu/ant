#pragma once

#include "silver/engine.h"
#include "silver/glue.h"

static inline ant_value_t sv_op_get_super_val(sv_vm_t *vm, ant_t *js, sv_frame_t *frame) {
  ant_value_t res = Silver_Super_Get(
    js, frame->func, vm->stack[vm->sp - 3],
    vm->stack[vm->sp - 2], vm->stack[vm->sp - 1], &vm->stack[vm->sp - 2]
  );
  if (is_err(res)) return res;
  
  vm->sp -= 3;
  vm->stack[vm->sp++] = res;
  return Ant_Value_Undefined;
}

static inline ant_value_t sv_op_get_super_field(sv_vm_t *vm, ant_t *js, sv_frame_t *frame, uint8_t *ip) {
  sv_atom_t *atom = &frame->func->atoms[sv_get_u32(ip + 1)];
  ant_value_t res = Silver_Super_GetField(js, frame->func, vm->stack[vm->sp - 2], vm->stack[vm->sp - 1], atom, sv_ic_slot_for_ip(frame->func, ip));
  if (is_err(res)) return res;
  
  vm->sp -= 2;
  vm->stack[vm->sp++] = res;
  return Ant_Value_Undefined;
}

static inline ant_value_t sv_op_put_super_field(sv_vm_t *vm, ant_t *js, sv_frame_t *frame, uint8_t *ip) {
  const sv_atom_t *atom = &frame->func->atoms[sv_get_u32(ip + 1)];
  ant_value_t res = Silver_Super_PutField(js, frame->func, vm->stack[vm->sp - 3], vm->stack[vm->sp - 2], atom, vm->stack[vm->sp - 1], sv_ic_slot_for_ip(frame->func, ip));
  if (is_err(res)) return res;
  
  vm->sp -= 3;
  vm->stack[vm->sp++] = res;
  return Ant_Value_Undefined;
}

static inline ant_value_t sv_op_put_super_val(sv_vm_t *vm, ant_t *js, sv_frame_t *frame) {
  ant_value_t res = Silver_Super_Put(
    js, frame->func, vm->stack[vm->sp - 4], vm->stack[vm->sp - 3],
    vm->stack[vm->sp - 2], vm->stack[vm->sp - 1], &vm->stack[vm->sp - 3]
  );
  if (is_err(res)) return res;
  
  vm->sp -= 4;
  vm->stack[vm->sp++] = res;
  return Ant_Value_Undefined;
}
