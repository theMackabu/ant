#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "ptr.h"
#include "silver/engine.h"
#include "descriptors.h"

#include "modules/assert.h"
#include "modules/iterator.h"
#include "modules/symbol.h"

enum {
  WRAP_MAP     = 0,
  WRAP_FILTER  = 1,
  WRAP_TAKE    = 2,
  WRAP_DROP    = 3,
  WRAP_FLATMAP = 4,
  WRAP_PASS    = 5,
  WRAP_FROM_SYNC = 6,
};

enum {
  ASYNC_TERM_EVERY   = 0,
  ASYNC_TERM_SOME    = 1,
  ASYNC_TERM_FIND    = 2,
  ASYNC_TERM_FOREACH = 3,
  ASYNC_TERM_REDUCE  = 4,
  ASYNC_TERM_TOARRAY = 5,
};

static ant_value_t g_wrap_iter_proto = 0;
static ant_value_t g_async_wrap_iter_proto = 0;

enum { ASYNC_TERMINAL_STATE_TAG = 0x41544954u }; // ATIT 

typedef struct {
  double index;
  int mode;
  bool has_acc;
} async_terminal_state_t;

static inline ant_value_t call_indexed_callback(ant_t *js, ant_value_t fn, ant_value_t value, double index) {
  ant_value_t call_args[2] = { value, js_mknum(index) };
  return sv_vm_call(js->vm, js, fn, js_mkundef(), call_args, 2, NULL, false);
}

static inline ant_value_t set_iter_result(ant_t *js, ant_value_t result, ant_value_t value, bool done) {
  js_set(js, result, "done", done ? js_true : js_false);
  js_set(js, result, "value", value);
  return result;
}

static ant_value_t wrap_iter_next(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t self = js->this_val;
  ant_value_t source = js_get_slot(self, SLOT_DATA);
  ant_value_t state_v = js_get_slot(self, SLOT_ITER_STATE);
  
  uint32_t state = (vtype(state_v) == T_NUM) ? (uint32_t)js_getnum(state_v) : 0;
  uint32_t kind  = ITER_STATE_KIND(state);
  uint32_t count = ITER_STATE_INDEX(state);
  
  ant_value_t result = js_mkobj(js);
  ant_value_t cb = js_get_slot(self, SLOT_CTOR);
  ant_value_t next_fn = js_getprop_fallback(js, source, "next");

  for (;;) {
    if (kind == WRAP_FLATMAP) {
    ant_value_t inner = js_get_slot(self, SLOT_ENTRIES);
    
    if (vtype(inner) != T_UNDEF) {
      ant_value_t inner_next = js_getprop_fallback(js, inner, "next");
      ant_value_t inner_step = sv_vm_call(js->vm, js, inner_next, inner, NULL, 0, NULL, false);
      if (!is_err(inner_step)) {
        ant_value_t inner_done = js_getprop_fallback(js, inner_step, "done");
        if (!js_truthy(js, inner_done)) return set_iter_result(
          js, result, js_getprop_fallback(js, inner_step, "value"
        ), false);
      }
      
      js_set_slot(self, SLOT_ENTRIES, js_mkundef());
    }}

    ant_value_t step;
    if (vtype(next_fn) == T_CFUNC) {
      ant_value_t old_this = js->this_val;
      js->this_val = source;
      step = js_as_cfunc(next_fn)(js, NULL, 0);
      js->this_val = old_this;
    } else step = sv_vm_call(js->vm, js, next_fn, source, NULL, 0, NULL, false);
    
    if (is_err(step)) return step;
    ant_value_t done = js_getprop_fallback(js, step, "done");
    
    if (js_truthy(js, done)) {
      if (kind == WRAP_FLATMAP) {
      ant_value_t inner = js_get_slot(self, SLOT_ENTRIES);
      if (vtype(inner) != T_UNDEF) {
        js_set_slot(self, SLOT_ENTRIES, js_mkundef());
      }}
      
      return set_iter_result(js, result, js_mkundef(), true);
    }

    ant_value_t value = js_getprop_fallback(js, step, "value");

    switch (kind) {
    case WRAP_MAP: {
      ant_value_t out_val;
      if (is_callable(cb)) {
        out_val = call_indexed_callback(js, cb, value, (double)count);
        if (is_err(out_val)) return out_val;
      } else out_val = value;

      count++;
      js_set_slot(self, SLOT_ITER_STATE, js_mknum((double)ITER_STATE_PACK(kind, count)));
      return set_iter_result(js, result, out_val, false);
    }

    case WRAP_FILTER: {
      ant_value_t test = call_indexed_callback(js, cb, value, (double)count);
      if (is_err(test)) return test;
      count++;
      js_set_slot(self, SLOT_ITER_STATE, js_mknum((double)ITER_STATE_PACK(kind, count)));
      if (js_truthy(js, test)) {
        return set_iter_result(js, result, value, false);
      }
      continue;
    }

    case WRAP_TAKE: {
      uint32_t limit = (vtype(cb) == T_NUM) ? (uint32_t)js_getnum(cb) : 0;
      if (count >= limit) {
        return set_iter_result(js, result, js_mkundef(), true);
      }
      js_set_slot(self, SLOT_ITER_STATE, js_mknum((double)ITER_STATE_PACK(kind, count + 1)));
      return set_iter_result(js, result, value, false);
    }

    case WRAP_DROP: {
      uint32_t limit = (vtype(cb) == T_NUM) ? (uint32_t)js_getnum(cb) : 0;
      count++;
      js_set_slot(self, SLOT_ITER_STATE, js_mknum((double)ITER_STATE_PACK(kind, count)));
      if (count <= limit) continue;
      return set_iter_result(js, result, value, false);
    }

    case WRAP_FLATMAP: {
      ant_value_t mapped = call_indexed_callback(js, cb, value, (double)count);
      if (is_err(mapped)) return mapped;
      count++;
      js_set_slot(self, SLOT_ITER_STATE, js_mknum((double)ITER_STATE_PACK(kind, count)));

      ant_value_t iter_fn = js_get_sym(js, mapped, get_iterator_sym());
      if (!is_callable(iter_fn)) {
        return set_iter_result(js, result, mapped, false);
      }

      ant_value_t inner = sv_vm_call(js->vm, js, iter_fn, mapped, NULL, 0, NULL, false);
      if (is_err(inner)) return inner;

      ant_value_t inner_next = js_getprop_fallback(js, inner, "next");
      ant_value_t inner_step = sv_vm_call(js->vm, js, inner_next, inner, NULL, 0, NULL, false);
      if (is_err(inner_step)) return inner_step;
      ant_value_t inner_done = js_getprop_fallback(js, inner_step, "done");
      if (!js_truthy(js, inner_done)) {
        js_set_slot_wb(js, self, SLOT_ENTRIES, inner);
        return set_iter_result(js, result, js_getprop_fallback(js, inner_step, "value"), false);
      }
      continue;
    }

    default:
      return set_iter_result(js, result, value, false);
    }
  }
}

