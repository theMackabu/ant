#include "silver/call.h"

static ant_value_t sv_call_default_ctor(
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
    js, closure, &plan->ctx, 
    mode, plan->inline_args
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
    ant_value_t native_res = sv_invoke_native(js, js_as_cfunc(func), args, argc, js_mkundef());
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

ant_value_t sv_vm_call_explicit_this(
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
