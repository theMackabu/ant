#ifndef SV_SUPER_H
#define SV_SUPER_H

#include "silver/engine.h"

static inline void sv_op_get_super(sv_vm_t *vm, ant_t *js) {
  ant_value_t obj = vm->stack[vm->sp - 1];
  vm->stack[vm->sp - 1] = js_get_proto(js, obj);
}

static inline void sv_op_get_super_val(sv_vm_t *vm, ant_t *js, sv_frame_t *frame) {
  ant_value_t prop = vm->stack[vm->sp - 1];
  ant_value_t obj = vm->stack[vm->sp - 2];
  ant_value_t receiver = vm->stack[vm->sp - 3];
  
  sv_func_t *current_func = frame ? frame->func : NULL;
  bool use_ctor_prototype =
    vtype(obj) == kTypeFunction &&
    vtype(receiver) != kTypeFunction &&
    current_func &&
    !current_func->is_static;

  ant_value_t proto;
  if (use_ctor_prototype) proto = js_getprop_fallback(js, obj, "prototype");
  else proto = js_get_proto(js, obj);
  vm->stack[vm->sp - 2] = proto;
  
  if (vtype(prop) == kTypeSymbol) {
    ant_value_t res = js_get_sym_with_receiver(js, proto, prop, receiver);
    vm->sp -= 3;
    vm->stack[vm->sp++] = res;
    return;
  }
  
  ant_value_t key_str = coerce_to_str(js, prop);
  vm->stack[vm->sp - 1] = key_str;
  
  ant_offset_t klen;
  ant_offset_t koff = vstr(js, key_str, &klen);
  
  const char *kptr = (const char *)(uintptr_t)(koff);
  ant_value_t res = js_getprop_super(js, proto, receiver, kptr);
  
  vm->sp -= 3;
  vm->stack[vm->sp++] = res;
}

static inline void sv_op_put_super_val(sv_vm_t *vm, ant_t *js) {
  ant_value_t val = vm->stack[vm->sp - 1];
  ant_value_t prop = vm->stack[vm->sp - 2];
  ant_value_t this = vm->stack[vm->sp - 4];
  ant_value_t key_str = coerce_to_str(js, prop);
  
  vm->stack[vm->sp - 2] = key_str;
  js_setprop(js, this, key_str, val);
  vm->sp -= 4;
}

#endif
