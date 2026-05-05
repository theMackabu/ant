#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <yyjson.h>
#include <uthash.h>

#include "gc/roots.h"
#include "utf8.h"
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

static inline bool json_value_needs_temp_root(ant_value_t value) {
  if (value <= NANBOX_PREFIX) return false;
  
  static const uint32_t mask =
    (1u << T_STR) | (1u << T_OBJ) | (1u << T_ARR) | (1u << T_FUNC) |
    (1u << T_PROMISE) | (1u << T_GENERATOR) | (1u << T_SYMBOL) | (1u << T_BIGINT);
    
  uint8_t t = vtype(value);
  return t < 32 && (mask >> t) & 1;
}

static inline bool json_temp_pin(gc_temp_root_scope_t *roots, ant_value_t value) {
  if (!json_value_needs_temp_root(value)) return true;
  return gc_temp_root_handle_valid(gc_temp_root_add(roots, value));
}

static inline ant_value_t json_parse_oom(ant_t *js) {
  return js_mkerr(js, "JSON.parse() failed: out of memory");
}

static inline ant_value_t json_stringify_oom(ant_t *js) {
  return js_mkerr(js, "JSON.stringify() failed: out of memory");
}

static ant_value_t yyjson_to_jsval(ant_t *js, yyjson_val *val, gc_temp_root_scope_t *roots) {
  if (!val) return js_mkundef();
  
  switch (yyjson_get_type(val)) {
  case YYJSON_TYPE_NULL: return js_mknull();
  case YYJSON_TYPE_BOOL: return js_bool(yyjson_get_bool(val));
  
  case YYJSON_TYPE_STR: {
    ant_value_t str = js_mkstr(js, yyjson_get_str(val), yyjson_get_len(val));
    if (is_err(str)) return str;
    if (!json_temp_pin(roots, str)) return json_parse_oom(js);
    return str;
  }
  
  case YYJSON_TYPE_NUM: {
    if (yyjson_is_sint(val)) return js_mknum((double)yyjson_get_sint(val));
    if (yyjson_is_uint(val)) return js_mknum((double)yyjson_get_uint(val));
    return js_mknum(yyjson_get_real(val));
  }
  
  case YYJSON_TYPE_ARR: {
    ant_value_t arr = js_mkarr(js);
    if (is_err(arr)) return arr;
    if (!json_temp_pin(roots, arr)) return json_parse_oom(js);
    size_t idx, max;
    yyjson_val *item;
    
    yyjson_arr_foreach(val, idx, max, item) {
      ant_value_t elem = yyjson_to_jsval(js, item, roots);
      if (is_err(elem)) return elem;
      js_arr_push(js, arr, elem);
    }
    
    return arr;
  }
  
  case YYJSON_TYPE_OBJ: {
    ant_value_t obj = js_newobj(js);
    if (is_err(obj)) return obj;
    if (!json_temp_pin(roots, obj)) return json_parse_oom(js);
    
    size_t idx, max; yyjson_val *key, *item;
    json_key_entry_t *hash = NULL, *entry, *tmp;
    
    yyjson_obj_foreach(val, idx, max, key, item) {
    const char *k = yyjson_get_str(key);
    
    size_t klen = yyjson_get_len(key);
    ant_value_t v = yyjson_to_jsval(js, item, roots);
    if (is_err(v)) {
      HASH_ITER(hh, hash, entry, tmp)
        HASH_DEL(hash, entry); free(entry);
      return v;
    }
    
    HASH_FIND(hh, hash, k, klen, entry);
    if (entry) js_saveval(js, entry->prop_off, v); else {
      ant_offset_t off = js_mkprop_fast_off(js, obj, k, klen, v);
      if (off == 0) {
        HASH_ITER(hh, hash, entry, tmp) 
          HASH_DEL(hash, entry); free(entry);
        return json_parse_oom(js);
      }
      entry = malloc(sizeof(json_key_entry_t));
      if (!entry) {
        HASH_ITER(hh, hash, entry, tmp)
          HASH_DEL(hash, entry); free(entry);
        return json_parse_oom(js);
      }
      entry->key = k; entry->key_len = klen; entry->prop_off = off;
      HASH_ADD_KEYPTR(hh, hash, entry->key, entry->key_len, entry);
    }}
    
    HASH_ITER(hh, hash, entry, tmp)
      HASH_DEL(hash, entry); free(entry);
      
    return obj;
  }
  
  default: return js_mkundef(); }
}

