#include "internal.h"
#include "sugar.h"

#include "modules/generator.h"
#include "modules/timer.h"
#include "silver/engine.h"

static constexpr uint32_t CORO_FREE_MAX = 256;
static constexpr size_t CORO_POOLED_ACT_MAX = 4096;

coroutine_t *coroutine_alloc(ant_t *js) {
  coroutine_t *coro = js->coro_free;
  if (!coro) return calloc(1, sizeof(coroutine_t));
  
  js->coro_free = coro->active_parent;
  js->coro_free_count--;
  
  sv_activation_t *act = coro->act;
  *coro = (coroutine_t){ .act = act };
  
  return coro;
}

bool coroutine_pending(const coroutine_t *coro) {
  return coro->tail_await || (coro->act && coro->act->frame_count > 0);
}

void coroutine_pool_destroy(ant_t *js) {
  while (js->coro_free) {
    coroutine_t *next = js->coro_free->active_parent;
    free(js->coro_free->act);
    free(js->coro_free);
    js->coro_free = next;
  }
  
  js->coro_free_count = 0;
}

static void destroy_coroutine_resources(coroutine_t *coro, bool keep_act) {
  if (!coro) return;
  if (coro->js) gc_forget_coroutine(coro->js, coro);

  if (coro->args) {
    free(coro->args);
    coro->args = NULL;
  }

  if (coro->act) {
    sv_activation_seal(coro->js, coro->act);
    coro->act->frame_count = 0;
    
    if (!keep_act || coro->act->capacity > CORO_POOLED_ACT_MAX) {
      free(coro->act);
      coro->act = NULL;
    }
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
  bool pooled = js && !js->destroying && js->coro_free_count < CORO_FREE_MAX;
  destroy_coroutine_resources(coro, pooled);
  
  if (!pooled) {
    free(coro->act);
    free(coro);
    return;
  }
  
  coro->active_parent = js->coro_free;
  js->coro_free = coro;
  js->coro_free_count++;
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

bool coroutine_cancel(coroutine_t *coro) {
  if (!coro) return false;

  bool suspended = coro->await_registered ||
    (coro->act && coro->act->frame_count > 0);

  coroutine_clear_await_registration(coro);
  return suspended;
}

static bool coroutine_detach_await_registration(coroutine_t *coro) {
  if (!coro || !coro->await_registered) return false;

  ant_t *js = coro->js;
  ant_value_t promise = coro->awaited_promise;
  bool direct_resume = coro->await_resume_job != NULL;
  
  coro->await_registered = false;
  coro->awaited_promise = js_mkundef();
  coro->await_resume_job = NULL;

  if (!direct_resume && js && vtype(promise) == kTypePromise)
    Ant_Promise_ClearAwaitCoroutine(js, promise, coro);

  bool held = (coro->hold_bits & CORO_HOLD_AWAIT) != 0;
  coro->hold_bits &= (uint8_t)~CORO_HOLD_AWAIT;
  
  return held;
}

void coroutine_clear_await_registration(coroutine_t *coro) {
  if (coroutine_detach_await_registration(coro)) coroutine_release(coro);
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

static ant_value_t coroutine_resume_and_recapture(
  ant_t *js, sv_vm_t *vm, coroutine_t *coro, ant_value_t value, bool is_error
) {
  vm->suspended_resume_value = value;
  vm->suspended_resume_is_error = is_error;
  vm->suspended_resume_kind = is_error ? SV_RESUME_THROW : SV_RESUME_NEXT;
  vm->suspended_resume_pending = true;

  ant_value_t result = sv_resume_suspended(vm);
  if (!vm->suspended || vm->suspended_entry_fp < 0) return result;

  sv_activation_t *act = sv_activation_capture(vm, vm->suspended_entry_fp, coro->act);
  if (act) {
    coro->act = act;
    gc_remember_coroutine(js, coro);
    return result;
  }

  sv_activation_discard(vm, vm->suspended_entry_fp);
  return js_mkerr(js, "out of memory capturing activation");
}

static void settle_tail_await(ant_t *js, coroutine_t *coro, ant_value_t value, bool is_error) {
  coro->tail_await = false;
  gc_forget_coroutine(js, coro);
  
  if (is_error) js_reject_promise(js, coro->async_promise, value);
  else js_resolve_promise(js, coro->async_promise, value);
  
  js_maybe_drain_microtasks_after_async_settle(js);
}

static void resume_coroutine_if_suspended(
  ant_t *js, coroutine_t *coro, ant_value_t value, bool is_error
) {
  if (!coro) return;
  
  if (coro->tail_await) {
    settle_tail_await(js, coro, value, is_error);
    return;
  }

  if (coro->act && coro->act->frame_count > 0) {
    sv_vm_t *vm = js->vm;
    coroutine_activate(js, coro);

    ant_value_t result;
    if (!sv_activation_install(vm, coro->act)) {
      sv_activation_seal(js, coro->act);
      coro->act->frame_count = 0;
      result = js_mkerr(js, "failed to install async activation");
    } else result = coroutine_resume_and_recapture(js, vm, coro, value, is_error);

    bool suspended_again = coroutine_pending(coro);
    coroutine_deactivate(js, coro);

    if (suspended_again) {
      generator_resume_pending_request(js, coro, result);
      return;
    }

    if (generator_resume_pending_request(js, coro, result)) return;

    if (is_err(result)) {
      ant_value_t reject_value = js_take_thrown(js, result);
      js_reject_promise(js, coro->async_promise, reject_value);
    } else js_resolve_promise(js, coro->async_promise, result);
    
    js_maybe_drain_microtasks_after_async_settle(js);
    
    return;
  }
}

void Ant_Coroutine_ResumeAwaitJob(ant_t *js, coroutine_t *coro, ant_value_t value) {
  coro->await_registered = false;
  coro->awaited_promise = js_mkundef();
  coro->await_resume_job = NULL;
  resume_coroutine_if_suspended(js, coro, value, false);
}

void Ant_Coroutine_SettleAndResume(ant_t *js, coroutine_t *coro, ant_value_t value, bool is_error) {
  if (!coro) return;
  if (!coroutine_detach_await_registration(coro)) coroutine_retain(coro);
  resume_coroutine_if_suspended(js, coro, value, is_error);
  coroutine_release(coro);
}
