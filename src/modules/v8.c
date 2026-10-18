#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "arena.h"
#include "cfunc.h"
#include "errors.h"
#include "internal.h"
#include "modules/buffer.h"
#include "silver/call.h"

// serialize / deserialize
//
// wire format:
//   [0xAB 0xCD]  — magic header (2 bytes)
//   <value>      — recursively encoded
//
// value tags (1 byte):
//   'U'  undefined
//   'N'  null
//   'T'  true
//   'F'  false
//   'Z'  NaN
//   'D'  double  (8 bytes little-endian)
//   'S'  string  (uint32 length LE + UTF-8 bytes)
//   'A'  array   (uint32 count LE + <value>…)
//   'O'  object  (uint32 count LE + (uint32 klen + key bytes + <value>)…)

#define SER_MAGIC_0 0xAB
#define SER_MAGIC_1 0xCD

typedef struct {
  uint8_t *data;
  size_t   len;
  size_t   cap;
} enc_t;

static bool enc_grow(enc_t *e, size_t need) {
  if (e->len + need <= e->cap) return true;
  size_t nc = e->cap ? e->cap * 2 : 64;
  while (nc < e->len + need) nc *= 2;
  uint8_t *p = realloc(e->data, nc);
  if (!p) return false;
  e->data = p;
  e->cap  = nc;
  return true;
}

static bool enc_u8(enc_t *e, uint8_t b) {
  if (!enc_grow(e, 1)) return false;
  e->data[e->len++] = b;
  return true;
}

static bool enc_u32(enc_t *e, uint32_t v) {
  if (!enc_grow(e, 4)) return false;
  e->data[e->len++] = (uint8_t)(v);
  e->data[e->len++] = (uint8_t)(v >> 8);
  e->data[e->len++] = (uint8_t)(v >> 16);
  e->data[e->len++] = (uint8_t)(v >> 24);
  return true;
}

static void enc_u32_at(enc_t *e, size_t at, uint32_t v) {
  e->data[at] = (uint8_t)(v);
  e->data[at + 1] = (uint8_t)(v >> 8);
  e->data[at + 2] = (uint8_t)(v >> 16);
  e->data[at + 3] = (uint8_t)(v >> 24);
}

static bool enc_bytes(enc_t *e, const void *src, size_t n) {
  if (!enc_grow(e, n)) return false;
  memcpy(e->data + e->len, src, n);
  e->len += n;
  return true;
}

typedef enum {
  SER_OK = 0,
  SER_OOM,
  SER_TOO_DEEP,
  SER_UNCLONEABLE,
} ser_status_t;

#define SER_TRY(expr) do { if (!(expr)) return SER_OOM; } while (0)

static ser_status_t ser_val(ant_t *js, enc_t *e, ant_value_t val, ant_value_t *bad);

static ser_status_t ser_object(ant_t *js, enc_t *e, ant_value_t obj, ant_value_t *bad) {
  SER_TRY(enc_u8(e, 'O'));
  size_t count_at = e->len;
  SER_TRY(enc_u32(e, 0));

  uint32_t count = 0;
  ser_status_t st = SER_OK;
  const char *k; size_t klen; ant_value_t v;
  ant_iter_t it = js_prop_iter_begin(js, obj);

  while (st == SER_OK && js_prop_iter_next(&it, &k, &klen, &v)) {
    st = enc_u32(e, (uint32_t)klen) && enc_bytes(e, k, klen) ? ser_val(js, e, v, bad) : SER_OOM;
    count++;
  }
  
  js_prop_iter_end(&it);
  enc_u32_at(e, count_at, count);
  
  return st;
}