typedef struct {
  ant_t *js;
  ant_value_t *stack;
  ant_value_t replacer_func;
  ant_value_t replacer_arr;
  ant_value_t error;
  ant_value_t holder;
  
  gc_temp_root_scope_t temp_roots;
  gc_temp_root_handle_t error_handle;
  gc_temp_root_handle_t holder_handle;
  
  int stack_size;
  int stack_cap;
  int replacer_arr_len;
  int has_cycle;
} json_cycle_ctx;

static inline bool json_has_abort(json_cycle_ctx *ctx) {
  return ctx->has_cycle || vtype(ctx->error) != T_UNDEF;
}

static inline ant_value_t json_normalize_error(ant_value_t value) {
  if (is_err(value) && vdata(value) != 0) return js_as_obj(value);
  return value;
}

static void json_set_error(json_cycle_ctx *ctx, ant_value_t value) {
  ctx->error = value;
  gc_temp_root_set(ctx->error_handle, value);
}

static inline bool json_ctx_pin_value(json_cycle_ctx *ctx, ant_value_t value) {
  if (json_temp_pin(&ctx->temp_roots, value)) return true;
  json_set_error(ctx, json_stringify_oom(ctx->js));
  return false;
}

static inline void json_set_holder(json_cycle_ctx *ctx, ant_value_t value) {
  ctx->holder = value;
  gc_temp_root_set(ctx->holder_handle, value);
}

static void json_capture_error(json_cycle_ctx *ctx, ant_value_t value) {
  if (vtype(ctx->error) != T_UNDEF) return;
  if (ctx->js->thrown_exists) {
    json_set_error(ctx, ctx->js->thrown_value);
    ctx->js->thrown_exists = false;
    ctx->js->thrown_value = js_mkundef();
    return;
  }
  json_set_error(ctx, json_normalize_error(value));
}

static yyjson_mut_val *json_string_to_yyjson(ant_t *js, yyjson_mut_doc *doc, ant_value_t value) {
  size_t byte_len = 0;
  char *str = js_getstr(js, value, &byte_len);
  size_t raw_len = 0;
  char *raw = utf8_json_quote(str, byte_len, &raw_len);
  if (!raw) goto oom;
  yyjson_mut_val *out = yyjson_mut_rawncpy(doc, raw, raw_len);
  free(raw);
  return out;

oom:
  free(raw);
  return NULL;
}

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
  const char *key, ant_value_t reviver,
  gc_temp_root_scope_t *roots
);

static ant_value_t json_apply_tojson(
  ant_t *js,
  const char *key,
  ant_value_t val,
  json_cycle_ctx *ctx
) {
  if (!is_special_object(val)) return val;
  ant_value_t toJSON = js_get(js, val, "toJSON");
  
  if (is_err(toJSON)) {
    json_capture_error(ctx, toJSON);
    return js_mkundef();
  }
  
  if (!is_callable(toJSON)) return val;
  ant_value_t key_arg = js_mkstr(js, key, strlen(key));
  if (is_err(key_arg)) {
    json_capture_error(ctx, key_arg);
    return js_mkundef();
  }
  
  if (!json_ctx_pin_value(ctx, key_arg)) return js_mkundef();
  ant_value_t args[1] = { key_arg };
  
  ant_value_t transformed = sv_vm_call(
    js->vm, js,
    toJSON, val,
    args, 1, NULL, false
  );
  
  if (is_err(transformed)) {
    json_capture_error(ctx, transformed);
    return js_mkundef();
  }
  if (!json_ctx_pin_value(ctx, transformed)) return js_mkundef();

  return transformed;
}

static ant_value_t json_apply_replacer(
  ant_t *js,
  const char *key,
  ant_value_t val,
  json_cycle_ctx *ctx
) {
  if (!is_callable(ctx->replacer_func)) return val;
  ant_value_t key_arg = js_mkstr(js, key, strlen(key));
  if (is_err(key_arg)) {
    json_capture_error(ctx, key_arg);
    return js_mkundef();
  }
  if (!json_ctx_pin_value(ctx, key_arg)) return js_mkundef();
  ant_value_t args[2] = { key_arg, val };
  
  ant_value_t transformed = sv_vm_call(
    js->vm, js, 
    ctx->replacer_func, ctx->holder, 
    args, 2, NULL, false
  );
  
  if (is_err(transformed)) {
    json_capture_error(ctx, transformed);
    return js_mkundef();
  }
  if (!json_ctx_pin_value(ctx, transformed)) return js_mkundef();

  return transformed;
}

