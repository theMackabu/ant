#ifndef SV_RETURNS_H
#define SV_RETURNS_H

#include "silver/engine.h"

static inline ant_value_t sv_op_halt(sv_vm_t *vm, sv_frame_t *frame) {
  ant_value_t r = vm->sp > 0 ? vm->stack[--vm->sp] : js_mkundef();
  vm->sp = frame->prev_sp;
  return r;
}

#endif
