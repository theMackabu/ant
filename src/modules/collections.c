#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "ant.h"
#include "gc.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "silver/engine.h"
#include "descriptors.h"

#include "modules/bigint.h"
#include "modules/collections.h"
#include "modules/symbol.h"

ant_value_t g_map_iter_proto = 0;
ant_value_t g_set_iter_proto = 0;

typedef struct {
  unsigned char stack[32];
  unsigned char *bytes;
  size_t len;
} collection_key_t;

static ant_value_t normalize_map_key(ant_value_t key) {
  if (vtype(key) == T_NUM) {
    double d = tod(key);
    if (d == 0.0 && signbit(d)) return js_mknum(0.0);
  }
  return key;
}

static void collection_key_reset(collection_key_t *key) {
  key->bytes = key->stack;
  key->len = 0;
}

static void collection_key_free(collection_key_t *key) {
  if (key->bytes != key->stack) free(key->bytes);
  collection_key_reset(key);
}

static bool collection_key_reserve(collection_key_t *key, size_t len) {
  if (len <= sizeof(key->stack)) return true;
  unsigned char *heap = malloc(len);
  if (!heap) return false;
  key->bytes = heap;
  return true;
}

static bool collection_key_init(ant_t *js, ant_value_t input, collection_key_t *out) {
  collection_key_reset(out);

  ant_value_t key = normalize_map_key(input);
  uint8_t tag = (uint8_t)vtype(key);

  if (vtype(key) == T_STR) {
    size_t str_len = 0;
    const char *str = js_getstr(js, key, &str_len);
    out->len = 1 + str_len;
    if (!collection_key_reserve(out, out->len)) return false;
    out->bytes[0] = tag;
    if (str_len > 0) memcpy(out->bytes + 1, str, str_len);
    return true;
  }

  if (vtype(key) == T_BIGINT) {
    size_t str_len = bigint_digits_len(js, key) + (bigint_is_negative(js, key) ? 1 : 0);
    out->len = 1 + str_len;
    if (!collection_key_reserve(out, out->len)) return false;
    out->bytes[0] = tag;
    if (str_len > 0) strbigint(js, key, (char *)(out->bytes + 1), str_len + 1);
    return true;
  }

  out->len = 1 + sizeof(ant_value_t);
  if (!collection_key_reserve(out, out->len)) return false;
  out->bytes[0] = tag;
  memcpy(out->bytes + 1, &key, sizeof(ant_value_t));
  
  return true;
}

static map_entry_t *map_find_entry(ant_t *js, map_entry_t **map_ptr, ant_value_t key_val) {
  collection_key_t key;
  if (!collection_key_init(js, key_val, &key)) return NULL;
  
  map_entry_t *entry = NULL;
  HASH_FIND(hh, *map_ptr, key.bytes, key.len, entry);
  collection_key_free(&key);
  
  return entry;
}

static set_entry_t *set_find_entry(ant_t *js, set_entry_t **set_ptr, ant_value_t value) {
  collection_key_t key;
  if (!collection_key_init(js, value, &key)) return NULL;
  
  set_entry_t *entry = NULL;
  HASH_FIND(hh, *set_ptr, key.bytes, key.len, entry);
  collection_key_free(&key);
  
  return entry;
}

static bool map_store_entry(
  ant_t *js,
  map_entry_t **map_ptr,
  ant_value_t raw_key,
  ant_value_t key_val,
  ant_value_t value
) {
  collection_key_t key;
  if (!collection_key_init(js, raw_key, &key)) return false;

  map_entry_t *entry = NULL;
  HASH_FIND(hh, *map_ptr, key.bytes, key.len, entry);
  if (entry) {
    entry->key_val = key_val;
    entry->value = value;
    collection_key_free(&key);
    return true;
  }

  entry = ant_calloc(sizeof(map_entry_t));
  if (!entry) {
    collection_key_free(&key);
    return false;
  }

  entry->key = malloc(key.len);
  if (!entry->key) {
    collection_key_free(&key);
    free(entry);
    return false;
  }

  memcpy(entry->key, key.bytes, key.len);
  entry->key_len = key.len;
  entry->key_val = key_val;
  entry->value = value;
  
  HASH_ADD_KEYPTR(hh, *map_ptr, entry->key, entry->key_len, entry);
  collection_key_free(&key);
  
  return true;
}

static bool set_store_entry(ant_t *js, set_entry_t **set_ptr, ant_value_t value) {
  collection_key_t key;
  if (!collection_key_init(js, value, &key)) return false;

  set_entry_t *entry = NULL;
  HASH_FIND(hh, *set_ptr, key.bytes, key.len, entry);
  if (entry) {
    collection_key_free(&key);
    return true;
  }

  entry = ant_calloc(sizeof(set_entry_t));
  if (!entry) {
    collection_key_free(&key);
    return false;
  }

  entry->key = malloc(key.len);
  if (!entry->key) {
    collection_key_free(&key);
    free(entry);
    return false;
  }

  memcpy(entry->key, key.bytes, key.len);
  entry->key_len = key.len;
  entry->value = value;
  
  HASH_ADD_KEYPTR(hh, *set_ptr, entry->key, entry->key_len, entry);
  collection_key_free(&key);
  
  return true;
}

