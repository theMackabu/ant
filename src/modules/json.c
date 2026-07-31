#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <yyjson.h>
#include <uthash.h>

#include "gc/roots.h"
#include "utf8.h"
#include "numbers.h"
#include "errors.h"
#include "internal.h"

#include "silver/engine.h"
#include "modules/json.h"
#include "modules/symbol.h"

typedef struct {
  const char *key;
  size_t key_len;
  UT_hash_handle hh;
} json_key_entry_t;

static void json_key_hash_free(json_key_entry_t **hash) {
  json_key_entry_t *entry, *tmp;
  HASH_ITER(hh, *hash, entry, tmp) {
    HASH_DEL(*hash, entry);
    free(entry);
  }
}

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
    json_key_entry_t *hash = NULL, *entry;

    yyjson_obj_foreach(val, idx, max, key, item) {
    const char *k = yyjson_get_str(key);

    size_t klen = yyjson_get_len(key);
    ant_value_t v = yyjson_to_jsval(js, item, roots);
    if (is_err(v)) {
      json_key_hash_free(&hash);
      return v;
    }

    HASH_FIND(hh, hash, k, klen, entry);
    if (entry) {
      const char *interned = intern_string(k, klen);
      ant_prop_loc_t loc = interned ? lkp_interned(obj, interned) : ANT_PROP_LOC_NONE;

      if (loc.obj) js_prop_store(js, loc, v);
      else {
        ant_value_t key_str = js_mkstr(js, k, klen);
        if (is_err(key_str)) {
          json_key_hash_free(&hash);
          return key_str;
        }
        ant_value_t set = js_setprop(js, obj, key_str, v);
        if (is_err(set)) {
          json_key_hash_free(&hash);
          return set;
        }
      }
    } else {
      if (is_err(js_mkprop_fast(js, obj, k, klen, v))) {
        json_key_hash_free(&hash);
        return json_parse_oom(js);
      }
      entry = malloc(sizeof(json_key_entry_t));
      if (!entry) {
        json_key_hash_free(&hash);
        return json_parse_oom(js);
      }
      entry->key = k; entry->key_len = klen;
      HASH_ADD_KEYPTR(hh, hash, entry->key, entry->key_len, entry);
    }}

    json_key_hash_free(&hash);
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
  ant_value_t cycle_start;
  
  gc_temp_root_scope_t temp_roots;
  gc_temp_root_handle_t error_handle;
  gc_temp_root_handle_t holder_handle;
  
  int stack_size;
  int stack_cap;
  int replacer_arr_len;
  int has_cycle;
  char cycle_key[128];

  char indent[64];
  size_t indent_len;

  const char *tojson_key;
  ant_value_t tojson_proto;
  uint32_t tojson_epoch;
  bool tojson_absent;
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

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
  bool oom;
} json_out_t;

static bool json_out_reserve(json_out_t *o, size_t extra) {
  if (o->oom) return false;
  if (o->len + extra <= o->cap) return true;

  size_t want = o->cap ? o->cap : 256;
  while (want < o->len + extra) {
    size_t next = want * 2;
    if (next < want) { o->oom = true; return false; }
    want = next;
  }

  char *next = realloc(o->buf, want);
  if (!next) { o->oom = true; return false; }

  o->buf = next;
  o->cap = want;
  
  return true;
}

static bool json_out_write(json_out_t *o, const char *src, size_t n) {
  if (!json_out_reserve(o, n)) return false;
  memcpy(o->buf + o->len, src, n);
  o->len += n;
  return true;
}

static bool json_out_char(json_out_t *o, char c) {
  if (!json_out_reserve(o, 1)) return false;
  o->buf[o->len++] = c;
  return true;
}

static bool json_out_quoted_raw(json_out_t *o, const char *str, size_t byte_len) {
  size_t i = 0;
  while (i < byte_len) {
    unsigned char c = (unsigned char)str[i];
    if (c < 0x20 || c == '"' || c == '\\' || c >= 0x80) break;
    i++;
  }

  if (i == byte_len) {
    if (!json_out_reserve(o, byte_len + 2)) return false;
    o->buf[o->len++] = '"';
    memcpy(o->buf + o->len, str, byte_len);
    o->len += byte_len;
    o->buf[o->len++] = '"';
    return true;
  }

  size_t raw_len = 0;
  char *raw = utf8_json_quote(str, byte_len, &raw_len);
  if (!raw) { o->oom = true; return false; }

  bool ok = json_out_write(o, raw, raw_len);
  free(raw);
  
  return ok;
}

