#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <yyjson.h>

#include "cfunc.h"
#include "utf8.h"
#include "numbers.h"
#include "hash.h"
#include "errors.h"
#include "internal.h"
#include "keys.h"
#include "arrays.h"
#include "gc/roots.h"
#include "silver/call.h"
#include "modules/json.h"

static inline bool json_value_needs_temp_root(ant_value_t value) {
  if (!is_tagged(value)) return false;
  
  static const uint32_t mask =
    (1u << kTypeString) | (1u << kTypeObject) | (1u << kTypeArray) | (1u << kTypeFunction) |
    (1u << kTypePromise) | (1u << kTypeGenerator) | (1u << kTypeSymbol) | (1u << kTypeBigInt);
    
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

static inline bool json_read_hex4(const char *p, uint32_t *out) {
  uint32_t value = 0;
  
  for (int k = 0; k < 4; k++) {
    unsigned char c = (unsigned char)p[k];
    uint32_t digit;
    if (c >= '0' && c <= '9') digit = c - '0';
    else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') digit = (c | 0x20) - 'a' + 10;
    else return false;
    value = (value << 4) | digit;
  }
  
  *out = value;
  return true;
}

/*
 * yyjson built standard-only rejects lone surrogate escapes
 * (\uD800..\uDFFF without a partner) and raw WTF-8 surrogate bytes, both of
 * which JSON.parse must accept. When a parse fails on an invalid string, the
 * text is rewritten so each lone surrogate becomes a U+FFFF marker plus four
 * hex digits (a genuine U+FFFF is doubled), parsed again in standard mode, and
 * the markers are decoded back into WTF-8 inside the document's own strings.
 * Valid input never takes this path.
 */
#define JSON_MARK "\xEF\xBF\xBF"

static constexpr uint8_t JSON_MARK_LEN = 3;
static constexpr uint8_t JSON_PLAN_SLOTS = 16;

static inline bool json_is_mark(const char *p) {
  return (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBF && (unsigned char)p[2] == 0xBF;
}

static inline size_t json_write_marked(char *dst, uint32_t cu) {
  static const char hex[] = "0123456789abcdef";
  memcpy(dst, JSON_MARK, JSON_MARK_LEN);
  dst[3] = hex[(cu >> 12) & 15];
  dst[4] = hex[(cu >> 8) & 15];
  dst[5] = hex[(cu >> 4) & 15];
  dst[6] = hex[cu & 15];
  return 7;
}

static inline size_t json_write_wtf8(char *dst, uint32_t cu) {
  dst[0] = (char)(0xE0 | (cu >> 12));
  dst[1] = (char)(0x80 | ((cu >> 6) & 0x3F));
  dst[2] = (char)(0x80 | (cu & 0x3F));
  return 3;
}

static char *json_surrogate_rewrite(const char *src, size_t len, size_t *out_len) {
  if (len > SIZE_MAX / 3 - 1) return NULL;
  char *dst = malloc(len * 3 + 1);
  if (!dst) return NULL;

  size_t i = 0, n = 0;
  bool in_str = false;

  while (i < len) {
    unsigned char c = (unsigned char)src[i];

    if (!in_str || c == '"') {
      if (c == '"') in_str = !in_str;
      dst[n++] = (char)c;
      i++;
      continue;
    }

    if (c == 0xED && i + 3 <= len) {
      unsigned char c1 = (unsigned char)src[i + 1], c2 = (unsigned char)src[i + 2];
      if (c1 >= 0xA0 && c1 <= 0xBF && (c2 & 0xC0) == 0x80) {
        n += json_write_marked(dst + n, ((c & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F));
        i += 3;
        continue;
      }
    }

    if (c == 0xEF && i + 3 <= len && json_is_mark(src + i)) {
      memcpy(dst + n, JSON_MARK JSON_MARK, 2 * JSON_MARK_LEN);
      n += 2 * JSON_MARK_LEN;
      i += 3;
      continue;
    }

    if (c != '\\') {
      dst[n++] = (char)c;
      i++;
      continue;
    }

    uint32_t hi, lo;
    if (i + 6 > len || src[i + 1] != 'u' || !json_read_hex4(src + i + 2, &hi)) {
      size_t take = i + 2 <= len ? 2 : 1;
      memcpy(dst + n, src + i, take);
      n += take;
      i += take;
      continue;
    }

    if (hi == 0xFFFF) {
      memcpy(dst + n, JSON_MARK JSON_MARK, 2 * JSON_MARK_LEN);
      n += 2 * JSON_MARK_LEN;
      i += 6;
      continue;
    }

    if ((hi & 0xF800) != 0xD800) {
      memcpy(dst + n, src + i, 6);
      n += 6;
      i += 6;
      continue;
    }

    if (
      hi <= 0xDBFF && i + 12 <= len && src[i + 6] == '\\' && src[i + 7] == 'u' &&
      json_read_hex4(src + i + 8, &lo) && (lo & 0xFC00) == 0xDC00
    ) {
      memcpy(dst + n, src + i, 12);
      n += 12;
      i += 12;
      continue;
    }

    n += json_write_marked(dst + n, hi);
    i += 6;
  }

  *out_len = n;
  return dst;
}

static void json_surrogate_decode(yyjson_doc *doc) {
  yyjson_val *vals = yyjson_doc_get_root(doc);
  size_t count = yyjson_doc_get_val_count(doc);

  for (size_t k = 0; k < count; k++) {
    yyjson_val *val = vals + k;
    if (!yyjson_is_str(val)) continue;

    size_t len = yyjson_get_len(val);
    char *str = (char *)yyjson_get_str(val);
    size_t r = 0;
    
    while (r + JSON_MARK_LEN <= len && !json_is_mark(str + r)) r++;
    if (r + JSON_MARK_LEN > len) continue;

    size_t w = r;
    while (r < len) {
      if (len - r >= 2 * JSON_MARK_LEN && json_is_mark(str + r) && json_is_mark(str + r + JSON_MARK_LEN)) {
        memcpy(str + w, JSON_MARK, JSON_MARK_LEN);
        w += JSON_MARK_LEN;
        r += 2 * JSON_MARK_LEN;
        continue;
      }
      
      uint32_t cu;
      if (len - r >= 7 && json_is_mark(str + r) && json_read_hex4(str + r + JSON_MARK_LEN, &cu)) {
        w += json_write_wtf8(str + w, cu);
        r += 7;
        continue;
      }
      
      str[w++] = str[r++];
    }

    str[w] = '\0';
    yyjson_set_strn(val, str, w);
  }
}

static yyjson_doc *json_read_doc(const char *str, size_t len, const yyjson_alc *alc) {
  yyjson_read_err err;
  yyjson_doc *doc = yyjson_read_opts((char *)str, len, 0, alc, &err);
  if (doc || err.code != YYJSON_READ_ERROR_INVALID_STRING) return doc;

  size_t rewritten_len = 0;
  char *rewritten = json_surrogate_rewrite(str, len, &rewritten_len);
  if (!rewritten) return NULL;

  doc = yyjson_read_opts(rewritten, rewritten_len, 0, NULL, NULL);
  free(rewritten);
  if (doc) json_surrogate_decode(doc);
  
  return doc;
}

static bool json_shape_matches(ant_shape_t *shape, yyjson_val *val) {
  if (!shape || ant_shape_count(shape) != yyjson_obj_size(val)) return false;
  size_t idx, max;
  yyjson_val *key, *item;
  
  yyjson_obj_foreach(val, idx, max, key, item) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(shape, (uint32_t)idx);
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING ||
        prop->attrs != ANT_PROP_ATTR_DEFAULT || prop->has_getter || prop->has_setter ||
        intern_length(prop->key.interned) != yyjson_get_len(key) ||
        memcmp(prop->key.interned, yyjson_get_str(key), yyjson_get_len(key)) != 0)
      return false;
  }
  
  return true;
}

void json_layout_cache_clear(ant_t *js) {
  json_layout_cache_t *cache = js->json_layout_cache;
  if (!cache) return;
  for (unsigned i = 0; i < cache->count; i++) 
    ant_shape_release(cache->entries[i].shape);
  free(cache);
  js->json_layout_cache = NULL;
}

static uint64_t json_layout_hash(yyjson_val *val, size_t *key_bytes) {
  uint64_t hash = yyjson_obj_size(val);
  *key_bytes = 0;
  size_t idx, max;
  yyjson_val *key, *item;
  yyjson_obj_foreach(val, idx, max, key, item) {
    size_t len = yyjson_get_len(key);
    hash = ant_hash_mix(hash ^ hash_key(yyjson_get_str(key), len), len + 1);
    if (len >= JSON_LAYOUT_CACHE_BYTES - *key_bytes) *key_bytes = JSON_LAYOUT_CACHE_BYTES;
    else *key_bytes += len + 1;
  }
  return hash;
}

static ant_shape_t *json_layout_cache_find(ant_t *js, yyjson_val *val, uint64_t hash) {
  json_layout_cache_t *cache = js->json_layout_cache;
  if (!cache) return NULL;
  for (unsigned i = 0; i < cache->count; i++) {
    json_layout_cache_entry_t entry = cache->entries[i];
    if (entry.hash != hash || !json_shape_matches(entry.shape, val)) continue;
    memmove(cache->entries + 1, cache->entries, i * sizeof(entry));
    cache->entries[0] = entry;
    return entry.shape;
  }
  return NULL;
}

static void json_layout_cache_insert(ant_t *js, ant_shape_t *shape, uint64_t hash, size_t key_bytes) {
  size_t bytes = ant_shape_storage_bytes(shape);
  if (key_bytes >= JSON_LAYOUT_CACHE_BYTES || bytes > JSON_LAYOUT_CACHE_BYTES - key_bytes) return;
  bytes += key_bytes;
  if (!js->json_layout_cache) js->json_layout_cache = calloc(1, sizeof(json_layout_cache_t));
  json_layout_cache_t *cache = js->json_layout_cache;
  if (!cache) return;
  while (cache->count && (cache->count == JSON_LAYOUT_CACHE_ENTRIES || cache->bytes + bytes > JSON_LAYOUT_CACHE_BYTES)) {
    json_layout_cache_entry_t *entry = &cache->entries[--cache->count];
    cache->bytes -= entry->bytes;
    ant_shape_release(entry->shape);
  }
  memmove(cache->entries + 1, cache->entries, cache->count * sizeof(*cache->entries));
  ant_shape_retain(shape);
  cache->entries[0] = (json_layout_cache_entry_t){ .shape = shape, .hash = hash, .bytes = bytes };
  cache->count++;
  cache->bytes += bytes;
}

static bool json_prepare_object(
  ant_t *js, ant_object_t *obj, yyjson_val *val,
  ant_shape_t *expected, bool *duplicates
) {
  *duplicates = false;
  if (ant_shape_proto(expected) == ant_object_proto(obj) && json_shape_matches(expected, val)) {
    ant_shape_transition_existing(&obj->shape, expected);
    return js_obj_ensure_prop_capacity(js, obj, ant_shape_count(expected));
  }

  size_t count = yyjson_obj_size(val);
  bool bulk = count >= JSON_BULK_LAYOUT_MIN_PROPS;
  uint64_t hash = 0;
  size_t key_bytes = 0;
  
  if (bulk) {
    hash = json_layout_hash(val, &key_bytes);
    ant_shape_t *cached = json_layout_cache_find(js, val, hash);
    if (cached && ant_shape_proto(cached) == ant_object_proto(obj)) {
      ant_shape_transition_existing(&obj->shape, cached);
      return js_obj_ensure_prop_capacity(js, obj, ant_shape_count(cached));
    }
    ant_shape_t *shape = ant_shape_clone_bulk(obj->shape, (uint32_t)count);
    if (!shape) return false;
    ant_shape_release(obj->shape);
    obj->shape = shape;
  }

  size_t idx, max;
  yyjson_val *key, *item;
  yyjson_obj_foreach(val, idx, max, key, item) {
    const char *interned = intern_string(js, yyjson_get_str(key), yyjson_get_len(key));
    if (!interned) return false;
    uint32_t slot;
    int32_t found = ant_shape_lookup_interned(obj->shape, interned);
    if (found >= 0) slot = (uint32_t)found;
    else {
      bool added = bulk
        ? ant_shape_add_interned(js, obj->shape, interned, ANT_PROP_ATTR_DEFAULT, &slot)
        : ant_shape_add_interned_tr(js, &obj->shape, interned, ANT_PROP_ATTR_DEFAULT, &slot);
      if (!added) return false;
    }
    if (slot != idx) *duplicates = true;
  }
  
  if (!js_obj_ensure_prop_capacity(js, obj, ant_shape_count(obj->shape))) return false;
  if (bulk && !*duplicates) json_layout_cache_insert(js, obj->shape, hash, key_bytes);
  
  return true;
}

static ant_value_t yyjson_to_jsval(
  ant_t *js, yyjson_val *val, gc_temp_root_scope_t *roots, ant_shape_t *expected
) {
  if (!val) return Ant_Value_Undefined;
  if (__builtin_expect(sv_check_c_stack_overflow(js), 0)) return Ant_Error_CreateStackOverflow(js);
  
  switch (yyjson_get_type(val)) {
  case YYJSON_TYPE_NULL: return Ant_Value_Null;
  case YYJSON_TYPE_BOOL: return Ant_Value_FromBool(yyjson_get_bool(val));
  
  case YYJSON_TYPE_STR: {
    ant_value_t str = js_mkstr(js, yyjson_get_str(val), yyjson_get_len(val));
    if (is_err(str)) return str;
    if (!json_temp_pin(roots, str)) return json_parse_oom(js);
    return str;
  }
  
  case YYJSON_TYPE_NUM: {
    if (yyjson_is_sint(val)) {
      int64_t number = yyjson_get_sint(val);
      return Ant_Value_FromNumber(number == 0 ? -0.0 : (double)number);
    }
    if (yyjson_is_uint(val)) return Ant_Value_FromNumber((double)yyjson_get_uint(val));
    return Ant_Value_FromNumber(yyjson_get_real(val));
  }
  
  case YYJSON_TYPE_ARR: {
    ant_value_t arr = Ant_Array_Create(js);
    if (is_err(arr)) return arr;
    if (!json_temp_pin(roots, arr)) return json_parse_oom(js);

    size_t count = yyjson_arr_size(val);
    if (count > MAX_DENSE_INITIAL_CAP) js_arr_reserve(js, arr, (uint32_t)count);

    size_t idx, max;
    yyjson_val *item;
    size_t mark = roots->len;
    ant_shape_t *previous = NULL;

    yyjson_arr_foreach(val, idx, max, item) {
      ant_value_t elem = yyjson_to_jsval(js, item, roots, previous);
      if (is_err(elem)) {
        ant_shape_release(previous);
        return elem;
      }
      js_arr_push(js, arr, elem);
      if (vtype(elem) == kTypeObject) {
        ant_shape_t *next = js_obj_ptr(elem)->shape;
        ant_shape_retain(next);
        ant_shape_release(previous);
        previous = next;
      }
      gc_temp_root_truncate(roots, mark);
    }

    ant_shape_release(previous);
    return arr;
  }
  
  case YYJSON_TYPE_OBJ: {
    ant_value_t obj = Ant_Object_Create(js);
    if (is_err(obj)) return obj;
    if (!json_temp_pin(roots, obj)) return json_parse_oom(js);

    ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
    size_t count = yyjson_obj_size(val);
    if (count > UINT32_MAX) return json_parse_oom(js);
    
    bool prepared = expected || count >= JSON_BULK_LAYOUT_MIN_PROPS;
    bool duplicates = false;
    
    if (prepared) {
      if (!json_prepare_object(js, ptr, val, expected, &duplicates)) return json_parse_oom(js);
    } else if (count > 1 && !js_obj_ensure_prop_capacity(js, ptr, (uint32_t)count)) return json_parse_oom(js);

    size_t idx, max; yyjson_val *key, *item;
    size_t mark = roots->len;

    yyjson_obj_foreach(val, idx, max, key, item) {
      ant_value_t v = yyjson_to_jsval(js, item, roots, NULL);
      if (is_err(v)) return v;
      if (!prepared) {
        if (is_err(mkprop_append_fast(js, obj, yyjson_get_str(key), yyjson_get_len(key), v)))
          return json_parse_oom(js);
        gc_temp_root_truncate(roots, mark);
        continue;
      }
      uint32_t slot = (uint32_t)idx;
      if (duplicates) {
        const char *interned = intern_find(js, yyjson_get_str(key), yyjson_get_len(key));
        int32_t found = ant_shape_lookup_interned(ptr->shape, interned);
        assert(found >= 0);
        slot = (uint32_t)found;
      }
      ant_object_prop_set_unchecked(ptr, slot, v);
      gc_write_barrier(js, ptr, v);
      gc_temp_root_truncate(roots, mark);
    }

    return obj;
  }
  
  default: return Ant_Value_Undefined; }
}

typedef struct {
  ant_t *js;
  
  ant_value_t *stack;
  ant_value_t replacer_func;
  ant_value_t property_list;
  ant_value_t error;
  ant_value_t holder;
  ant_value_t cycle_start;
  
  gc_temp_root_scope_t temp_roots;
  gc_temp_root_handle_t error_handle;
  gc_temp_root_handle_t holder_handle;
  
  int stack_size;
  int stack_cap;
  int has_cycle;
  bool has_error;
  size_t indent_len;
  const char *tojson_key;
  
  struct { 
    ant_value_t proto; 
    ant_prop_loc_t loc; 
  } tojson_cache[4];
  
  uint32_t tojson_epoch;
  uint8_t tojson_count;
  uint8_t tojson_next;

  const void *plan_seen[16];
  struct json_plan_slot *plans;
  struct json_plan *plans_retired;

  char cycle_key[128];
  char indent[64];
  ant_value_t stack_inline[32];
} json_cycle_ctx;

static inline bool json_has_abort(json_cycle_ctx *ctx) {
  return ctx->has_cycle || ctx->has_error;
}

static void json_set_error(json_cycle_ctx *ctx, ant_value_t value) {
  ctx->has_error = true;
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
  if (ctx->has_error) return;
  if (!is_err(value) && Ant_Exception_Pending(ctx->js))
    value = Ant_Exception_Current(ctx->js);
  json_set_error(ctx, value);
  if (Ant_Exception_Peek(ctx->js) == value) Ant_Exception_Clear(ctx->js);
}

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
  bool oom;
  char inline_buf[512];
} json_out_t;