static ant_value_t map_init_from_iterable(ant_t *js, map_entry_t **map_head, ant_value_t iterable) {
  js_iter_t it;
  if (!js_iter_open(js, iterable, &it)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Map constructor argument is not iterable");
  }

  ant_value_t entry;
  while (js_iter_next(js, &it, &entry)) {
  uint8_t entry_t = vtype(entry);
  if (entry_t != T_ARR && entry_t != T_OBJ) {
    js_iter_close(js, &it);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Map iterable entries must be pair sequences");
  }

  ant_offset_t entry_len = js_arr_len(js, entry);
  if (entry_len < 2) {
    js_iter_close(js, &it);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Map iterable entries must have at least 2 items");
  }

  ant_value_t key = normalize_map_key(js_arr_get(js, entry, 0));
  ant_value_t value = js_arr_get(js, entry, 1);
  if (!map_store_entry(js, map_head, key, key, value)) {
    js_iter_close(js, &it);
    return js_mkerr(js, "out of memory");
  }}

  return js_mkundef();
}

static ant_value_t set_init_from_iterable(ant_t *js, set_entry_t **set_head, ant_value_t iterable) {
  js_iter_t it;
  if (!js_iter_open(js, iterable, &it)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Set constructor argument is not iterable");
  }

  ant_value_t value;
  while (js_iter_next(js, &it, &value)) if (!set_store_entry(js, set_head, value)) {
    js_iter_close(js, &it);
    return js_mkerr(js, "out of memory");
  }

  return js_mkundef();
}

static ant_value_t weakmap_init_from_iterable(ant_t *js, weakmap_entry_t **wm_head, ant_value_t iterable) {
  js_iter_t it;
  if (!js_iter_open(js, iterable, &it)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "WeakMap constructor argument is not iterable");
  }

  ant_value_t entry;
  while (js_iter_next(js, &it, &entry)) {
    uint8_t entry_t = vtype(entry);
    if (entry_t != T_ARR && entry_t != T_OBJ) {
      js_iter_close(js, &it);
      return js_mkerr_typed(js, JS_ERR_TYPE, "WeakMap iterable entries must be pair sequences");
    }
    
    ant_offset_t entry_len = js_arr_len(js, entry);
    if (entry_len < 2) {
      js_iter_close(js, &it);
      return js_mkerr_typed(js, JS_ERR_TYPE, "WeakMap iterable entries must have at least 2 items");
    }
    
    ant_value_t key = js_arr_get(js, entry, 0);
    ant_value_t value = js_arr_get(js, entry, 1);
    if (!is_object_type(key)) {
      js_iter_close(js, &it);
      return js_mkerr(js, "WeakMap key must be an object");
    }
    
    weakmap_entry_t *wm_entry;
    HASH_FIND(hh, *wm_head, &key, sizeof(ant_value_t), wm_entry);
    if (wm_entry) {
      wm_entry->value = value;
      continue;
    }
    
    wm_entry = ant_calloc(sizeof(weakmap_entry_t));
    if (!wm_entry) {
      js_iter_close(js, &it);
      return js_mkerr(js, "out of memory");
    }
    
    wm_entry->key_obj = key;
    wm_entry->value = value;
    HASH_ADD(hh, *wm_head, key_obj, sizeof(ant_value_t), wm_entry);
  }

  return js_mkundef();
}

static ant_value_t weakset_init_from_iterable(ant_t *js, weakset_entry_t **ws_head, ant_value_t iterable) {
  js_iter_t it;
  if (!js_iter_open(js, iterable, &it)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "WeakSet constructor argument is not iterable");
  }

  ant_value_t value;
  while (js_iter_next(js, &it, &value)) {
    if (!is_object_type(value)) {
      js_iter_close(js, &it);
      return js_mkerr(js, "WeakSet value must be an object");
    }
    
    weakset_entry_t *entry;
    HASH_FIND(hh, *ws_head, &value, sizeof(ant_value_t), entry);
    if (entry) continue;
    
    entry = ant_calloc(sizeof(weakset_entry_t));
    if (!entry) {
      js_iter_close(js, &it);
      return js_mkerr(js, "out of memory");
    }
    
    entry->value_obj = value;
    HASH_ADD(hh, *ws_head, value_obj, sizeof(ant_value_t), entry);
  }

  return js_mkundef();
}

map_entry_t **get_map_from_obj(ant_t *js, ant_value_t obj) {
  (void)js;
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || ptr->type_tag != T_MAP) return NULL;
  return (map_entry_t **)(uintptr_t)js_getnum(ptr->u.data.value);
}

set_entry_t **get_set_from_obj(ant_t *js, ant_value_t obj) {
  (void)js;
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || ptr->type_tag != T_SET) return NULL;
  return (set_entry_t **)(uintptr_t)js_getnum(ptr->u.data.value);
}

static weakmap_entry_t **get_weakmap_from_obj(ant_t *js, ant_value_t obj) {
  (void)js;
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || ptr->type_tag != T_WEAKMAP) return NULL;
  return (weakmap_entry_t **)(uintptr_t)js_getnum(ptr->u.data.value);
}

static weakset_entry_t **get_weakset_from_obj(ant_t *js, ant_value_t obj) {
  (void)js;
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || ptr->type_tag != T_WEAKSET) return NULL;
  return (weakset_entry_t **)(uintptr_t)js_getnum(ptr->u.data.value);
}

static map_iterator_state_t *get_map_iter_state(ant_t *js, ant_value_t obj) {
  ant_value_t state_val = js_get_slot(obj, SLOT_ITER_STATE);
  if (vtype(state_val) == T_UNDEF) return NULL;
  return (map_iterator_state_t *)(uintptr_t)js_getnum(state_val);
}

static set_iterator_state_t *get_set_iter_state(ant_t *js, ant_value_t obj) {
  ant_value_t state_val = js_get_slot(obj, SLOT_ITER_STATE);
  if (vtype(state_val) == T_UNDEF) return NULL;
  return (set_iterator_state_t *)(uintptr_t)js_getnum(state_val);
}

