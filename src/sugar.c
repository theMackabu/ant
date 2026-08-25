#include "internal.h"
#include "sugar.h"

#include "modules/generator.h"
#include "modules/timer.h"
#include "silver/engine.h"

struct js_async_entry { coroutine_t *coro; };

static void retire_coroutine_storage(coroutine_t *coro) {
  if (!coro || !coro->js) return;
  coro->retired_next = coro->js->retired_coroutines;
  coro->js->retired_coroutines = coro;
}

static void destroy_coroutine_resources(coroutine_t *coro) {
  if (!coro) return;

  if (coro->args) {
    free(coro->args);
    coro->args = NULL;
  }

  if (coro->act) {
    sv_activation_seal(coro->js, coro->act);
    free(coro->act);
    coro->act = NULL;
  }

  if (coro->module_eval_ctx) {
    free(coro->module_eval_ctx);
    coro->module_eval_ctx = NULL;
  }

  coro->js = NULL;
  coro->active_parent = NULL;
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
    free(coro);
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

js_async_entry_t *js_eval_async_entry_create(coroutine_t *coro) {
  if (!coro) return NULL;

  js_async_entry_t *entry = malloc(sizeof(*entry));
  if (!entry) return NULL;

  entry->coro = coro;
  coroutine_retain(coro);
  
  return entry;
}

bool js_eval_async_entry_cancel(js_async_entry_t *entry) {
  if (!entry || !entry->coro) return false;

  coroutine_t *coro = entry->coro;
  entry->coro = NULL;
  
  bool suspended = coro->await_registered ||
    (coro->act && coro->act->frame_count > 0);
    
  coroutine_clear_await_registration(coro);
  coroutine_release(coro);
  
  return suspended;
}

void js_eval_async_entry_release(js_async_entry_t *entry) {
  if (!entry) return;
  if (entry->coro) coroutine_release(entry->coro);
  free(entry);
}

void reap_retired_coroutines(ant_t *js) {
  if (!js) return;

  coroutine_t *coro = js->retired_coroutines;
  js->retired_coroutines = NULL;

  while (coro) {
    coroutine_t *next = coro->retired_next;
    destroy_coroutine_resources(coro);
    free(coro);
    coro = next;
  }
}

void coroutine_clear_await_registration(coroutine_t *coro) {
  if (!coro || !coro->await_registered) return;

  ant_t *js = coro->js;
  ant_value_t promise = coro->awaited_promise;
  coro->await_registered = false;
  coro->awaited_promise = js_mkundef();

  if (js && vtype(promise) == kTypePromise)
    js_promise_clear_await_coroutine(js, promise, coro);

  coroutine_unhold(coro, CORO_HOLD_AWAIT);
}

void free_coroutine(coroutine_t *coro) {
  if (!coro) return;
  coroutine_clear_await_registration(coro);
  coroutine_release(coro);
}

static void coroutine_activate(ant_t *js, coroutine_t *coro) {
  if (!js || !coro) return;
  coro->active_parent = js->active_async_coro;
  coro->active_prev = NULL;
  if (js->active_async_coro) js->active_async_coro->active_prev = coro;
  js->active_async_coro = coro;
  if (coro->module_eval_ctx) js_module_eval_ctx_push(js, coro->module_eval_ctx);
  coroutine_hold(coro, CORO_HOLD_ACTIVE);
}

static void coroutine_deactivate(ant_t *js, coroutine_t *coro) {
  if (!js || !coro) return;
  if (coro->module_eval_ctx) js_module_eval_ctx_pop(js, coro->module_eval_ctx);
  if (coro->active_prev) coro->active_prev->active_parent = coro->active_parent;
  else if (js->active_async_coro == coro) js->active_async_coro = coro->active_parent;
  if (coro->active_parent) coro->active_parent->active_prev = coro->active_prev;
  coro->active_parent = NULL;
  coro->active_prev = NULL;
  coroutine_unhold(coro, CORO_HOLD_ACTIVE);
}

static inline void settle_coroutine(coroutine_t *coro, ant_value_t *args, int nargs, bool is_error) {
  coro->result = nargs > 0 ? args[0] : js_mkundef();
  coro->is_error = is_error;
}

static ant_value_t coroutine_resume_and_recapture(ant_t *js, sv_vm_t *vm, coroutine_t *coro) {
  vm->suspended_resume_value = coro->result;
  vm->suspended_resume_is_error = coro->is_error;
  vm->suspended_resume_kind = coro->is_error ? SV_RESUME_THROW : SV_RESUME_NEXT;
  vm->suspended_resume_pending = true;

  ant_value_t result = sv_resume_suspended(vm);
  if (!vm->suspended || vm->suspended_entry_fp < 0) return result;

  sv_activation_t *act = sv_activation_capture(vm, vm->suspended_entry_fp, coro->act);
  if (act) {
    coro->act = act;
    return result;
  }

  sv_activation_discard(vm, vm->suspended_entry_fp);
  return js_mkerr(js, "out of memory capturing activation");
}

static void resume_coroutine_if_suspended(ant_t *js, coroutine_t *coro) {
  if (!coro) return;
  coroutine_retain(coro);

  if (coro->act && coro->act->frame_count > 0) {
    sv_vm_t *vm = sv_vm_get_active(js);
    coroutine_activate(js, coro);

    ant_value_t result;
    if (!sv_activation_install(vm, coro->act)) {
      sv_activation_seal(js, coro->act);
      coro->act->frame_count = 0;
      result = js_mkerr(js, "failed to install async activation");
    } else result = coroutine_resume_and_recapture(js, vm, coro);

    bool suspended_again = coro->act && coro->act->frame_count > 0;
    coroutine_deactivate(js, coro);

    if (suspended_again) {
      generator_resume_pending_request(js, coro, result);
      coroutine_release(coro);
      return;
    }

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

  coroutine_release(coro);
}

ant_value_t resume_coroutine_wrapper(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t me = js->current_func;
  ant_value_t coro_val = js_get_slot(me, SLOT_CORO);
  if (vtype(coro_val) != kTypeNumber) return js_mkundef();
  
  coroutine_t *coro = (coroutine_t *)(uintptr_t)tod(coro_val);
  if (!coro) return js_mkundef();

  settle_coroutine(coro, args, nargs, false);
  resume_coroutine_if_suspended(js, coro);

  return js_mkundef();
}

ant_value_t reject_coroutine_wrapper(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t me = js->current_func;
  ant_value_t coro_val = js_get_slot(me, SLOT_CORO);
  
  if (vtype(coro_val) != kTypeNumber) return js_mkundef();
  
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
