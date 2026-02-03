#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ant.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "arena.h"
#include "gc.h"

#include "modules/collections.h"
#include "modules/symbol.h"

static map_registry_entry_t *map_registry = NULL;
static size_t map_registry_count = 0;
static size_t map_registry_cap = 0;

static set_registry_entry_t *set_registry = NULL;
static size_t set_registry_count = 0;
static size_t set_registry_cap = 0;

static jsval_t iter_return_this(ant_t *js, jsval_t *args, int nargs) {
  return js->this_val;
}

static void register_map(map_entry_t **head, jsoff_t obj_offset) {
  if (!head) return;
  if (map_registry_count >= map_registry_cap) {
    size_t new_cap = map_registry_cap ? map_registry_cap * 2 : 64;
    map_registry_entry_t *new_reg = realloc(map_registry, new_cap * sizeof(map_registry_entry_t));
    if (!new_reg) return;
    map_registry = new_reg;
    map_registry_cap = new_cap;
  }
  map_registry[map_registry_count].head = head;
  map_registry[map_registry_count].obj_offset = obj_offset;
  map_registry_count++;
}

static void register_set(set_entry_t **head, jsoff_t obj_offset) {
  if (!head) return;
  if (set_registry_count >= set_registry_cap) {
    size_t new_cap = set_registry_cap ? set_registry_cap * 2 : 64;
    set_registry_entry_t *new_reg = realloc(set_registry, new_cap * sizeof(set_registry_entry_t));
    if (!new_reg) return;
    set_registry = new_reg;
    set_registry_cap = new_cap;
  }
  set_registry[set_registry_count].head = head;
  set_registry[set_registry_count].obj_offset = obj_offset;
  set_registry_count++;
}

static const char *jsval_to_key(ant_t *js, jsval_t val) {
  if (vtype(val) == T_STR) {
    jsoff_t len;
    jsoff_t off = vstr(js, val, &len);
    return (char *)&js->mem[off];
  }
  return js_str(js, val);
}

map_entry_t **get_map_from_obj(ant_t *js, jsval_t obj) {
  jsval_t map_val = js_get_slot(js, obj, SLOT_MAP);
  if (vtype(map_val) == T_UNDEF) return NULL;
  return (map_entry_t **)(uintptr_t)js_getnum(map_val);
}

set_entry_t **get_set_from_obj(ant_t *js, jsval_t obj) {
  jsval_t set_val = js_get_slot(js, obj, SLOT_SET);
  if (vtype(set_val) == T_UNDEF) return NULL;
  return (set_entry_t **)(uintptr_t)js_getnum(set_val);
}

static weakmap_entry_t **get_weakmap_from_obj(ant_t *js, jsval_t obj) {
  jsval_t wm_val = js_get_slot(js, obj, SLOT_DATA);
  if (vtype(wm_val) == T_UNDEF) return NULL;
  return (weakmap_entry_t **)(uintptr_t)js_getnum(wm_val);
}

static weakset_entry_t **get_weakset_from_obj(ant_t *js, jsval_t obj) {
  jsval_t ws_val = js_get_slot(js, obj, SLOT_DATA);
  if (vtype(ws_val) == T_UNDEF) return NULL;
  return (weakset_entry_t **)(uintptr_t)js_getnum(ws_val);
}

static map_iterator_state_t *get_map_iter_state(ant_t *js, jsval_t obj) {
  jsval_t state_val = js_get_slot(js, obj, SLOT_ITER_STATE);
  if (vtype(state_val) == T_UNDEF) return NULL;
  return (map_iterator_state_t *)(uintptr_t)js_getnum(state_val);
}

static set_iterator_state_t *get_set_iter_state(ant_t *js, jsval_t obj) {
  jsval_t state_val = js_get_slot(js, obj, SLOT_ITER_STATE);
  if (vtype(state_val) == T_UNDEF) return NULL;
  return (set_iterator_state_t *)(uintptr_t)js_getnum(state_val);
}

static jsval_t map_set(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "Map.set() requires 2 arguments");
  
  jsval_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(js, this_val);
  if (!map_ptr) return js_mkerr(js, "Invalid Map object");
  const char *key_str = jsval_to_key(js, args[0]);
  
  map_entry_t *entry;
  HASH_FIND_STR(*map_ptr, key_str, entry);
  if (entry) {
    entry->value = args[1];
  } else {
    entry = ant_calloc(sizeof(map_entry_t));
    if (!entry) return js_mkerr(js, "out of memory");
    entry->key = strdup(key_str);
    entry->value = args[1];
    HASH_ADD_STR(*map_ptr, key, entry);
  }
  
  return this_val;
}

static jsval_t map_get(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Map.get() requires 1 argument");
  
  jsval_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(js, this_val);
  if (!map_ptr) return js_mkundef();
  
  const char *key_str = jsval_to_key(js, args[0]);
  
  map_entry_t *entry;
  HASH_FIND_STR(*map_ptr, key_str, entry);
  return entry ? entry->value : js_mkundef();
}

static jsval_t map_has(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Map.has() requires 1 argument");
  
  jsval_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(js, this_val);
  
  if (!map_ptr) return js_false;
  const char *key_str = jsval_to_key(js, args[0]);
  
  map_entry_t *entry;
  HASH_FIND_STR(*map_ptr, key_str, entry);
  return js_bool(entry != NULL);
}

