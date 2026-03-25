#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "silver/engine.h"
#include "descriptors.h"

#include "modules/iterator.h"
#include "modules/symbol.h"

enum {
  WRAP_MAP     = 0,
  WRAP_FILTER  = 1,
  WRAP_TAKE    = 2,
  WRAP_DROP    = 3,
  WRAP_FLATMAP = 4,
};

static ant_value_t g_wrap_iter_proto = 0;

static ant_value_t wrap_iter_next(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t self = js->this_val;
  ant_value_t source = js_get_slot(self, SLOT_DATA);
  ant_value_t state_v = js_get_slot(self, SLOT_ITER_STATE);
  
  uint32_t state = (vtype(state_v) == T_NUM) ? (uint32_t)js_getnum(state_v) : 0;
  uint32_t kind  = ITER_STATE_KIND(state);
  uint32_t count = ITER_STATE_INDEX(state);
  ant_value_t cb = js_get_slot(self, SLOT_CTOR);

  ant_value_t result = js_mkobj(js);
  ant_value_t next_fn = js_getprop_fallback(js, source, "next");

  for (;;) {
    if (kind == WRAP_FLATMAP) {
    ant_value_t inner = js_get_slot(self, SLOT_ENTRIES);
    
    if (vtype(inner) != T_UNDEF) {
      ant_value_t inner_next = js_getprop_fallback(js, inner, "next");
      ant_value_t inner_step = sv_vm_call(js->vm, js, inner_next, inner, NULL, 0, NULL, false);
      if (!is_err(inner_step)) {
      ant_value_t inner_done = js_getprop_fallback(js, inner_step, "done");
      if (!js_truthy(js, inner_done)) {
        js_set(js, result, "done", js_false);
        js_set(js, result, "value", js_getprop_fallback(js, inner_step, "value"));
        return result;
      }}
      
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
      
      js_set(js, result, "done", js_true);
      js_set(js, result, "value", js_mkundef());
      
      return result;
    }

    ant_value_t value = js_getprop_fallback(js, step, "value");

    switch (kind) {
    case WRAP_MAP: {
      ant_value_t out_val;
      if (is_callable(cb)) {
        ant_value_t call_args[2] = { value, js_mknum((double)count) };
        out_val = sv_vm_call(js->vm, js, cb, js_mkundef(), call_args, 2, NULL, false);
        if (is_err(out_val)) return out_val;
      } else out_val = value;

      count++;
      js_set_slot(self, SLOT_ITER_STATE, js_mknum((double)ITER_STATE_PACK(kind, count)));
      js_set(js, result, "done", js_false);
      js_set(js, result, "value", out_val);
      
      return result;
    }

    case WRAP_FILTER: {
      ant_value_t call_args[2] = { value, js_mknum((double)count) };
      ant_value_t test = sv_vm_call(js->vm, js, cb, js_mkundef(), call_args, 2, NULL, false);
      if (is_err(test)) return test;
      count++;
      js_set_slot(self, SLOT_ITER_STATE, js_mknum((double)ITER_STATE_PACK(kind, count)));
      if (js_truthy(js, test)) {
        js_set(js, result, "done", js_false);
        js_set(js, result, "value", value);
        return result;
      }
      continue;
    }

    case WRAP_TAKE: {
      uint32_t limit = (vtype(cb) == T_NUM) ? (uint32_t)js_getnum(cb) : 0;
      if (count >= limit) {
        js_set(js, result, "done", js_true);
        js_set(js, result, "value", js_mkundef());
        return result;
      }
      js_set_slot(self, SLOT_ITER_STATE, js_mknum((double)ITER_STATE_PACK(kind, count + 1)));
      js_set(js, result, "done", js_false);
      js_set(js, result, "value", value);
      return result;
    }

    case WRAP_DROP: {
      uint32_t limit = (vtype(cb) == T_NUM) ? (uint32_t)js_getnum(cb) : 0;
      count++;
      js_set_slot(self, SLOT_ITER_STATE, js_mknum((double)ITER_STATE_PACK(kind, count)));
      if (count <= limit) continue;
      js_set(js, result, "done", js_false);
      js_set(js, result, "value", value);
      return result;
    }

    case WRAP_FLATMAP: {
      ant_value_t call_args[2] = { value, js_mknum((double)count) };
      ant_value_t mapped = sv_vm_call(js->vm, js, cb, js_mkundef(), call_args, 2, NULL, false);
      if (is_err(mapped)) return mapped;
      count++;
      js_set_slot(self, SLOT_ITER_STATE, js_mknum((double)ITER_STATE_PACK(kind, count)));

      ant_value_t iter_fn = js_get_sym(js, mapped, get_iterator_sym());
      if (!is_callable(iter_fn)) {
        js_set(js, result, "done", js_false);
        js_set(js, result, "value", mapped);
        return result;
      }

      ant_value_t inner = sv_vm_call(js->vm, js, iter_fn, mapped, NULL, 0, NULL, false);
      if (is_err(inner)) return inner;

      ant_value_t inner_next = js_getprop_fallback(js, inner, "next");
      ant_value_t inner_step = sv_vm_call(js->vm, js, inner_next, inner, NULL, 0, NULL, false);
      if (is_err(inner_step)) return inner_step;
      ant_value_t inner_done = js_getprop_fallback(js, inner_step, "done");
      if (!js_truthy(js, inner_done)) {
        js_set_slot_wb(js, self, SLOT_ENTRIES, inner);
        js_set(js, result, "done", js_false);
        js_set(js, result, "value", js_getprop_fallback(js, inner_step, "value"));
        return result;
      }
      continue;
    }

    default:
      js_set(js, result, "done", js_false);
      js_set(js, result, "value", value);
      return result;
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

static ant_value_t iter_map(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Iterator.prototype.map requires a callable");
  ant_value_t source = get_source_iter(js);
  if (is_err(source)) return source;
  return make_wrap_iter(js, source, WRAP_MAP, args[0]);
}

static ant_value_t iter_filter(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Iterator.prototype.filter requires a callable");
  ant_value_t source = get_source_iter(js);
  if (is_err(source)) return source;
  return make_wrap_iter(js, source, WRAP_FILTER, args[0]);
}

static ant_value_t iter_take(ant_t *js, ant_value_t *args, int nargs) {
  double limit = (nargs >= 1 && vtype(args[0]) == T_NUM) ? js_getnum(args[0]) : 0;
  if (limit < 0) return js_mkerr(js, "Iterator.prototype.take requires a non-negative number");
  ant_value_t source = get_source_iter(js);
  if (is_err(source)) return source;
  return make_wrap_iter(js, source, WRAP_TAKE, js_mknum(limit));
}

static ant_value_t iter_drop(ant_t *js, ant_value_t *args, int nargs) {
  double limit = (nargs >= 1 && vtype(args[0]) == T_NUM) ? js_getnum(args[0]) : 0;
  if (limit < 0) return js_mkerr(js, "Iterator.prototype.drop requires a non-negative number");
  ant_value_t source = get_source_iter(js);
  if (is_err(source)) return source;
  return make_wrap_iter(js, source, WRAP_DROP, js_mknum(limit));
}

static ant_value_t iter_flatMap(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Iterator.prototype.flatMap requires a callable");
  ant_value_t source = get_source_iter(js);
  if (is_err(source)) return source;
  return make_wrap_iter(js, source, WRAP_FLATMAP, args[0]);
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
    ant_value_t call_args[2] = { value, js_mknum((double)counter++) };
    ant_value_t test = sv_vm_call(js->vm, js, fn, js_mkundef(), call_args, 2, NULL, false);
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
    ant_value_t call_args[2] = { value, js_mknum((double)counter++) };
    ant_value_t test = sv_vm_call(js->vm, js, fn, js_mkundef(), call_args, 2, NULL, false);
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
    ant_value_t call_args[2] = { value, js_mknum((double)counter++) };
    ant_value_t test = sv_vm_call(js->vm, js, fn, js_mkundef(), call_args, 2, NULL, false);
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
    ant_value_t call_args[2] = { value, js_mknum((double)counter++) };
    ant_value_t r = sv_vm_call(js->vm, js, fn, js_mkundef(), call_args, 2, NULL, false);
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
  js_set(js, ctor_obj, "from", js_mkfun(iter_from));

  ant_value_t ctor = js_obj_to_func(ctor_obj);
  js_set(js, iter_proto, "constructor", ctor);
  js_set(js, g, "Iterator", ctor);
}
