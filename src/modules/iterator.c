#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "utf8.h"
#include "gc/roots.h"
#include "gc/modules.h"

#include "ant.h"
#include "cfunc.h"
#include "errors.h"
#include "internal.h"
#include "protectors.h"
#include "ptr.h"
#include "silver/call.h"
#include "descriptors.h"

#include "modules/assert.h"
#include "modules/iterator.h"
#include "modules/generator.h"
#include "modules/symbol.h"

static constexpr uint32_t STRING_ITERATOR_NATIVE_TAG = Ant_NativeTag("STRI");

ant_value_t Ant_Iterator_CreateResult(ant_t *js, ant_value_t value, bool done) {
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, value);

  ant_value_t seed = js->mutable_roots.iterator_result_template;
  if (vtype(seed) != kTypeObject) {
    seed = Ant_Object_Create(js);
    if (is_err(seed)) {
      GC_ROOT_RESTORE(js, mark);
      return seed;
    }

    GC_ROOT_PIN(js, seed);
    js_mkprop_fast(js, seed, "value", 5, js_mkundef());
    js_mkprop_fast(js, seed, "done", 4, js_false);

    if (Ant_Exception_Pending(js)) {
      GC_ROOT_RESTORE(js, mark);
      return Ant_Exception_Current(js);
    }

    js->mutable_roots.iterator_result_template = seed;
  }

  ant_value_t result = js_mkobj_from_template(js, seed);
  if (!is_err(result)) {
    ant_object_t *obj = js_obj_ptr(result);
    ant_object_prop_set_unchecked(obj, 0, value);
    ant_object_prop_set_unchecked(obj, 1, js_bool(done));
    gc_write_barrier(js, obj, value);
  }

  GC_ROOT_RESTORE(js, mark);
  return result;
}

ant_value_t js_iter_result(ant_t *js, bool has_value, ant_value_t value) {
  return Ant_Iterator_CreateResult(js, has_value ? value : js_mkundef(), !has_value);
}

static ant_value_t get_iterator_prototype(ant_t *js) {
  if (vtype(js->sym.iterator_proto) == kTypeObject) return js->sym.iterator_proto;

  js->sym.iterator_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->sym.iterator_proto, js->sym.object_proto);
  
  defmethod_sym(js, js->sym.iterator_proto, js->sym.iterator_sym, Ant_Function_Create(sym_this_cb));
  ant_iterator_protector_holder_mark(js->sym.iterator_proto);

  return js->sym.iterator_proto;
}

// TODO: optimize?
static inline ant_value_t iter_get_element(ant_t *js, ant_value_t obj, uint32_t idx) {
  if (vtype(obj) == kTypeArray) return js_arr_get(js, obj, (ant_offset_t)idx);
  char buf[16]; snprintf(buf, sizeof(buf), "%u", idx);
  return js_get(js, obj, buf);
}

// TODO: optimize?
static inline ant_offset_t iter_get_length(ant_t *js, ant_value_t obj) {
  if (vtype(obj) == kTypeArray) return js_arr_len(js, obj);
  ant_value_t v = js_get(js, obj, "length");
  double len = vtype(v) == kTypeNumber ? js_getnum(v) : js_to_number(js, v);
  if (!(len > 0)) return 0;
  return len > 9007199254740991.0 ? (ant_offset_t)9007199254740991.0 : (ant_offset_t)len;
}

static bool advance_array(ant_t *js, iterator_t *it, ant_value_t *out) {
  ant_value_t iter = it->iterator;
  ant_value_t array = js_get_slot(iter, SLOT_DATA);
  ant_value_t state_v = js_get_slot(iter, SLOT_ITER_STATE);

  uint32_t state = (vtype(state_v) == kTypeNumber) 
    ? (uint32_t)js_getnum(state_v) : 0;
  
  uint32_t kind = iter_state_kind(state);
  uint32_t idx = iter_state_index(state);
  
  // array-likes read through getters, which may throw; a pending exception
  // ends the iteration instead of passing the error on as a value
  ant_offset_t len = iter_get_length(js, array);
  if (Ant_Exception_Pending(js) || idx >= (uint32_t)len) return false;

  switch (kind) {
    case ARR_ITER_KEYS:
      *out = js_mknum((double)idx);
      break;
    case ARR_ITER_ENTRIES: {
      ant_value_t elem = iter_get_element(js, array, idx);
      if (Ant_Exception_Pending(js)) return false;
      GC_ROOT_SAVE(mark, js);
      GC_ROOT_PIN(js, elem);
      ant_value_t pair = js_mkarr(js);
      js_arr_push(js, pair, js_mknum((double)idx));
      js_arr_push(js, pair, elem);
      GC_ROOT_RESTORE(js, mark);
      *out = pair;
      break;
    }
    default: {
      ant_value_t elem = iter_get_element(js, array, idx);
      if (Ant_Exception_Pending(js)) return false;
      *out = elem;
      break;
    }
  }

  js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)iter_state_pack(kind, idx + 1)));
  
  return true;
}

static bool string_iter_step(ant_t *js, iterator_t *it, ant_value_t *out) {
  ant_value_t iter = it->iterator;
  ant_object_t *obj = js_obj_ptr(iter);
  size_t idx = (size_t)(uintptr_t)js_get_native(iter, STRING_ITERATOR_NATIVE_TAG);

  size_t slen;
  char *s = js_getstr(js, obj->u.data.value, &slen);
  if (idx >= slen) return false;

  size_t char_bytes = utf8_char_len_at(s, slen, idx);
  *out = js_mkstr(js, s + idx, (ant_offset_t)char_bytes);
  js_set_native(iter, (void *)(uintptr_t)(idx + char_bytes), STRING_ITERATOR_NATIVE_TAG);
  
  return true;
}

static bool advance_string(ant_t *js, iterator_t *it, ant_value_t *out) {
  if (js_check_native_tag(it->iterator, STRING_ITERATOR_NATIVE_TAG)) return string_iter_step(js, it, out);
  js_mkerr_typed(js, JS_ERR_TYPE, "String Iterator next called on incompatible receiver");
  return false;
}

static ant_value_t arr_iter_next(ant_params_t) {
  return js_iter_next_result(js, advance_array);
}

// TODO: cleanup
bool js_iter_is_array_values(ant_value_t iterator, ant_value_t next, ant_value_t source) {
  return vtype(source) == kTypeArray && vtype(iterator) == kTypeObject &&
    vtype(next) == kTypeBuiltin && js_as_cfunc(next) == arr_iter_next &&
    js_get_slot(iterator, SLOT_DATA) == source &&
    js_get_slot(iterator, SLOT_ITER_STATE) == js_mknum(iter_state_pack(ARR_ITER_VALUES, 0));
}

static ant_value_t get_array_iterator_prototype(ant_t *js) {
  if (vtype(js->sym.array_iterator_proto) == kTypeObject) return js->sym.array_iterator_proto;

  ant_value_t iterator_proto = get_iterator_prototype(js);
  js->sym.array_iterator_proto = Ant_Object_CreateBare(js);
  
  bool iteration_intact = ant_protector_intact(js, ANT_PROTECTOR_ARRAY_ITERATION);
  js->sym.array_iterator_next = defmethod(js, js->sym.array_iterator_proto, "next", 4, Ant_Function_Create(arr_iter_next));
  
  if (iteration_intact) ant_protector_rearm(js, ANT_PROTECTOR_ARRAY_ITERATION);
  ant_iterator_protector_holder_mark(js->sym.array_iterator_proto);
  
  mkprop(js, js->sym.array_iterator_proto, js->sym.toStringTag_sym, ANT_STRING("Array Iterator"), ANT_PROP_ATTR_CONFIGURABLE);
  js_set_proto_init(js->sym.array_iterator_proto, iterator_proto);

  return js->sym.array_iterator_proto;
}

ant_value_t make_array_iterator(ant_t *js, ant_value_t array, array_iter_kind_t kind) {
  ant_value_t iter = Ant_Object_CreateBare(js);
  
  js_set_slot_wb(js, iter, SLOT_DATA, array);
  js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)iter_state_pack(kind, 0)));
  js_set_proto_init(iter, get_array_iterator_prototype(js));
  
  return iter;
}

ant_value_t js_array_iterator_at(ant_t *js, ant_value_t array, uint32_t index) {
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, array);
  
  ant_value_t iter = make_array_iterator(js, array, ARR_ITER_VALUES);
  if (!is_err(iter)) js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)iter_state_pack(ARR_ITER_VALUES, index)));
  GC_ROOT_RESTORE(js, mark);
  
  return iter;
}

static ant_value_t str_iter_next(ant_params_t) {
  if (!js_check_native_tag(js->this_val, STRING_ITERATOR_NATIVE_TAG))
    return js_mkerr_typed(js, JS_ERR_TYPE, "String Iterator next called on incompatible receiver");
  return js_iter_next_result(js, string_iter_step);
}