static jsval_t map_delete(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Map.delete() requires 1 argument");
  
  jsval_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(js, this_val);
  
  if (!map_ptr) return js_false;
  const char *key_str = jsval_to_key(js, args[0]);
  
  map_entry_t *entry;
  HASH_FIND_STR(*map_ptr, key_str, entry);
  if (entry) {
    HASH_DEL(*map_ptr, entry);
    free(entry->key);
    free(entry);
    return js_true;
  }
  return js_false;
}

static jsval_t map_clear(ant_t *js, jsval_t *args, int nargs) {
  jsval_t this_val = js->this_val;
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

static jsval_t map_size(ant_t *js, jsval_t *args, int nargs) {
  jsval_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(js, this_val);
  if (!map_ptr) return js_mknum(0);
  
  return js_mknum((double)HASH_COUNT(*map_ptr));
}

static jsval_t map_forEach(ant_t *js, jsval_t *args, int nargs) {
  jsval_t this_val = js->this_val;
  map_entry_t **map_ptr = get_map_from_obj(js, this_val);
  
  if (nargs < 1 || vtype(args[0]) != T_FUNC)
    return js_mkerr(js, "forEach requires a callback function");
  
  jsval_t callback = args[0];
  
  if (map_ptr && *map_ptr) {
    map_entry_t *entry, *tmp;
    HASH_ITER(hh, *map_ptr, entry, tmp) {
      jsval_t k = js_mkstr(js, entry->key, strlen(entry->key));
      jsval_t call_args[3] = { entry->value, k, this_val };
      jsval_t result = js_call(js, callback, call_args, 3);
      if (is_err(result)) return result;
    }
  }
  
  return js_mkundef();
}

static jsval_t map_iter_next(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t this_val = js->this_val;
  
  map_iterator_state_t *state = get_map_iter_state(js, this_val);
  if (!state) return js_mkerr(js, "Invalid iterator");
  
  jsval_t result = js_mkobj(js);
  
  if (!state->current) {
    js_set(js, result, "done", js_true);
    js_set(js, result, "value", js_mkundef());
    return result;
  }
  
  map_entry_t *entry = state->current;
  jsval_t value;
  
  switch (state->type) {
    case ITER_TYPE_MAP_VALUES:
      value = entry->value;
      break;
    case ITER_TYPE_MAP_KEYS:
      value = js_mkstr(js, entry->key, strlen(entry->key));
      break;
    case ITER_TYPE_MAP_ENTRIES: {
      jsval_t pair = js_mkarr(js);
      js_arr_push(js, pair, js_mkstr(js, entry->key, strlen(entry->key)));
      js_arr_push(js, pair, entry->value);
      value = pair;
      break;
    }
    default:
      value = js_mkundef();
  }
  
  state->current = entry->hh.next;
  
  js_set(js, result, "value", value);
  js_set(js, result, "done", js_false);
  return result;
}

static jsval_t create_map_iterator(ant_t *js, jsval_t map_obj, iter_type_t type) {
  map_entry_t **map_ptr = get_map_from_obj(js, map_obj);
  
  map_iterator_state_t *state = ant_calloc(sizeof(map_iterator_state_t));
  if (!state) return js_mkerr(js, "out of memory");
  
  state->head = map_ptr;
  state->current = map_ptr ? *map_ptr : NULL;
  state->type = type;
  
  jsval_t iter = js_mkobj(js);
  js_set_slot(js, iter, SLOT_ITER_STATE, ANT_PTR(state));
  js_set(js, iter, "next", js_mkfun(map_iter_next));
  
  const char *iter_key = get_iterator_sym_key();
  if (iter_key && iter_key[0]) {
    js_set(js, iter, iter_key, js_mkfun(iter_return_this));
  }
  
  return iter;
}

static jsval_t map_values(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  return create_map_iterator(js, js->this_val, ITER_TYPE_MAP_VALUES);
}

static jsval_t map_keys(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  return create_map_iterator(js, js->this_val, ITER_TYPE_MAP_KEYS);
}

static jsval_t map_entries(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  return create_map_iterator(js, js->this_val, ITER_TYPE_MAP_ENTRIES);
}

static jsval_t map_iterator(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  return create_map_iterator(js, js->this_val, ITER_TYPE_MAP_ENTRIES);
}

static jsval_t set_iter_next(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t this_val = js->this_val;
  
  set_iterator_state_t *state = get_set_iter_state(js, this_val);
  if (!state) return js_mkerr(js, "Invalid iterator");
  
  jsval_t result = js_mkobj(js);
  
  if (!state->current) {
    js_set(js, result, "done", js_true);
    js_set(js, result, "value", js_mkundef());
    return result;
  }
  
  set_entry_t *entry = state->current;
  jsval_t value;
  
  if (state->type == ITER_TYPE_SET_ENTRIES) {
    jsval_t pair = js_mkarr(js);
    js_arr_push(js, pair, entry->value);
    js_arr_push(js, pair, entry->value);
    value = pair;
  } else {
    value = entry->value;
  }
  
  state->current = entry->hh.next;
  
  js_set(js, result, "value", value);
  js_set(js, result, "done", js_false);
  return result;
}

static jsval_t create_set_iterator(ant_t *js, jsval_t set_obj, iter_type_t type) {
  set_entry_t **set_ptr = get_set_from_obj(js, set_obj);
  
  set_iterator_state_t *state = ant_calloc(sizeof(set_iterator_state_t));
  if (!state) return js_mkerr(js, "out of memory");
  
  state->head = set_ptr;
  state->current = set_ptr ? *set_ptr : NULL;
  state->type = type;
  
  jsval_t iter = js_mkobj(js);
  js_set_slot(js, iter, SLOT_ITER_STATE, ANT_PTR(state));
  js_set(js, iter, "next", js_mkfun(set_iter_next));
  
  const char *iter_key = get_iterator_sym_key();
  if (iter_key && iter_key[0]) {
    js_set(js, iter, iter_key, js_mkfun(iter_return_this));
  }
  
  return iter;
}

static jsval_t set_add(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Set.add() requires 1 argument");
  
  jsval_t this_val = js->this_val;
  set_entry_t **set_ptr = get_set_from_obj(js, this_val);
  if (!set_ptr) return js_mkerr(js, "Invalid Set object");
  
  const char *key_str = jsval_to_key(js, args[0]);
  
  set_entry_t *entry;
  HASH_FIND_STR(*set_ptr, key_str, entry);
  
  if (!entry) {
    entry = ant_calloc(sizeof(set_entry_t));
    if (!entry) return js_mkerr(js, "out of memory");
    entry->value = args[0];
    entry->key = strdup(key_str);
    HASH_ADD_KEYPTR(hh, *set_ptr, entry->key, strlen(entry->key), entry);
  }
  
  return this_val;
}

static jsval_t set_has(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Set.has() requires 1 argument");
  
  jsval_t this_val = js->this_val;
  set_entry_t **set_ptr = get_set_from_obj(js, this_val);
  if (!set_ptr) return js_false;
  
  const char *key_str = jsval_to_key(js, args[0]);
  
  set_entry_t *entry;
  HASH_FIND_STR(*set_ptr, key_str, entry);
  return js_bool(entry != NULL);
}

static jsval_t set_delete(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Set.delete() requires 1 argument");
  
  jsval_t this_val = js->this_val;
  set_entry_t **set_ptr = get_set_from_obj(js, this_val);
  if (!set_ptr) return js_false;
  
  const char *key_str = jsval_to_key(js, args[0]);
  set_entry_t *entry;
  HASH_FIND_STR(*set_ptr, key_str, entry);
  
  if (entry) {
    HASH_DEL(*set_ptr, entry);
    free(entry->key);
    free(entry);
    return js_true;
  }
  return js_false;
}

static jsval_t set_clear(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t this_val = js->this_val;
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

static jsval_t set_size(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t this_val = js->this_val;
  set_entry_t **set_ptr = get_set_from_obj(js, this_val);
  if (!set_ptr) return js_mknum(0);
  
  return js_mknum((double)HASH_COUNT(*set_ptr));
}

static jsval_t set_values(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  return create_set_iterator(js, js->this_val, ITER_TYPE_SET_VALUES);
}

static jsval_t set_entries(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  return create_set_iterator(js, js->this_val, ITER_TYPE_SET_ENTRIES);
}

static jsval_t set_iterator(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  return create_set_iterator(js, js->this_val, ITER_TYPE_SET_VALUES);
}

static jsval_t set_forEach(ant_t *js, jsval_t *args, int nargs) {
  jsval_t this_val = js->this_val;
  set_entry_t **set_ptr = get_set_from_obj(js, this_val);
  
  if (nargs < 1 || vtype(args[0]) != T_FUNC)
    return js_mkerr(js, "forEach requires a callback function");
  
  jsval_t callback = args[0];
  
  if (set_ptr && *set_ptr) {
    set_entry_t *entry, *tmp;
    HASH_ITER(hh, *set_ptr, entry, tmp) {
      jsval_t call_args[3] = { entry->value, entry->value, this_val };
      jsval_t result = js_call(js, callback, call_args, 3);
      if (is_err(result)) return result;
    }
  }
  
  return js_mkundef();
}

static jsval_t weakmap_set(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "WeakMap.set() requires 2 arguments");
  
  jsval_t this_val = js->this_val;
  weakmap_entry_t **wm_ptr = get_weakmap_from_obj(js, this_val);
  if (!wm_ptr) return js_mkerr(js, "Invalid WeakMap object");
  
  if (vtype(args[0]) != T_OBJ)
    return js_mkerr(js, "WeakMap key must be an object");
  
  jsval_t key_obj = args[0];
  
  weakmap_entry_t *entry;
  HASH_FIND(hh, *wm_ptr, &key_obj, sizeof(jsval_t), entry);
  if (entry) {
    entry->value = args[1];
  } else {
    entry = ant_calloc(sizeof(weakmap_entry_t));
    if (!entry) return js_mkerr(js, "out of memory");
    entry->key_obj = key_obj;
    entry->value = args[1];
    HASH_ADD(hh, *wm_ptr, key_obj, sizeof(jsval_t), entry);
  }
  
  return this_val;
}

static jsval_t weakmap_get(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "WeakMap.get() requires 1 argument");
  
  jsval_t this_val = js->this_val;
  weakmap_entry_t **wm_ptr = get_weakmap_from_obj(js, this_val);
  if (!wm_ptr) return js_mkundef();
  
  if (vtype(args[0]) != T_OBJ) return js_mkundef();
  
  jsval_t key_obj = args[0];
  weakmap_entry_t *entry;
  HASH_FIND(hh, *wm_ptr, &key_obj, sizeof(jsval_t), entry);
  return entry ? entry->value : js_mkundef();
}

