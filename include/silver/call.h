#ifndef SILVER_CALL_H
#define SILVER_CALL_H

#include "silver/engine.h"
#include "silver/feedback.h"
#include "modules/timer.h"
#include "../../src/silver/ops/async.h"

static constexpr int SV_CALL_INLINE_ARGS_CAP = 4;

static inline ant_value_t sv_invoke_native(
  ant_t *js, ant_cfunc_t fn, ant_value_t *args,
  int nargs, ant_value_t new_target
) {
  if (!gc_value_is_heap_ref(new_target)) return fn(js, args, nargs, new_target);

  sv_vm_t *vm = js->vm;
  sv_native_frame_t frame = {
    .caller = vm->native_frame,
    .new_target = new_target
  };

  vm->native_frame = &frame;
  ant_value_t result = fn(js, args, nargs, new_target);
  vm->native_frame = frame.caller;

  return result;
}

static inline void sv_vm_maybe_checkpoint_microtasks(ant_t *js) {
  if (!js || js->microtasks_draining || js->vm_exec_depth != 0) return;
  js_maybe_drain_microtasks(js);
}

typedef struct {
  ant_value_t this_val;
  ant_value_t super_val;
  ant_value_t new_target;
  ant_value_t *args;
  int argc;
  ant_value_t *alloc;
} sv_call_ctx_t;

typedef enum {
  SV_CALL_MODE_NORMAL = 0,
  SV_CALL_MODE_EXPLICIT_THIS,
  SV_CALL_MODE_CONSTRUCT,
} sv_call_mode_t;

typedef enum {
  SV_CALL_EXEC_NATIVE = 0,
  SV_CALL_EXEC_UNCURRIED_NATIVE,
  SV_CALL_EXEC_PROXY_APPLY,
  SV_CALL_EXEC_PROXY_CONSTRUCT,
  SV_CALL_EXEC_DEFAULT_CTOR,
  SV_CALL_EXEC_CLOSURE,
} sv_call_exec_kind_t;

typedef struct {
  sv_call_exec_kind_t kind;
  ant_value_t func;
  sv_closure_t *closure;
  sv_call_ctx_t ctx;
  ant_value_t inline_args[SV_CALL_INLINE_ARGS_CAP];
} sv_call_plan_t;

ant_value_t sv_jit_try_compile_and_call(sv_vm_t *vm, ant_t *js,
  sv_closure_t *closure, ant_value_t callee_func,
  sv_call_ctx_t *ctx, ant_value_t *out_this
);

static inline ant_value_t *sv_prepend_bound_args(
  sv_closure_t *closure, ant_value_t *args, int argc, int *out_total,
  ant_value_t *inline_args
) {
  int total = closure->bound_argc + argc;
  ant_value_t *combined = total <= SV_CALL_INLINE_ARGS_CAP
    ? inline_args
    : malloc(sizeof(ant_value_t) * (size_t)total);

  if (!combined) { *out_total = argc; return NULL; }
  memcpy(combined, closure->u.bound.argv, sizeof(ant_value_t) * (size_t)closure->bound_argc);
  memcpy(combined + closure->bound_argc, args, sizeof(ant_value_t) * (size_t)argc);

  *out_total = total;
  return combined;
}

static inline bool sv_call_mode_is_construct(sv_call_mode_t mode) {
  return mode == SV_CALL_MODE_CONSTRUCT;
}

static inline ant_value_t sv_call_normalize_this(ant_t *js, ant_value_t this_val, sv_call_mode_t mode) {
  if (mode == SV_CALL_MODE_NORMAL && sv_is_nullish_this(this_val)) return js->global;
  return this_val;
}