static ant_value_t map_set(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "Map.set() requires 2 arguments");
  
  ant_value_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(js, this_val);
  if (!map_ptr) return js_mkerr(js, "Invalid Map object");
  
  ant_value_t key_val = normalize_map_key(args[0]);
  if (!map_store_entry(js, map_ptr, args[0], key_val, args[1]))
    return js_mkerr(js, "out of memory");

  ant_object_t *map_obj = js_obj_ptr(this_val);
  if (map_obj) {
    gc_write_barrier(js, map_obj, key_val);
    gc_write_barrier(js, map_obj, args[1]);
  }

  return this_val;
}

static ant_value_t map_get(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Map.get() requires 1 argument");
  
  ant_value_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(js, this_val);
  if (!map_ptr) return js_mkundef();

  map_entry_t *entry = map_find_entry(js, map_ptr, args[0]);
  return entry ? entry->value : js_mkundef();
}

static ant_value_t map_has(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Map.has() requires 1 argument");
  
  ant_value_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(js, this_val);
  
  if (!map_ptr) return js_false;
  map_entry_t *entry = map_find_entry(js, map_ptr, args[0]);
  return js_bool(entry != NULL);
}

static ant_value_t map_delete(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Map.delete() requires 1 argument");
  
  ant_value_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(js, this_val);
  
  if (!map_ptr) return js_false;
  map_entry_t *entry = map_find_entry(js, map_ptr, args[0]);
  if (entry) {
    HASH_DEL(*map_ptr, entry);
    free(entry->key);
    free(entry);
    return js_true;
  }
  return js_false;
}

static ant_value_t map_clear(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(js, this_val);
  if (!map_ptr) return js_mkundef();
  
  map_entry_t *entry, *tmp;
  HASH_ITER(hh, *map_ptr, entry, tmp) {
    HASH_DEL(*map_ptr, entry);
    free(entry->key);
    free(entry);
  }
  *map_ptr = NULL;
  
  return js_mkundef();
}

static ant_value_t map_size(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(js, this_val);
  if (!map_ptr) return js_mknum(0);
  
  return js_mknum((double)HASH_COUNT(*map_ptr));
}

static ant_value_t map_forEach(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(js, this_val);
  
  if (nargs < 1 || vtype(args[0]) != T_FUNC)
    return js_mkerr(js, "forEach requires a callback function");
  
  ant_value_t callback = args[0];
  
  if (map_ptr && *map_ptr) {
  map_entry_t *entry, *tmp;
  HASH_ITER(hh, *map_ptr, entry, tmp) {
    ant_value_t k = entry->key_val;
    ant_value_t call_args[3] = { entry->value, k, this_val };
    ant_value_t result = sv_vm_call(js->vm, js, callback, js_mkundef(), call_args, 3, NULL, false);
    if (is_err(result)) return result;
  }}
  
  return js_mkundef();
}

bool advance_map(ant_t *js, js_iter_t *it, ant_value_t *out) {
  map_iterator_state_t *state = get_map_iter_state(js, it->iterator);
  if (!state || !state->current) return false;

  map_entry_t *entry = state->current;
  switch (state->type) {
    case ITER_TYPE_MAP_VALUES:
      *out = entry->value;
      break;
    case ITER_TYPE_MAP_KEYS:
      *out = entry->key_val;
      break;
    case ITER_TYPE_MAP_ENTRIES: {
      ant_value_t pair = js_mkarr(js);
      js_arr_push(js, pair, entry->key_val);
      js_arr_push(js, pair, entry->value);
      *out = pair;
      break;
    }
    default: *out = js_mkundef();
  }
  
  state->current = entry->hh.next;
  return true;
}

static ant_value_t map_iter_next(ant_t *js, ant_value_t *args, int nargs) {
  js_iter_t it = { .iterator = js->this_val };
  ant_value_t value;
  return js_iter_result(js, advance_map(js, &it, &value), value);
}

static ant_value_t create_map_iterator(ant_t *js, ant_value_t map_obj, iter_type_t type) {
  map_entry_t **map_ptr = get_map_from_obj(js, map_obj);
  
  map_iterator_state_t *state = ant_calloc(sizeof(map_iterator_state_t));
  if (!state) return js_mkerr(js, "out of memory");
  
  state->head = map_ptr;
  state->current = map_ptr ? *map_ptr : NULL;
  state->type = type;
  
  ant_value_t iter = js_mkobj(js);
  js_set_proto_init(iter, g_map_iter_proto);
  js_set_slot(iter, SLOT_ITER_STATE, ANT_PTR(state));
  
  return iter;
}

static ant_value_t map_values(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  return create_map_iterator(js, js->this_val, ITER_TYPE_MAP_VALUES);
}

static ant_value_t map_keys(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  return create_map_iterator(js, js->this_val, ITER_TYPE_MAP_KEYS);
}

static ant_value_t map_entries(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  return create_map_iterator(js, js->this_val, ITER_TYPE_MAP_ENTRIES);
}

bool advance_set(ant_t *js, js_iter_t *it, ant_value_t *out) {
  set_iterator_state_t *state = get_set_iter_state(js, it->iterator);
  if (!state || !state->current) return false;

  set_entry_t *entry = state->current;
  if (state->type == ITER_TYPE_SET_ENTRIES) {
    ant_value_t pair = js_mkarr(js);
    js_arr_push(js, pair, entry->value);
    js_arr_push(js, pair, entry->value);
    *out = pair;
  } else *out = entry->value;
  
  state->current = entry->hh.next;
  return true;
}

static ant_value_t set_iter_next(ant_t *js, ant_value_t *args, int nargs) {
  js_iter_t it = { .iterator = js->this_val };
  ant_value_t value;
  return js_iter_result(js, advance_set(js, &it, &value), value);
}

