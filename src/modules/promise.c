#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "cfunc.h"
#include "errors.h"
#include "internal.h"
#include "sugar.h"
#include "gc/roots.h"
#include "silver/call.h"
#include "silver/engine.h"
#include "silver/glue.h"
#include "modules/globals.h"
#include "modules/timer.h"
#include "modules/promise.h"
#include "protectors.h"

static const UT_icd promise_handler_icd = {
  .sz = sizeof(promise_handler_t),
  .init = NULL,
  .copy = NULL,
  .dtor = NULL,
};

static inline uint32_t promise_handler_count(const ant_promise_state_t *pd) {
  if (!pd) return 0;
  if (pd->has_inline_handler) return 1;
  return pd->handlers ? utarray_len(pd->handlers) : 0;
}

static inline bool promise_has_handlers(const ant_promise_state_t *pd) {
  return promise_handler_count(pd) != 0;
}

static bool promise_handler_append(ant_promise_state_t *pd, const promise_handler_t *handler) {
  if (!pd || !handler) return false;

  uint32_t count = promise_handler_count(pd);
  if (count == 0) {
    pd->inline_handler = *handler;
    pd->has_inline_handler = true;
    return true;
  }

  if (!pd->handlers) utarray_new(pd->handlers, &promise_handler_icd);
  if (!pd->handlers) return false;
  
  if (count == 1) {
    utarray_push_back(pd->handlers, &pd->inline_handler);
    pd->inline_handler = (promise_handler_t){ 0 };
    pd->has_inline_handler = false;
  }
  
  utarray_push_back(pd->handlers, handler);
  return true;
}

static inline promise_handler_t *promise_handler_at(ant_promise_state_t *pd, uint32_t index) {
  if (!pd || index >= promise_handler_count(pd)) return NULL;
  if (pd->has_inline_handler) return &pd->inline_handler;
  return (promise_handler_t *)utarray_eltptr(pd->handlers, (unsigned int)index);
}

static inline void promise_handlers_clear(ant_promise_state_t *pd) {
  if (!pd) return;
  pd->has_inline_handler = false;
  pd->inline_handler = (promise_handler_t){ 0 };
  if (pd->handlers) utarray_clear(pd->handlers);
}

static void promise_handlers_discard_processed_prefix(
  ant_promise_state_t *pd, uint32_t count
) {
  uint32_t total = promise_handler_count(pd);
  ANT_ASSERT(count <= total, "invalid Promise handler prefix");
  
  if (count == total) {
    promise_handlers_clear(pd);
    return;
  }

  utarray_erase(pd->handlers, 0, count);
  
  if (total - count == 1) {
    pd->inline_handler = *(promise_handler_t *)utarray_front(pd->handlers);
    pd->has_inline_handler = true;
    utarray_clear(pd->handlers);
  }
}

ant_promise_state_t *js_promise_data(ant_t *js, ant_value_t promise, bool create) {
  if (vtype(promise) != kTypePromise) return NULL;
  ant_object_t *obj = js_obj_ptr(js_as_obj(promise));
  if (!obj) return NULL;
  
  ant_promise_state_t *existing = ant_object_promise_state(obj);
  if (existing) return existing;
  if (!create) return NULL;

  ant_promise_state_t *entry = gc_promise_state_alloc(js);
  if (!entry) return NULL;
  
  entry->promise_id = ++js->last_promise_id;
  entry->trigger_parent = js_mkundef();
  entry->inline_handler = (promise_handler_t){ 0 };
  entry->handlers = NULL;
  entry->has_inline_handler = false;
  entry->state = 0;
  entry->value = js_mkundef();
  entry->trigger_queued = false;
  entry->has_rejection_handler = false;
  entry->processing = false;
  entry->unhandled_reported = false;
  ant_object_make_promise(obj, entry);
  
  return entry;
}

js_promise_settlement_t js_promise_get_settlement(
  ant_t *js, ant_value_t promise, ant_value_t *value_out
) {
  if (value_out) *value_out = js_mkundef();
  ant_promise_state_t *pd = js_promise_data(js, promise, false);

  if (!pd) return JS_PROMISE_INVALID;
  if (pd->state == 0) return JS_PROMISE_PENDING;
  if (value_out) *value_out = pd->value;

  return pd->state == 1 ? JS_PROMISE_FULFILLED : JS_PROMISE_REJECTED;
}

uint32_t js_promise_id(ant_t *js, ant_value_t p) {
  ant_promise_state_t *pd = js_promise_data(js, p, false);
  return pd ? pd->promise_id : 0;
}

bool js_mark_promise_trigger_queued(ant_t *js, ant_value_t promise) {
  ant_promise_state_t *pd = js_promise_data(js, promise, false);
  if (!pd) return true;
  if (pd->trigger_queued) return false;
  pd->trigger_queued = true;
  return true;
}

void js_mark_promise_trigger_dequeued(ant_t *js, ant_value_t promise) {
  ant_promise_state_t *pd = js_promise_data(js, promise, false);
  if (!pd) return;
  pd->trigger_queued = false;
}

ant_value_t js_mkpromise(ant_t *js) {
  ant_value_t obj = mkobj(js, 0);

  if (is_err(obj)) return obj;
  if (!js_promise_data(js, mkval(kTypePromise, vdata(obj)), true))
    return Ant_Error_CreateOutOfMemory(js);

  ant_value_t promise_proto = js->sym.promise_proto;
  if (is_object_type(promise_proto)) js_set_proto_init(obj, promise_proto);
  
  return mkval(kTypePromise, vdata(obj));
}

ant_value_t js_mkgenerator(ant_t *js) {
  ant_value_t obj = mkobj(js, 0);
  if (is_err(obj)) return obj;

  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return js_mkerr(js, "invalid generator object");
  ptr->type_tag = kTypeGenerator;

  ant_value_t gen = mkval(kTypeGenerator, vdata(obj));
  if (is_object_type(js->sym.generator_proto))
    js_set_proto_init(gen, js->sym.generator_proto);

  return gen;
}

void Ant_Promise_Observe(ant_t *js, ant_value_t promise, ant_value_t on_fulfilled, ant_value_t on_rejected) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, promise);
  GC_ROOT_PIN(js, on_fulfilled);
  GC_ROOT_PIN(js, on_rejected);

  ant_value_t result = is_err(promise) ? promise : js_promise_then(js, promise, on_fulfilled, on_rejected);
  if (is_err(result)) {
    ant_value_t reason = Ant_Error_ConsumeMarker(js, result);
    queue_microtask_with_args(js, on_rejected, &reason, 1);
  } else {
    ant_promise_state_t *pd = js_promise_data(js, result, false);
    if (pd) pd->has_rejection_handler = true;
  }

  GC_ROOT_RESTORE(js, root_mark);
}

void js_mark_promise_rejection_handled_chain(ant_t *js, ant_value_t promise) {
  ant_value_t current = promise;

  while (vtype(current) == kTypePromise) {
    ant_promise_state_t *pd = js_promise_data(js, current, false);
    if (!pd) break;
    
    if (pd->unhandled_reported) js_fire_rejection_handled(js, current, pd->value);
    pd->has_rejection_handler = true;
    pd->unhandled_reported = false;
    current = pd->trigger_parent;
  }
}

static inline ant_value_t js_get_thenable_then(ant_t *js, ant_value_t value) {
  if (!is_object_type(value) && vtype(value) != kTypeBuiltin) return js_mkundef();

  if (
    vtype(value) == kTypePromise &&
    ant_protector_intact(js, ANT_PROTECTOR_PROMISE_THEN) &&
    js_get_slot(value, SLOT_PROTO) == js->sym.promise_proto
  ) return js->sym.promise_then;

  return js_get_then_property(js, value);
}

ant_value_t js_promise_assimilate_awaitable(ant_t *js, ant_value_t value) {
  if (
    vtype(value) == kTypePromise &&
    ant_protector_intact(js, ANT_PROTECTOR_PROMISE_CONSTRUCTOR) &&
    js_get_slot(value, SLOT_PROTO) == js->sym.promise_proto
  ) return value;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, value);

  bool is_promise = vtype(value) == kTypePromise;
  ant_value_t ctor = is_promise
    ? js_getprop_fallback(js, value, "constructor")
    : js_mkundef();

  if (is_err(ctor)) {
    GC_ROOT_RESTORE(js, root_mark);
    return ctor;
  }

  if (is_promise && strict_eq_values(js, ctor, js->sym.promise_ctor)) {
    GC_ROOT_RESTORE(js, root_mark);
    return value;
  }

  ant_value_t promise = js_mkpromise(js);
  GC_ROOT_PIN(js, promise);
  if (is_err(promise)) {
    GC_ROOT_RESTORE(js, root_mark);
    return promise;
  }

  js_resolve_promise(js, promise, value);
  GC_ROOT_RESTORE(js, root_mark);

  return promise;
}

void Ant_Promise_ClearAwaitCoroutine(ant_t *js, ant_value_t promise, coroutine_t *coro) {
  if (vtype(promise) != kTypePromise || !coro) return;

  ant_promise_state_t *pd = js_promise_data(js, promise, false);
  if (!pd || promise_handler_count(pd) == 0) return;

  if (pd->has_inline_handler) {
    if (pd->inline_handler.await_coro == coro) pd->inline_handler.await_coro = NULL;
    return;
  }

  if (!pd->handlers) return;
  promise_handler_t *h = NULL;
  
  while ((h = (promise_handler_t *)utarray_next(pd->handlers, h)))
    if (h->await_coro == coro) h->await_coro = NULL;
}