static ant_value_t get_string_iterator_prototype(ant_t *js) {
  if (vtype(js->sym.string_iterator_proto) == kTypeObject) return js->sym.string_iterator_proto;

  ant_value_t iterator_proto = get_iterator_prototype(js);
  js->sym.string_iterator_proto = Ant_Object_CreateBare(js);
  
  bool iteration_intact = ant_protector_intact(js, ANT_PROTECTOR_STRING_ITERATION);
  defmethod(js, js->sym.string_iterator_proto, "next", 4, Ant_Function_Create(str_iter_next));
  
  if (iteration_intact) ant_protector_rearm(js, ANT_PROTECTOR_STRING_ITERATION);
  mkprop(js, js->sym.string_iterator_proto, js->sym.toStringTag_sym, ANT_STRING("String Iterator"), ANT_PROP_ATTR_CONFIGURABLE);
  
  js_set_proto_init(js->sym.string_iterator_proto, iterator_proto);
  ant_iterator_protector_holder_mark(js->sym.string_iterator_proto);

  return js->sym.string_iterator_proto;
}

static ant_value_t string_iterator(ant_params_t) {
  ant_value_t recv = js->this_val;
  if (is_null(recv) || is_undefined(recv)) return js_mkerr_typed(
    js, JS_ERR_TYPE, "String.prototype[Symbol.iterator] called on null or undefined"
  );

  ant_value_t str = js_template_to_string(js, recv);
  if (is_err(str)) return str;

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, str);
  ant_value_t iter = Ant_Object_CreateBare(js);

  js_set_native(iter, (void *)(uintptr_t)0, STRING_ITERATOR_NATIVE_TAG);
  js_set_slot_wb(js, iter, SLOT_DATA, str);
  js_set_proto_init(iter, get_string_iterator_prototype(js));
  GC_ROOT_RESTORE(js, mark);

  return iter;
}

ant_value_t js_string_iterator_at(ant_t *js, ant_value_t str, uint32_t offset) {
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, str);
  ant_value_t iter = Ant_Object_CreateBare(js);
  
  if (!is_err(iter)) {
    js_set_slot_wb(js, iter, SLOT_DATA, str);
    js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)offset));
    js_set_proto_init(iter, get_string_iterator_prototype(js));
  }
  
  GC_ROOT_RESTORE(js, mark);
  return iter;
}

typedef struct ant_iterator_entry {
  ant_value_t proto;
  ant_value_t next;
  js_iter_advance_fn advance;
} ant_iterator_entry_t;

void js_iter_register_advance(ant_t *js, ant_value_t proto, js_iter_advance_fn fn) {
  ant_value_t next = js_get(js, proto, "next");
  if (is_err(next)) return;

  for (size_t i = 0; i < js->iterators.len; i++) {
    if (js->iterators.entries[i].proto != proto) continue;
    js->iterators.entries[i] = (ant_iterator_entry_t){proto, next, fn};
    return;
  }

  if (js->iterators.len == js->iterators.cap) {
    size_t cap = js->iterators.cap ? js->iterators.cap * 2 : 8;
    if (cap < js->iterators.cap || cap > SIZE_MAX / sizeof(ant_iterator_entry_t)) return;

    ant_iterator_entry_t *entries = realloc(js->iterators.entries, cap * sizeof(*entries));
    if (!entries) return;

    js->iterators.entries = entries;
    js->iterators.cap = cap;
  }

  js->iterators.entries[js->iterators.len++] = (ant_iterator_entry_t){proto, next, fn};
}

void gc_mark_iterator_registry(ant_t *js, const ant_iterator_registry_t *registry, gc_mark_fn mark) {
  for (size_t i = 0; i < registry->len; i++) {
    mark(js, registry->entries[i].proto);
    mark(js, registry->entries[i].next);
  }
}

void gc_mark_iterators(ant_t *js, gc_mark_fn mark) {
  gc_mark_iterator_registry(js, &js->iterators, mark);
}

void cleanup_iterator_module(ant_t *js) {
  free(js->iterators.entries);
  js->iterators.entries = NULL;
  js->iterators.len = js->iterators.cap = 0;
}

static inline ant_value_t iterator_get_method(
  ant_t *js, ant_value_t iterator, const char *name, size_t len
) {
  const char *key = intern_find(js, name, len);
  ant_value_t current = iterator;

  for (int depth = 0; depth < MAX_PROTO_CHAIN_DEPTH; depth++) {
    if (vtype(current) != kTypeObject) break;
    ant_object_t *obj = js_obj_ptr(current);
    if (!obj || obj->flags.is_exotic) break;

    int32_t slot = key ? ant_shape_lookup_interned(obj->shape, key) : -1;
    if (slot >= 0) {
      const ant_shape_prop_t *prop = ant_shape_prop_at(obj->shape, (uint32_t)slot);
      if (!prop || prop->has_getter || prop->has_setter || (uint32_t)slot >= obj->prop_count) break;
      return ant_object_prop_get_unchecked(obj, (uint32_t)slot);
    }

    current = obj->proto;
    if (is_null(current) || is_undefined(current)) return js_mkundef();
  }

  return js_getprop_fallback_len(js, iterator, name, len);
}

bool js_iter_open(ant_t *js, ant_value_t iterable, iterator_t *it) {
  memset(it, 0, sizeof(*it));

  ant_value_t iter_fn = vtype(iterable) == kTypeArray && js_array_iteration_default(js, iterable)
    ? js->sym.array_values_fn
    : js_get_sym(js, iterable, js->sym.iterator_sym);
  
  if (!is_callable(iter_fn)) {
    if (!is_err(iter_fn) && !is_undefined(iter_fn) && !is_null(iter_fn))
      js_mkerr_typed(js, JS_ERR_TYPE, "iterator method is not callable");
    return false;
  }

  return js_iter_open_method(js, iterable, iter_fn, it);
}

bool js_iter_open_method(ant_t *js, ant_value_t iterable, ant_value_t iter_fn, iterator_t *it) {
  memset(it, 0, sizeof(*it));

  ant_value_t iterator = iter_fn == js->sym.array_values_fn && vtype(iterable) == kTypeArray
    ? make_array_iterator(js, iterable, ARR_ITER_VALUES)
    : sv_vm_call(js->vm, js, iter_fn, iterable, NULL, 0, NULL, js_mkundef());
  
  if (is_err(iterator)) return false;
  
  if (!is_object_type(iterator) && vtype(iterator) != kTypeBuiltin) {
    js_mkerr_typed(js, JS_ERR_TYPE, "iterator method must return an object");
    return false;
  }

  it->iterator = iterator;
  it->next_fn = iterator_get_method(js, iterator, "next", 4);
  
  if (is_err(it->next_fn)) return false;
  it->advance = js_iter_find_advance(js, iterator, it->next_fn);

  return true;
}

js_iter_advance_fn js_iter_find_advance(ant_t *js, ant_value_t iterator, ant_value_t next_fn) {
  js_iter_advance_fn advance = Ant_Generator_IterAdvance(iterator, next_fn);
  if (advance) return advance;

  ant_value_t proto = (vtype(iterator) == kTypeObject) ? js_get_proto(js, iterator) : js_mkundef();
  for (size_t i = 0; i < js->iterators.len; i++) {
    const ant_iterator_entry_t *entry = &js->iterators.entries[i];
    if (proto == entry->proto && next_fn == entry->next) return entry->advance;
  }

  return NULL;
}

ant_value_t Ant_Iterator_ToList(ant_t *js, ant_value_t iterable, const char *not_iterable) {
  ant_value_t list = js_mkarr(js);
  if (is_err(list)) return list;
  
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, list);
  
  iterator_t it;
  if (!js_iter_open(js, iterable, &it)) {
    GC_ROOT_RESTORE(js, mark);
    return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : js_mkerr_typed(js, JS_ERR_TYPE, "%s", not_iterable);
  }
  
  GC_ROOT_PIN(js, it.iterator);
  GC_ROOT_PIN(js, it.next_fn);
  
  ant_value_t value;
  while (js_iter_next(js, &it, &value)) js_arr_push(js, list, value);
  GC_ROOT_RESTORE(js, mark);
  
  return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : list;
}

bool js_iter_next(ant_t *js, iterator_t *it, ant_value_t *out) {
  if (it->advance) return it->advance(js, it, out);

  ant_value_t next_fn = it->next_fn;
  if (!is_callable(next_fn)) {
    js_mkerr_typed(js, JS_ERR_TYPE, "iterator next is not callable");
    return false;
  }

  ant_value_t result = sv_call_value(js, next_fn, it->iterator, NULL, 0);
  if (is_err(result)) return false;
  
  if (!is_object_type(result) && vtype(result) != kTypeBuiltin) {
    js_mkerr_typed(js, JS_ERR_TYPE, "iterator next must return an object");
    return false;
  }
  
  ant_value_t done = js_getprop_fallback(js, result, "done");
  if (is_err(done)) return false;

  if (js_truthy(js, done)) return false;
  *out = js_getprop_fallback(js, result, "value");

  return !is_err(*out);
}

static void js_iter_call_return(ant_t *js, iterator_t *it) {
  ant_value_t return_fn = iterator_get_method(js, it->iterator, "return", 6);
  if (is_err(return_fn) || (is_undefined(return_fn) || is_null(return_fn))) return;
  
  if (!is_callable(return_fn)) {
    js_mkerr_typed(js, JS_ERR_TYPE, "iterator return is not callable");
    return;
  }
  
  ant_value_t result = sv_vm_call(js->vm, js, return_fn, it->iterator, NULL, 0, NULL, js_mkundef());
  if (!is_err(result) && !is_object_type(result) && vtype(result) != kTypeBuiltin)
    js_mkerr_typed(js, JS_ERR_TYPE, "iterator return must return an object");
}