static ant_value_t create_set_iterator(ant_t *js, ant_value_t set_obj, iter_type_t type) {
  set_entry_t **set_ptr = get_set_from_obj(js, set_obj);
  
  set_iterator_state_t *state = ant_calloc(sizeof(set_iterator_state_t));
  if (!state) return js_mkerr(js, "out of memory");
  
  state->head = set_ptr;
  state->current = set_ptr ? *set_ptr : NULL;
  state->type = type;
  
  ant_value_t iter = js_mkobj(js);
  js_set_proto_init(iter, g_set_iter_proto);
  js_set_slot(iter, SLOT_ITER_STATE, ANT_PTR(state));
  
  return iter;
}

static ant_value_t set_add(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Set.add() requires 1 argument");
  
  ant_value_t this_val = js->this_val;
  set_entry_t **set_ptr = get_set_from_obj(js, this_val);
  if (!set_ptr) return js_mkerr(js, "Invalid Set object");
  
  if (!set_store_entry(js, set_ptr, args[0]))
    return js_mkerr(js, "out of memory");

  ant_object_t *set_obj = js_obj_ptr(this_val);
  if (set_obj) gc_write_barrier(js, set_obj, args[0]);

  return this_val;
}

static ant_value_t set_has(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Set.has() requires 1 argument");
  
  ant_value_t this_val = js->this_val;
  set_entry_t **set_ptr = get_set_from_obj(js, this_val);
  if (!set_ptr) return js_false;

  set_entry_t *entry = set_find_entry(js, set_ptr, args[0]);
  return js_bool(entry != NULL);
}

static ant_value_t set_delete(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Set.delete() requires 1 argument");
  
  ant_value_t this_val = js->this_val;
  set_entry_t **set_ptr = get_set_from_obj(js, this_val);
  if (!set_ptr) return js_false;

  set_entry_t *entry = set_find_entry(js, set_ptr, args[0]);
  
  if (entry) {
    HASH_DEL(*set_ptr, entry);
    free(entry->key);
    free(entry);
    return js_true;
  }
  return js_false;
}

static ant_value_t set_clear(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t this_val = js->this_val;
  set_entry_t **set_ptr = get_set_from_obj(js, this_val);
  if (!set_ptr) return js_mkundef();
  
  set_entry_t *entry, *tmp;
  HASH_ITER(hh, *set_ptr, entry, tmp) {
    HASH_DEL(*set_ptr, entry);
    free(entry->key);
    free(entry);
  }
  *set_ptr = NULL;
  
  return js_mkundef();
}

static ant_value_t set_size(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t this_val = js->this_val;
  set_entry_t **set_ptr = get_set_from_obj(js, this_val);
  if (!set_ptr) return js_mknum(0);
  
  return js_mknum((double)HASH_COUNT(*set_ptr));
}

static ant_value_t set_values(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  return create_set_iterator(js, js->this_val, ITER_TYPE_SET_VALUES);
}

static ant_value_t set_entries(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  return create_set_iterator(js, js->this_val, ITER_TYPE_SET_ENTRIES);
}

static ant_value_t set_forEach(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js->this_val;
  set_entry_t **set_ptr = get_set_from_obj(js, this_val);
  
  if (nargs < 1 || vtype(args[0]) != T_FUNC)
    return js_mkerr(js, "forEach requires a callback function");
  
  ant_value_t callback = args[0];
  
  if (set_ptr && *set_ptr) {
    set_entry_t *entry, *tmp;
    HASH_ITER(hh, *set_ptr, entry, tmp) {
      ant_value_t call_args[3] = { entry->value, entry->value, this_val };
      ant_value_t result = sv_vm_call(js->vm, js, callback, js_mkundef(), call_args, 3, NULL, false);
      if (is_err(result)) return result;
    }
  }
  
  return js_mkundef();
}

static ant_value_t weakmap_set(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "WeakMap.set() requires 2 arguments");
  
  ant_value_t this_val = js->this_val;
  weakmap_entry_t **wm_ptr = get_weakmap_from_obj(js, this_val);
  if (!wm_ptr) return js_mkerr(js, "Invalid WeakMap object");
  
  if (!is_object_type(args[0]))
    return js_mkerr(js, "WeakMap key must be an object");
  
  ant_value_t key_obj = args[0];
  
  weakmap_entry_t *entry;
  HASH_FIND(hh, *wm_ptr, &key_obj, sizeof(ant_value_t), entry);
  if (entry) {
    entry->value = args[1];
  } else {
    entry = ant_calloc(sizeof(weakmap_entry_t));
    if (!entry) return js_mkerr(js, "out of memory");
    entry->key_obj = key_obj;
    entry->value = args[1];
    HASH_ADD(hh, *wm_ptr, key_obj, sizeof(ant_value_t), entry);
  }
  
  ant_object_t *wm_obj = js_obj_ptr(this_val);
  if (wm_obj) {
    gc_write_barrier(js, wm_obj, key_obj);
    gc_write_barrier(js, wm_obj, args[1]);
  }
  
  return this_val;
}

static ant_value_t weakmap_get(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "WeakMap.get() requires 1 argument");
  
  ant_value_t this_val = js->this_val;
  weakmap_entry_t **wm_ptr = get_weakmap_from_obj(js, this_val);
  if (!wm_ptr) return js_mkundef();
  if (!is_object_type(args[0])) return js_mkundef();
  
  ant_value_t key_obj = args[0];
  weakmap_entry_t *entry;
  HASH_FIND(hh, *wm_ptr, &key_obj, sizeof(ant_value_t), entry);
  return entry ? entry->value : js_mkundef();
}

