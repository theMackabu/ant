#ifndef SV_CALLS_H
#define SV_CALLS_H

#include <limits.h>
#include "silver/engine.h"

typedef struct {
  jsval_t *args;
  int      argc;
  jsval_t *alloc;
} sv_call_args_t;

static inline void sv_call_args_reset(sv_call_args_t *a, jsval_t *args, int argc) {
  a->args = args;
  a->argc = argc;
  a->alloc = NULL;
}

static inline void sv_call_args_release(sv_call_args_t *a) {
  if (a->alloc) free(a->alloc);
  a->alloc = NULL;
}

static inline jsval_t sv_apply_normalize_args(ant_t *js, sv_call_args_t *a) {
  if (a->argc != 1) return js_mkundef();

  jsval_t arg_array = a->args[0];
  uint8_t t = vtype(arg_array);
  if (t == T_UNDEF || t == T_NULL) {
    a->args = NULL;
    a->argc = 0;
    return js_mkundef();
  }
  if (t != T_ARR) {
    return js_mkerr_typed(js, JS_ERR_TYPE,
      "apply arguments must be an array or null/undefined");
  }

  jsoff_t len = js_arr_len(js, arg_array);
  if (len <= 0) {
    a->args = NULL;
    a->argc = 0;
    return js_mkundef();
  }

  if (len > INT_MAX)
    return js_mkerr(js, "too many arguments");

  a->alloc = malloc((size_t)len * sizeof(jsval_t));
  if (!a->alloc) return js_mkerr(js, "out of memory");
  for (jsoff_t i = 0; i < len; i++)
    a->alloc[i] = js_arr_get(js, arg_array, i);
  a->args = a->alloc;
  a->argc = (int)len;
  return js_mkundef();
}

static inline jsval_t sv_op_new(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  uint16_t argc = sv_get_u16(ip + 1);
  jsval_t *args = &vm->stack[vm->sp - argc];
  jsval_t new_target = vm->stack[vm->sp - argc - 1];
  jsval_t func = vm->stack[vm->sp - argc - 2];
  js->new_target = new_target;

  if (vtype(func) == T_OBJ && is_proxy(js, func)) {
    jsval_t result = js_proxy_construct(js, func, args, argc, new_target);
    vm->sp -= argc + 2;
    if (is_err(result)) return result;
    vm->stack[vm->sp++] = result;
    return result;
  }

  jsval_t obj = mkobj(js, 0);

  if (vtype(func) == T_FUNC) {
    jsval_t proto_source = func;
    jsval_t func_obj = js_func_obj(func);
    jsval_t target_func = js_get_slot(js, func_obj, SLOT_TARGET_FUNC);
    if (vtype(target_func) == T_FUNC)
      proto_source = target_func;
    jsval_t proto = js_getprop_fallback(js, proto_source, "prototype");
    if (is_object_type(proto)) js_set_proto(js, obj, proto);
  }

  jsval_t ctor_this = obj;
  jsval_t result = sv_vm_call(vm, js, func, obj, args, argc, &ctor_this, true);
  vm->sp -= argc + 2;
  if (is_err(result)) return result;
  vm->stack[vm->sp++] = 
    is_object_type(result) ? result
    : (is_object_type(ctor_this) ? ctor_this : obj);
  return result;
}

static inline jsval_t sv_op_apply(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  uint16_t argc = sv_get_u16(ip + 1);
  jsval_t *args = &vm->stack[vm->sp - argc];
  jsval_t this = vm->stack[vm->sp - argc - 1];
  jsval_t func = vm->stack[vm->sp - argc - 2];
  sv_call_args_t call;
  
  sv_call_args_reset(&call, args, (int)argc);
  jsval_t norm = sv_apply_normalize_args(js, &call);
  if (is_err(norm)) return norm;

  jsval_t result = sv_vm_call(vm, js, func, this, call.args, call.argc, NULL, false);
  sv_call_args_release(&call);
  vm->sp -= argc + 2;
  if (!is_err(result)) vm->stack[vm->sp++] = result;
  return result;
}

static inline jsval_t sv_op_eval(sv_vm_t *vm, ant_t *js, sv_frame_t *frame, uint8_t *ip) {
  jsval_t code = vm->stack[--vm->sp];
  if (vtype(code) != T_STR) {
    vm->stack[vm->sp++] = code;
    return code;
  }
  jsoff_t len;
  jsoff_t off = vstr(js, code, &len);
  const char *str = (const char *)&js->mem[off];
  jsval_t result = js_eval_bytecode_eval_with_strict(
    js, str, len, sv_frame_is_strict(frame));
  if (!is_err(result)) vm->stack[vm->sp++] = result;
  return result;
}

static inline jsval_t sv_op_check_ctor(sv_vm_t *vm, ant_t *js) {
  if (vtype(sv_vm_get_new_target(vm, js)) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE,
      "Class constructor cannot be invoked without 'new'");
  return tov(0);
}

static inline void sv_op_check_ctor_ret(sv_vm_t *vm, sv_frame_t *frame) {
  jsval_t val = vm->stack[vm->sp - 1];
  if (is_object_type(val)) {
    vm->stack[vm->sp++] = val;
  } else vm->stack[vm->sp++] = frame->this;
}

#endif