void js_iter_close(ant_t *js, iterator_t *it) {
  if (!Ant_Exception_Pending(js)) {
    js_iter_call_return(js, it);
    return;
  }

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t completion = Ant_Exception_Peek(js);

  GC_ROOT_PIN(js, completion);
  Ant_Exception_Clear(js);
  js_iter_call_return(js, it);

  Ant_Exception_Set(js, completion);
  GC_ROOT_RESTORE(js, root_mark);
}

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

static constexpr uint32_t ASYNC_TERMINAL_STATE_TAG = Ant_NativeTag("ATIT");

typedef struct {
  double index;
  int mode;
  bool has_acc;
} async_terminal_state_t;

static inline ant_value_t call_indexed_callback(ant_t *js, ant_value_t fn, ant_value_t value, double index) {
  ant_value_t call_args[2] = { value, js_mknum(index) };
  return sv_vm_call(js->vm, js, fn, js_mkundef(), call_args, 2, NULL, js_mkundef());
}

static ant_value_t wrap_iter_next(ant_params_t) {
  ant_value_t self = js->this_val;
  ant_value_t source = js_get_slot(self, SLOT_DATA);
  ant_value_t state_v = js_get_slot(self, SLOT_ITER_STATE);
  
  uint32_t state = (vtype(state_v) == kTypeNumber) ? (uint32_t)js_getnum(state_v) : 0;
  uint32_t kind  = iter_state_kind(state);
  uint32_t count = iter_state_index(state);
  
  ant_value_t cb = js_get_slot(self, SLOT_CTOR);
  ant_value_t next_fn = js_getprop_fallback(js, source, "next");
  if (is_err(next_fn)) return next_fn;

  for (;;) {
    if (kind == WRAP_FLATMAP) {
    ant_value_t inner = js_get_slot(self, SLOT_ENTRIES);
    
    if (vtype(inner) != kTypeUndefined) {
      ant_value_t inner_next = js_getprop_fallback(js, inner, "next");
      if (is_err(inner_next)) return inner_next;

      ant_value_t inner_step = sv_vm_call(js->vm, js, inner_next, inner, NULL, 0, NULL, js_mkundef());
      if (is_err(inner_step)) return inner_step;

      ant_value_t inner_done = js_getprop_fallback(js, inner_step, "done");
      if (is_err(inner_done)) return inner_done;

      if (!js_truthy(js, inner_done)) {
        ant_value_t inner_value = js_getprop_fallback(js, inner_step, "value");
        if (is_err(inner_value)) return inner_value;
        return Ant_Iterator_CreateResult(js, inner_value, false);
      }
      
      js_set_slot(self, SLOT_ENTRIES, js_mkundef());
    }}

    ant_value_t step = sv_call_value(js, next_fn, source, NULL, 0);

    if (is_err(step)) return step;
    ant_value_t done = js_getprop_fallback(js, step, "done");
    if (is_err(done)) return done;
    
    if (js_truthy(js, done)) {
      if (kind == WRAP_FLATMAP) {
      ant_value_t inner = js_get_slot(self, SLOT_ENTRIES);
      if (vtype(inner) != kTypeUndefined) {
        js_set_slot(self, SLOT_ENTRIES, js_mkundef());
      }}
      
      return Ant_Iterator_CreateResult(js, js_mkundef(), true);
    }

    ant_value_t value = js_getprop_fallback(js, step, "value");
    if (is_err(value)) return value;

    switch (kind) {
    case WRAP_MAP: {
      ant_value_t out_val;
      if (is_callable(cb)) {
        out_val = call_indexed_callback(js, cb, value, (double)count);
        if (is_err(out_val)) return out_val;
      } else out_val = value;

      count++;
      js_set_slot(self, SLOT_ITER_STATE, js_mknum((double)iter_state_pack(kind, count)));
      return Ant_Iterator_CreateResult(js, out_val, false);
    }

    case WRAP_FILTER: {
      ant_value_t test = call_indexed_callback(js, cb, value, (double)count);
      if (is_err(test)) return test;
      count++;
      js_set_slot(self, SLOT_ITER_STATE, js_mknum((double)iter_state_pack(kind, count)));
      if (js_truthy(js, test)) {
        return Ant_Iterator_CreateResult(js, value, false);
      }
      continue;
    }

    case WRAP_TAKE: {
      uint32_t limit = (vtype(cb) == kTypeNumber) ? (uint32_t)js_getnum(cb) : 0;
      if (count >= limit) {
        return Ant_Iterator_CreateResult(js, js_mkundef(), true);
      }
      js_set_slot(self, SLOT_ITER_STATE, js_mknum((double)iter_state_pack(kind, count + 1)));
      return Ant_Iterator_CreateResult(js, value, false);
    }

    case WRAP_DROP: {
      uint32_t limit = (vtype(cb) == kTypeNumber) ? (uint32_t)js_getnum(cb) : 0;
      count++;
      js_set_slot(self, SLOT_ITER_STATE, js_mknum((double)iter_state_pack(kind, count)));
      if (count <= limit) continue;
      return Ant_Iterator_CreateResult(js, value, false);
    }

    case WRAP_FLATMAP: {
      ant_value_t mapped = call_indexed_callback(js, cb, value, (double)count);
      if (is_err(mapped)) return mapped;
      count++;
      js_set_slot(self, SLOT_ITER_STATE, js_mknum((double)iter_state_pack(kind, count)));

      ant_value_t iter_fn = js_get_sym(js, mapped, js->sym.iterator_sym);
      if (!is_callable(iter_fn)) {
        return Ant_Iterator_CreateResult(js, mapped, false);
      }

      ant_value_t inner = sv_vm_call(js->vm, js, iter_fn, mapped, NULL, 0, NULL, js_mkundef());
      if (is_err(inner)) return inner;

      ant_value_t inner_next = js_getprop_fallback(js, inner, "next");
      if (is_err(inner_next)) return inner_next;

      ant_value_t inner_step = sv_vm_call(js->vm, js, inner_next, inner, NULL, 0, NULL, js_mkundef());
      if (is_err(inner_step)) return inner_step;

      ant_value_t inner_done = js_getprop_fallback(js, inner_step, "done");
      if (is_err(inner_done)) return inner_done;

      if (!js_truthy(js, inner_done)) {
        ant_value_t inner_value = js_getprop_fallback(js, inner_step, "value");
        if (is_err(inner_value)) return inner_value;
        js_set_slot_wb(js, self, SLOT_ENTRIES, inner);
        return Ant_Iterator_CreateResult(js, inner_value, false);
      }
      continue;
    }

    default: return Ant_Iterator_CreateResult(js, value, false);
    }
  }
}

static ant_value_t make_wrap_iter(ant_t *js, ant_value_t source, int kind, ant_value_t cb) {
  ant_value_t iter = Ant_Object_CreateBare(js);
  
  js_set_proto_init(iter, js->builtins.wrap_iter_proto);
  js_set_slot_wb(js, iter, SLOT_DATA, source);
  js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)iter_state_pack(kind, 0)));
  js_set_slot_wb(js, iter, SLOT_CTOR, cb);
  
  return iter;
}

static ant_value_t get_source_iter(ant_t *js) {
  ant_value_t self = js->this_val;
  ant_value_t next = js_getprop_fallback(js, self, "next");
  if (is_err(next)) return next;
  if (is_callable(next)) return self;
  
  ant_value_t iter_fn = js_get_sym(js, self, js->sym.iterator_sym);
  if (is_err(iter_fn)) return iter_fn;
  if (!is_callable(iter_fn)) return js_mkerr_typed(js, JS_ERR_TYPE, "object is not iterable");
  
  return sv_vm_call(js->vm, js, iter_fn, self, NULL, 0, NULL, js_mkundef());
}

static ant_value_t iter_make_helper(ant_t *js, int kind, ant_value_t cb) {
  ant_value_t source = get_source_iter(js);
  if (is_err(source)) return source;
  return make_wrap_iter(js, source, kind, cb);
}

static ant_value_t iter_make_callable_helper(
  ant_native_params_t,
  int kind,
  const char *method
) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s requires a callable", method);
  return iter_make_helper(js, kind, args[0]);
}

static ant_value_t iter_make_count_helper(
  ant_native_params_t, int kind,
  const char *method
) {
  double limit = (nargs >= 1 && vtype(args[0]) == kTypeNumber) ? js_getnum(args[0]) : 0;
  if (limit < 0) return js_mkerr(js, "%s requires a non-negative number", method);
  return iter_make_helper(js, kind, js_mknum(limit));
}

static ant_value_t iter_map(ant_params_t) {
  return iter_make_callable_helper(js, args, nargs, WRAP_MAP, "Iterator.prototype.map");
}

static ant_value_t iter_filter(ant_params_t) {
  return iter_make_callable_helper(js, args, nargs, WRAP_FILTER, "Iterator.prototype.filter");
}

static ant_value_t iter_take(ant_params_t) {
  return iter_make_count_helper(js, args, nargs, WRAP_TAKE, "Iterator.prototype.take");
}

static ant_value_t iter_drop(ant_params_t) {
  return iter_make_count_helper(js, args, nargs, WRAP_DROP, "Iterator.prototype.drop");
}

static ant_value_t iter_flatMap(ant_params_t) {
  return iter_make_callable_helper(js, args, nargs, WRAP_FLATMAP, "Iterator.prototype.flatMap");
}