static inline ant_value_t sv_construct_prototype_from(
  ant_t *js, ant_value_t proto_source
) {
  ant_value_t proto = js_mkundef();
  ant_value_t source_obj = js_mkundef();
  uint8_t source_type = vtype(proto_source);

  if (source_type == kTypeFunction) source_obj = js_func_obj(proto_source);
  else if (source_type == kTypeObject) source_obj = proto_source;

  ant_object_t *ptr = is_object_type(source_obj)
    ? js_obj_ptr(source_obj) : NULL;

  int32_t slot = ptr && !ptr->flags.is_exotic && ptr->shape
    ? ant_shape_lookup_interned(ptr->shape, js->intern.prototype) : -1;

  const ant_shape_prop_t *prop = slot >= 0
    ? ant_shape_prop_at(ptr->shape, (uint32_t)slot) : NULL;

  if (prop && !prop->has_getter && !prop->has_setter)
    proto = ant_object_prop_get_unchecked(ptr, (uint32_t)slot);
  else
    proto = js_getprop_fallback(js, proto_source, "prototype");

  return (is_err(proto) || is_object_type(proto))
    ? proto : js->sym.object_proto;
}

static inline ant_value_t sv_prepare_construct_meta(
  ant_t *js,
  ant_value_t func,
  ant_value_t requested_new_target,
  ant_value_t *effective_new_target,
  ant_value_t *record_func
) {
  sv_closure_t *closure = NULL;
  if (vtype(func) == kTypeFunction) {
    closure = js_func_closure(func);
    if (closure && !(closure->call_flags & (SV_CALL_HAS_BOUND_THIS | SV_CALL_HAS_BOUND_ARGS))) {
      if (effective_new_target) *effective_new_target = requested_new_target;
      if (record_func) *record_func = func;
      return sv_construct_prototype_from(js, requested_new_target);
    }
  }

  ant_value_t target = closure
    ? js_resolve_bound_target_known_bound(func) : func;
  ant_value_t new_target =
    requested_new_target == func ? target : requested_new_target;

  if (effective_new_target) *effective_new_target = new_target;
  if (record_func && vtype(target) == kTypeFunction) *record_func = target;

  if (
    requested_new_target == func &&
    vtype(target) == kTypeObject && is_proxy(target)
  ) return js_mkundef();

  ant_value_t proto_source =
    requested_new_target == func
    ? target : requested_new_target;

  uint8_t source_type = vtype(proto_source);

  if (
    source_type != kTypeFunction && source_type != kTypeBuiltin &&
    !is_object_type(proto_source)
  ) return js_mkundef();

  return sv_construct_prototype_from(js, proto_source);
}

static inline ant_value_t sv_call_resolve_bound(
  ant_t *js, sv_closure_t *closure,
  sv_call_ctx_t *ctx, sv_call_mode_t mode, ant_value_t *inline_args
) {
  uint32_t flags = closure->call_flags;

  if (flags & SV_CALL_IS_ARROW) ctx->this_val = closure->bound_this;
  else if (!sv_call_mode_is_construct(mode) && (flags & SV_CALL_HAS_BOUND_THIS))
    ctx->this_val = closure->bound_this;

  if ((flags & SV_CALL_HAS_BOUND_ARGS) && closure->bound_argc > 0) {
    int total;
    ant_value_t *combined = sv_prepend_bound_args(
      closure, ctx->args, ctx->argc,
      &total, inline_args
    );
    if (!combined) return js_mkerr(js, "out of memory");
    ctx->args  = combined;
    ctx->argc  = total;
    if (combined != inline_args) ctx->alloc = combined;
  }

  if (flags & SV_CALL_HAS_SUPER) ctx->super_val = closure->super_val;
  return js_mkundef();
}

static inline void sv_call_cleanup(ant_t *js, sv_call_ctx_t *ctx) {
  if (ctx->alloc) { free(ctx->alloc); ctx->alloc = NULL; }
}

static inline ant_value_t sv_call_default_ctor(
  sv_vm_t *vm, ant_t *js, sv_closure_t *closure,
  sv_call_ctx_t *ctx, ant_value_t *out_this
);

static inline ant_value_t sv_call_resolve_closure(
  sv_vm_t *vm, ant_t *js, sv_closure_t *closure,
  ant_value_t callee_func, sv_call_ctx_t *ctx, ant_value_t *out_this
);

static inline bool sv_closure_is_plain_sync(const sv_closure_t *closure) {
  return 
    closure && closure->call_flags == 0 && closure->func &&
    !closure->func->is_async && !closure->func->is_generator;
}