ant_value_t js_promise_tail_await(ant_t *js, ant_value_t value) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, value);
  
  bool primitive = !is_object_type(value) && vtype(value) != kTypeBuiltin;
  if (!primitive) {
    value = js_promise_assimilate_awaitable(js, value);
    if (is_err(value)) {
      GC_ROOT_RESTORE(js, root_mark);
      return value;
    }
  }

  ant_value_t result = js_mkpromise(js);
  GC_ROOT_PIN(js, result);
  if (is_err(result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }

  if (primitive) {
    bool queued = queue_promise_resolve_job(js, result, value);
    GC_ROOT_RESTORE(js, root_mark);
    return queued ? result : js_mkerr(js, "out of memory queuing await");
  }

  ant_promise_state_t *pd = js_promise_data(js, value, false);
  bool direct = 
    pd->state == 1 && promise_handler_count(pd) == 0 &&
    !pd->trigger_queued && !pd->processing &&
    pd->trigger_parent == js_mkundef();

  promise_handler_t h = { js_mkundef(), js_mkundef(), result, NULL };
  bool queued = direct
    ? queue_promise_resolve_job(js, result, pd->value)
    : promise_handler_append(pd, &h);
  
  if (!queued) {
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkerr(js, "out of memory queuing await");
  }

  if (!direct) gc_write_barrier(js, js_obj_ptr(js_as_obj(value)), result);
  js_mark_promise_rejection_handled_chain(js, value);
  
  if (pd->state == 0) gc_root_pending_promise(js, js_obj_ptr(js_as_obj(value)));
  else if (!direct) queue_promise_trigger(js, value);

  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

js_await_result_t js_promise_await_coroutine(ant_t *js, ant_value_t promise, coroutine_t *coro, bool deferred) {
  js_await_result_t result = {
    .state = JS_AWAIT_PENDING,
    .value = js_mkundef(),
  };

  if (vtype(promise) != kTypePromise || !coro) return result;
  ant_promise_state_t *pd = js_promise_data(js, promise, false);
  
  if (!pd) {
    result.state = JS_AWAIT_ERROR;
    result.value = js_mkerr(js, "invalid promise state");
    return result;
  }

  bool direct_resume = 
    pd->state == 1 && promise_handler_count(pd) == 0 &&
    !pd->trigger_queued && !pd->processing && 
    pd->trigger_parent == js_mkundef();
    
  promise_handler_t h = { 
    js_mkundef(), js_mkundef(), 
    deferred ? PROMISE_AWAIT_DEFER : js_mkundef(), coro
  };
  
  bool queued = !direct_resume
    ? promise_handler_append(pd, &h) : deferred
    ? queue_await_defer_job(coro, pd->value)
    : queue_await_resume_job(coro, pd->value);
  
  if (!queued) {
    result.state = JS_AWAIT_ERROR;
    result.value = Ant_Error_CreateOutOfMemory(js);
    return result;
  }

  coro->awaited_promise = promise;
  coro->await_registered = true;
  if (!direct_resume) coroutine_hold(coro, CORO_HOLD_AWAIT);

  js_mark_promise_rejection_handled_chain(js, promise);
  
  if (pd->state == 0) gc_root_pending_promise(js, js_obj_ptr(js_as_obj(promise)));
  else if (!direct_resume) queue_promise_trigger(js, promise);

  return result;
}

static void promise_all_settle_element(
  ant_t *js, ant_value_t tracker, ant_value_t index, ant_value_t value, bool fulfilled
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, tracker);
  GC_ROOT_PIN(js, value);
  
  ant_value_t result = js_get_slot(tracker, SLOT_DATA);
  if (!fulfilled) {
    js_reject_promise(js, result, value);
    GC_ROOT_RESTORE(js, root_mark);
    return;
  }

  ant_value_t results = js_get_slot(tracker, SLOT_ENTRIES);
  js_arr_set_index(js, results, (ant_offset_t)tod(index), value);
  
  int remaining = (int)tod(js_get_slot(tracker, SLOT_MAP)) - 1;
  js_set_slot(tracker, SLOT_MAP, tov((double)remaining));
  if (remaining == 0) js_resolve_promise(js, result, mkval(kTypeArray, vdata(results)));
  
  GC_ROOT_RESTORE(js, root_mark);
}

void js_process_promise_handlers(ant_t *js, ant_value_t promise) {
  ant_object_t *pobj = js_obj_ptr(promise);
  ant_promise_state_t *pd = js_promise_data(js, promise, false);
  if (!pd) return;
  
  int state = pd->state;
  ant_value_t val = pd->value;
  
  uint32_t len = promise_handler_count(pd);
  if (len == 0) return;

  bool inline_batch = len == 1;
  promise_handler_t inline_handler = { 0 };
  
  if (inline_batch) {
    inline_handler = pd->inline_handler;
    promise_handlers_clear(pd);
  }
  
  gc_root_pending_promise(js, pobj);
  pd->processing = true;
  
  for (uint32_t i = 0; i < len; i++) {
    promise_handler_t *h = inline_batch
      ? &inline_handler
      : promise_handler_at(pd, i);
    
    if (!h) continue;
    promise_handler_t handler = *h;
    
    if (handler.await_coro) {
      coroutine_t *await_coro = handler.await_coro;
      h->await_coro = NULL;
      
      if (handler.nextPromise == PROMISE_AWAIT_DEFER)
        Ant_Coroutine_DeferResume(js, await_coro, val, state != 1);
      else Ant_Coroutine_SettleAndResume(js, await_coro, val, state != 1);
      
      continue;
    }
    
    if (handler.nextPromise == PROMISE_ALL_ELEMENT) {
      promise_all_settle_element(js, handler.onFulfilled, handler.onRejected, val, state == 1);
      continue;
    }
    
    GC_ROOT_SAVE(handler_root_mark, js);
    GC_ROOT_PIN(js, handler.onFulfilled);
    GC_ROOT_PIN(js, handler.onRejected);
    GC_ROOT_PIN(js, handler.nextPromise);

    ant_value_t callback = (state == 1) ? handler.onFulfilled : handler.onRejected;
    if (!is_callable(callback)) {
      if (state == 1) js_resolve_promise(js, handler.nextPromise, val);
      else js_reject_promise(js, handler.nextPromise, val);
      GC_ROOT_RESTORE(js, handler_root_mark);
      continue;
    }
    
    ant_value_t res = js_mkundef();
    if (vtype(callback) == kTypeBuiltin) {
      res = sv_invoke_builtin(js, callback, &val, 1, js_mkundef());
    } else {
      ant_value_t call_args[] = { val };
      res = sv_vm_call(js->vm, js, callback, js_mkundef(), call_args, 1, NULL, js_mkundef());
    }
    
    if (!is_err(res)) {
      GC_ROOT_PIN(js, res);
      js_resolve_promise(js, handler.nextPromise, res);
      GC_ROOT_RESTORE(js, handler_root_mark);
      continue;
    }
    
    js_reject_promise(js, handler.nextPromise, res);
    GC_ROOT_RESTORE(js, handler_root_mark);
  }

  pd->processing = false;
  if (!inline_batch) promise_handlers_discard_processed_prefix(pd, len);
  gc_unroot_pending_promise(js, js_obj_ptr(promise));
}

void js_resolve_promise(ant_t *js, ant_value_t p, ant_value_t val) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, p);
  GC_ROOT_PIN(js, val);

  ant_promise_state_t *pd = js_promise_data(js, p, false);
  if (!pd || pd->state != 0) {
    GC_ROOT_RESTORE(js, root_mark);
    return;
  }
  
  if (vtype(val) == kTypePromise && vdata(js_as_obj(val)) == vdata(js_as_obj(p))) {
    ant_value_t err = js_mkerr(js, "TypeError: Chaining cycle");
    GC_ROOT_PIN(js, err);
    js_reject_promise(js, p, err);
    GC_ROOT_RESTORE(js, root_mark);
    return;
  }

  if (is_object_type(val) || vtype(val) == kTypeBuiltin) {
    ant_value_t then_prop = js_get_thenable_then(js, val);
    GC_ROOT_PIN(js, then_prop);

    if (is_err(then_prop)) {
      js_reject_promise(js, p, then_prop);
      GC_ROOT_RESTORE(js, root_mark);
      return;
    }

    if (is_callable(then_prop)) {
      if (!queue_promise_thenable_job(js, p, val, then_prop)) {
        ant_value_t err = js_mkerr(js, "out of memory queuing Promise job");
        GC_ROOT_PIN(js, err);
        js_reject_promise(js, p, err);
      }
      GC_ROOT_RESTORE(js, root_mark);
      
      return;
    }
  }

  pd->state = 1;
  pd->value = val;
  
  gc_write_barrier(js, js_obj_ptr(js_as_obj(p)), val);
  if (promise_has_handlers(pd)) queue_promise_trigger(js, p);
  GC_ROOT_RESTORE(js, root_mark);
}

void js_reject_promise(ant_t *js, ant_value_t p, ant_value_t val) {
  if (is_err(val)) {
    GC_ROOT_SAVE(root_mark, js);
    GC_ROOT_PIN(js, p);
    val = Ant_Error_ConsumeMarker(js, val);
    GC_ROOT_RESTORE(js, root_mark);
  }

  ant_promise_state_t *pd = js_promise_data(js, p, false);
  if (!pd || pd->state != 0) return;

  pd->state = 2;
  pd->value = val;
  gc_write_barrier(js, js_obj_ptr(js_as_obj(p)), val);
  pd->unhandled_reported = false;

  if (js->pending_rejections.len >= js->pending_rejections.cap) {
    size_t new_cap = js->pending_rejections.cap ? js->pending_rejections.cap * 2 : 16;
    ant_value_t *ns = realloc(js->pending_rejections.items, new_cap * sizeof(*ns));
    if (ns) { js->pending_rejections.items = ns; js->pending_rejections.cap = new_cap; }
  }
  if (js->pending_rejections.len < js->pending_rejections.cap)
    js->pending_rejections.items[js->pending_rejections.len++] = p;

  queue_promise_trigger(js, p);
}

