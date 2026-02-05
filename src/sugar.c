#include "internal.h"
#include "gc.h"
#include "errors.h"
#include "sugar.h"

#define MCO_USE_VMEM_ALLOCATOR
#define MCO_ZERO_MEMORY
#define MCO_DEFAULT_STACK_SIZE (1024 * 1024)
#define MINICORO_IMPL
#include <minicoro.h>

uint32_t coros_this_tick = 0;
coroutine_queue_t pending_coroutines = {NULL, NULL};
static bool coro_stack_size_initialized = false;

bool has_pending_coroutines(void) {
  return pending_coroutines.head != NULL;
}

bool has_ready_coroutines(void) {
  coroutine_t *temp = pending_coroutines.head;
  while (temp) {
    if (temp->is_ready) return true;
    temp = temp->next;
  }
  return false;
}

void enqueue_coroutine(coroutine_t *coro) {
  if (!coro) return;
  coro->next = NULL;
  coro->prev = pending_coroutines.tail;
  
  if (pending_coroutines.tail) {
    pending_coroutines.tail->next = coro;
  } else pending_coroutines.head = coro;
  pending_coroutines.tail = coro;
}

void remove_coroutine(coroutine_t *coro) {
  if (!coro) return;
  
  if (coro->prev) {
    coro->prev->next = coro->next;
  } else pending_coroutines.head = coro->next;
  
  if (coro->next) {
    coro->next->prev = coro->prev;
  } else pending_coroutines.tail = coro->prev;
  
  coro->prev = NULL;
  coro->next = NULL;
}

void free_coroutine(coroutine_t *coro) {
  if (!coro) return;
  
  if (coro->mco) {
    if (mco_running() == coro->mco) fprintf(stderr, "WARNING: Attempting to free a running coroutine\n");
    async_exec_context_t *ctx = (async_exec_context_t *)mco_get_user_data(coro->mco);
    if (ctx) CORO_FREE(ctx);
    mco_destroy(coro->mco);
    coro->mco = NULL;
  }
  
  if (coro->args) CORO_FREE(coro->args);
  if (coro->for_let_stack) free(coro->for_let_stack);
  if (coro->scope_stack) utarray_free(coro->scope_stack);
  
  CORO_FREE(coro);
}

static void for_let_swap_with_coro(struct js *js, coroutine_t *coro) {
  struct for_let_ctx *tmp_stack = js->for_let_stack;
  int tmp_len = js->for_let_stack_len;
  int tmp_cap = js->for_let_stack_cap;
  
  js->for_let_stack = coro->for_let_stack;
  js->for_let_stack_len = coro->for_let_stack_len;
  js->for_let_stack_cap = coro->for_let_stack_cap;
  
  coro->for_let_stack = tmp_stack;
  coro->for_let_stack_len = tmp_len;
  coro->for_let_stack_cap = tmp_cap;
}

coro_saved_state_t coro_enter(struct js *js, coroutine_t *coro) {
  extern UT_array *global_scope_stack;
  coro_saved_state_t saved = { js->scope, global_scope_stack };
  js->scope = coro->scope;
  global_scope_stack = coro->scope_stack;
  for_let_swap_with_coro(js, coro);
  return saved;
}

void coro_leave(struct js *js, coroutine_t *coro, coro_saved_state_t saved) {
  extern UT_array *global_scope_stack;
  coro->scope = js->scope;
  coro->scope_stack = global_scope_stack;
  js->scope = saved.scope;
  global_scope_stack = saved.scope_stack;
  for_let_swap_with_coro(js, coro);
}

static size_t calculate_coro_stack_size(void) {
  static size_t cached_size = 0;
  if (coro_stack_size_initialized) return cached_size;
  coro_stack_size_initialized = true;
  const char *env_stack = getenv("ANT_CORO_STACK_SIZE");
  if (env_stack) {
    size_t size = (size_t)atoi(env_stack) * 1024;
    if (size >= 32 * 1024 && size <= 8 * 1024 * 1024) { 
      cached_size = size; return cached_size; 
    }
  }
  return cached_size;
}

static void mco_async_entry(mco_coro* mco) {
  async_exec_context_t *ctx = (async_exec_context_t *)mco_get_user_data(mco);
  
  struct js *js = ctx->js;
  coroutine_t *coro = ctx->coro;
  jsval_t result;
  
  jsval_t saved_super = js->super_val;
  jsval_t saved_new_target = js->new_target;
  if (coro) {
    js->super_val = coro->super_val;
    js->new_target = coro->new_target;
  }
  
  if (coro && coro->nargs > 0 && coro->args) {
    result = call_js_code_with_args(js, ctx->code, (jsoff_t)ctx->code_len, ctx->closure_scope, coro->args, coro->nargs, js_mkundef());
  } else result = call_js(js, ctx->code, (jsoff_t)ctx->code_len, ctx->closure_scope);
  
  js->super_val = saved_super;
  js->new_target = saved_new_target;
  
  ctx->result = result;
  ctx->has_error = is_err(result);
  
  if (ctx->has_error) {
    jsval_t reject_value = js->thrown_value;
    if (vtype(reject_value) == T_UNDEF) {
      reject_value = js_mkstr(js, js->errmsg ? js->errmsg : "Unknown error", js->errmsg ? strlen(js->errmsg) : 13);
    }
    js->flags &= (uint8_t)~F_THROW;
    js->thrown_value = js_mkundef();
    js_reject_promise(js, ctx->promise, reject_value);
  } else js_resolve_promise(js, ctx->promise, result);
}

