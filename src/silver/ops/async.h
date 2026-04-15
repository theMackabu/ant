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
      op == OP_YIELD_STAR_NEXT ||
      op == OP_YIELD_STAR_THROW ||
      op == OP_YIELD_STAR_RETURN
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
  if (vm && vm->suspended) return;

  if (is_err(result)) {
    ant_value_t reject_value = js->thrown_exists ? js->thrown_value : result;
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

typedef enum {
  SV_AWAIT_READY = 0,
  SV_AWAIT_ERROR,
  SV_AWAIT_SUSPENDED,
} sv_await_state_t;

typedef struct {
  sv_await_state_t state;
  ant_value_t value;
  bool handoff;
} sv_await_result_t;

static inline void sv_async_link_activation(ant_t *js, coroutine_t *coro) {
  if (!js || !coro) return;
  coro->active_parent = js->active_async_coro;
  js->active_async_coro = coro;
  coroutine_hold(coro, CORO_HOLD_ACTIVE);
}

static inline void sv_async_unlink_activation(ant_t *js, coroutine_t *coro) {
  if (!js || !coro) return;
  if (js->active_async_coro == coro) js->active_async_coro = coro->active_parent;
  coro->active_parent = NULL;
  coroutine_unhold(coro, CORO_HOLD_ACTIVE);
}

static inline bool sv_async_coro_matches_vm(const coroutine_t *coro, const sv_vm_t *vm) {
  if (!coro || !vm) return false;
  if (coro->sv_vm == vm) return true;
  return coro->owner_vm == vm;
}

static inline coroutine_t *sv_async_get_active_coro_for_vm(ant_t *js, sv_vm_t *vm) {
  if (!js || !js->active_async_coro) return NULL;

  if (!vm) return js->active_async_coro;

  for (coroutine_t *it = js->active_async_coro; it; it = it->active_parent) {
    if (sv_async_coro_matches_vm(it, vm)) return it;
  }

  return NULL;
}

static inline void sv_async_init_activation(
  coroutine_t *coro, ant_t *js, sv_vm_t *owner_vm, ant_value_t promise,
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
    .yield_value = js_mkundef(),
    .args = NULL,
    .awaited_promise = js_mkundef(),
    .async_promise = promise,
    .active_parent = NULL,
    .prev = NULL,
    .next = NULL,
    .mco = NULL,
    .owner_vm = owner_vm,
    .sv_vm = NULL,
    .resume_point = 0,
    .type = CORO_ASYNC_AWAIT,
    .owner_entry_fp = owner_vm ? owner_vm->fp : -1,
    .owner_saved_fp = owner_vm ? owner_vm->fp - 1 : -1,
    .nargs = nargs,
    .refcount = 1,
    .hold_bits = 0,
    .is_settled = false,
    .is_error = false,
    .is_done = false,
    .materialized = false,
    .mco_started = false,
    .is_ready = false,
    .did_suspend = false,
    .await_registered = false,
    .destroy_requested = false,
  };
}

static inline void sv_async_move_open_upvalues(
  sv_vm_t *source_vm, sv_vm_t *async_vm,
  ant_value_t *source_base, ant_value_t *dest_base, size_t stack_count
) {
  if (!source_vm || !async_vm || !source_base || !dest_base || stack_count == 0) return;

  sv_upvalue_t **src_pp = &source_vm->open_upvalues;
  sv_upvalue_t **dst_pp = &async_vm->open_upvalues;

  while (*src_pp) {
    sv_upvalue_t *uv = *src_pp;
    if (!sv_slot_in_range(source_base, stack_count, uv->location)) {
      src_pp = &uv->next;
      continue;
    }

    ptrdiff_t slot = uv->location - source_base;
    *src_pp = uv->next;
    uv->location = &dest_base[slot];
    uv->next = NULL;
    *dst_pp = uv;
    dst_pp = &uv->next;
  }
}

