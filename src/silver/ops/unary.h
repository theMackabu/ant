#ifndef SV_UNARY_H
#define SV_UNARY_H

#include "silver/engine.h"
#include <stdlib.h>
#include <string.h>

static inline void sv_op_not(sv_vm_t *vm, ant_t *js) {
  ant_value_t a = vm->stack[--vm->sp];
  vm->stack[vm->sp++] = mkval(T_BOOL, !js_truthy(js, a));
}

static inline void sv_op_typeof(sv_vm_t *vm, ant_t *js) {
  ant_value_t a = vm->stack[--vm->sp];
  const char *ts = typestr(vtype(a));
  vm->stack[vm->sp++] = js_mkstr(js, ts, strlen(ts));
}

static inline void sv_op_void(sv_vm_t *vm) {
  vm->sp--;
  vm->stack[vm->sp++] = mkval(T_UNDEF, 0);
}

static inline ant_value_t sv_op_delete(sv_vm_t *vm, ant_t *js) {
  ant_value_t key = vm->stack[--vm->sp];
  ant_value_t obj = vm->stack[--vm->sp];
  ant_value_t key_str = js_mkundef();

  if (vtype(key) == T_SYMBOL) {
    ant_value_t result = js_delete_sym_prop(js, obj, key);
    if (is_err(result)) return result;
    vm->stack[vm->sp++] = result;
    return js_mkundef();
  } else key_str = coerce_to_str(js, key);

  if (!is_err(key_str) && vtype(key_str) == T_STR) {
    ant_offset_t klen = 0;
    ant_offset_t koff = vstr(js, key_str, &klen);
    const char *kptr = (const char *)&js->mem[koff];
    ant_value_t result = js_delete_prop(js, obj, kptr, klen);
    if (is_err(result)) return result;
    vm->stack[vm->sp++] = result;
  } else vm->stack[vm->sp++] = mkval(T_BOOL, 0);

  return js_mkundef();
}

static inline void sv_op_delete_var(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  sv_atom_t *a = &func->atoms[sv_get_u32(ip + 1)];
  ant_value_t result = js_delete_prop(js, js->global, a->str, a->len);
  vm->stack[vm->sp++] = result;
}

#endif
