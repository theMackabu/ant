#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ant.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "modules/symbol.h"

static jsval_t g_iterator_sym = 0;
static jsval_t g_asyncIterator_sym = 0;
static jsval_t g_toStringTag_sym = 0;
static jsval_t g_hasInstance_sym = 0;
static jsval_t g_observable_sym = 0;

static char g_iter_sym_key[32] = {0};
static char g_asyncIter_sym_key[32] = {0};
static char g_toStringTag_sym_key[32] = {0};
static char g_observable_sym_key[32] = {0};

jsval_t get_iterator_symbol(void) { return g_iterator_sym; }
jsval_t get_asyncIterator_symbol(void) { return g_asyncIterator_sym; }
jsval_t get_observable_symbol(void) { return g_observable_sym; }

const char *get_iterator_sym_key(void) { return g_iter_sym_key; }
const char *get_asyncIterator_sym_key(void) { return g_asyncIter_sym_key; }
const char *get_toStringTag_sym_key(void) { return g_toStringTag_sym_key; }
const char *get_observable_sym_key(void) { return g_observable_sym_key; }

static jsval_t builtin_Symbol(struct js *js, jsval_t *args, int nargs) {
  const char *desc = NULL;
  if (nargs > 0 && vtype(args[0]) == T_STR) {
    desc = js_getstr(js, args[0], NULL);
  }
  return js_mksym(js, desc);
}

static jsval_t builtin_Symbol_for(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_STR) {
    return js_mkerr(js, "Symbol.for requires a string argument");
  }
  
  char *key = js_getstr(js, args[0], NULL);
  if (!key) return js_mkerr(js, "Invalid key");
  
  return js_mksym_for(js, key);
}

static jsval_t builtin_Symbol_keyFor(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_SYMBOL) {
    return js_mkundef();
  }
  
  const char *key = js_sym_key(args[0]);
  if (!key) return js_mkundef();
  
  return js_mkstr(js, key, strlen(key));
}

static jsval_t iterator_next(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t this_val = js_getthis(js);
  
  jsval_t arr = js_get(js, this_val, "__arr");
  jsval_t idx_val = js_get(js, this_val, "__idx");
  jsval_t len_val = js_get(js, this_val, "__len");
  
  int idx = (int)js_getnum(idx_val);
  int len = (int)js_getnum(len_val);
  
  jsval_t result = js_mkobj(js);
  
  if (idx >= len) {
    js_set(js, result, "done", js_mktrue());
    js_set(js, result, "value", js_mkundef());
  } else {
    char idxstr[16];
    snprintf(idxstr, sizeof(idxstr), "%d", idx);
    jsval_t value = js_get(js, arr, idxstr);
    js_set(js, result, "value", value);
    js_set(js, result, "done", js_mkfalse());
    js_set(js, this_val, "__idx", js_mknum(idx + 1));
  }
  
  return result;
}

static jsval_t array_iterator(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t arr = js_getthis(js);
  
  jsval_t len_val = js_get(js, arr, "length");
  int len = vtype(len_val) == T_NUM ? (int)js_getnum(len_val) : 0;
  
  jsval_t iter = js_mkobj(js);
  js_set(js, iter, "__arr", arr);
  js_set(js, iter, "__idx", js_mknum(0));
  js_set(js, iter, "__len", js_mknum(len));
  js_set(js, iter, "next", js_mkfun(iterator_next));
  
  return iter;
}

static jsval_t string_iterator_next(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t this_val = js_getthis(js);
  
  jsval_t str = js_get(js, this_val, "__str");
  jsval_t idx_val = js_get(js, this_val, "__idx");
  jsval_t len_val = js_get(js, this_val, "__len");
  
  int idx = (int)js_getnum(idx_val);
  int len = (int)js_getnum(len_val);
  
  jsval_t result = js_mkobj(js);
  
  if (idx >= len) {
    js_set(js, result, "done", js_mktrue());
    js_set(js, result, "value", js_mkundef());
  } else {
    char *s = js_getstr(js, str, NULL);
    char ch[2] = {s[idx], 0};
    js_set(js, result, "value", js_mkstr(js, ch, 1));
    js_set(js, result, "done", js_mkfalse());
    js_set(js, this_val, "__idx", js_mknum(idx + 1));
  }
  
  return result;
}

