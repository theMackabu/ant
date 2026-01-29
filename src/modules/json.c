#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <yyjson.h>
#include <uthash.h>

#include "errors.h"
#include "runtime.h"
#include "internal.h"

#include "modules/json.h"
#include "modules/symbol.h"

typedef struct {
  const char *key;
  size_t key_len;
  jsoff_t prop_off;
  UT_hash_handle hh;
} json_key_entry_t;

static jsval_t yyjson_to_jsval(struct js *js, yyjson_val *val) {
  if (!val) return js_mkundef();
  
  switch (yyjson_get_type(val)) {
    case YYJSON_TYPE_NULL: return js_mknull();
    case YYJSON_TYPE_BOOL: return js_bool(yyjson_get_bool(val));
    case YYJSON_TYPE_STR:  return js_mkstr(js, yyjson_get_str(val), yyjson_get_len(val));
    
    case YYJSON_TYPE_NUM: {
      if (yyjson_is_sint(val)) return js_mknum((double)yyjson_get_sint(val));
      if (yyjson_is_uint(val)) return js_mknum((double)yyjson_get_uint(val));
      return js_mknum(yyjson_get_real(val));
    }
    
    case YYJSON_TYPE_ARR: {
      jsval_t arr = js_mkarr(js);
      size_t idx, max;
      yyjson_val *item;
      
      yyjson_arr_foreach(val, idx, max, item)
        js_arr_push(js, arr, yyjson_to_jsval(js, item));
      
      return arr;
    }
    
    case YYJSON_TYPE_OBJ: {
      jsval_t obj = js_newobj(js);
      
      size_t idx, max; yyjson_val *key, *item;
      json_key_entry_t *hash = NULL, *entry, *tmp;
      
      yyjson_obj_foreach(val, idx, max, key, item) {
        const char *k = yyjson_get_str(key);
        size_t klen = yyjson_get_len(key);
        jsval_t v = yyjson_to_jsval(js, item);
        
        HASH_FIND(hh, hash, k, klen, entry);
        if (entry) js_saveval(js, entry->prop_off, v); else {
          jsoff_t off = js_mkprop_fast_off(js, obj, k, klen, v);
          entry = malloc(sizeof(json_key_entry_t));
          entry->key = k; entry->key_len = klen; entry->prop_off = off;
          HASH_ADD_KEYPTR(hh, hash, entry->key, entry->key_len, entry);
        }
      }
      
      HASH_ITER(hh, hash, entry, tmp) {
        HASH_DEL(hash, entry); free(entry);
      }
      
      return obj;
    }
    
    default: return js_mkundef();
  }
}

typedef struct {
  jsval_t *stack;
  int stack_size;
  int stack_cap;
  int has_cycle;
  struct js *js;
  jsval_t replacer_func;
  jsval_t replacer_arr;
  int replacer_arr_len;
  jsval_t holder;
} json_cycle_ctx;

static int json_cycle_check(json_cycle_ctx *ctx, jsval_t val) {
  for (int i = 0; i < ctx->stack_size; i++)
    if (ctx->stack[i] == val) { ctx->has_cycle = 1; return 1; }
  return 0;
}

static void json_cycle_push(json_cycle_ctx *ctx, jsval_t val) {
  if (ctx->stack_size >= ctx->stack_cap) {
    ctx->stack_cap = ctx->stack_cap ? ctx->stack_cap * 2 : 16;
    ctx->stack = realloc(ctx->stack, ctx->stack_cap * sizeof(jsval_t));
  }
  ctx->stack[ctx->stack_size++] = val;
}

static inline void json_cycle_pop(json_cycle_ctx *ctx) {
  if (ctx->stack_size > 0) ctx->stack_size--;
}

typedef struct { char *key; size_t key_len; jsval_t value; } prop_entry;

static int should_skip_prop(struct js *js, const char *key, size_t key_len, jsval_t value) {
  if (is_internal_prop(key, (jsoff_t)key_len)) return 1;
  if (!is_special_object(value)) return 0;
  return vtype(js_get_slot(js, value, SLOT_CODE)) == T_CFUNC;
}