static ant_value_t iter_every(ant_params_t) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Iterator.prototype.every requires a callable");
  ant_value_t fn = args[0];

  iterator_t it;
  if (!js_iter_open(js, js->this_val, &it))
    return Ant_Exception_Pending(js) ? Ant_Exception_Current(js)
      : js_mkerr_typed(js, JS_ERR_TYPE, "object is not iterable");

  ant_value_t value;
  uint32_t counter = 0;
  while (js_iter_next(js, &it, &value)) {
    ant_value_t test = call_indexed_callback(js, fn, value, (double)counter++);
    if (is_err(test)) { js_iter_close(js, &it); return test; }
    if (!js_truthy(js, test)) {
      js_iter_close(js, &it);
      return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : js_false;
    }
  }
  return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : js_true;
}

static ant_value_t iter_some(ant_params_t) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Iterator.prototype.some requires a callable");
  ant_value_t fn = args[0];

  iterator_t it;
  if (!js_iter_open(js, js->this_val, &it))
    return Ant_Exception_Pending(js) ? Ant_Exception_Current(js)
      : js_mkerr_typed(js, JS_ERR_TYPE, "object is not iterable");

  ant_value_t value;
  uint32_t counter = 0;
  while (js_iter_next(js, &it, &value)) {
    ant_value_t test = call_indexed_callback(js, fn, value, (double)counter++);
    if (is_err(test)) { js_iter_close(js, &it); return test; }
    if (js_truthy(js, test)) {
      js_iter_close(js, &it);
      return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : js_true;
    }
  }
  return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : js_false;
}

static ant_value_t iter_find(ant_params_t) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Iterator.prototype.find requires a callable");
  ant_value_t fn = args[0];

  iterator_t it;
  if (!js_iter_open(js, js->this_val, &it))
    return Ant_Exception_Pending(js) ? Ant_Exception_Current(js)
      : js_mkerr_typed(js, JS_ERR_TYPE, "object is not iterable");

  ant_value_t value;
  uint32_t counter = 0;
  while (js_iter_next(js, &it, &value)) {
    ant_value_t test = call_indexed_callback(js, fn, value, (double)counter++);
    if (is_err(test)) { js_iter_close(js, &it); return test; }
    if (js_truthy(js, test)) {
      js_iter_close(js, &it);
      return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : value;
    }
  }
  return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : js_mkundef();
}

static ant_value_t iter_forEach(ant_params_t) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Iterator.prototype.forEach requires a callable");
  ant_value_t fn = args[0];

  iterator_t it;
  if (!js_iter_open(js, js->this_val, &it))
    return Ant_Exception_Pending(js) ? Ant_Exception_Current(js)
      : js_mkerr_typed(js, JS_ERR_TYPE, "object is not iterable");

  ant_value_t value;
  uint32_t counter = 0;
  while (js_iter_next(js, &it, &value)) {
    ant_value_t r = call_indexed_callback(js, fn, value, (double)counter++);
    if (is_err(r)) { js_iter_close(js, &it); return r; }
  }
  return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : js_mkundef();
}

static ant_value_t iter_reduce(ant_params_t) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Iterator.prototype.reduce requires a callable");
  ant_value_t fn = args[0];
  bool has_init = (nargs >= 2);

  iterator_t it;
  if (!js_iter_open(js, js->this_val, &it))
    return Ant_Exception_Pending(js) ? Ant_Exception_Current(js)
      : js_mkerr_typed(js, JS_ERR_TYPE, "object is not iterable");

  ant_value_t acc = has_init ? args[1] : js_mkundef();
  bool first = !has_init;
  ant_value_t value;
  uint32_t counter = 0;

  while (js_iter_next(js, &it, &value)) {
    if (first) { acc = value; first = false; counter++; continue; }
    ant_value_t call_args[3] = { acc, value, js_mknum((double)counter++) };
    acc = sv_vm_call(js->vm, js, fn, js_mkundef(), call_args, 3, NULL, js_mkundef());
    if (is_err(acc)) { js_iter_close(js, &it); return acc; }
  }

  if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
  if (first)
    return js_mkerr_typed(js, JS_ERR_TYPE, "reduce of empty iterator with no initial value");
  return acc;
}

static ant_value_t iter_toArray(ant_params_t) {
  iterator_t it;
  if (!js_iter_open(js, js->this_val, &it))
    return Ant_Exception_Pending(js) ? Ant_Exception_Current(js)
      : js_mkerr_typed(js, JS_ERR_TYPE, "object is not iterable");

  ant_value_t arr = js_mkarr(js);
  ant_value_t value;
  while (js_iter_next(js, &it, &value))
    js_arr_push(js, arr, value);

  return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : arr;
}

static ant_value_t iter_from(ant_params_t) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Iterator.from requires an argument");
  ant_value_t obj = args[0];

  ant_value_t next = js_getprop_fallback(js, obj, "next");
  if (is_callable(next)) {
    return make_wrap_iter(js, obj, WRAP_MAP, js_mkundef());
  }

  ant_value_t iter_fn = js_get_sym(js, obj, js->sym.iterator_sym);
  if (!is_callable(iter_fn))
    return js_mkerr_typed(js, JS_ERR_TYPE, "object is not iterable");
    
  ant_value_t iterator = sv_vm_call(js->vm, js, iter_fn, obj, NULL, 0, NULL, js_mkundef());
  if (is_err(iterator)) return iterator;
  
  return iterator;
}

static ant_value_t iter_ctor(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Iterator is not directly constructable");
    
  ant_value_t obj = Ant_Object_CreateBare(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, js->sym.iterator_proto, call_new_target);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  
  return obj;
}

static ant_value_t async_iter_ctor(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "AsyncIterator constructor requires 'new'");

  ant_value_t obj = Ant_Object_CreateBare(js);
  ant_value_t proto = js_instance_proto_from_new_target(js, js->sym.async_iterator_proto, call_new_target);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  
  return obj;
}


static inline ant_value_t fulfilled_promise(ant_t *js, ant_value_t value) {
  ant_value_t promise = js_mkpromise(js);
  js_resolve_promise(js, promise, value);
  return promise;
}

static void async_iter_reject(ant_t *js, ant_value_t promise, ant_value_t reason) {
  if (is_err(reason)) reason = js_take_thrown(js, reason);
  js_reject_promise(js, promise, reason);
}

static inline ant_value_t rejected_promise(ant_t *js, ant_value_t reason) {
  ant_value_t promise = js_mkpromise(js);
  async_iter_reject(js, promise, reason);
  return promise;
}

static inline ant_value_t promise_from_call_result(ant_t *js, ant_value_t result) {
  if (vtype(result) == kTypePromise) return result;
  if (is_err(result)) return rejected_promise(js, result);
  return fulfilled_promise(js, result);
}

static ant_value_t async_iter_call_method(
  ant_t *js, ant_value_t receiver,
  const char *name, ant_args_t, bool *missing
) {
  ant_value_t fn = js_getprop_fallback(js, receiver, name);
  if (is_err(fn)) {
    if (missing) *missing = false;
    return fn;
  }

  if (missing) *missing = !is_callable(fn);
  if (!is_callable(fn)) return js_mkundef();

  return sv_vm_call(js->vm, js, fn, receiver, args, nargs, NULL, js_mkundef());
}

static ant_value_t make_async_wrap_iter(ant_t *js, ant_value_t source, int kind, ant_value_t cb) {
  ant_value_t iter = Ant_Object_CreateBare(js);
  js_set_proto_init(iter, js->builtins.async_wrap_iter_proto);
  js_set_slot_wb(js, iter, SLOT_DATA, source);
  js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)iter_state_pack(kind, 0)));
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

static ant_value_t async_wrap_on_step(ant_params_t) {
  ant_value_t state = js_func_data(js->current_func);
  ant_value_t iter = js_get(js, state, "iter");
  ant_value_t promise = js_get(js, state, "promise");
  ant_value_t step = nargs > 0 ? args[0] : js_mkundef();
  return async_wrap_handle_step(js, iter, promise, step);
}