static bool json_out_quoted(ant_t *js, json_out_t *o, ant_value_t value) {
  size_t byte_len = 0;
  char *str = js_getstr(js, value, &byte_len);
  if (!str) { o->oom = true; return false; }
  return json_out_quoted_raw(o, str, byte_len);
}

static bool json_out_number(json_out_t *o, double num) {
  if (isnan(num) || isinf(num)) return json_out_write(o, "null", 4);
  if (!json_out_reserve(o, 48)) return false;
  char *p = o->buf + o->len;

  if (
    num >= -9007199254740992.0 &&
    num <= 9007199254740992.0 && 
    (double)(int64_t)num == num
  ) {
    int64_t as_int = (int64_t)num;
    uint64_t u;
    
    if (as_int < 0) {
      *p++ = '-';
      u = (uint64_t)(-(as_int + 1)) + 1u;
    } else u = (uint64_t)as_int;

    char tmp[20];
    size_t n = 0;
    do { tmp[n++] = (char)('0' + (u % 10)); u /= 10; } while (u);
    while (n) *p++ = tmp[--n];

    o->len = (size_t)(p - o->buf);
    return true;
  }

  o->len += ant_number_to_shortest(num, p, 48);
  return true;
}

static int json_cycle_check(json_cycle_ctx *ctx, ant_value_t val, const char *key) {
  for (int i = 0; i < ctx->stack_size; i++) if (ctx->stack[i] == val) {
    ctx->has_cycle = 1;
    ctx->cycle_start = val;
    snprintf(ctx->cycle_key, sizeof(ctx->cycle_key), "%s", key ? key : "");
    return 1;
  }
  return 0;
}

