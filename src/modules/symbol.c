#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ant.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "modules/symbol.h"

typedef struct {
  jsval_t sym;
  char key[32];
} wellknown_sym_t;

static wellknown_sym_t g_iterator = {0};
static wellknown_sym_t g_asyncIterator = {0};
static wellknown_sym_t g_toStringTag = {0};
static wellknown_sym_t g_hasInstance = {0};
static wellknown_sym_t g_observable = {0};
static wellknown_sym_t g_toPrimitive = {0};
static wellknown_sym_t g_species = {0};

const char *get_iterator_sym_key(void) { return g_iterator.key; }
const char *get_asyncIterator_sym_key(void) { return g_asyncIterator.key; }
const char *get_toStringTag_sym_key(void) { return g_toStringTag.key; }
const char *get_observable_sym_key(void) { return g_observable.key; }
const char *get_toPrimitive_sym_key(void) { return g_toPrimitive.key; }
const char *get_hasInstance_sym_key(void) { return g_hasInstance.key; }
const char *get_species_sym_key(void) { return g_species.key; }

static const struct { jsval_t *sym; const char *name; } sym_table[] = {
  { &g_iterator.sym, "Symbol.iterator" },
  { &g_asyncIterator.sym, "Symbol.asyncIterator" },
  { &g_toStringTag.sym, "Symbol.toStringTag" },
  { &g_hasInstance.sym, "Symbol.hasInstance" },
  { &g_observable.sym, "Symbol.observable" },
  { &g_toPrimitive.sym, "Symbol.toPrimitive" },
  { &g_species.sym, "Symbol.species" },
};

bool is_symbol_key(const char *key, size_t key_len) {
  return 
    key_len > 7 
    && memcmp(key, "__sym_", 6) == 0 
    && key[key_len - 1] == '_' 
    && key[key_len - 2] == '_';
}

int sym_to_prop_key(jsval_t sym, char *buf, size_t bufsz) {
  return snprintf(buf, bufsz, "__sym_%llu__", (unsigned long long)js_sym_id(sym));
}

jsval_t get_wellknown_sym_by_key(const char *key, size_t key_len) {
  static const struct { wellknown_sym_t *sym; } table[] = {
    { &g_iterator }, { &g_asyncIterator }, { &g_toStringTag },
    { &g_hasInstance }, { &g_observable }, { &g_toPrimitive }, { &g_species }
  };
  for (size_t i = 0; i < sizeof(table)/sizeof(table[0]); i++) {
    if (table[i].sym->key[0] && strlen(table[i].sym->key) == key_len &&
        memcmp(table[i].sym->key, key, key_len) == 0)
      return table[i].sym->sym;
  }
  return (jsval_t)0;
}

const char *get_symbol_description_from_key(const char *sym_key, size_t key_len) {
  if (!is_symbol_key(sym_key, key_len)) return NULL;
  
  uint64_t id = 0;
  for (const char *p = sym_key + 6; *p >= '0' && *p <= '9'; p++) id = id * 10 + (*p - '0');
  
  for (size_t i = 0; i < sizeof(sym_table) / sizeof(sym_table[0]); i++) {
    if (js_sym_id(*sym_table[i].sym) == id) return sym_table[i].name;
  }
  
  return "Symbol()";
}

static inline void init_symbol(struct js *js, wellknown_sym_t *sym_var, const char *name) {
  sym_var->sym = js_mksym(js, name);
  sym_to_prop_key(sym_var->sym, sym_var->key, sizeof(sym_var->key));
}

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

static jsval_t builtin_Symbol_toString(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_val = js_getthis(js);
  
  if (vtype(this_val) != T_SYMBOL) {
    return js_mkerr(js, "Symbol.prototype.toString requires a symbol");
  }
  
  return js_symbol_to_string(js, this_val);
}

static jsval_t iterator_next(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_val = js_getthis(js);
  
  jsval_t arr = js_get(js, this_val, "__arr");
  jsval_t idx_val = js_get(js, this_val, "__idx");
  jsval_t len_val = js_get(js, this_val, "__len");
  
  int idx = (int)js_getnum(idx_val);
  int len = (int)js_getnum(len_val);
  
  jsval_t result = js_mkobj(js);
  
  if (idx >= len) {
    js_set(js, result, "done", js_true);
    js_set(js, result, "value", js_mkundef());
  } else {
    char idxstr[16];
    snprintf(idxstr, sizeof(idxstr), "%d", idx);
    jsval_t value = js_get(js, arr, idxstr);
    js_set(js, result, "value", value);
    js_set(js, result, "done", js_false);
    js_set(js, this_val, "__idx", js_mknum(idx + 1));
  }
  
  return result;
}