static ant_value_t weakmap_has(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "WeakMap.has() requires 1 argument");
  
  ant_value_t this_val = js->this_val;
  weakmap_entry_t **wm_ptr = get_weakmap_from_obj(js, this_val);
  if (!wm_ptr) return js_false;
  if (!is_object_type(args[0])) return js_false;
  
  ant_value_t key_obj = args[0];
  weakmap_entry_t *entry;
  HASH_FIND(hh, *wm_ptr, &key_obj, sizeof(ant_value_t), entry);
  return js_bool(entry != NULL);
}

static ant_value_t weakmap_delete(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "WeakMap.delete() requires 1 argument");
  
  ant_value_t this_val = js->this_val;
  weakmap_entry_t **wm_ptr = get_weakmap_from_obj(js, this_val);
  if (!wm_ptr) return js_false;
  if (!is_object_type(args[0])) return js_false;
  
  ant_value_t key_obj = args[0];
  weakmap_entry_t *entry;
  HASH_FIND(hh, *wm_ptr, &key_obj, sizeof(ant_value_t), entry);
  if (entry) {
    HASH_DEL(*wm_ptr, entry);
    free(entry);
    return js_true;
  }
  return js_false;
}

static ant_value_t weakset_add(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "WeakSet.add() requires 1 argument");
  
  ant_value_t this_val = js->this_val;
  weakset_entry_t **ws_ptr = get_weakset_from_obj(js, this_val);
  if (!ws_ptr) return js_mkerr(js, "Invalid WeakSet object");
  
  if (!is_object_type(args[0]))
    return js_mkerr(js, "WeakSet value must be an object");
  
  ant_value_t value_obj = args[0];
  
  weakset_entry_t *entry;
  HASH_FIND(hh, *ws_ptr, &value_obj, sizeof(ant_value_t), entry);
  
  if (!entry) {
    entry = ant_calloc(sizeof(weakset_entry_t));
    if (!entry) return js_mkerr(js, "out of memory");
    entry->value_obj = value_obj;
    HASH_ADD(hh, *ws_ptr, value_obj, sizeof(ant_value_t), entry);
  }
  
  return this_val;
}

static ant_value_t weakset_has(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "WeakSet.has() requires 1 argument");
  
  ant_value_t this_val = js->this_val;
  weakset_entry_t **ws_ptr = get_weakset_from_obj(js, this_val);
  if (!ws_ptr) return js_false;
  if (!is_object_type(args[0])) return js_false;
  
  ant_value_t value_obj = args[0];
  weakset_entry_t *entry;
  HASH_FIND(hh, *ws_ptr, &value_obj, sizeof(ant_value_t), entry);
  return js_bool(entry != NULL);
}

static ant_value_t weakset_delete(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "WeakSet.delete() requires 1 argument");
  
  ant_value_t this_val = js->this_val;
  weakset_entry_t **ws_ptr = get_weakset_from_obj(js, this_val);
  if (!ws_ptr) return js_false;
  if (!is_object_type(args[0])) return js_false;
  
  ant_value_t value_obj = args[0];
  weakset_entry_t *entry;
  HASH_FIND(hh, *ws_ptr, &value_obj, sizeof(ant_value_t), entry);
  
  if (entry) {
    HASH_DEL(*ws_ptr, entry);
    free(entry);
    return js_true;
  }
  return js_false;
}

static ant_value_t builtin_WeakRef(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_object_type(args[0])) {
    return js_mkerr(js, "WeakRef target must be an object");
  }
  
  ant_value_t wr_obj = js_mkobj(js);
  ant_value_t wr_proto = js_get_ctor_proto(js, "WeakRef", 7);
  if (is_special_object(wr_proto)) js_set_proto_init(wr_obj, wr_proto);
  js_set_slot(wr_obj, SLOT_DATA, args[0]);
  
  return wr_obj;
}

static ant_value_t weakref_deref(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t this_val = js->this_val;
  if (vtype(this_val) != T_OBJ) return js_mkundef();
  
  ant_value_t target = js_get_slot(this_val, SLOT_DATA);
  if (vtype(target) != T_OBJ) return js_mkundef();
  
  return target;
}

static ant_value_t builtin_FinalizationRegistry(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || (vtype(args[0]) != T_FUNC && vtype(args[0]) != T_CFUNC)) {
    return js_mkerr(js, "FinalizationRegistry callback must be a function");
  }
  
  ant_value_t fr_obj = js_mkobj(js);
  ant_value_t fr_proto = js_get_ctor_proto(js, "FinalizationRegistry", 20);
  if (is_special_object(fr_proto)) js_set_proto_init(fr_obj, fr_proto);
  
  js_set_slot(fr_obj, SLOT_MAP, js_mkarr(js));
  js_set_slot(fr_obj, SLOT_DATA, args[0]);
  
  return fr_obj;
}

static ant_value_t finreg_register(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js->this_val;
  if (vtype(this_val) != T_OBJ) return js_mkundef();
  
  if (nargs < 1 || vtype(args[0]) != T_OBJ) {
    return js_mkerr(js, "FinalizationRegistry.register target must be an object");
  }
  
  ant_value_t target = args[0];
  ant_value_t held_value = nargs > 1 ? args[1] : js_mkundef();
  ant_value_t unregister_token = nargs > 2 ? args[2] : js_mkundef();
  
  if (vdata(target) == vdata(held_value) && vtype(held_value) == T_OBJ) {
    return js_mkerr(js, "target and held value must not be the same");
  }
  
  ant_value_t registrations = js_get_slot(this_val, SLOT_MAP);
  if (vtype(registrations) != T_ARR) return js_mkundef();
  
  ant_value_t entry = js_mkarr(js);
  ant_offset_t len = js_arr_len(js, registrations);
  
  char idx[16];
  size_t idx_len = uint_to_str(idx, sizeof(idx), 0);
  js_setprop(js, entry, js_mkstr(js, idx, idx_len), target);
  idx_len = uint_to_str(idx, sizeof(idx), 1);
  js_setprop(js, entry, js_mkstr(js, idx, idx_len), held_value);
  idx_len = uint_to_str(idx, sizeof(idx), 2);
  js_setprop(js, entry, js_mkstr(js, idx, idx_len), unregister_token);
  js_setprop(js, entry, js->length_str, tov(3.0));
  
  idx_len = uint_to_str(idx, sizeof(idx), len);
  js_setprop(js, registrations, js_mkstr(js, idx, idx_len), entry);
  js_setprop(js, registrations, js->length_str, tov((double)(len + 1)));
  
  return js_mkundef();
}