static ant_value_t async_wrap_on_reject(ant_params_t) {
  ant_value_t state = js_func_data(js->current_func);
  ant_value_t promise = js_get(js, state, "promise");
  async_iter_reject(js, promise, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static ant_value_t async_wrap_on_callback(ant_params_t) {
  ant_value_t state = js_func_data(js->current_func);
  ant_value_t iter = js_get(js, state, "iter");
  ant_value_t promise = js_get(js, state, "promise");
  uint32_t kind = (uint32_t)js_getnum(js_get(js, state, "kind"));
  ant_value_t value = js_get(js, state, "value");
  ant_value_t result = nargs > 0 ? args[0] : js_mkundef();
  return async_wrap_handle_callback_result(js, iter, promise, kind, value, result);
}

static ant_value_t async_wrap_on_sync_value(ant_params_t) {
  ant_value_t state = js_func_data(js->current_func);
  ant_value_t promise = js_get(js, state, "promise");
  bool done = js_truthy(js, js_get(js, state, "done"));
  ant_value_t value = nargs > 0 ? args[0] : js_mkundef();
  js_resolve_promise(js, promise, Ant_Iterator_CreateResult(js, value, done));
  return js_mkundef();
}

static bool async_wrap_chain_step(ant_t *js, ant_value_t iter, ant_value_t promise, ant_value_t next_result) {
  if (is_err(next_result)) {
    async_iter_reject(js, promise, next_result);
    return true;
  }

  if (vtype(next_result) != kTypePromise) {
    async_wrap_handle_step(js, iter, promise, next_result);
    return true;
  }

  ant_value_t state = Ant_Object_CreateBare(js);
  js_set(js, state, "iter", iter);
  js_set(js, state, "promise", promise);
  ant_value_t on_resolve = js_heavy_mkfun(js, async_wrap_on_step, state);
  ant_value_t on_reject = js_heavy_mkfun(js, async_wrap_on_reject, state);
  ant_value_t then_result = js_promise_then(js, next_result, on_resolve, on_reject);
  if (is_err(then_result)) async_iter_reject(js, promise, then_result);
  else promise_mark_handled(then_result);
  return true;
}

static ant_value_t async_wrap_handle_inner_step(
  ant_t *js,
  ant_value_t iter,
  ant_value_t promise,
  ant_value_t step
) {
  if (!is_object_type(step)) {
    async_iter_reject(js, promise, js_mkerr_typed(js, JS_ERR_TYPE, "iterator result is not an object"));
    return js_mkundef();
  }

  ant_value_t done = js_getprop_fallback(js, step, "done");
  if (is_err(done)) {
    async_iter_reject(js, promise, done);
    return js_mkundef();
  }

  if (!js_truthy(js, done)) {
    ant_value_t value = js_getprop_fallback(js, step, "value");
    if (is_err(value)) async_iter_reject(js, promise, value);
    else js_resolve_promise(js, promise, Ant_Iterator_CreateResult(js, value, false));
    return js_mkundef();
  }

  js_set_slot(iter, SLOT_ENTRIES, js_mkundef());
  async_wrap_advance(js, iter, promise);
  return js_mkundef();
}

static ant_value_t async_wrap_on_inner_step(ant_params_t) {
  ant_value_t state = js_func_data(js->current_func);
  ant_value_t iter = js_get(js, state, "iter");
  ant_value_t promise = js_get(js, state, "promise");
  ant_value_t step = nargs > 0 ? args[0] : js_mkundef();
  return async_wrap_handle_inner_step(js, iter, promise, step);
}

static ant_value_t async_wrap_advance_inner(ant_t *js, ant_value_t iter, ant_value_t promise) {
  ant_value_t inner = js_get_slot(iter, SLOT_ENTRIES);
  if (vtype(inner) == kTypeUndefined || vtype(inner) == kTypeNull) return async_wrap_advance(js, iter, promise);

  bool missing = false;
  ant_value_t next_result = async_iter_call_method(js, inner, "next", NULL, 0, &missing);
  if (missing) {
    js_set_slot(iter, SLOT_ENTRIES, js_mkundef());
    return async_wrap_advance(js, iter, promise);
  }

  if (is_err(next_result)) {
    async_iter_reject(js, promise, next_result);
    return js_mkundef();
  }

  if (vtype(next_result) == kTypePromise) {
    ant_value_t state = Ant_Object_CreateBare(js);
    js_set(js, state, "iter", iter);
    js_set(js, state, "promise", promise);
    ant_value_t on_resolve = js_heavy_mkfun(js, async_wrap_on_inner_step, state);
    ant_value_t on_reject = js_heavy_mkfun(js, async_wrap_on_reject, state);
    ant_value_t then_result = js_promise_then(js, next_result, on_resolve, on_reject);
    if (is_err(then_result)) async_iter_reject(js, promise, then_result);
    else promise_mark_handled(then_result);
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
    async_iter_reject(js, promise, js_mkerr_typed(js, JS_ERR_TYPE, "iterator result is not an object"));
    return js_mkundef();
  }

  ant_value_t done_value = js_getprop_fallback(js, step, "done");
  if (is_err(done_value)) {
    async_iter_reject(js, promise, done_value);
    return js_mkundef();
  }

  bool done = js_truthy(js, done_value);
  ant_value_t state_v = js_get_slot(iter, SLOT_ITER_STATE);
  
  uint32_t state = (vtype(state_v) == kTypeNumber) ? (uint32_t)js_getnum(state_v) : 0;
  uint32_t kind = iter_state_kind(state);
  uint32_t count = iter_state_index(state);

  if (done && kind != WRAP_FROM_SYNC) {
    js_resolve_promise(js, promise, Ant_Iterator_CreateResult(js, js_mkundef(), true));
    return js_mkundef();
  }

  ant_value_t value = js_getprop_fallback(js, step, "value");

  if (is_err(value)) {
    async_iter_reject(js, promise, value);
    return js_mkundef();
  }

  if (kind == WRAP_FROM_SYNC && vtype(value) == kTypePromise) {
    ant_value_t state_obj = Ant_Object_CreateBare(js);
    js_set(js, state_obj, "promise", promise);
    js_set(js, state_obj, "done", done ? js_true : js_false);
    
    ant_value_t on_resolve = js_heavy_mkfun(js, async_wrap_on_sync_value, state_obj);
    ant_value_t on_reject = js_heavy_mkfun(js, async_wrap_on_reject, state_obj);
    ant_value_t then_result = js_promise_then(js, value, on_resolve, on_reject);
    
    if (is_err(then_result)) async_iter_reject(js, promise, then_result);
    else promise_mark_handled(then_result);
    return js_mkundef();
  }

  if (done) {
    js_resolve_promise(js, promise, Ant_Iterator_CreateResult(js, value, true));
    return js_mkundef();
  }

  ant_value_t cb = js_get_slot(iter, SLOT_CTOR);

  switch (kind) {
  case WRAP_MAP: {
    ant_value_t mapped = call_indexed_callback(js, cb, value, (double)count);
    if (is_err(mapped)) {
      async_iter_reject(js, promise, mapped);
      return js_mkundef();
    }
    js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)iter_state_pack(kind, count + 1)));
    return async_wrap_handle_callback_result(js, iter, promise, kind, value, mapped);
  }

  case WRAP_FILTER: {
    ant_value_t test = call_indexed_callback(js, cb, value, (double)count);
    if (is_err(test)) {
      async_iter_reject(js, promise, test);
      return js_mkundef();
    }
    js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)iter_state_pack(kind, count + 1)));
    return async_wrap_handle_callback_result(js, iter, promise, kind, value, test);
  }

  case WRAP_TAKE:
    js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)iter_state_pack(kind, count + 1)));
    js_resolve_promise(js, promise, Ant_Iterator_CreateResult(js, value, false));
    return js_mkundef();

  case WRAP_DROP: {
    double limit = (vtype(cb) == kTypeNumber) ? js_getnum(cb) : 0;
    js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)iter_state_pack(kind, count + 1)));
    if ((double)(count + 1) <= limit) async_wrap_advance(js, iter, promise);
    else js_resolve_promise(js, promise, Ant_Iterator_CreateResult(js, value, false));
    return js_mkundef();
  }

  case WRAP_FLATMAP: {
    ant_value_t mapped = call_indexed_callback(js, cb, value, (double)count);
    if (is_err(mapped)) {
      async_iter_reject(js, promise, mapped);
      return js_mkundef();
    }
    js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)iter_state_pack(kind, count + 1)));
    return async_wrap_handle_callback_result(js, iter, promise, kind, value, mapped);
  }

  default:
    js_resolve_promise(js, promise, Ant_Iterator_CreateResult(js, value, false));
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
  if (vtype(result) == kTypePromise) {
    ant_value_t state = Ant_Object_CreateBare(js);
    js_set(js, state, "iter", iter);
    js_set(js, state, "promise", promise);
    js_set(js, state, "kind", js_mknum((double)kind));
    js_set(js, state, "value", value);
    ant_value_t on_resolve = js_heavy_mkfun(js, async_wrap_on_callback, state);
    ant_value_t on_reject = js_heavy_mkfun(js, async_wrap_on_reject, state);
    ant_value_t then_result = js_promise_then(js, result, on_resolve, on_reject);
    if (is_err(then_result)) async_iter_reject(js, promise, then_result);
    else promise_mark_handled(then_result);
    return js_mkundef();
  }

  if (kind == WRAP_MAP) {
    js_resolve_promise(js, promise, Ant_Iterator_CreateResult(js, result, false));
    return js_mkundef();
  }

  if (kind == WRAP_FILTER) {
    if (js_truthy(js, result)) js_resolve_promise(js, promise, Ant_Iterator_CreateResult(js, value, false));
    else async_wrap_advance(js, iter, promise);
    return js_mkundef();
  }

  if (kind == WRAP_FLATMAP) {
    ant_value_t iter_fn = js_get_sym(js, result, js->sym.asyncIterator_sym);
    if (!is_callable(iter_fn)) iter_fn = js_get_sym(js, result, js->sym.iterator_sym);
    if (!is_callable(iter_fn)) {
      js_resolve_promise(js, promise, Ant_Iterator_CreateResult(js, result, false));
      return js_mkundef();
    }

    ant_value_t inner = sv_vm_call(js->vm, js, iter_fn, result, NULL, 0, NULL, js_mkundef());
    if (is_err(inner)) {
      async_iter_reject(js, promise, inner);
      return js_mkundef();
    }
    js_set_slot_wb(js, iter, SLOT_ENTRIES, inner);
    return async_wrap_advance_inner(js, iter, promise);
  }

  js_resolve_promise(js, promise, Ant_Iterator_CreateResult(js, result, false));
  return js_mkundef();
}