static inline ant_value_t sv_prepare_call(
  sv_vm_t *vm, ant_t *js, ant_value_t func,
  ant_value_t this_val, ant_value_t *args, int argc,
  ant_value_t *out_this, sv_call_mode_t mode,
  ant_value_t new_target, sv_call_plan_t *plan
) {
  bool is_construct_call = sv_call_mode_is_construct(mode);

  plan->kind = SV_CALL_EXEC_NATIVE;
  plan->func = func;
  plan->closure = NULL;

  plan->ctx = (sv_call_ctx_t){
    .this_val = this_val,
    .super_val = js_mkundef(),
    .new_target = new_target,
    .args = args,
    .argc = argc,
    .alloc = NULL,
  };

  if (out_this) *out_this = this_val;

  if (is_construct_call && vtype(func) == kTypeObject && is_proxy(func)) {
    plan->kind = SV_CALL_EXEC_PROXY_CONSTRUCT;
    return js_mkundef();
  }

  if (is_construct_call && !js_is_constructor(func))
    return js_mkerr_typed(js, JS_ERR_TYPE, "not a constructor");

  if (!is_construct_call && vtype(func) == kTypeObject && is_proxy(func)) {
    plan->kind = SV_CALL_EXEC_PROXY_APPLY;
    return js_mkundef();
  }

  if (vtype(func) == kTypeBuiltin) {
    plan->ctx.this_val = sv_call_normalize_this(js, this_val, mode);
    if (out_this) *out_this = plan->ctx.this_val;
    return js_mkundef();
  }

  if (vtype(func) != kTypeFunction)
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s is not a function", typestr(vtype(func)));

  sv_closure_t *closure = js_func_closure(func);
  plan->closure = closure;

  ant_value_t err = sv_call_resolve_bound(
    js, closure, &plan->ctx, mode, plan->inline_args
  );
  if (is_err(err)) return err;

  if (is_construct_call) plan->ctx.this_val = this_val;
  if (out_this) *out_this = plan->ctx.this_val;

  if (closure->call_flags & SV_CALL_IS_DEFAULT_CTOR) {
    plan->kind = SV_CALL_EXEC_DEFAULT_CTOR;
    return js_mkundef();
  }

  if (closure->func != NULL) {
    plan->kind = SV_CALL_EXEC_CLOSURE;
    return js_mkundef();
  }

  if (
    !is_construct_call && js->vm_exec_depth != 0 &&
    (closure->call_flags & SV_CALL_IS_UNCURRY) &&
    vtype(closure->bound_this) == kTypeBuiltin
  ) {
    plan->kind = SV_CALL_EXEC_UNCURRIED_NATIVE;
    plan->ctx.this_val = plan->ctx.argc ? plan->ctx.args[0] : js_mkundef();
    if (plan->ctx.argc) { plan->ctx.args++; plan->ctx.argc--; }
  }

  return js_mkundef();
}

static inline ant_value_t sv_execute_call_plan(
  sv_vm_t *vm, ant_t *js, sv_call_plan_t *plan, ant_value_t *out_this
) {
  switch (plan->kind) {
  case SV_CALL_EXEC_PROXY_APPLY: return js_proxy_apply(
    js, plan->func, plan->ctx.this_val,
    plan->ctx.args, plan->ctx.argc
  );

  case SV_CALL_EXEC_PROXY_CONSTRUCT: return js_proxy_construct(
    js, plan->func, plan->ctx.args,
    plan->ctx.argc, plan->ctx.new_target
  );

  case SV_CALL_EXEC_DEFAULT_CTOR: return sv_call_default_ctor(
    vm, js, plan->closure,
    &plan->ctx, out_this
  );

  case SV_CALL_EXEC_CLOSURE: return sv_call_resolve_closure(
    vm, js, plan->closure,
    plan->func, &plan->ctx, out_this
  );

  case SV_CALL_EXEC_NATIVE: {
    ant_value_t result = sv_call_native(
      js, plan->func, plan->ctx.this_val,
      plan->ctx.args, plan->ctx.argc, plan->ctx.new_target
    );
    sv_call_cleanup(js, &plan->ctx);
    return result;
  }

  case SV_CALL_EXEC_UNCURRIED_NATIVE: {
    ant_value_t saved_func = js->current_func;
    js->current_func = plan->func;
    ant_value_t result = sv_call_native(
      js, plan->closure->bound_this,
      plan->ctx.this_val, plan->ctx.args, plan->ctx.argc, js_mkundef()
    );
    js->current_func = saved_func;
    sv_call_cleanup(js, &plan->ctx);
    return result;
  }}

  return js_mkerr(js, "invalid call plan");
}