static ant_value_t builtin_resolve_internal(ant_params_t) {
  ant_value_t resolve_fn = js->current_func;
  if (js_get_slot(resolve_fn, SLOT_SETTLED) == js_true) return js_mkundef();

  js_set_slot(resolve_fn, SLOT_SETTLED, js_true);
  ant_value_t p = js_get_slot(resolve_fn, SLOT_DATA);
  js_resolve_promise(js, p, nargs > 0 ? args[0] : js_mkundef());
  
  return js_mkundef();
}

static ant_value_t builtin_reject_internal(ant_params_t) {
  ant_value_t resolve_fn = js_get_slot(js->current_func, SLOT_DATA);
  if (js_get_slot(resolve_fn, SLOT_SETTLED) == js_true) return js_mkundef();

  js_set_slot(resolve_fn, SLOT_SETTLED, js_true);
  ant_value_t p = js_get_slot(resolve_fn, SLOT_DATA);
  js_reject_promise(js, p, nargs > 0 ? args[0] : js_mkundef());
  
  return js_mkundef();
}

static ant_value_t make_promise_resolving_functions(
  ant_t *js,
  ant_value_t promise,
  ant_value_t *resolve_out,
  ant_value_t *reject_out
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, promise);

  *resolve_out = js_mkundef();
  *reject_out = js_mkundef();

  ant_value_t resolve_fn = js_make_data_cfunc(js, promise, builtin_resolve_internal);
  GC_ROOT_PIN(js, resolve_fn);
  if (is_err(resolve_fn)) {
    GC_ROOT_RESTORE(js, root_mark);
    return resolve_fn;
  }
  js_set_slot(resolve_fn, SLOT_SETTLED, js_false);

  ant_value_t reject_fn = js_make_data_cfunc(js, resolve_fn, builtin_reject_internal);
  GC_ROOT_PIN(js, reject_fn);
  if (is_err(reject_fn)) {
    GC_ROOT_RESTORE(js, root_mark);
    return reject_fn;
  }

  *resolve_out = resolve_fn;
  *reject_out = reject_fn;
  GC_ROOT_RESTORE(js, root_mark);
  
  return js_mkundef();
}

void js_process_promise_thenable_job(
  ant_t *js, ant_value_t promise, ant_value_t thenable, ant_value_t then_fn
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, promise);
  GC_ROOT_PIN(js, thenable);
  GC_ROOT_PIN(js, then_fn);

  if (
    vtype(thenable) == kTypePromise &&
    then_fn == js->sym.promise_then &&
    ant_protector_intact(js, ANT_PROTECTOR_PROMISE_SPECIES) &&
    js_get_slot(thenable, SLOT_PROTO) == js->sym.promise_proto
  ) {
    ant_promise_state_t *source = js_promise_data(js, thenable, false);
    promise_handler_t handler = {
      js_mkundef(), js_mkundef(), 
      promise, NULL
    };

    if (!promise_handler_append(source, &handler)) {
      ant_value_t err = js_mkerr(js, "out of memory adopting Promise");
      GC_ROOT_PIN(js, err);
      js_reject_promise(js, promise, err);
      GC_ROOT_RESTORE(js, root_mark);
      return;
    }

    gc_write_barrier(js, js_obj_ptr(js_as_obj(thenable)), promise);
    js_mark_promise_rejection_handled_chain(js, thenable);
    if (source->state == 0)
      gc_root_pending_promise(js, js_obj_ptr(js_as_obj(thenable)));
    else queue_promise_trigger(js, thenable);

    GC_ROOT_RESTORE(js, root_mark);
    return;
  }

  ant_value_t resolve_fn = js_mkundef();
  ant_value_t reject_fn = js_mkundef();
  ant_value_t pair_result = make_promise_resolving_functions(
    js, promise, 
    &resolve_fn, &reject_fn
  );
  
  GC_ROOT_PIN(js, pair_result);
  GC_ROOT_PIN(js, resolve_fn);
  GC_ROOT_PIN(js, reject_fn);
  
  if (is_err(pair_result)) {
    js_reject_promise(js, promise, pair_result);
    GC_ROOT_RESTORE(js, root_mark);
    return;
  }

  ant_value_t then_args[] = { resolve_fn, reject_fn };
  ant_value_t result = sv_vm_call(
    js->vm, js, then_fn, thenable,
    then_args, 2, NULL, js_mkundef()
  );
  
  if (is_err(result) || Ant_Exception_Pending(js)) {
    ant_value_t reject_val = js_take_thrown(js, result);
    GC_ROOT_PIN(js, reject_val);
    ant_value_t reject_args[] = { reject_val };
    sv_vm_call(
      js->vm, js, reject_fn, js_mkundef(), 
      reject_args, 1, NULL, js_mkundef()
    );
  }

  GC_ROOT_RESTORE(js, root_mark);
}

static ant_value_t builtin_Promise(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Promise constructor cannot be invoked without 'new'");
  }
  
  if (nargs == 0 || !is_callable(args[0])) {
    const char *val_str = nargs == 0 ? "undefined" : js_str(js, args[0]);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Promise resolver %s is not a function", val_str);
  }

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t executor = args[0];
  GC_ROOT_PIN(js, executor);

  ant_value_t p = js_mkpromise(js);
  if (is_err(p)) {
    GC_ROOT_RESTORE(js, root_mark);
    return p;
  }
  GC_ROOT_PIN(js, p);

  ant_value_t new_target = call_new_target;
  ant_value_t p_obj = js_as_obj(p);

  ant_value_t promise_proto = js->sym.promise_proto;
  ant_value_t instance_proto = js_instance_proto_from_new_target(js, promise_proto, call_new_target);
  GC_ROOT_PIN(js, instance_proto);

  if (vtype(new_target) == kTypeFunction || vtype(new_target) == kTypeBuiltin) js_set_slot(p_obj, SLOT_CTOR, new_target);
  if (is_object_type(instance_proto)) js_set_proto_init(p_obj, instance_proto);

  ant_value_t res_fn = js_mkundef();
  ant_value_t rej_fn = js_mkundef();
  ant_value_t pair_result = make_promise_resolving_functions(js, p, &res_fn, &rej_fn);
  
  GC_ROOT_PIN(js, pair_result);
  GC_ROOT_PIN(js, res_fn);
  GC_ROOT_PIN(js, rej_fn);
  
  if (is_err(pair_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return pair_result;
  }

  ant_value_t exec_args[] = { res_fn, rej_fn };
  ant_value_t exec_result = sv_vm_call(js->vm, js, executor, js_mkundef(), exec_args, 2, NULL, js_mkundef());
  
  if (js_terminating_error(js, exec_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return exec_result;
  }
  
  if (is_err(exec_result) || Ant_Exception_Pending(js)) {
    ant_value_t reject_val = js_take_thrown(js, exec_result);
    ant_value_t reject_args[] = { reject_val };
    sv_vm_call(js->vm, js, rej_fn, js_mkundef(), reject_args, 1, NULL, js_mkundef());
  }

  GC_ROOT_RESTORE(js, root_mark);
  return p;
}

static inline bool is_same_heap_value(ant_value_t a, ant_value_t b) {
  return vtype(a) == vtype(b) && vdata(a) == vdata(b);
}

static inline bool is_constructor_value(ant_value_t value) {
  return vtype(value) == kTypeFunction || vtype(value) == kTypeBuiltin;
}

static ant_value_t builtin_promise_capability_executor(ant_params_t) {
  ant_value_t executor = js->current_func;
  if (js_get_slot(executor, SLOT_SETTLED) == js_true)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Promise capability executor called twice");

  js_set_slot(executor, SLOT_SETTLED, js_true);
  js_set_slot_wb(js, executor, SLOT_DATA, nargs > 0 ? args[0] : js_mkundef());
  js_set_slot_wb(js, executor, SLOT_AUX, nargs > 1 ? args[1] : js_mkundef());
  
  return js_mkundef();
}

static ant_value_t new_promise_capability(
  ant_t *js,
  ant_value_t ctor,
  ant_value_t *promise_out,
  ant_value_t *resolve_out,
  ant_value_t *reject_out
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, ctor);

  *promise_out = js_mkundef();
  *resolve_out = js_mkundef();
  *reject_out = js_mkundef();

  ant_value_t executor = js_make_data_cfunc(
    js, js_mkundef(), 
    builtin_promise_capability_executor
  );
  
  GC_ROOT_PIN(js, executor);
  if (is_err(executor)) {
    GC_ROOT_RESTORE(js, root_mark);
    return executor;
  }
  
  js_set_slot(executor, SLOT_AUX, js_mkundef());
  js_set_slot(executor, SLOT_SETTLED, js_false);

  
  ant_value_t ctor_args[] = { executor };
  ant_value_t promise = jit_helper_new(js->vm, js, ctor, ctor, ctor_args, 1);
  if (is_err(promise)) {
    GC_ROOT_RESTORE(js, root_mark);
    return promise;
  }

  ant_value_t resolve = js_get_slot(executor, SLOT_DATA);
  ant_value_t reject = js_get_slot(executor, SLOT_AUX);
  
  if (!is_callable(resolve) || !is_callable(reject)) {
    ant_value_t err = js_mkerr_typed(
      js, JS_ERR_TYPE, 
      "Promise constructor did not provide callable resolve and reject functions"
    );
    GC_ROOT_RESTORE(js, root_mark);
    return err;
  }

  *promise_out = promise;
  *resolve_out = resolve;
  *reject_out = reject;
  GC_ROOT_RESTORE(js, root_mark);
  
  return js_mkundef();
}