static __attribute__((noinline)) bool json_out_grow(json_out_t *o, size_t extra) {
  if (o->oom) return false;
  size_t want = o->cap;
  
  while (want < o->len + extra) {
    size_t next = want * 2;
    if (next < want) { o->oom = true; return false; }
    want = next;
  }

  bool on_heap = o->buf != o->inline_buf;
  char *next = on_heap ? realloc(o->buf, want) : malloc(want);
  if (!next) { o->oom = true; return false; }
  if (!on_heap) memcpy(next, o->buf, o->len);

  o->buf = next;
  o->cap = want;
  
  return true;
}

static inline bool json_out_reserve(json_out_t *o, size_t extra) {
  return o->len + extra <= o->cap || json_out_grow(o, extra);
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
  if (byte_len > (SIZE_MAX - 2) / 6) { o->oom = true; return false; }
  if (!json_out_reserve(o, byte_len * 6 + 2)) return false;
  o->len += utf8_json_quote_into(o->buf + o->len, str, byte_len);
  return true;
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

static __attribute__((noinline)) bool json_cycle_grow(json_cycle_ctx *ctx) {
  int cap = ctx->stack_cap * 2;
  bool on_heap = ctx->stack != ctx->stack_inline;
  
  ant_value_t *next = on_heap
    ? realloc(ctx->stack, (size_t)cap * sizeof(*next))
    : malloc((size_t)cap * sizeof(*next));
  
  if (!next) {
    json_set_error(ctx, json_stringify_oom(ctx->js));
    return false;
  }
  
  if (!on_heap) memcpy(next, ctx->stack, (size_t)ctx->stack_size * sizeof(*next));
  ctx->stack = next;
  ctx->stack_cap = cap;
  
  return true;
}

static inline bool json_cycle_push(json_cycle_ctx *ctx, ant_value_t val) {
  if (ctx->stack_size >= ctx->stack_cap && !json_cycle_grow(ctx)) return false;
  ctx->stack[ctx->stack_size++] = val;
  return true;
}

static inline void json_cycle_pop(json_cycle_ctx *ctx) {
  if (ctx->stack_size > 0) ctx->stack_size--;
}

static inline int key_matches(const char *a, size_t a_len, const char *b, size_t b_len) {
  return a_len == b_len && memcmp(a, b, a_len) == 0;
}

static inline ant_value_t json_snapshot_keys(ant_t *js, ant_value_t value) {
  if (!is_special_object(value)) return Ant_Array_Create(js);
  return js_own_property_keys(js, value, false, true);
}

typedef enum { 
  JSON_W_OK,
  JSON_W_SKIP,
  JSON_W_ABORT
} json_write_t;

// circular dependencies
static json_write_t json_write_with_key(json_cycle_ctx *ctx, json_out_t *out, const char *key, size_t key_len, ant_value_t val, int in_array, int depth);
static ant_value_t apply_reviver(ant_t *js, ant_value_t holder, const char *key, size_t key_len, ant_value_t reviver, gc_temp_root_scope_t *roots);

static ant_prop_loc_t json_tojson_find_proto(ant_t *js, json_cycle_ctx *ctx, ant_value_t proto, bool *cacheable) {
  ant_value_t cur = proto;
  
  for (int depth = 0; is_object_type(cur); depth++) {
    ant_object_t *obj = depth < MAX_PROTO_CHAIN_DEPTH && !is_proxy(cur) ? js_obj_ptr(js_as_obj(cur)) : NULL;
    if (!obj || obj->flags.is_exotic) {
      *cacheable = false;
      return lkp_proto(js, proto, "toJSON", 6);
    }

    int32_t slot = obj->shape ? ant_shape_lookup_interned(obj->shape, ctx->tojson_key) : -1;
    if (slot >= 0) return (ant_prop_loc_t){ obj, (uint32_t)slot };

    ant_object_guard_absence(obj);
    cur = ant_object_proto(obj);
  }
  
  return ANT_PROP_LOC_NONE;
}

static ant_prop_loc_t json_tojson_lookup_proto(ant_t *js, json_cycle_ctx *ctx, ant_value_t proto) {
  if (ctx->tojson_epoch != js->ic.epoch) {
    ctx->tojson_epoch = js->ic.epoch;
    ctx->tojson_count = 0;
  }

  for (uint8_t i = 0; i < ctx->tojson_count; i++)
    if (ctx->tojson_cache[i].proto == proto) return ctx->tojson_cache[i].loc;

  bool cacheable = true;
  ant_prop_loc_t loc = json_tojson_find_proto(js, ctx, proto, &cacheable);
  if (!cacheable) return loc;

  uint8_t slot = ctx->tojson_count < 4 ? ctx->tojson_count++ : (ctx->tojson_next++ & 3);
  ctx->tojson_cache[slot].proto = proto;
  ctx->tojson_cache[slot].loc = loc;
  
  return loc;
}

static ant_value_t json_apply_tojson(ant_t *js, const char *key, size_t key_len, ant_value_t val, json_cycle_ctx *ctx) {
  bool bigint = vtype(val) == kTypeBigInt;
  if (!bigint && !is_special_object(val)) return val;

  ant_value_t toJSON = Ant_Value_Undefined;
  bool needs_generic = bigint || is_proxy(val);
  ant_prop_loc_t found = ANT_PROP_LOC_NONE;

  if (!needs_generic) {
    found = lkp_interned(val, ctx->tojson_key);

    if (!found.obj) {
      ant_value_t proto = js_get_proto(js, val);
      if (is_object_type(proto)) found = json_tojson_lookup_proto(js, ctx, proto);
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
      return Ant_Value_Undefined;
    }
    toJSON = generic;
  }

  if (!is_callable(toJSON)) return val;
  ant_value_t key_arg = js_mkstr(js, key, key_len);
  if (is_err(key_arg)) {
    json_capture_error(ctx, key_arg);
    return Ant_Value_Undefined;
  }
  
  if (!json_ctx_pin_value(ctx, key_arg)) return Ant_Value_Undefined;
  ant_value_t args[1] = { key_arg };
  
  ant_value_t transformed = sv_vm_call(
    js->vm, js,
    toJSON, val,
    args, 1, NULL, Ant_Value_Undefined
  );
  
  if (is_err(transformed)) {
    json_capture_error(ctx, transformed);
    return Ant_Value_Undefined;
  }
  if (!json_ctx_pin_value(ctx, transformed)) return Ant_Value_Undefined;

  return transformed;
}

static ant_value_t json_apply_replacer(
  ant_t *js,
  const char *key,
  size_t key_len,
  ant_value_t val,
  json_cycle_ctx *ctx
) {
  if (!is_callable(ctx->replacer_func)) return val;
  ant_value_t key_arg = js_mkstr(js, key, key_len);
  if (is_err(key_arg)) {
    json_capture_error(ctx, key_arg);
    return Ant_Value_Undefined;
  }
  if (!json_ctx_pin_value(ctx, key_arg)) return Ant_Value_Undefined;
  ant_value_t args[2] = { key_arg, val };
  
  ant_value_t transformed = sv_vm_call(
    js->vm, js, 
    ctx->replacer_func, ctx->holder, 
    args, 2, NULL, Ant_Value_Undefined
  );
  
  if (is_err(transformed)) {
    json_capture_error(ctx, transformed);
    return Ant_Value_Undefined;
  }
  if (!json_ctx_pin_value(ctx, transformed)) return Ant_Value_Undefined;

  return transformed;
}

static inline ant_value_t json_create_root_holder(ant_t *js, ant_value_t value, json_cycle_ctx *ctx) {
  ant_value_t holder = Ant_Object_CreateBare(js);
  if (is_err(holder)) return holder;
  if (!json_ctx_pin_value(ctx, holder)) return Ant_Value_Undefined;
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
    size_t idx_len = 0;

    if (i && !json_out_char(out, ',')) goto abort;
    if (!json_write_indent(ctx, out, depth + 1)) goto abort;
    ant_value_t elem = js_arr_get(js, val, i);

    if (is_err(elem) || Ant_Exception_Pending(js)) {
      json_capture_error(ctx, elem);
      goto abort;
    }

    if (has_replacer || is_special_object(elem) || vtype(elem) == kTypeBigInt) idx_len = uint_to_str(idxstr, sizeof(idxstr), (uint64_t)i);
    else idxstr[0] = '\0';

    json_write_t w = json_write_with_key(ctx, out, idxstr, idx_len, elem, 1, depth + 1);
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

typedef struct {
  const char *key;
  uint32_t slot;
} json_slot_key_t;

static bool json_collect_own_keys(
  ant_t *js, ant_value_t val, json_slot_key_t **out_keys, size_t *out_count,
  json_slot_key_t *inline_keys, size_t inline_cap
) {
  if (vtype(val) != kTypeObject || is_proxy(val)) return false;

  ant_object_t *ptr = js_obj_ptr(js_as_obj(val));
  if (!ptr || !ptr->shape || ptr->flags.is_exotic) return false;

  uint32_t count = ant_shape_count(ptr->shape);
  if (count > ptr->prop_count) count = ptr->prop_count;

  json_slot_key_t *keys = inline_keys;
  if (count > inline_cap) {
    keys = malloc(sizeof(*keys) * count);
    if (!keys) return false;
  }

  own_index_list_t indices;
  own_index_list_init(&indices);
  size_t n = 0;

  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING) continue;
    if (!(prop->attrs & ANT_PROP_ATTR_ENUMERABLE)) continue;

    unsigned long index = 0;
    const char *key = prop->key.interned;
    if (!own_key_is_array_index(key, intern_length(key), &index)) {
      keys[n++] = (json_slot_key_t){ key, i };
      continue;
    }

    if (!own_index_list_push(&indices, i, index)) {
      own_index_list_free(&indices);
      if (keys != inline_keys) free(keys);
      return false;
    }
  }

  if (indices.count) {
    own_index_keys_sort(indices.items, indices.count);
    memmove(keys + indices.count, keys, n * sizeof(*keys));
    for (uint32_t i = 0; i < indices.count; i++) {
      uint32_t slot = indices.items[i].slot;
      keys[i] = (json_slot_key_t){ ant_shape_prop_at(ptr->shape, slot)->key.interned, slot };
    }
    n += indices.count;
  }

  own_index_list_free(&indices);
  *out_keys = keys;
  *out_count = n;
  
  return true;
}

