#ifndef SV_ASYNC_H
#define SV_ASYNC_H

#include "ant.h"
#include "sugar.h"
#include "silver/engine.h"

#include <minicoro.h>

typedef struct {
  ant_t *js;
  coroutine_t *coro;
} sv_coro_header_t;

typedef struct {
  ant_t *js;
  
  coroutine_t *coro;
  sv_closure_t *closure;
  
  ant_value_t callee_func;
  ant_value_t super_val;
  ant_value_t this_val;
  ant_value_t *args;
  
  int argc;
  sv_vm_t *vm;
} sv_async_ctx_t;

static void sv_mco_async_entry(mco_coro *mco) {
  sv_async_ctx_t *ctx = (sv_async_ctx_t *)mco_get_user_data(mco);
  ant_t *js = ctx->js;
  MCO_CORO_STACK_ENTER(js, mco);

  ant_value_t super_val = ctx->super_val;
  if (ctx->coro) {
    super_val = ctx->coro->super_val;
    js->new_target = ctx->coro->new_target;
  }

  sv_vm_t *vm = ctx->vm;
  ant_value_t result = sv_execute_closure_entry(
    vm, ctx->closure, ctx->callee_func, 
    super_val, ctx->this_val, ctx->args, ctx->argc, NULL
  );

  ant_value_t promise = ctx->coro->async_promise;

  if (is_err(result)) {
    ant_value_t reject_value = js->thrown_value;
    if (vtype(reject_value) == T_UNDEF) reject_value = result;
    js->thrown_exists = false;
    js->thrown_value = js_mkundef();
    js_reject_promise(js, promise, reject_value);
  } else js_resolve_promise(js, promise, result);
}

typedef struct {
  ant_t *js;
  coroutine_t *coro;
  sv_func_t   *func;
  ant_value_t this_val;
  sv_vm_t *vm;
} sv_tla_ctx_t;

static void sv_mco_tla_entry(mco_coro *mco) {
  sv_tla_ctx_t *ctx = (sv_tla_ctx_t *)mco_get_user_data(mco);
  ant_t *js = ctx->js;
  sv_vm_t *vm = ctx->vm;
  MCO_CORO_STACK_ENTER(js, mco);

  ant_value_t result = sv_execute_entry(vm, ctx->func, ctx->this_val, NULL, 0);
  ant_value_t promise = ctx->coro->async_promise;

  if (is_err(result)) {
    ant_value_t reject_value = js->thrown_value;
    if (vtype(reject_value) == T_UNDEF) reject_value = result;
    js->thrown_exists = false;
    js->thrown_value = js_mkundef();
    js_reject_promise(js, promise, reject_value);
  } else js_resolve_promise(js, promise, result);
}

