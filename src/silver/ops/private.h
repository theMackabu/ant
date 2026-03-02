#ifndef SV_PRIVATE_H
#define SV_PRIVATE_H

#include "silver/engine.h"

static inline void sv_op_get_private(sv_vm_t *vm, ant_t *js) {
  jsval_t prop = vm->stack[--vm->sp];
  jsval_t obj = vm->stack[--vm->sp];
  jsval_t key_str = coerce_to_str(js, prop);
  jsoff_t klen;
  jsoff_t koff = vstr(js, key_str, &klen);
  const char *kptr = (const char *)&js->mem[koff];
  vm->stack[vm->sp++] = js_getprop_fallback(js, obj, kptr);
}

static inline void sv_op_put_private(sv_vm_t *vm, ant_t *js) {
  jsval_t prop = vm->stack[--vm->sp];
  jsval_t val = vm->stack[--vm->sp];
  jsval_t obj = vm->stack[--vm->sp];
  jsval_t key_str = coerce_to_str(js, prop);
  js_setprop(js, obj, key_str, val);
}

static inline void sv_op_def_private(sv_vm_t *vm, ant_t *js) {
  jsval_t val = vm->stack[--vm->sp];
  jsval_t prop = vm->stack[--vm->sp];
  jsval_t obj = vm->stack[vm->sp - 1];
  jsval_t key_str = coerce_to_str(js, prop);
  js_setprop(js, obj, key_str, val);
}

#endif