static ser_status_t ser_val(ant_t *js, enc_t *e, ant_value_t val, ant_value_t *bad) {
  if (sv_check_c_stack_overflow(js)) return SER_TOO_DEEP;
  uint8_t t = vtype(val);

  if (t == kTypeUndefined) return enc_u8(e, 'U') ? SER_OK : SER_OOM;
  if (t == kTypeNull) return enc_u8(e, 'N') ? SER_OK : SER_OOM;
  if (t == kTypeBool) return enc_u8(e, (val == js_true) ? 'T' : 'F') ? SER_OK : SER_OOM;

  if (t == kTypeNumber) {
    double d = js_getnum(val);
    if (isnan(d)) return enc_u8(e, 'Z') ? SER_OK : SER_OOM;
    SER_TRY(enc_u8(e, 'D'));
    return enc_bytes(e, &d, 8) ? SER_OK : SER_OOM;
  }

  if (t == kTypeString) {
    size_t slen;
    const char *s = js_getstr(js, val, &slen);
    if (!s) { s = ""; slen = 0; }
    SER_TRY(enc_u8(e, 'S'));
    SER_TRY(enc_u32(e, (uint32_t)slen));
    return enc_bytes(e, s, slen) ? SER_OK : SER_OOM;
  }

  if (js_is_array(val)) {
    ant_offset_t n = js_arr_len(js, val);
    SER_TRY(enc_u8(e, 'A'));
    SER_TRY(enc_u32(e, (uint32_t)n));
    for (ant_offset_t i = 0; i < n; i++) {
      ser_status_t st = ser_val(js, e, js_arr_get(js, val, i), bad);
      if (st != SER_OK) return st;
    }
    
    return SER_OK;
  }

  if (t == kTypeObject) return ser_object(js, e, val, bad);

  if (t == kTypeSymbol || t == kTypeFunction || t == kTypeBuiltin || js_is_arguments(val)) {
    *bad = val;
    return SER_UNCLONEABLE;
  }

  return enc_u8(e, 'U') ? SER_OK : SER_OOM;
}

#undef SER_TRY

static ant_value_t v8_serialize_error(ant_t *js, ser_status_t st, ant_value_t bad) {
  if (st == SER_TOO_DEEP) 
    return js_mkerr_typed(js, JS_ERR_RANGE, "Maximum call stack size exceeded");
  
  if (st != SER_UNCLONEABLE) 
    return js_mkerr(js, "serialize: out of memory");
  
  if (js_is_arguments(bad)) 
    return js_mkerr_typed(js, JS_ERR_GENERIC, "#<Object> could not be cloned.");
  
  if (vtype(bad) == kTypeSymbol) 
    return js_mkerr_typed(js, JS_ERR_GENERIC, "%s could not be cloned.", js_str(js, bad));

  ant_value_t str = js_tostring_val(js, bad);
  if (is_err(str)) return str;
  
  size_t len = 0;
  const char *text = js_getstr(js, str, &len);
  
  return js_mkerr_typed(js, JS_ERR_GENERIC, "%.*s could not be cloned.", (int)len, text ? text : "");
}

static ant_value_t v8_serialize(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "serialize: value required");

  enc_t e = {0};
  ant_value_t bad = js_mkundef();
  ser_status_t st = SER_OOM;
  
  if (!enc_u8(&e, SER_MAGIC_0) || !enc_u8(&e, SER_MAGIC_1)) goto fail;
  st = ser_val(js, &e, args[0], &bad);
  if (st != SER_OK) goto fail;

  ArrayBufferData *ab = create_array_buffer_data(js, e.len);
  if (!ab) {
    st = SER_OOM;
    goto fail;
  }
  
  memcpy(ab->data, e.data, e.len);
  free(e.data);
  return create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, e.len, js->builtins.buffer_proto);

fail:
  free(e.data);
  return v8_serialize_error(js, st, bad);
}

typedef struct {
  const uint8_t *data;
  size_t len;
  size_t pos;
} dec_t;

static bool dec_u8(dec_t *d, uint8_t *out) {
  if (d->pos >= d->len) return false;
  *out = d->data[d->pos++];
  return true;
}

static bool dec_u32(dec_t *d, uint32_t *out) {
  if (d->pos + 4 > d->len) return false;
  *out = (uint32_t)d->data[d->pos]
    | ((uint32_t)d->data[d->pos+1] << 8)
    | ((uint32_t)d->data[d->pos+2] << 16)
    | ((uint32_t)d->data[d->pos+3] << 24);
  d->pos += 4;
  return true;
}

static ant_value_t des_val(ant_t *js, dec_t *d) {
  if (sv_check_c_stack_overflow(js)) return Ant_Error_CreateStackOverflow(js);
  
  uint8_t tag;
  if (!dec_u8(d, &tag)) return js_mkundef();

  switch (tag) {
    case 'U': return js_mkundef();
    case 'N': return js_mknull();
    case 'T': return js_true;
    case 'F': return js_false;
    case 'Z': return js_mknum((double)NAN);
    case 'D': {
      if (d->pos + 8 > d->len) return js_mkundef();
      double v;
      memcpy(&v, d->data + d->pos, 8);
      d->pos += 8;
      return js_mknum(v);
    }
    case 'S': {
      uint32_t n;
      if (!dec_u32(d, &n) || d->pos + n > d->len) return js_mkundef();
      ant_value_t s = js_mkstr(js, (const char *)(d->data + d->pos), n);
      d->pos += n;
      return s;
    }
    case 'A': {
      uint32_t n;
      if (!dec_u32(d, &n)) return js_mkundef();
      ant_value_t arr = js_mkarr(js);
      for (uint32_t i = 0; i < n; i++) {
        ant_value_t elem = des_val(js, d);
        if (is_err(elem)) return elem;
        js_arr_push(js, arr, elem);
      }
      return arr;
    }
    case 'O': {
      uint32_t n;
      if (!dec_u32(d, &n)) return js_mkundef();
      ant_value_t obj = Ant_Object_Create(js);
      char kbuf[4096];
      for (uint32_t i = 0; i < n; i++) {
        uint32_t klen;
        if (!dec_u32(d, &klen) || klen >= sizeof(kbuf) || d->pos + klen > d->len) break;
        memcpy(kbuf, d->data + d->pos, klen);
        kbuf[klen] = '\0';
        d->pos += klen;
        ant_value_t v = des_val(js, d);
        if (is_err(v)) return v;
        js_set(js, obj, kbuf, v);
      }
      return obj;
    }
    default: return js_mkundef();
  }
}