static jsval_t weakmap_has(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "WeakMap.has() requires 1 argument");
  
  jsval_t this_val = js->this_val;
  weakmap_entry_t **wm_ptr = get_weakmap_from_obj(js, this_val);
  if (!wm_ptr) return js_false;
  
  if (vtype(args[0]) != T_OBJ) return js_false;
  
  jsval_t key_obj = args[0];
  weakmap_entry_t *entry;
  HASH_FIND(hh, *wm_ptr, &key_obj, sizeof(jsval_t), entry);
  return js_bool(entry != NULL);
}

static jsval_t weakmap_delete(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "WeakMap.delete() requires 1 argument");
  
  jsval_t this_val = js->this_val;
  weakmap_entry_t **wm_ptr = get_weakmap_from_obj(js, this_val);
  if (!wm_ptr) return js_false;
  
  if (vtype(args[0]) != T_OBJ) return js_false;
  
  jsval_t key_obj = args[0];
  weakmap_entry_t *entry;
  HASH_FIND(hh, *wm_ptr, &key_obj, sizeof(jsval_t), entry);
  if (entry) {
    HASH_DEL(*wm_ptr, entry);
    free(entry);
    return js_true;
  }
  return js_false;
}

static jsval_t weakset_add(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "WeakSet.add() requires 1 argument");
  
  jsval_t this_val = js->this_val;
  weakset_entry_t **ws_ptr = get_weakset_from_obj(js, this_val);
  if (!ws_ptr) return js_mkerr(js, "Invalid WeakSet object");
  
  if (vtype(args[0]) != T_OBJ)
    return js_mkerr(js, "WeakSet value must be an object");
  
  jsval_t value_obj = args[0];
  
  weakset_entry_t *entry;
  HASH_FIND(hh, *ws_ptr, &value_obj, sizeof(jsval_t), entry);
  
  if (!entry) {
    entry = ant_calloc(sizeof(weakset_entry_t));
    if (!entry) return js_mkerr(js, "out of memory");
    entry->value_obj = value_obj;
    HASH_ADD(hh, *ws_ptr, value_obj, sizeof(jsval_t), entry);
  }
  
  return this_val;
}

