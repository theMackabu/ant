#ifndef SV_CALLS_H
#define SV_CALLS_H

#include <limits.h>
#include "silver/call.h"
#include "silver/jit.h"
#include "silver/feedback.h"
#include "gc/roots.h"
#include "eval_env.h"
#include "objects.h"

typedef struct {
  ant_value_t *args;
  int argc;
  bool rooted;
  gc_temp_root_scope_t roots;
} sv_call_args_t;

static inline ant_value_t sv_load_stable_builtin(
  ant_t *js, sv_stable_builtin_t kind, ant_value_t *receiver_out
) {
  *receiver_out = js_mkundef();
  if (kind != SV_STABLE_BUILTIN_PROMISE_RESOLVE)
    return js_mkerr(js, "invalid stable builtin");

  if (!js->promise_resolve_lookup_protector_invalid) {
    *receiver_out = js->sym.promise_ctor;
    return js->sym.promise_resolve;
  }

  ant_value_t receiver = js_mkundef();
  if (sv_global_sandboxed(js)) {
    if (!sv_sandbox_global_lookup(js, js->intern.promise, 7, &receiver))
      return js_mkerr_typed(js, JS_ERR_REFERENCE, "'Promise' is not defined");
  } else {
    if (!lkp_proto(js, js->global, js->intern.promise, 7).obj)
      return js_mkerr_typed(js, JS_ERR_REFERENCE, "'Promise' is not defined");
    receiver = js_getprop_fallback(js, js->global, js->intern.promise);
  }

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, receiver);
  
  if (is_err(receiver)) {
    GC_ROOT_RESTORE(js, root_mark);
    return receiver;
  }

  ant_value_t func = js_getprop_fallback(
    js, receiver, 
    js->intern.resolve
  );
  
  *receiver_out = receiver;
  GC_ROOT_RESTORE(js, root_mark);

  return func;
}

static inline ant_value_t sv_op_call_stable_builtin(
  sv_vm_t *vm, ant_t *js, sv_stable_builtin_t kind,
  ant_value_t call_func, ant_value_t call_this,
  ant_value_t *args, int argc
) {
  if (
    kind == SV_STABLE_BUILTIN_PROMISE_RESOLVE && argc == 1 &&
    call_this == js->sym.promise_ctor &&
    call_func == js->sym.promise_resolve
  ) return js_promise_assimilate_awaitable(js, args[0]);

  return sv_vm_call(
    vm, js, call_func, call_this, args, argc, NULL, js_mkundef());
}

static inline ant_value_t sv_op_call_char_code_at(
  sv_vm_t *vm, ant_t *js, ant_value_t func, ant_value_t receiver,
  ant_value_t *args, int argc
) {
  ant_value_t result;
  if (!js_try_char_code_at(js, func, receiver, args, argc, &result))
    return sv_vm_call(vm, js, func, receiver, args, argc, NULL, js_mkundef());
  sv_vm_maybe_checkpoint_microtasks(js);
  return result;
}

static inline ant_value_t sv_op_call_string_intrinsic(
  sv_vm_t *vm, ant_t *js, ant_string_intrinsic_kind_t kind,
  ant_value_t call_func, ant_value_t call_this,
  ant_value_t *args, int argc
) {
  if (js_string_intrinsic_builtin_matches(call_func, kind))
    return js_string_intrinsic_call(js, kind, call_this, args, argc);

  return sv_vm_call(
    vm, js, call_func, call_this, args, argc, NULL, js_mkundef());
}

static inline void sv_call_args_reset(sv_call_args_t *a, ant_value_t *args, int argc) {
  a->args = args;
  a->argc = argc;
  a->rooted = false;
}

static inline void sv_call_args_release(sv_call_args_t *a) {
  if (a->rooted) gc_temp_root_scope_end(&a->roots);
  a->rooted = false;
}

static constexpr int SV_STACK_ARGS_MAX = 1024;

static inline int sv_stack_args_cap(sv_vm_t *vm, int first) {
  int room = (vm->stack_size - first) / 2;
  return room < SV_STACK_ARGS_MAX ? room : SV_STACK_ARGS_MAX;
}

static inline ant_value_t *sv_call_args_root(ant_t *js, sv_call_args_t *a, int count) {
  gc_temp_root_scope_begin(js, &a->roots);
  a->rooted = true;
  ant_value_t *slots = gc_temp_root_slots(&a->roots, (size_t)count);
  if (!slots) sv_call_args_release(a);
  return slots;
}

