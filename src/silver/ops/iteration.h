#ifndef SV_ITERATION_H
#define SV_ITERATION_H

#include "ant.h"
#include "async.h"
#include "utf8.h"
#include "property.h"
#include "silver/engine.h"
#include "modules/symbol.h"
#include "modules/collections.h"

enum: uint8_t {
  SV_ITER_GENERIC = 0,
  SV_ITER_ARRAY   = 1,
  SV_ITER_MAP     = 2,
  SV_ITER_SET     = 3,
  SV_ITER_STRING  = 4,
};

static inline ant_value_t sv_op_for_in(sv_vm_t *vm, ant_t *js) {
  ant_value_t obj = vm->stack[--vm->sp];
  ant_value_t keys = js_for_in_keys(js, obj);
  if (is_err(keys)) return keys;
  vm->stack[vm->sp++] = keys;
  return tov(0);
}

static inline bool sv_is_map_iter(
  ant_t *js, ant_value_t obj,
  map_iterator_state_t **out_state,
  iter_type_t *out_type
) {
  if (vtype(obj) != kTypeObject) return false;
  
  if (
    !js->builtins.map_iter_proto || 
    js_get_proto(js, obj) != js->builtins.map_iter_proto
  ) return false;
  
  map_iterator_state_t *st = get_map_iter_state(obj);
  if (!st) return false;
  
  *out_state = st;
  *out_type = st->type;
  
  return true;
}

static inline bool sv_is_set_iter(
  ant_t *js, ant_value_t obj,
  set_iterator_state_t **out_state,
  iter_type_t *out_type
) {
  if (vtype(obj) != kTypeObject) return false;
  
  if (
    !js->builtins.set_iter_proto || 
    js_get_proto(js, obj) != js->builtins.set_iter_proto
  ) return false;
  
  set_iterator_state_t *st = get_set_iter_state(obj);
  if (!st) return false;
  
  *out_state = st;
  *out_type = st->type;
  
  return true;
}

static inline ant_value_t sv_op_for_of(sv_vm_t *vm, ant_t *js) {
  ant_value_t iterable = vm->stack[--vm->sp];

  if (vtype(iterable) == kTypeArray) {
    vm->stack[vm->sp++] = iterable;
    vm->stack[vm->sp++] = tov(0);
    vm->stack[vm->sp++] = tov(SV_ITER_ARRAY);
    return tov(0);
  }

  if (vtype(iterable) == kTypeString) {
    if (str_is_heap_rope(iterable) || str_is_heap_builder(iterable)) {
      GC_ROOT_SAVE(str_root_mark, js);
      GC_ROOT_PIN(js, iterable);
      iterable = str_materialize(js, iterable);
      GC_ROOT_RESTORE(js, str_root_mark);
      if (is_err(iterable)) return iterable;
    }
    vm->stack[vm->sp++] = iterable;
    vm->stack[vm->sp++] = tov(0);
    vm->stack[vm->sp++] = tov(SV_ITER_STRING);
    return tov(0);
  }

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, iterable);

  ant_value_t iter_fn = js_get_sym(js, iterable, get_iterator_sym());
  GC_ROOT_PIN(js, iter_fn);
  if (!is_callable(iter_fn)) {
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkerr(js, "not iterable");
  }
  
  ant_value_t iterator = sv_vm_call(vm, js, iter_fn, iterable, NULL, 0, NULL, false);
  if (is_err(iterator)) {
    GC_ROOT_RESTORE(js, root_mark);
    return iterator;
  }
  
  GC_ROOT_PIN(js, iterator);
  map_iterator_state_t *map_st;
  iter_type_t map_type;
  
  if (sv_is_map_iter(js, iterator, &map_st, &map_type)) {
    vm->stack[vm->sp++] = iterator;
    vm->stack[vm->sp++] = tov((double)map_type);
    vm->stack[vm->sp++] = tov(SV_ITER_MAP);
    GC_ROOT_RESTORE(js, root_mark);
    return tov(0);
  }

  set_iterator_state_t *set_st;
  iter_type_t set_type;
  if (sv_is_set_iter(js, iterator, &set_st, &set_type)) {
    vm->stack[vm->sp++] = iterator;
    vm->stack[vm->sp++] = tov((double)set_type);
    vm->stack[vm->sp++] = tov(SV_ITER_SET);
    GC_ROOT_RESTORE(js, root_mark);
    return tov(0);
  }

  ant_value_t next_method = js_getprop_fallback(js, iterator, "next");
  GC_ROOT_PIN(js, next_method);
  vm->stack[vm->sp++] = iterator;
  vm->stack[vm->sp++] = next_method;
  vm->stack[vm->sp++] = tov(SV_ITER_GENERIC);
  GC_ROOT_RESTORE(js, root_mark);
  
  return tov(0);
}