static jsval_t weakset_has(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "WeakSet.has() requires 1 argument");
  
  jsval_t this_val = js->this_val;
  weakset_entry_t **ws_ptr = get_weakset_from_obj(js, this_val);
  if (!ws_ptr) return js_false;
  
  if (vtype(args[0]) != T_OBJ) return js_false;
  
  jsval_t value_obj = args[0];
  weakset_entry_t *entry;
  HASH_FIND(hh, *ws_ptr, &value_obj, sizeof(jsval_t), entry);
  return js_bool(entry != NULL);
}

static jsval_t weakset_delete(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "WeakSet.delete() requires 1 argument");
  
  jsval_t this_val = js->this_val;
  weakset_entry_t **ws_ptr = get_weakset_from_obj(js, this_val);
  if (!ws_ptr) return js_false;
  
  if (vtype(args[0]) != T_OBJ) return js_false;
  
  jsval_t value_obj = args[0];
  weakset_entry_t *entry;
  HASH_FIND(hh, *ws_ptr, &value_obj, sizeof(jsval_t), entry);
  
  if (entry) {
    HASH_DEL(*ws_ptr, entry);
    free(entry);
    return js_true;
  }
  return js_false;
}

static jsval_t builtin_WeakRef(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_OBJ) {
    return js_mkerr(js, "WeakRef target must be an object");
  }
  
  jsval_t wr_obj = js_mkobj(js);
  jsval_t wr_proto = js_get_ctor_proto(js, "WeakRef", 7);
  if (is_special_object(wr_proto)) js_set_proto(js, wr_obj, wr_proto);
  js_set_slot(js, wr_obj, SLOT_DATA, args[0]);
  
  return wr_obj;
}

static jsval_t weakref_deref(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t this_val = js->this_val;
  if (vtype(this_val) != T_OBJ) return js_mkundef();
  
  jsval_t target = js_get_slot(js, this_val, SLOT_DATA);
  if (vtype(target) != T_OBJ) return js_mkundef();
  
  return target;
}

static jsval_t builtin_FinalizationRegistry(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1 || (vtype(args[0]) != T_FUNC && vtype(args[0]) != T_CFUNC)) {
    return js_mkerr(js, "FinalizationRegistry callback must be a function");
  }
  
  jsval_t fr_obj = js_mkobj(js);
  jsval_t fr_proto = js_get_ctor_proto(js, "FinalizationRegistry", 20);
  if (is_special_object(fr_proto)) js_set_proto(js, fr_obj, fr_proto);
  
  js_set_slot(js, fr_obj, SLOT_DATA, args[0]);
  js_set_slot(js, fr_obj, SLOT_MAP, mkarr(js));
  
  return fr_obj;
}

