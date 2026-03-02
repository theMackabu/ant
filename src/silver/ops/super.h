#ifndef SV_SUPER_H
#define SV_SUPER_H

#include "silver/engine.h"

static inline void sv_op_get_super(sv_vm_t *vm, ant_t *js) {
  jsval_t obj = vm->stack[vm->sp - 1];
  vm->stack[vm->sp - 1] = js_get_proto(js, obj);
}

static inline void sv_op_get_super_val(sv_vm_t *vm, ant_t *js) {
  jsval_t prop = vm->stack[--vm->sp];
  jsval_t obj = vm->stack[--vm->sp];
  jsval_t receiver = vm->stack[--vm->sp];
  jsval_t proto = js_get_proto(js, obj);
  jsval_t key_str = coerce_to_str(js, prop);
  jsoff_t klen;
  jsoff_t koff = vstr(js, key_str, &klen);
  const char *kptr = (const char *)&js->mem[koff];
  vm->stack[vm->sp++] = js_getprop_super(js, proto, receiver, kptr);
}

static inline void sv_op_put_super_val(sv_vm_t *vm, ant_t *js) {
  jsval_t val = vm->stack[--vm->sp];
  jsval_t prop = vm->stack[--vm->sp];
  vm->sp--;
  jsval_t this = vm->stack[--vm->sp];
  jsval_t key_str = coerce_to_str(js, prop);
  js_setprop(js, this, key_str, val);
}

#endif
