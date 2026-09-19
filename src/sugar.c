#include "internal.h"
#include "sugar.h"
#include "gc/roots.h"

#include "modules/generator.h"
#include "modules/timer.h"
#include "silver/engine.h"

static void destroy_coroutine_resources(coroutine_t *coro) {
  if (!coro) return;
  if (coro->js) gc_forget_coroutine(coro->js, coro);

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
  destroy_coroutine_resources(coro);
  free(coro);
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

static void resume_coroutine_if_suspended(
  ant_t *js, coroutine_t *coro, ant_value_t value, bool is_error
) {
  if (!coro) return;

  if (coro->act && coro->act->frame_count > 0) {
    sv_vm_t *vm = js->vm;
    coroutine_activate(js, coro);

    ant_value_t result;
    if (!sv_activation_install(vm, coro->act)) {
      sv_activation_seal(js, coro->act);
      coro->act->frame_count = 0;
      result = js_mkerr(js, "failed to install async activation");
    } else result = coroutine_resume_and_recapture(js, vm, coro, value, is_error);

    bool suspended_again = coro->act && coro->act->frame_count > 0;
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

ant_value_t resume_coroutine_wrapper(ant_params_t) {
  ant_value_t me = js->current_func;
  ant_value_t coro_val = js_get_slot(me, SLOT_CORO);

  if (vtype(coro_val) != kTypeNumber) return js_mkundef();
  
  coroutine_t *coro = (coroutine_t *)(uintptr_t)tod(coro_val);
  if (!coro) return js_mkundef();

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t value = nargs > 0 ? args[0] : js_mkundef();

  GC_ROOT_PIN(js, value);
  coroutine_retain(coro);
  resume_coroutine_if_suspended(js, coro, value, false);
  
  coroutine_release(coro);
  GC_ROOT_RESTORE(js, root_mark);

  return js_mkundef();
}

ant_value_t reject_coroutine_wrapper(ant_params_t) {
  ant_value_t me = js->current_func;
  ant_value_t coro_val = js_get_slot(me, SLOT_CORO);
  
  if (vtype(coro_val) != kTypeNumber) return js_mkundef();
  
  coroutine_t *coro = (coroutine_t *)(uintptr_t)tod(coro_val);
  if (!coro) return js_mkundef();

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t value = nargs > 0 ? args[0] : js_mkundef();

  GC_ROOT_PIN(js, value);
  coroutine_retain(coro);
  resume_coroutine_if_suspended(js, coro, value, true);
  
  coroutine_release(coro);
  GC_ROOT_RESTORE(js, root_mark);

  return js_mkundef();
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
