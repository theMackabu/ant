#ifndef SV_CALLS_H
#define SV_CALLS_H

#include <limits.h>
#include "silver/engine.h"
#include "gc/roots.h"
#include "eval_env.h"

typedef struct {
  ant_value_t *args;
  int argc;
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
  ant_value_t effective_new_target = new_target;

  if (vtype(func) == T_OBJ && is_proxy(func)) {
    js->new_target = new_target;
    ant_value_t result = js_proxy_construct(js, func, args, argc, new_target);
    vm->sp -= argc + 2;
    if (is_err(result)) return result;
    vm->stack[vm->sp++] = result;
    return result;
  }
  if (!js_is_constructor(func)) {
    vm->sp -= argc + 2;
    return js_mkerr_typed(js, JS_ERR_TYPE, "not a constructor");
  }

  ant_value_t proto = js_mkundef();
  if (vtype(func) == T_FUNC || vtype(func) == T_CFUNC) {
    proto = sv_prepare_construct_meta(
      js, func, new_target, &effective_new_target, &record_func
    );
    if (is_err(proto)) {
      vm->sp -= argc + 2;
      return proto;
    }
  }
  js->new_target = effective_new_target;

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

static inline ant_value_t sv_op_call_super(sv_vm_t *vm, ant_t *js, sv_frame_t *frame, uint8_t *ip) {
  uint16_t argc = sv_get_u16(ip + 1);
  
  ant_value_t *args = &vm->stack[vm->sp - argc];
  ant_value_t new_target = vm->stack[vm->sp - argc - 1];
  ant_value_t func = vm->stack[vm->sp - argc - 2];
  ant_value_t this_val = vm->stack[vm->sp - argc - 3];

  js->new_target = new_target;
  ant_value_t super_this = this_val;
  ant_value_t result = sv_vm_call(vm, js, func, this_val, args, argc, &super_this, true);
  vm->sp -= argc + 3;
  if (is_err(result)) return result;

  ant_value_t effective_this = is_object_type(result) ? result : super_this;
  if (frame) frame->this = effective_this;
  vm->stack[vm->sp++] = effective_this;
  
  return effective_this;
}

static inline ant_value_t sv_op_super_apply(sv_vm_t *vm, ant_t *js, sv_frame_t *frame, uint8_t *ip) {
  uint16_t argc = sv_get_u16(ip + 1);
  
  ant_value_t *args = &vm->stack[vm->sp - argc];
  ant_value_t new_target = vm->stack[vm->sp - argc - 1];
  ant_value_t func = vm->stack[vm->sp - argc - 2];
  ant_value_t this = vm->stack[vm->sp - argc - 3];
  sv_call_args_t call;

  sv_call_args_reset(&call, args, (int)argc);
  ant_value_t norm = sv_apply_normalize_args(js, &call);
  if (is_err(norm)) return norm;

  js->new_target = new_target;
  ant_value_t super_this = this;
  ant_value_t result = sv_vm_call(vm, js, func, this, call.args, call.argc, &super_this, true);
  sv_call_args_release(&call);
  vm->sp -= argc + 3;
  if (is_err(result)) return result;

  ant_value_t effective_this = is_object_type(result) ? result : super_this;
  if (frame) frame->this = effective_this;
  vm->stack[vm->sp++] = effective_this;
  
  return effective_this;
}

static inline ant_value_t sv_op_new_apply(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  uint16_t argc = sv_get_u16(ip + 1);
  ant_value_t *args = &vm->stack[vm->sp - argc];
  ant_value_t new_target = vm->stack[vm->sp - argc - 1];
  ant_value_t func = vm->stack[vm->sp - argc - 2];
  ant_value_t record_func = func;
  ant_value_t effective_new_target = new_target;

  sv_call_args_t call;
  sv_call_args_reset(&call, args, (int)argc);
  ant_value_t norm = sv_apply_normalize_args(js, &call);
  if (is_err(norm)) { vm->sp -= argc + 2; return norm; }

  if (vtype(func) == T_OBJ && is_proxy(func)) {
    js->new_target = new_target;
    ant_value_t result = js_proxy_construct(js, func, call.args, call.argc, new_target);
    sv_call_args_release(&call);
    vm->sp -= argc + 2;
    if (is_err(result)) return result;
    vm->stack[vm->sp++] = result;
    return result;
  }
  if (!js_is_constructor(func)) {
    sv_call_args_release(&call);
    vm->sp -= argc + 2;
    return js_mkerr_typed(js, JS_ERR_TYPE, "not a constructor");
  }

  ant_value_t proto = js_mkundef();
  if (vtype(func) == T_FUNC || vtype(func) == T_CFUNC) {
    proto = sv_prepare_construct_meta(
      js, func, new_target, &effective_new_target, &record_func
    );
    if (is_err(proto)) {
      sv_call_args_release(&call);
      vm->sp -= argc + 2;
      return proto;
    }
  }
  js->new_target = effective_new_target;

  ant_value_t obj = js_mkobj_with_inobj_limit(js, sv_tfb_ctor_inobj_limit(record_func));
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  ant_value_t ctor_this = obj;
  ant_value_t result = sv_vm_call(vm, js, func, obj, call.args, call.argc, &ctor_this, true);
  sv_call_args_release(&call);
  vm->sp -= argc + 2;
  if (is_err(result)) return result;
  ant_value_t final_obj =
    is_object_type(result) ? result
    : (is_object_type(ctor_this) ? ctor_this : obj);
  sv_tfb_record_ctor_prop_count(record_func, final_obj);
  vm->stack[vm->sp++] = final_obj;
  return result;
}

static inline ant_value_t sv_eval_in_frame(
  sv_vm_t *vm, ant_t *js, sv_frame_t *frame,
  const char *source, ant_offset_t source_len, uint32_t scope_index
) {
  sv_func_t *caller = frame ? frame->func : NULL;
  
  if (!caller) return js_eval_bytecode_eval_with_strict(js, source, source_len, false);
  const sv_eval_scope_t *scope = sv_func_eval_scope(caller, scope_index);
  ant_value_t parent_env = sv_frame_eval_env(js, frame);
  
  if (!scope) return js_eval_bytecode_eval_in_env_with_strict(
    js, source, source_len, 
    sv_frame_is_strict(frame), frame->this, parent_env
  );

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t env = js_mkobj(js);
  
  if (is_err(env)) {
    GC_ROOT_RESTORE(js, root_mark);
    return env;
  }
  
  GC_ROOT_PIN(js, parent_env);
  GC_ROOT_PIN(js, env);
  js_set_proto_wb(js, env, parent_env);

  sv_eval_env_state_t *state = sv_eval_env_state_create(vm, frame, scope);
  if (!state) {
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkerr(js, "failed to capture direct eval bindings");
  }

  if (!sv_eval_env_state_attach(env, state)) {
    free(state);
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkerr(js, "failed to attach direct eval bindings");
  }
  
  ant_value_t result = js_eval_bytecode_eval_in_env_with_strict(
    js, source, source_len, 
    sv_frame_is_strict(frame), frame->this, env
  );
  
  GC_ROOT_RESTORE(js, root_mark);
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
  uint32_t scope_index = sv_get_u32(ip + 1);
  
  ant_value_t result = sv_eval_in_frame(vm, js, frame, str, len, scope_index);
  if (!is_err(result)) vm->stack[vm->sp++] = result;
  
  return result;
}

static inline ant_value_t sv_op_check_ctor(sv_vm_t *vm, ant_t *js) {
  if (vtype(sv_vm_get_new_target(vm, js)) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, SV_CLASS_CTOR_CALL_ERROR);
  return tov(0);
}

static inline void sv_op_check_ctor_ret(sv_vm_t *vm, sv_frame_t *frame) {
  ant_value_t val = vm->stack[vm->sp - 1];
  if (is_object_type(val)) {
    vm->stack[vm->sp++] = val;
  } else vm->stack[vm->sp++] = frame->this;
}

static inline bool sv_op_call_call_fused(
  sv_vm_t *vm, ant_t *js, ant_value_t xv,
  ant_value_t *args1, int n1, ant_value_t *args2, int n2,
  bool materialize_args2, ant_value_t *result_out
) {
#ifdef ANT_JIT
  if (n1 == 1 && vtype(xv) == T_FUNC) {
    sv_closure_t *c1 = js_func_closure(xv);
    sv_func_t *f1 = c1->func;
    if (
      f1 && f1->is_curried_step &&
      !(c1->call_flags & (SV_CALL_HAS_BOUND_ARGS | SV_CALL_HAS_EVAL_ENV))
    ) {
      uint32_t kidx = sv_get_u32(f1->code + 1);
      sv_func_t *f2 = (sv_func_t *)(uintptr_t)vdata(f1->constants[kidx]);
      if (!f2->jit_code) return false;

      // TODO: reduce nesting
      if (materialize_args2) {
        for (int i = 0; i < n2; i++) {
          ant_value_t value = args2[i];
          if (vtype(value) == T_STR && str_is_heap_builder(value)) {
            value = sv_string_builder_read_value(js, value);
            if (is_err(value)) {
              *result_out = value;
              return true;
            }
            args2[i] = value;
          }
        }
      }

      if (sv_check_c_stack_overflow(js)) {
        *result_out = js_mkerr_typed(js, JS_ERR_RANGE | JS_ERR_NO_STACK,
          "Maximum call stack size exceeded");
        return true;
      }

      sv_upvalue_t cell;
      cell.location = &cell.closed;
      cell.closed = args1[0];
      cell.next = NULL;
      cell.gc_epoch = 0;
      cell.in_remember_set = 1;

      sv_closure_t fake = {
        .call_flags = 0,
        .bound_argc = 0,
        .func = f2,
        .upvalues = NULL,
        .bound_this = js_mkundef(),
        .super_val = js_mkundef(),
        .func_obj = 0,
        .u.pending = { .name = NULL, .len = 0 },
        .js = js,
        .module_ctx = c1->module_ctx,
        .in_remember_set = 1,
        .gc_epoch = gc_get_epoch(),
      };
      fake.upvalues = fake.inline_upvals;

      for (int i = 0; i < f2->upvalue_count; i++) {
        sv_upval_desc_t *d = &f2->upval_descs[i];
        fake.inline_upvals[i] = d->is_local ? &cell : c1->upvalues[d->index];
      }

      js->new_target = js_mkundef();
      sv_jit_enter(js);
      ant_value_t result = ((sv_jit_func_t)f2->jit_code)(
        vm, js_mkundef(), js_mkundef(), js_mkundef(),
        args2, n2, &fake
      );
      sv_jit_leave(js);

      if (sv_is_jit_bailout(result)) {
        sv_jit_on_bailout(f2);
        sv_call_ctx_t ctx = {
          .this_val = js_mkundef(),
          .super_val = js_mkundef(),
          .args = args2,
          .argc = n2,
          .alloc = NULL,
        };
        result = sv_call_closure(vm, js, &fake, xv, &ctx, NULL);
      }
      sv_vm_maybe_checkpoint_microtasks(js);
      *result_out = result;
      return true;
    }
  }
#else
  (void)vm;
  (void)js;
  (void)xv;
  (void)args1;
  (void)n1;
  (void)args2;
  (void)n2;
  (void)materialize_args2;
  (void)result_out;
#endif
  return false;
}

static inline ant_value_t sv_op_call_call(
  sv_vm_t *vm, ant_t *js, ant_value_t xv,
  ant_value_t *args1, int n1, ant_value_t *args2, int n2
) {
  ant_value_t result;
  if (sv_op_call_call_fused(
        vm, js, xv, args1, n1, args2, n2, false, &result))
    return result;
  ant_value_t r = sv_vm_call(vm, js, xv, js_mkundef(), args1, n1, NULL, false);
  if (is_err(r)) return r;
  return sv_vm_call(vm, js, r, js_mkundef(), args2, n2, NULL, false);
}

static inline ant_value_t sv_op_call_call_slot(
  sv_vm_t *vm, ant_t *js, ant_value_t xv, ant_value_t arg1,
  uint16_t slot_idx
) {
  ant_value_t args1[1] = {arg1};
  sv_frame_t *frame = vm->fp >= 0 ? &vm->frames[vm->fp] : NULL;
  ant_value_t *slot = sv_frame_slot_ptr(frame, slot_idx);
  ant_value_t arg2 = slot ? *slot : js_mkundef();
  ant_value_t result;
  if (sv_op_call_call_fused(
        vm, js, xv, args1, 1, &arg2, 1, true, &result))
    return result;

  ant_value_t r = sv_vm_call(vm, js, xv, js_mkundef(), args1, 1, NULL, false);
  if (is_err(r)) return r;

  frame = vm->fp >= 0 ? &vm->frames[vm->fp] : NULL;
  slot = sv_frame_slot_ptr(frame, slot_idx);
  arg2 = slot ? *slot : js_mkundef();
  if (vtype(arg2) == T_STR && str_is_heap_builder(arg2)) {
    arg2 = sv_string_builder_read_value(js, arg2);
    if (is_err(arg2)) return arg2;
  }
  return sv_vm_call(vm, js, r, js_mkundef(), &arg2, 1, NULL, false);
}

static inline ant_value_t sv_op_call_call_slot_ptr(
  sv_vm_t *vm, ant_t *js, ant_value_t xv, ant_value_t arg1,
  ant_value_t *slot
) {
  ant_value_t args1[1] = {arg1};
  ant_value_t arg2 = slot ? *slot : js_mkundef();
  ant_value_t result;
  if (sv_op_call_call_fused(
        vm, js, xv, args1, 1, &arg2, 1, true, &result))
    return result;

  ant_value_t r = sv_vm_call(vm, js, xv, js_mkundef(), args1, 1, NULL, false);
  if (is_err(r)) return r;
  arg2 = slot ? *slot : js_mkundef();
  if (vtype(arg2) == T_STR && str_is_heap_builder(arg2)) {
    arg2 = sv_string_builder_read_value(js, arg2);
    if (is_err(arg2)) return arg2;
  }
  return sv_vm_call(vm, js, r, js_mkundef(), &arg2, 1, NULL, false);
}

#endif