static ant_value_t finreg_unregister(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js->this_val;
  if (vtype(this_val) != T_OBJ) return js_false;
  
  if (nargs < 1 || vtype(args[0]) != T_OBJ) {
    return js_mkerr(js, "FinalizationRegistry.unregister token must be an object");
  }
  
  ant_value_t token = args[0];
  ant_value_t registrations = js_get_slot(this_val, SLOT_MAP);
  if (vtype(registrations) != T_ARR) return js_false;
  
  ant_offset_t len = js_arr_len(js, registrations);
  bool removed = false;
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t entry = js_arr_get(js, registrations, i);
    if (vtype(entry) != T_ARR) continue;
    ant_value_t entry_token = js_arr_get(js, entry, 2);
    if (vtype(entry_token) == T_OBJ && vdata(entry_token) == vdata(token)) {
      char idx[16];
      size_t idx_len = uint_to_str(idx, sizeof(idx), i);
      js_setprop(js, registrations, js_mkstr(js, idx, idx_len), js_mkundef());
      removed = true;
    }
  }
  
  return js_bool(removed);
}

static ant_value_t map_groupBy(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr_typed(js, JS_ERR_TYPE, "Map.groupBy requires 2 arguments");
  
  ant_value_t items = args[0];
  ant_value_t callback = args[1];
  
  if (vtype(callback) != T_FUNC && vtype(callback) != T_CFUNC)
    return js_mkerr_typed(js, JS_ERR_TYPE, "callback is not a function");
  
  ant_value_t map_obj = js_mkobj(js);
  js_obj_ptr(map_obj)->type_tag = T_MAP;
  
  ant_value_t map_proto = js_get_ctor_proto(js, "Map", 3);
  if (is_special_object(map_proto)) js_set_proto_init(map_obj, map_proto);
  
  map_entry_t **map_head = ant_calloc(sizeof(map_entry_t *));
  if (!map_head) return js_mkerr(js, "out of memory");
  *map_head = NULL;
  js_set_slot(map_obj, SLOT_DATA, ANT_PTR(map_head));
  
  ant_offset_t len = js_arr_len(js, items);
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t val = js_arr_get(js, items, i);
    ant_value_t cb_args[2] = { val, tov((double)i) };
    
    ant_value_t key = normalize_map_key(
      sv_vm_call(js->vm, js, callback, 
      js_mkundef(), cb_args, 2, NULL, false)
    );
    
    if (is_err(key)) return key;
    map_entry_t *entry = map_find_entry(js, map_head, key);
    ant_value_t group;

    if (entry) group = entry->value; else {
      group = js_mkarr(js);
      if (!map_store_entry(js, map_head, key, key, group)) return js_mkerr(js, "out of memory");
    }
    
    js_arr_push(js, group, val);
  }
  
  return map_obj;
}

static ant_value_t builtin_Map(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Map constructor requires 'new'");
  }
  
  ant_value_t map_obj = js->this_val;
  if (vtype(map_obj) != T_OBJ) map_obj = js_mkobj(js);
  if (is_err(map_obj)) return map_obj;
  js_obj_ptr(map_obj)->type_tag = T_MAP;
  
  ant_value_t map_proto = js_get_ctor_proto(js, "Map", 3);
  ant_value_t instance_proto = js_instance_proto_from_new_target(js, map_proto);
  
  if (is_special_object(instance_proto)) js_set_proto_init(map_obj, instance_proto);
  
  map_entry_t **map_head = ant_calloc(sizeof(map_entry_t *));
  if (!map_head) return js_mkerr(js, "out of memory");
  *map_head = NULL;
  
  if (vtype(js->new_target) == T_FUNC || vtype(js->new_target) == T_CFUNC)
    js_set_slot(map_obj, SLOT_CTOR, js->new_target);
  js_set_slot(map_obj, SLOT_DATA, ANT_PTR(map_head));
  
  if (nargs == 0 || vtype(args[0]) == T_UNDEF || vtype(args[0]) == T_NULL) return map_obj;
  ant_value_t init_result = map_init_from_iterable(js, map_head, args[0]);
  if (is_err(init_result)) return init_result;
  
  return map_obj;
}

static ant_value_t builtin_Set(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Set constructor requires 'new'");
  }
  
  ant_value_t set_obj = js->this_val;
  if (vtype(set_obj) != T_OBJ) set_obj = js_mkobj(js);
  if (is_err(set_obj)) return set_obj;
  js_obj_ptr(set_obj)->type_tag = T_SET;
  
  ant_value_t set_proto = js_get_ctor_proto(js, "Set", 3);
  ant_value_t instance_proto = js_instance_proto_from_new_target(js, set_proto);

  if (is_special_object(instance_proto)) js_set_proto_init(set_obj, instance_proto);
  
  set_entry_t **set_head = ant_calloc(sizeof(set_entry_t *));
  if (!set_head) return js_mkerr(js, "out of memory");
  *set_head = NULL;
  
  if (vtype(js->new_target) == T_FUNC || vtype(js->new_target) == T_CFUNC)
    js_set_slot(set_obj, SLOT_CTOR, js->new_target);
  js_set_slot(set_obj, SLOT_DATA, ANT_PTR(set_head));
  
  if (nargs == 0 || vtype(args[0]) == T_UNDEF || vtype(args[0]) == T_NULL) return set_obj;
  ant_value_t init_result = set_init_from_iterable(js, set_head, args[0]);
  if (is_err(init_result)) return init_result;
  
  return set_obj;
}

