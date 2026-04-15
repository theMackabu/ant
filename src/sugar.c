#include "internal.h"
#include "sugar.h"

#include "modules/generator.h"
#include "modules/timer.h"
#include "silver/engine.h"

#define MCO_USE_VMEM_ALLOCATOR
#define MCO_ZERO_MEMORY
#define MCO_DEFAULT_STACK_SIZE (1024 * 1024)
#define MINICORO_IMPL
#include <minicoro.h>

uint32_t coros_this_tick = 0;
coroutine_queue_t pending_coroutines = {NULL, NULL};

static bool coro_stack_size_initialized = false;
static coroutine_t *retired_coroutines = NULL;

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
  if (coro->hold_bits & CORO_HOLD_PENDING) return;
  coro->next = NULL;
  coro->prev = pending_coroutines.tail;
  
  if (pending_coroutines.tail) {
    pending_coroutines.tail->next = coro;
  } else pending_coroutines.head = coro;
  pending_coroutines.tail = coro;
  coroutine_hold(coro, CORO_HOLD_PENDING);
}

void remove_coroutine(coroutine_t *coro) {
  if (!coro || !(coro->hold_bits & CORO_HOLD_PENDING)) return;
  
  if (coro->prev) {
    coro->prev->next = coro->next;
  } else pending_coroutines.head = coro->next;
  
  if (coro->next) {
    coro->next->prev = coro->prev;
  } else pending_coroutines.tail = coro->prev;
  
  coro->prev = NULL;
  coro->next = NULL;
  coroutine_unhold(coro, CORO_HOLD_PENDING);
}

static void retire_coroutine_storage(coroutine_t *coro) {
  if (!coro) return;
  coro->next = retired_coroutines;
  coro->prev = NULL;
  retired_coroutines = coro;
}

static void destroy_coroutine_resources(coroutine_t *coro) {
  if (!coro) return;

  if (coro->mco) {
    void *ctx = mco_get_user_data(coro->mco);
    if (ctx) CORO_FREE(ctx);
    mco_destroy(coro->mco);
    coro->mco = NULL;
  }

  if (coro->args) {
    CORO_FREE(coro->args);
    coro->args = NULL;
  }

  if (coro->sv_vm) {
    sv_vm_destroy(coro->sv_vm);
    coro->sv_vm = NULL;
  }

  coro->js = NULL;
  coro->owner_vm = NULL;
  coro->active_parent = NULL;
  coro->materialized = false;
}

void coroutine_retain(coroutine_t *coro) {
  if (!coro) return;
  coro->refcount++;
}

static void coroutine_release_storage(coroutine_t *coro) {
  if (!coro) return;

  ant_t *js = coro->js;
  if (js && js->vm_exec_depth > 0) retire_coroutine_storage(coro);
  else {
    destroy_coroutine_resources(coro);
    CORO_FREE(coro);
  }
}

void coroutine_release(coroutine_t *coro) {
  if (!coro || coro->refcount == 0) return;
  coro->refcount--;
  if (coro->refcount != 0) return;
  coroutine_release_storage(coro);
}

void coroutine_hold(coroutine_t *coro, uint8_t hold) {
  if (!coro || (coro->hold_bits & hold)) return;
  coro->hold_bits |= hold;
  coroutine_retain(coro);
}

void coroutine_unhold(coroutine_t *coro, uint8_t hold) {
  if (!coro || !(coro->hold_bits & hold)) return;
  coro->hold_bits &= (uint8_t)~hold;
  coroutine_release(coro);
}

void reap_retired_coroutines(void) {
  coroutine_t *coro = retired_coroutines;
  retired_coroutines = NULL;
  
  while (coro) {
    coroutine_t *next = coro->next;
    destroy_coroutine_resources(coro);
    CORO_FREE(coro);
    coro = next;
  }
}

void coroutine_clear_await_registration(coroutine_t *coro) {
  if (!coro || !coro->await_registered) return;

  ant_t *js = coro->js;
  ant_value_t promise = coro->awaited_promise;
  coro->await_registered = false;
  coro->awaited_promise = js_mkundef();

  if (js && vtype(promise) == T_PROMISE)
    js_promise_clear_await_coroutine(js, promise, coro);

  coroutine_unhold(coro, CORO_HOLD_AWAIT);
}

void free_coroutine(coroutine_t *coro) {
  if (!coro) return;
  coroutine_clear_await_registration(coro);
  coroutine_release(coro);
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
  coroutine_retain(coro);

  if (!coro->mco) {
    if (!coro->sv_vm || !coro->sv_vm->suspended) {
      coroutine_release(coro);
      return;
    }
    
    coro->is_ready = false;
    coro->sv_vm->suspended_resume_value = coro->result;
    coro->sv_vm->suspended_resume_is_error = coro->is_error;
    coro->sv_vm->suspended_resume_kind = coro->is_error ? SV_RESUME_THROW : SV_RESUME_NEXT;
    coro->sv_vm->suspended_resume_pending = true;
    
    coro->active_parent = js->active_async_coro;
    js->active_async_coro = coro;
    coroutine_hold(coro, CORO_HOLD_ACTIVE);
    ant_value_t result = sv_resume_suspended(coro->sv_vm);
    
    coro->is_settled = false;
    if (coro->sv_vm->suspended) {
      js->active_async_coro = coro->active_parent;
      coro->active_parent = NULL;
      coroutine_unhold(coro, CORO_HOLD_ACTIVE);
      if (generator_resume_pending_request(js, coro, result)) return;
      coroutine_release(coro);
      return;
    }
    
    js->active_async_coro = coro->active_parent;
    coro->active_parent = NULL;
    coroutine_unhold(coro, CORO_HOLD_ACTIVE);
    
    if (generator_resume_pending_request(js, coro, result)) {
      coroutine_release(coro);
      return;
    }
    
    if (is_err(result)) {
      ant_value_t reject_value = js->thrown_exists ? js->thrown_value : result;
      js->thrown_exists = false;
      js->thrown_value = js_mkundef();
      js_reject_promise(js, coro->async_promise, reject_value);
    } else js_resolve_promise(js, coro->async_promise, result);
    
    js_maybe_drain_microtasks_after_async_settle(js);
    coroutine_release(coro);
    
    return;
  }

  if (mco_status(coro->mco) != MCO_SUSPENDED) {
    coroutine_release(coro);
    return;
  }

  coro->is_ready = false;
  mco_result res;
  MCO_RESUME_SAVE(js, coro->mco, res);
  mco_state status = mco_status(coro->mco);

  if (res != MCO_SUCCESS || status == MCO_DEAD)
    remove_coroutine(coro);

  coroutine_release(coro);
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
  coroutine_retain(coro);
  coroutine_clear_await_registration(coro);
  
  ant_value_t args[1] = { value };
  settle_coroutine(coro, args, 1, is_error);
  resume_coroutine_if_suspended(js, coro);
  coroutine_release(coro);
}