static jsval_t finreg_register(ant_t *js, jsval_t *args, int nargs) {
  jsval_t this_val = js->this_val;
  if (vtype(this_val) != T_OBJ) return js_mkundef();
  
  if (nargs < 1 || vtype(args[0]) != T_OBJ) {
    return js_mkerr(js, "FinalizationRegistry.register target must be an object");
  }
  
  jsval_t target = args[0];
  jsval_t held_value = nargs > 1 ? args[1] : js_mkundef();
  jsval_t unregister_token = nargs > 2 ? args[2] : js_mkundef();
  
  if (vdata(target) == vdata(held_value) && vtype(held_value) == T_OBJ) {
    return js_mkerr(js, "target and held value must not be the same");
  }
  
  jsval_t registrations = js_get_slot(js, this_val, SLOT_MAP);
  if (vtype(registrations) != T_ARR) return js_mkundef();
  
  jsval_t entry = mkarr(js);
  jsoff_t len = js_arr_len(js, registrations);
  
  char idx[16];
  size_t idx_len = uint_to_str(idx, sizeof(idx), 0);
  setprop(js, entry, js_mkstr(js, idx, idx_len), target);
  idx_len = uint_to_str(idx, sizeof(idx), 1);
  setprop(js, entry, js_mkstr(js, idx, idx_len), held_value);
  idx_len = uint_to_str(idx, sizeof(idx), 2);
  setprop(js, entry, js_mkstr(js, idx, idx_len), unregister_token);
  setprop(js, entry, js_mkstr(js, "length", 6), tov(3.0));
  
  idx_len = uint_to_str(idx, sizeof(idx), len);
  setprop(js, registrations, js_mkstr(js, idx, idx_len), entry);
  setprop(js, registrations, js_mkstr(js, "length", 6), tov((double)(len + 1)));
  
  return js_mkundef();
}

static jsval_t finreg_unregister(ant_t *js, jsval_t *args, int nargs) {
  jsval_t this_val = js->this_val;
  if (vtype(this_val) != T_OBJ) return js_false;
  
  if (nargs < 1 || vtype(args[0]) != T_OBJ) {
    return js_mkerr(js, "FinalizationRegistry.unregister token must be an object");
  }
  
  jsval_t token = args[0];
  jsval_t registrations = js_get_slot(js, this_val, SLOT_MAP);
  if (vtype(registrations) != T_ARR) return js_false;
  
  jsoff_t len = js_arr_len(js, registrations);
  bool removed = false;
  
  for (jsoff_t i = 0; i < len; i++) {
    jsval_t entry = js_arr_get(js, registrations, i);
    if (vtype(entry) != T_ARR) continue;
    jsval_t entry_token = js_arr_get(js, entry, 2);
    if (vtype(entry_token) == T_OBJ && vdata(entry_token) == vdata(token)) {
      char idx[16];
      size_t idx_len = uint_to_str(idx, sizeof(idx), i);
      setprop(js, registrations, js_mkstr(js, idx, idx_len), js_mkundef());
      removed = true;
    }
  }
  
  return js_bool(removed);
}

static jsval_t builtin_Map(ant_t *js, jsval_t *args, int nargs) {
  jsval_t map_obj = js_mkobj(js);
  jsoff_t obj_offset = (jsoff_t)vdata(map_obj);
  
  jsval_t map_proto = js_get_ctor_proto(js, "Map", 3);
  if (is_special_object(map_proto)) js_set_proto(js, map_obj, map_proto);
  
  map_entry_t **map_head = ant_calloc(sizeof(map_entry_t *));
  if (!map_head) return js_mkerr(js, "out of memory");
  *map_head = NULL;
  
  register_map(map_head, obj_offset);
  js_set_slot(js, map_obj, SLOT_MAP, ANT_PTR(map_head));
  
  if (nargs == 0 || vtype(args[0]) != T_ARR) return map_obj;
  
  jsval_t iterable = args[0];
  jsoff_t length = js_arr_len(js, iterable);
  
  for (jsoff_t i = 0; i < length; i++) {
    jsval_t entry = js_arr_get(js, iterable, i);
    if (vtype(entry) != T_ARR) continue;
    
    jsoff_t entry_len = js_arr_len(js, entry);
    if (entry_len < 2) continue;
    
    jsval_t key = js_arr_get(js, entry, 0);
    jsval_t value = js_arr_get(js, entry, 1);
    const char *key_str = jsval_to_key(js, key);
    
    map_entry_t *map_entry;
    HASH_FIND_STR(*map_head, key_str, map_entry);
    if (map_entry) {
      map_entry->value = value;
      continue;
    }
    
    map_entry = ant_calloc(sizeof(map_entry_t));
    if (!map_entry) return js_mkerr(js, "out of memory");
    map_entry->key = strdup(key_str);
    map_entry->value = value;
    HASH_ADD_STR(*map_head, key, map_entry);
  }
  
  return map_obj;
}