static inline bool sv_check_c_stack_overflow(ant_t *js) {
  volatile char marker;
  if (js->cstk.limit == 0 || js->cstk.base == NULL) return false;

  uintptr_t base = (uintptr_t)js->cstk.base;
  uintptr_t curr = (uintptr_t)&marker;

  size_t used = (base > curr) ? (base - curr) : (curr - base);
  return used > js->cstk.limit;
}

static inline ant_value_t sv_vm_call(
  sv_vm_t *vm, ant_t *js, ant_value_t func,
  ant_value_t this_val, ant_value_t *args, int argc,
  ant_value_t *out_this, ant_value_t new_target
) {
  if (sv_check_c_stack_overflow(js))
    return js_mkerr_typed(js, JS_ERR_RANGE | JS_ERR_NO_STACK, "Maximum call stack size exceeded");

  bool is_construct_call = new_target != js_mkundef();
  sv_call_mode_t mode = is_construct_call
    ? SV_CALL_MODE_CONSTRUCT
    : SV_CALL_MODE_NORMAL;

  if (!is_construct_call && vtype(func) == kTypeBuiltin) {
    ant_value_t native_this = sv_call_normalize_this(js, this_val, mode);

    if (out_this) *out_this = native_this;
    ant_value_t saved_this = js->this_val;

    js->this_val = native_this;
    ant_value_t native_res = js_as_cfunc(func)(js, args, argc, js_mkundef());
    js->this_val = saved_this;

    sv_vm_maybe_checkpoint_microtasks(js);
    return native_res;
  }

  if (vtype(func) == kTypeFunction) {
    sv_closure_t *closure = js_func_closure(func);
    if (closure->func == NULL && closure->call_flags == 0) {
      if (is_construct_call && !js_is_constructor(func)) return js_mkerr_typed(js, JS_ERR_TYPE, "not a constructor");
      if (out_this) *out_this = this_val;

      ant_value_t result = sv_call_native(js, func, this_val, args, argc, new_target);
      sv_vm_maybe_checkpoint_microtasks(js);

      return result;
    }
  }

  sv_call_plan_t plan;
  ant_value_t err = sv_prepare_call(
    vm, js, func, this_val, args, argc,
    out_this, mode, new_target, &plan
  );

  if (is_err(err)) return err;
  ant_value_t result = sv_execute_call_plan(vm, js, &plan, out_this);
  sv_vm_maybe_checkpoint_microtasks(js);

  return result;
}

static inline ant_value_t sv_vm_call_explicit_this(
  sv_vm_t *vm, ant_t *js, ant_value_t func,
  ant_value_t this_val, ant_value_t *args, int argc
) {
  if (sv_check_c_stack_overflow(js))
    return js_mkerr_typed(js, JS_ERR_RANGE | JS_ERR_NO_STACK, "Maximum call stack size exceeded");

  sv_call_plan_t plan;
  ant_value_t err = sv_prepare_call(
    vm, js, func, this_val, args, argc, NULL,
    SV_CALL_MODE_EXPLICIT_THIS, js_mkundef(), &plan
  );

  if (is_err(err)) return err;
  ant_value_t result = sv_execute_call_plan(vm, js, &plan, NULL);
  sv_vm_maybe_checkpoint_microtasks(js);

  return result;
}

