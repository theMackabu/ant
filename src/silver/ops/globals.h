#ifndef SV_GLOBALS_H
#define SV_GLOBALS_H

#include "silver/engine.h"
#include "errors.h"

static inline jsval_t sv_global_get(ant_t *js, const char *str, uint32_t len) {
  jsoff_t off = lkp(js, js->global, str, len);
  if (off == 0) return js_mkundef();
  return resolveprop(js, mkval(T_PROP, off));
}

static inline jsval_t sv_op_get_global(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  sv_atom_t *a = &func->atoms[sv_get_u32(ip + 1)];
  jsval_t super_val = sv_vm_get_super_val(vm);
  if (a->len == 5 && memcmp(a->str, "super", 5) == 0 &&
      vtype(super_val) != T_UNDEF) {
    jsval_t sv = super_val;
    vm->stack[vm->sp++] = sv;
    return sv;
  }
  jsval_t val = sv_global_get(js, a->str, a->len);
  if (is_undefined(val))
    return js_mkerr_typed(
      js, JS_ERR_REFERENCE, "'%.*s' is not defined",
      (int)a->len, a->str);
  vm->stack[vm->sp++] = val;
  return val;
}

static inline void sv_op_get_global_undef(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  sv_atom_t *a = &func->atoms[sv_get_u32(ip + 1)];
  vm->stack[vm->sp++] = sv_global_get(js, a->str, a->len);
}

static inline jsval_t sv_op_put_global(
  sv_vm_t *vm, ant_t *js,
  sv_frame_t *frame, sv_func_t *func, uint8_t *ip
) {
  sv_atom_t *a = &func->atoms[sv_get_u32(ip + 1)];
  if (sv_frame_is_strict(frame) && lkp(js, js->global, a->str, a->len) == 0)
    return js_mkerr_typed(
      js, JS_ERR_REFERENCE, "'%.*s' is not defined",
      (int)a->len, a->str);
  jsval_t key = js_mkstr(js, a->str, a->len);
  return js_setprop(js, js->global, key, vm->stack[--vm->sp]);
}

#endif
