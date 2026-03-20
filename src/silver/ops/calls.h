#ifndef SV_CALLS_H
#define SV_CALLS_H

#include <limits.h>
#include "silver/engine.h"

typedef struct {
  ant_value_t *args;
  int      argc;
  ant_value_t *alloc;
} sv_call_args_t;

static inline void sv_call_args_reset(sv_call_args_t *a, ant_value_t *args, int argc) {
  a->args = args;
  a->argc = argc;
  a->alloc = NULL;
}

static inline void sv_call_args_release(sv_call_args_t *a) {
  if (a->alloc) free(a->alloc);
  a->alloc = NULL;
}

static inline ant_value_t sv_apply_normalize_args(ant_t *js, sv_call_args_t *a) {
  if (a->argc != 1) return js_mkundef();

  ant_value_t arg_array = a->args[0];
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

  ant_offset_t len = js_arr_len(js, arg_array);
  if (len <= 0) {
    a->args = NULL;
    a->argc = 0;
    return js_mkundef();
  }

  if (len > INT_MAX)
    return js_mkerr(js, "too many arguments");

  a->alloc = malloc((size_t)len * sizeof(ant_value_t));
  if (!a->alloc) return js_mkerr(js, "out of memory");
  for (ant_offset_t i = 0; i < len; i++)
    a->alloc[i] = js_arr_get(js, arg_array, i);
  a->args = a->alloc;
  a->argc = (int)len;
  return js_mkundef();
}

static inline ant_value_t sv_op_new(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  uint16_t argc = sv_get_u16(ip + 1);
  ant_value_t *args = &vm->stack[vm->sp - argc];
  ant_value_t new_target = vm->stack[vm->sp - argc - 1];
  ant_value_t func = vm->stack[vm->sp - argc - 2];
  ant_value_t record_func = func;
  js->new_target = new_target;

  if (vtype(func) == T_OBJ && is_proxy(func)) {
    ant_value_t result = js_proxy_construct(js, func, args, argc, new_target);
    vm->sp -= argc + 2;
    if (is_err(result)) return result;
    vm->stack[vm->sp++] = result;
    return result;
  }
  if (!js_is_constructor(js, func)) {
    vm->sp -= argc + 2;
    return js_mkerr_typed(js, JS_ERR_TYPE, "not a constructor");
  }

  ant_value_t proto = js_mkundef();
  if (vtype(func) == T_FUNC) {
    ant_value_t proto_source = func;
    ant_value_t func_obj = js_func_obj(func);
    ant_value_t target_func = js_get_slot(func_obj, SLOT_TARGET_FUNC);
    if (vtype(target_func) == T_FUNC) {
      proto_source = target_func;
      record_func = target_func;
    }
    proto = js_getprop_fallback(js, proto_source, "prototype");
  }

  ant_value_t obj = js_mkobj_with_inobj_limit(js, sv_tfb_ctor_inobj_limit(record_func));
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  ant_value_t ctor_this = obj;
  ant_value_t result = sv_vm_call(vm, js, func, obj, args, argc, &ctor_this, true);
  vm->sp -= argc + 2;
  if (is_err(result)) return result;
  ant_value_t final_obj =
    is_object_type(result) ? result
    : (is_object_type(ctor_this) ? ctor_this : obj);
  sv_tfb_record_ctor_prop_count(record_func, final_obj);
  vm->stack[vm->sp++] = final_obj;
  return result;
}

static inline ant_value_t sv_op_apply(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  uint16_t argc = sv_get_u16(ip + 1);
  ant_value_t *args = &vm->stack[vm->sp - argc];
  ant_value_t this = vm->stack[vm->sp - argc - 1];
  ant_value_t func = vm->stack[vm->sp - argc - 2];
  sv_call_args_t call;
  
  sv_call_args_reset(&call, args, (int)argc);
  ant_value_t norm = sv_apply_normalize_args(js, &call);
  if (is_err(norm)) return norm;

  ant_value_t result = sv_vm_call(vm, js, func, this, call.args, call.argc, NULL, false);
  sv_call_args_release(&call);
  vm->sp -= argc + 2;
  if (!is_err(result)) vm->stack[vm->sp++] = result;
  return result;
}

static inline ant_value_t sv_op_eval(sv_vm_t *vm, ant_t *js, sv_frame_t *frame, uint8_t *ip) {
  ant_value_t code = vm->stack[--vm->sp];
  if (vtype(code) != T_STR) {
    vm->stack[vm->sp++] = code;
    return code;
  }
  ant_offset_t len;
  ant_offset_t off = vstr(js, code, &len);
  const char *str = (const char *)(uintptr_t)(off);
  ant_value_t result = js_eval_bytecode_eval_with_strict(
    js, str, len, sv_frame_is_strict(frame));
  if (!is_err(result)) vm->stack[vm->sp++] = result;
  return result;
}

static inline ant_value_t sv_op_check_ctor(sv_vm_t *vm, ant_t *js) {
  if (vtype(sv_vm_get_new_target(vm, js)) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE,
      "Class constructor cannot be invoked without 'new'");
  return tov(0);
}

static inline void sv_op_check_ctor_ret(sv_vm_t *vm, sv_frame_t *frame) {
  ant_value_t val = vm->stack[vm->sp - 1];
  if (is_object_type(val)) {
    vm->stack[vm->sp++] = val;
  } else vm->stack[vm->sp++] = frame->this;
}

#endif
