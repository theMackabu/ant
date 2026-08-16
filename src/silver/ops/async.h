#ifndef SV_ASYNC_H
#define SV_ASYNC_H

#include "ant.h"
#include "sugar.h"
#include "gc/roots.h"
#include "silver/engine.h"

typedef enum {
  SV_AWAIT_READY = 0,
  SV_AWAIT_ERROR,
  SV_AWAIT_SUSPENDED,
} sv_await_state_t;

typedef struct {
  sv_await_state_t state;
  ant_value_t value;
} sv_await_result_t;

static inline void sv_async_link_activation(ant_t *js, coroutine_t *coro) {
  if (!js || !coro) return;
  coro->active_parent = js->active_async_coro;
  coro->active_prev = NULL;
  
  if (js->active_async_coro) js->active_async_coro->active_prev = coro;
  js->active_async_coro = coro;
  
  if (coro->module_eval_ctx) js_module_eval_ctx_push(js, coro->module_eval_ctx);
  coroutine_hold(coro, CORO_HOLD_ACTIVE);
}

static inline void sv_async_unlink_activation(ant_t *js, coroutine_t *coro) {
  if (!js || !coro) return;
  
  if (coro->module_eval_ctx) js_module_eval_ctx_pop(js, coro->module_eval_ctx);
  if (coro->active_prev) coro->active_prev->active_parent = coro->active_parent;
  else if (js->active_async_coro == coro) js->active_async_coro = coro->active_parent;
  if (coro->active_parent) coro->active_parent->active_prev = coro->active_prev;
  
  coro->active_parent = NULL;
  coro->active_prev = NULL;
  coroutine_unhold(coro, CORO_HOLD_ACTIVE);
}

// TODO: flatten
static inline coroutine_t *sv_async_active_coro(ant_t *js) {
  return js ? js->active_async_coro : NULL;
}

static inline void sv_async_init_activation(
  coroutine_t *coro, ant_t *js, ant_value_t promise,
  ant_value_t this_val, ant_value_t super_val, ant_value_t new_target,
  ant_value_t async_func, int nargs
) {
  if (!coro) return;
  *coro = (coroutine_t){
    .js = js,
    .this_val = this_val,
    .super_val = super_val,
    .new_target = new_target,
    .result = js_mkundef(),
    .async_func = async_func,
    .owner_gen = js_mkundef(),
    .args = NULL,
    .awaited_promise = js_mkundef(),
    .async_promise = promise,
    .active_parent = NULL,
    .type = CORO_ASYNC_AWAIT,
    .nargs = nargs,
    .refcount = 1,
    .hold_bits = 0,
    .is_error = false,
    .await_registered = false,
  };
}

static inline ant_value_t sv_capture_tla_module_ctx(ant_t *js, coroutine_t *coro) {
  if (!js || !coro || !js->esm.module_stack || vtype(js->esm.module_stack->module_ns) != T_OBJ)
    return js_mkundef();

  ant_module_t *ctx = calloc(1, sizeof(ant_module_t));
  if (!ctx) return js_mkerr(js, "out of memory for TLA module context");

  *ctx = *js->esm.module_stack;
  ctx->prev = NULL;
  ctx->prev_import_meta_prop = js_mkundef();
  coro->module_eval_ctx = ctx;
  
  return js_mkundef();
}

static inline ant_value_t sv_start_tla(
  ant_t *js, sv_func_t *func, ant_value_t this_val,
  js_async_entry_t **async_entry_out
) {
  if (async_entry_out) *async_entry_out = NULL;
  ant_value_t promise = js_mkpromise(js);
  
  if (func) {
    GC_ROOT_SAVE(root_mark, js);
    GC_ROOT_PIN(js, this_val);
    GC_ROOT_PIN(js, promise);
    
    coroutine_t *coro = (coroutine_t *)calloc(1, sizeof(coroutine_t));
    if (!coro) {
      GC_ROOT_RESTORE(js, root_mark);
      return js_mkerr(js, "out of memory for TLA coroutine");
    }
    
    sv_async_init_activation(
      coro, js, promise, this_val,
      js_mkundef(), js_mkundef(), js_mkundef(), 0
    );
    
    ant_value_t module_res = sv_capture_tla_module_ctx(js, coro);
    if (is_err(module_res)) {
      coroutine_release(coro);
      GC_ROOT_RESTORE(js, root_mark);
      return module_res;
    }
    
    js_async_entry_t *async_entry = async_entry_out
      ? js_eval_async_entry_create(coro)
      : NULL;
    
    if (async_entry_out && !async_entry) {
      coroutine_release(coro);
      GC_ROOT_RESTORE(js, root_mark);
      return js_mkerr(js, "out of memory for async entry handle");
    }
    
    sv_async_link_activation(js, coro);
    ant_value_t result = sv_execute_entry(
      js->vm, func,
      this_val, NULL, 0
    );
    sv_async_unlink_activation(js, coro);
    
    if (coro->act && coro->act->frame_count > 0) {
      if (async_entry_out) *async_entry_out = async_entry;
      coroutine_release(coro);
      GC_ROOT_RESTORE(js, root_mark);
      return promise;
    }
    
    js_eval_async_entry_release(async_entry);
    
    if (is_err(result)) {
      ant_value_t reject_value = js->thrown_exists ? js->thrown_value : result;
      js->thrown_exists = false;
      js->thrown_value = js_mkundef();
      js_reject_promise(js, promise, reject_value);
    } else {
      js_resolve_promise(js, promise, result);
    }
    coroutine_release(coro);
    GC_ROOT_RESTORE(js, root_mark);
    
    return promise;
  }

  return js_mkerr(js, "top-level await module cannot start");
}

