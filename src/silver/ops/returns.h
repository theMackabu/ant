#pragma once

#include "silver/engine.h"

static inline ant_value_t sv_op_halt(sv_vm_t *vm) {
  return vm->sp > 0 ? vm->stack[vm->sp - 1] : Ant_Value_Undefined;
}
