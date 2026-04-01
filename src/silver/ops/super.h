#ifndef SV_SUPER_H
#define SV_SUPER_H

#include "silver/engine.h"

static inline void sv_op_get_super(sv_vm_t *vm, ant_t *js) {
  ant_value_t obj = vm->stack[vm->sp - 1];
  vm->stack[vm->sp - 1] = js_get_proto(js, obj);
}

static inline void sv_op_get_super_val(sv_vm_t *vm, ant_t *js) {
  ant_value_t prop = vm->stack[--vm->sp];
  ant_value_t obj = vm->stack[--vm->sp];
  ant_value_t receiver = vm->stack[--vm->sp];
  ant_value_t proto;
  
  if (vtype(obj) == T_FUNC && vtype(receiver) != T_FUNC) {
    proto = js_getprop_fallback(js, obj, "prototype");
  } else proto = js_get_proto(js, obj);
  
  if (vtype(prop) == T_SYMBOL) {
    vm->stack[vm->sp++] = js_get_sym_with_receiver(js, proto, prop, receiver);
    return;
  }
  
  ant_value_t key_str = coerce_to_str(js, prop);
  ant_offset_t klen;
  ant_offset_t koff = vstr(js, key_str, &klen);
  
  const char *kptr = (const char *)(uintptr_t)(koff);
  vm->stack[vm->sp++] = js_getprop_super(js, proto, receiver, kptr);
}

static inline void sv_op_put_super_val(sv_vm_t *vm, ant_t *js) {
  ant_value_t val = vm->stack[--vm->sp];
  ant_value_t prop = vm->stack[--vm->sp];
  vm->sp--;
  ant_value_t this = vm->stack[--vm->sp];
  ant_value_t key_str = coerce_to_str(js, prop);
  js_setprop(js, this, key_str, val);
}

#endif