static ant_value_t promise_call_one(
  ant_t *js, ant_value_t fn, ant_value_t this_val, ant_value_t value
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, fn);
  GC_ROOT_PIN(js, this_val);
  GC_ROOT_PIN(js, value);

  ant_value_t args[] = { value };
  ant_value_t result = sv_vm_call(
    js->vm, js, fn, 
    this_val, args, 1, NULL, js_mkundef()
  );
  
  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static ant_value_t promise_resolve_with_ctor(ant_t *js, ant_value_t ctor, ant_value_t val) {
  if (strict_eq_values(js, ctor, js->sym.promise_ctor)) return js_promise_assimilate_awaitable(js, val);
  if (!js_is_constructor(ctor)) return js_mkerr_typed(js, JS_ERR_TYPE, "Promise.resolve receiver is not a constructor");

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, ctor);
  GC_ROOT_PIN(js, val);

  bool val_is_promise = vtype(val) == kTypePromise;
  ant_value_t val_ctor = val_is_promise
    ? js_getprop_fallback(js, val, "constructor")
    : js_mkundef();
  
  if (is_err(val_ctor)) {
    GC_ROOT_RESTORE(js, root_mark);
    return val_ctor;
  }
  
  if (val_is_promise && strict_eq_values(js, val_ctor, ctor)) {
    GC_ROOT_RESTORE(js, root_mark);
    return val;
  }

  ant_value_t promise = js_mkundef();
  ant_value_t resolve = js_mkundef();
  ant_value_t reject = js_mkundef();

  ant_value_t capability_result = new_promise_capability(
    js, ctor, &promise,
    &resolve, &reject
  );

  if (is_err(capability_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return capability_result;
  }
  
  GC_ROOT_PIN(js, promise);
  GC_ROOT_PIN(js, resolve);

  ant_value_t resolve_args[] = { val };
  ant_value_t resolve_result = sv_vm_call(
    js->vm, js, resolve, js_mkundef(), 
    resolve_args, 1, NULL, js_mkundef()
  );
  
  if (is_err(resolve_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return resolve_result;
  }

  GC_ROOT_RESTORE(js, root_mark);
  return promise;
}

static ant_value_t builtin_Promise_resolve(ant_params_t) {
  ant_value_t val = nargs > 0 ? args[0] : js_mkundef();
  return promise_resolve_with_ctor(js, js->this_val, val);
}

ant_value_t js_promise_reject_with_ctor(
  ant_t *js, ant_value_t ctor, ant_value_t val
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, ctor);
  GC_ROOT_PIN(js, val);

  ant_value_t promise = js_mkundef();
  ant_value_t resolve = js_mkundef();
  ant_value_t reject = js_mkundef();
  
  ant_value_t capability_result = new_promise_capability(
    js, ctor, &promise, 
    &resolve, &reject
  );
  
  if (is_err(capability_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return capability_result;
  }

  GC_ROOT_PIN(js, promise);
  GC_ROOT_PIN(js, reject);

  ant_value_t reject_result = promise_call_one(
    js, reject, js_mkundef(), val
  );

  GC_ROOT_RESTORE(js, root_mark);
  return is_err(reject_result) ? reject_result : promise;
}

static ant_value_t builtin_Promise_reject(ant_params_t) {
  ant_value_t val = nargs > 0 ? args[0] : js_mkundef();
  return js_promise_reject_with_ctor(js, js->this_val, val);
}

static void promise_init_derived_promise(
  ant_t *js,
  ant_value_t next_p,
  ant_value_t parent_p,
  ant_value_t species_ctor,
  ant_value_t promise_ctor
) {
  ant_value_t next_obj = js_as_obj(next_p);

  if (is_constructor_value(species_ctor) && !is_same_heap_value(species_ctor, promise_ctor)) {
    ant_value_t species_proto = js_get(js, species_ctor, "prototype");
    if (is_object_type(species_proto)) js_set_proto_init(next_obj, species_proto);
    js_set_slot(next_obj, SLOT_CTOR, species_ctor);
    return;
  }

  ant_value_t parent_obj = js_as_obj(parent_p);
  ant_value_t parent_proto = js_get_slot(parent_obj, SLOT_PROTO);
  if (vtype(parent_proto) == kTypeObject) js_set_proto_init(next_obj, parent_proto);

  ant_value_t parent_ctor = js_get_slot(parent_obj, SLOT_CTOR);
  if (is_constructor_value(parent_ctor)) js_set_slot(next_obj, SLOT_CTOR, parent_ctor);
}

static ant_value_t promise_then_species_ctor(ant_t *js, ant_value_t p_obj, ant_value_t *out) {
  *out = js->sym.promise_ctor;
  
  if (
    ant_protector_intact(js, ANT_PROTECTOR_PROMISE_SPECIES) && 
    js_get_slot(p_obj, SLOT_PROTO) == js->sym.promise_proto
  ) return js_mkundef();

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t ctor = js_get(js, p_obj, "constructor");
  
  GC_ROOT_PIN(js, ctor);
  if (is_err(ctor)) {
    GC_ROOT_RESTORE(js, root_mark);
    return ctor;
  }
  
  if (vtype(ctor) == kTypeUndefined) ctor = js_get_slot(p_obj, SLOT_CTOR);
  ant_value_t species = get_ctor_species_value(js, ctor);
  
  GC_ROOT_RESTORE(js, root_mark);
  if (is_err(species)) return species;

  if (vtype(species) == kTypeFunction || vtype(species) == kTypeBuiltin) *out = species;
  else if (vtype(species) != kTypeNull && vtype(species) != kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Promise species is not a constructor");

  return js_mkundef();
}

static ant_value_t builtin_promise_then(ant_params_t) {
  ant_value_t p = js->this_val;
  if (vtype(p) != kTypePromise) return js_mkerr(js, "not a promise");

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, p);

  ant_value_t promise_ctor = js->sym.promise_ctor;
  ant_value_t species_ctor = promise_ctor;
  GC_ROOT_PIN(js, species_ctor);

  ant_value_t species_err = promise_then_species_ctor(js, js_as_obj(p), &species_ctor);
  if (is_err(species_err)) {
    GC_ROOT_RESTORE(js, root_mark);
    return species_err;
  }

  ant_value_t nextP = js_mkpromise(js);
  GC_ROOT_PIN(js, nextP);
  promise_init_derived_promise(js, nextP, p, species_ctor, promise_ctor);

  ant_value_t onFulfilled = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t onRejected = nargs > 1 ? args[1] : js_mkundef();
  
  GC_ROOT_PIN(js, onFulfilled);
  GC_ROOT_PIN(js, onRejected);

  ant_promise_state_t *next_pd = js_promise_data(js, nextP, false);
  if (next_pd) next_pd->trigger_parent = p;

  ant_promise_state_t *pd = js_promise_data(js, p, false);
  if (pd) {
    promise_handler_t h = { onFulfilled, onRejected, nextP, NULL };
    if (!promise_handler_append(pd, &h)) {
      GC_ROOT_RESTORE(js, root_mark);
      return Ant_Error_CreateOutOfMemory(js);
    }
    
    gc_write_barrier(js, js_obj_ptr(js_as_obj(p)), nextP);
    gc_write_barrier(js, js_obj_ptr(js_as_obj(p)), onFulfilled);
    gc_write_barrier(js, js_obj_ptr(js_as_obj(p)), onRejected);
    
    if (is_callable(onRejected))
      js_mark_promise_rejection_handled_chain(js, p);
      
    if (pd->state == 0)
      gc_root_pending_promise(js, js_obj_ptr(p));
  }

  if (pd && pd->state != 0) queue_promise_trigger(js, p);
  GC_ROOT_RESTORE(js, root_mark);
  
  return nextP;
}

ant_value_t js_promise_then(ant_t *js, ant_value_t promise, ant_value_t on_fulfilled, ant_value_t on_rejected) {
  ant_value_t args_then[2] = { on_fulfilled, on_rejected };
  return sv_call_native_this(js, builtin_promise_then, promise, args_then, 2);
}

static ant_value_t builtin_promise_catch(ant_params_t) {
  ant_value_t args_then[] = { js_mkundef(), nargs > 0 ? args[0] : js_mkundef() };
  return builtin_promise_then(js, args_then, 2, js_mkundef());
}

static ant_value_t finally_value_thunk(ant_params_t) {
  ant_value_t me = js->current_func;
  return js_get_slot(me, SLOT_DATA);
}

static ant_value_t finally_thrower(ant_params_t) {
  ant_value_t me = js->current_func;
  ant_value_t reason = js_get_slot(me, SLOT_DATA);
  ant_value_t rejected = js_mkpromise(js);
  js_reject_promise(js, rejected, reason);
  return rejected;
}

static ant_value_t finally_identity_reject(ant_params_t) {
  ant_value_t reason = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t rejected = js_mkpromise(js);
  js_reject_promise(js, rejected, reason);
  return rejected;
}

static ant_value_t finally_chain_result(
  ant_t *js,
  ant_value_t result,
  ant_value_t completion,
  bool rejected
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, result);
  GC_ROOT_PIN(js, completion);

  ant_value_t promise = js_promise_assimilate_awaitable(js, result);
  GC_ROOT_PIN(js, promise);
  if (is_err(promise)) {
    GC_ROOT_RESTORE(js, root_mark);
    return promise;
  }

  ant_value_t then_fn = js_get_thenable_then(js, promise);
  GC_ROOT_PIN(js, then_fn);
  if (is_err(then_fn)) {
    GC_ROOT_RESTORE(js, root_mark);
    return then_fn;
  }
  if (!is_callable(then_fn)) {
    ant_value_t err = js_mkerr_typed(js, JS_ERR_TYPE, "Promise then is not callable");
    GC_ROOT_RESTORE(js, root_mark);
    return err;
  }

  ant_value_t on_fulfilled = js_make_data_cfunc(
    js, completion,
    rejected ? finally_thrower : finally_value_thunk
  );
  
  GC_ROOT_PIN(js, on_fulfilled);
  if (is_err(on_fulfilled)) {
    GC_ROOT_RESTORE(js, root_mark);
    return on_fulfilled;
  }

  ant_value_t on_rejected = Ant_Function_Create(finally_identity_reject);
  ant_value_t then_args[] = { on_fulfilled, on_rejected };
  
  ant_value_t chained = sv_vm_call(
    js->vm, js, then_fn, promise, 
    then_args, 2, NULL, js_mkundef()
  );
  
  GC_ROOT_RESTORE(js, root_mark);
  return chained;
}

