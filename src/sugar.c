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

static void clear_await_coro_from_promise_state(ant_promise_state_t *pd, coroutine_t *coro) {
  if (!pd || !coro || pd->handler_count == 0) return;

  if (pd->handler_count == 1) {
    if (pd->inline_handler.await_coro == coro) pd->inline_handler.await_coro = NULL;
    return;
  }

  if (!pd->handlers) return;
  promise_handler_t *h = NULL;
  
  while ((h = (promise_handler_t *)utarray_next(pd->handlers, h))) 
    if (h->await_coro == coro) h->await_coro = NULL;
}

static void clear_await_coro_from_object_list(ant_object_t *head, coroutine_t *coro) {
  for (ant_object_t *obj = head; obj; obj = obj->next)
    if (obj->promise_state) clear_await_coro_from_promise_state(obj->promise_state, coro);
}

static inline bool coroutine_is_queued(coroutine_t *coro) {
  return coro && (
    coro->prev || coro->next ||
    pending_coroutines.head == coro ||
    pending_coroutines.tail == coro
  );
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
  coro->active_parent = NULL;
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

void free_coroutine(coroutine_t *coro) {
  if (!coro || coro->free_pending) return;
  coro->free_pending = true;

  ant_t *js = coro->js;
  if (js) {
    clear_await_coro_from_object_list(js->objects, coro);
    clear_await_coro_from_object_list(js->objects_old, coro);
    clear_await_coro_from_object_list(js->permanent_objects, coro);
    
    if (
      coro->prev || coro->next ||
      pending_coroutines.head == coro ||
      pending_coroutines.tail == coro
    ) remove_coroutine(coro);
    
    if (js->active_async_coro == coro) js->active_async_coro = coro->active_parent;
    coro->active_parent = NULL;
  }

  if (!js || js->vm_exec_depth == 0)
    destroy_coroutine_resources(coro);

  retire_coroutine_storage(coro);
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
    coro->sv_vm->suspended_resume_kind = coro->is_error ? SV_RESUME_THROW : SV_RESUME_NEXT;
    coro->sv_vm->suspended_resume_pending = true;
    
    coro->active_parent = js->active_async_coro;
    js->active_async_coro = coro;
    ant_value_t result = sv_resume_suspended(coro->sv_vm);
    
    coro->is_settled = false;
    if (coro->sv_vm->suspended) {
      js->active_async_coro = coro->active_parent;
      coro->active_parent = NULL;
      if (generator_resume_pending_request(js, coro, result)) return;
      return;
    }
    
    js->active_async_coro = coro->active_parent;
    coro->active_parent = NULL;
    
    if (generator_resume_pending_request(js, coro, result)) return;
    if (coroutine_is_queued(coro)) remove_coroutine(coro);
    
    if (is_err(result)) {
      ant_value_t reject_value = js->thrown_exists ? js->thrown_value : result;
      js->thrown_exists = false;
      js->thrown_value = js_mkundef();
      js_reject_promise(js, coro->async_promise, reject_value);
    } else js_resolve_promise(js, coro->async_promise, result);
    
    js_maybe_drain_microtasks_after_async_settle(js);
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
  coro->awaited_promise = js_mkundef();
  resume_coroutine_if_suspended(js, coro);
}
