#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <yyjson.h>
#include <uthash.h>

#include "errors.h"
#include "runtime.h"
#include "internal.h"
#include "silver/engine.h"

#include "modules/json.h"
#include "modules/symbol.h"

typedef struct {
  const char *key;
  size_t key_len;
  ant_offset_t prop_off;
  UT_hash_handle hh;
} json_key_entry_t;

static ant_value_t yyjson_to_jsval(ant_t *js, yyjson_val *val) {
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
      ant_value_t arr = js_mkarr(js);
      size_t idx, max;
      yyjson_val *item;
      
      yyjson_arr_foreach(val, idx, max, item)
        js_arr_push(js, arr, yyjson_to_jsval(js, item));
      
      return arr;
    }
    
    case YYJSON_TYPE_OBJ: {
      ant_value_t obj = js_newobj(js);
      
      size_t idx, max; yyjson_val *key, *item;
      json_key_entry_t *hash = NULL, *entry, *tmp;
      
      yyjson_obj_foreach(val, idx, max, key, item) {
        const char *k = yyjson_get_str(key);
        size_t klen = yyjson_get_len(key);
        ant_value_t v = yyjson_to_jsval(js, item);
        
        HASH_FIND(hh, hash, k, klen, entry);
        if (entry) js_saveval(js, entry->prop_off, v); else {
          ant_offset_t off = js_mkprop_fast_off(js, obj, k, klen, v);
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
  ant_value_t *stack;
  int stack_size;
  int stack_cap;
  int has_cycle;
  ant_t *js;
  ant_value_t replacer_func;
  ant_value_t replacer_arr;
  int replacer_arr_len;
  ant_value_t holder;
} json_cycle_ctx;

static int json_cycle_check(json_cycle_ctx *ctx, ant_value_t val) {
  for (int i = 0; i < ctx->stack_size; i++)
    if (ctx->stack[i] == val) { ctx->has_cycle = 1; return 1; }
  return 0;
}

static void json_cycle_push(json_cycle_ctx *ctx, ant_value_t val) {
  if (ctx->stack_size >= ctx->stack_cap) {
    ctx->stack_cap = ctx->stack_cap ? ctx->stack_cap * 2 : 16;
    ctx->stack = realloc(ctx->stack, ctx->stack_cap * sizeof(ant_value_t));
  }
  ctx->stack[ctx->stack_size++] = val;
}

static inline void json_cycle_pop(json_cycle_ctx *ctx) {
  if (ctx->stack_size > 0) ctx->stack_size--;
}

static inline int key_matches(const char *a, size_t a_len, const char *b, size_t b_len) {
  return a_len == b_len && memcmp(a, b, a_len) == 0;
}

static inline bool json_is_array(ant_value_t value) {
  return vtype(value) == T_ARR;
}

static inline ant_value_t json_snapshot_keys(ant_t *js, ant_value_t value) {
  if (!is_special_object(value)) return js_mkarr(js);
  return js_for_in_keys(js, value);
}

static int is_key_in_replacer_arr(ant_t *js, json_cycle_ctx *ctx, const char *key, size_t key_len) {
  if (!is_special_object(ctx->replacer_arr)) return 1;
  
  for (int i = 0; i < ctx->replacer_arr_len; i++) {
  char idxstr[32];
  snprintf(idxstr, sizeof(idxstr), "%d", i);
  
  ant_value_t item = js_get(js, ctx->replacer_arr, idxstr);
  int type = vtype(item);
  
  if (type == T_STR) {
    size_t item_len;
    char *item_str = js_getstr(js, item, &item_len);
    if (key_matches(item_str, item_len, key, key_len)) return 1;
  } else if (type == T_NUM) {
    char numstr[32];
    snprintf(numstr, sizeof(numstr), "%.0f", js_getnum(item));
    if (key_matches(numstr, strlen(numstr), key, key_len)) return 1;
  }}
  
  return 0;
}

static yyjson_mut_val *ant_value_to_yyjson_with_key(
  ant_t *js, yyjson_mut_doc *doc, const char *key,
  ant_value_t val, json_cycle_ctx *ctx, int in_array
);
  
static ant_value_t apply_reviver(
  ant_t *js, ant_value_t holder,
  const char *key, ant_value_t reviver
);

static yyjson_mut_val *json_array_to_yyjson(
  ant_t *js, yyjson_mut_doc *doc, ant_value_t val, json_cycle_ctx *ctx
) {
  yyjson_mut_val *arr = yyjson_mut_arr(doc);
  ant_offset_t length = js_arr_len(js, val);
  ant_value_t saved_holder = ctx->holder;

  ctx->holder = val;
  for (ant_offset_t i = 0; i < length; i++) {
    char idxstr[32];
    uint_to_str(idxstr, sizeof(idxstr), (uint64_t)i);
    ant_value_t elem = js_arr_get(js, val, i);
    yyjson_mut_val *item = ant_value_to_yyjson_with_key(js, doc, idxstr, elem, ctx, 1);
    if (ctx->has_cycle) {
      ctx->holder = saved_holder;
      return NULL;
    }
    yyjson_mut_arr_add_val(arr, item);
  }

  ctx->holder = saved_holder;
  return arr;
}

static yyjson_mut_val *json_object_to_yyjson(
  ant_t *js, yyjson_mut_doc *doc, ant_value_t val, json_cycle_ctx *ctx
) {
  yyjson_mut_val *obj = yyjson_mut_obj(doc);
  ant_value_t keys = json_snapshot_keys(js, val);
  ant_value_t saved_holder = ctx->holder;

  if (is_err(keys)) {
    ctx->has_cycle = 1;
    return NULL;
  }

  ctx->holder = val;
  ant_offset_t key_count = js_arr_len(js, keys);
  
  for (ant_offset_t i = 0; i < key_count; i++) {
    ant_value_t key_val = js_arr_get(js, keys, i);
    size_t key_len = 0;
    char *key = js_getstr(js, key_val, &key_len);
    
    if (!key) continue;
    if (!is_key_in_replacer_arr(js, ctx, key, key_len)) continue;

    ant_value_t prop = js_get(js, val, key);
    int ptype = vtype(prop);
    if (ptype == T_UNDEF || ptype == T_FUNC) continue;

    yyjson_mut_val *jval = ant_value_to_yyjson_with_key(js, doc, key, prop, ctx, 0);
    if (ctx->has_cycle) {
      ctx->holder = saved_holder;
      return NULL;
    }
    
    if (jval == YYJSON_SKIP_VALUE) continue;
    yyjson_mut_obj_add(obj, yyjson_mut_strncpy(doc, key, key_len), jval);
  }

  ctx->holder = saved_holder;
  return obj;
}

static yyjson_mut_val *ant_value_to_yyjson_impl(ant_t *js, yyjson_mut_doc *doc, ant_value_t val, json_cycle_ctx *ctx, int in_array) {
  int type = vtype(val);
  yyjson_mut_val *result = NULL;
  
  if (is_special_object(val)) {
  ant_value_t toJSON = js_get(js, val, "toJSON");
  if (vtype(toJSON) == T_FUNC) {
    ant_value_t r = sv_vm_call(js->vm, js, toJSON, js_mkundef(), &val, 1, NULL, false);
    if (vtype(r) == T_ERR) { ctx->has_cycle = 1; return NULL; }
    return ant_value_to_yyjson_impl(js, doc, r, ctx, in_array);
  }}
  
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

  result = json_is_array(val)
    ? json_array_to_yyjson(js, doc, val, ctx)
    : json_object_to_yyjson(js, doc, val, ctx);

  json_cycle_pop(ctx);
  return result;
}

static yyjson_mut_val *ant_value_to_yyjson_with_key(
  ant_t *js, yyjson_mut_doc *doc, const char *key,
  ant_value_t val, json_cycle_ctx *ctx, int in_array
) {
  if (vtype(ctx->replacer_func) != T_FUNC)
    return ant_value_to_yyjson_impl(js, doc, val, ctx, in_array);
  
  ant_value_t key_str = js_mkstr(js, key, strlen(key));
  ant_value_t call_args[2] = { key_str, val };
  ant_value_t transformed = sv_vm_call(js->vm, js, ctx->replacer_func, js_mkundef(), call_args, 2, NULL, false);
  
  if (vtype(transformed) == T_ERR) {
    ctx->has_cycle = 1;
    return NULL;
  }
  
  return ant_value_to_yyjson_impl(js, doc, transformed, ctx, in_array);
}

static yyjson_mut_val *ant_value_to_yyjson(ant_t *js, yyjson_mut_doc *doc, ant_value_t val, json_cycle_ctx *ctx) {
  return ant_value_to_yyjson_with_key(js, doc, "", val, ctx, 0);
}

static ant_value_t apply_reviver_call(ant_t *js, ant_value_t holder, const char *key, ant_value_t reviver) {
  ant_value_t key_str = js_mkstr(js, key, strlen(key));
  ant_value_t call_args[2] = { key_str, js_get(js, holder, key) };
  return sv_vm_call(js->vm, js, reviver, holder, call_args, 2, NULL, false);
}

static void apply_reviver_to_array(ant_t *js, ant_value_t value, ant_value_t reviver) {
  ant_offset_t length = js_arr_len(js, value);

  for (ant_offset_t i = 0; i < length; i++) {
    char idxstr[32];
    size_t idx_len = uint_to_str(idxstr, sizeof(idxstr), (uint64_t)i);
    ant_value_t new_elem = apply_reviver(js, value, idxstr, reviver);
    if (vtype(new_elem) == T_UNDEF) js_delete_prop(js, value, idxstr, idx_len);
    else js_set(js, value, idxstr, new_elem);
  }
}

static void apply_reviver_to_object(ant_t *js, ant_value_t value, ant_value_t reviver) {
  ant_value_t keys = json_snapshot_keys(js, value);
  if (is_err(keys) || vtype(keys) != T_ARR) return;

  ant_offset_t key_count = js_arr_len(js, keys);
  for (ant_offset_t i = 0; i < key_count; i++) {
    ant_value_t key_val = js_arr_get(js, keys, i);
    size_t key_len = 0;
    char *key = js_getstr(js, key_val, &key_len);
    if (!key) continue;
    ant_value_t new_val = apply_reviver(js, value, key, reviver);
    if (vtype(new_val) == T_UNDEF) js_delete_prop(js, value, key, key_len);
    else js_set(js, value, key, new_val);
  }
}

static ant_value_t apply_reviver(ant_t *js, ant_value_t holder, const char *key, ant_value_t reviver) {
  ant_value_t val = js_get(js, holder, key);
  
  if (json_is_array(val)) apply_reviver_to_array(js, val, reviver);
  else if (vtype(val) == T_OBJ) apply_reviver_to_object(js, val, reviver);

  return apply_reviver_call(js, holder, key, reviver);
}

ant_value_t js_json_parse(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "JSON.parse() requires at least 1 argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "JSON.parse() argument must be a string");
  
  size_t len;
  char *json_str = js_getstr(js, args[0], &len);
  
  yyjson_doc *doc = yyjson_read(json_str, len, 0);
  if (!doc) return js_mkerr_typed(js, JS_ERR_SYNTAX, "JSON.parse: unexpected character");
  
  ant_value_t result = yyjson_to_jsval(js, yyjson_doc_get_root(doc));
  yyjson_doc_free(doc);
  
  if (nargs >= 2 && vtype(args[1]) == T_FUNC) {
    ant_value_t reviver = args[1];
    ant_value_t root = js_mkobj(js);
    js_set(js, root, "", result);
    result = apply_reviver(js, root, "", reviver);
  }
  
  return result;
}

ant_value_t json_parse_value(ant_t *js, ant_value_t value) {
  ant_value_t args[1] = { value };
  return js_json_parse(js, args, 1);
}

static yyjson_write_flag get_write_flags(ant_value_t *args, int nargs) {
  if (nargs < 3) return 0;
  
  int type = vtype(args[2]);
  if (type == T_UNDEF || type == T_NULL) return 0;
  if (type != T_NUM) return YYJSON_WRITE_PRETTY;
  
  int indent = (int)js_getnum(args[2]);
  if (indent <= 0) return 0;
  if (indent == 2) return YYJSON_WRITE_PRETTY_TWO_SPACES;
  
  return YYJSON_WRITE_PRETTY;
}

ant_value_t js_json_stringify(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t result;
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
    ant_value_t replacer = args[1];
    if (vtype(replacer) == T_FUNC) {
      ctx.replacer_func = replacer;
    } else if (is_special_object(replacer)) {
      ant_value_t len_val = js_get(js, replacer, "length");
      if (vtype(len_val) == T_NUM) {
        ctx.replacer_arr = replacer;
        ctx.replacer_arr_len = (int)js_getnum(len_val);
      }
    }
  }
  
  doc = yyjson_mut_doc_new(NULL);
  if (!doc) return js_mkerr(js, "JSON.stringify() failed: out of memory");
  
  yyjson_mut_val *root = ant_value_to_yyjson(js, doc, args[0], &ctx);
  
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

ant_value_t json_stringify_value(ant_t *js, ant_value_t value) {
  ant_value_t args[1] = { value };
  return js_json_stringify(js, args, 1);
}

void init_json_module() {
  ant_t *js = rt->js;
  ant_value_t json_obj = js_mkobj(js);
  
  js_set(js, json_obj, "parse", js_mkfun(js_json_parse));
  js_set(js, json_obj, "stringify", js_mkfun(js_json_stringify));
  
  js_set_sym(js, json_obj, get_toStringTag_sym(), js_mkstr(js, "JSON", 4));
  js_set(js, js_glob(js), "JSON", json_obj);
}