static jsval_t array_iterator(struct js *js, jsval_t *args, int nargs) {
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
  jsval_t this_val = js_getthis(js);
  
  jsval_t str = js_get(js, this_val, "__str");
  jsval_t idx_val = js_get(js, this_val, "__idx");
  jsval_t len_val = js_get(js, this_val, "__len");
  
  int idx = (int)js_getnum(idx_val);
  int len = (int)js_getnum(len_val);
  
  jsval_t result = js_mkobj(js);
  
  if (idx >= len) {
    js_set(js, result, "done", js_true);
    js_set(js, result, "value", js_mkundef());
  } else {
    char *s = js_getstr(js, str, NULL);
    char ch[2] = {s[idx], 0};
    js_set(js, result, "value", js_mkstr(js, ch, 1));
    js_set(js, result, "done", js_false);
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

static jsval_t species_getter(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  return js_getthis(js);
}

void js_define_species_getter(struct js *js, jsval_t ctor) {
  if (vtype(ctor) == T_FUNC) ctor = mkval(T_OBJ, vdata(ctor));
  if (vtype(ctor) != T_OBJ || g_species.key[0] == '\0') return;
  js_set_getter_desc(js, ctor, g_species.key, strlen(g_species.key), js_mkfun(species_getter), JS_DESC_C);
}

static jsval_t date_toPrimitive(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_val = js_getthis(js);
  
  const char *hint = "default";
  if (nargs > 0 && vtype(args[0]) == T_STR) {
    hint = js_getstr(js, args[0], NULL);
  } bool prefer_string = (hint == NULL || strcmp(hint, "number") != 0);
  
  const char *methods[2] = {
    prefer_string ? "toString" : "valueOf",
    prefer_string ? "valueOf" : "toString"
  };
  
  for (int i = 0; i < 2; i++) {
    jsval_t method = js_getprop_fallback(js, this_val, methods[i]);
    if (vtype(method) == T_FUNC || vtype(method) == T_CFUNC) {
      jsval_t result = js_call_with_this(js, method, this_val, NULL, 0);
      if (is_err(result) || !is_object_type(result)) return result;
    }
  }
  
  return js_mkerr(js, "Cannot convert object to primitive value");
}


void init_symbol_module(void) {
  struct js *js = rt->js;
  
  init_symbol(js, &g_iterator, "Symbol.iterator");
  init_symbol(js, &g_asyncIterator, "Symbol.asyncIterator");
  init_symbol(js, &g_toStringTag, "Symbol.toStringTag");
  init_symbol(js, &g_observable, "Symbol.observable");
  init_symbol(js, &g_toPrimitive, "Symbol.toPrimitive");
  init_symbol(js, &g_hasInstance, "Symbol.hasInstance");
  init_symbol(js, &g_species, "Symbol.species");

  jsval_t symbol_proto = js_mkobj(js);
  js_set(js, symbol_proto, "toString", js_mkfun(builtin_Symbol_toString));
  
  jsval_t symbol_ctor = js_mkobj(js);
  js_set_slot(js, symbol_ctor, SLOT_CFUNC, js_mkfun(builtin_Symbol));
  js_setprop(js, symbol_ctor, js_mkstr(js, "for", 3), js_mkfun(builtin_Symbol_for));
  js_set(js, symbol_ctor, "keyFor", js_mkfun(builtin_Symbol_keyFor));
  js_set(js, symbol_ctor, "prototype", symbol_proto);
  
  js_set(js, symbol_ctor, "iterator", g_iterator.sym);
  js_set(js, symbol_ctor, "asyncIterator", g_asyncIterator.sym);
  js_set(js, symbol_ctor, "toStringTag", g_toStringTag.sym);
  js_set(js, symbol_ctor, "hasInstance", g_hasInstance.sym);
  js_set(js, symbol_ctor, "observable", g_observable.sym);
  js_set(js, symbol_ctor, "toPrimitive", g_toPrimitive.sym);
  js_set(js, symbol_ctor, "species", g_species.sym);
  
  jsval_t func_symbol = js_obj_to_func(symbol_ctor);
  js_set(js, js_glob(js), "Symbol", func_symbol);
  
  // set internal types before ant module snapshot
  js_set(js, rt->ant_obj, get_toStringTag_sym_key(), js_mkstr(js, "Ant", 3));
  
  jsval_t array_ctor = js_get(js, js_glob(js), "Array");
  jsval_t array_proto = js_get(js, array_ctor, "prototype");
  js_set(js, array_proto, g_iterator.key, js_mkfun(array_iterator));
  
  jsval_t string_ctor = js_get(js, js_glob(js), "String");
  jsval_t string_proto = js_get(js, string_ctor, "prototype");
  js_set(js, string_proto, g_iterator.key, js_mkfun(string_iterator));
  
  jsval_t date_ctor = js_get(js, js_glob(js), "Date");
  jsval_t date_proto = js_get(js, date_ctor, "prototype");
  js_set(js, date_proto, g_toPrimitive.key, js_mkfun(date_toPrimitive));
  
  jsval_t promise_ctor = js_get(js, js_glob(js), "Promise");
  jsval_t promise_proto = js_get(js, promise_ctor, "prototype");
  js_set(js, promise_proto, g_toStringTag.key, js_mkstr(js, "Promise", 7));

  js_define_species_getter(js, promise_ctor);
  js_define_species_getter(js, array_ctor);
  js_define_species_getter(js, js_get(js, js_glob(js), "RegExp"));
}
