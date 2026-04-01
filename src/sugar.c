#include "internal.h"
#include "sugar.h"
#include "modules/timer.h"
#include "silver/engine.h"
#include "silver/vm.h"

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
    void *ctx = mco_get_user_data(coro->mco);
    if (ctx) CORO_FREE(ctx);
    mco_destroy(coro->mco);
    coro->mco = NULL;
  }
  
  if (coro->args) CORO_FREE(coro->args);
  if (coro->sv_vm) { sv_vm_destroy(coro->sv_vm); coro->sv_vm = NULL; }
  
  CORO_FREE(coro);
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
  }}
  
  return cached_size;
}

static inline void settle_coroutine(coroutine_t *coro, ant_value_t *args, int nargs, bool is_error) {
  coro->result = nargs > 0 ? args[0] : js_mkundef();
  coro->is_settled = true;
  coro->is_error = is_error;
  coro->is_ready = true;
}

static void resume_coroutine_if_suspended(ant_t *js, coroutine_t *coro) {
  if (!coro) return;

  if (!coro->mco) {
    if (!coro->sv_vm || !coro->sv_vm->suspended) return;
    
    coro->is_ready = false;
    coro->sv_vm->suspended_resume_value = coro->result;
    coro->sv_vm->suspended_resume_is_error = coro->is_error;
    coro->sv_vm->suspended_resume_pending = true;
    
    coro->active_parent = js->active_async_coro;
    js->active_async_coro = coro;
    ant_value_t result = sv_resume_suspended(coro->sv_vm);
    
    coro->is_settled = false;
    coro->awaited_promise = js_mkundef();
    
    if (coro->sv_vm->suspended) {
      js->active_async_coro = coro->active_parent;
      coro->active_parent = NULL;
      return;
    } remove_coroutine(coro);
    
    if (is_err(result)) {
      ant_value_t reject_value = js->thrown_value;
      if (vtype(reject_value) == T_UNDEF) reject_value = result;
      js->thrown_exists = false;
      js->thrown_value = js_mkundef();
      js_reject_promise(js, coro->async_promise, reject_value);
    } else js_resolve_promise(js, coro->async_promise, result);
    
    js_maybe_drain_microtasks_after_async_settle(js);
    js->active_async_coro = coro->active_parent;
    
    coro->active_parent = NULL;
    free_coroutine(coro);
    
    return;
  }

  if (mco_status(coro->mco) != MCO_SUSPENDED) return;

  coro->is_ready = false;
  mco_result res;
  MCO_RESUME_SAVE(js, coro->mco, res);
  mco_state status = mco_status(coro->mco);

  if (res != MCO_SUCCESS || status == MCO_DEAD) {
    remove_coroutine(coro);
    free_coroutine(coro);
  }
}

ant_value_t resume_coroutine_wrapper(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t me = js->current_func;
  ant_value_t coro_val = js_get_slot(me, SLOT_CORO);
  if (vtype(coro_val) != T_NUM) return js_mkundef();
  
  coroutine_t *coro = (coroutine_t *)(uintptr_t)tod(coro_val);
  if (!coro) return js_mkundef();

  settle_coroutine(coro, args, nargs, false);
  resume_coroutine_if_suspended(js, coro);

  return js_mkundef();
}

ant_value_t reject_coroutine_wrapper(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t me = js->current_func;
  ant_value_t coro_val = js_get_slot(me, SLOT_CORO);
  
  if (vtype(coro_val) != T_NUM) return js_mkundef();
  
  coroutine_t *coro = (coroutine_t *)(uintptr_t)tod(coro_val);
  if (!coro) return js_mkundef();

  settle_coroutine(coro, args, nargs, true);
  resume_coroutine_if_suspended(js, coro);

  return js_mkundef();
}

void settle_and_resume_coroutine(ant_t *js, coroutine_t *coro, ant_value_t value, bool is_error) {
  if (!coro) return;
  ant_value_t args[1] = { value };
  settle_coroutine(coro, args, 1, is_error);
  resume_coroutine_if_suspended(js, coro);
}