static ant_value_t make_wrap_iter(ant_t *js, ant_value_t source, int kind, ant_value_t cb) {
  ant_value_t iter = js_mkobj(js);
  
  js_set_proto_init(iter, g_wrap_iter_proto);
  js_set_slot_wb(js, iter, SLOT_DATA, source);
  js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)ITER_STATE_PACK(kind, 0)));
  js_set_slot_wb(js, iter, SLOT_CTOR, cb);
  
  return iter;
}

static ant_value_t get_source_iter(ant_t *js) {
  ant_value_t self = js->this_val;
  ant_value_t next = js_getprop_fallback(js, self, "next");
  if (is_callable(next)) return self;
  
  ant_value_t iter_fn = js_get_sym(js, self, get_iterator_sym());
  if (!is_callable(iter_fn)) return js_mkerr_typed(js, JS_ERR_TYPE, "object is not iterable");
  
  return sv_vm_call(js->vm, js, iter_fn, self, NULL, 0, NULL, false);
}

static ant_value_t iter_make_helper(ant_t *js, int kind, ant_value_t cb) {
  ant_value_t source = get_source_iter(js);
  if (is_err(source)) return source;
  return make_wrap_iter(js, source, kind, cb);
}

static ant_value_t iter_make_callable_helper(
  ant_t *js,
  ant_value_t *args,
  int nargs,
  int kind,
  const char *method
) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s requires a callable", method);
  return iter_make_helper(js, kind, args[0]);
}

static ant_value_t iter_make_count_helper(
  ant_t *js, ant_value_t *args,
  int nargs, int kind,
  const char *method
) {
  double limit = (nargs >= 1 && vtype(args[0]) == T_NUM) ? js_getnum(args[0]) : 0;
  if (limit < 0) return js_mkerr(js, "%s requires a non-negative number", method);
  return iter_make_helper(js, kind, js_mknum(limit));
}

static ant_value_t iter_map(ant_t *js, ant_value_t *args, int nargs) {
  return iter_make_callable_helper(js, args, nargs, WRAP_MAP, "Iterator.prototype.map");
}

static ant_value_t iter_filter(ant_t *js, ant_value_t *args, int nargs) {
  return iter_make_callable_helper(js, args, nargs, WRAP_FILTER, "Iterator.prototype.filter");
}

static ant_value_t iter_take(ant_t *js, ant_value_t *args, int nargs) {
  return iter_make_count_helper(js, args, nargs, WRAP_TAKE, "Iterator.prototype.take");
}

static ant_value_t iter_drop(ant_t *js, ant_value_t *args, int nargs) {
  return iter_make_count_helper(js, args, nargs, WRAP_DROP, "Iterator.prototype.drop");
}

static ant_value_t iter_flatMap(ant_t *js, ant_value_t *args, int nargs) {
  return iter_make_callable_helper(js, args, nargs, WRAP_FLATMAP, "Iterator.prototype.flatMap");
}

static ant_value_t iter_every(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Iterator.prototype.every requires a callable");
  ant_value_t fn = args[0];

  js_iter_t it;
  if (!js_iter_open(js, js->this_val, &it))
    return js_mkerr_typed(js, JS_ERR_TYPE, "object is not iterable");

  ant_value_t value;
  uint32_t counter = 0;
  while (js_iter_next(js, &it, &value)) {
    ant_value_t test = call_indexed_callback(js, fn, value, (double)counter++);
    if (is_err(test)) { js_iter_close(js, &it); return test; }
    if (!js_truthy(js, test)) { js_iter_close(js, &it); return js_false; }
  }
  return js_true;
}

static ant_value_t iter_some(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Iterator.prototype.some requires a callable");
  ant_value_t fn = args[0];

  js_iter_t it;
  if (!js_iter_open(js, js->this_val, &it))
    return js_mkerr_typed(js, JS_ERR_TYPE, "object is not iterable");

  ant_value_t value;
  uint32_t counter = 0;
  while (js_iter_next(js, &it, &value)) {
    ant_value_t test = call_indexed_callback(js, fn, value, (double)counter++);
    if (is_err(test)) { js_iter_close(js, &it); return test; }
    if (js_truthy(js, test)) { js_iter_close(js, &it); return js_true; }
  }
  return js_false;
}

static ant_value_t iter_find(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Iterator.prototype.find requires a callable");
  ant_value_t fn = args[0];

  js_iter_t it;
  if (!js_iter_open(js, js->this_val, &it))
    return js_mkerr_typed(js, JS_ERR_TYPE, "object is not iterable");

  ant_value_t value;
  uint32_t counter = 0;
  while (js_iter_next(js, &it, &value)) {
    ant_value_t test = call_indexed_callback(js, fn, value, (double)counter++);
    if (is_err(test)) { js_iter_close(js, &it); return test; }
    if (js_truthy(js, test)) { js_iter_close(js, &it); return value; }
  }
  return js_mkundef();
}

static ant_value_t iter_forEach(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Iterator.prototype.forEach requires a callable");
  ant_value_t fn = args[0];

  js_iter_t it;
  if (!js_iter_open(js, js->this_val, &it))
    return js_mkerr_typed(js, JS_ERR_TYPE, "object is not iterable");

  ant_value_t value;
  uint32_t counter = 0;
  while (js_iter_next(js, &it, &value)) {
    ant_value_t r = call_indexed_callback(js, fn, value, (double)counter++);
    if (is_err(r)) { js_iter_close(js, &it); return r; }
  }
  return js_mkundef();
}

static ant_value_t iter_reduce(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Iterator.prototype.reduce requires a callable");
  ant_value_t fn = args[0];
  bool has_init = (nargs >= 2);

  js_iter_t it;
  if (!js_iter_open(js, js->this_val, &it))
    return js_mkerr_typed(js, JS_ERR_TYPE, "object is not iterable");

  ant_value_t acc = has_init ? args[1] : js_mkundef();
  bool first = !has_init;
  ant_value_t value;
  uint32_t counter = 0;

  while (js_iter_next(js, &it, &value)) {
    if (first) { acc = value; first = false; counter++; continue; }
    ant_value_t call_args[3] = { acc, value, js_mknum((double)counter++) };
    acc = sv_vm_call(js->vm, js, fn, js_mkundef(), call_args, 3, NULL, false);
    if (is_err(acc)) { js_iter_close(js, &it); return acc; }
  }

  if (first)
    return js_mkerr_typed(js, JS_ERR_TYPE, "reduce of empty iterator with no initial value");
  return acc;
}