static inline sv_vm_t *sv_async_prepare_materialization(
  sv_vm_t *source_vm, ant_t *js, coroutine_t *coro
) {
  if (!source_vm || !js || !coro || coro->sv_vm) return coro ? coro->sv_vm : NULL;
  if (source_vm->fp < 0) return NULL;

  int entry_fp = source_vm->suspended_entry_fp;
  if (entry_fp < 0 || entry_fp > source_vm->fp) entry_fp = source_vm->fp;

  sv_frame_t *entry_frame = &source_vm->frames[entry_fp];
  int frame_count = source_vm->fp - entry_fp + 1;
  int stack_base = entry_frame->prev_sp;
  int stack_count = source_vm->sp - stack_base;
  int handler_base = entry_frame->handler_base;
  int handler_count = source_vm->handler_depth - handler_base;

  sv_vm_t *async_vm = sv_vm_create(js, SV_VM_ASYNC);
  if (!async_vm) return NULL;
  if (stack_count < 0 || stack_count > async_vm->stack_size) {
    sv_vm_destroy(async_vm);
    return NULL;
  }
  if (frame_count < 1 || frame_count > async_vm->max_frames) {
    sv_vm_destroy(async_vm);
    return NULL;
  }
  if (handler_count < 0 || handler_count > SV_HANDLER_MAX) {
    sv_vm_destroy(async_vm);
    return NULL;
  }

  if (stack_count > 0) {
    memcpy(
      async_vm->stack,
      &source_vm->stack[stack_base],
      sizeof(ant_value_t) * (size_t)stack_count
    );
  }
  async_vm->sp = stack_count;
  async_vm->fp = frame_count - 1;

  for (int i = 0; i < frame_count; i++) {
    sv_frame_t *src = &source_vm->frames[entry_fp + i];
    sv_frame_t *dst = &async_vm->frames[i];
    *dst = *src;
    dst->prev_sp = src->prev_sp - stack_base;
    dst->handler_base = src->handler_base - handler_base;
    dst->handler_top = src->handler_top - handler_base;

    if (src->bp)
      dst->bp = async_vm->stack + (src->bp - &source_vm->stack[stack_base]);
    if (src->lp)
      dst->lp = async_vm->stack + (src->lp - &source_vm->stack[stack_base]);
  }

  if (handler_count > 0) {
    memcpy(
      async_vm->handler_stack,
      &source_vm->handler_stack[handler_base],
      sizeof(sv_handler_t) * (size_t)handler_count
    );
  }
  async_vm->handler_depth = handler_count;

  async_vm->suspended = true;
  async_vm->suspended_entry_fp = 0;
  async_vm->suspended_saved_fp = -1;
  async_vm->suspended_resume_pending = false;
  async_vm->suspended_resume_is_error = false;
  async_vm->suspended_resume_kind = SV_RESUME_NEXT;
  async_vm->suspended_resume_value = js_mkundef();

  return async_vm;
}

static inline bool sv_async_materialize_activation(
  sv_vm_t *source_vm, sv_vm_t *async_vm, coroutine_t *coro
) {
  if (!source_vm || !async_vm || !coro || source_vm->fp < 0) return false;

  int entry_fp = source_vm->suspended_entry_fp;
  if (entry_fp < 0 || entry_fp > source_vm->fp) entry_fp = source_vm->fp;
  sv_frame_t *entry_frame = &source_vm->frames[entry_fp];
  ant_value_t *source_base = &source_vm->stack[entry_frame->prev_sp];
  size_t stack_count = (size_t)(source_vm->sp - entry_frame->prev_sp);
  sv_async_move_open_upvalues(
    source_vm, async_vm, source_base, async_vm->stack, stack_count
  );

  coro->owner_entry_fp = source_vm->suspended_entry_fp;
  coro->owner_saved_fp = source_vm->suspended_saved_fp;
  coro->sv_vm = async_vm;
  coro->materialized = true;
  return true;
}

static void sv_mco_tla_entry(mco_coro *mco) {
  sv_tla_ctx_t *ctx = (sv_tla_ctx_t *)mco_get_user_data(mco);
  ant_t *js = ctx->js;
  sv_vm_t *vm = ctx->vm;
  MCO_CORO_STACK_ENTER(js, mco);

  ant_value_t result = sv_execute_entry(vm, ctx->func, ctx->this_val, NULL, 0);
  ant_value_t promise = ctx->coro->async_promise;
  if (vm && vm->suspended) return;

  if (is_err(result)) {
    ant_value_t reject_value = js->thrown_exists ? js->thrown_value : result;
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
    GC_ROOT_PIN(js, promise);

    coroutine_t *coro = (coroutine_t *)CORO_MALLOC(sizeof(coroutine_t));
    if (!coro) {
      GC_ROOT_RESTORE(js, root_mark);
      return js_mkerr(js, "out of memory for TLA coroutine");
    }

    sv_async_init_activation(
      coro, js, js->vm, promise, this_val,
      js_mkundef(), js_mkundef(), js_mkundef(), 0
    );
    sv_async_link_activation(js, coro);

    ant_value_t result = sv_execute_entry(
      js->vm, func,
      this_val, NULL, 0
    );
    sv_async_unlink_activation(js, coro);

    if (coro->sv_vm && coro->sv_vm->suspended) {
      coroutine_release(coro);
      GC_ROOT_RESTORE(js, root_mark);
      return promise;
    }

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
    .owner_vm = async_vm,
    .sv_vm = async_vm,
    .mco_started = false,
    .is_ready = true,
    .did_suspend = false,
    .refcount = 1,
    .hold_bits = 0,
    .await_registered = false,
    .destroy_requested = false,
  };

  ctx->coro = coro;
  enqueue_coroutine(coro);
  MCO_RESUME_SAVE(js, mco, res);

  if (res != MCO_SUCCESS && mco_status(mco) != MCO_DEAD) {
    remove_coroutine(coro);
    coroutine_release(coro);
    return js_mkerr(js, "failed to start TLA coroutine");
  }

  coro->mco_started = true;
  if (mco_status(mco) == MCO_DEAD) {
    remove_coroutine(coro);
  }

  coroutine_release(coro);

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
      ant_value_t reject_value = js->thrown_exists ? js->thrown_value : result;
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
    GC_ROOT_PIN(js, promise);
    coroutine_t *coro = (coroutine_t *)CORO_MALLOC(sizeof(coroutine_t));
    if (!coro) {
      GC_ROOT_RESTORE(js, root_mark);
      return js_mkerr(js, "out of memory for async coroutine");
    }

    sv_async_init_activation(
      coro, js, caller_vm, promise, this_val,
      super_val, js->new_target, callee_func, argc
    );
    sv_async_link_activation(js, coro);
    
    ant_value_t result = sv_execute_closure_entry(
      caller_vm, closure, callee_func, 
      super_val, this_val, args, argc, NULL
    );

    sv_async_unlink_activation(js, coro);

    if (coro->sv_vm && coro->sv_vm->suspended) {
      coroutine_release(coro);
      GC_ROOT_RESTORE(js, root_mark);
      return promise;
    }
    
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
    .owner_vm = async_vm,
    .sv_vm = async_vm,
    .mco_started = false,
    .is_ready = true,
    .did_suspend = false,
    .refcount = 1,
    .hold_bits = 0,
    .await_registered = false,
    .destroy_requested = false,
  };

  ctx->coro = coro;
  enqueue_coroutine(coro);
  MCO_RESUME_SAVE(js, mco, res);
  mco_state start_status = mco_status(mco);

  if (res != MCO_SUCCESS && start_status != MCO_DEAD) {
    remove_coroutine(coro);
    coroutine_release(coro);
    return js_mkerr(js, "failed to start async coroutine");
  }

  coro->mco_started = true;
  if (start_status == MCO_DEAD) {
    remove_coroutine(coro);
  }

  coroutine_release(coro);

  return promise;
}