static json_write_t json_write_object_fast(
  json_cycle_ctx *ctx, json_out_t *out, ant_value_t val, int depth,
  const json_slot_key_t *keys, size_t key_count
) {
  ant_t *js = ctx->js;
  ant_value_t saved_holder = ctx->holder;
  ant_object_t *ptr = js_obj_ptr(js_as_obj(val));
  bool wrote_any = false;

  if (!json_out_char(out, '{')) return JSON_W_ABORT;
  json_set_holder(ctx, val);

  for (size_t i = 0; i < key_count; i++) {
    const char *key = keys[i].key;
    size_t key_len = intern_length(key);

    ant_prop_loc_t loc = { ptr, keys[i].slot };
    const ant_shape_prop_t *meta = ant_shape_prop_at(ptr->shape, loc.slot);

    if (!meta || meta->type != ANT_SHAPE_KEY_STRING || meta->key.interned != key) {
      loc = lkp_interned(val, key);
      if (!loc.obj) continue;
      meta = ant_shape_prop_at(loc.obj->shape, loc.slot);
    }

    ant_value_t prop;
    if (meta && (meta->has_getter || meta->has_setter)) {
      prop = js_getprop_fallback_len(js, val, key, key_len);
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

    json_write_t w = json_write_with_key(ctx, out, key, key_len, prop, 0, depth + 1);
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

typedef struct {
  const char *key;
  uint32_t key_len;
  uint32_t slot;
  uint32_t quoted_off;
  uint32_t quoted_len;
} json_plan_key_t;

typedef struct json_plan {
  ant_shape_t *shape;
  struct json_plan *next_retired;
  uint32_t count;
  uint32_t epoch;
  uint32_t key_count;
  uint32_t in_use;
  bool usable;
  char *quoted;
  json_plan_key_t keys[];
} json_plan_t;

typedef struct json_plan_slot {
  ant_shape_t *shape;
  json_plan_t *plan;
} json_plan_slot_t;

static inline uint32_t json_plan_count(const ant_object_t *ptr) {
  uint32_t count = ant_shape_count(ptr->shape);
  return count > ptr->prop_count ? ptr->prop_count : count;
}

static json_plan_t *json_plan_build(ant_t *js, ant_value_t val, ant_object_t *ptr) {
  json_slot_key_t inline_keys[24];
  json_slot_key_t *keys = NULL;
  size_t key_count = 0;
  if (!json_collect_own_keys(js, val, &keys, &key_count, inline_keys, 24)) return NULL;

  json_plan_t *plan = calloc(1, sizeof(*plan) + key_count * sizeof(json_plan_key_t));
  if (!plan) goto done;

  plan->shape = ptr->shape;
  ant_shape_retain(plan->shape);
  plan->count = json_plan_count(ptr);
  plan->epoch = js->ic.epoch;
  plan->key_count = (uint32_t)key_count;
  plan->usable = true;

  size_t quoted_cap = 0;
  for (size_t i = 0; i < key_count; i++) {
    const ant_shape_prop_t *meta = ant_shape_prop_at(ptr->shape, keys[i].slot);
    if (!meta || meta->has_getter || meta->has_setter) plan->usable = false;
    quoted_cap += (size_t)intern_length(keys[i].key) * 6 + 3;
  }
  if (!plan->usable) goto done;

  plan->quoted = malloc(quoted_cap ? quoted_cap : 1);
  if (!plan->quoted) { plan->usable = false; goto done; }

  size_t pos = 0;
  for (size_t i = 0; i < key_count; i++) {
    size_t key_len = intern_length(keys[i].key);
    size_t quoted_len = utf8_json_quote_into(plan->quoted + pos, keys[i].key, key_len);
    plan->quoted[pos + quoted_len++] = ':';
    plan->keys[i] = (json_plan_key_t){
      keys[i].key, (uint32_t)key_len, keys[i].slot, (uint32_t)pos, (uint32_t)quoted_len
    };
    pos += quoted_len;
  }

done:
  if (keys != inline_keys) free(keys);
  return plan;
}

static void json_plan_free(json_plan_t *plan) {
  if (!plan) return;
  ant_shape_release(plan->shape);
  free(plan->quoted);
  free(plan);
}

static void json_plan_drop(json_cycle_ctx *ctx, json_plan_t *plan) {
  if (!plan) return;
  if (!plan->in_use) {
    json_plan_free(plan);
    return;
  }
  plan->next_retired = ctx->plans_retired;
  ctx->plans_retired = plan;
}

static void json_plans_free(json_cycle_ctx *ctx) {
  for (json_plan_t *plan = ctx->plans_retired, *next; plan; plan = next) {
    next = plan->next_retired;
    json_plan_free(plan);
  }
  if (!ctx->plans) return;
  for (int i = 0; i < JSON_PLAN_SLOTS; i++) json_plan_free(ctx->plans[i].plan);
  free(ctx->plans);
}

static json_plan_t *json_plan_for(json_cycle_ctx *ctx, ant_value_t val) {
  if (vtype(val) != kTypeObject || is_proxy(val)) return NULL;
  ant_object_t *ptr = js_obj_ptr(js_as_obj(val));
  if (!ptr || !ptr->shape || ptr->flags.is_exotic) return NULL;

  ant_shape_t *shape = ptr->shape;
  size_t index = ((uintptr_t)shape >> 4) & (JSON_PLAN_SLOTS - 1);
  json_plan_slot_t *entry = ctx->plans ? &ctx->plans[index] : NULL;

  if (entry && entry->shape == shape) {
    json_plan_t *plan = entry->plan;
    if (plan->epoch == ctx->js->ic.epoch && plan->count == json_plan_count(ptr))
      return plan->usable ? plan : NULL;
  } else if (ctx->plan_seen[index] != shape) {
    ctx->plan_seen[index] = shape;
    return NULL;
  }

  json_plan_t *plan = json_plan_build(ctx->js, val, ptr);
  if (!plan) return NULL;

  if (!entry) {
    ctx->plans = calloc(JSON_PLAN_SLOTS, sizeof(*ctx->plans));
    if (!ctx->plans) {
      json_plan_free(plan);
      return NULL;
    }
    entry = &ctx->plans[index];
  }

  json_plan_drop(ctx, entry->plan);
  entry->shape = shape;
  entry->plan = plan;
  return plan->usable ? plan : NULL;
}

static bool json_plan_load_moved(
  json_cycle_ctx *ctx, ant_value_t val, ant_object_t *ptr, const json_plan_key_t *entry, ant_value_t *out
) {
  ant_prop_loc_t loc = { ptr, entry->slot };
  const ant_shape_prop_t *meta = ant_shape_prop_at(ptr->shape, loc.slot);

  if (!meta || meta->type != ANT_SHAPE_KEY_STRING || meta->key.interned != entry->key) {
    loc = lkp_interned(val, entry->key);
    if (!loc.obj) return false;
    meta = ant_shape_prop_at(loc.obj->shape, loc.slot);
  }

  if (!meta || (!meta->has_getter && !meta->has_setter)) {
    *out = js_prop_load(loc);
    return true;
  }

  *out = js_getprop_fallback_len(ctx->js, val, entry->key, entry->key_len);
  if (is_err(*out)) json_capture_error(ctx, *out);
  return true;
}

static json_write_t json_write_object_plan(
  json_cycle_ctx *ctx, json_out_t *out, ant_value_t val, int depth, json_plan_t *plan
) {
  ant_t *js = ctx->js;
  ant_value_t saved_holder = ctx->holder;
  ant_object_t *ptr = js_obj_ptr(js_as_obj(val));
  bool wrote_any = false;

  if (!json_out_char(out, '{')) return JSON_W_ABORT;
  json_set_holder(ctx, val);
  plan->in_use++;

  for (uint32_t i = 0; i < plan->key_count; i++) {
    const json_plan_key_t *entry = &plan->keys[i];
    ant_value_t prop;

    if (ptr->shape == plan->shape && js->ic.epoch == plan->epoch) prop = js_prop_load((ant_prop_loc_t){ ptr, entry->slot });
    else if (!json_plan_load_moved(ctx, val, ptr, entry, &prop)) continue;
    else if (json_has_abort(ctx)) goto abort;

    if (!json_ctx_pin_value(ctx, prop)) goto abort;
    size_t mark = out->len;

    if (wrote_any && !json_out_char(out, ',')) goto abort;
    if (!json_write_indent(ctx, out, depth + 1)) goto abort;
    if (!json_out_write(out, plan->quoted + entry->quoted_off, entry->quoted_len)) goto abort;
    if (ctx->indent_len && !json_out_char(out, ' ')) goto abort;

    json_write_t w = json_write_with_key(ctx, out, entry->key, entry->key_len, prop, 0, depth + 1);
    if (w == JSON_W_ABORT) goto abort;
    if (w == JSON_W_SKIP) { out->len = mark; continue; }

    wrote_any = true;
  }

  if (wrote_any && !json_write_indent(ctx, out, depth)) goto abort;
  if (!json_out_char(out, '}')) goto abort;

  plan->in_use--;
  json_set_holder(ctx, saved_holder);
  return JSON_W_OK;

abort:
  plan->in_use--;
  json_set_holder(ctx, saved_holder);
  return JSON_W_ABORT;
}

static json_write_t json_write_object_keys(
  json_cycle_ctx *ctx, json_out_t *out, ant_value_t val, int depth, ant_value_t keys
) {
  ant_t *js = ctx->js;
  ant_value_t saved_holder = ctx->holder;
  bool wrote_any = false;

  if (!json_out_char(out, '{')) return JSON_W_ABORT;
  json_set_holder(ctx, val);

  ant_offset_t key_count = js_arr_len(js, keys);
  for (ant_offset_t i = 0; i < key_count; i++) {
    ant_value_t key_val = js_arr_get(js, keys, i);
    size_t key_len = 0;
    char *key = js_getstr(js, key_val, &key_len);
    if (!key) continue;

    ant_value_t prop = js_getprop_fallback_len(js, val, key, key_len);
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

    json_write_t w = json_write_with_key(ctx, out, key, key_len, prop, 0, depth + 1);
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
  if (vtype(ctx->property_list) != kTypeUndefined)
    return json_write_object_keys(ctx, out, val, depth, ctx->property_list);

  json_plan_t *plan = json_plan_for(ctx, val);
  if (plan) return json_write_object_plan(ctx, out, val, depth, plan);

  json_slot_key_t inline_keys[24];
  json_slot_key_t *fast_keys = NULL;
  size_t fast_count = 0;

  if (json_collect_own_keys(js, val, &fast_keys, &fast_count, inline_keys, 24)) {
    json_write_t w = json_write_object_fast(ctx, out, val, depth, fast_keys, fast_count);
    if (fast_keys != inline_keys) free(fast_keys);
    return w;
  }

  ant_value_t keys = json_snapshot_keys(js, val);
  if (is_err(keys)) {
    json_capture_error(ctx, keys);
    return JSON_W_ABORT;
  }
  
  if (!json_ctx_pin_value(ctx, keys)) return JSON_W_ABORT;
  return json_write_object_keys(ctx, out, val, depth, keys);
}

static json_write_t json_write_impl(
  json_cycle_ctx *ctx, json_out_t *out, const char *key,
  ant_value_t val, int in_array, int depth
) {
  json_write_t result;

  switch (vtype(val)) {
    case kTypeNull: return json_out_write(out, "null", 4) ? JSON_W_OK : JSON_W_ABORT;
    case kTypeBool:
      return json_out_write(out, val == Ant_Value_True ? "true" : "false", val == Ant_Value_True ? 4 : 5)
        ? JSON_W_OK : JSON_W_ABORT;

    case kTypeUndefined:
    case kTypeFunction:
    case kTypeBuiltin:
    case kTypeSymbol:
      return in_array ? (json_out_write(out, "null", 4) ? JSON_W_OK : JSON_W_ABORT) : JSON_W_SKIP;

    case kTypeBigInt:
      json_capture_error(ctx, js_mkerr_typed(ctx->js, JS_ERR_TYPE,"Do not know how to serialize a BigInt"));
      return JSON_W_ABORT;

    case kTypeNumber: return json_out_number(out, Ant_Value_AsNumberUnchecked(val)) ? JSON_W_OK : JSON_W_ABORT;
    case kTypeString: return json_out_quoted(ctx->js, out, val) ? JSON_W_OK : JSON_W_ABORT;

    case kTypeObject:
    case kTypeArray: break;
    default: return json_out_write(out, "null", 4) ? JSON_W_OK : JSON_W_ABORT;
  }

  if (vtype(val) == kTypeObject) {
    ant_value_t prim = js_get_slot(val, SLOT_PRIMITIVE);
    switch (vtype(prim)) {
      case kTypeNumber:  return json_out_number(out, Ant_Value_AsNumberUnchecked(prim)) ? JSON_W_OK : JSON_W_ABORT;
      case kTypeString:  return json_out_quoted(ctx->js, out, prim) ? JSON_W_OK : JSON_W_ABORT;
      case kTypeBool: return json_out_write(out, prim == Ant_Value_True ? "true" : "false", prim == Ant_Value_True ? 4 : 5) ? JSON_W_OK : JSON_W_ABORT;

      case kTypeBigInt:
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
  if (!json_cycle_push(ctx, val)) return JSON_W_ABORT;

  result = is_array_value(val)
    ? json_write_array(ctx, out, val, depth)
    : json_write_object(ctx, out, val, depth);

  json_cycle_pop(ctx);
  return result;
}

static json_write_t json_write_with_key(
  json_cycle_ctx *ctx, json_out_t *out, const char *key, size_t key_len,
  ant_value_t val, int in_array, int depth
) {
  val = json_apply_tojson(ctx->js, key, key_len, val, ctx);
  if (json_has_abort(ctx)) return JSON_W_ABORT;

  val = json_apply_replacer(ctx->js, key, key_len, val, ctx);
  if (json_has_abort(ctx)) return JSON_W_ABORT;

  return json_write_impl(ctx, out, key, val, in_array, depth);
}

static ant_value_t apply_reviver_call(
  ant_t *js, ant_value_t holder, const char *key, size_t key_len,
  ant_value_t reviver, gc_temp_root_scope_t *roots
) {
  ant_value_t key_str = js_mkstr(js, key, key_len);
  if (is_err(key_str)) return key_str;
  
  if (!json_temp_pin(roots, key_str)) return json_parse_oom(js);
  ant_value_t current_value = js_getprop_fallback_len(js, holder, key, key_len);
  ant_value_t call_args[2] = { key_str, current_value };
  
  ant_value_t result = sv_vm_call(
    js->vm, js, reviver, holder,
    call_args, 2, NULL, Ant_Value_Undefined
  );
  
  if (!is_err(result) && !json_temp_pin(roots, result)) return json_parse_oom(js);
  
  return result;
}

static ant_value_t apply_reviver_to_array(
  ant_t *js, ant_value_t value, ant_value_t reviver, gc_temp_root_scope_t *roots
) {
  ant_offset_t length = js_arr_len(js, value);

  for (ant_offset_t i = 0; i < length; i++) {
    char idxstr[32];
    size_t idx_len = uint_to_str(idxstr, sizeof(idxstr), (uint64_t)i);
    
    ant_value_t new_elem = apply_reviver(js, value, idxstr, idx_len, reviver, roots);
    if (is_err(new_elem)) return new_elem;
    
    if (vtype(new_elem) == kTypeUndefined) {
      js_delete_prop(js, value, idxstr, idx_len);
      continue;
    }
    
    ant_value_t key_val = js_mkstr(js, idxstr, idx_len);
    if (is_err(key_val)) return key_val;
    
    if (!json_temp_pin(roots, key_val)) return json_parse_oom(js);
    js_setprop(js, value, key_val, new_elem);
  }
  
  return Ant_Value_Undefined;
}

static ant_value_t apply_reviver_to_object(
  ant_t *js, ant_value_t value, ant_value_t reviver, gc_temp_root_scope_t *roots
) {
  ant_value_t keys = json_snapshot_keys(js, value);
  if (is_err(keys)) return keys;
  
  if (vtype(keys) != kTypeArray) return Ant_Value_Undefined;
  if (!json_temp_pin(roots, keys)) return json_parse_oom(js);

  ant_offset_t key_count = js_arr_len(js, keys);
  for (ant_offset_t i = 0; i < key_count; i++) {
    ant_value_t key_val = js_arr_get(js, keys, i);
    size_t key_len = 0;
    
    char *key = js_getstr(js, key_val, &key_len);
    if (!key) continue;
    
    ant_value_t new_val = apply_reviver(js, value, key, key_len, reviver, roots);
    if (is_err(new_val)) return new_val;
    
    if (vtype(new_val) == kTypeUndefined) js_delete_prop(js, value, key, key_len);
    else js_setprop(js, value, key_val, new_val);
  }
  
  return Ant_Value_Undefined;
}

static ant_value_t apply_reviver(
  ant_t *js, ant_value_t holder, const char *key, size_t key_len,
  ant_value_t reviver, gc_temp_root_scope_t *roots
) {
  if (__builtin_expect(sv_check_c_stack_overflow(js), 0)) return Ant_Error_CreateStackOverflow(js);
  ant_value_t val = js_getprop_fallback_len(js, holder, key, key_len);
  if (is_err(val)) return val;

  ant_value_t walked = Ant_Value_Undefined;
  if (is_array_value(val)) walked = apply_reviver_to_array(js, val, reviver, roots);
  else if (vtype(val) == kTypeObject) walked = apply_reviver_to_object(js, val, reviver, roots);
  if (is_err(walked)) return walked;

  return apply_reviver_call(js, holder, key, key_len, reviver, roots);
}

ant_value_t js_json_parse(ant_params_t) {
  ant_value_t text = nargs > 0 ? args[0] : Ant_Value_Undefined;
  if (vtype(text) != kTypeString) {
    if (vtype(text) == kTypeSymbol)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert a Symbol value to a string");
    text = coerce_to_str(js, text);
    if (is_err(text)) return text;
  }
  
  ant_value_t root_storage[32];
  gc_temp_root_scope_t temp_roots;
  gc_temp_root_scope_begin_with(js, &temp_roots, root_storage, 32);
  
  size_t len;
  char *json_str = js_getstr(js, text, &len);

  _Alignas(16) char pool[4096];
  yyjson_alc pool_alc;
  size_t pool_need = yyjson_read_max_memory_usage(len, 0);
  bool use_pool = pool_need && pool_need <= sizeof(pool) && yyjson_alc_pool_init(&pool_alc, pool, sizeof(pool));
  yyjson_doc *doc = json_read_doc(json_str, len, use_pool ? &pool_alc : NULL);

  if (!doc) {
    gc_temp_root_scope_end(&temp_roots);
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "JSON.parse: unexpected character");
  }
  
  ant_value_t result = yyjson_to_jsval(js, yyjson_doc_get_root(doc), &temp_roots, NULL);
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
    
    ant_value_t root = Ant_Object_CreateBare(js);
    if (is_err(root)) {
      gc_temp_root_scope_end(&temp_roots);
      return root;
    }
    
    if (!json_temp_pin(&temp_roots, root)) {
      gc_temp_root_scope_end(&temp_roots);
      return json_parse_oom(js);
    }
    
    js_set(js, root, "", result);
    result = apply_reviver(js, root, "", 0, reviver, &temp_roots);
  }
  
  gc_temp_root_scope_end(&temp_roots);
  return result;
}

ant_value_t json_parse_value(ant_t *js, ant_value_t value) {
  ant_value_t args[1] = { value };
  return js_json_parse(js, args, 1, Ant_Value_Undefined, Ant_Value_Undefined, Ant_Value_Undefined);
}

static ant_value_t json_property_list_item(ant_t *js, ant_value_t item) {
  uint8_t t = vtype(item);
  if (t == kTypeString) return item;
  if (t == kTypeObject) {
    uint8_t prim = vtype(js_get_slot(item, SLOT_PRIMITIVE));
    if (prim != kTypeString && prim != kTypeNumber) return Ant_Value_Undefined;
  } else if (t != kTypeNumber) return Ant_Value_Undefined;
  return coerce_to_str(js, item);
}

static bool json_property_list_has(ant_t *js, ant_value_t list, ant_value_t item) {
  size_t item_len = 0;
  
  const char *item_str = js_getstr(js, item, &item_len);
  ant_offset_t count = js_arr_len(js, list);

  for (ant_offset_t i = 0; i < count; i++) {
    size_t len = 0;
    const char *str = js_getstr(js, js_arr_get(js, list, i), &len);
    if (key_matches(str, len, item_str, item_len)) return true;
  }

  return false;
}

static bool json_set_replacer(ant_t *js, json_cycle_ctx *ctx, ant_value_t replacer, ant_value_t *error_out) {
  if (is_callable(replacer)) {
    ctx->replacer_func = replacer;
    if (json_ctx_pin_value(ctx, replacer)) return true;
    *error_out = ctx->error;
    return false;
  }
  
  if (!is_object_type(replacer)) return true;

  bool replacer_is_array = false;
  ant_value_t checked = js_is_array_value_checked(js, replacer, &replacer_is_array);
  
  if (is_err(checked)) {
    *error_out = checked;
    return false;
  }
  
  if (!replacer_is_array) return true;
  ant_value_t len_val = js_get(js, replacer, "length");
  
  if (is_err(len_val)) {
    json_capture_error(ctx, len_val);
    *error_out = js_throw(js, ctx->error);
    return false;
  }
  
  if (vtype(len_val) != kTypeNumber) return true;

  ant_value_t list = Ant_Array_Create(js);
  if (is_err(list)) {
    *error_out = list;
    return false;
  }

  if (!json_ctx_pin_value(ctx, list)) {
    *error_out = ctx->error;
    return false;
  }

  double len = Ant_Value_AsNumberUnchecked(len_val);
  ant_offset_t count = len > 0 ? (ant_offset_t)len : 0;

  for (ant_offset_t i = 0; i < count; i++) {
    char idxstr[32];
    uint_to_str(idxstr, sizeof(idxstr), (uint64_t)i);

    ant_value_t item = js_get(js, replacer, idxstr);
    if (!is_err(item)) item = json_property_list_item(js, item);
    if (is_err(item)) {
      *error_out = item;
      return false;
    }

    if (vtype(item) == kTypeString && !json_property_list_has(js, list, item)) js_arr_push(js, list, item);
  }

  ctx->property_list = list;
  return true;
}

static bool json_set_indent(ant_t *js, json_cycle_ctx *ctx, ant_args_t) {
  ctx->indent_len = 0;
  if (nargs < 3) return true;
  ant_value_t space = args[2];

  if (is_special_object(space)) {
    uint8_t boxed = vtype(js_get_slot(space, SLOT_PRIMITIVE));
    if (boxed != kTypeNumber && boxed != kTypeString) return true;

    ant_value_t prim = boxed == kTypeNumber
      ? js_to_primitive(js, space, 0)
      : js_tostring_val(js, space);

    if (is_err(prim) || Ant_Exception_Pending(js)) {
      json_capture_error(ctx, prim);
      return false;
    }
    space = prim;
  }

  if (vtype(space) == kTypeNumber) {
    double d = Ant_Value_AsNumberUnchecked(space);
    if (isnan(d) || d < 1) return true;
    size_t n = d > 10 ? 10 : (size_t)d;
    memset(ctx->indent, ' ', n);
    ctx->indent_len = n;
    return true;
  }

  if (vtype(space) != kTypeString) return true;

  size_t byte_len = 0;
  char *str = js_getstr(js, space, &byte_len);
  if (!str || !byte_len) return true;

  size_t units = (size_t)str_utf16_len(js, space);
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

ant_value_t js_json_stringify(ant_params_t) {
  ant_value_t result;
  json_out_t out;
  
  out.buf = out.inline_buf;
  out.len = 0;
  out.cap = sizeof(out.inline_buf);
  out.oom = false;
  
  json_cycle_ctx ctx;
  memset(&ctx, 0, offsetof(json_cycle_ctx, cycle_key));
  
  ctx.js = js;
  ctx.replacer_func = Ant_Value_Undefined;
  ctx.property_list = Ant_Value_Undefined;
  ctx.error = Ant_Value_Undefined;
  ctx.holder = Ant_Value_Undefined;
  ctx.cycle_start = Ant_Value_Undefined;
  ctx.stack_cap = 32;
  ctx.stack = ctx.stack_inline;
  
  ant_value_t value = nargs > 0 ? args[0] : Ant_Value_Undefined;
  ctx.tojson_key = js->intern.to_json;
  ant_value_t root_storage[32];
  
  gc_temp_root_scope_begin_with(js, &ctx.temp_roots, root_storage, 32);
  ctx.error_handle = gc_temp_root_add(&ctx.temp_roots, ctx.error);
  ctx.holder_handle = gc_temp_root_add(&ctx.temp_roots, ctx.holder);
  
  if (!gc_temp_root_handle_valid(ctx.error_handle) || !gc_temp_root_handle_valid(ctx.holder_handle)) {
    gc_temp_root_scope_end(&ctx.temp_roots);
    return json_stringify_oom(js);
  }
  
  if (!json_ctx_pin_value(&ctx, value)) {
    result = ctx.error;
    goto cleanup;
  }
  
  if (nargs >= 2 && !json_set_replacer(js, &ctx, args[1], &result)) goto cleanup;
  
  if (!json_set_indent(js, &ctx, args, nargs)) {
    ant_value_t error = ctx.error;
    result = is_err(error) ? error : js_throw(js, error);
    goto cleanup;
  }

  if (is_callable(ctx.replacer_func)) {
    ant_value_t root_holder = json_create_root_holder(js, value, &ctx);
    if (is_err(root_holder) || ctx.has_error) {
      result = is_err(root_holder) ? root_holder : ctx.error;
      goto cleanup;
    }
    json_set_holder(&ctx, root_holder);
  }

  json_write_t root = json_write_with_key(&ctx, &out, "", 0, value, 0, 0);
  
  if (ctx.has_error) {
    ant_value_t error = ctx.error;
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
    result = Ant_Value_Undefined;
    goto cleanup;
  }

  result = js_mkstr(js, out.buf, out.len);

cleanup:
  json_plans_free(&ctx);
  if (out.buf != out.inline_buf) free(out.buf);
  if (ctx.stack != ctx.stack_inline) free(ctx.stack);
  gc_temp_root_scope_end(&ctx.temp_roots);
  return result;
}

ant_value_t json_stringify_value(ant_t *js, ant_value_t value) {
  ant_value_t args[1] = { value };
  return js_json_stringify(js, args, 1, Ant_Value_Undefined, Ant_Value_Undefined, Ant_Value_Undefined);
}

void init_json_module(ant_t *js) {
  ant_value_t json_obj = Ant_Object_Create(js);
  
  defmethod(js, json_obj, "parse", 5, Ant_Function_CreateWithArity(js_json_parse, 2));
  defmethod(js, json_obj, "stringify", 9, Ant_Function_CreateWithArity(js_json_stringify, 3));
  
  js_set_tostringtag(js, json_obj, js_mkstr(js, "JSON", 4));
  js_set_global_builtin(js, "JSON", json_obj);
}