static ant_value_t iter_toArray(ant_t *js, ant_value_t *args, int nargs) {
  js_iter_t it;
  if (!js_iter_open(js, js->this_val, &it))
    return js_mkerr_typed(js, JS_ERR_TYPE, "object is not iterable");

  ant_value_t arr = js_mkarr(js);
  ant_value_t value;
  while (js_iter_next(js, &it, &value))
    js_arr_push(js, arr, value);

  return arr;
}

static ant_value_t iter_from(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Iterator.from requires an argument");
  ant_value_t obj = args[0];

  ant_value_t next = js_getprop_fallback(js, obj, "next");
  if (is_callable(next)) {
    return make_wrap_iter(js, obj, WRAP_MAP, js_mkundef());
  }

  ant_value_t iter_fn = js_get_sym(js, obj, get_iterator_sym());
  if (!is_callable(iter_fn))
    return js_mkerr_typed(js, JS_ERR_TYPE, "object is not iterable");
    
  ant_value_t iterator = sv_vm_call(js->vm, js, iter_fn, obj, NULL, 0, NULL, false);
  if (is_err(iterator)) return iterator;
  
  return iterator;
}

static ant_value_t iter_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Iterator is not directly constructable");
    
  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, js->sym.iterator_proto);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  
  return obj;
}

static ant_value_t async_iter_ctor(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "AsyncIterator constructor requires 'new'");

  ant_value_t obj = js_mkobj(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, js->sym.async_iterator_proto);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  
  return obj;
}

static inline ant_value_t iter_result(ant_t *js, ant_value_t value, bool done) {
  ant_value_t result = js_mkobj(js);
  js_set(js, result, "done", done ? js_true : js_false);
  js_set(js, result, "value", value);
  return result;
}

static inline ant_value_t fulfilled_promise(ant_t *js, ant_value_t value) {
  ant_value_t promise = js_mkpromise(js);
  js_resolve_promise(js, promise, value);
  return promise;
}

static inline ant_value_t rejected_promise(ant_t *js, ant_value_t reason) {
  ant_value_t promise = js_mkpromise(js);
  js_reject_promise(js, promise, reason);
  return promise;
}

static inline ant_value_t promise_from_call_result(ant_t *js, ant_value_t result) {
  if (vtype(result) == T_PROMISE) return result;
  if (is_err(result)) return rejected_promise(js, result);
  return fulfilled_promise(js, result);
}

static ant_value_t async_iter_call_method(
  ant_t *js, ant_value_t receiver,
  const char *name, ant_value_t *args, int nargs, bool *missing
) {
  ant_value_t fn = js_getprop_fallback(js, receiver, name);
  if (missing) *missing = !is_callable(fn);
  if (!is_callable(fn)) return js_mkundef();
  return sv_vm_call(js->vm, js, fn, receiver, args, nargs, NULL, false);
}

static ant_value_t make_async_wrap_iter(ant_t *js, ant_value_t source, int kind, ant_value_t cb) {
  ant_value_t iter = js_mkobj(js);
  js_set_proto_init(iter, g_async_wrap_iter_proto);
  js_set_slot_wb(js, iter, SLOT_DATA, source);
  js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)ITER_STATE_PACK(kind, 0)));
  js_set_slot_wb(js, iter, SLOT_CTOR, cb);
  js_set_slot(iter, SLOT_ENTRIES, js_mkundef());
  return iter;
}

static ant_value_t async_wrap_advance(
  ant_t *js,
  ant_value_t iter,
  ant_value_t promise
);

static ant_value_t async_wrap_handle_step(
  ant_t *js,
  ant_value_t iter,
  ant_value_t promise,
  ant_value_t step
);

static ant_value_t async_wrap_handle_callback_result(
  ant_t *js,
  ant_value_t iter,
  ant_value_t promise,
  uint32_t kind,
  ant_value_t value,
  ant_value_t result
);

static ant_value_t async_wrap_on_step(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t iter = js_get(js, state, "iter");
  ant_value_t promise = js_get(js, state, "promise");
  ant_value_t step = nargs > 0 ? args[0] : js_mkundef();
  return async_wrap_handle_step(js, iter, promise, step);
}