static prop_entry *collect_props(struct js *js, jsval_t val, int *out_count) {
  prop_entry *props = NULL;
  int count = 0, cap = 0;
  const char *key;
  size_t key_len;
  jsval_t value;
  
  ant_iter_t iter = js_prop_iter_begin(js, val);
  while (js_prop_iter_next(&iter, &key, &key_len, &value)) {
    if (should_skip_prop(js, key, key_len, value)) continue;
    
    if (count >= cap) {
      cap = cap ? cap * 2 : 8;
      props = realloc(props, cap * sizeof(prop_entry));
    }
    
    props[count].key = malloc(key_len + 1);
    memcpy(props[count].key, key, key_len);
    props[count].key[key_len] = '\0';
    props[count].key_len = key_len;
    props[count].value = value;
    count++;
  }
  js_prop_iter_end(&iter);
  
  *out_count = count;
  return props;
}

static inline void free_props(prop_entry *props, int from, int to) {
  for (int i = from; i <= to; i++) free(props[i].key);
  free(props);
}

static inline int key_matches(const char *a, size_t a_len, const char *b, size_t b_len) {
  return a_len == b_len && memcmp(a, b, a_len) == 0;
}

static int is_key_in_replacer_arr(struct js *js, json_cycle_ctx *ctx, const char *key, size_t key_len) {
  if (!is_special_object(ctx->replacer_arr)) return 1;
  
  for (int i = 0; i < ctx->replacer_arr_len; i++) {
    char idxstr[32];
    snprintf(idxstr, sizeof(idxstr), "%d", i);
    jsval_t item = js_get(js, ctx->replacer_arr, idxstr);
    int type = vtype(item);
    
    if (type == T_STR) {
      size_t item_len;
      char *item_str = js_getstr(js, item, &item_len);
      if (key_matches(item_str, item_len, key, key_len)) return 1;
    } else if (type == T_NUM) {
      char numstr[32];
      snprintf(numstr, sizeof(numstr), "%.0f", js_getnum(item));
      if (key_matches(numstr, strlen(numstr), key, key_len)) return 1;
    }
  }
  return 0;
}

static yyjson_mut_val *jsval_to_yyjson_with_key(struct js *js, yyjson_mut_doc *doc, const char *key, jsval_t val, json_cycle_ctx *ctx, int in_array);

static yyjson_mut_val *jsval_to_yyjson_impl(struct js *js, yyjson_mut_doc *doc, jsval_t val, json_cycle_ctx *ctx, int in_array) {
  int type = vtype(val);
  yyjson_mut_val *result = NULL;
  
  if (is_special_object(val)) {
    jsval_t toJSON = js_get(js, val, "toJSON");
    if (vtype(toJSON) == T_FUNC) {
      jsval_t r = js_call(js, toJSON, &val, 1);
      if (vtype(r) == T_ERR) { ctx->has_cycle = 1; return NULL; }
      return jsval_to_yyjson_impl(js, doc, r, ctx, in_array);
    }
  }
  
  switch (type) {
    case T_NULL:   return yyjson_mut_null(doc);
    case T_BOOL:   return yyjson_mut_bool(doc, val == js_true);
    
    case T_UNDEF:  return in_array ? yyjson_mut_null(doc) : YYJSON_SKIP_VALUE;
    case T_FUNC:   return in_array ? yyjson_mut_null(doc) : YYJSON_SKIP_VALUE;
    
    case T_NUM: {
      double num = js_getnum(val);
      if (isnan(num) || isinf(num)) return yyjson_mut_null(doc);
      if (
        num >= (double)INT64_MIN && 
        num < (double)INT64_MAX && 
        num == (double)(int64_t)num
      ) return yyjson_mut_sint(doc, (int64_t)num);
      return yyjson_mut_real(doc, num);
    }
    
    case T_STR: {
      size_t len;
      char *str = js_getstr(js, val, &len);
      return yyjson_mut_strncpy(doc, str, len);
    }
    
    case T_OBJ:
    case T_ARR: break;
    default: return yyjson_mut_null(doc);
  }
  
  if (json_cycle_check(ctx, val)) return NULL;
  json_cycle_push(ctx, val);
  
  jsval_t length_val = js_get(js, val, "length");
  
  if (vtype(length_val) == T_NUM) {
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    int length = (int)js_getnum(length_val);
    
    jsval_t saved_holder = ctx->holder;
    ctx->holder = val;
    
    for (int i = 0; i < length; i++) {
      char idxstr[32];
      snprintf(idxstr, sizeof(idxstr), "%d", i);
      jsval_t elem = js_get(js, val, idxstr);
      yyjson_mut_val *item = jsval_to_yyjson_with_key(js, doc, idxstr, elem, ctx, 1);
      if (ctx->has_cycle) { ctx->holder = saved_holder; goto done; }
      yyjson_mut_arr_add_val(arr, item);
    }
    ctx->holder = saved_holder;
    result = arr;
    goto done;
  }
  
  yyjson_mut_val *obj = yyjson_mut_obj(doc);
  int prop_count;
  prop_entry *props = collect_props(js, val, &prop_count);
  
  jsval_t saved_holder = ctx->holder;
  ctx->holder = val;
  
  for (int i = 0; i < prop_count; i++) {
    prop_entry *p = &props[i];
    
    if (!is_key_in_replacer_arr(js, ctx, p->key, p->key_len)) {
      free(p->key); continue;
    }
    
    int ptype = vtype(p->value);
    if (ptype == T_UNDEF || ptype == T_FUNC) { free(p->key); continue; }
    
    yyjson_mut_val *jval = jsval_to_yyjson_with_key(js, doc, p->key, p->value, ctx, 0);
    if (ctx->has_cycle) { free_props(props, 0, i); ctx->holder = saved_holder; goto done; }
    if (jval == YYJSON_SKIP_VALUE) { free(p->key); continue; }
    
    yyjson_mut_obj_add(obj, yyjson_mut_strncpy(doc, p->key, p->key_len), jval);
    free(p->key);
  }
  
  ctx->holder = saved_holder;
  free(props);
  result = obj;

done:
  json_cycle_pop(ctx);
  return result;
}

