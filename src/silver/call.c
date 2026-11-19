#include "silver/call.h"
#include "proto.h"
#include "silver/jit.h"

ant_value_t sv_super_constructor(ant_value_t constructor) {
  ant_value_t parent;
  if (sv_super_constructor_fast(constructor, &parent)) return parent;
  if (vtype(constructor) != kTypeFunction) return Ant_Value_Undefined;
  return ant_object_proto(js_obj_ptr(js_func_obj(constructor)));
}

ant_value_t sv_super_construct_slow(
  sv_vm_t *vm, ant_t *js, ant_value_t constructor, ant_value_t this_val,
  ant_value_t *args, int argc, ant_value_t new_target
) {
  return sv_super_construct_parent(vm, js, sv_super_constructor(constructor), this_val, args, argc, new_target);
}

static ant_value_t sv_call_default_ctor(
  sv_vm_t *vm, ant_t *js, sv_closure_t *closure,
  sv_call_ctx_t *ctx, ant_value_t *out_this
) {
  if (vtype(ctx->new_target) == kTypeUndefined) {
    sv_call_cleanup(js, ctx);
    return js_mkerr_typed(js, JS_ERR_TYPE, SV_CLASS_CTOR_CALL_ERROR);
  }

  if (!(closure->call_flags & SV_CALL_HAS_SUPER)) {
    sv_call_cleanup(js, ctx);
    return Ant_Value_Undefined;
  }

  ant_value_t super_this = ctx->this_val;
  ant_value_t parent = sv_super_constructor(closure->super_val);
  ant_value_t result = sv_vm_call(
    vm, js, parent, ctx->this_val,
    ctx->args, ctx->argc, &super_this, ctx->new_target
  );
  if (__builtin_expect(is_err(result), 0)) result = sv_super_construct_error(js, parent, ctx->new_target, result);

  if (out_this) *out_this = super_this;
  sv_call_cleanup(js, ctx);

  return result;
}

static inline __attribute__((always_inline)) ant_value_t sv_prepare_call(
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
    .super_val = Ant_Value_Undefined,
    .new_target = new_target,
    .args = args,
    .argc = argc,
    .alloc = NULL,
  };

  if (out_this) *out_this = this_val;

  if (is_construct_call && vtype(func) == kTypeObject && is_proxy(func)) {
    plan->kind = SV_CALL_EXEC_PROXY_CONSTRUCT;
    return Ant_Value_Undefined;
  }

  if (is_construct_call && !js_is_constructor(func))
    return js_mkerr_typed(js, JS_ERR_TYPE, "not a constructor");

  if (!is_construct_call && vtype(func) == kTypeObject && is_proxy(func)) {
    plan->kind = SV_CALL_EXEC_PROXY_APPLY;
    return Ant_Value_Undefined;
  }

  if (vtype(func) == kTypeBuiltin) {
    plan->ctx.this_val = sv_call_normalize_this(js, this_val, mode);
    if (out_this) *out_this = plan->ctx.this_val;
    return Ant_Value_Undefined;
  }

  if (vtype(func) != kTypeFunction)
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s is not a function", typestr(vtype(func)));

  sv_closure_t *closure = js_func_closure(func);
  plan->closure = closure;

  ant_value_t err = sv_call_resolve_bound(
    js, closure, &plan->ctx, 
    mode, plan->inline_args
  );
  
  if (is_err(err)) return err;
  if (is_construct_call) plan->ctx.this_val = this_val;
  if (out_this) *out_this = plan->ctx.this_val;

  if (closure->call_flags & SV_CALL_IS_DEFAULT_CTOR) {
    plan->kind = SV_CALL_EXEC_DEFAULT_CTOR;
    return Ant_Value_Undefined;
  }

  if (closure->func != NULL) {
    plan->kind = SV_CALL_EXEC_CLOSURE;
    return Ant_Value_Undefined;
  }

  if (
    !is_construct_call && js->vm_exec_depth != 0 &&
    (closure->call_flags & SV_CALL_IS_UNCURRY) &&
    vtype(closure->bound_this) == kTypeBuiltin
  ) {
    plan->kind = SV_CALL_EXEC_UNCURRIED_NATIVE;
    plan->ctx.this_val = plan->ctx.argc ? plan->ctx.args[0] : Ant_Value_Undefined;
    if (plan->ctx.argc) { plan->ctx.args++; plan->ctx.argc--; }
  }

  return Ant_Value_Undefined;
}

static inline __attribute__((always_inline)) ant_value_t sv_execute_call_plan(
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
    ant_value_t result = sv_invoke_builtin(
      js, plan->closure->bound_this, plan->ctx.this_val,
      plan->func, plan->ctx.args, plan->ctx.argc, Ant_Value_Undefined
    );
    sv_call_cleanup(js, &plan->ctx);
    return result;
  }}

  return js_mkerr(js, "invalid call plan");
}