static ant_value_t async_wrap_on_reject(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t promise = js_get(js, state, "promise");
  js_reject_promise(js, promise, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static ant_value_t async_wrap_on_callback(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t iter = js_get(js, state, "iter");
  ant_value_t promise = js_get(js, state, "promise");
  uint32_t kind = (uint32_t)js_getnum(js_get(js, state, "kind"));
  ant_value_t value = js_get(js, state, "value");
  ant_value_t result = nargs > 0 ? args[0] : js_mkundef();
  return async_wrap_handle_callback_result(js, iter, promise, kind, value, result);
}

static ant_value_t async_wrap_on_sync_value(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t promise = js_get(js, state, "promise");
  bool done = js_truthy(js, js_get(js, state, "done"));
  ant_value_t value = nargs > 0 ? args[0] : js_mkundef();
  js_resolve_promise(js, promise, iter_result(js, value, done));
  return js_mkundef();
}

static bool async_wrap_chain_step(ant_t *js, ant_value_t iter, ant_value_t promise, ant_value_t next_result) {
  if (is_err(next_result)) {
    js_reject_promise(js, promise, next_result);
    return true;
  }

  if (vtype(next_result) != T_PROMISE) {
    async_wrap_handle_step(js, iter, promise, next_result);
    return true;
  }

  ant_value_t state = js_mkobj(js);
  js_set(js, state, "iter", iter);
  js_set(js, state, "promise", promise);
  ant_value_t on_resolve = js_heavy_mkfun(js, async_wrap_on_step, state);
  ant_value_t on_reject = js_heavy_mkfun(js, async_wrap_on_reject, state);
  ant_value_t then_result = js_promise_then(js, next_result, on_resolve, on_reject);
  promise_mark_handled(then_result);
  return true;
}

static ant_value_t async_wrap_handle_inner_step(
  ant_t *js,
  ant_value_t iter,
  ant_value_t promise,
  ant_value_t step
) {
  if (!is_object_type(step)) {
    js_reject_promise(js, promise, js_mkerr_typed(js, JS_ERR_TYPE, "iterator result is not an object"));
    return js_mkundef();
  }

  if (!js_truthy(js, js_getprop_fallback(js, step, "done"))) {
    js_resolve_promise(js, promise, iter_result(js, js_getprop_fallback(js, step, "value"), false));
    return js_mkundef();
  }

  js_set_slot(iter, SLOT_ENTRIES, js_mkundef());
  async_wrap_advance(js, iter, promise);
  return js_mkundef();
}

static ant_value_t async_wrap_on_inner_step(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t iter = js_get(js, state, "iter");
  ant_value_t promise = js_get(js, state, "promise");
  ant_value_t step = nargs > 0 ? args[0] : js_mkundef();
  return async_wrap_handle_inner_step(js, iter, promise, step);
}

static ant_value_t async_wrap_advance_inner(ant_t *js, ant_value_t iter, ant_value_t promise) {
  ant_value_t inner = js_get_slot(iter, SLOT_ENTRIES);
  if (vtype(inner) == T_UNDEF || vtype(inner) == T_NULL) return async_wrap_advance(js, iter, promise);

  bool missing = false;
  ant_value_t next_result = async_iter_call_method(js, inner, "next", NULL, 0, &missing);
  if (missing) {
    js_set_slot(iter, SLOT_ENTRIES, js_mkundef());
    return async_wrap_advance(js, iter, promise);
  }

  if (is_err(next_result)) {
    js_reject_promise(js, promise, next_result);
    return js_mkundef();
  }

  if (vtype(next_result) == T_PROMISE) {
    ant_value_t state = js_mkobj(js);
    js_set(js, state, "iter", iter);
    js_set(js, state, "promise", promise);
    ant_value_t on_resolve = js_heavy_mkfun(js, async_wrap_on_inner_step, state);
    ant_value_t on_reject = js_heavy_mkfun(js, async_wrap_on_reject, state);
    ant_value_t then_result = js_promise_then(js, next_result, on_resolve, on_reject);
    promise_mark_handled(then_result);
    return js_mkundef();
  }

  return async_wrap_handle_inner_step(js, iter, promise, next_result);
}

static ant_value_t async_wrap_handle_step(
  ant_t *js,
  ant_value_t iter,
  ant_value_t promise,
  ant_value_t step
) {
  if (!is_object_type(step)) {
    js_reject_promise(js, promise, js_mkerr_typed(js, JS_ERR_TYPE, "iterator result is not an object"));
    return js_mkundef();
  }

  bool done = js_truthy(js, js_getprop_fallback(js, step, "done"));
  ant_value_t state_v = js_get_slot(iter, SLOT_ITER_STATE);
  
  uint32_t state = (vtype(state_v) == T_NUM) ? (uint32_t)js_getnum(state_v) : 0;
  uint32_t kind = ITER_STATE_KIND(state);
  uint32_t count = ITER_STATE_INDEX(state);
  ant_value_t value = js_getprop_fallback(js, step, "value");

  if (kind == WRAP_FROM_SYNC && vtype(value) == T_PROMISE) {
    ant_value_t state_obj = js_mkobj(js);
    js_set(js, state_obj, "promise", promise);
    js_set(js, state_obj, "done", done ? js_true : js_false);
    
    ant_value_t on_resolve = js_heavy_mkfun(js, async_wrap_on_sync_value, state_obj);
    ant_value_t on_reject = js_heavy_mkfun(js, async_wrap_on_reject, state_obj);
    ant_value_t then_result = js_promise_then(js, value, on_resolve, on_reject);
    
    promise_mark_handled(then_result);
    return js_mkundef();
  }

  if (done) {
    js_resolve_promise(
      js, promise,
      iter_result(js, kind == WRAP_FROM_SYNC ? value : js_mkundef(), true)
    );
    return js_mkundef();
  }

  ant_value_t cb = js_get_slot(iter, SLOT_CTOR);

  switch (kind) {
  case WRAP_MAP: {
    ant_value_t mapped = call_indexed_callback(js, cb, value, (double)count);
    if (is_err(mapped)) {
      js_reject_promise(js, promise, mapped);
      return js_mkundef();
    }
    js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)ITER_STATE_PACK(kind, count + 1)));
    return async_wrap_handle_callback_result(js, iter, promise, kind, value, mapped);
  }

  case WRAP_FILTER: {
    ant_value_t test = call_indexed_callback(js, cb, value, (double)count);
    if (is_err(test)) {
      js_reject_promise(js, promise, test);
      return js_mkundef();
    }
    js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)ITER_STATE_PACK(kind, count + 1)));
    return async_wrap_handle_callback_result(js, iter, promise, kind, value, test);
  }

  case WRAP_TAKE:
    js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)ITER_STATE_PACK(kind, count + 1)));
    js_resolve_promise(js, promise, iter_result(js, value, false));
    return js_mkundef();

  case WRAP_DROP: {
    double limit = (vtype(cb) == T_NUM) ? js_getnum(cb) : 0;
    js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)ITER_STATE_PACK(kind, count + 1)));
    if ((double)(count + 1) <= limit) async_wrap_advance(js, iter, promise);
    else js_resolve_promise(js, promise, iter_result(js, value, false));
    return js_mkundef();
  }

  case WRAP_FLATMAP: {
    ant_value_t mapped = call_indexed_callback(js, cb, value, (double)count);
    if (is_err(mapped)) {
      js_reject_promise(js, promise, mapped);
      return js_mkundef();
    }
    js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)ITER_STATE_PACK(kind, count + 1)));
    return async_wrap_handle_callback_result(js, iter, promise, kind, value, mapped);
  }

  default:
    js_resolve_promise(js, promise, iter_result(js, value, false));
    return js_mkundef();
  }
}

static ant_value_t async_wrap_handle_callback_result(
  ant_t *js,
  ant_value_t iter,
  ant_value_t promise,
  uint32_t kind,
  ant_value_t value,
  ant_value_t result
) {
  if (vtype(result) == T_PROMISE) {
    ant_value_t state = js_mkobj(js);
    js_set(js, state, "iter", iter);
    js_set(js, state, "promise", promise);
    js_set(js, state, "kind", js_mknum((double)kind));
    js_set(js, state, "value", value);
    ant_value_t on_resolve = js_heavy_mkfun(js, async_wrap_on_callback, state);
    ant_value_t on_reject = js_heavy_mkfun(js, async_wrap_on_reject, state);
    ant_value_t then_result = js_promise_then(js, result, on_resolve, on_reject);
    promise_mark_handled(then_result);
    return js_mkundef();
  }

  if (kind == WRAP_MAP) {
    js_resolve_promise(js, promise, iter_result(js, result, false));
    return js_mkundef();
  }

  if (kind == WRAP_FILTER) {
    if (js_truthy(js, result)) js_resolve_promise(js, promise, iter_result(js, value, false));
    else async_wrap_advance(js, iter, promise);
    return js_mkundef();
  }

  if (kind == WRAP_FLATMAP) {
    ant_value_t iter_fn = js_get_sym(js, result, get_asyncIterator_sym());
    if (!is_callable(iter_fn)) iter_fn = js_get_sym(js, result, get_iterator_sym());
    if (!is_callable(iter_fn)) {
      js_resolve_promise(js, promise, iter_result(js, result, false));
      return js_mkundef();
    }

    ant_value_t inner = sv_vm_call(js->vm, js, iter_fn, result, NULL, 0, NULL, false);
    if (is_err(inner)) {
      js_reject_promise(js, promise, inner);
      return js_mkundef();
    }
    js_set_slot_wb(js, iter, SLOT_ENTRIES, inner);
    return async_wrap_advance_inner(js, iter, promise);
  }

  js_resolve_promise(js, promise, iter_result(js, result, false));
  return js_mkundef();
}