static ant_value_t json_cycle_error(ant_t *js, const json_cycle_ctx *ctx) {
  const char *ctor = ctx->cycle_start == js->global ? "global" : "Object";
  char message[384];
  snprintf(
    message, sizeof(message),
    "Converting circular structure to JSON\n"
    "    --> starting at object with constructor '%s'\n"
    "    --- property '%s' closes the circle",
    ctor,
    ctx->cycle_key
  );
  return js_mkerr_typed(js, JS_ERR_TYPE, "%s", message);
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

typedef enum { 
  JSON_W_OK,
  JSON_W_SKIP,
  JSON_W_ABORT 
} json_write_t;

static json_write_t json_write_with_key(
  json_cycle_ctx *ctx, json_out_t *out, const char *key,
  ant_value_t val, int in_array, int depth
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

  ant_value_t toJSON = js_mkundef();
  bool needs_generic = is_proxy(val);
  ant_prop_loc_t found = ANT_PROP_LOC_NONE;

  if (!needs_generic) {
    found = lkp_interned(val, ctx->tojson_key);

    if (!found.obj) {
      ant_value_t proto = js_get_proto(js, val);
      bool known_absent =
        ctx->tojson_proto == proto &&
        ctx->tojson_epoch == ant_ic_epoch_counter &&
        ctx->tojson_absent;

      if (!known_absent && is_object_type(proto)) {
        found = lkp_proto(js, proto, "toJSON", 6);
        ctx->tojson_proto = proto;
        ctx->tojson_epoch = ant_ic_epoch_counter;
        ctx->tojson_absent = !found.obj;
      }
    }

    if (found.obj) {
      const ant_shape_prop_t *meta = ant_shape_prop_at(found.obj->shape, found.slot);
      if (meta && (meta->has_getter || meta->has_setter)) needs_generic = true;
      else toJSON = js_prop_load(found);
    }
  }

  if (needs_generic) {
    ant_value_t generic = js_get(js, val, "toJSON");
    if (is_err(generic)) {
      json_capture_error(ctx, generic);
      return js_mkundef();
    }
    toJSON = generic;
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

static bool json_write_indent(json_cycle_ctx *ctx, json_out_t *out, int depth) {
  if (!ctx->indent_len) return true;
  if (!json_out_char(out, '\n')) return false;
  for (int i = 0; i < depth; i++)
    if (!json_out_write(out, ctx->indent, ctx->indent_len)) return false;
  return true;
}

static json_write_t json_write_array(
  json_cycle_ctx *ctx, json_out_t *out, ant_value_t val, int depth
) {
  ant_t *js = ctx->js;
  ant_offset_t length = js_arr_len(js, val);
  ant_value_t saved_holder = ctx->holder;

  if (!json_out_char(out, '[')) return JSON_W_ABORT;
  json_set_holder(ctx, val);
  bool has_replacer = is_callable(ctx->replacer_func);

  for (ant_offset_t i = 0; i < length; i++) {
    char idxstr[32];

    if (i && !json_out_char(out, ',')) goto abort;
    if (!json_write_indent(ctx, out, depth + 1)) goto abort;
    ant_value_t elem = js_arr_get(js, val, i);

    if (has_replacer || is_special_object(elem)) uint_to_str(idxstr, sizeof(idxstr), (uint64_t)i);
    else idxstr[0] = '\0';

    json_write_t w = json_write_with_key(ctx, out, idxstr, elem, 1, depth + 1);
    if (w == JSON_W_ABORT) goto abort;
    if (w == JSON_W_SKIP && !json_out_write(out, "null", 4)) goto abort;
  }

  if (length && !json_write_indent(ctx, out, depth)) goto abort;
  if (!json_out_char(out, ']')) goto abort;

  json_set_holder(ctx, saved_holder);
  return JSON_W_OK;

abort:
  json_set_holder(ctx, saved_holder);
  return JSON_W_ABORT;
}

static bool json_key_is_index(const char *key) {
  if (!key || !*key) return false;
  if (key[0] == '0') return key[1] == '\0';
  for (const char *p = key; *p; p++) if (*p < '0' || *p > '9') return false;
  return true;
}

static bool json_collect_own_keys(
  ant_t *js, ant_value_t val, const char ***out_keys, size_t *out_count,
  const char **inline_keys, size_t inline_cap
) {
  if (vtype(val) != T_OBJ || is_proxy(val)) return false;

  ant_object_t *ptr = js_obj_ptr(js_as_obj(val));
  if (!ptr || !ptr->shape || ptr->flags.is_exotic) return false;

  uint32_t count = ant_shape_count(ptr->shape);
  const char **keys = inline_keys;
  size_t n = 0;

  if (count > inline_cap) {
    keys = malloc(sizeof(*keys) * count);
    if (!keys) return false;
  }

  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING) continue;
    if (!(prop->attrs & ANT_PROP_ATTR_ENUMERABLE)) continue;

    if (json_key_is_index(prop->key.interned)) {
      if (keys != inline_keys) free(keys);
      return false;
    }
    keys[n++] = prop->key.interned;
  }

  *out_keys = keys;
  *out_count = n;
  return true;
}

static json_write_t json_write_object_fast(
  json_cycle_ctx *ctx, json_out_t *out, ant_value_t val, int depth,
  const char **keys, size_t key_count
) {
  ant_t *js = ctx->js;
  ant_value_t saved_holder = ctx->holder;
  bool wrote_any = false;

  if (!json_out_char(out, '{')) return JSON_W_ABORT;
  json_set_holder(ctx, val);

  for (size_t i = 0; i < key_count; i++) {
    const char *key = keys[i];
    size_t key_len = strlen(key);
    if (!is_key_in_replacer_arr(js, ctx, key, key_len)) continue;

    ant_prop_loc_t loc = lkp_interned(val, key);
    if (!loc.obj) continue;

    ant_value_t prop;
    const ant_shape_prop_t *meta = ant_shape_prop_at(loc.obj->shape, loc.slot);

    if (meta && (meta->has_getter || meta->has_setter)) {
      prop = js_get(js, val, key);
      if (is_err(prop)) {
        json_capture_error(ctx, prop);
        goto abort;
      }
    } else prop = js_prop_load(loc);

    if (!json_ctx_pin_value(ctx, prop)) goto abort;
    size_t mark = out->len;

    if (wrote_any && !json_out_char(out, ',')) goto abort;
    if (!json_write_indent(ctx, out, depth + 1)) goto abort;
    if (!json_out_quoted_raw(out, key, key_len)) goto abort;
    if (!json_out_char(out, ':')) goto abort;
    if (ctx->indent_len && !json_out_char(out, ' ')) goto abort;

    json_write_t w = json_write_with_key(ctx, out, key, prop, 0, depth + 1);
    if (w == JSON_W_ABORT) goto abort;
    if (w == JSON_W_SKIP) { out->len = mark; continue; }

    wrote_any = true;
  }

  if (wrote_any && !json_write_indent(ctx, out, depth)) goto abort;
  if (!json_out_char(out, '}')) goto abort;

  json_set_holder(ctx, saved_holder);
  return JSON_W_OK;

abort:
  json_set_holder(ctx, saved_holder);
  return JSON_W_ABORT;
}

static json_write_t json_write_object(
  json_cycle_ctx *ctx, json_out_t *out, ant_value_t val, int depth
) {
  ant_t *js = ctx->js;

  const char *inline_keys[24];
  const char **fast_keys = NULL;
  size_t fast_count = 0;

  if (json_collect_own_keys(js, val, &fast_keys, &fast_count, inline_keys, 24)) {
    json_write_t w = json_write_object_fast(ctx, out, val, depth, fast_keys, fast_count);
    if (fast_keys != inline_keys) free((void *)fast_keys);
    return w;
  }

  ant_value_t keys = json_snapshot_keys(js, val);
  ant_value_t saved_holder = ctx->holder;
  bool wrote_any = false;

  if (is_err(keys)) {
    json_capture_error(ctx, keys);
    return JSON_W_ABORT;
  }
  
  if (!json_ctx_pin_value(ctx, keys)) return JSON_W_ABORT;
  if (!json_out_char(out, '{')) return JSON_W_ABORT;
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
      goto abort;
    }
    if (!json_ctx_pin_value(ctx, prop)) goto abort;

    size_t mark = out->len;

    if (wrote_any && !json_out_char(out, ',')) goto abort;
    if (!json_write_indent(ctx, out, depth + 1)) goto abort;
    if (!json_out_quoted_raw(out, key, key_len)) goto abort;
    if (!json_out_char(out, ':')) goto abort;
    if (ctx->indent_len && !json_out_char(out, ' ')) goto abort;

    json_write_t w = json_write_with_key(ctx, out, key, prop, 0, depth + 1);
    if (w == JSON_W_ABORT) goto abort;
    if (w == JSON_W_SKIP) { out->len = mark; continue; }

    wrote_any = true;
  }

  if (wrote_any && !json_write_indent(ctx, out, depth)) goto abort;
  if (!json_out_char(out, '}')) goto abort;

  json_set_holder(ctx, saved_holder);
  return JSON_W_OK;