ant_value_t sv_vm_call_mode(
  sv_vm_t *vm, ant_t *js, ant_value_t func,
  ant_value_t this_val, ant_value_t *args, int argc,
  ant_value_t *out_this, sv_call_mode_t mode, ant_value_t new_target
) {
  sv_call_plan_t plan;
  ant_value_t err = sv_prepare_call(
    vm, js, func, this_val, args, argc,
    out_this, mode, new_target, &plan
  );

  if (is_err(err)) return err;
  return sv_execute_call_plan(vm, js, &plan, out_this);
}

ant_value_t sv_vm_call(
  sv_vm_t *vm, ant_t *js, ant_value_t func,
  ant_value_t this_val, ant_value_t *args, int argc,
  ant_value_t *out_this, ant_value_t new_target
) {
  if (__builtin_expect(js->destroying, 0)) return Ant_Value_Undefined;
  if (js_interrupt_pending(js)) return js_interrupt_raise(js);
  
  if (sv_check_c_stack_overflow(js))
    return Ant_Error_CreateStackOverflow(js);

  bool is_construct_call = new_target != Ant_Value_Undefined;
  sv_call_mode_t mode = is_construct_call
    ? SV_CALL_MODE_CONSTRUCT
    : SV_CALL_MODE_NORMAL;

  if (!is_construct_call && vtype(func) == kTypeBuiltin) {
    ant_value_t native_this = sv_call_normalize_this(js, this_val, mode);

    if (out_this) *out_this = native_this;
    ant_value_t native_res = sv_invoke_builtin(js, func, native_this, func, args, argc, Ant_Value_Undefined);
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

__attribute__((noinline, cold)) ant_value_t sv_super_construct_error(
  ant_t *js, ant_value_t parent, ant_value_t new_target, ant_value_t error
) {
  if ((vtype(parent) == kTypeObject && is_proxy(parent)) || js_is_constructor(parent)) return error;

  char parent_text[64];
  size_t parent_len = same_object_identity(parent, js->sym.function_proto)
    ? (size_t)snprintf(parent_text, sizeof(parent_text), "null")
    : tostr(js, parent, parent_text, sizeof(parent_text));
  if (parent_len >= sizeof(parent_text)) parent_len = sizeof(parent_text) - 1;

  ant_value_t name = is_object_type(new_target) || vtype(new_target) == kTypeBuiltin
    ? js_getprop_fallback(js, new_target, "name") : Ant_Value_Undefined;
  ant_offset_t name_len = 0;
  const char *name_text = vtype(name) == kTypeString ? (const char *)(uintptr_t)vstr(js, name, &name_len) : NULL;

  Ant_Exception_Clear(js);
  if (!name_text || name_len == 0) return js_mkerr_typed(
    js, JS_ERR_TYPE, "Super constructor %.*s of anonymous class is not a constructor",
    (int)parent_len, parent_text
  );
  return js_mkerr_typed(
    js, JS_ERR_TYPE, "Super constructor %.*s of %.*s is not a constructor",
    (int)parent_len, parent_text, (int)name_len, name_text
  );
}

static bool sv_new_target_is_class(ant_value_t new_target) {
  if (vtype(new_target) != kTypeFunction) return false;
  const sv_closure_t *closure = js_func_closure(new_target);
  if (closure->call_flags & (SV_CALL_HAS_BOUND_THIS | SV_CALL_HAS_BOUND_ARGS)) return false;
  return (closure->call_flags & SV_CALL_IS_DEFAULT_CTOR) || (closure->func && closure->func->is_class_ctor);
}

static __attribute__((noinline)) ant_value_t sv_construct_default_prototype(
  ant_t *js, ant_value_t func, ant_value_t new_target, ant_value_t proto
) {
  bool native = vtype(func) == kTypeBuiltin || (
    vtype(func) == kTypeFunction && !js_func_closure(func)->func &&
    !(js_func_closure(func)->call_flags & SV_CALL_IS_DEFAULT_CTOR));
  if (is_object_type(proto)) {
    uint8_t type = vtype(new_target);
    ant_value_t raw = type == kTypeFunction || type == kTypeBuiltin
      ? lkp_interned_val(js, js_as_obj(new_target), js->intern.prototype) : proto;
    if (is_object_type(raw) || vtype(raw) == kTypeBuiltin) return proto;
  }
  ant_value_t own = native ? js_getprop_fallback(js, func, "prototype") : Ant_Value_Undefined;
  if (vtype(own) == kTypeBuiltin) own = js_cfunc_promote(js, own);
  return is_object_type(own) ? own : js->sym.object_proto;
}

static ant_value_t sv_construct_ordinary(
  sv_vm_t *vm, ant_t *js, ant_value_t func,
  ant_value_t *args, int argc, ant_value_t new_target
) {
  ant_value_t effective_new_target = new_target;
  bool proxied = vtype(new_target) == kTypeObject && is_proxy(new_target);
  ant_value_t proto = proxied
    ? js_getprop_fallback(js, new_target, "prototype")
    : sv_prepare_construct_meta(js, func, new_target, &effective_new_target);
  if (is_err(proto)) return proto;
  if (proxied && vtype(proto) == kTypeBuiltin) proto = js_cfunc_promote(js, proto);
  if (proxied && !is_object_type(proto)) proto = Ant_Value_Undefined;

  ant_value_t seed_proto = proto;
  if (new_target != func && (proxied ? !is_object_type(proto) : proto == js->sym.object_proto))
    seed_proto = sv_construct_default_prototype(js, func, new_target, proto);

  ant_value_t seed = Ant_Object_CreateBare(js);
  if (is_err(seed)) return seed;
  if (is_object_type(seed_proto)) js_set_proto_init(seed, seed_proto);

  ant_value_t ctor_this = seed;
  ant_value_t result;

  if (__builtin_expect(vtype(effective_new_target) == kTypeObject && is_proxy(effective_new_target), 0)) {
    ant_value_t outer_target = js->construct_proxy_target;
    ant_value_t outer_proto = js->construct_proxy_proto;
    js->construct_proxy_target = effective_new_target;
    js->construct_proxy_proto = proto;
    result = sv_vm_call(vm, js, func, seed, args, argc, &ctor_this, effective_new_target);
    js->construct_proxy_target = outer_target;
    js->construct_proxy_proto = outer_proto;
  } else result = sv_vm_call(vm, js, func, seed, args, argc, &ctor_this, effective_new_target);

  if (is_err(result)) return result;
  return sv_construct_result(result, ctor_this, seed);
}

ant_value_t sv_construct(
  sv_vm_t *vm, ant_t *js, ant_value_t func,
  ant_value_t *args, int argc, ant_value_t new_target
) {
  if (vtype(func) == kTypeObject && is_proxy(func))
    return js_proxy_construct(js, func, args, argc, new_target);

  if (vtype(func) != kTypeFunction || new_target == func || sv_new_target_is_class(new_target))
    return sv_construct_ordinary(vm, js, func, args, argc, new_target);

  sv_closure_t *closure = js_func_closure(func);
  uint32_t flags = closure->call_flags;

  sv_func_t *fn = closure->func;
  bool forwards = (flags & SV_CALL_IS_DEFAULT_CTOR) && (flags & SV_CALL_HAS_SUPER);
  bool derived = fn && fn->is_derived_ctor && !fn->is_generator && !fn->is_async;

  if ((flags & (SV_CALL_IS_ARROW | SV_CALL_IS_UNCURRY)) || (!forwards && !derived))
    return sv_construct_ordinary(vm, js, func, args, argc, new_target);

  if (js_interrupt_pending(js)) return js_interrupt_raise(js);
  if (sv_check_c_stack_overflow(js)) return Ant_Error_CreateStackOverflow(js);

  sv_call_plan_t plan;
  ant_value_t ctor_this = Ant_Value_Undefined;
  ant_value_t err = sv_prepare_call(
    vm, js, func, Ant_Value_Undefined, args, argc,
    &ctor_this, SV_CALL_MODE_CONSTRUCT, new_target, &plan
  );

  if (is_err(err)) return err;
  if (plan.kind == SV_CALL_EXEC_DEFAULT_CTOR) {
    ant_value_t parent = sv_super_constructor(plan.closure->super_val);
    ant_value_t result = sv_construct(vm, js, parent, plan.ctx.args, plan.ctx.argc, new_target);
    sv_call_cleanup(js, &plan.ctx);
    return result;
  }
  if (plan.kind != SV_CALL_EXEC_CLOSURE) {
    sv_call_cleanup(js, &plan.ctx);
    return sv_construct_ordinary(vm, js, func, args, argc, new_target);
  }

  sv_func_note_deferred_receiver(plan.closure->func);
  ant_value_t result = sv_call_resolve_closure(vm, js, plan.closure, plan.func, &plan.ctx, &ctor_this);
  sv_vm_maybe_checkpoint_microtasks(js);

  if (is_err(result)) return result;
  return sv_construct_result(result, ctor_this, Ant_Value_Undefined);
}

ant_value_t sv_vm_call_explicit_this(
  sv_vm_t *vm, ant_t *js, ant_value_t func,
  ant_value_t this_val, ant_value_t *args, int argc
) {
  if (sv_check_c_stack_overflow(js))
    return Ant_Error_CreateStackOverflow(js);

  sv_call_plan_t plan;
  ant_value_t err = sv_prepare_call(
    vm, js, func, this_val, args, argc, NULL,
    SV_CALL_MODE_EXPLICIT_THIS, Ant_Value_Undefined, &plan
  );

  if (is_err(err)) return err;
  ant_value_t result = sv_execute_call_plan(vm, js, &plan, NULL);
  sv_vm_maybe_checkpoint_microtasks(js);

  return result;
}