static ant_value_t builtin_WeakMap(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "WeakMap constructor requires 'new'");
  }
  
  ant_value_t wm_obj = js->this_val;
  if (vtype(wm_obj) != T_OBJ) wm_obj = js_mkobj(js);
  if (is_err(wm_obj)) return wm_obj;
  js_obj_ptr(wm_obj)->type_tag = T_WEAKMAP;
  
  ant_value_t wm_proto = js_get_ctor_proto(js, "WeakMap", 7);
  ant_value_t instance_proto = js_instance_proto_from_new_target(js, wm_proto);

  if (is_special_object(instance_proto)) js_set_proto_init(wm_obj, instance_proto);
  
  weakmap_entry_t **wm_head = ant_calloc(sizeof(weakmap_entry_t *));
  if (!wm_head) return js_mkerr(js, "out of memory");
  *wm_head = NULL;
  
  if (vtype(js->new_target) == T_FUNC || vtype(js->new_target) == T_CFUNC)
    js_set_slot(wm_obj, SLOT_CTOR, js->new_target);
  js_set_slot(wm_obj, SLOT_DATA, ANT_PTR(wm_head));
  
  if (nargs == 0 || vtype(args[0]) == T_UNDEF || vtype(args[0]) == T_NULL) return wm_obj;
  ant_value_t init_result = weakmap_init_from_iterable(js, wm_head, args[0]);
  if (is_err(init_result)) return init_result;
  
  return wm_obj;
}

static ant_value_t builtin_WeakSet(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "WeakSet constructor requires 'new'");
  }
  
  ant_value_t ws_obj = js->this_val;
  if (vtype(ws_obj) != T_OBJ) ws_obj = js_mkobj(js);
  if (is_err(ws_obj)) return ws_obj;
  js_obj_ptr(ws_obj)->type_tag = T_WEAKSET;
  
  ant_value_t ws_proto = js_get_ctor_proto(js, "WeakSet", 7);
  ant_value_t instance_proto = js_instance_proto_from_new_target(js, ws_proto);

  if (is_special_object(instance_proto)) js_set_proto_init(ws_obj, instance_proto);
  
  weakset_entry_t **ws_head = ant_calloc(sizeof(weakset_entry_t *));
  if (!ws_head) return js_mkerr(js, "out of memory");
  *ws_head = NULL;
  
  if (vtype(js->new_target) == T_FUNC || vtype(js->new_target) == T_CFUNC)
    js_set_slot(ws_obj, SLOT_CTOR, js->new_target);
  js_set_slot(ws_obj, SLOT_DATA, ANT_PTR(ws_head));
  
  if (nargs == 0 || vtype(args[0]) == T_UNDEF || vtype(args[0]) == T_NULL) return ws_obj;
  ant_value_t init_result = weakset_init_from_iterable(js, ws_head, args[0]);
  if (is_err(init_result)) return init_result;
  
  return ws_obj;
}