static jsval_t builtin_Set(ant_t *js, jsval_t *args, int nargs) {
  jsval_t set_obj = js_mkobj(js);
  jsoff_t obj_offset = (jsoff_t)vdata(set_obj);
  
  jsval_t set_proto = js_get_ctor_proto(js, "Set", 3);
  if (is_special_object(set_proto)) js_set_proto(js, set_obj, set_proto);
  
  set_entry_t **set_head = ant_calloc(sizeof(set_entry_t *));
  if (!set_head) return js_mkerr(js, "out of memory");
  *set_head = NULL;
  
  register_set(set_head, obj_offset);
  js_set_slot(js, set_obj, SLOT_SET, ANT_PTR(set_head));
  
  if (nargs == 0 || vtype(args[0]) != T_ARR) return set_obj;
  
  jsval_t iterable = args[0];
  jsoff_t length = js_arr_len(js, iterable);
  
  for (jsoff_t i = 0; i < length; i++) {
    jsval_t value = js_arr_get(js, iterable, i);
    const char *key_str = jsval_to_key(js, value);
    
    set_entry_t *entry;
    HASH_FIND_STR(*set_head, key_str, entry);
    if (entry) continue;
    
    entry = ant_calloc(sizeof(set_entry_t));
    if (!entry) return js_mkerr(js, "out of memory");
    entry->value = value;
    entry->key = strdup(key_str);
    HASH_ADD_KEYPTR(hh, *set_head, entry->key, strlen(entry->key), entry);
  }
  
  return set_obj;
}

static jsval_t builtin_WeakMap(ant_t *js, jsval_t *args, int nargs) {
  jsval_t wm_obj = js_mkobj(js);
  
  jsval_t wm_proto = js_get_ctor_proto(js, "WeakMap", 7);
  if (is_special_object(wm_proto)) js_set_proto(js, wm_obj, wm_proto);
  
  weakmap_entry_t **wm_head = ant_calloc(sizeof(weakmap_entry_t *));
  if (!wm_head) return js_mkerr(js, "out of memory");
  *wm_head = NULL;
  
  js_set_slot(js, wm_obj, SLOT_DATA, ANT_PTR(wm_head));
  
  if (nargs == 0 || vtype(args[0]) != T_ARR) return wm_obj;
  
  jsval_t iterable = args[0];
  jsoff_t length = js_arr_len(js, iterable);
  
  for (jsoff_t i = 0; i < length; i++) {
    jsval_t entry = js_arr_get(js, iterable, i);
    if (vtype(entry) != T_ARR) continue;
    
    jsoff_t entry_len = js_arr_len(js, entry);
    if (entry_len < 2) continue;
    
    jsval_t key = js_arr_get(js, entry, 0);
    jsval_t value = js_arr_get(js, entry, 1);
    
    if (vtype(key) != T_OBJ) return js_mkerr(js, "WeakMap key must be an object");
    
    weakmap_entry_t *wm_entry;
    HASH_FIND(hh, *wm_head, &key, sizeof(jsval_t), wm_entry);
    if (wm_entry) {
      wm_entry->value = value;
      continue;
    }
    
    wm_entry = ant_calloc(sizeof(weakmap_entry_t));
    if (!wm_entry) return js_mkerr(js, "out of memory");
    wm_entry->key_obj = key;
    wm_entry->value = value;
    HASH_ADD(hh, *wm_head, key_obj, sizeof(jsval_t), wm_entry);
  }
  
  return wm_obj;
}

static jsval_t builtin_WeakSet(ant_t *js, jsval_t *args, int nargs) {
  jsval_t ws_obj = js_mkobj(js);
  
  jsval_t ws_proto = js_get_ctor_proto(js, "WeakSet", 7);
  if (is_special_object(ws_proto)) js_set_proto(js, ws_obj, ws_proto);
  
  weakset_entry_t **ws_head = ant_calloc(sizeof(weakset_entry_t *));
  if (!ws_head) return js_mkerr(js, "out of memory");
  *ws_head = NULL;
  
  js_set_slot(js, ws_obj, SLOT_DATA, ANT_PTR(ws_head));
  
  if (nargs == 0 || vtype(args[0]) != T_ARR) return ws_obj;
  
  jsval_t iterable = args[0];
  jsoff_t length = js_arr_len(js, iterable);
  
  for (jsoff_t i = 0; i < length; i++) {
    jsval_t value = js_arr_get(js, iterable, i);
    
    if (vtype(value) != T_OBJ) return js_mkerr(js, "WeakSet value must be an object");
    
    weakset_entry_t *entry;
    HASH_FIND(hh, *ws_head, &value, sizeof(jsval_t), entry);
    if (entry) continue;
    
    entry = ant_calloc(sizeof(weakset_entry_t));
    if (!entry) return js_mkerr(js, "out of memory");
    entry->value_obj = value;
    HASH_ADD(hh, *ws_head, value_obj, sizeof(jsval_t), entry);
  }
  
  return ws_obj;
}