static ant_value_t async_wrap_advance(ant_t *js, ant_value_t iter, ant_value_t promise) {
  ant_value_t state_v = js_get_slot(iter, SLOT_ITER_STATE);
  uint32_t state = (vtype(state_v) == T_NUM) ? (uint32_t)js_getnum(state_v) : 0;
  uint32_t kind = ITER_STATE_KIND(state);
  ant_value_t cb = js_get_slot(iter, SLOT_CTOR);

  if (kind == WRAP_TAKE) {
    double limit = (vtype(cb) == T_NUM) ? js_getnum(cb) : 0;
    if ((double)ITER_STATE_INDEX(state) >= limit) {
      js_resolve_promise(js, promise, iter_result(js, js_mkundef(), true));
      return js_mkundef();
    }
  }

  if (kind == WRAP_FLATMAP && vtype(js_get_slot(iter, SLOT_ENTRIES)) != T_UNDEF)
    return async_wrap_advance_inner(js, iter, promise);

  ant_value_t source = js_get_slot(iter, SLOT_DATA);
  bool missing = false;
  ant_value_t next_result = async_iter_call_method(js, source, "next", NULL, 0, &missing);
  if (missing) js_reject_promise(js, promise, js_mkerr_typed(js, JS_ERR_TYPE, "object is not async iterable"));
  else async_wrap_chain_step(js, iter, promise, next_result);
  return js_mkundef();
}

static ant_value_t async_wrap_next(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t promise = js_mkpromise(js);
  async_wrap_advance(js, js->this_val, promise);
  return promise;
}

static ant_value_t async_wrap_return(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t source = js_get_slot(js->this_val, SLOT_DATA);
  bool missing = false;
  ant_value_t result = async_iter_call_method(js, source, "return", args, nargs, &missing);
  if (missing) return fulfilled_promise(js, iter_result(js, nargs > 0 ? args[0] : js_mkundef(), true));
  ant_value_t state_v = js_get_slot(js->this_val, SLOT_ITER_STATE);
  uint32_t state = (vtype(state_v) == T_NUM) ? (uint32_t)js_getnum(state_v) : 0;
  if (ITER_STATE_KIND(state) == WRAP_FROM_SYNC) {
    ant_value_t promise = js_mkpromise(js);
    async_wrap_chain_step(js, js->this_val, promise, result);
    return promise;
  }
  return promise_from_call_result(js, result);
}

static ant_value_t async_wrap_throw(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t source = js_get_slot(js->this_val, SLOT_DATA);
  bool missing = false;
  ant_value_t result = async_iter_call_method(js, source, "throw", args, nargs, &missing);
  if (missing) return rejected_promise(js, nargs > 0 ? args[0] : js_mkundef());
  ant_value_t state_v = js_get_slot(js->this_val, SLOT_ITER_STATE);
  uint32_t state = (vtype(state_v) == T_NUM) ? (uint32_t)js_getnum(state_v) : 0;
  if (ITER_STATE_KIND(state) == WRAP_FROM_SYNC) {
    ant_value_t promise = js_mkpromise(js);
    async_wrap_chain_step(js, js->this_val, promise, result);
    return promise;
  }
  return promise_from_call_result(js, result);
}

static ant_value_t async_iter_from(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) == T_UNDEF || vtype(args[0]) == T_NULL)
    return js_mkerr_typed(js, JS_ERR_TYPE, "AsyncIterator.from requires an object");

  ant_value_t obj = args[0];
  ant_value_t iter_fn = js_get_sym(js, obj, get_asyncIterator_sym());
  if (is_callable(iter_fn)) {
    ant_value_t iterator = sv_vm_call(js->vm, js, iter_fn, obj, NULL, 0, NULL, false);
    if (is_err(iterator)) return iterator;
    return make_async_wrap_iter(js, iterator, WRAP_PASS, js_mkundef());
  }

  iter_fn = js_get_sym(js, obj, get_iterator_sym());
  if (is_callable(iter_fn)) {
    ant_value_t iterator = sv_vm_call(js->vm, js, iter_fn, obj, NULL, 0, NULL, false);
    if (is_err(iterator)) return iterator;
    return make_async_wrap_iter(js, iterator, WRAP_FROM_SYNC, js_mkundef());
  }

  ant_value_t next = js_getprop_fallback(js, obj, "next");
  if (is_callable(next)) return make_async_wrap_iter(js, obj, WRAP_FROM_SYNC, js_mkundef());

  return js_mkerr_typed(js, JS_ERR_TYPE, "object is not async iterable");
}

static ant_value_t get_async_source_iter(ant_t *js) {
  ant_value_t self = js->this_val;
  ant_value_t next = js_getprop_fallback(js, self, "next");
  if (is_callable(next)) return self;

  ant_value_t iter_fn = js_get_sym(js, self, get_asyncIterator_sym());
  if (!is_callable(iter_fn)) return js_mkerr_typed(js, JS_ERR_TYPE, "object is not async iterable");

  return sv_vm_call(js->vm, js, iter_fn, self, NULL, 0, NULL, false);
}

static ant_value_t async_iter_make_helper(ant_t *js, int kind, ant_value_t cb) {
  ant_value_t source = get_async_source_iter(js);
  if (is_err(source)) return source;
  return make_async_wrap_iter(js, source, kind, cb);
}

static ant_value_t async_iter_make_callable_helper(
  ant_t *js,
  ant_value_t *args,
  int nargs,
  int kind,
  const char *method
) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s requires a callable", method);
  return async_iter_make_helper(js, kind, args[0]);
}

static ant_value_t async_iter_make_count_helper(
  ant_t *js,
  ant_value_t *args,
  int nargs,
  int kind,
  const char *method
) {
  double limit = (nargs >= 1 && vtype(args[0]) == T_NUM) ? js_getnum(args[0]) : 0;
  if (limit < 0) return js_mkerr_typed(js, JS_ERR_TYPE, "%s requires a non-negative number", method);
  return async_iter_make_helper(js, kind, js_mknum(limit));
}