static inline ant_value_t sv_start_tla(ant_t *js, sv_func_t *func, ant_value_t this_val) {
  if (++coros_this_tick > CORO_PER_TICK_LIMIT) {
    js->fatal_error = true;
    return js_mkerr_typed(js, JS_ERR_RANGE | JS_ERR_NO_STACK,
      "Maximum async operations per tick exceeded");
  }

  ant_value_t promise = js_mkpromise(js);
  sv_tla_ctx_t *ctx = (sv_tla_ctx_t *)CORO_MALLOC(sizeof(sv_tla_ctx_t));
  if (!ctx) return js_mkerr(js, "out of memory for TLA context");

  sv_vm_t *async_vm = sv_vm_create(js, SV_VM_ASYNC);
  if (!async_vm) { CORO_FREE(ctx); return js_mkerr(js, "out of memory for TLA VM"); }

  ctx->js = js;
  ctx->func = func;
  ctx->this_val = this_val;
  ctx->coro = NULL;
  ctx->vm = async_vm;

  size_t stack_size = 0;
  const char *env_stack = getenv("ANT_CORO_STACK_SIZE");
  if (env_stack) {
    size_t sz = (size_t)atoi(env_stack) * 1024;
    if (sz >= 32 * 1024 && sz <= 8 * 1024 * 1024) stack_size = sz;
  }

  mco_desc desc = mco_desc_init(sv_mco_tla_entry, stack_size);
  desc.user_data = ctx;

  mco_coro *mco = NULL;
  mco_result res = mco_create(&mco, &desc);
  if (res != MCO_SUCCESS) {
    sv_vm_destroy(async_vm); CORO_FREE(ctx);
    return js_mkerr(js, "failed to create TLA coroutine");
  }

  coroutine_t *coro = (coroutine_t *)CORO_MALLOC(sizeof(coroutine_t));
  if (!coro) {
    mco_destroy(mco);
    sv_vm_destroy(async_vm); CORO_FREE(ctx);
    return js_mkerr(js, "out of memory for TLA coroutine");
  }

  *coro = (coroutine_t){
    .js = js,
    .type = CORO_ASYNC_AWAIT,
    .this_val = this_val,
    .super_val = js_mkundef(),
    .new_target = js_mkundef(),
    .awaited_promise = js_mkundef(),
    .result = js_mkundef(),
    .async_func = js_mkundef(),
    .args = NULL,
    .nargs = 0,
    .is_settled = false,
    .is_error = false,
    .is_done = false,
    .resume_point = 0,
    .yield_value = js_mkundef(),
    .async_promise = promise,
    .next = NULL,
    .mco = mco,
    .mco_started = false,
    .is_ready = true,
    .sv_vm = async_vm,
  };

  ctx->coro = coro;
  enqueue_coroutine(coro);
  MCO_RESUME_SAVE(js, mco, res);

  if (res != MCO_SUCCESS && mco_status(mco) != MCO_DEAD) {
    remove_coroutine(coro);
    free_coroutine(coro);
    return js_mkerr(js, "failed to start TLA coroutine");
  }

  coro->mco_started = true;
  if (mco_status(mco) == MCO_DEAD) {
    remove_coroutine(coro);
    free_coroutine(coro);
    js->needs_gc = true;
  }

  return promise;
}

static inline ant_value_t sv_start_async_closure(
  sv_vm_t *caller_vm, ant_t *js,
  sv_closure_t *closure, ant_value_t callee_func, ant_value_t super_val,
  ant_value_t this_val, ant_value_t *args, int argc
) {
  if (++coros_this_tick > CORO_PER_TICK_LIMIT) {
    js->fatal_error = true;
    return js_mkerr_typed(js, JS_ERR_RANGE | JS_ERR_NO_STACK,
      "Maximum async operations per tick exceeded");
  }

  ant_value_t promise = js_mkpromise(js);
  sv_async_ctx_t *ctx = (sv_async_ctx_t *)CORO_MALLOC(sizeof(sv_async_ctx_t));
  if (!ctx) return js_mkerr(js, "out of memory for async context");

  sv_vm_t *async_vm = sv_vm_create(js, SV_VM_ASYNC);
  if (!async_vm) { CORO_FREE(ctx); return js_mkerr(js, "out of memory for async VM"); }

  ctx->js = js;
  ctx->closure = closure;
  ctx->callee_func = callee_func;
  ctx->super_val = super_val;
  ctx->this_val = this_val;
  ctx->args = NULL;
  ctx->argc = argc;
  ctx->coro = NULL;
  ctx->vm = async_vm;

  if (argc > 0 && args) {
    ctx->args = (ant_value_t *)CORO_MALLOC(sizeof(ant_value_t) * (size_t)argc);
    if (!ctx->args) {
      sv_vm_destroy(async_vm); CORO_FREE(ctx);
      return js_mkerr(js, "out of memory for async args");
    }
    memcpy(ctx->args, args, sizeof(ant_value_t) * (size_t)argc);
  }

  size_t stack_size = 0;
  const char *env_stack = getenv("ANT_CORO_STACK_SIZE");
  if (env_stack) {
    size_t sz = (size_t)atoi(env_stack) * 1024;
    if (sz >= 32 * 1024 && sz <= 8 * 1024 * 1024) stack_size = sz;
  }

  mco_desc desc = mco_desc_init(sv_mco_async_entry, stack_size);
  desc.user_data = ctx;

  mco_coro *mco = NULL;
  mco_result res = mco_create(&mco, &desc);
  if (res != MCO_SUCCESS) {
    if (ctx->args) CORO_FREE(ctx->args);
    sv_vm_destroy(async_vm); CORO_FREE(ctx);
    return js_mkerr(js, "failed to create async coroutine");
  }

  coroutine_t *coro = (coroutine_t *)CORO_MALLOC(sizeof(coroutine_t));
  if (!coro) {
    mco_destroy(mco);
    if (ctx->args) CORO_FREE(ctx->args);
    sv_vm_destroy(async_vm); CORO_FREE(ctx);
    return js_mkerr(js, "out of memory for coroutine");
  }

  *coro = (coroutine_t){
    .js = js,
    .type = CORO_ASYNC_AWAIT,
    .this_val = this_val,
    .super_val = super_val,
    .new_target = js->new_target,
    .awaited_promise = js_mkundef(),
    .result = js_mkundef(),
    .async_func = callee_func,
    .args = ctx->args,
    .nargs = argc,
    .is_settled = false,
    .is_error = false,
    .is_done = false,
    .resume_point = 0,
    .yield_value = js_mkundef(),
    .async_promise = promise,
    .next = NULL,
    .mco = mco,
    .mco_started = false,
    .is_ready = true,
    .sv_vm = async_vm,
  };

  ctx->coro = coro;
  enqueue_coroutine(coro);
  MCO_RESUME_SAVE(js, mco, res);

  if (res != MCO_SUCCESS && mco_status(mco) != MCO_DEAD) {
    remove_coroutine(coro);
    free_coroutine(coro);
    return js_mkerr(js, "failed to start async coroutine");
  }

  coro->mco_started = true;
  if (mco_status(mco) == MCO_DEAD) {
    remove_coroutine(coro);
    free_coroutine(coro);
    js->needs_gc = true;
  }

  return promise;
}