static inline ant_value_t json_create_root_holder(ant_t *js, ant_value_t value, json_cycle_ctx *ctx) {
  ant_value_t holder = js_mkobj(js);
  if (is_err(holder)) return holder;
  if (!json_ctx_pin_value(ctx, holder)) return js_mkundef();
  js_set(js, holder, "", value);
  return holder;
}

static yyjson_mut_val *json_array_to_yyjson(
  ant_t *js, yyjson_mut_doc *doc, ant_value_t val, json_cycle_ctx *ctx
) {
  yyjson_mut_val *arr = yyjson_mut_arr(doc);
  ant_offset_t length = js_arr_len(js, val);
  ant_value_t saved_holder = ctx->holder;

  json_set_holder(ctx, val);
  for (ant_offset_t i = 0; i < length; i++) {
    char idxstr[32];
    uint_to_str(idxstr, sizeof(idxstr), (uint64_t)i);
    ant_value_t elem = js_arr_get(js, val, i);
    yyjson_mut_val *item = ant_value_to_yyjson_with_key(js, doc, idxstr, elem, ctx, 1);
    if (json_has_abort(ctx)) {
      json_set_holder(ctx, saved_holder);
      return NULL;
    }
    yyjson_mut_arr_add_val(arr, item);
  }

  json_set_holder(ctx, saved_holder);
  return arr;
}

static yyjson_mut_val *json_object_to_yyjson(
  ant_t *js, yyjson_mut_doc *doc, ant_value_t val, json_cycle_ctx *ctx
) {
  yyjson_mut_val *obj = yyjson_mut_obj(doc);
  ant_value_t keys = json_snapshot_keys(js, val);
  ant_value_t saved_holder = ctx->holder;

  if (is_err(keys)) {
    json_capture_error(ctx, keys);
    return NULL;
  }
  if (!json_ctx_pin_value(ctx, keys)) return NULL;

  json_set_holder(ctx, val);
  ant_offset_t key_count = js_arr_len(js, keys);
  
  for (ant_offset_t i = 0; i < key_count; i++) {
    ant_value_t key_val = js_arr_get(js, keys, i);
    size_t key_len = 0;
    char *key = js_getstr(js, key_val, &key_len);
    
    if (!key) continue;
    if (!is_key_in_replacer_arr(js, ctx, key, key_len)) continue;

    ant_value_t prop = js_get(js, val, key);
    if (is_err(prop)) {
      json_capture_error(ctx, prop);
      json_set_holder(ctx, saved_holder);
      return NULL;
    }
    
    yyjson_mut_val *jval = ant_value_to_yyjson_with_key(js, doc, key, prop, ctx, 0);
    if (json_has_abort(ctx)) {
      json_set_holder(ctx, saved_holder);
      return NULL;
    }
    
    if (jval == YYJSON_SKIP_VALUE) continue;
    yyjson_mut_obj_add(obj, yyjson_mut_strncpy(doc, key, key_len), jval);
  }

  json_set_holder(ctx, saved_holder);
  return obj;
}

static yyjson_mut_val *ant_value_to_yyjson_impl(ant_t *js, yyjson_mut_doc *doc, ant_value_t val, json_cycle_ctx *ctx, int in_array) {
  int type = vtype(val);
  yyjson_mut_val *result = NULL;
  
  switch (type) {
    case T_NULL:   return yyjson_mut_null(doc);
    case T_BOOL:   return yyjson_mut_bool(doc, val == js_true);
    
    case T_UNDEF:  return in_array ? yyjson_mut_null(doc) : YYJSON_SKIP_VALUE;
    case T_FUNC:   return in_array ? yyjson_mut_null(doc) : YYJSON_SKIP_VALUE;
    case T_SYMBOL: return in_array ? yyjson_mut_null(doc) : YYJSON_SKIP_VALUE;
    
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
      return json_string_to_yyjson(js, doc, val);
    }
    
    case T_OBJ:
    case T_ARR: break;
    default: return yyjson_mut_null(doc);
  }
  
  if (json_cycle_check(ctx, val)) return NULL;
  json_cycle_push(ctx, val);

  result = is_array_value(val)
    ? json_array_to_yyjson(js, doc, val, ctx)
    : json_object_to_yyjson(js, doc, val, ctx);

  json_cycle_pop(ctx);
  return result;
}

