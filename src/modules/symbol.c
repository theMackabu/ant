#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ant.h"
#include "utf8.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "silver/engine.h"
#include "modules/symbol.h"
#include "descriptors.h"
#include "gc/roots.h"
#include "gc/modules.h"

#define DECL_SYM(name, _desc) static ant_value_t g_##name = {0};
WELLKNOWN_SYMBOLS(DECL_SYM)
#undef DECL_SYM

#define DEF_GET_SYM(name, _desc) ant_value_t get_##name##_sym(void) { return g_##name; }
WELLKNOWN_SYMBOLS(DEF_GET_SYM)
#undef DEF_GET_SYM

static ant_value_t builtin_Symbol(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) != T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Symbol is not a constructor");

  const char *desc = NULL;
  if (nargs > 0 && vtype(args[0]) == T_STR) {
    desc = js_getstr(js, args[0], NULL);
  }
  return js_mksym(js, desc);
}

static ant_value_t builtin_Symbol_for(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_STR) {
    return js_mkerr(js, "Symbol.for requires a string argument");
  }
  
  char *key = js_getstr(js, args[0], NULL);
  if (!key) return js_mkerr(js, "Invalid key");
  
  return js_mksym_for(js, key);
}

static ant_value_t builtin_Symbol_keyFor(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_SYMBOL) {
    return js_mkundef();
  }
  
  const char *key = js_sym_key(args[0]);
  if (!key) return js_mkundef();
  
  return js_mkstr(js, key, strlen(key));
}

static ant_value_t builtin_Symbol_toString(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  
  if (vtype(this_val) != T_SYMBOL) {
    return js_mkerr(js, "Symbol.prototype.toString requires a symbol");
  }
  
  return js_symbol_to_string(js, this_val);
}

static ant_value_t get_iterator_prototype(ant_t *js) {
  if (vtype(js->sym.iterator_proto) == T_OBJ) return js->sym.iterator_proto;

  js->sym.iterator_proto = js_mkobj(js);
  js_set_proto_init(js->sym.iterator_proto, js->object);
  js_set_sym(js, js->sym.iterator_proto, g_iterator, js_mkfun(sym_this_cb));
  
  return js->sym.iterator_proto;
}

static bool advance_array(ant_t *js, js_iter_t *it, ant_value_t *out) {
  ant_value_t iter = it->iterator;
  ant_value_t array = js_get_slot(iter, SLOT_DATA);
  ant_value_t state_v = js_get_slot(iter, SLOT_ITER_STATE);
  
  uint32_t state = (vtype(state_v) == T_NUM) ? (uint32_t)js_getnum(state_v) : 0;
  uint32_t kind = ITER_STATE_KIND(state);
  uint32_t idx  = ITER_STATE_INDEX(state);
  ant_offset_t len = js_arr_len(js, array);
  if (idx >= (uint32_t)len) return false;

  switch (kind) {
  case ARR_ITER_KEYS:
    *out = js_mknum((double)idx);
    break;
  case ARR_ITER_ENTRIES: {
    ant_value_t pair = js_mkarr(js);
    js_arr_push(js, pair, js_mknum((double)idx));
    js_arr_push(js, pair, js_arr_get(js, array, (ant_offset_t)idx));
    *out = pair;
    break;
  }
  default:
    *out = js_arr_get(js, array, (ant_offset_t)idx);
    break;
  }

  js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)ITER_STATE_PACK(kind, idx + 1)));
  return true;
}

static bool advance_string(ant_t *js, js_iter_t *it, ant_value_t *out) {
  ant_value_t iter = it->iterator;
  ant_value_t str = js_get_slot(iter, SLOT_DATA);
  ant_value_t idx_v = js_get_slot(iter, SLOT_ITER_STATE);
  int idx = (vtype(idx_v) == T_NUM) ? (int)js_getnum(idx_v) : 0;

  size_t slen;
  char *s = js_getstr(js, str, &slen);
  if (idx >= (int)slen) return false;

  unsigned char c = (unsigned char)s[idx];
  int char_bytes = utf8_sequence_length(c);
  if (char_bytes < 1) char_bytes = 1;
  if (idx + char_bytes > (int)slen) char_bytes = (int)slen - idx;

  *out = js_mkstr(js, s + idx, (ant_offset_t)char_bytes);
  js_set_slot(iter, SLOT_ITER_STATE, js_mknum(idx + char_bytes));
  return true;
}

static ant_value_t arr_iter_next(ant_t *js, ant_value_t *args, int nargs) {
  js_iter_t it = { .iterator = js->this_val };
  ant_value_t value;
  return js_iter_result(js, advance_array(js, &it, &value), value);
}