static inline ant_value_t sv_call_default_ctor(
  sv_vm_t *vm, ant_t *js, sv_closure_t *closure,
  sv_call_ctx_t *ctx, ant_value_t *out_this
) {
  if (vtype(ctx->new_target) == kTypeUndefined) {
    sv_call_cleanup(js, ctx);
    return js_mkerr_typed(js, JS_ERR_TYPE, SV_CLASS_CTOR_CALL_ERROR);
  }

  ant_value_t super_ctor = closure->super_val;
  uint8_t st = vtype(super_ctor);

  if (st == kTypeFunction || st == kTypeBuiltin) {
    ant_value_t super_this = ctx->this_val;
    ant_value_t result = sv_vm_call(
      vm, js, super_ctor, ctx->this_val,
      ctx->args, ctx->argc, &super_this, ctx->new_target
    );

    if (out_this) *out_this = super_this;
    sv_call_cleanup(js, ctx);

    return result;
  }

  sv_call_cleanup(js, ctx);
  return js_mkundef();
}

ant_value_t sv_call_generator_closure_dispatch(
  sv_vm_t *vm, ant_t *js, sv_closure_t *closure,
  ant_value_t callee_func, ant_value_t super_val,
  ant_value_t new_target, ant_value_t this_val,
  ant_value_t *args, int argc
);

static inline ant_value_t sv_call_async_closure(
  sv_vm_t *vm, ant_t *js, sv_closure_t *closure,
  ant_value_t callee_func, sv_call_ctx_t *ctx
) {
  ant_value_t result = sv_start_async_closure(
    vm, js, closure, callee_func,
    ctx->super_val, ctx->new_target,
    ctx->this_val, ctx->args, ctx->argc
  );
  sv_call_cleanup(js, ctx);
  return result;
}

static inline ant_value_t sv_call_generator_closure(
  sv_vm_t *vm, ant_t *js, sv_closure_t *closure,
  ant_value_t callee_func, sv_call_ctx_t *ctx
) {
  ant_value_t result = sv_call_generator_closure_dispatch(
    vm, js, closure, callee_func,
    ctx->super_val, ctx->new_target,
    ctx->this_val, ctx->args, ctx->argc
  );
  sv_call_cleanup(js, ctx);
  return result;
}

static inline ant_value_t sv_call_closure(
  sv_vm_t *vm, ant_t *js, sv_closure_t *closure,
  ant_value_t callee_func, sv_call_ctx_t *ctx, ant_value_t *out_this
) {
  ant_value_t result = sv_execute_closure_entry(
    vm, closure, callee_func, ctx->super_val, ctx->new_target,
    ctx->this_val, ctx->args, ctx->argc, out_this
  );
  sv_call_cleanup(js, ctx);
  return result;
}

static inline ant_value_t sv_call_resolve_closure(
  sv_vm_t *vm, ant_t *js, sv_closure_t *closure,
  ant_value_t callee_func, sv_call_ctx_t *ctx, ant_value_t *out_this
) {
  if (closure->func->is_generator)
    return sv_call_generator_closure(vm, js, closure, callee_func, ctx);
  if (closure->func->is_async)
    return sv_call_async_closure(vm, js, closure, callee_func, ctx);
  if (!closure->func->is_generator) {
    sv_func_t *fn = closure->func;
    if (fn->jit_code) {
      sv_jit_maybe_tier_up(js, fn, closure);
      sv_jit_enter(js);
      ant_value_t result = ((sv_jit_func_t)fn->jit_code)(
        vm, ctx->this_val, ctx->new_target,
        ctx->super_val, ctx->args, ctx->argc, closure
      );
      sv_jit_leave(js);
      if (sv_is_jit_bailout(result)) {
        sv_jit_on_bailout(fn);
      } else { sv_call_cleanup(js, ctx); return result; }
    }
    {
      uint32_t cc = ++fn->call_count;
      if (__builtin_expect(cc == SV_TFB_ALLOC_THRESHOLD, 0))
        sv_tfb_ensure(fn);
      if (!fn->jit_compile_failed && cc > SV_JIT_THRESHOLD) {
        ant_value_t result = sv_jit_try_compile_and_call(vm, js, closure, callee_func, ctx, out_this);
        if (result != SV_JIT_RETRY_INTERP) return result;
      }
    }
  }
  return sv_call_closure(vm, js, closure, callee_func, ctx, out_this);
}


#endif
