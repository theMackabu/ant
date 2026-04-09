#ifndef SV_ASYNC_H
#define SV_ASYNC_H

#include "ant.h"
#include "gc/roots.h"
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

static inline bool sv_async_func_supports_lazy_start(sv_func_t *func) {
  if (!func || !func->has_await || func->is_generator) return false;

  for (int off = 0; off < func->code_len;) {
    sv_op_t op = (sv_op_t)func->code[off];
    if (
      op == OP_AWAIT_ITER_NEXT ||
      op == OP_YIELD ||
      op == OP_YIELD_STAR ||
      op == OP_INITIAL_YIELD
    ) return false;
    
    int size = sv_op_size[op];
    if (size <= 0) return false;
    
    off += size;
  }

  return true;
}
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
    js_maybe_drain_microtasks_after_async_settle(js);
  } else {
    js_resolve_promise(js, promise, result);
    js_maybe_drain_microtasks_after_async_settle(js);
  }
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
    js_maybe_drain_microtasks_after_async_settle(js);
  } else {
    js_resolve_promise(js, promise, result);
    js_maybe_drain_microtasks_after_async_settle(js);
  }
}

static inline ant_value_t sv_start_tla(ant_t *js, sv_func_t *func, ant_value_t this_val) {
  if (++coros_this_tick > CORO_PER_TICK_LIMIT) {
    js->fatal_error = true;
    return js_mkerr_typed(js, JS_ERR_RANGE | JS_ERR_NO_STACK,
      "Maximum async operations per tick exceeded");
  }

  ant_value_t promise = js_mkpromise(js);

  if (func && (!func->has_await || sv_async_func_supports_lazy_start(func))) {
    GC_ROOT_SAVE(root_mark, js);
    GC_ROOT_PIN(js, this_val);
    
    sv_vm_t *async_vm = sv_vm_create(js, SV_VM_ASYNC);
    if (!async_vm) {
      GC_ROOT_RESTORE(js, root_mark);
      return js_mkerr(js, "out of memory for TLA VM");
    }
    
    coroutine_t *coro = (coroutine_t *)CORO_MALLOC(sizeof(coroutine_t));
    if (!coro) {
      sv_vm_destroy(async_vm);
      GC_ROOT_RESTORE(js, root_mark);
      return js_mkerr(js, "out of memory for TLA coroutine");
    }
    
    GC_ROOT_PIN(js, promise);
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
      .active_parent = NULL,
      .is_settled = false,
      .is_error = false,
      .is_done = false,
      .resume_point = 0,
      .yield_value = js_mkundef(),
      .async_promise = promise,
      .next = NULL,
      .mco = NULL,
      .mco_started = false,
      .is_ready = false,
      .did_suspend = false,
      .sv_vm = async_vm,
    };
    
    coro->active_parent = js->active_async_coro;
    js->active_async_coro = coro;
    
    ant_value_t result = sv_execute_entry(
      async_vm, func, 
      this_val, NULL, 0
    );
    
    if (async_vm->suspended) {
      js->active_async_coro = coro->active_parent;
      coro->active_parent = NULL;
      enqueue_coroutine(coro);
      GC_ROOT_RESTORE(js, root_mark);
      return promise;
    }
    
    if (is_err(result)) {
      ant_value_t reject_value = js->thrown_value;
      if (vtype(reject_value) == T_UNDEF) reject_value = result;
      js->thrown_exists = false;
      js->thrown_value = js_mkundef();
      js_reject_promise(js, promise, reject_value);
    } else js_resolve_promise(js, promise, result);
    
    js->active_async_coro = coro->active_parent;
    coro->active_parent = NULL;
    
    free_coroutine(coro);
    GC_ROOT_RESTORE(js, root_mark);
    
    return promise;
  }

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
    .active_parent = NULL,
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
    .did_suspend = false,
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

  if (caller_vm && closure && closure->func && !closure->func->has_await) {
    GC_ROOT_SAVE(root_mark, js);
    GC_ROOT_PIN(js, callee_func);
    GC_ROOT_PIN(js, super_val);
    GC_ROOT_PIN(js, this_val);
    
    if (args) {
      for (int i = 0; i < argc; i++) GC_ROOT_PIN(js, args[i]);
    }
    
    ant_value_t promise = js_mkpromise(js);
    GC_ROOT_PIN(js, promise);
    
    ant_value_t result = sv_execute_closure_entry(
      caller_vm, closure, callee_func, super_val, this_val, args, argc, NULL
    );
    
    if (is_err(result)) {
      ant_value_t reject_value = js->thrown_value;
      if (vtype(reject_value) == T_UNDEF) reject_value = result;
      js->thrown_exists = false;
      js->thrown_value = js_mkundef();
      js_reject_promise(js, promise, reject_value);
    } else js_resolve_promise(js, promise, result);
    
    GC_ROOT_RESTORE(js, root_mark);
    return promise;
  }

  if (closure && closure->func && sv_async_func_supports_lazy_start(closure->func)) {
    GC_ROOT_SAVE(root_mark, js);
    GC_ROOT_PIN(js, callee_func);
    GC_ROOT_PIN(js, super_val);
    GC_ROOT_PIN(js, this_val);
    
    if (args) {
      for (int i = 0; i < argc; i++) GC_ROOT_PIN(js, args[i]);
    }
    
    ant_value_t promise = js_mkpromise(js);
    sv_vm_t *async_vm = sv_vm_create(js, SV_VM_ASYNC);
    if (!async_vm) {
      GC_ROOT_RESTORE(js, root_mark);
      return js_mkerr(js, "out of memory for async VM");
    }

    coroutine_t *coro = (coroutine_t *)CORO_MALLOC(sizeof(coroutine_t));
    if (!coro) {
      sv_vm_destroy(async_vm);
      GC_ROOT_RESTORE(js, root_mark);
      return js_mkerr(js, "out of memory for coroutine");
    }
    
    GC_ROOT_PIN(js, promise);
    *coro = (coroutine_t){
      .js = js,
      .type = CORO_ASYNC_AWAIT,
      .this_val = this_val,
      .super_val = super_val,
      .new_target = js->new_target,
      .awaited_promise = js_mkundef(),
      .result = js_mkundef(),
      .async_func = callee_func,
      .args = NULL,
      .nargs = argc,
      .active_parent = NULL,
      .is_settled = false,
      .is_error = false,
      .is_done = false,
      .resume_point = 0,
      .yield_value = js_mkundef(),
      .async_promise = promise,
      .next = NULL,
      .mco = NULL,
      .mco_started = false,
      .is_ready = false,
      .did_suspend = false,
      .sv_vm = async_vm,
    };
    
    coro->active_parent = js->active_async_coro;
    js->active_async_coro = coro;
    
    ant_value_t result = sv_execute_closure_entry(
      async_vm, closure, callee_func, 
      super_val, this_val, args, argc, NULL
    );
    
    if (async_vm->suspended) {
      js->active_async_coro = coro->active_parent;
      coro->active_parent = NULL;
      enqueue_coroutine(coro);
      GC_ROOT_RESTORE(js, root_mark);
      return promise;
    }
    
    if (is_err(result)) {
      ant_value_t reject_value = js->thrown_value;
      if (vtype(reject_value) == T_UNDEF) reject_value = result;
      js->thrown_exists = false;
      js->thrown_value = js_mkundef();
      js_reject_promise(js, promise, reject_value);
    } else js_resolve_promise(js, promise, result);

    js->active_async_coro = coro->active_parent;
    coro->active_parent = NULL;
    
    free_coroutine(coro);
    GC_ROOT_RESTORE(js, root_mark);
    
    return promise;
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
    .active_parent = NULL,
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
    .did_suspend = false,
    .sv_vm = async_vm,
  };

  ctx->coro = coro;
  enqueue_coroutine(coro);
  MCO_RESUME_SAVE(js, mco, res);
  mco_state start_status = mco_status(mco);

  if (res != MCO_SUCCESS && start_status != MCO_DEAD) {
    remove_coroutine(coro);
    free_coroutine(coro);
    return js_mkerr(js, "failed to start async coroutine");
  }

  coro->mco_started = true;
  if (start_status == MCO_DEAD) {
    remove_coroutine(coro);
    free_coroutine(coro);
  }

  return promise;
}

static inline ant_value_t sv_await_value(ant_t *js, ant_value_t value) {
  if (vtype(value) != T_PROMISE) return value;

  mco_coro *current_mco = mco_running();
  if (!current_mco)
    current_mco = NULL;

  coroutine_t *coro = NULL;
  if (current_mco) {
    sv_coro_header_t *hdr = (sv_coro_header_t *)mco_get_user_data(current_mco);
    if (hdr) coro = hdr->coro;
  } else if (js->active_async_coro) coro = js->active_async_coro;

  if (!coro)
    return js_mkerr(js, "await can only be used inside async functions");

  coro->awaited_promise = value;
  coro->is_settled = false;
  coro->is_ready = false;
  js_await_result_t await_result = js_promise_await_coroutine(js, value, coro);

  if (await_result.state == JS_AWAIT_ERROR) {
    coro->is_settled = false;
    coro->awaited_promise = js_mkundef();
    return js_throw(js, await_result.value);
  }

  coro->did_suspend = true;
  if (!current_mco) {
    if (coro->sv_vm) coro->sv_vm->suspended = true;
    return js_mkundef();
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


#endif