static inline ant_value_t sv_await_value(ant_t *js, ant_value_t value) {
  if (vtype(value) != T_PROMISE) return value;

  mco_coro *current_mco = mco_running();
  if (!current_mco)
    return js_mkerr(js, "await can only be used inside async functions");

  coroutine_t *coro = NULL;
  sv_coro_header_t *hdr = (sv_coro_header_t *)mco_get_user_data(current_mco);
  if (hdr) coro = hdr->coro;

  if (!coro)
    return js_mkerr(js, "invalid async context");

  coro->awaited_promise = value;
  coro->is_settled = false;
  coro->is_ready = false;

  ant_value_t resume_obj = mkobj(js, 0);
  js_set_slot(js, resume_obj, SLOT_CORO, tov((double)(uintptr_t)coro));
  js_set_slot(js, resume_obj, SLOT_CFUNC, js_mkfun(resume_coroutine_wrapper));

  ant_value_t reject_obj = mkobj(js, 0);
  js_set_slot(js, reject_obj, SLOT_CORO, tov((double)(uintptr_t)coro));
  js_set_slot(js, reject_obj, SLOT_CFUNC, js_mkfun(reject_coroutine_wrapper));

  ant_value_t then_fn = js_getprop_fallback(js, value, "then");
  if (vtype(then_fn) == T_FUNC || vtype(then_fn) == T_CFUNC) {
    ant_value_t then_args[] = {
      js_obj_to_func(resume_obj),
      js_obj_to_func(reject_obj)
    };
    sv_vm_call(
      js->vm, js, then_fn, value, 
      then_args, 2, NULL, false
    );
  }

  mco_result mco_res = mco_yield(current_mco);
  if (mco_res != MCO_SUCCESS)
    return js_mkerr(js, "failed to yield coroutine");

  MCO_CORO_STACK_ENTER(js, current_mco);
  ant_value_t result = coro->result;
  bool is_error = coro->is_error;
  
  coro->is_settled = false;
  coro->awaited_promise = js_mkundef();
  if (is_error) return js_throw(js, result);

  return result;
}

static inline void sv_vm_gc_roots_async(void (*op_val)(void *, ant_value_t *), void *ctx) {
  for (coroutine_t *c = pending_coroutines.head; c; c = c->next) 
    if (c->sv_vm) sv_vm_gc_roots(c->sv_vm, op_val, ctx);
}

#endif