static inline bool sv_array_iter_pristine(ant_t *js, ant_value_t arr) {
  if (is_callable(js_get_sym(js, arr, get_asyncIterator_sym()))) return false;
  return js_get_sym(js, arr, get_iterator_sym()) == js->sym.array_values_fn;
}

static inline ant_value_t sv_op_for_await_of(sv_vm_t *vm, ant_t *js) {
  ant_value_t iterable = vm->stack[--vm->sp];

  if (vtype(iterable) == kTypeArray && sv_array_iter_pristine(js, iterable)) {
    vm->stack[vm->sp++] = iterable;
    vm->stack[vm->sp++] = tov(0);
    vm->stack[vm->sp++] = SV_AITER_ARRAY_TAG;
    return tov(0);
  }

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, iterable);

  ant_value_t iter_fn = js_get_sym(js, iterable, get_asyncIterator_sym());
  GC_ROOT_PIN(js, iter_fn);

  if (!is_callable(iter_fn)) {
    iter_fn = js_get_sym(js, iterable, get_iterator_sym());
    GC_ROOT_PIN(js, iter_fn);
    if (!is_callable(iter_fn)) {
      GC_ROOT_RESTORE(js, root_mark);
      return js_mkerr(js, "not iterable");
    }
  }

  ant_value_t iterator = sv_vm_call(vm, js, iter_fn, iterable, NULL, 0, NULL, false);
  if (is_err(iterator)) {
    GC_ROOT_RESTORE(js, root_mark);
    return iterator;
  }
  GC_ROOT_PIN(js, iterator);

  ant_value_t next_method = js_getprop_fallback(js, iterator, "next");
  GC_ROOT_PIN(js, next_method);
  vm->stack[vm->sp++] = iterator;
  vm->stack[vm->sp++] = next_method;
  vm->stack[vm->sp++] = tov(SV_ITER_GENERIC);
  GC_ROOT_RESTORE(js, root_mark);
  return tov(0);
}

static inline ant_value_t sv_iter_result_get_named(
  ant_t *js,
  ant_value_t result,
  const char *interned,
  const char *key,
  ant_offset_t key_len
) {
  ant_object_t *ptr = is_object_type(result) 
    ? js_obj_ptr(js_as_obj(result)) 
    : NULL;
    
  ant_value_t out = js_mkundef();
  bool should_fallback = false;

  if (interned && sv_try_get_shape_data_prop(js, ptr, interned, &out, &should_fallback)) return out;
  return js_getprop_fallback_len(js, result, key, (size_t)key_len);
}

static inline void sv_iter_result_unpack(
  ant_t *js, ant_value_t result,
  ant_value_t *out_done, ant_value_t *out_value
) {
  *out_done = sv_iter_result_get_named(js, result, js->intern.done, "done", 4);
  *out_value = sv_iter_result_get_named(js, result, js->intern.value, "value", 5);
}