static yyjson_mut_val *jsval_to_yyjson_with_key(struct js *js, yyjson_mut_doc *doc, const char *key, jsval_t val, json_cycle_ctx *ctx, int in_array) {
  if (vtype(ctx->replacer_func) != T_FUNC)
    return jsval_to_yyjson_impl(js, doc, val, ctx, in_array);
  
  jsval_t key_str = js_mkstr(js, key, strlen(key));
  jsval_t call_args[2] = { key_str, val };
  jsval_t transformed = js_call(js, ctx->replacer_func, call_args, 2);
  
  if (vtype(transformed) == T_ERR) {
    ctx->has_cycle = 1;
    return NULL;
  }
  
  return jsval_to_yyjson_impl(js, doc, transformed, ctx, in_array);
}

static yyjson_mut_val *jsval_to_yyjson(struct js *js, yyjson_mut_doc *doc, jsval_t val, json_cycle_ctx *ctx) {
  return jsval_to_yyjson_with_key(js, doc, "", val, ctx, 0);
}

static jsval_t apply_reviver(struct js *js, jsval_t holder, const char *key, jsval_t reviver) {
  jsval_t val = js_get(js, holder, key);
  
  if (is_special_object(val)) {
    jsval_t len_val = js_get(js, val, "length");
    if (vtype(len_val) == T_NUM) {
      int length = (int)js_getnum(len_val);
      for (int i = 0; i < length; i++) {
        char idxstr[32];
        snprintf(idxstr, sizeof(idxstr), "%d", i);
        jsval_t new_elem = apply_reviver(js, val, idxstr, reviver);
        if (vtype(new_elem) == T_UNDEF) js_del(js, val, idxstr);
        else js_set(js, val, idxstr, new_elem);
      }
    } else {
      const char *prop_key;
      size_t prop_key_len;
      jsval_t prop_value;
      
      jsval_t keys_arr = js_mkobj(js);
      int key_count = 0;
      ant_iter_t iter = js_prop_iter_begin(js, val);
      while (js_prop_iter_next(&iter, &prop_key, &prop_key_len, &prop_value)) {
        if (is_internal_prop(prop_key, (jsoff_t)prop_key_len)) continue;
        char idxstr[32];
        snprintf(idxstr, sizeof(idxstr), "%d", key_count);
        js_set(js, keys_arr, idxstr, js_mkstr(js, prop_key, prop_key_len));
        key_count++;
      }
      js_prop_iter_end(&iter);
      
      for (int i = 0; i < key_count; i++) {
        char idxstr[32];
        snprintf(idxstr, sizeof(idxstr), "%d", i);
        jsval_t key_str = js_get(js, keys_arr, idxstr);
        size_t klen;
        char *kstr = js_getstr(js, key_str, &klen);
        jsval_t new_val = apply_reviver(js, val, kstr, reviver);
        if (vtype(new_val) == T_UNDEF) js_del(js, val, kstr);
        else js_set(js, val, kstr, new_val);
      }
    }
  }
  
  jsval_t key_str = js_mkstr(js, key, strlen(key));
  jsval_t call_args[2] = { key_str, js_get(js, holder, key) };
  
  return js_call_with_this(js, reviver, holder, call_args, 2);
}