static ant_value_t get_array_iterator_prototype(ant_t *js) {
  if (vtype(js->sym.array_iterator_proto) == T_OBJ) return js->sym.array_iterator_proto;

  ant_value_t iterator_proto = get_iterator_prototype(js);
  js->sym.array_iterator_proto = js_mkobj(js);
  js_set(js, js->sym.array_iterator_proto, "next", js_mkfun(arr_iter_next));
  js_set_proto_init(js->sym.array_iterator_proto, iterator_proto);

  return js->sym.array_iterator_proto;
}

ant_value_t make_array_iterator(ant_t *js, ant_value_t array, int kind) {
  ant_value_t iter = js_mkobj(js);
  js_set_slot_wb(js, iter, SLOT_DATA, array);
  js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)ITER_STATE_PACK(kind, 0)));
  js_set_proto_init(iter, get_array_iterator_prototype(js));
  return iter;
}

static ant_value_t str_iter_next(ant_t *js, ant_value_t *args, int nargs) {
  js_iter_t it = { .iterator = js->this_val };
  ant_value_t value;
  return js_iter_result(js, advance_string(js, &it, &value), value);
}

static ant_value_t get_string_iterator_prototype(ant_t *js) {
  if (vtype(js->sym.string_iterator_proto) == T_OBJ) return js->sym.string_iterator_proto;

  ant_value_t iterator_proto = get_iterator_prototype(js);
  js->sym.string_iterator_proto = js_mkobj(js);
  js_set(js, js->sym.string_iterator_proto, "next", js_mkfun(str_iter_next));
  js_set_proto_init(js->sym.string_iterator_proto, iterator_proto);

  return js->sym.string_iterator_proto;
}

static ant_value_t string_iterator(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t iter = js_mkobj(js);
  
  js_set_slot_wb(js, iter, SLOT_DATA, js->this_val);
  js_set_slot(iter, SLOT_ITER_STATE, js_mknum(0));
  js_set_proto_init(iter, get_string_iterator_prototype(js));
  
  return iter;
}

static struct { 
  ant_value_t proto;
  js_iter_advance_fn fn;
} g_advance_table[8];

static int g_advance_count = 0;

void js_iter_register_advance(ant_value_t proto, js_iter_advance_fn fn) {
  if (g_advance_count < 8) 
    g_advance_table[g_advance_count++] = (typeof(g_advance_table[0])){ proto, fn };
}

bool js_iter_open(ant_t *js, ant_value_t iterable, js_iter_t *it) {
  memset(it, 0, sizeof(*it));

  ant_value_t iter_fn = js_get_sym(js, iterable, get_iterator_sym());
  if (!is_callable(iter_fn)) return false;

  ant_value_t iterator = sv_vm_call(js->vm, js, iter_fn, iterable, NULL, 0, NULL, false);
  if (is_err(iterator)) return false;

  it->iterator = iterator;
  it->next_fn = js_getprop_fallback(js, iterator, "next");
  it->advance = NULL;

  ant_value_t proto = (vtype(iterator) == T_OBJ) ? js_get_proto(js, iterator) : js_mkundef();
  for (int i = 0; i < g_advance_count; i++)
    if (proto == g_advance_table[i].proto) { it->advance = g_advance_table[i].fn; break; }

  return true;
}

bool js_iter_next(ant_t *js, js_iter_t *it, ant_value_t *out) {
  if (it->advance) return it->advance(js, it, out);

  ant_value_t next_fn = it->next_fn;
  ant_value_t result;

  if (vtype(next_fn) == T_CFUNC) {
    ant_value_t old_this = js->this_val;
    js->this_val = it->iterator;
    result = js_as_cfunc(next_fn)(js, NULL, 0);
    js->this_val = old_this;
  }
  
  else if (is_callable(next_fn)) result = sv_vm_call(js->vm, js, next_fn, it->iterator, NULL, 0, NULL, false);
  else return false;

  if (is_err(result)) return false;
  ant_value_t done = js_getprop_fallback(js, result, "done");
  
  if (js_truthy(js, done)) return false;
  *out = js_getprop_fallback(js, result, "value");
  
  return true;
}

void js_iter_close(ant_t *js, js_iter_t *it) {
  if (it->advance) return;
  ant_value_t return_fn = js_getprop_fallback(js, it->iterator, "return");
  if (is_callable(return_fn)) sv_vm_call(js->vm, js, return_fn, it->iterator, NULL, 0, NULL, false);
}