static inline sv_await_result_t sv_await_value(sv_vm_t *vm, ant_t *js, ant_value_t value) {
  sv_await_result_t out = {
    .state = SV_AWAIT_READY,
    .value = js_mkundef(),
    .handoff = false,
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

  mco_coro *current_mco = mco_running();
  if (!current_mco) current_mco = NULL;

  coroutine_t *coro = NULL;
  if (current_mco) {
    sv_coro_header_t *hdr = (sv_coro_header_t *)mco_get_user_data(current_mco);
    if (hdr) coro = hdr->coro;
  } else coro = sv_async_get_active_coro_for_vm(js, vm);

  if (!coro) {
    out.state = SV_AWAIT_ERROR;
    out.value = js_mkerr(js, "await can only be used inside async functions");
    return out;
  }

  sv_vm_t *prepared_vm = NULL;
  bool handoff = false;
  if (!current_mco && vm && coro->owner_vm == vm && !coro->sv_vm) {
    prepared_vm = sv_async_prepare_materialization(vm, js, coro);
    if (!prepared_vm) {
      out.state = SV_AWAIT_ERROR;
      out.value = js_mkerr(js, "out of memory for async VM");
      return out;
    }
    handoff = true;
  }

  coro->is_settled = false;
  coro->is_ready = false;
  js_await_result_t await_result = js_promise_await_coroutine(js, value, coro);

  if (await_result.state == JS_AWAIT_ERROR) {
    if (prepared_vm) {
      sv_vm_destroy(prepared_vm);
      coro->sv_vm = NULL;
      coro->materialized = false;
    }
    coro->is_settled = false;
    out.state = SV_AWAIT_ERROR;
    out.value = js_throw(js, await_result.value);
    return out;
  }

  coro->did_suspend = true;
  if (!current_mco) {
    if (prepared_vm) {
      if (!sv_async_materialize_activation(vm, prepared_vm, coro)) {
        coroutine_clear_await_registration(coro);
        sv_vm_destroy(prepared_vm);
        out.state = SV_AWAIT_ERROR;
        out.value = js_mkerr(js, "failed to materialize async activation");
        return out;
      }
    }
    out.state = SV_AWAIT_SUSPENDED;
    out.handoff = handoff;
    if (handoff) coro->sv_vm->suspended = true;
    else if (coro->sv_vm) coro->sv_vm->suspended = true;
    return out;
  }
  
  mco_result mco_res = mco_yield(current_mco);
  if (mco_res != MCO_SUCCESS) {
    out.state = SV_AWAIT_ERROR;
    out.value = js_mkerr(js, "failed to yield coroutine");
    return out;
  }

  MCO_CORO_STACK_ENTER(js, current_mco);
  out.value = coro->result;
  bool is_error = coro->is_error;
  
  coro->is_settled = false;
  coro->awaited_promise = js_mkundef();
  if (is_error) {
    out.state = SV_AWAIT_ERROR;
    out.value = js_throw(js, out.value);
    return out;
  }

  out.state = SV_AWAIT_READY;
  return out;
}


#endif