static ant_value_t async_iter_map(ant_t *js, ant_value_t *args, int nargs) {
  return async_iter_make_callable_helper(js, args, nargs, WRAP_MAP, "AsyncIterator.prototype.map");
}

static ant_value_t async_iter_filter(ant_t *js, ant_value_t *args, int nargs) {
  return async_iter_make_callable_helper(js, args, nargs, WRAP_FILTER, "AsyncIterator.prototype.filter");
}

static ant_value_t async_iter_take(ant_t *js, ant_value_t *args, int nargs) {
  return async_iter_make_count_helper(js, args, nargs, WRAP_TAKE, "AsyncIterator.prototype.take");
}

static ant_value_t async_iter_drop(ant_t *js, ant_value_t *args, int nargs) {
  return async_iter_make_count_helper(js, args, nargs, WRAP_DROP, "AsyncIterator.prototype.drop");
}

static ant_value_t async_iter_flatMap(ant_t *js, ant_value_t *args, int nargs) {
  return async_iter_make_callable_helper(js, args, nargs, WRAP_FLATMAP, "AsyncIterator.prototype.flatMap");
}

static ant_value_t async_terminal_advance(ant_t *js, ant_value_t state);
static ant_value_t async_terminal_finish_callback(ant_t *js, ant_value_t state, ant_value_t result);
static void async_terminal_close_and_reject(ant_t *js, ant_value_t state, ant_value_t reason);

static void async_terminal_state_finalize(ant_t *js, ant_object_t *obj) {
  free(obj->native.ptr);
  obj->native.ptr = NULL;
}

static inline async_terminal_state_t *async_terminal_state(ant_value_t state) {
  if (!js_check_native_tag(state, ASYNC_TERMINAL_STATE_TAG)) return NULL;
  return (async_terminal_state_t *)js_get_native_ptr(state);
}

static inline int async_terminal_mode(ant_value_t state) {
  async_terminal_state_t *st = async_terminal_state(state);
  return st ? st->mode : ASYNC_TERM_TOARRAY;
}

static inline double async_terminal_index(ant_value_t state) {
  async_terminal_state_t *st = async_terminal_state(state);
  return st ? st->index : 0;
}

static inline bool async_terminal_has_acc(ant_value_t state) {
  async_terminal_state_t *st = async_terminal_state(state);
  return st && st->has_acc;
}