static ant_value_t async_wrap_advance(ant_t *js, ant_value_t iter, ant_value_t promise) {
  ant_value_t state_v = js_get_slot(iter, SLOT_ITER_STATE);
  uint32_t state = (vtype(state_v) == kTypeNumber) ? (uint32_t)js_getnum(state_v) : 0;
  uint32_t kind = iter_state_kind(state);
  ant_value_t cb = js_get_slot(iter, SLOT_CTOR);

  if (kind == WRAP_TAKE) {
    double limit = (vtype(cb) == kTypeNumber) ? js_getnum(cb) : 0;
    if ((double)iter_state_index(state) >= limit) {
      js_resolve_promise(js, promise, Ant_Iterator_CreateResult(js, js_mkundef(), true));
      return js_mkundef();
    }
  }

  if (kind == WRAP_FLATMAP && vtype(js_get_slot(iter, SLOT_ENTRIES)) != kTypeUndefined)
    return async_wrap_advance_inner(js, iter, promise);

  ant_value_t source = js_get_slot(iter, SLOT_DATA);
  bool missing = false;
  ant_value_t next_result = async_iter_call_method(js, source, "next", NULL, 0, &missing);
  if (missing) async_iter_reject(js, promise, js_mkerr_typed(js, JS_ERR_TYPE, "object is not async iterable"));
  else async_wrap_chain_step(js, iter, promise, next_result);
  return js_mkundef();
}

static ant_value_t async_wrap_next(ant_params_t) {
  (void)args; (void)nargs;
  ant_value_t promise = js_mkpromise(js);
  async_wrap_advance(js, js->this_val, promise);
  return promise;
}

static ant_value_t async_wrap_return(ant_params_t) {
  ant_value_t source = js_get_slot(js->this_val, SLOT_DATA);
  bool missing = false;
  ant_value_t result = async_iter_call_method(js, source, "return", args, nargs, &missing);
  if (missing) return fulfilled_promise(js, Ant_Iterator_CreateResult(js, nargs > 0 ? args[0] : js_mkundef(), true));
  ant_value_t state_v = js_get_slot(js->this_val, SLOT_ITER_STATE);
  uint32_t state = (vtype(state_v) == kTypeNumber) ? (uint32_t)js_getnum(state_v) : 0;
  if (iter_state_kind(state) == WRAP_FROM_SYNC) {
    ant_value_t promise = js_mkpromise(js);
    async_wrap_chain_step(js, js->this_val, promise, result);
    return promise;
  }
  return promise_from_call_result(js, result);
}

static ant_value_t async_wrap_throw(ant_params_t) {
  ant_value_t source = js_get_slot(js->this_val, SLOT_DATA);
  bool missing = false;
  ant_value_t result = async_iter_call_method(js, source, "throw", args, nargs, &missing);
  if (missing) return rejected_promise(js, nargs > 0 ? args[0] : js_mkundef());
  ant_value_t state_v = js_get_slot(js->this_val, SLOT_ITER_STATE);
  uint32_t state = (vtype(state_v) == kTypeNumber) ? (uint32_t)js_getnum(state_v) : 0;
  if (iter_state_kind(state) == WRAP_FROM_SYNC) {
    ant_value_t promise = js_mkpromise(js);
    async_wrap_chain_step(js, js->this_val, promise, result);
    return promise;
  }
  return promise_from_call_result(js, result);
}

static ant_value_t async_iter_from(ant_params_t) {
  if (nargs < 1 || vtype(args[0]) == kTypeUndefined || vtype(args[0]) == kTypeNull)
    return js_mkerr_typed(js, JS_ERR_TYPE, "AsyncIterator.from requires an object");

  ant_value_t obj = args[0];
  ant_value_t iter_fn = js_get_sym(js, obj, js->sym.asyncIterator_sym);
  if (is_err(iter_fn)) return iter_fn;
  if (is_callable(iter_fn)) {
    ant_value_t iterator = sv_vm_call(js->vm, js, iter_fn, obj, NULL, 0, NULL, js_mkundef());
    if (is_err(iterator)) return iterator;
    return make_async_wrap_iter(js, iterator, WRAP_PASS, js_mkundef());
  }

  iter_fn = js_get_sym(js, obj, js->sym.iterator_sym);
  if (is_err(iter_fn)) return iter_fn;
  if (is_callable(iter_fn)) {
    ant_value_t iterator = sv_vm_call(js->vm, js, iter_fn, obj, NULL, 0, NULL, js_mkundef());
    if (is_err(iterator)) return iterator;
    return make_async_wrap_iter(js, iterator, WRAP_FROM_SYNC, js_mkundef());
  }

  ant_value_t next = js_getprop_fallback(js, obj, "next");
  if (is_err(next)) return next;
  if (is_callable(next)) return make_async_wrap_iter(js, obj, WRAP_FROM_SYNC, js_mkundef());

  return js_mkerr_typed(js, JS_ERR_TYPE, "object is not async iterable");
}

static ant_value_t get_async_source_iter(ant_t *js) {
  ant_value_t self = js->this_val;
  ant_value_t next = js_getprop_fallback(js, self, "next");
  if (is_err(next)) return next;
  if (is_callable(next)) return self;

  ant_value_t iter_fn = js_get_sym(js, self, js->sym.asyncIterator_sym);
  if (is_err(iter_fn)) return iter_fn;
  if (!is_callable(iter_fn)) return js_mkerr_typed(js, JS_ERR_TYPE, "object is not async iterable");

  return sv_vm_call(js->vm, js, iter_fn, self, NULL, 0, NULL, js_mkundef());
}

static ant_value_t async_iter_make_helper(ant_t *js, int kind, ant_value_t cb) {
  ant_value_t source = get_async_source_iter(js);
  if (is_err(source)) return source;
  return make_async_wrap_iter(js, source, kind, cb);
}

static ant_value_t async_iter_make_callable_helper(
  ant_native_params_t,
  int kind,
  const char *method
) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s requires a callable", method);
  return async_iter_make_helper(js, kind, args[0]);
}

static ant_value_t async_iter_make_count_helper(
  ant_native_params_t,
  int kind,
  const char *method
) {
  double limit = (nargs >= 1 && vtype(args[0]) == kTypeNumber) ? js_getnum(args[0]) : 0;
  if (limit < 0) return js_mkerr_typed(js, JS_ERR_TYPE, "%s requires a non-negative number", method);
  return async_iter_make_helper(js, kind, js_mknum(limit));
}

static ant_value_t async_iter_map(ant_params_t) {
  return async_iter_make_callable_helper(js, args, nargs, WRAP_MAP, "AsyncIterator.prototype.map");
}

static ant_value_t async_iter_filter(ant_params_t) {
  return async_iter_make_callable_helper(js, args, nargs, WRAP_FILTER, "AsyncIterator.prototype.filter");
}

static ant_value_t async_iter_take(ant_params_t) {
  return async_iter_make_count_helper(js, args, nargs, WRAP_TAKE, "AsyncIterator.prototype.take");
}

static ant_value_t async_iter_drop(ant_params_t) {
  return async_iter_make_count_helper(js, args, nargs, WRAP_DROP, "AsyncIterator.prototype.drop");
}

static ant_value_t async_iter_flatMap(ant_params_t) {
  return async_iter_make_callable_helper(js, args, nargs, WRAP_FLATMAP, "AsyncIterator.prototype.flatMap");
}

static ant_value_t async_terminal_advance(ant_t *js, ant_value_t state);
static ant_value_t async_terminal_finish_callback(ant_t *js, ant_value_t state, ant_value_t result);
static void async_terminal_close_and_reject(ant_t *js, ant_value_t state, ant_value_t reason);

static void async_terminal_state_finalize(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  free(js_get_native(value, ASYNC_TERMINAL_STATE_TAG));
  js_clear_native(value, ASYNC_TERMINAL_STATE_TAG);
}