void init_collections_module(void) {
  ant_t *js = rt->js;
  
  jsval_t glob = js->global;
  jsval_t object_proto = js->object;
  
  const char *iter_key = get_iterator_sym_key();
  const char *toStringTag_key = get_toStringTag_sym_key();
  
  jsval_t map_proto = js_mkobj(js);
  js_set_proto(js, map_proto, object_proto);
  js_set(js, map_proto, "set", js_mkfun(map_set));
  js_set(js, map_proto, "get", js_mkfun(map_get));
  js_set(js, map_proto, "has", js_mkfun(map_has));
  js_set(js, map_proto, "delete", js_mkfun(map_delete));
  js_set(js, map_proto, "clear", js_mkfun(map_clear));
  js_set(js, map_proto, "size", js_mkfun(map_size));
  js_set(js, map_proto, "entries", js_mkfun(map_entries));
  js_set(js, map_proto, "keys", js_mkfun(map_keys));
  js_set(js, map_proto, "values", js_mkfun(map_values));
  js_set(js, map_proto, "forEach", js_mkfun(map_forEach));
  js_set(js, map_proto, iter_key, js_mkfun(map_iterator));
  js_set(js, map_proto, toStringTag_key, js_mkstr(js, "Map", 3));
  
  jsval_t map_ctor = js_mkobj(js);
  js_set_slot(js, map_ctor, SLOT_CFUNC, js_mkfun(builtin_Map));
  js_mkprop_fast(js, map_ctor, "prototype", 9, map_proto);
  js_mkprop_fast(js, map_ctor, "name", 4, ANT_STRING("Map"));
  js_set_descriptor(js, map_ctor, "name", 4, 0);
  js_set(js, glob, "Map", js_obj_to_func(map_ctor));
  
  jsval_t set_proto = js_mkobj(js);
  js_set_proto(js, set_proto, object_proto);
  js_set(js, set_proto, "add", js_mkfun(set_add));
  js_set(js, set_proto, "has", js_mkfun(set_has));
  js_set(js, set_proto, "delete", js_mkfun(set_delete));
  js_set(js, set_proto, "clear", js_mkfun(set_clear));
  js_set(js, set_proto, "size", js_mkfun(set_size));
  js_set(js, set_proto, "values", js_mkfun(set_values));
  js_set(js, set_proto, "keys", js_mkfun(set_values));
  js_set(js, set_proto, "entries", js_mkfun(set_entries));
  js_set(js, set_proto, "forEach", js_mkfun(set_forEach));
  js_set(js, set_proto, iter_key, js_mkfun(set_iterator));
  js_set(js, set_proto, toStringTag_key, js_mkstr(js, "Set", 3));
  
  jsval_t set_ctor = js_mkobj(js);
  js_set_slot(js, set_ctor, SLOT_CFUNC, js_mkfun(builtin_Set));
  js_mkprop_fast(js, set_ctor, "prototype", 9, set_proto);
  js_mkprop_fast(js, set_ctor, "name", 4, ANT_STRING("Set"));
  js_set_descriptor(js, set_ctor, "name", 4, 0);
  js_set(js, glob, "Set", js_obj_to_func(set_ctor));
  
  jsval_t weakmap_proto = js_mkobj(js);
  js_set_proto(js, weakmap_proto, object_proto);
  js_set(js, weakmap_proto, "set", js_mkfun(weakmap_set));
  js_set(js, weakmap_proto, "get", js_mkfun(weakmap_get));
  js_set(js, weakmap_proto, "has", js_mkfun(weakmap_has));
  js_set(js, weakmap_proto, "delete", js_mkfun(weakmap_delete));
  js_set(js, weakmap_proto, toStringTag_key, js_mkstr(js, "WeakMap", 7));
  
  jsval_t weakmap_ctor = js_mkobj(js);
  js_set_slot(js, weakmap_ctor, SLOT_CFUNC, js_mkfun(builtin_WeakMap));
  js_mkprop_fast(js, weakmap_ctor, "prototype", 9, weakmap_proto);
  js_mkprop_fast(js, weakmap_ctor, "name", 4, ANT_STRING("WeakMap"));
  js_set_descriptor(js, weakmap_ctor, "name", 4, 0);
  js_set(js, glob, "WeakMap", js_obj_to_func(weakmap_ctor));
  
  jsval_t weakset_proto = js_mkobj(js);
  js_set_proto(js, weakset_proto, object_proto);
  js_set(js, weakset_proto, "add", js_mkfun(weakset_add));
  js_set(js, weakset_proto, "has", js_mkfun(weakset_has));
  js_set(js, weakset_proto, "delete", js_mkfun(weakset_delete));
  js_set(js, weakset_proto, toStringTag_key, js_mkstr(js, "WeakSet", 7));
  
  jsval_t weakset_ctor = js_mkobj(js);
  js_set_slot(js, weakset_ctor, SLOT_CFUNC, js_mkfun(builtin_WeakSet));
  js_mkprop_fast(js, weakset_ctor, "prototype", 9, weakset_proto);
  js_mkprop_fast(js, weakset_ctor, "name", 4, ANT_STRING("WeakSet"));
  js_set_descriptor(js, weakset_ctor, "name", 4, 0);
  js_set(js, glob, "WeakSet", js_obj_to_func(weakset_ctor));
  
  jsval_t weakref_proto = js_mkobj(js);
  js_set_proto(js, weakref_proto, object_proto);
  js_set(js, weakref_proto, "deref", js_mkfun(weakref_deref));
  js_set(js, weakref_proto, toStringTag_key, js_mkstr(js, "WeakRef", 7));
  
  jsval_t weakref_ctor = js_mkobj(js);
  js_set_slot(js, weakref_ctor, SLOT_CFUNC, js_mkfun(builtin_WeakRef));
  js_mkprop_fast(js, weakref_ctor, "prototype", 9, weakref_proto);
  js_mkprop_fast(js, weakref_ctor, "name", 4, ANT_STRING("WeakRef"));
  js_set_descriptor(js, weakref_ctor, "name", 4, 0);
  js_set(js, glob, "WeakRef", js_obj_to_func(weakref_ctor));
  
  jsval_t finreg_proto = js_mkobj(js);
  js_set_proto(js, finreg_proto, object_proto);
  js_set(js, finreg_proto, "register", js_mkfun(finreg_register));
  js_set(js, finreg_proto, "unregister", js_mkfun(finreg_unregister));
  js_set(js, finreg_proto, toStringTag_key, js_mkstr(js, "FinalizationRegistry", 20));
  
  jsval_t finreg_ctor = js_mkobj(js);
  js_set_slot(js, finreg_ctor, SLOT_CFUNC, js_mkfun(builtin_FinalizationRegistry));
  js_mkprop_fast(js, finreg_ctor, "prototype", 9, finreg_proto);
  js_mkprop_fast(js, finreg_ctor, "name", 4, ANT_STRING("FinalizationRegistry"));
  js_set_descriptor(js, finreg_ctor, "name", 4, 0);
  js_set(js, glob, "FinalizationRegistry", js_obj_to_func(finreg_ctor));
}