static ant_value_t async_terminal_on_reject(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t promise = js_get_slot(state, SLOT_CTOR);
  js_reject_promise(js, promise, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static ant_value_t async_terminal_on_callback_reject(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  async_terminal_close_and_reject(js, state, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static ant_value_t async_terminal_on_callback(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t result = nargs > 0 ? args[0] : js_mkundef();
  return async_terminal_finish_callback(js, state, result);
}

static ant_value_t async_terminal_on_close(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t promise = js_get_slot(state, SLOT_CTOR);
  js_resolve_promise(js, promise, js_get_slot(state, SLOT_AUX));
  return js_mkundef();
}

static ant_value_t async_terminal_on_close_reject(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t promise = js_get_slot(state, SLOT_CTOR);
  js_reject_promise(js, promise, js_get_slot(state, SLOT_AUX));
  return js_mkundef();
}

static void async_terminal_close_and_resolve(ant_t *js, ant_value_t state, ant_value_t value) {
  ant_value_t promise = js_get_slot(state, SLOT_CTOR);
  ant_value_t iter = js_get_slot(state, SLOT_DATA);
  js_set_slot_wb(js, state, SLOT_AUX, value);

  bool missing = false;
  ant_value_t result = async_iter_call_method(js, iter, "return", NULL, 0, &missing);
  if (missing) {
    js_resolve_promise(js, promise, value);
    return;
  }
  
  if (is_err(result)) {
    js_reject_promise(js, promise, result);
    return;
  }
  
  if (vtype(result) == T_PROMISE) {
    ant_value_t on_resolve = js_heavy_mkfun(js, async_terminal_on_close, state);
    ant_value_t on_reject = js_heavy_mkfun(js, async_terminal_on_reject, state);
    ant_value_t then_result = js_promise_then(js, result, on_resolve, on_reject);
    promise_mark_handled(then_result);
    return;
  }

  js_resolve_promise(js, promise, value);
}

static void async_terminal_close_and_reject(ant_t *js, ant_value_t state, ant_value_t reason) {
  ant_value_t promise = js_get_slot(state, SLOT_CTOR);
  ant_value_t iter = js_get_slot(state, SLOT_DATA);
  js_set_slot_wb(js, state, SLOT_AUX, reason);

  bool missing = false;
  ant_value_t result = async_iter_call_method(js, iter, "return", NULL, 0, &missing);
  if (missing) {
    js_reject_promise(js, promise, reason);
    return;
  }
  
  if (is_err(result)) {
    js_reject_promise(js, promise, result);
    return;
  }
  
  if (vtype(result) == T_PROMISE) {
    ant_value_t on_resolve = js_heavy_mkfun(js, async_terminal_on_close_reject, state);
    ant_value_t on_reject = js_heavy_mkfun(js, async_terminal_on_reject, state);
    ant_value_t then_result = js_promise_then(js, result, on_resolve, on_reject);
    promise_mark_handled(then_result);
    return;
  }

  js_reject_promise(js, promise, reason);
}

static bool async_terminal_apply_callback_result(ant_t *js, ant_value_t state, ant_value_t result) {
  int mode = async_terminal_mode(state);

  if (mode == ASYNC_TERM_REDUCE) {
    js_set_slot_wb(js, state, SLOT_SET, result);
    async_terminal_state_t *st = async_terminal_state(state);
    if (st) st->has_acc = true;
    return true;
  }

  switch (mode) {
  case ASYNC_TERM_EVERY:
    if (!js_truthy(js, result)) {
      async_terminal_close_and_resolve(js, state, js_false);
      return false;
    }
    return true;
  case ASYNC_TERM_SOME:
    if (js_truthy(js, result)) {
      async_terminal_close_and_resolve(js, state, js_true);
      return false;
    }
    return true;
  case ASYNC_TERM_FIND:
    if (js_truthy(js, result)) {
      async_terminal_close_and_resolve(js, state, js_get_slot(state, SLOT_AUX));
      return false;
    }
    return true;
  default:
    return true;
  }
}

static bool async_terminal_handle_step(ant_t *js, ant_value_t state, ant_value_t step) {
  ant_value_t promise = js_get_slot(state, SLOT_CTOR);

  if (!is_object_type(step)) {
    js_reject_promise(js, promise, js_mkerr_typed(js, JS_ERR_TYPE, "iterator result is not an object"));
    return false;
  }

  int mode = async_terminal_mode(state);
  ant_value_t done = js_getprop_fallback(js, step, "done");
  ant_value_t value = js_getprop_fallback(js, step, "value");

  if (js_truthy(js, done)) {
    switch (mode) {
    case ASYNC_TERM_EVERY: js_resolve_promise(js, promise, js_true); break;
    case ASYNC_TERM_SOME: js_resolve_promise(js, promise, js_false); break;
    
    case ASYNC_TERM_FIND: js_resolve_promise(js, promise, js_mkundef());    break;
    case ASYNC_TERM_FOREACH: js_resolve_promise(js, promise, js_mkundef()); break;
    
    case ASYNC_TERM_REDUCE:
      if (!async_terminal_has_acc(state)) {
        js_reject_promise(js, promise, js_mkerr_typed(js, JS_ERR_TYPE, "reduce of empty iterator with no initial value"));
      } else js_resolve_promise(js, promise, js_get_slot(state, SLOT_SET));
      break;
    
    default:
      js_resolve_promise(js, promise, js_get_slot(state, SLOT_ENTRIES));
      break;
    }
    
    return false;
  }

  double index = async_terminal_index(state);
  async_terminal_state_t *st = async_terminal_state(state);
  if (st) st->index = index + 1;

  if (mode == ASYNC_TERM_TOARRAY) {
    js_arr_push(js, js_get_slot(state, SLOT_ENTRIES), value);
    return true;
  }

  ant_value_t fn = js_get_slot(state, SLOT_MAP);
  if (!is_callable(fn)) {
    js_reject_promise(js, promise, js_mkerr_typed(js, JS_ERR_TYPE, "callback is not callable"));
    return false;
  }

  if (mode == ASYNC_TERM_REDUCE) {
    if (!async_terminal_has_acc(state)) {
      js_set_slot_wb(js, state, SLOT_SET, value);
      if (st) st->has_acc = true;
      return true;
    }
    
    ant_value_t call_args[3] = { js_get_slot(state, SLOT_SET), value, js_mknum(index) };
    ant_value_t next_acc = sv_vm_call(js->vm, js, fn, js_mkundef(), call_args, 3, NULL, false);
    if (is_err(next_acc)) {
      async_terminal_close_and_reject(js, state, next_acc);
      return false;
    }
    
    if (vtype(next_acc) == T_PROMISE) {
      ant_value_t on_resolve = js_heavy_mkfun(js, async_terminal_on_callback, state);
      ant_value_t on_reject = js_heavy_mkfun(js, async_terminal_on_callback_reject, state);
      ant_value_t then_result = js_promise_then(js, next_acc, on_resolve, on_reject);
      promise_mark_handled(then_result);
      return false;
    }
    return async_terminal_apply_callback_result(js, state, next_acc);
  }

  ant_value_t result = call_indexed_callback(js, fn, value, (double)index);
  if (is_err(result)) {
    async_terminal_close_and_reject(js, state, result);
    return false;
  }
  
  js_set_slot_wb(js, state, SLOT_AUX, value);
  if (vtype(result) == T_PROMISE) {
    ant_value_t on_resolve = js_heavy_mkfun(js, async_terminal_on_callback, state);
    ant_value_t on_reject = js_heavy_mkfun(js, async_terminal_on_callback_reject, state);
    ant_value_t then_result = js_promise_then(js, result, on_resolve, on_reject);
    promise_mark_handled(then_result);
    return false;
  }
  
  return async_terminal_apply_callback_result(js, state, result);
}

static ant_value_t async_terminal_on_step(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = js_get_slot(js_getcurrentfunc(js), SLOT_DATA);
  ant_value_t step = nargs > 0 ? args[0] : js_mkundef();
  if (async_terminal_handle_step(js, state, step)) return async_terminal_advance(js, state);
  return js_mkundef();
}

static ant_value_t async_terminal_finish_callback(
  ant_t *js,
  ant_value_t state,
  ant_value_t result
) {
  if (vtype(result) == T_PROMISE) {
    ant_value_t on_resolve = js_heavy_mkfun(js, async_terminal_on_callback, state);
    ant_value_t on_reject = js_heavy_mkfun(js, async_terminal_on_callback_reject, state);
    ant_value_t then_result = js_promise_then(js, result, on_resolve, on_reject);
    promise_mark_handled(then_result);
    return js_mkundef();
  }

  if (async_terminal_apply_callback_result(js, state, result)) return async_terminal_advance(js, state);
  return js_mkundef();
}

static ant_value_t async_terminal_advance(ant_t *js, ant_value_t state) {
for (;;) {
  ant_value_t iter = js_get_slot(state, SLOT_DATA);
  ant_value_t promise = js_get_slot(state, SLOT_CTOR);
  bool missing = false;
  ant_value_t next_result = async_iter_call_method(js, iter, "next", NULL, 0, &missing);
  
  if (missing) {
    js_reject_promise(js, promise, js_mkerr_typed(js, JS_ERR_TYPE, "object is not async iterable"));
    return js_mkundef();
  }
  
  if (is_err(next_result)) {
    js_reject_promise(js, promise, next_result);
    return js_mkundef();
  }
  
  if (vtype(next_result) == T_PROMISE) {
    ant_value_t on_resolve = js_heavy_mkfun(js, async_terminal_on_step, state);
    ant_value_t on_reject = js_heavy_mkfun(js, async_terminal_on_reject, state);
    ant_value_t then_result = js_promise_then(js, next_result, on_resolve, on_reject);
    promise_mark_handled(then_result);
    return js_mkundef();
  }
  
  if (!async_terminal_handle_step(js, state, next_result)) return js_mkundef();
}}

static ant_value_t async_iter_terminal(ant_t *js, ant_value_t *args, int nargs, int mode) {
  if (mode != ASYNC_TERM_TOARRAY && (nargs < 1 || !is_callable(args[0])))
    return js_mkerr_typed(js, JS_ERR_TYPE, "AsyncIterator helper requires a callable");

  ant_value_t iter = get_async_source_iter(js);
  if (is_err(iter)) return rejected_promise(js, iter);

  ant_value_t promise = js_mkpromise(js);
  ant_value_t state = js_mkobj(js);
  async_terminal_state_t *st = calloc(1, sizeof(*st));
  
  if (!st) {
    js_reject_promise(js, promise, js_mkerr(js, "out of memory"));
    return promise;
  }
  
  st->mode = mode;
  st->index = 0;
  st->has_acc = (mode == ASYNC_TERM_REDUCE && nargs > 1);
  
  js_set_native_tag(state, ASYNC_TERMINAL_STATE_TAG);
  js_set_native_ptr(state, st);
  js_set_finalizer(state, async_terminal_state_finalize);

  js_set_slot_wb(js, state, SLOT_DATA, iter);
  js_set_slot_wb(js, state, SLOT_CTOR, promise);
  js_set_slot_wb(js, state, SLOT_MAP, mode == ASYNC_TERM_TOARRAY ? js_mkundef() : args[0]);
  js_set_slot_wb(js, state, SLOT_ENTRIES, js_mkarr(js));
  js_set_slot_wb(js, state, SLOT_SET, (mode == ASYNC_TERM_REDUCE && nargs > 1) ? args[1] : js_mkundef());
  js_set_slot(state, SLOT_AUX, js_mkundef());

  async_terminal_advance(js, state);
  return promise;
}

static ant_value_t async_iter_every(ant_t *js, ant_value_t *args, int nargs) {
  return async_iter_terminal(js, args, nargs, ASYNC_TERM_EVERY);
}

static ant_value_t async_iter_some(ant_t *js, ant_value_t *args, int nargs) {
  return async_iter_terminal(js, args, nargs, ASYNC_TERM_SOME);
}

static ant_value_t async_iter_find(ant_t *js, ant_value_t *args, int nargs) {
  return async_iter_terminal(js, args, nargs, ASYNC_TERM_FIND);
}

static ant_value_t async_iter_forEach(ant_t *js, ant_value_t *args, int nargs) {
  return async_iter_terminal(js, args, nargs, ASYNC_TERM_FOREACH);
}

static ant_value_t async_iter_reduce(ant_t *js, ant_value_t *args, int nargs) {
  return async_iter_terminal(js, args, nargs, ASYNC_TERM_REDUCE);
}

static ant_value_t async_iter_toArray(ant_t *js, ant_value_t *args, int nargs) {
  return async_iter_terminal(js, args, nargs, ASYNC_TERM_TOARRAY);
}

void init_iterator_module(void) {
  ant_t *js = rt->js;
  ant_value_t g = js_glob(js);
  ant_value_t iter_proto = js->sym.iterator_proto;

  g_wrap_iter_proto = js_mkobj(js);
  js_set_proto_init(g_wrap_iter_proto, iter_proto);
  js_set(js, g_wrap_iter_proto, "next", js_mkfun(wrap_iter_next));

  js_set(js, iter_proto, "map",     js_mkfun(iter_map));
  js_set(js, iter_proto, "filter",  js_mkfun(iter_filter));
  js_set(js, iter_proto, "take",    js_mkfun(iter_take));
  js_set(js, iter_proto, "drop",    js_mkfun(iter_drop));
  js_set(js, iter_proto, "flatMap", js_mkfun(iter_flatMap));
  js_set(js, iter_proto, "every",   js_mkfun(iter_every));
  js_set(js, iter_proto, "some",    js_mkfun(iter_some));
  js_set(js, iter_proto, "find",    js_mkfun(iter_find));
  js_set(js, iter_proto, "forEach", js_mkfun(iter_forEach));
  js_set(js, iter_proto, "reduce",  js_mkfun(iter_reduce));
  js_set(js, iter_proto, "toArray", js_mkfun(iter_toArray));
  js_set_sym(js, iter_proto, get_toStringTag_sym(), js_mkstr(js, "Iterator", 8));

  ant_value_t ctor_obj = js_mkobj(js);
  js_set_slot(ctor_obj, SLOT_CFUNC, js_mkfun(iter_ctor));
  js_mkprop_fast(js, ctor_obj, "prototype", 9, iter_proto);
  js_mkprop_fast(js, ctor_obj, "name", 4, js_mkstr(js, "Iterator", 8));
  js_set_descriptor(js, ctor_obj, "name", 4, 0);
  
  ant_value_t ctor = js_obj_to_func(ctor_obj);
  js_set(js, ctor, "from", js_mkfun(iter_from));
  js_set(js, iter_proto, "constructor", ctor);
  js_set(js, g, "Iterator", ctor);

  ant_value_t async_iter_proto = js_mkobj(js);
  js->sym.async_iterator_proto = async_iter_proto;
  js_set_proto_init(async_iter_proto, js->sym.object_proto);
  js_set_sym(js, async_iter_proto, get_asyncIterator_sym(), js_mkfun(sym_this_cb));
  js_set_sym(js, async_iter_proto, get_toStringTag_sym(), js_mkstr(js, "AsyncIterator", 13));

  ant_value_t async_ctor_obj = js_mkobj(js);
  js_set_slot(async_ctor_obj, SLOT_CFUNC, js_mkfun(async_iter_ctor));
  js_mkprop_fast(js, async_ctor_obj, "prototype", 9, async_iter_proto);
  js_mkprop_fast(js, async_ctor_obj, "name", 4, js_mkstr(js, "AsyncIterator", 13));
  js_set_descriptor(js, async_ctor_obj, "name", 4, 0);
  
  ant_value_t async_ctor = js_obj_to_func(async_ctor_obj);
  js_set(js, async_iter_proto, "constructor", async_ctor);
  js_set(js, g, "AsyncIterator", async_ctor);

  g_async_wrap_iter_proto = js_mkobj(js);
  js_set_proto_init(g_async_wrap_iter_proto, async_iter_proto);
  js_set(js, g_async_wrap_iter_proto, "next", js_mkfun(async_wrap_next));
  js_set(js, g_async_wrap_iter_proto, "return", js_mkfun(async_wrap_return));
  js_set(js, g_async_wrap_iter_proto, "throw", js_mkfun(async_wrap_throw));
}

void init_async_iterator_helpers(void) {
  ant_t *js = rt->js;
  ant_value_t g = js_glob(js);
  
  ant_value_t ctor = js_get(js, g, "AsyncIterator");
  ant_value_t proto = js->sym.async_iterator_proto;

  js_set(js, ctor, "from", js_mkfun(async_iter_from));
  js_set(js, proto, "map", js_mkfun(async_iter_map));
  js_set(js, proto, "filter", js_mkfun(async_iter_filter));
  js_set(js, proto, "take", js_mkfun(async_iter_take));
  js_set(js, proto, "drop", js_mkfun(async_iter_drop));
  js_set(js, proto, "flatMap", js_mkfun(async_iter_flatMap));
  js_set(js, proto, "every", js_mkfun(async_iter_every));
  js_set(js, proto, "some", js_mkfun(async_iter_some));
  js_set(js, proto, "find", js_mkfun(async_iter_find));
  js_set(js, proto, "forEach", js_mkfun(async_iter_forEach));
  js_set(js, proto, "reduce", js_mkfun(async_iter_reduce));
  js_set(js, proto, "toArray", js_mkfun(async_iter_toArray));
}