static inline ant_value_t sv_call_args_place(
  ant_t *js, sv_call_args_t *a, int fixed,
  const ant_value_t *src, int len, int cap
) {
  if ((int64_t)fixed + len > INT_MAX) return js_mkerr(js, "too many arguments");
  ant_value_t *dst = a->args;
  
  if (fixed + len > cap) {
    dst = sv_call_args_root(js, a, fixed + len);
    if (!dst) return js_mkerr(js, "out of memory");
    if (fixed > 0) memcpy(dst, a->args, (size_t)fixed * sizeof(ant_value_t));
  }

  if (len > 0) memcpy(dst + fixed, src, (size_t)len * sizeof(ant_value_t));
  a->args = dst;
  a->argc = fixed + len;
  
  return js_mkundef();
}

static inline ant_value_t sv_apply_normalize_args(ant_t *js, sv_call_args_t *a, int cap) {
  if (a->argc != 1) return js_mkundef();

  ant_value_t arg_array = a->args[0];
  uint8_t t = vtype(arg_array);
  if (t == kTypeUndefined || t == kTypeNull) {
    a->args = NULL;
    a->argc = 0;
    return js_mkundef();
  }
  if (t != kTypeArray) {
    return js_mkerr_typed(js, JS_ERR_TYPE,
      "apply arguments must be an array or null/undefined");
  }

  int dense_len = 0;
  const ant_value_t *dense = Ant_Array_DenseView(arg_array, &dense_len);
  if (dense) return sv_call_args_place(js, a, 0, dense, dense_len, cap);

  ant_offset_t len = js_arr_len(js, arg_array);
  if (len <= 0) {
    a->args = NULL;
    a->argc = 0;
    return js_mkundef();
  }

  if (len > INT_MAX)
    return js_mkerr(js, "too many arguments");

  ant_value_t *slots = sv_call_args_root(js, a, (int)len);
  if (!slots) return js_mkerr(js, "out of memory");
  for (ant_offset_t i = 0; i < len; i++) slots[i] = js_arr_get(js, arg_array, i);
  
  a->args = slots;
  a->argc = (int)len;
  
  return js_mkundef();
}

static inline ant_value_t sv_spread_call_args(sv_vm_t *vm, ant_t *js, sv_call_args_t *a, int cap) {
  int fixed = a->argc - 1;
  ant_value_t iterable = a->args[fixed];
  int len = 0;

  if (vtype(iterable) == kTypeArray && js_array_iteration_default(js, iterable)) {
    const ant_value_t *dense = Ant_Array_DenseView(iterable, &len);
    if (dense) return sv_call_args_place(js, a, fixed, dense, len, cap);
  }

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, iterable);
  
  ant_value_t arr = js_mkarr(js);
  GC_ROOT_PIN(js, arr);

  ant_value_t status = is_err(arr) ? arr : sv_spread_into(vm, js, arr, iterable);
  if (!is_err(status)) {
    const ant_value_t *spread = Ant_Array_DenseView(arr, &len);
    status = spread
      ? sv_call_args_place(js, a, fixed, spread, len, cap)
      : js_mkerr(js, "invalid spread state");
  }

  GC_ROOT_RESTORE(js, root_mark);
  return status;
}

static inline ant_value_t sv_stack_call_args(
  sv_vm_t *vm, ant_t *js, sv_call_args_t *call,
  int first, int argc, bool spread
) {
  ant_value_t *args = &vm->stack[first];
  sv_call_args_reset(call, args, argc);
  
  int cap = sv_stack_args_cap(vm, first);
  ant_value_t status = spread
    ? sv_spread_call_args(vm, js, call, cap)
    : sv_apply_normalize_args(js, call, cap);
  
  if (!is_err(status) && call->args == args) vm->sp = first + call->argc;
  return status;
}

static inline ant_value_t sv_op_new(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  uint16_t argc = sv_get_u16(ip + 1);
  ant_value_t *args = &vm->stack[vm->sp - argc];
  ant_value_t new_target = vm->stack[vm->sp - argc - 1];
  ant_value_t func = vm->stack[vm->sp - argc - 2];
  ant_value_t effective_new_target = new_target;

  if (vtype(func) == kTypeObject && is_proxy(func)) {
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
  if (vtype(func) == kTypeFunction || vtype(func) == kTypeBuiltin) {
    proto = sv_prepare_construct_meta(js, func, new_target, &effective_new_target);
    
    if (is_err(proto)) {
      vm->sp -= argc + 2;
      return proto;
    }
  }

  ant_value_t obj = js_mkundef();
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, proto);
  GC_ROOT_PIN(js, obj);
  
  obj = Ant_Object_CreateBare(js);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  
  ant_value_t ctor_this = obj;
  ant_value_t result = sv_vm_call(vm, js, func, obj, args, argc, &ctor_this, effective_new_target);
  GC_ROOT_RESTORE(js, root_mark);
  
  vm->sp -= argc + 2;
  if (is_err(result)) return result;
  
  ant_value_t final_obj = is_object_type(result) 
    ? result
    : (is_object_type(ctor_this) ? ctor_this : obj);
  
  vm->stack[vm->sp++] = final_obj;
  
  return result;
}