jsval_t js_json_parse(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "JSON.parse() requires at least 1 argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "JSON.parse() argument must be a string");
  
  size_t len;
  char *json_str = js_getstr(js, args[0], &len);
  
  yyjson_doc *doc = yyjson_read(json_str, len, 0);
  if (!doc) return js_mkerr_typed(js, JS_ERR_SYNTAX, "JSON.parse: unexpected character");
  
  jsval_t result = yyjson_to_jsval(js, yyjson_doc_get_root(doc));
  yyjson_doc_free(doc);
  
  if (nargs >= 2 && vtype(args[1]) == T_FUNC) {
    jsval_t reviver = args[1];
    jsval_t root = js_mkobj(js);
    js_set(js, root, "", result);
    result = apply_reviver(js, root, "", reviver);
  }
  
  return result;
}

static yyjson_write_flag get_write_flags(jsval_t *args, int nargs) {
  if (nargs < 3) return 0;
  
  int type = vtype(args[2]);
  if (type == T_UNDEF || type == T_NULL) return 0;
  if (type != T_NUM) return YYJSON_WRITE_PRETTY;
  
  int indent = (int)js_getnum(args[2]);
  if (indent <= 0) return 0;
  if (indent == 2) return YYJSON_WRITE_PRETTY_TWO_SPACES;
  
  return YYJSON_WRITE_PRETTY;
}

jsval_t js_json_stringify(struct js *js, jsval_t *args, int nargs) {
  jsval_t result;
  yyjson_mut_doc *doc = NULL;
  json_cycle_ctx ctx = {0};
  char *json_str = NULL;
  size_t len;
  
  if (nargs < 1) return js_mkerr(js, "JSON.stringify() requires at least 1 argument");
  
  int top_type = vtype(args[0]);
  if (top_type == T_UNDEF || top_type == T_FUNC || top_type == T_SYMBOL)
    return js_mkundef();
  
  ctx.js = js;
  ctx.replacer_func = js_mkundef();
  ctx.replacer_arr = js_mkundef();
  ctx.replacer_arr_len = 0;
  ctx.holder = js_mkundef();
  
  if (nargs >= 2) {
    jsval_t replacer = args[1];
    if (vtype(replacer) == T_FUNC) {
      ctx.replacer_func = replacer;
    } else if (is_special_object(replacer)) {
      jsval_t len_val = js_get(js, replacer, "length");
      if (vtype(len_val) == T_NUM) {
        ctx.replacer_arr = replacer;
        ctx.replacer_arr_len = (int)js_getnum(len_val);
      }
    }
  }
  
  doc = yyjson_mut_doc_new(NULL);
  if (!doc) return js_mkerr(js, "JSON.stringify() failed: out of memory");
  
  yyjson_mut_val *root = jsval_to_yyjson(js, doc, args[0], &ctx);
  
  if (ctx.has_cycle) {
    result = js_mkerr_typed(js, JS_ERR_TYPE, "Converting circular structure to JSON");
    goto cleanup;
  }
  
  if (root == YYJSON_SKIP_VALUE) {
    result = js_mkundef();
    goto cleanup;
  }
  
  yyjson_mut_doc_set_root(doc, root);
  json_str = yyjson_mut_write(doc, get_write_flags(args, nargs), &len);
  
  if (!json_str) {
    result = js_mkerr(js, "JSON.stringify() failed: write error");
    goto cleanup;
  }
  
  result = js_mkstr(js, json_str, len);

cleanup:
  free(json_str);
  free(ctx.stack);
  yyjson_mut_doc_free(doc);
  return result;
}

void init_json_module() {
  struct js *js = rt->js;
  jsval_t json_obj = js_mkobj(js);
  
  js_set(js, json_obj, "parse", js_mkfun(js_json_parse));
  js_set(js, json_obj, "stringify", js_mkfun(js_json_stringify));
  
  js_set(js, json_obj, get_toStringTag_sym_key(), js_mkstr(js, "JSON", 4));
  js_set(js, js_glob(js), "JSON", json_obj);
}