abort:
  json_set_holder(ctx, saved_holder);
  return JSON_W_ABORT;
}

static json_write_t json_write_impl(
  json_cycle_ctx *ctx, json_out_t *out, const char *key,
  ant_value_t val, int in_array, int depth
) {
  json_write_t result;

  switch (vtype(val)) {
    case T_NULL: return json_out_write(out, "null", 4) ? JSON_W_OK : JSON_W_ABORT;
    case T_BOOL:
      return json_out_write(out, val == js_true ? "true" : "false", val == js_true ? 4 : 5)
        ? JSON_W_OK : JSON_W_ABORT;

    case T_UNDEF:
    case T_FUNC:
    case T_CFUNC:
    case T_SYMBOL:
      return in_array ? (json_out_write(out, "null", 4) ? JSON_W_OK : JSON_W_ABORT) : JSON_W_SKIP;

    case T_BIGINT:
      json_capture_error(ctx, js_mkerr_typed(ctx->js, JS_ERR_TYPE,"Do not know how to serialize a BigInt"));
      return JSON_W_ABORT;

    case T_NUM: return json_out_number(out, js_getnum(val)) ? JSON_W_OK : JSON_W_ABORT;
    case T_STR: return json_out_quoted(ctx->js, out, val) ? JSON_W_OK : JSON_W_ABORT;

    case T_OBJ:
    case T_ARR: break;
    default: return json_out_write(out, "null", 4) ? JSON_W_OK : JSON_W_ABORT;
  }

  if (vtype(val) == T_OBJ) {
    ant_value_t prim = js_get_slot(val, SLOT_PRIMITIVE);
    switch (vtype(prim)) {
      case T_NUM:  return json_out_number(out, js_getnum(prim)) ? JSON_W_OK : JSON_W_ABORT;
      case T_STR:  return json_out_quoted(ctx->js, out, prim) ? JSON_W_OK : JSON_W_ABORT;
      case T_BOOL: return json_out_write(out, prim == js_true ? "true" : "false", prim == js_true ? 4 : 5) ? JSON_W_OK : JSON_W_ABORT;

      case T_BIGINT: 
        json_capture_error(ctx, js_mkerr_typed(ctx->js, JS_ERR_TYPE, "Do not know how to serialize a BigInt"));
        return JSON_W_ABORT;

      default: break;
    }
  }

  if (depth >= 2000) {
    json_capture_error(ctx, js_mkerr_typed(ctx->js, JS_ERR_RANGE, "Maximum call stack size exceeded"));
    return JSON_W_ABORT;
  }

  if (json_cycle_check(ctx, val, key)) return JSON_W_ABORT;
  json_cycle_push(ctx, val);

  result = is_array_value(val)
    ? json_write_array(ctx, out, val, depth)
    : json_write_object(ctx, out, val, depth);

  json_cycle_pop(ctx);
  return result;
}