static yyjson_mut_val *ant_value_to_yyjson_with_key(
  ant_t *js, yyjson_mut_doc *doc, const char *key,
  ant_value_t val, json_cycle_ctx *ctx, int in_array
) {
  val = json_apply_tojson(js, key, val, ctx);
  if (json_has_abort(ctx)) return NULL;

  val = json_apply_replacer(js, key, val, ctx);
  if (json_has_abort(ctx)) return NULL;

  return ant_value_to_yyjson_impl(js, doc, val, ctx, in_array);
}

static yyjson_mut_val *ant_value_to_yyjson(ant_t *js, yyjson_mut_doc *doc, ant_value_t val, json_cycle_ctx *ctx) {
  return ant_value_to_yyjson_with_key(js, doc, "", val, ctx, 0);
}

static ant_value_t apply_reviver_call(
  ant_t *js,
  ant_value_t holder,
  const char *key,
  ant_value_t reviver,
  gc_temp_root_scope_t *roots
) {
  ant_value_t key_str = js_mkstr(js, key, strlen(key));
  if (is_err(key_str)) return key_str;
  if (!json_temp_pin(roots, key_str)) return json_parse_oom(js);
  ant_value_t current_value = js_get(js, holder, key);
  ant_value_t call_args[2] = { key_str, current_value };
  
  ant_value_t result = sv_vm_call(
    js->vm, js, reviver, holder,
    call_args, 2, NULL, false
  );
  if (!is_err(result) && !json_temp_pin(roots, result)) return json_parse_oom(js);
  
  return result;
}

static void apply_reviver_to_array(
  ant_t *js,
  ant_value_t value,
  ant_value_t reviver,
  gc_temp_root_scope_t *roots
) {
  ant_offset_t length = js_arr_len(js, value);

  for (ant_offset_t i = 0; i < length; i++) {
  char idxstr[32];
  size_t idx_len = uint_to_str(idxstr, sizeof(idxstr), (uint64_t)i);
  ant_value_t new_elem = apply_reviver(js, value, idxstr, reviver, roots);
  if (vtype(new_elem) == T_UNDEF) js_delete_prop(js, value, idxstr, idx_len);
  else {
    ant_value_t key_val = js_mkstr(js, idxstr, idx_len);
    if (is_err(key_val)) return;
    if (!json_temp_pin(roots, key_val)) return;
    js_setprop(js, value, key_val, new_elem);
  }}
}

static void apply_reviver_to_object(
  ant_t *js,
  ant_value_t value,
  ant_value_t reviver,
  gc_temp_root_scope_t *roots
) {
  ant_value_t keys = json_snapshot_keys(js, value);
  if (is_err(keys) || vtype(keys) != T_ARR) return;
  if (!json_temp_pin(roots, keys)) return;

  ant_offset_t key_count = js_arr_len(js, keys);
  for (ant_offset_t i = 0; i < key_count; i++) {
    ant_value_t key_val = js_arr_get(js, keys, i);
    size_t key_len = 0;
    char *key = js_getstr(js, key_val, &key_len);
    if (!key) continue;
    ant_value_t new_val = apply_reviver(js, value, key, reviver, roots);
    if (vtype(new_val) == T_UNDEF) js_delete_prop(js, value, key, key_len);
    else js_set(js, value, key, new_val);
  }
}

static ant_value_t apply_reviver(
  ant_t *js,
  ant_value_t holder,
  const char *key,
  ant_value_t reviver,
  gc_temp_root_scope_t *roots
) {
  ant_value_t val = js_get(js, holder, key);
  
  if (is_array_value(val)) apply_reviver_to_array(js, val, reviver, roots);
  else if (vtype(val) == T_OBJ) apply_reviver_to_object(js, val, reviver, roots);

  return apply_reviver_call(js, holder, key, reviver, roots);
}