static inline ant_value_t sv_iter_advance(
  sv_vm_t *vm, ant_t *js, int hint, ant_value_t *out_value, bool *out_done
) {
  int tag = hint ? hint : (int)js_getnum(vm->stack[vm->sp - 1]);
  switch (tag) {
  case SV_ITER_ARRAY: {
    ant_value_t arr = vm->stack[vm->sp - 3];
    int idx = (int)js_getnum(vm->stack[vm->sp - 2]);
    ant_offset_t len = js_arr_len(js, arr);
    if (idx >= (int)len) {
      *out_value = js_mkundef();
      *out_done = true;
    } else {
      *out_value = js_arr_get(js, arr, (ant_offset_t)idx);
      *out_done = false;
      vm->stack[vm->sp - 2] = tov(idx + 1);
    }
    return tov(0);
  }

  case SV_ITER_MAP: {
    map_iterator_state_t *st = get_map_iter_state(vm->stack[vm->sp - 3]);
    if (!st) return js_mkerr(js, "Invalid Map iterator");
    if (!st->current) {
      *out_value = js_mkundef();
      *out_done = true;
    } else {
      map_entry_t *entry = st->current;
      ant_value_t value;
      switch (st->type) {
      case ITER_TYPE_MAP_VALUES:
        value = entry->value;
        break;
      case ITER_TYPE_MAP_KEYS:
        value = entry->key_val;
        break;
      case ITER_TYPE_MAP_ENTRIES: {
        ant_value_t pair = js_mkarr(js);
        js_arr_push(js, pair, entry->key_val);
        js_arr_push(js, pair, entry->value);
        value = pair;
        break;
      }
      default:
        value = js_mkundef();
      }
      st->current = entry->hh.next;
      *out_value = value;
      *out_done = false;
    }
    return tov(0);
  }

  case SV_ITER_SET: {
    set_iterator_state_t *st = get_set_iter_state(vm->stack[vm->sp - 3]);
    if (!st) return js_mkerr(js, "Invalid Set iterator");
    if (!st->current) {
      *out_value = js_mkundef();
      *out_done = true;
    } else {
      set_entry_t *entry = st->current;
      ant_value_t value;
      if (st->type == ITER_TYPE_SET_ENTRIES) {
        ant_value_t pair = js_mkarr(js);
        js_arr_push(js, pair, entry->value);
        js_arr_push(js, pair, entry->value);
        value = pair;
      } else {
        value = entry->value;
      }
      st->current = entry->hh.next;
      *out_value = value;
      *out_done = false;
    }
    return tov(0);
  }

  case SV_ITER_STRING: {
    ant_value_t str = vm->stack[vm->sp - 3];
    int idx = (int)js_getnum(vm->stack[vm->sp - 2]);
    ant_offset_t slen = str_len_fast(js, str);
    if (idx >= (int)slen) {
      *out_value = js_mkundef();
      *out_done = true;
    } else {
      ant_offset_t off = vstr(js, str, NULL);
      utf8proc_int32_t cp;
      ant_offset_t cb_len = (ant_offset_t)utf8_next(
        (const utf8proc_uint8_t *)(uintptr_t)(off + idx),
        (utf8proc_ssize_t)(slen - idx),
        &cp
      );
      *out_value = js_mkstr(js, (const void *)(uintptr_t)(off + idx), cb_len);
      *out_done = false;
      vm->stack[vm->sp - 2] = tov(idx + (int)cb_len);
    }
    return tov(0);
  }

  default: {
    ant_value_t next_method = vm->stack[vm->sp - 2];
    ant_value_t iterator = vm->stack[vm->sp - 3];
    if (!is_callable(next_method))
      return js_mkerr(js, "iterator.next is not a function");
    ant_value_t result = sv_vm_call(vm, js, next_method, iterator, NULL, 0, NULL, false);
    if (is_err(result)) return result;
    if (!is_object_type(result))
      return js_mkerr_typed(js, JS_ERR_TYPE, "Iterator result is not an object");
    ant_value_t done = js_mkundef();
    sv_iter_result_unpack(js, result, &done, out_value);
    if (is_err(done)) return done;
    if (is_err(*out_value)) return *out_value;
    *out_done = js_truthy(js, done);
    return tov(0);
  }}
}

static inline ant_value_t sv_op_iter_next(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  int hint = (int)sv_get_u8(ip + 1);
  ant_value_t value;
  bool done = false;
  
  ant_value_t status = sv_iter_advance(vm, js, hint, &value, &done);
  if (is_err(status)) return status;
  vm->stack[vm->sp++] = value;
  vm->stack[vm->sp++] = done ? js_true : js_false;
  
  return tov(0);
}

static inline void sv_op_iter_get_value(sv_vm_t *vm, ant_t *js) {
  ant_value_t obj = vm->stack[--vm->sp];
  ant_value_t done = js_mkundef();
  ant_value_t value = js_mkundef();
  sv_iter_result_unpack(js, obj, &done, &value);
  vm->stack[vm->sp++] = value;
  vm->stack[vm->sp++] = mkval(kTypeBool, js_truthy(js, done));
}

static inline void sv_op_iter_close(sv_vm_t *vm, ant_t *js) {
  ant_value_t tag_val = vm->stack[vm->sp - 1];
  if (vtype(tag_val) != kTypeNumber) {
    vm->sp -= 3;
    return;
  }

  int tag = (int)js_getnum(tag_val);
  if (tag == SV_ITER_GENERIC) {
    ant_value_t iterator = vm->stack[vm->sp - 3];
    ant_value_t return_fn = js_getprop_fallback(js, iterator, "return");
    if (is_callable(return_fn))
      sv_vm_call(vm, js, return_fn, iterator, NULL, 0, NULL, false);
  }
  vm->sp -= 3;
}