static inline ant_value_t sv_start_async_closure(
  sv_vm_t *caller_vm, ant_t *js,
  sv_closure_t *closure, ant_value_t callee_func, ant_value_t super_val,
  ant_value_t this_val, ant_value_t *args, int argc
) {
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
      ant_value_t reject_value = js->thrown_exists ? js->thrown_value : result;
      js->thrown_exists = false;
      js->thrown_value = js_mkundef();
      js_reject_promise(js, promise, reject_value);
    } else js_resolve_promise(js, promise, result);
    
    GC_ROOT_RESTORE(js, root_mark);
    return promise;
  }

  if (caller_vm && closure && closure->func) {
    GC_ROOT_SAVE(root_mark, js);
    GC_ROOT_PIN(js, callee_func);
    GC_ROOT_PIN(js, super_val);
    GC_ROOT_PIN(js, this_val);
    
    if (args)
      for (int i = 0; i < argc; i++) GC_ROOT_PIN(js, args[i]);
    
    ant_value_t promise = js_mkpromise(js);
    GC_ROOT_PIN(js, promise);
    
    coroutine_t *coro = (coroutine_t *)calloc(1, sizeof(coroutine_t));
    if (!coro) {
      GC_ROOT_RESTORE(js, root_mark);
      return js_mkerr(js, "out of memory for async coroutine");
    }

    sv_async_init_activation(
      coro, js, promise, this_val,
      super_val, js->new_target, callee_func, argc
    );
    
    sv_async_link_activation(js, coro);
    ant_value_t result = sv_execute_closure_entry(
      caller_vm, closure, callee_func, 
      super_val, this_val, args, argc, NULL
    );
    sv_async_unlink_activation(js, coro);
  
    if (coro->act && coro->act->frame_count > 0) {
      coroutine_release(coro);
      GC_ROOT_RESTORE(js, root_mark);
      return promise;
    }
  
    if (is_err(result)) {
      ant_value_t reject_value = js->thrown_exists ? js->thrown_value : result;
      js->thrown_exists = false;
      js->thrown_value = js_mkundef();
      js_reject_promise(js, promise, reject_value);
    } else js_resolve_promise(js, promise, result);

    coroutine_release(coro);
    GC_ROOT_RESTORE(js, root_mark);
  
    return promise;
  }

  return js_mkerr(js, "async function cannot start (unsupported form)");
}

static inline sv_await_result_t sv_await_value(sv_vm_t *vm, ant_t *js, ant_value_t value) {
  sv_await_result_t out = {
    .state = SV_AWAIT_READY,
    .value = js_mkundef(),
  };

  value = js_promise_assimilate_awaitable(js, value);
  if (is_err(value)) {
    out.state = SV_AWAIT_ERROR;
    out.value = value;
    return out;
  }
  if (vtype(value) != T_PROMISE) {
    out.value = value;
    return out;
  }

  coroutine_t *coro = sv_async_active_coro(js);
  if (!coro) {
    out.state = SV_AWAIT_ERROR;
    out.value = js_mkerr(js, "await can only be used inside async functions");
    return out;
  }

  js_await_result_t await_result = js_promise_await_coroutine(js, value, coro);
  if (await_result.state == JS_AWAIT_ERROR) {
    out.state = SV_AWAIT_ERROR;
    out.value = js_throw(js, await_result.value);
    return out;
  }

  int entry_fp = vm->suspended_entry_fp;
  if (entry_fp < 0 || entry_fp > vm->fp) {
    coroutine_clear_await_registration(coro);
    out.state = SV_AWAIT_ERROR;
    out.value = js_mkerr(js, "await suspended without a valid entry frame");
    return out;
  }

  sv_activation_t *act = sv_activation_capture(vm, entry_fp, coro->act);
  if (!act) {
    coroutine_clear_await_registration(coro);
    out.state = SV_AWAIT_ERROR;
    out.value = js_mkerr(js, "out of memory capturing async activation");
    return out;
  }

  coro->act = act;
  out.state = SV_AWAIT_SUSPENDED;
  
  return out;
}


#endif