ant_value_t js_json_parse(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "JSON.parse() requires at least 1 argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "JSON.parse() argument must be a string");
  gc_temp_root_scope_t temp_roots;
  gc_temp_root_scope_begin(js, &temp_roots);
  
  size_t len;
  char *json_str = js_getstr(js, args[0], &len);
  
  yyjson_doc *doc = yyjson_read(json_str, len, 0);
  
  if (!doc) {
    gc_temp_root_scope_end(&temp_roots);
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "JSON.parse: unexpected character");
  }
  
  ant_value_t result = yyjson_to_jsval(js, yyjson_doc_get_root(doc), &temp_roots);
  yyjson_doc_free(doc);
  if (is_err(result)) {
    gc_temp_root_scope_end(&temp_roots);
    return result;
  }
  
  if (nargs >= 2 && is_callable(args[1])) {
    ant_value_t reviver = args[1];
    if (!json_temp_pin(&temp_roots, reviver)) {
      gc_temp_root_scope_end(&temp_roots);
      return json_parse_oom(js);
    }
    ant_value_t root = js_mkobj(js);
    if (is_err(root)) {
      gc_temp_root_scope_end(&temp_roots);
      return root;
    }
    if (!json_temp_pin(&temp_roots, root)) {
      gc_temp_root_scope_end(&temp_roots);
      return json_parse_oom(js);
    }
    js_set(js, root, "", result);
    result = apply_reviver(js, root, "", reviver, &temp_roots);
  }
  
  gc_temp_root_scope_end(&temp_roots);
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
  
  json_cycle_ctx ctx = {
    .js = js,
    .replacer_func = js_mkundef(),
    .replacer_arr = js_mkundef(),
    .error = js_mkundef(),
    .holder = js_mkundef(),
  };
  
  char *json_str = NULL;
  size_t len;
  ant_value_t root_holder = js_mkundef();
  
  if (nargs < 1) return js_mkerr(js, "JSON.stringify() requires at least 1 argument");
  gc_temp_root_scope_begin(js, &ctx.temp_roots);
  ctx.error_handle = gc_temp_root_add(&ctx.temp_roots, ctx.error);
  ctx.holder_handle = gc_temp_root_add(&ctx.temp_roots, ctx.holder);
  
  if (!gc_temp_root_handle_valid(ctx.error_handle) || !gc_temp_root_handle_valid(ctx.holder_handle)) {
    gc_temp_root_scope_end(&ctx.temp_roots);
    return json_stringify_oom(js);
  }
  
  if (!json_ctx_pin_value(&ctx, args[0])) {
    result = ctx.error;
    goto cleanup;
  }
  
  int top_type = vtype(args[0]);
  
  if (nargs < 2 && top_type == T_STR) {
    size_t byte_len = 0;
    size_t raw_len = 0;
    
    char *str = js_getstr(js, args[0], &byte_len);
    char *raw = utf8_json_quote(str, byte_len, &raw_len);
    
    if (!raw) {
      result = js_mkerr(js, "JSON.stringify() failed: out of memory");
      goto cleanup;
    }
    result = js_mkstr(js, raw, raw_len);
    free(raw);
    goto cleanup;
  }
  
  if (nargs >= 2) {
  ant_value_t replacer = args[1];
  if (is_callable(replacer)) {
  ctx.replacer_func = replacer;
  if (!json_ctx_pin_value(&ctx, replacer)) {
    result = ctx.error;
    goto cleanup;
  }}
  
  else if (is_special_object(replacer)) {
  ant_value_t len_val = js_get(js, replacer, "length");
  
  if (vtype(len_val) == T_NUM) {
    ctx.replacer_arr = replacer;
    ctx.replacer_arr_len = (int)js_getnum(len_val);
    if (!json_ctx_pin_value(&ctx, replacer)) {
      result = ctx.error;
      goto cleanup;
    }
  }}} 
  
  doc = yyjson_mut_doc_new(NULL);
  if (!doc) {
    result = js_mkerr(js, "JSON.stringify() failed: out of memory");
    goto cleanup;
  }

  root_holder = json_create_root_holder(js, args[0], &ctx);
  if (is_err(root_holder)) {
    result = root_holder;
    goto cleanup;
  }
  
  if (vtype(root_holder) == T_UNDEF && vtype(ctx.error) != T_UNDEF) {
    result = ctx.error;
    goto cleanup;
  }
  
  json_set_holder(&ctx, root_holder);
  yyjson_mut_val *root = ant_value_to_yyjson(js, doc, args[0], &ctx);
  
  if (vtype(ctx.error) != T_UNDEF) {
    ant_value_t error = json_normalize_error(ctx.error);
    result = is_err(error) ? error : js_throw(js, error);
    goto cleanup;
  }

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
  gc_temp_root_scope_end(&ctx.temp_roots);
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
