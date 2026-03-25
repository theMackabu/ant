#ifndef SV_ITERATION_H
#define SV_ITERATION_H

#include "ant.h"
#include "async.h"
#include "utf8.h"
#include "silver/engine.h"
#include "modules/symbol.h"
#include "modules/collections.h"

#define SV_ITER_GENERIC  0
#define SV_ITER_ARRAY    1
#define SV_ITER_MAP      2
#define SV_ITER_SET      3
#define SV_ITER_STRING   4

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
  if (vtype(obj) != T_OBJ) return false;
  if (!g_map_iter_proto || js_get_proto(js, obj) != g_map_iter_proto) return false;
  
  ant_value_t state_val = js_get_slot(obj, SLOT_ITER_STATE);
  if (vtype(state_val) == T_UNDEF) return false;
  
  map_iterator_state_t *st = (map_iterator_state_t *)(uintptr_t)js_getnum(state_val);
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
  if (vtype(obj) != T_OBJ) return false;
  if (!g_set_iter_proto || js_get_proto(js, obj) != g_set_iter_proto) return false;
  
  ant_value_t state_val = js_get_slot(obj, SLOT_ITER_STATE);
  if (vtype(state_val) == T_UNDEF) return false;
  
  set_iterator_state_t *st = (set_iterator_state_t *)(uintptr_t)js_getnum(state_val);
  if (!st) return false;
  
  *out_state = st;
  *out_type = st->type;
  
  return true;
}

static inline ant_value_t sv_op_for_of(sv_vm_t *vm, ant_t *js) {
  ant_value_t iterable = vm->stack[--vm->sp];

  if (vtype(iterable) == T_ARR) {
    vm->stack[vm->sp++] = iterable;
    vm->stack[vm->sp++] = tov(0);
    vm->stack[vm->sp++] = tov(SV_ITER_ARRAY);
    return tov(0);
  }

  if (vtype(iterable) == T_STR) {
    if (str_is_heap_rope(iterable)) {
      iterable = rope_flatten(js, iterable);
      if (is_err(iterable)) return iterable;
    }
    vm->stack[vm->sp++] = iterable;
    vm->stack[vm->sp++] = tov(0);
    vm->stack[vm->sp++] = tov(SV_ITER_STRING);
    return tov(0);
  }

  ant_value_t iter_fn = js_get_sym(js, iterable, get_iterator_sym());
  uint8_t ft = vtype(iter_fn);
  if (ft != T_FUNC && ft != T_CFUNC) return js_mkerr(js, "not iterable");
  ant_value_t iterator = sv_vm_call(vm, js, iter_fn, iterable, NULL, 0, NULL, false);
  if (is_err(iterator)) return iterator;

  map_iterator_state_t *map_st;
  iter_type_t map_type;
  if (sv_is_map_iter(js, iterator, &map_st, &map_type)) {
    vm->stack[vm->sp++] = ANT_PTR(map_st);
    vm->stack[vm->sp++] = tov((double)map_type);
    vm->stack[vm->sp++] = tov(SV_ITER_MAP);
    return tov(0);
  }

  set_iterator_state_t *set_st;
  iter_type_t set_type;
  if (sv_is_set_iter(js, iterator, &set_st, &set_type)) {
    vm->stack[vm->sp++] = ANT_PTR(set_st);
    vm->stack[vm->sp++] = tov((double)set_type);
    vm->stack[vm->sp++] = tov(SV_ITER_SET);
    return tov(0);
  }

  ant_value_t next_method = js_getprop_fallback(js, iterator, "next");
  vm->stack[vm->sp++] = iterator;
  vm->stack[vm->sp++] = next_method;
  vm->stack[vm->sp++] = tov(SV_ITER_GENERIC);
  return tov(0);
}

static inline ant_value_t sv_op_for_await_of(sv_vm_t *vm, ant_t *js) {
  ant_value_t iterable = vm->stack[--vm->sp];
  ant_value_t iter_fn = js_get_sym(js, iterable, get_asyncIterator_sym());

  uint8_t ft = vtype(iter_fn);
  if (ft != T_FUNC && ft != T_CFUNC) {
    iter_fn = js_get_sym(js, iterable, get_iterator_sym());
    ft = vtype(iter_fn);
    if (ft != T_FUNC && ft != T_CFUNC) return js_mkerr(js, "not iterable");
  }

  ant_value_t iterator = sv_vm_call(vm, js, iter_fn, iterable, NULL, 0, NULL, false);
  if (is_err(iterator)) return iterator;
  ant_value_t next_method = js_getprop_fallback(js, iterator, "next");
  vm->stack[vm->sp++] = iterator;
  vm->stack[vm->sp++] = next_method;
  vm->stack[vm->sp++] = tov(SV_ITER_GENERIC);
  return tov(0);
}