static ant_value_t finally_fulfilled_wrapper(ant_params_t) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t me = js->current_func;
  ant_value_t callback = js_get_slot(me, SLOT_DATA);
  ant_value_t value = nargs > 0 ? args[0] : js_mkundef();
  GC_ROOT_PIN(js, callback);
  GC_ROOT_PIN(js, value);

  ant_value_t result = js_mkundef();
  if (is_callable(callback)) {
    result = sv_vm_call(js->vm, js, callback, js_mkundef(), NULL, 0, NULL, js_mkundef());
    if (is_err(result)) {
      GC_ROOT_RESTORE(js, root_mark);
      return result;
    }
  }
  GC_ROOT_PIN(js, result);

  ant_value_t chained = finally_chain_result(js, result, value, false);
  GC_ROOT_RESTORE(js, root_mark);
  return chained;
}

static ant_value_t finally_rejected_wrapper(ant_params_t) {
  GC_ROOT_SAVE(root_mark, js);
  
  ant_value_t me = js->current_func;
  ant_value_t callback = js_get_slot(me, SLOT_DATA);
  ant_value_t reason = nargs > 0 ? args[0] : js_mkundef();
  
  GC_ROOT_PIN(js, callback);
  GC_ROOT_PIN(js, reason);

  ant_value_t result = js_mkundef();
  if (is_callable(callback)) {
    result = sv_vm_call(js->vm, js, callback, js_mkundef(), NULL, 0, NULL, js_mkundef());
    if (is_err(result)) {
      GC_ROOT_RESTORE(js, root_mark);
      return result;
    }
  }
  GC_ROOT_PIN(js, result);

  ant_value_t chained = finally_chain_result(js, result, reason, true);
  GC_ROOT_RESTORE(js, root_mark);
  return chained;
}

static ant_value_t builtin_promise_finally(ant_params_t) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t callback = nargs > 0 ? args[0] : js_mkundef();
  GC_ROOT_PIN(js, callback);

  ant_value_t fulfilled_fn = js_make_data_cfunc(js, callback, finally_fulfilled_wrapper);
  GC_ROOT_PIN(js, fulfilled_fn);
  if (is_err(fulfilled_fn)) {
    GC_ROOT_RESTORE(js, root_mark);
    return fulfilled_fn;
  }

  ant_value_t rejected_fn = js_make_data_cfunc(js, callback, finally_rejected_wrapper);
  GC_ROOT_PIN(js, rejected_fn);
  if (is_err(rejected_fn)) {
    GC_ROOT_RESTORE(js, root_mark);
    return rejected_fn;
  }

  ant_value_t args_then[] = { fulfilled_fn, rejected_fn };
  ant_value_t ret = builtin_promise_then(js, args_then, 2, js_mkundef());
  GC_ROOT_RESTORE(js, root_mark);
  
  return ret;
}

static ant_value_t builtin_Promise_try(ant_params_t) {
  if (nargs == 0) return builtin_Promise_resolve(js, args, 0, js_mkundef());
  
  ant_value_t fn = args[0];
  ant_value_t *call_args = nargs > 1 ? &args[1] : NULL;
  int call_nargs = nargs > 1 ? nargs - 1 : 0;
  ant_value_t res = sv_vm_call(js->vm, js, fn, js_mkundef(), call_args, call_nargs, NULL, js_mkundef());
  
  if (is_err(res)) {
    ant_value_t reject_val = js_take_thrown(js, res);
    ant_value_t rej_args[] = { reject_val };
    return builtin_Promise_reject(js, rej_args, 1, js_mkundef());
  }
  
  ant_value_t res_args[] = { res };
  return builtin_Promise_resolve(js, res_args, 1, js_mkundef());
}

static ant_value_t builtin_Promise_withResolvers(ant_params_t) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t p = js_mkpromise(js);
  GC_ROOT_PIN(js, p);

  ant_value_t res_fn = js_mkundef();
  ant_value_t rej_fn = js_mkundef();
  ant_value_t pair_result = make_promise_resolving_functions(js, p, &res_fn, &rej_fn);
  GC_ROOT_PIN(js, pair_result);
  GC_ROOT_PIN(js, res_fn);
  GC_ROOT_PIN(js, rej_fn);
  if (is_err(pair_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return pair_result;
  }

  ant_value_t result = Ant_Object_Create(js);
  GC_ROOT_PIN(js, result);
  js_setprop(js, result, js_mkstr(js, "promise", 7), p);
  js_setprop(js, result, js_mkstr(js, "resolve", 7), res_fn);
  js_setprop(js, result, js_mkstr(js, "reject", 6), rej_fn);

  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static ant_value_t promise_get_resolve(ant_t *js, ant_value_t ctor) {
  ant_value_t resolve = js_getprop_fallback(js, ctor, "resolve");
  if (is_err(resolve) || is_callable(resolve)) return resolve;
  return js_mkerr_typed(js, JS_ERR_TYPE, "Promise resolve is not callable");
}

static ant_value_t promise_reject_abrupt(
  ant_t *js,
  ant_value_t promise,
  ant_value_t reject,
  ant_value_t abrupt
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, promise);
  GC_ROOT_PIN(js, reject);

  ant_value_t reason = js_take_thrown(js, abrupt);
  GC_ROOT_PIN(js, reason);
  ant_value_t reject_result = promise_call_one(
    js, reject, js_mkundef(), reason
  );
  GC_ROOT_RESTORE(js, root_mark);
  return is_err(reject_result) ? reject_result : promise;
}

static ant_value_t builtin_Promise_all_resolve_handler(ant_params_t) {
  ant_value_t me = js->current_func;
  if (js_get_slot(me, SLOT_SETTLED) == js_true) return js_mkundef();
  js_set_slot(me, SLOT_SETTLED, js_true);

  ant_value_t tracker = js_get_slot(me, SLOT_DATA);
  ant_value_t index_val = js_get_slot(me, SLOT_AUX);
  
  int index = (int)tod(index_val);
  ant_value_t value = nargs > 0 ? args[0] : js_mkundef();
  
  ant_value_t results = js_get_slot(tracker, SLOT_ENTRIES);
  js_arr_set_index(js, results, (ant_offset_t)index, value);
  
  ant_value_t remaining_val = js_get_slot(tracker, SLOT_MAP);
  int remaining = (int)tod(remaining_val) - 1;
  js_set_slot(tracker, SLOT_MAP, tov((double)remaining));
  
  if (remaining == 0) {
    return promise_call_one(
      js,
      js_get_slot(tracker, SLOT_DATA),
      js_mkundef(),
      mkval(kTypeArray, vdata(results))
    );
  }
  
  return js_mkundef();
}

static ant_value_t builtin_Promise_all_reject_handler(ant_params_t) {
  ant_value_t resolve_fn = js_get_slot(js->current_func, SLOT_DATA);
  if (js_get_slot(resolve_fn, SLOT_SETTLED) == js_true) return js_mkundef();
  js_set_slot(resolve_fn, SLOT_SETTLED, js_true);

  ant_value_t tracker = js_get_slot(resolve_fn, SLOT_DATA);
  ant_value_t reason = nargs > 0 ? args[0] : js_mkundef();
  return promise_call_one(
    js, js_get_slot(tracker, SLOT_AUX), js_mkundef(), reason
  );
}

static ant_value_t promise_all_settled_make_result(ant_t *js, bool fulfilled, ant_value_t value) {
  ant_value_t result = mkobj(js, 0);
  if (is_err(result)) return result;
  
  js_setprop(
    js, result,
    js_mkstr(js, "status", 6),
    js_mkstr(js, fulfilled ? "fulfilled" : "rejected", fulfilled ? 9 : 8)
  );
  
  js_setprop(
    js, result,
    js_mkstr(js, fulfilled ? "value" : "reason", fulfilled ? 5 : 6), value
  );
  
  return result;
}

static ant_value_t promise_all_settled_store_result(
  ant_t *js,
  ant_value_t tracker,
  int index,
  bool fulfilled,
  ant_value_t value
) {
  ant_value_t results = js_get_slot(tracker, SLOT_ENTRIES);
  ant_value_t result = promise_all_settled_make_result(js, fulfilled, value);
  ant_value_t remaining_val = 0;
  int remaining = 0;

  if (is_err(result)) return result;
  js_arr_set_index(js, results, (ant_offset_t)index, result);

  remaining_val = js_get_slot(tracker, SLOT_MAP);
  remaining = (int)tod(remaining_val) - 1;
  js_set_slot(tracker, SLOT_MAP, tov((double)remaining));

  if (remaining == 0) {
    return promise_call_one(
      js,
      js_get_slot(tracker, SLOT_DATA),
      js_mkundef(),
      mkval(kTypeArray, vdata(results))
    );
  }

  return js_mkundef();
}