static jsval_t string_iterator(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t str = js_getthis(js);
  
  size_t len;
  js_getstr(js, str, &len);
  
  jsval_t iter = js_mkobj(js);
  js_set(js, iter, "__str", str);
  js_set(js, iter, "__idx", js_mknum(0));
  js_set(js, iter, "__len", js_mknum((double)len));
  js_set(js, iter, "next", js_mkfun(string_iterator_next));
  
  return iter;
}

const char *get_symbol_description_from_key(const char *sym_key, size_t key_len) {
  if (
    key_len < 9 || sym_key[0] != '_' || sym_key[1] != '_' || 
    sym_key[2] != 's' || sym_key[3] != 'y' || sym_key[4] != 'm' || sym_key[5] != '_'
  ) return NULL;
  
  if (g_iter_sym_key[0] && strncmp(sym_key, g_iter_sym_key, key_len) == 0 && g_iter_sym_key[key_len] == '\0') return "Symbol.iterator";
  if (g_toStringTag_sym_key[0] && strncmp(sym_key, g_toStringTag_sym_key, key_len) == 0 && g_toStringTag_sym_key[key_len] == '\0') return "Symbol.toStringTag";
  
  return "Symbol()";
}

void init_symbol_module(void) {
  struct js *js = rt->js;
  
  g_iterator_sym = js_mksym(js, "Symbol.iterator");
  g_asyncIterator_sym = js_mksym(js, "Symbol.asyncIterator");
  g_toStringTag_sym = js_mksym(js, "Symbol.toStringTag");
  g_hasInstance_sym = js_mksym(js, "Symbol.hasInstance");
  g_observable_sym = js_mksym(js, "Symbol.observable");
  
  snprintf(g_iter_sym_key, sizeof(g_iter_sym_key), "__sym_%llu__", (unsigned long long)js_sym_id(g_iterator_sym));
  snprintf(g_asyncIter_sym_key, sizeof(g_asyncIter_sym_key), "__sym_%llu__", (unsigned long long)js_sym_id(g_asyncIterator_sym));
  snprintf(g_toStringTag_sym_key, sizeof(g_toStringTag_sym_key), "__sym_%llu__", (unsigned long long)js_sym_id(g_toStringTag_sym));
  snprintf(g_observable_sym_key, sizeof(g_observable_sym_key), "__sym_%llu__", (unsigned long long)js_sym_id(g_observable_sym));
  
  jsval_t symbol_ctor = js_mkobj(js);
  js_set_slot(js, symbol_ctor, SLOT_CFUNC, js_mkfun(builtin_Symbol));
  js_setprop(js, symbol_ctor, js_mkstr(js, "for", 3), js_mkfun(builtin_Symbol_for));
  js_set(js, symbol_ctor, "keyFor", js_mkfun(builtin_Symbol_keyFor));
  
  js_set(js, symbol_ctor, "iterator", g_iterator_sym);
  js_set(js, symbol_ctor, "asyncIterator", g_asyncIterator_sym);
  js_set(js, symbol_ctor, "toStringTag", g_toStringTag_sym);
  js_set(js, symbol_ctor, "hasInstance", g_hasInstance_sym);
  js_set(js, symbol_ctor, "observable", g_observable_sym);
  
  jsval_t func_symbol = js_obj_to_func(symbol_ctor);
  js_set(js, js_glob(js), "Symbol", func_symbol);
  
  // set internal types before module ready
  js_set(js, rt->ant_obj, get_toStringTag_sym_key(), js_mkstr(js, "Ant", 3));
  
  jsval_t array_ctor = js_get(js, js_glob(js), "Array");
  jsval_t array_proto = js_get(js, array_ctor, "prototype");
  js_set(js, array_proto, g_iter_sym_key, js_mkfun(array_iterator));
  
  jsval_t string_ctor = js_get(js, js_glob(js), "String");
  jsval_t string_proto = js_get(js, string_ctor, "prototype");
  js_set(js, string_proto, g_iter_sym_key, js_mkfun(string_iterator));
  
  jsval_t map_ctor = js_get(js, js_glob(js), "Map");
  jsval_t map_proto = js_get(js, map_ctor, "prototype");
  js_set(js, map_proto, g_toStringTag_sym_key, js_mkstr(js, "Map", 3));
  
  jsval_t set_ctor = js_get(js, js_glob(js), "Set");
  jsval_t set_proto = js_get(js, set_ctor, "prototype");
  js_set(js, set_proto, g_toStringTag_sym_key, js_mkstr(js, "Set", 3));
}