static inline __attribute__((noinline)) ant_value_t sv_op_apply(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  uint16_t argc = sv_get_u16(ip + 1);
  int base = vm->sp - argc - 2;
  
  ant_value_t func = vm->stack[base];
  ant_value_t this = vm->stack[base + 1];
  sv_call_args_t call;

  ant_value_t status = sv_stack_call_args(vm, js, &call, base + 2, argc, false);
  if (is_err(status)) { vm->sp = base; return status; }

  ant_value_t result = sv_vm_call(vm, js, func, this, call.args, call.argc, NULL, js_mkundef());
  sv_call_args_release(&call);
  
  vm->sp = base;
  if (!is_err(result)) vm->stack[vm->sp++] = result;
  
  return result;
}

static inline __attribute__((noinline)) ant_value_t sv_op_call_spread(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  uint16_t argc = sv_get_u16(ip + 1);
  int base = vm->sp - argc - 2;
  
  ant_value_t func = vm->stack[base];
  ant_value_t this = vm->stack[base + 1];
  sv_call_args_t call;

  ant_value_t status = sv_stack_call_args(vm, js, &call, base + 2, argc, true);
  if (is_err(status)) { vm->sp = base; return status; }

  ant_value_t result = sv_vm_call(vm, js, func, this, call.args, call.argc, NULL, js_mkundef());
  sv_call_args_release(&call);
  
  vm->sp = base;
  if (!is_err(result)) vm->stack[vm->sp++] = result;
  
  return result;
}

static inline ant_value_t sv_super_call(
  sv_vm_t *vm, ant_t *js, sv_frame_t *frame,
  int base, ant_value_t *args, int argc
) {
  ant_value_t this_val = vm->stack[base];
  ant_value_t func = vm->stack[base + 1];
  ant_value_t new_target = vm->stack[base + 2];

  ant_value_t super_this = this_val;
  ant_value_t result = sv_vm_call(vm, js, func, this_val, args, argc, &super_this, new_target);
  
  vm->sp = base;
  if (is_err(result)) return result;

  ant_value_t effective_this = is_object_type(result) 
    ? result 
    : super_this;
  
  if (frame) frame->this = effective_this;
  vm->stack[vm->sp++] = effective_this;
  
  return effective_this;
}

static inline ant_value_t sv_op_call_super(sv_vm_t *vm, ant_t *js, sv_frame_t *frame, uint8_t *ip) {
  uint16_t argc = sv_get_u16(ip + 1);
  int base = vm->sp - argc - 3;
  return sv_super_call(vm, js, frame, base, &vm->stack[base + 3], argc);
}

static inline ant_value_t sv_op_super_forward(sv_vm_t *vm, ant_t *js, sv_frame_t *frame) {
  return sv_super_call(vm, js, frame, vm->sp - 3, frame ? frame->bp : NULL, frame ? frame->argc : 0);
}

static inline __attribute__((noinline)) ant_value_t sv_op_super_apply(sv_vm_t *vm, ant_t *js, sv_frame_t *frame, uint8_t *ip) {
  uint16_t argc = sv_get_u16(ip + 1);
  int base = vm->sp - argc - 3;
  sv_call_args_t call;

  ant_value_t status = sv_stack_call_args(vm, js, &call, base + 3, argc, false);
  if (is_err(status)) { vm->sp = base; return status; }

  ant_value_t result = sv_super_call(vm, js, frame, base, call.args, call.argc);
  sv_call_args_release(&call);
  
  return result;
}