static ant_value_t builtin_Promise_allSettled_resolve_handler(ant_params_t) {
  ant_value_t me = js->current_func;
  if (js_get_slot(me, SLOT_SETTLED) == js_true) return js_mkundef();
  js_set_slot(me, SLOT_SETTLED, js_true);

  ant_value_t tracker = js_get_slot(me, SLOT_DATA);
  ant_value_t index_val = js_get_slot(me, SLOT_AUX);
  int index = (int)tod(index_val);
  ant_value_t value = nargs > 0 ? args[0] : js_mkundef();

  return promise_all_settled_store_result(js, tracker, index, true, value);
}

static ant_value_t builtin_Promise_allSettled_reject_handler(ant_params_t) {
  ant_value_t resolve_fn = js_get_slot(js->current_func, SLOT_DATA);
  if (js_get_slot(resolve_fn, SLOT_SETTLED) == js_true) return js_mkundef();
  js_set_slot(resolve_fn, SLOT_SETTLED, js_true);

  ant_value_t tracker = js_get_slot(resolve_fn, SLOT_DATA);
  ant_value_t index_val = js_get_slot(resolve_fn, SLOT_AUX);
  int index = (int)tod(index_val);
  ant_value_t reason = nargs > 0 ? args[0] : js_mkundef();

  return promise_all_settled_store_result(js, tracker, index, false, reason);
}

typedef struct {
  ant_value_t tracker;
  ant_value_t ctor;
  ant_value_t promise_resolve;
  int index;
} promise_all_iter_ctx_t;

static ant_value_t promise_invoke_then(
  ant_t *js,
  ant_value_t promise,
  ant_value_t on_fulfilled,
  ant_value_t on_rejected
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, promise);
  GC_ROOT_PIN(js, on_fulfilled);
  GC_ROOT_PIN(js, on_rejected);

  ant_value_t then = js_get_thenable_then(js, promise);
  GC_ROOT_PIN(js, then);
  if (is_err(then)) {
    GC_ROOT_RESTORE(js, root_mark);
    return then;
  }
  if (!is_callable(then)) {
    ant_value_t err = js_mkerr_typed(js, JS_ERR_TYPE, "Promise then is not callable");
    GC_ROOT_RESTORE(js, root_mark);
    return err;
  }

  ant_value_t then_args[] = { on_fulfilled, on_rejected };
  ant_value_t result = sv_vm_call(
    js->vm, js, then, promise, 
    then_args, 2, NULL, js_mkundef()
  );
  
  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static iter_action_t promise_all_iter_cb(ant_t *js, ant_value_t value, void *ctx, ant_value_t *out) {
  GC_ROOT_SAVE(root_mark, js);
  promise_all_iter_ctx_t *pctx = (promise_all_iter_ctx_t *)ctx;
  ant_value_t item = promise_call_one(
    js, pctx->promise_resolve, pctx->ctor, value
  );
  GC_ROOT_PIN(js, item);
  if (is_err(item)) {
    *out = item;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }

  ant_value_t resolve_obj = mkobj(js, 0);
  if (is_err(resolve_obj)) {
    *out = resolve_obj;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }
  GC_ROOT_PIN(js, resolve_obj);
  js_obj_extra_reserve(js_obj_ptr(resolve_obj), 4);
  js_set_slot(resolve_obj, SLOT_CFUNC, Ant_Function_Create(builtin_Promise_all_resolve_handler));
  js_set_slot(resolve_obj, SLOT_DATA, pctx->tracker);
  js_set_slot(resolve_obj, SLOT_AUX, tov((double)pctx->index));
  
  ant_value_t resolve_fn = js_obj_to_func(js, resolve_obj);
  GC_ROOT_PIN(js, resolve_fn);
  js_set_slot(resolve_fn, SLOT_SETTLED, js_false);

  ant_value_t reject_obj = mkobj(js, 0);
  if (is_err(reject_obj)) {
    *out = reject_obj;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }
  
  GC_ROOT_PIN(js, reject_obj);
  js_obj_extra_reserve(js_obj_ptr(reject_obj), 2);
  js_set_slot(reject_obj, SLOT_CFUNC, Ant_Function_Create(builtin_Promise_all_reject_handler));
  js_set_slot(reject_obj, SLOT_DATA, resolve_fn);
  ant_value_t reject_fn = js_obj_to_func(js, reject_obj);
  GC_ROOT_PIN(js, reject_fn);

  int remaining = (int)tod(js_get_slot(pctx->tracker, SLOT_MAP)) + 1;
  js_set_slot(pctx->tracker, SLOT_MAP, tov((double)remaining));

  ant_value_t then_result = promise_invoke_then(js, item, resolve_fn, reject_fn);
  if (is_err(then_result)) {
    *out = then_result;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }

  pctx->index++;
  GC_ROOT_RESTORE(js, root_mark);
  return ITER_CONTINUE;
}

static iter_action_t promise_all_settled_iter_cb(ant_t *js, ant_value_t value, void *ctx, ant_value_t *out) {
  GC_ROOT_SAVE(root_mark, js);
  promise_all_iter_ctx_t *pctx = (promise_all_iter_ctx_t *)ctx;
  
  ant_value_t item = promise_call_one(
    js, pctx->promise_resolve, 
    pctx->ctor, value
  );
  
  GC_ROOT_PIN(js, item);
  if (is_err(item)) {
    *out = item;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }

  ant_value_t resolve_obj = mkobj(js, 0);
  if (is_err(resolve_obj)) {
    *out = resolve_obj;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }
  
  GC_ROOT_PIN(js, resolve_obj);
  js_obj_extra_reserve(js_obj_ptr(resolve_obj), 4);
  
  js_set_slot(resolve_obj, SLOT_CFUNC, Ant_Function_Create(builtin_Promise_allSettled_resolve_handler));
  js_set_slot(resolve_obj, SLOT_DATA, pctx->tracker);
  js_set_slot(resolve_obj, SLOT_AUX, tov((double)pctx->index));
  
  ant_value_t resolve_fn = js_obj_to_func(js, resolve_obj);
  GC_ROOT_PIN(js, resolve_fn);
  js_set_slot(resolve_fn, SLOT_SETTLED, js_false);

  ant_value_t reject_obj = mkobj(js, 0);
  if (is_err(reject_obj)) {
    *out = reject_obj;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }
  
  GC_ROOT_PIN(js, reject_obj);
  js_obj_extra_reserve(js_obj_ptr(reject_obj), 2);
  
  js_set_slot(reject_obj, SLOT_CFUNC, Ant_Function_Create(builtin_Promise_allSettled_reject_handler));
  js_set_slot(reject_obj, SLOT_DATA, resolve_fn);
  ant_value_t reject_fn = js_obj_to_func(js, reject_obj);
  GC_ROOT_PIN(js, reject_fn);

  int remaining = (int)tod(js_get_slot(pctx->tracker, SLOT_MAP)) + 1;
  js_set_slot(pctx->tracker, SLOT_MAP, tov((double)remaining));

  ant_value_t then_result = promise_invoke_then(js, item, resolve_fn, reject_fn);
  if (is_err(then_result)) {
    *out = then_result;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }

  pctx->index++;
  GC_ROOT_RESTORE(js, root_mark);
  return ITER_CONTINUE;
}

static bool promise_all_fast_eligible(ant_t *js, ant_value_t ctor, ant_value_t iterable) {
  if (ctor != js->sym.promise_ctor) return false;
  
  if (!ant_protector_intact(js,
    ANT_PROTECTOR_PROMISE_CONSTRUCTOR | ANT_PROTECTOR_PROMISE_RESOLVE_LOOKUP |
    ANT_PROTECTOR_PROMISE_SPECIES | ANT_PROTECTOR_PROMISE_THEN
  )) return false;

  if (vtype(iterable) != kTypeArray || !js_array_iteration_default(js, iterable)) return false;

  ant_object_t *arr = js_dense_array_obj(iterable);
  if (!arr) return false;

  for (ant_offset_t i = 0; i < arr->u.array.len; i++) {
    ant_value_t v = arr->u.array.data[i];
    if (is_empty_slot(v)) return false;
    
    if (vtype(v) == kTypePromise) {
      if (js_get_slot(v, SLOT_PROTO) != js->sym.promise_proto) return false;
      continue;
    }
    
    if (is_object_type(v) || vtype(v) == kTypeBuiltin) return false;
  }
  
  return true;
}