static inline async_terminal_state_t *async_terminal_state(ant_value_t state) {
  return (async_terminal_state_t *)js_get_native(state, ASYNC_TERMINAL_STATE_TAG);
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

static ant_value_t async_terminal_on_reject(ant_params_t) {
  ant_value_t state = js_func_data(js->current_func);
  ant_value_t promise = js_get_slot(state, SLOT_CTOR);
  async_iter_reject(js, promise, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static ant_value_t async_terminal_on_callback_reject(ant_params_t) {
  ant_value_t state = js_func_data(js->current_func);
  async_terminal_close_and_reject(js, state, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static ant_value_t async_terminal_on_callback(ant_params_t) {
  ant_value_t state = js_func_data(js->current_func);
  ant_value_t result = nargs > 0 ? args[0] : js_mkundef();
  return async_terminal_finish_callback(js, state, result);
}

static ant_value_t async_terminal_on_close(ant_params_t) {
  ant_value_t state = js_func_data(js->current_func);
  ant_value_t promise = js_get_slot(state, SLOT_CTOR);
  js_resolve_promise(js, promise, js_get_slot(state, SLOT_AUX));
  return js_mkundef();
}

static ant_value_t async_terminal_on_close_reject(ant_params_t) {
  ant_value_t state = js_func_data(js->current_func);
  ant_value_t promise = js_get_slot(state, SLOT_CTOR);
  async_iter_reject(js, promise, js_get_slot(state, SLOT_AUX));
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
    async_iter_reject(js, promise, result);
    return;
  }
  
  if (vtype(result) == kTypePromise) {
    ant_value_t on_resolve = js_heavy_mkfun(js, async_terminal_on_close, state);
    ant_value_t on_reject = js_heavy_mkfun(js, async_terminal_on_reject, state);
    ant_value_t then_result = js_promise_then(js, result, on_resolve, on_reject);
    if (is_err(then_result)) async_iter_reject(js, promise, then_result);
    else promise_mark_handled(then_result);
    return;
  }

  js_resolve_promise(js, promise, value);
}

static void async_terminal_close_and_reject(ant_t *js, ant_value_t state, ant_value_t reason) {
  if (is_err(reason)) reason = js_take_thrown(js, reason);
  ant_value_t promise = js_get_slot(state, SLOT_CTOR);
  ant_value_t iter = js_get_slot(state, SLOT_DATA);
  js_set_slot_wb(js, state, SLOT_AUX, reason);

  bool missing = false;
  ant_value_t result = async_iter_call_method(js, iter, "return", NULL, 0, &missing);
  if (missing) {
    async_iter_reject(js, promise, reason);
    return;
  }
  
  if (is_err(result)) {
    async_iter_reject(js, promise, result);
    return;
  }
  
  if (vtype(result) == kTypePromise) {
    ant_value_t on_resolve = js_heavy_mkfun(js, async_terminal_on_close_reject, state);
    ant_value_t on_reject = js_heavy_mkfun(js, async_terminal_on_reject, state);
    ant_value_t then_result = js_promise_then(js, result, on_resolve, on_reject);
    if (is_err(then_result)) async_iter_reject(js, promise, then_result);
    else promise_mark_handled(then_result);
    return;
  }

  async_iter_reject(js, promise, reason);
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
    async_iter_reject(js, promise, js_mkerr_typed(js, JS_ERR_TYPE, "iterator result is not an object"));
    return false;
  }

  int mode = async_terminal_mode(state);
  ant_value_t done = js_getprop_fallback(js, step, "done");
  if (is_err(done)) {
    async_iter_reject(js, promise, done);
    return false;
  }

  if (js_truthy(js, done)) {
    switch (mode) {
    case ASYNC_TERM_EVERY: js_resolve_promise(js, promise, js_true); break;
    case ASYNC_TERM_SOME: js_resolve_promise(js, promise, js_false); break;
    
    case ASYNC_TERM_FIND: js_resolve_promise(js, promise, js_mkundef());    break;
    case ASYNC_TERM_FOREACH: js_resolve_promise(js, promise, js_mkundef()); break;
    
    case ASYNC_TERM_REDUCE:
      if (!async_terminal_has_acc(state)) {
        async_iter_reject(js, promise, js_mkerr_typed(js, JS_ERR_TYPE, "reduce of empty iterator with no initial value"));
      } else js_resolve_promise(js, promise, js_get_slot(state, SLOT_SET));
      break;
    
    default:
      js_resolve_promise(js, promise, js_get_slot(state, SLOT_ENTRIES));
      break;
    }
    
    return false;
  }

  ant_value_t value = js_getprop_fallback(js, step, "value");
  if (is_err(value)) {
    async_iter_reject(js, promise, value);
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
    async_iter_reject(js, promise, js_mkerr_typed(js, JS_ERR_TYPE, "callback is not callable"));
    return false;
  }

  if (mode == ASYNC_TERM_REDUCE) {
    if (!async_terminal_has_acc(state)) {
      js_set_slot_wb(js, state, SLOT_SET, value);
      if (st) st->has_acc = true;
      return true;
    }
    
    ant_value_t call_args[3] = { js_get_slot(state, SLOT_SET), value, js_mknum(index) };
    ant_value_t next_acc = sv_vm_call(js->vm, js, fn, js_mkundef(), call_args, 3, NULL, js_mkundef());
    if (is_err(next_acc)) {
      async_terminal_close_and_reject(js, state, next_acc);
      return false;
    }
    
    if (vtype(next_acc) == kTypePromise) {
      ant_value_t on_resolve = js_heavy_mkfun(js, async_terminal_on_callback, state);
      ant_value_t on_reject = js_heavy_mkfun(js, async_terminal_on_callback_reject, state);
      ant_value_t then_result = js_promise_then(js, next_acc, on_resolve, on_reject);
      if (is_err(then_result)) async_terminal_close_and_reject(js, state, then_result);
      else promise_mark_handled(then_result);
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
  if (vtype(result) == kTypePromise) {
    ant_value_t on_resolve = js_heavy_mkfun(js, async_terminal_on_callback, state);
    ant_value_t on_reject = js_heavy_mkfun(js, async_terminal_on_callback_reject, state);
    ant_value_t then_result = js_promise_then(js, result, on_resolve, on_reject);
    if (is_err(then_result)) async_terminal_close_and_reject(js, state, then_result);
    else promise_mark_handled(then_result);
    return false;
  }
  
  return async_terminal_apply_callback_result(js, state, result);
}

static ant_value_t async_terminal_on_step(ant_params_t) {
  ant_value_t state = js_func_data(js->current_func);
  ant_value_t step = nargs > 0 ? args[0] : js_mkundef();
  if (async_terminal_handle_step(js, state, step)) return async_terminal_advance(js, state);
  return js_mkundef();
}

static ant_value_t async_terminal_finish_callback(
  ant_t *js,
  ant_value_t state,
  ant_value_t result
) {
  if (vtype(result) == kTypePromise) {
    ant_value_t on_resolve = js_heavy_mkfun(js, async_terminal_on_callback, state);
    ant_value_t on_reject = js_heavy_mkfun(js, async_terminal_on_callback_reject, state);
    ant_value_t then_result = js_promise_then(js, result, on_resolve, on_reject);
    if (is_err(then_result)) async_terminal_close_and_reject(js, state, then_result);
    else promise_mark_handled(then_result);
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
    async_iter_reject(js, promise, js_mkerr_typed(js, JS_ERR_TYPE, "object is not async iterable"));
    return js_mkundef();
  }
  
  if (is_err(next_result)) {
    async_iter_reject(js, promise, next_result);
    return js_mkundef();
  }
  
  if (vtype(next_result) == kTypePromise) {
    ant_value_t on_resolve = js_heavy_mkfun(js, async_terminal_on_step, state);
    ant_value_t on_reject = js_heavy_mkfun(js, async_terminal_on_reject, state);
    ant_value_t then_result = js_promise_then(js, next_result, on_resolve, on_reject);
    if (is_err(then_result)) async_iter_reject(js, promise, then_result);
    else promise_mark_handled(then_result);
    return js_mkundef();
  }
  
  if (!async_terminal_handle_step(js, state, next_result)) return js_mkundef();
}}

static ant_value_t async_iter_terminal(ant_native_params_t, int mode) {
  if (mode != ASYNC_TERM_TOARRAY && (nargs < 1 || !is_callable(args[0])))
    return js_mkerr_typed(js, JS_ERR_TYPE, "AsyncIterator helper requires a callable");

  ant_value_t iter = get_async_source_iter(js);
  if (is_err(iter)) return rejected_promise(js, iter);

  ant_value_t promise = js_mkpromise(js);
  ant_value_t state = Ant_Object_CreateBare(js);
  async_terminal_state_t *st = calloc(1, sizeof(*st));
  
  if (!st) {
    async_iter_reject(js, promise, Ant_Error_CreateOutOfMemory(js));
    return promise;
  }
  
  st->mode = mode;
  st->index = 0;
  st->has_acc = (mode == ASYNC_TERM_REDUCE && nargs > 1);
  
  js_set_native(state, st, ASYNC_TERMINAL_STATE_TAG);
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

static ant_value_t async_iter_every(ant_params_t) {
  return async_iter_terminal(js, args, nargs, ASYNC_TERM_EVERY);
}

static ant_value_t async_iter_some(ant_params_t) {
  return async_iter_terminal(js, args, nargs, ASYNC_TERM_SOME);
}

static ant_value_t async_iter_find(ant_params_t) {
  return async_iter_terminal(js, args, nargs, ASYNC_TERM_FIND);
}

static ant_value_t async_iter_forEach(ant_params_t) {
  return async_iter_terminal(js, args, nargs, ASYNC_TERM_FOREACH);
}

static ant_value_t async_iter_reduce(ant_params_t) {
  return async_iter_terminal(js, args, nargs, ASYNC_TERM_REDUCE);
}

static ant_value_t async_iter_toArray(ant_params_t) {
  return async_iter_terminal(js, args, nargs, ASYNC_TERM_TOARRAY);
}

static ant_value_t iterator_tag_get(ant_params_t) {
  return ANT_STRING("Iterator");
}

static ant_value_t iter_dispose(ant_params_t) {
  ant_value_t self = js->this_val;
  ant_value_t return_fn = iterator_get_method(js, self, "return", 6);
  
  if (is_err(return_fn)) return return_fn;
  if (is_undefined(return_fn) || is_null(return_fn)) return js_mkundef();
  if (!is_callable(return_fn)) return js_mkerr_typed(js, JS_ERR_TYPE, "iterator return is not callable");
  
  ant_value_t result = sv_vm_call(js->vm, js, return_fn, self, NULL, 0, NULL, js_mkundef());
  return is_err(result) ? result : js_mkundef();
}

static ant_value_t async_iter_dispose_done(ant_params_t) {
  return js_mkundef();
}

static ant_value_t async_iter_async_dispose(ant_params_t) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t self = js->this_val;
  GC_ROOT_PIN(js, self);
  
  ant_value_t out;
  ant_value_t return_fn = iterator_get_method(js, self, "return", 6);
  GC_ROOT_PIN(js, return_fn);
  
  if (is_err(return_fn)) out = rejected_promise(js, return_fn);
  else if (is_undefined(return_fn) || is_null(return_fn)) out = fulfilled_promise(js, js_mkundef());
  else if (!is_callable(return_fn)) out = rejected_promise(js, js_mkerr_typed(js, JS_ERR_TYPE, "iterator return is not callable"));
  else {
    ant_value_t arg = js_mkundef();
    ant_value_t result = sv_vm_call(js->vm, js, return_fn, self, &arg, 1, NULL, js_mkundef());
    if (!is_err(result)) result = js_promise_assimilate_awaitable(js, result);
    out = is_err(result) ? rejected_promise(js, result) : js_promise_then(js, result, Ant_Function_Create(async_iter_dispose_done), js_mkundef());
  }
  
  GC_ROOT_RESTORE(js, root_mark);
  return out;
}

static ant_value_t async_iterator_tag_get(ant_params_t) {
  return ANT_STRING("AsyncIterator");
}

static ant_value_t iterator_tag_set(ant_params_t) {
  ant_value_t receiver = js->this_val;
  
  if (
    (!is_object_type(receiver) && vtype(receiver) != kTypeBuiltin) ||
    receiver == js->sym.iterator_proto || receiver == js->sym.async_iterator_proto
  ) return js_mkerr_typed(js, JS_ERR_TYPE, "cannot set iterator prototype tag");

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, receiver);
  ant_value_t value = nargs ? args[0] : js_mkundef();
  
  GC_ROOT_PIN(js, value);
  ant_value_t descriptor = Ant_Object_CreateBare(js);
  
  GC_ROOT_PIN(js, descriptor);
  js_set(js, descriptor, "value", value);
  js_set(js, descriptor, "writable", js_true);
  js_set(js, descriptor, "enumerable", js_true);
  js_set(js, descriptor, "configurable", js_true);
  
  ant_value_t result = js_define_property(js, receiver, js->sym.toStringTag_sym, descriptor, false);
  GC_ROOT_RESTORE(js, mark);
  
  return is_err(result) ? result : js_mkundef();
}

void init_iterator_module(ant_t *js) {
  ant_value_t iter_proto = get_iterator_prototype(js);
  
  js_iter_register_advance(js, get_array_iterator_prototype(js), advance_array);
  js_iter_register_advance(js, get_string_iterator_prototype(js), advance_string);
  
  bool string_iteration_intact = ant_protector_intact(js, ANT_PROTECTOR_STRING_ITERATION);
  defmethod_sym(js, js->sym.string_proto, js->sym.iterator_sym, Ant_Function_Create(string_iterator));
  if (string_iteration_intact) ant_protector_rearm(js, ANT_PROTECTOR_STRING_ITERATION);
  
  js->builtins.wrap_iter_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.wrap_iter_proto, iter_proto);
  defmethod(js, js->builtins.wrap_iter_proto, "next", 4, Ant_Function_Create(wrap_iter_next));

  defmethod(js, iter_proto, "map", 3,     Ant_Function_CreateWithArity(iter_map, 1));
  defmethod(js, iter_proto, "filter", 6,  Ant_Function_CreateWithArity(iter_filter, 1));
  defmethod(js, iter_proto, "take", 4,    Ant_Function_CreateWithArity(iter_take, 1));
  defmethod(js, iter_proto, "drop", 4,    Ant_Function_CreateWithArity(iter_drop, 1));
  defmethod(js, iter_proto, "flatMap", 7, Ant_Function_CreateWithArity(iter_flatMap, 1));
  defmethod(js, iter_proto, "every", 5,   Ant_Function_CreateWithArity(iter_every, 1));
  defmethod(js, iter_proto, "some", 4,    Ant_Function_CreateWithArity(iter_some, 1));
  defmethod(js, iter_proto, "find", 4,    Ant_Function_CreateWithArity(iter_find, 1));
  defmethod(js, iter_proto, "forEach", 7, Ant_Function_CreateWithArity(iter_forEach, 1));
  defmethod(js, iter_proto, "reduce", 6,  Ant_Function_CreateWithArity(iter_reduce, 1));
  defmethod(js, iter_proto, "toArray", 7, Ant_Function_Create(iter_toArray));
  
  js_set_sym_getter_desc(js, iter_proto, js->sym.toStringTag_sym, Ant_Function_Create(iterator_tag_get), JS_DESC_C);
  static const ant_cfunc_meta_t dispose_meta = { iter_dispose, "[Symbol.dispose]", 0, 0 };
  
  defmethod_sym(js, iter_proto, js->sym.dispose_sym, js_mkfun_meta(&dispose_meta));
  js_set_sym_setter_desc(js, iter_proto, js->sym.toStringTag_sym, Ant_Function_Create(iterator_tag_set), JS_DESC_C);

  ant_value_t ctor_obj = Ant_Object_CreateBare(js);
  js_set_slot(ctor_obj, SLOT_CFUNC, Ant_Function_Create(iter_ctor));
  js_setprop_readonly_nonconfigurable(js, ctor_obj, "prototype", 9, iter_proto);
  js_set_function_name(js, ctor_obj, "Iterator", sizeof("Iterator") - 1);
  
  ant_value_t ctor = js_obj_to_func(js, ctor_obj);
  defmethod(js, ctor, "from", 4, Ant_Function_CreateWithArity(iter_from, 1));
  defhidden(js, iter_proto, "constructor", 11, ctor);
  js_set_global_builtin(js, "Iterator", ctor);

  ant_value_t async_iter_proto = Ant_Object_CreateBare(js);
  js->sym.async_iterator_proto = async_iter_proto;
  js_set_proto_init(async_iter_proto, js->sym.object_proto);
  
  mkprop(
    js, async_iter_proto, js->sym.asyncIterator_sym, 
    Ant_Function_Create(sym_this_cb), ANT_PROP_ATTR_WRITABLE | ANT_PROP_ATTR_CONFIGURABLE);
  
  js_set_sym_getter_desc(js, async_iter_proto, js->sym.toStringTag_sym, Ant_Function_Create(async_iterator_tag_get), JS_DESC_C);
  static const ant_cfunc_meta_t async_dispose_meta = { async_iter_async_dispose, "[Symbol.asyncDispose]", 0, 0 };
  
  defmethod_sym(js, async_iter_proto, js->sym.asyncDispose_sym, js_mkfun_meta(&async_dispose_meta));
  js_set_sym_setter_desc(js, async_iter_proto, js->sym.toStringTag_sym, Ant_Function_Create(iterator_tag_set), JS_DESC_C);

  ant_value_t async_ctor_obj = Ant_Object_CreateBare(js);
  js_set_slot(async_ctor_obj, SLOT_CFUNC, Ant_Function_Create(async_iter_ctor));
  js_setprop_readonly_nonconfigurable(js, async_ctor_obj, "prototype", 9, async_iter_proto);
  js_set_function_name(js, async_ctor_obj, "AsyncIterator", sizeof("AsyncIterator") - 1);
  
  ant_value_t async_ctor = js_obj_to_func(js, async_ctor_obj);
  defhidden(js, async_iter_proto, "constructor", 11, async_ctor);
  js_set_global_builtin(js, "AsyncIterator", async_ctor);

  js->builtins.async_wrap_iter_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.async_wrap_iter_proto, async_iter_proto);
  defmethod(js, js->builtins.async_wrap_iter_proto, "next", 4, Ant_Function_Create(async_wrap_next));
  defmethod(js, js->builtins.async_wrap_iter_proto, "return", 6, Ant_Function_Create(async_wrap_return));
  defmethod(js, js->builtins.async_wrap_iter_proto, "throw", 5, Ant_Function_Create(async_wrap_throw));

  defmethod(js, async_ctor, "from", 4, Ant_Function_Create(async_iter_from));
  defmethod(js, async_iter_proto, "map", 3, Ant_Function_Create(async_iter_map));
  defmethod(js, async_iter_proto, "filter", 6, Ant_Function_Create(async_iter_filter));
  defmethod(js, async_iter_proto, "take", 4, Ant_Function_Create(async_iter_take));
  defmethod(js, async_iter_proto, "drop", 4, Ant_Function_Create(async_iter_drop));
  defmethod(js, async_iter_proto, "flatMap", 7, Ant_Function_Create(async_iter_flatMap));
  defmethod(js, async_iter_proto, "every", 5, Ant_Function_Create(async_iter_every));
  defmethod(js, async_iter_proto, "some", 4, Ant_Function_Create(async_iter_some));
  defmethod(js, async_iter_proto, "find", 4, Ant_Function_Create(async_iter_find));
  defmethod(js, async_iter_proto, "forEach", 7, Ant_Function_Create(async_iter_forEach));
  defmethod(js, async_iter_proto, "reduce", 6, Ant_Function_Create(async_iter_reduce));
  defmethod(js, async_iter_proto, "toArray", 7, Ant_Function_Create(async_iter_toArray));
}