static inline __attribute__((noinline)) ant_value_t sv_op_new_apply(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  uint16_t argc = sv_get_u16(ip + 1);
  int base = vm->sp - argc - 2;
  
  ant_value_t func = vm->stack[base];
  ant_value_t new_target = vm->stack[base + 1];
  ant_value_t effective_new_target = new_target;
  sv_call_args_t call;

  ant_value_t status = sv_stack_call_args(vm, js, &call, base + 2, argc, false);
  if (is_err(status)) { 
    vm->sp = base; 
    return status; 
  }

  if (vtype(func) == kTypeObject && is_proxy(func)) {
    ant_value_t result = js_proxy_construct(js, func, call.args, call.argc, new_target);
    sv_call_args_release(&call);
    vm->sp = base;
    
    if (is_err(result)) return result;
    vm->stack[vm->sp++] = result;
    
    return result;
  }
  
  if (!js_is_constructor(func)) {
    sv_call_args_release(&call);
    vm->sp = base;
    return js_mkerr_typed(js, JS_ERR_TYPE, "not a constructor");
  }

  ant_value_t proto = js_mkundef();
  if (vtype(func) == kTypeFunction || vtype(func) == kTypeBuiltin) {
    proto = sv_prepare_construct_meta(js, func, new_target, &effective_new_target);
    
    if (is_err(proto)) {
      sv_call_args_release(&call);
      vm->sp = base;
      return proto;
    }
  }

  ant_value_t obj = js_mkundef();
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, proto);
  GC_ROOT_PIN(js, obj);
  
  obj = Ant_Object_CreateBare(js);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  
  ant_value_t ctor_this = obj;
  ant_value_t result = sv_vm_call(vm, js, func, obj, call.args, call.argc, &ctor_this, effective_new_target);
  
  GC_ROOT_RESTORE(js, root_mark);
  sv_call_args_release(&call);
  
  vm->sp = base;
  if (is_err(result)) return result;
  
  ant_value_t final_obj = 
    is_object_type(result) ? result : 
    (is_object_type(ctor_this) ? ctor_this : obj);

  vm->stack[vm->sp++] = final_obj;

  return result;
}