jsval_t start_async_in_coroutine(struct js *js, const char *code, size_t code_len, jsval_t closure_scope, jsval_t *args, int nargs) {
  if (++coros_this_tick > CORO_PER_TICK_LIMIT) {
    js->fatal_error = true;
    return js_mkerr_typed(js, JS_ERR_RANGE | JS_ERR_NO_STACK, "Maximum async operations per tick exceeded");
  }
  
  jsval_t promise = js_mkpromise(js);  
  async_exec_context_t *ctx = (async_exec_context_t *)CORO_MALLOC(sizeof(async_exec_context_t));
  if (!ctx) return js_mkerr(js, "out of memory for async context");
  
  ctx->js = js;
  ctx->code = code;
  ctx->code_len = code_len;
  ctx->closure_scope = closure_scope;
  ctx->result = js_mkundef();
  ctx->promise = promise;
  ctx->has_error = false;
  ctx->coro = NULL;
  
  size_t stack_size = calculate_coro_stack_size();
  mco_desc desc = mco_desc_init(mco_async_entry, stack_size);
  desc.user_data = ctx;
  
  mco_coro* mco = NULL;
  mco_result res = mco_create(&mco, &desc);
  if (res != MCO_SUCCESS) {
    CORO_FREE(ctx);
    return js_mkerr(js, "failed to create minicoro coroutine");
  }
  
  coroutine_t *coro = (coroutine_t *)CORO_MALLOC(sizeof(coroutine_t));
  if (!coro) {
    mco_destroy(mco);
    CORO_FREE(ctx);
    return js_mkerr(js, "out of memory for coroutine");
  }
  
  *coro = (coroutine_t){
    .js = js,
    .type = CORO_ASYNC_AWAIT,
    .scope = closure_scope,
    .this_val = js->this_val,
    .super_val = js->super_val,
    .new_target = js->new_target,
    .awaited_promise = js_mkundef(),
    .result = js_mkundef(),
    .async_func = js->current_func,
    .args = NULL,
    .nargs = nargs,
    .is_settled = false,
    .is_error = false,
    .is_done = false,
    .resume_point = 0,
    .yield_value = js_mkundef(),
    .next = NULL,
    .mco = mco,
    .mco_started = false,
    .is_ready = true,
    .for_let_stack = NULL,
    .for_let_stack_len = 0,
    .for_let_stack_cap = 0,
  };
  
  if (nargs > 0) {
    coro->args = (jsval_t *)CORO_MALLOC(sizeof(jsval_t) * nargs);
    if (!coro->args) {
      mco_destroy(mco); CORO_FREE(coro); CORO_FREE(ctx);
      return js_mkerr(js, "out of memory for coroutine args");
    }
    memcpy(coro->args, args, sizeof(jsval_t) * nargs);
  }
  
  extern UT_array *global_scope_stack;
  utarray_new(coro->scope_stack, &jsoff_icd);
  
  if (!coro->scope_stack) {
    mco_destroy(mco); CORO_FREE(coro); CORO_FREE(ctx);
    return js_mkerr(js, "out of memory for coroutine scope stack");
  }
  
  jsoff_t glob_off = (jsoff_t)vdata(js_glob(js));
  utarray_push_back(coro->scope_stack, &glob_off);
  
  ctx->coro = coro;  
  enqueue_coroutine(coro);
  
  coro_saved_state_t saved = coro_enter(js, coro);
  res = mco_resume(mco);
  coro_leave(js, coro, saved);
  
  if (res != MCO_SUCCESS && mco_status(mco) != MCO_DEAD) {
    remove_coroutine(coro);
    free_coroutine(coro);
    return js_mkerr(js, "failed to start coroutine");
  }
  
  coro->mco_started = true;
  if (mco_status(mco) == MCO_DEAD) {
    remove_coroutine(coro);
    free_coroutine(coro);
    js_gc_safepoint(js);
  }
  
  return promise;
}

static inline void settle_coroutine(coroutine_t *coro, jsval_t *args, int nargs, bool is_error) {
  coro->result = nargs > 0 ? args[0] : js_mkundef();
  coro->is_settled = true;
  coro->is_error = is_error;
  coro->is_ready = true;
}

static void resume_coroutine_if_suspended(struct js *js, coroutine_t *coro) {
  if (!coro || !coro->mco || mco_status(coro->mco) != MCO_SUSPENDED) return;

  remove_coroutine(coro);
  coro_saved_state_t saved = coro_enter(js, coro);
  mco_result res = mco_resume(coro->mco);
  coro_leave(js, coro, saved);

  if (res == MCO_SUCCESS && mco_status(coro->mco) != MCO_DEAD) {
    coro->is_ready = false;
    enqueue_coroutine(coro);
  } else free_coroutine(coro);
}

jsval_t resume_coroutine_wrapper(struct js *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t coro_val = js_get_slot(js, me, SLOT_CORO);
  if (vtype(coro_val) != T_NUM) return js_mkundef();
  
  coroutine_t *coro = (coroutine_t *)(uintptr_t)tod(coro_val);
  if (!coro) return js_mkundef();

  settle_coroutine(coro, args, nargs, false);
  resume_coroutine_if_suspended(js, coro);

  return js_mkundef();
}

jsval_t reject_coroutine_wrapper(struct js *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t coro_val = js_get_slot(js, me, SLOT_CORO);
  
  if (vtype(coro_val) != T_NUM) return js_mkundef();
  
  coroutine_t *coro = (coroutine_t *)(uintptr_t)tod(coro_val);
  if (!coro) return js_mkundef();

  settle_coroutine(coro, args, nargs, true);
  resume_coroutine_if_suspended(js, coro);

  return js_mkundef();
}