void collections_gc_reserve_roots(void (*op_val)(void *, jsval_t *), void *ctx) {
  for (size_t i = 0; i < map_registry_count; i++) {
    map_entry_t **head = map_registry[i].head;
    if (head && *head) {
      map_entry_t *entry, *tmp;
      HASH_ITER(hh, *head, entry, tmp) op_val(ctx, &entry->value);
    }
  }
  
  for (size_t i = 0; i < set_registry_count; i++) {
    set_entry_t **head = set_registry[i].head;
    if (head && *head) {
      set_entry_t *entry, *tmp;
      HASH_ITER(hh, *head, entry, tmp) op_val(ctx, &entry->value);
    }
  }
}

static void free_map_entries(map_entry_t **head) {
  if (!head || !*head) return;
  map_entry_t *entry, *tmp;
  HASH_ITER(hh, *head, entry, tmp) {
    HASH_DEL(*head, entry);
    free(entry->key);
    free(entry);
  }
}

static void free_set_entries(set_entry_t **head) {
  if (!head || !*head) return;
  set_entry_t *entry, *tmp;
  HASH_ITER(hh, *head, entry, tmp) {
    HASH_DEL(*head, entry);
    free(entry->key);
    free(entry);
  }
}

void collections_gc_update_roots(jsoff_t (*fwd_off)(void *ctx, jsoff_t old), GC_OP_VAL_ARGS) {
  size_t write_idx = 0;
  
  for (size_t i = 0; i < map_registry_count; i++) {
    jsoff_t old_off = map_registry[i].obj_offset;
    jsoff_t new_off = fwd_off(ctx, old_off);
    
    if (new_off == old_off && old_off != 0) {
      free_map_entries(map_registry[i].head);
      free(map_registry[i].head);
      continue;
    }
    
    map_registry[i].obj_offset = new_off;
    
    map_entry_t **head = map_registry[i].head;
    if (head && *head) {
      map_entry_t *entry, *tmp;
      HASH_ITER(hh, *head, entry, tmp) op_val(ctx, &entry->value);
    }
    
    if (write_idx != i) map_registry[write_idx] = map_registry[i];
    write_idx++;
  }
  map_registry_count = write_idx;
  
  write_idx = 0;
  for (size_t i = 0; i < set_registry_count; i++) {
    jsoff_t old_off = set_registry[i].obj_offset;
    jsoff_t new_off = fwd_off(ctx, old_off);
    
    if (new_off == old_off && old_off != 0) {
      free_set_entries(set_registry[i].head);
      free(set_registry[i].head);
      continue;
    }
    
    set_registry[i].obj_offset = new_off;
    
    set_entry_t **head = set_registry[i].head;
    if (head && *head) {
      set_entry_t *entry, *tmp;
      HASH_ITER(hh, *head, entry, tmp) op_val(ctx, &entry->value);
    }
    
    if (write_idx != i) set_registry[write_idx] = set_registry[i];
    write_idx++;
  }
  set_registry_count = write_idx;
}

void cleanup_collections_module(void) {
  for (size_t i = 0; i < map_registry_count; i++) {
    free_map_entries(map_registry[i].head);
    free(map_registry[i].head);
  }
  free(map_registry);
  map_registry = NULL;
  map_registry_count = 0;
  map_registry_cap = 0;
  
  for (size_t i = 0; i < set_registry_count; i++) {
    free_set_entries(set_registry[i].head);
    free(set_registry[i].head);
  }
  free(set_registry);
  set_registry = NULL;
  set_registry_count = 0;
  set_registry_cap = 0;
}

#undef CLEANUP_REGISTRY