static json_write_t json_write_with_key(
  json_cycle_ctx *ctx, json_out_t *out, const char *key,
  ant_value_t val, int in_array, int depth
) {
  val = json_apply_tojson(ctx->js, key, val, ctx);
  if (json_has_abort(ctx)) return JSON_W_ABORT;

  val = json_apply_replacer(ctx->js, key, val, ctx);
  if (json_has_abort(ctx)) return JSON_W_ABORT;

  return json_write_impl(ctx, out, key, val, in_array, depth);
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

static bool json_set_indent(ant_t *js, json_cycle_ctx *ctx, ant_value_t *args, int nargs) {
  ctx->indent_len = 0;
  if (nargs < 3) return true;
  ant_value_t space = args[2];

  if (is_special_object(space)) {
    uint8_t boxed = vtype(js_get_slot(space, SLOT_PRIMITIVE));
    if (boxed != T_NUM && boxed != T_STR) return true;

    ant_value_t prim = boxed == T_NUM
      ? js_to_primitive(js, space, 0)
      : js_tostring_val(js, space);

    if (is_err(prim) || js->thrown_exists) {
      json_capture_error(ctx, prim);
      return false;
    }
    space = prim;
  }

  if (vtype(space) == T_NUM) {
    double d = js_getnum(space);
    if (isnan(d) || d < 1) return true;
    size_t n = d > 10 ? 10 : (size_t)d;
    memset(ctx->indent, ' ', n);
    ctx->indent_len = n;
    return true;
  }

  if (vtype(space) != T_STR) return true;

  size_t byte_len = 0;
  char *str = js_getstr(js, space, &byte_len);
  if (!str || !byte_len) return true;

  size_t units = utf16_strlen(str, byte_len);
  size_t take = byte_len;

  if (units > 10) {
    size_t char_bytes = 0;
    int off = utf16_index_to_byte_offset(str, byte_len, 10, &char_bytes);
    if (off >= 0) take = (size_t)off;
  }
  if (take > sizeof(ctx->indent)) take = sizeof(ctx->indent);

  memcpy(ctx->indent, str, take);
  ctx->indent_len = take;
  
  return true;
}

ant_value_t js_json_stringify(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t result;
  json_out_t out = {0};
  
  json_cycle_ctx ctx = {
    .js = js,
    .replacer_func = js_mkundef(),
    .replacer_arr = js_mkundef(),
    .error = js_mkundef(),
    .holder = js_mkundef(),
  };
  
  ant_value_t root_holder = js_mkundef();
  if (nargs < 1) return js_mkerr(js, "JSON.stringify() requires at least 1 argument");
  
  ctx.tojson_key = intern_string("toJSON", 6);
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
  
  if (!json_set_indent(js, &ctx, args, nargs)) {
    ant_value_t error = json_normalize_error(ctx.error);
    result = is_err(error) ? error : js_throw(js, error);
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
  json_write_t root = json_write_with_key(&ctx, &out, "", args[0], 0, 0);
  
  if (vtype(ctx.error) != T_UNDEF) {
    ant_value_t error = json_normalize_error(ctx.error);
    result = is_err(error) ? error : js_throw(js, error);
    goto cleanup;
  }

  if (ctx.has_cycle) {
    result = json_cycle_error(js, &ctx);
    goto cleanup;
  }
  
  if (out.oom) {
    result = json_stringify_oom(js);
    goto cleanup;
  }

  if (root == JSON_W_SKIP) {
    result = js_mkundef();
    goto cleanup;
  }

  result = js_mkstr(js, out.buf, out.len);

cleanup:
  free(out.buf);
  free(ctx.stack);
  gc_temp_root_scope_end(&ctx.temp_roots);
  return result;
}

ant_value_t json_stringify_value(ant_t *js, ant_value_t value) {
  ant_value_t args[1] = { value };
  return js_json_stringify(js, args, 1);
}

void init_json_module(ant_t *js) {
  ant_value_t json_obj = js_mkobj(js);
  
  js_set(js, json_obj, "parse", js_mkfun(js_json_parse));
  js_set(js, json_obj, "stringify", js_mkfun(js_json_stringify));
  
  js_set_sym(js, json_obj, get_toStringTag_sym(), js_mkstr(js, "JSON", 4));
  js_set(js, js_glob(js), "JSON", json_obj);
}