static inline ant_value_t sv_op_iter_close_async(sv_vm_t *vm, ant_t *js) {
  ant_value_t result = SV_AITER_CLOSE_SKIP;
  ant_value_t tag_val = vm->stack[vm->sp - 1];

  if (vtype(tag_val) == kTypeNumber && (int)js_getnum(tag_val) == SV_ITER_GENERIC) {
    ant_value_t iterator = vm->stack[vm->sp - 3];
    ant_value_t return_fn = js_getprop_fallback(js, iterator, "return");
    if (is_err(return_fn)) {
      vm->sp -= 3;
      return return_fn;
    }
    int return_type = vtype(return_fn);
    if (return_type != kTypeUndefined && return_type != kTypeNull) {
      if (!is_callable(return_fn)) {
        vm->sp -= 3;
        return js_mkerr_typed(js, JS_ERR_TYPE, "iterator.return is not a function");
      }
      result = sv_vm_call(vm, js, return_fn, iterator, NULL, 0, NULL, false);
      if (is_err(result)) {
        vm->sp -= 3;
        return result;
      }
    }
  }

  vm->sp -= 3;
  vm->stack[vm->sp++] = result;
  return tov(0);
}

static inline ant_value_t sv_op_iter_close_check(sv_vm_t *vm, ant_t *js) {
  ant_value_t result = vm->stack[--vm->sp];
  if (result == SV_AITER_CLOSE_SKIP || is_object_type(result)) return tov(0);
  return js_mkerr_typed(js, JS_ERR_TYPE, "Async iterator return result is not an object");
}

static inline ant_value_t sv_op_destructure_init(sv_vm_t *vm, ant_t *js) {
  return sv_op_for_of(vm, js);
}

static inline void sv_op_destructure_close(sv_vm_t *vm, ant_t *js) {
  sv_op_iter_close(vm, js);
}

static inline ant_value_t sv_op_destructure_next(sv_vm_t *vm, ant_t *js) {
  ant_value_t value;
  bool done = false;
  
  ant_value_t status = sv_iter_advance(vm, js, 0, &value, &done);
  if (is_err(status)) return status;
  vm->stack[vm->sp++] = done ? js_mkundef() : value;
  
  return tov(0);
}

static inline ant_value_t sv_op_destructure_rest(sv_vm_t *vm, ant_t *js) {
  ant_value_t rest = js_mkarr(js);
  for (;;) {
    ant_value_t value;
    bool done = false;
    ant_value_t status = sv_iter_advance(vm, js, 0, &value, &done);
    if (is_err(status)) return status;
    if (done) break;
    js_arr_push(js, rest, value);
  }
  vm->stack[vm->sp++] = rest;
  return tov(0);
}

static inline ant_value_t sv_op_iter_call(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  ant_value_t method = vm->stack[vm->sp - 1];
  ant_value_t iterator = vm->stack[vm->sp - 4];
  if (!is_callable(method))
    return js_mkerr(js, "iterator method is not callable");
  ant_value_t result = sv_vm_call(vm, js, method, iterator, NULL, 0, NULL, false);
  if (is_err(result)) return result;
  vm->stack[vm->sp++] = result;
  return tov(0);
}