static ant_value_t v8_deserialize(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "deserialize: Buffer required");

  uint8_t *data; size_t len;
  if (!Ant_Buffer_GetViewBytes(buffer_get_typedarray_data(args[0]), &data, &len))
    return js_mkerr_typed(js, JS_ERR_TYPE, "deserialize: expected an attached Buffer");

  if (len < 2 || data[0] != SER_MAGIC_0 || data[1] != SER_MAGIC_1)
    return js_mkerr(js, "deserialize: invalid or incompatible buffer");

  dec_t d = { .data = data, .len = len, .pos = 2 };
  return des_val(js, &d);
}

static ant_value_t v8_get_heap_statistics(ant_params_t) {
  size_t rss = 0;
  uv_resident_set_memory(&rss);

  size_t arena_committed  = js->obj_arena.committed;
  size_t arena_reserved   = js->obj_arena.reserved;
  size_t arena_live_bytes = js->obj_arena.live_count * js->obj_arena.elem_size;

  size_t closure_committed = js->closure_arena.committed;
  size_t closure_reserved  = js->closure_arena.reserved;
  size_t closure_live      = js->closure_arena.live_count * js->closure_arena.elem_size;

  size_t pool_live  = js->gc.pool_last_live;
  size_t pool_alloc = js->gc.pool_alloc;
  size_t pool_total = pool_live + pool_alloc;

  size_t extra_alloc = js->alloc_bytes.closures + js->alloc_bytes.upvalues;

  size_t used_heap  = arena_live_bytes + closure_live + pool_total + extra_alloc;
  size_t total_heap = arena_committed + closure_committed + pool_total + extra_alloc;
  size_t heap_limit = arena_reserved + closure_reserved;

  ant_value_t obj = Ant_Object_Create(js);
  js_set(js, obj, "total_heap_size",             js_mknum((double)total_heap));
  js_set(js, obj, "total_heap_size_executable",  js_mknum(0));
  js_set(js, obj, "total_physical_size",         js_mknum((double)rss));
  js_set(js, obj, "total_available_size",        js_mknum((double)(heap_limit > used_heap ? heap_limit - used_heap : 0)));
  js_set(js, obj, "total_global_handles_size",   js_mknum(0));
  js_set(js, obj, "used_global_handles_size",    js_mknum(0));
  js_set(js, obj, "used_heap_size",              js_mknum((double)used_heap));
  js_set(js, obj, "heap_size_limit",             js_mknum((double)heap_limit));
  js_set(js, obj, "malloced_memory",             js_mknum((double)extra_alloc));
  js_set(js, obj, "peak_malloced_memory",        js_mknum((double)extra_alloc));
  js_set(js, obj, "does_zap_garbage",            js_mknum(0));
  js_set(js, obj, "number_of_native_contexts",   js_mknum(1));
  js_set(js, obj, "number_of_detached_contexts", js_mknum(0));
  js_set(js, obj, "total_heap_blinded_size",     js_mknum(0));
  
  return obj;
}

