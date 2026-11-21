#pragma once

#include "silver/engine.h"
#include "silver/glue.h"

static inline void sv_op_get_super(sv_vm_t *vm, ant_t *js) {
  ant_value_t obj = vm->stack[vm->sp - 1];
  vm->stack[vm->sp - 1] = js_get_proto(js, obj);
}

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