static inline ant_value_t sv_op_iter_next(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  int hint = (int)sv_get_u8(ip + 1);
  int tag = hint ? hint : (int)js_getnum(vm->stack[vm->sp - 1]);
  switch (tag) {
  case SV_ITER_ARRAY: {
    ant_value_t arr = vm->stack[vm->sp - 3];
    int idx = (int)js_getnum(vm->stack[vm->sp - 2]);
    ant_offset_t len = js_arr_len(js, arr);
    if (idx >= (int)len) {
      vm->stack[vm->sp++] = js_mkundef();
      vm->stack[vm->sp++] = js_true;
    } else {
      vm->stack[vm->sp++] = js_arr_get(js, arr, (ant_offset_t)idx);
      vm->stack[vm->sp++] = js_false;
      vm->stack[vm->sp - 4] = tov(idx + 1);
    }
    return tov(0);
  }

  case SV_ITER_MAP: {
    map_iterator_state_t *st =
      (map_iterator_state_t *)(uintptr_t)js_getnum(vm->stack[vm->sp - 3]);
    if (!st->current) {
      vm->stack[vm->sp++] = js_mkundef();
      vm->stack[vm->sp++] = js_true;
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
      vm->stack[vm->sp++] = value;
      vm->stack[vm->sp++] = js_false;
    }
    return tov(0);
  }

  case SV_ITER_SET: {
    set_iterator_state_t *st =
      (set_iterator_state_t *)(uintptr_t)js_getnum(vm->stack[vm->sp - 3]);
    if (!st->current) {
      vm->stack[vm->sp++] = js_mkundef();
      vm->stack[vm->sp++] = js_true;
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
      vm->stack[vm->sp++] = value;
      vm->stack[vm->sp++] = js_false;
    }
    return tov(0);
  }

  case SV_ITER_STRING: {
    ant_value_t str = vm->stack[vm->sp - 3];
    int idx = (int)js_getnum(vm->stack[vm->sp - 2]);
    ant_offset_t slen = str_len_fast(js, str);
    if (idx >= (int)slen) {
      vm->stack[vm->sp++] = js_mkundef();
      vm->stack[vm->sp++] = js_true;
    } else {
      ant_offset_t off = vstr(js, str, NULL);
      utf8proc_int32_t cp;
      ant_offset_t cb_len = (ant_offset_t)utf8_next(
        (const utf8proc_uint8_t *)(uintptr_t)(off + idx),
        (utf8proc_ssize_t)(slen - idx),
        &cp
      );
      vm->stack[vm->sp++] = js_mkstr(js, (const void *)(uintptr_t)(off + idx), cb_len);
      vm->stack[vm->sp++] = js_false;
      vm->stack[vm->sp - 4] = tov(idx + (int)cb_len);
    }
    return tov(0);
  }

  default: {
    ant_value_t next_method = vm->stack[vm->sp - 2];
    ant_value_t iterator = vm->stack[vm->sp - 3];
    uint8_t ft = vtype(next_method);
    if (ft != T_FUNC && ft != T_CFUNC)
      return js_mkerr(js, "iterator.next is not a function");
    ant_value_t result = sv_vm_call(vm, js, next_method, iterator, NULL, 0, NULL, false);
    if (is_err(result)) return result;
    if (!is_object_type(result))
      return js_mkerr_typed(js, JS_ERR_TYPE, "Iterator result is not an object");
    ant_value_t done = js_getprop_fallback(js, result, "done");
    ant_value_t value = js_getprop_fallback(js, result, "value");
    vm->stack[vm->sp++] = value;
    vm->stack[vm->sp++] = mkval(T_BOOL, js_truthy(js, done));
    return tov(0);
  }}
}

static inline void sv_op_iter_get_value(sv_vm_t *vm, ant_t *js) {
  ant_value_t obj = vm->stack[--vm->sp];
  ant_value_t done = js_getprop_fallback(js, obj, "done");
  ant_value_t value = js_getprop_fallback(js, obj, "value");
  vm->stack[vm->sp++] = value;
  vm->stack[vm->sp++] = mkval(T_BOOL, js_truthy(js, done));
}

static inline void sv_op_iter_close(sv_vm_t *vm, ant_t *js) {
  int tag = (int)js_getnum(vm->stack[vm->sp - 1]);
  if (tag == SV_ITER_GENERIC) {
    ant_value_t iterator = vm->stack[vm->sp - 3];
    ant_value_t return_fn = js_getprop_fallback(js, iterator, "return");
    uint8_t ft = vtype(return_fn);
    if (ft == T_FUNC || ft == T_CFUNC)
      sv_vm_call(vm, js, return_fn, iterator, NULL, 0, NULL, false);
  }
  vm->sp -= 3;
}

static inline ant_value_t sv_op_iter_call(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  ant_value_t method = vm->stack[vm->sp - 1];
  ant_value_t iterator = vm->stack[vm->sp - 4];
  uint8_t ft = vtype(method);
  if (ft != T_FUNC && ft != T_CFUNC)
    return js_mkerr(js, "iterator method is not callable");
  ant_value_t result = sv_vm_call(vm, js, method, iterator, NULL, 0, NULL, false);
  if (is_err(result)) return result;
  vm->stack[vm->sp++] = result;
  return tov(0);
}

static inline ant_value_t sv_op_await_iter_next(sv_vm_t *vm, ant_t *js) {
  ant_value_t next_method = vm->stack[vm->sp - 2];
  ant_value_t iterator = vm->stack[vm->sp - 3];
  uint8_t ft = vtype(next_method);
  if (ft != T_FUNC && ft != T_CFUNC)
    return js_mkerr(js, "iterator.next is not a function");
  ant_value_t result = sv_vm_call(vm, js, next_method, iterator, NULL, 0, NULL, false);
  if (is_err(result)) return result;
  if (vtype(result) == T_PROMISE) {
    ant_value_t awaited = sv_await_value(js, result);
    if (is_err(awaited)) return awaited;
    result = awaited;
  }
  ant_value_t done = js_getprop_fallback(js, result, "done");
  ant_value_t value = js_getprop_fallback(js, result, "value");
  if (vtype(value) == T_PROMISE) {
    ant_value_t awaited_val = sv_await_value(js, value);
    if (is_err(awaited_val)) return awaited_val;
    value = awaited_val;
  }
  vm->stack[vm->sp++] = value;
  vm->stack[vm->sp++] = mkval(T_BOOL, js_truthy(js, done));
  return tov(0);
}

#endif
