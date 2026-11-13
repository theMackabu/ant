#pragma once

#include "silver/engine.h"
#include "globals.h"

#include <stdlib.h>
#include <string.h>

static inline void sv_op_not(sv_vm_t *vm, ant_t *js) {
  ant_value_t a = vm->stack[--vm->sp];
  vm->stack[vm->sp++] = mkval(kTypeBool, !js_truthy(js, a));
}

static inline void sv_op_typeof(sv_vm_t *vm, ant_t *js) {
  ant_value_t a = vm->stack[--vm->sp];
  const char *ts = is_callable(a) ? "function" : typestr(vtype(a));
  vm->stack[vm->sp++] = js_mkstr(js, ts, strlen(ts));
}

static inline void sv_op_void(sv_vm_t *vm) {
  vm->sp--;
  vm->stack[vm->sp++] = mkval(kTypeUndefined, 0);
}

static inline ant_value_t sv_op_delete(sv_vm_t *vm, ant_t *js) {
  ant_value_t result = Ant_Delete(js, vm->stack[vm->sp - 2], vm->stack[vm->sp - 1]);
  vm->sp -= 2;
  if (is_err(result)) return result;
  vm->stack[vm->sp++] = result;
  return Ant_Value_Undefined;
}

static inline void sv_op_delete_var(
  sv_vm_t *vm, ant_t *js, sv_func_t *func, uint8_t *ip
) {
  sv_atom_t *a = &func->atoms[sv_get_u32(ip + 1)];
  ant_value_t result = sv_global_delete(js, a->str, a->len);
  vm->stack[vm->sp++] = result;
}

static inline void sv_op_delete_eval_var(
  sv_vm_t *vm, ant_t *js,
  sv_frame_t *frame, sv_func_t *func, uint8_t *ip
) {
  sv_atom_t *a = &func->atoms[sv_get_u32(ip + 1)];
  ant_value_t result = sv_env_delete(
    js, sv_frame_eval_env(js, frame), 
    a->str, a->len
  );
  vm->stack[vm->sp++] = result;
}