void init_collections_module(void) {
  ant_t *js = rt->js;
  
  ant_value_t glob = js->global;
  ant_value_t object_proto = js->sym.object_proto;
  
  ant_value_t iter_sym = get_iterator_sym();
  ant_value_t tag_sym = get_toStringTag_sym();
  
  g_map_iter_proto = js_mkobj(js);
  js_set_proto_init(g_map_iter_proto, js->sym.iterator_proto);
  js_set(js, g_map_iter_proto, "next", js_mkfun(map_iter_next));
  js_iter_register_advance(g_map_iter_proto, advance_map);
  
  g_set_iter_proto = js_mkobj(js);
  js_set_proto_init(g_set_iter_proto, js->sym.iterator_proto);
  js_set(js, g_set_iter_proto, "next", js_mkfun(set_iter_next));
  js_iter_register_advance(g_set_iter_proto, advance_set);
  
  ant_value_t map_proto = js_mkobj(js);
  js_set_proto_init(map_proto, object_proto);
  js_set(js, map_proto, "set", js_mkfun(map_set));
  js_set(js, map_proto, "get", js_mkfun(map_get));
  js_set(js, map_proto, "has", js_mkfun(map_has));
  js_set(js, map_proto, "delete", js_mkfun(map_delete));
  js_set(js, map_proto, "clear", js_mkfun(map_clear));
  js_set_getter_desc(js, map_proto, "size", 4, js_mkfun(map_size), JS_DESC_C);
  js_set(js, map_proto, "entries", js_mkfun(map_entries));
  js_set(js, map_proto, "keys", js_mkfun(map_keys));
  js_set(js, map_proto, "values", js_mkfun(map_values));
  js_set(js, map_proto, "forEach", js_mkfun(map_forEach));
  js_set_sym(js, map_proto, iter_sym, js_get(js, map_proto, "entries"));
  js_set_sym(js, map_proto, tag_sym, js_mkstr(js, "Map", 3));
  
  ant_value_t map_ctor = js_mkobj(js);
  js_set_slot(map_ctor, SLOT_CFUNC, js_mkfun(builtin_Map));
  js_mkprop_fast(js, map_ctor, "prototype", 9, map_proto);
  js_mkprop_fast(js, map_ctor, "name", 4, ANT_STRING("Map"));
  js_set_descriptor(js, map_ctor, "name", 4, 0);
  js_set(js, map_ctor, "groupBy", js_mkfun(map_groupBy));
  js_define_species_getter(js, map_ctor);
  js_set(js, glob, "Map", js_obj_to_func(map_ctor));
  
  ant_value_t set_proto = js_mkobj(js);
  js_set_proto_init(set_proto, object_proto);
  js_set(js, set_proto, "add", js_mkfun(set_add));
  js_set(js, set_proto, "has", js_mkfun(set_has));
  js_set(js, set_proto, "delete", js_mkfun(set_delete));
  js_set(js, set_proto, "clear", js_mkfun(set_clear));
  js_set_getter_desc(js, set_proto, "size", 4, js_mkfun(set_size), JS_DESC_C);
  js_set(js, set_proto, "values", js_mkfun(set_values));
  js_set_exact(js, set_proto, "keys", js_get(js, set_proto, "values"));
  js_set(js, set_proto, "entries", js_mkfun(set_entries));
  js_set(js, set_proto, "forEach", js_mkfun(set_forEach));
  js_set_sym(js, set_proto, iter_sym, js_get(js, set_proto, "values"));
  js_set_sym(js, set_proto, tag_sym, js_mkstr(js, "Set", 3));
  
  ant_value_t set_ctor = js_mkobj(js);
  js_set_slot(set_ctor, SLOT_CFUNC, js_mkfun(builtin_Set));
  js_mkprop_fast(js, set_ctor, "prototype", 9, set_proto);
  js_mkprop_fast(js, set_ctor, "name", 4, ANT_STRING("Set"));
  js_set_descriptor(js, set_ctor, "name", 4, 0);
  js_define_species_getter(js, set_ctor);
  js_set(js, glob, "Set", js_obj_to_func(set_ctor));
  
  ant_value_t weakmap_proto = js_mkobj(js);
  js_set_proto_init(weakmap_proto, object_proto);
  js_set(js, weakmap_proto, "set", js_mkfun(weakmap_set));
  js_set(js, weakmap_proto, "get", js_mkfun(weakmap_get));
  js_set(js, weakmap_proto, "has", js_mkfun(weakmap_has));
  js_set(js, weakmap_proto, "delete", js_mkfun(weakmap_delete));
  js_set_sym(js, weakmap_proto, tag_sym, js_mkstr(js, "WeakMap", 7));
  
  ant_value_t weakmap_ctor = js_mkobj(js);
  js_set_slot(weakmap_ctor, SLOT_CFUNC, js_mkfun(builtin_WeakMap));
  js_mkprop_fast(js, weakmap_ctor, "prototype", 9, weakmap_proto);
  js_mkprop_fast(js, weakmap_ctor, "name", 4, ANT_STRING("WeakMap"));
  js_set_descriptor(js, weakmap_ctor, "name", 4, 0);
  js_set(js, glob, "WeakMap", js_obj_to_func(weakmap_ctor));
  
  ant_value_t weakset_proto = js_mkobj(js);
  js_set_proto_init(weakset_proto, object_proto);
  js_set(js, weakset_proto, "add", js_mkfun(weakset_add));
  js_set(js, weakset_proto, "has", js_mkfun(weakset_has));
  js_set(js, weakset_proto, "delete", js_mkfun(weakset_delete));
  js_set_sym(js, weakset_proto, tag_sym, js_mkstr(js, "WeakSet", 7));
  
  ant_value_t weakset_ctor = js_mkobj(js);
  js_set_slot(weakset_ctor, SLOT_CFUNC, js_mkfun(builtin_WeakSet));
  js_mkprop_fast(js, weakset_ctor, "prototype", 9, weakset_proto);
  js_mkprop_fast(js, weakset_ctor, "name", 4, ANT_STRING("WeakSet"));
  js_set_descriptor(js, weakset_ctor, "name", 4, 0);
  js_set(js, glob, "WeakSet", js_obj_to_func(weakset_ctor));
  
  ant_value_t weakref_proto = js_mkobj(js);
  js_set_proto_init(weakref_proto, object_proto);
  js_set(js, weakref_proto, "deref", js_mkfun(weakref_deref));
  js_set_sym(js, weakref_proto, tag_sym, js_mkstr(js, "WeakRef", 7));
  
  ant_value_t weakref_ctor = js_mkobj(js);
  js_set_slot(weakref_ctor, SLOT_CFUNC, js_mkfun(builtin_WeakRef));
  js_mkprop_fast(js, weakref_ctor, "prototype", 9, weakref_proto);
  js_mkprop_fast(js, weakref_ctor, "name", 4, ANT_STRING("WeakRef"));
  js_set_descriptor(js, weakref_ctor, "name", 4, 0);
  js_set(js, glob, "WeakRef", js_obj_to_func(weakref_ctor));
  
  ant_value_t finreg_proto = js_mkobj(js);
  js_set_proto_init(finreg_proto, object_proto);
  js_set(js, finreg_proto, "register", js_mkfun(finreg_register));
  js_set(js, finreg_proto, "unregister", js_mkfun(finreg_unregister));
  js_set_sym(js, finreg_proto, tag_sym, js_mkstr(js, "FinalizationRegistry", 20));
  
  ant_value_t finreg_ctor = js_mkobj(js);
  js_set_slot(finreg_ctor, SLOT_CFUNC, js_mkfun(builtin_FinalizationRegistry));
  js_mkprop_fast(js, finreg_ctor, "prototype", 9, finreg_proto);
  js_mkprop_fast(js, finreg_ctor, "name", 4, ANT_STRING("FinalizationRegistry"));
  js_set_descriptor(js, finreg_ctor, "name", 4, 0);
  js_set(js, glob, "FinalizationRegistry", js_obj_to_func(finreg_ctor));
}
