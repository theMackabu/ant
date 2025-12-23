#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ant.h"
#include "runtime.h"
#include "modules/symbol.h"

#define MAX_REGISTRY 256
static struct {
  char *key;
  jsval_t symbol;
} g_symbol_registry[MAX_REGISTRY];
static int g_registry_count = 0;

static jsval_t g_iterator_sym = 0;
static jsval_t g_toStringTag_sym = 0;
static jsval_t g_hasInstance_sym = 0;

static char g_iter_sym_key[32] = {0};
static char g_toStringTag_sym_key[32] = {0};

jsval_t get_iterator_symbol(void) { return g_iterator_sym; }
const char *get_iterator_sym_key(void) { return g_iter_sym_key; }
const char *get_toStringTag_sym_key(void) { return g_toStringTag_sym_key; }

static jsval_t builtin_Symbol(struct js *js, jsval_t *args, int nargs) {
  const char *desc = NULL;
  if (nargs > 0 && js_type(args[0]) == JS_STR) {
    desc = js_getstr(js, args[0], NULL);
  }
  return js_mksym(js, desc);
}

static jsval_t builtin_Symbol_for(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1 || js_type(args[0]) != JS_STR) {
    return js_mkerr(js, "Symbol.for requires a string argument");
  }
  
  char *key = js_getstr(js, args[0], NULL);
  if (!key) return js_mkerr(js, "Invalid key");
  
  for (int i = 0; i < g_registry_count; i++) {
    if (g_symbol_registry[i].key && strcmp(g_symbol_registry[i].key, key) == 0) {
      return g_symbol_registry[i].symbol;
    }
  }
  
  if (g_registry_count >= MAX_REGISTRY) {
    return js_mkerr(js, "Symbol registry full");
  }
  
  jsval_t sym = js_mksym(js, key);
  g_symbol_registry[g_registry_count].key = strdup(key);
  g_symbol_registry[g_registry_count].symbol = sym;
  
  jsval_t sym_obj = js_mkobj(js);
  js_set(js, sym_obj, "__sym_id", js_get(js, sym, "__sym_id"));
  js_set(js, sym_obj, "description", js_get(js, sym, "description"));
  js_set(js, sym_obj, "__registry_key", js_mkstr(js, key, strlen(key)));
  
  g_symbol_registry[g_registry_count].symbol = sym;
  g_registry_count++;
  
  return sym;
}

static bool is_symbol_val(struct js *js, jsval_t v) {
  if (js_type(v) != JS_OBJ) return false;
  jsval_t marker = js_get(js, v, "__sym__");
  return js_type(marker) == JS_TRUE;
}

static jsval_t builtin_Symbol_keyFor(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1 || !is_symbol_val(js, args[0])) {
    return js_mkundef();
  }
  
  jsval_t sym = args[0];
  jsval_t sym_id = js_get(js, sym, "__sym_id");
  
  for (int i = 0; i < g_registry_count; i++) {
    jsval_t reg_id = js_get(js, g_symbol_registry[i].symbol, "__sym_id");
    if (js_type(sym_id) == JS_NUM && js_type(reg_id) == JS_NUM &&
        js_getnum(sym_id) == js_getnum(reg_id)) {
      return js_mkstr(js, g_symbol_registry[i].key, strlen(g_symbol_registry[i].key));
    }
  }
  
  return js_mkundef();
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
  int len = js_type(len_val) == JS_NUM ? (int)js_getnum(len_val) : 0;
  
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
  g_toStringTag_sym = js_mksym(js, "Symbol.toStringTag");
  g_hasInstance_sym = js_mksym(js, "Symbol.hasInstance");
  
  jsval_t iter_sym_id = js_get(js, g_iterator_sym, "__sym_id");
  snprintf(g_iter_sym_key, sizeof(g_iter_sym_key), "__sym_%.0f__", js_getnum(iter_sym_id));
  
  jsval_t tag_sym_id = js_get(js, g_toStringTag_sym, "__sym_id");
  snprintf(g_toStringTag_sym_key, sizeof(g_toStringTag_sym_key), "__sym_%.0f__", js_getnum(tag_sym_id));
  
  jsval_t symbol_ctor = js_mkobj(js);
  js_set(js, symbol_ctor, "__native_func", js_mkfun(builtin_Symbol));
  js_setprop(js, symbol_ctor, js_mkstr(js, "for", 3), js_mkfun(builtin_Symbol_for));
  js_set(js, symbol_ctor, "keyFor", js_mkfun(builtin_Symbol_keyFor));
  
  js_set(js, symbol_ctor, "iterator", g_iterator_sym);
  js_set(js, symbol_ctor, "toStringTag", g_toStringTag_sym);
  js_set(js, symbol_ctor, "hasInstance", g_hasInstance_sym);
  
  jsval_t func_symbol = symbol_ctor;
  *(uint64_t*)&func_symbol = (func_symbol & ~((uint64_t)0xF << 48)) | ((uint64_t)0x7 << 48);
  
  js_set(js, js_glob(js), "Symbol", func_symbol);
  
  jsval_t array_ctor = js_get(js, js_glob(js), "Array");
  jsval_t array_proto = js_get(js, array_ctor, "prototype");
  js_set(js, array_proto, g_iter_sym_key, js_mkfun(array_iterator));
  
  jsval_t string_ctor = js_get(js, js_glob(js), "String");
  jsval_t string_proto = js_get(js, string_ctor, "prototype");
  js_set(js, string_proto, g_iter_sym_key, js_mkfun(string_iterator));
  
  // set internal types before module ready
  jsval_t map_ctor = js_get(js, js_glob(js), "Map");
  jsval_t map_proto = js_get(js, map_ctor, "prototype");
  if (js_type(map_proto) == JS_OBJ) {
    js_set(js, map_proto, g_toStringTag_sym_key, js_mkstr(js, "Map", 3));
  }
  
  jsval_t set_ctor = js_get(js, js_glob(js), "Set");
  jsval_t set_proto = js_get(js, set_ctor, "prototype");
  if (js_type(set_proto) == JS_OBJ) {
    js_set(js, set_proto, g_toStringTag_sym_key, js_mkstr(js, "Set", 3));
  }
}