static inline ant_value_t sv_eval_in_frame(
  sv_vm_t *vm, ant_t *js, sv_frame_t *frame, const char *source,
  ant_offset_t source_len, uint32_t scope_index, ant_value_t new_target
) {
  sv_func_t *caller = frame ? frame->func : NULL;
  
  if (!caller) return js_eval_bytecode_eval_with_strict(js, source, source_len, false);
  GC_ROOT_SAVE(root_mark, js);
  
  ant_value_t env = sv_eval_capture_env(vm, js, frame, scope_index);
  if (is_err(env)) { 
    GC_ROOT_RESTORE(js, root_mark); 
    return env;
  }
  
  GC_ROOT_PIN(js, env);
  ant_value_t result = js_eval_bytecode_eval_in_env_with_strict(
    js, source, source_len, sv_frame_is_strict(frame), 
    frame->this, env, new_target, caller->allows_new_target
  );
  
  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static inline ant_value_t sv_op_eval(sv_vm_t *vm, ant_t *js, sv_frame_t *frame, uint8_t *ip) {
  ant_value_t new_target = vm->stack[vm->sp - 1];
  ant_value_t code = vm->stack[vm->sp - 2];
  
  if (vtype(code) != kTypeString) {
    vm->sp -= 2;
    vm->stack[vm->sp++] = code;
    return code;
  }
  
  if (__builtin_expect(js->codegen_blocked & ANT_CODEGEN_NO_STRINGS, 0)) return js_codegen_refused(js);
  
  ant_offset_t len;
  ant_offset_t off = vstr(js, code, &len);
  
  const char *str = (const char *)(uintptr_t)(off);
  uint32_t scope_index = sv_get_u32(ip + 1);
  
  ant_value_t result = sv_eval_in_frame(vm, js, frame, str, len, scope_index, new_target);
  vm->sp -= 2;
  if (!is_err(result)) vm->stack[vm->sp++] = result;
  
  return result;
}

static inline ant_value_t sv_op_check_ctor(sv_vm_t *vm, ant_t *js) {
  if (vtype(sv_vm_get_new_target(vm)) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, SV_CLASS_CTOR_CALL_ERROR);
  return tov(0);
}

static inline void sv_op_check_ctor_ret(sv_vm_t *vm, sv_frame_t *frame) {
  ant_value_t val = vm->stack[vm->sp - 1];
  if (is_object_type(val)) {
    vm->stack[vm->sp++] = val;
  } else vm->stack[vm->sp++] = frame->this;
}

static inline ant_value_t sv_materialize_builder_args(ant_t *js, ant_value_t *args, int n) {
  for (int i = 0; i < n; i++) {
    if (vtype(args[i]) != kTypeString || !str_is_heap_builder(args[i])) continue;
    ant_value_t value = sv_string_builder_read_value(js, args[i]);
    if (is_err(value)) return value;
    args[i] = value;
  }
  return js_mkundef();
}

static inline bool sv_op_call_call_fused(
  sv_vm_t *vm, ant_t *js, ant_value_t xv,
  ant_value_t *args1, int n1, ant_value_t *args2, int n2,
  bool materialize_args2, ant_value_t *result_out
) {
  if (n1 == 1 && vtype(xv) == kTypeFunction) {
    sv_closure_t *c1 = js_func_closure(xv);
    sv_func_t *f1 = c1->func;
    if (
      f1 && f1->is_curried_step &&
      !(c1->call_flags & (SV_CALL_HAS_BOUND_ARGS | SV_CALL_HAS_EVAL_ENV))
    ) {
      uint32_t kidx = sv_get_u32(f1->code + 1);
      sv_func_t *f2 = (sv_func_t *)vptr(f1->constants[kidx]);
      sv_func_note_multi_closure(f2);
      if (!f2->jit_code) return false;

      if (materialize_args2) {
        ant_value_t err = sv_materialize_builder_args(js, args2, n2);
        if (is_err(err)) {
          *result_out = err;
          return true;
        }
      }

      if (sv_check_c_stack_overflow(js)) {
        *result_out = Ant_Error_CreateStackOverflow(js);
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
        .gc_epoch = gc_get_epoch(js),
      };
      fake.upvalues = fake.inline_upvals;

      for (int i = 0; i < f2->upvalue_count; i++) {
        sv_upval_desc_t *d = &f2->upval_descs[i];
        fake.inline_upvals[i] = d->is_local ? &cell : c1->upvalues[d->index];
      }

      ant_value_t result = sv_jit_invoke(js, SV_JIT_FROM_INTERP, f2->jit_code, vm, &(sv_call_ctx_t){
        .this_val = js_mkundef(),
        .new_target = js_mkundef(),
        .super_val = js_mkundef(),
        .args = args2,
        .argc = n2,
      }, &fake);

      if (sv_is_jit_bailout(result)) {
        sv_jit_on_bailout(f2);
        sv_call_ctx_t ctx = {
          .this_val = js_mkundef(),
          .super_val = js_mkundef(),
          .new_target = js_mkundef(),
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
  
  return false;
}

static inline ant_value_t sv_op_call_call(
  sv_vm_t *vm, ant_t *js, ant_value_t xv,
  ant_value_t *args1, int n1, ant_value_t *args2, int n2
) {
  ant_value_t result;
  if (sv_op_call_call_fused(vm, js, xv, args1, n1, args2, n2, false, &result)) return result;
  ant_value_t r = sv_vm_call(vm, js, xv, js_mkundef(), args1, n1, NULL, js_mkundef());
  if (is_err(r)) return r;
  return sv_vm_call(vm, js, r, js_mkundef(), args2, n2, NULL, js_mkundef());
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

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, arg2);

  if (sv_op_call_call_fused(vm, js, xv, args1, 1, &arg2, 1, true, &result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }

  ant_value_t r = sv_vm_call(vm, js, xv, js_mkundef(), args1, 1, NULL, js_mkundef());
  if (is_err(r)) { 
    GC_ROOT_RESTORE(js, root_mark);
    return r;
  }

  frame = vm->fp >= 0 ? &vm->frames[vm->fp] : NULL;
  slot = sv_frame_slot_ptr(frame, slot_idx);
  arg2 = slot ? *slot : js_mkundef();
  
  if (vtype(arg2) == kTypeString && str_is_heap_builder(arg2)) {
    GC_ROOT_PIN(js, r);
    arg2 = sv_string_builder_read_value(js, arg2);
    if (is_err(arg2)) { GC_ROOT_RESTORE(js, root_mark); return arg2; }
  }
  
  result = sv_vm_call(vm, js, r, js_mkundef(), &arg2, 1, NULL, js_mkundef());
  GC_ROOT_RESTORE(js, root_mark);
  
  return result;
}

static inline ant_value_t sv_op_call_call_slot_ptr(
  sv_vm_t *vm, ant_t *js, ant_value_t xv, ant_value_t arg1,
  ant_value_t *slot
) {
  ant_value_t args1[1] = {arg1};
  ant_value_t arg2 = slot ? *slot : js_mkundef();
  
  ant_value_t result;
  if (sv_op_call_call_fused(vm, js, xv, args1, 1, &arg2, 1, true, &result)) return result;

  ant_value_t r = sv_vm_call(vm, js, xv, js_mkundef(), args1, 1, NULL, js_mkundef());
  if (is_err(r)) return r;
  
  arg2 = slot ? *slot : js_mkundef();
  if (vtype(arg2) == kTypeString && str_is_heap_builder(arg2)) {
    arg2 = sv_string_builder_read_value(js, arg2);
    if (is_err(arg2)) return arg2;
  }
  
  return sv_vm_call(vm, js, r, js_mkundef(), &arg2, 1, NULL, js_mkundef());
}

#endif