static inline sv_await_result_t sv_op_await_iter_next(sv_vm_t *vm, ant_t *js) {
  sv_await_result_t out = {
    .state = SV_AWAIT_READY,
    .value = js_mkundef(),
  };

  if (vm->sp >= 4 && vm->stack[vm->sp - 2] == SV_AITER_AWAIT_MARK) {
    ant_value_t value = vm->stack[--vm->sp];
    vm->stack[vm->sp - 1] = SV_AITER_ARRAY_TAG;
    vm->stack[vm->sp++] = value;
    vm->stack[vm->sp++] = mkval(kTypeBool, 0);
    return out;
  }

  if (vm->sp >= 3 && vm->stack[vm->sp - 1] == SV_AITER_ARRAY_TAG) {
    ant_value_t arr = vm->stack[vm->sp - 3];
    int idx = (int)js_getnum(vm->stack[vm->sp - 2]);
    ant_offset_t len = js_arr_len(js, arr);

    if (idx >= (int)len) {
      vm->stack[vm->sp++] = js_mkundef();
      vm->stack[vm->sp++] = mkval(kTypeBool, 1);
      return out;
    }

    ant_value_t value = js_arr_get(js, arr, (ant_offset_t)idx);
    vm->stack[vm->sp - 2] = tov(idx + 1);

    if (!is_object_type(value)) {
      vm->stack[vm->sp++] = value;
      vm->stack[vm->sp++] = mkval(kTypeBool, 0);
      return out;
    }

    vm->stack[vm->sp - 1] = SV_AITER_AWAIT_MARK;
    vm->suspended_entry_fp = vm->fp;
    vm->suspended_saved_fp = vm->fp - 1;
    sv_await_result_t awaited = sv_await_value(vm, js, value);
    vm->suspended_entry_fp = -1;
    vm->suspended_saved_fp = -1;
    if (awaited.state != SV_AWAIT_READY) return awaited;

    vm->stack[vm->sp - 1] = SV_AITER_ARRAY_TAG;
    vm->stack[vm->sp++] = awaited.value;
    vm->stack[vm->sp++] = mkval(kTypeBool, 0);
    return out;
  }

  bool has_resumed_value = vm->sp >= 5 &&
    vtype(vm->stack[vm->sp - 2]) == kTypeBool &&
    vm->stack[vm->sp - 3] == SV_AITER_STEP_MARK;

  if (has_resumed_value) {
    ant_value_t value = vm->stack[--vm->sp];
    ant_value_t done = vm->stack[--vm->sp];
    vm->stack[vm->sp - 1] = js_truthy(js, done)
      ? SV_AITER_STEP_MARK
      : tov(SV_ITER_GENERIC);
    vm->stack[vm->sp++] = value;
    vm->stack[vm->sp++] = done;
    return out;
  }

  ant_value_t result = js_mkundef();
  bool has_resumed_result = vm->sp >= 4 &&
    vm->stack[vm->sp - 2] == SV_AITER_STEP_MARK;

  if (has_resumed_result) result = vm->stack[--vm->sp];
  else {
    ant_value_t next_method = vm->stack[vm->sp - 2];
    ant_value_t iterator = vm->stack[vm->sp - 3];
    vm->stack[vm->sp - 1] = SV_AITER_STEP_MARK;
    if (!is_callable(next_method)) return (sv_await_result_t){ 
      .state = SV_AWAIT_ERROR,
      .value = js_mkerr(js, "iterator.next is not a function") 
    };
    
    result = sv_vm_call(
      vm, js,
      next_method, iterator,
      NULL, 0, NULL, false
    );
    
    if (is_err(result)) return (sv_await_result_t){ 
      .state = SV_AWAIT_ERROR,
      .value = result 
    };
  }

  if (!has_resumed_result && vtype(result) == kTypePromise) {
    vm->suspended_entry_fp = vm->fp;
    vm->suspended_saved_fp = vm->fp - 1;
    
    sv_await_result_t awaited = sv_await_value(vm, js, result);
    vm->suspended_entry_fp = -1;
    vm->suspended_saved_fp = -1;
    
    if (awaited.state != SV_AWAIT_READY) return awaited;
    result = awaited.value;
  }
  
  ant_value_t done = js_mkundef();
  ant_value_t value = js_mkundef();
  sv_iter_result_unpack(js, result, &done, &value);
  
  if (is_err(done)) return (sv_await_result_t){ 
    .state = SV_AWAIT_ERROR,
    .value = done
  };
  
  if (is_err(value)) return (sv_await_result_t){
    .state = SV_AWAIT_ERROR,
    .value = value
  };
  
  if (vtype(value) == kTypePromise) {
    vm->stack[vm->sp++] = mkval(kTypeBool, js_truthy(js, done));
    vm->suspended_entry_fp = vm->fp;
    vm->suspended_saved_fp = vm->fp - 1;
    
    sv_await_result_t awaited_val = sv_await_value(vm, js, value);
    vm->suspended_entry_fp = -1;
    vm->suspended_saved_fp = -1;
    
    if (awaited_val.state != SV_AWAIT_READY) return awaited_val;
    vm->sp--;
    value = awaited_val.value;
  }
  
  bool is_done = js_truthy(js, done);
  vm->stack[vm->sp - 1] = is_done ? SV_AITER_STEP_MARK : tov(SV_ITER_GENERIC);
  vm->stack[vm->sp++] = value;
  vm->stack[vm->sp++] = mkval(kTypeBool, is_done);
  
  return out;
}

#endif