static ant_value_t v8_get_heap_space_statistics(ant_params_t) {
  ant_value_t nursery = Ant_Object_Create(js);
  size_t arena_committed  = js->obj_arena.committed;
  size_t arena_live_bytes = js->obj_arena.live_count * js->obj_arena.elem_size;
  double arena_available = (double)(arena_committed / 2 > arena_live_bytes / 2 ? arena_committed / 2 - arena_live_bytes / 2 : 0);

  js_set(js, nursery, "space_name",            js_mkstr(js, "new_space", 9));
  js_set(js, nursery, "space_size",            js_mknum((double)arena_committed / 2));
  js_set(js, nursery, "space_used_size",       js_mknum((double)arena_live_bytes / 2));
  js_set(js, nursery, "space_available_size",  js_mknum(arena_available));
  js_set(js, nursery, "physical_space_size",   js_mknum((double)arena_committed / 2));

  ant_value_t oldspace = Ant_Object_Create(js);
  size_t old_live = js->old_live_count * js->obj_arena.elem_size;
  double old_size = (double)(arena_committed / 2 > old_live ? arena_committed / 2 - old_live : 0);
  
  js_set(js, oldspace, "space_name",           js_mkstr(js, "old_space", 9));
  js_set(js, oldspace, "space_size",           js_mknum((double)arena_committed / 2));
  js_set(js, oldspace, "space_used_size",      js_mknum((double)old_live));
  js_set(js, oldspace, "space_available_size", js_mknum(old_size));
  js_set(js, oldspace, "physical_space_size",  js_mknum((double)arena_committed / 2));

  ant_value_t arr = js_mkarr(js);
  js_arr_push(js, arr, nursery);
  js_arr_push(js, arr, oldspace);
  
  return arr;
}

static ant_value_t v8_noop(ant_params_t) {
  return js_mkundef();
}

static ant_value_t v8_noop_false(ant_params_t) {
  return js_false;
}

static ant_value_t v8_write_heap_snapshot(ant_params_t) {
  return js_mkstr(js, "", 0);
}

static ant_value_t v8_get_heap_snapshot(ant_params_t) {
  return js_mkundef();
}

static ant_value_t v8_get_heap_code_statistics(ant_params_t) {
  size_t closure_structs = js->closure_arena.live_count * js->closure_arena.elem_size;
  size_t bytecode_alloc  = js->alloc_bytes.closures;

  ant_pool_stats_t rope_stats = js_rope_pool_stats(js);
  ant_value_t obj = Ant_Object_Create(js);
  
  js_set(js, obj, "code_and_metadata_size",      js_mknum((double)(closure_structs + bytecode_alloc)));
  js_set(js, obj, "bytecode_and_metadata_size",  js_mknum((double)bytecode_alloc));
  js_set(js, obj, "external_script_source_size", js_mknum((double)rope_stats.used));
  js_set(js, obj, "cpu_profiler_metadata_size",  js_mknum(0));
  
  return obj;
}

static ant_value_t v8_cached_data_version_tag(ant_params_t) {
  return js_mknum(0xA0A0A0);
}

ant_value_t v8_library(ant_t *js) {
  ant_value_t lib = Ant_Object_CreateBare(js);

  js_set(js, lib, "serialize",              Ant_Function_CreateWithArity(v8_serialize, 1));
  js_set(js, lib, "deserialize",            Ant_Function_CreateWithArity(v8_deserialize, 1));
  js_set(js, lib, "writeHeapSnapshot",      Ant_Function_CreateWithArity(v8_write_heap_snapshot, 2));
  js_set(js, lib, "getHeapSnapshot",        Ant_Function_CreateWithArity(v8_get_heap_snapshot, 1));
  js_set(js, lib, "getHeapStatistics",      Ant_Function_Create(v8_get_heap_statistics));
  js_set(js, lib, "getHeapSpaceStatistics", Ant_Function_Create(v8_get_heap_space_statistics));
  js_set(js, lib, "getHeapCodeStatistics",  Ant_Function_Create(v8_get_heap_code_statistics));
  js_set(js, lib, "cachedDataVersionTag",   Ant_Function_Create(v8_cached_data_version_tag));
  js_set(js, lib, "setFlagsFromString",     Ant_Function_CreateWithArity(v8_noop, 1));
  js_set(js, lib, "stopCoverage",           Ant_Function_CreateWithArity(v8_noop, 1));
  js_set(js, lib, "takeCoverage",           Ant_Function_CreateWithArity(v8_noop, 1));
  js_set(js, lib, "promiseEvents",          Ant_Function_CreateWithArity(v8_noop, 1));

  ant_value_t snapshot = Ant_Object_CreateBare(js);
  js_set(js, snapshot, "isBuildingSnapshot",      Ant_Function_Create(v8_noop_false));
  js_set(js, snapshot, "addSerializeCallback",    Ant_Function_CreateWithArity(v8_noop, 1));
  js_set(js, snapshot, "addDeserializeCallback",  Ant_Function_CreateWithArity(v8_noop, 1));
  
  js_set(js, lib, "startupSnapshot", snapshot);
  js_set(js, lib, "constants", Ant_Object_CreateBare(js));

  return lib;
}