static ant_value_t promise_all_fast(ant_t *js, ant_value_t iterable) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, iterable);
  
  ant_object_t *arr = js_dense_array_obj(iterable);
  uint32_t len = (uint32_t)arr->u.array.len;

  ant_value_t result = js_mkpromise(js);
  GC_ROOT_PIN(js, result);
  
  ant_value_t results = js_mkarr_dense_literal(js, arr->u.array.data, len);
  GC_ROOT_PIN(js, results);
  
  ant_value_t tracker = mkobj(js, 0);
  GC_ROOT_PIN(js, tracker);
  
  if (is_err(result) || is_err(results) || is_err(tracker)) {
    GC_ROOT_RESTORE(js, root_mark);
    return is_err(result) ? result : is_err(results) ? results : tracker;
  }

  js_obj_extra_reserve(js_obj_ptr(tracker), 3);
  js_set_slot_wb(js, tracker, SLOT_DATA, result);
  js_set_slot_wb(js, tracker, SLOT_ENTRIES, results);
  js_set_slot(tracker, SLOT_MAP, tov((double)len));

  if (len == 0) {
    js_resolve_promise(js, result, mkval(kTypeArray, vdata(results)));
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }

  for (uint32_t i = 0; i < len; i++) {
    ant_value_t item = js_dense_array_obj(iterable)->u.array.data[i];
    GC_ROOT_SAVE(item_mark, js);
    GC_ROOT_PIN(js, item);
    
    if (vtype(item) != kTypePromise) {
      ant_value_t wrapped = js_mkpromise(js);
      if (is_err(wrapped)) {
        GC_ROOT_RESTORE(js, root_mark);
        return wrapped;
      }
      js_resolve_promise(js, wrapped, item);
      item = wrapped;
    }

    ant_promise_state_t *pd = js_promise_data(js, item, false);
    promise_handler_t handler = { tracker, tov((double)i), PROMISE_ALL_ELEMENT, NULL };
    if (!pd || !promise_handler_append(pd, &handler)) {
      GC_ROOT_RESTORE(js, root_mark);
      return Ant_Error_CreateOutOfMemory(js);
    }

    gc_write_barrier(js, js_obj_ptr(js_as_obj(item)), tracker);
    js_mark_promise_rejection_handled_chain(js, item);
    if (pd->state == 0) gc_root_pending_promise(js, js_obj_ptr(js_as_obj(item)));
    else queue_promise_trigger(js, item);
    GC_ROOT_RESTORE(js, item_mark);
  }

  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static ant_value_t builtin_Promise_all(ant_params_t) {
  if (nargs > 0 && promise_all_fast_eligible(js, js->this_val, args[0]))
    return promise_all_fast(js, args[0]);

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t ctor = js->this_val;
  GC_ROOT_PIN(js, ctor);

  ant_value_t result_promise = js_mkundef();
  ant_value_t result_resolve = js_mkundef();
  ant_value_t result_reject = js_mkundef();
  
  ant_value_t capability_result = new_promise_capability(
    js, ctor, &result_promise, 
    &result_resolve, &result_reject
  );
  
  GC_ROOT_PIN(js, result_promise);
  GC_ROOT_PIN(js, result_resolve);
  GC_ROOT_PIN(js, result_reject);
  if (is_err(capability_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return capability_result;
  }

  ant_value_t promise_resolve = promise_get_resolve(js, ctor);
  GC_ROOT_PIN(js, promise_resolve);
  if (is_err(promise_resolve)) {
    ant_value_t result = promise_reject_abrupt(
      js, result_promise, result_reject, promise_resolve
    );
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }

  ant_value_t iterable = nargs > 0 ? args[0] : js_mkundef();
  GC_ROOT_PIN(js, iterable);

  ant_value_t tracker = mkobj(js, 0);
  GC_ROOT_PIN(js, tracker);
  ant_value_t results = js_mkarr(js);
  GC_ROOT_PIN(js, results);

  js_set_slot_wb(js, tracker, SLOT_DATA, result_resolve);
  js_set_slot_wb(js, tracker, SLOT_AUX, result_reject);
  js_set_slot_wb(js, tracker, SLOT_ENTRIES, results);
  js_set_slot(tracker, SLOT_MAP, tov(1.0));

  promise_all_iter_ctx_t ctx = {
    .tracker = tracker,
    .ctor = ctor,
    .promise_resolve = promise_resolve,
    .index = 0
  };
  ant_value_t iter_result = js_iter_foreach(js, iterable, promise_all_iter_cb, &ctx);

  if (is_err(iter_result)) {
    ant_value_t result = promise_reject_abrupt(
      js, result_promise, 
      result_reject, iter_result
    );
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }

  int len = ctx.index;
  js_arr_set_dense_length(js, results, (ant_offset_t)len);

  int remaining = (int)tod(js_get_slot(tracker, SLOT_MAP)) - 1;
  js_set_slot(tracker, SLOT_MAP, tov((double)remaining));
  if (remaining == 0) {
    ant_value_t resolve_result = promise_call_one(
      js, result_resolve, js_mkundef(), 
      mkval(kTypeArray, vdata(results))
    );
    if (is_err(resolve_result)) {
      ant_value_t result = promise_reject_abrupt(
        js, result_promise, 
        result_reject, resolve_result
      );
      GC_ROOT_RESTORE(js, root_mark);
      return result;
    }
  }
  GC_ROOT_RESTORE(js, root_mark);
  return result_promise;
}

static ant_value_t builtin_Promise_allSettled(ant_params_t) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t ctor = js->this_val;
  GC_ROOT_PIN(js, ctor);

  ant_value_t result_promise = js_mkundef();
  ant_value_t result_resolve = js_mkundef();
  ant_value_t result_reject = js_mkundef();
  
  ant_value_t capability_result = new_promise_capability(
    js, ctor, &result_promise, 
    &result_resolve, &result_reject
  );
  
  GC_ROOT_PIN(js, result_promise);
  GC_ROOT_PIN(js, result_resolve);
  GC_ROOT_PIN(js, result_reject);
  
  if (is_err(capability_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return capability_result;
  }

  ant_value_t promise_resolve = promise_get_resolve(js, ctor);
  GC_ROOT_PIN(js, promise_resolve);
  if (is_err(promise_resolve)) {
    ant_value_t result = promise_reject_abrupt(
      js, result_promise, 
      result_reject, promise_resolve
    );
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }

  ant_value_t iterable = nargs > 0 ? args[0] : js_mkundef();
  GC_ROOT_PIN(js, iterable);

  ant_value_t tracker = mkobj(js, 0);
  GC_ROOT_PIN(js, tracker);
  ant_value_t results = js_mkarr(js);
  GC_ROOT_PIN(js, results);

  js_set_slot_wb(js, tracker, SLOT_DATA, result_resolve);
  js_set_slot_wb(js, tracker, SLOT_AUX, result_reject);
  js_set_slot_wb(js, tracker, SLOT_ENTRIES, results);
  js_set_slot(tracker, SLOT_MAP, tov(1.0));

  promise_all_iter_ctx_t ctx = {
    .tracker = tracker,
    .ctor = ctor,
    .promise_resolve = promise_resolve,
    .index = 0
  };
  ant_value_t iter_result = js_iter_foreach(js, iterable, promise_all_settled_iter_cb, &ctx);

  if (is_err(iter_result)) {
    ant_value_t result = promise_reject_abrupt(
      js, result_promise, result_reject, iter_result
    );
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }

  int len = ctx.index;
  js_arr_set_dense_length(js, results, (ant_offset_t)len);

  int remaining = (int)tod(js_get_slot(tracker, SLOT_MAP)) - 1;
  js_set_slot(tracker, SLOT_MAP, tov((double)remaining));
  if (remaining == 0) {
    ant_value_t resolve_result = promise_call_one(
      js, result_resolve, js_mkundef(), 
      mkval(kTypeArray, vdata(results))
    );
    if (is_err(resolve_result)) {
      ant_value_t result = promise_reject_abrupt(
        js, result_promise, 
        result_reject, resolve_result
      );
      GC_ROOT_RESTORE(js, root_mark);
      return result;
    }
  }
  GC_ROOT_RESTORE(js, root_mark);
  return result_promise;
}

typedef struct {
  ant_value_t ctor;
  ant_value_t promise_resolve;
  ant_value_t resolve_fn;
  ant_value_t reject_fn;
} promise_race_iter_ctx_t;

static iter_action_t promise_race_iter_cb(ant_t *js, ant_value_t value, void *ctx, ant_value_t *out) {
  GC_ROOT_SAVE(root_mark, js);
  promise_race_iter_ctx_t *pctx = (promise_race_iter_ctx_t *)ctx;
  ant_value_t item = promise_call_one(
    js, pctx->promise_resolve, pctx->ctor, value
  );
  GC_ROOT_PIN(js, item);
  if (is_err(item)) {
    *out = item;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }

  ant_value_t then_result = promise_invoke_then(
    js, item, pctx->resolve_fn, pctx->reject_fn
  );
  if (is_err(then_result)) {
    *out = then_result;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }

  GC_ROOT_RESTORE(js, root_mark);
  return ITER_CONTINUE;
}

static ant_value_t builtin_Promise_race(ant_params_t) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t ctor = js->this_val;
  GC_ROOT_PIN(js, ctor);

  ant_value_t result_promise = js_mkundef();
  ant_value_t resolve_fn = js_mkundef();
  ant_value_t reject_fn = js_mkundef();
  ant_value_t capability_result = new_promise_capability(
    js, ctor, &result_promise, 
    &resolve_fn, &reject_fn
  );
  
  GC_ROOT_PIN(js, result_promise);
  GC_ROOT_PIN(js, resolve_fn);
  GC_ROOT_PIN(js, reject_fn);
  if (is_err(capability_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return capability_result;
  }

  ant_value_t promise_resolve = promise_get_resolve(js, ctor);
  GC_ROOT_PIN(js, promise_resolve);
  if (is_err(promise_resolve)) {
    ant_value_t result = promise_reject_abrupt(
      js, result_promise, 
      reject_fn, promise_resolve
    );
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }

  ant_value_t iterable = nargs > 0 ? args[0] : js_mkundef();
  GC_ROOT_PIN(js, iterable);

  promise_race_iter_ctx_t ctx = {
    .ctor = ctor,
    .promise_resolve = promise_resolve,
    .resolve_fn = resolve_fn,
    .reject_fn = reject_fn
  };

  ant_value_t iter_result = js_iter_foreach(js, iterable, promise_race_iter_cb, &ctx);
  if (is_err(iter_result)) {
    ant_value_t result = promise_reject_abrupt(
      js, result_promise, 
      reject_fn, iter_result
    );
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }

  GC_ROOT_RESTORE(js, root_mark);
  return result_promise;
}

static ant_value_t mk_aggregate_error(ant_t *js, ant_value_t errors) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, errors);
  
  ant_value_t args[] = { errors, js_mkstr(js, "All promises were rejected", 26) };
  ant_prop_loc_t off = lkp(js, js_glob(js), "AggregateError", 14);
  
  ant_value_t ctor = off.obj ? js_prop_load(off) : js_mkundef();
  GC_ROOT_PIN(js, ctor);
  
  ant_value_t ret = sv_vm_call(js->vm, js, ctor, js_mkundef(), args, 2, NULL, js_mkundef());
  GC_ROOT_RESTORE(js, root_mark);
  
  return ret;
}

