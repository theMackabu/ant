#pragma once

#include "silver/engine.h"
#include "property.h"

static inline ant_value_t sv_op_get_field_opt(sv_vm_t *vm, ant_t *js, sv_func_t *func, uint8_t *ip) {
  uint32_t idx = sv_get_u32(ip + 1);
  sv_atom_t *a = &func->atoms[idx];
  ant_value_t obj = vm->stack[--vm->sp];
  uint8_t t = vtype(obj);
  if (t == kTypeNull || t == kTypeUndefined) {
    vm->stack[vm->sp++] = Ant_Value_Undefined;
    return Ant_Value_Undefined;
  } else {
    ant_value_t res = sv_prop_get_field_ic(js, obj, a, func, ip);
    if (is_err(res)) return res;
    vm->stack[vm->sp++] = res;
    return Ant_Value_Undefined;
  }
}

static inline ant_value_t sv_op_get_elem_opt(sv_vm_t *vm, ant_t *js, sv_func_t *func, uint8_t *ip) {
  ant_value_t key = vm->stack[--vm->sp];
  ant_value_t obj = vm->stack[--vm->sp];
  uint8_t t = vtype(obj);
  if (t == kTypeNull || t == kTypeUndefined) {
    vm->stack[vm->sp++] = Ant_Value_Undefined;
    return Ant_Value_Undefined;
  }
  vm->stack[vm->sp++] = obj;
  vm->stack[vm->sp++] = key;
  return sv_op_get_elem(vm, js, func, ip);
}