ant_value_t maybe_call_symbol_method(
  ant_t *js, ant_value_t target,
  ant_value_t sym,
  ant_value_t this_arg, ant_value_t *args,
  int nargs, bool *called
) {
  *called = false;
  if (vtype(sym) != T_SYMBOL || !is_object_type(target)) return js_mkundef();

  ant_value_t method = js_get_sym(js, target, sym);
  if (is_err(method)) return method;

  uint8_t mt = vtype(method);
  if (mt == T_UNDEF || mt == T_NULL) return js_mkundef();
  if (mt != T_FUNC && mt != T_CFUNC) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Symbol method is not callable");
  }

  *called = true;
  return sv_vm_call(js->vm, js, method, this_arg, args, nargs, NULL, false);
}

void js_define_species_getter(ant_t *js, ant_value_t ctor) {
  if (!is_object_type(ctor) || vtype(g_species) != T_SYMBOL) return;
  ctor = js_as_obj(ctor);
  js_set_sym_getter_desc(js, ctor, g_species, js_mkfun(sym_this_cb), JS_DESC_C);
}

void init_symbol_module(void) {
  ant_t *js = rt->js;

  js->sym.iterator_proto = js_mkundef();
  js->sym.array_iterator_proto = js_mkundef();
  js->sym.string_iterator_proto = js_mkundef();
  
  gc_register_root(&js->sym.iterator_proto);
  gc_register_root(&js->sym.array_iterator_proto);
  gc_register_root(&js->sym.string_iterator_proto);

  #define INIT_SYM(name, desc) g_##name = js_mksym_well_known(js, desc);
  WELLKNOWN_SYMBOLS(INIT_SYM)
  #undef INIT_SYM

  ant_value_t symbol_proto = js_mkobj(js);
  js_set(js, symbol_proto, "toString", js_mkfun(builtin_Symbol_toString));
  
  ant_value_t symbol_ctor = js_mkobj(js);
  js_set_slot(symbol_ctor, SLOT_CFUNC, js_mkfun(builtin_Symbol));
  js_setprop(js, symbol_ctor, js_mkstr(js, "for", 3), js_mkfun(builtin_Symbol_for));
  js_set(js, symbol_ctor, "keyFor", js_mkfun(builtin_Symbol_keyFor));
  js_set(js, symbol_ctor, "prototype", symbol_proto);
  
  #define SET_CTOR_SYM(name, _desc) js_set(js, symbol_ctor, #name, g_##name);
  WELLKNOWN_SYMBOLS(SET_CTOR_SYM)
  #undef SET_CTOR_SYM
  
  ant_value_t func_symbol = js_obj_to_func(symbol_ctor);
  js_set(js, js_glob(js), "Symbol", func_symbol);

  // set internal types before ant module snapshot
  ant_value_t array_ctor = js_get(js, js_glob(js), "Array");
  ant_value_t array_proto = js_get(js, array_ctor, "prototype");
  
  (void)get_array_iterator_prototype(js);
  (void)get_string_iterator_prototype(js);
  
  js_iter_register_advance(js->sym.array_iterator_proto, advance_array);
  js_iter_register_advance(js->sym.string_iterator_proto, advance_string);
  
  js_set_sym(js, rt->ant_obj, g_toStringTag, js_mkstr(js, "Ant", 3));
  js_set_sym(js, array_proto, g_iterator, js_get(js, array_proto, "values"));

  ant_value_t array_unscopables = js_mkobj(js);
  js_set(js, array_unscopables, "find", js_true);
  js_set(js, array_unscopables, "findIndex", js_true);
  js_set(js, array_unscopables, "fill", js_true);
  js_set(js, array_unscopables, "copyWithin", js_true);
  js_set(js, array_unscopables, "entries", js_true);
  js_set(js, array_unscopables, "keys", js_true);
  js_set(js, array_unscopables, "values", js_true);
  js_set(js, array_unscopables, "flat", js_true);
  js_set(js, array_unscopables, "flatMap", js_true);
  js_set_sym(js, array_proto, g_unscopables, array_unscopables);
  
  ant_value_t string_ctor = js_get(js, js_glob(js), "String");
  ant_value_t string_proto = js_get(js, string_ctor, "prototype");
  js_set_sym(js, string_proto, g_iterator, js_mkfun(string_iterator));
  
  ant_value_t promise_ctor = js_get(js, js_glob(js), "Promise");
  ant_value_t promise_proto = js_get(js, promise_ctor, "prototype");
  js_set_sym(js, promise_proto, g_toStringTag, js_mkstr(js, "Promise", 7));

  js_define_species_getter(js, promise_ctor);
  js_define_species_getter(js, array_ctor);
}

void gc_mark_symbols(ant_t *js, gc_mark_fn mark) {
  mark(js, js->sym.iterator_proto);
  mark(js, js->sym.array_iterator_proto);
  mark(js, js->sym.string_iterator_proto);

  #define GC_SYM(name, _desc) mark(js, g_##name);
  WELLKNOWN_SYMBOLS(GC_SYM)
  #undef GC_SYM
}