static ant_value_t promise_any_try_resolve(
  ant_t *js, ant_value_t tracker, ant_value_t value
) {
  if (js_get_slot(tracker, SLOT_SETTLED) == js_true) return js_mkundef();
  js_set_slot(tracker, SLOT_SETTLED, js_true);
  return promise_call_one(
    js, js_get_slot(tracker, SLOT_DATA), js_mkundef(), value
  );
}

static ant_value_t promise_any_record_rejection(
  ant_t *js, ant_value_t tracker, int index, ant_value_t reason
) {
  ant_value_t errors = js_get_slot(tracker, SLOT_ENTRIES);
  js_arr_set_index(js, errors, (ant_offset_t)index, reason);
  
  int remaining = (int)tod(js_get_slot(tracker, SLOT_MAP)) - 1;
  js_set_slot(tracker, SLOT_MAP, tov((double)remaining));
  
  if (remaining != 0) return js_mkundef();
  js_set_slot(tracker, SLOT_SETTLED, js_true);
  return promise_call_one(
    js,
    js_get_slot(tracker, SLOT_AUX),
    js_mkundef(),
    mk_aggregate_error(js, errors)
  );
}

static ant_value_t builtin_Promise_any_resolve_handler(ant_params_t) {
  ant_value_t resolve_fn = js->current_func;
  if (js_get_slot(resolve_fn, SLOT_SETTLED) == js_true) return js_mkundef();
  js_set_slot(resolve_fn, SLOT_SETTLED, js_true);

  ant_value_t tracker = js_get_slot(resolve_fn, SLOT_DATA);
  return promise_any_try_resolve(
    js, tracker, nargs > 0 ? args[0] : js_mkundef()
  );
}

static ant_value_t builtin_Promise_any_reject_handler(ant_params_t) {
  ant_value_t resolve_fn = js_get_slot(js->current_func, SLOT_DATA);
  if (js_get_slot(resolve_fn, SLOT_SETTLED) == js_true) return js_mkundef();
  js_set_slot(resolve_fn, SLOT_SETTLED, js_true);

  ant_value_t tracker = js_get_slot(resolve_fn, SLOT_DATA);
  if (js_get_slot(tracker, SLOT_SETTLED) == js_true) return js_mkundef();
  
  int index = (int)tod(js_get_slot(resolve_fn, SLOT_AUX));
  return promise_any_record_rejection(
    js, tracker, index, nargs > 0 ? args[0] : js_mkundef()
  );
}

typedef struct {
  ant_value_t tracker;
  ant_value_t ctor;
  ant_value_t promise_resolve;
  int index;
} promise_any_iter_ctx_t;

static iter_action_t promise_any_iter_cb(
  ant_t *js, ant_value_t value, void *ctx, ant_value_t *out
) {
  GC_ROOT_SAVE(root_mark, js);
  promise_any_iter_ctx_t *pctx = (promise_any_iter_ctx_t *)ctx;
  ant_value_t item = promise_call_one(
    js, pctx->promise_resolve,
    pctx->ctor, value
  );

  GC_ROOT_PIN(js, item);
  if (is_err(item)) {
    *out = item;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }

  ant_value_t resolve_obj = mkobj(js, 0);
  if (is_err(resolve_obj)) {
    *out = resolve_obj;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }

  GC_ROOT_PIN(js, resolve_obj);
  js_set_slot(resolve_obj, SLOT_CFUNC, Ant_Function_Create(builtin_Promise_any_resolve_handler));
  js_set_slot(resolve_obj, SLOT_SETTLED, js_false);
  js_set_slot(resolve_obj, SLOT_DATA, pctx->tracker);
  js_set_slot(resolve_obj, SLOT_AUX, tov((double)pctx->index));

  ant_value_t resolve_fn = js_obj_to_func(js, resolve_obj);
  GC_ROOT_PIN(js, resolve_fn);

  ant_value_t reject_obj = mkobj(js, 0);
  if (is_err(reject_obj)) {
    *out = reject_obj;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }

  GC_ROOT_PIN(js, reject_obj);
  js_set_slot(reject_obj, SLOT_CFUNC, Ant_Function_Create(builtin_Promise_any_reject_handler));
  js_set_slot(reject_obj, SLOT_DATA, resolve_fn);

  ant_value_t reject_fn = js_obj_to_func(js, reject_obj);
  GC_ROOT_PIN(js, reject_fn);

  int remaining = (int)tod(js_get_slot(pctx->tracker, SLOT_MAP)) + 1;
  js_set_slot(pctx->tracker, SLOT_MAP, tov((double)remaining));

  ant_value_t then_result = promise_invoke_then(
    js, item, resolve_fn, reject_fn
  );
  if (is_err(then_result)) {
    *out = then_result;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }

  pctx->index++;
  GC_ROOT_RESTORE(js, root_mark);

  return ITER_CONTINUE;
}

static ant_value_t builtin_Promise_any(ant_params_t) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t ctor = js->this_val;
  GC_ROOT_PIN(js, ctor);

  ant_value_t result_promise = js_mkundef();
  ant_value_t result_resolve = js_mkundef();
  ant_value_t result_reject = js_mkundef();

  ant_value_t capability_result = new_promise_capability(
    js, ctor, &result_promise,
    &result_resolve, &result_reject
  );

  GC_ROOT_PIN(js, result_promise);
  GC_ROOT_PIN(js, result_resolve);
  GC_ROOT_PIN(js, result_reject);
  if (is_err(capability_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return capability_result;
  }

  ant_value_t promise_resolve = promise_get_resolve(js, ctor);
  GC_ROOT_PIN(js, promise_resolve);
  if (is_err(promise_resolve)) {
    ant_value_t result = promise_reject_abrupt(
      js, result_promise,
      result_reject, promise_resolve
    );
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }

  ant_value_t iterable = nargs > 0 ? args[0] : js_mkundef();
  GC_ROOT_PIN(js, iterable);

  ant_value_t tracker = mkobj(js, 0);
  GC_ROOT_PIN(js, tracker);
  ant_value_t errors = js_mkarr(js);
  GC_ROOT_PIN(js, errors);

  js_set_slot_wb(js, tracker, SLOT_DATA, result_resolve);
  js_set_slot_wb(js, tracker, SLOT_AUX, result_reject);
  js_set_slot_wb(js, tracker, SLOT_ENTRIES, errors);
  js_set_slot(tracker, SLOT_MAP, tov(1.0));
  js_set_slot(tracker, SLOT_SETTLED, js_false);

  promise_any_iter_ctx_t ctx = {
    .tracker = tracker,
    .ctor = ctor,
    .promise_resolve = promise_resolve,
    .index = 0
  };
  ant_value_t iter_result = js_iter_foreach(
    js, iterable,
    promise_any_iter_cb, &ctx
  );

  if (is_err(iter_result)) {
    ant_value_t result = promise_reject_abrupt(
      js, result_promise,
      result_reject, iter_result
    );
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }

  int len = ctx.index;
  js_arr_set_dense_length(js, errors, (ant_offset_t)len);

  int remaining = (int)tod(js_get_slot(tracker, SLOT_MAP)) - 1;
  js_set_slot(tracker, SLOT_MAP, tov((double)remaining));

  if (remaining == 0) {
    js_set_slot(tracker, SLOT_SETTLED, js_true);
    ant_value_t reject_result = promise_call_one(
      js, result_reject, js_mkundef(),
      mk_aggregate_error(js, errors)
    );

    if (is_err(reject_result)) {
      GC_ROOT_RESTORE(js, root_mark);
      return reject_result;
    }
  }

  GC_ROOT_RESTORE(js, root_mark);
  return result_promise;
}


bool js_is_promise_ctor_cfunc(ant_cfunc_t fn) {
  return fn == builtin_Promise;
}

void js_promise_init_prototype(ant_t *js, ant_value_t promise_proto) {
  js->sym.promise_then = defmethod(js, promise_proto, "then", 4, Ant_Function_CreateWithArity(builtin_promise_then, 2));
  defmethod(js, promise_proto, "catch", 5, Ant_Function_CreateWithArity(builtin_promise_catch, 1));
  defmethod(js, promise_proto, "finally", 7, Ant_Function_CreateWithArity(builtin_promise_finally, 1));
}

void js_promise_init_constructor(ant_t *js, ant_value_t ctor_obj) {
  js_set_slot(ctor_obj, SLOT_CFUNC, Ant_Function_CreateWithArity(builtin_Promise, 1));
  
  js->sym.promise_resolve = defmethod(js, ctor_obj, "resolve", 7, Ant_Function_CreateWithArity(builtin_Promise_resolve, 1));
  defmethod(js, ctor_obj, "reject", 6, Ant_Function_CreateWithArity(builtin_Promise_reject, 1));
  defmethod(js, ctor_obj, "try", 3, Ant_Function_CreateWithArity(builtin_Promise_try, 1));
  defmethod(js, ctor_obj, "withResolvers", 13, Ant_Function_Create(builtin_Promise_withResolvers));
  defmethod(js, ctor_obj, "all", 3, Ant_Function_CreateWithArity(builtin_Promise_all, 1));
  defmethod(js, ctor_obj, "allSettled", 10, Ant_Function_CreateWithArity(builtin_Promise_allSettled, 1));
  defmethod(js, ctor_obj, "race", 4, Ant_Function_CreateWithArity(builtin_Promise_race, 1));
  defmethod(js, ctor_obj, "any", 3, Ant_Function_CreateWithArity(builtin_Promise_any, 1));
}
