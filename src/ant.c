#if defined(__GNUC__) && !defined(__clang__)
  #pragma GCC optimize("O3,inline")
#endif

#include <limits.h>
#include <compat.h> // IWYU pragma: keep

#include "ant.h"
#include "utf8.h"
#include "ptr.h"
#include "cfunc.h"
#include "debug.h"
#include "tokens.h"
#include "common.h"
#include "sugar.h"
#include "base64.h"
#include "builder.h"
#include "stringify.h"
#include "runtime.h"
#include "loop.h"
#include "internal.h"
#include "errors.h"
#include "handles.h"
#include "descriptors.h"
#include "shapes.h"
#include "numbers.h"

#include "gc.h"
#include "gc/objects.h"
#include "gc/roots.h"
#include "realm.h"
#include "bootstrap.h"
#include "gc/weak.h"
#include "gc/bigints.h"

#include "esm/remote.h"
#include "esm/loader.h"
#include "esm/exports.h"
#include "esm/builtin_bundle.h"

#include "silver/lexer.h"
#include "silver/compiler.h"
#include "silver/call.h"
#include "silver/glue.h"
#include "silver/jit.h"
#include "silver/ops/using.h"
#include "silver/ops/async.h"
#include "silver/ops/eval_env.h"

#include "modules/regex.h"
#include "modules/domexception.h"
#include "modules/json.h"
#include "modules/collections.h"
#include "modules/process.h"
#include "modules/abort.h"
#include "modules/eventsource.h"
#include "modules/websocket.h"
#include "modules/dns.h"

#ifndef ANT_WASM_EMBED
#include <uv.h>
#endif
#include <assert.h>
#include <pcre2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <utarray.h>
#include <uthash.h>
#include <float.h>
#ifndef ANT_WASM_EMBED
#include <tlsuv/tlsuv.h>
#include <tlsuv/http.h>
#endif

#ifdef _WIN32
#include <sys/stat.h>
#else
#include <sys/time.h>
#include <sys/stat.h>
#ifndef ANT_WASM_EMBED
#include <sys/resource.h>
#endif
#endif

#include "modules/atomics.h"
#include "modules/bigint.h"
#include "modules/timer.h"
#include "modules/symbol.h"
#include "modules/iterator.h"
#include "modules/date.h"
#include "modules/buffer.h"
#include "modules/events.h"
#include "modules/blob.h"
#include "modules/cron.h"
#include "modules/lmdb.h"
#include "modules/regex.h"
#include "modules/globals.h"
#include "modules/rpc.h"

#define D(x) ((double)(x))

static_assert(sizeof(double) == 8, "NaN-boxing requires 64-bit IEEE 754 doubles");
static_assert(sizeof(uint64_t) == 8, "NaN-boxing requires 64-bit integers");
static_assert(sizeof(double) == sizeof(uint64_t), "double and uint64_t must have same size");

#if defined(__STDC_IEC_559__) || defined(__GCC_IEC_559)
#elif defined(__FAST_MATH__)
  #error "NaN-boxing is incompatible with -ffast-math"
#elif DBL_MANT_DIG != 53 || DBL_MAX_EXP != 1024
  #error "NaN-boxing requires IEEE 754 binary64 doubles"
#endif

typedef struct interned_string {
  uint64_t hash;
  char *str;
  size_t len;
  struct interned_string *next;
} interned_string_t;

typedef struct {
  uint64_t gc_epoch;
  const char *key;
  uint32_t flags;
  uint32_t desc_len;
  char desc[];
} ant_symbol_heap_t;

static size_t intern_count = 0;
static size_t intern_bytes = 0;

static interned_string_t **intern_buckets = NULL;
static size_t intern_bucket_count = 0;

#define INTERN_BUCKET_MIN 1024u
#define INTERN_LOAD_NUM   4u
#define INTERN_LOAD_DEN   5u

static bool intern_table_init(void) {
  if (intern_buckets) return true;
  intern_bucket_count = INTERN_BUCKET_MIN;
  intern_buckets = calloc(1, sizeof(*intern_buckets) * intern_bucket_count);
  if (!intern_buckets) {
    intern_bucket_count = 0;
    return false;
  }
  return true;
}

static bool intern_table_rehash(size_t new_bucket_count) {
  if (!intern_buckets || new_bucket_count < INTERN_BUCKET_MIN) return false;

  interned_string_t **next = calloc(1, sizeof(*next) * new_bucket_count);
  if (!next) return false;

  for (size_t i = 0; i < intern_bucket_count; i++) {
    interned_string_t *entry = intern_buckets[i];
    while (entry) {
      interned_string_t *link = entry->next;
      size_t bucket = (size_t)(entry->hash & (new_bucket_count - 1));
      entry->next = next[bucket];
      next[bucket] = entry;
      entry = link;
    }
  }

  free(intern_buckets);
  intern_buckets = next;
  intern_bucket_count = new_bucket_count;

  return true;
}

static const UT_icd promise_handler_icd = {
  .sz = sizeof(promise_handler_t),
  .init = NULL,
  .copy = NULL,
  .dtor = NULL,
};

static uint32_t next_promise_id = 1;
static uint32_t get_promise_id(ant_t *js, ant_value_t p);

static ant_promise_state_t *get_promise_data(ant_t *js, ant_value_t promise, bool create);
static ant_proxy_state_t *get_proxy_data(ant_value_t obj);

static inline uint32_t promise_handler_count(const ant_promise_state_t *pd) {
  if (!pd) return 0;
  if (pd->has_inline_handler) return 1;
  return pd->handlers ? utarray_len(pd->handlers) : 0;
}

static inline bool promise_has_handlers(const ant_promise_state_t *pd) {
  return promise_handler_count(pd) != 0;
}

static bool promise_handler_append(ant_promise_state_t *pd, const promise_handler_t *handler) {
  if (!pd || !handler) return false;

  uint32_t count = promise_handler_count(pd);
  if (count == 0) {
    pd->inline_handler = *handler;
    pd->has_inline_handler = true;
    return true;
  }

  if (!pd->handlers) utarray_new(pd->handlers, &promise_handler_icd);
  if (!pd->handlers) return false;
  
  if (count == 1) {
    utarray_push_back(pd->handlers, &pd->inline_handler);
    pd->inline_handler = (promise_handler_t){ 0 };
    pd->has_inline_handler = false;
  }
  
  utarray_push_back(pd->handlers, handler);
  return true;
}

static inline promise_handler_t *promise_handler_at(ant_promise_state_t *pd, uint32_t index) {
  if (!pd || index >= promise_handler_count(pd)) return NULL;
  if (pd->has_inline_handler) return &pd->inline_handler;
  return (promise_handler_t *)utarray_eltptr(pd->handlers, (unsigned int)index);
}

static inline void promise_handlers_clear(ant_promise_state_t *pd) {
  if (!pd) return;
  pd->has_inline_handler = false;
  pd->inline_handler = (promise_handler_t){ 0 };
  if (pd->handlers) utarray_clear(pd->handlers);
}

static void promise_handlers_discard_processed_prefix(
  ant_promise_state_t *pd, uint32_t count
) {
  uint32_t total = promise_handler_count(pd);
  ANT_ASSERT(count <= total, "invalid Promise handler prefix");
  
  if (count == total) {
    promise_handlers_clear(pd);
    return;
  }

  utarray_erase(pd->handlers, 0, count);
  
  if (total - count == 1) {
    pd->inline_handler = *(promise_handler_t *)utarray_front(pd->handlers);
    pd->has_inline_handler = true;
    utarray_clear(pd->handlers);
  }
}

ant_object_t *js_obj_ptr(ant_value_t v) {
  if (!is_object_type(v)) return NULL;
  ant_value_t as_obj = js_as_obj(v);
  return (ant_object_t *)vptr(as_obj);
}

ant_value_t js_obj_from_ptr(ant_object_t *obj) {
  if (!obj) return js_mkundef();
  return mkref(kTypeObject, obj);
}

void js_mark_constructor(ant_value_t value, bool is_constructor) {
  ant_object_t *obj = js_obj_ptr(value);
  if (obj) obj->flags.is_constructor = is_constructor ? 1u : 0u;
}

static ant_value_t obj_extra_get(ant_object_t *obj, internal_slot_t slot) {
  ant_extra_slot_t *entry = ant_object_extra_slot(obj, (uint8_t)slot);
  return entry ? entry->value : js_mkundef();
}

static bool obj_extra_reserve(ant_object_t *obj, uint8_t capacity) {
  if (!obj) return false;
  ant_object_sidecar_t *sidecar = ant_object_sidecar(obj);
  uint8_t current_capacity = sidecar ? sidecar->extra_cap : obj->extra_cap;
  ant_extra_slot_t *entries = ant_object_extra_slots_ptr(obj);

  if (capacity <= current_capacity) return true;
  ant_extra_slot_t *next = realloc(entries, sizeof(*next) * capacity);
  if (!next) return false;

  if (sidecar) {
    sidecar->extra_slots = next;
    sidecar->extra_cap = capacity;
  } else {
    obj->extra_slots = next;
    obj->extra_cap = capacity;
  }
  return true;
}

static bool obj_extra_set(ant_object_t *obj, internal_slot_t slot, ant_value_t value) {
  if (!obj) return false;
  
  ant_extra_slot_t *entry = ant_object_extra_slot(obj, (uint8_t)slot);
  if (entry) {
    entry->value = value;
    return true;
  }

  uint8_t count = 0;
  ant_object_sidecar_t *sidecar = ant_object_sidecar(obj);
  ant_extra_slot_t *entries = ant_object_extra_slots(obj, &count);
  uint8_t capacity = sidecar ? sidecar->extra_cap : obj->extra_cap;
  
  if (count == UINT8_MAX) return false;
  uint8_t next_count = (uint8_t)(count + 1);

  if (next_count > capacity) {
    if (!obj_extra_reserve(obj, next_count)) return false;
    entries = ant_object_extra_slots_ptr(obj);
  }

  entries[count].slot = (uint8_t)slot;
  entries[count].value = value;
  
  if (sidecar) sidecar->extra_count = next_count;
  else obj->extra_count = next_count;
  
  return true;
}

ant_value_t js_prop_load(ant_prop_loc_t loc) {
  if (!loc.obj || loc.slot >= loc.obj->prop_count) return js_mkundef();
  return ant_object_prop_get_unchecked(loc.obj, loc.slot);
}

bool js_prop_store(ant_t *js, ant_prop_loc_t loc, ant_value_t value) {
  if (!loc.obj || loc.slot >= loc.obj->prop_count) return false;
  ant_object_prop_set_unchecked(loc.obj, loc.slot, value);
  gc_write_barrier(js, loc.obj, value);
  const ant_shape_prop_t *prop = ant_shape_prop_at(loc.obj->shape, loc.slot);
  if (prop && prop->type == ANT_SHAPE_KEY_STRING) 
    ant_property_mutation_invalidate(js, loc.obj, prop->key.interned);
  return true;
}

bool js_obj_ensure_prop_capacity(ant_t *js, ant_object_t *obj, uint32_t needed) {
  if (!obj) return false;
  uint32_t inobj_limit = ant_object_inobj_limit(obj);
  uint32_t old_count = obj->prop_count;
  if (needed <= obj->prop_count) return true;

  if (needed > inobj_limit) {
    uint32_t overflow_needed = needed - inobj_limit;
    if (overflow_needed > ant_object_overflow_cap(obj)) {
      uint8_t shift = obj->overflow_cap ? (uint8_t)(obj->overflow_cap + 1) : 2;
      if (shift > 31) return false;

      while (shift < 31 && ((uint32_t)1 << shift) < overflow_needed) shift++;
      if (((uint32_t)1 << shift) < overflow_needed) return false;

      size_t slots = (size_t)1 << shift;
      if (slots > SIZE_MAX / sizeof(ant_value_t)) return false;

      size_t old_slots = ant_object_overflow_cap(obj);
      ant_value_t *next = realloc(obj->overflow_prop, sizeof(*next) * slots);
      if (!next) return false;
      
      gc_note_overflow_alloc(js, sizeof(*next) * (slots - old_slots));
      obj->overflow_prop = next;
      obj->overflow_cap = shift;
    }
  } else if (obj->overflow_prop) {
    gc_note_overflow_free(js, ant_object_overflow_cap(obj) * sizeof(*obj->overflow_prop));
    free(obj->overflow_prop);
    obj->overflow_prop = NULL;
    obj->overflow_cap = 0;
  }

  obj->prop_count = needed;
  for (uint32_t i = old_count; i < needed; i++) {
    ant_object_prop_set_unchecked(obj, i, js_mkundef());
  }

  return true;
}

bool js_obj_ensure_unique_shape(ant_object_t *obj) {
  if (!obj || !obj->shape) return false;
  if (!ant_shape_is_shared(obj->shape)) return true;

  ant_shape_t *copy = ant_shape_clone(obj->shape);
  if (!copy) return false;

  ant_shape_release(obj->shape);
  obj->shape = copy;
  return true;
}

static void obj_delete_prop_slot(ant_object_t *obj, uint32_t slot) {
  if (!obj || slot >= obj->prop_count) return;
  ant_object_prop_set_unchecked(obj, slot, js_mkundef());

  uint32_t shape_count = ant_shape_count(obj->shape);
  if (!ant_shape_should_compact(obj->shape)) {
    obj->prop_count = shape_count;
    return;
  }

  uint32_t dst = 0;
  for (uint32_t src = 0; src < shape_count; src++) {
    if (!ant_shape_prop_at(obj->shape, src)) continue;
    if (dst != src)
      ant_object_prop_set_unchecked(obj, dst, ant_object_prop_get_unchecked(obj, src));
    dst++;
  }
  
  for (uint32_t i = dst; i < obj->prop_count; i++)
    ant_object_prop_set_unchecked(obj, i, js_mkundef());
  obj->prop_count = ant_shape_compact(obj->shape);
}

static ant_exotic_ops_t *obj_ensure_exotic_ops(ant_object_t *obj) {
  if (!obj) return NULL;
  
  ant_exotic_ops_t *owned = ant_object_owned_exotic_ops(obj);
  if (owned) return owned;
  
  ant_object_sidecar_t *sidecar = ant_object_ensure_sidecar(obj);
  if (!sidecar) return NULL;
  
  ant_exotic_ops_t *ops = realloc(sidecar->owned_exotic_ops, sizeof(*ops));
  if (!ops) return NULL;
  
  *ops = obj->exotic_ops 
    ? *obj->exotic_ops 
    : (ant_exotic_ops_t){0};
  
  ops->keys = ANT_EXOTIC_KEYS_ANY;
  sidecar->owned_exotic_ops = ops;
  obj->exotic_ops = ops;
  
  return ops;
}

static ant_object_t *obj_alloc(ant_t *js, uint8_t type_tag, uint8_t inobj_limit) {
  gc_alloc_check(js);

  ant_object_t *obj = (ant_object_t *)fixed_arena_alloc_uninit(&js->obj_arena);
  if (!obj) return NULL;

  obj->ic_identity = 0;
  obj->type_tag = type_tag;
  obj->proto = js_mkundef();
  
  if (type_tag == kTypeArray) obj->u.array.data = NULL;
  else obj->u.data.value = js_mkundef();
  
  obj->u.array.len = 0;
  obj->u.array.cap = 0;
  
  obj->shape = ant_shape_new_with_inobj_limit(inobj_limit);
  if (!obj->shape) {
    obj->mark_epoch = ANT_GC_DEAD;
    fixed_arena_free_elem(&js->obj_arena, obj);
    return NULL;
  }
  obj->inobj_limit = ant_shape_get_inobj_limit(obj->shape);
  
  obj->overflow_prop = NULL;
  obj->overflow_cap = 0;
  obj->prop_count = 0;
  
#if ANT_INOBJ_MAX_SLOTS == 4
  const ant_value_t undef = js_mkundef();
  obj->inobj[0] = undef;
  obj->inobj[1] = undef;
  obj->inobj[2] = undef;
  obj->inobj[3] = undef;
#else
  for (uint32_t i = 0; i < ANT_INOBJ_MAX_SLOTS; i++)
    obj->inobj[i] = js_mkundef();
#endif
  
  obj->exotic_ops = NULL;
  obj->extra_slots = NULL;
  obj->extra_count = 0;
  obj->extra_cap = 0;
  
  obj->finalizer = NULL;
  obj->native_ptr = NULL;
  obj->native_tag = 0;
  
  obj->mark_epoch = 0;
  obj->flags = (ant_object_flags_t){.extensible = 1};

  obj->next = js->objects;
  js->objects = obj;
  
  return obj;
}

static ant_value_t get_slot(ant_value_t obj, internal_slot_t slot);
static void set_slot(ant_value_t obj, internal_slot_t slot, ant_value_t value);

static ant_value_t get_proto(ant_t *js, ant_value_t obj);
static void set_proto(ant_t *js, ant_value_t obj, ant_value_t proto);

const char *typestr(ant_value_type_t t) {
  static const char *names[] = {
    [kTypeUndefined] = "undefined",
    [kTypeNull] = "object",
    [kTypeBool] = "boolean",
    [kTypeNumber] = "number",
    [kTypeBigInt] = "bigint",
    [kTypeString] = "string",
    [kTypeSymbol] = "symbol",
    [kTypeObject] = "object",
    [kTypeArray] = "object",
    [kTypeFunction] = "function",
    [kTypeBuiltin] = "function",
    [kTypePromise] = "object",
    [kTypeGenerator] = "object",
    [kTypeTypedArray] = "typedarray",
    [kTypeError] = "err",
    [kTypeFunctionInfo] = "functioninfo",
    [kTypeSourceCode] = "sourcecode"
  };

  return (t < sizeof(names) / sizeof(names[0])) ? names[t] : "??";
}

static void js_native_ctor_prepare(ant_t *js, ant_value_t obj, uint8_t flags) {
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return;

  if (vtype(ptr->proto) == kTypeUndefined && is_object_type(js->sym.function_proto))
    ptr->proto = js->sym.function_proto;

  if (flags & SV_CALL_IS_DEFAULT_CTOR) {
    ptr->flags.is_constructor = 1;
    return;
  }

  ant_value_t cfunc = obj_extra_get(ptr, SLOT_CFUNC);
  if (vtype(cfunc) != kTypeBuiltin || !ptr->shape || !js->intern.prototype) return;
  if (ant_shape_lookup_interned(ptr->shape, js->intern.prototype) < 0) return;

  ptr->flags.is_constructor = 1;
  if (ant_shape_lookup_interned(ptr->shape, js->intern.length) < 0)
    js_define_function_length(js, obj, (int)js_cfunc_length(cfunc));
}

ant_value_t js_obj_to_func_ex(ant_t *js, ant_value_t obj, uint8_t flags) {
  ant_value_t func_obj = (vtype(obj) == kTypeObject) ? obj : mkval(kTypeObject, vdata(obj));
  js_native_ctor_prepare(js, func_obj, flags);

  sv_closure_t *closure = js_closure_alloc(js);

  if (!closure) {
    ant_value_t error = Ant_Exception_Peek(js);
    if (!is_err(error)) error = is_err(js->exception_oom)
      ? js->exception_oom
      : js_mkerr_typed(js, JS_ERR_TYPE, "out of memory allocating function");

    Ant_Exception_Set(js, error);
    return error;
  }
  
  closure->func = NULL;
  closure->upvalues = NULL;
  closure->func_obj = func_obj;
  closure->bound_this = js_mkundef();
  closure->bound_argc = 0;
  closure->super_val = js_mkundef();
  closure->call_flags = flags;

  if (flags & SV_CALL_HAS_BOUND_ARGS) {
    closure->u.bound.argv = NULL;
    closure->u.bound.args_arr = js_mkundef();
  } else {
    closure->u.pending.name = NULL;
    closure->u.pending.len = 0;
  }
  
  return mkref(kTypeFunction, closure);
}

ant_value_t js_obj_to_func(ant_t *js, ant_value_t obj) {
  return js_obj_to_func_ex(js, obj, 0);
}

ant_value_t js_mktypedarray(void *data) {
  return mkval(kTypeTypedArray, ant_external_handle_intern(data));
}

void *js_gettypedarray(ant_value_t val) {
  if (vtype(val) != kTypeTypedArray) return NULL;
  return ant_external_handle_get(vdata(val));
}

ant_value_t js_get_slot(ant_value_t obj, internal_slot_t slot) { 
  return get_slot(js_as_obj(obj), slot); 
}

static inline bool is_unboxed_obj(ant_t *js, ant_value_t val, ant_value_t expected_proto) {
  if (vtype(val) != kTypeObject) return false;
  if (vtype(get_slot(val, SLOT_PRIMITIVE)) != kTypeUndefined) return false;
  ant_value_t proto = get_slot(val, SLOT_PROTO);
  return vdata(proto) == vdata(expected_proto);
}

uint32_t js_to_uint32(double d) {
  if (d >= 0.0 && d < 4294967296.0) return (uint32_t)d;
  if (!isfinite(d)) return 0;
  double sign = (d < 0) ? -1.0 : 1.0;
  double posInt = sign * floor(fabs(d));
  double val = fmod(posInt, 4294967296.0);
  if (val < 0) val += 4294967296.0;
  return (uint32_t) val;
}

int32_t js_to_int32(double d) {
  uint32_t uint32 = js_to_uint32(d);
  if (uint32 >= 2147483648U) return (int32_t)(uint32 - 4294967296.0);
  return (int32_t) uint32;
}

ant_offset_t vstrlen(ant_t *js, ant_value_t v) { 
  if (str_is_heap_rope(v)) {
    ant_rope_heap_t *rope = ant_str_rope_ptr(v);
    return rope ? rope->len : 0;
  }
  if (str_is_heap_builder(v)) {
    ant_string_builder_t *builder = ant_str_builder_ptr(v);
    return builder ? builder->len : 0;
  }
  ant_flat_string_t *flat = ant_str_flat_ptr(v);
  return flat ? flat->len : 0;
}

static ant_value_t make_data_cfunc(
  ant_t *js, ant_value_t data,
  ant_cfunc_t fn
);

static ant_value_t proxy_read_target(ant_t *js, ant_value_t obj);
static ant_value_t proxy_aware_length(ant_t *js, ant_value_t obj, ant_offset_t *out_len);
static ant_value_t proxy_aware_get_elem(ant_t *js, ant_value_t obj, const char *key, size_t key_len);
static ant_value_t proxy_delete_index(ant_t *js, ant_value_t obj, ant_offset_t idx);
static ant_value_t array_method_has_index(ant_t *js, ant_value_t arr, ant_offset_t idx);
static ant_value_t array_method_get_index(ant_t *js, ant_value_t arr, ant_offset_t idx);
static ant_value_t string_to_integer_or_infinity(ant_t *js, ant_value_t value, double *out);
static inline bool array_method_dense_element(ant_value_t arr, ant_offset_t idx, ant_value_t *out);

static inline ant_object_t *array_method_elements(ant_value_t arr);
static inline ant_value_t array_method_element(ant_t *js, ant_value_t arr, ant_object_t *elems, ant_offset_t idx, ant_value_t *out);
static ant_value_t array_set_index_throw(ant_t *js, ant_value_t obj, ant_offset_t idx, ant_value_t value);
static ant_value_t array_set_length_throw(ant_t *js, ant_value_t obj, ant_offset_t len);

#define PROXY_AWARE_LENGTH_OR_RETURN(obj_expr, out_var) \
  ant_offset_t out_var = 0; \
  do { \
    ant_value_t _len_result = proxy_aware_length(js, (obj_expr), &(out_var)); \
    if (is_err(_len_result)) return _len_result; \
  } while (0)

static ant_offset_t get_dense_buf(ant_value_t arr);
static ant_offset_t dense_capacity(ant_offset_t doff);
static ant_offset_t get_array_length(ant_t *js, ant_value_t arr);
static ant_value_t arr_get(ant_t *js, ant_value_t arr, ant_offset_t idx);

static bool arr_has(ant_t *js, ant_value_t arr, ant_offset_t idx);
static bool try_accessor_getter(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t *out);
static bool try_dynamic_setter(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t value);
static uintptr_t lkp_with_setter(ant_t *js, ant_value_t obj, const char *buf, size_t len, ant_value_t *setter_out, bool *has_setter_out);

static ant_value_t js_cfunc_name_value(ant_t *js, ant_value_t cfunc);
static ant_value_t js_cfunc_length_value(ant_value_t cfunc);
static bool js_cfunc_has_prototype(ant_value_t cfunc);
static ant_value_t setup_func_prototype_property(ant_t *js, ant_value_t func, bool mark_constructor);
static ant_value_t js_expose_cfunc_for_key(ant_t *js, ant_value_t value, const char *key, size_t key_len);

static inline ant_value_t lkp_val(ant_t *js, ant_value_t obj, const char *buf, size_t len);
static bool js_try_get(ant_t *js, ant_value_t obj, const char *key, ant_value_t *out);
static bool js_try_get_len(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t *out);
static inline ant_value_t lkp_sym_proto_val(ant_t *js, ant_value_t obj, ant_offset_t sym_off);
static size_t strpromise(ant_t *js, ant_value_t value, char *buf, size_t len);
static size_t strobj(ant_t *js, ant_value_t obj, char *buf, size_t len);

static ant_value_t coerce_to_str_hint(ant_t *js, ant_value_t value, int hint);
static ant_value_t js_mkstr_int32(ant_t *js, int32_t value);
static ant_value_t try_ordinary_to_primitive(ant_t *js, ant_value_t value, int hint);
static ant_value_t try_ordinary_to_primitive_number(ant_t *js, ant_value_t value);
static ant_value_t try_cached_ordinary_to_primitive_number(ant_t *js, ant_value_t value);
static ant_value_t builtin_number_toLocaleString(ant_params_t);
static ant_value_t builtin_string_toString(ant_params_t);
static ant_value_t object_define_property(ant_t *js, ant_value_t obj, ant_value_t prop, ant_value_t descriptor);
static ant_value_t object_define_properties(ant_t *js, ant_value_t obj, ant_value_t props);
static ant_value_t builtin_promise_then(ant_params_t);
static ant_value_t proxy_get_method(ant_t *js, ant_value_t handler, const char *name);
static ant_value_t proxy_get_with_receiver(ant_t *js, ant_value_t proxy, const char *key, size_t key_len, ant_value_t receiver);
static ant_value_t proxy_get(ant_t *js, ant_value_t proxy, const char *key, size_t key_len);
static ant_value_t proxy_get_val(ant_t *js, ant_value_t proxy, ant_value_t key_val);
static ant_value_t proxy_get_prototype_of(ant_t *js, ant_value_t proxy);
static bool proxy_target_is_extensible(ant_value_t obj);
static ant_value_t proxy_set_prototype_of(ant_t *js, ant_value_t proxy, ant_value_t proto);
static ant_value_t proxy_is_extensible(ant_t *js, ant_value_t proxy);
static ant_value_t proxy_prevent_extensions(ant_t *js, ant_value_t proxy);
static ant_value_t proxy_set_with_receiver(ant_t *js, ant_value_t proxy, const char *key, size_t key_len, ant_value_t value, ant_value_t receiver);
static ant_value_t proxy_set(ant_t *js, ant_value_t proxy, const char *key, size_t key_len, ant_value_t value);
static ant_value_t proxy_set_rejected(ant_t *js, const char *key, size_t key_len, ant_value_t value, bool *declined);
static ant_value_t proxy_set_rejected_sym(ant_t *js, ant_value_t sym, ant_value_t value, bool *declined);
static ant_value_t proxy_has(ant_t *js, ant_value_t proxy, const char *key, size_t key_len);
static ant_value_t proxy_has_val(ant_t *js, ant_value_t proxy, ant_value_t key_val);
static ant_value_t proxy_get_own_property_descriptor(ant_t *js, ant_value_t proxy, ant_value_t key_val);
static ant_value_t proxy_own_keys_filtered(ant_t *js, ant_value_t obj, bool include_symbols, bool enumerable_only);
static bool descriptor_bool_value(ant_t *js, ant_value_t desc, const char *key, size_t key_len, bool default_value);
static ant_value_t proxy_has_own(ant_t *js, ant_value_t proxy, ant_value_t key_val);
static ant_value_t proxy_delete(ant_t *js, ant_value_t proxy, const char *key, size_t key_len);
static ant_value_t proxy_delete_val(ant_t *js, ant_value_t proxy, ant_value_t key_val);
static ant_value_t proxy_define_property(ant_t *js, ant_value_t proxy, ant_value_t key_val, ant_value_t descriptor);

static inline void array_len_set(ant_t *js, ant_value_t obj, ant_offset_t new_len);
static inline const ant_shape_prop_t *prop_shape_meta(ant_prop_loc_t loc);

static bool js_try_get_sym_with_receiver(
  ant_t *js, ant_value_t obj, ant_value_t sym,
  ant_value_t receiver, ant_value_t *out
);

static bool setprop_sym_reaches_proxy(
  ant_t *js, ant_value_t obj, ant_value_t proxy, ant_value_t sym, ant_value_t v,
  bool keyed, bool *declined, ant_value_t *done, ant_value_t *next
);

static ant_value_t proxy_set_sym(
  ant_t *js, ant_value_t proxy, ant_value_t sym, ant_value_t value,
  ant_value_t receiver, ant_value_t *target_out
);

ant_value_t unwrap_primitive(ant_t *js, ant_value_t val) {
  if (__builtin_expect(vtype(val) != kTypeObject, 1)) return val;
  ant_value_t prim = get_slot(val, SLOT_PRIMITIVE);
  if (__builtin_expect(vtype(prim) == kTypeUndefined, 1)) return val;
  return prim;
}

static ant_value_t to_string_val(ant_t *js, ant_value_t val) {
  uint8_t t = vtype(val);
  if (t == kTypeString) return val;
  if (t == kTypeObject) {
    ant_value_t prim = get_slot(val, SLOT_PRIMITIVE);
    if (vtype(prim) == kTypeString) return prim;
  }
  return coerce_to_str_hint(js, val, 1);
}

bool js_truthy(ant_t *js, ant_value_t v) {
  static const void *dispatch[] = {
    [kTypeObject]    = &&l_true,
    [kTypeFunction]  = &&l_true,
    [kTypeBuiltin]   = &&l_true,
    [kTypeArray]     = &&l_true,
    [kTypePromise]   = &&l_true,
    [kTypeGenerator] = &&l_true,
    [kTypeSymbol]    = &&l_true,
    [kTypeBool]      = &&l_bool,
    [kTypeString]    = &&l_str,
    [kTypeBigInt]    = &&l_bigint,
    [kTypeNumber]    = &&l_num,
  };

  uint8_t t = vtype(v);
  if (t < sizeof(dispatch) / sizeof(*dispatch) && dispatch[t])
    goto *dispatch[t];
  return false;

  l_true:   return true;
  l_bool:   return vdata(v) != 0;
  l_str:    return vstrlen(js, v) > 0;
  l_bigint: return !bigint_is_zero(js, v);
  l_num: {
    double d = tod(v);
    return d != 0.0 && !isnan(d);
  }
}

size_t uint_to_str(char *buf, size_t bufsize, uint64_t val) {
  if (bufsize == 0) return 0;
  if (val == 0) {
    buf[0] = '0';
    buf[1] = '\0';
    return 1;
  }
  char temp[24];
  size_t len = 0;
  while (val > 0 && len < sizeof(temp)) {
    temp[len++] = '0' + (val % 10);
    val /= 10;
  }
  if (len >= bufsize) len = bufsize - 1;
  for (size_t i = 0; i < len; i++) {
    buf[i] = temp[len - 1 - i];
  }
  buf[len] = '\0';
  return len;
}

static int find_multiref(ant_t *js, ant_value_t obj) {
  for (int i = 0; i < js->stringify->multiref_count; i++)
    if (js->stringify->multiref_objs[i] == obj) return js->stringify->multiref_ids[i];
  return 0;
}

static bool is_on_stack(ant_t *js, ant_value_t obj) {
  for (int i = 0; i < js->stringify->depth; i++)
    if (js->stringify->stack[i] == obj) return true;
  return false;
}

static void mark_multiref(ant_t *js, ant_value_t obj) {
  if (find_multiref(js, obj)) return;
  
  if (js->stringify->multiref_count < MAX_MULTIREF_OBJS) {
    js->stringify->multiref_objs[js->stringify->multiref_count] = obj;
    js->stringify->multiref_ids[js->stringify->multiref_count] = ++js->stringify->multiref_next_id;
    js->stringify->multiref_count++;
  }
}

static void stringify_begin_pass(ant_t *js) {
  js->stringify->depth = 0;
  js->stringify->indent = 0;
  js->stringify->multiref_count = 0;
  js->stringify->multiref_next_id = 0;
}

static size_t stringify_ref_label(ant_t *js, ant_value_t obj, char *buf, size_t len, size_t n) {
  int id = find_multiref(js, obj);
  if (!id) return n;

  char label[24];
  size_t label_len = (size_t)snprintf(label, sizeof(label), "<ref *%d> ", id);
  if (len == 0) return n + label_len;

  size_t cap = len - 1;
  size_t have = n < cap ? n : cap;
  if (label_len < cap) memmove(buf + label_len, buf, have < cap - label_len ? have : cap - label_len);
  memcpy(buf, label, label_len < cap ? label_len : cap);

  size_t total = n + label_len;
  buf[total < cap ? total : cap] = '\0';
  return total;
}

static bool stringify_has_shown_props(ant_value_t obj) {
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  if (!ptr || !ptr->shape) return false;
  uint32_t count = ant_shape_count(ptr->shape);
  for (uint32_t i = 0; i < count; i++)
    if (ant_shape_get_attrs(ptr->shape, i) & ANT_PROP_ATTR_ENUMERABLE) return true;
  return false;
}

static bool stringify_too_deep(ant_t *js) {
  return js->stringify->depth - 1 > js->stringify->max_depth;
}

static size_t stringify_depth_marker(ant_t *js, ant_value_t obj, const char *fallback, char *buf, size_t len) {
  if (vtype(get_proto(js, obj)) == kTypeNull) return cpy(buf, len, "[Object: null prototype]", 24);
  ant_offset_t name_len = 0;
  const char *name = get_class_name(js, obj, &name_len, NULL);
  if (!name) { name = fallback; name_len = (ant_offset_t)strlen(fallback); }
  size_t n = cpy(buf, len, "[", 1);
  n += cpy(buf + n, REMAIN(n, len), name, name_len);
  return n + cpy(buf + n, REMAIN(n, len), "]", 1);
}

static int get_circular_ref(ant_t *js, ant_value_t obj) {
  if (!is_on_stack(js, obj)) return 0;
  mark_multiref(js, obj);
  int ref = find_multiref(js, obj);
  return ref ? ref : -1;
}

static void push_stringify(ant_t *js, ant_value_t obj) {
  if (js->stringify->depth < MAX_STRINGIFY_DEPTH)
    js->stringify->stack[js->stringify->depth++] = obj;
}

static void pop_stringify(ant_t *js) {
  if (js->stringify->depth > 0) js->stringify->depth--;
}

static size_t add_indent(char *buf, size_t len, int level) {
  size_t wanted = (size_t)(level * 2);
  size_t n = 0;
  for (int i = 0; i < level * 2 && n < len; i++) {
    buf[n++] = ' ';
  }
  return wanted;
}

const char *get_str_prop(ant_t *js, ant_value_t obj, const char *key, ant_offset_t klen, ant_offset_t *out_len) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, obj);
  ant_value_t v = js_getprop_fallback_len(js, obj, key, (size_t)klen);
  
  GC_ROOT_PIN(js, v);
  if (vtype(v) != kTypeString) {
    GC_ROOT_RESTORE(js, root_mark);
    return NULL;
  }
  
  const char *str = (const char *)(uintptr_t)(vstr(js, v, out_len));
  GC_ROOT_RESTORE(js, root_mark);
  
  return str;
}

static bool is_small_array(ant_t *js, ant_value_t obj, int *elem_count) {
  ant_offset_t length = get_array_length(js, obj);
  if (length > 64) { if (elem_count) *elem_count = (int)length; return false; }
  
  int count = 0; bool has_nested = false;
  for (ant_offset_t i = 0; i < length; i++) {
    ant_value_t val = arr_get(js, obj, i); uint8_t t = vtype(val);
    if (t == kTypeObject || t == kTypeArray || t == kTypeFunction) has_nested = true;
    count++;
  }
  
  if (elem_count) *elem_count = count;
  return count <= 4 && !has_nested && !(js->stringify && (js->stringify->flags & ANT_INSPECT_EXPAND));
}

static inline bool is_array_index(const char *key, ant_offset_t klen) {
  if (klen == 0 || (klen > 1 && key[0] == '0')) return false;
  for (ant_offset_t i = 0; i < klen; i++) {
    if (key[i] < '0' || key[i] > '9') return false;
  }
  return true;
}

static inline bool parse_array_index(const char *key, size_t klen, ant_offset_t max_len, unsigned long *out_idx) {
  if (
    klen == 0 || key[0] < '0' || key[0] > '9' ||
    (klen > 1 && key[0] == '0')
  ) return false;
  
  const size_t max_ulong_digits = sizeof(unsigned long) == 8 ? 20u : 10u;
  if (klen > max_ulong_digits) return false;

  unsigned long parsed_idx = 0;
  if (klen == max_ulong_digits) for (size_t i = 0; i < klen; i++) {
    if (key[i] < '0' || key[i] > '9') return false;
    unsigned long digit = (unsigned long)(key[i] - '0');
    if (
      parsed_idx > ULONG_MAX / 10 ||
      (parsed_idx == ULONG_MAX / 10 && digit > ULONG_MAX % 10)
    ) return false;
    parsed_idx = parsed_idx * 10 + digit;
  }
  
  else for (size_t i = 0; i < klen; i++) {
    if (key[i] < '0' || key[i] > '9') return false;
    parsed_idx = parsed_idx * 10 + (unsigned long)(key[i] - '0');
  }
  
  if (parsed_idx >= max_len) return false;
  *out_idx = parsed_idx;
  
  return true;
}

static inline ant_object_t *array_obj_ptr(ant_value_t obj) {
  if (!is_object_type(obj)) return NULL;
  ant_object_t *ptr = js_obj_ptr(obj);
  return (ptr && ptr->type_tag == kTypeArray) ? ptr : NULL;
}

static inline ant_object_t *real_array_obj_ptr(ant_value_t obj) {
  ant_object_t *ptr = array_obj_ptr(obj);
  return ant_object_is_array(ptr) ? ptr : NULL;
}

static inline bool array_dense_storage(const ant_object_t *ptr) {
  return ptr && ptr->flags.fast_array && !ptr->flags.is_exotic && ptr->u.array.data;
}

static inline bool array_dense_covers(const ant_object_t *ptr, ant_offset_t n) {
  return array_dense_storage(ptr) && n <= ptr->u.array.len && n <= ptr->u.array.cap;
}

static inline bool array_dense_read(const ant_object_t *ptr, ant_offset_t idx, ant_value_t *out) {
  if (!array_dense_storage(ptr) || idx >= ptr->u.array.len || idx >= ptr->u.array.cap) return false;
  ant_value_t value = ptr->u.array.data[idx];
  if (is_empty_slot(value)) return false;
  *out = value;
  return true;
}

static inline bool array_may_have_holes(ant_value_t obj) {
  ant_object_t *ptr = array_obj_ptr(obj);
  return ptr ? ptr->flags.may_have_holes : true;
}

static inline bool array_may_have_dense_elements(ant_value_t obj) {
  ant_object_t *ptr = array_obj_ptr(obj);
  return ptr ? ptr->flags.may_have_dense_elements : true;
}

static inline void array_mark_may_have_holes(ant_value_t obj) {
  ant_object_t *ptr = array_obj_ptr(obj);
  if (ptr) ptr->flags.may_have_holes = 1;
}

static inline void array_mark_packed(ant_value_t obj) {
  ant_object_t *ptr = array_obj_ptr(obj);
  if (
    ptr && ptr->flags.fast_array && ptr->u.array.data && 
    ptr->u.array.len <= ptr->u.array.cap
  ) ptr->flags.may_have_holes = 0;
}

static inline void array_define_or_set_index(ant_t *js, ant_value_t obj, const char *key, size_t klen) {
  if (!key) return;
  if (!array_obj_ptr(obj)) return;

  unsigned long idx = 0;
  if (!parse_array_index(key, klen, ((ant_offset_t)UINT32_MAX), &idx)) return;

  ant_offset_t cur_len = get_array_length(js, obj);
  ant_offset_t next_len = (ant_offset_t)idx + 1;
  
  if (next_len > cur_len) {
    if ((ant_offset_t)idx > cur_len) array_mark_may_have_holes(obj);
    array_len_set(js, obj, next_len);
  }
}

static ant_offset_t get_array_length(ant_t *js, ant_value_t arr) {
  if (!is_object_type(arr)) return 0;

  ant_object_t *arr_ptr = array_obj_ptr(arr);
  if (arr_ptr) return (ant_offset_t)arr_ptr->u.array.len;

  ant_value_t val = lkp_interned_val(js, arr, js->intern.length);
  if (vtype(val) == kTypeNumber) return (ant_offset_t) tod(val);
  
  return 0;
}

static ant_offset_t to_length_offset(double d) {
  if (!(d > 0)) return 0;
  return d > 9007199254740991.0 ? (ant_offset_t)9007199254740991.0 : (ant_offset_t)d;
}

static ant_value_t array_like_length_checked(ant_t *js, ant_value_t obj, ant_offset_t *out) {
  ant_object_t *ptr = real_array_obj_ptr(obj);
  *out = ptr ? (ant_offset_t)ptr->u.array.len : 0;
  if (ptr) return js_mkundef();

  ant_value_t val = js_getprop_fallback(js, obj, "length");
  if (is_err(val)) return val;
  val = js_to_numeric(js, val);
  
  if (is_err(val)) return val;
  if (vtype(val) == kTypeBigInt)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert a BigInt value to a number");
  
  *out = to_length_offset(tod(val));
  return js_mkundef();
}

static ant_offset_t array_like_length(ant_t *js, ant_value_t obj) {
  ant_object_t *ptr = real_array_obj_ptr(obj);
  
  if (ptr) return (ant_offset_t)ptr->u.array.len;
  if (!is_object_type(obj)) return 0;

  ant_value_t val = lkp_interned_val(js, obj, js->intern.length);
  uint8_t t = vtype(val);
  
  if (t == kTypeNumber) return to_length_offset(tod(val));
  if (t == kTypeString || t == kTypeBool) return to_length_offset(js_to_number(js, val));
  
  return 0;
}

static ant_value_t get_obj_ctor(ant_t *js, ant_value_t obj) {
  ant_value_t ctor = get_slot(obj, SLOT_CTOR);
  if (vtype(ctor) == kTypeFunction) return ctor;
  ant_value_t proto = get_slot(obj, SLOT_PROTO);
  if (vtype(proto) != kTypeObject) return js_mkundef();
  return lkp_interned_val(js, proto, js->intern.constructor);
}

static const char *get_func_name(ant_t *js, ant_value_t func, ant_offset_t *out_len) {
  if (vtype(func) == kTypeBuiltin) {
    const ant_cfunc_meta_t *meta = js_as_cfunc_meta(func);
    if (!meta || !meta->name) return NULL;
    if (out_len) *out_len = (ant_offset_t)strlen(meta->name);
    return meta->name;
  }

  if (vtype(func) != kTypeFunction) return NULL;
  ant_value_t func_obj = js_func_obj(func);
  ant_value_t name = lkp_val(js, func_obj, "name", 4);
  
  if (vtype(name) == kTypeString) {
    ant_offset_t str_off = vstr(js, name, out_len);
    return (const char *)(uintptr_t)(str_off);
  }

  ant_value_t cfunc_slot = get_slot(func_obj, SLOT_CFUNC);
  if (vtype(cfunc_slot) == kTypeBuiltin) return get_func_name(js, cfunc_slot, out_len);
  
  return NULL;
}

static bool js_cfunc_try_get_own(ant_t *js, ant_value_t cfunc, const char *key, size_t key_len, ant_value_t *out) {
  if (key_len == 4 && memcmp(key, "name", 4) == 0) {
    ant_value_t name = js_cfunc_name_value(js, cfunc);
    if (vtype(name) != kTypeUndefined) {
      *out = name;
      return true;
    }
    return false;
  }

  if (key_len == 6 && memcmp(key, "length", 6) == 0) {
    *out = js_cfunc_length_value(cfunc);
    return true;
  }

  if (key_len == 9 && memcmp(key, "prototype", 9) == 0 && js_cfunc_has_prototype(cfunc)) {
    ant_value_t promoted = js_cfunc_promote(js, cfunc);
    if (is_err(promoted)) {
      *out = promoted;
      return true;
    }
    *out = js_get(js, promoted, "prototype");
    return true;
  }

  return false;
}

const char *get_class_name(ant_t *js, ant_value_t obj, ant_offset_t *out_len, const char *skip) {
  const char *name = get_func_name(js, get_obj_ctor(js, obj), out_len);
  if (!name) return NULL;
  if (skip && *out_len == (ant_offset_t)strlen(skip) && memcmp(name, skip, *out_len) == 0) return NULL;
  return name;
}

static inline ant_offset_t dense_iterable_length(ant_t *js, ant_value_t obj) {
  ant_offset_t doff = get_dense_buf(obj);
  if (!doff) return 0;
  ant_offset_t dense_len = dense_capacity(doff);
  ant_offset_t semantic_len = get_array_length(js, obj);
  return dense_len < semantic_len ? dense_len : semantic_len;
}

static size_t strarr_body(ant_t *js, ant_value_t obj, char *buf, size_t len) {
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  ant_offset_t length = get_array_length(js, obj);
  
  ant_offset_t d_len = dense_iterable_length(js, obj);
  ant_offset_t iter_len = (d_len >= length) ? length : d_len;
  
  bool arguments = ptr && ptr->flags.arguments_object;
  ant_offset_t class_len = 0;
  const char *class_name = arguments ? NULL : get_class_name(js, obj, &class_len, "Array");
  
  int elem_count = 0;
  bool hidden_nested = false;
  int hidden = js_inspect_hidden_count(js, obj, &hidden_nested);
  bool inline_mode = is_small_array(js, obj, &elem_count) && !hidden_nested;
  size_t n = 0;
  
  if (arguments) n += cpy(buf + n, REMAIN(n, len), "[Arguments] ", 12);
  if (class_name) {
    n += cpy(buf + n, REMAIN(n, len), class_name, class_len);
    n += (size_t) snprintf(buf + n, REMAIN(n, len), "(%u) ", (unsigned) length);
  }
  
  char open = arguments ? '{' : '[';
  char close = arguments ? '}' : ']';
  if (length == 0 && hidden == 0 && !(arguments && stringify_has_shown_props(obj)))
    return n + (size_t) snprintf(buf + n, REMAIN(n, len), "%c%c", open, close);
  
  n += (size_t) snprintf(buf + n, REMAIN(n, len), inline_mode ? "%c " : "%c\n", open);
  if (!inline_mode) js->stringify->indent++;
  
  bool printed_first = false;
  for (ant_offset_t i = 0; i < iter_len; i++) {
    bool found = arr_has(js, obj, i);
    if (arguments && !found) continue;
    if (printed_first) n += cpy(buf + n, REMAIN(n, len), inline_mode ? ", " : ",\n", 2);
    if (!inline_mode) n += add_indent(buf + n, REMAIN(n, len), js->stringify->indent);

    if (arguments) n += (size_t) snprintf(buf + n, REMAIN(n, len), "'%u': ", (unsigned) i);
    ant_value_t val = arr_get(js, obj, i);
    n += found ? tostr(js, val, buf + n, REMAIN(n, len)) : cpy(buf + n, REMAIN(n, len), "undefined", 9);
    printed_first = true;
  }

  if (ptr && ptr->shape) {
    uint32_t shape_count = ant_shape_count(ptr->shape);
    for (uint32_t i = 0; i < shape_count; i++) {
      const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
      if (!prop || prop->type == ANT_SHAPE_KEY_SYMBOL) continue;
      
      const char *key = prop->key.interned;
      ant_offset_t klen = (ant_offset_t)intern_length(key);
      
      if (is_length_key(key, klen)) continue;
      if (!(prop->attrs & ANT_PROP_ATTR_ENUMERABLE)) continue;
      
      if (printed_first) n += cpy(buf + n, REMAIN(n, len), inline_mode ? ", " : ",\n", 2);
      if (!inline_mode) n += add_indent(buf + n, REMAIN(n, len), js->stringify->indent);
      
      ant_value_t val = (i < ptr->prop_count) ? ant_object_prop_get_unchecked(ptr, i) : js_mkundef();
      if (arguments && is_array_index(key, klen)) {
        n += (size_t) snprintf(buf + n, REMAIN(n, len), "'%.*s': ", (int)klen, key);
        n += tostr(js, val, buf + n, REMAIN(n, len));
      } else if (is_array_index(key, klen)) n += tostr(js, val, buf + n, REMAIN(n, len)); else {
        n += cpy(buf + n, REMAIN(n, len), key, klen);
        n += cpy(buf + n, REMAIN(n, len), ": ", 2);
        n += tostr(js, val, buf + n, REMAIN(n, len));
      }
      
      printed_first = true;
    }
  }

  if (hidden) {
    js_inspect_builder_t hidden_builder;
    js_inspect_builder_init_fixed(&hidden_builder, js, buf, len, n);
    hidden_builder.inline_mode = inline_mode;
    hidden_builder.first = !printed_first;
    hidden_builder.did_indent = true;
    js_inspect_hidden_props(&hidden_builder, obj);
    n = hidden_builder.n;
  }
  
  if (!inline_mode) {
    js->stringify->indent--;
    n += cpy(buf + n, REMAIN(n, len), "\n", 1);
    n += add_indent(buf + n, REMAIN(n, len), js->stringify->indent);
  }
  
  return n + (size_t) snprintf(buf + n, REMAIN(n, len), inline_mode ? " %c" : "%c", close);
}

static size_t strarr(ant_t *js, ant_value_t obj, char *buf, size_t len) {
  int ref = get_circular_ref(js, obj);
  if (ref) return ref > 0 ? (size_t) snprintf(buf, len, "[Circular *%d]", ref) : cpy(buf, len, "[Circular]", 10);

  push_stringify(js, obj);
  bool arguments = js_obj_ptr(js_as_obj(obj))->flags.arguments_object;
  bool has_contents = get_array_length(js, obj) > 0 || stringify_has_shown_props(obj);
  
  size_t n = stringify_too_deep(js) && has_contents
    ? stringify_depth_marker(js, obj, arguments ? "Arguments" : "Array", buf, len)
    : strarr_body(js, obj, buf, len);
  
  pop_stringify(js);
  return stringify_ref_label(js, obj, buf, len, n);
}

static size_t strdate(ant_t *js, ant_value_t obj, char *buf, size_t len) {
  ant_value_t time_val = js_get_slot(obj, SLOT_DATA);
  if (vtype(time_val) != kTypeNumber || !isfinite(js_getnum(time_val)))
    return cpy(buf, len, "Invalid Date", 12);

  static const date_string_spec_t kSpec = {DATE_STRING_FMT_ISO, DATE_STRING_PART_ALL};
  ant_value_t iso = get_date_string(js, obj, kSpec);
  if (is_err(iso) || vtype(iso) != kTypeString) return cpy(buf, len, "Invalid Date", 12);

  ant_offset_t slen;
  ant_offset_t soff = vstr(js, iso, &slen);
  
  return cpy(buf, len, (const char *)(uintptr_t)(soff), slen);
}

static size_t strobj_host_proxy(ant_t *js, ant_value_t obj, char *buf, size_t len) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t copy = Ant_Object_CreateBare(js);
  
  GC_ROOT_PIN(js, copy);
  (void)js_copy_data_props(js, copy, obj, NULL, 0);
  
  size_t n = strobj(js, copy, buf, len);
  GC_ROOT_RESTORE(js, root_mark);
  
  return n;
}

static size_t strobj_error(ant_t *js, ant_value_t obj, char *buf, size_t len, size_t n) {
  js_inspect_builder_t builder;
  js_inspect_builder_init_fixed(&builder, js, buf, len, n);
  js_inspect_error(&builder, obj);
  return builder.n;
}

static size_t strobj_map(ant_t *js, ant_value_t obj, map_entry_t *head, char *buf, size_t len, size_t n) {
  js_inspect_builder_t builder;
  js_inspect_builder_init_fixed(&builder, js, buf, len, n);
  js_inspect_map(&builder, obj, head);
  return builder.n;
}

static size_t strobj_set(ant_t *js, ant_value_t obj, set_entry_t *head, char *buf, size_t len, size_t n) {
  js_inspect_builder_t builder;
  js_inspect_builder_init_fixed(&builder, js, buf, len, n);
  js_inspect_set(&builder, obj, head);
  return builder.n;
}

static ant_value_t strobj_inspect_tag(ant_t *js, ant_value_t obj) {
  ant_offset_t sym_off = (ant_offset_t)vdata(js->sym.toStringTag_sym);
  prop_meta_t meta;
  ant_value_t cur = obj;
  
  for (int hops = 0; is_object_type(cur) && !is_proxy(cur) && hops < 64; hops++) {
    if (lookup_symbol_prop_meta(js, cur, sym_off, &meta)) {
      if (cur == obj && meta.enumerable) return js_mkundef();
      if (!meta.has_getter) {
        ant_prop_loc_t loc = lkp_sym(cur, sym_off);
        return loc.obj ? js_prop_load(loc) : js_mkundef();
      }
      
      GC_ROOT_SAVE(mark, js);
      ant_value_t saved = Ant_Exception_Peek(js);
      GC_ROOT_PIN(js, saved);
      ant_value_t tag = js_call_getter(js, obj, meta.getter);
      if (is_err(tag) || Ant_Exception_Pending(js)) tag = js_mkundef();
      Ant_Exception_Set(js, saved);
      GC_ROOT_RESTORE(js, mark);
      return tag;
    }
    cur = js_get_proto(js, cur);
  }
  
  return js_mkundef();
}

static size_t strobj_trailing_props(ant_t *js, ant_value_t obj, char *buf, size_t len, size_t n) {
  js_inspect_builder_t builder;
  js_inspect_builder_init_fixed(&builder, js, buf, len, n);
  js_inspect_trailing_props(&builder, obj);
  return builder.n;
}

static size_t strobj_inspect(ant_t *js, ant_value_t obj, char *buf, size_t len, size_t n) {
  ant_value_t tag = strobj_inspect_tag(js, obj);
  js_inspect_builder_t builder;
  js_inspect_builder_init_fixed(&builder, js, buf, len, n);
  
  bool ok;
  if (vtype(tag) == kTypeString) {
    ant_offset_t tag_len = 0;
    const char *tag_str = (const char *)(uintptr_t)vstr(js, tag, &tag_len);
    ok = js_inspect_tagged_header(&builder, obj, tag_str, (size_t)tag_len);
  } else ok = js_inspect_plain_header(&builder, obj);
  
  if (ok) ok = js_inspect_object_body(&builder, obj);
  if (ok) js_inspect_close(&builder);
  return builder.n;
}

static size_t strregexp(ant_t *js, ant_value_t obj, char *buf, size_t len) {
  ant_value_t source = regexp_source_value(js, obj);
  ant_value_t flags = get_slot(obj, SLOT_REGEXP_FLAGS_STRING);

  size_t source_len = 0, flags_len = 0;
  const char *source_str = vtype(source) == kTypeString ? js_getstr(js, source, &source_len) : "(?:)";
  const char *flags_str = vtype(flags) == kTypeString ? js_getstr(js, flags, &flags_len) : "";
  if (vtype(source) != kTypeString) source_len = 4;

  size_t n = cpy(buf, len, "/", 1);
  n += cpy(buf + n, REMAIN(n, len), source_str, source_len);
  n += cpy(buf + n, REMAIN(n, len), "/", 1);
  
  return n + cpy(buf + n, REMAIN(n, len), flags_str, flags_len);
}

static size_t strobj_body(ant_t *js, ant_value_t obj, char *buf, size_t len, size_t n) {
  if (get_slot(obj, SLOT_ERROR_BRAND) == js_true || vtype(get_slot(obj, SLOT_ERR_TYPE)) == kTypeNumber)
    return strobj_error(js, obj, buf, len, n);
  if (is_date_instance(obj)) return strobj_trailing_props(js, obj, buf, len, n + strdate(js, obj, buf + n, REMAIN(n, len)));
  if (js_obj_ptr(obj)->flags.regexp_brand) return strobj_trailing_props(js, obj, buf, len, n + strregexp(js, obj, buf + n, REMAIN(n, len)));
  
  TypedArrayData *ta = buffer_get_typedarray_data(obj);
  if (ta && ta->buffer) return n + stringify_typedarray(js, obj, ta, buf + n, REMAIN(n, len));
  
  ArrayBufferData *ab = buffer_get_arraybuffer_data(obj);
  if (ab) return n + stringify_arraybuffer(ab, buf + n, REMAIN(n, len));
  
  DataViewData *dv = buffer_get_dataview_data(obj);
  if (dv && dv->buffer) return n + stringify_dataview(dv, buf + n, REMAIN(n, len));
  
  blob_data_t *bd = blob_get_data(obj);
  if (bd) return n + stringify_blob(bd, js_brand_id(obj) == BRAND_FILE, buf + n, REMAIN(n, len));
  
  bool too_deep = stringify_too_deep(js);
  map_entry_t **map = get_map_from_obj(obj);
  if (map) return too_deep && *map ? n + cpy(buf + n, REMAIN(n, len), "[Map]", 5) : strobj_map(js, obj, *map, buf, len, n);
  
  set_entry_t **set = get_set_from_obj(obj);
  if (set) return too_deep && *set ? n + cpy(buf + n, REMAIN(n, len), "[Set]", 5) : strobj_set(js, obj, *set, buf, len, n);
  if (too_deep && stringify_has_shown_props(obj)) return n + stringify_depth_marker(js, obj, "Object", buf + n, REMAIN(n, len));
  
  return strobj_inspect(js, obj, buf, len, n);
}

static ant_value_t strobj_call_custom_inspect(ant_t *js, ant_value_t obj) {
  ant_value_t inspect_sym = js->sym.inspect_sym;
  if (vtype(inspect_sym) != kTypeSymbol) return js_mkundef();

  ant_value_t inspect_fn = lkp_sym_proto_val(js, obj, (ant_offset_t)vdata(inspect_sym));
  if (!is_callable(inspect_fn)) return js_mkundef();

  GC_ROOT_SAVE(exception_mark, js);
  ant_value_t saved = Ant_Exception_Peek(js);
  GC_ROOT_PIN(js, saved);

  ant_value_t depth_arg = js_mknum((double)(MAX_STRINGIFY_DEPTH - js->stringify->depth));
  ant_value_t result = sv_call_value(js, inspect_fn, obj, &depth_arg, 1);

  if (is_err(result) || Ant_Exception_Pending(js) || vtype(result) != kTypeString) {
    Ant_Exception_Set(js, saved);
    GC_ROOT_RESTORE(js, exception_mark);
    return js_mkundef();
  }

  Ant_Exception_Set(js, saved);
  GC_ROOT_RESTORE(js, exception_mark);

  return result;
}

static size_t strobj(ant_t *js, ant_value_t obj, char *buf, size_t len) {
  if (js_proxy_is_host(obj)) return strobj_host_proxy(js, obj, buf, len);
  
  int ref = get_circular_ref(js, obj);
  if (ref) return ref > 0 ? (size_t)snprintf(buf, len, "[Circular *%d]", ref) : cpy(buf, len, "[Circular]", 10);
  
  push_stringify(js, obj);
  
  size_t n = 0;
  ant_value_t custom = strobj_call_custom_inspect(js, obj);
  if (vtype(custom) == kTypeString) {
    size_t slen = 0;
    const char *s = js_getstr(js, custom, &slen);
    n = cpy(buf, len, s ? s : "", slen);
  } else n = strobj_body(js, obj, buf, len, 0);
  
  pop_stringify(js);
  return stringify_ref_label(js, obj, buf, len, n);
}

size_t js_number_to_chars(ant_value_t value, char *buf, size_t len) {
  double dv = tod(value);

  if (__builtin_expect(isnan(dv), 0)) 
    return cpy(buf, len, "NaN", 3);
    
  if (__builtin_expect(isinf(dv), 0)) 
    return cpy(buf, len, dv > 0 ? "Infinity" : "-Infinity", dv > 0 ? 8 : 9);

  return ant_number_to_shortest(dv, buf, len);
}

static inline ant_offset_t assert_flat_string_len(ant_value_t value, const char **out_ptr) {
  ant_flat_string_t *flat = ant_str_flat_ptr(value);
  assert(flat != NULL);
  ant_offset_t len = flat->len;
  if (out_ptr) *out_ptr = flat->bytes;
  return len;
}

static inline ant_rope_heap_t *assert_rope_ptr(ant_value_t value) {
  assert(vtype(value) == kTypeString);
  assert(str_is_heap_rope(value));
  ant_rope_heap_t *ptr = ant_str_rope_ptr(value);
  assert(ptr != NULL);
  return ptr;
}

static inline ant_offset_t rope_len(ant_value_t value) {
  ant_rope_heap_t *ptr = assert_rope_ptr(value);
  return ptr->len;
}

static inline uint16_t rope_depth(ant_value_t value) {
  ant_rope_heap_t *ptr = assert_rope_ptr(value);
  return ptr->depth;
}

static inline ant_value_t rope_left(ant_value_t value) {
  ant_rope_heap_t *ptr = assert_rope_ptr(value);
  return ptr->left;
}

static inline ant_value_t rope_right(ant_value_t value) {
  ant_rope_heap_t *ptr = assert_rope_ptr(value);
  return ptr->right;
}

static inline ant_value_t rope_cached_flat(ant_value_t value) {
  ant_rope_heap_t *ptr = assert_rope_ptr(value);
  return ptr->cached;
}

static inline void rope_canonicalize_flat(ant_value_t rope, ant_value_t flat) {
  ant_rope_heap_t *ptr = assert_rope_ptr(rope);
  ptr->cached = flat;
  ptr->left = js_mkundef();
  ptr->right = js_mkundef();
  ptr->depth = 0;
  ptr->len = ant_str_flat_ptr(flat)->len;
}

static inline ant_string_builder_t *assert_builder_ptr(ant_value_t value) {
  assert(vtype(value) == kTypeString);
  assert(str_is_heap_builder(value));
  
  ant_string_builder_t *ptr = ant_str_builder_ptr(value);
  assert(ptr != NULL);
  
  return ptr;
}

static inline ant_offset_t builder_len(ant_value_t value) {
  ant_string_builder_t *ptr = assert_builder_ptr(value);
  return ptr->len;
}

static inline ant_value_t builder_cached_flat(ant_value_t value) {
  ant_string_builder_t *ptr = assert_builder_ptr(value);
  return ptr->cached;
}

static inline void builder_set_cached_flat(ant_value_t builder, ant_value_t flat) {
  ant_string_builder_t *ptr = assert_builder_ptr(builder);
  ptr->cached = flat;
}

static bool rope_flatten_into(ant_value_t str, char *dest, ant_offset_t total_len, ant_offset_t *start) {
  assert(vtype(str) == kTypeString);

  ant_value_t local[32];
  ant_value_t *stack = local;
  
  size_t sp = 0, cap = 32;
  ant_offset_t pos = total_len;
  ant_value_t current = str;

  for (;;) {
    while (str_is_heap_rope(current)) {
      ant_value_t cached = rope_cached_flat(current);
      if (vtype(cached) == kTypeString && !str_is_heap_rope(cached)) {
        current = cached;
        break;
      }

      ant_value_t left = rope_left(current);
      current = rope_right(current);
      if (!ant_value_stack_push_with_spill(
        &stack, &sp, &cap, local, left
      )) {
        if (stack != local) free(stack);
        return false;
      }
    }

    const char *src;
    ant_offset_t len = assert_flat_string_len(current, &src);
    if (len > pos) {
      if (stack != local) free(stack);
      return false;
    }
    pos -= len;
    memcpy(dest + pos, src, (size_t)len);

    if (sp == 0) break;
    current = stack[--sp];
  }

  if (stack != local) free(stack);
  *start = pos;
  
  return true;
}

ant_value_t rope_flatten(ant_t *js, ant_value_t rope) {
  assert(vtype(rope) == kTypeString);
  if (!str_is_heap_rope(rope)) return rope;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, rope);
  
  ant_value_t cached = rope_cached_flat(rope);
  GC_ROOT_PIN(js, cached);
  if (vtype(cached) == kTypeString && !str_is_heap_rope(cached)) {
    GC_ROOT_RESTORE(js, root_mark);
    return cached;
  }
  
  ant_offset_t total_len = rope_len(rope);
  ant_value_t flat = js_mkstr(js, NULL, total_len);
  GC_ROOT_PIN(js, flat);

  if (is_err(flat)) {
    GC_ROOT_RESTORE(js, root_mark);
    return flat;
  }

  ant_flat_string_t *flat_ptr = (ant_flat_string_t *)vptr(flat);
  ant_offset_t start = 0;
  
  if (!rope_flatten_into(rope, flat_ptr->bytes, total_len, &start)) {
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkerr(js, "string flatten failed");
  }
  
  if (start > 0) memmove(flat_ptr->bytes, flat_ptr->bytes + start, (size_t)(total_len - start));
  flat_ptr->len = total_len - start;
  
  str_flat_finish(flat_ptr, str_detect_ascii_bytes(flat_ptr->bytes, (size_t)flat_ptr->len));
  if (vtype(cached) == kTypeNumber) str_flat_set_utf16_len(flat_ptr, (ant_offset_t)tod(cached));

  rope_canonicalize_flat(rope, flat);
  GC_ROOT_RESTORE(js, root_mark);
  
  return flat;
}

static ant_value_t builder_flatten(ant_t *js, ant_value_t builder) {
  assert(vtype(builder) == kTypeString);
  if (!str_is_heap_builder(builder)) return builder;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, builder);

  ant_value_t cached = builder_cached_flat(builder);
  GC_ROOT_PIN(js, cached);
  
  if (vtype(cached) == kTypeString && !str_is_heap_rope(cached) && !str_is_heap_builder(cached)) {
    GC_ROOT_RESTORE(js, root_mark);
    return cached;
  }

  ant_string_builder_t *ptr = assert_builder_ptr(builder);
  ant_value_t snapshot = ptr->snapshot;
  GC_ROOT_PIN(js, snapshot);
  ant_value_t flat = js_mkstr(js, NULL, (size_t)ptr->len);
  
  GC_ROOT_PIN(js, flat);
  if (is_err(flat)) {
    GC_ROOT_RESTORE(js, root_mark);
    return flat;
  }

  ant_flat_string_t *flat_ptr = (ant_flat_string_t *)vptr(flat);
  size_t cursor = 0;

  if (vtype(snapshot) == kTypeString) {
    snapshot = str_materialize(js, snapshot);
    if (is_err(snapshot)) {
      GC_ROOT_RESTORE(js, root_mark);
      return snapshot;
    }

    ant_flat_string_t *snapshot_flat = ant_str_flat_ptr(snapshot);
    if (snapshot_flat && snapshot_flat->len > 0) {
      memcpy(flat_ptr->bytes, snapshot_flat->bytes, (size_t)snapshot_flat->len);
      cursor = (size_t)snapshot_flat->len;
    }
  }
  
  for (ant_builder_chunk_t *chunk = ptr->head; chunk; chunk = chunk->next) {
    ant_value_t chunk_value = chunk->value;
    if (str_is_heap_rope(chunk_value) || str_is_heap_builder(chunk_value)) {
    chunk_value = str_materialize(js, chunk_value);
    if (is_err(chunk_value)) {
      GC_ROOT_RESTORE(js, root_mark);
      return chunk_value;
    }}
    
    ant_flat_string_t *chunk_flat = ant_str_flat_ptr(chunk_value);
    if (!chunk_flat || chunk_flat->len == 0) continue;
    memcpy(flat_ptr->bytes + cursor, chunk_flat->bytes, (size_t)chunk_flat->len);
    cursor += (size_t)chunk_flat->len;
  }
  
  if (ptr->tail_len > 0) {
    memcpy(flat_ptr->bytes + cursor, ptr->tail, ptr->tail_len);
    cursor += ptr->tail_len;
  }
  
  flat_ptr->len = (ant_offset_t)cursor;
  str_flat_finish(flat_ptr, ptr->ascii_state);
  ptr->len = flat_ptr->len;
  
  if (vtype(cached) == kTypeNumber)
    str_flat_set_utf16_len(flat_ptr, (ant_offset_t)tod(cached));
  
  ptr->snapshot = flat;
  ptr->head = NULL;
  ptr->chunk_tail = NULL;
  ptr->tail_len = 0;
  
  builder_set_cached_flat(builder, flat);
  GC_ROOT_RESTORE(js, root_mark);
  
  return flat;
}

ant_value_t str_materialize(ant_t *js, ant_value_t value) {
  if (vtype(value) != kTypeString) return value;
  if (str_is_heap_rope(value)) return rope_flatten(js, value);
  if (str_is_heap_builder(value)) return builder_flatten(js, value);
  return value;
}

ant_offset_t vstr(ant_t *js, ant_value_t value, ant_offset_t *len) {
  if (str_is_heap_rope(value) || str_is_heap_builder(value)) {
    ant_value_t flat = str_materialize(js, value);
    assert(!is_err(flat));
    value = flat;
  }
  
  const char *ptr = NULL;
  ant_offset_t slen = assert_flat_string_len(value, &ptr);
  if (len) *len = slen;
  return (ant_offset_t)(uintptr_t)ptr;
}

static bool strstring_has_template(const char *str, size_t slen) {
  for (const char *p = str; (p = memchr(p, '$', (size_t)(str + slen - p))); p++)
    if (p + 1 < str + slen && p[1] == '{') return true;
  return false;
}

static char strstring_quote(const char *str, ant_offset_t slen) {
  size_t n = (size_t)slen;

  if (!memchr(str, '\'', n)) return '\'';
  if (!memchr(str, '"', n)) return '"';
  if (!memchr(str, '`', n) && !strstring_has_template(str, n)) return '`';

  return '\'';
}

static const uint8_t strstring_escape_tbl[256] = {
  [0x00] = 1, [0x01] = 1, [0x02] = 1, [0x03] = 1, [0x04] = 1, [0x05] = 1, [0x06] = 1,
  [0x07] = 1, [0x08] = 1, [0x09] = 1, [0x0a] = 1, [0x0b] = 1, [0x0c] = 1, [0x0d] = 1,
  [0x0e] = 1, [0x0f] = 1, [0x10] = 1, [0x11] = 1, [0x12] = 1, [0x13] = 1, [0x14] = 1,
  [0x15] = 1, [0x16] = 1, [0x17] = 1, [0x18] = 1, [0x19] = 1, [0x1a] = 1, [0x1b] = 1,
  [0x1c] = 1, [0x1d] = 1, [0x1e] = 1, [0x1f] = 1, [0x7f] = 1, ['\\'] = 1,
};

size_t strstring(ant_t *js, ant_value_t value, char *buf, size_t len) {
  ant_offset_t slen, off = vstr(js, value, &slen);
  const char *str = (const char *)(uintptr_t)off;
  char quote = strstring_quote(str, slen);

  size_t n = 0; size_t avail = 0;
  char *dst = fixed_buf_write_ptr(buf, len, n, &avail);
  
  ant_offset_t i = 0;
  n += cpy(dst, avail, &quote, 1);

  while (i < slen) {
    ant_offset_t start = i;
    const char *escaped = NULL;
    
    size_t escaped_len = 2;
    char scratch[4];

    while (
      i < slen &&
      !strstring_escape_tbl[(unsigned char)str[i]] &&
      str[i] != quote
    ) i++;

    if (i > start) {
      dst = fixed_buf_write_ptr(buf, len, n, &avail);
      n += cpy(dst, avail, str + start, (size_t)(i - start));
    }

    if (i >= slen) break;

    switch (str[i]) {
      case '\n': escaped = "\\n";  break;
      case '\r': escaped = "\\r";  break;
      case '\t': escaped = "\\t";  break;
      case '\b': escaped = "\\b";  break;
      case '\f': escaped = "\\f";  break;
      case '\v': escaped = "\\v";  break;
      case '\\': escaped = "\\\\"; break;
      
      default: {
        unsigned char c = (unsigned char)str[i];
        scratch[0] = '\\';

        if (c == (unsigned char)quote) scratch[1] = quote;
        else {
          scratch[1] = 'x';
          scratch[2] = "0123456789ABCDEF"[c >> 4];
          scratch[3] = "0123456789ABCDEF"[c & 0x0f];
          escaped_len = 4;
        }

        escaped = scratch;
        break;
      }
    }

    dst = fixed_buf_write_ptr(buf, len, n, &avail);
    n += cpy(dst, avail, escaped, escaped_len);
    i++;
  }

  dst = fixed_buf_write_ptr(buf, len, n, &avail);
  n += cpy(dst, avail, &quote, 1);

  return n;
}

const char *intern_string(const char *str, size_t len) {
  if (!intern_table_init()) return NULL;

  if ((intern_count + 1) * INTERN_LOAD_DEN >= intern_bucket_count * INTERN_LOAD_NUM) {
    size_t next_bucket_count = intern_bucket_count << 1;
    if (next_bucket_count > intern_bucket_count) intern_table_rehash(next_bucket_count);
  }

  uint64_t h = hash_key(str, len);
  size_t bucket = (size_t)(h & (intern_bucket_count - 1));
  
  for (interned_string_t *e = intern_buckets[bucket]; e; e = e->next) {
    if (e->hash == h && e->len == len && memcmp(e->str, str, len) == 0) return e->str;
  }
  
  size_t alloc_size = sizeof(interned_string_t) + len + 1;
  interned_string_t *entry = (interned_string_t *)calloc(1, alloc_size);
  if (!entry) return NULL;
  
  entry->str = (char *)(entry + 1);
  memcpy(entry->str, str, len);
  entry->str[len] = '\0';
  entry->len = len;
  entry->hash = h;
  entry->next = intern_buckets[bucket];
  intern_buckets[bucket] = entry;
  
  intern_count++;
  intern_bytes += alloc_size;
  
  return entry->str;
}

const char *intern_find(const char *str, size_t len) {
  if (!intern_buckets) return NULL;

  uint64_t h = hash_key(str, len);
  size_t bucket = (size_t)(h & (intern_bucket_count - 1));

  for (interned_string_t *e = intern_buckets[bucket]; e; e = e->next) {
    if (e->hash == h && e->len == len && memcmp(e->str, str, len) == 0) return e->str;
  }

  return NULL;
}

const char *intern_find_flat(ant_t *js, const char *bytes, size_t len) {
  ant_intern_memo_entry_t *entry = &js->intern_memo[((uintptr_t)bytes >> 4) & (INTERN_MEMO_SIZE - 1)];
  if (entry->bytes == bytes) return entry->interned;
  
  const char *interned = intern_find(bytes, len);
  if (interned) *entry = (ant_intern_memo_entry_t){ bytes, interned };
  
  return interned;
}

size_t intern_length(const char *interned) {
  return ((const interned_string_t *)(const void *)interned)[-1].len;
}

js_intern_stats_t js_intern_stats(void) {
  return (js_intern_stats_t){ 
    .count = intern_count, 
    .bytes = intern_bytes 
  };
}

struct func_format {
  const char *prefix;
  size_t prefix_len;
  const char *anon;
  size_t anon_len;
};

// TODO: migrate to Symbol.inspect
static const struct func_format formats[] = {
  [0] = { "[Function: ",               11, "[Function (anonymous)]",               22 },
  [1] = { "[AsyncFunction: ",          16, "[AsyncFunction (anonymous)]",          27 },
  [2] = { "[GeneratorFunction: ",      20, "[GeneratorFunction (anonymous)]",      31 },
  [3] = { "[AsyncGeneratorFunction: ", 25, "[AsyncGeneratorFunction (anonymous)]", 36 },
};

static inline bool func_has_code(ant_value_t code_val) {
  uint8_t type = vtype(code_val);
  return type == kTypeSourceCode || type == kTypeString;
}

static const char *func_code_text(ant_t *js, ant_value_t code_val) {
  if (vtype(code_val) == kTypeSourceCode) return (const char *)vptr(code_val);
  if (vtype(code_val) != kTypeString) return NULL;
  ant_offset_t len = 0;
  return (const char *)(uintptr_t)vstr(js, code_val, &len);
}

static size_t strclass(ant_t *js, ant_value_t value, const char *name, ant_offset_t name_len, char *buf, size_t len) {
  size_t n = cpy(buf, len, "[class ", 7);
  n += name && name_len > 0
    ? cpy(buf + n, REMAIN(n, len), name, name_len)
    : cpy(buf + n, REMAIN(n, len), "(anonymous)", 11);

  ant_offset_t parent_len = 0;
  const char *parent = get_func_name(js, get_proto(js, value), &parent_len);
  if (parent && parent_len > 0) {
    n += cpy(buf + n, REMAIN(n, len), " extends ", 9);
    n += cpy(buf + n, REMAIN(n, len), parent, parent_len);
  }

  return n + cpy(buf + n, REMAIN(n, len), "]", 1);
}

// todo: make it work with bytecode NAME
static size_t strfunc(ant_t *js, ant_value_t value, char *buf, size_t len) {
  ant_offset_t name_len = 0;
  const char *name = get_func_name(js, value, &name_len);
  
  ant_value_t func_obj = js_func_obj(value);
  ant_value_t code_slot = get_slot(func_obj, SLOT_CODE);
  ant_value_t builtin_slot = get_slot(func_obj, SLOT_BUILTIN);
  ant_value_t async_slot = get_slot(func_obj, SLOT_ASYNC);
  sv_closure_t *closure = js_func_closure(value);
  
  if (
    closure && ((closure->call_flags & SV_CALL_IS_DEFAULT_CTOR) || 
    (closure->func && closure->func->is_class_ctor))
  ) return strclass(js, value, name, name_len, buf, len);
  
  bool is_async = (async_slot == js_true);
  bool has_code = func_has_code(code_slot);
  bool is_generator = closure != NULL && closure->func != NULL && closure->func->is_generator;
  
  const struct func_format *fmt = &formats[(is_generator ? 2 : 0) | (is_async ? 1 : 0)];
  
  if (vtype(builtin_slot) == kTypeNumber) {
    if (name && name_len > 0) {
      size_t n = cpy(buf, len, fmt->prefix, fmt->prefix_len);
      n += cpy(buf + n, REMAIN(n, len), name, name_len);
      n += cpy(buf + n, REMAIN(n, len), "]", 1);
      return n;
    }
    return cpy(buf, len, fmt->anon, fmt->anon_len);
  }
  
  if (!has_code) {
    ant_value_t cfunc_slot = get_slot(func_obj, SLOT_CFUNC);
    bool is_native = (vtype(cfunc_slot) == kTypeBuiltin);
    size_t n;
    
    if (name && name_len > 0) {
      n = cpy(buf, len, fmt->prefix, fmt->prefix_len);
      n += cpy(buf + n, REMAIN(n, len), name, name_len);
      n += cpy(buf + n, REMAIN(n, len), "]", 1);
    } else {
      n = cpy(buf, len, fmt->anon, fmt->anon_len);
    }
    
    if (!is_native) return n;
    
    ant_value_t proto = get_slot(func_obj, SLOT_PROTO);
    uint8_t pt = vtype(proto);
    if (pt != kTypeObject && pt != kTypeFunction) return n;
    
    ant_value_t ctor = lkp_val(js, proto, "constructor", 11);
    uint8_t ct = vtype(ctor);
    if (ct != kTypeFunction && ct != kTypeBuiltin) return n;
    
    ant_offset_t ctor_name_len = 0;
    const char *ctor_name = get_func_name(js, ctor, &ctor_name_len);
    if (ctor_name && ctor_name_len > 0) {
      n += cpy(buf + n, REMAIN(n, len), " ", 1);
      n += cpy(buf + n, REMAIN(n, len), ctor_name, ctor_name_len);
    }
    return n;
  }
  
  if (name && name_len > 0) {
    size_t n = cpy(buf, len, fmt->prefix, fmt->prefix_len);
    n += cpy(buf + n, REMAIN(n, len), name, name_len);
    n += cpy(buf + n, REMAIN(n, len), "]", 1);
    return n;
  }
  
  return cpy(buf, len, fmt->anon, fmt->anon_len);
}

static size_t strfunc_inspect(ant_t *js, ant_value_t value, char *buf, size_t len) {
  int ref = get_circular_ref(js, value);
  if (ref) return ref > 0 ? (size_t)snprintf(buf, len, "[Circular *%d]", ref) : cpy(buf, len, "[Circular]", 10);

  push_stringify(js, value);
  size_t n = strobj_trailing_props(js, value, buf, len, strfunc(js, value, buf, len));
  pop_stringify(js);
  return stringify_ref_label(js, value, buf, len, n);
}

static size_t strcfunc(ant_t *js, ant_value_t value, char *buf, size_t len) {
  ant_offset_t name_len = 0;
  const char *name = get_func_name(js, value, &name_len);
  const struct func_format *fmt = &formats[0];

  if (name && name_len > 0) {
    size_t n = cpy(buf, len, fmt->prefix, fmt->prefix_len);
    n += cpy(buf + n, REMAIN(n, len), name, name_len);
    n += cpy(buf + n, REMAIN(n, len), "]", 1);
    return n;
  }

  return cpy(buf, len, fmt->anon, fmt->anon_len);
}

static size_t strcfunc_inspect(ant_t *js, ant_value_t value, char *buf, size_t len) {
  size_t n = strcfunc(js, value, buf, len);
  if (!js->stringify || !(js->stringify->flags & ANT_INSPECT_SHOW_HIDDEN)) return n;

  js_inspect_builder_t builder;
  js_inspect_builder_init_fixed(&builder, js, buf, len, n);
  js_inspect_hidden_trailer(&builder, value);
  return builder.n;
}

size_t tostr(ant_t *js, ant_value_t value, char *buf, size_t len) {
  switch (vtype(value)) {
    case kTypeUndefined:   return ANT_COPY(buf, len, "undefined");
    case kTypeNull:    return ANT_COPY(buf, len, "null");
    
    case kTypeBool: {
      bool b = vdata(value) & 1;
      return b ? ANT_COPY(buf, len, "true") : ANT_COPY(buf, len, "false");
    }
    
    case kTypeArray:    return strarr(js, value, buf, len);
    case kTypeObject:   return strobj(js, value, buf, len);
    case kTypeString:   return strstring(js, value, buf, len);
    case kTypePromise:  return strpromise(js, value, buf, len);
    case kTypeFunction: return strfunc_inspect(js, value, buf, len);
    case kTypeBuiltin:  return strcfunc_inspect(js, value, buf, len);
    
    case kTypeNumber: {
      if (tod(value) == 0 && signbit(tod(value))) return ANT_COPY(buf, len, "-0");
      return js_number_to_chars(value, buf, len);
    }
    
    case kTypeBigInt: {
      size_t n = strbigint(js, value, buf, len);
      return n + cpy(buf + n, REMAIN(n, len), "n", 1);
    }
    
    case kTypeError: {
      if (is_err(value)) {
        ant_value_t stack = Ant_Exception_Stack(js, value);
        if (vtype(stack) == kTypeString) {
          ant_offset_t slen;
          ant_offset_t off = vstr(js, stack, &slen);
          return cpy(buf, len, (const char *)(uintptr_t)(off), slen);
        }
      }
      return ANT_COPY(buf, len, "Error");
    }
    
    case kTypeSymbol: {
      const char *desc = js_sym_desc(value);
      if (desc) return (size_t) snprintf(buf, len, "Symbol(%s)", desc);
      return ANT_COPY(buf, len, "Symbol()");
    }
    
    case kTypeGenerator: return strobj(js, mkval(kTypeObject, vdata(value)), buf, len);
    default: return (size_t) snprintf(buf, len, "VTYPE%d", vtype(value));
  }
}

static char *tostr_alloc(ant_t *js, ant_value_t value) {
  size_t cap = 64;
  char *buf = calloc(1, cap);
  size_t n = tostr(js, value, buf, cap);
  if (n >= cap) {
    free(buf);
    buf = calloc(1, n + 1);
    tostr(js, value, buf, n + 1);
  }
  return buf;
}

static js_cstr_t js_cstr_render(
  ant_t *js, ant_value_t value,
  char *stack_buf, size_t stack_size, bool quote_strings
) {
  js_cstr_t out = { .ptr = "", .len = 0, .needs_free = false };

  if (is_err(value)) {
    if (is_err(value)) {
      ant_value_t stack = Ant_Exception_Stack(js, value);
      if (vtype(stack) == kTypeString) {
        ant_offset_t slen;
        ant_offset_t off = vstr(js, stack, &slen);
        out.ptr = (const char *)(uintptr_t)(off);
        out.len = slen;
        return out;
      }
    }
    out.ptr = "Error";
    out.len = 5;
    return out;
  }

  if (!quote_strings && vtype(value) == kTypeString) {
    size_t len = 0;
    char *str = js_getstr(js, value, &len);
    out.ptr = str ? str : ""; out.len = len;
    return out;
  }

  size_t capacity = stack_size;
  char *buf = stack_buf;
  out.needs_free = false;

  if (!buf || capacity == 0) {
    capacity = 64;
    buf = calloc(1, capacity);
    if (!buf) return out;
    out.needs_free = true;
  }

  for (;;) {
    stringify_begin_pass(js);
    size_t len = tostr(js, value, buf, capacity);

    if (len < capacity - 1) {
      out.ptr = buf;
      out.len = len;
      return out;
    }

    size_t new_capacity = capacity * 2;
    char *next = out.needs_free 
      ? realloc(buf, new_capacity) 
      : calloc(1, new_capacity);
    
    if (!next) {
      if (out.needs_free) free(buf);
      out.ptr = ""; out.len = 0;
      out.needs_free = false;
      return out;
    }

    if (!out.needs_free) {
      memcpy(next, buf, capacity);
      out.needs_free = true;
    }

    buf = next;
    capacity = new_capacity;
  }
}

static js_cstr_t js_cstr_impl(
  ant_t *js, ant_value_t value, char *stack_buf, size_t stack_size, 
  bool quote_strings, int max_depth, uint8_t flags
) {
  ant_stringify_ctx_t ctx;
  ant_stringify_enter(js, &ctx, max_depth);
  ctx.flags = flags;
  
  js_cstr_t out = js_cstr_render(js, value, stack_buf, stack_size, quote_strings);
  Ant_List_Remove(&ctx);
  
  return out;
}

js_cstr_t js_to_cstr(ant_t *js, ant_value_t value, char *stack_buf, size_t stack_size) {
  return js_cstr_impl(js, value, stack_buf, stack_size, false, ANT_INSPECT_MAX_DEPTH, 0);
}

js_cstr_t js_inspect_cstr(ant_t *js, ant_value_t value, char *stack_buf, size_t stack_size) {
  return js_cstr_impl(js, value, stack_buf, stack_size, true, ANT_INSPECT_MAX_DEPTH, 0);
}

js_cstr_t js_inspect_cstr_depth(ant_t *js, ant_value_t value, char *stack_buf, size_t stack_size, int max_depth) {
  return js_cstr_impl(js, value, stack_buf, stack_size, true, max_depth, 0);
}

js_cstr_t js_inspect_cstr_opts(ant_t *js, ant_value_t value, char *stack_buf, size_t stack_size, int max_depth, uint8_t flags) {
  return js_cstr_impl(js, value, stack_buf, stack_size, true, max_depth, flags);
}

ant_value_t js_tostring_val(ant_t *js, ant_value_t value) {
  uint8_t t = vtype(value);
  char *buf; size_t len, buflen;
  
  static const void *jump_table[] = {
    [kTypeObject] = &&L_OBJ, [kTypeString] = &&L_STR,
    [kTypeUndefined] = &&L_UNDEF, [kTypeNull] = &&L_NULL, [kTypeNumber] = &&L_NUM,
    [kTypeBool] = &&L_BOOL, [kTypeFunction] = &&L_OBJ, [kTypeBuiltin] = &&L_OBJ,
    [kTypeError] = &&L_DEFAULT, [kTypeArray] = &&L_OBJ,
    [kTypePromise] = &&L_DEFAULT, [kTypeTypedArray] = &&L_DEFAULT,
    [kTypeBigInt] = &&L_BIGINT, [kTypeSymbol] = &&L_DEFAULT,
    [kTypeGenerator] = &&L_DEFAULT
  };
  
  if (t < sizeof(jump_table) / sizeof(jump_table[0])) goto *jump_table[t];
  goto L_DEFAULT;

  L_STR:   return value;
  L_UNDEF: return js_mkstr(js, "undefined", 9);
  L_NULL:  return js_mkstr(js, "null", 4);
  L_BOOL:  return vdata(value) ? js_mkstr(js, "true", 4) : js_mkstr(js, "false", 5);
  L_OBJ:   return coerce_to_str_hint(js, value, 1);
  
  L_NUM: {
    int32_t integer;
    if (ant_number_as_int32(tod(value), &integer)) return js_mkstr_int32(js, integer);
    char num_buf[32];
    len = js_number_to_chars(value, num_buf, sizeof(num_buf));
    return js_mkstr_ascii(js, num_buf, len);
  }
    
  L_BIGINT: {
    buflen = bigint_digits_len(js, value);
    char bigint_stack_buf[128];
    
    size_t capacity = buflen + 2;
    bool needs_free = capacity > sizeof(bigint_stack_buf);
    buf = needs_free ? (char *)calloc(1, capacity) : bigint_stack_buf;
    
    if (!buf) return js_mkerr(js, "oom");
    len = strbigint(js, value, buf, capacity);
    
    ant_value_t result = js_mkstr(js, buf, len);
    if (needs_free) free(buf);
    
    return result;
  }
    
  L_DEFAULT: {
    buf = tostr_alloc(js, value);
    ant_value_t result = js_mkstr(js, buf, strlen(buf));
    free(buf); return result;
  }
}

static const char *js_str_render(ant_t *js, ant_value_t value) {
  if (is_err(value)) {
    if (is_err(value)) {
      ant_value_t stack = Ant_Exception_Stack(js, value);
      if (vtype(stack) == kTypeString) {
        ant_offset_t slen, off = vstr(js, stack, &slen);
        return (const char *)(uintptr_t)off;
      }
    }
    return "Error";
  }
  
  size_t capacity = 4096;
  char *buf = (char *)calloc(1, capacity);
  if (!buf) return "";
  
  size_t len;
  for (;;) {
    stringify_begin_pass(js);
    len = tostr(js, value, buf, capacity);
    
    if (len < capacity - 1) break;
    
    capacity *= 2;
    buf = (char *)realloc(buf, capacity);
    if (!buf) return "";
  }
  
  ant_value_t str = js_mkstr(js, buf, len);
  free(buf);
  
  if (is_err(str)) return "";
  ant_offset_t off = vstr(js, str, NULL);
  return (const char *)(uintptr_t)off;
}

const char *js_str(ant_t *js, ant_value_t value) {
  ant_stringify_ctx_t ctx;
  ant_stringify_enter(js, &ctx, ANT_INSPECT_MAX_DEPTH);
  
  const char *out = js_str_render(js, value);
  Ant_List_Remove(&ctx);
  
  return out;
}

static inline ant_offset_t get_dense_buf(ant_value_t arr) {
  ant_object_t *ptr = js_obj_ptr(js_as_obj(arr));
  if (!ptr || !ptr->flags.fast_array || !ptr->u.array.data || ptr->u.array.cap == 0) return 0;
  return (ant_offset_t)(uintptr_t)ptr;
}

static inline ant_object_t *dense_obj(ant_offset_t doff) {
  if (!doff) return NULL;
  ant_object_t *ptr = (ant_object_t *)(uintptr_t)doff;
  if (!ptr || !ptr->flags.fast_array || !ptr->u.array.data || ptr->u.array.cap == 0) return NULL;
  return ptr;
}

static inline ant_value_t *dense_data(ant_offset_t doff) {
  ant_object_t *ptr = dense_obj(doff);
  return ptr ? ptr->u.array.data : NULL;
}

static inline ant_offset_t dense_capacity(ant_offset_t doff) {
  ant_object_t *ptr = dense_obj(doff);
  return ptr ? (ant_offset_t)ptr->u.array.cap : 0;
}

static inline ant_value_t dense_get(ant_offset_t doff, ant_offset_t idx) {
  ant_object_t *ptr = dense_obj(doff);
  if (!ptr || idx >= ptr->u.array.cap) return js_mkundef();
  return ptr->u.array.data[idx];
}

static inline void dense_set(ant_t *js, ant_offset_t doff, ant_offset_t idx, ant_value_t val) {
  ant_object_t *ptr = dense_obj(doff);
  if (!ptr || idx >= ptr->u.array.cap) return;
  ptr->u.array.data[idx] = val;
  if (!is_empty_slot(val)) ptr->flags.may_have_dense_elements = 1;
  gc_write_barrier_elem(js, ptr, (uint32_t)idx, val);
}

static ant_offset_t dense_grow(ant_t *js, ant_value_t arr, ant_offset_t needed) {
  ant_object_t *obj = js_obj_ptr(js_as_obj(arr));
  
  if (!obj) return 0;
  if (needed > UINT32_MAX) return 0;

  ant_offset_t old_cap = obj->u.array.cap;
  ant_offset_t new_cap = old_cap ? old_cap : MAX_DENSE_INITIAL_CAP;

  while (new_cap < needed) new_cap *= 2;
  if (new_cap > UINT32_MAX) new_cap = UINT32_MAX;
  if ((size_t)new_cap > SIZE_MAX / sizeof(ant_value_t)) return 0;

  ant_value_t *next = gc_array_storage_grow(js, obj->u.array.data, (uint32_t)old_cap, (uint32_t)new_cap);
  if (!next) return 0;
  
  gc_array_grew(js);
  for (ant_offset_t i = old_cap; i < new_cap; i++) next[i] = T_EMPTY;

  obj->u.array.data = next;
  obj->u.array.cap = (uint32_t)new_cap;
  obj->flags.dense_length_fits = obj->u.array.len <= obj->u.array.cap;
  obj->flags.fast_array = 1;
  
  return (ant_offset_t)(uintptr_t)obj;
}

// TODO: make get and set dry
static inline ant_value_t arr_get(ant_t *js, ant_value_t arr, ant_offset_t idx) {
  ant_object_t *fast = array_obj_ptr(arr);
  
  if (
    fast && fast->flags.fast_array && fast->u.array.data &&
    idx < (ant_offset_t)fast->u.array.len &&
    idx < (ant_offset_t)fast->u.array.cap
  ) {
    ant_value_t v = fast->u.array.data[idx];
    if (!is_empty_slot(v)) return v;
  }

  ant_offset_t semantic_len = get_array_length(js, arr);
  
  if (idx >= semantic_len) return js_mkundef();
  ant_offset_t doff = get_dense_buf(arr);
  
  if (doff) {
    ant_offset_t len = dense_iterable_length(js, arr);
    if (idx < len) {
      ant_value_t v = dense_get(doff, idx);
      if (!is_empty_slot(v)) return v;
    }
  }
  
  char idxstr[16];
  size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);

  ant_value_t cur = arr;
  for (int depth = 0; is_object_type(cur) && depth < MAX_PROTO_CHAIN_DEPTH; depth++) {
    ant_value_t cur_obj = js_as_obj(cur);
    if (!array_obj_ptr(cur_obj)) goto shape_lookup;

    ant_offset_t cur_doff = get_dense_buf(cur_obj);
    if (!cur_doff) goto shape_lookup;

    ant_offset_t cur_len = dense_iterable_length(js, cur_obj);
    if (idx >= cur_len) goto shape_lookup;

    ant_value_t v = dense_get(cur_doff, idx);
    if (!is_empty_slot(v)) return v;

shape_lookup:;
    ant_prop_loc_t off = lkp(js, cur_obj, idxstr, idxlen);
    if (off.obj) {
      const ant_shape_prop_t *prop_meta = prop_shape_meta(off);
      if (prop_meta && prop_meta->has_getter) {
        ant_value_t accessor_result;
        if (try_accessor_getter(js, arr, idxstr, idxlen, &accessor_result))
          return accessor_result;
      }
      return js_prop_load(off);
    }

    ant_value_t proto = get_proto(js, cur_obj);
    if (!is_object_type(proto)) break;
    cur = proto;
  }

  return js_mkundef();
}

static inline void arr_set(ant_t *js, ant_value_t arr, ant_offset_t idx, ant_value_t val) {
  ant_offset_t cur_len = get_array_length(js, arr);
  ant_offset_t doff = get_dense_buf(arr);
  
  if (doff) {
    ant_offset_t len = dense_iterable_length(js, arr);
    
    if (idx < len) {
      dense_set(js, doff, idx, val);
      return;
    }
    
    ant_offset_t density_limit = len > 0 ? len * 4 : 64;
    if (idx >= density_limit) goto sparse;
    
    ant_offset_t cap = dense_capacity(doff);
    if (idx >= cap) {
      doff = dense_grow(js, arr, idx + 1);
      if (doff == 0) goto sparse;
    }
    
    if (idx > len) array_mark_may_have_holes(arr);
    for (ant_offset_t i = len; i < idx; i++) {
      ant_value_t v = dense_get(doff, i);
      if (!is_empty_slot(v) && vtype(v) == kTypeUndefined) dense_set(js, doff, i, T_EMPTY);
    }
    
    dense_set(js, doff, idx, val);
    if (idx >= cur_len) array_len_set(js, arr, idx + 1);
    
    return;
  }
  
  sparse:;
  char idxstr[24];
  size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (uint64_t)idx);
  ant_value_t key = js_mkstr(js, idxstr, idxlen);
  
  js_setprop(js, arr, key, val);
}

static bool arr_own_dense_element(
  ant_t *js, ant_value_t arr, const char *key, size_t klen, ant_value_t *out
) {
  unsigned long idx;
  ant_offset_t doff = get_dense_buf(arr);
  if (!doff || !parse_array_index(key, klen, dense_iterable_length(js, arr), &idx)) return false;
  ant_value_t v = dense_get(doff, (ant_offset_t)idx);
  if (is_empty_slot(v)) return false;
  if (out) *out = v;
  return true;
}

static inline bool arr_has(ant_t *js, ant_value_t arr, ant_offset_t idx) {
  ant_offset_t semantic_len = get_array_length(js, arr);
  if (idx >= semantic_len) return false;
  ant_offset_t doff = get_dense_buf(arr);
  
  if (doff) {
    ant_offset_t len = dense_iterable_length(js, arr);
    if (idx < len) {
      ant_value_t v = dense_get(doff, idx);
      if (!is_empty_slot(v)) return true;
    }
  }
  
  char idxstr[16];
  size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);

  ant_value_t cur = arr;
  for (int depth = 0; is_object_type(cur) && depth < MAX_PROTO_CHAIN_DEPTH; depth++) {
    ant_value_t cur_obj = js_as_obj(cur);
    if (!array_obj_ptr(cur_obj)) goto shape_has;

    ant_offset_t cur_doff = get_dense_buf(cur_obj);
    if (!cur_doff) goto shape_has;

    ant_offset_t cur_len = dense_iterable_length(js, cur_obj);
    if (idx >= cur_len) goto shape_has;
    if (!is_empty_slot(dense_get(cur_doff, idx))) return true;

shape_has:;
    if (lkp(js, cur_obj, idxstr, idxlen).obj) return true;

    ant_value_t proto = get_proto(js, cur_obj);
    if (!is_object_type(proto)) break;
    cur = proto;
  }

  return false;
}

static inline bool array_result_extensible(ant_value_t result) {
  ant_object_t *ptr = is_object_type(result) ? js_obj_ptr(js_as_obj(result)) : NULL;
  return !ptr || ptr->flags.extensible;
}

static __attribute__((noinline)) ant_value_t array_result_set_checked(ant_t *js, ant_value_t result, ant_offset_t idx, ant_value_t val) {
  if (!array_result_extensible(result)) {
    ant_object_t *ptr = js_obj_ptr(js_as_obj(result));
    bool has = arr_has(js, result, idx);
    if (ptr->flags.sealed && has)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot redefine property: %u", (unsigned)idx);
    if (!has)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot add property %u, object is not extensible", (unsigned)idx);
  }
  
  arr_set(js, result, idx, val);
  return js_mkundef();
}

static inline ant_value_t array_result_set(ant_t *js, ant_value_t result, bool fresh, ant_offset_t idx, ant_value_t val) {
  if (__builtin_expect(!fresh, 0)) return array_result_set_checked(js, result, idx, val);
  arr_set(js, result, idx, val);
  return js_mkundef();
}

// TODO: move into arguments.c
enum { ANT_ARGUMENTS_NATIVE_TAG = 0x41524753u }; // ARGS

typedef struct {
  sv_frame_t *direct_frame;
  sv_upvalue_t **cells;
  int frame_index;
  uint32_t mapped_count;
  uint8_t in_setter;
  uint8_t deleted[];
} ant_arguments_state_t;

static inline sv_frame_t *js_arguments_frame(ant_t *js, ant_arguments_state_t *state) {
  if (state->direct_frame) return state->direct_frame;
  return &js->vm->frames[state->frame_index];
}

static inline ant_arguments_state_t *js_arguments_state(ant_value_t obj) {
  return (ant_arguments_state_t *)js_get_native(obj, ANT_ARGUMENTS_NATIVE_TAG);
}

static inline sv_upvalue_t *js_arguments_cell(ant_arguments_state_t *state, unsigned long idx) {
  if (!state || !state->cells || idx >= state->mapped_count || state->deleted[idx]) return NULL;
  return state->cells[idx];
}

static inline ant_value_t js_arguments_mapped_value(ant_t *js, ant_arguments_state_t *state, unsigned long idx, bool *mapped) {
  sv_upvalue_t *cell = js_arguments_cell(state, idx);
  *mapped = true;
  if (cell) return *cell->location;
  if (state && state->frame_index >= 0 && idx < state->mapped_count && !state->deleted[idx])
    return js_arguments_frame(js, state)->bp[idx];
  *mapped = false;
  return js_mkundef();
}

static inline void js_arguments_write_mapped(ant_t *js, ant_arguments_state_t *state, unsigned long idx, ant_value_t value) {
  sv_upvalue_t *cell = js_arguments_cell(state, idx);
  if (cell) {
    *cell->location = value;
    gc_upvalue_write_barrier(js, cell, value);
  } else if (state && state->frame_index >= 0 && idx < state->mapped_count && !state->deleted[idx])
    js_arguments_frame(js, state)->bp[idx] = value;
}

static void js_arguments_link_cell(ant_t *js, ant_value_t obj, ant_arguments_state_t *state, size_t idx, sv_upvalue_t *uv) {
  if (!state || idx >= state->mapped_count || idx > UINT8_MAX || state->deleted[idx]) return;
  state->cells[idx] = uv;
  uv->maps_arguments = 1;
  uv->args_index = (uint8_t)idx;
  uv->args_owner = (uint32_t)(((uint8_t *)js_obj_ptr(obj) - js->obj_arena.base) / js->obj_arena.elem_size);
}

void js_arguments_link_upvalue(ant_t *js, sv_frame_t *frame, sv_upvalue_t *uv) {
  if (!frame->bp || uv->location < frame->bp) return;
  ant_value_t obj = frame->arguments_obj;
  js_arguments_link_cell(js, obj, js_arguments_state(obj), (size_t)(uv->location - frame->bp), uv);
}

void js_arguments_upvalue_written(ant_t *js, sv_upvalue_t *uv, ant_value_t value) {
  ant_object_t *owner = (ant_object_t *)(js->obj_arena.base + (size_t)uv->args_owner * js->obj_arena.elem_size);
  if (!fixed_arena_contains(&js->obj_arena, owner) || owner->mark_epoch == ANT_GC_DEAD) return;
  if (!ant_object_is_arguments(owner)) return;

  ant_value_t obj = js_obj_from_ptr(owner);
  ant_arguments_state_t *state = js_arguments_state(obj);
  uint32_t idx = uv->args_index;
  if (!state || idx >= state->mapped_count || state->cells[idx] != uv || state->deleted[idx]) return;

  if (owner->u.array.data && idx < owner->u.array.len && idx < owner->u.array.cap) {
    owner->u.array.data[idx] = value;
    gc_write_barrier(js, owner, value);
    return;
  }
  state->in_setter = 1;
  arr_set(js, obj, (ant_offset_t)idx, value);
  state->in_setter = 0;
}

void gc_mark_arguments_cells(ant_t *js, ant_value_t obj) {
  ant_arguments_state_t *state = js_arguments_state(obj);
  if (state && state->cells) gc_mark_upvalue_cells(js, state->cells, state->mapped_count);
}

static ant_value_t js_arguments_getter(ant_t *js, ant_value_t obj, const char *key, size_t key_len) {
  ant_offset_t arr_len = get_array_length(js, obj);
  unsigned long idx = 0;
  
  if (!parse_array_index(key, key_len, arr_len, &idx)) return js_mkundef();
  if ((ant_offset_t)idx >= arr_len) return js_mkundef();

  bool mapped = false;
  ant_value_t value = js_arguments_mapped_value(js, js_arguments_state(obj), idx, &mapped);
  return mapped ? value : arr_get(js, obj, (ant_offset_t)idx);
}

static bool js_arguments_setter(
  ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t value
) {
  unsigned long idx = 0;
  if (!parse_array_index(key, key_len, (ant_offset_t)-1, &idx)) return false;

  ant_arguments_state_t *state = js_arguments_state(obj);
  if (state && state->in_setter) return false;
  if (state) state->in_setter = 1;
  arr_set(js, obj, (ant_offset_t)idx, value);
  
  if (state) state->in_setter = 0;
  js_arguments_write_mapped(js, state, idx, value);
  return true;
}

typedef struct {
  bool accessor;
  bool has_value;
  bool has_writable;
  bool writable;
  bool has_enumerable;
  bool enumerable;
  bool has_configurable;
  bool configurable;
  bool abrupt;
  ant_value_t value;
} define_desc_fields_t;

static inline ant_value_t define_abrupt(define_desc_fields_t *fields, ant_value_t err) {
  if (fields) fields->abrupt = true;
  return err;
}

static void js_arguments_after_define(
  ant_t *js, ant_value_t obj, const char *key, 
  size_t key_len, const define_desc_fields_t *desc
) {
  unsigned long idx = 0;
  if (!parse_array_index(key, key_len, (ant_offset_t)-1, &idx)) return;
  
  ant_arguments_state_t *state = js_arguments_state(obj);
  if (!state || (uint32_t)idx >= state->mapped_count || state->deleted[idx]) return;

  if (desc->accessor) {
    state->deleted[idx] = 1;
    return;
  }

  if (desc->has_value) js_arguments_write_mapped(js, state, idx, desc->value);
  if (desc->has_writable && !desc->writable) state->deleted[idx] = 1;
}

static void js_arguments_unmap_all(ant_t *js, ant_value_t obj) {
  ant_arguments_state_t *state = js_arguments_state(obj);
  if (!state) return;
  
  for (uint32_t i = 0; i < state->mapped_count; i++) {
    bool mapped = false;
    ant_value_t value = js_arguments_mapped_value(js, state, i, &mapped);
    if (!mapped) continue;
    
    state->in_setter = 1;
    arr_set(js, obj, (ant_offset_t)i, value);
    state->in_setter = 0;
    state->deleted[i] = 1;
  }
}

static bool js_arguments_deleter(ant_t *js, ant_value_t obj, const char *key, size_t key_len) {
  unsigned long idx = 0;
  if (!parse_array_index(key, key_len, (ant_offset_t)-1, &idx)) return true;

  ant_arguments_state_t *state = js_arguments_state(obj);
  if (state && (uint32_t)idx < state->mapped_count) state->deleted[idx] = 1;
  return true;
}

static const ant_exotic_ops_t js_arguments_exotic_ops = {
  .getter = js_arguments_getter,
  .setter = js_arguments_setter,
  .deleter = js_arguments_deleter,
};

static void js_arguments_finalizer(ant_t *js, ant_object_t *obj) {
  ant_value_t value = js_obj_from_ptr(obj);
  ant_arguments_state_t *state = js_arguments_state(value);
  
  free(state);
  js_clear_native(value, ANT_ARGUMENTS_NATIVE_TAG);
}

// TODO: arrays.c
static ant_value_t alloc_array_with_proto_capacity(
  ant_t *js, ant_value_t proto, uint32_t minimum_capacity,
  uint32_t overwritten_prefix, bool exact_capacity
) {
  ant_object_t *obj = obj_alloc(js, kTypeArray, (uint8_t)ANT_INOBJ_MAX_SLOTS);
  if (!obj) return js_mkerr(js, "oom");
  
  ant_value_t arr = mkref(kTypeArray, obj);
  if (is_object_type(proto)) js_set_proto_init(arr, proto);

  uint32_t capacity = exact_capacity
    ? (minimum_capacity ? minimum_capacity : 1u) 
    : MAX_DENSE_INITIAL_CAP;
  
  while (capacity < minimum_capacity && capacity <= UINT32_MAX / 2) capacity *= 2;
  if (capacity < minimum_capacity) capacity = minimum_capacity;
  if ((size_t)capacity > SIZE_MAX / sizeof(*obj->u.array.data)) return js_mkerr(js, "oom");

  obj->u.array.cap = capacity;
  obj->u.array.len = 0;
  obj->u.array.data = gc_array_storage_alloc(js, obj->u.array.cap);
  
  if (obj->u.array.data) {
    gc_array_grew(js);
    ant_value_t *data = obj->u.array.data;
    size_t fill_start = overwritten_prefix < capacity ? overwritten_prefix : capacity;
    for (size_t i = fill_start; i < capacity; i++) data[i] = T_EMPTY;
    obj->flags.fast_array = 1;
    obj->flags.dense_length_fits = 1;
    obj->flags.may_have_holes = 0;
    obj->flags.may_have_dense_elements = 0;
  } else {
    obj->u.array.cap = 0;
    obj->u.array.len = 0;
    obj->flags.fast_array = 0;
    obj->flags.may_have_holes = 1;
    obj->flags.may_have_dense_elements = 1;
  }

  return arr;
}

static ant_value_t alloc_array_with_proto(ant_t *js, ant_value_t proto) {
  return alloc_array_with_proto_capacity(js, proto, MAX_DENSE_INITIAL_CAP, 0, false);
}

// TODO: flatten
static inline ant_value_t mkarr(ant_t *js) {
  return alloc_array_with_proto(js, js->sym.array_proto);
}

ant_value_t js_mkarr(ant_t *js) { 
  return mkarr(js); 
}

static ant_value_t mkarr_dense_literal_capacity(
  ant_t *js, const ant_value_t *elements, uint32_t count, bool exact_capacity
) {
  uint32_t capacity = exact_capacity ? gc_array_storage_fit(count) : count;
  ant_value_t arr = alloc_array_with_proto_capacity(js, js->sym.array_proto, capacity, count, exact_capacity);
  if (is_err(arr)) return arr;

  ant_object_t *obj = array_obj_ptr(arr);
  if (!obj || !obj->flags.fast_array || !obj->u.array.data || obj->u.array.cap < count) {
    if (obj && obj->u.array.data) {
      uint32_t reset_count = count < obj->u.array.cap ? count : obj->u.array.cap;
      for (uint32_t i = 0; i < reset_count; i++) obj->u.array.data[i] = T_EMPTY;
    }
    
    for (uint32_t i = 0; i < count; i++) js_arr_push(js, arr, elements[i]);
    return arr;
  }

  bool has_holes = false;
  bool has_elements = false;
  
  for (uint32_t i = 0; i < count; i++) {
    ant_value_t value = elements[i];
    obj->u.array.data[i] = value;
    if (is_empty_slot(value)) has_holes = true;
    else has_elements = true;
  }
  
  obj->u.array.len = count;
  obj->flags.may_have_holes = has_holes;
  obj->flags.may_have_dense_elements = has_elements;
  
  return arr;
}

ant_value_t js_mkarr_dense_literal(ant_t *js, const ant_value_t *elements, uint32_t count) {
  return mkarr_dense_literal_capacity(js, elements, count, false);
}

ant_value_t js_mkarr_dense_uninit(ant_t *js, uint32_t count, ant_value_t **out_data) {
  *out_data = NULL;
  if (count == 0) return mkarr(js);
  
  ant_value_t arr = alloc_array_with_proto_capacity(js, js->sym.array_proto, gc_array_storage_fit(count), count, true);
  if (is_err(arr)) return arr;

  ant_object_t *obj = array_obj_ptr(arr);
  if (!obj || !obj->flags.fast_array || !obj->u.array.data || obj->u.array.cap < count)
    return js_mkerr(js, "oom");

  obj->u.array.len = count;
  obj->flags.may_have_dense_elements = 1;
  *out_data = obj->u.array.data;
  
  return arr;
}

static ant_value_t builtin_arguments_callee_thrower(ant_params_t) {
  return js_mkerr_typed(js, JS_ERR_TYPE,
    "'caller', 'callee', and 'arguments' properties may not be accessed "
    "on strict mode functions or the arguments objects for calls to them"
  );
}

static ant_value_t arguments_template(ant_t *js, bool is_strict, bool has_iterator) {
  ant_value_t *cached = is_strict
    ? (has_iterator ? &js->builtins.arguments_iter_template : &js->builtins.arguments_template)
    : (has_iterator ? &js->builtins.sloppy_arguments_iter_template : &js->builtins.sloppy_arguments_template);
    
  if (vtype(*cached) == kTypeObject) return *cached;

  GC_ROOT_SAVE(roots, js);
  ant_value_t seed = Ant_Object_CreateBare(js);
  
  if (is_err(seed)) return seed;
  GC_ROOT_PIN(js, seed);
  
  setprop_cstr(js, seed, "length", 6, tov(0));
  js_set_descriptor(js, seed, "length", 6, JS_DESC_W | JS_DESC_C);
  if (is_strict) {
    static const ant_cfunc_meta_t thrower_meta = { builtin_arguments_callee_thrower, "", 0, 0 };
    ant_value_t thrower = js_mkfun_meta(&thrower_meta);
    js_set_accessor_desc(js, seed, "callee", 6, thrower, thrower, 0);
  } else {
    setprop_cstr(js, seed, "callee", 6, js_mkundef());
    js_set_descriptor(js, seed, "callee", 6, JS_DESC_W | JS_DESC_C);
  }
  
  if (has_iterator) {
    js_set_sym(js, seed, js->sym.iterator_sym, js_mkundef());
    js_set_sym_descriptor(js, seed, js->sym.iterator_sym, JS_DESC_W | JS_DESC_C);
  }
  
  ant_object_t *ptr = js_obj_ptr(seed);
  if (ptr->prop_count != (has_iterator ? 3u : 2u) || ant_object_inobj_limit(ptr) < ptr->prop_count) {
    GC_ROOT_RESTORE(js, roots);
    return js_mkerr(js, "oom");
  }
  
  ANT_ASSERT(
    ant_shape_lookup_interned(ptr->shape, js->intern.length) == ANT_ARGUMENTS_SLOT_LENGTH &&
    ant_shape_lookup_interned(ptr->shape, intern_string("callee", 6)) == ANT_ARGUMENTS_SLOT_CALLEE,
    "arguments template slots out of order"
  );
  
  *cached = seed;
  GC_ROOT_RESTORE(js, roots);
  
  return seed;
}

ant_value_t js_create_arguments_object(
  ant_t *js, sv_vm_t *vm,
  ant_value_t callee, sv_frame_t *frame,
  int argc, int mapped_count, bool is_strict
) {
  GC_ROOT_SAVE(root_mark, js);

  uint32_t count = frame && frame->bp && argc > 0 ? (uint32_t)argc : 0;
  ant_value_t arr = mkarr_dense_literal_capacity(js, count ? frame->bp : NULL, count, true);
  
  if (is_err(arr)) {
    GC_ROOT_RESTORE(js, root_mark);
    return arr;
  } 
  
  GC_ROOT_PIN(js, arr);
  ant_object_t *target = js_obj_ptr(arr);
  
  target->flags.arguments_object = 1;
  if (is_object_type(js->sym.object_proto)) js_set_proto_init(arr, js->sym.object_proto);

  ant_value_t iter_fn = js->sym.array_values_fn;
  bool has_iterator =
    vtype(js->sym.iterator_sym) == kTypeSymbol &&
    (vtype(iter_fn) == kTypeFunction || vtype(iter_fn) == kTypeBuiltin);
  GC_ROOT_PIN(js, iter_fn);
  
  ant_value_t seed = arguments_template(js, is_strict, has_iterator);
  if (is_err(seed)) {
    GC_ROOT_RESTORE(js, root_mark);
    return seed;
  }
  
  ant_object_t *source = js_obj_ptr(seed);
  ant_shape_retain(source->shape);
  ant_shape_release(target->shape);
  
  target->shape = source->shape;
  target->inobj_limit = source->inobj_limit;
  target->prop_count = source->prop_count;
  target->inobj[ANT_ARGUMENTS_SLOT_LENGTH] = tov((double)count);
  target->inobj[ANT_ARGUMENTS_SLOT_CALLEE] = is_strict ? source->inobj[ANT_ARGUMENTS_SLOT_CALLEE] : callee;
  gc_write_barrier(js, target, target->inobj[ANT_ARGUMENTS_SLOT_CALLEE]);
  
  if (has_iterator) {
    target->inobj[ANT_ARGUMENTS_SLOT_ITERATOR] = iter_fn;
    gc_write_barrier(js, target, iter_fn);
  }

  if (!is_strict && mapped_count > 0 && frame && vm) {
    size_t cells_off = (sizeof(ant_arguments_state_t) + (size_t)mapped_count + _Alignof(sv_upvalue_t *) - 1) & ~(_Alignof(sv_upvalue_t *) - 1);
    ant_arguments_state_t *state = calloc(1, cells_off + (size_t)mapped_count * sizeof(sv_upvalue_t *));
    
    if (!state) {
      GC_ROOT_RESTORE(js, root_mark);
      return js_mkerr(js, "oom");
    }

    state->cells = (sv_upvalue_t **)((uint8_t *)state + cells_off);
    state->frame_index = (int)(frame - vm->frames);
    state->mapped_count = (uint32_t)mapped_count;
    
    js_set_native(arr, state, ANT_ARGUMENTS_NATIVE_TAG);
    target->finalizer = js_arguments_finalizer;
    target->flags.is_exotic = 1;
    target->exotic_ops = &js_arguments_exotic_ops;

    for (sv_upvalue_t *uv = vm->open_upvalues; uv; uv = uv->next) {
      if (uv->location < frame->bp) break;
      if (uv->location < frame->bp + mapped_count)
        js_arguments_link_cell(js, arr, state, (size_t)(uv->location - frame->bp), uv);
    }
  }

  GC_ROOT_RESTORE(js, root_mark);
  return arr;
}

void js_arguments_rebind_frame(ant_t *js, ant_value_t obj, int frame_index) {
  ant_arguments_state_t *state = js_arguments_state(obj);
  if (!state || state->frame_index < 0) return;
  state->frame_index = frame_index;
  state->direct_frame = NULL;
}

void js_arguments_bind_direct(ant_t *js, ant_value_t obj, sv_frame_t *frame) {
  ant_arguments_state_t *state = js_arguments_state(obj);
  if (!state || state->frame_index < 0 || !frame) return;
  state->direct_frame = frame;
}

void js_arguments_detach(ant_t *js, ant_value_t obj) {
  ant_arguments_state_t *state = js_arguments_state(obj);
  if (!state || state->frame_index < 0) return;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, obj);

  sv_frame_t *frame = js_arguments_frame(js, state);
  ant_offset_t arr_len = get_array_length(js, obj);
  ant_offset_t limit = (ant_offset_t)state->mapped_count;
  if (arr_len < limit) limit = arr_len;

  for (ant_offset_t i = 0; i < limit; i++) {
    if (state->deleted[i]) continue;
    sv_upvalue_t *cell = js_arguments_cell(state, (unsigned long)i);
    state->in_setter = 1;
    arr_set(js, obj, i, cell ? *cell->location : frame->bp[i]);
    state->in_setter = 0;
  }

  state->frame_index = -1;
  state->direct_frame = NULL;
  GC_ROOT_RESTORE(js, root_mark);
}

void js_arguments_sync_slot(ant_t *js, ant_value_t obj, uint32_t idx, ant_value_t value) {
  ant_arguments_state_t *state = js_arguments_state(obj);
  if (!state || state->frame_index < 0 ||
      idx >= state->mapped_count || state->deleted[idx]) {
    return;
  }

  state->in_setter = 1;
  arr_set(js, obj, (ant_offset_t)idx, value);
  state->in_setter = 0;
}

static inline void arr_del(ant_t *js, ant_value_t arr, ant_offset_t idx) {
  ant_offset_t semantic_len = get_array_length(js, arr);
  if (idx >= semantic_len) return;
  
  array_mark_may_have_holes(arr);
  ant_offset_t doff = get_dense_buf(arr);
  
  if (doff) {
    ant_offset_t len = dense_iterable_length(js, arr);
    if (idx < len) dense_set(js, doff, idx, T_EMPTY);
  }
  
  char idxstr[16];
  size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);
  
  js_delete_prop(js, arr, idxstr, idxlen);
}

static inline ant_value_t mkstr_with_ascii(ant_t *js, const void *ptr, size_t len, bool known_ascii) {
  ant_flat_string_t *flat = (ant_flat_string_t *)js_type_alloc(
    js, ANT_ALLOC_STRING, sizeof(*flat) + len + 1, 
    _Alignof(ant_flat_string_t)
  );
  
  if (!flat) return js_mkerr(js, "oom");
  flat->len = (ant_offset_t)len;
  
  if (ptr && len > 0) memcpy(flat->bytes, ptr, len);
  flat->bytes[len] = '\0';
  
  uint8_t ascii = known_ascii ? STR_ASCII_YES : ((ptr || len == 0)
    ? str_detect_ascii_bytes(flat->bytes, len)
    : STR_ASCII_UNKNOWN);
  
  if (ptr) str_flat_finish(flat, ascii);
  else str_flat_init_meta(flat, ascii);

  return mkref(kTypeString, flat);
}

static ant_value_t js_empty_string(ant_t *js) {
  if (js->empty_str) return js->empty_str;
  ant_value_t empty = js_mkstr_permanent(js, "", 0);
  if (!is_err(empty)) js->empty_str = empty;
  return empty;
}

ant_value_t js_mkstr(ant_t *js, const void *ptr, size_t len) {
  if (len == 0 && ptr) return js_empty_string(js);
  return mkstr_with_ascii(js, ptr, len, false);
}

ant_value_t js_mkstr_ascii(ant_t *js, const void *ptr, size_t len) {
  return mkstr_with_ascii(js, ptr, len, true);
}

static ant_value_t js_mkstr_int32(ant_t *js, int32_t value) {
  size_t len = ant_int32_chars_len(value);
  ant_value_t str = mkstr_with_ascii(js, NULL, len, true);
  if (!is_err(str)) ant_int32_write(ant_str_flat_ptr(str)->bytes, len, value);
  return str;
}

ant_value_t js_mkstr_byte_range(ant_t *js, const char *parent, size_t start, size_t len) {
  bool known_ascii = str_flat_ascii_state(str_flat_from_bytes(parent)) == STR_ASCII_YES;
  return mkstr_with_ascii(js, parent + start, len, known_ascii);
}

void *js_permanent_alloc(ant_t *js, size_t size, size_t align) {
  if (js->pool.permanent.block_size == 0) js->pool.permanent.block_size = ANT_POOL_STRING_BLOCK_SIZE;
  return pool_alloc_chain(&js->pool.permanent.head, NULL, js->pool.permanent.block_size, size, align);
}

ant_value_t js_mkstr_permanent(ant_t *js, const void *ptr, size_t len) {
  size_t size = sizeof(ant_flat_string_t) + len + 1;
  ant_flat_string_t *flat = js_permanent_alloc(js, size, _Alignof(ant_flat_string_t));
  
  if (!flat) return js_mkerr(js, "oom");
  flat->len = (ant_offset_t)len;
  
  if (ptr && len > 0) memcpy(flat->bytes, ptr, len);
  flat->bytes[len] = '\0';
  
  str_flat_init_meta(flat, (ptr || len == 0)
    ? str_detect_ascii_bytes(flat->bytes, len)
    : STR_ASCII_UNKNOWN
  );
  
  flat->meta |= STR_META_PERMANENT;
  return mkref(kTypeString, flat);
}

static ant_value_t js_mkrope(ant_t *js, ant_value_t left, ant_value_t right, ant_offset_t total_len, uint16_t depth) {
  if (js->rope_gc.young_alloc >= GC_ROPE_NURSERY_THRESHOLD) {
    GC_ROOT_SAVE(root_mark, js);
    GC_ROOT_PIN(js, left);
    GC_ROOT_PIN(js, right);
    gc_pressure(js);
    GC_ROOT_RESTORE(js, root_mark);
  }

  ant_rope_heap_t *rope = js_rope_alloc(js);
  if (!rope) return js_mkerr(js, "oom");
  rope->len = total_len;
  rope->depth = depth;
  rope->flags = ANT_ROPE_FLAG_YOUNG;
  rope->mark_epoch = 0;
  rope->left = left;
  rope->right = right;
  rope->cached = js_mkundef();
  return ant_mkrope_value(rope);
}


static ant_value_t mkobj_with_inobj_limit(ant_t *js, ant_offset_t parent, uint8_t inobj_limit) {
  ant_object_t *obj = obj_alloc(js, kTypeObject, inobj_limit);

  if (!obj) {
    ant_value_t failure = is_err(js->exception_oom)
      ? js->exception_oom : mkval(kTypeError, 0);
    Ant_Exception_Set(js, failure);
    return failure;
  }

  return mkref(kTypeObject, obj);
}

ant_value_t mkobj(ant_t *js, ant_offset_t parent) {
  return mkobj_with_inobj_limit(js, parent, (uint8_t)ANT_INOBJ_MAX_SLOTS);
}

ant_value_t Ant_Exception_CreateRecord(ant_t *js, ant_value_t value, ant_value_t stack) {
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, value);
  GC_ROOT_PIN(js, stack);

  ant_object_t *record = obj_alloc(js, kTypeError, 0);
  if (record) {
    record->u.exception.value = value;
    record->u.exception.stack = stack;
  }

  GC_ROOT_RESTORE(js, mark);
  return record ? mkref(kTypeError, record)
    : is_err(js->exception_oom)
    ? js->exception_oom : mkval(kTypeError, 0);
}

ant_value_t js_mkobj_from_template(ant_t *js, ant_value_t template) {
  if (vtype(template) != kTypeObject) return js_mkerr(js, "invalid object template");
  if (gc_alloc_due(js)) {
    GC_ROOT_SAVE(mark, js);
    GC_ROOT_PIN(js, template);
    gc_alloc_check(js);
    GC_ROOT_RESTORE(js, mark);
  }

  ant_object_t *source = js_obj_ptr(template);
  if (
    source->flags.is_exotic || source->finalizer || source->native_ptr ||
    source->extra_slots || ant_object_promise_state(source)
  ) return js_mkerr(js, "invalid object template state");

  ant_value_t *overflow = NULL;
  size_t overflow_bytes = source->overflow_prop ? (size_t)ant_object_overflow_cap(source) * sizeof(*overflow) : 0;
  
  if (overflow_bytes) {
    overflow = malloc(overflow_bytes);
    if (!overflow) return js_mkerr(js, "oom");
    memcpy(overflow, source->overflow_prop, overflow_bytes);
  }
  
  ant_object_t *target = fixed_arena_alloc_uninit(&js->obj_arena);
  if (!target) { free(overflow); return js_mkerr(js, "oom"); }
  if (overflow_bytes) gc_note_overflow_alloc(js, overflow_bytes);
  
  *target = *source;
  target->overflow_prop = overflow;
  ant_shape_retain(target->shape);
  target->mark_epoch = 0;
  target->ic_identity = 0;
  target->flags.generation = 0;
  target->flags.in_remember_set = 0;
  target->flags.gc_permanent = 0;
  target->next = js->objects;
  js->objects = target;
  
  return mkref(kTypeObject, target);
}

ant_value_t Ant_Object_Create(ant_t *js) {
  ant_value_t obj = mkobj(js, 0);
  js_set_proto_init(obj, js->sym.object_proto);
  return obj;
}

bool js_array_iteration_default(ant_t *js, ant_value_t arr) {
  ant_object_t *ptr = array_obj_ptr(arr);
  
  if (!ptr || vtype(js->sym.iterator_sym) != kTypeSymbol) return false;
  if (!js->array_iteration_protector_invalid && (ptr->flags.arguments_object || ptr->proto == js->sym.array_proto)) return true;
  
  ant_prop_loc_t loc = lkp_sym_proto(js, arr, (ant_offset_t)vdata(js->sym.iterator_sym));
  if (!loc.obj) return false;
  
  const ant_shape_prop_t *meta = prop_shape_meta(loc);
  if (meta && (meta->has_getter || meta->has_setter)) return false;
  if (js_prop_load(loc) != js->sym.array_values_fn) return false;
  if (vtype(js->sym.array_iterator_proto) != kTypeObject) return true;
  
  ant_value_t next;
  return
    js_try_get_own_data_prop(js, js->sym.array_iterator_proto, "next", 4, &next) &&
    next == js->sym.array_iterator_next;
}

ant_value_t js_arr_spread_into(ant_t *js, ant_value_t dst, ant_value_t src) {
  ant_offset_t len = 0;
  ant_value_t len_result = array_like_length_checked(js, src, &len);
  if (is_err(len_result)) return len_result;
  
  ant_offset_t base = get_array_length(js, dst);
  ant_offset_t doff = get_dense_buf(dst);
  if (doff && real_array_obj_ptr(dst) && dense_capacity(doff) < base + len)
    doff = dense_grow(js, dst, base + len);
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t value;
    if (!array_method_dense_element(src, i, &value)) {
      value = array_method_get_index(js, src, i);
      if (is_err(value)) return value;
      len_result = array_like_length_checked(js, src, &len);
      if (is_err(len_result)) return len_result;
    }
    
    if (doff && base + i < dense_capacity(doff) && base + i == get_array_length(js, dst)) {
      dense_set(js, doff, base + i, value);
      array_len_set(js, dst, base + i + 1);
    } else js_arr_push(js, dst, value);
  }
  
  return js_mkundef();
}

ant_offset_t js_arr_len(ant_t *js, ant_value_t arr) {
  if (!array_obj_ptr(arr)) {
    if (is_proxy(arr)) {
      ant_value_t length = js_get(js, arr, "length");
      if (vtype(length) == kTypeNumber) return (ant_offset_t)tod(length);
    }
    return 0;
  }
  return array_like_length(js, arr);
}

ant_value_t js_arr_get(ant_t *js, ant_value_t arr, ant_offset_t idx) {
  if (is_proxy(arr)) {
    char idxstr[32];
    uint_to_str(idxstr, sizeof(idxstr), (uint64_t)idx);
    return js_get(js, arr, idxstr);
  }
  if (vtype(arr) != kTypeArray) return js_mkundef();
  return arr_get(js, arr, idx);
}

static inline bool is_const_prop(ant_prop_loc_t loc) {
  if (!loc.obj || !loc.obj->shape) return false;
  uint8_t attrs = ant_shape_get_attrs(loc.obj->shape, loc.slot);
  return (attrs & ANT_PROP_ATTR_WRITABLE) == 0;
}

static inline const ant_shape_prop_t *prop_shape_meta(ant_prop_loc_t loc) {
  if (!loc.obj || !loc.obj->shape) return NULL;
  return ant_shape_prop_at(loc.obj->shape, loc.slot);
}

static inline bool is_nonconfig_prop(ant_prop_loc_t loc) {
  if (!loc.obj || !loc.obj->shape) return false;
  uint8_t attrs = ant_shape_get_attrs(loc.obj->shape, loc.slot);
  return (attrs & ANT_PROP_ATTR_CONFIGURABLE) == 0;
}

static void js_init_intern_cache(ant_t *js) {
  js->intern.length = intern_string("length", 6);
  js->intern.buffer = intern_string("buffer", 6);
  js->intern.prototype = intern_string("prototype", 9);
  js->intern.exec = intern_string("exec", 4);
  js->intern.replace = intern_string("replace", 7);
  js->intern.constructor = intern_string("constructor", 11);
  js->intern.promise = intern_string("Promise", 7);
  js->intern.resolve = intern_string("resolve", 7);
  js->intern.then = intern_string("then", 4);
  js->intern.name = intern_string("name", 4);
  js->intern.message = intern_string("message", 7);
  js->intern.done = intern_string("done", 4);
  js->intern.value = intern_string("value", 5);
  js->intern.get = intern_string("get", 3);
  js->intern.set = intern_string("set", 3);
  js->intern.enumerable = intern_string("enumerable", 10);
  js->intern.configurable = intern_string("configurable", 12);
  js->intern.writable = intern_string("writable", 8);
  js->intern.arguments = intern_string("arguments", 9);
  js->intern.callee = intern_string("callee", 6);
  js->intern.next = intern_string("next", 4);
  js->intern.define_property = intern_string("defineProperty", 14);
  js->intern.get_own_property_descriptor = intern_string("getOwnPropertyDescriptor", 24);
}

typedef enum {
  MKPROP_USE_DEFAULT_ATTRS = 1 << 0,
  MKPROP_EXPOSE_CFUNC = 1 << 1,
  MKPROP_KEYED_STORE = 1 << 2,
} mkprop_mode_t;

static ant_value_t mkprop_interned_attrs_impl(
  ant_t *js, ant_value_t obj, const char *interned_key, size_t key_len,
  ant_value_t v, uint8_t attrs, mkprop_mode_t mode
) {
  obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);

  if (!ptr || !ptr->shape || !interned_key) return js_mkerr(js, "invalid object");
  if ((mode & MKPROP_USE_DEFAULT_ATTRS) && !attrs) attrs = ANT_PROP_ATTR_DEFAULT;

  if (mode & MKPROP_EXPOSE_CFUNC) {
    v = js_expose_cfunc_for_key(js, v, interned_key, key_len);
    if (is_err(v)) return v;
  }

  uint32_t slot = 0;
  bool added = false;
  int32_t found = ant_shape_lookup_interned(ptr->shape, interned_key);

  if (found >= 0) {
    slot = (uint32_t)found;
    if (!js_obj_ensure_unique_shape(ptr)) return js_mkerr(js, "oom");
    ant_shape_set_attrs_interned(js, ptr->shape, interned_key, attrs);
  } else {
    bool transitioned = (mode & MKPROP_KEYED_STORE)
      ? ant_shape_add_interned_keyed_tr(js, &ptr->shape, interned_key, attrs, &slot)
      : ant_shape_add_interned_tr(js, &ptr->shape, interned_key, attrs, &slot);
    if (!transitioned) return js_mkerr(js, "oom");
    ant_object_invalidate_guarded_absence(js, ptr);
    added = true;
  }

  if (added) ant_property_mutation_invalidate(js, ptr, interned_key);
  if (added && !js_obj_ensure_prop_capacity(js, ptr, ant_shape_count(ptr->shape))) return js_mkerr(js, "oom");
  if (slot >= ptr->prop_count && !js_obj_ensure_prop_capacity(js, ptr, slot + 1)) return js_mkerr(js, "oom");

  ant_object_prop_set_unchecked(ptr, slot, v);
  gc_write_barrier(js, ptr, v);

  if (!added) ant_property_mutation_invalidate(js, ptr, interned_key);
  return v;
}

static ant_value_t mkprop_bytes_attrs_impl(
  ant_t *js, ant_value_t obj, const char *key, size_t key_len,
  ant_value_t v, uint8_t attrs, mkprop_mode_t mode
) {
  const char *interned = intern_string(key, key_len);
  if (!interned) return js_mkerr(js, "oom");
  return mkprop_interned_attrs_impl(js, obj, interned, key_len, v, attrs, mode);
}

static inline ant_value_t mkprop_bytes_exact_attrs(
  ant_t *js, ant_value_t obj, const char *key, size_t key_len,
  ant_value_t v, uint8_t attrs
) {
  return mkprop_bytes_attrs_impl(js, obj, key, key_len, v, attrs, 0);
}

static inline ant_value_t mkprop_bytes(
  ant_t *js, ant_value_t obj, const char *key, size_t key_len,
  ant_value_t v, uint8_t attrs
) {
  return mkprop_bytes_attrs_impl(
    js, obj, key, key_len, v, attrs,
    MKPROP_USE_DEFAULT_ATTRS | MKPROP_EXPOSE_CFUNC
  );
}

static ant_value_t mkprop_symbol_attrs_impl(
  ant_t *js, ant_value_t obj, ant_value_t k, ant_value_t v,
  uint8_t attrs, mkprop_mode_t mode
) {
  obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);

  if (!ptr || !ptr->shape) return js_mkerr(js, "invalid object");
  if ((mode & MKPROP_USE_DEFAULT_ATTRS) && !attrs) attrs = ANT_PROP_ATTR_DEFAULT;

  uint32_t slot = 0;
  bool added = false;
  ant_offset_t sym_off = (ant_offset_t)vdata(k);
  int32_t found = ant_shape_lookup_symbol(ptr->shape, sym_off);
  if (found >= 0) {
    slot = (uint32_t)found;
    if (!js_obj_ensure_unique_shape(ptr)) return js_mkerr(js, "oom");
    ant_shape_set_attrs_symbol(js, ptr->shape, sym_off, attrs);
  } else {
    bool transitioned = (mode & MKPROP_KEYED_STORE)
      ? ant_shape_add_symbol_keyed_tr(js, &ptr->shape, sym_off, attrs, &slot)
      : ant_shape_add_symbol_tr(js, &ptr->shape, sym_off, attrs, &slot);
    if (!transitioned) return js_mkerr(js, "oom");
    ant_object_invalidate_guarded_absence(js, ptr);
    added = true;
  }

  if (added && !js_obj_ensure_prop_capacity(js, ptr, ant_shape_count(ptr->shape))) return js_mkerr(js, "oom");
  if (slot >= ptr->prop_count && !js_obj_ensure_prop_capacity(js, ptr, slot + 1)) return js_mkerr(js, "oom");

  ant_object_prop_set_unchecked(ptr, slot, v);
  gc_write_barrier(js, ptr, v);
  ant_symbol_property_mutation_invalidate(js, ptr, sym_off);
  
  return v;
}

static inline ant_value_t mkprop_impl(
  ant_t *js, ant_value_t obj, ant_value_t k, ant_value_t v,
  uint8_t attrs, bool default_attrs
) {
  if (vtype(k) == kTypeSymbol) return mkprop_symbol_attrs_impl(
    js, obj, k, v, attrs, 
    default_attrs ? MKPROP_USE_DEFAULT_ATTRS : 0
  );

  ant_offset_t key_len = 0;
  ant_offset_t key_off = vstr(js, k, &key_len);
  const char *key = (const char *)(uintptr_t)key_off;
  
  return default_attrs
    ? mkprop_bytes(js, obj, key, (size_t)key_len, v, attrs)
    : mkprop_bytes_exact_attrs(js, obj, key, (size_t)key_len, v, attrs);
}

ant_value_t mkprop(
  ant_t *js, ant_value_t obj, ant_value_t k, ant_value_t v, uint8_t attrs
) {
  return mkprop_impl(js, obj, k, v, attrs, true);
}

ant_value_t mkprop_exact_attrs(
  ant_t *js, ant_value_t obj, ant_value_t k, ant_value_t v, uint8_t attrs
) {
  return mkprop_impl(js, obj, k, v, attrs, false);
}

ant_value_t mkprop_interned(ant_t *js, ant_value_t obj, const char *interned_key, ant_value_t v, uint8_t attrs) {
  return mkprop_interned_attrs_impl(
    js, obj, interned_key, strlen(interned_key), v, attrs,
    MKPROP_USE_DEFAULT_ATTRS | MKPROP_EXPOSE_CFUNC
  );
}

ant_value_t mkprop_interned_exact(ant_t *js, ant_value_t obj, const char *interned_key, ant_value_t v, uint8_t attrs) {
  return mkprop_interned_attrs_impl(
    js, obj, interned_key, 0, v,
    attrs, MKPROP_USE_DEFAULT_ATTRS
  );
}

static ant_value_t mkprop_interned_keyed(ant_t *js, ant_value_t obj, const char *interned_key, ant_value_t v) {
  return mkprop_interned_attrs_impl(
    js, obj, interned_key, 0, v,
    0, MKPROP_USE_DEFAULT_ATTRS | MKPROP_KEYED_STORE
  );
}

ant_value_t js_mkprop_fast(ant_t *js, ant_value_t obj, const char *key, size_t len, ant_value_t v) {
  const char *interned = intern_string(key, len);
  if (!interned) return js_mkerr(js, "oom");
  return mkprop_interned(js, obj, interned, v, 0);
}

ant_value_t mkprop_append_fast(ant_t *js, ant_value_t obj, const char *key, size_t len, ant_value_t v) {
  const char *interned = intern_string(key, len);
  if (!interned) return js_mkerr(js, "oom");

  obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || !ptr->shape) return js_mkerr(js, "invalid object");

  uint32_t slot = 0;
  int32_t found = ant_shape_lookup_interned(ptr->shape, interned);
  
  if (found >= 0) slot = (uint32_t)found;
  else {
    if (!ant_shape_add_interned_tr(js, &ptr->shape, interned, ANT_PROP_ATTR_DEFAULT, &slot)) return js_mkerr(js, "oom");
    ant_object_invalidate_guarded_absence(js, ptr);
  }

  if (
    slot >= ptr->prop_count &&
    !js_obj_ensure_prop_capacity(js, ptr, ant_shape_count(ptr->shape))
  ) return js_mkerr(js, "oom");

  ant_object_prop_set_unchecked(ptr, slot, v);
  gc_write_barrier(js, ptr, v);
  ant_property_mutation_invalidate(js, ptr, interned);
  
  return v;
}

static void set_slot(ant_value_t obj, internal_slot_t slot, ant_value_t val) {
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || slot < 0 || slot > SLOT_MAX) return;
  
  ANT_ASSERT(
    slot != SLOT_PROTO,
    "prototype written without its isolate (use set_slot_wb)"
  );
  
  if (slot == SLOT_DATA) {
    ptr->u.data.value = val;
    return;
  }
  
  if (obj_extra_set(ptr, slot, val)) {
    if (slot == SLOT_ARGUMENTS_OBJECT) ptr->flags.arguments_object = 0;
    if (slot == SLOT_REGEXP_FLAGS_STRING) ptr->flags.regexp_brand = vtype(val) == kTypeString;
  }
}

// TODO: dry
static void set_slot_wb(ant_t *js, ant_value_t obj, internal_slot_t slot, ant_value_t val) {
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || slot < 0 || slot > SLOT_MAX) return;
  
  if (slot == SLOT_PROTO) {
    ptr->proto = val;
    gc_write_barrier(js, ptr, val);
    ant_ic_epoch_bump(js);
    return;
  }
  
  if (slot == SLOT_DATA) {
    ptr->u.data.value = val;
    gc_write_barrier(js, ptr, val);
    return;
  }
  
  if (obj_extra_set(ptr, slot, val)) {
    if (slot == SLOT_ARGUMENTS_OBJECT) ptr->flags.arguments_object = 0;
    if (slot == SLOT_REGEXP_FLAGS_STRING) ptr->flags.regexp_brand = vtype(val) == kTypeString;
  }
  
  gc_write_barrier(js, ptr, val);
}

void js_set_slot(ant_value_t obj, internal_slot_t slot, ant_value_t value) {
  set_slot(js_as_obj(obj), slot, value);
}

void js_set_slot_wb(ant_t *js, ant_value_t obj, internal_slot_t slot, ant_value_t value) {
  set_slot_wb(js, js_as_obj(obj), slot, value);
}

bool js_reserve_slots(ant_value_t obj, uint8_t capacity) {
  return obj_extra_reserve(js_obj_ptr(js_as_obj(obj)), capacity);
}

static ant_value_t get_slot(ant_value_t obj, internal_slot_t slot) {
  ant_object_t *ptr = js_obj_ptr(obj);
  
  if (!ptr || slot < 0 || slot > SLOT_MAX) return js_mkundef();
  if (slot == SLOT_PROTO) return ptr->proto;
  if (slot == SLOT_DATA) return ptr->u.data.value;
  if (slot == SLOT_ARGUMENTS_OBJECT && ptr->flags.arguments_object) return js_true;
  
  return obj_extra_get(ptr, slot);
}

void js_set_func_source(ant_t *js, ant_value_t fn, const char *code, size_t len, js_func_source_t source) {
  ant_value_t text;
  
  if (source == JS_FUNC_SOURCE_UNIT) {
    text = js_mkstr(js, code, len);
    if (is_err(text)) return;
  } else {
    if (source == JS_FUNC_SOURCE_COPY || !ant_cage_contains(code)) code = code_arena_alloc(js, code, len);
    if (!code || !ant_cage_contains(code)) return;
    text = mkref(kTypeSourceCode, code);
  }
  
  js_set_slot_wb(js, fn, SLOT_CODE, text);
  js_set_slot(fn, SLOT_CODE_LEN, tov((double)len));
}

static inline ant_object_t *js_to_primitive_cache_object(ant_value_t value) {
  if (vtype(value) == kTypeFunction) return js_obj_ptr(js_func_obj(value));
  if (vtype(value) == kTypeObject || vtype(value) == kTypeArray) return js_obj_ptr(js_as_obj(value));
  return NULL;
}

static inline bool js_to_primitive_cache_hit(ant_t *js, ant_value_t value) {
  ant_object_t *obj = js_to_primitive_cache_object(value);
  const ant_to_primitive_cache_t *cache = &js->runtime_cache.to_primitive_absent;
  
  if (!obj || cache->ic_epoch != js->ic.epoch) return false;
  if (cache->object == obj) return true;
  
  return !obj->flags.is_exotic &&
    cache->shape == obj->shape &&
    cache->proto == obj->proto;
}

double js_to_number(ant_t *js, ant_value_t arg) {
  if (vtype(arg) == kTypeNull) return 0.0;
  
  if (vtype(arg) == kTypeSymbol) {
    js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert a Symbol value to a number");
    return JS_NAN;
  }
  
  if (vtype(arg) == kTypeUndefined) return JS_NAN;
  if (vtype(arg) == kTypeNumber) return tod(arg);
  if (vtype(arg) == kTypeBool) return vdata(arg) ? 1.0 : 0.0;
  if (vtype(arg) == kTypeBigInt) return bigint_to_double(js, arg);

  if (vtype(arg) == kTypeString) {
    ant_flat_string_t *flat = ant_str_flat_ptr(arg);
    const char *base = NULL;
    const char *end = NULL;

    if (flat) {
      base = flat->bytes;
      end = base + flat->len;
    } else {
      ant_offset_t len = 0;
      ant_offset_t off = vstr(js, arg, &len);
      base = (const char *)(uintptr_t)off;
      end = base + len;
    }
    
    double val = JS_NAN; if (
      ant_number_parse(base, (size_t)(end - base), 
      ANT_NUMBER_PARSE_JS_NUMBER, &val, NULL)
    ) return val;
    
    return JS_NAN;
  }

  if (is_object_type(arg) || vtype(arg) == kTypeBuiltin) {
    ant_value_t primitive = js_to_primitive_cache_hit(js, arg)
      ? try_cached_ordinary_to_primitive_number(js, arg)
      : js_to_primitive(js, arg, 2);
    
    if (is_err(primitive)) return JS_NAN;
    if (vtype(primitive) == kTypeNumber) return tod(primitive);
    if (vtype(primitive) == kTypeBool) return vdata(primitive) ? 1.0 : 0.0;
    if (vtype(primitive) == kTypeNull) return 0.0;
    if (vtype(primitive) == kTypeUndefined) return JS_NAN;
    if (vtype(primitive) == kTypeBigInt) return bigint_to_double(js, primitive);

    return js_to_number(js, primitive);
  }
  
  return JS_NAN;
}

ant_value_t js_to_numeric(ant_t *js, ant_value_t arg) {
  uint8_t t = vtype(arg);
  if (t == kTypeNumber || t == kTypeBigInt) return arg;
  
  if (is_object_type(arg) || t == kTypeBuiltin) {
    arg = js_to_primitive_cache_hit(js, arg)
      ? try_cached_ordinary_to_primitive_number(js, arg)
      : js_to_primitive(js, arg, 2);
    if (is_err(arg)) return arg;
    t = vtype(arg);
    if (t == kTypeNumber || t == kTypeBigInt) return arg;
  }
  
  if (t == kTypeSymbol)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert a Symbol value to a number");
  return tov(js_to_number(js, arg));
}

static ant_value_t setup_func_prototype(ant_t *js, ant_value_t func) {
  return setup_func_prototype_property(js, func, true);
}

static inline bool same_object_identity(ant_value_t a, ant_value_t b) {
  if (!is_object_type(a) || !is_object_type(b)) return false;
  return vdata(js_as_obj(a)) == vdata(js_as_obj(b));
}

static inline ant_value_t proto_next_obj_or_null(ant_t *js, ant_value_t cur) {
  if (!is_object_type(cur)) return js_mknull();
  ant_value_t next = get_proto(js, cur);
  return is_object_type(next) ? next : js_mknull();
}

typedef struct {
  int depth;
  bool overflow;
  ant_value_t fast;
} proto_overflow_guard_t;

static inline void proto_overflow_guard_init(proto_overflow_guard_t *g) {
  g->depth = 0;
  g->overflow = false;
  g->fast = js_mknull();
}

static inline bool proto_overflow_guard_hit_cycle(ant_t *js, proto_overflow_guard_t *g, ant_value_t cur) {
  if (!g->overflow) {
    if (++g->depth < MAX_PROTO_CHAIN_DEPTH) return false;
    g->overflow = true;
    g->fast = cur;
  }

  g->fast = proto_next_obj_or_null(js, g->fast);
  g->fast = proto_next_obj_or_null(js, g->fast);
  return same_object_identity(cur, g->fast);
}

typedef enum: uint8_t {
  PROTO_WALK_F_OBJECT_ONLY = 1u << 0,
  PROTO_WALK_F_LOOKUP      = 1u << 1,
} proto_walk_flags_t;

static inline bool proto_walk_next(ant_t *js, ant_value_t *cur, uint8_t *t, proto_walk_flags_t flags) {
  uint8_t ct = *t;

  if (flags & PROTO_WALK_F_OBJECT_ONLY) {
    if (!is_object_type(*cur)) return false;
    ant_value_t next = get_proto(js, *cur);
    uint8_t nt = vtype(next);
    if (nt == kTypeNull || nt == kTypeUndefined || !is_object_type(next)) return false;
    *cur = next; *t = nt;
    return true;
  }

  if (T_FLAG_FIND(ct) & T_OBJECT_MASK) {
    ant_value_t as_obj = js_as_obj(*cur);
    ant_value_t proto = get_slot(as_obj, SLOT_PROTO);
    
    uint8_t pt = vtype(proto);
    if (pt == kTypeObject || pt == kTypeArray || pt == kTypeFunction) {
      *cur = proto;
      *t = pt;
      return true;
    }
    
    if (T_FLAG_FIND(ct) & T_NEEDS_PROTO_FALLBACK) {
      ant_value_t fallback = js_primitive_prototype(js, ct);
      uint8_t ft = vtype(fallback);
      if (ft == kTypeNull || ft == kTypeUndefined) return false;
      *cur = fallback;
      *t = ft;
      return true;
    }
    
    return false;
  }

  if (is_boxable_primitive_type(ct)) {
    ant_value_t proto = js_primitive_prototype(js, ct);
    uint8_t pt = vtype(proto);
    if (pt == kTypeNull || pt == kTypeUndefined) return false;
    *cur = proto; *t = pt;
    return true;
  }

  return false;
}

typedef struct {
  int depth;
  bool overflow;
  bool fast_active;
  ant_value_t fast_cur;
  uint8_t fast_t;
} proto_walk_overflow_guard_t;

static inline void proto_walk_overflow_guard_init(proto_walk_overflow_guard_t *g) {
  g->depth = 0;
  g->overflow = false;
  g->fast_active = false;
  g->fast_cur = js_mknull();
  g->fast_t = kTypeNull;
}

static inline bool proto_walk_overflow_guard_hit_cycle(
  ant_t *js,
  proto_walk_overflow_guard_t *g,
  ant_value_t cur,
  uint8_t cur_t,
  proto_walk_flags_t flags
) {
  if (!g->overflow) {
    if (++g->depth < MAX_PROTO_CHAIN_DEPTH) return false;
    g->overflow = true;
    g->fast_active = true;
    g->fast_cur = cur;
    g->fast_t = cur_t;
  }

  if (g->fast_active && !proto_walk_next(js, &g->fast_cur, &g->fast_t, flags))
    g->fast_active = false;
  if (g->fast_active && !proto_walk_next(js, &g->fast_cur, &g->fast_t, flags))
    g->fast_active = false;

  return g->fast_active && same_object_identity(cur, g->fast_cur);
}

ant_value_t js_instance_proto_from_new_target(ant_t *js, ant_value_t fallback_proto, ant_value_t new_target) {
  ant_value_t instance_proto = js_mkundef();

  if (vtype(new_target) == kTypeFunction || vtype(new_target) == kTypeBuiltin) {
    ant_value_t nt_obj = js_as_obj(new_target);
    ant_value_t nt_proto = lkp_interned_val(js, nt_obj, js->intern.prototype);
    if (is_object_type(nt_proto)) instance_proto = nt_proto;
  }

  if (!is_object_type(instance_proto) && is_object_type(fallback_proto)) instance_proto = fallback_proto;
  return instance_proto;
}

ant_value_t js_construct_native(ant_t *js, ant_cfunc_t ctor, ant_value_t *args, int nargs) {
  return sv_invoke_native(js, ctor, args, nargs, js_true);
}

bool proto_chain_contains(ant_t *js, ant_value_t obj, ant_value_t proto_target) {
  if (!is_object_type(obj) || !is_object_type(proto_target)) return false;
  ant_value_t cur = obj;
  uint8_t t = vtype(cur);
  proto_overflow_guard_t guard;
  proto_overflow_guard_init(&guard);

  while (true) {
    if (!proto_walk_next(js, &cur, &t, PROTO_WALK_F_OBJECT_ONLY)) break;
    if (same_object_identity(cur, proto_target)) return true;
    if (proto_overflow_guard_hit_cycle(js, &guard, cur)) break;
  }
  return false;
}

static inline bool is_wrapper_ctor_target(ant_t *js, ant_value_t this_val, ant_value_t new_target) {
  if (vtype(new_target) == kTypeUndefined) return false;
  if (vtype(this_val) != kTypeObject) return false;
  if (vtype(get_slot(this_val, SLOT_PRIMITIVE)) != kTypeUndefined) return false;
  return true;
}

ant_value_t get_ctor_species_value(ant_t *js, ant_value_t ctor) {
  if (!is_object_type(ctor) && vtype(ctor) != kTypeBuiltin) return js_mkundef();
  return js_get_sym(js, ctor, js->sym.species_sym);
}

bool same_ctor_identity(ant_t *js, ant_value_t a, ant_value_t b) {
  if (vtype(a) == vtype(b) && vdata(a) == vdata(b)) return true;
  
  if (vtype(a) == kTypeFunction && vtype(b) == kTypeBuiltin) {
    ant_value_t c = get_slot(a, SLOT_CFUNC);
    return vtype(c) == kTypeBuiltin && vdata(c) == vdata(b);
  }
  
  if (vtype(a) == kTypeBuiltin && vtype(b) == kTypeFunction) {
    ant_value_t c = get_slot(b, SLOT_CFUNC);
    return vtype(c) == kTypeBuiltin && vdata(c) == vdata(a);
  }
  
  if (vtype(a) == kTypeFunction && vtype(b) == kTypeFunction) {
    ant_value_t ca = get_slot(a, SLOT_CFUNC);
    ant_value_t cb = get_slot(b, SLOT_CFUNC);
    if (vtype(ca) == kTypeBuiltin && vtype(cb) == kTypeBuiltin && vdata(ca) == vdata(cb)) return true;
  }
  
  return false;
}

static ant_value_t array_constructor_from_receiver(ant_t *js, ant_value_t receiver) {
  if (!is_object_type(receiver)) return js_mkundef();
  
  ant_object_t *plain = real_array_obj_ptr(receiver);
  if (plain && plain->proto == js->sym.array_proto && !js->array_species_protector_invalid)
    return js->sym.array_ctor;
  
  ant_value_t species_source = receiver;
  if (is_proxy(species_source)) {
    species_source = proxy_read_target(js, species_source);
  }
  
  bool receiver_is_array = real_array_obj_ptr(species_source) != NULL;
  if (!receiver_is_array) {
    ant_value_t array_proto = js->sym.array_proto;
    if (is_object_type(array_proto) && is_object_type(species_source))
      receiver_is_array = proto_chain_contains(js, species_source, array_proto);
  }
  if (!receiver_is_array) return js_mkundef();

  ant_value_t ctor = js_getprop_fallback(js, receiver, "constructor");
  if (is_err(ctor)) return ctor;
  if (ant_realm_foreign_array_ctor(js, ctor)) return js_mkundef();

  if (is_object_type(ctor) || vtype(ctor) == kTypeBuiltin) {
    ctor = get_ctor_species_value(js, ctor);
    if (is_err(ctor)) return ctor;
    if (vtype(ctor) == kTypeNull) return js_mkundef();
  }
  
  if (vtype(ctor) == kTypeUndefined) return ctor;
  if (!js_is_constructor(ctor))
    return js_mkerr_typed(js, JS_ERR_TYPE, "object.constructor[Symbol.species] is not a constructor");
  
  return ctor;
}

static constexpr ant_offset_t ARRAY_RESULT_PREALLOC_MAX = 1024;

static ant_value_t array_alloc_intrinsic(ant_t *js, ant_value_t source, ant_offset_t length_hint) {
  if (length_hint == 0) return mkarr(js);
  
  ant_object_t *src = array_obj_ptr(source);
  bool exact = length_hint <= ARRAY_RESULT_PREALLOC_MAX ||
    (src && src->flags.fast_array && length_hint <= src->u.array.cap);
  
  ant_value_t arr = exact && length_hint <= UINT32_MAX
    ? alloc_array_with_proto_capacity(js, js->sym.array_proto, (uint32_t)length_hint, 0, false)
    : mkarr(js);
  
  if (is_err(arr)) return arr;
  array_len_set(js, arr, length_hint);
  array_mark_may_have_holes(arr);
  
  return arr;
}

static ant_value_t array_construct_from_ctor(ant_t *js, ant_value_t ctor, ant_offset_t length_hint) {
  ant_value_t seed = Ant_Object_CreateBare(js);
  if (is_err(seed)) return seed;

  ant_value_t proto = js_get(js, ctor, "prototype");
  if (is_err(proto)) return proto;
  if (is_object_type(proto)) js_set_proto_init(seed, proto);

  ant_value_t ctor_args[1] = { tov((double)length_hint) };
  ant_value_t constructed = sv_vm_call(js->vm, js, ctor, seed, ctor_args, 1, NULL, ctor);
  if (is_err(constructed)) return constructed;

  ant_value_t result = is_object_type(constructed) ? constructed : seed;
  set_slot(js_as_obj(result), SLOT_CTOR, ctor);
  return result;
}

static ant_value_t array_alloc_from_ctor_with_length(ant_t *js, ant_value_t ctor, ant_offset_t length_hint) {
  if (vtype(ctor) != kTypeFunction && vtype(ctor) != kTypeBuiltin) return mkarr(js);
  if (same_ctor_identity(js, ctor, js->sym.array_ctor)) return array_alloc_intrinsic(js, js_mkundef(), length_hint);
  return array_construct_from_ctor(js, ctor, length_hint);
}

static inline ant_value_t array_species_create(ant_t *js, ant_value_t receiver, ant_offset_t length, bool *intrinsic) {
  ant_value_t ctor = array_constructor_from_receiver(js, receiver);
  if (is_err(ctor)) return ctor;
  
  *intrinsic = vtype(ctor) == kTypeUndefined || same_ctor_identity(js, ctor, js->sym.array_ctor);
  if (*intrinsic) return array_alloc_intrinsic(js, receiver, length);
  return array_alloc_from_ctor_with_length(js, ctor, length);
}

static __attribute__((noinline)) bool array_length_readonly_slow(ant_t *js, ant_value_t arr) {
  descriptor_entry_t *desc = lookup_descriptor(js, js_as_obj(arr), "length", 6);
  return desc && !desc->writable;
}

static inline bool array_length_readonly(ant_t *js, ant_value_t arr) {
  ant_object_t *ptr = real_array_obj_ptr(arr);
  if (__builtin_expect(!ptr || !(ptr->flags.raw & (ANT_OBJECT_FLAG_FROZEN | ANT_OBJECT_FLAG_EXOTIC)), 1)) return false;
  return array_length_readonly_slow(js, arr);
}

static ant_value_t array_length_readonly_error(ant_t *js) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot assign to read only property 'length' of object '[object Array]'");
}

static inline ant_value_t set_declined(bool *declined, ant_value_t value) {
  if (declined) *declined = true;
  return value;
}

static __attribute__((noinline, cold)) ant_value_t array_length_write_ignored(ant_t *js, ant_value_t value, bool *declined) {
  return sv_is_strict_context(js) ? array_length_readonly_error(js) : set_declined(declined, value);
}

static ant_value_t array_grow_readonly_result(ant_t *js, uint64_t idx, ant_value_t value, bool *declined) {
  if (!sv_is_strict_context(js)) return set_declined(declined, value);
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot add property %llu, object is not extensible", (unsigned long long)idx);
}

static ant_value_t validate_array_length(ant_t *js, ant_value_t v) {
  if (vtype(v) != kTypeNumber)
    return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid array length");
  double d = tod(v);
  if (d < 0 || d != (uint32_t)d || d >= 4294967296.0)
    return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid array length");
  return js_mkundef();
}

static inline ant_value_t check_object_extensibility(ant_t *js, ant_value_t obj) {
  obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return js_mkundef();

  if (ptr->flags.frozen) {
    return sv_is_strict_context(js)
      ? js_mkerr(js, "cannot add property to frozen object")
      : js_false;
  }

  if (ptr->flags.sealed) {
    return sv_is_strict_context(js)
      ? js_mkerr(js, "cannot add property to sealed object")
      : js_false;
  }

  if (!ptr->flags.extensible) {
    return sv_is_strict_context(js)
      ? js_mkerr(js, "cannot add property to non-extensible object")
      : js_false;
  }

  return js_mkundef();
}

static inline void array_len_set(ant_t *js, ant_value_t obj, ant_offset_t new_len) {
  ant_object_t *arr_ptr = array_obj_ptr(obj);
  if (arr_ptr) {
    if (new_len > (ant_offset_t)UINT32_MAX) new_len = (ant_offset_t)UINT32_MAX;
    arr_ptr->u.array.len = (uint32_t)new_len;
    arr_ptr->flags.dense_length_fits = new_len <= arr_ptr->u.array.cap;
    return;
  }

  ant_value_t new_len_val = tov((double)new_len);
  ant_prop_loc_t len_off = lkp_interned(obj, js->intern.length);
  
  if (len_off.obj) js_prop_store(js, len_off, new_len_val);
  else js_mkprop_fast(js, obj, "length", 6, new_len_val);
}

static constexpr ant_value_t JS_SETPROP_ARRAY_NOT_HANDLED = ANT_SENTINEL_TAG | 0xA551ULL;

typedef enum {
  PROTO_INDEX_ABSENT,
  PROTO_INDEX_WRITABLE,
  PROTO_INDEX_INTERCEPTS,
} proto_index_property_t;

static proto_index_property_t proto_index_property(ant_t *js, ant_value_t proto, uint64_t idx) {
  if (is_proxy(proto)) return PROTO_INDEX_INTERCEPTS;
  ant_value_t proto_obj = js_as_obj(proto);
  ant_object_t *ptr = js_obj_ptr(proto_obj);

  char key[24];
  size_t klen = uint_to_str(key, sizeof(key), idx);
  const char *interned = intern_find(key, klen);
  int32_t slot = interned && ptr->shape ? ant_shape_lookup_interned(ptr->shape, interned) : -1;
  if (slot >= 0) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, (uint32_t)slot);
    return !prop || prop->has_getter || prop->has_setter || !(prop->attrs & ANT_PROP_ATTR_WRITABLE)
      ? PROTO_INDEX_INTERCEPTS : PROTO_INDEX_WRITABLE;
  }

  descriptor_entry_t *desc = ptr->flags.is_exotic ? lookup_descriptor(js, proto_obj, key, klen) : NULL;
  if (!desc) return PROTO_INDEX_ABSENT;
  return desc->has_getter || desc->has_setter || !desc->writable
    ? PROTO_INDEX_INTERCEPTS : PROTO_INDEX_WRITABLE;
}

static __attribute__((noinline)) bool array_store_reaches_proto_slow(ant_t *js, ant_value_t arr, uint64_t idx) {
  ant_value_t cur = get_proto(js, arr);

  for (int depth = 0; is_object_type(cur); depth++) {
    ant_object_t *ptr = js_obj_ptr(js_as_obj(cur));
    if (!ptr || depth >= MAX_PROTO_CHAIN_DEPTH) return true;
    if (ptr->flags.is_exotic || ant_shape_may_have_index_keys(ptr->shape)) {
      proto_index_property_t found = proto_index_property(js, cur, idx);
      if (found != PROTO_INDEX_ABSENT) return found == PROTO_INDEX_INTERCEPTS;
    }
    ant_value_t next = ptr->proto;
    cur = is_object_type(next) || vtype(next) == kTypeNull ? next : get_proto(js, cur);
  }

  return false;
}

static __attribute__((noinline)) bool array_chain_revalidate(ant_t *js) {
  ant_object_t *array_proto = (ant_object_t *)vptr(js->sym.array_proto);
  ant_object_t *object_proto = (ant_object_t *)vptr(js->sym.object_proto);
  bool plain =
    array_proto->proto == js->sym.object_proto && object_proto->proto == mkval(kTypeNull, 0) &&
    !((array_proto->flags.raw | object_proto->flags.raw) & ANT_OBJECT_FLAG_EXOTIC) &&
    !ant_shape_may_have_index_keys(array_proto->shape) &&
    !ant_shape_may_have_index_keys(object_proto->shape);
  
  if (plain) {
    ant_object_guard_absence(array_proto);
    ant_object_guard_absence(object_proto);
    js->array_chain_plain_epoch = js->ic.epoch;
  }
  
  return plain;
}

static inline bool array_proto_chain_plain(ant_t *js, ant_object_t *arr) {
  if (arr->proto != js->sym.array_proto) return false;
  if (((ant_object_t *)vptr(js->sym.array_proto))->u.array.len != 0) return false;
  return js->array_chain_plain_epoch == js->ic.epoch || array_chain_revalidate(js);
}

static inline bool arguments_proto_chain_plain(ant_t *js, ant_object_t *args) {
  if (args->proto != js->sym.object_proto) return false;
  ant_object_t *object_proto = (ant_object_t *)vptr(js->sym.object_proto);
  return
    object_proto->proto == mkval(kTypeNull, 0) && !object_proto->flags.is_exotic &&
    !ant_shape_may_have_index_keys(object_proto->shape);
}

static inline bool array_elements_proto_plain(ant_t *js, ant_object_t *ptr) {
  return ptr->flags.arguments_object ? arguments_proto_chain_plain(js, ptr) : array_proto_chain_plain(js, ptr);
}

static inline ant_object_t *array_dense_source(ant_t *js, ant_value_t arr) {
  if (is_proxy(arr)) return NULL;
  ant_object_t *ptr = array_obj_ptr(arr);
  return array_dense_storage(ptr) && array_elements_proto_plain(js, ptr) ? ptr : NULL;
}

static inline bool array_search_eq(ant_t *js, ant_value_t elem, ant_value_t search) {
  if (vtype(search) == kTypeNumber) return vtype(elem) == kTypeNumber && tod(elem) == tod(search);
  if (elem == search) return true;
  return vtype(elem) == vtype(search) && strict_eq_values(js, elem, search);
}

static inline bool array_store_reaches_proto(ant_t *js, ant_value_t arr, ant_object_t *ptr, uint64_t idx) {
  return !array_proto_chain_plain(js, ptr) && array_store_reaches_proto_slow(js, arr, idx);
}

static ant_value_t js_setprop_array_fast(
  ant_t *js, ant_value_t obj, ant_value_t k,
  ant_value_t v, ant_offset_t klen, const char *key, bool *declined
) {
  unsigned long idx;
  if (!parse_array_index(key, klen, (ant_offset_t)-1, &idx))
    return JS_SETPROP_ARRAY_NOT_HANDLED;
  
  ant_offset_t cur_len = get_array_length(js, obj);
  ant_offset_t doff = get_dense_buf(obj);
  ant_object_t *ptr = array_obj_ptr(obj);

  if (idx >= cur_len && array_length_readonly(js, obj))
    return ptr && array_store_reaches_proto(js, obj, ptr, idx)
      ? JS_SETPROP_ARRAY_NOT_HANDLED : array_grow_readonly_result(js, idx, v, declined);
  
  if (doff) {
    ant_offset_t dense_len = dense_iterable_length(js, obj);
    if (idx < dense_len) {
      if (ptr && ptr->flags.frozen) 
        return sv_is_strict_context(js) ? js_mkerr(js, "assignment to read-only array element") : set_declined(declined, v);
      if (is_empty_slot(dense_get(doff, (ant_offset_t)idx))) {
        if (array_store_reaches_proto(js, obj, ptr, idx)) return JS_SETPROP_ARRAY_NOT_HANDLED;
        ant_value_t extensibility_error = check_object_extensibility(js, obj);
        if (is_err(extensibility_error)) return extensibility_error;
        if (extensibility_error == js_false) return set_declined(declined, v);
      }
      dense_set(js, doff, (ant_offset_t)idx, v);
      return v;
    }

    ant_offset_t density_limit = dense_len > 0 ? dense_len * 4 : 64;
    if (idx >= density_limit) goto sparse;
    if (array_store_reaches_proto(js, obj, ptr, idx)) return JS_SETPROP_ARRAY_NOT_HANDLED;
    
    ant_value_t extensibility_error = check_object_extensibility(js, obj);
    if (is_err(extensibility_error)) return extensibility_error;
    if (extensibility_error == js_false) return set_declined(declined, v);
    
    arr_set(js, obj, (ant_offset_t)idx, v);
    return v;
  }
  
  sparse:;
  if (idx < cur_len || (ptr && array_store_reaches_proto(js, obj, ptr, idx))) return JS_SETPROP_ARRAY_NOT_HANDLED;
  
  ant_value_t extensibility_error = check_object_extensibility(js, obj);
  if (is_err(extensibility_error)) return extensibility_error;
  if (extensibility_error == js_false) return set_declined(declined, v);
  
  ant_value_t result = mkprop(js, obj, k, v, 0);
  if (is_err(result)) return result;
  array_define_or_set_index(js, obj, key, (size_t)klen);
  
  return v;
}

static inline void prop_meta_defaults(prop_meta_t *out) {
  *out = (prop_meta_t){
    .has_getter = false,
    .has_setter = false,
    .writable = true,
    .enumerable = true,
    .configurable = true,
    .getter = js_mkundef(),
    .setter = js_mkundef(),
  };
}

static inline void prop_meta_from_shape(prop_meta_t *out, const ant_shape_prop_t *prop) {
  out->has_getter = prop->has_getter != 0;
  out->has_setter = prop->has_setter != 0;
  out->writable = (prop->attrs & ANT_PROP_ATTR_WRITABLE) != 0;
  out->enumerable = (prop->attrs & ANT_PROP_ATTR_ENUMERABLE) != 0;
  out->configurable = (prop->attrs & ANT_PROP_ATTR_CONFIGURABLE) != 0;
  out->getter = prop->getter;
  out->setter = prop->setter;
}

static inline void prop_meta_from_desc(prop_meta_t *out, const descriptor_entry_t *desc) {
  out->has_getter = desc->has_getter;
  out->has_setter = desc->has_setter;
  out->writable = desc->writable;
  out->enumerable = desc->enumerable;
  out->configurable = desc->configurable;
  out->getter = desc->getter;
  out->setter = desc->setter;
}

bool lookup_prop_meta(
  ant_t *js,
  ant_value_t cur_obj,
  prop_meta_key_t key_kind,
  const char *key,
  size_t klen,
  ant_offset_t sym_off,
  prop_meta_t *out
) {
  if (!js || !out || !is_object_type(cur_obj)) return false;
  if (key_kind == PROP_META_STRING && !key) return false;

  prop_meta_defaults(out);

  ant_object_t *cur_ptr = js_obj_ptr(cur_obj);
  if (!cur_ptr) return false;
  if (key_kind == PROP_META_STRING && key && is_length_key(key, klen) &&
      ant_object_is_array(cur_ptr)) {
    out->has_getter = false;
    out->has_setter = false;
    out->writable = true;
    out->enumerable = false;
    out->configurable = false;
    out->getter = js_mkundef();
    out->setter = js_mkundef();
    return true;
  }

  if (cur_ptr->shape) {
    int32_t slot = -1;
    if (key_kind == PROP_META_SYMBOL) {
      slot = ant_shape_lookup_symbol(cur_ptr->shape, sym_off);
    } else {
      const char *interned_key = intern_find(key, klen);
      if (interned_key) slot = ant_shape_lookup_interned(cur_ptr->shape, interned_key);
    }

    if (slot >= 0) {
      const ant_shape_prop_t *prop = ant_shape_prop_at(cur_ptr->shape, (uint32_t)slot);
      if (!prop) return false;
      prop_meta_from_shape(out, prop);
      return true;
    }
  }

  if (!cur_ptr->flags.is_exotic) return false;

  descriptor_entry_t *desc = key_kind == PROP_META_SYMBOL
    ? lookup_sym_descriptor(js, cur_obj, sym_off)
    : lookup_descriptor(js, cur_obj, key, klen);

  if (!desc) return false;
  prop_meta_from_desc(out, desc);

  return true;
}

bool js_try_get_own_data_prop(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t *out) {
  if (out) *out = js_mkundef();
  if (!key || !out) return false;

  uint8_t t = vtype(obj);
  if (t == kTypeFunction) obj = js_func_obj(obj);
  else if (t != kTypeObject && t != kTypeArray) return false;

  ant_value_t as_obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(as_obj);
  
  if (ptr && ptr->flags.is_exotic && is_proxy(as_obj)) return false;

  if (ptr && ptr->shape && !(real_array_obj_ptr(as_obj) && is_length_key(key, key_len))) {
    const char *interned = intern_find(key, key_len);
    int32_t slot = interned ? ant_shape_lookup_interned(ptr->shape, interned) : -1;
    
    if (slot >= 0) {
      const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, (uint32_t)slot);
      if (!prop || prop->has_getter || prop->has_setter) return false;
      
      *out = ant_object_prop_get_unchecked(ptr, (uint32_t)slot);
      return true;
    }
  }

  prop_meta_t meta;
  bool has_meta = lookup_string_prop_meta(js, as_obj, key, key_len, &meta);
  if (has_meta && (meta.has_getter || meta.has_setter)) return false;

  ant_prop_loc_t off = lkp(js, as_obj, key, (ant_offset_t)key_len);
  if (off.obj) {
    *out = js_prop_load(off);
    return true;
  }

  if (real_array_obj_ptr(as_obj) && is_length_key(key, (ant_offset_t)key_len)) {
    *out = tov((double)get_array_length(js, as_obj));
    return true;
  }

  return false;
}

static ant_value_t call_proto_accessor(
  ant_t *js, ant_value_t prim, ant_value_t accessor, bool has_accessor,
  ant_value_t *arg, int arg_count, bool is_setter
) {
  if (!has_accessor || (vtype(accessor) != kTypeFunction && vtype(accessor) != kTypeBuiltin))
    return js_mkundef();
  
  if (!is_setter && js_is_symbol_description_getter(accessor)) {
    ant_value_t symbol = prim;
    if (is_object_type(symbol)) symbol = js_get_slot(symbol, SLOT_PRIMITIVE);
    if (vtype(symbol) == kTypeSymbol) return js_symbol_description_value(js, symbol);
  }

  ant_value_t result = sv_vm_call(js->vm, js, accessor, prim, arg, arg_count, NULL, js_mkundef());
  if (is_setter) return is_err(result) ? result : (arg ? *arg : js_mkundef());

  return result;
}

static ant_value_t js_prop_load_from(ant_t *js, ant_value_t receiver, ant_prop_loc_t off) {
  const ant_shape_prop_t *meta = prop_shape_meta(off);
  if (!meta || !(meta->has_getter || meta->has_setter)) return js_prop_load(off);
  return call_proto_accessor(js, receiver, meta->getter, meta->has_getter, NULL, 0, false);
}

ant_value_t js_call_getter(ant_t *js, ant_value_t receiver, ant_value_t getter) {
  if (vtype(getter) != kTypeBuiltin) return call_proto_accessor(js, receiver, getter, true, NULL, 0, false);
  return sv_call_builtin_this(js, getter, receiver, NULL, 0, js_mkundef());
}

// TODO: decompose into smaller helpers
static ant_value_t setprop_impl(ant_t *js, ant_value_t obj, ant_value_t k, ant_value_t v, bool keyed, bool *declined);

[[gnu::noinline]]
static bool setprop_sym_reaches_proxy(
  ant_t *js, ant_value_t obj, ant_value_t proxy, ant_value_t sym, ant_value_t v,
  bool keyed, bool *declined, ant_value_t *done, ant_value_t *next
) {
  ant_value_t target = js_mkundef();
  ant_value_t result = proxy_set_sym(js, proxy, sym, v, obj, &target);
  
  if (is_err(result)) *done = result;
  else if (result == js_false) *done = proxy_set_rejected_sym(js, sym, v, declined);
  else if (result == js_true) *done = v;
  else if (same_object_identity(proxy, obj)) *done = setprop_impl(js, target, sym, v, keyed, declined);
  
  else {
    *next = target;
    return false;
  }
  
  return true;
}

static inline bool string_wrapper_owns_key(ant_t *js, ant_value_t obj, const char *key, size_t key_len) {
  ant_value_t str = js_string_wrapper_value(obj);
  if (vtype(str) != kTypeString) return false;
  unsigned long idx = 0;
  return is_length_key(key, key_len) || parse_array_index(key, key_len, str_utf16_len(js, str), &idx);
}

static inline bool string_wrapper_owns_index(ant_t *js, ant_value_t obj, uint32_t idx) {
  ant_value_t str = js_string_wrapper_value(obj);
  return vtype(str) == kTypeString && (ant_offset_t)idx < str_utf16_len(js, str);
}

static ant_value_t string_wrapper_write_refused(ant_t *js, const char *key, size_t key_len, ant_value_t value, bool *declined) {
  if (!sv_is_strict_context(js)) return set_declined(declined, value);
  
  return js_mkerr_typed(js, JS_ERR_TYPE,
    "Cannot assign to read only property '%.*s' of object '[object String]'", 
    (int)key_len, key
  );
}

static inline bool exotic_has_getter(const ant_object_t *ptr) {
  return ptr->flags.is_exotic && ptr->exotic_ops && ptr->exotic_ops->getter;
}

static inline ant_value_t exotic_own_get(ant_t *js, ant_value_t obj, const ant_object_t *ptr, const char *key, size_t len) {
  return exotic_has_getter(ptr) ? ptr->exotic_ops->getter(js, obj, key, len) : js_mkundef();
}

static bool array_own_value(ant_t *js, ant_value_t obj, const char *key, size_t len, ant_value_t *out) {
  if (!array_obj_ptr(obj)) return false;
  if (real_array_obj_ptr(obj) && is_length_key(key, len)) {
    *out = tov((double)get_array_length(js, obj));
    return true;
  }

  unsigned long idx;
  ant_offset_t doff = get_dense_buf(obj);
  if (!doff || !parse_array_index(key, len, dense_iterable_length(js, obj), &idx)) return false;
  ant_value_t value = dense_get(doff, (ant_offset_t)idx);
  if (is_empty_slot(value)) return false;
  *out = value;
  return true;
}

static ant_value_t setprop_impl(ant_t *js, ant_value_t obj, ant_value_t k, ant_value_t v, bool keyed, bool *declined) {
  uint8_t ot = vtype(obj);

  if (ot == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_promote(js, obj);
    return setprop_impl(js, promoted, k, v, keyed, declined);
  }

  if (ot == kTypeString || ot == kTypeNumber || ot == kTypeBool) {
    ant_offset_t klen; ant_offset_t koff = vstr(js, k, &klen);
    const char *key = (char *)(uintptr_t)(koff);
    
    ant_value_t proto = js_primitive_prototype(js, ot);
    if (is_object_type(proto)) {
    ant_value_t setter = js_mkundef();
    bool has_setter = false;
    
    lkp_with_setter(js, proto, key, klen, &setter, &has_setter);
    if (has_setter && (vtype(setter) == kTypeFunction || vtype(setter) == kTypeBuiltin)) {
      call_proto_accessor(js, obj, setter, true, &v, 1, true);
      return v;
    }}
    
    if (sv_is_strict_context(js)) return js_mkerr_typed(js, JS_ERR_TYPE,
      "Cannot create property '%.*s' on %s",
      (int)klen, key, typestr(ot)
    );
    
    return v;
  }

  if (vtype(obj) == kTypeFunction) obj = js_func_obj(obj);
  if (vtype(k) == kTypeSymbol) {
    ant_offset_t sym_off = (ant_offset_t)vdata(k);
    ant_value_t cur = obj;
    proto_overflow_guard_t guard;
    proto_overflow_guard_init(&guard);

    bool receiver_missed = false;
    for (bool on_receiver = true; is_object_type(cur); on_receiver = false) {
      ant_value_t cur_obj = js_as_obj(cur);
      ant_object_t *cur_ptr = (ant_object_t *)vptr(cur_obj);
      prop_meta_t meta;
      bool found;
      if (!cur_ptr->flags.is_exotic) {
        int32_t slot = cur_ptr->shape ? ant_shape_lookup_symbol(cur_ptr->shape, sym_off) : -1;
        const ant_shape_prop_t *prop = slot >= 0 ? ant_shape_prop_at(cur_ptr->shape, (uint32_t)slot) : NULL;
        found = prop != NULL;
        if (found) prop_meta_from_shape(&meta, prop);
        else if (on_receiver) receiver_missed = true;
      } else if (is_proxy(cur_obj)) {
        ant_value_t done;
        if (setprop_sym_reaches_proxy(js, obj, cur_obj, k, v, keyed, declined, &done, &cur)) return done;
        continue;
      } else found = lookup_symbol_prop_meta(js, cur_obj, sym_off, &meta);
      if (found) {
        if (meta.has_setter) {
          ant_value_t setter = meta.setter;
          if (vtype(setter) == kTypeFunction || vtype(setter) == kTypeBuiltin) {
            ant_value_t result = sv_vm_call(js->vm, js, setter, obj, &v, 1, NULL, js_mkundef());
            if (is_err(result)) return result;
            return v;
          }
        }
        if (meta.has_getter && !meta.has_setter) {
          if (sv_is_strict_context(js)) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot set property which has only a getter");
          return set_declined(declined, v);
        }
        if (!meta.has_getter && !meta.has_setter && !meta.writable) {
          if (sv_is_strict_context(js)) return js_mkerr(js, "assignment to read-only property");
          return set_declined(declined, v);
        }
        break;
      }
      
      ant_value_t proto = is_object_type(cur_ptr->proto) ? cur_ptr->proto : get_proto(js, cur_obj);
      if (!is_object_type(proto)) break;
      cur = proto;
      if (proto_overflow_guard_hit_cycle(js, &guard, cur)) break;
    }

    ant_prop_loc_t existing = receiver_missed ? ANT_PROP_LOC_NONE : lkp_sym(obj, sym_off);
    
    if (existing.obj) {
      if (is_const_prop(existing)) return js_mkerr(js, "assignment to constant");
      js_prop_store(js, existing, v);
      ant_symbol_property_mutation_invalidate(js, existing.obj, sym_off);
      return v;
    }
    
    {
      ant_value_t extensibility_error = check_object_extensibility(js, obj);
      if (is_err(extensibility_error)) return extensibility_error;
      if (extensibility_error == js_false) return set_declined(declined, v);
    }
    
    return mkprop_symbol_attrs_impl(
      js, obj, k, v, 0, 
      MKPROP_USE_DEFAULT_ATTRS | (keyed ? MKPROP_KEYED_STORE : 0)
    );
  }

  ant_offset_t klen; ant_offset_t koff = vstr(js, k, &klen);
  const char *key = (char *)(uintptr_t)(koff);
  regexp_note_property_write(js, key, (size_t)klen);
  if (string_wrapper_owns_key(js, obj, key, (size_t)klen))
    return string_wrapper_write_refused(js, key, (size_t)klen, v, declined);

  if (array_obj_ptr(obj) && klen > 0 && key[0] >= '0' && key[0] <= '9') {
    ant_arguments_state_t *args_state = js_arguments_state(obj);
    if (args_state && !args_state->in_setter && js_arguments_setter(js, obj, key, (size_t)klen, v)) return v;
  }
  
  if (array_obj_ptr(obj) && !is_proxy(obj) && klen > 0 && key[0] >= '0' && key[0] <= '9') {
    ant_value_t result = js_setprop_array_fast(js, obj, k, v, klen, key, declined);
    if (result != JS_SETPROP_ARRAY_NOT_HANDLED) return result;
  }

  ant_object_t *len_obj = real_array_obj_ptr(obj);
  if (len_obj && is_length_key(key, klen)) {
    ant_value_t err = validate_array_length(js, v);
    if (is_err(err)) return err;
    
    if (
      __builtin_expect((len_obj->flags.raw & (ANT_OBJECT_FLAG_FROZEN | ANT_OBJECT_FLAG_EXOTIC)) != 0, 0) && 
      array_length_readonly_slow(js, obj)
    ) return array_length_write_ignored(js, v, declined);
    
    ant_offset_t doff = get_dense_buf(obj);
    ant_offset_t cur_len = get_array_length(js, obj);
    ant_offset_t new_len_val = (ant_offset_t) tod(v);
    
    if (doff) {
      ant_offset_t cap = dense_capacity(doff);
      ant_offset_t clear_to = (cur_len < cap) ? cur_len : cap;
      if (new_len_val < clear_to) 
        for (ant_offset_t i = new_len_val; i < clear_to; i++) dense_set(js, doff, i, T_EMPTY);
    }
    
    if (new_len_val > cur_len) array_mark_may_have_holes(obj);
    array_len_set(js, obj, new_len_val);
    
    return v;
  }
  
  if (is_proxy(obj)) {
    ant_value_t result = proxy_set(js, obj, key, klen, v);
    if (is_err(result)) return result;
    return result == js_false ? proxy_set_rejected(js, key, klen, v, declined) : v;
  }
  
  if (try_dynamic_setter(js, obj, key, klen, v))
    return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : v;
  ant_prop_loc_t existing = lkp(js, obj, key, klen);
  
  {
    const char *interned_key = intern_string(key, (size_t)klen);
    bool found_desc = false;
    bool desc_on_receiver = false;
    bool desc_has_getter = false;
    bool desc_has_setter = false;
    bool desc_writable = true;
    ant_value_t desc_setter = js_mkundef();

    ant_value_t cur = obj;
    proto_overflow_guard_t guard;
    proto_overflow_guard_init(&guard);
    bool on_receiver = true;
    while (is_object_type(cur)) {
      ant_value_t cur_obj = js_as_obj(cur);
      if (!on_receiver && is_proxy(cur_obj)) {
        ant_value_t result = proxy_set_with_receiver(js, cur_obj, key, klen, v, obj);
        if (is_err(result)) return result;
        return result == js_false ? proxy_set_rejected(js, key, klen, v, declined) : v;
      }
      ant_object_t *cur_ptr = js_obj_ptr(cur_obj);
      if (!cur_ptr) break;

      bool found_here = false;
      if (cur_ptr->shape && interned_key) {
        int32_t slot = ant_shape_lookup_interned(cur_ptr->shape, interned_key);
        if (slot >= 0) {
          const ant_shape_prop_t *prop = ant_shape_prop_at(cur_ptr->shape, (uint32_t)slot);
          if (prop) {
            found_here = true;
            desc_has_getter = prop->has_getter != 0;
            desc_has_setter = prop->has_setter != 0;
            desc_writable = (prop->attrs & ANT_PROP_ATTR_WRITABLE) != 0;
            desc_setter = prop->setter;
          }
        }
      }

      ant_value_t exotic = !found_here && !on_receiver ? exotic_own_get(js, cur_obj, cur_ptr, key, klen) : js_mkundef();
      if (exotic == T_EMPTY) return v;
      if (vtype(exotic) != kTypeUndefined) {
        found_here = true;
        desc_has_getter = desc_has_setter = false;
        desc_writable = true;
      }

      if (!found_here && cur_ptr->flags.is_exotic) {
        descriptor_entry_t *desc = lookup_descriptor(js, cur_obj, key, klen);
        if (desc) {
          found_here = true;
          desc_has_getter = desc->has_getter;
          desc_has_setter = desc->has_setter;
          desc_writable = desc->writable;
          desc_setter = desc->setter;
        }
      }

      if (found_here) {
        found_desc = true;
        desc_on_receiver = on_receiver;
        break;
      }

      ant_value_t proto = get_proto(js, cur_obj);
      if (!is_object_type(proto)) break;
      cur = proto;
      on_receiver = false;
      if (proto_overflow_guard_hit_cycle(js, &guard, cur)) break;
    }

    if (!found_desc) goto no_descriptor;
    if (!desc_on_receiver && !desc_has_setter && !desc_has_getter && desc_writable) goto no_descriptor;
    
    if (desc_has_setter) {
      ant_value_t setter = desc_setter;
      uint8_t setter_type = vtype(setter);
      if (setter_type == kTypeFunction || setter_type == kTypeBuiltin) {
        ant_value_t result = sv_vm_call(js->vm, js, setter, obj, &v, 1, NULL, js_mkundef());
        if (is_err(result)) return result;
        return v;
      }
    }
    
    if (desc_has_getter && !desc_has_setter) {
      if (sv_is_strict_context(js)) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot set property which has only a getter");
      return set_declined(declined, v);
    }
    
    if (!desc_writable) {
      if (sv_is_strict_context(js)) return js_mkerr(js, "assignment to read-only property");
      return set_declined(declined, v);
    }
    
    if (!existing.obj) goto no_descriptor;
  }
  
no_descriptor:
  if (!existing.obj) goto create_new;
  if (is_const_prop(existing)) return js_mkerr(js, "assignment to constant");

  js_prop_store(js, existing, v);
  array_define_or_set_index(js, obj, key, (size_t)klen);
  return v;

create_new:
  {
    ant_value_t extensibility_error = check_object_extensibility(js, obj);
    if (is_err(extensibility_error)) return extensibility_error;
    if (extensibility_error == js_false) return set_declined(declined, v);
  }

  const char *interned_key = intern_string(key, (size_t)klen);
  if (!interned_key) return js_mkerr(js, "oom");
  ant_value_t result = keyed
    ? mkprop_interned_keyed(js, obj, interned_key, v)
    : mkprop_interned_exact(js, obj, interned_key, v, 0);
  if (is_err(result)) return result;
  array_define_or_set_index(js, obj, key, (size_t)klen);
  
  return v;
}

ant_value_t js_setprop(ant_t *js, ant_value_t obj, ant_value_t k, ant_value_t v) {
  return setprop_impl(js, obj, k, v, false, NULL);
}

ant_value_t js_setprop_keyed(ant_t *js, ant_value_t obj, ant_value_t k, ant_value_t v) {
  return setprop_impl(js, obj, k, v, true, NULL);
}

static inline const char *index_key_find(ant_t *js, uint32_t idx, char *buf, size_t *len) {
  if (idx < INTERN_INDEX_CACHE_SIZE && js->intern.idx[idx]) return js->intern.idx[idx];
  *len = uint_to_str(buf, 16, idx);
  const char *interned = intern_find(buf, *len);
  if (interned && idx < INTERN_INDEX_CACHE_SIZE) js->intern.idx[idx] = interned;
  return interned;
}

static inline const char *index_key_intern(ant_t *js, uint32_t idx, char *buf, size_t *len) {
  if (!*len) *len = uint_to_str(buf, 16, idx);
  const char *interned = intern_string(buf, *len);
  if (interned && idx < INTERN_INDEX_CACHE_SIZE) js->intern.idx[idx] = interned;
  return interned;
}

static inline const char *index_key_get_or_intern(ant_t *js, uint32_t idx) {
  if (idx < INTERN_INDEX_CACHE_SIZE && js->intern.idx[idx]) return js->intern.idx[idx];
  char buf[16];
  const char *interned = intern_string(buf, uint_to_str(buf, sizeof(buf), idx));
  if (interned && idx < INTERN_INDEX_CACHE_SIZE) js->intern.idx[idx] = interned;
  return interned;
}

static inline bool index_keys_absent_from_protos(const ant_object_t *ptr) {
  for (ant_value_t proto = ptr->proto; is_object_type(proto);) {
    const ant_object_t *p = js_obj_ptr(proto);
    if (!p || p->flags.is_exotic || p->type_tag == kTypeArray || ant_shape_may_have_index_keys(p->shape)) return false;
    proto = p->proto;
  }
  return true;
}

bool js_get_plain_index(ant_t *js, ant_value_t obj, uint32_t idx, ant_value_t *out) {
  ant_object_t *ptr = vtype(obj) == kTypeObject ? js_obj_ptr(obj) : NULL;
  if (!ptr || ptr->flags.is_exotic || !ptr->shape) return false;
  
  ant_value_t str = ptr->extra_slots 
    ? js_string_wrapper_value(obj) 
    : js_mkundef();
  
  if (vtype(str) == kTypeString) {
    ant_offset_t byte_len = 0;
    const char *data = (const char *)(uintptr_t)vstr(js, str, &byte_len);
    uint32_t code_unit = utf16_code_unit_at(data, (size_t)byte_len, idx);
    
    if (code_unit != 0xFFFFFFFF) {
      *out = js_string_from_utf16_code_unit(js, code_unit);
      return true;
    }
  }
  
  char key[16];
  size_t key_len = 0;
  
  const char *interned = index_key_find(js, idx, key, &key_len);
  int32_t slot = interned ? ant_shape_lookup_interned(ptr->shape, interned) : -1;
  
  if (slot >= 0) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, (uint32_t)slot);
    if (!prop || prop->has_getter || prop->has_setter) return false;
    
    *out = ant_object_prop_get_unchecked(ptr, (uint32_t)slot);
    return true;
  }
  
  if (!index_keys_absent_from_protos(ptr)) return false;
  *out = js_mkundef();
  
  return true;
}

bool js_string_past_end_is_undefined(ant_t *js, ant_value_t str) {
  const ant_object_t *ptr = js_obj_ptr(vtype(str) == kTypeString ? js->sym.string_proto : str);
  return ptr && !ptr->flags.is_exotic && !ant_shape_may_have_index_keys(ptr->shape) && index_keys_absent_from_protos(ptr);
}

ant_value_t js_get_index_fast(ant_t *js, ant_value_t obj, ant_value_t key) {
  if (vtype(key) != kTypeNumber) return T_EMPTY;
  
  double d = tod(key);
  if (!(d >= 0 && d < (double)UINT32_MAX)) return T_EMPTY;
  
  uint32_t idx = (uint32_t)d;
  if (d != (double)idx) return T_EMPTY;

  if (vtype(obj) == kTypeString) {
    ant_offset_t byte_len = 0;
    const char *data = (const char *)(uintptr_t)vstr(js, obj, &byte_len);
    uint32_t code_unit = utf16_code_unit_at(data, (size_t)byte_len, idx);
    if (code_unit != 0xFFFFFFFF) return js_string_from_utf16_code_unit(js, code_unit);
    return js_string_past_end_is_undefined(js, obj) ? js_mkundef() : T_EMPTY;
  }

  ant_object_t *ptr = vtype(obj) == kTypeObject ? (ant_object_t *)vptr(obj) : NULL;
  if (!ptr || ptr->flags.is_exotic || !ptr->shape) return T_EMPTY;
  
  const char *interned = idx < INTERN_INDEX_CACHE_SIZE ? js->intern.idx[idx] : NULL;
  int32_t slot = interned ? ant_shape_lookup_interned(ptr->shape, interned) : -1;
  
  if (slot >= 0) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, (uint32_t)slot);
    if (!prop || prop->has_getter || prop->has_setter) return T_EMPTY;
    return ant_object_prop_get_unchecked(ptr, (uint32_t)slot);
  }

  ant_value_t out;
  return js_get_plain_index(js, obj, idx, &out) ? out : T_EMPTY;
}

static ant_value_t setprop_index_impl(ant_t *js, ant_value_t obj, uint32_t idx, ant_value_t value, bool *declined) {
  ant_arguments_state_t *args_state = js_arguments_state(obj);
  if (args_state && !args_state->in_setter) {
    char key[16];
    size_t key_len = uint_to_str(key, sizeof(key), idx);
    if (js_arguments_setter(js, obj, key, key_len, value)) return value;
  }

  ant_object_t *ptr = array_obj_ptr(obj);
  if (ptr && (ant_offset_t)idx >= get_array_length(js, obj) && array_length_readonly(js, obj)) {
    if (!array_store_reaches_proto(js, obj, ptr, idx)) return array_grow_readonly_result(js, idx, value, declined);
    goto keyed;
  }
  if (ptr && !is_proxy(obj)) {
    ant_offset_t dense = get_dense_buf(obj);
    if (dense) {
      ant_offset_t dense_len = dense_iterable_length(js, obj);
      bool missing = (ant_offset_t)idx >= dense_len || is_empty_slot(dense_get(dense, (ant_offset_t)idx));
      if (missing && array_store_reaches_proto(js, obj, ptr, idx)) goto keyed;
      
      if ((ant_offset_t)idx < dense_len) {
        if (ptr->flags.frozen) 
          return sv_is_strict_context(js) ? js_mkerr(js, "assignment to read-only array element") : set_declined(declined, value);
        if (missing) {
          ant_value_t extensibility_error = check_object_extensibility(js, obj);
          if (is_err(extensibility_error)) return extensibility_error;
          if (extensibility_error == js_false) return set_declined(declined, value);
        }
        dense_set(js, dense, (ant_offset_t)idx, value);
        return value;
      }

      uint64_t density_limit = dense_len > 0 ? (uint64_t)dense_len * 4u : 64u;
      if ((uint64_t)idx < density_limit) {
        ant_value_t extensibility_error = check_object_extensibility(js, obj);
        if (is_err(extensibility_error)) return extensibility_error;
        if (extensibility_error == js_false) return set_declined(declined, value);
        arr_set(js, obj, (ant_offset_t)idx, value);
        return value;
      }
    }
  }

keyed:;
  char key[16];
  size_t key_len = 0;
  
  ant_object_t *plain = vtype(obj) == kTypeObject ? js_obj_ptr(obj) : NULL;
  if (plain && !plain->flags.is_exotic && plain->shape) {
    const char *interned = index_key_find(js, idx, key, &key_len);
    int32_t slot = interned ? ant_shape_lookup_interned(plain->shape, interned) : -1;
    
    if (slot >= 0) {
      const ant_shape_prop_t *prop = ant_shape_prop_at(plain->shape, (uint32_t)slot);
      if (prop && !prop->has_getter && !prop->has_setter && (prop->attrs & ANT_PROP_ATTR_WRITABLE)) {
        ant_object_prop_set_unchecked(plain, (uint32_t)slot, value);
        gc_write_barrier(js, plain, value);
        return value;
      }
    } else if (
      plain->flags.extensible && index_keys_absent_from_protos(plain) &&
      (!plain->extra_slots || !string_wrapper_owns_index(js, obj, idx))
    ) {
      if (!interned) interned = index_key_intern(js, idx, key, &key_len);
      if (!interned) return js_mkerr(js, "oom");
      
      ant_value_t added = mkprop_interned_keyed(js, obj, interned, value);
      return is_err(added) ? added : value;
    }
  }
  
  if (!key_len) key_len = uint_to_str(key, sizeof(key), idx);
  ant_value_t property = js_mkstr(js, key, key_len);
  if (is_err(property)) return property;
  
  return setprop_impl(js, obj, property, value, true, declined);
}

ant_value_t js_setprop_index(ant_t *js, ant_value_t obj, uint32_t idx, ant_value_t value) {
  return setprop_index_impl(js, obj, idx, value, NULL);
}

static ant_value_t js_setprop_throw(ant_t *js, ant_value_t obj, ant_value_t key, bool is_index, uint32_t idx, ant_value_t value) {
  sv_vm_t *vm = js->vm;
  int saved_fp = vm->jit_mode_fp;
  bool saved_strict = vm->jit_mode_strict;
  
  vm->jit_mode_fp = vm->fp;
  vm->jit_mode_strict = true;
  ant_value_t stored = is_index ? js_setprop_index(js, obj, idx, value) : js_setprop(js, obj, key, value);
  vm->jit_mode_fp = saved_fp;
  vm->jit_mode_strict = saved_strict;
  
  return stored;
}

static inline ant_value_t js_setprop_index_throw(ant_t *js, ant_value_t obj, uint32_t idx, ant_value_t value) {
  return js_setprop_throw(js, obj, js_mkundef(), true, idx, value);
}

ant_value_t setprop_cstr(ant_t *js, ant_value_t obj, const char *key, size_t len, ant_value_t v) {
  obj = js_as_obj(obj);
  const char *interned = intern_string(key, len);
  if (!interned) return js_mkerr(js, "oom");
  return mkprop_interned(js, obj, interned, v, 0);
}

ant_value_t js_define_own_prop(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t v) {
  obj = js_as_obj(obj);
  if (is_proxy(obj)) {
    ant_value_t result = proxy_set(js, obj, key, klen, v);
    if (is_err(result)) return result;
    return v;
  }

  if (try_dynamic_setter(js, obj, key, klen, v))
    return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : v;
  ant_prop_loc_t existing = lkp(js, obj, key, klen);

  {
    bool has_desc = false;
    bool desc_writable = true;
    bool desc_has_setter = false;
    ant_value_t desc_setter = js_mkundef();
    ant_value_t as_obj = js_as_obj(obj);
    ant_object_t *ptr = js_obj_ptr(as_obj);
    const char *interned_key = intern_string(key, klen);

    if (ptr && ptr->shape && interned_key) {
      int32_t slot = ant_shape_lookup_interned(ptr->shape, interned_key);
      if (slot >= 0) {
        const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, (uint32_t)slot);
        if (prop) {
          has_desc = true;
          desc_writable = (prop->attrs & ANT_PROP_ATTR_WRITABLE) != 0;
          desc_has_setter = prop->has_setter != 0;
          desc_setter = prop->setter;
        }
      }
    }

    if (!has_desc && ptr && ptr->flags.is_exotic) {
      descriptor_entry_t *desc = lookup_descriptor(js, as_obj, key, klen);
      if (desc) {
        has_desc = true;
        desc_writable = desc->writable;
        desc_has_setter = desc->has_setter;
        desc_setter = desc->setter;
      }
    }

    if (has_desc) {
      if (!desc_writable && !desc_has_setter) {
        if (sv_is_strict_context(js)) return js_mkerr(js, "assignment to read-only property");
        return v;
      }
      if (desc_has_setter) {
        ant_value_t setter = desc_setter;
        uint8_t setter_type = vtype(setter);
        if (setter_type == kTypeFunction || setter_type == kTypeBuiltin) {
          ant_value_t result = sv_vm_call(js->vm, js, setter, obj, &v, 1, NULL, js_mkundef());
          if (is_err(result)) return result;
          return v;
        }
      }
    }
  }

  if (existing.obj) {
    if (is_const_prop(existing)) return js_mkerr(js, "assignment to constant");
    js_prop_store(js, existing, v);
    array_define_or_set_index(js, obj, key, klen);
    return v;
  }

  {
    ant_value_t extensibility_error = check_object_extensibility(js, obj);
    if (is_err(extensibility_error)) return extensibility_error;
    if (extensibility_error == js_false) return v;
  }

  const char *interned_key = intern_string(key, klen);
  if (!interned_key) return js_mkerr(js, "oom");
  ant_value_t created = mkprop_interned_exact(js, obj, interned_key, v, 0);
  
  if (!is_err(created)) array_define_or_set_index(js, obj, key, klen);
  return is_err(created) ? created : v;
}

ant_value_t setprop_interned(ant_t *js, ant_value_t obj, const char *key, size_t len, ant_value_t v) {
  ant_value_t k = js_mkstr(js, key, len);
  if (is_err(k)) return k;
  return js_setprop(js, obj, k, v);
}

ant_value_t js_setprop_readonly_nonconfigurable(ant_t *js, ant_value_t obj, const char *key, size_t keylen, ant_value_t v) {
  ant_value_t k = js_mkstr(js, key, keylen);
  if (is_err(k)) return k;
  return mkprop_exact_attrs(js, obj, k, v, 0);
}

enum {
  SYM_FLAG_GLOBAL     = 1u,
  SYM_FLAG_WELL_KNOWN = 2u,
  SYM_FLAG_HAS_DESC   = 4u,
  SYM_FLAG_DESC_ASCII = 8u,
};

typedef struct sym_registry_entry {
  const char *key;
  ant_value_t sym;
  UT_hash_handle hh;
} sym_registry_entry_t;

ant_value_t js_mksym(ant_t *js, const char *desc) {
  bool has_desc = desc != NULL;
  
  size_t desc_len = has_desc ? strlen(desc) : 0;
  size_t total = offsetof(ant_symbol_heap_t, desc) +
    (has_desc ? desc_len + 1 : 0);
  
  ant_symbol_heap_t *sym_ptr = (ant_symbol_heap_t *)js_type_alloc(
    js, ANT_ALLOC_SYMBOL, total, 
    _Alignof(ant_symbol_heap_t)
  ); if (!sym_ptr) return js_mkerr(js, "oom");

  sym_ptr->gc_epoch = 0;
  sym_ptr->key = NULL;
  sym_ptr->flags = has_desc ? SYM_FLAG_HAS_DESC : 0;

  if (has_desc && str_detect_ascii_bytes(desc, desc_len) == STR_ASCII_YES)
    sym_ptr->flags |= SYM_FLAG_DESC_ASCII;
  sym_ptr->desc_len = (uint32_t)desc_len;
  
  if (has_desc) {
    memcpy(sym_ptr->desc, desc, desc_len);
    sym_ptr->desc[desc_len] = '\0';
  }

  return mkref(kTypeSymbol, sym_ptr);
}

ant_value_t js_mksym_well_known(ant_t *js, const char *desc) {
  ant_value_t sym = js_mksym(js, desc);
  if (is_err(sym)) return sym;
  ant_symbol_heap_t *ptr = (ant_symbol_heap_t *)vptr(sym);
  if (ptr) ptr->flags |= SYM_FLAG_WELL_KNOWN;
  return sym;
}

static inline ant_symbol_heap_t *sym_ptr(ant_value_t v) {
  return (ant_symbol_heap_t *)vptr(v);
}

static inline uint32_t sym_get_flags(ant_value_t v) {
  ant_symbol_heap_t *ptr = sym_ptr(v);
  return ptr ? ptr->flags : 0;
}

static inline uintptr_t sym_get_key_ptr(ant_value_t v) {
  ant_symbol_heap_t *ptr = sym_ptr(v);
  return ptr ? (uintptr_t)ptr->key : 0;
}

const inline char *js_sym_desc(ant_value_t sym) {
  ant_symbol_heap_t *ptr = sym_ptr(sym);
  if (!ptr || !(ptr->flags & SYM_FLAG_HAS_DESC)) return NULL;
  return ptr->desc;
}

ant_value_t js_symbol_description_value(ant_t *js, ant_value_t symbol) {
  ant_symbol_heap_t *ptr = sym_ptr(symbol);
  if (!ptr || !(ptr->flags & SYM_FLAG_HAS_DESC)) return js_mkundef();
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, symbol);
  ant_value_t result = mkstr_with_ascii(
    js, ptr->desc, ptr->desc_len, (ptr->flags & SYM_FLAG_DESC_ASCII) != 0);
  GC_ROOT_RESTORE(js, mark);
  return result;
}

ant_value_t js_mksym_for(ant_t *js, const char *key) {
  const char *interned = intern_string(key, strlen(key));

  sym_registry_entry_t *reg = js->sym.registry;
  sym_registry_entry_t *found = NULL;
  HASH_FIND_PTR(reg, &interned, found);
  if (found) return found->sym;

  ant_value_t sym = js_mksym(js, key);
  if (is_err(sym)) return sym;

  ant_symbol_heap_t *ptr = sym_ptr(sym);
  if (!ptr) return js_mkerr(js, "oom");
  ptr->flags |= SYM_FLAG_GLOBAL;
  ptr->key = interned;

  sym_registry_entry_t *entry = calloc(1, sizeof(sym_registry_entry_t));
  if (entry) {
    entry->key = interned;
    entry->sym = sym;
    HASH_ADD_PTR(reg, key, entry);
    js->sym.registry = reg;
  }

  return sym;
}

const char *js_sym_key(ant_value_t sym) {
  if (vtype(sym) != kTypeSymbol) return NULL;
  uint32_t flags = sym_get_flags(sym);
  if (!(flags & SYM_FLAG_GLOBAL) || (flags & SYM_FLAG_WELL_KNOWN)) return NULL;
  return (const char *)sym_get_key_ptr(sym);
}

bool js_symbol_gc_mark(ant_value_t sym, uint64_t epoch) {
  if (vtype(sym) != kTypeSymbol) return false;
  ant_symbol_heap_t *ptr = sym_ptr(sym);
  if (!ptr || ptr->gc_epoch == epoch) return false;
  ptr->gc_epoch = epoch;
  return true;
}

bool js_symbol_gc_is_marked(ant_value_t sym, uint64_t epoch) {
  if (vtype(sym) != kTypeSymbol) return false;
  ant_symbol_heap_t *ptr = sym_ptr(sym);
  return ptr && ptr->gc_epoch == epoch;
}

bool js_symbol_gc_is_permanent(ant_value_t sym) {
  if (vtype(sym) != kTypeSymbol) return false;
  uint32_t flags = sym_get_flags(sym);
  return (flags & (SYM_FLAG_GLOBAL | SYM_FLAG_WELL_KNOWN)) != 0;
}

ant_prop_loc_t lkp_interned(ant_value_t obj, const char *search_intern) {
  obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!search_intern || !ptr || !ptr->shape) return ANT_PROP_LOC_NONE;

  int32_t shape_slot = ant_shape_lookup_interned(ptr->shape, search_intern);
  if (shape_slot < 0) return ANT_PROP_LOC_NONE;
  return (ant_prop_loc_t){ptr, (uint32_t)shape_slot};
}

inline ant_prop_loc_t lkp(ant_t *js, ant_value_t obj, const char *buf, size_t len) {
  const char *search_intern = intern_find(buf, len);
  if (!search_intern) return ANT_PROP_LOC_NONE;
  return lkp_interned(obj, search_intern);
}

inline ant_value_t lkp_interned_val(ant_t *js, ant_value_t obj, const char *search_intern) {
  obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!search_intern || !ptr || !ptr->shape) return js_mkundef();
  int32_t slot = ant_shape_lookup_interned(ptr->shape, search_intern);
  if (slot < 0) return js_mkundef();
  return ant_object_prop_get_unchecked(ptr, (uint32_t)slot);
}

static inline ant_value_t lkp_val(ant_t *js, ant_value_t obj, const char *buf, size_t len) {
  const char *interned = intern_find(buf, len);
  if (!interned) return js_mkundef();
  return lkp_interned_val(js, obj, interned);
}

ant_prop_loc_t lkp_sym(ant_value_t obj, ant_offset_t sym_off) {
  obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || !ptr->shape) return ANT_PROP_LOC_NONE;
  int32_t slot = ant_shape_lookup_symbol(ptr->shape, sym_off);
  if (slot < 0) return ANT_PROP_LOC_NONE;
  return (ant_prop_loc_t){ptr, (uint32_t)slot};
}

ant_prop_loc_t lkp_sym_proto(ant_t *js, ant_value_t obj, ant_offset_t sym_off) {
  ant_value_t cur = obj;
  proto_overflow_guard_t guard;
  proto_overflow_guard_init(&guard);
  while (is_object_type(cur)) {
    obj = cur;
    ant_prop_loc_t loc = lkp_sym(obj, sym_off);
    if (loc.obj) return loc;
    ant_value_t proto = get_proto(js, js_as_obj(cur));
    if (!is_object_type(proto)) break;
    cur = proto;
    if (proto_overflow_guard_hit_cycle(js, &guard, cur)) break;
  }
  return ANT_PROP_LOC_NONE;
}

static inline ant_value_t lkp_sym_proto_val(ant_t *js, ant_value_t obj, ant_offset_t sym_off) {
  ant_value_t cur = obj;
  proto_overflow_guard_t guard;
  proto_overflow_guard_init(&guard);
  
  while (is_object_type(cur)) {
    ant_value_t as_obj = js_as_obj(cur);
    ant_object_t *ptr = js_obj_ptr(as_obj);
    
    if (ptr && ptr->shape) {
      int32_t slot = ant_shape_lookup_symbol(ptr->shape, sym_off);
      if (slot >= 0) return ant_object_prop_get_unchecked(ptr, (uint32_t)slot);
    }
    
    ant_value_t proto = get_proto(js, as_obj);
    if (!is_object_type(proto)) break;
    
    cur = proto;
    if (proto_overflow_guard_hit_cycle(js, &guard, cur)) break;
  }
  
  return js_mkundef();
}

static uintptr_t lkp_with_getter(ant_t *js, ant_value_t obj, const char *buf, size_t len, ant_value_t *getter_out, bool *has_getter_out) {
  *has_getter_out = false;
  *getter_out = js_mkundef();
  const char *search_intern = intern_find(buf, len);
  
  ant_value_t current = obj;
  proto_overflow_guard_t guard;
  proto_overflow_guard_init(&guard);
  
  while (is_object_type(current)) {
    current = js_as_obj(current);
    uintptr_t current_id = (uintptr_t)vdata(current);

    ant_object_t *ptr = js_obj_ptr(current);
    if (ptr && ptr->shape && search_intern) {
      int32_t slot = ant_shape_lookup_interned(ptr->shape, search_intern);
      if (slot >= 0) {
        const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, (uint32_t)slot);
        if (prop && prop->has_getter) {
          *getter_out = prop->getter;
          *has_getter_out = true;
        }
        return current_id;
      }
    }

    if (ptr && ptr->flags.is_exotic) {
      descriptor_entry_t *desc = lookup_descriptor(js, current, buf, len);
      if (desc && desc->has_getter) {
        *getter_out = desc->getter;
        *has_getter_out = true;
        return current_id;
      }
    }
    
    ant_value_t proto = get_proto(js, current);
    if (!is_object_type(proto)) break;
    current = proto;
    if (proto_overflow_guard_hit_cycle(js, &guard, current)) break;
  }
  
  return 0;
}

static uintptr_t lkp_with_setter(ant_t *js, ant_value_t obj, const char *buf, size_t len, ant_value_t *setter_out, bool *has_setter_out) {
  *has_setter_out = false;
  *setter_out = js_mkundef();
  const char *search_intern = intern_find(buf, len);
  
  ant_value_t current = obj;
  proto_overflow_guard_t guard;
  proto_overflow_guard_init(&guard);
  while (is_object_type(current)) {
    current = js_as_obj(current);
    uintptr_t current_id = (uintptr_t)vdata(current);

    ant_object_t *ptr = js_obj_ptr(current);
    if (ptr && ptr->shape && search_intern) {
      int32_t slot = ant_shape_lookup_interned(ptr->shape, search_intern);
      if (slot >= 0) {
        const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, (uint32_t)slot);
        if (prop && prop->has_setter) {
          *setter_out = prop->setter;
          *has_setter_out = true;
        }
        return current_id;
      }
    }

    if (ptr && ptr->flags.is_exotic) {
      descriptor_entry_t *desc = lookup_descriptor(js, current, buf, len);
      if (desc && desc->has_setter) {
        *setter_out = desc->setter;
        *has_setter_out = true;
        return current_id;
      }
    }
    
    ant_value_t proto = get_proto(js, current);
    if (!is_object_type(proto)) break;
    current = proto;
    if (proto_overflow_guard_hit_cycle(js, &guard, current)) break;
  }
  
  return 0;
}

ant_value_t js_get_proto(ant_t *js, ant_value_t obj) {
  uint8_t t = vtype(obj);

  if (t == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) == kTypeFunction) return js_get_proto(js, promoted);
    return ant_realm_function_proto(js, ant_realm_of_builtin(js, obj));
  }

  if (!is_object_type(obj)) return js_mknull();
  ant_value_t as_obj = js_as_obj(obj);

  ant_object_t *ptr = js_obj_ptr(as_obj);
  ant_value_t proto = ptr ? ptr->proto : js_mkundef();
  if (is_object_type(proto) || vtype(proto) == kTypeNull) return proto;
  
  if (t != kTypeObject) return js_primitive_prototype(js, t);
  return js_mknull();
}

static ant_value_t get_proto(ant_t *js, ant_value_t obj) {
  return js_get_proto(js, obj);
}

void js_set_proto(ant_t *js, ant_value_t obj, ant_value_t proto) {
  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_promote(js, obj);
    if (is_err(promoted)) return;
    obj = promoted;
  }
  if (!is_object_type(obj)) return;

  ant_value_t as_obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(as_obj);
  if (!ptr) return;

  ptr->proto = proto;
  ant_ic_epoch_bump(js);
}

void js_set_proto_init(ant_value_t obj, ant_value_t proto) {
  if (!is_object_type(obj)) return;
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  if (!ptr) return;
  ptr->proto = proto;
}

static void set_proto(ant_t *js, ant_value_t obj, ant_value_t proto) {
  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_promote(js, obj);
    if (is_err(promoted)) return;
    obj = promoted;
  }
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  js_set_proto(js, obj, proto);
  if (ptr) gc_write_barrier(js, ptr, proto);
}

void js_set_proto_wb(ant_t *js, ant_value_t obj, ant_value_t proto) {
  set_proto(js, obj, proto);
}

ant_value_t js_primitive_prototype(ant_t *js, uint8_t type) {
  switch (type) {
    case kTypeObject:       return js->sym.object_proto;
    case kTypeArray:       return js->sym.array_proto;
    case kTypeString:       return js->sym.string_proto;
    case kTypeNumber:       return js->sym.number_proto;
    case kTypeBool:      return js->sym.boolean_proto;
    case kTypeFunction:      return js->sym.function_proto;
    case kTypePromise:   return js->sym.promise_proto;
    case kTypeBigInt:    return js->sym.bigint_proto;
    case kTypeSymbol:    return js->sym.symbol_proto;
    case kTypeGenerator: return js->sym.generator_proto;
    default:          return js_mknull();
  }
}

ant_prop_loc_t lkp_proto(ant_t *js, ant_value_t obj, const char *key, size_t len) {
  uint8_t t = vtype(obj);
  const char *key_intern = intern_find(key, len);
  if (!key_intern) return ANT_PROP_LOC_NONE;

  ant_value_t cur = obj;
  proto_walk_overflow_guard_t guard;
  proto_walk_overflow_guard_init(&guard);

  while (true) {
    if (t == kTypeObject || t == kTypeArray || t == kTypeFunction || t == kTypePromise || t == kTypeGenerator) {
      ant_value_t as_obj = js_as_obj(cur);
      ant_prop_loc_t loc = lkp_interned(as_obj, key_intern);
      if (loc.obj) return loc;
      const ant_object_t *ptr = (const ant_object_t *)vptr(as_obj);
      ant_value_t element;
      if (cur != obj && (vtype(exotic_own_get(js, as_obj, ptr, key, len)) != kTypeUndefined ||
                         array_own_value(js, as_obj, key, len, &element))) break;
    } else if (t == kTypeBuiltin) {
      ant_value_t func_proto = js->sym.function_proto;
      uint8_t ft = vtype(func_proto);
      if (ft == kTypeObject || ft == kTypeArray || ft == kTypeFunction) {
        ant_prop_loc_t loc = lkp_interned(js_as_obj(func_proto), key_intern);
        if (loc.obj) return loc;
      }
      break;
    } else if (!is_boxable_primitive_type(t)) break;

    if (!proto_walk_next(js, &cur, &t, PROTO_WALK_F_LOOKUP)) break;
    if (proto_walk_overflow_guard_hit_cycle(js, &guard, cur, t, PROTO_WALK_F_LOOKUP)) break;
  }

  return ANT_PROP_LOC_NONE;
}

static inline size_t utf16_code_unit_to_wtf8(uint16_t code_unit, char out[4]) {
  if (code_unit >= 0xD800 && code_unit <= 0xDFFF) {
    out[0] = (char)(0xE0 | (code_unit >> 12));
    out[1] = (char)(0x80 | ((code_unit >> 6) & 0x3F));
    out[2] = (char)(0x80 | (code_unit & 0x3F));
    return 3;
  }
  return (size_t)utf8_encode(code_unit, out);
}

static ant_value_t js_mk_ascii_char_cached(ant_t *js, unsigned char ch) {
  ant_value_t cached = js->ascii_chars[ch];
  if (cached != 0) return cached;

  char byte = (char)ch;
  cached = js_mkstr_permanent(js, &byte, 1);
  if (!is_err(cached)) js->ascii_chars[ch] = cached;
  return cached;
}

ant_value_t js_string_from_utf16_code_unit(ant_t *js, uint32_t code_unit) {
  if (code_unit < 128) return js_mk_ascii_char_cached(js, (unsigned char)code_unit);
  char buf[4];
  size_t out_len = utf16_code_unit_to_wtf8((uint16_t)code_unit, buf);
  return js_mkstr(js, buf, out_len);
}

static bool js_try_get_string_index(
  ant_t *js, ant_value_t str,
  const char *key, size_t key_len, ant_value_t *out
) {
  if (!is_array_index(key, (ant_offset_t)key_len)) return false;

  unsigned long idx = 0;
  ant_offset_t byte_len = 0;
  ant_offset_t str_off = vstr(js, str, &byte_len);
  const char *str_data = (const char *)(uintptr_t)(str_off);
  ant_offset_t str_len = str_utf16_len(js, str);
  if (!parse_array_index(key, key_len, str_len, &idx)) return false;

  uint32_t code_unit = utf16_code_unit_at(str_data, byte_len, (ant_offset_t)idx);
  if (code_unit == 0xFFFFFFFF) return false;

  *out = js_string_from_utf16_code_unit(js, code_unit);
  return true;
}

static bool js_try_get_string_own_exotic(
  ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t *out, bool *is_length
) {
  if (is_length) *is_length = false;
  
  bool length_key = is_length_key(key, key_len);
  if (!length_key && !is_array_index(key, (ant_offset_t)key_len)) return false;

  ant_value_t str = vtype(obj) == kTypeObject ? js_string_wrapper_value(obj) : obj;
  if (vtype(str) != kTypeString) return false;

  if (length_key) {
    *out = tov((double)str_utf16_len(js, str));
    if (is_length) *is_length = true;
    return true;
  }

  return js_try_get_string_index(js, str, key, key_len, out);
}

static ant_value_t try_dynamic_getter(ant_t *js, ant_value_t obj, const char *key, size_t key_len) {
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  return ptr ? exotic_own_get(js, obj, ptr, key, key_len) : js_mkundef();
}

static bool try_dynamic_setter(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t value) {
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  if (!ptr || !ptr->flags.is_exotic) return false;
  if (!ptr->exotic_ops || !ptr->exotic_ops->setter) return false;
  return ptr->exotic_ops->setter(js, obj, key, key_len, value);
}

static bool exotic_delete_allowed(ant_t *js, ant_value_t obj, const char *key, size_t key_len) {
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  if (!ptr || !ptr->flags.is_exotic || !ptr->exotic_ops || !ptr->exotic_ops->deleter) return true;
  return ptr->exotic_ops->deleter(js, obj, key, key_len);
}

static bool try_accessor_getter(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t *out) {
  ant_value_t getter = js_mkundef();
  bool has_getter = false;
  lkp_with_getter(js, obj, key, key_len, &getter, &has_getter);

  ant_value_t result = call_proto_accessor(js, obj, getter, has_getter, NULL, 0, false);
  if (!has_getter && vtype(result) == kTypeUndefined) return false;

  *out = result;
  return true;
}

typedef struct {
  char *buffer;
  size_t capacity;
  size_t size;
  bool is_dynamic;
} string_builder_t;

static void string_builder_init(string_builder_t *sb, char *static_buf, size_t static_cap) {
  sb->buffer = static_buf;
  sb->capacity = static_cap;
  sb->size = 0;
  sb->is_dynamic = false;
}

static void string_builder_dispose(string_builder_t *sb) {
  if (sb->is_dynamic && sb->buffer) free(sb->buffer);
  sb->buffer = NULL;
  sb->capacity = 0;
  sb->size = 0;
  sb->is_dynamic = false;
}

static bool string_builder_append(string_builder_t *sb, const char *data, size_t len) {
  if (len > SIZE_MAX - sb->size) return false;
  size_t needed = sb->size + len;

  if (needed > sb->capacity) {
    size_t new_capacity = sb->capacity 
      ? sb->capacity : 256;
    
    while (new_capacity < needed) {
      if (new_capacity > SIZE_MAX / 2) { new_capacity = needed; break; }
      new_capacity *= 2;
    }
    
    char *new_buffer = sb->is_dynamic
      ? (char *)realloc(sb->buffer, new_capacity)
      : (char *)calloc(1, new_capacity);
      
    if (!new_buffer) return false;
    if (!sb->is_dynamic && sb->size > 0) memcpy(new_buffer, sb->buffer, sb->size);
    
    sb->buffer = new_buffer;
    sb->capacity = new_capacity;
    sb->is_dynamic = true;
  }
  
  if (len > 0) {
    memcpy(sb->buffer + sb->size, data, len);
    sb->size += len;
  }
  
  return true;
}

static ant_value_t string_builder_finalize(ant_t *js, string_builder_t *sb) {
  ant_value_t result = js_mkstr(js, sb->buffer, sb->size);
  string_builder_dispose(sb);
  return result;
}

static bool string_builder_append_value(
  ant_t *js, string_builder_t *sb, ant_value_t value, ant_value_t *err
) {
  ant_value_t s = js_tostring_val(js, value);
  if (is_err(s)) {
    if (err) *err = s;
    return false;
  }

  ant_offset_t slen = 0;
  ant_offset_t soff = vstr(js, s, &slen);
  
  if (string_builder_append(sb, 
    (const char *)(uintptr_t)soff, 
    (size_t)slen)
  ) return true;

  if (err) *err = js_mkerr(js, "oom");
  return false;
}

static bool string_template_append_value(
  ant_t *js, string_builder_t *sb, 
  ant_value_t value, ant_value_t *err
) {
switch (vtype(value)) {
  case kTypeString: {
    ant_offset_t len = 0;
    ant_offset_t off = vstr(js, value, &len);
    return string_builder_append(sb, (const char *)(uintptr_t)off, (size_t)len);
  }
  
  case kTypeNumber: {
    char buf[32];
    size_t len = js_number_to_chars(value, buf, sizeof(buf));
    return string_builder_append(sb, buf, len);
  }
  
  case kTypeBool: {
    if (vdata(value)) return string_builder_append(sb, "true", 4);
    return string_builder_append(sb, "false", 5);
  }
  
  case kTypeNull: return string_builder_append(sb, "null", 4);
  case kTypeUndefined: return string_builder_append(sb, "undefined", 9);
  default: return string_builder_append_value(js, sb, value, err);
}}

ant_offset_t str_len_fast(ant_t *js, ant_value_t str) {
  if (vtype(str) != kTypeString) return 0;
  if (str_is_heap_rope(str)) return rope_len(str);
  if (str_is_heap_builder(str)) return builder_len(str);
  return assert_flat_string_len(str, NULL);
}

static ant_offset_t flat_utf16_len(ant_flat_string_t *flat) {
  if (!flat) return 0;

  uint8_t ascii_state = str_flat_ascii_state(flat);
  if (ascii_state == STR_ASCII_UNKNOWN) {
    ascii_state = str_detect_ascii_bytes(flat->bytes, (size_t)flat->len);
    str_flat_set_ascii_state(flat, ascii_state);
  }
  
  if (ascii_state == STR_ASCII_YES) return flat->len;
  ant_offset_t cached = str_flat_cached_utf16_len(flat);
  
  if (cached == STR_UTF16_LEN_UNKNOWN) {
    cached = (ant_offset_t)utf16_strlen(flat->bytes, (size_t)flat->len);
    str_flat_set_utf16_len(flat, cached);
  }
  
  return cached;
}

static ant_offset_t builder_utf16_len_lazy(ant_t *js, ant_value_t value) {
  ant_string_builder_t *builder = assert_builder_ptr(value);
  ant_value_t cached = builder->cached;

  if (vtype(cached) == kTypeNumber) return (ant_offset_t)tod(cached);
  if (vtype(cached) == kTypeString) return str_utf16_len(js, cached);

  ant_offset_t total = 0;
  if (builder->ascii_state == STR_ASCII_YES) total = builder->len;
  else {
    if (vtype(builder->snapshot) == kTypeString)
      total += str_utf16_len(js, builder->snapshot);
    for (ant_builder_chunk_t *chunk = builder->head; chunk; chunk = chunk->next)
      total += str_utf16_len(js, chunk->value);
    total += (ant_offset_t)utf16_strlen(builder->tail, builder->tail_len);
  }

  builder->cached = tov((double)total);
  return total;
}

typedef struct {
  ant_rope_heap_t *rope;
  ant_offset_t left_len;
} rope_utf16_frame_t;

// TODO: make cleaner
static ant_offset_t rope_utf16_len_lazy(ant_t *js, ant_value_t root) {
  if (rope_depth(root) == ANT_ROPE_DEPTH_SATURATED) {
    ant_value_t flat = rope_flatten(js, root);
    return is_err(flat) ? 0 : flat_utf16_len(ant_str_flat_ptr(flat));
  }

  enum { ROPE_UTF16_LOCAL_FRAMES = 64 };
  rope_utf16_frame_t local_frames[ROPE_UTF16_LOCAL_FRAMES];
  rope_utf16_frame_t *frames = local_frames;
  
  size_t frame_cap = ROPE_UTF16_LOCAL_FRAMES;
  size_t sp = 0;
  
  ant_value_t current = root;
  ant_offset_t result = 0;

  for (;;) {
  if (!str_is_heap_rope(current)) {
    result = str_utf16_len(js, current);
  } else {
    ant_rope_heap_t *rope = assert_rope_ptr(current);
    ant_value_t cached = rope->cached;
    
    if (vtype(cached) == kTypeNumber) result = (ant_offset_t)tod(cached);
    else if (vtype(cached) == kTypeString) result = str_utf16_len(js, cached);
    else {
      if (sp == frame_cap) {
        size_t next_cap = frame_cap * 2u;
        rope_utf16_frame_t *expanded = frames == local_frames
          ? (rope_utf16_frame_t *)malloc(next_cap * sizeof(*expanded))
          : (rope_utf16_frame_t *)realloc(frames, next_cap * sizeof(*expanded));
        if (!expanded) {
          ant_value_t flat = rope_flatten(js, root);
          if (is_err(flat)) return 0;
          return flat_utf16_len(ant_str_flat_ptr(flat));
        }
        if (frames == local_frames)
          memcpy(expanded, frames, sp * sizeof(*expanded));
        frames = expanded;
        frame_cap = next_cap;
      }
      frames[sp++] = (rope_utf16_frame_t){
        .rope = rope,
        .left_len = STR_UTF16_LEN_UNKNOWN,
      };
      current = rope->left;
      continue;
    }}

    while (sp > 0) {
      rope_utf16_frame_t *frame = &frames[sp - 1u];
      if (frame->left_len == STR_UTF16_LEN_UNKNOWN) {
        frame->left_len = result;
        current = frame->rope->right;
        break;
      }

      result += frame->left_len;
      frame->rope->cached = tov((double)result);
      sp--;
    }

    if (sp == 0) {
      if (frames != local_frames) free(frames);
      return result;
    }
  }
}

ant_offset_t str_utf16_len(ant_t *js, ant_value_t str) {
  if (vtype(str) != kTypeString) return 0;
  
  if (str_is_heap_rope(str)) {
    ant_value_t cached = rope_cached_flat(str);
    if (vtype(cached) == kTypeNumber) return (ant_offset_t)tod(cached);
    if (vtype(cached) == kTypeString) return str_utf16_len(js, cached);
    return rope_utf16_len_lazy(js, str);
  }
  
  if (str_is_heap_builder(str)) return builder_utf16_len_lazy(js, str);
  return flat_utf16_len(ant_str_flat_ptr(str));
}

ant_value_t do_string_num_concat(ant_t *js, ant_value_t str, ant_value_t num, bool num_first) {
  bool flat_str = !str_is_heap_rope(str) && !str_is_heap_builder(str);
  size_t str_len = flat_str ? (size_t)ant_str_flat_ptr(str)->len : 0;

  int32_t integer;
  bool is_integer = ant_number_as_int32(tod(num), &integer);
  if (flat_str && str_len == 0 && is_integer) return js_mkstr_int32(js, integer);

  char digits[32];
  size_t num_len;
  if (is_integer) {
    num_len = ant_int32_chars_len(integer);
    ant_int32_write(digits, num_len, integer);
  } else num_len = js_number_to_chars(num, digits, sizeof(digits));

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, str);
  ant_value_t result;

  if (flat_str && str_len == 0) {
    GC_ROOT_RESTORE(js, root_mark);
    return mkstr_with_ascii(js, digits, num_len, true);
  }

  if (flat_str && num_len > 0 && str_len + num_len < STR_SHORT_CONS_THRESHOLD) {
    size_t total_len = str_len + num_len;
    result = js_mkstr(js, NULL, total_len);
    
    if (!is_err(result)) {
      ant_flat_string_t *in = ant_str_flat_ptr(str);
      uint8_t in_ascii = str_flat_ascii_state(in);
      if (num_first) str_flat_fill_concat(
        ant_str_flat_ptr(result), digits, num_len, STR_ASCII_YES, in->bytes, str_len, in_ascii);
      else str_flat_fill_concat(
        ant_str_flat_ptr(result), in->bytes, str_len, in_ascii, digits, num_len, STR_ASCII_YES);
    }
  } else {
    ant_value_t num_str = mkstr_with_ascii(js, digits, num_len, true);
    result = is_err(num_str) ? num_str : (num_first
      ? do_string_op(js, TOK_PLUS, num_str, str)
      : do_string_op(js, TOK_PLUS, str, num_str));
  }

  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

ant_value_t do_string_op(ant_t *js, uint8_t op, ant_value_t l, ant_value_t r) {
  if (op == TOK_PLUS) {
    if (str_is_heap_builder(l)) {
      l = builder_flatten(js, l);
      if (is_err(l)) return l;
    }
    
    if (str_is_heap_builder(r)) {
      r = builder_flatten(js, r);
      if (is_err(r)) return r;
    }
    
    ant_offset_t n1 = str_len_fast(js, l);
    ant_offset_t n2 = str_len_fast(js, r);
    ant_offset_t total_len = n1 + n2;
    
    if (n2 == 0) return l;
    if (n1 == 0) return r;

    if (
      total_len < STR_SHORT_CONS_THRESHOLD &&
      !str_is_heap_rope(l) && !str_is_heap_rope(r)
    ) {
      GC_ROOT_SAVE(root_mark, js);
      GC_ROOT_PIN(js, l);
      GC_ROOT_PIN(js, r);
      ant_value_t flat = js_mkstr(js, NULL, (size_t)total_len);
      if (!is_err(flat)) {
        ant_flat_string_t *left = ant_str_flat_ptr(l);
        ant_flat_string_t *right = ant_str_flat_ptr(r);
        str_flat_fill_concat(
          ant_str_flat_ptr(flat),
          left->bytes, (size_t)n1, str_flat_ascii_state(left),
          right->bytes, (size_t)n2, str_flat_ascii_state(right)
        );
      }
      GC_ROOT_RESTORE(js, root_mark);
      return flat;
    }
    
    uint16_t left_depth = str_is_heap_rope(l) ? rope_depth(l) : 0;
    uint16_t right_depth = str_is_heap_rope(r) ? rope_depth(r) : 0;
    unsigned int new_depth = (unsigned int)(left_depth > right_depth ? left_depth : right_depth) + 1u;

    if (new_depth > ANT_ROPE_DEPTH_SATURATED)
      new_depth = ANT_ROPE_DEPTH_SATURATED;
    return js_mkrope(js, l, r, total_len, (uint16_t)new_depth);
  }
  
  ant_offset_t n1, off1 = vstr(js, l, &n1);
  ant_offset_t n2, off2 = vstr(js, r, &n2);
  
  if (op == TOK_EQ) {
    bool eq = n1 == n2 &&
      memcmp((const void *)(uintptr_t)off1, (const void *)(uintptr_t)off2, n1) == 0;
    return mkval(kTypeBool, eq ? 1 : 0);
  } else if (op == TOK_NE) {
    bool eq = n1 == n2 &&
      memcmp((const void *)(uintptr_t)off1, (const void *)(uintptr_t)off2, n1) == 0;
    return mkval(kTypeBool, eq ? 0 : 1);
  } else if (op == TOK_LT || op == TOK_LE || op == TOK_GT || op == TOK_GE) {
    ant_offset_t min_len = n1 < n2 ? n1 : n2;
    int cmp = memcmp((const void *)(uintptr_t)off1, (const void *)(uintptr_t)off2, min_len);
    
    if (cmp == 0) {
      if (n1 == n2) {
        return mkval(kTypeBool, (op == TOK_LE || op == TOK_GE) ? 1 : 0);
      } else cmp = (n1 < n2) ? -1 : 1;
    }
    
    switch (op) {
      case TOK_LT: return mkval(kTypeBool, cmp < 0 ? 1 : 0);
      case TOK_LE: return mkval(kTypeBool, cmp <= 0 ? 1 : 0);
      case TOK_GT: return mkval(kTypeBool, cmp > 0 ? 1 : 0);
      case TOK_GE: return mkval(kTypeBool, cmp >= 0 ? 1 : 0);
      default:     return js_mkerr(js, "bad str op");
    }
  } else return js_mkerr(js, "bad str op");
}


typedef enum { ITER_CONTINUE, ITER_BREAK, ITER_ERROR } iter_action_t;
typedef iter_action_t (*iter_callback_t)(ant_t *js, ant_value_t value, void *ctx, ant_value_t *out);

static bool js_try_invoke(ant_t *js, ant_value_t obj, const char *method, ant_value_t *args, int nargs, ant_value_t *out_result) {
  ant_value_t fn = js_getprop_fallback(js, obj, method);
  if (is_err(fn)) {
    *out_result = fn;
    return true;
  }

  if (!is_callable(fn)) return false;
  *out_result = sv_call_value(js, fn, obj, args, nargs);

  return true;
}

static size_t format_number_to_locale_string(ant_value_t num, char *buf, size_t capacity) {
  char raw[64];
  size_t raw_len = js_number_to_chars(num, raw, sizeof(raw));
  
  double d = tod(num);
  if (!isfinite(d) || memchr(raw, 'e', raw_len) || memchr(raw, 'E', raw_len)) {
    if (raw_len > capacity) return 0;
    memcpy(buf, raw, raw_len);
    return raw_len;
  }

  char *dot = memchr(raw, '.', raw_len);
  size_t int_len = dot ? (size_t)(dot - raw) : raw_len;
  size_t start = raw[0] == '-' ? 1 : 0;
  size_t frac_len = dot ? raw_len - int_len : 0;
  size_t separators = int_len > start ? (int_len - start - 1) / 3 : 0;
  size_t needed = int_len + separators + frac_len;
  if (needed > capacity) return 0;

  size_t pos = 0;
  if (start) buf[pos++] = '-';
  for (size_t i = start; i < int_len; i++) {
    buf[pos++] = raw[i];
    size_t remaining = int_len - 1 - i;
    if (remaining > 0 && remaining % 3 == 0) buf[pos++] = ',';
  }
  if (frac_len) memcpy(buf + pos, dot, frac_len);
  return pos + frac_len;
}

static inline bool is_primitive(ant_value_t v) {
  uint8_t t = vtype(v);
  return is_boxable_primitive_type(t) || t == kTypeNull || t == kTypeUndefined;
}

static bool js_try_exotic_to_primitive(
  ant_t *js, ant_value_t value, int hint,
  ant_value_t *out_result, bool *out_absent
) {
  *out_absent = false;
  ant_value_t tp_sym = js->sym.toPrimitive_sym;
  
  if (vtype(tp_sym) != kTypeSymbol) {
    *out_absent = true;
    return false;
  }

  ant_value_t tp_fn;
  if (!js_try_get_sym_with_receiver(js, value, tp_sym, value, &tp_fn)) {
    *out_absent = true;
    return false;
  }

  if (is_err(tp_fn)) {
    *out_result = tp_fn;
    return true;
  }
  
  if (vtype(tp_fn) == kTypeUndefined || vtype(tp_fn) == kTypeNull) return false;
  if (!is_callable(tp_fn)) {
    *out_result = js_mkerr_typed(js, JS_ERR_TYPE, "Symbol.toPrimitive is not a function");
    return true;
  }
  
  const char *hint_str = hint == 1 ? "string" : (hint == 2 ? "number" : "default");
  ant_value_t hint_arg = js_mkstr(js, hint_str, strlen(hint_str));
  ant_value_t result = sv_vm_call(js->vm, js, tp_fn, value, &hint_arg, 1, NULL, js_mkundef());

  *out_result = (is_err(result) || is_primitive(result))
    ? result
    : js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert object to primitive value");
  
  return true;
}

static inline bool ordinary_primitive_result(ant_value_t result) {
  return is_err(result) || is_primitive(result);
}

static ant_value_t try_ordinary_to_primitive_number(ant_t *js, ant_value_t value) {
  ant_value_t result;
  if (js_try_invoke(js, value, "valueOf", NULL, 0, &result) && ordinary_primitive_result(result)) return result;
  if (js_try_invoke(js, value, "toString", NULL, 0, &result) &&  ordinary_primitive_result(result)) return result;
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert object to primitive value");
}

static ant_value_t try_cached_ordinary_to_primitive_number(
  ant_t *js, ant_value_t value
) {
  const ant_to_primitive_cache_t *cache = &js->runtime_cache.to_primitive_absent;
  ant_object_t *obj = js_to_primitive_cache_object(value);
  
  uint32_t slot = cache->own_valueof_data_slot;
  ant_value_t result;

  if (obj && slot < obj->prop_count) {
    ant_value_t method = ant_object_prop_get_unchecked(obj, slot);
    if (is_callable(method)) {
      result = sv_call_value(js, method, value, NULL, 0);
      if (ordinary_primitive_result(result)) return result;
    }

    if (js_try_invoke(js, value, "toString", NULL, 0, &result) && ordinary_primitive_result(result)) return result;
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert object to primitive value");
  }

  return try_ordinary_to_primitive_number(js, value);
}

static ant_value_t try_ordinary_to_primitive_string(ant_t *js, ant_value_t value) {
  ant_value_t result;
  if (js_try_invoke(js, value, "toString", NULL, 0, &result) && ordinary_primitive_result(result)) return result;
  if (js_try_invoke(js, value, "valueOf", NULL, 0, &result) && ordinary_primitive_result(result)) return result;
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert object to primitive value");
}

static ant_value_t try_ordinary_to_primitive(ant_t *js, ant_value_t value, int hint) {
  return hint == 1
    ? try_ordinary_to_primitive_string(js, value)
    : try_ordinary_to_primitive_number(js, value);
}

static bool js_guard_to_primitive_absence(ant_t *js, ant_value_t value) {
  if (vtype(value) == kTypeFunction) value = js_func_obj(value);
  else if (vtype(value) != kTypeObject && vtype(value) != kTypeArray) return false;

  ant_value_t cur = js_as_obj(value);
  proto_overflow_guard_t guard;
  proto_overflow_guard_init(&guard);

  while (is_object_type(cur)) {
    if (is_proxy(cur)) return false;
    ant_object_t *obj = js_obj_ptr(js_as_obj(cur));
    if (!obj) break;
    ant_object_guard_absence(obj);

    ant_value_t proto = get_proto(js, cur);
    if (!is_object_type(proto)) break;
    cur = js_as_obj(proto);
    if (proto_overflow_guard_hit_cycle(js, &guard, cur)) break;
  }

  return true;
}

ant_value_t js_to_primitive(ant_t *js, ant_value_t value, int hint) {
  if (is_primitive(value)) return value;
  if (!is_object_type(value) && vtype(value) != kTypeBuiltin) return value;

  if (js_to_primitive_cache_hit(js, value))
    return try_ordinary_to_primitive(js, value, hint);

  ant_object_t *cache_obj = js_to_primitive_cache_object(value);
  bool cacheable = cache_obj && !cache_obj->flags.is_exotic;

  ant_value_t result;
  bool exotic_absent = false;
  if (js_try_exotic_to_primitive(js, value, hint, &result, &exotic_absent))
    return result;

  if (cacheable && exotic_absent && js_guard_to_primitive_absence(js, value)) {
    ant_to_primitive_cache_t *cache = &js->runtime_cache.to_primitive_absent;
    cache->object = cache_obj;
    cache->shape = cache_obj->shape;
    cache->proto = cache_obj->proto;
    cache->own_valueof_data_slot = UINT32_MAX;

    ant_prop_loc_t valueof = lkp(js, value, "valueOf", 7);
    const ant_shape_prop_t *valueof_meta = prop_shape_meta(valueof);
    
    if (
      valueof.obj == cache_obj && valueof_meta &&
      !valueof_meta->has_getter && !valueof_meta->has_setter
    ) cache->own_valueof_data_slot = valueof.slot;
    cache->ic_epoch = js->ic.epoch;
  }
  
  return try_ordinary_to_primitive(js, value, hint);
}

bool strict_eq_values(ant_t *js, ant_value_t l, ant_value_t r) {
  uint8_t t = vtype(l);
  if (t != vtype(r)) return false;
  if (t == kTypeString) {
    ant_offset_t n1, n2, off1 = vstr(js, l, &n1), off2 = vstr(js, r, &n2);
    return n1 == n2 &&
      memcmp((const void *)(uintptr_t)off1, (const void *)(uintptr_t)off2, n1) == 0;
  }
  if (t == kTypeNumber) return tod(l) == tod(r);
  if (t == kTypeBigInt) return bigint_compare(js, l, r) == 0;
  return vdata(l) == vdata(r);
}

bool same_value_values(ant_t *js, ant_value_t l, ant_value_t r) {
  uint8_t t = vtype(l);
  if (t != vtype(r)) return false;
  if (t == kTypeUndefined || t == kTypeNull) return true;
  if (t == kTypeNumber) {
    double dl = tod(l);
    double dr = tod(r);
    if (isnan(dl) && isnan(dr)) return true;
    if (dl == 0.0 && dr == 0.0) return !!signbit(dl) == !!signbit(dr);
    return dl == dr;
  }
  return strict_eq_values(js, l, r);
}

ant_value_t coerce_to_str(ant_t *js, ant_value_t v) {
  if (vtype(v) == kTypeString) return v;
  
  if (is_object_type(v) || vtype(v) == kTypeBuiltin) {
    ant_value_t prim = js_to_primitive(js, v, 1);
    if (is_err(prim)) return prim;
    if (vtype(prim) == kTypeString) return prim;
    return js_tostring_val(js, prim);
  }
  
  return js_tostring_val(js, v);
}

typedef struct {
  ant_value_t js_key;
  const char *bytes;
  size_t length;
  char *heap_bytes;
  const char *interned;
  bool is_symbol;
  bool requires_root;
  bool resolved;
  bool index_like;
} property_key_view_t;

static void property_key_view_dispose(property_key_view_t *key) {
  if (key->heap_bytes) free(key->heap_bytes);
  key->js_key = js_mkundef();
  key->heap_bytes = NULL;
  key->bytes = NULL;
  key->length = 0;
  key->is_symbol = false;
  key->requires_root = false;
  key->resolved = false;
}

static inline void property_key_view_resolve(ant_t *js, property_key_view_t *key) {
  if (key->resolved || key->is_symbol) return;
  key->resolved = true;
  
  ant_flat_string_t *flat = ant_str_flat_ptr(key->js_key);
  key->interned = flat && flat->bytes == key->bytes
    ? intern_find_flat(js, key->bytes, key->length)
    : intern_find(key->bytes, key->length);

  key->index_like = is_length_key(key->bytes, key->length) || is_array_index(key->bytes, (ant_offset_t)key->length);
}

static inline void property_key_view_reset(
  property_key_view_t *key, ant_value_t value
) {
  *key = (property_key_view_t){
    .js_key = value,
    .bytes = NULL,
    .length = 0,
    .heap_bytes = NULL,
    .is_symbol = false,
    .requires_root = false,
  };
}

static inline ant_value_t property_key_view_bind_value(
  ant_t *js, property_key_view_t *key, ant_value_t value
) {
  key->js_key = value;
  if (vtype(value) == kTypeSymbol) {
    key->is_symbol = true;
    return js_mkundef();
  }

  ant_offset_t length = 0;
  ant_offset_t offset = vstr(js, value, &length);
  key->bytes = (const char *)(uintptr_t)offset;
  key->length = (size_t)length;
  return js_mkundef();
}

static ant_value_t property_key_view_init_slow(
  ant_t *js, ant_value_t value, property_key_view_t *key,
  char *stack_buf, size_t stack_capacity
) {
  if (is_object_type(value) || vtype(value) == kTypeBuiltin) {
    value = js_to_primitive(js, value, 1);
    key->js_key = value;
    if (is_err(value)) return value;
    key->requires_root = vtype(value) == kTypeSymbol || vtype(value) == kTypeString;
  }

  if (vtype(value) == kTypeSymbol || vtype(value) == kTypeString)
    return property_key_view_bind_value(js, key, value);

  switch (vtype(value)) {
    case kTypeNumber:
      key->length = js_number_to_chars(value, stack_buf, stack_capacity);
      key->bytes = stack_buf;
      return js_mkundef();
    case kTypeBigInt: {
      size_t length = strbigint(js, value, stack_buf, stack_capacity);
      if (length < stack_capacity) {
        key->length = length;
        key->bytes = stack_buf;
        return js_mkundef();
      }

      char *output = (char *)calloc(1, length + 1);
      if (!output) return js_mkerr(js, "oom");
      key->heap_bytes = output;
      key->length = strbigint(js, value, output, length + 1);
      key->bytes = output;
      return js_mkundef();
    }
    case kTypeBool:
      key->bytes = vdata(value) ? "true" : "false";
      key->length = vdata(value) ? 4 : 5;
      return js_mkundef();
    case kTypeNull:
      key->bytes = "null";
      key->length = 4;
      return js_mkundef();
    case kTypeUndefined:
      key->bytes = "undefined";
      key->length = 9;
      return js_mkundef();
    default: break;
  }

  ant_value_t string = js_tostring_val(js, value);
  key->js_key = string;
  
  if (is_err(string)) return string;
  key->requires_root = true;

  return property_key_view_bind_value(js, key, string);
}

static inline ant_value_t property_key_view_init(
  ant_t *js, ant_value_t value, property_key_view_t *key,
  char *stack_buf, size_t stack_capacity
) {
  property_key_view_reset(key, value);
  if (vtype(value) == kTypeSymbol || vtype(value) == kTypeString)
    return property_key_view_bind_value(js, key, value);
  return property_key_view_init_slow(
    js, value, key, stack_buf, stack_capacity
  );
}

static inline bool property_key_view_requires_pin(
  const property_key_view_t *key, bool may_materialize
) {
  return key->requires_root || may_materialize;
}

static ant_value_t property_key_view_materialize(ant_t *js, property_key_view_t *key) {
  if (key->is_symbol || vtype(key->js_key) == kTypeString) return key->js_key;

  ant_value_t string = js_mkstr(js, key->bytes, key->length);
  key->js_key = string;
  if (is_err(string)) return string;
  key->requires_root = true;

  property_key_view_bind_value(js, key, string);
  return string;
}

static ant_value_t coerce_to_str_hint(ant_t *js, ant_value_t v, int hint) {
  if (vtype(v) == kTypeString) return v;

  if (is_object_type(v) || vtype(v) == kTypeBuiltin) {
    v = js_to_primitive(js, v, hint);
    if (is_err(v) || vtype(v) == kTypeString) return v;
  }

  if (vtype(v) == kTypeSymbol)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert a Symbol value to a string");

  return js_tostring_val(js, v);
}

ant_value_t js_template_to_string(ant_t *js, ant_value_t v) {
  return coerce_to_str_hint(js, v, 1);
}

ant_value_t coerce_to_str_concat(ant_t *js, ant_value_t v) {
  return coerce_to_str_hint(js, v, 0);
}

static ant_value_t check_frozen_sealed(ant_t *js, ant_value_t obj, const char *action) {
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  if (!ptr) return js_mkundef();

  if (ptr->flags.frozen) {
    if (sv_is_strict_context(js)) return js_mkerr(js, "cannot %s property of frozen object", action);
    return js_false;
  }
  if (ptr->flags.sealed) {
    if (sv_is_strict_context(js)) return js_mkerr(js, "cannot %s property of sealed object", action);
    return js_false;
  }
  return js_mkundef();
}

ant_value_t js_delete_prop_ordinary(ant_t *js, ant_value_t obj, const char *key, size_t len) {
  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_promote(js, obj);
    if (is_err(promoted)) return promoted;
    obj = promoted;
  }
  
  ant_value_t original_obj = obj;
  obj = js_as_obj(obj);
  
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return js_true;
  
  if (is_proxy(obj)) {
    ant_value_t result = proxy_delete(js, obj, key, len);
    return is_err(result) ? result : js_bool(js_truthy(js, result));
  }

  if (string_wrapper_owns_key(js, obj, key, len)) return js_false;

  if (ptr->flags.frozen || ptr->flags.sealed) {
    bool own = 
      lkp(js, obj, key, len).obj || 
      arr_own_dense_element(js, original_obj, key, len, NULL) ||
      (real_array_obj_ptr(original_obj) && is_length_key(key, len));
    return js_bool(!own);
  }

  if (real_array_obj_ptr(original_obj) && is_length_key(key, len)) return js_false;

  if (array_obj_ptr(original_obj)) {
    ant_offset_t doff = get_dense_buf(original_obj);
    unsigned long del_idx = 0;
    
    if (doff && parse_array_index(key, len, get_array_length(js, original_obj), &del_idx)) {
      array_mark_may_have_holes(original_obj);
      ant_offset_t dense_len = dense_iterable_length(js, original_obj);
      if ((ant_offset_t)del_idx < dense_len) dense_set(js, doff, (ant_offset_t)del_idx, T_EMPTY);
    }
  }

  const char *interned = intern_string(key, len);
  if (!interned) return js_bool(exotic_delete_allowed(js, obj, key, len));

  int32_t shape_slot = ant_shape_lookup_interned(ptr->shape, interned);
  if (shape_slot < 0) return js_bool(exotic_delete_allowed(js, obj, key, len));

  uint8_t attrs = ant_shape_get_attrs(ptr->shape, (uint32_t)shape_slot);
  if ((attrs & ANT_PROP_ATTR_CONFIGURABLE) == 0) return js_false;

  if (ptr->flags.is_exotic) {
    descriptor_entry_t *desc = lookup_descriptor(js, obj, key, len);
    if (desc && !desc->configurable) return js_false;
  }

  uint32_t slot = (uint32_t)shape_slot;
  if (!js_obj_ensure_unique_shape(ptr)) return js_mkerr(js, "oom");
  if (!ant_shape_prop_at(ptr->shape, slot)) return js_true;
  if (!ant_shape_remove_slot(js, ptr->shape, slot)) return js_mkerr(js, "oom");
  
  ant_property_mutation_invalidate(js, ptr, interned);
  obj_delete_prop_slot(ptr, slot);
  
  return js_true;
}

ant_value_t js_delete_prop(ant_t *js, ant_value_t obj, const char *key, size_t len) {
  ant_value_t result = js_delete_prop_ordinary(js, obj, key, len);
  if (result == js_false && sv_is_strict_context(js))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot delete property '%.*s' of object", (int)len, key);
  return result;
}

ant_value_t js_delete_sym_prop(ant_t *js, ant_value_t obj, ant_value_t sym) {
  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_promote(js, obj);
    if (is_err(promoted)) return promoted;
    obj = promoted;
  }
  obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return js_true;
  if (is_proxy(obj)) {
    ant_value_t result = proxy_delete_val(js, obj, sym);
    return is_err(result) ? result : js_bool(js_truthy(js, result));
  }

  ant_value_t err = check_frozen_sealed(js, obj, "delete");
  if (vtype(err) != kTypeUndefined) return err;

  ant_offset_t sym_off = (ant_offset_t)vdata(sym);
  int32_t shape_slot = ant_shape_lookup_symbol(ptr->shape, sym_off);
  if (shape_slot < 0) return js_true;

  uint8_t attrs = ant_shape_get_attrs(ptr->shape, (uint32_t)shape_slot);
  if ((attrs & ANT_PROP_ATTR_CONFIGURABLE) == 0) {
    if (sv_is_strict_context(js)) return js_mkerr_typed(js, JS_ERR_TYPE, "cannot delete non-configurable property");
    return js_false;
  }

  uint32_t slot = (uint32_t)shape_slot;
  if (!js_obj_ensure_unique_shape(ptr)) return js_mkerr(js, "oom");
  if (!ant_shape_prop_at(ptr->shape, slot)) return js_true;
  if (!ant_shape_remove_slot(js, ptr->shape, slot)) return js_mkerr(js, "oom");
  ant_symbol_property_mutation_invalidate(js, ptr, sym_off);
  obj_delete_prop_slot(ptr, slot);
  
  return js_true;
}

static ant_value_t iter_call_noargs_with_this(ant_t *js, ant_value_t this_val, ant_value_t method) {
  ant_value_t result = sv_vm_call(js->vm, js, method, this_val, NULL, 0, NULL, js_mkundef());
  return result;
}

static ant_value_t iter_close_iterator(ant_t *js, ant_value_t iterator) {
  ant_prop_loc_t return_off = lkp_proto(js, iterator, "return", 6);
  if (!return_off.obj) return js_mkundef();
  ant_value_t return_method = js_prop_load(return_off);
  if (!is_callable(return_method))
    return js_mkerr(js, "iterator.return is not a function");
  return iter_call_noargs_with_this(js, iterator, return_method);
}

static ant_value_t iter_foreach_method(
  ant_t *js, ant_value_t iterable, ant_value_t iter_method, iter_callback_t cb, void *ctx
) {
  if (!is_callable(iter_method)) return js_mkerr_typed(js, JS_ERR_TYPE, "not iterable");
  
  ant_value_t iterator = iter_call_noargs_with_this(js, iterable, iter_method);
  if (is_err(iterator)) return iterator;
  
  ant_value_t out = js_mkundef();
  
  while (true) {
    ant_prop_loc_t next_off = lkp_proto(js, iterator, "next", 4);
    if (!next_off.obj) { return js_mkerr(js, "iterator.next is not a function"); }
    
    ant_value_t next_method = js_prop_load(next_off);
    if (!is_callable(next_method)) {
      return js_mkerr(js, "iterator.next is not a function");
    }
    
    ant_value_t result = iter_call_noargs_with_this(js, iterator, next_method);
    if (is_err(result)) { return result; }
    
    ant_prop_loc_t done_off = lkp(js, result, "done", 4);
    ant_value_t done_val = done_off.obj ? js_prop_load(done_off) : js_mkundef();
    if (js_truthy(js, done_val)) break;
    
    ant_prop_loc_t value_off = lkp(js, result, "value", 5);
    ant_value_t value = value_off.obj ? js_prop_load(value_off) : js_mkundef();
    
    iter_action_t action = cb(js, value, ctx, &out);
    if (action == ITER_BREAK) {
      ant_value_t close_result = iter_close_iterator(js, iterator);
      if (is_err(close_result)) { return close_result; }
      break;
    }
    if (action == ITER_ERROR) {
      ant_value_t close_result = iter_close_iterator(js, iterator);
      if (is_err(close_result)) return close_result;
      return out;
    }
  }
  
  return out;
}

static ant_value_t iter_foreach(ant_t *js, ant_value_t iterable, iter_callback_t cb, void *ctx) {
  if (vtype(js->sym.iterator_sym) != kTypeSymbol) return js_mkerr_typed(js, JS_ERR_TYPE, "not iterable");
  ant_value_t iter_method = js_get_sym(js, iterable, js->sym.iterator_sym);
  if (is_err(iter_method)) return iter_method;
  return iter_foreach_method(js, iterable, iter_method, cb, ctx);
}

ant_value_t js_symbol_to_string(ant_t *js, ant_value_t sym) {
  const char *desc = js_sym_desc(sym);
  if (!desc) return js_mkstr(js, "Symbol()", 8);
  
  size_t desc_len = strlen(desc);
  size_t total = 7 + desc_len + 1;
  
  char stack_buf[128];
  char *buf = (total + 1 <= sizeof(stack_buf)) ? stack_buf : malloc(total + 1);
  if (!buf) return js_mkerr(js, "out of memory");
  
  memcpy(buf, "Symbol(", 7);
  memcpy(buf + 7, desc, desc_len);
  buf[7 + desc_len] = ')';
  buf[total] = '\0';
  
  ant_value_t result = js_mkstr(js, buf, total);
  if (buf != stack_buf) free(buf);
  return result;
}

static ant_value_t builtin_String(ant_params_t) {
  ant_value_t sval;
  
  if (nargs == 0) sval = js_mkstr(js, "", 0);
  else if (vtype(args[0]) == kTypeString) sval = args[0];
  
  else if (vtype(args[0]) == kTypeSymbol) {
    sval = js_symbol_to_string(js, args[0]);
    if (is_err(sval)) return sval;
  } else {
    sval = coerce_to_str(js, args[0]);
    if (is_err(sval)) return sval;
  }
  
  if (is_wrapper_ctor_target(js, js->this_val, call_new_target)) {
    set_slot(js->this_val, SLOT_PRIMITIVE, sval);
    ant_value_t length = mkprop_exact_attrs(js, js->this_val, js->length_str, tov((double)str_utf16_len(js, sval)), 0);
    if (is_err(length)) return length;
  }
  
  return sval;
}

static ant_value_t builtin_Number_isNaN(ant_params_t) {
  if (nargs == 0) return mkval(kTypeBool, 0);
  ant_value_t arg = args[0];
  
  if (vtype(arg) != kTypeNumber) return mkval(kTypeBool, 0);
  
  double val = tod(arg);
  return mkval(kTypeBool, isnan(val) ? 1 : 0);
}

static ant_value_t builtin_Number_isFinite(ant_params_t) {
  if (nargs == 0) return mkval(kTypeBool, 0);
  ant_value_t arg = args[0];
  
  if (vtype(arg) != kTypeNumber) return mkval(kTypeBool, 0);
  
  double val = tod(arg);
  return mkval(kTypeBool, isfinite(val) ? 1 : 0);
}

static ant_value_t builtin_global_isNaN(ant_params_t) {
  if (nargs == 0) return mkval(kTypeBool, 1);
  double val = js_to_number(js, args[0]);
  return mkval(kTypeBool, isnan(val) ? 1 : 0);
}

static ant_value_t builtin_global_isFinite(ant_params_t) {
  if (nargs == 0) return mkval(kTypeBool, 0);
  double val = js_to_number(js, args[0]);
  return mkval(kTypeBool, isfinite(val) ? 1 : 0);
}

ant_value_t js_codegen_refused(ant_t *js) {
  return js_mkerr_typed(js, JS_ERR_EVAL, "Code generation from strings disallowed for this context");
}

static ant_value_t builtin_eval(ant_params_t) {
  if (nargs == 0) return js_mkundef();
  ant_value_t code = args[0];
  
  if (vtype(code) != kTypeString) return code;
  if (__builtin_expect(js->codegen_blocked & ANT_CODEGEN_NO_STRINGS, 0)) return js_codegen_refused(js);
  
  ant_offset_t code_len = 0;
  ant_offset_t code_off = vstr(js, code, &code_len);
  const char *code_str = (const char *)(uintptr_t)(code_off);
  
  return js_eval_bytecode_eval_with_strict(js, code_str, (size_t)code_len, false);
}

ant_value_t js_builtin_eval(ant_t *js) {
  if (vtype(js->sym.eval_fn) == kTypeBuiltin) return js->sym.eval_fn;
  ant_value_t eval_fn = js_cfunc_expose_named(js, Ant_Function_CreateWithArity(builtin_eval, 1), "eval", 4);
  if (vtype(eval_fn) == kTypeBuiltin) js->sym.eval_fn = eval_fn;
  return eval_fn;
}

static ant_value_t builtin_Number_isInteger(ant_params_t) {
  if (nargs == 0) return mkval(kTypeBool, 0);
  ant_value_t arg = args[0];
  
  if (vtype(arg) != kTypeNumber) return mkval(kTypeBool, 0);
  
  double val = tod(arg);
  if (!isfinite(val)) return mkval(kTypeBool, 0);
  return mkval(kTypeBool, (val == floor(val)) ? 1 : 0);
}

static ant_value_t builtin_Number_isSafeInteger(ant_params_t) {
  if (nargs == 0) return mkval(kTypeBool, 0);
  ant_value_t arg = args[0];
  
  if (vtype(arg) != kTypeNumber) return mkval(kTypeBool, 0);
  
  double val = tod(arg);
  if (!isfinite(val)) return mkval(kTypeBool, 0);
  if (val != floor(val)) return mkval(kTypeBool, 0);
  
  return mkval(kTypeBool, (val >= -9007199254740991.0 && val <= 9007199254740991.0) ? 1 : 0);
}

static ant_value_t builtin_Number(ant_params_t) {
  ant_value_t nval = tov(nargs > 0 ? js_to_number(js, args[0]) : 0.0);
  if (is_wrapper_ctor_target(js, js->this_val, call_new_target))
    set_slot(js->this_val, SLOT_PRIMITIVE, nval);
  return nval;
}

static ant_value_t builtin_Boolean(ant_params_t) {
  ant_value_t bval = mkval(kTypeBool, nargs > 0 && js_truthy(js, args[0]) ? 1 : 0);
  if (is_wrapper_ctor_target(js, js->this_val, call_new_target))
    set_slot(js->this_val, SLOT_PRIMITIVE, bval);
  return bval;
}

ant_value_t js_normalize_sloppy_this(ant_t *js, ant_value_t value) {
  if (vtype(value) == kTypeUndefined || vtype(value) == kTypeNull) return js->global_this;
  if (is_object_type(value) || vtype(value) == kTypeBuiltin) return value;

  uint8_t type = vtype(value);
  if (!is_boxable_primitive_type(type)) return value;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, value);
  ant_value_t wrapper = Ant_Object_CreateBare(js);
  if (!is_err(wrapper)) {
    GC_ROOT_PIN(js, wrapper);
    js_set_slot_wb(js, wrapper, SLOT_PRIMITIVE, value);
    ant_value_t proto = js_primitive_prototype(js, type);
    if (is_object_type(proto)) js_set_proto_wb(js, wrapper, proto);
  }
  GC_ROOT_RESTORE(js, root_mark);
  return wrapper;
}

static ant_value_t builtin_Object(ant_params_t) {
  if (nargs == 0 || vtype(args[0]) == kTypeNull || vtype(args[0]) == kTypeUndefined) {
    ant_value_t obj_proto = js->sym.object_proto;
    if (is_unboxed_obj(js, js->this_val, obj_proto)) return js->this_val;
    return Ant_Object_Create(js);
  }
  
  ant_value_t arg = args[0];
  uint8_t t = vtype(arg);
  
  if (t == kTypeObject || t == kTypeArray || t == kTypeFunction) return arg;
  if (is_boxable_primitive_type(t))
    return js_normalize_sloppy_this(js, arg);
  
  return arg;
}

static ant_value_t builtin_function_empty(ant_params_t);

static const char *const anonymous_prefixes[2][2] = {
  { "function anonymous(", "function* anonymous(" },
  { "async function anonymous(", "async function* anonymous(" },
};

static ant_value_t build_dynamic_function(ant_native_params_t, ant_value_t new_target, bool is_async, bool is_generator) {
  size_t params_len = 0;
  
  for (int i = 0; i < nargs - 1; i++) {
    args[i] = coerce_to_str(js, args[i]);
    if (is_err(args[i])) return args[i];
    params_len += vstrlen(js, args[i]);
    if (i < nargs - 2) params_len += 1;
  }
  
  ant_value_t body = nargs > 0 ? coerce_to_str(js, args[nargs - 1]) : js_mkstr(js, "", 0);
  if (is_err(body)) return body;
  size_t body_len = vstrlen(js, body);
  
  const char *prefix = anonymous_prefixes[!!is_async][!!is_generator];
  size_t prefix_len = strlen(prefix);
  
  sv_function_parts_t parts = { .is_async = is_async, .is_generator = is_generator };
  parts.params_off = (ant_offset_t)prefix_len;
  parts.params_end = parts.params_off + (ant_offset_t)params_len;
  parts.body_off = parts.params_end + 5;
  parts.body_end = parts.body_off + (ant_offset_t)body_len;
  parts.len = parts.body_end + 2;
  
  char *text = (char *)malloc((size_t)parts.len + 1);
  if (!text) return js_mkerr(js, "oom");
  
  size_t pos = prefix_len;
  memcpy(text, prefix, prefix_len);
  for (int i = 0; i < nargs - 1; i++) {
    ant_offset_t param_len, param_off = vstr(js, args[i], &param_len);
    memcpy(text + pos, (const void *)(uintptr_t)param_off, param_len);
    pos += param_len;
    if (i < nargs - 2) text[pos++] = ',';
  }
  
  memcpy(text + pos, "\n) {\n", 5);
  pos += 5;
  
  ant_offset_t body_str = vstr(js, body, NULL);
  memcpy(text + pos, (const void *)(uintptr_t)body_str, body_len);
  pos += body_len;
  
  memcpy(text + pos, "\n}", 2);
  pos += 2;
  text[pos] = '\0';
  parts.text = text;

  ant_value_t func_obj = mkobj(js, 0);
  if (is_err(func_obj)) { free(text); return func_obj; }

  sv_func_t *compiled = sv_compile_function(js, &parts);
  if (!compiled) {
    free(text);
    if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "invalid function body");
  }

  sv_closure_t *closure = js_closure_alloc(js);
  if (!closure) {
    sv_code_unit_unpin(compiled->unit);
    free(text);
    return js_mkerr(js, "oom");
  }
  
  closure->func = compiled;
  closure->bound_this = js_mkundef();
  closure->call_flags = 0;
  closure->func_obj = func_obj;
  sv_code_unit_unpin(compiled->unit);

  js_set_func_source(js, func_obj, text, (size_t)parts.len,
    compiled->unit ? JS_FUNC_SOURCE_UNIT : JS_FUNC_SOURCE_COPY);
  free(text);
  
  ant_value_t length_result = mkprop_interned_exact(
    js, func_obj, js->intern.length,
    tov((double)compiled->function_length), ANT_PROP_ATTR_CONFIGURABLE
  );
  
  ant_value_t name_result = is_err(length_result)
    ? length_result
    : js_set_function_name(js, func_obj, "anonymous", 9);
  if (is_err(name_result)) return name_result;
  
  if (is_async && is_generator) {
    set_slot(func_obj, SLOT_ASYNC, js_true);
    ant_value_t async_generator_proto = get_slot(js_glob(js), SLOT_ASYNC_GENERATOR_PROTO);
    if (is_object_type(async_generator_proto)) js_set_proto_init(func_obj, async_generator_proto);
  }
  
  else if (is_async) {
    set_slot(func_obj, SLOT_ASYNC, js_true);
    ant_value_t async_proto = get_slot(js_glob(js), SLOT_ASYNC_PROTO);
    if (is_object_type(async_proto)) js_set_proto_init(func_obj, async_proto);
  }
  
  else if (is_generator) {
    ant_value_t generator_proto = get_slot(js_glob(js), SLOT_GENERATOR_PROTO);
    if (is_object_type(generator_proto)) js_set_proto_init(func_obj, generator_proto);
  }
  
  else {
    ant_value_t func_proto = get_slot(js_glob(js), SLOT_FUNC_PROTO);
    ant_value_t instance_proto = js_instance_proto_from_new_target(js, func_proto, new_target);
    if (is_object_type(instance_proto)) js_set_proto_init(func_obj, instance_proto);
  }

  ant_value_t func = mkref(kTypeFunction, closure);
  if (!is_async || is_generator) {
    ant_value_t proto_setup = is_generator
      ? setup_func_prototype_property(js, func, false)
      : setup_func_prototype(js, func);
    if (is_err(proto_setup)) return proto_setup;
  }
  
  if (is_generator) {
    ant_value_t prototype = js_get(js, func, "prototype");
    ant_value_t parent_proto = is_async ? js->sym.async_generator_proto : js->sym.generator_proto;
    if (is_object_type(prototype) && is_object_type(parent_proto)) js_set_proto_wb(js, prototype, parent_proto);
  }
  
  return func;
}

static ant_value_t builtin_Function(ant_params_t) {
  if (__builtin_expect(js->codegen_blocked & ANT_CODEGEN_NO_STRINGS, 0)) return js_codegen_refused(js);
  return build_dynamic_function(js, args, nargs, call_new_target, false, false);
}

static ant_value_t builtin_AsyncFunction(ant_params_t) {
  if (__builtin_expect(js->codegen_blocked & ANT_CODEGEN_NO_STRINGS, 0)) return js_codegen_refused(js);
  return build_dynamic_function(js, args, nargs, call_new_target, true, false);
}

static ant_value_t builtin_AsyncGeneratorFunction(ant_params_t) {
  if (__builtin_expect(js->codegen_blocked & ANT_CODEGEN_NO_STRINGS, 0)) return js_codegen_refused(js);
  return build_dynamic_function(js, args, nargs, call_new_target, true, true);
}

static ant_value_t builtin_GeneratorFunction(ant_params_t) {
  if (__builtin_expect(js->codegen_blocked & ANT_CODEGEN_NO_STRINGS, 0)) return js_codegen_refused(js);
  return build_dynamic_function(js, args, nargs, call_new_target, false, true);
}

static ant_value_t builtin_function_empty(ant_params_t) {
  return js_mkundef();
}

static ant_value_t builtin_function_call(ant_params_t) {
  ant_value_t func = js->this_val;
  if (vtype(func) != kTypeFunction && vtype(func) != kTypeBuiltin) {
    return js_mkerr(js, "call requires a function");
  }
  
  ant_value_t this_arg = (nargs > 0) ? args[0] : js_mkundef();
  ant_value_t *call_args = NULL;
  
  int call_nargs = (nargs > 1) ? nargs - 1 : 0;
  if (call_nargs > 0) call_args = &args[1];

  return sv_vm_call_explicit_this(js->vm, js, func, this_arg, call_args, call_nargs);
}

ant_value_t js_arg_list_from(ant_t *js, ant_value_t array_like, js_arg_list_t *list) {
  list->args = NULL;
  list->argc = 0;
  gc_temp_root_scope_begin(js, &list->roots);

  ant_offset_t len = 0;
  ant_value_t result = js_mkundef();
  
  if (real_array_obj_ptr(array_like)) len = get_array_length(js, array_like);
  else result = proxy_aware_length(js, array_like, &len);
  if (is_err(result)) goto fail;

  if (len > (ant_offset_t)INT_MAX) {
    result = js_mkerr_typed(js, JS_ERR_RANGE, "Argument list length exceeds engine limit");
    goto fail;
  }

  ant_value_t *args = gc_temp_root_slots(&list->roots, (size_t)len);
  if (!args) { result = js_mkerr(js, "out of memory"); goto fail; }

  for (ant_offset_t i = 0; i < len; i++) {
    if (array_method_dense_element(array_like, i, &args[i])) continue;
    args[i] = array_method_get_index(js, array_like, i);
    if (is_err(args[i])) { result = args[i]; goto fail; }
  }

  list->args = args;
  list->argc = (int)len;
  return js_mkundef();

fail:
  js_arg_list_release(list);
  return result;
}

void js_arg_list_release(js_arg_list_t *list) {
  gc_temp_root_scope_end(&list->roots);
  list->args = NULL;
  list->argc = 0;
}

static ant_value_t builtin_function_toString(ant_params_t) {
  ant_value_t func = js->this_val;
  uint8_t t = vtype(func);
  
  if (t != kTypeFunction && t != kTypeBuiltin) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Function.prototype.toString requires that 'this' be a Function");
  }
  
  // TODO: make dry
  if (t == kTypeBuiltin) {
    ant_offset_t name_len = 0;
    const char *name = get_func_name(js, func, &name_len);
    if (name && name_len > 0) {
      size_t total = 9 + name_len + 21 + 1;
      char *buf = calloc(1, total);
      size_t n = 0;
      n += cpy(buf + n, total - n, "function ", 9);
      n += cpy(buf + n, total - n, name, name_len);
      n += cpy(buf + n, total - n, "() { [native code] }", 20);
      ant_value_t result = js_mkstr(js, buf, n);
      free(buf);
      return result;
    }
    return ANT_STRING("function() { [native code] }");
  }
  
  sv_closure_t *bound = js_func_closure(func);
  if (bound && (bound->call_flags & SV_CALL_HAS_BOUND_THIS))
    return ANT_STRING("function () { [native code] }");

  ant_value_t func_obj = js_func_obj(func);
  ant_value_t cfunc_slot = get_slot(func_obj, SLOT_CFUNC);
  
  // TODO: make dry
  // native, unless it carries source text (a body-less dynamic function)
  if (vtype(cfunc_slot) == kTypeBuiltin && !func_has_code(get_slot(func_obj, SLOT_CODE))) {
    ant_offset_t name_len = 0;
    const char *name = get_func_name(js, func, &name_len);
    if (name && name_len > 0) {
      size_t total = 9 + name_len + 21 + 1;
      char *buf = calloc(1, total);
      size_t n = 0;
      n += cpy(buf + n, total - n, "function ", 9);
      n += cpy(buf + n, total - n, name, name_len);
      n += cpy(buf + n, total - n, "() { [native code] }", 20);
      ant_value_t result = js_mkstr(js, buf, n);
      free(buf);
      return result;
    }
    return ANT_STRING("function() { [native code] }");
  }
  
  ant_value_t code_val = get_slot(func_obj, SLOT_CODE);
  ant_value_t len_val = get_slot(func_obj, SLOT_CODE_LEN);
  
  const char *code = func_code_text(js, code_val);
  if (code && vtype(len_val) == kTypeNumber) {
    size_t code_len = (size_t)tod(len_val);
    
    if (code && code_len > 0) {
      ant_value_t async_slot = get_slot(func_obj, SLOT_ASYNC);
      sv_closure_t *closure = js_func_closure(func);
      
      bool is_async = (async_slot == js_true);
      bool is_arrow = closure && (closure->call_flags & SV_CALL_IS_ARROW) != 0;
      
      if (is_arrow) {
        const char *paren_end = memchr(code, ')', code_len);
        if (!paren_end) goto fallback_arrow;
        
        size_t params_len = paren_end - code + 1;
        const char *body = paren_end + 1;
        size_t body_len = code_len - params_len;
        
        size_t len = (is_async ? 6 : 0) + params_len + 4 + body_len + 1;
        char *buf = calloc(1, len);
        size_t n = 0;
        
        if (is_async) n += cpy(buf + n, REMAIN(n, len), "async ", 6);
        n += cpy(buf + n, REMAIN(n, len), code, params_len);
        n += cpy(buf + n, REMAIN(n, len), " => ", 4);
        n += cpy(buf + n, REMAIN(n, len), body, body_len);
        
        ant_value_t result = js_mkstr(js, buf, n);
        free(buf);
        return result;
        fallback_arrow:;
      }
      
      return js_mkstr(js, code, code_len);
    }
  }
  
  sv_closure_t *cl = js_func_closure(func);
  if (cl->func != NULL) {
    sv_func_t *fn = cl->func;
    if (fn && fn->debug->source && fn->debug->source_end > fn->debug->source_start) {
      int start = fn->debug->source_start;
      int end   = fn->debug->source_end;
      if (start >= 0 && end <= fn->debug->source_len && end > start)
        return js_mkstr(js, fn->debug->source + start, (size_t)(end - start));
    }
  }

  char stack_buf[256];
  js_cstr_t text = js_to_cstr(js, func, stack_buf, sizeof(stack_buf));
  
  ant_value_t result = js_mkstr(js, text.ptr, text.len);
  if (text.needs_free) free((void *)text.ptr);
  
  return result;
}

ant_value_t builtin_function_apply(ant_params_t) {
  ant_value_t func = js->this_val;
  if (vtype(func) != kTypeFunction && vtype(func) != kTypeBuiltin)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Function.prototype.apply requires that 'this' be a Function");
  
  ant_value_t this_arg = (nargs > 0) ? args[0] : js_mkundef();
  ant_value_t arg_array = (nargs > 1) ? args[1] : js_mkundef();
  
  if (vtype(arg_array) != kTypeArray && vtype(arg_array) != kTypeObject)
    return sv_vm_call_explicit_this(js->vm, js, func, this_arg, NULL, 0);

  js_arg_list_t list;
  ant_value_t err = js_arg_list_from(js, arg_array, &list);
  if (is_err(err)) return err;

  ant_value_t result = sv_vm_call_explicit_this(js->vm, js, func, this_arg, list.args, list.argc);
  js_arg_list_release(&list);
  
  return result;
}

bool js_is_function_apply_builtin(ant_value_t func) {
  return 
    vtype(func) == kTypeBuiltin && 
    js_as_cfunc(func) == builtin_function_apply;
}

static ant_value_t builtin_bound_proxy_call(ant_params_t) {
  ant_value_t bound = js->current_func;
  if (vtype(bound) != kTypeFunction) return js_mkerr_typed(js, JS_ERR_TYPE, "invalid bound proxy call");

  ant_value_t target = js_get_slot(js_func_obj(bound), SLOT_TARGET_FUNC);
  if (vtype(target) == kTypeUndefined) return js_mkerr_typed(js, JS_ERR_TYPE, "invalid bound proxy target");

  if (vtype(call_new_target) != kTypeUndefined) {
    return js_proxy_construct(js, target, args, nargs, call_new_target);
  }

  return sv_vm_call(js->vm, js, target, js->this_val, args, nargs, NULL, js_mkundef());
}

static __attribute__((noinline, cold))
bool js_bound_target_next(ant_value_t value, ant_value_t *next) {
  if (vtype(value) != kTypeFunction) return false;
  sv_closure_t *closure = js_func_closure(value);
  if (
    !closure ||
    !(closure->call_flags & (SV_CALL_HAS_BOUND_THIS | SV_CALL_HAS_BOUND_ARGS))
  ) return false;

  ant_value_t target = get_slot(js_func_obj(value), SLOT_TARGET_FUNC);
  if (vtype(target) == kTypeUndefined) return false;
  *next = target;
  return true;
}

static __attribute__((noinline, cold))
ant_value_t js_resolve_bound_target_slow(ant_value_t value) {
  ant_value_t slow = value;
  ant_value_t fast = value;
  ant_value_t next;

  for (;;) {
    if (!js_bound_target_next(slow, &next)) break;
    slow = next;
    if (!js_bound_target_next(fast, &next)) break;
    fast = next;
    if (!js_bound_target_next(fast, &next)) break;
    fast = next;
    ANT_ASSERT(slow != fast, "bound target chain contains a cycle");
  }

  while (js_bound_target_next(value, &next)) value = next;
  return value;
}

ant_value_t js_resolve_bound_target_known_bound(ant_value_t value) {
  ant_value_t target = get_slot(js_func_obj(value), SLOT_TARGET_FUNC);
  if (vtype(target) == kTypeUndefined) return value;

  sv_closure_t *closure =
    vtype(target) == kTypeFunction ? js_func_closure(target) : NULL;
  if (
    closure &&
    (closure->call_flags & (SV_CALL_HAS_BOUND_THIS | SV_CALL_HAS_BOUND_ARGS))
  ) return js_resolve_bound_target_slow(value);
  return target;
}

ant_value_t js_resolve_bound_target(ant_value_t value) {
  sv_closure_t *closure =
    vtype(value) == kTypeFunction ? js_func_closure(value) : NULL;
  if (
    !closure ||
    !(closure->call_flags & (SV_CALL_HAS_BOUND_THIS | SV_CALL_HAS_BOUND_ARGS))
  ) return value;
  return js_resolve_bound_target_known_bound(value);
}

static inline bool bound_argv_copy(
  sv_closure_t *closure,
  const ant_value_t *prefix, int prefix_count,
  const ant_value_t *suffix, int suffix_count
) {
  int total = prefix_count + suffix_count;
  if (total == 0) {
    closure->u.bound.argv = NULL;
    closure->bound_argc = 0;
    return true;
  }

  ant_value_t *argv = malloc(sizeof(*argv) * (size_t)total);
  if (!argv) return false;
  if (prefix_count > 0)
    memcpy(argv, prefix, sizeof(*argv) * (size_t)prefix_count);
  if (suffix_count > 0)
    memcpy(argv + prefix_count, suffix, sizeof(*argv) * (size_t)suffix_count);
  closure->u.bound.argv = argv;
  closure->bound_argc = total;
  return true;
}

ant_value_t js_define_function_length(ant_t *js, ant_value_t fn, int length) {
  return mkprop_interned_exact(js, js_as_obj(fn), js->intern.length, tov((double)length), ANT_PROP_ATTR_CONFIGURABLE);
}

static ant_value_t builtin_function_bind(ant_params_t) {
  ant_value_t func = js->this_val;
  uint8_t func_type = vtype(func);
  bool func_is_proxy = func_type == kTypeObject && is_proxy(func);
  
  if (func_type != kTypeFunction && func_type != kTypeBuiltin && !func_is_proxy) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "bind requires a function");
  }
  if (func_is_proxy && !is_callable(func)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "bind requires a function");
  }

  ant_value_t this_arg = (nargs > 0) ? args[0] : js_mkundef();
  int bound_argc = (nargs > 1) ? nargs - 1 : 0;
  ant_value_t *bound_args = (bound_argc > 0) ? &args[1] : NULL;
  
  int orig_length = 0;
  
  if (func_is_proxy) {
    ant_value_t has_length = proxy_has_own(js, func, js->length_str);
    if (is_err(has_length)) return has_length;
    ant_value_t len_val = proxy_get(js, func, "length", 6);
    if (is_err(len_val)) return len_val;
    if (vtype(len_val) == kTypeNumber) orig_length = (int) tod(len_val);
  } else {
    ant_value_t target = func_type == kTypeBuiltin ? js_cfunc_lookup_promoted(js, func) : func;
    ant_value_t len_val = vtype(target) == kTypeBuiltin
      ? js_cfunc_length_value(target)
      : lkp_interned_val(js, js_func_obj(target), js->intern.length);
    if (vtype(len_val) == kTypeNumber) orig_length = (int) tod(len_val);
  }
  
  int bound_length = orig_length - bound_argc;
  if (bound_length < 0) bound_length = 0;

  const char *target_name = "";
  size_t target_name_len = 0;
  ant_value_t target_name_val = js_mkundef();
  if (func_is_proxy) {
    target_name_val = proxy_get(js, func, "name", 4);
    if (is_err(target_name_val)) return target_name_val;
  } else if (func_type == kTypeBuiltin) {
    const ant_cfunc_meta_t *meta = js_as_cfunc_meta(func);
    if (meta && meta->name) {
      target_name = meta->name;
      target_name_len = strlen(meta->name);
    }
  } else {
    target_name_val = js_getprop_fallback(js, func, "name");
  }
  if (vtype(target_name_val) == kTypeString) {
    ant_offset_t nlen = 0;
    ant_offset_t noff = vstr(js, target_name_val, &nlen);
    target_name = (const char *)(uintptr_t)noff;
    target_name_len = (size_t)nlen;
  }

  if (func_is_proxy) {
    ant_value_t bound_func = mkobj(js, 0);
    if (is_err(bound_func)) return bound_func;

    set_slot(bound_func, SLOT_CFUNC, Ant_Function_Create(builtin_bound_proxy_call));
    set_slot(bound_func, SLOT_TARGET_FUNC, func);

    ant_value_t func_proto = get_slot(js_glob(js), SLOT_FUNC_PROTO);
    if (vtype(func_proto) == kTypeFunction) js_set_proto_init(bound_func, func_proto);

    uint8_t bind_flags = SV_CALL_HAS_BOUND_THIS;
    if (bound_argc > 0) bind_flags |= SV_CALL_HAS_BOUND_ARGS;
    
    ant_value_t bound = js_obj_to_func_ex(js, bound_func, bind_flags);
    if (is_err(bound)) return bound;
    
    sv_closure_t *bc = js_func_closure(bound);
    bc->bound_this = this_arg;
    
    if (bound_argc > 0 && !bound_argv_copy(bc, bound_args, bound_argc, NULL, 0))
      return js_mkerr(js, "oom");

    ant_value_t length_result = js_define_function_length(js, bound_func, bound_length);
    if (is_err(length_result)) return length_result;
    
    ant_value_t name_result = js_set_function_name_prefixed(
      js, bound_func, "bound ", 6, 
      target_name, target_name_len
    );
    
    if (is_err(name_result)) return name_result;
    js_mark_constructor(bound_func, js_is_constructor(func));

    return bound;
  }

  if (func_type == kTypeBuiltin) {
    ant_value_t bound_func = mkobj(js, 0);
    if (is_err(bound_func)) return bound_func;
    
    set_slot(bound_func, SLOT_CFUNC, func);
    set_slot(bound_func, SLOT_TARGET_FUNC, func);
    
    ant_value_t func_proto = get_slot(js_glob(js), SLOT_FUNC_PROTO);
    if (vtype(func_proto) == kTypeFunction) js_set_proto_init(bound_func, func_proto);

    uint8_t bind_flags = SV_CALL_HAS_BOUND_THIS;
    if (js_as_cfunc(func) == builtin_function_call) bind_flags |= SV_CALL_IS_UNCURRY;
    if (bound_argc > 0) bind_flags |= SV_CALL_HAS_BOUND_ARGS;
    
    ant_value_t bound = js_obj_to_func_ex(js, bound_func, bind_flags);
    if (is_err(bound)) return bound;
    
    sv_closure_t *bc = js_func_closure(bound);
    bc->bound_this = this_arg;
    
    if (
      bound_argc > 0 && 
      !bound_argv_copy(bc, bound_args, bound_argc, NULL, 0)
    ) return js_mkerr(js, "oom");
    
    ant_value_t length_result = js_define_function_length(js, bound_func, bound_length);
    if (is_err(length_result)) return length_result;
    
    ant_value_t name_result = js_set_function_name_prefixed(
      js, bound_func, "bound ", 6, 
      target_name, target_name_len
    );
    
    if (is_err(name_result)) return name_result;
    js_mark_constructor(bound_func, js_is_constructor(func));
    
    return bound;
  }

  ant_value_t func_obj = js_func_obj(func);
  ant_value_t bound_func = mkobj(js, 0);
  if (is_err(bound_func)) return bound_func;

  sv_closure_t *orig = js_func_closure(func);
  sv_closure_t *bound_closure = js_closure_alloc(js);
  if (!bound_closure) return js_mkerr(js, "oom");
  
  bound_closure->func = orig->func;
  bound_closure->call_flags = orig->call_flags;
  bound_closure->upvalues = NULL;
  
  bound_closure->bound_this = 
    (orig->call_flags & (SV_CALL_IS_ARROW | SV_CALL_HAS_BOUND_THIS)) 
    ? orig->bound_this : this_arg;
  
  bound_closure->call_flags |= SV_CALL_HAS_BOUND_THIS;
  bound_closure->super_val = orig->super_val;
  bound_closure->func_obj = bound_func;
  
  if (bound_closure->call_flags & SV_CALL_HAS_BOUND_ARGS) {
    bound_closure->u.bound.argv = NULL;
    bound_closure->u.bound.args_arr = js_mkundef();
  } else {
    bound_closure->u.pending.name = NULL;
    bound_closure->u.pending.len = 0;
  }

  if (orig->func && orig->func->upvalue_count > 0 && orig->upvalues) {
    size_t upvalue_bytes = sizeof(sv_upvalue_t *) * (size_t)orig->func->upvalue_count;
    bound_closure->upvalues = orig->func->upvalue_count <= SV_CLOSURE_INLINE_UPVALS
      ? bound_closure->inline_upvals
      : malloc(upvalue_bytes);
    if (!bound_closure->upvalues) return js_mkerr(js, "oom");
    memcpy(bound_closure->upvalues, orig->upvalues, upvalue_bytes);
  }
  
  int orig_bound_argc = (orig->call_flags & SV_CALL_HAS_BOUND_ARGS) ? orig->bound_argc : 0;
  if (bound_argc > 0 || orig_bound_argc > 0) {
    bound_closure->call_flags |= SV_CALL_HAS_BOUND_ARGS;
    bound_closure->u.bound.argv = NULL;
    bound_closure->u.bound.args_arr = js_mkundef();
  }

  ant_value_t async_slot = get_slot(func_obj, SLOT_ASYNC);
  if (vtype(async_slot) == kTypeBool && vdata(async_slot) == 1) {
    set_slot(bound_func, SLOT_ASYNC, js_true);
  }

  ant_value_t target_proto = get_proto(js, func);
  if (is_object_type(target_proto) || vtype(target_proto) == kTypeNull) {
    js_set_proto_init(bound_func, target_proto);
  } else if (vtype(async_slot) == kTypeBool && vdata(async_slot) == 1) {
    ant_value_t async_proto = get_slot(js_glob(js), SLOT_ASYNC_PROTO);
    if (is_object_type(async_proto)) js_set_proto_init(bound_func, async_proto);
  } else {
    ant_value_t func_proto = get_slot(js_glob(js), SLOT_FUNC_PROTO);
    if (vtype(func_proto) == kTypeFunction) js_set_proto_init(bound_func, func_proto);
  }

  ant_value_t data_slot = get_slot(func_obj, SLOT_DATA);
  if (vtype(data_slot) != kTypeUndefined) {
    set_slot(bound_func, SLOT_DATA, data_slot);
  }

  ant_value_t eval_env = get_slot(func_obj, SLOT_EVAL_ENV);
  if (is_object_type(eval_env))
    js_set_slot_wb(js, bound_func, SLOT_EVAL_ENV, eval_env);

  ant_value_t cfunc_slot = get_slot(func_obj, SLOT_CFUNC);
  ant_value_t call_target = js_resolve_bound_target(func);
  set_slot(bound_func, SLOT_TARGET_FUNC, call_target);
  
  if (bound_argc > 0 || orig_bound_argc > 0) {
    ant_value_t bound_arr = mkarr(js);
    for (int i = 0; i < orig_bound_argc; i++)
      arr_set(js, bound_arr, (ant_offset_t)i, orig->u.bound.argv[i]);
    for (int i = 0; i < bound_argc; i++)
      arr_set(js, bound_arr, (ant_offset_t)(orig_bound_argc + i), bound_args[i]);
    bound_closure->u.bound.args_arr = bound_arr;
    if (!bound_argv_copy(
      bound_closure,
      orig_bound_argc > 0 ? orig->u.bound.argv : NULL, orig_bound_argc,
      bound_args, bound_argc
    )) return js_mkerr(js, "oom");
  }

  if (vtype(cfunc_slot) == kTypeBuiltin) {
    set_slot(bound_func, SLOT_CFUNC, cfunc_slot);
  }
  
  ant_value_t length_result = js_define_function_length(js, bound_func, bound_length);
  if (is_err(length_result)) return length_result;
  
  ant_value_t name_result = js_set_function_name_prefixed(
    js, bound_func, "bound ", 6, 
    target_name, target_name_len
  );
  
  if (is_err(name_result)) return name_result;
  ant_value_t bound = mkref(kTypeFunction, bound_closure);
  js_mark_constructor(bound_func, js_is_constructor(func));
  
  return bound;
}

static ant_value_t builtin_Array(ant_params_t) {
  ant_value_t arr = mkarr(js);
  if (is_err(arr)) return arr;
  
  if (nargs == 1 && vtype(args[0]) == kTypeNumber) {
    ant_value_t err = validate_array_length(js, args[0]);
    if (is_err(err)) return err;
    ant_offset_t new_len = (ant_offset_t)tod(args[0]);
    ant_offset_t doff = get_dense_buf(arr);
    if (doff && new_len <= MAX_DENSE_PREALLOC_LEN) 
      if (new_len > dense_capacity(doff)) doff = dense_grow(js, arr, new_len);
    if (new_len > 0) array_mark_may_have_holes(arr);
    array_len_set(js, arr, new_len);
  } else if (nargs > 0) for (int i = 0; i < nargs; i++) 
    arr_set(js, arr, (ant_offset_t)i, args[i]);

  ant_value_t instance_proto = js_instance_proto_from_new_target(js, js->sym.array_proto, call_new_target);
  
  if (is_object_type(instance_proto)) js_set_proto_init(arr, instance_proto);
  if (vtype(call_new_target) == kTypeFunction || vtype(call_new_target) == kTypeBuiltin) {
    set_slot(arr, SLOT_CTOR, call_new_target);
  }
  
  return arr;
}

static ant_value_t builtin_error_captureStackTrace(ant_params_t) {
  if (nargs < 1 || !is_object_type(args[0])) {
    return js_mkerr(js, "argument must be an object");
  }

  ant_value_t target = args[0];
  ant_value_t error_ctor = lkp_val(js, js_glob(js), "Error", 5);
  ant_value_t prep = js_mkundef();

  if (vtype(error_ctor) == kTypeFunction || vtype(error_ctor) == kTypeBuiltin) {
    prep = lkp_val(js, js_func_obj(error_ctor), "prepareStackTrace", 17);
  }

  if (vtype(prep) == kTypeFunction || vtype(prep) == kTypeBuiltin) {
    ant_value_t callsites = js_build_callsite_array(js);
    ant_value_t prep_args[2] = { target, callsites };
    ant_value_t result = sv_vm_call(js->vm, js, prep, js_mkundef(), prep_args, 2, NULL, js_mkundef());
    if (Ant_Exception_Pending(js)) return js_mkundef();
    js_error_define_stack(js, target, js_mkundef(), result);
  } else js_capture_stack(js, target);

  return js_mkundef();
}

static ant_value_t builtin_error_isError(ant_params_t) {
  if (nargs < 1) return js_false;
  ant_value_t val = args[0];
  if (!is_object_type(val)) return js_false;
  return get_slot(val, SLOT_ERROR_BRAND) == js_true ? js_true : js_false;
}

static ant_value_t error_install_message_cause(ant_t *js, ant_value_t err, ant_value_t message, ant_value_t options) {
  if (vtype(message) != kTypeUndefined) {
    ant_value_t msg = js_template_to_string(js, message);
    if (is_err(msg)) return msg;
    mkprop_bytes(js, err, "message", 7, msg, JS_DESC_W | JS_DESC_C);
  }
  
  if (is_object_type(options) && lkp_proto(js, options, "cause", 5).obj) {
    ant_value_t cause = js_get(js, options, "cause");
    if (is_err(cause)) return cause;
    mkprop_bytes(js, err, "cause", 5, cause, JS_DESC_W | JS_DESC_C);
  }
  
  return js_mkundef();
}

static ant_value_t builtin_Error(ant_params_t) {
  bool is_new = (vtype(call_new_target) != kTypeUndefined);
  ant_value_t this_val = js->this_val;
  
  if (!is_new) {
    this_val = Ant_Object_CreateBare(js);
    ant_value_t proto = lkp_interned_val(js, js_func_obj(js->current_func), js->intern.prototype);
    if (vtype(proto) != kTypeUndefined) js_set_proto_init(this_val, proto);
    else js_set_proto_init(this_val, js->builtins.error_protos[JS_ERR_GENERIC]);
  }
  
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, this_val);
  js_capture_stack(js, this_val);
  
  ant_value_t installed = error_install_message_cause(
    js, this_val, nargs > 0 ? args[0] : js_mkundef(), 
    nargs > 1 ? args[1] : js_mkundef()
  );
  
  set_slot(this_val, SLOT_ERROR_BRAND, js_true);
  GC_ROOT_RESTORE(js, mark);
  
  return is_err(installed) ? installed : this_val;
}

ant_value_t Ant_Error_CreateConstructorEx(ant_t *js, const char *name, ant_value_t cfunc, ant_value_t *out_proto) {
  size_t name_len = strlen(name);
  ant_value_t error_ctor = js->builtins.error_ctor;
  
  ant_value_t proto = Ant_Object_CreateBare(js);
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, proto);
  set_proto(js, proto, js->builtins.error_protos[JS_ERR_GENERIC]);
  mkprop_bytes(js, proto, "name", 4, js_mkstr(js, name, name_len), JS_DESC_W | JS_DESC_C);
  mkprop_bytes(js, proto, "message", 7, js_mkstr(js, "", 0), JS_DESC_W | JS_DESC_C);
  
  ant_value_t ctor = mkobj(js, 0);
  GC_ROOT_PIN(js, ctor);
  set_proto(js, ctor, error_ctor);
  set_slot(ctor, SLOT_CFUNC, cfunc);
  js_setprop_readonly_nonconfigurable(js, ctor, "prototype", 9, proto);
  js_set_function_name(js, ctor, name, name_len);
  
  ant_value_t ctor_func = js_obj_to_func(js, ctor);
  defhidden(js, proto, "constructor", 11, ctor_func);
  GC_ROOT_RESTORE(js, mark);
  
  if (out_proto) *out_proto = proto;
  return ctor_func;
}

ant_value_t Ant_Error_CreateConstructor(ant_t *js, const char *name, ant_value_t *out_proto) {
  return Ant_Error_CreateConstructorEx(js, name, Ant_Function_CreateWithArity(builtin_Error, 1), out_proto);
}

static ant_value_t builtin_Error_toString(ant_params_t) {
  ant_value_t this_val = js_getthis(js);
  
  ant_value_t name = js_getprop_fallback_len(js, this_val, "name", 4);
  if (vtype(name) == kTypeUndefined) name = js_mkstr(js, "Error", 5);
  else if (vtype(name) != kTypeString) {
    const char *s = js_str(js, name);
    name = js_mkstr(js, s, strlen(s));
  }
  name = js_error_display_name(js, this_val, name);
  if (is_err(name)) return name;
  
  ant_value_t msg = js_getprop_fallback_len(js, this_val, "message", 7);
  if (vtype(msg) == kTypeUndefined) msg = js_mkstr(js, "", 0);
  else if (vtype(msg) != kTypeString) {
    const char *s = js_str(js, msg);
    msg = js_mkstr(js, s, strlen(s));
  }
  
  ant_offset_t name_len, msg_len;
  ant_offset_t name_off = vstr(js, name, &name_len);
  ant_offset_t msg_off = vstr(js, msg, &msg_len);
  
  const char *name_str = (const char *)(uintptr_t)(name_off);
  const char *msg_str = (const char *)(uintptr_t)(msg_off);
  
  if (name_len == 0) return msg;
  if (msg_len == 0) return name;

  size_t total = (size_t)(name_len + 2 + msg_len);
  char *buf = malloc(total + 1);
  if (!buf) return js_mkerr(js, "out of memory");

  memcpy(buf, name_str, (size_t)name_len);
  buf[name_len] = ':';
  buf[name_len + 1] = ' ';
  memcpy(buf + name_len + 2, msg_str, (size_t)msg_len);
  buf[total] = '\0';
  
  ant_value_t result = js_mkstr(js, buf, total);
  free(buf);
  return result;
}

static ant_value_t builtin_AggregateError(ant_params_t) {
  bool is_new = (vtype(call_new_target) != kTypeUndefined);
  ant_value_t this_val = js->this_val;
  
  if (!is_new) {
    this_val = Ant_Object_CreateBare(js);
    ant_prop_loc_t proto_off = lkp_interned(js_func_obj(js->current_func), js->intern.prototype);
    if (proto_off.obj) js_set_proto_init(this_val, js_prop_load(proto_off));
    else js_set_proto_init(this_val, js->builtins.error_protos[JS_ERR_AGGREGATE]);
  }
  
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, this_val);
  
  ant_value_t installed = error_install_message_cause(
    js, this_val, nargs > 1 ? args[1] : js_mkundef(), 
    nargs > 2 ? args[2] : js_mkundef()
  );
  
  if (is_err(installed)) { 
    GC_ROOT_RESTORE(js, mark); 
    return installed;
  }
  
  ant_value_t errors = Ant_Iterator_ToList(js, nargs > 0 ? args[0] : js_mkundef(), "AggregateError errors is not iterable");
  if (is_err(errors)) {
    GC_ROOT_RESTORE(js, mark);
    return errors;
  }
  
  mkprop_bytes(js, this_val, "errors", 6, errors, JS_DESC_W | JS_DESC_C);
  set_slot(this_val, SLOT_ERROR_BRAND, js_true);
  
  js_capture_stack(js, this_val);
  GC_ROOT_RESTORE(js, mark);

  return this_val;
}

static ant_value_t builtin_SuppressedError(ant_params_t) {
  bool is_new = (vtype(call_new_target) != kTypeUndefined);
  ant_value_t this_val = js->this_val;

  if (!is_new) {
    this_val = Ant_Object_CreateBare(js);
    ant_prop_loc_t proto_off = lkp_interned(js_func_obj(js->current_func), js->intern.prototype);
    if (proto_off.obj) js_set_proto_init(this_val, js_prop_load(proto_off));
    else js_set_proto_init(this_val, js->builtins.suppressed_error_proto);
  }

  ant_value_t error = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t suppressed = nargs > 1 ? args[1] : js_mkundef();
  
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, this_val);
  
  ant_value_t installed = error_install_message_cause(js, this_val, nargs > 2 ? args[2] : js_mkundef(), js_mkundef());
  
  GC_ROOT_RESTORE(js, mark);
  if (is_err(installed)) return installed;

  mkprop_bytes(js, this_val, "error", 5, error, JS_DESC_W | JS_DESC_C);
  mkprop_bytes(js, this_val, "suppressed", 10, suppressed, JS_DESC_W | JS_DESC_C);

  set_slot(this_val, SLOT_ERROR_BRAND, js_true);
  js_capture_stack(js, this_val);

  return this_val;
}

static ant_value_t disposable_stack_init(ant_t *js, ant_value_t obj, int brand, ant_value_t proto) {
  if (vtype(obj) != kTypeObject) obj = Ant_Object_CreateBare(js);
  if (is_err(obj)) return obj;
  if (is_object_type(proto)) js_set_proto_init(obj, proto);

  ant_value_t entries = js_mkarr(js);
  if (is_err(entries)) return entries;
  set_slot(obj, SLOT_BRAND, js_mknum((double)brand));
  set_slot(obj, SLOT_ENTRIES, entries);
  set_slot(obj, SLOT_SETTLED, js_false);
  
  return obj;
}

static ant_value_t builtin_DisposableStack(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "DisposableStack constructor requires 'new'");
  }

  ant_value_t instance_proto = js_instance_proto_from_new_target(js, js->builtins.disposable_stack_proto, call_new_target);
  
  return disposable_stack_init(js, js->this_val, BRAND_DISPOSABLE_STACK, instance_proto);
}

static ant_value_t builtin_AsyncDisposableStack(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "AsyncDisposableStack constructor requires 'new'");
  }

  ant_value_t instance_proto = js_instance_proto_from_new_target(js, js->builtins.async_disposable_stack_proto, call_new_target);
  
  return disposable_stack_init(js, js->this_val, BRAND_ASYNC_DISPOSABLE_STACK, instance_proto);
}

static bool disposable_stack_has_brand(ant_value_t obj, int brand) {
  if (!is_object_type(obj)) return false;
  ant_value_t actual = get_slot(js_as_obj(obj), SLOT_BRAND);
  return vtype(actual) == kTypeNumber && (int)js_getnum(actual) == brand;
}

static ant_value_t disposable_stack_entries_checked(ant_t *js, ant_value_t stack, int brand, const char *name) {
  if (!disposable_stack_has_brand(stack, brand)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s method called on incompatible receiver", name);
  }

  if (get_slot(js_as_obj(stack), SLOT_SETTLED) == js_true) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s is already disposed", name);
  }

  ant_value_t entries = get_slot(js_as_obj(stack), SLOT_ENTRIES);
  if (vtype(entries) != kTypeArray) return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid %s", name);
  return entries;
}

static ant_value_t disposable_stack_push_record(
  ant_t *js, ant_value_t stack, int brand, const char *name,
  sv_disposal_record_kind_t kind, ant_value_t value, ant_value_t method
) {
  if (!is_callable(method)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s requires a callable disposer", name);
  }

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, stack);
  GC_ROOT_PIN(js, value);
  GC_ROOT_PIN(js, method);

  ant_value_t entries = disposable_stack_entries_checked(js, stack, brand, name);
  GC_ROOT_PIN(js, entries);
  if (is_err(entries)) {
    GC_ROOT_RESTORE(js, root_mark);
    return entries;
  }

  ant_value_t record = js_mkarr(js);
  GC_ROOT_PIN(js, record);
  if (is_err(record)) {
    GC_ROOT_RESTORE(js, root_mark);
    return record;
  }

  js_arr_push(js, record, js_mknum((double)kind));
  js_arr_push(js, record, value);
  js_arr_push(js, record, method);
  js_arr_push(js, entries, record);

  GC_ROOT_RESTORE(js, root_mark);
  return js_mkundef();
}

static ant_value_t builtin_DisposableStack_defer(ant_params_t) {
  ant_value_t fn = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t result = disposable_stack_push_record(
    js, js->this_val, BRAND_DISPOSABLE_STACK, 
    "DisposableStack", SV_DISPOSAL_RECORD_DEFER, js_mkundef(), fn
  );
  return is_err(result) ? result : js_mkundef();
}

static ant_value_t builtin_DisposableStack_adopt(ant_params_t) {
  ant_value_t value = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t fn = nargs > 1 ? args[1] : js_mkundef();
  ant_value_t result = disposable_stack_push_record(
    js, js->this_val, BRAND_DISPOSABLE_STACK, 
    "DisposableStack", SV_DISPOSAL_RECORD_ADOPT, value, fn
  );
  return is_err(result) ? result : value;
}

static ant_value_t builtin_AsyncDisposableStack_defer(ant_params_t) {
  ant_value_t fn = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t result = disposable_stack_push_record(
    js, js->this_val, BRAND_ASYNC_DISPOSABLE_STACK, 
    "AsyncDisposableStack", SV_DISPOSAL_RECORD_DEFER, js_mkundef(), fn
  );
  return is_err(result) ? result : js_mkundef();
}

static ant_value_t builtin_AsyncDisposableStack_adopt(ant_params_t) {
  ant_value_t value = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t fn = nargs > 1 ? args[1] : js_mkundef();
  ant_value_t result = disposable_stack_push_record(
    js, js->this_val, BRAND_ASYNC_DISPOSABLE_STACK, 
    "AsyncDisposableStack", SV_DISPOSAL_RECORD_ADOPT, value, fn
  );
  return is_err(result) ? result : value;
}

static ant_value_t disposable_stack_use(
  ant_t *js, ant_value_t stack, int brand, const char *name,
  ant_value_t resource, ant_value_t dispose_sym, ant_value_t fallback_sym
) {
  ant_value_t entries = disposable_stack_entries_checked(js, stack, brand, name);
  if (is_err(entries)) return entries;

  if (vtype(resource) == kTypeNull || vtype(resource) == kTypeUndefined) return resource;

  ant_value_t method = js_get_sym(js, resource, dispose_sym);
  if (is_err(method)) return method;

  if ((vtype(method) == kTypeUndefined || vtype(method) == kTypeNull) && vtype(fallback_sym) == kTypeSymbol) {
    method = js_get_sym(js, resource, fallback_sym);
    if (is_err(method)) return method;
  }

  if (!is_callable(method))
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s resource is not disposable", name);

  ant_value_t result = disposable_stack_push_record(
    js, stack, brand, name,
    SV_DISPOSAL_RECORD_USE, resource, method
  );

  return is_err(result) ? result : resource;
}

static ant_value_t builtin_DisposableStack_use(ant_params_t) {
  ant_value_t resource = nargs > 0 ? args[0] : js_mkundef();
  return disposable_stack_use(
    js, js->this_val, BRAND_DISPOSABLE_STACK, "DisposableStack", resource, js->sym.dispose_sym, js_mkundef()
  );
}

static ant_value_t builtin_AsyncDisposableStack_use(ant_params_t) {
  ant_value_t resource = nargs > 0 ? args[0] : js_mkundef();
  return disposable_stack_use(
    js, js->this_val, BRAND_ASYNC_DISPOSABLE_STACK, "AsyncDisposableStack",
    resource, js->sym.asyncDispose_sym, js->sym.dispose_sym
  );
}

static ant_value_t disposable_stack_move(ant_t *js, int brand, const char *name, ant_value_t proto) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t stack = js->this_val;
  GC_ROOT_PIN(js, stack);

  ant_value_t entries = disposable_stack_entries_checked(js, stack, brand, name);
  GC_ROOT_PIN(js, entries);
  if (is_err(entries)) {
    GC_ROOT_RESTORE(js, root_mark);
    return entries;
  }

  ant_value_t moved = disposable_stack_init(js, js_mkundef(), brand, proto);
  GC_ROOT_PIN(js, moved);
  if (is_err(moved)) {
    GC_ROOT_RESTORE(js, root_mark);
    return moved;
  }

  set_slot(moved, SLOT_ENTRIES, entries);
  set_slot(js_as_obj(stack), SLOT_ENTRIES, js_mkarr(js));
  set_slot(js_as_obj(stack), SLOT_SETTLED, js_true);

  GC_ROOT_RESTORE(js, root_mark);
  return moved;
}

static ant_value_t builtin_DisposableStack_move(ant_params_t) {
  (void)args; (void)nargs;
  return disposable_stack_move(js, BRAND_DISPOSABLE_STACK, "DisposableStack", js->builtins.disposable_stack_proto);
}

static ant_value_t builtin_AsyncDisposableStack_move(ant_params_t) {
  (void)args; (void)nargs;
  return disposable_stack_move(js, BRAND_ASYNC_DISPOSABLE_STACK, "AsyncDisposableStack", js->builtins.async_disposable_stack_proto);
}

static ant_value_t builtin_DisposableStack_dispose(ant_params_t) {
  ant_value_t stack = js->this_val;
  if (!disposable_stack_has_brand(stack, BRAND_DISPOSABLE_STACK)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "DisposableStack method called on incompatible receiver");
  }
  if (get_slot(js_as_obj(stack), SLOT_SETTLED) == js_true) return js_mkundef();

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, stack);
  ant_value_t entries = get_slot(js_as_obj(stack), SLOT_ENTRIES);
  
  GC_ROOT_PIN(js, entries);
  ant_value_t completion = js_mkundef();
  
  GC_ROOT_PIN(js, completion);
  set_slot(js_as_obj(stack), SLOT_SETTLED, js_true);

  ant_offset_t len = vtype(entries) == kTypeArray ? js_arr_len(js, entries) : 0;
  bool has_completion = false;
  if (len > 0) completion = sv_dispose_records_sync(js, entries, len, &completion, &has_completion, false);

  if (is_err(completion)) {
    GC_ROOT_RESTORE(js, root_mark);
    return completion;
  }

  set_slot(js_as_obj(stack), SLOT_ENTRIES, js_mkarr(js));
  if (has_completion) {
    ant_value_t thrown = js_throw(js, completion);
    GC_ROOT_RESTORE(js, root_mark);
    return thrown;
  }

  GC_ROOT_RESTORE(js, root_mark);
  return js_mkundef();
}

static ant_value_t builtin_AsyncDisposableStack_disposeAsync(ant_params_t) {
  ant_value_t stack = js->this_val;
  ant_value_t result_promise = js_mkpromise(js);
  if (is_err(result_promise)) return result_promise;

  if (!disposable_stack_has_brand(stack, BRAND_ASYNC_DISPOSABLE_STACK)) {
    ant_value_t error = Ant_Error_Create(
      js, JS_ERR_TYPE, "AsyncDisposableStack method called on incompatible receiver"
    );
    js_reject_promise(js, result_promise, error);
    return result_promise;
  }
  
  if (get_slot(js_as_obj(stack), SLOT_SETTLED) == js_true) {
    js_resolve_promise(js, result_promise, js_mkundef());
    return result_promise;
  }

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, stack);
  GC_ROOT_PIN(js, result_promise);

  ant_value_t entries = get_slot(js_as_obj(stack), SLOT_ENTRIES);
  GC_ROOT_PIN(js, entries);
  set_slot(js_as_obj(stack), SLOT_SETTLED, js_true);

  ant_value_t state = Ant_Object_CreateBare(js);
  GC_ROOT_PIN(js, state);
  if (is_err(state)) {
    GC_ROOT_RESTORE(js, root_mark);
    return state;
  }
  
  set_slot(state, SLOT_ENTRIES, entries);
  set_slot(state, SLOT_DATA, result_promise);
  set_slot(state, SLOT_AUX, js_mkundef());
  set_slot(state, SLOT_ITER_STATE, js_mknum((double)(vtype(entries) == kTypeArray ? js_arr_len(js, entries) : 0)));
  set_slot(js_as_obj(stack), SLOT_ENTRIES, js_mkarr(js));

  ant_value_t result = sv_async_dispose_continue(js, state, false, js_mkundef());
  GC_ROOT_RESTORE(js, root_mark);
  
  return result;
}

static ant_value_t builtin_object_is(ant_params_t) {
  if (nargs < 2) return js_false;
  return same_value_values(js, args[0], args[1]) ? js_true : js_false;
}

enum obj_enum_mode { 
  OBJ_ENUM_KEYS,
  OBJ_ENUM_VALUES,
  OBJ_ENUM_ENTRIES
};

static ant_value_t map_to_entry(ant_t *js, ant_value_t key, ant_value_t val) {
  ant_value_t pair[2] = { key, val };
  return mkarr_dense_literal_capacity(js, pair, 2, true);
}

typedef struct {
  uint32_t slot;
  unsigned long index;
} own_index_key_t;

typedef struct {
  own_index_key_t *items;
  uint32_t count;
  uint32_t cap;
  own_index_key_t stack[32];
} own_index_list_t;

static inline void own_index_list_init(own_index_list_t *list) {
  list->items = list->stack;
  list->count = 0;
  list->cap = (uint32_t)(sizeof(list->stack) / sizeof(list->stack[0]));
}

static inline void own_index_list_free(own_index_list_t *list) {
  if (list->items != list->stack) free(list->items);
}

static bool own_index_list_push(own_index_list_t *list, uint32_t slot, unsigned long index) {
  if (list->count == list->cap) {
    uint32_t cap = list->cap * 2;
    bool on_stack = list->items == list->stack;
    own_index_key_t *next = on_stack ? malloc(cap * sizeof(*next)) : realloc(list->items, cap * sizeof(*next));
    if (!next) return false;
    if (on_stack) memcpy(next, list->stack, list->count * sizeof(*next));
    list->items = next;
    list->cap = cap;
  }
  
  list->items[list->count++] = (own_index_key_t){ .slot = slot, .index = index };
  return true;
}

static int own_index_key_cmp(const void *a, const void *b) {
  unsigned long x = ((const own_index_key_t *)a)->index;
  unsigned long y = ((const own_index_key_t *)b)->index;
  return (x > y) - (x < y);
}

static void own_index_keys_sort(own_index_key_t *keys, uint32_t count) {
  if (count <= 16) {
    for (uint32_t i = 1; i < count; i++) {
      own_index_key_t key = keys[i];
      uint32_t pos = i;
      for (; pos > 0 && keys[pos - 1].index > key.index; pos--) keys[pos] = keys[pos - 1];
      keys[pos] = key;
    }
    return;
  }
  
  own_index_key_t *tmp = malloc(count * sizeof(*tmp));
  if (!tmp) { qsort(keys, count, sizeof(*keys), own_index_key_cmp); return; }
  
  own_index_key_t *src = keys, *dst = tmp;
  for (unsigned shift = 0; shift < 32; shift += 8) {
    uint32_t offsets[257] = {0};
    for (uint32_t i = 0; i < count; i++) offsets[((src[i].index >> shift) & 0xff) + 1]++;
    if (offsets[((src[0].index >> shift) & 0xff) + 1] == count) continue;
    
    for (unsigned d = 1; d < 257; d++) offsets[d] += offsets[d - 1];
    for (uint32_t i = 0; i < count; i++) dst[offsets[(src[i].index >> shift) & 0xff]++] = src[i];
    
    own_index_key_t *swap = src; src = dst; dst = swap;
  }
  
  if (src != keys) memcpy(keys, src, count * sizeof(*keys));
  free(tmp);
}

static bool own_slot_list_push(uint32_t **slots, uint32_t *count, uint32_t *cap, uint32_t *stack, uint32_t slot) {
  if (*count == *cap) {
    uint32_t next_cap = *cap * 2;
    uint32_t *next = NULL;
    if (*slots == stack) {
      next = calloc(next_cap, sizeof(*next));
      if (next) memcpy(next, *slots, *count * sizeof(*next));
    } else {
      next = realloc(*slots, next_cap * sizeof(*next));
    }
    if (!next) return false;
    *slots = next;
    *cap = next_cap;
  }
  (*slots)[(*count)++] = slot;
  return true;
}

static bool own_key_is_array_index(const char *key, size_t key_len, unsigned long *out_idx) {
  if (key_len == 0) return false;
  if (key[0] < '0' || key[0] > '9') return false;
  if (key_len > 1 && key[0] == '0') return false;
  if (key_len == 1) {
    if (out_idx) *out_idx = (unsigned long)(key[0] - '0');
    return true;
  }
  if (key_len > 10) return false;

  unsigned long parsed = (unsigned long)(key[0] - '0');
  for (size_t i = 1; i < key_len; i++) {
    if (key[i] < '0' || key[i] > '9') return false;
    unsigned digit = (unsigned)(key[i] - '0');
    if (parsed > ((unsigned long)UINT32_MAX - digit) / 10UL) return false;
    parsed = parsed * 10UL + digit;
  }
  if (parsed >= (unsigned long)UINT32_MAX) return false;
  if (out_idx) *out_idx = parsed;
  return true;
}

static bool arr_fill_numeric_typed(ant_t *js, ant_value_t arr, const TypedArrayData *ta) {
  if (ta->type == TYPED_ARRAY_BIGINT64 || ta->type == TYPED_ARRAY_BIGUINT64) return false;
  
  ant_object_t *dst = array_obj_ptr(arr);
  
  if (
    !dst || !dst->flags.fast_array || !dst->u.array.data || 
    dst->u.array.len || dst->u.array.cap < ta->length
  ) return false;
  
  for (size_t i = 0; i < ta->length; i++) 
    buffer_typedarray_data_read_index(js, ta, i, &dst->u.array.data[i]);
  
  dst->u.array.len = (uint32_t)ta->length;
  dst->flags.may_have_dense_elements = ta->length > 0;
  
  return true;
}

static bool arr_fill_dense_values(ant_value_t arr, ant_offset_t doff, ant_offset_t len) {
  ant_object_t *dst = array_obj_ptr(arr);
  ant_object_t *from = dense_obj(doff);
  
  if (
    !dst || !from || !dst->flags.fast_array || !dst->u.array.data || 
    dst->u.array.len || dst->u.array.cap < len
  ) return false;
  
  uint32_t count = 0;
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t v = from->u.array.data[i];
    if (!is_empty_slot(v)) dst->u.array.data[count++] = v;
  }
  
  dst->u.array.len = count;
  dst->flags.may_have_dense_elements = count > 0;
  
  return true;
}

typedef enum {
  OBJECT_FILL_NONE,
  OBJECT_FILL_DONE,
  OBJECT_FILL_RESUME,
} object_fill_t;

static object_fill_t arr_fill_object_values(ant_t *js, ant_object_t *ptr, ant_value_t *out, uint32_t *resume) {
  ant_shape_t *shape = ptr->shape;
  uint32_t count = ant_shape_count(shape);
  if (count > ptr->prop_count) count = ptr->prop_count;
  
  ant_value_t arr = alloc_array_with_proto_capacity(js, js->sym.array_proto, count, 0, false);
  ant_object_t *dst = is_err(arr) ? NULL : array_obj_ptr(arr);
  if (!dst || !dst->flags.fast_array || !dst->u.array.data) return OBJECT_FILL_NONE;
  
  own_index_list_t indices;
  own_index_list_init(&indices);
  
  bool index_keys = ant_shape_may_have_index_keys(shape);
  ant_value_t *data = dst->u.array.data;
  uint32_t n = 0;
  
  object_fill_t result = OBJECT_FILL_DONE;
  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING || !(prop->attrs & ANT_PROP_ATTR_ENUMERABLE)) continue;
    
    if (prop->has_getter || prop->has_setter) {
      if (index_keys) goto fail;
      result = OBJECT_FILL_RESUME;
      *resume = i;
      break;
    }
    
    unsigned long index = 0;
    const char *key = prop->key.interned;
    
    if (!index_keys || !own_key_is_array_index(key, intern_length(key), &index)) data[n++] = ant_object_prop_get_unchecked(ptr, i);
    else if (!own_index_list_push(&indices, i, index)) goto fail;
  }
  
  if (indices.count) {
    own_index_keys_sort(indices.items, indices.count);
    memmove(data + indices.count, data, n * sizeof(*data));
    for (uint32_t k = 0; k < indices.count; k++) data[k] = ant_object_prop_get_unchecked(ptr, indices.items[k].slot);
    n += indices.count;
  }
  
  own_index_list_free(&indices);
  dst->u.array.len = n;
  dst->flags.may_have_dense_elements = n > 0;
  *out = arr;
  return result;
  
fail:
  own_index_list_free(&indices);
  return OBJECT_FILL_NONE;
}

static inline ant_value_t object_enum_item(ant_t *js, enum obj_enum_mode mode, const char *key, size_t key_len, ant_value_t val) {
  if (mode == OBJ_ENUM_VALUES) return val;
  ant_value_t key_val = js_mkstr(js, key, key_len);
  return mode == OBJ_ENUM_KEYS ? key_val : map_to_entry(js, key_val, val);
}

static constexpr uint32_t OBJECT_ENUM_SINK_STACK = 32;

typedef struct {
  ant_value_t items[OBJECT_ENUM_SINK_STACK];
  ant_value_t arr;
  ant_value_t error;
  uint32_t count;
  uint32_t hint;
  bool spilled;
} object_enum_sink_t;

static void object_enum_spill(ant_t *js, object_enum_sink_t *sink) {
  uint32_t cap = sink->hint > sink->count ? sink->hint : sink->count + 1;
  ant_value_t arr = alloc_array_with_proto_capacity(js, js->sym.array_proto, cap, sink->count, false);
  if (is_err(arr)) { sink->error = arr; return; }
  
  ant_object_t *dst = array_obj_ptr(arr);
  if (dst && dst->flags.fast_array && dst->u.array.data) {
    memcpy(dst->u.array.data, sink->items, sink->count * sizeof(*sink->items));
    dst->u.array.len = sink->count;
    dst->flags.may_have_dense_elements = sink->count > 0;
  } else for (uint32_t i = 0; i < sink->count; i++) js_arr_push(js, arr, sink->items[i]);
  
  sink->arr = arr;
  sink->spilled = true;
}

static __attribute__((noinline)) void object_enum_emit_spilled(ant_t *js, object_enum_sink_t *sink, ant_value_t val) {
  if (!sink->spilled) object_enum_spill(js, sink);
  if (is_err(sink->error)) return;
  
  ant_object_t *dst = array_obj_ptr(sink->arr);
  if (!dst || !dst->flags.fast_array || !dst->u.array.data || dst->u.array.len >= dst->u.array.cap) {
    js_arr_push(js, sink->arr, val);
    return;
  }
  
  uint32_t idx = dst->u.array.len++;
  dst->u.array.data[idx] = val;
  dst->flags.may_have_dense_elements = 1;
  gc_write_barrier_elem(js, dst, idx, val);
}

static inline __attribute__((always_inline)) void object_enum_emit(ant_t *js, object_enum_sink_t *sink, ant_value_t val) {
  if (__builtin_expect(!sink->spilled && sink->count < OBJECT_ENUM_SINK_STACK, 1)) sink->items[sink->count++] = val;
  else object_enum_emit_spilled(js, sink, val);
}

static ant_value_t object_enum_collect(ant_t *js, ant_value_t obj, enum obj_enum_mode mode);
static ant_value_t object_enum_props(ant_t *js, ant_value_t obj, ant_object_t *ptr, enum obj_enum_mode mode, object_enum_sink_t *sink);
static ant_value_t object_enum_props_from(ant_t *js, ant_value_t obj, ant_object_t *ptr, enum obj_enum_mode mode, object_enum_sink_t *sink, uint32_t start);

static ant_value_t object_enum(ant_t *js, ant_value_t obj, enum obj_enum_mode mode) {
  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) != kTypeFunction) return mkarr(js);
    obj = promoted;
  }
  
  bool is_arr = (vtype(obj) == kTypeArray);
  if (vtype(obj) == kTypeFunction) obj = js_func_obj(obj);

  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || !ptr->shape) return mkarr(js);
  
  TypedArrayData *typed = ptr->flags.is_exotic ? buffer_get_typedarray_data(obj) : NULL;
  bool wrapper = !is_arr && !typed && vtype(js_string_wrapper_value(obj)) == kTypeString;
  if (wrapper || (is_arr && ant_shape_may_have_index_keys(ptr->shape))) return object_enum_collect(js, obj, mode);
  
  object_enum_sink_t sink;
  sink.error = js_mkundef();
  sink.count = 0;
  sink.spilled = false;
  
  if (mode == OBJ_ENUM_VALUES && !is_arr && !typed && !ptr->flags.is_exotic && !ptr->flags.arguments_object) {
    uint32_t resume = 0;
    object_fill_t fill = arr_fill_object_values(js, ptr, &sink.arr, &resume);
    if (fill == OBJECT_FILL_DONE) return sink.arr;
    
    if (fill == OBJECT_FILL_RESUME) {
      sink.spilled = true;
      ant_value_t result = object_enum_props_from(js, obj, ptr, mode, &sink, resume);
      if (is_err(result)) return result;
      return is_err(sink.error) ? sink.error : sink.arr;
    }
  }
  
  ant_offset_t doff = is_arr ? get_dense_buf(obj) : 0;
  bool typed_live = Ant_Buffer_ViewIsLive(typed);
  ant_offset_t elem_len = doff ? dense_iterable_length(js, obj) : typed_live ? (ant_offset_t)typed->length : 0;
  
  uint64_t want = (uint64_t)elem_len + ant_shape_count(ptr->shape);
  sink.hint = want > UINT32_MAX ? UINT32_MAX : (uint32_t)want;
  
  if (elem_len) {
    object_enum_spill(js, &sink);
    if (is_err(sink.error)) return sink.error;
  }
  
  bool filled = elem_len && mode == OBJ_ENUM_VALUES && (doff 
    ? arr_fill_dense_values(sink.arr, doff, elem_len) 
    : typed_live && arr_fill_numeric_typed(js, sink.arr, typed));
  
  for (ant_offset_t i = filled ? elem_len : 0; i < elem_len; i++) {
    ant_value_t v = js_mkundef();
    if (doff) v = dense_get(doff, i);
    else if (!buffer_typedarray_data_read_index(js, typed, (size_t)i, &v)) {
      if (is_err(v)) return v;
      break;
    }
    if (is_empty_slot(v)) continue;
    
    char idxstr[24];
    size_t idxlen = mode == OBJ_ENUM_VALUES ? 0 : uint_to_str(idxstr, sizeof(idxstr), (uint64_t)i);
    object_enum_emit(js, &sink, object_enum_item(js, mode, idxstr, idxlen, v));
  }

  ant_value_t result = object_enum_props(js, obj, ptr, mode, &sink);
  if (is_err(result)) return result;
  if (is_err(sink.error)) return sink.error;
  
  return sink.spilled ? sink.arr : mkarr_dense_literal_capacity(js, sink.items, sink.count, false);
}

static bool own_key_is_dense_shadow(ant_t *js, ant_value_t obj, const char *key, size_t key_len) {
  ant_offset_t doff = get_dense_buf(obj);
  if (!doff) return false;

  unsigned long idx = 0;
  if (!own_key_is_array_index(key, key_len, &idx)) return false;
  if (idx >= (unsigned long)dense_iterable_length(js, obj)) return false;
  return !is_empty_slot(dense_get(doff, (ant_offset_t)idx));
}

static bool own_key_is_enumerable(ant_t *js, ant_value_t obj, ant_object_t *ptr, uint32_t slot) {
  if (!ptr || !ptr->shape) return false;
  bool enumerable = (ant_shape_get_attrs(ptr->shape, slot) & ANT_PROP_ATTR_ENUMERABLE) != 0;
  const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, slot);
  if (!prop) return false;

  if (prop->type == ANT_SHAPE_KEY_SYMBOL) {
    prop_meta_t meta;
    if (lookup_symbol_prop_meta(js, obj, prop->key.sym_off, &meta)) enumerable = meta.enumerable;
    return enumerable;
  }

  if (ptr->flags.is_exotic) {
    descriptor_entry_t *desc = lookup_descriptor(js, js_as_obj(obj), prop->key.interned, intern_length(prop->key.interned));
    if (desc) enumerable = desc->enumerable;
  }
  return enumerable;
}

static inline ant_value_t js_object_view(ant_value_t v) {
  uint8_t t = vtype(v);
  if (t == kTypeGenerator || t == kTypePromise) return mkval(kTypeObject, vdata(v));
  return v;
}

static __attribute__((noinline)) void own_keys_push_string_indexes(
  ant_t *js, ant_value_t arr, ant_offset_t *out_idx, ant_offset_t len
) {
  for (ant_offset_t i = 0; i < len; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (uint64_t)i);
    arr_set(js, arr, (*out_idx)++, js_mkstr(js, idxstr, idxlen));
  }
}

ant_value_t js_own_property_keys(ant_t *js, ant_value_t obj, bool include_symbols, bool enumerable_only) {
  obj = js_object_view(obj);
  
  if (is_proxy(obj)) 
    return proxy_own_keys_filtered(js, obj, include_symbols, enumerable_only);
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, obj);

  ant_value_t arr = mkarr(js);
  GC_ROOT_PIN(js, arr);
  ant_offset_t out_idx = 0;

  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) == kTypeFunction) obj = promoted;
  }
  
  if (vtype(obj) == kTypeFunction) obj = js_func_obj(obj);
  if (vtype(obj) != kTypeObject && vtype(obj) != kTypeArray) goto done;

  ant_object_t *ptr = js_obj_ptr(obj);
  bool has_string_exotic = false;
  
  ant_offset_t string_exotic_len = 0;
  ant_value_t string_primitive = js_string_wrapper_value(obj);

  if (vtype(string_primitive) == kTypeString) {
    string_exotic_len = str_utf16_len(js, string_primitive);
    has_string_exotic = true;
    own_keys_push_string_indexes(js, arr, &out_idx, string_exotic_len);
    if (!enumerable_only) arr_set(js, arr, out_idx++, js->length_str);
  }

  TypedArrayData *typed = ptr && ptr->flags.is_exotic ? buffer_get_typedarray_data(obj) : NULL;
  if (Ant_Buffer_ViewIsLive(typed))
    own_keys_push_string_indexes(js, arr, &out_idx, (ant_offset_t)typed->length);

  if (!ptr || !ptr->shape) goto done;
  bool is_arr = (vtype(obj) == kTypeArray);
  uint32_t count = ant_shape_count(ptr->shape);

  if (!is_arr) {
    bool has_index_key = false;
    for (uint32_t i = 0; i < count; i++) {
      const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
      
      if (!prop || prop->type != ANT_SHAPE_KEY_STRING) continue;
      if (i >= ptr->prop_count) continue;
      if (enumerable_only && !own_key_is_enumerable(js, obj, ptr, i)) continue;

      const char *key = prop->key.interned;
      size_t key_len = intern_length(key);
      unsigned long string_idx = 0;
      
      if (
        has_string_exotic && (is_length_key(key, (ant_offset_t)key_len) || 
        parse_array_index(key, key_len, string_exotic_len, &string_idx))
      ) continue;
      
      if (own_key_is_array_index(key, key_len, NULL)) {
        has_index_key = true;
        break;
      }
      
      arr_set(js, arr, out_idx++, js_mkstr(js, key, key_len));
    }

    if (!has_index_key) {
      if (include_symbols) for (uint32_t i = 0; i < count; i++) {
        const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
        if (!prop || prop->type != ANT_SHAPE_KEY_SYMBOL) continue;
        if (i >= ptr->prop_count) continue;
        if (enumerable_only && !own_key_is_enumerable(js, obj, ptr, i)) continue;
        arr_set(js, arr, out_idx++, mkval(kTypeSymbol, prop->key.sym_off));
      }
      goto done;
    }

    arr = mkarr(js);
    GC_ROOT_PIN(js, arr);
    out_idx = 0;
    if (has_string_exotic) own_keys_push_string_indexes(js, arr, &out_idx, string_exotic_len);
  }

  if (is_arr) {
    ant_offset_t doff = get_dense_buf(obj);
    ant_offset_t dense_len = doff ? dense_iterable_length(js, obj) : 0;
    for (ant_offset_t i = 0; i < dense_len; i++) {
      if (is_empty_slot(dense_get(doff, i))) continue;
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
      arr_set(js, arr, out_idx++, js_mkstr(js, idxstr, idxlen));
    }
  }

  own_index_list_t indices;
  own_index_list_init(&indices);
  
  uint32_t stack_string_slots[64];
  uint32_t *string_slots = stack_string_slots;
  uint32_t string_count = 0;
  uint32_t string_cap = (uint32_t)(sizeof(stack_string_slots) / sizeof(stack_string_slots[0]));
  
  ant_offset_t dense_shadow_len = is_arr && get_dense_buf(obj) ? dense_iterable_length(js, obj) : 0;
  ant_offset_t dense_shadow_buf = is_arr ? get_dense_buf(obj) : 0;

  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING) continue;
    if (i >= ptr->prop_count) continue;
    if (enumerable_only && !own_key_is_enumerable(js, obj, ptr, i)) continue;

    const char *key = prop->key.interned;
    size_t key_len = intern_length(key);
    unsigned long string_idx = 0;
    
    if (
      has_string_exotic && (is_length_key(key, (ant_offset_t)key_len) || 
      parse_array_index(key, key_len, string_exotic_len, &string_idx))
    ) continue;
    
    unsigned long parsed = 0;
    if (!own_key_is_array_index(key, key_len, &parsed)) {
      if (!own_slot_list_push(&string_slots, &string_count, &string_cap, stack_string_slots, i)) {
        own_index_list_free(&indices);
        if (string_slots != stack_string_slots) free(string_slots);
        arr = js_mkerr(js, "oom");
        goto done;
      }
      continue;
    }

    if (
      dense_shadow_buf &&
      parsed < (unsigned long)dense_shadow_len &&
      !is_empty_slot(dense_get(dense_shadow_buf, (ant_offset_t)parsed))
    ) continue;

    if (!own_index_list_push(&indices, i, parsed)) {
      own_index_list_free(&indices);
      if (string_slots != stack_string_slots) free(string_slots);
      arr = js_mkerr(js, "oom");
      goto done;
    }
  }

  own_index_keys_sort(indices.items, indices.count);
  for (uint32_t i = 0; i < indices.count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, indices.items[i].slot);
    if (!prop) continue;
    const char *key = prop->key.interned;
    arr_set(js, arr, out_idx++, js_mkstr(js, key, intern_length(key)));
  }

  if (!enumerable_only && ((is_arr && !ptr->flags.arguments_object) || has_string_exotic)) {
    arr_set(js, arr, out_idx++, js->length_str);
  }

  for (uint32_t i = 0; i < string_count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, string_slots[i]);
    if (!prop) continue;
    const char *key = prop->key.interned;
    size_t key_len = intern_length(key);
    arr_set(js, arr, out_idx++, js_mkstr(js, key, key_len));
  }

  if (include_symbols) {
    for (uint32_t i = 0; i < count; i++) {
      const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
      if (!prop || prop->type != ANT_SHAPE_KEY_SYMBOL) continue;
      if (i >= ptr->prop_count) continue;
      if (enumerable_only && !own_key_is_enumerable(js, obj, ptr, i)) continue;
      arr_set(js, arr, out_idx++, mkval(kTypeSymbol, prop->key.sym_off));
    }
  }

  own_index_list_free(&indices);
  if (string_slots != stack_string_slots) free(string_slots);

done:
  GC_ROOT_RESTORE(js, root_mark);
  return arr;
}

static bool proxy_key_same(ant_t *js, ant_value_t a, ant_value_t b) {
  if (vtype(a) != vtype(b)) return false;
  if (vtype(a) == kTypeSymbol) return vdata(a) == vdata(b);
  if (vtype(a) != kTypeString) return false;

  ant_offset_t a_len = 0, b_len = 0;
  ant_offset_t a_off = vstr(js, a, &a_len);
  ant_offset_t b_off = vstr(js, b, &b_len);
  return a_len == b_len &&
    memcmp((const void *)(uintptr_t)a_off, (const void *)(uintptr_t)b_off, a_len) == 0;
}

static bool proxy_keys_include(ant_t *js, ant_value_t keys, ant_value_t key) {
  if (vtype(keys) != kTypeArray) return false;
  ant_offset_t len = get_array_length(js, keys);
  for (ant_offset_t i = 0; i < len; i++) {
    if (proxy_key_same(js, arr_get(js, keys, i), key)) return true;
  }
  return false;
}

static bool proxy_target_key_is_nonconfigurable(ant_t *js, ant_value_t target, ant_value_t key) {
  if (vtype(key) == kTypeSymbol) {
    ant_prop_loc_t off = lkp_sym(target, (ant_offset_t)vdata(key));
    if (off.obj && is_nonconfig_prop(off)) return true;
    prop_meta_t meta;
    return lookup_symbol_prop_meta(js, js_as_obj(target), (ant_offset_t)vdata(key), &meta) && !meta.configurable;
  }

  if (vtype(key) != kTypeString) return false;
  ant_offset_t key_len = 0;
  ant_offset_t key_off = vstr(js, key, &key_len);
  const char *key_ptr = (const char *)(uintptr_t)key_off;
  ant_prop_loc_t off = lkp(js, target, key_ptr, (size_t)key_len);
  if (off.obj && is_nonconfig_prop(off)) return true;
  prop_meta_t meta;
  return lookup_string_prop_meta(js, js_as_obj(target), key_ptr, (size_t)key_len, &meta) && !meta.configurable;
}

static ant_value_t proxy_enum(ant_t *js, ant_value_t obj, enum obj_enum_mode mode);

static ant_value_t proxy_host_own_keys(
  ant_t *js, ant_proxy_state_t *data, bool include_symbols, bool enumerable_only
) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t keys = data->host->keys(js, data->target);
  GC_ROOT_PIN(js, keys);
  
  ant_value_t own = is_err(keys) ? keys : js_own_property_keys(js, data->target, include_symbols, enumerable_only);
  GC_ROOT_PIN(js, own);
  
  ant_offset_t own_count = is_err(own) ? 0 : get_array_length(js, own);
  for (ant_offset_t i = 0; i < own_count; i++) {
    ant_value_t key = arr_get(js, own, i);
    size_t key_len = 0;
    const char *key_str = vtype(key) == kTypeString ? js_getstr(js, key, &key_len) : NULL;
    if (key_str && vtype(data->host->get(js, data->target, key_str, key_len)) != kTypeUndefined) continue;
    js_arr_push(js, keys, key);
  }
  
  GC_ROOT_RESTORE(js, root_mark);
  return is_err(own) ? own : keys;
}

static ant_value_t proxy_host_pairs(ant_t *js, ant_proxy_state_t *data, bool include_symbols) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t pairs = data->host->flat_entries(js, data->target);
  GC_ROOT_PIN(js, pairs);
  
  ant_value_t own = is_err(pairs) ? pairs : js_own_property_keys(js, data->target, include_symbols, true);
  GC_ROOT_PIN(js, own);
  
  ant_offset_t own_count = is_err(own) ? 0 : get_array_length(js, own);
  for (ant_offset_t i = 0; i < own_count && !is_err(own); i++) {
    GC_ROOT_SAVE(key_mark, js);
    ant_value_t key = arr_get(js, own, i);
    
    size_t key_len = 0;
    bool sym = vtype(key) == kTypeSymbol;
    const char *key_str = sym ? NULL : js_getstr(js, key, &key_len);
    
    if (sym || (key_str && vtype(data->host->get(js, data->target, key_str, key_len)) == kTypeUndefined)) {
      ant_value_t val = sym ? js_get_sym(js, data->target, key) : js_get(js, data->target, key_str);
      GC_ROOT_PIN(js, val);
      if (is_err(val)) own = val;
      else {
        js_arr_push(js, pairs, key);
        js_arr_push(js, pairs, val);
      }
    }
    
    GC_ROOT_RESTORE(js, key_mark);
  }
  
  GC_ROOT_RESTORE(js, root_mark);
  return is_err(own) ? own : pairs;
}

static ant_value_t proxy_key_enumerable(ant_t *js, ant_value_t proxy, ant_value_t key) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (data && data->host && vtype(key) == kTypeString) {
    size_t key_len = 0;
    const char *key_str = js_getstr(js, key, &key_len);
    if (key_str && !lkp(js, data->target, key_str, key_len).obj) return js_true;
  }
  
  ant_value_t desc = proxy_get_own_property_descriptor(js, proxy, key);
  if (is_err(desc)) return desc;
  
  return js_bool(vtype(desc) != kTypeUndefined && descriptor_bool_value(js, desc, "enumerable", 10, false));
}

static ant_value_t proxy_own_keys_raw(ant_t *js, ant_value_t obj) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, obj);

  ant_proxy_state_t *data = get_proxy_data(obj);
  if (!data) {
    GC_ROOT_RESTORE(js, root_mark);
    return mkarr(js);
  }
  
  if (data->revoked) {
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot perform 'ownKeys' on a proxy that has been revoked");
  }
  
  if (data->host) {
    GC_ROOT_RESTORE(js, root_mark);
    return proxy_host_own_keys(js, data, true, false);
  }

  ant_value_t keys = mkarr(js);
  GC_ROOT_PIN(js, keys);

  ant_value_t trap = proxy_get_method(js, data->handler, "ownKeys");
  if (is_err(trap)) {
    GC_ROOT_RESTORE(js, root_mark);
    return trap;
  }
  if (vtype(trap) == kTypeUndefined) {
    keys = js_own_property_keys(js, data->target, true, false);
    GC_ROOT_RESTORE(js, root_mark);
    return keys;
  }

  ant_value_t trap_args[1] = { data->target };
  ant_value_t result = sv_vm_call(js->vm, js, trap, data->handler, trap_args, 1, NULL, js_mkundef());
  if (is_err(result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }
  if (vtype(result) != kTypeArray) {
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkerr_typed(js, JS_ERR_TYPE, "ownKeys trap must return an array");
  }
  GC_ROOT_PIN(js, result);

  ant_offset_t len = get_array_length(js, result);
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t ki = arr_get(js, result, i);
    if (vtype(ki) != kTypeString && vtype(ki) != kTypeSymbol) {
      GC_ROOT_RESTORE(js, root_mark);
      return js_mkerr_typed(js, JS_ERR_TYPE, "ownKeys trap result must contain only strings or symbols");
    }
    for (ant_offset_t j = 0; j < i; j++) {
      ant_value_t kj = arr_get(js, result, j);
      if (vtype(ki) != vtype(kj)) continue;
      if (vtype(ki) == kTypeSymbol) {
        if (vdata(ki) == vdata(kj)) {
          GC_ROOT_RESTORE(js, root_mark);
          return js_mkerr_typed(js, JS_ERR_TYPE, "ownKeys trap result must not contain duplicate entries");
        }
        continue;
      }

      ant_offset_t ki_len;
      ant_offset_t ki_off = vstr(js, ki, &ki_len);
      ant_offset_t kj_len;
      ant_offset_t kj_off = vstr(js, kj, &kj_len);
      if (ki_len == kj_len &&
          memcmp((const void *)(uintptr_t)ki_off, (const void *)(uintptr_t)kj_off, ki_len) == 0) {
        GC_ROOT_RESTORE(js, root_mark);
        return js_mkerr_typed(js, JS_ERR_TYPE, "ownKeys trap result must not contain duplicate entries");
      }
    }
  }

  ant_value_t target_keys = js_own_property_keys(js, data->target, true, false);
  GC_ROOT_PIN(js, target_keys);
  if (is_err(target_keys)) {
    GC_ROOT_RESTORE(js, root_mark);
    return target_keys;
  }

  ant_offset_t target_len = get_array_length(js, target_keys);
  bool target_extensible = proxy_target_is_extensible(data->target);
  for (ant_offset_t i = 0; i < target_len; i++) {
    ant_value_t target_key = arr_get(js, target_keys, i);
    if (proxy_target_key_is_nonconfigurable(js, data->target, target_key) &&
        !proxy_keys_include(js, result, target_key)) {
      GC_ROOT_RESTORE(js, root_mark);
      return js_mkerr_typed(js, JS_ERR_TYPE, "'ownKeys' on proxy: trap result did not include non-configurable key");
    }
    if (!target_extensible && !proxy_keys_include(js, result, target_key)) {
      GC_ROOT_RESTORE(js, root_mark);
      return js_mkerr_typed(js, JS_ERR_TYPE, "'ownKeys' on proxy: trap result did not include all target keys");
    }
  }

  if (!target_extensible) {
    for (ant_offset_t i = 0; i < len; i++) {
      ant_value_t result_key = arr_get(js, result, i);
      if (!proxy_keys_include(js, target_keys, result_key)) {
        GC_ROOT_RESTORE(js, root_mark);
        return js_mkerr_typed(js, JS_ERR_TYPE, "'ownKeys' on proxy: trap result included extra key for non-extensible target");
      }
    }
  }

  keys = result;
  GC_ROOT_RESTORE(js, root_mark);
  return keys;
}

static ant_value_t proxy_own_keys_filtered(ant_t *js, ant_value_t obj, bool include_symbols, bool enumerable_only) {
  ant_proxy_state_t *data = get_proxy_data(obj);
  if (data && data->host) return proxy_host_own_keys(js, data, include_symbols, enumerable_only);
  
  ant_value_t keys = proxy_own_keys_raw(js, obj);
  if (is_err(keys) || (include_symbols && !enumerable_only)) return keys;
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, obj);
  GC_ROOT_PIN(js, keys);
  
  ant_value_t out = mkarr(js);
  GC_ROOT_PIN(js, out);
  
  ant_offset_t key_count = get_array_length(js, keys);
  for (ant_offset_t i = 0; i < key_count; i++) {
    ant_value_t key = arr_get(js, keys, i);
    if (vtype(key) == kTypeSymbol && !include_symbols) continue;

    ant_value_t enumerable = enumerable_only ? proxy_key_enumerable(js, obj, key) : js_true;
    if (is_err(enumerable)) { out = enumerable; break; }
    if (js_truthy(js, enumerable)) js_arr_push(js, out, key);
  }
  
  GC_ROOT_RESTORE(js, root_mark);
  return out;
}

static ant_value_t proxy_not_prevented_error(ant_t *js, ant_value_t proxy, const char *host_message, const char *message) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  return js_mkerr_typed(js, JS_ERR_TYPE, "%s", data && data->host ? host_message : message);
}

static ant_value_t proxy_set_integrity_level(ant_t *js, ant_value_t obj, bool freeze) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, obj);

  ant_value_t prevent_result = proxy_prevent_extensions(js, obj);
  if (is_err(prevent_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return prevent_result;
  }
  
  if (!js_truthy(js, prevent_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return proxy_not_prevented_error(js, obj, freeze ? "Cannot freeze" : "Cannot seal", "Proxy preventExtensions trap returned falsy");
  }

  ant_value_t keys = proxy_own_keys_raw(js, obj);
  GC_ROOT_PIN(js, keys);
  if (is_err(keys)) {
    GC_ROOT_RESTORE(js, root_mark);
    return keys;
  }

  ant_offset_t len = get_array_length(js, keys);
  for (ant_offset_t i = 0; i < len; i++) {
    GC_ROOT_SAVE(iter_mark, js);
    
    ant_value_t key = arr_get(js, keys, i);
    GC_ROOT_PIN(js, key);

    ant_value_t desc = Ant_Object_CreateBare(js);
    GC_ROOT_PIN(js, desc);
    
    js_setprop(js, desc, js_mkstr(js, "configurable", 12), js_false);
    if (freeze) js_setprop(js, desc, js_mkstr(js, "writable", 8), js_false);

    ant_value_t result = proxy_define_property(js, obj, key, desc);
    if (is_err(result) || !js_truthy(js, result)) {
      GC_ROOT_RESTORE(js, iter_mark);
      GC_ROOT_RESTORE(js, root_mark);
      return is_err(result) ? result : js_mkerr_typed(js, JS_ERR_TYPE, "Proxy defineProperty trap returned falsy");
    }
    
    GC_ROOT_RESTORE(js, iter_mark);
  }

  GC_ROOT_RESTORE(js, root_mark);
  return obj;
}

static ant_value_t proxy_test_integrity_level(ant_t *js, ant_value_t obj, bool frozen) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, obj);

  ant_value_t extensible = proxy_is_extensible(js, obj);
  if (is_err(extensible)) {
    GC_ROOT_RESTORE(js, root_mark);
    return extensible;
  }
  if (js_truthy(js, extensible)) {
    GC_ROOT_RESTORE(js, root_mark);
    return js_false;
  }

  ant_value_t keys = proxy_own_keys_raw(js, obj);
  GC_ROOT_PIN(js, keys);
  if (is_err(keys)) {
    GC_ROOT_RESTORE(js, root_mark);
    return keys;
  }

  ant_offset_t len = get_array_length(js, keys);
  for (ant_offset_t i = 0; i < len; i++) {
    GC_ROOT_SAVE(iter_mark, js);
    ant_value_t key = arr_get(js, keys, i);
    GC_ROOT_PIN(js, key);

    ant_value_t desc = proxy_get_own_property_descriptor(js, obj, key);
    GC_ROOT_PIN(js, desc);
    if (is_err(desc)) {
      GC_ROOT_RESTORE(js, iter_mark);
      GC_ROOT_RESTORE(js, root_mark);
      return desc;
    }
    if (vtype(desc) == kTypeUndefined ||
        descriptor_bool_value(js, desc, "configurable", 12, false) ||
        (frozen && descriptor_bool_value(js, desc, "writable", 8, false))) {
      GC_ROOT_RESTORE(js, iter_mark);
      GC_ROOT_RESTORE(js, root_mark);
      return js_false;
    }

    GC_ROOT_RESTORE(js, iter_mark);
  }

  GC_ROOT_RESTORE(js, root_mark);
  return js_true;
}

static ant_value_t proxy_enum_host_pairs(ant_t *js, ant_proxy_state_t *data, enum obj_enum_mode mode) {
  ant_value_t pairs = proxy_host_pairs(js, data, false);
  if (is_err(pairs)) return pairs;
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, pairs);
  ant_value_t out = mkarr(js);
  GC_ROOT_PIN(js, out);
  
  ant_offset_t len = get_array_length(js, pairs);
  for (ant_offset_t i = 0; i + 1 < len; i += 2) {
    GC_ROOT_SAVE(item_mark, js);
    ant_value_t value = arr_get(js, pairs, i + 1);
    ant_value_t item = mode == OBJ_ENUM_VALUES ? value : map_to_entry(js, arr_get(js, pairs, i), value);
    GC_ROOT_PIN(js, item);
    js_arr_push(js, out, item);
    GC_ROOT_RESTORE(js, item_mark);
  }
  
  GC_ROOT_RESTORE(js, root_mark);
  return out;
}

static ant_value_t proxy_enum(ant_t *js, ant_value_t obj, enum obj_enum_mode mode) {
  if (mode == OBJ_ENUM_KEYS) return proxy_own_keys_filtered(js, obj, false, true);
  
  ant_proxy_state_t *host_data = get_proxy_data(obj);
  if (host_data && host_data->host && host_data->host->flat_entries) return proxy_enum_host_pairs(js, host_data, mode);
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, obj);

  ant_value_t keys = proxy_own_keys_raw(js, obj);
  GC_ROOT_PIN(js, keys);
  
  ant_value_t out = is_err(keys) ? keys : mkarr(js);
  GC_ROOT_PIN(js, out);

  ant_offset_t key_count = is_err(keys) ? 0 : get_array_length(js, keys);
  for (ant_offset_t i = 0; i < key_count && !is_err(out); i++) {
    GC_ROOT_SAVE(iter_mark, js);
    
    ant_value_t key = arr_get(js, keys, i);
    GC_ROOT_PIN(js, key);
    
    ant_value_t enumerable = vtype(key) == kTypeString ? proxy_key_enumerable(js, obj, key) : js_false;
    if (is_err(enumerable)) out = enumerable;
    
    else if (js_truthy(js, enumerable)) {
      ant_value_t value = proxy_get_val(js, obj, key);
      GC_ROOT_PIN(js, value);
      
      ant_value_t item = is_err(value) || mode == OBJ_ENUM_VALUES ? value : map_to_entry(js, key, value);
      GC_ROOT_PIN(js, item);
      
      if (is_err(item)) out = item;
      else js_arr_push(js, out, item);
    }
    
    GC_ROOT_RESTORE(js, iter_mark);
  }

  GC_ROOT_RESTORE(js, root_mark);
  return out;
}

static ant_value_t object_nullish_error(ant_t *js) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert undefined or null to object");
}

static ant_value_t builtin_object_keys(ant_params_t) {
  ant_value_t source = nargs > 0 ? args[0] : js_mkundef();
  if (vtype(source) == kTypeNull || vtype(source) == kTypeUndefined) return object_nullish_error(js);
  
  ant_value_t obj = js_object_view(args[0]);
  if (vtype(obj) == kTypeString) return js_for_in_keys(js, obj);

  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) != kTypeFunction) return mkarr(js);
    obj = promoted;
  }

  if (vtype(obj) != kTypeObject && vtype(obj) != kTypeArray && vtype(obj) != kTypeFunction) return mkarr(js);
  if (is_proxy(obj)) return proxy_enum(js, obj, OBJ_ENUM_KEYS);

  return js_own_property_keys(js, obj, false, true);
}

static ant_value_t for_in_keys_add(ant_t *js, ant_value_t out, ant_value_t seen, ant_value_t key) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, out);
  GC_ROOT_PIN(js, seen);
  GC_ROOT_PIN(js, key);
  
  if (vtype(key) != kTypeString) return js_mkundef();

  ant_offset_t key_len = 0;
  ant_offset_t key_off = vstr(js, key, &key_len);
  const char *key_ptr = (const char *)(uintptr_t)(key_off);

  if (lkp(js, seen, key_ptr, key_len).obj) goto done;

  ant_value_t mark = setprop_cstr(js, seen, key_ptr, key_len, js_true);
  if (is_err(mark)) {
    GC_ROOT_RESTORE(js, root_mark);
    return mark;
  }

  if (vtype(out) == kTypeArray) js_arr_push(js, out, key);
done:
  GC_ROOT_RESTORE(js, root_mark);
  return js_mkundef();
}

static ant_value_t for_in_keys_add_cstr(
  ant_t *js, ant_value_t out, ant_value_t seen, const char *key, size_t key_len, bool enumerable
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, out);
  GC_ROOT_PIN(js, seen);

  const char *interned = intern_string(key, key_len);
  if (!interned) {
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkerr(js, "oom");
  }

  if (lkp_interned(seen, interned).obj) goto done;

  ant_value_t mark = mkprop_interned(js, seen, interned, js_true, 0);
  if (is_err(mark)) {
    GC_ROOT_RESTORE(js, root_mark);
    return mark;
  }

  if (enumerable && vtype(out) == kTypeArray) {
    ant_value_t key_val = js_mkstr(js, key, key_len);
    GC_ROOT_PIN(js, key_val);
    js_arr_push(js, out, key_val);
  }

done:
  GC_ROOT_RESTORE(js, root_mark);
  return js_mkundef();
}

static ant_value_t for_in_keys_add_indices(ant_t *js, ant_value_t out, ant_value_t seen, ant_offset_t len) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, out);
  GC_ROOT_PIN(js, seen);
  
  for (ant_offset_t i = 0; i < len; i++) {
    char idx[16];
    size_t idx_len = uint_to_str(idx, sizeof(idx), (uint64_t)i);
    
    ant_value_t key = js_mkstr(js, idx, idx_len);
    GC_ROOT_PIN(js, key);
    
    ant_value_t r = for_in_keys_add(js, out, seen, key);
    if (is_err(r)) { GC_ROOT_RESTORE(js, root_mark); return r; }
  }
  
  GC_ROOT_RESTORE(js, root_mark);
  return js_mkundef();
}

static ant_value_t for_in_keys_collect_ordinary_own(
  ant_t *js, ant_value_t out, ant_value_t seen, ant_value_t obj, ant_object_t *ptr
) {
  bool is_arr = (vtype(obj) == kTypeArray);
  uint32_t count = ant_shape_count(ptr->shape);
  ant_value_t r = js_mkundef();

  if (!is_arr) {
    bool has_index_key = false;
    for (uint32_t i = 0; i < count; i++) {
      const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
      if (!prop || prop->type != ANT_SHAPE_KEY_STRING) continue;
      if (i >= ptr->prop_count) continue;
      const char *key = prop->key.interned;
      if (own_key_is_array_index(key, intern_length(key), NULL)) {
        has_index_key = true;
        break;
      }
    }

    if (!has_index_key) {
      for (uint32_t i = 0; i < count; i++) {
        const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
        if (!prop || prop->type != ANT_SHAPE_KEY_STRING) continue;
        if (i >= ptr->prop_count) continue;
        const char *key = prop->key.interned;
        bool enumerable = own_key_is_enumerable(js, obj, ptr, i);
        r = for_in_keys_add_cstr(js, out, seen, key, intern_length(key), enumerable);
        if (is_err(r)) return r;
      }
      return js_mkundef();
    }
  }

  if (is_arr) {
    ant_offset_t doff = get_dense_buf(obj);
    ant_offset_t dense_len = doff ? dense_iterable_length(js, obj) : 0;
    for (ant_offset_t i = 0; i < dense_len; i++) {
      if (is_empty_slot(dense_get(doff, i))) continue;
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
      r = for_in_keys_add_cstr(js, out, seen, idxstr, idxlen, true);
      if (is_err(r)) return r;
    }
  }

  own_index_list_t indices;
  own_index_list_init(&indices);

  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING) continue;
    if (i >= ptr->prop_count) continue;

    const char *key = prop->key.interned;
    size_t key_len = intern_length(key);
    if (is_arr && own_key_is_dense_shadow(js, obj, key, key_len)) continue;

    unsigned long parsed = 0;
    if (!own_key_is_array_index(key, key_len, &parsed)) continue;
    if (!own_index_list_push(&indices, i, parsed)) {
      own_index_list_free(&indices);
      return js_mkerr(js, "oom");
    }
  }

  own_index_keys_sort(indices.items, indices.count);
  for (uint32_t i = 0; i < indices.count && !is_err(r); i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, indices.items[i].slot);
    if (!prop) continue;
    const char *key = prop->key.interned;
    bool enumerable = own_key_is_enumerable(js, obj, ptr, indices.items[i].slot);
    r = for_in_keys_add_cstr(js, out, seen, key, intern_length(key), enumerable);
  }

  own_index_list_free(&indices);
  if (is_err(r)) return r;

  if (is_arr) {
    r = for_in_keys_add_cstr(js, out, seen, "length", 6, false);
    if (is_err(r)) return r;
  }

  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING) continue;
    if (i >= ptr->prop_count) continue;

    const char *key = prop->key.interned;
    size_t key_len = intern_length(key);
    if (own_key_is_array_index(key, key_len, NULL)) continue;

    bool enumerable = own_key_is_enumerable(js, obj, ptr, i);
    r = for_in_keys_add_cstr(js, out, seen, key, key_len, enumerable);
    if (is_err(r)) return r;
  }

  return js_mkundef();
}

static ant_value_t for_in_keys_add_exotic_indices(
  ant_t *js, ant_value_t out, ant_value_t seen, ant_value_t obj, ant_object_t *ptr
) {
  if (vtype(obj) != kTypeObject) return js_mkundef();
  ant_value_t prim = get_slot(obj, SLOT_PRIMITIVE);
  if (vtype(prim) == kTypeString) return for_in_keys_add_indices(js, out, seen, str_utf16_len(js, prim));
  TypedArrayData *typed = ptr->flags.is_exotic ? buffer_get_typedarray_data(obj) : NULL;
  if (Ant_Buffer_ViewIsLive(typed))
    return for_in_keys_add_indices(js, out, seen, (ant_offset_t)typed->length);
  return js_mkundef();
}

static inline ant_value_t for_in_keys_collect_chain(
  ant_t *js, ant_value_t out, ant_value_t seen, ant_value_t obj
) {
  ant_value_t cur = obj;
  GC_ROOT_PIN(js, cur);

  for (int depth = 0; is_object_type(cur) && depth < MAX_PROTO_CHAIN_DEPTH; depth++) {
    GC_ROOT_SAVE(iter_mark, js);
    ant_value_t as_cur = (vtype(cur) == kTypeFunction) ? js_func_obj(cur) : cur;
    
    ant_object_t *cur_ptr = js_obj_ptr(as_cur);
    ant_value_t r, proto;
    
    if (!cur_ptr) goto next_proto;
    r = for_in_keys_add_exotic_indices(js, out, seen, as_cur, cur_ptr);
    if (is_err(r)) goto err;
    
    if (!cur_ptr->shape) goto next_proto;
    r = for_in_keys_collect_ordinary_own(js, out, seen, as_cur, cur_ptr);
    
    if (is_err(r)) goto err;
    goto next_proto;

  next_proto:
    GC_ROOT_RESTORE(js, iter_mark);
    proto = js_get_proto(js, cur);
    if (!is_object_type(proto)) break;
    cur = proto;
    continue;
  err:
    GC_ROOT_RESTORE(js, iter_mark);
    return r;
  }

  return out;
}

ant_value_t js_for_in_keys(ant_t *js, ant_value_t obj) {
  GC_ROOT_SAVE(root_mark, js);
  uint8_t t = vtype(obj);
  ant_value_t out = mkarr(js);
  ant_value_t result = out;
  
  GC_ROOT_PIN(js, obj);
  GC_ROOT_PIN(js, out);
  
  if (t == kTypeNull || t == kTypeUndefined) goto done;

  ant_value_t seen = mkobj(js, 0);
  GC_ROOT_PIN(js, seen);

  if (t == kTypeString) {
    result = for_in_keys_add_indices(js, out, seen, str_utf16_len(js, obj));
    if (is_err(result)) goto done;
    result = out;
    goto done;
  }

  if (t == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) != kTypeFunction) goto done;
    obj = promoted;
    t = kTypeFunction;
  }

  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) goto done;
  
  if (is_proxy(obj)) {
    result = proxy_enum(js, obj, OBJ_ENUM_KEYS);
    goto done;
  }
  
  result = for_in_keys_collect_chain(js, out, seen, obj);

done:
  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static ant_value_t builtin_object_values(ant_params_t) {
  ant_value_t source = nargs > 0 ? args[0] : js_mkundef();
  if (vtype(source) == kTypeNull || vtype(source) == kTypeUndefined) return object_nullish_error(js);
  ant_value_t obj = js_object_view(args[0]);
  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) != kTypeFunction) return mkarr(js);
    obj = promoted;
  }
  if (vtype(obj) == kTypeString) return object_enum_collect(js, obj, OBJ_ENUM_VALUES);
  if (vtype(obj) != kTypeObject && vtype(obj) != kTypeArray && vtype(obj) != kTypeFunction) return mkarr(js);
  if (is_proxy(obj)) return proxy_enum(js, obj, OBJ_ENUM_VALUES);
  return object_enum(js, obj, OBJ_ENUM_VALUES);
}

static ant_value_t builtin_object_entries(ant_params_t) {
  ant_value_t source = nargs > 0 ? args[0] : js_mkundef();
  if (vtype(source) == kTypeNull || vtype(source) == kTypeUndefined) return object_nullish_error(js);
  ant_value_t obj = js_object_view(args[0]);
  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) != kTypeFunction) return mkarr(js);
    obj = promoted;
  }
  if (vtype(obj) == kTypeString) return object_enum_collect(js, obj, OBJ_ENUM_ENTRIES);
  if (vtype(obj) != kTypeObject && vtype(obj) != kTypeArray && vtype(obj) != kTypeFunction) return mkarr(js);
  if (is_proxy(obj)) return proxy_enum(js, obj, OBJ_ENUM_ENTRIES);
  return object_enum(js, obj, OBJ_ENUM_ENTRIES);
}

static ant_value_t builtin_object_getPrototypeOf(ant_params_t) {
  if (nargs == 0) return js_mkerr(js, "Object.getPrototypeOf requires an argument");
  ant_value_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t == kTypeString || t == kTypeNumber || t == kTypeBool || t == kTypeBigInt)
    return js_primitive_prototype(js, t);
  if (t == kTypeBuiltin) return js_get_proto(js, obj);
  
  if (is_object_type(obj)) {
    if (is_proxy(obj)) return proxy_get_prototype_of(js, obj);
    return get_proto(js, obj);
  }
  
  return js_mknull();
}

static ant_value_t builtin_object_setPrototypeOf(ant_params_t) {
  if (nargs < 2) return js_mkerr(js, "Object.setPrototypeOf requires 2 arguments");
  
  ant_value_t obj = args[0];
  ant_value_t proto = args[1];
  
  uint8_t t = vtype(obj);
  if (t == kTypeBuiltin) {
    obj = js_cfunc_promote(js, obj);
    t = kTypeFunction;
  }

  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) {
    return js_mkerr(js, "Object.setPrototypeOf: first argument must be an object");
  }
  
  uint8_t pt = vtype(proto);
  if (pt == kTypeBuiltin) {
    proto = js_cfunc_promote(js, proto);
    pt = kTypeFunction;
  }

  if (pt != kTypeObject && pt != kTypeArray && pt != kTypeFunction && pt != kTypeNull) {
    return js_mkerr(js, "Object.setPrototypeOf: prototype must be an object or null");
  }
  
  if (pt != kTypeNull && proto_chain_contains(js, proto, obj))
    return js_mkerr(js, "Cyclic __proto__ value");

  if (is_proxy(obj)) {
    ant_value_t result = proxy_set_prototype_of(js, obj, proto);
    if (is_err(result)) return result;
    if (!js_truthy(js, result))
      return js_mkerr_typed(js, JS_ERR_TYPE, "'setPrototypeOf' on proxy: trap returned falsy");
    return obj;
  }
  
  set_proto(js, obj, proto);
  return obj;
}

static ant_value_t builtin_proto_getter(ant_params_t) {
  ant_value_t this_val = js->this_val;
  uint8_t t = vtype(this_val);
  
  if (t == kTypeUndefined || t == kTypeNull) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot read property '__proto__' of %s", typestr(t));
  }
  
  if (t == kTypeObject || t == kTypeArray || t == kTypeFunction) {
    return get_proto(js, this_val);
  }
  
  return js_primitive_prototype(js, t);
}

static ant_value_t builtin_proto_setter(ant_params_t) {
  ant_value_t this_val = js->this_val;
  uint8_t t = vtype(this_val);
  
  if (t == kTypeUndefined || t == kTypeNull) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot set property '__proto__' of %s", typestr(t));
  }
  
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) {
    return js_mkundef();
  }
  
  if (nargs == 0) return js_mkundef();
  
  ant_value_t proto = args[0];
  uint8_t pt = vtype(proto);
  
  if (pt != kTypeObject && pt != kTypeArray && pt != kTypeFunction && pt != kTypeNull) {
    return js_mkundef();
  }
  
  if (pt != kTypeNull && proto_chain_contains(js, proto, this_val))
    return js_mkundef();
  
  set_proto(js, this_val, proto);
  return js_mkundef();
}

static ant_value_t legacy_accessor_this_obj(ant_t *js, ant_value_t this_val) {
  uint8_t t = vtype(this_val);
  
  if (t == kTypeUndefined || t == kTypeNull) return js_mkerr_typed(
    js, JS_ERR_TYPE, "Cannot convert undefined or null to object"
  );
  
  if (t == kTypeBuiltin) return js_cfunc_promote(js, this_val);
  if (t == kTypeObject || t == kTypeArray || t == kTypeFunction) return js_as_obj(this_val);
  
  if (t == kTypeString || t == kTypeNumber || t == kTypeBool || t == kTypeBigInt) {
    ant_value_t boxed = builtin_Object(js, &this_val, 1, js_mkundef());
    if (is_err(boxed)) return boxed;
    if (is_object_type(boxed)) return js_as_obj(boxed);
  }
  
  return js_mkerr_typed(js, JS_ERR_TYPE, "Legacy accessor methods require an object receiver");
}

static bool legacy_accessor_key(
  ant_t *js, ant_value_t key,
  ant_value_t *key_val_out,
  const char **key_str_out,
  ant_offset_t *key_len_out,
  ant_offset_t *sym_off_out
) {
  if (key_val_out) *key_val_out = key;
  if (key_str_out) *key_str_out = NULL;
  if (key_len_out) *key_len_out = 0;
  if (sym_off_out) *sym_off_out = 0;

  if (vtype(key) == kTypeSymbol) {
    if (sym_off_out) *sym_off_out = (ant_offset_t)vdata(key);
    return true;
  }

  ant_value_t key_val = (vtype(key) == kTypeString) ? key : coerce_to_str(js, key);
  if (is_err(key_val)) return false;
  if (key_val_out) *key_val_out = key_val;
  
  ant_offset_t key_len = 0;
  ant_offset_t key_off = vstr(js, key_val, &key_len);
  
  if (key_str_out) *key_str_out = (const char *)(uintptr_t)key_off;
  if (key_len_out) *key_len_out = key_len;
  
  return true;
}

static ant_value_t builtin_object___defineGetter__(ant_params_t) {
  ant_value_t obj = legacy_accessor_this_obj(js, js->this_val);
  if (is_err(obj)) return obj;
  if (nargs < 2) return js_mkundef();

  ant_value_t getter = args[1];
  uint8_t gt = vtype(getter);
  
  if (gt != kTypeFunction && gt != kTypeBuiltin) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Object.prototype.__defineGetter__: Expecting function");
  }

  ant_value_t key_val = js_mkundef();
  const char *key_str = NULL;
  ant_offset_t key_len = 0;
  ant_offset_t sym_off = 0;
  
  if (!legacy_accessor_key(js, args[0], &key_val, &key_str, &key_len, &sym_off)) {
    return key_val;
  }

  if (is_proxy(js_as_obj(obj))) {
    GC_ROOT_SAVE(root_mark, js);
    GC_ROOT_PIN(js, obj);
    GC_ROOT_PIN(js, key_val);
    GC_ROOT_PIN(js, getter);
    
    ant_value_t desc = Ant_Object_CreateBare(js);
    if (is_err(desc)) {
      GC_ROOT_RESTORE(js, root_mark);
      return desc;
    }
    
    GC_ROOT_PIN(js, desc);
    js_setprop(js, desc, js_mkstr(js, "get", 3), getter);
    js_setprop(js, desc, js_mkstr(js, "enumerable", 10), js_true);
    js_setprop(js, desc, js_mkstr(js, "configurable", 12), js_true);
    
    ant_value_t result = object_define_property(js, obj, key_val, desc);
    GC_ROOT_RESTORE(js, root_mark);
    if (is_err(result)) return result;
    
    return js_mkundef();
  }

  if (vtype(key_val) == kTypeSymbol) {
    js_set_sym_getter_desc(js, js_as_obj(obj), key_val, getter, JS_DESC_E | JS_DESC_C);
  } else js_set_getter_desc(js, js_as_obj(obj), key_str, (size_t)key_len, getter, JS_DESC_E | JS_DESC_C);
  
  return js_mkundef();
}

static ant_value_t builtin_object___defineSetter__(ant_params_t) {
  ant_value_t obj = legacy_accessor_this_obj(js, js->this_val);
  if (is_err(obj)) return obj;
  if (nargs < 2) return js_mkundef();

  ant_value_t setter = args[1];
  uint8_t st = vtype(setter);
  
  if (st != kTypeFunction && st != kTypeBuiltin) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Object.prototype.__defineSetter__: Expecting function");
  }

  ant_value_t key_val = js_mkundef();
  const char *key_str = NULL;
  ant_offset_t key_len = 0;
  ant_offset_t sym_off = 0;
  
  if (!legacy_accessor_key(js, args[0], &key_val, &key_str, &key_len, &sym_off)) {
    return key_val;
  }

  if (is_proxy(js_as_obj(obj))) {
    GC_ROOT_SAVE(root_mark, js);
    GC_ROOT_PIN(js, obj);
    GC_ROOT_PIN(js, key_val);
    GC_ROOT_PIN(js, setter);
    
    ant_value_t desc = Ant_Object_CreateBare(js);
    if (is_err(desc)) {
      GC_ROOT_RESTORE(js, root_mark);
      return desc;
    }
    
    GC_ROOT_PIN(js, desc);
    js_setprop(js, desc, js_mkstr(js, "set", 3), setter);
    js_setprop(js, desc, js_mkstr(js, "enumerable", 10), js_true);
    js_setprop(js, desc, js_mkstr(js, "configurable", 12), js_true);
    
    ant_value_t result = object_define_property(js, obj, key_val, desc);
    GC_ROOT_RESTORE(js, root_mark);
    if (is_err(result)) return result;
    
    return js_mkundef();
  }

  if (vtype(key_val) == kTypeSymbol) {
    js_set_sym_setter_desc(js, js_as_obj(obj), key_val, setter, JS_DESC_E | JS_DESC_C);
  } else js_set_setter_desc(js, js_as_obj(obj), key_str, (size_t)key_len, setter, JS_DESC_E | JS_DESC_C);
  
  return js_mkundef();
}

static ant_value_t legacy_lookup_accessor_from_descriptor(ant_t *js, ant_value_t desc, bool want_getter) {
  if (vtype(desc) != kTypeObject) return js_mkundef();
  ant_prop_loc_t off = lkp_interned(desc, want_getter ? js->intern.get : js->intern.set);
  if (!off.obj) return js_mkundef();
  return js_prop_load(off);
}

static ant_value_t legacy_lookup_accessor(ant_t *js, ant_value_t this_val, ant_value_t key, bool want_getter) {
  ant_value_t obj = legacy_accessor_this_obj(js, this_val);
  if (is_err(obj)) return obj;

  ant_value_t key_val = js_mkundef();
  const char *key_str = NULL;
  ant_offset_t key_len = 0;
  ant_offset_t sym_off = 0;
  
  if (!legacy_accessor_key(js, key, &key_val, &key_str, &key_len, &sym_off)) {
    return key_val;
  }

  for (ant_value_t cur = obj; is_object_type(cur); ) {
    if (is_proxy(cur)) {
      ant_value_t desc = proxy_get_own_property_descriptor(js, cur, key_val);
      if (is_err(desc)) return desc;
      if (vtype(desc) == kTypeObject) return legacy_lookup_accessor_from_descriptor(js, desc, want_getter);
      cur = proxy_get_prototype_of(js, cur);
      if (is_err(cur)) return cur;
      continue;
    }

    prop_meta_t meta;
    bool has_meta = (vtype(key_val) == kTypeSymbol)
      ? lookup_symbol_prop_meta(js, cur, sym_off, &meta)
      : lookup_string_prop_meta(js, cur, key_str, (size_t)key_len, &meta);
    if (has_meta) {
      if (want_getter) return meta.has_getter ? meta.getter : js_mkundef();
      return meta.has_setter ? meta.setter : js_mkundef();
    }
    
    if (vtype(key_val) == kTypeSymbol) {
      if (lkp_sym(cur, sym_off).obj) return js_mkundef();
    } else {
      if (real_array_obj_ptr(cur) && is_length_key(key_str, key_len)) return js_mkundef();
      if (array_obj_ptr(cur) && is_array_index(key_str, key_len)) {
        unsigned long idx = 0;
        if (
          parse_array_index(key_str, key_len, get_array_length(js, cur), &idx) &&
          arr_has(js, cur, (ant_offset_t)idx)
        ) return js_mkundef();
      }
      if (lkp(js, cur, key_str, key_len).obj) return js_mkundef();
    }
    cur = get_proto(js, cur);
  }

  return js_mkundef();
}

static ant_value_t builtin_object___lookupGetter__(ant_params_t) {
  if (nargs < 1) return js_mkundef();
  return legacy_lookup_accessor(js, js->this_val, args[0], true);
}

static ant_value_t builtin_object___lookupSetter__(ant_params_t) {
  if (nargs < 1) return js_mkundef();
  return legacy_lookup_accessor(js, js->this_val, args[0], false);
}

static ant_value_t builtin_object_create(ant_params_t) {
  if (nargs == 0) return js_mkerr(js, "Object.create requires a prototype argument");
  
  ant_value_t proto = args[0];
  uint8_t pt = vtype(proto);
  
  if (pt != kTypeObject && pt != kTypeArray && pt != kTypeFunction && pt != kTypeNull)
    return js_mkerr(js, "Object.create: prototype must be an object or null");

  ant_value_t obj = Ant_Object_CreateBare(js);
  if (pt == kTypeNull) js_set_proto_init(obj, js_mknull());
  else js_set_proto_init(obj, proto);

  if (nargs >= 2 && vtype(args[1]) != kTypeUndefined) {
    ant_value_t result = object_define_properties(js, obj, args[1]);
    if (is_err(result)) return result;
  }

  return obj;
}

static bool typed_array_has_index(ant_value_t obj, const char *key, size_t key_len) {
  ant_object_t *ptr = is_object_type(obj) ? js_obj_ptr(js_as_obj(obj)) : NULL;
  if (!ptr || !ptr->flags.is_exotic) return false;
  
  TypedArrayData *typed = buffer_get_typedarray_data(js_as_obj(obj));
  unsigned long index = 0;
  
  return
    Ant_Buffer_ViewIsLive(typed) &&
    parse_array_index(key, key_len, (ant_offset_t)typed->length, &index);
}

static ant_value_t object_has_own_key(ant_t *js, ant_value_t obj, property_key_view_t *key) {
  uint8_t t = vtype(obj);

  if (t == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) == kTypeFunction) {
      obj = promoted;
      t = kTypeFunction;
    } else {
      if (key->is_symbol) return mkval(kTypeBool, 0);
      ant_value_t value = js_mkundef();
      return mkval(kTypeBool, js_cfunc_try_get_own(js, obj, key->bytes, key->length, &value) ? 1 : 0);
    }
  }

  if (key->is_symbol) {
    if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) return mkval(kTypeBool, 0);
    ant_value_t as_obj = js_as_obj(obj);
    if (is_proxy(as_obj)) return proxy_has_own(js, as_obj, key->js_key);

    ant_offset_t sym_off = (ant_offset_t)vdata(key->js_key);
    ant_prop_loc_t off = lkp_sym(as_obj, sym_off);
    if (off.obj) return mkval(kTypeBool, 1);
    prop_meta_t meta;
    return mkval(kTypeBool, lookup_symbol_prop_meta(js, as_obj, sym_off, &meta) ? 1 : 0);
  }

  const char *key_str = key->bytes;
  size_t key_len = key->length;
  property_key_view_resolve(js, key);

  if (t == kTypeString) {
    ant_value_t string_exotic_value = js_mkundef();
    if (js_try_get_string_own_exotic(js, obj, key_str, key_len, &string_exotic_value, NULL))
      return mkval(kTypeBool, 1);
    return mkval(kTypeBool, 0);
  }

  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) return mkval(kTypeBool, 0);
  
  ant_value_t as_obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(as_obj);
  
  bool exotic = ptr && ptr->flags.is_exotic;
  if (!exotic && !key->index_like) return js_bool(lkp_interned(as_obj, key->interned).obj);
  
  if (exotic && is_proxy(as_obj)) {
    ant_value_t materialized = property_key_view_materialize(js, key);
    if (is_err(materialized)) return materialized;
    return proxy_has_own(js, as_obj, materialized);
  }

  if (key->index_like && array_obj_ptr(as_obj)) {
    if (real_array_obj_ptr(as_obj) && is_length_key(key_str, key_len)) return mkval(kTypeBool, 1);
    if (arr_own_dense_element(js, as_obj, key_str, key_len, NULL)) return mkval(kTypeBool, 1);
  }

  if (lkp_interned(as_obj, key->interned).obj) return mkval(kTypeBool, 1);
  
  if (exotic) {
    if (key->index_like && typed_array_has_index(as_obj, key_str, key_len)) return mkval(kTypeBool, 1);
    descriptor_entry_t *desc = lookup_descriptor(js, as_obj, key_str, key_len);
    return mkval(kTypeBool, (desc && (desc->has_getter || desc->has_setter)) ? 1 : 0);
  }

  ant_value_t string_exotic_value = js_mkundef();
  
  return mkval(kTypeBool, 
    key->index_like && t == kTypeObject &&
    js_try_get_string_own_exotic(js, obj, key_str, key_len, &string_exotic_value, NULL)
  );
}

ant_value_t Ant_Object_HasOwn(
  ant_t *js, ant_value_t obj, ant_value_t raw_key
) {
  uint8_t t = vtype(obj);
  if (t == kTypeNull || t == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert undefined or null to object");

  char key_stack[128];
  property_key_view_t key;
  ant_value_t error = property_key_view_init(
    js, raw_key, &key, 
    key_stack, sizeof(key_stack)
  );
  
  if (is_err(error)) {
    property_key_view_dispose(&key);
    return error;
  }

  bool may_materialize = is_object_type(obj) && is_proxy(js_as_obj(obj));
  bool root_key = property_key_view_requires_pin(&key, may_materialize);
  
  size_t root_mark = root_key ? gc_root_scope(js) : 0;
  if (root_key) GC_ROOT_PIN(js, key.js_key);

  ant_value_t result = object_has_own_key(js, obj, &key);
  property_key_view_dispose(&key);
  if (root_key) GC_ROOT_RESTORE(js, root_mark);
  
  return result;
}

static ant_value_t builtin_object_hasOwn(ant_params_t) {
  ant_value_t target = nargs >= 1 ? js_object_view(args[0]) : js_mkundef();
  ant_value_t key = nargs >= 2 ? args[1] : js_mkundef();
  return Ant_Object_HasOwn(js, target, key);
}

typedef struct {
  sv_callback_t cb;
  js_group_add_fn add;
  void *ctx;
  ant_offset_t index;
} group_by_ctx_t;

static iter_action_t group_by_step(ant_t *js, ant_value_t value, void *ctx, ant_value_t *out) {
  group_by_ctx_t *group_by = ctx;
  ant_value_t cb_args[2] = { value, tov((double)group_by->index++) };
  ant_value_t key = sv_callback_call(js->vm, js, &group_by->cb, cb_args, 2);
  if (is_err(key)) { *out = key; return ITER_ERROR; }
  
  ant_value_t added = group_by->add(js, group_by->ctx, key, value);
  if (is_err(added)) { *out = added; return ITER_ERROR; }
  
  return ITER_CONTINUE;
}

ant_value_t js_group_by(ant_t *js, ant_value_t items, ant_value_t callback, js_group_add_fn add, void *ctx) {
  if (!is_callable(callback)) return js_mkerr_typed(js, JS_ERR_TYPE, "callback is not a function");
  group_by_ctx_t group_by = { sv_callback_prepare(js, callback, js_mkundef()), add, ctx, 0 };

  if (array_obj_ptr(items) && js_array_iteration_default(js, items)) {
    ant_offset_t len = 0;
    ant_value_t len_result = array_like_length_checked(js, items, &len);
    if (is_err(len_result)) return len_result;
    
    for (ant_offset_t i = 0; i < len; i++) {
      ant_value_t value = array_method_get_index(js, items, i);
      if (is_err(value)) return value;
      ant_value_t err = js_mkundef();
      if (group_by_step(js, value, &group_by, &err) == ITER_ERROR) return err;
      len_result = array_like_length_checked(js, items, &len);
      if (is_err(len_result)) return len_result;
    }
    
    return js_mkundef();
  }

  if (vtype(js->sym.iterator_sym) != kTypeSymbol || !is_callable(js_get_sym(js, items, js->sym.iterator_sym)))
    return js_mkerr_typed(js, JS_ERR_TYPE, "object is not iterable");
  
  ant_value_t iterated = iter_foreach(js, items, group_by_step, &group_by);
  return is_err(iterated) ? iterated : js_mkundef();
}

static ant_value_t object_group_add(ant_t *js, void *ctx, ant_value_t key, ant_value_t value) {
  ant_value_t result = *(ant_value_t *)ctx;
  
  if (vtype(key) == kTypeSymbol) {
    ant_value_t group = js_get_sym(js, result, key);
    if (vtype(group) != kTypeArray) {
      group = mkarr(js);
      js_set_sym(js, result, key, group);
    }
    
    js_arr_push(js, group, value);
    return js_mkundef();
  }
  
  ant_value_t key_str = js_tostring_val(js, key);
  if (is_err(key_str)) return key_str;

  ant_offset_t klen;
  ant_offset_t koff = vstr(js, key_str, &klen);
  ant_prop_loc_t grp_off = lkp(js, result, (const char *)(uintptr_t)koff, klen);
  ant_value_t group;
  
  if (grp_off.obj) group = js_prop_load(grp_off);
  else {
    group = mkarr(js);
    js_setprop(js, result, key_str, group);
  }
  
  js_arr_push(js, group, value);
  return js_mkundef();
}

static ant_value_t builtin_object_groupBy(ant_params_t) {
  if (nargs < 2) return js_mkerr_typed(js, JS_ERR_TYPE, "Object.groupBy requires 2 arguments");
  
  ant_value_t result = Ant_Object_CreateBare(js);
  js_set_proto_init(result, js_mknull());
  
  ant_value_t grouped = js_group_by(js, args[0], args[1], object_group_add, &result);
  return is_err(grouped) ? grouped : result;
}

static bool define_lookup_existing_meta(
  ant_t *js, ant_value_t obj,
  bool sym_key,
  ant_offset_t sym_off,
  const char *prop_str,
  ant_offset_t prop_len,
  ant_prop_loc_t existing,
  prop_meta_t *out
) {
  if (sym_key && lookup_symbol_prop_meta(js, obj, sym_off, out)) return true;
  if (!sym_key && lookup_string_prop_meta(js, obj, prop_str, (size_t)prop_len, out)) return true;
  if (!existing.obj || !existing.obj->shape) return false;

  const ant_shape_prop_t *prop = ant_shape_prop_at(existing.obj->shape, existing.slot);
  if (!prop) return false;

  prop_meta_from_shape(out, prop);
  return true;
}

static bool define_desc_field(
  ant_t *js, ant_value_t descriptor, const char *name, size_t name_len,
  bool *has_out, ant_value_t *val_out, ant_value_t *err_out
) {
  ant_prop_loc_t off = lkp(js, descriptor, name, name_len);
  if (off.obj) {
    const ant_shape_prop_t *meta = prop_shape_meta(off);
    if (!meta || (!meta->has_getter && !meta->has_setter)) {
      *has_out = true;
      *val_out = js_prop_load(off);
      return true;
    }
  } else if (!lkp_proto(js, descriptor, name, name_len).obj) return true;

  ant_value_t v = js_getprop_fallback_len(js, descriptor, name, name_len);

  if (is_err(v)) {
    *err_out = v;
    return false;
  }

  if (Ant_Exception_Pending(js)) {
    *err_out = js_throw(js, js_take_thrown(js, js_mkundef()));
    return false;
  }

  *has_out = true;
  *val_out = v;
  
  return true;
}

enum {
  DESC_ENUMERABLE,
  DESC_CONFIGURABLE,
  DESC_VALUE,
  DESC_WRITABLE,
  DESC_GET,
  DESC_SET,
  DESC_FIELD_COUNT
};

typedef struct {
  bool has[DESC_FIELD_COUNT];
  ant_value_t val[DESC_FIELD_COUNT];
} define_desc_raw_t;

static bool define_desc_read_plain(ant_t *js, ant_value_t descriptor, define_desc_raw_t *raw) {
  ant_object_t *ptr = js_obj_ptr(descriptor);
  if (!ptr || ptr->flags.is_exotic || !ptr->shape) return false;

  const char *names[DESC_FIELD_COUNT] = {
    js->intern.enumerable,
    js->intern.configurable,
    js->intern.value,
    js->intern.writable,
    js->intern.get,
    js->intern.set,
  };

  bool missing = false;
  for (int f = 0; f < DESC_FIELD_COUNT; f++) {
    int32_t slot = ant_shape_lookup_interned(ptr->shape, names[f]);
    const ant_shape_prop_t *prop = slot >= 0 ? ant_shape_prop_at(ptr->shape, (uint32_t)slot) : NULL;
    if (!prop) { missing = true; continue; }
    if (prop->has_getter || prop->has_setter) return false;
    raw->has[f] = true;
    raw->val[f] = ant_object_prop_get_unchecked(ptr, (uint32_t)slot);
  }

  int depth = 0;
  for (ant_value_t proto = ptr->proto; missing && is_object_type(proto);) {
    const ant_object_t *p = js_obj_ptr(proto);
    if (!p || p->flags.is_exotic || !p->shape || ++depth > MAX_PROTO_CHAIN_DEPTH) return false;
    for (int f = 0; f < DESC_FIELD_COUNT; f++)
      if (!raw->has[f] && ant_shape_lookup_interned(p->shape, names[f]) >= 0) return false;
    proto = p->proto;
  }
  
  return true;
}

static inline bool define_desc_accessor_invalid(ant_value_t fn) {
  uint8_t t = vtype(fn);
  return t != kTypeFunction && t != kTypeBuiltin && t != kTypeUndefined;
}

static const struct { const char *name; size_t len; } define_desc_field_names[DESC_FIELD_COUNT] = {
  { "enumerable", 10 },
  { "configurable", 12 },
  { "value", 5 },
  { "writable", 8 },
  { "get", 3 },
  { "set", 3 },
};

static void describe_value_text(ant_t *js, ant_value_t value, char *buf, size_t cap) {
  uint8_t t = vtype(value);
  if (t == kTypeArray) {
    snprintf(buf, cap, "%s", js_is_arguments(value) ? "[object Arguments]" : "[object Array]");
    return;
  }
  if (is_object_type(value) && t != kTypeFunction) {
    ant_offset_t len = 0;
    const char *name = get_class_name(js, value, &len, NULL);
    snprintf(buf, cap, "#<%.*s>", name ? (int)len : 6, name ? name : "Object");
    return;
  }
  char stack_buf[96];
  js_cstr_t text = js_to_cstr(js, value, stack_buf, sizeof(stack_buf));
  snprintf(buf, cap, "%.*s", (int)text.len, text.ptr ? text.ptr : "");
  if (text.needs_free) free((void *)text.ptr);
}

static __attribute__((noinline, cold)) ant_value_t descriptor_value_error(ant_t *js, const char *prefix, ant_value_t value) {
  char text[128];
  describe_value_text(js, value, text, sizeof(text));
  return js_mkerr(js, "%s%s", prefix, text);
}

static ant_value_t define_desc_check(ant_t *js, ant_value_t descriptor, const define_desc_raw_t *raw) {
  if (raw->has[DESC_GET] && define_desc_accessor_invalid(raw->val[DESC_GET]))
    return descriptor_value_error(js, "Getter must be a function: ", raw->val[DESC_GET]);
  if (raw->has[DESC_SET] && define_desc_accessor_invalid(raw->val[DESC_SET]))
    return descriptor_value_error(js, "Setter must be a function: ", raw->val[DESC_SET]);
  if ((raw->has[DESC_VALUE] || raw->has[DESC_WRITABLE]) && (raw->has[DESC_GET] || raw->has[DESC_SET]))
    return descriptor_value_error(js, "Invalid property descriptor. Cannot both specify accessors and a value or writable attribute, ", descriptor);
  return js_mkundef();
}

static ant_value_t define_desc_read(ant_t *js, ant_value_t descriptor, define_desc_raw_t *raw) {
  const __typeof__(define_desc_field_names[0]) *fields = define_desc_field_names;

  bool proxy = is_proxy(descriptor);
  if (proxy || !define_desc_read_plain(js, descriptor, raw)) for (int f = 0; f < DESC_FIELD_COUNT; f++) {
    ant_value_t err = 0;
    
    if (proxy) {
      ant_value_t has = proxy_has(js, descriptor, fields[f].name, fields[f].len);
      
      if (is_err(has)) return has;
      if (!js_truthy(js, has)) continue;
      
      ant_value_t val = proxy_get(js, descriptor, fields[f].name, fields[f].len);
      if (is_err(val)) return val;
      
      raw->has[f] = true;
      raw->val[f] = val;
    } 
    
    else if (!define_desc_field(js, descriptor, fields[f].name, fields[f].len, &raw->has[f], &raw->val[f], &err)) return err;
    if (f == DESC_GET && raw->has[f] && define_desc_accessor_invalid(raw->val[f]))
      return descriptor_value_error(js, "Getter must be a function: ", raw->val[f]);
  }

  return define_desc_check(js, descriptor, raw);
}

static void array_materialize_dense_for_define(ant_t *js, ant_value_t obj, const char *key, size_t klen) {
  if (!key || !array_obj_ptr(obj)) return;

  unsigned long idx = 0;
  if (!parse_array_index(key, klen, ((ant_offset_t)UINT32_MAX), &idx)) return;

  ant_offset_t doff = get_dense_buf(obj);
  if (!doff) return;

  ant_offset_t cap = dense_capacity(doff);
  bool moved = false;

  for (ant_offset_t i = 0; i < cap; i++) {
    ant_value_t existing = dense_get(doff, i);
    if (is_empty_slot(existing)) continue;

    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);

    dense_set(js, doff, i, T_EMPTY);
    mkprop(
      js, obj, js_mkstr(js, idxstr, idxlen), existing,
      ANT_PROP_ATTR_ENUMERABLE | ANT_PROP_ATTR_WRITABLE | ANT_PROP_ATTR_CONFIGURABLE
    );
    moved = true;
  }

  if (!moved) return;

  ant_object_t *ptr = array_obj_ptr(obj);
  if (ptr) ptr->flags.fast_array = 0;
  array_mark_may_have_holes(obj);
}

static __attribute__((noinline)) bool string_wrapper_define(
  ant_t *js, ant_value_t obj, ant_value_t as_obj, const char *key, size_t key_len,
  const define_desc_fields_t *desc, ant_value_t *result
) {
  ant_value_t current = js_mkundef();
  bool is_length = false;
  if (!js_try_get_string_own_exotic(js, as_obj, key, key_len, &current, &is_length)) return false;

  bool compatible =
    !desc->accessor && !(desc->has_writable && desc->writable) &&
    !(desc->has_configurable && desc->configurable) &&
    !(desc->has_enumerable && desc->enumerable == is_length) &&
    !(desc->has_value && !same_value_values(js, desc->value, current));
    
  *result = compatible ? obj : js_mkerr_typed(js, JS_ERR_TYPE, "Cannot redefine property: %.*s", (int)key_len, key);
  return true;
}

static ant_value_t define_desc_object(ant_t *js, const define_desc_raw_t *raw) {
  GC_ROOT_SAVE(mark, js);
  define_desc_raw_t pinned = *raw;
  for (int f = 0; f < DESC_FIELD_COUNT; f++) GC_ROOT_PIN(js, pinned.val[f]);
  
  ant_value_t object = Ant_Object_CreateBare(js);
  GC_ROOT_PIN(js, object);
  
  for (int f = 0; f < DESC_FIELD_COUNT && !is_err(object); f++) if (pinned.has[f]) {
    ant_value_t key = js_mkstr(js, define_desc_field_names[f].name, define_desc_field_names[f].len);
    ant_value_t set = js_setprop(js, object, key, pinned.val[f]);
    if (is_err(set)) object = set;
  }
  
  GC_ROOT_RESTORE(js, mark);
  return object;
}

// TODO: decompose this huge function into small pieces
static ant_value_t object_define_property_keyed(
  ant_t *js, ant_value_t obj, property_key_view_t *key, 
  ant_value_t descriptor, const define_desc_raw_t *pre_raw, define_desc_fields_t *fields
) {
  bool sym_key = key->is_symbol;
  ant_value_t prop = key->js_key;

  if (vtype(descriptor) == kTypeBuiltin) descriptor = js_cfunc_promote(js, descriptor);
  if (!pre_raw && !is_object_type(descriptor)) return descriptor_value_error(js, "Property description must be an object: ", descriptor);
  
  ant_value_t as_obj = js_as_obj(obj);
  if (is_proxy(as_obj)) {
    if (!is_object_type(descriptor)) descriptor = define_desc_object(js, pre_raw);
    if (is_err(descriptor)) return descriptor;
    prop = property_key_view_materialize(js, key);
    if (is_err(prop)) return prop;
    ant_value_t proxy_result = proxy_define_property(js, as_obj, prop, descriptor);
    if (is_err(proxy_result)) return proxy_result;
    if (!js_truthy(js, proxy_result))
      return js_mkerr_typed(js, JS_ERR_TYPE, "'defineProperty' on proxy: trap returned falsy");
    return obj;
  }
  
  ant_offset_t prop_len = 0;
  const char *prop_str = NULL;
  ant_offset_t sym_off = 0;
  
  if (sym_key) {
    sym_off = (ant_offset_t)vdata(prop);
    const char *desc = js_sym_desc(prop);
    prop_str = desc ? desc : "symbol";
    prop_len = (ant_offset_t)strlen(prop_str);
  } else {
    prop_str = key->bytes;
    prop_len = (ant_offset_t)key->length;
  }
    
  bool has_value = false, has_get = false, has_set = false;
  bool has_writable = false, has_enumerable = false, has_configurable = false;
  ant_value_t value = js_mkundef();
  ant_value_t getter_val = js_mkundef();
  ant_value_t setter_val = js_mkundef();
  bool writable = false, enumerable = false, configurable = false;

  {
    define_desc_raw_t raw = {0};
    if (pre_raw) raw = *pre_raw;
    else {
      ant_value_t read_err = define_desc_read(js, descriptor, &raw);
      if (is_err(read_err)) return read_err;
    }

    has_enumerable = raw.has[DESC_ENUMERABLE];
    enumerable = has_enumerable && js_truthy(js, raw.val[DESC_ENUMERABLE]);
    
    has_configurable = raw.has[DESC_CONFIGURABLE];
    configurable = has_configurable && js_truthy(js, raw.val[DESC_CONFIGURABLE]);
    has_value = raw.has[DESC_VALUE];
    
    if (has_value) value = raw.val[DESC_VALUE];
    has_writable = raw.has[DESC_WRITABLE];
    writable = has_writable && js_truthy(js, raw.val[DESC_WRITABLE]);
    has_get = raw.has[DESC_GET];
    
    if (has_get) getter_val = raw.val[DESC_GET];
    has_set = raw.has[DESC_SET];
    
    if (has_set) setter_val = raw.val[DESC_SET];
  }

  if ((has_value || has_writable) && (has_get || has_set))
    return descriptor_value_error(js, "Invalid property descriptor. Cannot both specify accessors and a value or writable attribute, ", descriptor);

  define_desc_fields_t desc_fields = {
    .accessor = has_get || has_set, .has_value = has_value,
    .has_writable = has_writable, .writable = writable,
    .has_enumerable = has_enumerable, .enumerable = enumerable,
    .has_configurable = has_configurable, .configurable = configurable,
    .value = value,
  };
  
  if (fields) *fields = desc_fields;

  const ant_object_t *def_ptr = js_obj_ptr(as_obj);
  if (!sym_key && def_ptr && def_ptr->flags.is_exotic && def_ptr->exotic_ops && def_ptr->exotic_ops->definer) {
    bool plain_data =
      !has_get && !has_set && !(has_configurable && !configurable) &&
      !(has_enumerable && !enumerable) && !(has_writable && !writable);
    ant_value_t defined = def_ptr->exotic_ops->definer(js, as_obj, prop_str, (size_t)prop_len, value, has_value, plain_data);
    if (defined == T_EMPTY) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot define property %.*s", (int)prop_len, prop_str);
    if (is_err(defined)) return define_abrupt(fields, defined);
    if (vtype(defined) != kTypeUndefined) return obj;
  }

  ant_object_t *wrapper_ptr = !sym_key && vtype(as_obj) == kTypeObject ? (ant_object_t *)vptr(as_obj) : NULL;
  if (wrapper_ptr && wrapper_ptr->extra_slots) {
    define_desc_fields_t wrapper_desc = desc_fields;
    ant_value_t wrapper_result;
    if (string_wrapper_define(js, obj, as_obj, prop_str, (size_t)prop_len, &wrapper_desc, &wrapper_result)) return wrapper_result;
  }

  ant_object_t *arr_ptr = 
    (!sym_key && is_length_key(prop_str, prop_len))
    ? real_array_obj_ptr(as_obj) : NULL;
    
  if (arr_ptr) {
    if (has_get || has_set) return js_mkerr(js, "Invalid property descriptor. Cannot use accessors for array length");
    if ((has_enumerable && enumerable) || (has_configurable && configurable))
      return js_mkerr(js, "Cannot redefine array length attributes");

    ant_offset_t new_len = (ant_offset_t)arr_ptr->u.array.len;
    if (has_value && vtype(value) != kTypeNumber) {
      double number = js_to_number(js, value);
      if (Ant_Exception_Pending(js)) return define_abrupt(fields, Ant_Exception_Current(js));
      value = js_mknum(number);
    }
    if (has_value) {
      ant_value_t len_err = validate_array_length(js, value);
      if (is_err(len_err)) return define_abrupt(fields, len_err);
      new_len = (ant_offset_t)tod(value);
    }
    
    if (array_length_readonly(js, as_obj)) {
      if ((has_writable && writable) || new_len != get_array_length(js, as_obj))
        return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot redefine property: length");
      return obj;
    }

    ant_offset_t doff = get_dense_buf(as_obj);
    if (doff) {
      ant_offset_t cap = dense_capacity(doff);
      ant_offset_t cur_len = get_array_length(js, as_obj);
      ant_offset_t clear_to = (cur_len < cap) ? cur_len : cap;
      if (new_len < clear_to) {
        for (ant_offset_t i = new_len; i < clear_to; i++) dense_set(js, doff, i, T_EMPTY);
      }
    }
    
    if (new_len > get_array_length(js, as_obj)) array_mark_may_have_holes(as_obj);
    array_len_set(js, as_obj, new_len);
    if (has_writable && !writable && !js_array_make_length_readonly(js, as_obj)) return js_mkerr(js, "oom");
    
    return obj;
  }
  
  unsigned long define_idx = 0;
  if (
    !sym_key && real_array_obj_ptr(as_obj) &&
    parse_array_index(prop_str, (size_t)prop_len, ((ant_offset_t)UINT32_MAX), &define_idx) &&
    (ant_offset_t)define_idx >= get_array_length(js, as_obj) && array_length_readonly(js, as_obj)
  ) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot define property %lu, object is not extensible", define_idx);
  
  if (!sym_key && !has_get && !has_set && has_value
      && has_writable && writable
      && has_enumerable && enumerable
      && has_configurable && configurable) {
    ant_object_t *fast_arr = array_obj_ptr(as_obj);
    unsigned long fast_idx = 0;

    if (
      fast_arr && fast_arr->flags.fast_array &&
      fast_arr->flags.extensible && !fast_arr->flags.sealed && !fast_arr->flags.frozen &&
      parse_array_index(prop_str, (size_t)prop_len, ((ant_offset_t)UINT32_MAX), &fast_idx)
    ) {
      ant_offset_t doff = get_dense_buf(as_obj);
      if (doff && (ant_offset_t)fast_idx < dense_capacity(doff)) {
        dense_set(js, doff, (ant_offset_t)fast_idx, value);
        array_define_or_set_index(js, as_obj, prop_str, (size_t)prop_len);
        return obj;
      }
    }
  }

  if (!sym_key) array_materialize_dense_for_define(js, as_obj, prop_str, (size_t)prop_len);

  ant_prop_loc_t existing_off = sym_key
    ? lkp_sym(as_obj, sym_off)
    : lkp(js, as_obj, prop_str, prop_len);
  
  prop_meta_t existing_meta;
  bool has_existing_meta = define_lookup_existing_meta(
    js, as_obj, sym_key, sym_off, prop_str, 
    prop_len, existing_off, &existing_meta
  );
  
  bool has_existing_prop = existing_off.obj || has_existing_meta;
  ant_object_t *obj_ptr = js_obj_ptr(as_obj);
  
  if (!has_existing_prop) {
    if (obj_ptr && obj_ptr->flags.frozen)
      return js_mkerr(js, "Cannot define property %.*s, object is not extensible", (int)prop_len, prop_str);
    if (obj_ptr && obj_ptr->flags.sealed)
      return js_mkerr(js, "Cannot define property %.*s, object is not extensible", (int)prop_len, prop_str);
    if (obj_ptr && !obj_ptr->flags.extensible)
      return js_mkerr(js, "Cannot define property %.*s, object is not extensible", (int)prop_len, prop_str);
  }
  
  if (has_existing_meta) {
    if (!has_writable) writable = existing_meta.writable;
    if (!has_enumerable) enumerable = existing_meta.enumerable;
    if (!has_configurable) configurable = existing_meta.configurable;
  }

  if (has_existing_prop) {
    bool existing_accessor = has_existing_meta && (existing_meta.has_getter || existing_meta.has_setter);
    bool new_accessor = has_get || has_set;
    bool new_data = has_value || has_writable;
    bool existing_nonconfig =
      (has_existing_meta && !existing_meta.configurable) ||
      (existing_off.obj && is_nonconfig_prop(existing_off)) ||
      (obj_ptr && (obj_ptr->flags.sealed || obj_ptr->flags.frozen));
    bool existing_readonly =
      (has_existing_meta && !existing_accessor && !existing_meta.writable) ||
      (existing_off.obj && is_const_prop(existing_off)) ||
      (obj_ptr && obj_ptr->flags.frozen);
    if (existing_nonconfig) {
      if (has_configurable && configurable) return js_mkerr(js,
        "Cannot redefine property %.*s: cannot change configurable from false to true",
        (int)prop_len, prop_str
      );
      if (has_existing_meta && has_enumerable && enumerable != existing_meta.enumerable)
        return js_mkerr(js,
          "Cannot redefine property %.*s: cannot change enumerable of a non-configurable property",
          (int)prop_len, prop_str
        );
      if (has_existing_meta && !existing_meta.writable && has_writable && writable)
        return js_mkerr(js,
          "Cannot redefine property %.*s: cannot change writable from false to true",
          (int)prop_len, prop_str
        );
      if (has_existing_meta && existing_accessor && new_data)
        return js_mkerr(js,
          "Cannot redefine property %.*s: cannot convert accessor property to data property",
          (int)prop_len, prop_str
        );
      if (has_existing_meta && !existing_accessor && new_accessor)
        return js_mkerr(js,
          "Cannot redefine property %.*s: cannot convert data property to accessor property",
          (int)prop_len, prop_str
        );
      if (has_existing_meta && existing_accessor && has_get) {
        ant_value_t existing_getter = existing_meta.has_getter ? existing_meta.getter : js_mkundef();
        if (getter_val != existing_getter)
          return js_mkerr(js,
            "Cannot redefine property %.*s: cannot replace getter of a non-configurable property",
            (int)prop_len, prop_str
          );
      }
      if (has_existing_meta && existing_accessor && has_set) {
        ant_value_t existing_setter = existing_meta.has_setter ? existing_meta.setter : js_mkundef();
        if (setter_val != existing_setter)
          return js_mkerr(js,
            "Cannot redefine property %.*s: cannot replace setter of a non-configurable property",
            (int)prop_len, prop_str
          );
      }
      if (existing_readonly && has_writable && writable)
        return js_mkerr(js,
          "Cannot redefine property %.*s: cannot change writable from false to true",
          (int)prop_len, prop_str
        );
      if (existing_readonly && has_value &&
          (!existing_off.obj || !same_value_values(js, js_prop_load(existing_off), value)))
        return js_mkerr(js, "Cannot assign to read-only property '%.*s'", (int)prop_len, prop_str);
    }
  }

  if (!has_value && !has_writable && !has_get && !has_set && has_existing_prop) {
    int desc_flags =
      (writable ? JS_DESC_W : 0) |
      (enumerable ? JS_DESC_E : 0) |
      (configurable ? JS_DESC_C : 0);

    if (has_existing_meta && (existing_meta.has_getter || existing_meta.has_setter)) {
      if (sym_key) {
        if (existing_meta.has_getter) js_set_sym_getter_desc(js, as_obj, prop, existing_meta.getter, desc_flags);
        if (existing_meta.has_setter) js_set_sym_setter_desc(js, as_obj, prop, existing_meta.setter, desc_flags);
      } else {
        if (existing_meta.has_getter && existing_meta.has_setter)
          js_set_accessor_desc(js, as_obj, prop_str, prop_len, existing_meta.getter, existing_meta.setter, desc_flags);
        else if (existing_meta.has_getter)
          js_set_getter_desc(js, as_obj, prop_str, prop_len, existing_meta.getter, desc_flags);
        else
          js_set_setter_desc(js, as_obj, prop_str, prop_len, existing_meta.setter, desc_flags);
      }
    } else if (!sym_key) {
      js_set_descriptor(js, as_obj, prop_str, prop_len, desc_flags);
    } else {
      uint8_t attrs = 0;
      if (writable) attrs |= ANT_PROP_ATTR_WRITABLE;
      if (enumerable) attrs |= ANT_PROP_ATTR_ENUMERABLE;
      if (configurable) attrs |= ANT_PROP_ATTR_CONFIGURABLE;

      if (existing_off.obj) {
        if (!js_obj_ensure_unique_shape(existing_off.obj)) return js_mkerr(js, "oom");
        ant_shape_set_attrs_symbol(js, existing_off.obj->shape, sym_off, attrs);
      }
    }

    if (!sym_key) array_define_or_set_index(js, as_obj, prop_str, (size_t)prop_len);
    return obj;
  }

  if (has_get || has_set) {
    int desc_flags = 
      (enumerable ? JS_DESC_E : 0) |
      (configurable ? JS_DESC_C : 0);
    
    ant_value_t getter = has_get ? getter_val : js_mkundef();
    ant_value_t setter = has_set ? setter_val : js_mkundef();

    if (sym_key) {
      if (has_get) js_set_sym_getter_desc(js, as_obj, prop, getter, desc_flags);
      if (has_set) js_set_sym_setter_desc(js, as_obj, prop, setter, desc_flags);
    } else {
      if (has_get && has_set) js_set_accessor_desc(js, as_obj, prop_str, prop_len, getter, setter, desc_flags);
      else if (has_get) js_set_getter_desc(js, as_obj, prop_str, prop_len, getter, desc_flags);
      else js_set_setter_desc(js, as_obj, prop_str, prop_len, setter, desc_flags);
    }
  } else {
    int desc_flags = 
      (writable ? JS_DESC_W : 0) | 
      (enumerable ? JS_DESC_E : 0) | 
      (configurable ? JS_DESC_C : 0);

    uint8_t attrs = 0;
    if (writable) attrs |= ANT_PROP_ATTR_WRITABLE;
    if (enumerable) attrs |= ANT_PROP_ATTR_ENUMERABLE;
    if (configurable) attrs |= ANT_PROP_ATTR_CONFIGURABLE;

    if (existing_off.obj) {
      bool is_frozen = obj_ptr ? obj_ptr->flags.frozen : false;
      bool is_nonconfig = is_nonconfig_prop(existing_off) || is_frozen;
      bool is_readonly = is_const_prop(existing_off) || is_frozen;
      
      if (is_nonconfig) {
        if (configurable) return js_mkerr(js,
          "Cannot redefine property %.*s: cannot change configurable from false to true", 
          (int)prop_len, prop_str
        );
        
        if (is_readonly && has_writable && writable) return js_mkerr(js, 
          "Cannot redefine property %.*s: cannot change writable from false to true", 
          (int)prop_len, prop_str
        );
      }
      
      if (is_nonconfig && is_readonly && has_value && !same_value_values(js, js_prop_load(existing_off), value))
        return js_mkerr(js, "Cannot assign to read-only property '%.*s'", (int)prop_len, prop_str);
      if (has_value) {
        js_prop_store(js, existing_off, value);
        if (sym_key) ant_symbol_property_mutation_invalidate(js, existing_off.obj, sym_off);
      }
      if (!sym_key) js_set_descriptor(js, as_obj, prop_str, prop_len, desc_flags);

      if (existing_off.obj) {
        if (!js_obj_ensure_unique_shape(existing_off.obj)) return js_mkerr(js, "oom");
        if (sym_key) ant_shape_set_attrs_symbol(js, existing_off.obj->shape, sym_off, attrs);
        else ant_shape_set_attrs_interned(js, existing_off.obj->shape, intern_string(prop_str, prop_len), attrs);
        ant_shape_clear_accessor_slot(js, existing_off.obj->shape, existing_off.slot);
      }
    } else {
      if (!has_value) value = js_mkundef();      
      ant_value_t added = sym_key
        ? mkprop_exact_attrs(js, as_obj, prop, value, attrs)
        : mkprop_bytes_exact_attrs(
          js, as_obj, prop_str, 
          (size_t)prop_len, value, attrs
        );
      if (is_err(added)) return added;
    }
  }

  if (!sym_key) array_define_or_set_index(js, as_obj, prop_str, (size_t)prop_len);

  return obj;
}

static ant_value_t object_define_property_reporting(
  ant_t *js, ant_value_t obj, ant_value_t prop, ant_value_t descriptor,
  const define_desc_raw_t *raw, bool *abrupt
) {
  if (vtype(obj) == kTypeBuiltin) obj = js_cfunc_promote(js, obj);
  if (!is_object_type(obj))
    return js_mkerr(js, "Object.defineProperty called on non-object");

  char key_stack[128];
  property_key_view_t key;
  ant_value_t error = property_key_view_init(
    js, prop, &key, 
    key_stack, sizeof(key_stack)
  );
  
  if (is_err(error)) {
    property_key_view_dispose(&key);
    return error;
  }

  bool root_key = property_key_view_requires_pin(&key, is_proxy(js_as_obj(obj)));
  size_t root_mark = root_key ? gc_root_scope(js) : 0;
  if (root_key) GC_ROOT_PIN(js, key.js_key);

  define_desc_fields_t fields = {0};
  ant_value_t result = object_define_property_keyed(js, obj, &key, descriptor, raw, &fields);
  
  if (!is_err(result) && !key.is_symbol && js_is_arguments(obj))
    js_arguments_after_define(js, obj, key.bytes, key.length, &fields);
  
  property_key_view_dispose(&key);
  if (root_key) GC_ROOT_RESTORE(js, root_mark);
  if (abrupt) *abrupt = fields.abrupt;
  
  return result;
}

static ant_value_t object_define_property(
  ant_t *js, ant_value_t obj, ant_value_t prop, ant_value_t descriptor
) {
  return object_define_property_reporting(js, obj, prop, descriptor, NULL, NULL);
}

static ant_value_t builtin_object_defineProperty(ant_params_t) {
  if (nargs < 3) return js_mkerr(js, "Object.defineProperty requires 3 arguments");
  return object_define_property(js, args[0], args[1], args[2]);
}

static ant_value_t define_desc_snapshot(ant_t *js, ant_value_t descriptor) {
  if (vtype(descriptor) == kTypeBuiltin) descriptor = js_cfunc_promote(js, descriptor);
  if (!is_object_type(descriptor)) return descriptor_value_error(js, "Property description must be an object: ", descriptor);
  
  define_desc_raw_t raw = {0};
  ant_value_t err = define_desc_read(js, descriptor, &raw);
  return is_err(err) ? err : define_desc_object(js, &raw);
}

static ant_value_t reflect_define_property(ant_t *js, ant_value_t obj, ant_value_t prop, ant_value_t descriptor) {
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, obj);
  
  char key_stack[128];
  property_key_view_t key;
  
  ant_value_t key_val = property_key_view_init(js, prop, &key, key_stack, sizeof(key_stack));
  if (!is_err(key_val)) key_val = property_key_view_materialize(js, &key);
  property_key_view_dispose(&key);
  
  if (is_err(key_val)) {
    GC_ROOT_RESTORE(js, mark);
    return key_val;
  }
  
  GC_ROOT_PIN(js, key_val);
  ant_value_t snapshot = define_desc_snapshot(js, descriptor);
  
  if (is_err(snapshot)) {
    GC_ROOT_RESTORE(js, mark);
    return snapshot;
  }
  
  GC_ROOT_PIN(js, snapshot);
  if (is_object_type(obj) && is_proxy(js_as_obj(obj))) {
    ant_value_t trapped = proxy_define_property(js, js_as_obj(obj), key_val, snapshot);
    GC_ROOT_RESTORE(js, mark);
    return is_err(trapped) ? trapped : js_bool(js_truthy(js, trapped));
  }
  
  ant_value_t saved = Ant_Exception_Peek(js);
  GC_ROOT_PIN(js, saved);
  bool abrupt = false;
  ant_value_t result = object_define_property_reporting(js, obj, key_val, snapshot, NULL, &abrupt);
  
  if (is_err(result) && !abrupt) {
    Ant_Exception_Set(js, saved);
    result = js_false;
  } else if (!is_err(result)) result = js_true;
  
  GC_ROOT_RESTORE(js, mark);
  return result;
}

ant_value_t js_define_property(ant_t *js, ant_value_t obj, ant_value_t prop, ant_value_t descriptor, bool reflect_mode) {
  return reflect_mode
    ? reflect_define_property(js, obj, prop, descriptor)
    : object_define_property(js, obj, prop, descriptor);
}

static ant_value_t object_define_properties_from_slots(
  ant_t *js, ant_value_t obj, ant_object_t *ptr, uint32_t count,
  const define_desc_raw_t *raws, bool symbols
) {
  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop || !(ant_shape_get_attrs(ptr->shape, i) & ANT_PROP_ATTR_ENUMERABLE)) continue;
    if ((prop->type == ANT_SHAPE_KEY_SYMBOL) != symbols) continue;
    ant_value_t key = symbols
      ? mkval(kTypeSymbol, prop->key.sym_off)
      : js_mkstr(js, prop->key.interned, intern_length(prop->key.interned));
    ant_value_t desc = ant_object_prop_get_unchecked(ptr, i);
    ant_value_t result = object_define_property_reporting(js, obj, key, desc, &raws[i], NULL);
    if (is_err(result)) return result;
  }
  return obj;
}

static ant_value_t object_define_properties_from_data(ant_t *js, ant_value_t obj, ant_value_t props, bool *handled) {
  *handled = false;
  
  if (vtype(props) != kTypeObject || is_proxy(props) || is_proxy(js_as_obj(obj))) return js_mkundef();
  if (js_as_obj(obj) == props) return js_mkundef();
  ant_object_t *ptr = js_obj_ptr(props);
  
  if (!ptr || !ptr->shape || ptr->flags.is_exotic || ant_object_extra_count(ptr) != 0) return js_mkundef();
  if (ant_shape_may_have_index_keys(ptr->shape)) return js_mkundef();

  uint32_t count = ant_shape_count(ptr->shape);
  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop || i >= ptr->prop_count) return js_mkundef();
    if (prop->has_getter || prop->has_setter) return js_mkundef();
  }

  define_desc_raw_t stack_raws[32];
  define_desc_raw_t *raws = count <= 32 ? stack_raws : calloc(count, sizeof(*raws));
  
  if (!raws) return js_mkerr(js, "oom");
  if (raws == stack_raws) memset(stack_raws, 0, count * sizeof(*raws));

  ant_value_t result = js_mkundef();
  bool target_is_descriptor = false;
  
  for (uint32_t i = 0; i < count; i++) {
    if (!(ant_shape_get_attrs(ptr->shape, i) & ANT_PROP_ATTR_ENUMERABLE)) continue;
    ant_value_t desc = ant_object_prop_get_unchecked(ptr, i);
    if (vtype(desc) == kTypeBuiltin) goto done;
    target_is_descriptor |= desc == js_as_obj(obj);
    
    if (!is_object_type(desc)) {
      *handled = true;
      result = descriptor_value_error(js, "Property description must be an object: ", desc);
      goto done;
    }
    
    if (!define_desc_read_plain(js, desc, &raws[i])) goto done;
    
    ant_value_t err = define_desc_check(js, desc, &raws[i]);
    if (is_err(err)) {
      *handled = true;
      result = err;
      goto done;
    }
  }

  *handled = true;
  GC_ROOT_SAVE(root_mark, js);
  for (uint32_t i = 0; target_is_descriptor && i < count; i++)
    for (int f = 0; f < DESC_FIELD_COUNT; f++) if (raws[i].has[f]) GC_ROOT_PIN(js, raws[i].val[f]);

  result = object_define_properties_from_slots(js, obj, ptr, count, raws, false);
  if (!is_err(result)) result = object_define_properties_from_slots(js, obj, ptr, count, raws, true);
  GC_ROOT_RESTORE(js, root_mark);

done:
  if (raws != stack_raws) free(raws);
  return result;
}

static ant_value_t object_define_properties_get(ant_t *js, ant_value_t source, bool proxied, ant_value_t key) {
  if (proxied) return proxy_get_val(js, source, key);
  if (vtype(key) == kTypeSymbol) return js_get_sym(js, source, key);
  
  ant_offset_t key_len = 0;
  ant_offset_t key_off = vstr(js, key, &key_len);
  
  return js_getprop_fallback_len(js, source, (const char *)(uintptr_t)key_off, (size_t)key_len);
}

static ant_value_t object_define_properties(ant_t *js, ant_value_t obj, ant_value_t props) {
  uint8_t pt = vtype(props);
  if (pt == kTypeUndefined || pt == kTypeNull)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert undefined or null to object");

  if (pt == kTypeString) {
    props = builtin_Object(js, &props, 1, js_mkundef());
    if (is_err(props)) return props;
  } else if (pt == kTypeBuiltin) props = js_cfunc_promote(js, props);
  else if (!is_object_type(props)) return obj;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, obj);
  GC_ROOT_PIN(js, props);

  bool handled = false;
  ant_value_t result = object_define_properties_from_data(js, obj, props, &handled);
  if (handled) {
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }

  ant_value_t source = js_as_obj(props);
  bool proxied = is_proxy(source);
  
  ant_value_t keys = proxied ? proxy_enum(js, source, OBJ_ENUM_KEYS) : js_own_property_keys(js, props, true, true);
  GC_ROOT_PIN(js, keys);
  
  ant_value_t descriptors = is_err(keys) ? keys : mkarr(js);
  GC_ROOT_PIN(js, descriptors);
  
  if (is_err(descriptors)) {
    GC_ROOT_RESTORE(js, root_mark);
    return descriptors;
  }

  ant_offset_t key_count = js_arr_len(js, keys);
  for (ant_offset_t i = 0; i < key_count; i++) {
    ant_value_t desc = object_define_properties_get(js, source, proxied, js_arr_get(js, keys, i));
    if (vtype(desc) == kTypeBuiltin) desc = js_cfunc_promote(js, desc);
    if (!is_err(desc) && !is_object_type(desc))
      desc = descriptor_value_error(js, "Property description must be an object: ", desc);
    if (is_err(desc)) {
      GC_ROOT_RESTORE(js, root_mark);
      return desc;
    }
    js_arr_push(js, descriptors, desc);
  }

  for (ant_offset_t i = 0; i < key_count; i++) {
    ant_value_t desc = define_desc_snapshot(js, js_arr_get(js, descriptors, i));
    if (is_err(desc)) {
      GC_ROOT_RESTORE(js, root_mark);
      return desc;
    }
    arr_set(js, descriptors, i, desc);
  }

  for (ant_offset_t i = 0; i < key_count; i++) {
    result = object_define_property(js, obj, js_arr_get(js, keys, i), js_arr_get(js, descriptors, i));
    if (is_err(result)) {
      GC_ROOT_RESTORE(js, root_mark);
      return result;
    }
  }

  GC_ROOT_RESTORE(js, root_mark);
  return obj;
}

static ant_value_t builtin_object_defineProperties(ant_params_t) {
  ant_value_t obj = nargs >= 1 ? args[0] : js_mkundef();
  uint8_t t = vtype(obj);
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction)
    return js_mkerr(js, "Object.defineProperties called on non-object");
  return object_define_properties(js, obj, nargs >= 2 ? args[1] : js_mkundef());
}

bool js_is_own_enumerable_prop(
  ant_t *js, ant_value_t source, ant_object_t *source_ptr,
  const ant_iter_key_t *key
) {
  if (!key) return false;

  if (key->is_symbol) {
    if (!source_ptr || source_ptr->flags.is_exotic) {
      prop_meta_t meta;
      return !lookup_symbol_prop_meta(js, js_as_obj(source), key->sym_off, &meta) || meta.enumerable;
    }
    return (ant_shape_get_attrs(source_ptr->shape, key->slot) & ANT_PROP_ATTR_ENUMERABLE) != 0;
  }

  if (!key->str) return false;

  if (!source_ptr || source_ptr->flags.is_exotic) {
    descriptor_entry_t *desc = lookup_descriptor(js, js_as_obj(source), key->str, key->key_len);
    return !desc || desc->enumerable;
  }

  return (ant_shape_get_attrs(source_ptr->shape, key->slot) & ANT_PROP_ATTR_ENUMERABLE) != 0;
}

typedef struct {
  ant_value_t sym;
  const char *interned;
} copy_key_t;

typedef struct {
  const copy_key_t *keys;
  int count;
  bool assign;
  bool collect;
  enum obj_enum_mode mode;
} copy_ctx_t;

static bool copy_key_of(ant_t *js, ant_value_t key, copy_key_t *out) {
  if (vtype(key) == kTypeSymbol) {
    *out = (copy_key_t){ .sym = key };
    return true;
  }
  
  size_t len = 0;
  const char *str = vtype(key) == kTypeString ? js_getstr(js, key, &len) : NULL;
  *out = (copy_key_t){ .sym = js_mkundef(), .interned = str ? intern_string(str, len) : NULL };
  
  return out->interned != NULL;
}

static bool copy_excluded_key_of(ant_t *js, ant_value_t key, copy_key_t *out) {
  ant_flat_string_t *flat = ant_str_flat_ptr(key);
  const char *interned = flat ? intern_find_flat(js, flat->bytes, (size_t)flat->len) : NULL;
  if (!interned) return copy_key_of(js, key, out);
  *out = (copy_key_t){ .sym = js_mkundef(), .interned = interned };
  return true;
}

static inline bool copy_key_is_sym(copy_key_t key) {
  return vtype(key.sym) == kTypeSymbol;
}

static inline int32_t copy_key_slot(ant_shape_t *shape, copy_key_t key) {
  return copy_key_is_sym(key)
    ? ant_shape_lookup_symbol(shape, (ant_offset_t)vdata(key.sym))
    : ant_shape_lookup_interned(shape, key.interned);
}

static inline ant_value_t copy_key_get(ant_t *js, ant_value_t src, copy_key_t key) {
  return copy_key_is_sym(key) ? js_get_sym(js, src, key.sym) : js_get(js, src, key.interned);
}

static bool copy_key_excluded(const copy_ctx_t *cx, copy_key_t key) {
  for (int i = 0; i < cx->count; i++)
    if (cx->keys[i].sym == key.sym && cx->keys[i].interned == key.interned) return true;
  return false;
}

static ant_value_t copy_define_slow(ant_t *js, ant_value_t dst, copy_key_t key, ant_value_t val) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, val);
  
  ant_value_t key_val = copy_key_is_sym(key)
    ? key.sym 
    : js_mkstr(js, key.interned, intern_length(key.interned));
    
  GC_ROOT_PIN(js, key_val);
  
  ant_value_t desc = Ant_Object_CreateBare(js);
  GC_ROOT_PIN(js, desc);
  
  js_set(js, desc, "value", val);
  js_set(js, desc, "writable", js_true);
  js_set(js, desc, "enumerable", js_true);
  js_set(js, desc, "configurable", js_true);
  
  ant_value_t result = object_define_property(js, dst, key_val, desc);
  GC_ROOT_RESTORE(js, root_mark);
  
  return is_err(result) ? result : val;
}

static ant_value_t copy_define(ant_t *js, ant_value_t dst, copy_key_t key, ant_value_t val) {
  ant_object_t *ptr = js_obj_ptr(dst);
  if (vtype(dst) != kTypeObject || !ptr || !ptr->shape || ptr->flags.is_exotic || !ptr->flags.extensible)
    return copy_define_slow(js, dst, key, val);
  
  bool is_sym = copy_key_is_sym(key);
  int32_t slot = copy_key_slot(ptr->shape, key);
  
  if (slot < 0) return is_sym
    ? mkprop_symbol_attrs_impl(js, dst, key.sym, val, 0, MKPROP_USE_DEFAULT_ATTRS)
    : mkprop_interned_exact(js, dst, key.interned, val, 0);
  
  const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, (uint32_t)slot);
  
  bool plain = 
    prop && !prop->has_getter && !prop->has_setter &&
    ant_shape_get_attrs(ptr->shape, (uint32_t)slot) == ANT_PROP_ATTR_DEFAULT;
  
  if (!plain || (uint32_t)slot >= ptr->prop_count) return copy_define_slow(js, dst, key, val);
  
  ant_object_prop_set_unchecked(ptr, (uint32_t)slot, val);
  gc_write_barrier(js, ptr, val);
  
  if (is_sym) ant_symbol_property_mutation_invalidate(js, ptr, (ant_offset_t)vdata(key.sym));
  else ant_property_mutation_invalidate(js, ptr, key.interned);
  
  return val;
}

static bool chain_lacks_plain_keys(ant_value_t start, const char *first, const char *second) {
  for (int depth = 0; depth < MAX_PROTO_CHAIN_DEPTH; depth++) {
    if (!is_object_type(start)) return true;
    ant_object_t *ptr = js_obj_ptr(js_as_obj(start));
    if (!ptr || ptr->flags.is_exotic) return false;
    if (ptr->shape && ant_shape_lookup_interned(ptr->shape, first) >= 0) return false;
    if (second && ptr->shape && ant_shape_lookup_interned(ptr->shape, second) >= 0) return false;
    start = ptr->proto;
  }
  
  return false;
}

static inline bool chain_lacks_plain_key(ant_value_t start, const char *interned) {
  return chain_lacks_plain_keys(start, interned, NULL);
}

static inline bool is_plain_set_target(const ant_object_t *ptr) {
  return ptr && ptr->shape && !ptr->flags.is_exotic && ptr->type_tag == kTypeObject;
}

static inline bool own_writable_data_slot(const ant_object_t *ptr, int32_t slot) {
  if (slot < 0 || (uint32_t)slot >= ptr->prop_count) return false;
  const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, (uint32_t)slot);
  return prop && !prop->has_getter && !prop->has_setter &&
    (ant_shape_get_attrs(ptr->shape, (uint32_t)slot) & ANT_PROP_ATTR_WRITABLE);
}

static inline void store_own_data_slot(ant_t *js, ant_object_t *ptr, uint32_t slot, const char *interned, ant_value_t val) {
  ant_object_prop_set_unchecked(ptr, slot, val);
  gc_write_barrier(js, ptr, val);
  ant_property_mutation_invalidate(js, ptr, interned);
}

static ant_value_t copy_set(ant_t *js, ant_value_t dst, copy_key_t key, ant_value_t val) {
  ant_object_t *ptr = js_obj_ptr(dst);
  
  bool plain =
    !copy_key_is_sym(key) && vtype(dst) == kTypeObject && is_plain_set_target(ptr) &&
    vtype(js_string_wrapper_value(dst)) != kTypeString;
  
  int32_t slot = plain ? ant_shape_lookup_interned(ptr->shape, key.interned) : -1;
  if (plain && own_writable_data_slot(ptr, slot)) {
    store_own_data_slot(js, ptr, (uint32_t)slot, key.interned, val);
    return val;
  }
  
  if (plain && slot < 0 && ptr->flags.extensible && chain_lacks_plain_key(ptr->proto, key.interned))
    return mkprop_interned_exact(js, dst, key.interned, val, 0);
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, val);
  
  ant_value_t key_val = copy_key_is_sym(key)
    ? key.sym
    : js_mkstr(js, key.interned, intern_length(key.interned));
  
  GC_ROOT_PIN(js, key_val);
  ant_value_t result = is_err(key_val) ? key_val : js_setprop_throw(js, dst, key_val, false, 0, val);
  
  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static __attribute__((noinline)) ant_value_t copy_collect(ant_t *js, const copy_ctx_t *cx, ant_value_t arr, copy_key_t key, ant_value_t val) {
  if (!copy_key_is_sym(key)) js_arr_push(js, arr, object_enum_item(js, cx->mode, key.interned, intern_length(key.interned), val));
  return js_mkundef();
}

static inline ant_value_t copy_store(ant_t *js, const copy_ctx_t *cx, ant_value_t dst, copy_key_t key, ant_value_t val) {
  return cx->assign ? copy_set(js, dst, key, val) : copy_define(js, dst, key, val);
}

static inline ant_value_t copy_emit(ant_t *js, const copy_ctx_t *cx, ant_value_t dst, copy_key_t key, ant_value_t val) {
  return cx->collect ? copy_collect(js, cx, dst, key, val) : copy_store(js, cx, dst, key, val);
}

static ant_value_t copy_host_pairs(ant_t *js, ant_value_t dst, ant_proxy_state_t *data, const copy_ctx_t *cx) {
  ant_value_t pairs = proxy_host_pairs(js, data, true);
  if (is_err(pairs)) return pairs;
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, pairs);
  ant_value_t result = js_mkundef();
  
  ant_offset_t len = get_array_length(js, pairs);
  for (ant_offset_t i = 0; i + 1 < len && !is_err(result); i += 2) {
    copy_key_t ck;
    if (!copy_key_of(js, arr_get(js, pairs, i), &ck)) result = js_mkerr(js, "out of memory");
    else if (!copy_key_excluded(cx, ck)) result = copy_store(js, cx, dst, ck, arr_get(js, pairs, i + 1));
  }
  
  GC_ROOT_RESTORE(js, root_mark);
  return is_err(result) ? result : js_mkundef();
}

static ant_value_t copy_proxy_data_props(
  ant_t *js, ant_value_t dst, ant_value_t proxy, const copy_ctx_t *cx
) {
  ant_proxy_state_t *host_data = get_proxy_data(proxy);
  if (host_data && host_data->host && host_data->host->flat_entries) return copy_host_pairs(js, dst, host_data, cx);
  
  ant_value_t keys = proxy_own_keys_raw(js, proxy);
  if (is_err(keys)) return keys;
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, keys);
  ant_value_t result = js_mkundef();

  ant_offset_t key_count = js_arr_len(js, keys);
  for (ant_offset_t k = 0; k < key_count && !is_err(result); k++) {
    GC_ROOT_SAVE(key_mark, js);
    
    ant_value_t key = js_arr_get(js, keys, k);
    GC_ROOT_PIN(js, key);
    
    copy_key_t ck;
    if (!copy_key_of(js, key, &ck)) result = js_mkerr(js, "out of memory");
    else if (!copy_key_excluded(cx, ck)) {
      ant_value_t enumerable = proxy_key_enumerable(js, proxy, key);
      if (is_err(enumerable)) result = enumerable;
      else if (js_truthy(js, enumerable)) {
        ant_value_t val = proxy_get_val(js, proxy, key);
        GC_ROOT_PIN(js, val);
        result = is_err(val) ? val : copy_store(js, cx, dst, ck, val);
      }
    }
    
    GC_ROOT_RESTORE(js, key_mark);
  }

  GC_ROOT_RESTORE(js, root_mark);
  return is_err(result) ? result : js_mkundef();
}

static ant_value_t copy_string_props(ant_t *js, ant_value_t dst, ant_value_t str, const copy_ctx_t *cx) {
  ant_offset_t len = str_utf16_len(js, str);
  
  for (ant_offset_t i = 0; i < len; i++) {
    copy_key_t ck = { .sym = js_mkundef(), .interned = index_key_get_or_intern(js, (uint32_t)i) };
    if (!ck.interned) return js_mkerr(js, "out of memory");
    if (copy_key_excluded(cx, ck)) continue;
    
    ant_offset_t byte_len = 0;
    const char *data = (const char *)(uintptr_t)vstr(js, str, &byte_len);
    ant_value_t val = js_string_from_utf16_code_unit(js, utf16_code_unit_at(data, (size_t)byte_len, (size_t)i));
    
    ant_value_t result = copy_emit(js, cx, dst, ck, val);
    if (is_err(result)) return result;
  }
  
  return js_mkundef();
}

static ant_value_t copy_dense_props_assign(ant_t *js, ant_value_t dst, ant_value_t arr, const copy_ctx_t *cx) {
  ant_offset_t doff = get_dense_buf(arr);
  ant_offset_t len = doff ? dense_iterable_length(js, arr) : 0;
  
  uint32_t stack_present[64];
  uint32_t *present = len <= 64 ? stack_present : malloc((size_t)len * sizeof(*present));
  if (!present) return js_mkerr(js, "out of memory");

  uint32_t count = 0;
  for (ant_offset_t i = 0; i < len; i++) if (!is_empty_slot(dense_get(doff, i))) present[count++] = (uint32_t)i;

  ant_value_t result = js_mkundef();
  for (uint32_t k = 0; k < count && !is_err(result); k++) {
    uint32_t i = present[k];
    copy_key_t ck = { .sym = js_mkundef(), .interned = index_key_get_or_intern(js, i) };
    if (!ck.interned) { result = js_mkerr(js, "out of memory"); break; }
    if (copy_key_excluded(cx, ck)) continue;

    ant_value_t val = js_mkundef();
    doff = get_dense_buf(arr);
    bool dense_hit = doff && i < dense_iterable_length(js, arr) && !is_empty_slot(val = dense_get(doff, i));

    if (!dense_hit) {
      ant_object_t *ptr = js_obj_ptr(arr);
      int32_t slot = ptr && ptr->shape ? ant_shape_lookup_interned(ptr->shape, ck.interned) : -1;
      if (
        slot < 0 || (uint32_t)slot >= ptr->prop_count ||
        !(ant_shape_get_attrs(ptr->shape, (uint32_t)slot) & ANT_PROP_ATTR_ENUMERABLE)
      ) continue;
      val = copy_key_get(js, arr, ck);
      if (is_err(val)) { result = val; break; }
    }

    result = copy_store(js, cx, dst, ck, val);
  }

  if (present != stack_present) free(present);
  return is_err(result) ? result : js_mkundef();
}

static bool copy_dense_index_layout(ant_t *js, ant_value_t dst, const copy_ctx_t *cx, ant_offset_t doff, ant_offset_t len) {
  if (cx->count || len > UINT32_MAX || vtype(dst) != kTypeObject) return false;

  ant_object_t *dst_ptr = js_obj_ptr(dst);
  if (
    !dst_ptr || !dst_ptr->shape || ant_shape_count(dst_ptr->shape) != 0 ||
    dst_ptr->flags.is_exotic || !dst_ptr->flags.extensible ||
    vtype(js_string_wrapper_value(dst)) == kTypeString
  ) return false;

  for (ant_offset_t i = 0; i < len; i++) if (is_empty_slot(dense_get(doff, i))) return false;

  ant_shape_t *shape = ant_shape_index_layout(js, &js->index_layout_shape, dst_ptr->shape, (uint32_t)len);
  if (!shape || !js_obj_ensure_prop_capacity(js, dst_ptr, (uint32_t)len)) return false;

  ant_shape_transition_existing(&dst_ptr->shape, shape);
  ant_object_invalidate_guarded_absence(js, dst_ptr);

  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t val = dense_get(doff, i);
    ant_object_prop_set_unchecked(dst_ptr, (uint32_t)i, val);
    gc_write_barrier(js, dst_ptr, val);
  }

  return true;
}

static ant_value_t copy_dense_props(ant_t *js, ant_value_t dst, ant_value_t arr, const copy_ctx_t *cx) {
  if (cx->assign) return copy_dense_props_assign(js, dst, arr, cx);
  
  ant_offset_t doff = get_dense_buf(arr);
  ant_offset_t len = doff ? dense_iterable_length(js, arr) : 0;
  
  if (doff && copy_dense_index_layout(js, dst, cx, doff, len)) return js_mkundef();
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t val = dense_get(doff, i);
    if (is_empty_slot(val)) continue;
    
    copy_key_t ck = { .sym = js_mkundef(), .interned = index_key_get_or_intern(js, (uint32_t)i) };
    if (!ck.interned) return js_mkerr(js, "out of memory");
    if (copy_key_excluded(cx, ck)) continue;
    
    ant_value_t result = copy_store(js, cx, dst, ck, val);
    if (is_err(result)) return result;
  }
  
  return js_mkundef();
}

static copy_key_t copy_key_at(const ant_shape_prop_t *prop) {
  return prop->type == ANT_SHAPE_KEY_SYMBOL
    ? (copy_key_t){ .sym = mkval(kTypeSymbol, prop->key.sym_off) }
    : (copy_key_t){ .sym = js_mkundef(), .interned = prop->key.interned };
}

static uint32_t copy_shape_keys_in_slot_order(ant_object_t *ptr, copy_key_t *keys, uint32_t *slots) {
  uint32_t count = ant_shape_count(ptr->shape), key_count = 0;
  for (uint32_t i = 0; i < count && i < ptr->prop_count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop || prop->type == ANT_SHAPE_KEY_DELETED) continue;
    slots[key_count] = i;
    keys[key_count++] = copy_key_at(prop);
  }
  return key_count;
}

static uint32_t copy_shape_keys_ordered(ant_object_t *ptr, copy_key_t *keys, uint32_t *slots, own_index_key_t *indices) {
  ant_shape_t *shape = ptr->shape;
  uint32_t count = ant_shape_count(shape);
  uint32_t index_count = 0, key_count = 0;
  
  for (uint32_t i = 0; i < count && i < ptr->prop_count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING) continue;
    
    unsigned long index = 0;
    if (indices && own_key_is_array_index(prop->key.interned, intern_length(prop->key.interned), &index))
      indices[index_count++] = (own_index_key_t){ .slot = i, .index = index };
    else {
      slots[key_count] = i;
      keys[key_count++] = copy_key_at(prop);
    }
  }
  
  if (index_count) {
    own_index_keys_sort(indices, index_count);
    memmove(keys + index_count, keys, (size_t)key_count * sizeof(*keys));
    memmove(slots + index_count, slots, (size_t)key_count * sizeof(*slots));
    for (uint32_t k = 0; k < index_count; k++) {
      slots[k] = indices[k].slot;
      keys[k] = copy_key_at(ant_shape_prop_at(shape, indices[k].slot));
    }
    key_count += index_count;
  }
  
  for (uint32_t i = 0; i < count && i < ptr->prop_count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_SYMBOL) continue;
    slots[key_count] = i;
    keys[key_count++] = copy_key_at(prop);
  }
  
  return key_count;
}

typedef struct copy_key_snapshot {
  copy_key_t *keys;
  uint32_t *slots;
  uint32_t count;
  copy_key_t stack[32];
  uint32_t stack_slots[32];
} copy_key_snapshot_t;

static inline bool copy_key_at_slot(const ant_object_t *ptr, uint32_t slot, copy_key_t key) {
  if (slot >= ptr->prop_count || slot >= ant_shape_count(ptr->shape)) return false;
  const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, slot);
  return prop && (copy_key_is_sym(key)
    ? prop->type == ANT_SHAPE_KEY_SYMBOL && prop->key.sym_off == (ant_offset_t)vdata(key.sym)
    : prop->type == ANT_SHAPE_KEY_STRING && prop->key.interned == key.interned);
}

static void copy_key_snapshot_free(copy_key_snapshot_t *snap) {
  if (snap->keys != snap->stack) free(snap->keys);
  if (snap->slots != snap->stack_slots) free(snap->slots);
}

static bool copy_key_snapshot_take(ant_object_t *ptr, copy_key_snapshot_t *snap, bool spec_order) {
  uint32_t count = ant_shape_count(ptr->shape);
  
  bool small = count <= 32;
  bool order_indices = spec_order && ant_shape_may_have_index_keys(ptr->shape);
  
  own_index_key_t stack_indices[32];
  own_index_key_t *indices = !order_indices ? NULL
    : small ? stack_indices : malloc((size_t)count * sizeof(*indices));
  
  snap->keys = small ? snap->stack : malloc((size_t)count * sizeof(*snap->keys));
  snap->slots = small ? snap->stack_slots : malloc((size_t)count * sizeof(*snap->slots));
  
  bool ok = snap->keys && snap->slots && (indices || !order_indices);
  
  snap->count = !ok ? 0 : spec_order
    ? copy_shape_keys_ordered(ptr, snap->keys, snap->slots, indices)
    : copy_shape_keys_in_slot_order(ptr, snap->keys, snap->slots);
  
  if (indices != stack_indices) free(indices);
  if (!ok) copy_key_snapshot_free(snap);
  
  return ok;
}

static ant_value_t copy_key_snapshot_props(
  ant_t *js, ant_value_t dst, ant_value_t src, ant_object_t *ptr,
  const copy_ctx_t *cx, const copy_key_snapshot_t *snap
) {
  ant_value_t result = js_mkundef();
  for (uint32_t k = 0; k < snap->count && !is_err(result); k++) {
    copy_key_t ck = snap->keys[k];
    if (copy_key_excluded(cx, ck)) continue;
    
    int32_t slot = copy_key_at_slot(ptr, snap->slots[k], ck) ? (int32_t)snap->slots[k] : copy_key_slot(ptr->shape, ck);
    if (slot < 0 || (uint32_t)slot >= ptr->prop_count) continue;
    if ((ant_shape_get_attrs(ptr->shape, (uint32_t)slot) & ANT_PROP_ATTR_ENUMERABLE) == 0) continue;
    
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, (uint32_t)slot);
    ant_value_t val = !(prop->has_getter || prop->has_setter) ? ant_object_prop_get_unchecked(ptr, (uint32_t)slot)
      : prop->has_getter ? js_call_getter(js, src, prop->getter) : js_mkundef();
    
    result = is_err(val) ? val : copy_emit(js, cx, dst, ck, val);
  }
  
  return result;
}

static bool copy_adopt_shape(ant_t *js, ant_value_t dst, ant_object_t *src, bool via_set) {
  ant_object_t *dst_ptr = vtype(dst) == kTypeObject ? js_obj_ptr(dst) : NULL;
  ant_shape_t *shape = src->shape;
  uint32_t count = ant_shape_count(shape);
  
  if (
    count == 0 || count > src->prop_count || ant_shape_may_have_index_keys(shape) ||
    !dst_ptr || !dst_ptr->shape || ant_shape_count(dst_ptr->shape) != 0 || dst_ptr->prop_count != 0 ||
    dst_ptr->flags.is_exotic || !dst_ptr->flags.extensible || dst_ptr->type_tag != kTypeObject ||
    ant_object_inobj_limit(dst_ptr) != ant_shape_get_inobj_limit(shape) ||
    (via_set && vtype(js_string_wrapper_value(dst)) == kTypeString)
  ) return false;
  
  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING || prop->has_getter || prop->has_setter) return false;
    if (ant_shape_get_attrs(shape, i) != ANT_PROP_ATTR_DEFAULT || ant_property_key_is_watched(js, prop->key.interned)) return false;
  }
  
  for (uint32_t i = 0; via_set && i < count; i++)
    if (!chain_lacks_plain_key(dst_ptr->proto, ant_shape_prop_at(shape, i)->key.interned)) return false;
  
  if (!js_obj_ensure_prop_capacity(js, dst_ptr, count)) return false;
  ant_shape_retain(shape);
  ant_shape_release(dst_ptr->shape);
  
  dst_ptr->shape = shape;
  ant_object_invalidate_guarded_absence(js, dst_ptr);
  
  for (uint32_t i = 0; i < count; i++) {
    ant_value_t val = ant_object_prop_get_unchecked(src, i);
    ant_object_prop_set_unchecked(dst_ptr, i, val);
    gc_write_barrier(js, dst_ptr, val);
  }
  
  return true;
}

static bool copy_ordinary_data_props(
  ant_t *js, ant_value_t dst, ant_value_t src, const copy_ctx_t *cx, ant_value_t *result
) {
  bool is_arr = vtype(src) == kTypeArray;
  ant_object_t *ptr = is_arr || vtype(src) == kTypeObject ? js_obj_ptr(src) : NULL;
  
  if (!ptr || !ptr->shape || ptr->flags.is_exotic || ptr->flags.arguments_object) return false;
  ant_value_t wrapped = js_string_wrapper_value(src);
  
  ant_shape_t *shape = ptr->shape;
  uint32_t count = ant_shape_count(shape);
  
  if (count == 0 && !is_arr && vtype(wrapped) != kTypeString) {
    *result = js_mkundef();
    return true;
  }
  
  bool has_accessor = false, symbol_accessor = false;
  for (uint32_t i = 0; i < count && !symbol_accessor; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(shape, i);
    if (!prop || !(prop->has_getter || prop->has_setter)) continue;
    has_accessor = true;
    symbol_accessor = prop->type == ANT_SHAPE_KEY_SYMBOL;
  }
  
  bool index_keys = ant_shape_may_have_index_keys(shape);
  bool accessor_order_visible = symbol_accessor || (has_accessor && index_keys);
  if (is_arr && (has_accessor || cx->collect)) return false;
  
  bool indexed = is_arr || vtype(wrapped) == kTypeString;
  if (
    !has_accessor && !indexed && !cx->count && !index_keys && 
    copy_adopt_shape(js, dst, ptr, cx->assign)
  ) {
    *result = js_mkundef();
    return true;
  }
  
  copy_key_snapshot_t snap;
  bool snapshot = has_accessor || cx->assign || cx->collect;
  
  if (snapshot && !copy_key_snapshot_take(ptr, &snap, cx->assign || cx->collect || accessor_order_visible)) {
    *result = js_mkerr(js, "out of memory");
    return true;
  }
  
  *result = is_arr ? copy_dense_props(js, dst, src, cx)
    : vtype(wrapped) == kTypeString ? copy_string_props(js, dst, wrapped, cx)
    : js_mkundef();
  
  if (snapshot) {
    if (!is_err(*result)) *result = copy_key_snapshot_props(js, dst, src, ptr, cx, &snap);
    copy_key_snapshot_free(&snap);
    return true;
  }
  
  if (is_err(*result)) return true;
  
  ant_object_t *dst_ptr = js_obj_ptr(dst);
  if (dst_ptr && dst_ptr->shape && !js_obj_ensure_prop_capacity(js, dst_ptr, ant_shape_count(dst_ptr->shape) + count)) {
    *result = js_mkerr(js, "out of memory");
    return true;
  }
  
  for (uint32_t i = 0; i < count && !is_err(*result); i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(shape, i);
    if (!prop || i >= ptr->prop_count) continue;
    if ((ant_shape_get_attrs(shape, i) & ANT_PROP_ATTR_ENUMERABLE) == 0) continue;
    
    copy_key_t ck = copy_key_at(prop);
    if (copy_key_excluded(cx, ck)) continue;
    *result = copy_define(js, dst, ck, ant_object_prop_get_unchecked(ptr, i));
  }
  
  return true;
}

static ant_value_t copy_object_data_props(ant_t *js, ant_value_t dst, ant_value_t src, const copy_ctx_t *cx) {
  ant_value_t keys = js_own_property_keys(js, src, true, true);
  if (is_err(keys)) return keys;
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, keys);
  ant_value_t view = js_object_view(src);
  ant_value_t result = js_mkundef();
  
  ant_offset_t key_count = js_arr_len(js, keys);
  for (ant_offset_t k = 0; k < key_count && !is_err(result); k++) {
    GC_ROOT_SAVE(key_mark, js);
    
    ant_value_t key = js_arr_get(js, keys, k);
    GC_ROOT_PIN(js, key);
    
    copy_key_t ck;
    if (!copy_key_of(js, key, &ck)) result = js_mkerr(js, "out of memory");
    else if (!copy_key_excluded(cx, ck)) {
      ant_value_t own = Ant_Object_HasOwn(js, view, key);
      if (is_err(own)) result = own;
      else if (js_truthy(js, own)) {
        ant_value_t val = copy_key_get(js, src, ck);
        GC_ROOT_PIN(js, val);
        result = is_err(val) ? val : copy_emit(js, cx, dst, ck, val);
      }
    }
    
    GC_ROOT_RESTORE(js, key_mark);
  }
  
  GC_ROOT_RESTORE(js, root_mark);
  return is_err(result) ? result : js_mkundef();
}

static ant_value_t copy_data_props(ant_t *js, ant_value_t dst, ant_value_t src, const copy_ctx_t *cx) {
  if (vtype(src) == kTypeString) return copy_string_props(js, dst, src, cx);
  if (!is_object_type(src)) return js_mkundef();
  
  ant_value_t source = js_as_obj(src);
  if (is_proxy(source)) return copy_proxy_data_props(js, dst, source, cx);
  
  ant_value_t result;
  if (copy_ordinary_data_props(js, dst, src, cx, &result)) return result;
  return copy_object_data_props(js, dst, src, cx);
}

static ant_value_t object_enum_collect(ant_t *js, ant_value_t obj, enum obj_enum_mode mode) {
  ant_value_t arr = mkarr(js);
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, arr);
  GC_ROOT_PIN(js, obj);
  
  copy_ctx_t cx = { .collect = true, .mode = mode };
  ant_value_t result = copy_data_props(js, arr, obj, &cx);
  
  GC_ROOT_RESTORE(js, root_mark);
  return is_err(result) ? result : mkval(kTypeArray, vdata(arr));
}

static inline bool object_enum_enumerable(ant_t *js, ant_value_t obj, ant_object_t *ptr, const ant_shape_prop_t *prop) {
  if (!(prop->attrs & ANT_PROP_ATTR_ENUMERABLE)) return false;
  if (!ptr->flags.is_exotic) return true;
  descriptor_entry_t *desc = lookup_descriptor(js, js_as_obj(obj), prop->key.interned, intern_length(prop->key.interned));
  return !desc || desc->enumerable;
}

static ant_value_t object_enum_visit(
  ant_t *js, ant_value_t obj, ant_object_t *ptr, enum obj_enum_mode mode,
  object_enum_sink_t *sink, const copy_key_snapshot_t *snap
) {
  for (uint32_t k = 0; k < snap->count; k++) {
    copy_key_t ck = snap->keys[k];
    if (copy_key_is_sym(ck)) continue;
    
    int32_t slot = copy_key_at_slot(ptr, snap->slots[k], ck) ? (int32_t)snap->slots[k] : copy_key_slot(ptr->shape, ck);
    if (slot < 0 || (uint32_t)slot >= ptr->prop_count) continue;
    
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, (uint32_t)slot);
    if (!object_enum_enumerable(js, obj, ptr, prop)) continue;
    
    ant_value_t val = !(prop->has_getter || prop->has_setter) ? ant_object_prop_get_unchecked(ptr, (uint32_t)slot)
      : prop->has_getter ? js_call_getter(js, obj, prop->getter) : js_mkundef();
    
    if (is_err(val)) return val;
    object_enum_emit(js, sink, object_enum_item(js, mode, ck.interned, intern_length(ck.interned), val));
  }
  
  return js_mkundef();
}

static ant_value_t object_enum_props_from(ant_t *js, ant_value_t obj, ant_object_t *ptr, enum obj_enum_mode mode, object_enum_sink_t *sink, uint32_t start) {
  uint32_t count = ant_shape_count(ptr->shape);
  uint32_t limit = count < ptr->prop_count ? count : ptr->prop_count;
  uint32_t n = limit > start ? limit - start : 0;
  
  copy_key_snapshot_t snap;
  snap.count = 0;
  snap.keys = n <= 32 ? snap.stack : malloc((size_t)n * sizeof(*snap.keys));
  snap.slots = n <= 32 ? snap.stack_slots : malloc((size_t)n * sizeof(*snap.slots));
  
  if (!snap.keys || !snap.slots) {
    copy_key_snapshot_free(&snap);
    return js_mkerr(js, "out of memory");
  }
  
  for (uint32_t i = start; i < limit; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING) continue;
    snap.keys[snap.count] = copy_key_at(prop);
    snap.slots[snap.count++] = i;
  }
  
  ant_value_t result = object_enum_visit(js, obj, ptr, mode, sink, &snap);
  copy_key_snapshot_free(&snap);
  
  return result;
}

static ant_value_t object_enum_props_ordered(ant_t *js, ant_value_t obj, ant_object_t *ptr, enum obj_enum_mode mode, object_enum_sink_t *sink) {
  ant_shape_t *shape = ptr->shape;
  uint32_t count = ant_shape_count(shape);
  
  own_index_list_t indices;
  own_index_list_init(&indices);
  
  for (uint32_t i = 0; i < count && i < ptr->prop_count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING || !object_enum_enumerable(js, obj, ptr, prop)) continue;
    
    if (prop->has_getter || prop->has_setter) {
      own_index_list_free(&indices);
      copy_key_snapshot_t snap;
      if (!copy_key_snapshot_take(ptr, &snap, true)) return js_mkerr(js, "out of memory");
      ant_value_t result = object_enum_visit(js, obj, ptr, mode, sink, &snap);
      copy_key_snapshot_free(&snap);
      return result;
    }
    
    unsigned long index = 0;
    if (!own_key_is_array_index(prop->key.interned, intern_length(prop->key.interned), &index)) continue;
    if (!own_index_list_push(&indices, i, index)) {
      own_index_list_free(&indices);
      return js_mkerr(js, "out of memory");
    }
  }
  
  own_index_keys_sort(indices.items, indices.count);
  for (uint32_t k = 0; k < indices.count; k++) {
    uint32_t slot = indices.items[k].slot;
    const char *key = ant_shape_prop_at(shape, slot)->key.interned;
    object_enum_emit(js, sink, object_enum_item(js, mode, key, intern_length(key), ant_object_prop_get_unchecked(ptr, slot)));
  }
  own_index_list_free(&indices);
  
  for (uint32_t i = 0; i < count && i < ptr->prop_count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING || !object_enum_enumerable(js, obj, ptr, prop)) continue;
    
    const char *key = prop->key.interned;
    if (own_key_is_array_index(key, intern_length(key), NULL)) continue;
    object_enum_emit(js, sink, object_enum_item(js, mode, key, intern_length(key), ant_object_prop_get_unchecked(ptr, i)));
  }
  
  return js_mkundef();
}

static ant_value_t object_enum_props(ant_t *js, ant_value_t obj, ant_object_t *ptr, enum obj_enum_mode mode, object_enum_sink_t *sink) {
  ant_shape_t *shape = ptr->shape;
  if (ant_shape_may_have_index_keys(shape)) return object_enum_props_ordered(js, obj, ptr, mode, sink);
  
  uint32_t count = ant_shape_count(shape);
  uint32_t n = sink->count;
  
  for (uint32_t i = 0; i < count && i < ptr->prop_count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING || !object_enum_enumerable(js, obj, ptr, prop)) continue;
    
    if (prop->has_getter || prop->has_setter) {
      sink->count = n;
      return object_enum_props_from(js, obj, ptr, mode, sink, i);
    }
    
    const char *key = prop->key.interned;
    ant_value_t item = object_enum_item(js, mode, key, intern_length(key), ant_object_prop_get_unchecked(ptr, i));
    if (!sink->spilled && n < OBJECT_ENUM_SINK_STACK) { sink->items[n++] = item; continue; }
    
    sink->count = n;
    object_enum_emit_spilled(js, sink, item);
    n = sink->count;
  }
  
  sink->count = n;
  return js_mkundef();
}

ant_value_t js_copy_data_props(
  ant_t *js, ant_value_t dst, ant_value_t src, 
  const ant_value_t *excluded, int excluded_count
) {
  if (!is_object_type(dst)) return js_mkundef();
  
  copy_key_t stack_keys[16];
  copy_key_t *keys = excluded_count <= 16 ? stack_keys : malloc((size_t)excluded_count * sizeof(*keys));
  if (!keys) return js_mkerr(js, "out of memory");
  
  int count = 0;
  for (int i = 0; i < excluded_count; i++)
    if (copy_excluded_key_of(js, excluded[i], &keys[count])) count++;
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, dst);
  GC_ROOT_PIN(js, src);
  
  copy_ctx_t cx = { .keys = keys, .count = count };
  ant_value_t result = copy_data_props(js, dst, src, &cx);
  
  GC_ROOT_RESTORE(js, root_mark);
  if (keys != stack_keys) free(keys);
  
  return is_err(result) ? result : js_mkundef();
}

static ant_value_t builtin_object_assign(ant_params_t) {
  if (nargs == 0) return js_mkerr(js, "Object.assign requires at least 1 argument");
  
  ant_value_t target = args[0];
  uint8_t t = vtype(target);
  
  if (t == kTypeNull || t == kTypeUndefined) {
    return js_mkerr(js, "Cannot convert undefined or null to object");
  }
  
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) {
    target = Ant_Object_CreateBare(js);
  }
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, target);
  
  copy_ctx_t cx = { .assign = true };
  ant_value_t as_obj = js_as_obj(target);
  ant_value_t result = js_mkundef();
  
  for (int i = 1; i < nargs && !is_err(result); i++)
    result = copy_data_props(js, as_obj, args[i], &cx);
  
  GC_ROOT_RESTORE(js, root_mark);
  return is_err(result) ? result : target;
}

ant_value_t builtin_object_freeze(ant_params_t) {
  if (nargs == 0) return js_mkundef();
  
  ant_value_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) return obj;
  ant_value_t as_obj = js_as_obj(obj);
  if (is_proxy(as_obj)) return proxy_set_integrity_level(js, as_obj, true);

  ant_object_t *ptr = js_obj_ptr(as_obj);
  if (!ptr || !ptr->shape) return obj;
  if (!js_obj_ensure_unique_shape(ptr)) return js_mkerr(js, "oom");
  if (js_is_arguments(obj)) js_arguments_unmap_all(js, obj);

  uint32_t count = ant_shape_count(ptr->shape);
  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop) continue;
    
    uint8_t attrs = ant_shape_get_attrs(ptr->shape, i);
    attrs &= (uint8_t)~ANT_PROP_ATTR_WRITABLE;
    attrs &= (uint8_t)~ANT_PROP_ATTR_CONFIGURABLE;
    
    if (prop->type == ANT_SHAPE_KEY_STRING) {
      const char *key = prop->key.interned;
      ant_shape_set_attrs_interned(js, ptr->shape, key, attrs);
    } else ant_shape_set_attrs_symbol(js, ptr->shape, prop->key.sym_off, attrs);
  }
  
  ptr->flags.frozen = 1;
  ptr->flags.sealed = 1;
  ptr->flags.extensible = 0;
  
  return obj;
}

static ant_value_t builtin_object_isFrozen(ant_params_t) {
  if (nargs == 0) return js_true;
  
  ant_value_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) return js_true;
  ant_value_t as_obj = js_as_obj(obj);
  if (is_proxy(as_obj)) return proxy_test_integrity_level(js, as_obj, true);
  
  ant_object_t *ptr = js_obj_ptr(as_obj);
  return js_bool(ptr && ptr->flags.frozen);
}

static ant_value_t builtin_object_seal(ant_params_t) {
  if (nargs == 0) return js_mkundef();
  
  ant_value_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) return obj;
  ant_value_t as_obj = js_as_obj(obj);
  if (is_proxy(as_obj)) return proxy_set_integrity_level(js, as_obj, false);
  
  ant_object_t *ptr = js_obj_ptr(as_obj);
  if (!ptr || !ptr->shape) return obj;
  if (!js_obj_ensure_unique_shape(ptr)) return js_mkerr(js, "oom");
  
  ptr->flags.sealed = 1;
  ptr->flags.extensible = 0;

  uint32_t count = ant_shape_count(ptr->shape);
  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop) continue;

    uint8_t attrs = ant_shape_get_attrs(ptr->shape, i);
    attrs &= (uint8_t)~ANT_PROP_ATTR_CONFIGURABLE;

    if (prop->type == ANT_SHAPE_KEY_STRING) {
      const char *key = prop->key.interned;
      ant_shape_set_attrs_interned(js, ptr->shape, key, attrs);
    } else {
      ant_shape_set_attrs_symbol(js, ptr->shape, prop->key.sym_off, attrs);
    }
  }
  
  return obj;
}

static ant_value_t builtin_object_isSealed(ant_params_t) {
  if (nargs == 0) return js_true;
  
  ant_value_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) return js_true;
  ant_value_t as_obj = js_as_obj(obj);
  if (is_proxy(as_obj)) return proxy_test_integrity_level(js, as_obj, false);
  
  ant_object_t *ptr = js_obj_ptr(as_obj);
  if (ptr && (ptr->flags.sealed || ptr->flags.frozen)) return js_true;
  
  return js_false;
}

static iter_action_t object_from_entries_iter_cb(ant_t *js, ant_value_t entry, void *ctx, ant_value_t *out) {
  ant_value_t *result = ctx;
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, *result);
  GC_ROOT_PIN(js, entry);

  if (vtype(entry) != kTypeArray && vtype(entry) != kTypeObject) {
    *out = js_mkerr(js, "Object.fromEntries iterable values must be entry objects");
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }

  ant_value_t key = arr_get(js, entry, 0);
  GC_ROOT_PIN(js, key);
  ant_value_t val = arr_get(js, entry, 1);
  GC_ROOT_PIN(js, val);

  char key_stack[128];
  property_key_view_t key_view;
  ant_value_t error = property_key_view_init(
    js, key, &key_view, 
    key_stack, sizeof(key_stack)
  );
  
  if (is_err(error)) {
    *out = error;
    property_key_view_dispose(&key_view);
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }
  if (property_key_view_requires_pin(&key_view, false))
    GC_ROOT_PIN(js, key_view.js_key);

  ant_value_t added;
  if (key_view.is_symbol) added = mkprop_symbol_attrs_impl(
    js, *result, key_view.js_key, val, 0, MKPROP_USE_DEFAULT_ATTRS | MKPROP_KEYED_STORE
  );
  else {
    const char *interned = intern_string(key_view.bytes, key_view.length);
    added = interned
      ? mkprop_interned_keyed(js, *result, interned, val)
      : js_mkerr(js, "oom");
  }
    
  property_key_view_dispose(&key_view);
  if (is_err(added)) {
    *out = added;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }

  *out = *result;
  GC_ROOT_RESTORE(js, root_mark);

  return ITER_CONTINUE;
}

static ant_value_t builtin_object_fromEntries(ant_params_t) {
  if (nargs == 0) return js_mkerr(js, "Object.fromEntries requires an iterable argument");

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t iterable = args[0];
  GC_ROOT_PIN(js, iterable);

  ant_value_t result = Ant_Object_Create(js);
  GC_ROOT_PIN(js, result);

  ant_value_t iter_result = iter_foreach(js, iterable, object_from_entries_iter_cb, &result);
  if (is_err(iter_result)) {
    if (vtype(iterable) == kTypeArray || vtype(iterable) == kTypeObject || vtype(iterable) == kTypeFunction) {
      GC_ROOT_RESTORE(js, root_mark);
      return iter_result;
    }
    ant_value_t err = js_mkerr(js, "Object.fromEntries requires an iterable");
    GC_ROOT_RESTORE(js, root_mark);
    return err;
  }

  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static ant_value_t descriptor_template(ant_t *js, bool accessor) {
  ant_value_t *slot = accessor ? &js->builtins.accessor_desc_template : &js->builtins.data_desc_template;
  if (vtype(*slot) == kTypeObject) return *slot;
  
  ant_value_t template = Ant_Object_Create(js);
  if (is_err(template)) return template;
  
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, template);
  
  ant_value_t first = mkprop_interned(js, template, accessor ? js->intern.get : js->intern.value, js_mkundef(), 0);
  ant_value_t second = mkprop_interned(js, template, accessor ? js->intern.set : js->intern.writable, accessor ? js_mkundef() : js_false, 0);
  
  ant_value_t third = mkprop_interned(js, template, js->intern.enumerable, js_false, 0);
  ant_value_t fourth = mkprop_interned(js, template, js->intern.configurable, js_false, 0);
  
  GC_ROOT_RESTORE(js, mark);

  if (is_err(first)) return first;
  if (is_err(second)) return second;
  if (is_err(third)) return third;
  if (is_err(fourth)) return fourth;
  
  ant_object_t *ptr = js_obj_ptr(template);
  if (!ptr->shape || ant_shape_count(ptr->shape) != 4 || ptr->prop_count != 4)
    return js_mkerr(js, "invalid descriptor template");
  
  *slot = template;
  return template;
}

static ant_value_t from_property_descriptor(
  ant_t *js, bool accessor, ant_value_t first, ant_value_t second, bool enumerable, bool configurable
) {
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, first);
  GC_ROOT_PIN(js, second);
  
  ant_value_t template = descriptor_template(js, accessor);
  ant_value_t desc = is_err(template) ? template : js_mkobj_from_template(js, template);
  GC_ROOT_RESTORE(js, mark);
  if (is_err(desc)) return desc;
  
  ant_object_t *obj = js_obj_ptr(desc);
  ant_object_prop_set_unchecked(obj, 0, first);
  ant_object_prop_set_unchecked(obj, 1, second);
  ant_object_prop_set_unchecked(obj, 2, js_bool(enumerable));
  ant_object_prop_set_unchecked(obj, 3, js_bool(configurable));
  gc_write_barrier(js, obj, first);
  gc_write_barrier(js, obj, second);
  
  return desc;
}

ant_value_t js_from_data_descriptor(ant_t *js, ant_value_t value, bool writable, bool enumerable, bool configurable) {
  return from_property_descriptor(js, false, value, js_bool(writable), enumerable, configurable);
}

ant_value_t js_from_accessor_descriptor(ant_t *js, ant_value_t getter, ant_value_t setter, bool enumerable, bool configurable) {
  return from_property_descriptor(js, true, getter, setter, enumerable, configurable);
}

static ant_value_t object_get_own_property_descriptor_keyed(
  ant_t *js, ant_value_t obj, property_key_view_t *key
) {
  uint8_t t = vtype(obj);

  if (t == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) == kTypeFunction) { obj = promoted; t = kTypeFunction; }
  }
  
  if (t == kTypeBuiltin) {
    if (key->is_symbol) return js_mkundef();
    
    ant_value_t value = js_mkundef();
    if (!js_cfunc_try_get_own(js, obj, key->bytes, key->length, &value))
      return js_mkundef();
    
    return js_from_data_descriptor(js, value, false, false, true);
  }

  const char *key_str;
  ant_offset_t key_len;
  
  bool is_sym = key->is_symbol;
  if (is_sym) {
    const char *d = js_sym_desc(key->js_key);
    key_str = d ? d : "symbol";
    key_len = (ant_offset_t)strlen(key_str);
  } else {
    key_str = key->bytes;
    key_len = (ant_offset_t)key->length;
  }

  ant_value_t string_exotic_value = js_mkundef();
  bool string_exotic_length = false;
  bool has_string_exotic = false;

  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) {
    has_string_exotic = !is_sym && js_try_get_string_own_exotic(
      js, obj, key_str, (size_t)key_len, &string_exotic_value, &string_exotic_length
    );
    if (!has_string_exotic) return js_mkundef();

    return js_from_data_descriptor(js, string_exotic_value, false, !string_exotic_length, false);
  }
  
  ant_value_t as_obj = js_as_obj(obj);
  if (is_proxy(as_obj)) {
    ant_value_t materialized = property_key_view_materialize(js, key);
    if (is_err(materialized)) return materialized;
    return proxy_get_own_property_descriptor(js, as_obj, materialized);
  }

  bool is_arr_obj = array_obj_ptr(as_obj) != NULL;
  bool is_arr_length = !is_sym && real_array_obj_ptr(as_obj) && is_length_key(key_str, key_len);
  bool has_arr_index = false;
  ant_value_t arr_index_val = js_mkundef();
  
  if (!is_sym && is_arr_obj)
    has_arr_index = arr_own_dense_element(js, as_obj, key_str, (size_t)key_len, &arr_index_val);

  ant_offset_t sym_off = is_sym ? (ant_offset_t)vdata(key->js_key) : 0;
  prop_meta_t sym_meta; prop_meta_t str_meta;
  
  bool has_sym_meta = is_sym ? lookup_symbol_prop_meta(js, as_obj, sym_off, &sym_meta) : false;
  bool has_str_meta = is_sym ? false : lookup_string_prop_meta(js, as_obj, key_str, (size_t)key_len, &str_meta);

  ant_prop_loc_t prop_off = is_sym ? lkp_sym(as_obj, sym_off) : lkp(js, as_obj, key_str, key_len);
  if (!prop_off.obj && !(is_sym ? has_sym_meta : has_str_meta) && !is_arr_length && !has_arr_index) {
    has_string_exotic = !is_sym && t == kTypeObject && js_try_get_string_own_exotic(
      js, obj, key_str, (size_t)key_len, 
      &string_exotic_value, &string_exotic_length
    );
    
    if (!has_string_exotic) {
      const ant_object_t *exotic_ptr = js_obj_ptr(as_obj);
      ant_value_t element = !is_sym && exotic_ptr
        ? exotic_own_get(js, as_obj, exotic_ptr, key_str, (size_t)key_len) : js_mkundef();
      if (vtype(element) == kTypeUndefined || element == T_EMPTY) return js_mkundef();
      
      return js_from_data_descriptor(js, element, true, true, true);
    }
  }

  bool has_getter = false;
  bool has_setter = false;
  
  ant_value_t getter = js_mkundef();
  ant_value_t setter = js_mkundef();
  
  bool writable = true;
  bool enumerable = true;
  bool configurable = true;

  if (is_sym && has_sym_meta) {
    has_getter = sym_meta.has_getter;
    has_setter = sym_meta.has_setter;
    getter = sym_meta.getter;
    setter = sym_meta.setter;
    writable = sym_meta.writable;
    enumerable = sym_meta.enumerable;
    configurable = sym_meta.configurable;
  } else if (!is_sym && has_str_meta) {
    has_getter = str_meta.has_getter;
    has_setter = str_meta.has_setter;
    getter = str_meta.getter;
    setter = str_meta.setter;
    writable = str_meta.writable;
    enumerable = str_meta.enumerable;
    configurable = str_meta.configurable;
  }

  if (has_getter || has_setter) return js_from_accessor_descriptor(
    js, has_getter ? getter : js_mkundef(), has_setter ? setter : js_mkundef(), enumerable, configurable
  );

  ant_value_t prop_val = js_mkundef();
  if (prop_off.obj) prop_val = js_prop_load(prop_off);
  else if (has_string_exotic) {
    prop_val = string_exotic_value;
    writable = false;
    enumerable = !string_exotic_length;
    configurable = false;
  } else if (has_arr_index) {
    prop_val = arr_index_val;
    ant_object_t *arr_ptr = js_obj_ptr(as_obj);
    if (arr_ptr && (arr_ptr->flags.frozen || arr_ptr->flags.sealed)) configurable = false;
    if (arr_ptr && arr_ptr->flags.frozen) writable = false;
  } else if (is_arr_length) {
    prop_val = tov((double)get_array_length(js, as_obj));
    writable = !array_length_readonly(js, as_obj);
    enumerable = false;
    configurable = false;
  }
  
  return js_from_data_descriptor(js, prop_val, writable, enumerable, configurable);
}

ant_value_t builtin_object_getOwnPropertyDescriptor(ant_params_t) {
  if (nargs < 1) return js_mkundef();

  ant_value_t obj = js_object_view(args[0]);
  ant_value_t raw_key = nargs >= 2 ? args[1] : js_mkundef();
  char key_stack[128];
  
  property_key_view_t key;
  ant_value_t error = property_key_view_init(
    js, raw_key, &key, 
    key_stack, sizeof(key_stack)
  );
  
  if (is_err(error)) {
    property_key_view_dispose(&key);
    return error;
  }

  bool may_materialize = is_object_type(obj) && is_proxy(js_as_obj(obj));
  bool root_key = property_key_view_requires_pin(&key, may_materialize);
  
  size_t root_mark = root_key ? gc_root_scope(js) : 0;
  if (root_key) GC_ROOT_PIN(js, key.js_key);

  ant_value_t result = object_get_own_property_descriptor_keyed(js, obj, &key);
  property_key_view_dispose(&key);
  if (root_key) GC_ROOT_RESTORE(js, root_mark);
  
  return result;
}

static inline bool own_prop_names_is_dense_shadow(
  ant_t *js, ant_value_t obj,
  const char *key, ant_offset_t key_len
) {
  ant_offset_t doff = get_dense_buf(obj);
  if (!doff) return false;
  
  ant_offset_t dense_len = dense_iterable_length(js, obj);
  if (dense_len <= 0 || !is_array_index(key, key_len)) return false;
  
  unsigned long dense_idx = 0;
  return parse_array_index(key, (size_t)key_len, dense_len, &dense_idx);
}

static ant_value_t builtin_object_getOwnPropertyNames(ant_params_t) {
  if (nargs == 0) return mkarr(js);
  ant_value_t obj = js_object_view(args[0]);

  if (vtype(obj) == kTypeString) {
    ant_value_t arr = mkarr(js);
    ant_offset_t idx = 0;
    ant_offset_t slen = str_utf16_len(js, obj);
    
    for (ant_offset_t i = 0; i < slen; i++) {
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (uint64_t)i);
      arr_set(js, arr, idx++, js_mkstr(js, idxstr, idxlen));
    }
    
    arr_set(js, arr, idx++, js->length_str);
    return mkval(kTypeArray, vdata(arr));
  }

  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) == kTypeFunction) obj = promoted;
  }

  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t arr = mkarr(js);
    ant_offset_t idx = 0;
    arr_set(js, arr, idx++, js->length_str);
    
    if (vtype(js_cfunc_name_value(js, obj)) == kTypeString) arr_set(js, arr, idx++, ANT_STRING("name"));
    if (js_cfunc_has_prototype(obj)) arr_set(js, arr, idx++, ANT_STRING("prototype"));
    
    return mkval(kTypeArray, vdata(arr));
  }
  
  if (vtype(obj) != kTypeObject && vtype(obj) != kTypeArray && vtype(obj) != kTypeFunction) return mkarr(js);
  if (vtype(obj) == kTypeFunction) obj = js_func_obj(obj);
  
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || !ptr->shape) return mkarr(js);
  if (vtype(obj) != kTypeArray) return js_own_property_keys(js, obj, false, false);
  bool is_arr_obj = (vtype(obj) == kTypeArray);
  
  ant_value_t arr = mkarr(js);
  ant_offset_t idx = 0;
  
  if (is_arr_obj) {
  for (ant_offset_t i = 0;; i++) {
    ant_offset_t doff = get_dense_buf(obj);
    if (!doff) break;
    
    ant_offset_t dense_len = dense_iterable_length(js, obj);
    if (i >= dense_len) break;

    ant_value_t v = dense_get(doff, i);
    if (is_empty_slot(v)) continue;
    
    char idxstr[16]; size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
    arr_set(js, arr, idx++, js_mkstr(js, idxstr, idxlen));
  }}
  
  uint32_t count = ant_shape_count(ptr->shape);
  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop || prop->type == ANT_SHAPE_KEY_SYMBOL) continue;
    const char *key = prop->key.interned;
    ant_offset_t klen = (ant_offset_t)intern_length(key);

    if (is_arr_obj && own_prop_names_is_dense_shadow(js, obj, key, klen)) continue;
    arr_set(js, arr, idx++, js_mkstr(js, key, (size_t)klen));
  }
  
  if (is_arr_obj && !ptr->flags.arguments_object) arr_set(js, arr, idx++, js->length_str);
  return mkval(kTypeArray, vdata(arr));
}

static ant_value_t proxy_own_symbols(ant_t *js, ant_value_t proxy) {
  ant_value_t keys = proxy_own_keys_filtered(js, proxy, true, false);
  if (is_err(keys)) return keys;
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, keys);
  ant_value_t arr = mkarr(js);
  GC_ROOT_PIN(js, arr);
  
  ant_offset_t key_count = get_array_length(js, keys);
  for (ant_offset_t i = 0; i < key_count; i++) {
    ant_value_t key = arr_get(js, keys, i);
    if (vtype(key) == kTypeSymbol) js_arr_push(js, arr, key);
  }
  
  GC_ROOT_RESTORE(js, root_mark);
  return arr;
}

static ant_value_t builtin_object_getOwnPropertySymbols(ant_params_t) {
  if (nargs == 0) return mkarr(js);
  ant_value_t obj = args[0];

  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) == kTypeFunction) obj = promoted;
  }
  
  uint8_t t = vtype(obj);
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) return mkarr(js);
  if (t == kTypeFunction) obj = js_func_obj(obj);
  if (is_proxy(obj)) return proxy_own_symbols(js, obj);

  ant_object_t *ptr = js_obj_ptr(obj);
  ant_value_t arr = mkarr(js);
  ant_offset_t idx = 0;
  if (!ptr || !ptr->shape) return mkval(kTypeArray, vdata(arr));

  uint32_t count = ant_shape_count(ptr->shape);
  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_SYMBOL) continue;
    arr_set(js, arr, idx++, mkval(kTypeSymbol, prop->key.sym_off));
  }

  return mkval(kTypeArray, vdata(arr));
}

static ant_value_t object_add_descriptors_for_keys(
  ant_t *js,
  ant_value_t result,
  ant_value_t obj,
  ant_value_t keys
) {
  ant_offset_t len = get_array_length(js, keys);

  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t key = arr_get(js, keys, i);
    if (vtype(key) == kTypeUndefined) continue;

    ant_value_t desc_args[2] = { obj, key };
    ant_value_t desc = builtin_object_getOwnPropertyDescriptor(js, desc_args, 2, js_mkundef());
    if (is_err(desc)) return desc;
    if (vtype(desc) == kTypeUndefined) continue;

    ant_value_t set_result = js_setprop(js, result, key, desc);
    if (is_err(set_result)) return set_result;
  }

  return js_mkundef();
}

ant_value_t js_get_own_property_descriptor(ant_t *js, ant_value_t obj, ant_value_t key) {
  ant_value_t args[2] = { obj, key };
  return builtin_object_getOwnPropertyDescriptor(js, args, 2, js_mkundef());
}

static ant_value_t builtin_object_getOwnPropertyDescriptors(ant_params_t) {
  ant_value_t result = Ant_Object_CreateBare(js);
  
  if (is_object_type(js->sym.object_proto)) js_set_proto_init(result, js->sym.object_proto);
  if (nargs == 0 || vtype(args[0]) == kTypeNull || vtype(args[0]) == kTypeUndefined) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert undefined or null to object");
  }

  ant_value_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) {
    obj = builtin_Object(js, &obj, 1, js_mkundef());
    if (is_err(obj)) return obj;
    t = vtype(obj);
  }
  
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) return result;
  ant_value_t names = builtin_object_getOwnPropertyNames(js, &obj, 1, js_mkundef());
  if (is_err(names)) return names;
  
  ant_value_t err = object_add_descriptors_for_keys(js, result, obj, names);
  if (is_err(err)) return err;
  
  ant_value_t symbols = builtin_object_getOwnPropertySymbols(js, &obj, 1, js_mkundef());
  if (is_err(symbols)) return symbols;
  
  err = object_add_descriptors_for_keys(js, result, obj, symbols);
  if (is_err(err)) return err;

  return result;
}

static ant_value_t builtin_object_isExtensible(ant_params_t) {
  if (nargs == 0) return js_false;
  
  ant_value_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) return js_false;
  ant_value_t as_obj = js_as_obj(obj);
  if (is_proxy(as_obj)) return proxy_is_extensible(js, as_obj);
  
  ant_object_t *ptr = js_obj_ptr(as_obj);
  if (!ptr) return js_true;
  if (ptr->flags.frozen || ptr->flags.sealed) return js_false;
  return js_bool(ptr->flags.extensible);
}

static ant_value_t builtin_object_preventExtensions(ant_params_t) {
  if (nargs == 0) return js_mkundef();
  
  ant_value_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) return obj;
  ant_value_t as_obj = js_as_obj(obj);
  if (is_proxy(as_obj)) {
    ant_value_t result = proxy_prevent_extensions(js, as_obj);
    if (is_err(result)) return result;
    
    if (!js_truthy(js, result)) return proxy_not_prevented_error(
      js, as_obj, "Cannot prevent extensions", 
      "'preventExtensions' on proxy: trap returned falsy"
    );
    
    return obj;
  }
  
  ant_object_t *ptr = js_obj_ptr(as_obj);
  if (ptr) ptr->flags.extensible = 0;
  return obj;
}

static ant_value_t builtin_object_hasOwnProperty(ant_params_t) {
  ant_value_t obj = js_object_view(js->this_val);
  ant_value_t key = nargs >= 1 ? args[0] : js_mkundef();
  return Ant_Object_HasOwn(js, obj, key);
}

static bool proto_chain_contains_cycle_safe(ant_t *js, ant_value_t start, ant_value_t target);

bool js_is_prototype_of(ant_t *js, ant_value_t proto_obj, ant_value_t obj) {
  uint8_t obj_type = vtype(obj);
  if (obj_type != kTypeObject && obj_type != kTypeArray && obj_type != kTypeFunction) return false;
  uint8_t proto_type = vtype(proto_obj);
  if (proto_type != kTypeObject && proto_type != kTypeArray && proto_type != kTypeFunction) return false;
  ant_value_t current = get_proto(js, obj);
  return proto_chain_contains_cycle_safe(js, current, proto_obj);
}

ant_value_t builtin_object_isPrototypeOf(ant_params_t) {
  if (nargs < 1) return mkval(kTypeBool, 0);
  return mkval(kTypeBool, js_is_prototype_of(js, js->this_val, args[0]) ? 1 : 0);
}

static ant_value_t object_property_is_enumerable_key(
  ant_t *js, ant_value_t obj, property_key_view_t *key
) {
  uint8_t t = vtype(obj);
  ant_value_t string_exotic = js_mkundef();
  bool string_length = false;
  
  if (t == kTypeString) return mkval(kTypeBool, !key->is_symbol && js_try_get_string_own_exotic(
    js, obj, key->bytes, key->length, &string_exotic, &string_length
  ) && !string_length);
  
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) return mkval(kTypeBool, 0);
  ant_value_t as_obj = js_as_obj(obj);
  bool is_arr_obj = array_obj_ptr(as_obj) != NULL;

  if (key->is_symbol) {
    ant_offset_t sym_off = (ant_offset_t)vdata(key->js_key);
    ant_prop_loc_t off = lkp_sym(as_obj, sym_off);
    if (!off.obj) return mkval(kTypeBool, 0);
    prop_meta_t meta;
    if (lookup_symbol_prop_meta(js, as_obj, sym_off, &meta))
      return mkval(kTypeBool, meta.enumerable ? 1 : 0);
    return mkval(kTypeBool, 1);
  }

  const char *key_str = key->bytes;
  size_t key_len = key->length;
  
  if (real_array_obj_ptr(as_obj) && is_length_key(key_str, key_len)) return mkval(kTypeBool, 0);
  if (is_arr_obj) {
    unsigned long idx;
    if (parse_array_index(key_str, key_len, get_array_length(js, as_obj), &idx))
      return mkval(kTypeBool, arr_has(js, as_obj, (ant_offset_t)idx) ? 1 : 0);
  }

  if (t == kTypeObject && js_try_get_string_own_exotic(js, obj, key_str, key_len, &string_exotic, &string_length))
    return mkval(kTypeBool, string_length ? 0 : 1);
  
  ant_prop_loc_t off = lkp(js, as_obj, key_str, key_len);
  if (!off.obj) return mkval(kTypeBool, 0);

  const ant_shape_prop_t *prop_meta = prop_shape_meta(off);
  if (prop_meta && !js_obj_ptr(as_obj)->flags.is_exotic) {
    bool enumerable = (prop_meta->attrs & ANT_PROP_ATTR_ENUMERABLE) != 0;
    return mkval(kTypeBool, enumerable ? 1 : 0);
  }

  ant_object_t *ptr = js_obj_ptr(as_obj);
  if (ptr && ptr->flags.is_exotic) {
    prop_meta_t meta;
    if (lookup_string_prop_meta(js, as_obj, key_str, (size_t)key_len, &meta))
      return mkval(kTypeBool, meta.enumerable ? 1 : 0);
  }
  return mkval(kTypeBool, 1);
}

ant_value_t Ant_Object_PropertyIsEnumerable(ant_t *js, ant_value_t obj, ant_value_t raw_key) {
  char key_stack[128];
  property_key_view_t key;
  ant_value_t error = property_key_view_init(
    js, raw_key, &key, 
    key_stack, sizeof(key_stack)
  );
  
  ant_value_t result = error;
  if (!is_err(error)) result = object_property_is_enumerable_key(js, obj, &key);

  property_key_view_dispose(&key);
  return result;
}

static ant_value_t builtin_object_propertyIsEnumerable(ant_params_t) {
  return Ant_Object_PropertyIsEnumerable(js, js->this_val, nargs >= 1 ? args[0] : js_mkundef());
}

typedef enum {
  OBJECT_TAG_OBJECT,
  OBJECT_TAG_UNDEFINED,
  OBJECT_TAG_NULL,
  OBJECT_TAG_ARRAY,
  OBJECT_TAG_ARGUMENTS,
  OBJECT_TAG_FUNCTION,
  OBJECT_TAG_ERROR,
  OBJECT_TAG_BOOLEAN,
  OBJECT_TAG_NUMBER,
  OBJECT_TAG_STRING,
  OBJECT_TAG_DATE,
  OBJECT_TAG_BIGINT,
  OBJECT_TAG_PROMISE,
  OBJECT_TAG_REGEXP,
  OBJECT_TAG_COUNT,
} object_tag_t;

static_assert(
  OBJECT_TAG_COUNT == sizeof(((ant_t *)0)->mutable_roots.object_tag_strings) / sizeof(ant_value_t), 
  "resize object_tag_strings in isolate_values.h"
);

#define OBJECT_TAG_TEXT(name) { "[object " name "]", sizeof("[object " name "]") - 1 }

static const struct { const char *text; uint8_t len; } object_tag_texts[OBJECT_TAG_COUNT] = {
  [OBJECT_TAG_OBJECT]    = OBJECT_TAG_TEXT("Object"),
  [OBJECT_TAG_UNDEFINED] = OBJECT_TAG_TEXT("Undefined"),
  [OBJECT_TAG_NULL]      = OBJECT_TAG_TEXT("Null"),
  [OBJECT_TAG_ARRAY]     = OBJECT_TAG_TEXT("Array"),
  [OBJECT_TAG_ARGUMENTS] = OBJECT_TAG_TEXT("Arguments"),
  [OBJECT_TAG_FUNCTION]  = OBJECT_TAG_TEXT("Function"),
  [OBJECT_TAG_ERROR]     = OBJECT_TAG_TEXT("Error"),
  [OBJECT_TAG_BOOLEAN]   = OBJECT_TAG_TEXT("Boolean"),
  [OBJECT_TAG_NUMBER]    = OBJECT_TAG_TEXT("Number"),
  [OBJECT_TAG_STRING]    = OBJECT_TAG_TEXT("String"),
  [OBJECT_TAG_DATE]      = OBJECT_TAG_TEXT("Date"),
  [OBJECT_TAG_BIGINT]    = OBJECT_TAG_TEXT("BigInt"),
  [OBJECT_TAG_PROMISE]   = OBJECT_TAG_TEXT("Promise"),
  [OBJECT_TAG_REGEXP]    = OBJECT_TAG_TEXT("RegExp"),
};

static const uint8_t object_type_tags[kTypeSourceCode + 1] = {
  [kTypeUndefined] = OBJECT_TAG_UNDEFINED,
  [kTypeNull]      = OBJECT_TAG_NULL,
  [kTypeBool]      = OBJECT_TAG_BOOLEAN,
  [kTypeNumber]    = OBJECT_TAG_NUMBER,
  [kTypeString]    = OBJECT_TAG_STRING,
  [kTypeArray]     = OBJECT_TAG_ARRAY,
  [kTypeFunction]  = OBJECT_TAG_FUNCTION,
  [kTypeBuiltin]   = OBJECT_TAG_FUNCTION,
  [kTypeError]     = OBJECT_TAG_ERROR,
  [kTypeBigInt]    = OBJECT_TAG_BIGINT,
  [kTypePromise]   = OBJECT_TAG_PROMISE,
};

static object_tag_t object_object_tag(ant_t *js, ant_value_t obj, ant_object_t *ptr, ant_value_t *err) {
  if (ptr->flags.is_exotic && is_proxy(obj)) {
    bool is_array = false;
    *err = js_is_array_value_checked(js, obj, &is_array);
    if (is_array) return OBJECT_TAG_ARRAY;
  }
  
  if (is_callable(obj)) return OBJECT_TAG_FUNCTION;
  if (ptr->flags.regexp_brand) return OBJECT_TAG_REGEXP;
  if (ant_object_extra_count(ptr) == 0) return OBJECT_TAG_OBJECT;
  if (get_slot(obj, SLOT_ERROR_BRAND) == js_true) return OBJECT_TAG_ERROR;
  
  switch (vtype(get_slot(obj, SLOT_PRIMITIVE))) {
    case kTypeBool:   return OBJECT_TAG_BOOLEAN;
    case kTypeNumber: return OBJECT_TAG_NUMBER;
    case kTypeString: return OBJECT_TAG_STRING;
    default: break;
  }
  
  return is_date_instance(obj) ? OBJECT_TAG_DATE : OBJECT_TAG_OBJECT;
}

static ant_value_t object_tag_string(ant_t *js, object_tag_t tag) {
  ant_value_t *cached = &js->mutable_roots.object_tag_strings[tag];
  if (!*cached) *cached = js_mkstr(js, object_tag_texts[tag].text, object_tag_texts[tag].len);
  return *cached;
}

static ant_value_t object_tag_string_for(ant_t *js, ant_value_t tag_val) {
  size_t way = (size_t)(vdata(tag_val) >> 4) & 7;
  ant_value_t *key = &js->mutable_roots.object_tag_cache_keys[way];
  ant_value_t *value = &js->mutable_roots.object_tag_cache_values[way];
  if (*key == tag_val) return *value;
  
  ant_offset_t tag_len = 0;
  const char *tag = (const char *)(uintptr_t)vstr(js, tag_val, &tag_len);
  
  char static_buf[64];
  string_builder_t sb;
  string_builder_init(&sb, static_buf, sizeof(static_buf));
  string_builder_append(&sb, "[object ", 8);
  string_builder_append(&sb, tag, tag_len);
  string_builder_append(&sb, "]", 1);
  
  ant_value_t result = string_builder_finalize(js, &sb);
  if (!is_err(result)) { *key = tag_val; *value = result; }
  return result;
}

ant_value_t Ant_Object_ToStringTag(ant_t *js, ant_value_t obj) {
  uint8_t t = vtype(obj);
  
  if (t != kTypeUndefined && t != kTypeNull) {
    ant_value_t tag_val = js_get_sym(js, obj, js->sym.toStringTag_sym);
    if (is_err(tag_val)) return tag_val;
    if (vtype(tag_val) == kTypeString) return object_tag_string_for(js, tag_val);
  }
  
  ant_value_t err = js_mkundef();
  object_tag_t tag = js_is_arguments(obj) ? OBJECT_TAG_ARGUMENTS
    : t == kTypeObject ? object_object_tag(js, obj, js_obj_ptr(obj), &err)
    : t <= kTypeSourceCode ? (object_tag_t)object_type_tags[t] : OBJECT_TAG_OBJECT;
  
  return is_err(err) ? err : object_tag_string(js, tag);
}

static ant_value_t builtin_object_toString(ant_params_t) {
  return Ant_Object_ToStringTag(js, js->this_val);
}

static ant_value_t builtin_object_valueOf(ant_params_t) {
  return js->this_val;
}

static ant_value_t builtin_object_toLocaleString(ant_params_t) {
  ant_value_t result;
  if (!js_try_invoke(js, js->this_val, "toString", NULL, 0, &result))
    return js_mkerr_typed(js, JS_ERR_TYPE, "toString is not a function");
  return result;
}

static inline ant_value_t require_callback(ant_native_params_t, const char *name) {
  if (nargs == 0 || !is_callable(args[0]))
    return js_mkerr(js, "%s requires a function argument", name);
  return args[0];
}

static inline void array_copy_write(ant_t *js, ant_value_t result, ant_offset_t doff, ant_offset_t idx, ant_value_t value) {
  if (doff) dense_set(js, doff, idx, value);
  else arr_set(js, result, idx, value);
}

static inline ant_value_t array_copy_element(
  ant_t *js, ant_value_t arr, ant_object_t *elems,
  ant_value_t result, ant_offset_t doff, ant_offset_t from, ant_offset_t to
) {
  ant_value_t value;
  if (!array_dense_read(elems, from, &value)) {
    value = array_method_get_index(js, arr, from);
    if (is_err(value)) return value;
  }
  array_copy_write(js, result, doff, to, value);
  return js_mkundef();
}

static ant_value_t array_copy_spliced(
  ant_t *js, ant_value_t arr, ant_offset_t len, ant_offset_t start,
  ant_offset_t skip, const ant_value_t *items, ant_offset_t item_count
) {
  ant_offset_t out_len = len - skip + item_count;
  
  if (out_len > UINT32_MAX) return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid array length");
  if (out_len == 0) return mkarr(js);
  
  ant_value_t result = alloc_array_with_proto_capacity(
    js, js->sym.array_proto, 
    gc_array_storage_fit((uint32_t)out_len), 0, true
  );
  
  if (is_err(result)) return result;
  
  ant_offset_t doff = get_dense_buf(result);
  if (doff && dense_capacity(doff) >= out_len) {
    array_mark_may_have_holes(result);
    array_len_set(js, result, out_len);
  } else doff = 0;
  
  GC_ROOT_SAVE(roots, js);
  GC_ROOT_PIN(js, result);
  
  ant_object_t *elems = array_obj_ptr(arr);
  ant_value_t err = js_mkundef();
  
  for (ant_offset_t i = 0; i < start && !is_err(err); i++)
    err = array_copy_element(js, arr, elems, result, doff, i, i);
  for (ant_offset_t k = 0; k < item_count && !is_err(err); k++)
    array_copy_write(js, result, doff, start + k, items[k]);
  for (ant_offset_t i = start + skip; i < len && !is_err(err); i++)
    err = array_copy_element(js, arr, elems, result, doff, i, i - skip + item_count);
  
  GC_ROOT_RESTORE(js, roots);
  if (is_err(err)) return err;
  if (doff) array_obj_ptr(result)->flags.may_have_holes = 0;
  
  return result;
}

static ant_value_t arguments_get_index(ant_t *js, ant_value_t obj, ant_offset_t idx) {
  char key[24];
  uint_to_str(key, sizeof(key), (uint64_t)idx);
  return js_getprop_fallback(js, obj, key);
}

static ant_value_t arguments_delete_index(ant_t *js, ant_value_t obj, ant_offset_t idx) {
  char key[24];
  size_t key_len = uint_to_str(key, sizeof(key), (uint64_t)idx);
  return js_delete_prop(js, obj, key, key_len);
}

static ant_value_t arguments_move_index(ant_t *js, ant_value_t obj, ant_offset_t from, ant_offset_t to) {
  if (!arr_has(js, obj, from)) return arguments_delete_index(js, obj, to);
  ant_value_t value = arguments_get_index(js, obj, from);
  if (is_err(value)) return value;
  return js_setprop_index(js, obj, (uint32_t)to, value);
}

static ant_value_t arguments_set_length(ant_t *js, ant_value_t obj, ant_offset_t len) {
  ant_value_t result = js_setprop(js, obj, js->length_str, tov((double)len));
  if (is_err(result)) return result;
  ant_value_t stored = lkp_interned_val(js, obj, js->intern.length);
  if (vtype(stored) != kTypeNumber || tod(stored) != (double)len)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot assign to read only property 'length' of object");
  return tov((double)len);
}

static ant_value_t arguments_check_mutable(ant_t *js, ant_value_t obj, bool adds) {
  ant_object_t *ptr = js_obj_ptr(obj);
  if (ptr->flags.frozen)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot assign to read only property of frozen object");
  if (ptr->flags.sealed || (adds && !ptr->flags.extensible))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot add or delete property, object is not extensible");
  return js_mkundef();
}

static ant_value_t arguments_push(ant_t *js, ant_value_t obj, ant_value_t *args, int nargs) {
  ant_offset_t len = 0;
  ant_value_t result = array_like_length_checked(js, obj, &len);
  if (is_err(result)) return result;
  if (nargs > 0 && is_err(result = arguments_check_mutable(js, obj, true))) return result;
  for (int i = 0; i < nargs; i++, len++) {
    result = js_setprop_index(js, obj, (uint32_t)len, args[i]);
    if (is_err(result)) return result;
  }
  return arguments_set_length(js, obj, len);
}

static ant_value_t arguments_pop(ant_t *js, ant_value_t obj) {
  ant_offset_t len = 0;
  ant_value_t result = array_like_length_checked(js, obj, &len);
  if (is_err(result)) return result;
  if (len == 0) {
    result = arguments_set_length(js, obj, 0);
    return is_err(result) ? result : js_mkundef();
  }
  if (is_err(result = arguments_check_mutable(js, obj, false))) return result;
  ant_value_t value = arguments_get_index(js, obj, len - 1);
  if (is_err(value)) return value;
  if (is_err(result = arguments_delete_index(js, obj, len - 1))) return result;
  if (is_err(result = arguments_set_length(js, obj, len - 1))) return result;
  return value;
}

static ant_value_t arguments_shift(ant_t *js, ant_value_t obj) {
  ant_offset_t len = 0;
  ant_value_t result = array_like_length_checked(js, obj, &len);
  if (is_err(result)) return result;
  if (len == 0) {
    result = arguments_set_length(js, obj, 0);
    return is_err(result) ? result : js_mkundef();
  }
  if (is_err(result = arguments_check_mutable(js, obj, false))) return result;
  ant_value_t first = arguments_get_index(js, obj, 0);
  if (is_err(first)) return first;
  for (ant_offset_t k = 1; k < len; k++)
    if (is_err(result = arguments_move_index(js, obj, k, k - 1))) return result;
  if (is_err(result = arguments_delete_index(js, obj, len - 1))) return result;
  if (is_err(result = arguments_set_length(js, obj, len - 1))) return result;
  return first;
}

static ant_value_t arguments_unshift(ant_t *js, ant_value_t obj, ant_value_t *args, int nargs) {
  ant_offset_t len = 0;
  ant_value_t result = array_like_length_checked(js, obj, &len);
  if (is_err(result)) return result;
  if (nargs > 0) {
    if (is_err(result = arguments_check_mutable(js, obj, true))) return result;
    for (ant_offset_t k = len; k > 0; k--)
      if (is_err(result = arguments_move_index(js, obj, k - 1, k - 1 + (ant_offset_t)nargs))) return result;
    for (int i = 0; i < nargs; i++)
      if (is_err(result = js_setprop_index(js, obj, (uint32_t)i, args[i]))) return result;
  }
  return arguments_set_length(js, obj, len + (ant_offset_t)nargs);
}

static inline bool array_string_wrapper_length(ant_t *js, ant_value_t arr, ant_offset_t *len) {
  ant_value_t prim = js_string_wrapper_value(arr);
  if (vtype(prim) != kTypeString) return false;
  *len = str_utf16_len(js, prim);
  return true;
}

static ant_value_t array_string_wrapper_write_error(ant_t *js, const char *key) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot assign to read only property '%s' of object '[object String]'", key);
}

static ant_value_t array_method_receiver(ant_t *js, ant_value_t value, const char *name) {
  uint8_t t = vtype(value);
  if (t == kTypeArray || t == kTypeObject) return value;
  if (t == kTypeUndefined || t == kTypeNull)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Array.prototype.%s called on null or undefined", name);
  return is_boxable_primitive_type(t) ? js_normalize_sloppy_this(js, value) : value;
}

static ant_value_t builtin_array_push(ant_params_t) {
  ant_value_t arr = js->this_val;
  arr = array_method_receiver(js, arr, "push");
  
  if (is_err(arr)) return arr;
  ant_offset_t string_len;
  if (array_string_wrapper_length(js, arr, &string_len)) return array_string_wrapper_write_error(js, "length");

  if (is_proxy(arr)) {
    PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
    for (int i = 0; i < nargs; i++, len++) {
      ant_value_t set = array_set_index_throw(js, arr, len, args[i]);
      if (is_err(set)) return set;
    }
    
    ant_value_t set_len = array_set_length_throw(js, arr, len);
    if (is_err(set_len)) return set_len;
    return tov((double) len);
  }
  
  if (js_is_arguments(arr)) return arguments_push(js, arr, args, nargs);
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);

  ant_object_t *target = js_obj_ptr(js_as_obj(arr));
  if (target && (target->flags.frozen || array_length_readonly(js, arr)))
    return array_length_readonly_error(js);
  if (target && nargs > 0 && (target->flags.sealed || !target->flags.extensible))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot add property %u, object is not extensible", (unsigned)len);
  
  ant_offset_t doff = get_dense_buf(arr);
  if (doff) {
    ant_object_t *dense = dense_obj(doff);
    bool plain = dense && array_proto_chain_plain(js, dense);
    int i = 0;
    for (; i < nargs && (plain || !array_store_reaches_proto_slow(js, arr, len)); i++) {
      ant_offset_t cap = dense_capacity(doff);
      if (len >= cap) {
        doff = dense_grow(js, arr, len + 1);
        if (doff == 0) return js_mkerr(js, "oom");
      }
      if (is_empty_slot(args[i])) array_mark_may_have_holes(arr);
      dense_set(js, doff, len, args[i]);
      len++;
    }
    array_len_set(js, arr, len);
    if (i == nargs) return tov((double) len);

    for (; i < nargs; i++) {
      ant_value_t stored = js_setprop_index_throw(js, arr, (uint32_t)len, args[i]);
      if (is_err(stored)) return stored;
      len++;
    }
    ant_value_t len_val = tov((double) len);
    ant_value_t set = js_setprop_throw(js, arr, js->length_str, false, 0, len_val);
    return is_err(set) ? set : len_val;
  }
  
  for (int i = 0; i < nargs; i++, len++) {
    ant_value_t stored = js_setprop_index_throw(js, arr, (uint32_t)len, args[i]);
    if (is_err(stored)) return stored;
  }

  ant_value_t new_len = tov((double) len);
  ant_value_t set = js_setprop_throw(js, arr, js->length_str, false, 0, new_len);
  return is_err(set) ? set : new_len;
}

void js_arr_reserve(ant_t *js, ant_value_t arr, uint32_t n) {
  if (vtype(arr) != kTypeArray && vtype(arr) != kTypeObject) return;
  ant_offset_t doff = get_dense_buf(arr);
  if (!doff) return;
  if (n > dense_capacity(doff)) dense_grow(js, arr, n);
}

void js_arr_push(ant_t *js, ant_value_t arr, ant_value_t val) {
  if (vtype(arr) != kTypeArray && vtype(arr) != kTypeObject) return;
  
  ant_offset_t doff = get_dense_buf(arr);
  if (doff) {
    ant_offset_t len = get_array_length(js, arr);
    ant_offset_t cap = dense_capacity(doff);
    if (len >= cap) {
      doff = dense_grow(js, arr, len + 1);
      if (doff == 0) return;
    }
    if (is_empty_slot(val)) array_mark_may_have_holes(arr);
    dense_set(js, doff, len, val);
    array_len_set(js, arr, len + 1);
    return;
  }
  
  ant_offset_t len = 0;
  ant_value_t len_result = proxy_aware_length(js, arr, &len);
  if (is_err(len_result)) return;
  
  if (!is_empty_slot(val)) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)len);
    js_mkprop_fast(js, arr, idxstr, idxlen, val);
  }
  
  array_len_set(js, arr, len + 1);
}

static ant_value_t array_set_index_throw(ant_t *js, ant_value_t obj, ant_offset_t idx, ant_value_t value) {
  if (idx <= UINT32_MAX) return js_setprop_index_throw(js, obj, (uint32_t)idx, value);
  char key[24];
  size_t klen = uint_to_str(key, sizeof(key), (uint64_t)idx);
  return js_setprop_throw(js, obj, js_mkstr(js, key, klen), false, 0, value);
}

static ant_value_t array_delete_index_throw(ant_t *js, ant_value_t obj, ant_offset_t idx) {
  char key[24];
  size_t klen = uint_to_str(key, sizeof(key), (uint64_t)idx);
  ant_value_t deleted = js_delete_prop_ordinary(js, obj, key, klen);
  if (is_err(deleted)) return deleted;
  if (deleted == js_false) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot delete property '%s' of [object Array]", key);
  return js_mkundef();
}

static ant_value_t array_set_length_throw(ant_t *js, ant_value_t obj, ant_offset_t len) {
  ant_value_t set = js_setprop_throw(js, obj, js->length_str, false, 0, tov((double)len));
  return is_err(set) ? set : js_mkundef();
}

static ant_value_t array_move_range_throw(ant_t *js, ant_value_t arr, ant_offset_t from, ant_offset_t to, ant_offset_t count) {
  for (ant_offset_t n = 0; n < count; n++) {
    ant_offset_t i = to > from ? count - 1 - n : n;
    ant_value_t has = array_method_has_index(js, arr, from + i);
    if (is_err(has)) return has;
    
    ant_value_t done;
    if (js_truthy(js, has)) {
      ant_value_t value = array_method_get_index(js, arr, from + i);
      if (is_err(value)) return value;
      done = array_set_index_throw(js, arr, to + i, value);
    } else done = array_delete_index_throw(js, arr, to + i);
    if (is_err(done)) return done;
  }
  return js_mkundef();
}

static inline ant_object_t *array_mutable_dense(ant_t *js, ant_value_t arr) {
  if (vtype(arr) != kTypeArray) return NULL;
  ant_object_t *ptr = array_obj_ptr(arr);
  const uint16_t mask =
    ANT_OBJECT_FLAG_FAST_ARRAY | ANT_OBJECT_FLAG_DENSE_LENGTH_FITS | ANT_OBJECT_FLAG_FROZEN |
    ANT_OBJECT_FLAG_SEALED | ANT_OBJECT_FLAG_EXOTIC | ANT_OBJECT_FLAG_ARGUMENTS;
  if (!ptr || (ptr->flags.raw & mask) != (ANT_OBJECT_FLAG_FAST_ARRAY | ANT_OBJECT_FLAG_DENSE_LENGTH_FITS)) return NULL;
  if (!ptr->u.array.data) return NULL;
  return ptr;
}

static inline bool array_hole_reads_proto(ant_t *js, ant_object_t *ptr, ant_value_t slot) {
  return is_empty_slot(slot) && !array_proto_chain_plain(js, ptr);
}

static ant_value_t array_pop_generic(ant_t *js, ant_value_t arr) {
  ant_offset_t len = 0;
  ant_value_t len_result = array_like_length_checked(js, arr, &len);
  if (is_err(len_result)) return len_result;
  
  if (len == 0) {
    ant_value_t set = array_set_length_throw(js, arr, 0);
    return is_err(set) ? set : js_mkundef();
  }
  
  ant_value_t last = array_method_get_index(js, arr, len - 1);
  if (is_err(last)) return last;
  ant_value_t done = array_delete_index_throw(js, arr, len - 1);
  if (is_err(done)) return done;
  done = array_set_length_throw(js, arr, len - 1);
  
  return is_err(done) ? done : last;
}

static ant_value_t array_shift_generic(ant_t *js, ant_value_t arr) {
  ant_offset_t len = 0;
  ant_value_t len_result = array_like_length_checked(js, arr, &len);
  if (is_err(len_result)) return len_result;
  
  if (len == 0) {
    ant_value_t set = array_set_length_throw(js, arr, 0);
    return is_err(set) ? set : js_mkundef();
  }
  
  ant_value_t first = array_method_get_index(js, arr, 0);
  if (is_err(first)) return first;
  ant_value_t done = array_move_range_throw(js, arr, 1, 0, len - 1);
  if (is_err(done)) return done;
  done = array_delete_index_throw(js, arr, len - 1);
  if (is_err(done)) return done;
  done = array_set_length_throw(js, arr, len - 1);
  
  return is_err(done) ? done : first;
}

static ant_value_t array_splice_generic(
  ant_t *js, ant_value_t arr, ant_offset_t len, ant_offset_t start,
  ant_offset_t delete_count, ant_value_t *items, ant_offset_t insert_count,
  ant_value_t removed, bool intrinsic
) {
  for (ant_offset_t k = 0; k < delete_count; k++) {
    ant_value_t has = array_method_has_index(js, arr, start + k);
    if (is_err(has)) return has;
    if (!js_truthy(js, has)) continue;
    ant_value_t value = array_method_get_index(js, arr, start + k);
    if (is_err(value)) return value;
    ant_value_t set = array_result_set(js, removed, intrinsic, k, value);
    if (is_err(set)) return set;
  }
  
  ant_value_t done = array_set_length_throw(js, removed, delete_count);
  if (is_err(done)) return done;
  
  ant_offset_t tail = len - start - delete_count;
  if (insert_count != delete_count) {
    done = array_move_range_throw(js, arr, start + delete_count, start + insert_count, tail);
    if (is_err(done)) return done;
  }
  
  for (ant_offset_t k = len; k > len - delete_count + insert_count; k--) {
    done = array_delete_index_throw(js, arr, k - 1);
    if (is_err(done)) return done;
  }
  
  for (ant_offset_t k = 0; k < insert_count; k++) {
    done = array_set_index_throw(js, arr, start + k, items[k]);
    if (is_err(done)) return done;
  }
  
  done = array_set_length_throw(js, arr, len - delete_count + insert_count);
  return is_err(done) ? done : removed;
}

static ant_value_t builtin_array_pop(ant_params_t) {
  ant_object_t *fast = array_mutable_dense(js, js->this_val);
  if (__builtin_expect(fast != NULL, 1)) {
    uint32_t len = fast->u.array.len;
    if (len == 0) return js_mkundef();
    ant_value_t last = fast->u.array.data[--len];
    if (__builtin_expect(array_hole_reads_proto(js, fast, last), 0)) return array_pop_generic(js, js->this_val);
    fast->u.array.data[len] = T_EMPTY;
    fast->u.array.len = len;
    return is_empty_slot(last) ? js_mkundef() : last;
  }
  
  ant_value_t arr = js->this_val;

  arr = array_method_receiver(js, arr, "pop");
  if (is_err(arr)) return arr;
  ant_offset_t string_len;
  if (array_string_wrapper_length(js, arr, &string_len)) return array_string_wrapper_write_error(js, "length");

  if (is_proxy(arr)) {
    PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
    if (len == 0) {
      js_setprop(js, arr, js->length_str, tov(0.0));
      return js_mkundef();
    }
    
    len--;
    char idxstr[16];
    
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)len);
    ant_value_t result = proxy_aware_get_elem(js, arr, idxstr, idxlen);
    
    if (is_err(result)) return result;
    ant_value_t del = proxy_delete(js, arr, idxstr, idxlen);
    
    if (is_err(del)) return del;
    js_setprop(js, arr, js->length_str, tov((double) len));
    
    return result;
  }
  
  if (js_is_arguments(arr)) return arguments_pop(js, arr);

  ant_offset_t doff = get_dense_buf(arr);
  if (doff) {
    ant_object_t *ptr = (ant_object_t *)(uintptr_t)doff;
    bool is_array = ptr->type_tag == kTypeArray;
    if (ptr->flags.sealed || array_length_readonly(js, arr)) return array_pop_generic(js, arr);
    
    ant_offset_t len = is_array ? ptr->u.array.len : get_array_length(js, arr);
    if (len == 0) return js_mkundef();
    if (len > ptr->u.array.cap) goto pop_slow;
    len--;
    
    ant_value_t result = ptr->u.array.data[len];
    if (array_hole_reads_proto(js, ptr, result)) return array_pop_generic(js, arr);
    if (is_empty_slot(result)) result = js_mkundef();
    ptr->u.array.data[len] = T_EMPTY;
    
    if (is_array) {
      ptr->u.array.len = (uint32_t)len;
      ptr->flags.dense_length_fits = true;
    } else array_len_set(js, arr, len);
    
    return result;
  }

  pop_slow:
  return array_pop_generic(js, arr);
}

static ant_value_t builtin_array_slice(ant_params_t) {
  ant_value_t arr = js->this_val;
  ant_value_t string_val = unwrap_primitive(js, arr);
  bool string_like = (vtype(string_val) == kTypeString);

  if (!string_like) {
    arr = array_method_receiver(js, arr, "slice");
    if (is_err(arr)) return arr;
  }
  
  ant_offset_t len = 0;
  ant_offset_t string_byte_len = 0;
  ant_offset_t string_off = 0;
  
  if (string_like) {
    string_off = vstr(js, string_val, &string_byte_len);
    len = str_utf16_len(js, string_val);
  } else {
    ant_value_t len_result = array_like_length_checked(js, arr, &len);
    if (is_err(len_result)) return len_result;
  }
  
  ant_offset_t start = 0, end = len;
  double dlen = D(len);
  if (nargs >= 1 && vtype(args[0]) == kTypeNumber) {
    double d = tod(args[0]);
    if (d < 0) start = (ant_offset_t) (d + dlen < 0 ? 0 : d + dlen);
    else start = (ant_offset_t) (d > dlen ? dlen : d);
  }
  
  if (nargs >= 2 && vtype(args[1]) == kTypeNumber) {
    double d = tod(args[1]);
    if (d < 0) end = (ant_offset_t) (d + dlen < 0 ? 0 : d + dlen);
    else end = (ant_offset_t) (d > dlen ? dlen : d);
  }
  
  if (start > end) start = end;
  ant_offset_t count = end - start;
  bool intrinsic = false;
  
  ant_value_t result = array_species_create(js, arr, count, &intrinsic);
  if (is_err(result)) return result;
  
  ant_object_t *src = !string_like && vtype(arr) == kTypeArray ? array_obj_ptr(arr) : NULL;
  ant_object_t *dst = intrinsic && vtype(result) == kTypeArray ? array_obj_ptr(result) : NULL;
  
  if (
    src && dst && array_dense_covers(src, end) &&
    dst->flags.fast_array && dst->u.array.data && (dst->u.array.len == 0 || dst->u.array.len == count) &&
    array_elements_proto_plain(js, src) &&
    (count <= dst->u.array.cap || dense_grow(js, result, count))
  ) {
    bool has_holes = false, has_elements = false;
    bool dst_old = dst->flags.generation == 1;
    for (ant_offset_t i = 0; i < count; i++) {
      ant_value_t value = src->u.array.data[start + i];
      dst->u.array.data[i] = value;
      if (dst_old) gc_write_barrier_elem(js, dst, (uint32_t)i, value);
      if (is_empty_slot(value)) has_holes = true;
      else has_elements = true;
    }
    dst->u.array.len = (uint32_t)count;
    dst->flags.may_have_holes = has_holes;
    dst->flags.may_have_dense_elements |= has_elements;
    return result;
  }
  
  ant_offset_t result_idx = 0;
  bool skipped = false;
  for (ant_offset_t i = start; i < end; i++, result_idx++) {
    ant_value_t elem;
    if (string_like) {
      uint32_t code_unit = utf16_code_unit_at((const char *)(uintptr_t)string_off, string_byte_len, i);
      if (code_unit == 0xFFFFFFFF) break;
      elem = js_string_from_utf16_code_unit(js, code_unit);
    } else {
      ant_value_t has = array_method_has_index(js, arr, i);
      if (is_err(has)) return has;
      if (!js_truthy(js, has)) { skipped = true; continue; }
      elem = array_method_get_index(js, arr, i);
      if (is_err(elem)) return elem;
    }
    
    ant_value_t set = array_result_set(js, result, intrinsic, result_idx, elem);
    if (is_err(set)) return set;
  }
  
  if (!string_like && get_array_length(js, result) != result_idx)
    js_setprop(js, result, js->length_str, tov((double)result_idx));
  if (intrinsic && !skipped && result_idx == count) array_mark_packed(result);
  return result;
}

static ant_value_t array_join_body(ant_params_t) {
  ant_value_t arr = js->this_val;
  arr = array_method_receiver(js, arr, "join");
  if (is_err(arr)) return arr;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, arr);

  const char *sep = ",";
  ant_offset_t sep_len = 1;
  ant_value_t sep_value = js_mkundef();
  ant_value_t sep_arg = nargs >= 1 ? args[0] : js_mkundef();
  
  GC_ROOT_PIN(js, sep_arg);
  GC_ROOT_PIN(js, sep_value);
  
  ant_offset_t len = 0;
  ant_value_t len_result = proxy_aware_length(js, arr, &len);
  if (is_err(len_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return len_result;
  }

  if (vtype(sep_arg) != kTypeUndefined) {
    sep_value = coerce_to_str_hint(js, sep_arg, 1);
    if (is_err(sep_value)) {
      GC_ROOT_RESTORE(js, root_mark);
      return sep_value;
    }
    ant_offset_t sep_off = vstr(js, sep_value, &sep_len);
    sep = (const char *)(uintptr_t)sep_off;
  }
  
  if (len == 0) {
    ant_value_t empty = js_mkstr(js, "", 0);
    GC_ROOT_RESTORE(js, root_mark);
    return empty;
  }

  ant_object_t *fast_arr = array_obj_ptr(arr);
  bool fast_primitives = fast_arr && fast_arr->flags.fast_array &&
    !fast_arr->flags.may_have_holes && fast_arr->u.array.data &&
    (ant_offset_t)fast_arr->u.array.len == len;
  
  size_t fast_total = 0;
  if (fast_primitives) {
    if (len > 1 && (size_t)sep_len > SIZE_MAX / ((size_t)len - 1))
      fast_primitives = false;
    else fast_total = len > 1 ? ((size_t)len - 1) * (size_t)sep_len : 0;
  }
  
  for (ant_offset_t i = 0; fast_primitives && i < len; i++) {
    ant_value_t elem = fast_arr->u.array.data[i];
    size_t elem_len = 0;
    
    switch (vtype(elem)) {
      case kTypeString:
        if (str_is_heap_rope(elem) || str_is_heap_builder(elem)) {
          fast_primitives = false;
          continue;
        }
        elem_len = (size_t)vstrlen(js, elem);
        break;
      case kTypeBool: elem_len = vdata(elem) ? 4 : 5; break;
      case kTypeNull:
      case kTypeUndefined: break;
      default: fast_primitives = false; continue;
    }
    
    if (elem_len > SIZE_MAX - fast_total) fast_primitives = false;
    else fast_total += elem_len;
  }

  if (fast_primitives) {
    ant_value_t result = js_mkstr(js, NULL, fast_total);
    GC_ROOT_PIN(js, result);
    if (is_err(result)) {
      GC_ROOT_RESTORE(js, root_mark);
      return result;
    }

    ant_flat_string_t *flat = (ant_flat_string_t *)vptr(result);
    fast_arr = array_obj_ptr(arr);
    if (vtype(sep_value) == kTypeString) {
      ant_offset_t sep_off = vstr(js, sep_value, &sep_len);
      sep = (const char *)(uintptr_t)sep_off;
    }
    
    size_t pos = 0;
    for (ant_offset_t i = 0; i < len; i++) {
      if (i > 0) {
        memcpy(flat->bytes + pos, sep, (size_t)sep_len);
        pos += (size_t)sep_len;
      }
      
      ant_value_t elem = fast_arr->u.array.data[i];
      const char *elem_ptr = NULL;
      size_t elem_len = 0;
      
      switch (vtype(elem)) {
        case kTypeString: {
          ant_offset_t str_len = 0;
          ant_offset_t str_off = vstr(js, elem, &str_len);
          elem_ptr = (const char *)(uintptr_t)str_off;
          elem_len = (size_t)str_len;
          break;
        }
        case kTypeBool:
          elem_ptr = vdata(elem) ? "true" : "false";
          elem_len = vdata(elem) ? 4 : 5;
          break;
        default: break;
      }
      
      if (elem_len > 0) {
        memcpy(flat->bytes + pos, elem_ptr, elem_len);
        pos += elem_len;
      }
    }
    
    flat->bytes[pos] = '\0';
    str_flat_finish(flat, str_detect_ascii_bytes(flat->bytes, pos));
    GC_ROOT_RESTORE(js, root_mark);
    
    return result;
  }
  
  char static_buf[1024];
  string_builder_t sb;
  string_builder_init(&sb, static_buf, sizeof(static_buf));
  
  ant_value_t elem = js_mkundef();
  ant_value_t elem_str = js_mkundef();
  
  GC_ROOT_PIN(js, elem);
  GC_ROOT_PIN(js, elem_str);

  for (ant_offset_t i = 0; i < len; i++) {
    if (i > 0 && !string_builder_append(&sb, sep, (size_t)sep_len)) {
      string_builder_dispose(&sb);
      ant_value_t err = js_mkerr(js, "oom");
      GC_ROOT_RESTORE(js, root_mark);
      return err;
    }

    if (!array_method_dense_element(arr, i, &elem)) elem = array_method_get_index(js, arr, i);
    elem_str = js_mkundef();
    
    if (is_err(elem)) {
      string_builder_dispose(&sb);
      GC_ROOT_RESTORE(js, root_mark);
      return elem;
    }
    
    if (vtype(elem) == kTypeNull || vtype(elem) == kTypeUndefined) continue;

    const char *elem_ptr = NULL;
    size_t elem_len = 0;
    
    char num_buf[32];
    
    switch (vtype(elem)) {
      case kTypeString: {
        ant_offset_t str_len = 0;
        ant_offset_t str_off = vstr(js, elem, &str_len);
        elem_ptr = (const char *)(uintptr_t)str_off;
        elem_len = (size_t)str_len;
        break;
      }
      case kTypeNumber:
        elem_len = js_number_to_chars(elem, num_buf, sizeof(num_buf));
        elem_ptr = num_buf;
        break;
      case kTypeBool:
        elem_ptr = vdata(elem) ? "true" : "false";
        elem_len = vdata(elem) ? 4 : 5;
        break;
      default: {
        elem_str = coerce_to_str_hint(js, elem, 1);
        if (is_err(elem_str)) {
          string_builder_dispose(&sb);
          GC_ROOT_RESTORE(js, root_mark);
          return elem_str;
        }
        ant_offset_t str_len = 0;
        ant_offset_t str_off = vstr(js, elem_str, &str_len);
        elem_ptr = (const char *)(uintptr_t)str_off;
        elem_len = (size_t)str_len;
        break;
      }
    }

    if (!string_builder_append(&sb, elem_ptr, elem_len)) {
      string_builder_dispose(&sb);
      ant_value_t err = js_mkerr(js, "oom");
      GC_ROOT_RESTORE(js, root_mark);
      return err;
    }
  }
  
  ant_value_t ret = string_builder_finalize(js, &sb);
  GC_ROOT_RESTORE(js, root_mark);
  
  return ret;
}

typedef struct {
  ant_value_t search;
  double search_num;
  uint8_t search_type;
  uint8_t mode;
} array_includes_query_t;

enum {
  ARRAY_INCLUDES_MATCH_GENERIC = 0,
  ARRAY_INCLUDES_MATCH_NUM     = 1,
  ARRAY_INCLUDES_MATCH_NAN     = 2,
  ARRAY_INCLUDES_MATCH_ID      = 3,
};

static inline array_includes_query_t array_includes_prepare_query(ant_value_t search) {
  array_includes_query_t q = {
    .search = search,
    .search_num = 0.0,
    .search_type = vtype(search),
    .mode = ARRAY_INCLUDES_MATCH_GENERIC,
  };

  if (q.search_type == kTypeNumber) {
    q.search_num = tod(search);
    q.mode = isnan(q.search_num) 
      ? ARRAY_INCLUDES_MATCH_NAN 
      : ARRAY_INCLUDES_MATCH_NUM;
  } else if (
    q.search_type != kTypeString &&
    q.search_type != kTypeBigInt
  ) q.mode = ARRAY_INCLUDES_MATCH_ID;

  return q;
}

static inline bool array_includes_matches(ant_t *js, const array_includes_query_t *q, ant_value_t val) {
switch (q->mode) {
  case ARRAY_INCLUDES_MATCH_NUM: return vtype(val) == kTypeNumber && tod(val) == q->search_num;
  case ARRAY_INCLUDES_MATCH_NAN: return vtype(val) == kTypeNumber && isnan(tod(val));
  case ARRAY_INCLUDES_MATCH_ID:  return vtype(val) == q->search_type && vdata(val) == vdata(q->search);
  default: return strict_eq_values(js, val, q->search);
}}

static inline ant_offset_t array_includes_length_from_number(double len_num) {
  if (isnan(len_num) || len_num <= 0) return 0;
  if (len_num >= (double)UINT32_MAX) return UINT32_MAX;
  return (ant_offset_t)len_num;
}

static inline ant_offset_t array_includes_start_index(ant_native_params_t, ant_offset_t len) {
  int64_t start = 0;
  
  if (nargs >= 2 && vtype(args[1]) != kTypeUndefined) {
  double from_index_num = js_to_number(js, args[1]);
  if (!isnan(from_index_num)) start = (int64_t)from_index_num;
  if (start < 0) {
    start += (int64_t)len;
    if (start < 0) start = 0;
  }}

  if ((uint64_t)start >= (uint64_t)len) return len;
  return (ant_offset_t)start;
}

static ant_value_t array_includes_length_value(ant_t *js, ant_value_t arr) {
  if (real_array_obj_ptr(arr) && !is_proxy(arr)) return tov((double)get_array_length(js, arr));
  if (is_proxy(arr)) return proxy_get(js, arr, "length", 6);
  return js_getprop_fallback_len(js, arr, "length", 6);
}

static ant_value_t array_includes_get_index_value(
  ant_t *js, ant_value_t arr, ant_offset_t idx, char *idxstr, size_t idxlen
) {
  if (is_proxy(arr)) return proxy_get(js, arr, idxstr, idxlen);

  if (array_obj_ptr(arr)) {
    if (arr_has(js, arr, idx)) return arr_get(js, arr, idx);
    if (!lkp_proto(js, arr, idxstr, idxlen).obj) return js_mkundef();
    idxstr[idxlen] = '\0';
    return js_getprop_super(js, get_proto(js, arr), arr, idxstr);
  }

  return js_getprop_fallback_len(js, arr, idxstr, idxlen);
}

static bool array_includes_get_dense_index_value(
  ant_value_t obj, ant_offset_t idx, ant_value_t *out
) {
  ant_value_t arr = (vtype(obj) == kTypeFunction) ? js_func_obj(obj) : obj;
  if (vtype(arr) != kTypeArray) return false;

  ant_offset_t doff = get_dense_buf(arr);
  if (!doff || idx >= dense_capacity(doff)) return false;

  ant_value_t val = dense_get(doff, idx);
  if (is_empty_slot(val)) return false;

  *out = val;
  return true;
}

static bool array_includes_get_proto_dense_index_value(
  ant_t *js, ant_value_t arr, ant_offset_t idx, ant_value_t *out
) {
  ant_value_t proto = get_proto(js, arr);
  for (int depth = 0; is_object_type(proto) && depth < MAX_PROTO_CHAIN_DEPTH; depth++) {
    if (array_includes_get_dense_index_value(proto, idx, out)) return true;
    proto = get_proto(js, proto);
  }

  return false;
}

static ant_value_t array_includes_get_array_index_value(
  ant_t *js, ant_value_t arr, ant_offset_t idx, char *idxstr, size_t idxlen
) {
  if (arr_has(js, arr, idx)) return arr_get(js, arr, idx);
  ant_value_t proto_dense_val = js_mkundef();
  if (array_includes_get_proto_dense_index_value(js, arr, idx, &proto_dense_val))
    return proto_dense_val;
  if (!lkp_proto(js, arr, idxstr, idxlen).obj) return js_mkundef();
  idxstr[idxlen] = '\0';
  return js_getprop_super(js, get_proto(js, arr), arr, idxstr);
}

static bool array_includes_object_may_have_indexed_props(
  ant_t *js, ant_value_t obj, bool include_dense
) {
  if (!is_object_type(obj) || is_proxy(obj)) return is_proxy(obj);

  ant_value_t as_obj = (vtype(obj) == kTypeFunction) ? js_func_obj(obj) : obj;
  ant_object_t *ptr = js_obj_ptr(as_obj);
  if (!ptr) return false;
  if (ptr->flags.is_exotic) return true;

  if (include_dense && vtype(as_obj) == kTypeArray) {
    ant_offset_t doff = get_dense_buf(as_obj);
    if (doff) {
      ant_offset_t dense_len = dense_capacity(doff);
      for (ant_offset_t i = 0; i < dense_len; i++) 
        if (!is_empty_slot(dense_get(doff, i))) return true;
    }
  }

  if (!ptr->shape) return false;
  uint32_t shape_count = ant_shape_count(ptr->shape);
  for (uint32_t i = 0; i < shape_count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop || prop->type == ANT_SHAPE_KEY_SYMBOL) continue;
    if (i >= ptr->prop_count) continue;
    
    const char *key = prop->key.interned;
    size_t key_len = intern_length(key);
    if (is_array_index(key, (ant_offset_t)key_len)) return true;
  }

  return false;
}

static bool array_includes_can_skip_hole_lookups(ant_t *js, ant_value_t arr) {
  if (array_includes_object_may_have_indexed_props(js, arr, false)) return false;

  ant_value_t proto = get_proto(js, arr);
  for (int depth = 0; is_object_type(proto) && depth < MAX_PROTO_CHAIN_DEPTH; depth++) {
    if (array_includes_object_may_have_indexed_props(js, proto, true)) return false;
    proto = get_proto(js, proto);
  }

  return true;
}

static ant_value_t array_includes_dense_fast(
  ant_t *js, ant_value_t arr, const array_includes_query_t *query, ant_offset_t len, ant_offset_t start
) {
  if (!array_obj_ptr(arr) || is_proxy(arr)) return js_mkundef();

  ant_offset_t doff = get_dense_buf(arr);
  if (!doff) return js_mkundef();
  
  ant_value_t *dense = dense_data(doff);
  if (!dense) return js_mkundef();

  ant_offset_t dense_len = dense_iterable_length(js, arr);
  if (dense_len != len) return js_mkundef();

  if (!array_may_have_holes(arr)) {
    for (ant_offset_t i = start; i < len; i++) 
      if (array_includes_matches(js, query, dense[i])) return mkval(kTypeBool, 1);
    return mkval(kTypeBool, 0);
  }

  for (ant_offset_t i = start; i < len; i++) {
    ant_value_t val = dense[i];
    if (is_empty_slot(val)) return js_mkundef();
    if (array_includes_matches(js, query, val)) return mkval(kTypeBool, 1);
  }

  return mkval(kTypeBool, 0);
}

static ant_value_t array_includes_array_slow(
  ant_t *js, ant_value_t arr, const array_includes_query_t *query, ant_offset_t len, ant_offset_t start
) {
  bool skip_hole_lookups =
    query->search_type != kTypeUndefined &&
    array_includes_can_skip_hole_lookups(js, arr);
  
  ant_value_t *dense = NULL;
  ant_offset_t dense_len = 0;

  if (skip_hole_lookups) {
    if (!array_may_have_dense_elements(arr)) return mkval(kTypeBool, 0);
    
    ant_offset_t doff = get_dense_buf(arr);
    if (doff) {
      dense = dense_data(doff);
      dense_len = dense_iterable_length(js, arr);
    }
    
    if (dense) for (ant_offset_t i = start; i < dense_len; i++) {
      ant_value_t val = dense[i];
      if (is_empty_slot(val)) continue;
      if (array_includes_matches(js, query, val)) return mkval(kTypeBool, 1);
    }
    
    return mkval(kTypeBool, 0);
  }

  for (ant_offset_t i = start; i < len; i++) {
    ant_value_t val;
    
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (uint64_t)i);
    
    val = array_includes_get_array_index_value(js, arr, i, idxstr, idxlen);
    if (is_err(val)) return val;
    
    if (array_includes_matches(js, query, val)) return mkval(kTypeBool, 1);
  }

  return mkval(kTypeBool, 0);
}

static ant_value_t array_includes_generic(
  ant_t *js, ant_value_t arr, const array_includes_query_t *query, ant_value_t *args, int nargs
) {
  ant_value_t len_val = array_includes_length_value(js, arr);
  if (is_err(len_val)) return len_val;

  ant_offset_t len = array_includes_length_from_number(js_to_number(js, len_val));
  if (len == 0) return mkval(kTypeBool, 0);
  
  ant_offset_t start = array_includes_start_index(js, args, nargs, len);
  if (start >= len) return mkval(kTypeBool, 0);

  for (ant_offset_t i = start; i < len; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (uint64_t)i);

    ant_value_t val = array_includes_get_index_value(js, arr, i, idxstr, idxlen);
    if (is_err(val)) return val;
    if (array_includes_matches(js, query, val)) return mkval(kTypeBool, 1);
  }
  
  return mkval(kTypeBool, 0);
}

ant_value_t js_array_includes_call(ant_t *js, ant_value_t arr, ant_value_t *args, int nargs) {
  arr = array_method_receiver(js, arr, "includes");
  if (is_err(arr)) return arr;
  
  array_includes_query_t query = array_includes_prepare_query(
    (nargs > 0) ? args[0] : js_mkundef()
  );

  if (real_array_obj_ptr(arr) && !is_proxy(arr)) {
    ant_offset_t len = get_array_length(js, arr);
    if (len == 0) return mkval(kTypeBool, 0);
    
    ant_offset_t start = array_includes_start_index(js, args, nargs, len);
    if (start >= len) return mkval(kTypeBool, 0);
    
    ant_value_t fast = array_includes_dense_fast(js, arr, &query, len, start);
    if (vtype(fast) != kTypeUndefined) return fast;
    
    return array_includes_array_slow(js, arr, &query, len, start);
  }

  return array_includes_generic(js, arr, &query, args, nargs);
}

bool js_is_array_includes_builtin(ant_value_t func) {
  return vtype(func) == kTypeBuiltin && js_cfunc_same_entrypoint(func, builtin_array_includes);
}

ant_value_t builtin_array_includes(ant_params_t) {
  return js_array_includes_call(js, js->this_val, args, nargs);
}

static ant_value_t builtin_array_every(ant_params_t) {
  ant_value_t arr = js->this_val;
  
  arr = array_method_receiver(js, arr, "every");
  if (is_err(arr)) return arr;
  
  ant_value_t callback = require_callback(js, args, nargs, "every");
  if (is_err(callback)) return callback;
  ant_value_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();
  
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  sv_callback_t cb = sv_callback_prepare(js, callback, this_arg);
  ant_object_t *elems = array_method_elements(arr);
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t val;
    ant_value_t has = array_method_element(js, arr, elems, i, &val);
    if (is_err(has)) return has;
    if (has != js_true) continue;
    ant_value_t call_args[3] = { val, tov((double)i), arr };
    ant_value_t result = sv_callback_call(js->vm, js, &cb, call_args, 3);
    if (is_err(result)) return result;
    if (!js_truthy(js, result)) return mkval(kTypeBool, 0);
  }
  
  return mkval(kTypeBool, 1);
}

static ant_value_t builtin_array_forEach(ant_params_t) {
  ant_value_t arr = js->this_val;
  
  arr = array_method_receiver(js, arr, "forEach");
  if (is_err(arr)) return arr;
  
  ant_value_t callback = require_callback(js, args, nargs, "forEach");
  if (is_err(callback)) return callback;
  ant_value_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();
  
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  sv_callback_t cb = sv_callback_prepare(js, callback, this_arg);
  ant_object_t *elems = array_method_elements(arr);
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t val;
    ant_value_t has = array_method_element(js, arr, elems, i, &val);
    if (is_err(has)) return has;
    if (has != js_true) continue;
    ant_value_t call_args[3] = { val, tov((double)i), arr };
    ant_value_t result = sv_callback_call(js->vm, js, &cb, call_args, 3);
    if (is_err(result)) return result;
  }
  
  return js_mkundef();
}

static ant_value_t builtin_array_reverse(ant_params_t) {
  (void)args; (void)nargs;
  ant_value_t arr = js->this_val;

  arr = array_method_receiver(js, arr, "reverse");
  if (is_err(arr)) return arr;
  ant_offset_t string_len;
  if (array_string_wrapper_length(js, arr, &string_len) && string_len > 1) return array_string_wrapper_write_error(js, "0");
  
  ant_object_t *frozen = is_proxy(arr) ? NULL : js_obj_ptr(js_as_obj(arr));
  if (frozen && frozen->flags.frozen) {
    ant_offset_t frozen_len = 0;
    ant_value_t len_result = array_like_length_checked(js, arr, &frozen_len);
    if (is_err(len_result)) return len_result;
    if (frozen_len > 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot assign to read only property '0' of object");
  }

  if (is_proxy(arr)) {
    PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
    if (len <= 1) return arr;
    ant_value_t read_from = proxy_read_target(js, arr);
    ant_offset_t lower = 0;
    while (lower < len / 2) {
      ant_offset_t upper_idx = len - lower - 1;
      bool lower_exists = arr_has(js, read_from, lower);
      bool upper_exists = arr_has(js, read_from, upper_idx);
      char lower_key[16]; size_t lower_len = uint_to_str(lower_key, sizeof(lower_key), (unsigned)lower);
      char upper_key[16]; size_t upper_len = uint_to_str(upper_key, sizeof(upper_key), (unsigned)upper_idx);
      ant_value_t lower_val = lower_exists ? proxy_get(js, arr, lower_key, lower_len) : js_mkundef();
      if (is_err(lower_val)) return lower_val;
      ant_value_t upper_val = upper_exists ? proxy_get(js, arr, upper_key, upper_len) : js_mkundef();
      if (is_err(upper_val)) return upper_val;
      ant_value_t step = js_mkundef();
      if (lower_exists && upper_exists) {
        step = array_set_index_throw(js, arr, lower, upper_val);
        if (!is_err(step)) step = array_set_index_throw(js, arr, upper_idx, lower_val);
      } else if (upper_exists) {
        step = array_set_index_throw(js, arr, lower, upper_val);
        if (!is_err(step)) step = proxy_delete(js, arr, upper_key, upper_len);
      } else if (lower_exists) {
        step = array_set_index_throw(js, arr, upper_idx, lower_val);
        if (!is_err(step)) step = proxy_delete(js, arr, lower_key, lower_len);
      }
      if (is_err(step)) return step;
      lower++;
    } return arr;
  }

  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  if (len <= 1) return arr;

  ant_offset_t doff = get_dense_buf(arr);
  if (doff) {
    for (ant_offset_t i = 0; i < len / 2; i++) {
      ant_value_t a = dense_get(doff, i);
      ant_value_t b = dense_get(doff, len - 1 - i);
      dense_set(js, doff, i, b);
      dense_set(js, doff, len - 1 - i, a);
    }
    return arr;
  }

  for (ant_offset_t lower = 0; lower < len / 2; lower++) {
    ant_offset_t upper = len - lower - 1;
    bool lower_exists = arr_has(js, arr, lower);
    bool upper_exists = arr_has(js, arr, upper);
    ant_value_t lower_val = lower_exists ? arr_get(js, arr, lower) : js_mkundef();
    ant_value_t upper_val = upper_exists ? arr_get(js, arr, upper) : js_mkundef();

    if (lower_exists && upper_exists) {
      arr_set(js, arr, lower, upper_val);
      arr_set(js, arr, upper, lower_val);
    } else if (upper_exists) {
      arr_set(js, arr, lower, upper_val);
      arr_del(js, arr, upper);
    } else if (lower_exists) {
      arr_set(js, arr, upper, lower_val);
      arr_del(js, arr, lower);
    }
  }
  return arr;
}

static ant_value_t builtin_array_map(ant_params_t) {
  ant_value_t arr = js->this_val;
  
  arr = array_method_receiver(js, arr, "map");
  if (is_err(arr)) return arr;
  
  ant_value_t callback = require_callback(js, args, nargs, "map");
  if (is_err(callback)) return callback;
  ant_value_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();
  
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  sv_callback_t cb = sv_callback_prepare(js, callback, this_arg);
  ant_object_t *elems = array_method_elements(arr);
  
  bool intrinsic = false;
  ant_value_t result = array_species_create(js, arr, len, &intrinsic);
  if (is_err(result)) return result;
  bool skipped = false;
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t val;
    ant_value_t has = array_method_element(js, arr, elems, i, &val);
    if (is_err(has)) return has;
    if (has != js_true) { skipped = true; continue; }
    ant_value_t call_args[3] = { val, tov((double)i), arr };
    ant_value_t mapped = sv_callback_call(js->vm, js, &cb, call_args, 3);
    if (is_err(mapped)) return mapped;
    ant_value_t set = array_result_set(js, result, intrinsic, i, mapped);
    if (is_err(set)) return set;
  }
  
  if (intrinsic && !skipped) array_mark_packed(result);
  return result;
}

static ant_value_t builtin_array_filter(ant_params_t) {
  ant_value_t arr = js->this_val;
  
  arr = array_method_receiver(js, arr, "filter");
  if (is_err(arr)) return arr;
  
  ant_value_t callback = require_callback(js, args, nargs, "filter");
  if (is_err(callback)) return callback;
  ant_value_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();
  
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  sv_callback_t cb = sv_callback_prepare(js, callback, this_arg);
  ant_object_t *elems = array_method_elements(arr);
  
  bool intrinsic = false;
  ant_value_t result = array_species_create(js, arr, 0, &intrinsic);
  if (is_err(result)) return result;
  
  ant_offset_t result_idx = 0;
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t val;
    ant_value_t has = array_method_element(js, arr, elems, i, &val);
    if (is_err(has)) return has;
    if (has != js_true) continue;
    ant_value_t call_args[3] = { val, tov((double)i), arr };
    ant_value_t test = sv_callback_call(js->vm, js, &cb, call_args, 3);
    
    if (is_err(test)) return test;
    if (!js_truthy(js, test)) continue;
    ant_value_t set = array_result_set(js, result, intrinsic, result_idx++, val);
    if (is_err(set)) return set;
  }
  
  return result;
}

static ant_value_t builtin_array_reduce(ant_params_t) {
  ant_value_t arr = js->this_val;
  
  arr = array_method_receiver(js, arr, "reduce");
  if (is_err(arr)) return arr;
  
  ant_value_t callback = require_callback(js, args, nargs, "reduce");
  if (is_err(callback)) return callback;
  bool has_initial = (nargs >= 2);
  
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  sv_callback_t cb = sv_callback_prepare(js, callback, js_mkundef());
  ant_object_t *elems = array_method_elements(arr);
  
  ant_value_t accumulator = has_initial ? args[1] : js_mkundef();
  bool first = !has_initial;
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t val;
    ant_value_t has = array_method_element(js, arr, elems, i, &val);
    if (is_err(has)) return has;
    if (has != js_true) continue;
    if (first) { accumulator = val; first = false; continue; }
    ant_value_t call_args[4] = { accumulator, val, tov((double)i), arr };
    accumulator = sv_callback_call(js->vm, js, &cb, call_args, 4);
    if (is_err(accumulator)) return accumulator;
  }
  
  if (first) return js_mkerr(js, "reduce of empty array with no initial value");
  return accumulator;
}

static inline ant_value_t *packed_array_data_for_length(ant_t *js, ant_value_t arr, ant_offset_t len) {
  if (vtype(arr) != kTypeArray || array_may_have_holes(arr)) return NULL;

  ant_offset_t doff = get_dense_buf(arr);
  ant_value_t *dense = doff ? dense_data(doff) : NULL;
  if (!dense || dense_iterable_length(js, arr) != len) return NULL;

  for (ant_offset_t i = 0; i < len; i++)
    if (is_empty_slot(dense[i])) return NULL;

  return dense;
}

static inline ant_value_t flat_should_spread(ant_t *js, ant_value_t val, int depth, bool *out) {
  *out = false;
  if (depth <= 0) return js_mkundef();

  if (real_array_obj_ptr(val)) {
    *out = true;
    return js_mkundef();
  }

  return is_proxy(val) ? js_is_array_value_checked(js, val, out) : js_mkundef();
}

static inline ant_value_t flat_helper(ant_t *js, ant_value_t arr, ant_value_t result, bool fresh, ant_offset_t *result_idx, int depth) {
  bool input_is_array = real_array_obj_ptr(arr) != NULL;
  ant_offset_t len = input_is_array ? get_array_length(js, arr) : 0;
  
  if (!input_is_array) {
    ant_value_t len_result = proxy_aware_length(js, arr, &len);
    if (is_err(len_result)) return len_result;
  }
  
  if (len == 0) return js_mkundef();

  ant_value_t *dense = packed_array_data_for_length(js, arr, len);
  if (dense) {
    for (ant_offset_t i = 0; i < len; i++) {
      bool spread = false;
      ant_value_t spread_res = flat_should_spread(js, dense[i], depth, &spread);
      if (is_err(spread_res)) return spread_res;
      if (!spread) {
        ant_value_t set = array_result_set(js, result, fresh, (*result_idx)++, dense[i]);
        if (is_err(set)) return set;
        continue;
      }

      ant_value_t flat_res = flat_helper(js, dense[i], result, fresh, result_idx, depth - 1);
      if (is_err(flat_res)) return flat_res;
    }
    
    return js_mkundef();
  }

  for (ant_offset_t i = 0; i < len; i++) {
    if (input_is_array) {
      if (!arr_has(js, arr, i)) continue;
    } else {
      ant_value_t has = array_method_has_index(js, arr, i);
      if (is_err(has)) return has;
      if (!js_truthy(js, has)) continue;
    }

    ant_value_t val = input_is_array ? arr_get(js, arr, i) : array_method_get_index(js, arr, i);
    if (is_err(val)) return val;

    bool spread = false;
    ant_value_t spread_res = flat_should_spread(js, val, depth, &spread);
    if (is_err(spread_res)) return spread_res;
    
    if (!spread) {
      ant_value_t set = array_result_set(js, result, fresh, (*result_idx)++, val);
      if (is_err(set)) return set;
      continue;
    }

    ant_value_t flat_res = flat_helper(js, val, result, fresh, result_idx, depth - 1);
    if (is_err(flat_res)) return flat_res;
  }

  return js_mkundef();
}

static inline ant_value_t flat_append_mapped_value(ant_t *js, ant_value_t mapped, ant_value_t result, bool fresh, ant_offset_t *result_idx) {
  bool spread = false;
  ant_value_t spread_res = flat_should_spread(js, mapped, 1, &spread);
  if (is_err(spread_res)) return spread_res;
  if (!spread) return array_result_set(js, result, fresh, (*result_idx)++, mapped);

  return flat_helper(js, mapped, result, fresh, result_idx, 0);
}

static ant_value_t builtin_array_flat(ant_params_t) {
  ant_value_t arr = js->this_val;
  arr = array_method_receiver(js, arr, "flat");
  if (is_err(arr)) return arr;
  
  int depth = 1;
  if (nargs >= 1 && vtype(args[0]) == kTypeNumber) {
    depth = (int) tod(args[0]);
    if (depth < 0) depth = 0;
  }
  
  bool intrinsic = false;
  ant_value_t result = array_species_create(js, arr, 0, &intrinsic);
  if (is_err(result)) return result;
  ant_offset_t result_idx = 0;
  
  ant_value_t flat_res = flat_helper(js, arr, result, intrinsic, &result_idx, depth);
  if (is_err(flat_res)) return flat_res;
  return result;
}

static ant_value_t builtin_array_concat(ant_params_t) {
  ant_value_t arr = js->this_val;
  arr = array_method_receiver(js, arr, "concat");
  if (is_err(arr)) return arr;
  
  bool intrinsic = false;
  ant_value_t result = array_species_create(js, arr, 0, &intrinsic);
  if (is_err(result)) return result;
  
  ant_offset_t result_idx = 0;
  for (int a = -1; a < nargs; a++) {
    ant_value_t arg = (a < 0) ? arr : args[a];
    bool spreadable = false;
    
    if (vtype(arg) == kTypeArray || vtype(arg) == kTypeObject) {
      bool array_default_spreadable = real_array_obj_ptr(arg) != NULL;
      if (!array_default_spreadable && is_proxy(arg)) {
        ant_value_t target = proxy_read_target(js, arg);
        array_default_spreadable = real_array_obj_ptr(target) != NULL;
      }
      
      ant_value_t spread_val = js_get_sym(js, arg, js->sym.isConcatSpreadable_sym);
      if (is_err(spread_val)) return spread_val;
      if (vtype(spread_val) == kTypeUndefined) spreadable = array_default_spreadable;
      else spreadable = js_truthy(js, spread_val);
    }
    
    if (spreadable) {
      ant_offset_t arg_len = 0;
      ant_value_t len_result = array_like_length_checked(js, arg, &arg_len);
      if (is_err(len_result)) return len_result;
      
      ant_object_t *src = array_dense_source(js, arg);
      if (src && (arg_len > src->u.array.len || arg_len > src->u.array.cap)) src = NULL;
      ant_offset_t rdoff = real_array_obj_ptr(result) && (intrinsic || array_result_extensible(result)) ? get_dense_buf(result) : 0;
      if (rdoff && src && dense_capacity(rdoff) < result_idx + arg_len)
        rdoff = dense_grow(js, result, result_idx + arg_len);
      
      for (ant_offset_t i = 0; i < arg_len; i++, result_idx++) {
        ant_value_t elem;
        if (src) {
          elem = src->u.array.data[i];
          if (is_empty_slot(elem)) continue;
        } else {
          ant_value_t has = array_method_has_index(js, arg, i);
          if (is_err(has)) return has;
          if (!js_truthy(js, has)) continue;
          elem = array_method_get_index(js, arg, i);
          if (is_err(elem)) return elem;
        }
        if (rdoff && result_idx < dense_capacity(rdoff) && result_idx <= get_array_length(js, result)) {
          dense_set(js, rdoff, result_idx, elem);
          if (result_idx == get_array_length(js, result)) array_len_set(js, result, result_idx + 1);
        } else {
          ant_value_t set = array_result_set(js, result, intrinsic, result_idx, elem);
          if (is_err(set)) return set;
        }
      }
    } else {
      ant_value_t set = array_result_set(js, result, intrinsic, result_idx++, arg);
      if (is_err(set)) return set;
    }
  }
  
  if (real_array_obj_ptr(result)) {
    if (get_array_length(js, result) < result_idx) {
      array_mark_may_have_holes(result);
      array_len_set(js, result, result_idx);
    }
  } else {
    ant_value_t set = js_setprop(js, result, js->length_str, tov((double)result_idx));
    if (is_err(set)) return set;
  }
  
  return result;
}

static ant_value_t builtin_array_at(ant_params_t) {
  ant_value_t arr = js->this_val;
  arr = array_method_receiver(js, arr, "at");
  if (is_err(arr)) return arr;
  
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  
  ant_value_t rel_val = nargs > 0 ? js_to_numeric(js, args[0]) : tov(0);
  if (is_err(rel_val)) return rel_val;
  if (vtype(rel_val) == kTypeBigInt)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert a BigInt value to a number");
  
  double rel = tod(rel_val);
  rel = isnan(rel) ? 0 : trunc(rel);
  double k = rel < 0 ? (double)len + rel : rel;
  if (k < 0 || k >= (double)len) return js_mkundef();
  
  return array_method_get_index(js, arr, (ant_offset_t)k);
}

static ant_value_t builtin_array_fill(ant_params_t) {
  ant_value_t arr = js->this_val;
  arr = array_method_receiver(js, arr, "fill");
  if (is_err(arr)) return arr;

  ant_value_t value = nargs >= 1 ? args[0] : js_mkundef();

  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  
  ant_offset_t start = 0, end = len;
  if (nargs >= 2 && vtype(args[1]) == kTypeNumber) {
    int s = (int) tod(args[1]);
    if (s < 0) s = (int)len + s;
    if (s < 0) s = 0;
    start = (ant_offset_t) s;
  }
  if (nargs >= 3 && vtype(args[2]) == kTypeNumber) {
    int e = (int) tod(args[2]);
    if (e < 0) e = (int)len + e;
    if (e < 0) e = 0;
    end = (ant_offset_t) e;
  }
  if (start > len) start = len;
  if (end > len) end = len;
  
  ant_offset_t string_len;
  if (start < end && array_string_wrapper_length(js, arr, &string_len)) {
    char key[24];
    uint_to_str(key, sizeof(key), (uint64_t)start);
    return array_string_wrapper_write_error(js, key);
  }
  
  ant_object_t *dense = real_array_obj_ptr(arr);
  if (
    !is_proxy(arr) && array_dense_covers(dense, end) &&
    dense->flags.extensible && !dense->flags.sealed && !dense->flags.frozen
  ) {
    ant_offset_t doff = get_dense_buf(arr);
    for (ant_offset_t i = start; i < end; i++) dense_set(js, doff, i, value);
    return arr;
  }
  
  for (ant_offset_t i = start; i < end; i++) {
    ant_value_t set = array_set_index_throw(js, arr, i, value);
    if (is_err(set)) return set;
  }
  
  return arr;
}

static ant_value_t array_find_impl(ant_native_params_t, bool return_index, const char *name) {
  ant_value_t arr = array_method_receiver(js, js->this_val, name);
  if (is_err(arr)) return arr;
  
  ant_value_t callback = require_callback(js, args, nargs, name);
  if (is_err(callback)) return callback;
  ant_value_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();
  
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  sv_callback_t cb = sv_callback_prepare(js, callback, this_arg);
  if (len == 0) return return_index ? tov(-1) : js_mkundef();
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t val = array_method_get_index(js, arr, i);
    if (is_err(val)) return val;
    
    ant_value_t call_args[3] = { val, tov((double)i), arr };
    ant_value_t result = sv_callback_call(js->vm, js, &cb, call_args, 3);
    
    if (is_err(result)) return result;
    if (js_truthy(js, result)) return return_index ? tov((double)i) : val;
  }
  
  return return_index ? tov(-1) : js_mkundef();
}

static ant_value_t builtin_array_find(ant_params_t) {
  return array_find_impl(js, args, nargs, false, "find");
}

static ant_value_t builtin_array_findIndex(ant_params_t) {
  return array_find_impl(js, args, nargs, true, "findIndex");
}

static ant_value_t array_find_last_impl(ant_native_params_t, bool return_index, const char *name) {
  ant_value_t arr = array_method_receiver(js, js->this_val, name);
  if (is_err(arr)) return arr;
  
  ant_value_t callback = require_callback(js, args, nargs, name);
  if (is_err(callback)) return callback;
  ant_value_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();
  
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  sv_callback_t cb = sv_callback_prepare(js, callback, this_arg);
  if (len == 0) return return_index ? tov(-1) : js_mkundef();
  
  for (ant_offset_t i = len; i > 0; i--) {
    ant_value_t val = array_method_get_index(js, arr, i - 1);
    if (is_err(val)) return val;
    
    ant_value_t call_args[3] = { val, tov((double)(i - 1)), arr };
    ant_value_t result = sv_callback_call(js->vm, js, &cb, call_args, 3);
    
    if (is_err(result)) return result;
    if (js_truthy(js, result)) return return_index ? tov((double)(i - 1)) : val;
  }
  
  return return_index ? tov(-1) : js_mkundef();
}

static ant_value_t builtin_array_findLast(ant_params_t) {
  return array_find_last_impl(js, args, nargs, false, "findLast");
}

static ant_value_t builtin_array_findLastIndex(ant_params_t) {
  return array_find_last_impl(js, args, nargs, true, "findLastIndex");
}

static ant_value_t builtin_array_flatMap(ant_params_t) {
  ant_value_t arr = js->this_val;
  arr = array_method_receiver(js, arr, "flatMap");
  if (is_err(arr)) return arr;
  
  ant_value_t callback = require_callback(js, args, nargs, "flatMap");
  if (is_err(callback)) return callback;
  ant_value_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();
  bool input_is_array = real_array_obj_ptr(arr) != NULL;
  ant_offset_t len = input_is_array ? get_array_length(js, arr) : 0;
  if (!input_is_array) {
    ant_value_t len_result = proxy_aware_length(js, arr, &len);
    if (is_err(len_result)) return len_result;
  }
  
  bool intrinsic = false;
  ant_value_t result = array_species_create(js, arr, 0, &intrinsic);
  if (is_err(result)) return result;
  ant_offset_t result_idx = 0;
  sv_callback_t cb = sv_callback_prepare(js, callback, this_arg);
  ant_object_t *elems = array_method_elements(arr);

  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t elem;
    ant_value_t has = array_method_element(js, arr, elems, i, &elem);
    if (is_err(has)) return has;
    if (has != js_true) continue;
    ant_value_t call_args[3] = { elem, tov((double)i), arr };
    ant_value_t mapped = sv_callback_call(js->vm, js, &cb, call_args, 3);
    if (is_err(mapped)) return mapped;
    ant_value_t flat_res = flat_append_mapped_value(js, mapped, result, intrinsic, &result_idx);
    if (is_err(flat_res)) return flat_res;
  }
  
  return result;
}

static ant_value_t builtin_array_indexOf(ant_params_t) {
  ant_value_t arr = js->this_val;
  arr = array_method_receiver(js, arr, "indexOf");
  if (is_err(arr)) return arr;

  if (nargs == 0) return tov(-1);  
  ant_value_t search = args[0];
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  
  ant_offset_t start = 0;
  if (nargs >= 2 && vtype(args[1]) == kTypeNumber) {
    int s = (int) tod(args[1]);
    if (s < 0) s = (int)len + s;
    if (s < 0) s = 0;
    start = (ant_offset_t) s;
  }
  
  ant_object_t *dense = array_dense_source(js, arr);
  if (dense && len <= dense->u.array.len && len <= dense->u.array.cap) {
    for (ant_offset_t i = start; i < len; i++) {
      ant_value_t elem = dense->u.array.data[i];
      if (!is_empty_slot(elem) && array_search_eq(js, elem, search)) return tov((double)i);
    }
    return tov(-1);
  }
  
  for (ant_offset_t i = start; i < len; i++) {
    ant_value_t elem = array_method_get_index(js, arr, i);
    if (is_err(elem)) return elem;
    if (vtype(elem) == kTypeUndefined) {
      ant_value_t has = array_method_has_index(js, arr, i);
      if (is_err(has)) return has;
      if (!js_truthy(js, has)) continue;
    }
    if (strict_eq_values(js, elem, search)) return tov((double)i);
  }
  return tov(-1);
}

static ant_value_t builtin_array_lastIndexOf(ant_params_t) {
  ant_value_t arr = js->this_val;
  arr = array_method_receiver(js, arr, "lastIndexOf");
  if (is_err(arr)) return arr;
  if (nargs == 0) return tov(-1);
  
  ant_value_t search = args[0];
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  
  int start = (int)len - 1;
  if (nargs >= 2 && vtype(args[1]) == kTypeNumber) {
    start = (int) tod(args[1]);
    if (start < 0) start = (int)len + start;
  }
  if (start >= (int)len) start = (int)len - 1;
  
  ant_object_t *dense = array_dense_source(js, arr);
  if (dense && len <= dense->u.array.len && len <= dense->u.array.cap) {
    for (int i = start; i >= 0; i--) {
      ant_value_t elem = dense->u.array.data[i];
      if (!is_empty_slot(elem) && array_search_eq(js, elem, search)) return tov((double)i);
    }
    return tov(-1);
  }
  
  for (int i = start; i >= 0; i--) {
    ant_value_t elem = array_method_get_index(js, arr, (ant_offset_t)i);
    if (is_err(elem)) return elem;
    if (vtype(elem) == kTypeUndefined) {
      ant_value_t has = array_method_has_index(js, arr, (ant_offset_t)i);
      if (is_err(has)) return has;
      if (!js_truthy(js, has)) continue;
    }
    if (strict_eq_values(js, elem, search)) return tov((double)i);
  }
  return tov(-1);
}

static ant_value_t builtin_array_reduceRight(ant_params_t) {
  ant_value_t arr = js->this_val;
  arr = array_method_receiver(js, arr, "reduceRight");
  if (is_err(arr)) return arr;
  if (nargs == 0 || vtype(args[0]) != kTypeFunction) {
    return js_mkerr(js, "reduceRight requires a function argument");
  }
  
  ant_value_t callback = args[0];
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  sv_callback_t cb = sv_callback_prepare(js, callback, js_mkundef());
  
  ant_object_t *elems = array_method_elements(arr);
  ant_value_t accumulator = nargs >= 2 ? args[1] : js_mkundef();
  bool first = nargs < 2;
  
  for (ant_offset_t i = len; i > 0; i--) {
    ant_value_t val;
    ant_value_t has = array_method_element(js, arr, elems, i - 1, &val);
    if (is_err(has)) return has;
    if (has != js_true) continue;
    if (first) { accumulator = val; first = false; continue; }
    ant_value_t call_args[4] = { accumulator, val, tov((double)(i - 1)), arr };
    accumulator = sv_callback_call(js->vm, js, &cb, call_args, 4);
    if (is_err(accumulator)) return accumulator;
  }
  
  if (first) return js_mkerr_typed(js, JS_ERR_TYPE, "reduceRight of empty array with no initial value");
  return accumulator;
}

static ant_value_t builtin_array_shift(ant_params_t) {
  ant_object_t *fast = array_mutable_dense(js, js->this_val);
  if (__builtin_expect(fast != NULL, 1)) {
    uint32_t len = fast->u.array.len;
    if (len == 0) return js_mkundef();
    if (__builtin_expect(fast->flags.may_have_holes && !array_proto_chain_plain(js, fast), 0))
      return array_shift_generic(js, js->this_val);
    ant_value_t *data = fast->u.array.data;
    ant_value_t first = data[0];
    memmove(&data[0], &data[1], sizeof(ant_value_t) * (size_t)(len - 1));
    gc_elements_moved(fast);
    data[len - 1] = T_EMPTY;
    fast->u.array.len = len - 1;
    return is_empty_slot(first) ? js_mkundef() : first;
  }
  
  ant_value_t arr = js->this_val;
  arr = array_method_receiver(js, arr, "shift");
  if (is_err(arr)) return arr;
  ant_offset_t string_len;
  if (array_string_wrapper_length(js, arr, &string_len)) return array_string_wrapper_write_error(js, "length");
  if (js_is_arguments(arr)) return arguments_shift(js, arr);

  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  if (len == 0) return array_length_readonly(js, arr) ? array_shift_generic(js, arr) : js_mkundef();

  if (is_proxy(arr)) {
    ant_value_t read_from = proxy_read_target(js, arr);
    ant_value_t first = proxy_get(js, arr, "0", 1);
    if (is_err(first)) return first;

    for (ant_offset_t i = 1; i < len; i++) {
      char src[16];
      size_t srclen = uint_to_str(src, sizeof(src), (unsigned)i);
      char dst[16];
      size_t dstlen = uint_to_str(dst, sizeof(dst), (unsigned)(i - 1));
      if (arr_has(js, read_from, i)) {
        ant_value_t elem = proxy_get(js, arr, src, srclen);
        if (is_err(elem)) return elem;
        js_setprop(js, arr, js_mkstr(js, dst, dstlen), elem);
      } else {
        ant_value_t del = proxy_delete(js, arr, dst, dstlen);
        if (is_err(del)) return del;
      }
    }

    ant_value_t del = proxy_delete_index(js, arr, len - 1);
    if (is_err(del)) return del;
    js_setprop(js, arr, js->length_str, tov((double)(len - 1)));
    return first;
  }

  ant_offset_t doff = get_dense_buf(arr);
  if (doff && !is_proxy(arr)) {
    ant_offset_t d_len = dense_iterable_length(js, arr);
    ant_object_t *dense = dense_obj(doff);
    if (len != d_len || dense->flags.sealed || array_length_readonly(js, arr)) goto shift_slow;
    if (dense->flags.may_have_holes && !array_proto_chain_plain(js, dense)) goto shift_slow;
    if (d_len == 0) return js_mkundef();
    ant_value_t *d = dense_data(doff);
    if (!d) return js_mkundef();
    ant_value_t first = dense_get(doff, 0);
    if (is_empty_slot(first)) first = js_mkundef();
    memmove(&d[0], &d[1], sizeof(ant_value_t) * (size_t)(d_len - 1));
    gc_elements_moved(dense_obj(doff));
    dense_set(js, doff, d_len - 1, T_EMPTY);
    array_len_set(js, arr, len - 1);
    return first;
  }

  shift_slow:
  return array_shift_generic(js, arr);
}

static ant_value_t builtin_array_unshift(ant_params_t) {
  ant_value_t arr = js->this_val;
  arr = array_method_receiver(js, arr, "unshift");
  if (is_err(arr)) return arr;
  ant_offset_t string_len;
  if (array_string_wrapper_length(js, arr, &string_len)) return array_string_wrapper_write_error(js, "length");
  if (array_length_readonly(js, arr)) return array_length_readonly_error(js);
  if (js_is_arguments(arr)) return arguments_unshift(js, arr, args, nargs);

  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);

  if (is_proxy(arr)) {
    ant_value_t read_from = proxy_read_target(js, arr);
    for (int i = (int)len - 1; i >= 0; i--) {
      char src[16];
      size_t srclen = uint_to_str(src, sizeof(src), (unsigned)i);
      char dst[16];
      size_t dstlen = uint_to_str(dst, sizeof(dst), (unsigned)(i + nargs));
      if (arr_has(js, read_from, (ant_offset_t)i)) {
        ant_value_t elem = proxy_get(js, arr, src, srclen);
        if (is_err(elem)) return elem;
        js_setprop(js, arr, js_mkstr(js, dst, dstlen), elem);
      } else {
        ant_value_t del = proxy_delete(js, arr, dst, dstlen);
        if (is_err(del)) return del;
      }
    }

    for (int i = 0; i < nargs; i++) {
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
      js_setprop(js, arr, js_mkstr(js, idxstr, idxlen), args[i]);
    }

    ant_offset_t new_len = len + nargs;
    js_setprop(js, arr, js->length_str, tov((double)new_len));
    return tov((double)new_len);
  }

  ant_offset_t doff = get_dense_buf(arr);
  if (doff && !is_proxy(arr)) {
    ant_offset_t d_len = dense_iterable_length(js, arr);
    if (len != d_len) goto unshift_slow;
    ant_offset_t new_len = len + nargs;
    ant_offset_t cap = dense_capacity(doff);
    if (new_len > cap) {
      doff = dense_grow(js, arr, new_len);
      if (doff == 0) return js_mkerr(js, "oom");
    }
    ant_value_t *d = dense_data(doff);
    if (!d) return js_mkerr(js, "oom");
    memmove(&d[nargs], &d[0], sizeof(ant_value_t) * (size_t)d_len);
    gc_elements_moved(dense_obj(doff));
    for (int i = 0; i < nargs; i++)
      dense_set(js, doff, (ant_offset_t)i, args[i]);
    array_len_set(js, arr, new_len);
    return tov((double) new_len);
  }

  unshift_slow:
  ant_value_t read_from = is_proxy(arr) ? proxy_read_target(js, arr) : arr;

  for (int i = (int)len - 1; i >= 0; i--) {
    if (arr_has(js, read_from, (ant_offset_t)i)) {
      ant_value_t elem = arr_get(js, read_from, (ant_offset_t)i);
      char dst[16];
      size_t dstlen = uint_to_str(dst, sizeof(dst), (unsigned)(i + nargs));
      js_setprop(js, arr, js_mkstr(js, dst, dstlen), elem);
    }
  }
  
  for (int i = 0; i < nargs; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
    ant_value_t key = js_mkstr(js, idxstr, idxlen);
    js_setprop(js, arr, key, args[i]);
  }
  
  ant_offset_t new_len = len + nargs;
  if (array_obj_ptr(arr)) array_len_set(js, arr, new_len);
  else js_setprop(js, arr, js->length_str, tov((double) new_len));
  
  return tov((double) new_len);
}

static ant_value_t builtin_array_some(ant_params_t) {
  ant_value_t arr = js->this_val;
  
  arr = array_method_receiver(js, arr, "some");
  if (is_err(arr)) return arr;
  
  ant_value_t callback = require_callback(js, args, nargs, "some");
  if (is_err(callback)) return callback;
  ant_value_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();
  
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  sv_callback_t cb = sv_callback_prepare(js, callback, this_arg);
  ant_object_t *elems = array_method_elements(arr);
  if (len == 0) return mkval(kTypeBool, 0);
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t val;
    ant_value_t has = array_method_element(js, arr, elems, i, &val);
    if (is_err(has)) return has;
    if (has != js_true) continue;
    
    ant_value_t call_args[3] = { val, tov((double)i), arr };
    ant_value_t result = sv_callback_call(js->vm, js, &cb, call_args, 3);
    
    if (is_err(result)) return result;
    if (js_truthy(js, result)) return mkval(kTypeBool, 1);
  }
  
  return mkval(kTypeBool, 0);
}

static constexpr ant_offset_t SORT_INLINE_KEYS = 32;

static inline uint64_t sort_int_string_key(int32_t value) {
  uint64_t digits = value < 0 ? (uint64_t)-(int64_t)value : (uint64_t)value;
  uint64_t count = 1;
  for (uint64_t v = digits; v >= 10; v /= 10) count++;
  for (uint64_t d = count; d < 10; d++) digits *= 10;
  return ((uint64_t)(value >= 0) << 40) | (digits << 4) | count;
}

static inline int sort_string_cmp(ant_t *js, ant_value_t a, ant_value_t b) {
  ant_offset_t len_a, len_b;
  const char *sa = (const char *)(uintptr_t)vstr(js, a, &len_a);
  const char *sb = (const char *)(uintptr_t)vstr(js, b, &len_b);
  int cmp = memcmp(sa, sb, len_a < len_b ? len_a : len_b);
  if (cmp) return cmp;
  return (len_a > len_b) - (len_a < len_b);
}

static inline bool array_sort_writes_dense(ant_value_t arr, ant_offset_t len) {
  ant_object_t *ptr = real_array_obj_ptr(arr);
  return
    !is_proxy(arr) && array_dense_storage(ptr) &&
    ptr->u.array.len == len && ptr->u.array.cap >= len && ptr->flags.extensible && !ptr->flags.sealed;
}

static ant_value_t builtin_array_sort(ant_params_t) {
  ant_value_t arr = js->this_val;
  ant_value_t compareFn = js_mkundef();
  
  ant_value_t result = arr;
  ant_value_t *vals = NULL, *keys = NULL, *temp_vals = NULL, *temp_keys = NULL;
  
  uint64_t *int_keys = NULL, *temp_int_keys = NULL;
  uint64_t inline_int_keys[SORT_INLINE_KEYS * 2];
  ant_offset_t count = 0, undef_count = 0, len = 0;
  
  gc_temp_root_scope_t temp_scope = {0};
  bool temp_scope_active = false;
  
  arr = array_method_receiver(js, arr, "sort");
  if (is_err(arr)) return arr;
  result = arr;
  ant_offset_t string_len;
  if (array_string_wrapper_length(js, arr, &string_len) && string_len > 1) return array_string_wrapper_write_error(js, "0");
  
  if (nargs >= 1) {
    uint8_t t = vtype(args[0]);
    if (t == kTypeFunction || t == kTypeBuiltin) compareFn = args[0];
    else if (t != kTypeUndefined) return js_mkerr_typed(js, JS_ERR_TYPE, "compareFn must be a function or undefined");
  }

  gc_temp_root_scope_begin(js, &temp_scope);
  temp_scope_active = true;
  
  if (!gc_temp_root_handle_valid(gc_temp_root_add(&temp_scope, arr))) goto oom;
  if (!gc_temp_root_handle_valid(gc_temp_root_add(&temp_scope, compareFn))) goto oom;
  
  ant_value_t len_result = array_like_length_checked(js, arr, &len);
  if (is_err(len_result)) { result = len_result; goto done; }
  if (len == 0) goto done;
  
  ant_object_t *target = is_proxy(arr) ? NULL : js_obj_ptr(js_as_obj(arr));
  if (target && target->flags.frozen && len > 1) { 
    result = js_mkerr_typed(js, JS_ERR_TYPE, "Cannot assign to read only property of frozen object"); 
    goto done; 
  }
  
  vals = malloc(len * sizeof(ant_value_t));
  if (!vals) goto oom;
  
  ant_object_t *dense = array_dense_source(js, arr);
  if (dense && len <= dense->u.array.len && len <= dense->u.array.cap) {
    for (ant_offset_t i = 0; i < len; i++) {
      ant_value_t v = dense->u.array.data[i];
      if (is_empty_slot(v)) continue;
      if (vtype(v) == kTypeUndefined) undef_count++;
      else vals[count++] = v;
    }
  } else for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t has = array_method_has_index(js, arr, i);
    if (is_err(has)) { result = has; goto done; }
    if (has != js_true) continue;
    
    ant_value_t v = array_method_get_index(js, arr, i);
    
    if (is_err(v)) { result = v; goto done; }
    if (vtype(v) == kTypeUndefined) undef_count++;
    else vals[count++] = v;
    if (!gc_temp_root_push(&temp_scope, v)) goto oom;
  }
  
  if (count <= 1) goto writeback;
  for (ant_offset_t i = 0; i < count; i++)
    if (!gc_temp_root_handle_valid(gc_temp_root_add(&temp_scope, vals[i]))) goto oom;
  
  bool use_cmp = vtype(compareFn) != kTypeUndefined;
  bool use_ints = !use_cmp;
  
  for (ant_offset_t i = 0; use_ints && i < count; i++) use_ints = 
    vtype(vals[i]) == kTypeNumber && 
    tod(vals[i]) == (double)(int32_t)tod(vals[i]);
  
  bool use_keys = !use_cmp && !use_ints;
  sv_callback_t cb = sv_callback_prepare(js, compareFn, js_mkundef());

  if (use_ints) {
    int_keys = count <= SORT_INLINE_KEYS ? inline_int_keys : malloc(count * sizeof(*int_keys));
    temp_int_keys = count <= SORT_INLINE_KEYS ? inline_int_keys + SORT_INLINE_KEYS : malloc(count * sizeof(*temp_int_keys));
    if (!int_keys || !temp_int_keys) goto oom;
    for (ant_offset_t i = 0; i < count; i++) int_keys[i] = sort_int_string_key((int32_t)tod(vals[i]));
  }

  if (use_keys) {
    keys = malloc(count * sizeof(ant_value_t));
    if (!keys) goto oom;
    
    for (ant_offset_t i = 0; i < count; i++) {
      ant_value_t key = js_tostring_val(js, vals[i]);
      if (is_err(key)) {
        result = key;
        goto done;
      }
      
      keys[i] = key;
      if (!gc_temp_root_push(&temp_scope, key)) goto oom;
    }
  }
  
  temp_vals = malloc(count * sizeof(ant_value_t));
  if (use_keys) temp_keys = malloc(count * sizeof(ant_value_t));
  if (!temp_vals || (use_keys && !temp_keys)) goto oom;
  
  for (ant_offset_t width = 1; width < count; width *= 2) {
    for (ant_offset_t left = 0; left < count; left += width * 2) {
      ant_offset_t mid = left + width;
      ant_offset_t right = (mid + width < count) ? mid + width : count;
      if (mid >= count) break;
      
      ant_offset_t i = left, j = mid, k = 0;
      while (i < mid && j < right) {
        int cmp;
        if (use_ints) cmp = (int_keys[i] > int_keys[j]) - (int_keys[i] < int_keys[j]);
        else if (use_keys) cmp = sort_string_cmp(js, keys[i], keys[j]);
        else {
          ant_value_t call_args[2] = { vals[i], vals[j] };
          ant_value_t order = sv_callback_call(js->vm, js, &cb, call_args, 2);
          if (vtype(order) != kTypeNumber && !is_err(order)) order = js_to_numeric(js, order);
          if (!is_err(order) && vtype(order) == kTypeBigInt)
            order = js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert a BigInt value to a number");
          if (is_err(order)) { result = order; goto done; }
          double d = tod(order);
          cmp = (d > 0) - (d < 0);
        }
        if (cmp <= 0) {
          temp_vals[k] = vals[i];
          if (use_keys) temp_keys[k] = keys[i];
          if (use_ints) temp_int_keys[k] = int_keys[i];
          k++; i++;
        } else {
          temp_vals[k] = vals[j];
          if (use_keys) temp_keys[k] = keys[j];
          if (use_ints) temp_int_keys[k] = int_keys[j];
          k++; j++;
        }
      }
      while (i < mid) {
        temp_vals[k] = vals[i];
        if (use_keys) temp_keys[k] = keys[i];
        if (use_ints) temp_int_keys[k] = int_keys[i];
        k++; i++;
      }
      while (j < right) {
        temp_vals[k] = vals[j];
        if (use_keys) temp_keys[k] = keys[j];
        if (use_ints) temp_int_keys[k] = int_keys[j];
        k++; j++;
      }
      
      memcpy(&vals[left], temp_vals, k * sizeof(ant_value_t));
      if (use_keys) memcpy(&keys[left], temp_keys, k * sizeof(ant_value_t));
      if (use_ints) memcpy(&int_keys[left], temp_int_keys, k * sizeof(*int_keys));
    }
  }
  
writeback:;
  ant_offset_t filled = count + undef_count;
  if (array_sort_writes_dense(arr, len)) {
    ant_offset_t doff = get_dense_buf(arr);
    for (ant_offset_t i = 0; i < len; i++)
      dense_set(js, doff, i, i < count ? vals[i] : i < filled ? js_mkundef() : T_EMPTY);
    if (filled < len) array_mark_may_have_holes(arr);
  } else for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t step;
    if (i < filled) step = js_setprop_index(js, arr, (uint32_t)i, i < count ? vals[i] : js_mkundef());
    else {
      char key[24];
      step = js_delete_prop(js, arr, key, uint_to_str(key, sizeof(key), (uint64_t)i));
    }
    if (is_err(step)) { 
      result = step; 
      goto done; 
    }
  }
  result = arr;
  goto done;
  
oom:
  result = js_mkerr(js, "out of memory");

done:
  if (temp_scope_active) gc_temp_root_scope_end(&temp_scope);
  
  if (int_keys != inline_int_keys) {
    free(temp_int_keys);
    free(int_keys);
  }
  
  free(temp_keys);
  free(temp_vals);
  free(keys);
  free(vals);
  
  return result;
}

static ant_value_t builtin_array_splice(ant_params_t) {
  ant_value_t arr = js->this_val;
  arr = array_method_receiver(js, arr, "splice");
  if (is_err(arr)) return arr;
  ant_offset_t string_len;
  if (array_string_wrapper_length(js, arr, &string_len)) return array_string_wrapper_write_error(js, "length");

  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  ant_value_t read_from = is_proxy(arr) ? proxy_read_target(js, arr) : arr;

  int start = 0;
  if (nargs >= 1 && vtype(args[0]) == kTypeNumber) {
    start = (int) tod(args[0]);
    if (start < 0) start = (int)len + start;
    if (start < 0) start = 0;
    if (start > (int)len) start = (int)len;
  }

  int deleteCount = nargs == 0 ? 0 : (int)len - start;
  if (nargs >= 2 && vtype(args[1]) == kTypeNumber) {
    deleteCount = (int) tod(args[1]);
    if (deleteCount < 0) deleteCount = 0;
    if (deleteCount > (int)len - start) deleteCount = (int)len - start;
  }

  int insertCount = nargs > 2 ? nargs - 2 : 0;

  bool intrinsic = false;
  ant_value_t removed = array_species_create(js, arr, (ant_offset_t)deleteCount, &intrinsic);
  if (is_err(removed)) return removed;

  ant_offset_t doff = get_dense_buf(arr);
  if (doff && !is_proxy(arr) && !js_is_arguments(arr)) {
    ant_offset_t d_len = dense_iterable_length(js, arr);
    const uint16_t mutable_mask =
      ANT_OBJECT_FLAG_EXTENSIBLE | ANT_OBJECT_FLAG_FROZEN | ANT_OBJECT_FLAG_SEALED | ANT_OBJECT_FLAG_EXOTIC;
    if (d_len != len || (dense_obj(doff)->flags.raw & mutable_mask) != ANT_OBJECT_FLAG_EXTENSIBLE) goto splice_slow;
    bool skipped = false;
    for (int i = 0; i < deleteCount; i++) {
      if (!arr_has(js, arr, (ant_offset_t)(start + i))) { skipped = true; continue; }
      ant_value_t elem = arr_get(js, arr, (ant_offset_t)(start + i));
      ant_value_t set = array_result_set(js, removed, intrinsic, (ant_offset_t)i, elem);
      if (is_err(set)) return set;
    }
    if (intrinsic && !skipped) array_mark_packed(removed);

    int shift = insertCount - deleteCount;
    ant_offset_t new_len = (ant_offset_t)((int)d_len + shift);

    if (shift != 0) {
      if (new_len > dense_capacity(doff)) {
        doff = dense_grow(js, arr, new_len);
        if (doff == 0) return js_mkerr(js, "oom");
      }
      ant_offset_t move_start = (ant_offset_t)(start + deleteCount);
      ant_offset_t move_dest = (ant_offset_t)(start + insertCount);
      ant_offset_t move_count = d_len - move_start;
      ant_value_t *d = dense_data(doff);
      if (!d) return js_mkerr(js, "oom");
      if (move_count > 0) {
        memmove(&d[move_dest], &d[move_start], sizeof(ant_value_t) * (size_t)move_count);
        gc_elements_moved(dense_obj(doff));
      }
    }

    for (int i = 0; i < insertCount; i++)
      dense_set(js, doff, (ant_offset_t)(start + i), args[2 + i]);

    if (shift < 0) {
      for (ant_offset_t i = new_len; i < d_len; i++)
        dense_set(js, doff, i, T_EMPTY);
    }

    array_len_set(js, arr, new_len);
    return removed;
  }

  if (is_proxy(arr)) {
    for (int i = 0; i < deleteCount; i++) {
      ant_offset_t src_idx = (ant_offset_t)(start + i);
      if (!arr_has(js, read_from, src_idx)) continue;
      char src[16], dst[16];
      size_t srclen = uint_to_str(src, sizeof(src), (unsigned)src_idx);
      size_t dstlen = uint_to_str(dst, sizeof(dst), (unsigned)i);
      ant_value_t elem = proxy_get(js, arr, src, srclen);
      if (is_err(elem)) return elem;
      js_setprop(js, removed, js_mkstr(js, dst, dstlen), elem);
    }

    js_setprop(js, removed, js->length_str, tov((double)deleteCount));
    int shift = insertCount - deleteCount;

    if (shift > 0) {
      for (int i = (int)len - 1; i >= start + deleteCount; i--) {
        char src[16], dst[16];
        size_t srclen = uint_to_str(src, sizeof(src), (unsigned)i);
        size_t dstlen = uint_to_str(dst, sizeof(dst), (unsigned)(i + shift));
        if (arr_has(js, read_from, (ant_offset_t)i)) {
          ant_value_t elem = proxy_get(js, arr, src, srclen);
          if (is_err(elem)) return elem;
          js_setprop(js, arr, js_mkstr(js, dst, dstlen), elem);
        } else {
          ant_value_t del = proxy_delete(js, arr, dst, dstlen);
          if (is_err(del)) return del;
        }
      }
    } else if (shift < 0) {
      for (int i = start + deleteCount; i < (int)len; i++) {
        char src[16], dst[16];
        size_t srclen = uint_to_str(src, sizeof(src), (unsigned)i);
        size_t dstlen = uint_to_str(dst, sizeof(dst), (unsigned)(i + shift));
        if (arr_has(js, read_from, (ant_offset_t)i)) {
          ant_value_t elem = proxy_get(js, arr, src, srclen);
          if (is_err(elem)) return elem;
          js_setprop(js, arr, js_mkstr(js, dst, dstlen), elem);
        } else {
          ant_value_t del = proxy_delete(js, arr, dst, dstlen);
          if (is_err(del)) return del;
        }
      }
      for (int i = (int)len + shift; i < (int)len; i++) {
        ant_value_t del = proxy_delete_index(js, arr, (ant_offset_t)i);
        if (is_err(del)) return del;
      }
    }

    for (int i = 0; i < insertCount; i++) {
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)(start + i));
      js_setprop(js, arr, js_mkstr(js, idxstr, idxlen), args[2 + i]);
    }

    js_setprop(js, arr, js->length_str, tov((double)((int)len + shift)));
    return removed;
  }

  splice_slow:
  return array_splice_generic(
    js, arr, len, (ant_offset_t)start, (ant_offset_t)deleteCount,
    insertCount > 0 ? &args[2] : NULL, (ant_offset_t)insertCount, removed, intrinsic
  );
}

static ant_value_t builtin_array_copyWithin(ant_params_t) {
  ant_value_t arr = js->this_val;
  ant_value_t result = arr;
  gc_temp_root_scope_t temp_roots = {0};
  
  bool temp_roots_active = false;
  arr = array_method_receiver(js, arr, "copyWithin");
  if (is_err(arr)) return arr;

  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  ant_value_t read_from = is_proxy(arr) ? proxy_read_target(js, arr) : arr;

  int target = 0, start = 0, end = (int)len;
  if (nargs >= 1 && vtype(args[0]) == kTypeNumber) {
    target = (int) tod(args[0]);
    if (target < 0) target = (int)len + target;
    if (target < 0) target = 0;
  }
  if (nargs >= 2 && vtype(args[1]) == kTypeNumber) {
    start = (int) tod(args[1]);
    if (start < 0) start = (int)len + start;
    if (start < 0) start = 0;
  }
  if (nargs >= 3 && vtype(args[2]) == kTypeNumber) {
    end = (int) tod(args[2]);
    if (end < 0) end = (int)len + end;
    if (end < 0) end = 0;
  }
  
  if (end > (int)len) end = (int)len;
  int count = end - start;
  if (count > (int)len - target) count = (int)len - target;
  if (count <= 0) return arr;
  
  ant_offset_t string_len;
  if (array_string_wrapper_length(js, arr, &string_len)) return array_string_wrapper_write_error(js, "0");
  ant_object_t *frozen = is_proxy(arr) ? NULL : js_obj_ptr(js_as_obj(arr));
  if (frozen && frozen->flags.frozen)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot assign to read only property '%d' of object", target);

  ant_offset_t doff = get_dense_buf(arr);
  if (doff && !is_proxy(arr)) {
    if (start < target) {
      for (int i = count - 1; i >= 0; i--) {
        ant_value_t v = dense_get(doff, (ant_offset_t)(start + i));
        dense_set(js, doff, (ant_offset_t)(target + i), is_empty_slot(v) ? js_mkundef() : v);
      }
    } else {
      for (int i = 0; i < count; i++) {
        ant_value_t v = dense_get(doff, (ant_offset_t)(start + i));
        dense_set(js, doff, (ant_offset_t)(target + i), is_empty_slot(v) ? js_mkundef() : v);
      }
    }
    return arr;
  }

  if (is_proxy(arr)) {
    bool *temp_exists = calloc((size_t)count, sizeof(*temp_exists));
    gc_temp_root_scope_t proxy_roots;
    gc_temp_root_scope_begin(js, &proxy_roots);
    ant_value_t *values = gc_temp_root_slots(&proxy_roots, (size_t)count);
    ant_value_t proxy_result = arr;
    if (!temp_exists || !values) {
      proxy_result = js_mkerr(js, "out of memory");
      goto proxy_done;
    }

    for (int i = 0; i < count; i++) {
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)(start + i));
      temp_exists[i] = arr_has(js, read_from, (ant_offset_t)(start + i));
      if (!temp_exists[i]) continue;
      values[i] = proxy_get(js, arr, idxstr, idxlen);
      if (is_err(values[i])) { proxy_result = values[i]; goto proxy_done; }
    }

    for (int i = 0; i < count; i++) {
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)(target + i));
      ant_value_t step = temp_exists[i]
        ? array_set_index_throw(js, arr, (ant_offset_t)(target + i), values[i])
        : proxy_delete(js, arr, idxstr, idxlen);
      if (is_err(step)) { proxy_result = step; goto proxy_done; }
    }

  proxy_done:
    gc_temp_root_scope_end(&proxy_roots);
    free(temp_exists);
    return proxy_result;
  }

  ant_value_t *temp = (ant_value_t *)malloc(count * sizeof(ant_value_t));
  if (!temp) return js_mkerr(js, "out of memory");
  gc_temp_root_scope_begin(js, &temp_roots);
  temp_roots_active = true;
  
  if (!gc_temp_root_handle_valid(gc_temp_root_add(&temp_roots, arr))) goto oom;
  if (!gc_temp_root_handle_valid(gc_temp_root_add(&temp_roots, read_from))) goto oom;
  
  for (int i = 0; i < count; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)(start + i));
    ant_prop_loc_t elem_off = lkp(js, read_from, idxstr, idxlen);
    temp[i] = elem_off.obj ? js_prop_load(elem_off) : js_mkundef();
    if (!gc_temp_root_handle_valid(gc_temp_root_add(&temp_roots, temp[i]))) goto oom;
  }
  
  for (int i = 0; i < count; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)(target + i));
    ant_value_t key = js_mkstr(js, idxstr, idxlen);
    if (is_err(key)) {
      result = key;
      goto done;
    }
    if (!gc_temp_root_handle_valid(gc_temp_root_add(&temp_roots, key))) goto oom;
    ant_value_t set_result = js_setprop(js, arr, key, temp[i]);
    if (is_err(set_result)) {
      result = set_result;
      goto done;
    }
  }

done:
  if (temp_roots_active) gc_temp_root_scope_end(&temp_roots);
  free(temp);
  return result;

oom:
  result = js_mkerr(js, "out of memory");
  goto done;
}

static ant_value_t builtin_array_toSorted(ant_params_t) {
  ant_value_t arr = js->this_val;
  arr = array_method_receiver(js, arr, "toSorted");
  if (is_err(arr)) return arr;
  
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  ant_value_t result = array_copy_spliced(js, arr, len, len, 0, NULL, 0);
  if (is_err(result)) return result;
  
  ant_value_t sorted = sv_call_native_this(js, builtin_array_sort, result, args, nargs);
  if (is_err(sorted)) return sorted;
  return mkval(kTypeArray, vdata(result));
}

static ant_value_t builtin_array_toReversed(ant_params_t) {
  ant_value_t arr = js->this_val;
  arr = array_method_receiver(js, arr, "toReversed");
  if (is_err(arr)) return arr;
  
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  ant_value_t result = array_copy_spliced(js, arr, len, len, 0, NULL, 0);
  if (is_err(result)) return result;
  
  ant_value_t reversed = sv_call_native_this(js, builtin_array_reverse, result, NULL, 0);
  if (is_err(reversed)) return reversed;
  return mkval(kTypeArray, vdata(result));
}

static ant_value_t builtin_array_toSpliced(ant_params_t) {
  ant_value_t arr = js->this_val;
  arr = array_method_receiver(js, arr, "toSpliced");
  if (is_err(arr)) return arr;
  
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  double dlen = (double)len;
  
  double start_d = 0.0;
  if (nargs >= 1) {
    ant_value_t status = string_to_integer_or_infinity(js, args[0], &start_d);
    if (is_err(status)) return status;
  }
  
  ant_offset_t start = start_d < 0
    ? (start_d + dlen <= 0 ? 0 : (ant_offset_t)(start_d + dlen))
    : (start_d >= dlen ? len : (ant_offset_t)start_d);
  
  ant_offset_t skip = nargs == 0 ? 0 : len - start;
  if (nargs >= 2) {
    double skip_d = 0.0;
    ant_value_t status = string_to_integer_or_infinity(js, args[1], &skip_d);
    if (is_err(status)) return status;
    skip = skip_d <= 0 ? 0 : skip_d >= (double)(len - start) ? len - start : (ant_offset_t)skip_d;
  }
  
  ant_offset_t insert = nargs > 2 ? (ant_offset_t)(nargs - 2) : 0;
  ant_value_t result = array_copy_spliced(js, arr, len, start, skip, insert ? args + 2 : NULL, insert);
  if (is_err(result)) return result;
  
  return mkval(kTypeArray, vdata(result));
}

static ant_value_t builtin_array_with(ant_params_t) {
  ant_value_t arr = js->this_val;
  arr = array_method_receiver(js, arr, "with");
  if (is_err(arr)) return arr;
  
  if (nargs < 2) return js_mkerr(js, "with requires index and value arguments");
  PROXY_AWARE_LENGTH_OR_RETURN(arr, len);
  
  int idx = (int) tod(args[0]);
  if (idx < 0) idx = (int)len + idx;
  if (idx < 0 || (ant_offset_t)idx >= len) return js_mkerr(js, "Invalid index");
  
  ant_value_t result = array_copy_spliced(js, arr, len, (ant_offset_t)idx, 1, &args[1], 1);
  if (is_err(result)) return result;
  
  return mkval(kTypeArray, vdata(result));
}

static ant_value_t builtin_array_keys(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, js->this_val, "keys");
  return is_err(arr) ? arr : make_array_iterator(js, arr, ARR_ITER_KEYS);
}

static ant_value_t builtin_array_values(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, js->this_val, "values");
  return is_err(arr) ? arr : make_array_iterator(js, arr, ARR_ITER_VALUES);
}

static ant_value_t builtin_array_entries(ant_params_t) {
  ant_value_t arr = array_method_receiver(js, js->this_val, "entries");
  return is_err(arr) ? arr : make_array_iterator(js, arr, ARR_ITER_ENTRIES);
}

static ant_value_t builtin_array_toString(ant_params_t) {
  ant_value_t arr = js->this_val;
  ant_value_t join_result;
  
  if (js_try_invoke(js, arr, "join", NULL, 0, &join_result)) {
    if (is_err(join_result)) return join_result;
    return join_result;
  }
  
  return builtin_object_toString(js, args, nargs, js_mkundef());
}

static bool array_locale_uses_default_string_methods(ant_t *js) {
  ant_value_t string_proto = js->sym.string_proto;
  ant_value_t object_proto = js->sym.object_proto;
  
  ant_object_t *string_ptr = js_obj_ptr(js_as_obj(string_proto));
  ant_object_t *object_ptr = js_obj_ptr(js_as_obj(object_proto));

  if (!string_ptr || !object_ptr || string_ptr->flags.is_exotic || object_ptr->flags.is_exotic) return false;
  if (!same_object_identity(string_ptr->proto, object_proto)) return false;
  if (lkp(js, string_proto, "toLocaleString", 14).obj) return false;

  ant_prop_loc_t locale_off = lkp(js, object_proto, "toLocaleString", 14);
  ant_prop_loc_t tostring_off = lkp(js, string_proto, "toString", 8);
  if (!locale_off.obj || !tostring_off.obj) return false;

  const ant_shape_prop_t *locale_meta = prop_shape_meta(locale_off);
  const ant_shape_prop_t *tostring_meta = prop_shape_meta(tostring_off);
  
  if (
    (locale_meta && (locale_meta->has_getter || locale_meta->has_setter)) ||
    (tostring_meta && (tostring_meta->has_getter || tostring_meta->has_setter))
  ) return false;

  ant_value_t locale_method = js_prop_load(locale_off);
  ant_value_t tostring_method = js_prop_load(tostring_off);
  
  return vtype(locale_method) == kTypeBuiltin &&
    js_cfunc_same_entrypoint(locale_method, builtin_object_toLocaleString) &&
    vtype(tostring_method) == kTypeBuiltin &&
    js_cfunc_same_entrypoint(tostring_method, builtin_string_toString);
}

static inline uint8_t merge_ascii_state(uint8_t left, uint8_t right) {
  if (left == STR_ASCII_NO || right == STR_ASCII_NO) return STR_ASCII_NO;
  if (left == STR_ASCII_YES && right == STR_ASCII_YES) return STR_ASCII_YES;
  return STR_ASCII_UNKNOWN;
}

static bool array_try_builtin_string_locale(
  ant_t *js, ant_value_t arr, 
  ant_offset_t len, ant_value_t *out
) {
  ant_object_t *array = array_obj_ptr(arr);
  
  if (
    !array || !array->flags.fast_array || !array->u.array.data ||
    (ant_offset_t)array->u.array.len != len
  ) return false;

  size_t total = len > 1 ? (size_t)len - 1 : 0;
  uint8_t ascii_state = STR_ASCII_YES;
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t elem = array->u.array.data[i];
    if (
      vtype(elem) != kTypeString || str_is_heap_rope(elem) ||
      str_is_heap_builder(elem)
    ) return false;

    size_t elem_len = (size_t)vstrlen(js, elem);
    if (elem_len > SIZE_MAX - total) {
      *out = js_mkerr(js, "oom");
      return true;
    }
    total += elem_len;

    ant_flat_string_t *elem_flat = ant_str_flat_ptr(elem);
    uint8_t elem_ascii = elem_flat
      ? str_flat_ascii_state(elem_flat)
      : STR_ASCII_UNKNOWN;
    ascii_state = merge_ascii_state(ascii_state, elem_ascii);
  }

  if (!array_locale_uses_default_string_methods(js)) return false;
  ant_value_t result = js_mkstr(js, NULL, total);
  
  *out = result;
  if (is_err(result)) return true;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, result);
  
  ant_flat_string_t *flat = ant_str_flat_ptr(result);
  array = array_obj_ptr(arr);
  size_t pos = 0;
  
  for (ant_offset_t i = 0; i < len; i++) {
    if (i > 0) flat->bytes[pos++] = ',';
    ant_offset_t elem_len = 0;
    ant_offset_t elem_off = vstr(js, array->u.array.data[i], &elem_len);
    memcpy(
      flat->bytes + pos,
      (const char *)(uintptr_t)elem_off,
      (size_t)elem_len
    );
    pos += (size_t)elem_len;
  }
  
  flat->bytes[pos] = '\0';
  str_flat_finish(flat, ascii_state);
  GC_ROOT_RESTORE(js, root_mark);
  
  return true;
}

static ant_value_t array_to_locale_string_body(ant_params_t) {
  ant_value_t arr = js->this_val;
  arr = array_method_receiver(js, arr, "toLocaleString");
  if (is_err(arr)) return arr;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, arr);
  
  bool input_is_array = real_array_obj_ptr(arr) != NULL;
  ant_offset_t len = input_is_array ? get_array_length(js, arr) : 0;
  
  if (!input_is_array) {
    ant_value_t len_result = proxy_aware_length(js, arr, &len);
    if (is_err(len_result)) {
      GC_ROOT_RESTORE(js, root_mark);
      return len_result;
    }
  }
  if (len == 0) {
    ant_value_t empty = js_mkstr(js, "", 0);
    GC_ROOT_RESTORE(js, root_mark);
    return empty;
  }
  
  ant_value_t string_result;
  if (
    input_is_array &&
    array_try_builtin_string_locale(js, arr, len, &string_result)
  ) {
    GC_ROOT_RESTORE(js, root_mark);
    return string_result;
  }

  char static_buf[1024];
  string_builder_t sb;
  string_builder_init(&sb, static_buf, sizeof(static_buf));

  ant_object_t *numeric_fast_arr = input_is_array ? array_obj_ptr(arr) : NULL;
  bool all_fast_numbers = numeric_fast_arr && numeric_fast_arr->flags.fast_array &&
    !numeric_fast_arr->flags.may_have_holes && numeric_fast_arr->u.array.data &&
    (ant_offset_t)numeric_fast_arr->u.array.len == len;
  
  for (ant_offset_t i = 0; all_fast_numbers && i < len; i++)
    all_fast_numbers = vtype(numeric_fast_arr->u.array.data[i]) == kTypeNumber;

  ant_prop_loc_t number_locale_off = all_fast_numbers
    ? lkp(js, js->sym.number_proto, "toLocaleString", 14)
    : ANT_PROP_LOC_NONE;
  
  const ant_shape_prop_t *number_locale_meta = prop_shape_meta(number_locale_off);
  ant_value_t number_locale_method = number_locale_off.obj
    ? js_prop_load(number_locale_off)
    : js_mkundef();
  
  all_fast_numbers = all_fast_numbers && number_locale_off.obj &&
    (!number_locale_meta || !number_locale_meta->has_getter) &&
    vtype(number_locale_method) == kTypeBuiltin &&
    js_cfunc_same_entrypoint(number_locale_method, builtin_number_toLocaleString);

  if (all_fast_numbers) {
    for (ant_offset_t i = 0; i < len; i++) {
      if (i > 0) {
        if (sb.size < sb.capacity) sb.buffer[sb.size++] = ',';
        else if (!string_builder_append(&sb, ",", 1)) {
          string_builder_dispose(&sb);
          ant_value_t err = js_mkerr(js, "oom");
          GC_ROOT_RESTORE(js, root_mark);
          return err;
        }
      }
      char localized_buf[128];
      bool write_direct = sb.capacity - sb.size >= sizeof(localized_buf);
      char *localized_out = write_direct ? sb.buffer + sb.size : localized_buf;
      size_t localized_capacity = write_direct
        ? sb.capacity - sb.size
        : sizeof(localized_buf);
      size_t localized_len = format_number_to_locale_string(
        numeric_fast_arr->u.array.data[i], localized_out, localized_capacity
      );
      if (localized_len == 0) {
        string_builder_dispose(&sb);
        ant_value_t err = js_mkerr(js, "number formatting failed");
        GC_ROOT_RESTORE(js, root_mark);
        return err;
      }
      if (write_direct) sb.size += localized_len;
      else if (!string_builder_append(&sb, localized_buf, localized_len)) {
        string_builder_dispose(&sb);
        ant_value_t err = js_mkerr(js, "oom");
        GC_ROOT_RESTORE(js, root_mark);
        return err;
      }
    }
    ant_value_t ret = string_builder_finalize(js, &sb);
    GC_ROOT_RESTORE(js, root_mark);
    return ret;
  }
  
  for (ant_offset_t i = 0; i < len; i++) {
    if (i > 0 && !string_builder_append(&sb, ",", 1)) {
      string_builder_dispose(&sb);
      ant_value_t err = js_mkerr(js, "oom");
      GC_ROOT_RESTORE(js, root_mark);
      return err;
    }

    GC_ROOT_SAVE(elem_mark, js);
    ant_value_t elem = input_is_array ? arr_get(js, arr, i) : array_method_get_index(js, arr, i);
    GC_ROOT_PIN(js, elem);
    if (is_err(elem)) {
      string_builder_dispose(&sb);
      GC_ROOT_RESTORE(js, elem_mark);
      GC_ROOT_RESTORE(js, root_mark);
      return elem;
    }
    if (vtype(elem) == kTypeNull || vtype(elem) == kTypeUndefined) {
      GC_ROOT_RESTORE(js, elem_mark);
      continue;
    }

    ant_value_t method = js_getprop_fallback(js, elem, "toLocaleString");
    GC_ROOT_PIN(js, method);
    if (is_err(method)) {
      string_builder_dispose(&sb);
      GC_ROOT_RESTORE(js, elem_mark);
      GC_ROOT_RESTORE(js, root_mark);
      return method;
    }

    uint8_t method_type = vtype(method);
    if (!is_callable(method)) {
      string_builder_dispose(&sb);
      ant_value_t err = js_mkerr_typed(
        js, JS_ERR_TYPE, 
        "toLocaleString is not a function"
      );
      GC_ROOT_RESTORE(js, elem_mark);
      GC_ROOT_RESTORE(js, root_mark);
      return err;
    }

    if (vtype(elem) == kTypeNumber && method_type == kTypeBuiltin &&
        js_cfunc_same_entrypoint(method, builtin_number_toLocaleString)) {
      char localized_buf[128];
      size_t localized_len = format_number_to_locale_string(
        elem, localized_buf, 
        sizeof(localized_buf)
      );
      if (localized_len == 0 ||
          !string_builder_append(&sb, localized_buf, localized_len)) {
        string_builder_dispose(&sb);
        ant_value_t err = js_mkerr(js, "oom");
        GC_ROOT_RESTORE(js, elem_mark);
        GC_ROOT_RESTORE(js, root_mark);
        return err;
      }
      GC_ROOT_RESTORE(js, elem_mark);
      continue;
    }

    ant_value_t localized = sv_call_value(js, method, elem, args, nargs);

    GC_ROOT_PIN(js, localized);
    if (is_err(localized)) {
      string_builder_dispose(&sb);
      GC_ROOT_RESTORE(js, elem_mark);
      GC_ROOT_RESTORE(js, root_mark);
      return localized;
    }

    ant_value_t localized_str = coerce_to_str_hint(js, localized, 1);
    GC_ROOT_PIN(js, localized_str);
    if (is_err(localized_str)) {
      string_builder_dispose(&sb);
      GC_ROOT_RESTORE(js, elem_mark);
      GC_ROOT_RESTORE(js, root_mark);
      return localized_str;
    }

    ant_offset_t elem_len = 0;
    ant_offset_t elem_off = vstr(js, localized_str, &elem_len);
    if (!string_builder_append(
      &sb, (const char *)(uintptr_t)elem_off, (size_t)elem_len
    )) {
      string_builder_dispose(&sb);
      ant_value_t err = js_mkerr(js, "oom");
      GC_ROOT_RESTORE(js, elem_mark);
      GC_ROOT_RESTORE(js, root_mark);
      return err;
    }
    GC_ROOT_RESTORE(js, elem_mark);
  }
  
  ant_value_t ret = string_builder_finalize(js, &sb);
  GC_ROOT_RESTORE(js, root_mark);
  
  return ret;
}

static ant_value_t array_join_guarded(ant_native_params_t, ant_value_t new_target, ant_cfunc_t body) {
  ant_value_t receiver = js->this_val;
  for (ant_array_join_frame_t *frame = js->array_join_stack; frame; frame = frame->prev)
    if (frame->receiver == receiver && is_object_type(receiver)) return js_mkstr(js, "", 0);

  ant_array_join_frame_t frame = { js->array_join_stack, receiver };
  js->array_join_stack = &frame;
  
  ant_value_t result = body(js, args, nargs, new_target);
  js->array_join_stack = frame.prev;
  
  return result;
}

static ant_value_t builtin_array_join(ant_params_t) {
  return array_join_guarded(js, args, nargs, call_new_target, array_join_body);
}

static ant_value_t builtin_array_toLocaleString(ant_params_t) {
  return array_join_guarded(js, args, nargs, call_new_target, array_to_locale_string_body);
}

static ant_value_t builtin_Array_isArray(ant_params_t) {
  if (nargs == 0) return mkval(kTypeBool, 0);
  bool result = false;
  ant_value_t is_array_res = js_is_array_value_checked(js, args[0], &result);
  if (is_err(is_array_res)) return is_array_res;
  return mkval(kTypeBool, result ? 1 : 0);
}

static ant_value_t builtin_Array_isTemplateObject(ant_params_t) {
  if (nargs == 0) return js_false;
  return js_bool(vtype(args[0]) == kTypeArray && js_check_brand(args[0], BRAND_TEMPLATE_OBJECT));
}

typedef struct {
  ant_value_t write_target;
  ant_value_t result;
  ant_value_t mapFn;
  ant_value_t mapThis;
  ant_offset_t index;
} array_from_iter_ctx_t;

static iter_action_t array_from_iter_cb(ant_t *js, ant_value_t value, void *ctx, ant_value_t *out) {
  array_from_iter_ctx_t *fctx = (array_from_iter_ctx_t *)ctx;
  ant_value_t elem = value;

  if (is_callable(fctx->mapFn)) {
    ant_value_t call_args[2] = { elem, tov((double)fctx->index) };
    elem = sv_vm_call(js->vm, js, fctx->mapFn, fctx->mapThis, call_args, 2, NULL, js_mkundef());
    if (is_err(elem)) { *out = elem; return ITER_ERROR; }
  }

  if (vtype(fctx->write_target) == kTypeArray) arr_set(js, fctx->write_target, fctx->index, elem);
  else {
    char idxstr[16]; size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)fctx->index);
    js_setprop(js, fctx->write_target, js_mkstr(js, idxstr, idxlen), elem);
  }

  fctx->index++;
  return ITER_CONTINUE;
}

static ant_value_t builtin_Array_from(ant_params_t) {
  if (nargs == 0) return mkarr(js);

  ant_value_t src = args[0];
  ant_value_t mapFn = (nargs >= 2 && is_callable(args[1])) ? args[1] : js_mkundef();
  ant_value_t mapThis = (nargs >= 3) ? args[2] : js_mkundef();

  ant_value_t ctor = js->this_val;
  bool use_ctor = (vtype(ctor) == kTypeFunction || vtype(ctor) == kTypeBuiltin);
  ant_value_t result = use_ctor ? array_alloc_from_ctor_with_length(js, ctor, 0) : mkarr(js);
  if (is_err(result)) return result;

  bool result_is_proxy = is_proxy(result);
  ant_value_t write_target = result_is_proxy ? proxy_read_target(js, result) : result;
  ant_value_t iter_sym = js->sym.iterator_sym;

  if (vtype(src) == kTypeString) {
    if (str_is_heap_rope(src) || str_is_heap_builder(src)) {
      src = str_materialize(js, src);
      if (is_err(src)) return src;
    }
    ant_offset_t slen = str_len_fast(js, src);
    array_from_iter_ctx_t ctx = { write_target, result, mapFn, mapThis, 0 };
    for (ant_offset_t i = 0; i < slen; ) {
      ant_offset_t off = vstr(js, src, NULL);
      utf8proc_int32_t cp;
      ant_offset_t cb_len = (ant_offset_t)utf8_next(
        (const utf8proc_uint8_t *)(uintptr_t)(off + i),
        (utf8proc_ssize_t)(slen - i),
        &cp
      );
      ant_value_t ch = js_mkstr(js, (const void *)(uintptr_t)(off + i), cb_len);
      
      ant_value_t out;
      iter_action_t act = array_from_iter_cb(js, ch, &ctx, &out);
      
      if (act == ITER_ERROR) return out;
      i += cb_len;
    }
    if (vtype(result) != kTypeArray) js_setprop(js, result, js->length_str, tov((double)ctx.index));
  } else if (vtype(src) == kTypeArray) {
    bool default_iter = js_array_iteration_default(js, src);

    if (default_iter && !is_callable(mapFn) && real_array_obj_ptr(result) && get_array_length(js, result) == 0) {
      ant_value_t copied = js_arr_spread_into(js, result, src);
      if (is_err(copied)) return copied;
    } else if (default_iter) {
      array_from_iter_ctx_t ctx = { write_target, result, mapFn, mapThis, 0 };
      ant_offset_t len = 0;
      ant_value_t len_result = array_like_length_checked(js, src, &len);
      if (is_err(len_result)) return len_result;
      for (ant_offset_t i = 0; i < len; i++) {
        ant_value_t unused;
        iter_action_t act = array_from_iter_cb(js, arr_get(js, src, i), &ctx, &unused);
        if (act == ITER_ERROR) return unused;
        len_result = array_like_length_checked(js, src, &len);
        if (is_err(len_result)) return len_result;
      }
      if (vtype(result) != kTypeArray) js_setprop(js, result, js->length_str, tov((double)ctx.index));
    } else {
      array_from_iter_ctx_t ctx = { write_target, result, mapFn, mapThis, 0 };
      ant_value_t iter_result = iter_foreach(js, src, array_from_iter_cb, &ctx);
      if (is_err(iter_result)) return iter_result;
      if (vtype(result) != kTypeArray) js_setprop(js, result, js->length_str, tov((double)ctx.index));
    }
  } else {
    ant_value_t iter_method = js_mkundef();
    bool src_is_array = false;
    if (vtype(iter_sym) == kTypeSymbol) {
      iter_method = js_get_sym(js, src, iter_sym);
      if (is_err(iter_method)) return iter_method;
      if (is_proxy(src)) src_is_array = vtype(proxy_read_target(js, src)) == kTypeArray;
    }

    if (src_is_array && vtype(iter_method) == kTypeBuiltin) {
      array_from_iter_ctx_t ctx = { write_target, result, mapFn, mapThis, 0 };
      PROXY_AWARE_LENGTH_OR_RETURN(src, len);
      for (ant_offset_t i = 0; i < len; i++) {
        ant_value_t unused;
        ant_value_t item = array_method_get_index(js, src, i);
        if (is_err(item)) return item;
        iter_action_t act = array_from_iter_cb(js, item, &ctx, &unused);
        if (act == ITER_ERROR) return unused;
      }
      if (vtype(result) != kTypeArray) js_setprop(js, result, js->length_str, tov((double)len));
    } else if (vtype(iter_method) != kTypeUndefined && vtype(iter_method) != kTypeNull) {
      array_from_iter_ctx_t ctx = { write_target, result, mapFn, mapThis, 0 };
      ant_value_t iter_result = iter_foreach_method(js, src, iter_method, array_from_iter_cb, &ctx);
      if (is_err(iter_result)) return iter_result;
      if (vtype(result) != kTypeArray) js_setprop(js, result, js->length_str, tov((double)ctx.index));
    } else if (vtype(src) == kTypeObject) {
      array_from_iter_ctx_t ctx = { write_target, result, mapFn, mapThis, 0 };
      PROXY_AWARE_LENGTH_OR_RETURN(src, len);
      for (ant_offset_t i = 0; i < len; i++) {
        ant_value_t unused;
        ant_value_t item = array_method_get_index(js, src, i);
        if (is_err(item)) return item;
        iter_action_t act = array_from_iter_cb(js, item, &ctx, &unused);
        if (act == ITER_ERROR) return unused;
      }
      if (vtype(result) != kTypeArray) js_setprop(js, result, js->length_str, tov((double)len));
    }
  }

  if (!use_ctor) return mkval(kTypeArray, vdata(result));
  return result;
}

static ant_value_t builtin_Array_of(ant_params_t) {
  ant_value_t ctor = js->this_val;
  bool use_ctor = (vtype(ctor) == kTypeFunction || vtype(ctor) == kTypeBuiltin);
  ant_value_t arr = use_ctor ? array_alloc_from_ctor_with_length(js, ctor, (ant_offset_t)nargs) : mkarr(js);
  if (is_err(arr)) return arr;

  bool arr_is_proxy = is_proxy(arr);
  ant_value_t write_target = arr_is_proxy ? proxy_read_target(js, arr) : arr;

  for (int i = 0; i < nargs; i++) {
    if (vtype(write_target) == kTypeArray) arr_set(js, write_target, (ant_offset_t)i, args[i]);
    else {
      char idxstr[16]; size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
      js_setprop(js, write_target, js_mkstr(js, idxstr, idxlen), args[i]);
    }
  }

  if (vtype(arr) != kTypeArray) js_setprop(js, arr, js->length_str, tov((double) nargs));
  if (!use_ctor) return mkval(kTypeArray, vdata(arr));

  return arr;
}

typedef enum {
  FROM_ASYNC_PROMISE,
  FROM_ASYNC_RESULT,
  FROM_ASYNC_SOURCE,
  FROM_ASYNC_NEXT,
  FROM_ASYNC_MAPFN,
  FROM_ASYNC_THIS,
  FROM_ASYNC_INDEX,
  FROM_ASYNC_LENGTH,
  FROM_ASYNC_KIND,
  FROM_ASYNC_REASON,
  FROM_ASYNC_FIELD_COUNT,
} from_async_field_t;

typedef enum {
  FROM_ASYNC_ASYNC_ITERATOR,
  FROM_ASYNC_SYNC_ITERATOR,
  FROM_ASYNC_ARRAY_LIKE,
} from_async_kind_t;

static ant_value_t from_async_step(ant_t *js, ant_value_t state);

static inline ant_value_t from_async_field(ant_t *js, ant_value_t state, from_async_field_t field) {
  return arr_get(js, state, field);
}

static inline from_async_kind_t from_async_kind(ant_t *js, ant_value_t state) {
  return (from_async_kind_t)tod(from_async_field(js, state, FROM_ASYNC_KIND));
}

static inline double from_async_index(ant_t *js, ant_value_t state) {
  return tod(from_async_field(js, state, FROM_ASYNC_INDEX));
}

static ant_value_t from_async_reject(ant_t *js, ant_value_t state, ant_value_t reason) {
  js_reject_promise(js, from_async_field(js, state, FROM_ASYNC_PROMISE), reason);
  return js_mkundef();
}

static ant_value_t from_async_fail(ant_t *js, ant_value_t state, ant_value_t err) {
  return from_async_reject(js, state, Ant_Error_ConsumeMarker(js, err));
}

static ant_value_t from_async_on_rejected(ant_params_t) {
  return from_async_reject(js, get_slot(js->current_func, SLOT_DATA), nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t from_async_on_closed(ant_params_t) {
  ant_value_t state = get_slot(js->current_func, SLOT_DATA);
  return from_async_reject(js, state, from_async_field(js, state, FROM_ASYNC_REASON));
}

static ant_value_t from_async_await(
  ant_t *js, ant_value_t state, ant_value_t value,
  ant_value_t (*on_fulfilled)(ant_params_t), ant_value_t (*on_rejected)(ant_params_t)
) {
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, state);
  
  ant_value_t promise = js_promise_assimilate_awaitable(js, value);
  if (is_err(promise)) {
    GC_ROOT_RESTORE(js, mark);
    return from_async_fail(js, state, promise);
  }
  
  GC_ROOT_PIN(js, promise);
  ant_value_t fulfill = js_heavy_mkfun(js, on_fulfilled, state);
  GC_ROOT_PIN(js, fulfill);
  ant_value_t reject = js_heavy_mkfun(js, on_rejected, state);
  Ant_Promise_Observe(js, promise, fulfill, reject);
  
  GC_ROOT_RESTORE(js, mark);
  return js_mkundef();
}

static ant_value_t from_async_close_and_reject(ant_t *js, ant_value_t state, ant_value_t reason) {
  if (from_async_kind(js, state) == FROM_ASYNC_ARRAY_LIKE) return from_async_reject(js, state, reason);

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, state);
  GC_ROOT_PIN(js, reason);
  arr_set(js, state, FROM_ASYNC_REASON, reason);

  ant_value_t iterator = from_async_field(js, state, FROM_ASYNC_SOURCE);
  ant_value_t ret = js_getprop_fallback(js, iterator, "return");
  bool has_return = !is_err(ret) && is_callable(ret);
  
  ant_value_t closed = has_return ? sv_vm_call(js->vm, js, ret, iterator, NULL, 0, NULL, js_mkundef()) : ret;
  if (is_err(closed)) Ant_Error_ConsumeMarker(js, closed);

  bool await_close = has_return && !is_err(closed) && from_async_kind(js, state) == FROM_ASYNC_ASYNC_ITERATOR;
  ant_value_t result = await_close
    ? from_async_await(js, state, closed, from_async_on_closed, from_async_on_closed)
    : from_async_reject(js, state, reason);

  GC_ROOT_RESTORE(js, mark);
  return result;
}

static ant_value_t from_async_on_close_rejected(ant_params_t) {
  return from_async_close_and_reject(js, get_slot(js->current_func, SLOT_DATA), nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t from_async_mapped(ant_t *js, ant_value_t state, ant_value_t value) {
  ant_value_t target = from_async_field(js, state, FROM_ASYNC_RESULT);
  double index = from_async_index(js, state);

  define_desc_raw_t raw = {
    .has = { true, true, true, true, false, false },
    .val = { js_true, js_true, value, js_true, js_mkundef(), js_mkundef() },
  };
  
  ant_value_t defined = object_define_property_reporting(js, target, tov(index), js_mkundef(), &raw, NULL);
  if (is_err(defined)) return from_async_close_and_reject(js, state, Ant_Error_ConsumeMarker(js, defined));

  arr_set(js, state, FROM_ASYNC_INDEX, tov(index + 1));
  return from_async_step(js, state);
}

static ant_value_t from_async_on_mapped(ant_params_t) {
  return from_async_mapped(js, get_slot(js->current_func, SLOT_DATA), nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t from_async_value(ant_t *js, ant_value_t state, ant_value_t value) {
  ant_value_t mapfn = from_async_field(js, state, FROM_ASYNC_MAPFN);
  if (vtype(mapfn) == kTypeUndefined) return from_async_mapped(js, state, value);

  ant_value_t call_args[2] = { value, from_async_field(js, state, FROM_ASYNC_INDEX) };
  ant_value_t this_arg = from_async_field(js, state, FROM_ASYNC_THIS);
  ant_value_t mapped = sv_vm_call(js->vm, js, mapfn, this_arg, call_args, 2, NULL, js_mkundef());
  
  if (is_err(mapped)) return from_async_close_and_reject(js, state, Ant_Error_ConsumeMarker(js, mapped));
  return from_async_await(js, state, mapped, from_async_on_mapped, from_async_on_close_rejected);
}

static ant_value_t from_async_on_value(ant_params_t) {
  return from_async_value(js, get_slot(js->current_func, SLOT_DATA), nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t from_async_finish(ant_t *js, ant_value_t state) {
  ant_value_t target = from_async_field(js, state, FROM_ASYNC_RESULT);
  ant_value_t set = js_setprop(js, target, js->length_str, from_async_field(js, state, FROM_ASYNC_INDEX));
  
  if (is_err(set)) return from_async_fail(js, state, set);
  js_resolve_promise(js, from_async_field(js, state, FROM_ASYNC_PROMISE), target);
  
  return js_mkundef();
}

static ant_value_t from_async_next_result(ant_t *js, ant_value_t state, ant_value_t result) {
  if (!is_object_type(result)) return from_async_fail(
    js, state, js_mkerr_typed(js, JS_ERR_TYPE, "Iterator result %s is not an object", js_str(js, result))
  );

  ant_value_t done = js_getprop_fallback(js, result, "done");
  if (is_err(done)) return from_async_fail(js, state, done);
  if (js_truthy(js, done)) return from_async_finish(js, state);

  ant_value_t value = js_getprop_fallback(js, result, "value");
  if (is_err(value)) return from_async_fail(js, state, value);
  
  if (from_async_kind(js, state) == FROM_ASYNC_ASYNC_ITERATOR) return from_async_value(js, state, value);
  return from_async_await(js, state, value, from_async_on_value, from_async_on_close_rejected);
}

static ant_value_t from_async_on_next_result(ant_params_t) {
  return from_async_next_result(js, get_slot(js->current_func, SLOT_DATA), nargs > 0 ? args[0] : js_mkundef());
}

static ant_value_t from_async_step(ant_t *js, ant_value_t state) {
  ant_value_t source = from_async_field(js, state, FROM_ASYNC_SOURCE);
  from_async_kind_t kind = from_async_kind(js, state);

  if (kind == FROM_ASYNC_ARRAY_LIKE) {
    double index = from_async_index(js, state);
    if (index >= tod(from_async_field(js, state, FROM_ASYNC_LENGTH))) return from_async_finish(js, state);
    
    char key[32];
    size_t key_len = uint_to_str(key, sizeof(key), (unsigned)index);
    ant_value_t value = js_getprop_fallback_len(js, source, key, key_len);
    
    if (is_err(value)) return from_async_fail(js, state, value);
    return from_async_await(js, state, value, from_async_on_value, from_async_on_rejected);
  }

  ant_value_t next = from_async_field(js, state, FROM_ASYNC_NEXT);
  ant_value_t result = sv_vm_call(js->vm, js, next, source, NULL, 0, NULL, js_mkundef());
  
  if (is_err(result)) return from_async_fail(js, state, result);
  if (kind == FROM_ASYNC_SYNC_ITERATOR) return from_async_next_result(js, state, result);
  
  return from_async_await(js, state, result, from_async_on_next_result, from_async_on_rejected);
}

static ant_value_t from_async_construct(ant_t *js, ant_value_t ctor, bool with_length, ant_offset_t length) {
  if (!js_is_constructor(ctor) || same_ctor_identity(js, ctor, js->sym.array_ctor))
    return array_alloc_intrinsic(js, js_mkundef(), with_length ? length : 0);

  ant_value_t seed = Ant_Object_CreateBare(js);
  if (is_err(seed)) return seed;
  
  ant_value_t proto = js_get(js, ctor, "prototype");
  if (is_err(proto)) return proto;
  if (is_object_type(proto)) js_set_proto_init(seed, proto);

  ant_value_t ctor_args[1] = { tov((double)length) };
  ant_value_t constructed = sv_vm_call(js->vm, js, ctor, seed, ctor_args, with_length ? 1 : 0, NULL, ctor);
  
  if (is_err(constructed)) return constructed;
  return is_object_type(constructed) ? constructed : seed;
}

static ant_value_t from_async_open_iterator(ant_t *js, ant_value_t items, ant_value_t method, ant_value_t *next) {
  if (!is_callable(method)) return js_mkerr_typed(js, JS_ERR_TYPE, "%s is not a function", js_str(js, method));
  
  ant_value_t iterator = sv_vm_call(js->vm, js, method, items, NULL, 0, NULL, js_mkundef());
  if (is_err(iterator)) return iterator;
  if (!is_object_type(iterator)) return js_mkerr_typed(js, JS_ERR_TYPE, "Result of the Symbol.iterator method is not an object");
  
  *next = js_getprop_fallback(js, iterator, "next");
  return is_err(*next) ? *next : iterator;
}

static ant_value_t from_async_start(
  ant_t *js, ant_value_t ctor, ant_value_t items,
  ant_value_t mapfn, ant_value_t this_arg, ant_value_t promise
) {
  if (vtype(mapfn) != kTypeUndefined && !is_callable(mapfn))
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s is not a function", js_str(js, mapfn));
  if (is_nullish(items)) return js_mkerr_typed(
    js, JS_ERR_TYPE, "Cannot read properties of %s (reading 'Symbol(Symbol.asyncIterator)')",
    vtype(items) == kTypeNull ? "null" : "undefined"
  );

  ant_value_t lookup = is_object_type(items) ? items : builtin_Object(js, &items, 1, js_mkundef());
  if (is_err(lookup)) return lookup;

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, lookup);

  from_async_kind_t kind = FROM_ASYNC_ASYNC_ITERATOR;
  ant_value_t method = js_get_sym(js, lookup, js->sym.asyncIterator_sym);
  if (!is_err(method) && is_nullish(method)) {
    kind = FROM_ASYNC_SYNC_ITERATOR;
    method = js_get_sym(js, lookup, js->sym.iterator_sym);
  }
  if (!is_err(method) && is_nullish(method)) kind = FROM_ASYNC_ARRAY_LIKE;

  ant_value_t source = lookup;
  ant_value_t next = js_mkundef();
  ant_value_t target = method;
  ant_offset_t length = 0;
  
  GC_ROOT_PIN(js, source);
  GC_ROOT_PIN(js, next);
  GC_ROOT_PIN(js, target);

  if (!is_err(target) && kind == FROM_ASYNC_ARRAY_LIKE) {
    target = array_like_length_checked(js, lookup, &length);
    if (!is_err(target)) target = from_async_construct(js, ctor, true, length);
  } else if (!is_err(target)) {
    source = from_async_open_iterator(js, items, method, &next);
    target = is_err(source) ? source : from_async_construct(js, ctor, false, 0);
  }

  ant_value_t state = is_err(target) ? target : mkarr(js);
  GC_ROOT_PIN(js, state);
  
  if (!is_err(state)) {
    ant_value_t fields[FROM_ASYNC_FIELD_COUNT] = {
      [FROM_ASYNC_PROMISE] = promise, [FROM_ASYNC_RESULT] = target,
      [FROM_ASYNC_SOURCE] = source, [FROM_ASYNC_NEXT] = next,
      [FROM_ASYNC_MAPFN] = mapfn, [FROM_ASYNC_THIS] = this_arg,
      [FROM_ASYNC_INDEX] = tov(0), [FROM_ASYNC_LENGTH] = tov((double)length),
      [FROM_ASYNC_KIND] = tov((double)kind), [FROM_ASYNC_REASON] = js_mkundef(),
    };
    
    for (int f = 0; f < FROM_ASYNC_FIELD_COUNT; f++) arr_set(js, state, f, fields[f]);
    state = from_async_step(js, state);
  }

  GC_ROOT_RESTORE(js, mark);
  return state;
}

static ant_value_t builtin_Array_fromAsync(ant_params_t) {
  ant_value_t promise = js_mkpromise(js);
  if (is_err(promise)) return promise;

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, promise);
  
  ant_value_t started = from_async_start(
    js, js->this_val, nargs > 0 ? args[0] : js_mkundef(),
    nargs > 1 ? args[1] : js_mkundef(), nargs > 2 ? args[2] : js_mkundef(), promise
  );
  
  if (is_err(started)) js_reject_promise(js, promise, Ant_Error_ConsumeMarker(js, started));
  GC_ROOT_RESTORE(js, mark);

  return promise;
}

static ant_value_t js_mkstr_utf16_range(
  ant_t *js, const char *str, size_t byte_len,
  size_t utf16_start, size_t utf16_end
);
static inline bool utf16_range_equals_bytes(
  const char *str, size_t byte_len,
  size_t utf16_start, size_t utf16_end,
  const char *expected, size_t expected_len
);

static inline double string_number_to_integer(double number) {
  return isnan(number) || number == 0.0 ? 0.0 : trunc(number);
}

static ant_value_t string_to_integer_or_infinity(
  ant_t *js, ant_value_t value, double *out
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, value);

  if (is_object_type(value) || vtype(value) == kTypeBuiltin) {
    value = js_to_primitive(js, value, 2);
    if (is_err(value)) {
      GC_ROOT_RESTORE(js, root_mark);
      return value;
    }
  }

  if (vtype(value) == kTypeSymbol || vtype(value) == kTypeBigInt) {
    ant_value_t err = js_mkerr_typed(
      js, JS_ERR_TYPE, "Cannot convert a %s value to a number", 
      vtype(value) == kTypeSymbol ? "Symbol" : "BigInt");
    GC_ROOT_RESTORE(js, root_mark);
    return err;
  }

  *out = string_number_to_integer(js_to_number(js, value));
  GC_ROOT_RESTORE(js, root_mark);
  
  return js_mkundef();
}

static ant_value_t string_index_of_strings(
  ant_t *js, ant_value_t str, ant_value_t search, double position
) {
  size_t str_utf16 = (size_t)str_utf16_len(js, str);
  size_t search_utf16 = (size_t)str_utf16_len(js, search);
  
  size_t start = position <= 0.0
    ? 0 : position >= (double)str_utf16
    ? str_utf16 : (size_t)position;

  if (search_utf16 == 0) return tov((double)start);
  if (search_utf16 > str_utf16 - start) return tov(-1);

  ant_offset_t str_len, search_len;
  (void)vstr(js, str, &str_len);
  
  ant_offset_t search_off = vstr(js, search, &search_len);
  ant_offset_t str_off = vstr(js, str, &str_len);
  const char *str_ptr = (const char *)(uintptr_t)str_off;
  const char *search_ptr = (const char *)(uintptr_t)search_off;

  if (str_is_ascii(str_ptr) && str_is_ascii(search_ptr)) {
    const char *p = str_ptr + start;
    const char *last = str_ptr + str_len - search_len;
    while (p <= last) {
      p = memchr(p, search_ptr[0], (size_t)(last - p) + 1);
      if (!p) break;
      if (search_len == 1 ||
          memcmp(p + 1, search_ptr + 1, (size_t)search_len - 1) == 0)
        return tov((double)(p - str_ptr));
      p++;
    }
  } else {
    size_t last = str_utf16 - search_utf16;
    uint32_t first = utf16_code_unit_at(search_ptr, (size_t)search_len, 0);
    for (size_t i = start; i <= last; i++) if (
      utf16_code_unit_at(str_ptr, (size_t)str_len, i) == first && 
      utf16_range_equals_bytes(
        str_ptr, (size_t)str_len, i, i + 
        search_utf16,search_ptr, (size_t)search_len)
    ) return tov((double)i);
  }

  return tov(-1);
}

static ant_value_t string_index_of_impl(
  ant_t *js, ant_value_t this_val, ant_value_t *args, int nargs
) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t receiver = this_val;
  ant_value_t search_arg = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t position_arg = nargs > 1 ? args[1] : js_mkundef();
  
  GC_ROOT_PIN(js, receiver);
  GC_ROOT_PIN(js, search_arg);
  GC_ROOT_PIN(js, position_arg);

  ant_value_t result;
  uint8_t receiver_type = vtype(receiver);
  if (receiver_type == kTypeNull || receiver_type == kTypeUndefined) {
    result = js_mkerr_typed(
      js, JS_ERR_TYPE, "String.prototype.indexOf called on null or undefined");
    goto done;
  }

  ant_value_t str = coerce_to_str_hint(js, receiver, 1);
  GC_ROOT_PIN(js, str);
  if (is_err(str)) { result = str; goto done; }

  ant_value_t search = coerce_to_str_hint(js, search_arg, 1);
  GC_ROOT_PIN(js, search);
  if (is_err(search)) { result = search; goto done; }

  double position = 0.0;
  if (nargs > 1) {
    ant_value_t status = string_to_integer_or_infinity(
      js, position_arg, &position);
    if (is_err(status)) { result = status; goto done; }
  }

  result = string_index_of_strings(js, str, search, position);

done:
  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static ant_value_t js_mkstr_utf16_range(
  ant_t *js, const char *str, size_t byte_len,
  size_t utf16_start, size_t utf16_end
) {
  size_t byte_start, byte_end;
  utf16_range_splits_t splits = utf16_range_to_byte_range(
    str, byte_len, utf16_start, utf16_end, &byte_start, &byte_end
  );

  if (!splits.prefix_surrogate && !splits.suffix_surrogate)
    return js_mkstr_byte_range(js, str, byte_start, byte_end - byte_start);

  string_builder_t sb;
  char static_buf[64]; char encoded[4];
  string_builder_init(&sb, static_buf, sizeof(static_buf));

  if (splits.prefix_surrogate) {
    size_t len = utf16_code_unit_to_wtf8(splits.prefix_surrogate, encoded);
    if (!string_builder_append(&sb, encoded, len)) goto oom;
  }

  if (!string_builder_append(
    &sb, str + byte_start, byte_end - byte_start
  )) goto oom;

  if (splits.suffix_surrogate) {
    size_t len = utf16_code_unit_to_wtf8(splits.suffix_surrogate, encoded);
    if (!string_builder_append(&sb, encoded, len)) goto oom;
  }

  return string_builder_finalize(js, &sb);

oom:
  string_builder_dispose(&sb);
  return js_mkerr(js, "oom");
}

static inline bool utf16_range_equals_bytes(
  const char *str, size_t byte_len,
  size_t utf16_start, size_t utf16_end,
  const char *expected, size_t expected_len
) {
  size_t byte_start, byte_end;
  utf16_range_splits_t splits = utf16_range_to_byte_range(
    str, byte_len, utf16_start, utf16_end, &byte_start, &byte_end
  );

  size_t body_len = byte_end - byte_start;
  if (!splits.prefix_surrogate && !splits.suffix_surrogate)
    return body_len == expected_len && memcmp(str + byte_start, expected, expected_len) == 0;

  size_t prefix_len = splits.prefix_surrogate ? 3 : 0;
  size_t suffix_len = splits.suffix_surrogate ? 3 : 0;
  if (prefix_len + body_len + suffix_len != expected_len) return false;

  char encoded[4];
  size_t pos = 0;

  if (splits.prefix_surrogate) {
    utf16_code_unit_to_wtf8(splits.prefix_surrogate, encoded);
    if (memcmp(expected, encoded, 3) != 0) return false;
    pos += 3;
  }

  if (body_len > 0 && memcmp(expected + pos, str + byte_start, body_len) != 0) return false;
  pos += body_len;

  if (splits.suffix_surrogate) {
    utf16_code_unit_to_wtf8(splits.suffix_surrogate, encoded);
    if (memcmp(expected + pos, encoded, 3) != 0) return false;
  }

  return true;
}

static ant_value_t js_mkstr_utf16_range_cached(
  ant_t *js, const char *str, size_t byte_len,
  size_t utf16_start, size_t utf16_end
) {
  if (utf16_end == utf16_start + 1) {
    size_t byte_start, byte_end;
    utf16_range_splits_t splits = utf16_range_to_byte_range(
      str, byte_len, utf16_start, utf16_end, &byte_start, &byte_end);
    if (!splits.prefix_surrogate && !splits.suffix_surrogate &&
        byte_end == byte_start + 1 && (unsigned char)str[byte_start] < 128)
      return js_mk_ascii_char_cached(js, (unsigned char)str[byte_start]);
  }

  return js_mkstr_utf16_range(
    js, str, byte_len, utf16_start, utf16_end);
}

static size_t string_substring_index(double value, size_t length) {
  if (value <= 0.0) return 0;
  if (value >= (double)length) return length;
  return (size_t)value;
}

static ant_value_t string_substring_strings(
  ant_t *js, ant_value_t str,
  double start_number, double end_number, bool default_end
) {
  size_t utf16_len = (size_t)str_utf16_len(js, str);
  if (default_end) end_number = (double)utf16_len;

  size_t start = string_substring_index(start_number, utf16_len);
  size_t end = string_substring_index(end_number, utf16_len);
  
  if (start > end) {
    size_t tmp = start;
    start = end;
    end = tmp;
  }

  if (start == 0 && end == utf16_len) return str;

  ant_offset_t byte_len, str_off = vstr(js, str, &byte_len);
  const char *str_ptr = (const char *)(uintptr_t)str_off;
  return js_mkstr_utf16_range_cached(
    js, str_ptr, (size_t)byte_len, start, end);
}

static ant_value_t string_substring_impl(
  ant_t *js, ant_value_t this_val, ant_value_t *args, int nargs
) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t receiver = this_val;
  ant_value_t start_arg = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t end_arg = nargs > 1 ? args[1] : js_mkundef();
  
  GC_ROOT_PIN(js, receiver);
  GC_ROOT_PIN(js, start_arg);
  GC_ROOT_PIN(js, end_arg);

  ant_value_t result;
  uint8_t receiver_type = vtype(receiver);
  if (receiver_type == kTypeNull || receiver_type == kTypeUndefined) {
    result = js_mkerr_typed(
      js, JS_ERR_TYPE, "String.prototype.substring called on null or undefined");
    goto done;
  }

  ant_value_t str = coerce_to_str_hint(js, receiver, 1);
  GC_ROOT_PIN(js, str);
  if (is_err(str)) { result = str; goto done; }

  double start_number = 0.0;
  ant_value_t status = string_to_integer_or_infinity(
    js, start_arg, &start_number);
  if (is_err(status)) { result = status; goto done; }

  bool default_end = nargs <= 1 || vtype(end_arg) == kTypeUndefined;
  double end_number = 0.0;
  if (!default_end) {
    status = string_to_integer_or_infinity(js, end_arg, &end_number);
    if (is_err(status)) { result = status; goto done; }
  }

  result = string_substring_strings(
    js, str, start_number, end_number, default_end);

done:
  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static bool string_fast_integer_arg(
  ant_value_t *args, int nargs, int index, double *out
) {
  if (index >= nargs || vtype(args[index]) == kTypeUndefined) {
    *out = 0.0;
    return true;
  }
  if (vtype(args[index]) != kTypeNumber) return false;
  *out = string_number_to_integer(tod(args[index]));
  return true;
}

ant_value_t js_string_intrinsic_call(
  ant_t *js, ant_string_intrinsic_kind_t kind,
  ant_value_t this_val, ant_value_t *args, int nargs
) {
  switch (kind) {
  case ANT_STRING_INTRINSIC_INDEX_OF: {
    double position;
    if (nargs > 0 && ant_str_flat_ptr(this_val) &&
        ant_str_flat_ptr(args[0]) &&
        string_fast_integer_arg(args, nargs, 1, &position))
      return string_index_of_strings(js, this_val, args[0], position);
    return string_index_of_impl(js, this_val, args, nargs);
  }
  case ANT_STRING_INTRINSIC_SUBSTRING: {
    double start_number, end_number = 0.0;
    bool default_end = nargs <= 1 || vtype(args[1]) == kTypeUndefined;
    if (ant_str_flat_ptr(this_val) &&
        string_fast_integer_arg(args, nargs, 0, &start_number) &&
        (default_end ||
         string_fast_integer_arg(args, nargs, 1, &end_number))) {
      GC_ROOT_SAVE(root_mark, js);
      GC_ROOT_PIN(js, this_val);
      ant_value_t result = string_substring_strings(
        js, this_val, start_number, end_number, default_end);
      GC_ROOT_RESTORE(js, root_mark);
      return result;
    }
    return string_substring_impl(js, this_val, args, nargs);
  }}
  return js_mkerr(js, "invalid string intrinsic");
}

static ant_value_t builtin_string_indexOf(ant_params_t) {
  return js_string_intrinsic_call(js, ANT_STRING_INTRINSIC_INDEX_OF, js->this_val, args, nargs);
}

static ant_value_t builtin_string_substring(ant_params_t) {
  return js_string_intrinsic_call(js, ANT_STRING_INTRINSIC_SUBSTRING, js->this_val, args, nargs);
}

bool js_string_intrinsic_builtin_matches(ant_value_t func, ant_string_intrinsic_kind_t kind) {
  if (vtype(func) != kTypeBuiltin) return false;
  switch (kind) {
    case ANT_STRING_INTRINSIC_INDEX_OF: return js_cfunc_same_entrypoint(func, builtin_string_indexOf);
    case ANT_STRING_INTRINSIC_SUBSTRING: return js_cfunc_same_entrypoint(func, builtin_string_substring);
  }
  return false;
}

static ant_value_t builtin_string_substr(ant_params_t) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != kTypeString) return js_mkerr(js, "substr called on non-string");
  ant_offset_t byte_len, str_off = vstr(js, str, &byte_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  size_t utf16_len = (size_t)str_utf16_len(js, str);
  
  if (nargs < 1) return str;
  
  double d_start = js_to_number(js, args[0]);
  if (isnan(d_start)) d_start = 0;
  else d_start = trunc(d_start);

  ant_offset_t start;
  if (d_start <= -(double)utf16_len) {
    start = 0;
  } else if (d_start < 0) {
    start = (ant_offset_t)((double)utf16_len + d_start);
  } else if (d_start >= (double)utf16_len) {
    start = (ant_offset_t)utf16_len;
  } else {
    start = (ant_offset_t)d_start;
  }
  
  ant_offset_t len = (ant_offset_t)utf16_len - start;
  if (nargs >= 2 && vtype(args[1]) != kTypeUndefined) {
    double d = js_to_number(js, args[1]);
    if (isnan(d) || d <= 0) len = 0;
    else if (d < (double)len) len = (ant_offset_t)d;
  }
  
  return js_mkstr_utf16_range(js, str_ptr, byte_len, start, start + len);
}

static ant_value_t string_split_code_units(ant_t *js, ant_value_t str, ant_value_t arr, uint32_t limit) {
  GC_ROOT_SAVE(roots, js);
  GC_ROOT_PIN(js, str);
  GC_ROOT_PIN(js, arr);
  
  ant_value_t part = js_mkundef();
  GC_ROOT_PIN(js, part);

  ant_offset_t len;
  const char *bytes = (const char *)(uintptr_t)vstr(js, str, &len);
  utf16_iterator_t iterator = utf16_iterator(bytes, len);
  bool ascii = str_is_ascii(bytes);
  
  uint16_t unit;
  for (uint32_t index = 0; index < limit && utf16_next_code_unit(&iterator, &unit); index++) {
    part = ascii 
      ? js_mk_ascii_char_cached(js, (unsigned char)bytes[index])
      : js_string_from_utf16_code_unit(js, unit);
    if (is_err(part)) break;
    arr_set(js, arr, index, part);
  }

  ant_value_t result = is_err(part) ? part : arr;
  GC_ROOT_RESTORE(js, roots);
  
  return result;
}

static ant_value_t string_split_impl(ant_t *js, ant_value_t str, ant_value_t *args, int nargs) {
  if (vtype(str) != kTypeString) return js_mkerr(js, "split called on non-string");
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  ant_value_t arr = mkarr(js);

  if (is_err(arr)) return arr;

  uint32_t limit = UINT32_MAX;
  if (nargs >= 2 && vtype(args[1]) == kTypeNumber) {
    double d = tod(args[1]);
    if (d >= 0 && d <= UINT32_MAX) {
      limit = (uint32_t)d;
    }
  }
  if (limit == 0) {
    return mkval(kTypeArray, vdata(arr));
  }
  if (nargs == 0) goto return_whole;

  ant_value_t sep_arg = args[0];
  if (vtype(sep_arg) == kTypeObject) {
    ant_prop_loc_t source_off = lkp(js, sep_arg, "source", 6);
    if (!source_off.obj) {
      sep_arg = js_to_primitive(js, sep_arg, 1);
      if (is_err(sep_arg)) return sep_arg;
      sep_arg = js_tostring_val(js, sep_arg);
      if (is_err(sep_arg)) return sep_arg;
      args[0] = sep_arg;
      goto split_string_separator;
    }
    ant_value_t source_val = js_prop_load(source_off);
    if (vtype(source_val) != kTypeString) goto return_whole;

    ant_offset_t plen, poff = vstr(js, source_val, &plen);
    const char *pattern_ptr = (char *)(uintptr_t)(poff);

    if (plen == 0 || (plen == 4 && memcmp(pattern_ptr, "(?:)", 4) == 0)) {
      ant_offset_t idx = 0;
      for (ant_offset_t i = 0; i < str_len && idx < limit; i++) {
        ant_value_t part = js_mkstr_byte_range(js, str_ptr, i, 1);
        arr_set(js, arr, idx, part);
        idx++;
      }
      return mkval(kTypeArray, vdata(arr));
    }

    char pcre2_pattern_stack[512];
    js_cstr_t pcre2_pattern = js_to_pcre2_pattern_cstr(
      pattern_ptr, plen, pcre2_pattern_stack,
      sizeof(pcre2_pattern_stack), false
    );
    
    if (!pcre2_pattern.ptr) goto return_whole;
    uint32_t options = PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_UNSET_BACKREF;
    int errcode;
    
    PCRE2_SIZE erroffset;
    pcre2_code *re = pcre2_compile(
      (PCRE2_SPTR)pcre2_pattern.ptr, pcre2_pattern.len,
      options, &errcode, &erroffset, NULL
    );

    if (pcre2_pattern.needs_free) free((void *)pcre2_pattern.ptr);
    if (re == NULL) goto return_whole;

    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(re, NULL);
    uint32_t capture_count;
    pcre2_pattern_info(re, PCRE2_INFO_CAPTURECOUNT, &capture_count);

    if (str_len == 0) {
      int rc = pcre2_match(re, (PCRE2_SPTR)str_ptr, 0, 0, 0, match_data, NULL);
      if (rc >= 0) {
        pcre2_match_data_free(match_data);
        pcre2_code_free(re);
        return mkval(kTypeArray, vdata(arr));
      }
    }

    ant_offset_t idx = 0;
    PCRE2_SIZE search_pos = 0;
    PCRE2_SIZE segment_start = 0;
    PCRE2_SIZE last_match_end = (PCRE2_SIZE)-1;
    bool had_any_split = false;
    
    while (idx < limit && search_pos <= str_len) {
      int rc = pcre2_match(re, (PCRE2_SPTR)str_ptr, str_len, search_pos, 0, match_data, NULL);
      if (rc < 0) break;

      PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
      PCRE2_SIZE match_start = ovector[0];
      PCRE2_SIZE match_end = ovector[1];

      if (match_start == match_end && match_start == last_match_end) {
        search_pos = match_end + 1;
        continue;
      }

      if (match_start == match_end && capture_count > 0) {
        bool is_pure_empty_capture = true;
        for (uint32_t i = 1; i <= capture_count; i++) {
          PCRE2_SIZE cap_start = ovector[2*i];
          PCRE2_SIZE cap_end = ovector[2*i+1];
          if (cap_start == PCRE2_UNSET || cap_end != cap_start) {
            is_pure_empty_capture = false;
            break;
          }
        }
        if (is_pure_empty_capture) {
          search_pos = match_end + 1;
          continue;
        }
      }
      
      had_any_split = true;

      ant_value_t part = js_mkstr_byte_range(js, str_ptr, segment_start, match_start - segment_start);
      arr_set(js, arr, idx, part);
      idx++;

      for (uint32_t i = 1; i <= capture_count && idx < limit; i++) {
        PCRE2_SIZE cap_start = ovector[2*i];
        PCRE2_SIZE cap_end = ovector[2*i+1];
        if (cap_start == PCRE2_UNSET) {
          arr_set(js, arr, idx, js_mkundef());
        } else {
          part = js_mkstr_byte_range(js, str_ptr, cap_start, cap_end - cap_start);
          arr_set(js, arr, idx, part);
        }
        idx++;
      }

      last_match_end = match_end;
      segment_start = match_end;
      if (match_start == match_end) {
        search_pos = match_end + 1;
      } else {
        search_pos = match_end;
      }
    }

    if (!had_any_split) {
      pcre2_match_data_free(match_data);
      pcre2_code_free(re);
      arr_set(js, arr, 0, js_mkstr_byte_range(js, str_ptr, 0, str_len));
      return mkval(kTypeArray, vdata(arr));
    }

    if (idx < limit) {
      ant_value_t part = js_mkstr_byte_range(js, str_ptr, segment_start, str_len - segment_start);
      arr_set(js, arr, idx, part);
      idx++;
    }

    pcre2_match_data_free(match_data);
    pcre2_code_free(re);
    return mkval(kTypeArray, vdata(arr));
  }

  if (vtype(sep_arg) != kTypeString) goto return_whole;

split_string_separator:;
  ant_offset_t sep_len, sep_off = vstr(js, sep_arg, &sep_len);
  const char *sep_ptr = (char *)(uintptr_t)(sep_off);
  ant_offset_t idx = 0, start = 0;

  if (sep_len == 0)
    return string_split_code_units(js, str, arr, limit);

  for (ant_offset_t i = 0; i + sep_len <= str_len && idx < limit; i++) {
    if (memcmp(str_ptr + i, sep_ptr, sep_len) != 0) continue;
    ant_value_t part = js_mkstr_byte_range(js, str_ptr, start, i - start);
    arr_set(js, arr, idx, part);
    idx++;
    start = i + sep_len;
    i += sep_len - 1;
  }
  if (idx < limit && start <= str_len) {
    ant_value_t part = js_mkstr_byte_range(js, str_ptr, start, str_len - start);
    arr_set(js, arr, idx, part);
    idx++;
  }

  return mkval(kTypeArray, vdata(arr));

return_whole:
  if (limit > 0) {
    arr_set(js, arr, 0, str);
  }
  return mkval(kTypeArray, vdata(arr));
}

static ant_value_t builtin_string_split(ant_params_t) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != kTypeString) return js_mkerr(js, "split called on non-string");

  if (nargs > 0 && is_object_type(args[0])) {
    bool called = false;
    ant_value_t call_args[2];
    int call_nargs = 1;
    call_args[0] = str;
    if (nargs >= 2) {
      call_args[1] = args[1];
      call_nargs = 2;
    }
    ant_value_t dispatched = maybe_call_symbol_method(
      js, args[0], js->sym.split_sym, args[0], call_args, call_nargs, &called
    );
    if (is_err(dispatched)) return dispatched;
    if (called) return dispatched;
  }

  return string_split_impl(js, str, args, nargs);
}

static ant_value_t builtin_string_slice(ant_params_t) {
  ant_value_t receiver = js->this_val;
  if (str_is_heap_rope(receiver)) receiver = rope_cached_flat(receiver);
  ant_flat_string_t *flat = ant_str_flat_ptr(receiver);
  
  bool numeric_start = nargs == 0 || vtype(args[0]) == kTypeUndefined ||
    (vtype(args[0]) == kTypeNumber && tod(args[0]) >= 0);
    
  bool numeric_end = nargs < 2 || vtype(args[1]) == kTypeUndefined ||
    (vtype(args[1]) == kTypeNumber && tod(args[1]) >= 0);
    
  if (flat && str_flat_ascii_state(flat) == STR_ASCII_YES && numeric_start && numeric_end) {
    size_t len = (size_t)flat->len;
    double start_arg = nargs && vtype(args[0]) == kTypeNumber ? tod(args[0]) : 0;
    double end_arg = nargs >= 2 && vtype(args[1]) == kTypeNumber ? tod(args[1]) : (double)len;
    
    size_t start = start_arg >= (double)len ? len : (size_t)start_arg;
    size_t end = end_arg >= (double)len ? len : (size_t)end_arg;
    
    if (start == 0 && end == len) return receiver;
    size_t result_len = end > start ? end - start : 0;
    GC_ROOT_SAVE(mark, js);
    GC_ROOT_PIN(js, receiver);
    
    ant_value_t result = js_mkstr(js, NULL, result_len);
    if (!is_err(result)) {
      ant_flat_string_t *out = ant_str_flat_ptr(result);
      memcpy(out->bytes, flat->bytes + start, result_len);
      str_flat_init_meta(out, STR_ASCII_YES);
    } GC_ROOT_RESTORE(js, mark);
    
    return result;
  }

  ant_value_t this_unwrapped = unwrap_primitive(js, js->this_val);
  ant_value_t str = js_tostring_val(js, this_unwrapped);
  if (is_err(str)) return str;
  
  ant_offset_t byte_len, str_off = vstr(js, str, &byte_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  size_t utf16_len = (size_t)str_utf16_len(js, str);
  ant_offset_t start = 0, end = (ant_offset_t)utf16_len;
  double dstr_len = D(utf16_len);
  
  if (nargs >= 1 && vtype(args[0]) == kTypeNumber) {
    double d = tod(args[0]);
    if (d < 0) {
      start = (ant_offset_t) (d + dstr_len < 0 ? 0 : d + dstr_len);
    } else start = (ant_offset_t) (d > dstr_len ? dstr_len : d);
  }
  
  if (nargs >= 2 && vtype(args[1]) == kTypeNumber) {
    double d = tod(args[1]);
    if (d < 0) {
      end = (ant_offset_t) (d + dstr_len < 0 ? 0 : d + dstr_len);
    } else end = (ant_offset_t) (d > dstr_len ? dstr_len : d);
  }
  
  if (start > end) start = end;
  return js_mkstr_utf16_range(js, str_ptr, byte_len, start, end);
}

static ant_value_t builtin_string_includes(ant_params_t) {
  if (vtype(js->this_val) == kTypeNull || vtype(js->this_val) == kTypeUndefined)
    return js_mkerr_typed(js, JS_ERR_TYPE, "String.prototype.includes called on null or undefined");

  ant_value_t str = to_string_val(js, js->this_val);
  if (is_err(str)) return str;
  
  if (vtype(str) != kTypeString) 
    return js_mkerr(js, "includes called on non-string");

  if (nargs > 0 && is_object_type(args[0])) {
    ant_value_t maybe_err = reject_regexp_arg(js, args[0], "includes");
    if (is_err(maybe_err)) return maybe_err;
  }

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, str);
  
  ant_value_t index = js_string_intrinsic_call(js, ANT_STRING_INTRINSIC_INDEX_OF, str, args, nargs);
  GC_ROOT_RESTORE(js, root_mark);
  
  if (is_err(index)) return index;
  return js_bool(vtype(index) == kTypeNumber && js_getnum(index) >= 0);
}

static inline ant_offset_t string_clamped_position(ant_t *js, ant_value_t value, ant_offset_t len) {
  double pos = js_to_number(js, value);
  if (isnan(pos) || pos < 0) return 0;
  if (pos > D(len)) return len;
  return (ant_offset_t)pos;
}

static ant_value_t builtin_string_startsWith(ant_params_t) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != kTypeString) return js_mkerr(js, "startsWith called on non-string");
  if (nargs == 0) return mkval(kTypeBool, 0);
  ant_value_t search = args[0];
  
  if (is_object_type(search)) {
    ant_value_t maybe_err = reject_regexp_arg(js, search, "startsWith");
    if (is_err(maybe_err)) return maybe_err;
  }
  
  search = js_tostring_val(js, search);
  if (is_err(search)) return search;
  
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  ant_offset_t search_len, search_off = vstr(js, search, &search_len);
  
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  const char *search_ptr = (char *)(uintptr_t)(search_off);

  if (str_is_ascii(str_ptr)) {
    ant_offset_t start = nargs >= 2 ? string_clamped_position(js, args[1], str_len) : 0;
    if (search_len > str_len - start) return mkval(kTypeBool, 0);
    if (search_len == 0) return mkval(kTypeBool, 1);
    return mkval(kTypeBool, memcmp(str_ptr + start, search_ptr, search_len) == 0 ? 1 : 0);
  }

  ant_offset_t str_units = str_utf16_len(js, str);
  ant_offset_t search_units = str_utf16_len(js, search);
  ant_offset_t start = nargs >= 2 ? string_clamped_position(js, args[1], str_units) : 0;
  
  if (search_units > str_units - start) return mkval(kTypeBool, 0);
  if (search_len == 0) return mkval(kTypeBool, 1);

  return mkval(kTypeBool, utf16_range_equals_bytes(
    str_ptr, str_len, start, start + search_units,
    search_ptr, search_len
  ) ? 1 : 0);
}

static ant_value_t builtin_string_endsWith(ant_params_t) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != kTypeString) return js_mkerr(js, "endsWith called on non-string");
  if (nargs == 0) return mkval(kTypeBool, 0);
  ant_value_t search = args[0];
  
  if (is_object_type(search)) {
    ant_value_t maybe_err = reject_regexp_arg(js, search, "endsWith");
    if (is_err(maybe_err)) return maybe_err;
  }
  
  search = js_tostring_val(js, search);
  if (is_err(search)) return search;
  
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  ant_offset_t search_len, search_off = vstr(js, search, &search_len);
  
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  const char *search_ptr = (char *)(uintptr_t)(search_off);

  if (str_is_ascii(str_ptr)) {
    ant_offset_t end = (nargs >= 2 && vtype(args[1]) != kTypeUndefined)
      ? string_clamped_position(js, args[1], str_len)
      : str_len;
    ant_offset_t start = end - search_len;
    if (start < 0) return mkval(kTypeBool, 0);
    if (search_len == 0) return mkval(kTypeBool, 1);
    return mkval(kTypeBool, memcmp(str_ptr + start, search_ptr, search_len) == 0 ? 1 : 0);
  }

  ant_offset_t str_units = str_utf16_len(js, str);
  ant_offset_t search_units = str_utf16_len(js, search);
  
  ant_offset_t end = (nargs >= 2 && vtype(args[1]) != kTypeUndefined)
    ? string_clamped_position(js, args[1], str_units)
    : str_units;
  
  ant_offset_t start = end - search_units;
  if (start < 0) return mkval(kTypeBool, 0);
  if (search_len == 0) return mkval(kTypeBool, 1);

  return mkval(kTypeBool, utf16_range_equals_bytes(
    str_ptr, str_len, start, start + search_units,
    search_ptr, search_len
  ) ? 1 : 0);
}

static ant_value_t builtin_string_template(ant_params_t) {
  ant_value_t str = to_string_val(js, js->this_val);
  
  if (is_err(str)) return str;
  if (vtype(str) != kTypeString) return js_mkerr(js, "template called on non-string");
  if (nargs < 1 || vtype(args[0]) != kTypeObject) return str;
  
  ant_value_t data = args[0];
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  
  string_builder_t sb;
  char static_buf[512];
  string_builder_init(&sb, static_buf, sizeof(static_buf));

  ant_offset_t i = 0;
  ant_offset_t literal_start = 0;
  
  while (i < str_len) {
    if (i >= str_len - 3) goto next_char;
    if (str_ptr[i] != '{' || str_ptr[i + 1] != '{') goto next_char;
    
    ant_offset_t key_start = i + 2;
    ant_offset_t key_end = key_start;
    
    while (
      key_end < str_len - 1 && 
      !(str_ptr[key_end] == '}' && 
      str_ptr[key_end + 1] == '}')
    ) key_end++;
    
    if (key_end >= str_len - 1) goto next_char;
    if (!string_builder_append(&sb, str_ptr + literal_start, (size_t)(i - literal_start))) goto oom;
    
    ant_prop_loc_t prop_off = lkp(js, data, str_ptr + key_start, key_end - key_start);
    if (prop_off.obj) {
      ant_value_t err = js_mkundef();
      ant_value_t value = js_prop_load(prop_off);
      if (!string_template_append_value(js, &sb, value, &err)) {
        string_builder_dispose(&sb);
        return is_err(err) ? err : js_mkerr(js, "oom");
      }
    }
    
    i = key_end + 2;
    literal_start = i;
    continue;

  next_char:
    i++;
  }

  if (!string_builder_append(&sb, str_ptr + literal_start, (size_t)(str_len - literal_start))) goto oom;
  return string_builder_finalize(js, &sb);

oom:
  string_builder_dispose(&sb);
  return js_mkerr(js, "oom");
}

static size_t html_attr_escaped_len(const char *s, ant_offset_t len) {
  size_t out = 0;
  for (ant_offset_t i = 0; i < len; i++) out += (s[i] == '"') ? 6 : 1;
  return out;
}

static void html_attr_append_escaped(char *out, size_t *pos, const char *s, ant_offset_t len) {
  for (ant_offset_t i = 0; i < len; i++) if (s[i] == '"') {
    memcpy(out + *pos, "&quot;", 6);
    *pos += 6;
  } else out[(*pos)++] = s[i];
}

static ant_value_t builtin_string_html(ant_native_params_t, const char *tag, const char *attr) {
  ant_value_t str = js_tostring_val(js, unwrap_primitive(js, js->this_val));
  if (is_err(str)) return str;

  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (const char *)(uintptr_t)str_off;
  
  size_t tag_len = strlen(tag);
  size_t attr_len = attr ? strlen(attr) : 0;
  
  ant_value_t attr_val = js_mkundef();
  ant_offset_t value_len = 0;
  
  const char *value_ptr = NULL;
  size_t escaped_len = 0;

  if (attr) {
    attr_val = nargs > 0 ? js_tostring_val(js, args[0]) : js_mkstr(js, "undefined", 9);
    if (is_err(attr_val)) return attr_val;
    ant_offset_t value_off = vstr(js, attr_val, &value_len);
    value_ptr = (const char *)(uintptr_t)value_off;
    escaped_len = html_attr_escaped_len(value_ptr, value_len);
  }

  size_t total = 1 + tag_len + (attr 
    ? (1 + attr_len + 2 + escaped_len + 1) 
    : 0) + 1 + (size_t)str_len + 2 + tag_len + 1;
  
  char *buf = (char *)calloc(1, total);
  if (!buf) return js_mkerr(js, "oom");

  size_t pos = 0;
  buf[pos++] = '<';
  memcpy(buf + pos, tag, tag_len); pos += tag_len;
  
  if (attr) {
    buf[pos++] = ' ';
    memcpy(buf + pos, attr, attr_len); pos += attr_len;
    buf[pos++] = '=';
    buf[pos++] = '"';
    html_attr_append_escaped(buf, &pos, value_ptr, value_len);
    buf[pos++] = '"';
  }
  
  buf[pos++] = '>';
  memcpy(buf + pos, str_ptr, (size_t)str_len); pos += (size_t)str_len;
  buf[pos++] = '<';
  buf[pos++] = '/';
  memcpy(buf + pos, tag, tag_len); pos += tag_len;
  buf[pos++] = '>';

  ant_value_t out = js_mkstr(js, buf, pos);
  free(buf);
  
  return out;
}

static ant_value_t builtin_string_anchor(ant_params_t) {
  return builtin_string_html(js, args, nargs, "a", "name");
}

static ant_value_t builtin_string_big(ant_params_t) {
  return builtin_string_html(js, args, nargs, "big", NULL);
}

static ant_value_t builtin_string_bold(ant_params_t) {
  return builtin_string_html(js, args, nargs, "b", NULL);
}

static ant_value_t builtin_string_fixed(ant_params_t) {
  return builtin_string_html(js, args, nargs, "tt", NULL);
}

static ant_value_t builtin_string_fontcolor(ant_params_t) {
  return builtin_string_html(js, args, nargs, "font", "color");
}

static ant_value_t builtin_string_fontsize(ant_params_t) {
  return builtin_string_html(js, args, nargs, "font", "size");
}

static ant_value_t builtin_string_italics(ant_params_t) {
  return builtin_string_html(js, args, nargs, "i", NULL);
}

static ant_value_t builtin_string_link(ant_params_t) {
  return builtin_string_html(js, args, nargs, "a", "href");
}

static ant_value_t builtin_string_small(ant_params_t) {
  return builtin_string_html(js, args, nargs, "small", NULL);
}

static ant_value_t builtin_string_strike(ant_params_t) {
  return builtin_string_html(js, args, nargs, "strike", NULL);
}

static ant_value_t builtin_string_sub(ant_params_t) {
  return builtin_string_html(js, args, nargs, "sub", NULL);
}

static ant_value_t builtin_string_sup(ant_params_t) {
  return builtin_string_html(js, args, nargs, "sup", NULL);
}

ant_value_t builtin_string_charCodeAt(ant_params_t) {
  ant_value_t str = to_string_val(js, js->this_val);
  
  if (is_err(str)) return str;
  if (vtype(str) != kTypeString) return js_mkerr(js, "charCodeAt called on non-string");
  
  double idx_d = nargs < 1 ? 0.0 : js_to_number(js, args[0]);
  if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
  if (isnan(idx_d)) idx_d = 0.0;
  if (idx_d >= (double)LONG_MAX || idx_d <= (double)LONG_MIN) return tov(JS_NAN);
  
  long idx_l = (long) idx_d;
  if (idx_l < 0) return tov(JS_NAN);
  
  ant_offset_t byte_len; ant_offset_t str_off = vstr(js, str, &byte_len);
  const char *str_data = (const char *)(uintptr_t)(str_off);
  
  uint32_t code_unit = utf16_code_unit_at(str_data, byte_len, idx_l);
  if (code_unit == 0xFFFFFFFF) return tov(JS_NAN);
  
  return tov((double) code_unit);
}

bool js_try_char_code_at(
  ant_t *js, ant_value_t func, ant_value_t receiver,
  ant_value_t *args, int argc, ant_value_t *result
) {
  if (vtype(func) == kTypeFunction) {
    sv_closure_t *closure = js_func_closure(func);
    if (closure->func || (closure->call_flags & SV_CALL_HAS_BOUND_ARGS)) return false;
    func = get_slot(closure->func_obj, SLOT_CFUNC);
    
    if (closure->call_flags & SV_CALL_HAS_BOUND_THIS) receiver = closure->bound_this;
    if (vtype(func) == kTypeBuiltin && js_as_cfunc(func) == builtin_function_call) {
      if (argc == 0) return false;
      func = receiver; receiver = *args++; argc--;
    }
  }
  
  if (vtype(func) != kTypeBuiltin || js_as_cfunc(func) != builtin_string_charCodeAt) return false;
  if (vtype(receiver) != kTypeString) return false;
  if (argc > 0 && vtype(args[0]) != kTypeNumber) return false;
  if (str_is_heap_rope(receiver)) receiver = rope_cached_flat(receiver);
  if (vtype(receiver) != kTypeString) return false;
  if (str_is_heap_rope(receiver) || str_is_heap_builder(receiver)) return false;

  ant_offset_t length;
  const char *bytes = (const char *)(uintptr_t)vstr(js, receiver, &length);
  
  if (!str_is_ascii(bytes)) return false;
  double index = argc ? tod(args[0]) : 0;
  
  index = isnan(index) ? 0 : trunc(index);
  *result = index < 0 || index >= (double)length
    ? tov(JS_NAN) : tov((double)(unsigned char)bytes[(size_t)index]);
  
  return true;
}

static ant_value_t builtin_string_codePointAt(ant_params_t) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != kTypeString) return js_mkerr(js, "codePointAt called on non-string");
  
  double idx_d = nargs < 1 ? 0.0 : js_to_number(js, args[0]);
  if (isnan(idx_d)) idx_d = 0.0;
  if (isinf(idx_d) || idx_d > (double)LONG_MAX) return js_mkundef();
  
  long idx_l = (long) idx_d;
  if (idx_l < 0) return js_mkundef();
  
  ant_offset_t byte_len;
  ant_offset_t str_off = vstr(js, str, &byte_len);
  const char *str_data = (const char *)(uintptr_t)(str_off);
  
  uint32_t cp = utf16_codepoint_at(str_data, byte_len, idx_l);
  if (cp == 0xFFFFFFFF) return js_mkundef();
  
  return tov((double) cp);
}

static ant_value_t builtin_string_toLowerCase(ant_params_t) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != kTypeString) return js_mkerr(js, "toLowerCase called on non-string");
  
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  if (str_len == 0) return js_mkstr(js, "", 0);

  const utf8proc_uint8_t *src = (const utf8proc_uint8_t *)str_ptr;
  utf8proc_ssize_t src_len = (utf8proc_ssize_t)str_len;

  ant_offset_t out_len = 0;
  utf8proc_ssize_t pos = 0;
  
  while (pos < src_len) {
    utf8proc_int32_t cp;
    utf8proc_ssize_t n = utf8_next(src + pos, src_len - pos, &cp);
    if (cp < 0) { out_len++; pos++; continue; }
    utf8proc_uint8_t tmp[4];
    out_len += (ant_offset_t)utf8proc_encode_char(utf8proc_tolower(cp), tmp);
    pos += n;
  }

  ant_value_t result = js_mkstr(js, NULL, out_len);
  if (is_err(result)) return result;
  
  ant_offset_t result_len, result_off = vstr(js, result, &result_len);
  char *result_ptr = (char *)(uintptr_t)(result_off);
  uint8_t ascii_state = STR_ASCII_YES;

  pos = 0;
  ant_offset_t wpos = 0;
  
  while (pos < src_len) {
    utf8proc_int32_t cp;
    utf8proc_ssize_t n = utf8_next(src + pos, src_len - pos, &cp);
    
    if (cp < 0) {
      unsigned char byte = src[pos];
      if (byte >= 0x80) ascii_state = STR_ASCII_NO;
      result_ptr[wpos++] = (char)byte;
      pos++; continue;
    }
    
    utf8proc_int32_t mapped = utf8proc_tolower(cp);
    if (mapped >= 0x80) ascii_state = STR_ASCII_NO;
    
    wpos += (ant_offset_t)utf8proc_encode_char(mapped, (utf8proc_uint8_t *)(result_ptr + wpos));
    pos += n;
  }
  
  str_set_ascii_state(result_ptr, ascii_state);
  return result;
}

static ant_value_t builtin_string_toUpperCase(ant_params_t) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != kTypeString) return js_mkerr(js, "toUpperCase called on non-string");
  
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  if (str_len == 0) return js_mkstr(js, "", 0);

  const utf8proc_uint8_t *src = (const utf8proc_uint8_t *)str_ptr;
  utf8proc_ssize_t src_len = (utf8proc_ssize_t)str_len;

  ant_offset_t out_len = 0;
  utf8proc_ssize_t pos = 0;
  
  while (pos < src_len) {
    utf8proc_int32_t cp;
    utf8proc_ssize_t n = utf8_next(src + pos, src_len - pos, &cp);
    if (cp < 0) { out_len++; pos++; continue; }
    utf8proc_uint8_t tmp[4];
    out_len += (ant_offset_t)utf8proc_encode_char(utf8proc_toupper(cp), tmp);
    pos += n;
  }

  ant_value_t result = js_mkstr(js, NULL, out_len);
  if (is_err(result)) return result;
  
  ant_offset_t result_len, result_off = vstr(js, result, &result_len);
  char *result_ptr = (char *)(uintptr_t)(result_off);
  uint8_t ascii_state = STR_ASCII_YES;

  pos = 0;
  ant_offset_t wpos = 0;
  
  while (pos < src_len) {
    utf8proc_int32_t cp;
    utf8proc_ssize_t n = utf8_next(src + pos, src_len - pos, &cp);
    
    if (cp < 0) {
      unsigned char byte = src[pos];
      if (byte >= 0x80) ascii_state = STR_ASCII_NO;
      result_ptr[wpos++] = (char)byte;
      pos++; continue;
    }
    
    utf8proc_int32_t mapped = utf8proc_toupper(cp);
    if (mapped >= 0x80) ascii_state = STR_ASCII_NO;
    
    wpos += (ant_offset_t)utf8proc_encode_char(mapped, (utf8proc_uint8_t *)(result_ptr + wpos));
    pos += n;
  }
  
  str_set_ascii_state(result_ptr, ascii_state);
  return result;
}

static ant_value_t builtin_string_trim(ant_params_t) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != kTypeString) return js_mkerr(js, "trim called on non-string");
  
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  
  ant_offset_t start = 0, end = str_len;
  while (start < end && is_space(str_ptr[start])) start++;
  while (end > start && is_space(str_ptr[end - 1])) end--;
  
  return js_mkstr_byte_range(js, str_ptr, start, end - start);
}

static ant_value_t builtin_string_trimStart(ant_params_t) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != kTypeString) return js_mkerr(js, "trimStart called on non-string");
  
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  
  ant_offset_t start = 0;
  while (start < str_len && is_space(str_ptr[start])) start++;
  
  return js_mkstr_byte_range(js, str_ptr, start, str_len - start);
}

static ant_value_t builtin_string_trimEnd(ant_params_t) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != kTypeString) return js_mkerr(js, "trimEnd called on non-string");
  
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  
  ant_offset_t end = str_len;
  while (end > 0 && is_space(str_ptr[end - 1])) end--;
  
  return js_mkstr_byte_range(js, str_ptr, 0, end);
}

static ant_value_t builtin_string_repeat(ant_params_t) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != kTypeString) return js_mkerr(js, "repeat called on non-string");
  if (nargs < 1 || vtype(args[0]) != kTypeNumber) return js_mkerr(js, "repeat count required");
  
  double count_d = tod(args[0]);
  if (count_d < 0 || count_d != (double)(long)count_d) return js_mkerr(js, "invalid repeat count");
  ant_offset_t count = (ant_offset_t) count_d;
  
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  
  if (count == 0 || str_len == 0) return js_mkstr(js, "", 0);
  
  ant_value_t result = js_mkstr(js, NULL, str_len * count);
  if (is_err(result)) return result;
  
  ant_offset_t result_len, result_off = vstr(js, result, &result_len);
  char *result_ptr = (char *)(uintptr_t)(result_off);
  
  for (ant_offset_t i = 0; i < count; i++)
    memcpy(result_ptr + i * str_len, str_ptr, str_len);
  
  str_flat_finish(
    str_flat_from_bytes(result_ptr),
    str_is_ascii(str_ptr) ? STR_ASCII_YES : STR_ASCII_NO
  );
  
  return result;
}

static ant_value_t builtin_string_padStart(ant_params_t) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != kTypeString) return js_mkerr(js, "padStart called on non-string");
  if (nargs < 1 || vtype(args[0]) != kTypeNumber) return str;
  
  ant_offset_t target_len = (ant_offset_t)tod(args[0]);
  if (target_len <= 0) return str;
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  size_t str_units_len = (size_t)str_utf16_len(js, str);
  
  if ((size_t)target_len <= str_units_len) return str;
  
  ant_value_t pad_val = js_mkstr(js, " ", 1);
  if (nargs >= 2 && vtype(args[1]) != kTypeUndefined) {
    pad_val = coerce_to_str(js, args[1]);
    if (is_err(pad_val)) return pad_val;
  }
  ant_offset_t pad_len, pad_off = vstr(js, pad_val, &pad_len);
  const char *pad_str = (char *)(uintptr_t)(pad_off);
  size_t pad_utf16_len = (size_t)str_utf16_len(js, pad_val);
  
  if (pad_utf16_len == 0) return str;
  
  size_t fill_utf16_len = (size_t)target_len - str_units_len;
  size_t full_repeats = fill_utf16_len / pad_utf16_len;
  size_t rem_utf16 = fill_utf16_len % pad_utf16_len;
  size_t rem_bytes = 0;
  if (rem_utf16 > 0) {
    int off = utf16_index_to_byte_offset(pad_str, (size_t)pad_len, rem_utf16, NULL);
    if (off < 0) return str;
    rem_bytes = (size_t)off;
  }
  size_t fill_bytes = full_repeats * (size_t)pad_len + rem_bytes;
  size_t total_bytes = fill_bytes + (size_t)str_len;

  ant_value_t result = js_mkstr(js, NULL, (ant_offset_t)total_bytes);
  if (is_err(result)) return result;
  
  ant_offset_t result_len, result_off = vstr(js, result, &result_len);
  char *result_ptr = (char *)(uintptr_t)(result_off);
  
  size_t pos = 0;
  for (size_t i = 0; i < full_repeats; i++) {
    memcpy(result_ptr + pos, pad_str, (size_t)pad_len);
    pos += (size_t)pad_len;
  }
  
  if (rem_bytes > 0) {
    memcpy(result_ptr + pos, pad_str, rem_bytes);
    pos += rem_bytes;
  }
  
  memcpy(result_ptr + pos, str_ptr, (size_t)str_len);
  str_flat_finish(
    str_flat_from_bytes(result_ptr),
    (str_is_ascii(pad_str) && str_is_ascii(str_ptr)) ? STR_ASCII_YES : STR_ASCII_NO
  );
  
  return result;
}

static ant_value_t builtin_string_padEnd(ant_params_t) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != kTypeString) return js_mkerr(js, "padEnd called on non-string");
  if (nargs < 1 || vtype(args[0]) != kTypeNumber) return str;
  
  ant_offset_t target_len = (ant_offset_t)tod(args[0]);
  if (target_len <= 0) return str;
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  size_t str_units_len = (size_t)str_utf16_len(js, str);
  
  if ((size_t)target_len <= str_units_len) return str;
  
  ant_value_t pad_val = js_mkstr(js, " ", 1);
  if (nargs >= 2 && vtype(args[1]) != kTypeUndefined) {
    pad_val = coerce_to_str(js, args[1]);
    if (is_err(pad_val)) return pad_val;
  }
  ant_offset_t pad_len, pad_off = vstr(js, pad_val, &pad_len);
  const char *pad_str = (char *)(uintptr_t)(pad_off);
  size_t pad_utf16_len = (size_t)str_utf16_len(js, pad_val);
  
  if (pad_utf16_len == 0) return str;
  
  size_t fill_utf16_len = (size_t)target_len - str_units_len;
  size_t full_repeats = fill_utf16_len / pad_utf16_len;
  size_t rem_utf16 = fill_utf16_len % pad_utf16_len;
  size_t rem_bytes = 0;
  if (rem_utf16 > 0) {
    int off = utf16_index_to_byte_offset(pad_str, (size_t)pad_len, rem_utf16, NULL);
    if (off < 0) return str;
    rem_bytes = (size_t)off;
  }
  size_t fill_bytes = full_repeats * (size_t)pad_len + rem_bytes;
  size_t total_bytes = (size_t)str_len + fill_bytes;

  ant_value_t result = js_mkstr(js, NULL, (ant_offset_t)total_bytes);
  if (is_err(result)) return result;
  
  ant_offset_t result_len, result_off = vstr(js, result, &result_len);
  char *result_ptr = (char *)(uintptr_t)(result_off);
  
  memcpy(result_ptr, str_ptr, (size_t)str_len);
  size_t pos = (size_t)str_len;
  for (size_t i = 0; i < full_repeats; i++) {
    memcpy(result_ptr + pos, pad_str, (size_t)pad_len);
    pos += (size_t)pad_len;
  }
  
  if (rem_bytes > 0) {
    memcpy(result_ptr + pos, pad_str, rem_bytes);
    pos += rem_bytes;
  }
  
  str_flat_finish(
    str_flat_from_bytes(result_ptr),
    (str_is_ascii(str_ptr) && str_is_ascii(pad_str)) ? STR_ASCII_YES : STR_ASCII_NO
  );
  
  return result;
}

static ant_value_t builtin_string_charAt(ant_params_t) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != kTypeString) return js_mkerr(js, "charAt called on non-string");
  
  double idx_d = nargs < 1 ? 0.0 : js_to_number(js, args[0]);
  if (isnan(idx_d)) idx_d = 0;
  else if (idx_d < 0) idx_d = -floor(-idx_d);
  else idx_d = floor(idx_d);
  if (idx_d < 0 || isinf(idx_d)) return js_mkstr(js, "", 0);
  
  ant_offset_t idx = (ant_offset_t) idx_d;
  ant_offset_t byte_len;
  ant_offset_t str_off = vstr(js, str, &byte_len);
  
  const char *str_data = (const char *)(uintptr_t)(str_off);
  uint32_t code_unit = utf16_code_unit_at(str_data, byte_len, idx);
  if (code_unit == 0xFFFFFFFF) return js_mkstr(js, "", 0);
  
  return js_string_from_utf16_code_unit(js, code_unit);
}

static ant_value_t builtin_string_at(ant_params_t) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != kTypeString) return js_mkerr(js, "at called on non-string");
  
  double idx_d = nargs < 1 ? 0.0 : js_to_number(js, args[0]);
  if (isnan(idx_d) || isinf(idx_d)) return js_mkundef();

  ant_offset_t byte_len; ant_offset_t str_off = vstr(js, str, &byte_len);
  const char *str_data = (const char *)(uintptr_t)(str_off);
  size_t utf16_len = (size_t)str_utf16_len(js, str);
  
  long idx = (long) idx_d;
  if (idx < 0) idx += (long) utf16_len;
  if (idx < 0 || idx >= (long) utf16_len) return js_mkundef();
  
  uint32_t code_unit = utf16_code_unit_at(str_data, byte_len, idx);
  if (code_unit == 0xFFFFFFFF) return js_mkundef();
  
  return js_string_from_utf16_code_unit(js, code_unit);
}

static ant_value_t builtin_string_localeCompare(ant_params_t) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != kTypeString) return js_mkerr(js, "localeCompare called on non-string");
  if (nargs < 1) return tov(0);
  
  ant_value_t that = args[0];
  if (vtype(that) != kTypeString) {
    that = coerce_to_str(js, that);
    if (is_err(that)) return that;
  }
  
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  ant_offset_t that_len, that_off = vstr(js, that, &that_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  const char *that_ptr = (char *)(uintptr_t)(that_off);
  
  int result = strcoll(str_ptr, that_ptr);
  if (result < 0) return tov(-1.0);
  if (result > 0) return tov(1.0);
  
  return tov(0);
}

static ant_value_t builtin_string_lastIndexOf(ant_params_t) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != kTypeString) return js_mkerr(js, "lastIndexOf called on non-string");
  if (nargs == 0) return tov(-1);

  ant_value_t search = args[0];
  if (vtype(search) != kTypeString) return tov(-1);

  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  ant_offset_t search_len, search_off = vstr(js, search, &search_len);

  const char *str_ptr = (char *)(uintptr_t)(str_off);
  const char *search_ptr = (char *)(uintptr_t)(search_off);

  if (str_is_ascii(str_ptr)) {
    size_t max_start = (size_t)str_len;
    if (nargs >= 2 && vtype(args[1]) == kTypeNumber) {
      double pos = tod(args[1]);
      if (isnan(pos)) pos = D(str_len);
      if (pos < 0) pos = 0;
      if (pos > D(str_len)) pos = D(str_len);
      max_start = (size_t) pos;
    }

    if (search_len == 0) return tov(D(max_start));
    if (search_len > str_len || !str_is_ascii(search_ptr)) return tov(-1);

    size_t byte_limit = max_start;
    if (byte_limit > (size_t)str_len - search_len)
      byte_limit = (size_t)str_len - search_len;

    char first = search_ptr[0];
    for (size_t i = byte_limit + 1; i > 0; i--) {
      if (str_ptr[i - 1] == first &&
          memcmp(str_ptr + i - 1, search_ptr, search_len) == 0)
        return tov(D(i - 1));
    }
    return tov(-1);
  }

  size_t utf16_len = (size_t)str_utf16_len(js, str);
  size_t max_start = utf16_len;
  
  if (nargs >= 2 && vtype(args[1]) == kTypeNumber) {
    double pos = tod(args[1]);
    if (isnan(pos)) pos = D(utf16_len);
    if (pos < 0) pos = 0;
    if (pos > D(utf16_len)) pos = D(utf16_len);
    max_start = (size_t) pos;
  }

  if (search_len == 0) return tov(D(max_start));
  if (search_len > str_len) return tov(-1);

  size_t byte_limit;
  if (max_start >= utf16_len) byte_limit = (size_t)str_len;
  else byte_limit = utf16_index_to_byte_offset_floor(str_ptr, str_len, max_start);

  if (byte_limit > (size_t)str_len - search_len)
    byte_limit = (size_t)str_len - search_len;

  char first = search_ptr[0];
  for (size_t i = byte_limit + 1; i > 0; i--) {
    if (str_ptr[i - 1] == first &&
        memcmp(str_ptr + i - 1, search_ptr, search_len) == 0)
      return tov(D(byte_offset_to_utf16(str_ptr, i - 1)));
  }
  
  return tov(-1);
}

static ant_value_t builtin_string_concat(ant_params_t) {
  GC_ROOT_SAVE(root_mark, js);
  
  ant_value_t this_unwrapped = unwrap_primitive(js, js->this_val);
  ant_value_t str = coerce_to_str_hint(js, this_unwrapped, 1);
  
  GC_ROOT_PIN(js, str);
  if (is_err(str)) {
    GC_ROOT_RESTORE(js, root_mark);
    return str;
  }

  if (nargs == 0) {
    GC_ROOT_RESTORE(js, root_mark);
    return str;
  }

  ant_offset_t base_len = 0;
  vstr(js, str, &base_len);
  size_t total_len = (size_t)base_len;
  
  ant_value_t stack_args[8];
  ant_value_t *str_args = stack_args;
  
  bool args_need_free = 
    nargs > (int)(sizeof(stack_args) / sizeof(stack_args[0]));
  
  if (args_need_free) {
    if ((size_t)nargs > SIZE_MAX / sizeof(*str_args)) {
      ant_value_t err = js_mkerr(js, "string too large");
      GC_ROOT_RESTORE(js, root_mark);
      return err;
    }
    
    str_args = (ant_value_t *)calloc(1, (size_t)nargs * sizeof(*str_args));
    if (!str_args) {
      ant_value_t err = js_mkerr(js, "oom");
      GC_ROOT_RESTORE(js, root_mark);
      return err;
    }
  }

  for (int i = 0; i < nargs; i++) {
    str_args[i] = coerce_to_str_hint(js, args[i], 1);
    GC_ROOT_PIN(js, str_args[i]);
    if (is_err(str_args[i])) {
      ant_value_t err = str_args[i];
      if (args_need_free) free(str_args);
      GC_ROOT_RESTORE(js, root_mark);
      return err;
    }

    ant_offset_t arg_len = 0;
    vstr(js, str_args[i], &arg_len);
    if ((size_t)arg_len > SIZE_MAX - total_len) {
      ant_value_t err = js_mkerr(js, "string too large");
      if (args_need_free) free(str_args);
      GC_ROOT_RESTORE(js, root_mark);
      return err;
    }
    total_len += (size_t)arg_len;
  }

  ant_value_t ret = js_mkstr(js, NULL, total_len);
  GC_ROOT_PIN(js, ret);
  
  if (is_err(ret)) {
    if (args_need_free) free(str_args);
    GC_ROOT_RESTORE(js, root_mark);
    return ret;
  }

  ant_flat_string_t *flat = (ant_flat_string_t *)vptr(ret);
  ant_offset_t base_off = vstr(js, str, &base_len);
  
  memcpy(flat->bytes, (const void *)(uintptr_t)base_off, (size_t)base_len);
  size_t pos = (size_t)base_len;

  for (int i = 0; i < nargs; i++) {
    ant_offset_t arg_len, arg_off = vstr(js, str_args[i], &arg_len);
    memcpy(flat->bytes + pos, (const void *)(uintptr_t)arg_off, (size_t)arg_len);
    pos += (size_t)arg_len;
  }
  
  flat->bytes[pos] = '\0';
  str_flat_finish(flat, str_detect_ascii_bytes(flat->bytes, pos));
  
  if (args_need_free) free(str_args);
  GC_ROOT_RESTORE(js, root_mark);
  
  return ret;
}

static ant_value_t builtin_string_normalize(ant_params_t) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != kTypeString) return js_mkerr(js, "normalize called on non-string");

  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (const char *)(uintptr_t)(str_off);

  if (str_len == 0) return js_mkstr(js, "", 0);
  utf8proc_option_t opts = UTF8PROC_COMPOSE | UTF8PROC_STABLE;

  if (nargs >= 1 && vtype(args[0]) != kTypeUndefined) {
    ant_value_t form_val = js_tostring_val(js, args[0]);
    if (is_err(form_val)) return form_val;
    ant_offset_t flen, foff = vstr(js, form_val, &flen);
    const char *form = (const char *)(uintptr_t)(foff);

    if (flen == 3 && memcmp(form, "NFC", 3) == 0) {
      opts = UTF8PROC_COMPOSE | UTF8PROC_STABLE;
    } else if (flen == 3 && memcmp(form, "NFD", 3) == 0) {
      opts = UTF8PROC_DECOMPOSE | UTF8PROC_STABLE;
    } else if (flen == 4 && memcmp(form, "NFKC", 4) == 0) {
      opts = UTF8PROC_COMPOSE | UTF8PROC_STABLE | UTF8PROC_COMPAT;
    } else if (flen == 4 && memcmp(form, "NFKD", 4) == 0) {
      opts = UTF8PROC_DECOMPOSE | UTF8PROC_STABLE | UTF8PROC_COMPAT;
    } else return js_mkerr_typed(js, JS_ERR_RANGE, "The normalization form should be one of NFC, NFD, NFKC, NFKD");
  }

  utf8proc_uint8_t *result = NULL;
  utf8proc_ssize_t rlen = utf8proc_map(
    (const utf8proc_uint8_t *)str_ptr, (utf8proc_ssize_t)str_len, &result, opts
  );

  if (rlen < 0 || !result) {
    if (result) free(result);
    return js_mkstr(js, str_ptr, str_len);
  }

  ant_value_t ret = js_mkstr(js, (const char *)result, (ant_offset_t)rlen);
  free(result);
  
  return ret;
}

static ant_value_t builtin_string_fromCharCode(ant_params_t) {
  if (nargs == 0) return js_mkstr(js, "", 0);

  if ((size_t)nargs > SIZE_MAX / 3) return js_mkerr(js, "string too large");
  size_t capacity = (size_t)nargs * 3;
  char static_buf[256];
  bool needs_free = capacity > sizeof(static_buf);
  char *buf = needs_free ? (char *)malloc(capacity) : static_buf;
  if (!buf) return js_mkerr(js, "oom");

  size_t len = 0;
  for (int i = 0; i < nargs; i++) {
    double d = vtype(args[i]) == kTypeNumber ? tod(args[i]) : js_to_number(js, args[i]);
    uint32_t code_unit = isfinite(d) ? ((uint32_t)d & 0xFFFFu) : 0;

    if (code_unit < 0x80) {
      buf[len++] = (char)code_unit;
    } else if (code_unit < 0x800) {
      buf[len++] = (char)(0xC0 | (code_unit >> 6));
      buf[len++] = (char)(0x80 | (code_unit & 0x3F));
    } else {
      buf[len++] = (char)(0xE0 | (code_unit >> 12));
      buf[len++] = (char)(0x80 | ((code_unit >> 6) & 0x3F));
      buf[len++] = (char)(0x80 | (code_unit & 0x3F));
    }
  }

  ant_value_t result = js_mkstr(js, buf, len);
  if (needs_free) free(buf);
  return result;
}

static ant_value_t builtin_string_fromCodePoint(ant_params_t) {
  if (nargs == 0) return js_mkstr(js, "", 0);

  if ((size_t)nargs > SIZE_MAX / 4) return js_mkerr(js, "string too large");
  size_t capacity = (size_t)nargs * 4;
  char static_buf[256];
  bool needs_free = capacity > sizeof(static_buf);
  char *buf = needs_free ? (char *)calloc(1, capacity) : static_buf;
  if (!buf) return js_mkerr(js, "oom");

  size_t len = 0;
  for (int i = 0; i < nargs; i++) {
    if (vtype(args[i]) != kTypeNumber) continue;
    double d = tod(args[i]);
    
    if (d < 0 || d > 0x10FFFF || d != floor(d)) {
      if (needs_free) free(buf);
      return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid code point");
    }
    
    uint32_t cp = (uint32_t)d;

    if (nargs >= 16 && cp >= 0x10000) {
      buf[len++] = (char)(0xF0 | (cp >> 18));
      buf[len++] = (char)(0x80 | ((cp >> 12) & 0x3F));
      buf[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      buf[len++] = (char)(0x80 | (cp & 0x3F));
      continue;
    }
    
    if (cp < 0x80) {
      buf[len++] = (char)cp;
    } else if (cp < 0x800) {
      buf[len++] = (char)(0xC0 | (cp >> 6));
      buf[len++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      buf[len++] = (char)(0xE0 | (cp >> 12));
      buf[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      buf[len++] = (char)(0x80 | (cp & 0x3F));
    } else {
      buf[len++] = (char)(0xF0 | (cp >> 18));
      buf[len++] = (char)(0x80 | ((cp >> 12) & 0x3F));
      buf[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      buf[len++] = (char)(0x80 | (cp & 0x3F));
    }
  }

  ant_value_t result = js_mkstr(js, buf, len);
  if (needs_free) free(buf);
  return result;
}

static ant_value_t builtin_string_raw(ant_params_t) {
  if (nargs < 1 || is_null(args[0]) || is_undefined(args[0])) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "String.raw requires a template object");
  }

  ant_value_t tmpl = args[0];
  if (!is_object_type(tmpl)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "String.raw requires a template object");
  }

  ant_value_t raw = js_get(js, tmpl, "raw");
  if (is_null(raw) || is_undefined(raw) || !is_object_type(raw)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "String.raw requires template.raw");
  }

  ant_value_t raw_len_val = js_get(js, raw, "length");
  double raw_len_num = js_to_number(js, raw_len_val);
  if (!isfinite(raw_len_num) || raw_len_num <= 0) return js_mkstr(js, "", 0);

  size_t literal_count = (size_t)raw_len_num;
  if (literal_count == 0) return js_mkstr(js, "", 0);

  string_builder_t sb;
  char static_buf[256];
  string_builder_init(&sb, static_buf, sizeof(static_buf));
  
  ant_value_t err = js_mkundef();
  for (size_t i = 0; i < literal_count; i++) {
    ant_value_t chunk = js_mkundef();
    if (vtype(raw) == kTypeArray) chunk = js_arr_get(js, raw, (ant_offset_t)i);
    else {
      char key[32];
      snprintf(key, sizeof(key), "%zu", i);
      chunk = js_get(js, raw, key);
    }
    
    if (!string_builder_append_value(js, &sb, chunk, &err)) {
      string_builder_dispose(&sb);
      return is_err(err) ? err : js_mkerr(js, "oom");
    }
    
    if (i + 1 >= literal_count || (int)(i + 1) >= nargs) continue;
    if (!string_builder_append_value(js, &sb, args[i + 1], &err)) {
      string_builder_dispose(&sb);
      return is_err(err) ? err : js_mkerr(js, "oom");
    }
  }

  return string_builder_finalize(js, &sb);
}

static ant_value_t builtin_number_toString(ant_params_t) {
  ant_value_t num = unwrap_primitive(js, js->this_val);
  if (vtype(num) != kTypeNumber) return js_mkerr(js, "toString called on non-number");
  
  int radix = 10;
  if (nargs >= 1 && vtype(args[0]) == kTypeNumber) {
    radix = (int)tod(args[0]);
    if (radix < 2 || radix > 36) {
      return js_mkerr(js, "radix must be between 2 and 36");
    }
  }
  
  if (radix == 10) return js_tostring_val(js, num);
  double val = tod(num);
  
  if (isnan(val)) return js_mkstr(js, "NaN", 3);
  if (isinf(val)) return val > 0 ? js_mkstr(js, "Infinity", 8) : js_mkstr(js, "-Infinity", 9);
  
  char buf[128];
  char *p = buf + sizeof(buf) - 1;
  *p = '\0';
  
  bool negative = val < 0;
  if (negative) val = -val;
  
  long long int_part = (long long)val;
  double frac_part = val - (double)int_part;
  
  if (int_part == 0) {
    *--p = '0';
  } else {
    while (int_part > 0 && p > buf) {
      int digit = int_part % radix;
      *--p = (char)(digit < 10 ? '0' + digit : 'a' + (digit - 10));
      int_part /= radix;
    }
  }
  
  if (negative && p > buf) {
    *--p = '-';
  }
  
  size_t int_len = strlen(p);
  
  if (frac_part > 0.0000001) {
    char frac_buf[64];
    int frac_pos = 0;
    frac_buf[frac_pos++] = '.';
    
    for (int i = 0; i < 16 && frac_part > 0.0000001 && frac_pos < 63; i++) {
      frac_part *= radix;
      int digit = (int)frac_part;
      frac_buf[frac_pos++] = (char)(digit < 10 ? '0' + digit : 'a' + (digit - 10));
      frac_part -= digit;
    }
    frac_buf[frac_pos] = '\0';
    
    char result[192];
    snprintf(result, sizeof(result), "%s%s", p, frac_buf);
    return js_mkstr(js, result, strlen(result));
  }
  
  return js_mkstr(js, p, int_len);
}

static ant_value_t builtin_number_toFixed(ant_params_t) {
  ant_value_t num = unwrap_primitive(js, js->this_val);
  if (vtype(num) != kTypeNumber) return js_mkerr(js, "toFixed called on non-number");
  
  double d = tod(num);
  if (isnan(d)) return js_mkstr(js, "NaN", 3);
  if (isinf(d)) return d > 0 ? js_mkstr(js, "Infinity", 8) : js_mkstr(js, "-Infinity", 9);
  
  int digits = 0;
  if (nargs >= 1 && vtype(args[0]) != kTypeUndefined) {
    digits = (int) tod(args[0]);
    if (digits < 0 || digits > 100) {
      return js_mkerr_typed(js, JS_ERR_RANGE, "toFixed() digits argument must be between 0 and 100");
    }
  }
  
  if (fabs(d) >= 1e21) {
    char buf[64];
    size_t len = js_number_to_chars(num, buf, sizeof(buf));
    return js_mkstr(js, buf, len);
  }

  char buf[160];
  size_t len = ant_number_to_fixed(d, digits, buf, sizeof(buf));
  if (len == 0) return js_mkerr(js, "number formatting failed");
  
  return js_mkstr(js, buf, len);
}

static ant_value_t builtin_number_toPrecision(ant_params_t) {
  ant_value_t num = unwrap_primitive(js, js->this_val);
  if (vtype(num) != kTypeNumber) return js_mkerr(js, "toPrecision called on non-number");
  
  double d = tod(num);
  if (isnan(d)) return js_mkstr(js, "NaN", 3);
  if (isinf(d)) return d > 0 ? js_mkstr(js, "Infinity", 8) : js_mkstr(js, "-Infinity", 9);
  
  if (nargs < 1 || vtype(args[0]) == kTypeUndefined) {
    char buf[64];
    size_t len = js_number_to_chars(num, buf, sizeof(buf));
    return js_mkstr(js, buf, len);
  }
  
  int precision = (int) tod(args[0]);
  if (precision < 1 || precision > 100) {
    return js_mkerr_typed(js, JS_ERR_RANGE, "toPrecision() argument must be between 1 and 100");
  }
  
  char buf[160];
  size_t len = ant_number_to_precision(d, precision, buf, sizeof(buf));
  if (len == 0) return js_mkerr(js, "number formatting failed");
  
  return js_mkstr(js, buf, len);
}

static ant_value_t builtin_number_toExponential(ant_params_t) {
  ant_value_t num = unwrap_primitive(js, js->this_val);
  if (vtype(num) != kTypeNumber) return js_mkerr(js, "toExponential called on non-number");
  
  double d = tod(num);
  if (isnan(d)) return js_mkstr(js, "NaN", 3);
  if (isinf(d)) return d > 0 ? js_mkstr(js, "Infinity", 8) : js_mkstr(js, "-Infinity", 9);
  
  int digits = -1;
  if (nargs >= 1 && vtype(args[0]) != kTypeUndefined) {
    digits = (int) tod(args[0]);
    if (digits < 0 || digits > 100) {
      return js_mkerr_typed(js, JS_ERR_RANGE, "toExponential() argument must be between 0 and 100");
    }
  }
  
  char buf[160];
  size_t len = ant_number_to_exponential(d, digits, buf, sizeof(buf));
  if (len == 0) return js_mkerr(js, "number formatting failed");
  
  return js_mkstr(js, buf, len);
}

static ant_value_t builtin_number_valueOf(ant_params_t) {
  (void) args; (void) nargs;
  ant_value_t num = unwrap_primitive(js, js->this_val);
  if (vtype(num) != kTypeNumber) return js_mkerr(js, "valueOf called on non-number");
  return num;
}

static ant_value_t builtin_number_toLocaleString(ant_params_t) {
  (void) args; (void) nargs;
  ant_value_t num = unwrap_primitive(js, js->this_val);
  if (vtype(num) != kTypeNumber) return js_mkerr(js, "toLocaleString called on non-number");
  char buf[128];
  size_t len = format_number_to_locale_string(num, buf, sizeof(buf));
  if (len == 0) return js_mkerr(js, "number formatting failed");
  return js_mkstr(js, buf, len);
}

static ant_value_t string_this_value(ant_t *js, const char *method) {
  ant_value_t self = js->this_val;
  if (vtype(self) == kTypeString) return self;
  
  ant_value_t wrapped = js_string_wrapper_value(self);
  if (vtype(wrapped) == kTypeString) return wrapped;
  
  return js_mkerr_typed(js, JS_ERR_TYPE, "String.prototype.%s requires that 'this' be a String", method);
}

static ant_value_t builtin_string_valueOf(ant_params_t) {
  return string_this_value(js, "valueOf");
}

static ant_value_t builtin_string_toString(ant_params_t) {
  return string_this_value(js, "toString");
}

static ant_value_t builtin_boolean_valueOf(ant_params_t) {
  (void) args; (void) nargs;
  ant_value_t b = unwrap_primitive(js, js->this_val);
  if (vtype(b) != kTypeBool) return js_mkerr(js, "valueOf called on non-boolean");
  return b;
}

static ant_value_t builtin_boolean_toString(ant_params_t) {
  (void) args; (void) nargs;
  ant_value_t b = unwrap_primitive(js, js->this_val);
  if (vtype(b) != kTypeBool) return js_mkerr(js, "toString called on non-boolean");
  return vdata(b) ? js_mkstr(js, "true", 4) : js_mkstr(js, "false", 5);
}

static ant_value_t builtin_parseInt(ant_params_t) {
  if (nargs < 1) return tov(JS_NAN);
  
  ant_value_t str_val = args[0];
  if (vtype(str_val) != kTypeString) {
    const char *str = js_str(js, str_val);
    str_val = js_mkstr(js, str, strlen(str));
  }
  
  ant_offset_t str_len, str_off = vstr(js, str_val, &str_len);
  const char *str = (char *)(uintptr_t)(str_off);
  
  int radix = 0;
  if (nargs >= 2 && vtype(args[1]) == kTypeNumber) {
    radix = (int) tod(args[1]);
    if (radix != 0 && (radix < 2 || radix > 36)) return tov(JS_NAN);
  }
  
  ant_offset_t i = 0;
  while (i < str_len && is_space(str[i])) i++;
  
  if (i >= str_len) return tov(JS_NAN);
  
  int sign = 1;
  if (str[i] == '-') {
    sign = -1;
    i++;
  } else if (str[i] == '+') {
    i++;
  }
  
  if ((radix == 0 || radix == 16) && i + 1 < str_len && str[i] == '0' && (str[i + 1] == 'x' || str[i + 1] == 'X')) {
    radix = 16;
    i += 2;
  }
  
  if (radix == 0) radix = 10;
  
  double result = 0;
  bool found_digit = false;
  
  while (i < str_len) {
    char ch = str[i];
    int digit = -1;
    
    if (ch >= '0' && ch <= '9') {
      digit = ch - '0';
    } else if (ch >= 'a' && ch <= 'z') {
      digit = ch - 'a' + 10;
    } else if (ch >= 'A' && ch <= 'Z') {
      digit = ch - 'A' + 10;
    }
    
    if (digit < 0 || digit >= radix) break;
    
    result = result * radix + digit;
    found_digit = true;
    i++;
  }
  
  if (!found_digit) return tov(JS_NAN);
  
  return tov(sign * result);
}

static ant_value_t builtin_parseFloat(ant_params_t) {
  if (nargs < 1) return tov(JS_NAN);
  
  ant_value_t str_val = args[0];
  if (vtype(str_val) != kTypeString) {
    const char *str = js_str(js, str_val);
    str_val = js_mkstr(js, str, strlen(str));
  }
  
  ant_offset_t str_len, str_off = vstr(js, str_val, &str_len);
  const char *str = (char *)(uintptr_t)(str_off);
  
  ant_offset_t i = 0;
  while (i < str_len && is_space(str[i])) i++;
  
  if (i >= str_len) return tov(JS_NAN);
  
  double result = JS_NAN;
  size_t processed = 0;
  
  if (!ant_number_parse(
    str + i,
    (size_t)(str_len - i),
    ANT_NUMBER_PARSE_FLOAT_PREFIX,
    &result, &processed
  ) || processed == 0) return tov(JS_NAN);

  return tov(result);
}

double js_parse_int_value(ant_t *js, ant_value_t arg) {
  ant_value_t result = builtin_parseInt(js, &arg, 1, js_mkundef());
  return vtype(result) == kTypeNumber ? tod(result) : JS_NAN;
}

double js_parse_float_value(ant_t *js, ant_value_t arg) {
  ant_value_t result = builtin_parseFloat(js, &arg, 1, js_mkundef());
  return vtype(result) == kTypeNumber ? tod(result) : JS_NAN;
}

static ant_value_t builtin_btoa(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "btoa requires 1 argument");
  
  ant_value_t str_val = args[0];
  if (vtype(str_val) != kTypeString) {
    const char *str = js_str(js, str_val);
    str_val = js_mkstr(js, str, strlen(str));
  }
  
  ant_offset_t str_len, str_off = vstr(js, str_val, &str_len);
  const char *str = (char *)(uintptr_t)(str_off);

  size_t bytes_len = 0;
  bool is_latin1 = false;
  uint8_t *bytes = utf8_to_latin1(str, (size_t)str_len, &bytes_len, &is_latin1);
  
  if (!bytes) return is_latin1
    ? js_mkerr(js, "out of memory")
    : js_mkerr(js, "btoa: string contains characters outside Latin1 range");

  size_t out_len;
  char *out = ant_base64_encode(bytes, bytes_len, &out_len);
  free(bytes);
  if (!out) return js_mkerr(js, "out of memory");
  
  ant_value_t result = js_mkstr(js, out, out_len);
  free(out);
  
  return result;
}

static ant_value_t builtin_atob(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "atob requires 1 argument");
  
  ant_value_t str_val = args[0];
  if (vtype(str_val) != kTypeString) {
    const char *str = js_str(js, str_val);
    str_val = js_mkstr(js, str, strlen(str));
  }
  
  ant_offset_t str_len, str_off = vstr(js, str_val, &str_len);
  const char *str = (char *)(uintptr_t)(str_off);
  if (str_len == 0) return js_mkstr(js, "", 0);
  
  size_t cap = ((size_t)str_len / 4) * 3 + 3, read = 0, out_len = 0;
  uint8_t *out = malloc(cap);
  if (!out) return js_mkerr(js, "out of memory");

  const char *error = NULL;
  if (!ant_base64_decode_into(str, (size_t)str_len, false, ANT_BASE64_LOOSE, out, cap, &read, &out_len, &error)) {
    free(out);
    return js_throw(js, make_dom_exception(js, "The string to be decoded is not correctly encoded.", "InvalidCharacterError"));
  }

  size_t result_len = 0;
  char *result_str = latin1_to_utf8(out, out_len, &result_len);
  free(out);
  if (!result_str) return js_mkerr(js, "out of memory");

  ant_value_t result = js_mkstr(js, result_str, result_len);
  free(result_str);
  
  return result;
}

static ant_value_t builtin_resolve_internal(ant_params_t);
static ant_value_t builtin_reject_internal(ant_params_t);

static size_t strpromise(ant_t *js, ant_value_t value, char *buf, size_t len) {
  uint32_t pid = get_promise_id(js, value);
  ant_promise_state_t *pd = get_promise_data(js, value, false);
  
  const char *content;
  char *allocated = NULL;
  
  if (!pd || pd->state == 0) {
    content = "<pending>";
  } else if (pd->state == 2) {
    char *val = tostr_alloc(js, pd->value);
    allocated = calloc(1, strlen(val) + 12);
    sprintf(allocated, "<rejected> %s", val);
    free(val);
    content = allocated;
  } else { content = allocated = tostr_alloc(js, pd->value); }
  
  uint32_t trigger_pid = 0;
  if (pd && vtype(pd->trigger_parent) == kTypePromise) {
    trigger_pid = get_promise_id(js, pd->trigger_parent);
  }
  size_t result = trigger_pid
    ? (size_t)snprintf(buf, len, "Promise {\n  %s,\n  Symbol(async_id): %u,\n  Symbol(trigger_async_id): %u\n}", content, pid, trigger_pid)
    : (size_t)snprintf(buf, len, "Promise {\n  %s,\n  Symbol(async_id): %u\n}", content, pid);
  
  if (allocated) free(allocated);
  return result;
}

static ant_promise_state_t *get_promise_data(ant_t *js, ant_value_t promise, bool create) {
  if (vtype(promise) != kTypePromise) return NULL;
  ant_object_t *obj = js_obj_ptr(js_as_obj(promise));
  if (!obj) return NULL;
  
  ant_promise_state_t *existing = ant_object_promise_state(obj);
  if (existing) return existing;
  if (!create) return NULL;

  ant_promise_state_t *entry = (ant_promise_state_t *)calloc(1, sizeof(*entry));
  if (!entry) return NULL;
  
  entry->promise_id = next_promise_id++;
  entry->trigger_parent = js_mkundef();
  entry->inline_handler = (promise_handler_t){ 0 };
  entry->handlers = NULL;
  entry->has_inline_handler = false;
  entry->state = 0;
  entry->value = js_mkundef();
  entry->trigger_queued = false;
  entry->has_rejection_handler = false;
  entry->processing = false;
  entry->unhandled_reported = false;
  ant_object_make_promise(obj, entry);
  
  return entry;
}

js_promise_settlement_t js_promise_get_settlement(
  ant_t *js, ant_value_t promise, ant_value_t *value_out
) {
  if (value_out) *value_out = js_mkundef();
  ant_promise_state_t *pd = get_promise_data(js, promise, false);

  if (!pd) return JS_PROMISE_INVALID;
  if (pd->state == 0) return JS_PROMISE_PENDING;
  if (value_out) *value_out = pd->value;

  return pd->state == 1 ? JS_PROMISE_FULFILLED : JS_PROMISE_REJECTED;
}

static uint32_t get_promise_id(ant_t *js, ant_value_t p) {
  ant_promise_state_t *pd = get_promise_data(js, p, false);
  return pd ? pd->promise_id : 0;
}

bool js_mark_promise_trigger_queued(ant_t *js, ant_value_t promise) {
  ant_promise_state_t *pd = get_promise_data(js, promise, false);
  if (!pd) return true;
  if (pd->trigger_queued) return false;
  pd->trigger_queued = true;
  return true;
}

void js_mark_promise_trigger_dequeued(ant_t *js, ant_value_t promise) {
  ant_promise_state_t *pd = get_promise_data(js, promise, false);
  if (!pd) return;
  pd->trigger_queued = false;
}

static ant_value_t make_data_cfunc(
  ant_t *js, ant_value_t data,
  ant_cfunc_t fn
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, data);

  ant_value_t obj = mkobj(js, 0);
  if (is_err(obj)) {
    GC_ROOT_RESTORE(js, root_mark);
    return obj;
  }

  GC_ROOT_PIN(js, obj);
  set_slot(obj, SLOT_DATA, data);
  set_slot(obj, SLOT_CFUNC, js_mkfun_dyn(fn));

  ant_value_t func = js_obj_to_func(js, obj);
  GC_ROOT_RESTORE(js, root_mark);
  return func;
}

ant_value_t js_mkpromise(ant_t *js) {
  ant_value_t obj = mkobj(js, 0);

  if (is_err(obj)) return obj;
  if (!get_promise_data(js, mkval(kTypePromise, vdata(obj)), true))
    return js_mkerr(js, "out of memory");

  set_slot(obj, SLOT_CTOR, js->sym.promise_ctor);
  ant_value_t promise_proto = js->sym.promise_proto;
  
  if (is_object_type(promise_proto)) js_set_proto_init(obj, promise_proto);
  return mkval(kTypePromise, vdata(obj));
}

ant_value_t js_mkgenerator(ant_t *js) {
  ant_value_t obj = mkobj(js, 0);
  if (is_err(obj)) return obj;

  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return js_mkerr(js, "invalid generator object");
  ptr->type_tag = kTypeGenerator;

  ant_value_t gen = mkval(kTypeGenerator, vdata(obj));
  if (is_object_type(js->sym.generator_proto)) {
    js_set_proto_init(gen, js->sym.generator_proto);
  }

  return gen;
}

ant_value_t js_promise_then(ant_t *js, ant_value_t promise, ant_value_t on_fulfilled, ant_value_t on_rejected) {
  ant_value_t args_then[2] = { on_fulfilled, on_rejected };
  return sv_call_native_this(js, builtin_promise_then, promise, args_then, 2);
}

void Ant_Promise_Observe(ant_t *js, ant_value_t promise, ant_value_t on_fulfilled, ant_value_t on_rejected) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, promise);
  GC_ROOT_PIN(js, on_fulfilled);
  GC_ROOT_PIN(js, on_rejected);

  ant_value_t result = is_err(promise) ? promise : js_promise_then(js, promise, on_fulfilled, on_rejected);
  if (is_err(result)) {
    ant_value_t reason = Ant_Error_ConsumeMarker(js, result);
    queue_microtask_with_args(js, on_rejected, &reason, 1);
  } else {
    ant_promise_state_t *pd = get_promise_data(js, result, false);
    if (pd) pd->has_rejection_handler = true;
  }

  GC_ROOT_RESTORE(js, root_mark);
}

void js_mark_promise_rejection_handled_chain(ant_t *js, ant_value_t promise) {
  ant_value_t current = promise;

  while (vtype(current) == kTypePromise) {
    ant_promise_state_t *pd = get_promise_data(js, current, false);
    if (!pd) break;
    
    if (pd->unhandled_reported) js_fire_rejection_handled(js, current, pd->value);
    pd->has_rejection_handler = true;
    pd->unhandled_reported = false;
    current = pd->trigger_parent;
  }
}

static inline ant_value_t js_get_thenable_then(ant_t *js, ant_value_t value) {
  if (!is_object_type(value) && vtype(value) != kTypeBuiltin) return js_mkundef();

  if (
    vtype(value) == kTypePromise &&
    !js->promise_then_protector_invalid &&
    get_slot(value, SLOT_PROTO) == js->sym.promise_proto
  ) return js->sym.promise_then;

  return js_getprop_fallback(js, value, "then");
}

ant_value_t js_promise_assimilate_awaitable(ant_t *js, ant_value_t value) {
  if (
    vtype(value) == kTypePromise &&
    !js->promise_constructor_protector_invalid &&
    get_slot(value, SLOT_PROTO) == js->sym.promise_proto
  ) return value;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, value);

  bool is_promise = vtype(value) == kTypePromise;
  ant_value_t ctor = is_promise
    ? js_getprop_fallback(js, value, "constructor")
    : js_mkundef();

  if (is_err(ctor)) {
    GC_ROOT_RESTORE(js, root_mark);
    return ctor;
  }

  if (is_promise && strict_eq_values(js, ctor, js->sym.promise_ctor)) {
    GC_ROOT_RESTORE(js, root_mark);
    return value;
  }

  ant_value_t promise = js_mkpromise(js);
  GC_ROOT_PIN(js, promise);
  if (is_err(promise)) {
    GC_ROOT_RESTORE(js, root_mark);
    return promise;
  }

  js_resolve_promise(js, promise, value);
  GC_ROOT_RESTORE(js, root_mark);

  return promise;
}

static ant_value_t make_promise_resolving_functions(
  ant_t *js,
  ant_value_t promise,
  ant_value_t *resolve_out,
  ant_value_t *reject_out
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, promise);

  *resolve_out = js_mkundef();
  *reject_out = js_mkundef();

  ant_value_t resolve_fn = make_data_cfunc(js, promise, builtin_resolve_internal);
  GC_ROOT_PIN(js, resolve_fn);
  if (is_err(resolve_fn)) {
    GC_ROOT_RESTORE(js, root_mark);
    return resolve_fn;
  }
  set_slot(resolve_fn, SLOT_SETTLED, js_false);

  ant_value_t reject_fn = make_data_cfunc(js, resolve_fn, builtin_reject_internal);
  GC_ROOT_PIN(js, reject_fn);
  if (is_err(reject_fn)) {
    GC_ROOT_RESTORE(js, root_mark);
    return reject_fn;
  }

  *resolve_out = resolve_fn;
  *reject_out = reject_fn;
  GC_ROOT_RESTORE(js, root_mark);
  
  return js_mkundef();
}

void Ant_Promise_ClearAwaitCoroutine(ant_t *js, ant_value_t promise, coroutine_t *coro) {
  if (vtype(promise) != kTypePromise || !coro) return;

  ant_promise_state_t *pd = get_promise_data(js, promise, false);
  if (!pd || promise_handler_count(pd) == 0) return;

  if (pd->has_inline_handler) {
    if (pd->inline_handler.await_coro == coro) pd->inline_handler.await_coro = NULL;
    return;
  }

  if (!pd->handlers) return;
  promise_handler_t *h = NULL;
  
  while ((h = (promise_handler_t *)utarray_next(pd->handlers, h)))
    if (h->await_coro == coro) h->await_coro = NULL;
}

js_await_result_t js_promise_await_coroutine(ant_t *js, ant_value_t promise, coroutine_t *coro) {
  js_await_result_t result = {
    .state = JS_AWAIT_PENDING,
    .value = js_mkundef(),
  };

  if (vtype(promise) != kTypePromise || !coro) return result;
  ant_promise_state_t *pd = get_promise_data(js, promise, false);
  
  if (!pd) {
    result.state = JS_AWAIT_ERROR;
    result.value = js_mkerr(js, "invalid promise state");
    return result;
  }

  bool direct_resume = 
    pd->state == 1 && promise_handler_count(pd) == 0 &&
    !pd->trigger_queued && !pd->processing && 
    pd->trigger_parent == js_mkundef();
    
  promise_handler_t h = { 
    js_mkundef(), js_mkundef(), 
    js_mkundef(), coro
  };
  
  bool queued = direct_resume
    ? queue_await_resume_job(coro, pd->value)
    : promise_handler_append(pd, &h);
  
  if (!queued) {
    result.state = JS_AWAIT_ERROR;
    result.value = js_mkerr(js, "out of memory");
    return result;
  }

  coro->awaited_promise = promise;
  coro->await_registered = true;
  if (!direct_resume) coroutine_hold(coro, CORO_HOLD_AWAIT);

  js_mark_promise_rejection_handled_chain(js, promise);
  
  if (pd->state == 0) gc_root_pending_promise(js, js_obj_ptr(js_as_obj(promise)));
  else if (!direct_resume) queue_promise_trigger(js, promise);

  return result;
}

void js_process_promise_handlers(ant_t *js, ant_value_t promise) {
  ant_object_t *pobj = js_obj_ptr(promise);
  ant_promise_state_t *pd = get_promise_data(js, promise, false);
  if (!pd) return;
  
  int state = pd->state;
  ant_value_t val = pd->value;
  
  uint32_t len = promise_handler_count(pd);
  if (len == 0) return;

  bool inline_batch = len == 1;
  promise_handler_t inline_handler = { 0 };
  
  if (inline_batch) {
    inline_handler = pd->inline_handler;
    promise_handlers_clear(pd);
  }
  
  gc_root_pending_promise(js, pobj);
  pd->processing = true;
  
  for (uint32_t i = 0; i < len; i++) {
    promise_handler_t *h = inline_batch
      ? &inline_handler
      : promise_handler_at(pd, i);
    
    if (!h) continue;
    promise_handler_t handler = *h;
    
    if (handler.await_coro) {
      coroutine_t *await_coro = handler.await_coro;
      h->await_coro = NULL;
      Ant_Coroutine_SettleAndResume(js, await_coro, val, state != 1);
      continue;
    }
    
    GC_ROOT_SAVE(handler_root_mark, js);
    GC_ROOT_PIN(js, handler.onFulfilled);
    GC_ROOT_PIN(js, handler.onRejected);
    GC_ROOT_PIN(js, handler.nextPromise);

    ant_value_t callback = (state == 1) ? handler.onFulfilled : handler.onRejected;
    if (!is_callable(callback)) {
      if (state == 1) js_resolve_promise(js, handler.nextPromise, val);
      else js_reject_promise(js, handler.nextPromise, val);
      GC_ROOT_RESTORE(js, handler_root_mark);
      continue;
    }
    
    ant_value_t res = js_mkundef();
    if (vtype(callback) == kTypeBuiltin) {
      res = sv_invoke_builtin(js, callback, &val, 1, js_mkundef());
    } else {
      ant_value_t call_args[] = { val };
      res = sv_vm_call(js->vm, js, callback, js_mkundef(), call_args, 1, NULL, js_mkundef());
    }
    
    if (!is_err(res)) {
      GC_ROOT_PIN(js, res);
      js_resolve_promise(js, handler.nextPromise, res);
      GC_ROOT_RESTORE(js, handler_root_mark);
      continue;
    }
    
    js_reject_promise(js, handler.nextPromise, res);
    GC_ROOT_RESTORE(js, handler_root_mark);
  }

  pd->processing = false;
  if (!inline_batch) promise_handlers_discard_processed_prefix(pd, len);
  gc_unroot_pending_promise(js, js_obj_ptr(promise));
}

void js_resolve_promise(ant_t *js, ant_value_t p, ant_value_t val) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, p);
  GC_ROOT_PIN(js, val);

  ant_promise_state_t *pd = get_promise_data(js, p, false);
  if (!pd || pd->state != 0) {
    GC_ROOT_RESTORE(js, root_mark);
    return;
  }
  
  if (vtype(val) == kTypePromise && vdata(js_as_obj(val)) == vdata(js_as_obj(p))) {
    ant_value_t err = js_mkerr(js, "TypeError: Chaining cycle");
    GC_ROOT_PIN(js, err);
    js_reject_promise(js, p, err);
    GC_ROOT_RESTORE(js, root_mark);
    return;
  }

  if (is_object_type(val) || vtype(val) == kTypeBuiltin) {
    ant_value_t then_prop = js_get_thenable_then(js, val);
    GC_ROOT_PIN(js, then_prop);

    if (is_err(then_prop)) {
      js_reject_promise(js, p, then_prop);
      GC_ROOT_RESTORE(js, root_mark);
      return;
    }

    if (is_callable(then_prop)) {
      if (!queue_promise_thenable_job(js, p, val, then_prop)) {
        ant_value_t err = js_mkerr(js, "out of memory queuing Promise job");
        GC_ROOT_PIN(js, err);
        js_reject_promise(js, p, err);
      }
      GC_ROOT_RESTORE(js, root_mark);
      
      return;
    }
  }

  pd->state = 1;
  pd->value = val;
  
  gc_write_barrier(js, js_obj_ptr(js_as_obj(p)), val);
  if (promise_has_handlers(pd)) queue_promise_trigger(js, p);
  GC_ROOT_RESTORE(js, root_mark);
}

void js_process_promise_thenable_job(
  ant_t *js, ant_value_t promise, ant_value_t thenable, ant_value_t then_fn
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, promise);
  GC_ROOT_PIN(js, thenable);
  GC_ROOT_PIN(js, then_fn);

  if (
    vtype(thenable) == kTypePromise &&
    then_fn == js->sym.promise_then &&
    !js->promise_species_protector_invalid &&
    get_slot(thenable, SLOT_PROTO) == js->sym.promise_proto
  ) {
    ant_promise_state_t *source = get_promise_data(js, thenable, false);
    promise_handler_t handler = {
      js_mkundef(), js_mkundef(), 
      promise, NULL
    };

    if (!promise_handler_append(source, &handler)) {
      ant_value_t err = js_mkerr(js, "out of memory adopting Promise");
      GC_ROOT_PIN(js, err);
      js_reject_promise(js, promise, err);
      GC_ROOT_RESTORE(js, root_mark);
      return;
    }

    gc_write_barrier(js, js_obj_ptr(js_as_obj(thenable)), promise);
    js_mark_promise_rejection_handled_chain(js, thenable);
    if (source->state == 0)
      gc_root_pending_promise(js, js_obj_ptr(js_as_obj(thenable)));
    else queue_promise_trigger(js, thenable);

    GC_ROOT_RESTORE(js, root_mark);
    return;
  }

  ant_value_t resolve_fn = js_mkundef();
  ant_value_t reject_fn = js_mkundef();
  ant_value_t pair_result = make_promise_resolving_functions(
    js, promise, 
    &resolve_fn, &reject_fn
  );
  
  GC_ROOT_PIN(js, pair_result);
  GC_ROOT_PIN(js, resolve_fn);
  GC_ROOT_PIN(js, reject_fn);
  
  if (is_err(pair_result)) {
    js_reject_promise(js, promise, pair_result);
    GC_ROOT_RESTORE(js, root_mark);
    return;
  }

  ant_value_t then_args[] = { resolve_fn, reject_fn };
  ant_value_t result = sv_vm_call(
    js->vm, js, then_fn, thenable,
    then_args, 2, NULL, js_mkundef()
  );
  
  if (is_err(result) || Ant_Exception_Pending(js)) {
    ant_value_t reject_val = js_take_thrown(js, result);
    GC_ROOT_PIN(js, reject_val);
    ant_value_t reject_args[] = { reject_val };
    sv_vm_call(
      js->vm, js, reject_fn, js_mkundef(), 
      reject_args, 1, NULL, js_mkundef()
    );
  }

  GC_ROOT_RESTORE(js, root_mark);
}

void js_reject_promise(ant_t *js, ant_value_t p, ant_value_t val) {
  if (is_err(val)) {
    GC_ROOT_SAVE(root_mark, js);
    GC_ROOT_PIN(js, p);
    val = Ant_Error_ConsumeMarker(js, val);
    GC_ROOT_RESTORE(js, root_mark);
  }

  ant_promise_state_t *pd = get_promise_data(js, p, false);
  if (!pd || pd->state != 0) return;

  pd->state = 2;
  pd->value = val;
  gc_write_barrier(js, js_obj_ptr(js_as_obj(p)), val);
  pd->unhandled_reported = false;

  if (js->pending_rejections.len >= js->pending_rejections.cap) {
    size_t new_cap = js->pending_rejections.cap ? js->pending_rejections.cap * 2 : 16;
    ant_value_t *ns = realloc(js->pending_rejections.items, new_cap * sizeof(*ns));
    if (ns) { js->pending_rejections.items = ns; js->pending_rejections.cap = new_cap; }
  }
  if (js->pending_rejections.len < js->pending_rejections.cap)
    js->pending_rejections.items[js->pending_rejections.len++] = p;

  queue_promise_trigger(js, p);
}

static ant_value_t builtin_resolve_internal(ant_params_t) {
  ant_value_t resolve_fn = js->current_func;
  if (get_slot(resolve_fn, SLOT_SETTLED) == js_true) return js_mkundef();

  set_slot(resolve_fn, SLOT_SETTLED, js_true);
  ant_value_t p = get_slot(resolve_fn, SLOT_DATA);
  js_resolve_promise(js, p, nargs > 0 ? args[0] : js_mkundef());
  
  return js_mkundef();
}

static ant_value_t builtin_reject_internal(ant_params_t) {
  ant_value_t resolve_fn = get_slot(js->current_func, SLOT_DATA);
  if (get_slot(resolve_fn, SLOT_SETTLED) == js_true) return js_mkundef();

  set_slot(resolve_fn, SLOT_SETTLED, js_true);
  ant_value_t p = get_slot(resolve_fn, SLOT_DATA);
  js_reject_promise(js, p, nargs > 0 ? args[0] : js_mkundef());
  
  return js_mkundef();
}

static ant_value_t builtin_Promise(ant_params_t) {
  if (vtype(call_new_target) == kTypeUndefined) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Promise constructor cannot be invoked without 'new'");
  }
  
  if (nargs == 0 || !is_callable(args[0])) {
    const char *val_str = nargs == 0 ? "undefined" : js_str(js, args[0]);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Promise resolver %s is not a function", val_str);
  }

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t executor = args[0];
  GC_ROOT_PIN(js, executor);

  ant_value_t p = js_mkpromise(js);
  if (is_err(p)) {
    GC_ROOT_RESTORE(js, root_mark);
    return p;
  }
  GC_ROOT_PIN(js, p);

  ant_value_t new_target = call_new_target;
  ant_value_t p_obj = js_as_obj(p);

  ant_value_t promise_proto = js->sym.promise_proto;
  ant_value_t instance_proto = js_instance_proto_from_new_target(js, promise_proto, call_new_target);
  GC_ROOT_PIN(js, instance_proto);

  if (vtype(new_target) == kTypeFunction || vtype(new_target) == kTypeBuiltin) set_slot(p_obj, SLOT_CTOR, new_target);
  if (is_object_type(instance_proto)) js_set_proto_init(p_obj, instance_proto);

  ant_value_t res_fn = js_mkundef();
  ant_value_t rej_fn = js_mkundef();
  ant_value_t pair_result = make_promise_resolving_functions(js, p, &res_fn, &rej_fn);
  
  GC_ROOT_PIN(js, pair_result);
  GC_ROOT_PIN(js, res_fn);
  GC_ROOT_PIN(js, rej_fn);
  
  if (is_err(pair_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return pair_result;
  }

  ant_value_t exec_args[] = { res_fn, rej_fn };
  ant_value_t exec_result = sv_vm_call(js->vm, js, executor, js_mkundef(), exec_args, 2, NULL, js_mkundef());
  
  if (is_err(exec_result) || Ant_Exception_Pending(js)) {
    ant_value_t reject_val = js_take_thrown(js, exec_result);
    ant_value_t reject_args[] = { reject_val };
    sv_vm_call(js->vm, js, rej_fn, js_mkundef(), reject_args, 1, NULL, js_mkundef());
  }

  GC_ROOT_RESTORE(js, root_mark);
  return p;
}

static inline bool is_same_heap_value(ant_value_t a, ant_value_t b) {
  return vtype(a) == vtype(b) && vdata(a) == vdata(b);
}

static inline bool is_constructor_value(ant_value_t value) {
  return vtype(value) == kTypeFunction || vtype(value) == kTypeBuiltin;
}

static ant_value_t builtin_promise_capability_executor(ant_params_t) {
  ant_value_t executor = js->current_func;
  if (get_slot(executor, SLOT_SETTLED) == js_true)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Promise capability executor called twice");

  set_slot(executor, SLOT_SETTLED, js_true);
  js_set_slot_wb(js, executor, SLOT_DATA, nargs > 0 ? args[0] : js_mkundef());
  js_set_slot_wb(js, executor, SLOT_AUX, nargs > 1 ? args[1] : js_mkundef());
  
  return js_mkundef();
}

static ant_value_t new_promise_capability(
  ant_t *js,
  ant_value_t ctor,
  ant_value_t *promise_out,
  ant_value_t *resolve_out,
  ant_value_t *reject_out
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, ctor);

  *promise_out = js_mkundef();
  *resolve_out = js_mkundef();
  *reject_out = js_mkundef();

  ant_value_t executor = make_data_cfunc(
    js, js_mkundef(), 
    builtin_promise_capability_executor
  );
  
  GC_ROOT_PIN(js, executor);
  if (is_err(executor)) {
    GC_ROOT_RESTORE(js, root_mark);
    return executor;
  }
  
  set_slot(executor, SLOT_AUX, js_mkundef());
  set_slot(executor, SLOT_SETTLED, js_false);

  
  ant_value_t ctor_args[] = { executor };
  ant_value_t promise = jit_helper_new(js->vm, js, ctor, ctor, ctor_args, 1);
  if (is_err(promise)) {
    GC_ROOT_RESTORE(js, root_mark);
    return promise;
  }

  ant_value_t resolve = get_slot(executor, SLOT_DATA);
  ant_value_t reject = get_slot(executor, SLOT_AUX);
  
  if (!is_callable(resolve) || !is_callable(reject)) {
    ant_value_t err = js_mkerr_typed(
      js, JS_ERR_TYPE, 
      "Promise constructor did not provide callable resolve and reject functions"
    );
    GC_ROOT_RESTORE(js, root_mark);
    return err;
  }

  *promise_out = promise;
  *resolve_out = resolve;
  *reject_out = reject;
  GC_ROOT_RESTORE(js, root_mark);
  
  return js_mkundef();
}

static ant_value_t promise_call_one(
  ant_t *js, ant_value_t fn, ant_value_t this_val, ant_value_t value
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, fn);
  GC_ROOT_PIN(js, this_val);
  GC_ROOT_PIN(js, value);

  ant_value_t args[] = { value };
  ant_value_t result = sv_vm_call(
    js->vm, js, fn, 
    this_val, args, 1, NULL, js_mkundef()
  );
  
  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static ant_value_t promise_resolve_with_ctor(ant_t *js, ant_value_t ctor, ant_value_t val) {
  if (strict_eq_values(js, ctor, js->sym.promise_ctor)) return js_promise_assimilate_awaitable(js, val);
  if (!js_is_constructor(ctor)) return js_mkerr_typed(js, JS_ERR_TYPE, "Promise.resolve receiver is not a constructor");

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, ctor);
  GC_ROOT_PIN(js, val);

  bool val_is_promise = vtype(val) == kTypePromise;
  ant_value_t val_ctor = val_is_promise
    ? js_getprop_fallback(js, val, "constructor")
    : js_mkundef();
  
  if (is_err(val_ctor)) {
    GC_ROOT_RESTORE(js, root_mark);
    return val_ctor;
  }
  
  if (val_is_promise && strict_eq_values(js, val_ctor, ctor)) {
    GC_ROOT_RESTORE(js, root_mark);
    return val;
  }

  ant_value_t promise = js_mkundef();
  ant_value_t resolve = js_mkundef();
  ant_value_t reject = js_mkundef();

  ant_value_t capability_result = new_promise_capability(
    js, ctor, &promise,
    &resolve, &reject
  );

  if (is_err(capability_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return capability_result;
  }
  
  GC_ROOT_PIN(js, promise);
  GC_ROOT_PIN(js, resolve);

  ant_value_t resolve_args[] = { val };
  ant_value_t resolve_result = sv_vm_call(
    js->vm, js, resolve, js_mkundef(), 
    resolve_args, 1, NULL, js_mkundef()
  );
  
  if (is_err(resolve_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return resolve_result;
  }

  GC_ROOT_RESTORE(js, root_mark);
  return promise;
}

static ant_value_t builtin_Promise_resolve(ant_params_t) {
  ant_value_t val = nargs > 0 ? args[0] : js_mkundef();
  return promise_resolve_with_ctor(js, js->this_val, val);
}

static ant_value_t promise_reject_with_ctor(
  ant_t *js, ant_value_t ctor, ant_value_t val
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, ctor);
  GC_ROOT_PIN(js, val);

  ant_value_t promise = js_mkundef();
  ant_value_t resolve = js_mkundef();
  ant_value_t reject = js_mkundef();
  
  ant_value_t capability_result = new_promise_capability(
    js, ctor, &promise, 
    &resolve, &reject
  );
  
  if (is_err(capability_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return capability_result;
  }

  GC_ROOT_PIN(js, promise);
  GC_ROOT_PIN(js, reject);

  ant_value_t reject_result = promise_call_one(
    js, reject, js_mkundef(), val
  );

  GC_ROOT_RESTORE(js, root_mark);
  return is_err(reject_result) ? reject_result : promise;
}

static ant_value_t builtin_Promise_reject(ant_params_t) {
  ant_value_t val = nargs > 0 ? args[0] : js_mkundef();
  return promise_reject_with_ctor(js, js->this_val, val);
}

static void promise_init_derived_promise(
  ant_t *js,
  ant_value_t next_p,
  ant_value_t parent_p,
  ant_value_t species_ctor,
  ant_value_t promise_ctor
) {
  ant_value_t next_obj = js_as_obj(next_p);

  if (is_constructor_value(species_ctor) && !is_same_heap_value(species_ctor, promise_ctor)) {
    ant_value_t species_proto = js_get(js, species_ctor, "prototype");
    if (is_object_type(species_proto)) js_set_proto_init(next_obj, species_proto);
    set_slot(next_obj, SLOT_CTOR, species_ctor);
    return;
  }

  ant_value_t parent_obj = js_as_obj(parent_p);
  ant_value_t parent_proto = get_slot(parent_obj, SLOT_PROTO);
  if (vtype(parent_proto) == kTypeObject) js_set_proto_init(next_obj, parent_proto);

  ant_value_t parent_ctor = get_slot(parent_obj, SLOT_CTOR);
  if (is_constructor_value(parent_ctor)) set_slot(next_obj, SLOT_CTOR, parent_ctor);
}

static ant_value_t builtin_promise_then(ant_params_t) {
  ant_value_t p = js->this_val;
  if (vtype(p) != kTypePromise) return js_mkerr(js, "not a promise");

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, p);

  ant_value_t promise_ctor = js->sym.promise_ctor;
  GC_ROOT_PIN(js, promise_ctor);
  ant_value_t species_ctor = promise_ctor;
  GC_ROOT_PIN(js, species_ctor);
  ant_value_t p_obj = js_as_obj(p);
  ant_value_t ctor = js_get(js, p_obj, "constructor");
  GC_ROOT_PIN(js, ctor);

  if (is_err(ctor)) {
    GC_ROOT_RESTORE(js, root_mark);
    return ctor;
  }
  if (vtype(ctor) == kTypeUndefined) ctor = get_slot(p_obj, SLOT_CTOR);

  ant_value_t species = get_ctor_species_value(js, ctor);
  GC_ROOT_PIN(js, species);
  if (is_err(species)) {
    GC_ROOT_RESTORE(js, root_mark);
    return species;
  }

  if (vtype(species) == kTypeFunction || vtype(species) == kTypeBuiltin) {
    species_ctor = species;
  } else if (vtype(species) == kTypeNull) {
    species_ctor = promise_ctor;
  } else if (vtype(species) != kTypeUndefined) {
    ant_value_t err = js_mkerr_typed(js, JS_ERR_TYPE, "Promise species is not a constructor");
    GC_ROOT_RESTORE(js, root_mark);
    return err;
  }

  ant_value_t nextP = js_mkpromise(js);
  GC_ROOT_PIN(js, nextP);
  promise_init_derived_promise(js, nextP, p, species_ctor, promise_ctor);

  ant_value_t onFulfilled = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t onRejected = nargs > 1 ? args[1] : js_mkundef();
  
  GC_ROOT_PIN(js, onFulfilled);
  GC_ROOT_PIN(js, onRejected);

  ant_promise_state_t *next_pd = get_promise_data(js, nextP, false);
  if (next_pd) next_pd->trigger_parent = p;

  ant_promise_state_t *pd = get_promise_data(js, p, false);
  if (pd) {
    promise_handler_t h = { onFulfilled, onRejected, nextP, NULL };
    if (!promise_handler_append(pd, &h)) {
      GC_ROOT_RESTORE(js, root_mark);
      return js_mkerr(js, "out of memory");
    }
    
    gc_write_barrier(js, js_obj_ptr(js_as_obj(p)), nextP);
    gc_write_barrier(js, js_obj_ptr(js_as_obj(p)), onFulfilled);
    gc_write_barrier(js, js_obj_ptr(js_as_obj(p)), onRejected);
    
    if (is_callable(onRejected))
      js_mark_promise_rejection_handled_chain(js, p);
      
    if (pd->state == 0)
      gc_root_pending_promise(js, js_obj_ptr(p));
  }

  if (pd && pd->state != 0) queue_promise_trigger(js, p);
  GC_ROOT_RESTORE(js, root_mark);
  
  return nextP;
}

static ant_value_t builtin_promise_catch(ant_params_t) {
  ant_value_t args_then[] = { js_mkundef(), nargs > 0 ? args[0] : js_mkundef() };
  return builtin_promise_then(js, args_then, 2, js_mkundef());
}

static ant_value_t finally_value_thunk(ant_params_t) {
  ant_value_t me = js->current_func;
  return get_slot(me, SLOT_DATA);
}

static ant_value_t finally_thrower(ant_params_t) {
  ant_value_t me = js->current_func;
  ant_value_t reason = get_slot(me, SLOT_DATA);
  ant_value_t rejected = js_mkpromise(js);
  js_reject_promise(js, rejected, reason);
  return rejected;
}

static ant_value_t finally_identity_reject(ant_params_t) {
  ant_value_t reason = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t rejected = js_mkpromise(js);
  js_reject_promise(js, rejected, reason);
  return rejected;
}

static ant_value_t finally_chain_result(
  ant_t *js,
  ant_value_t result,
  ant_value_t completion,
  bool rejected
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, result);
  GC_ROOT_PIN(js, completion);

  ant_value_t promise = js_promise_assimilate_awaitable(js, result);
  GC_ROOT_PIN(js, promise);
  if (is_err(promise)) {
    GC_ROOT_RESTORE(js, root_mark);
    return promise;
  }

  ant_value_t then_fn = js_get_thenable_then(js, promise);
  GC_ROOT_PIN(js, then_fn);
  if (is_err(then_fn)) {
    GC_ROOT_RESTORE(js, root_mark);
    return then_fn;
  }
  if (!is_callable(then_fn)) {
    ant_value_t err = js_mkerr_typed(js, JS_ERR_TYPE, "Promise then is not callable");
    GC_ROOT_RESTORE(js, root_mark);
    return err;
  }

  ant_value_t on_fulfilled = make_data_cfunc(
    js, completion,
    rejected ? finally_thrower : finally_value_thunk
  );
  
  GC_ROOT_PIN(js, on_fulfilled);
  if (is_err(on_fulfilled)) {
    GC_ROOT_RESTORE(js, root_mark);
    return on_fulfilled;
  }

  ant_value_t on_rejected = Ant_Function_Create(finally_identity_reject);
  ant_value_t then_args[] = { on_fulfilled, on_rejected };
  
  ant_value_t chained = sv_vm_call(
    js->vm, js, then_fn, promise, 
    then_args, 2, NULL, js_mkundef()
  );
  
  GC_ROOT_RESTORE(js, root_mark);
  return chained;
}

static ant_value_t finally_fulfilled_wrapper(ant_params_t) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t me = js->current_func;
  ant_value_t callback = get_slot(me, SLOT_DATA);
  ant_value_t value = nargs > 0 ? args[0] : js_mkundef();
  GC_ROOT_PIN(js, callback);
  GC_ROOT_PIN(js, value);

  ant_value_t result = js_mkundef();
  if (is_callable(callback)) {
    result = sv_vm_call(js->vm, js, callback, js_mkundef(), NULL, 0, NULL, js_mkundef());
    if (is_err(result)) {
      GC_ROOT_RESTORE(js, root_mark);
      return result;
    }
  }
  GC_ROOT_PIN(js, result);

  ant_value_t chained = finally_chain_result(js, result, value, false);
  GC_ROOT_RESTORE(js, root_mark);
  return chained;
}

static ant_value_t finally_rejected_wrapper(ant_params_t) {
  GC_ROOT_SAVE(root_mark, js);
  
  ant_value_t me = js->current_func;
  ant_value_t callback = get_slot(me, SLOT_DATA);
  ant_value_t reason = nargs > 0 ? args[0] : js_mkundef();
  
  GC_ROOT_PIN(js, callback);
  GC_ROOT_PIN(js, reason);

  ant_value_t result = js_mkundef();
  if (is_callable(callback)) {
    result = sv_vm_call(js->vm, js, callback, js_mkundef(), NULL, 0, NULL, js_mkundef());
    if (is_err(result)) {
      GC_ROOT_RESTORE(js, root_mark);
      return result;
    }
  }
  GC_ROOT_PIN(js, result);

  ant_value_t chained = finally_chain_result(js, result, reason, true);
  GC_ROOT_RESTORE(js, root_mark);
  return chained;
}

static ant_value_t builtin_promise_finally(ant_params_t) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t callback = nargs > 0 ? args[0] : js_mkundef();
  GC_ROOT_PIN(js, callback);

  ant_value_t fulfilled_fn = make_data_cfunc(js, callback, finally_fulfilled_wrapper);
  GC_ROOT_PIN(js, fulfilled_fn);
  if (is_err(fulfilled_fn)) {
    GC_ROOT_RESTORE(js, root_mark);
    return fulfilled_fn;
  }

  ant_value_t rejected_fn = make_data_cfunc(js, callback, finally_rejected_wrapper);
  GC_ROOT_PIN(js, rejected_fn);
  if (is_err(rejected_fn)) {
    GC_ROOT_RESTORE(js, root_mark);
    return rejected_fn;
  }

  ant_value_t args_then[] = { fulfilled_fn, rejected_fn };
  ant_value_t ret = builtin_promise_then(js, args_then, 2, js_mkundef());
  GC_ROOT_RESTORE(js, root_mark);
  
  return ret;
}

static ant_value_t builtin_Promise_try(ant_params_t) {
  if (nargs == 0) return builtin_Promise_resolve(js, args, 0, js_mkundef());
  
  ant_value_t fn = args[0];
  ant_value_t *call_args = nargs > 1 ? &args[1] : NULL;
  int call_nargs = nargs > 1 ? nargs - 1 : 0;
  ant_value_t res = sv_vm_call(js->vm, js, fn, js_mkundef(), call_args, call_nargs, NULL, js_mkundef());
  
  if (is_err(res)) {
    ant_value_t reject_val = js_take_thrown(js, res);
    ant_value_t rej_args[] = { reject_val };
    return builtin_Promise_reject(js, rej_args, 1, js_mkundef());
  }
  
  ant_value_t res_args[] = { res };
  return builtin_Promise_resolve(js, res_args, 1, js_mkundef());
}

static ant_value_t builtin_Promise_withResolvers(ant_params_t) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t p = js_mkpromise(js);
  GC_ROOT_PIN(js, p);

  ant_value_t res_fn = js_mkundef();
  ant_value_t rej_fn = js_mkundef();
  ant_value_t pair_result = make_promise_resolving_functions(js, p, &res_fn, &rej_fn);
  GC_ROOT_PIN(js, pair_result);
  GC_ROOT_PIN(js, res_fn);
  GC_ROOT_PIN(js, rej_fn);
  if (is_err(pair_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return pair_result;
  }

  ant_value_t result = Ant_Object_Create(js);
  GC_ROOT_PIN(js, result);
  js_setprop(js, result, js_mkstr(js, "promise", 7), p);
  js_setprop(js, result, js_mkstr(js, "resolve", 7), res_fn);
  js_setprop(js, result, js_mkstr(js, "reject", 6), rej_fn);

  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static ant_value_t promise_get_resolve(ant_t *js, ant_value_t ctor) {
  ant_value_t resolve = js_getprop_fallback(js, ctor, "resolve");
  if (is_err(resolve) || is_callable(resolve)) return resolve;
  return js_mkerr_typed(js, JS_ERR_TYPE, "Promise resolve is not callable");
}

static ant_value_t promise_reject_abrupt(
  ant_t *js,
  ant_value_t promise,
  ant_value_t reject,
  ant_value_t abrupt
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, promise);
  GC_ROOT_PIN(js, reject);

  ant_value_t reason = js_take_thrown(js, abrupt);
  GC_ROOT_PIN(js, reason);
  ant_value_t reject_result = promise_call_one(
    js, reject, js_mkundef(), reason
  );
  GC_ROOT_RESTORE(js, root_mark);
  return is_err(reject_result) ? reject_result : promise;
}

static ant_value_t builtin_Promise_all_resolve_handler(ant_params_t) {
  ant_value_t me = js->current_func;
  if (get_slot(me, SLOT_SETTLED) == js_true) return js_mkundef();
  set_slot(me, SLOT_SETTLED, js_true);

  ant_value_t tracker = get_slot(me, SLOT_DATA);
  ant_value_t index_val = get_slot(me, SLOT_AUX);
  
  int index = (int)tod(index_val);
  ant_value_t value = nargs > 0 ? args[0] : js_mkundef();
  
  ant_value_t results = get_slot(tracker, SLOT_ENTRIES);
  arr_set(js, results, (ant_offset_t)index, value);
  
  ant_value_t remaining_val = get_slot(tracker, SLOT_MAP);
  int remaining = (int)tod(remaining_val) - 1;
  set_slot(tracker, SLOT_MAP, tov((double)remaining));
  
  if (remaining == 0) {
    return promise_call_one(
      js,
      get_slot(tracker, SLOT_DATA),
      js_mkundef(),
      mkval(kTypeArray, vdata(results))
    );
  }
  
  return js_mkundef();
}

static ant_value_t builtin_Promise_all_reject_handler(ant_params_t) {
  ant_value_t resolve_fn = get_slot(js->current_func, SLOT_DATA);
  if (get_slot(resolve_fn, SLOT_SETTLED) == js_true) return js_mkundef();
  set_slot(resolve_fn, SLOT_SETTLED, js_true);

  ant_value_t tracker = get_slot(resolve_fn, SLOT_DATA);
  ant_value_t reason = nargs > 0 ? args[0] : js_mkundef();
  return promise_call_one(
    js, get_slot(tracker, SLOT_AUX), js_mkundef(), reason
  );
}

static ant_value_t promise_all_settled_make_result(ant_t *js, bool fulfilled, ant_value_t value) {
  ant_value_t result = mkobj(js, 0);
  if (is_err(result)) return result;
  
  js_setprop(
    js, result,
    js_mkstr(js, "status", 6),
    js_mkstr(js, fulfilled ? "fulfilled" : "rejected", fulfilled ? 9 : 8)
  );
  
  js_setprop(
    js, result,
    js_mkstr(js, fulfilled ? "value" : "reason", fulfilled ? 5 : 6), value
  );
  
  return result;
}

static ant_value_t promise_all_settled_store_result(
  ant_t *js,
  ant_value_t tracker,
  int index,
  bool fulfilled,
  ant_value_t value
) {
  ant_value_t results = get_slot(tracker, SLOT_ENTRIES);
  ant_value_t result = promise_all_settled_make_result(js, fulfilled, value);
  ant_value_t remaining_val = 0;
  int remaining = 0;

  if (is_err(result)) return result;
  arr_set(js, results, (ant_offset_t)index, result);

  remaining_val = get_slot(tracker, SLOT_MAP);
  remaining = (int)tod(remaining_val) - 1;
  set_slot(tracker, SLOT_MAP, tov((double)remaining));

  if (remaining == 0) {
    return promise_call_one(
      js,
      get_slot(tracker, SLOT_DATA),
      js_mkundef(),
      mkval(kTypeArray, vdata(results))
    );
  }

  return js_mkundef();
}

static ant_value_t builtin_Promise_allSettled_resolve_handler(ant_params_t) {
  ant_value_t me = js->current_func;
  if (get_slot(me, SLOT_SETTLED) == js_true) return js_mkundef();
  set_slot(me, SLOT_SETTLED, js_true);

  ant_value_t tracker = get_slot(me, SLOT_DATA);
  ant_value_t index_val = get_slot(me, SLOT_AUX);
  int index = (int)tod(index_val);
  ant_value_t value = nargs > 0 ? args[0] : js_mkundef();

  return promise_all_settled_store_result(js, tracker, index, true, value);
}

static ant_value_t builtin_Promise_allSettled_reject_handler(ant_params_t) {
  ant_value_t resolve_fn = get_slot(js->current_func, SLOT_DATA);
  if (get_slot(resolve_fn, SLOT_SETTLED) == js_true) return js_mkundef();
  set_slot(resolve_fn, SLOT_SETTLED, js_true);

  ant_value_t tracker = get_slot(resolve_fn, SLOT_DATA);
  ant_value_t index_val = get_slot(resolve_fn, SLOT_AUX);
  int index = (int)tod(index_val);
  ant_value_t reason = nargs > 0 ? args[0] : js_mkundef();

  return promise_all_settled_store_result(js, tracker, index, false, reason);
}

typedef struct {
  ant_value_t tracker;
  ant_value_t ctor;
  ant_value_t promise_resolve;
  int index;
} promise_all_iter_ctx_t;

static ant_value_t promise_invoke_then(
  ant_t *js,
  ant_value_t promise,
  ant_value_t on_fulfilled,
  ant_value_t on_rejected
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, promise);
  GC_ROOT_PIN(js, on_fulfilled);
  GC_ROOT_PIN(js, on_rejected);

  ant_value_t then = js_get_thenable_then(js, promise);
  GC_ROOT_PIN(js, then);
  if (is_err(then)) {
    GC_ROOT_RESTORE(js, root_mark);
    return then;
  }
  if (!is_callable(then)) {
    ant_value_t err = js_mkerr_typed(js, JS_ERR_TYPE, "Promise then is not callable");
    GC_ROOT_RESTORE(js, root_mark);
    return err;
  }

  ant_value_t then_args[] = { on_fulfilled, on_rejected };
  ant_value_t result = sv_vm_call(
    js->vm, js, then, promise, 
    then_args, 2, NULL, js_mkundef()
  );
  
  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static iter_action_t promise_all_iter_cb(ant_t *js, ant_value_t value, void *ctx, ant_value_t *out) {
  GC_ROOT_SAVE(root_mark, js);
  promise_all_iter_ctx_t *pctx = (promise_all_iter_ctx_t *)ctx;
  ant_value_t item = promise_call_one(
    js, pctx->promise_resolve, pctx->ctor, value
  );
  GC_ROOT_PIN(js, item);
  if (is_err(item)) {
    *out = item;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }

  ant_value_t resolve_obj = mkobj(js, 0);
  if (is_err(resolve_obj)) {
    *out = resolve_obj;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }
  GC_ROOT_PIN(js, resolve_obj);
  set_slot(resolve_obj, SLOT_CFUNC, Ant_Function_Create(builtin_Promise_all_resolve_handler));
  set_slot(resolve_obj, SLOT_DATA, pctx->tracker);
  set_slot(resolve_obj, SLOT_AUX, tov((double)pctx->index));
  
  ant_value_t resolve_fn = js_obj_to_func(js, resolve_obj);
  GC_ROOT_PIN(js, resolve_fn);
  set_slot(resolve_fn, SLOT_SETTLED, js_false);

  ant_value_t reject_obj = mkobj(js, 0);
  if (is_err(reject_obj)) {
    *out = reject_obj;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }
  
  GC_ROOT_PIN(js, reject_obj);
  set_slot(reject_obj, SLOT_CFUNC, Ant_Function_Create(builtin_Promise_all_reject_handler));
  set_slot(reject_obj, SLOT_DATA, resolve_fn);
  ant_value_t reject_fn = js_obj_to_func(js, reject_obj);
  GC_ROOT_PIN(js, reject_fn);

  int remaining = (int)tod(get_slot(pctx->tracker, SLOT_MAP)) + 1;
  set_slot(pctx->tracker, SLOT_MAP, tov((double)remaining));

  ant_value_t then_result = promise_invoke_then(js, item, resolve_fn, reject_fn);
  if (is_err(then_result)) {
    *out = then_result;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }

  pctx->index++;
  GC_ROOT_RESTORE(js, root_mark);
  return ITER_CONTINUE;
}

static iter_action_t promise_all_settled_iter_cb(ant_t *js, ant_value_t value, void *ctx, ant_value_t *out) {
  GC_ROOT_SAVE(root_mark, js);
  promise_all_iter_ctx_t *pctx = (promise_all_iter_ctx_t *)ctx;
  
  ant_value_t item = promise_call_one(
    js, pctx->promise_resolve, 
    pctx->ctor, value
  );
  
  GC_ROOT_PIN(js, item);
  if (is_err(item)) {
    *out = item;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }

  ant_value_t resolve_obj = mkobj(js, 0);
  if (is_err(resolve_obj)) {
    *out = resolve_obj;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }
  GC_ROOT_PIN(js, resolve_obj);
  set_slot(resolve_obj, SLOT_CFUNC, Ant_Function_Create(builtin_Promise_allSettled_resolve_handler));
  set_slot(resolve_obj, SLOT_DATA, pctx->tracker);
  set_slot(resolve_obj, SLOT_AUX, tov((double)pctx->index));
  
  ant_value_t resolve_fn = js_obj_to_func(js, resolve_obj);
  GC_ROOT_PIN(js, resolve_fn);
  set_slot(resolve_fn, SLOT_SETTLED, js_false);

  ant_value_t reject_obj = mkobj(js, 0);
  if (is_err(reject_obj)) {
    *out = reject_obj;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }
  
  GC_ROOT_PIN(js, reject_obj);
  set_slot(reject_obj, SLOT_CFUNC, Ant_Function_Create(builtin_Promise_allSettled_reject_handler));
  set_slot(reject_obj, SLOT_DATA, resolve_fn);
  ant_value_t reject_fn = js_obj_to_func(js, reject_obj);
  GC_ROOT_PIN(js, reject_fn);

  int remaining = (int)tod(get_slot(pctx->tracker, SLOT_MAP)) + 1;
  set_slot(pctx->tracker, SLOT_MAP, tov((double)remaining));

  ant_value_t then_result = promise_invoke_then(js, item, resolve_fn, reject_fn);
  if (is_err(then_result)) {
    *out = then_result;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }

  pctx->index++;
  GC_ROOT_RESTORE(js, root_mark);
  return ITER_CONTINUE;
}

static ant_value_t builtin_Promise_all(ant_params_t) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t ctor = js->this_val;
  GC_ROOT_PIN(js, ctor);

  ant_value_t result_promise = js_mkundef();
  ant_value_t result_resolve = js_mkundef();
  ant_value_t result_reject = js_mkundef();
  
  ant_value_t capability_result = new_promise_capability(
    js, ctor, &result_promise, 
    &result_resolve, &result_reject
  );
  
  GC_ROOT_PIN(js, result_promise);
  GC_ROOT_PIN(js, result_resolve);
  GC_ROOT_PIN(js, result_reject);
  if (is_err(capability_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return capability_result;
  }

  ant_value_t promise_resolve = promise_get_resolve(js, ctor);
  GC_ROOT_PIN(js, promise_resolve);
  if (is_err(promise_resolve)) {
    ant_value_t result = promise_reject_abrupt(
      js, result_promise, result_reject, promise_resolve
    );
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }

  ant_value_t iterable = nargs > 0 ? args[0] : js_mkundef();
  GC_ROOT_PIN(js, iterable);

  ant_value_t tracker = mkobj(js, 0);
  GC_ROOT_PIN(js, tracker);
  ant_value_t results = mkarr(js);
  GC_ROOT_PIN(js, results);

  js_set_slot_wb(js, tracker, SLOT_DATA, result_resolve);
  js_set_slot_wb(js, tracker, SLOT_AUX, result_reject);
  js_set_slot_wb(js, tracker, SLOT_ENTRIES, results);
  set_slot(tracker, SLOT_MAP, tov(1.0));

  promise_all_iter_ctx_t ctx = {
    .tracker = tracker,
    .ctor = ctor,
    .promise_resolve = promise_resolve,
    .index = 0
  };
  ant_value_t iter_result = iter_foreach(js, iterable, promise_all_iter_cb, &ctx);

  if (is_err(iter_result)) {
    ant_value_t result = promise_reject_abrupt(
      js, result_promise, 
      result_reject, iter_result
    );
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }

  int len = ctx.index;
  ant_offset_t doff = get_dense_buf(results);
  if (doff) {
    if ((ant_offset_t)len > dense_capacity(doff)) doff = dense_grow(js, results, (ant_offset_t)len);
    if (doff) array_len_set(js, results, (ant_offset_t)len);
  }

  int remaining = (int)tod(get_slot(tracker, SLOT_MAP)) - 1;
  set_slot(tracker, SLOT_MAP, tov((double)remaining));
  if (remaining == 0) {
    ant_value_t resolve_result = promise_call_one(
      js, result_resolve, js_mkundef(), 
      mkval(kTypeArray, vdata(results))
    );
    if (is_err(resolve_result)) {
      ant_value_t result = promise_reject_abrupt(
        js, result_promise, 
        result_reject, resolve_result
      );
      GC_ROOT_RESTORE(js, root_mark);
      return result;
    }
  }
  GC_ROOT_RESTORE(js, root_mark);
  return result_promise;
}

static ant_value_t builtin_Promise_allSettled(ant_params_t) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t ctor = js->this_val;
  GC_ROOT_PIN(js, ctor);

  ant_value_t result_promise = js_mkundef();
  ant_value_t result_resolve = js_mkundef();
  ant_value_t result_reject = js_mkundef();
  
  ant_value_t capability_result = new_promise_capability(
    js, ctor, &result_promise, 
    &result_resolve, &result_reject
  );
  
  GC_ROOT_PIN(js, result_promise);
  GC_ROOT_PIN(js, result_resolve);
  GC_ROOT_PIN(js, result_reject);
  
  if (is_err(capability_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return capability_result;
  }

  ant_value_t promise_resolve = promise_get_resolve(js, ctor);
  GC_ROOT_PIN(js, promise_resolve);
  if (is_err(promise_resolve)) {
    ant_value_t result = promise_reject_abrupt(
      js, result_promise, 
      result_reject, promise_resolve
    );
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }

  ant_value_t iterable = nargs > 0 ? args[0] : js_mkundef();
  GC_ROOT_PIN(js, iterable);

  ant_value_t tracker = mkobj(js, 0);
  GC_ROOT_PIN(js, tracker);
  ant_value_t results = mkarr(js);
  GC_ROOT_PIN(js, results);

  js_set_slot_wb(js, tracker, SLOT_DATA, result_resolve);
  js_set_slot_wb(js, tracker, SLOT_AUX, result_reject);
  js_set_slot_wb(js, tracker, SLOT_ENTRIES, results);
  set_slot(tracker, SLOT_MAP, tov(1.0));

  promise_all_iter_ctx_t ctx = {
    .tracker = tracker,
    .ctor = ctor,
    .promise_resolve = promise_resolve,
    .index = 0
  };
  ant_value_t iter_result = iter_foreach(js, iterable, promise_all_settled_iter_cb, &ctx);

  if (is_err(iter_result)) {
    ant_value_t result = promise_reject_abrupt(
      js, result_promise, result_reject, iter_result
    );
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }

  int len = ctx.index;
  ant_offset_t doff = get_dense_buf(results);
  if (doff) {
    if ((ant_offset_t)len > dense_capacity(doff)) doff = dense_grow(js, results, (ant_offset_t)len);
    if (doff) array_len_set(js, results, (ant_offset_t)len);
  }

  int remaining = (int)tod(get_slot(tracker, SLOT_MAP)) - 1;
  set_slot(tracker, SLOT_MAP, tov((double)remaining));
  if (remaining == 0) {
    ant_value_t resolve_result = promise_call_one(
      js, result_resolve, js_mkundef(), 
      mkval(kTypeArray, vdata(results))
    );
    if (is_err(resolve_result)) {
      ant_value_t result = promise_reject_abrupt(
        js, result_promise, 
        result_reject, resolve_result
      );
      GC_ROOT_RESTORE(js, root_mark);
      return result;
    }
  }
  GC_ROOT_RESTORE(js, root_mark);
  return result_promise;
}

typedef struct {
  ant_value_t ctor;
  ant_value_t promise_resolve;
  ant_value_t resolve_fn;
  ant_value_t reject_fn;
} promise_race_iter_ctx_t;

static iter_action_t promise_race_iter_cb(ant_t *js, ant_value_t value, void *ctx, ant_value_t *out) {
  GC_ROOT_SAVE(root_mark, js);
  promise_race_iter_ctx_t *pctx = (promise_race_iter_ctx_t *)ctx;
  ant_value_t item = promise_call_one(
    js, pctx->promise_resolve, pctx->ctor, value
  );
  GC_ROOT_PIN(js, item);
  if (is_err(item)) {
    *out = item;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }

  ant_value_t then_result = promise_invoke_then(
    js, item, pctx->resolve_fn, pctx->reject_fn
  );
  if (is_err(then_result)) {
    *out = then_result;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }

  GC_ROOT_RESTORE(js, root_mark);
  return ITER_CONTINUE;
}

static ant_value_t builtin_Promise_race(ant_params_t) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t ctor = js->this_val;
  GC_ROOT_PIN(js, ctor);

  ant_value_t result_promise = js_mkundef();
  ant_value_t resolve_fn = js_mkundef();
  ant_value_t reject_fn = js_mkundef();
  ant_value_t capability_result = new_promise_capability(
    js, ctor, &result_promise, 
    &resolve_fn, &reject_fn
  );
  
  GC_ROOT_PIN(js, result_promise);
  GC_ROOT_PIN(js, resolve_fn);
  GC_ROOT_PIN(js, reject_fn);
  if (is_err(capability_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return capability_result;
  }

  ant_value_t promise_resolve = promise_get_resolve(js, ctor);
  GC_ROOT_PIN(js, promise_resolve);
  if (is_err(promise_resolve)) {
    ant_value_t result = promise_reject_abrupt(
      js, result_promise, 
      reject_fn, promise_resolve
    );
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }

  ant_value_t iterable = nargs > 0 ? args[0] : js_mkundef();
  GC_ROOT_PIN(js, iterable);

  promise_race_iter_ctx_t ctx = {
    .ctor = ctor,
    .promise_resolve = promise_resolve,
    .resolve_fn = resolve_fn,
    .reject_fn = reject_fn
  };

  ant_value_t iter_result = iter_foreach(js, iterable, promise_race_iter_cb, &ctx);
  if (is_err(iter_result)) {
    ant_value_t result = promise_reject_abrupt(
      js, result_promise, 
      reject_fn, iter_result
    );
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }

  GC_ROOT_RESTORE(js, root_mark);
  return result_promise;
}

static ant_value_t mk_aggregate_error(ant_t *js, ant_value_t errors) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, errors);
  ant_value_t args[] = { errors, js_mkstr(js, "All promises were rejected", 26) };
  ant_prop_loc_t off = lkp(js, js_glob(js), "AggregateError", 14);
  ant_value_t ctor = off.obj ? js_prop_load(off) : js_mkundef();
  GC_ROOT_PIN(js, ctor);
  ant_value_t ret = sv_vm_call(js->vm, js, ctor, js_mkundef(), args, 2, NULL, js_mkundef());
  GC_ROOT_RESTORE(js, root_mark);
  return ret;
}

static ant_value_t promise_any_try_resolve(
  ant_t *js, ant_value_t tracker, ant_value_t value
) {
  if (get_slot(tracker, SLOT_SETTLED) == js_true) return js_mkundef();
  set_slot(tracker, SLOT_SETTLED, js_true);
  return promise_call_one(
    js, get_slot(tracker, SLOT_DATA), js_mkundef(), value
  );
}

static ant_value_t promise_any_record_rejection(
  ant_t *js, ant_value_t tracker, int index, ant_value_t reason
) {
  ant_value_t errors = get_slot(tracker, SLOT_ENTRIES);
  arr_set(js, errors, (ant_offset_t)index, reason);
  
  int remaining = (int)tod(get_slot(tracker, SLOT_MAP)) - 1;
  set_slot(tracker, SLOT_MAP, tov((double)remaining));
  
  if (remaining != 0) return js_mkundef();
  set_slot(tracker, SLOT_SETTLED, js_true);
  return promise_call_one(
    js,
    get_slot(tracker, SLOT_AUX),
    js_mkundef(),
    mk_aggregate_error(js, errors)
  );
}

static ant_value_t builtin_Promise_any_resolve_handler(ant_params_t) {
  ant_value_t resolve_fn = js->current_func;
  if (get_slot(resolve_fn, SLOT_SETTLED) == js_true) return js_mkundef();
  set_slot(resolve_fn, SLOT_SETTLED, js_true);

  ant_value_t tracker = get_slot(resolve_fn, SLOT_DATA);
  return promise_any_try_resolve(
    js, tracker, nargs > 0 ? args[0] : js_mkundef()
  );
}

static ant_value_t builtin_Promise_any_reject_handler(ant_params_t) {
  ant_value_t resolve_fn = get_slot(js->current_func, SLOT_DATA);
  if (get_slot(resolve_fn, SLOT_SETTLED) == js_true) return js_mkundef();
  set_slot(resolve_fn, SLOT_SETTLED, js_true);

  ant_value_t tracker = get_slot(resolve_fn, SLOT_DATA);
  if (get_slot(tracker, SLOT_SETTLED) == js_true) return js_mkundef();
  
  int index = (int)tod(get_slot(resolve_fn, SLOT_AUX));
  return promise_any_record_rejection(
    js, tracker, index, nargs > 0 ? args[0] : js_mkundef()
  );
}

typedef struct {
  ant_value_t tracker;
  ant_value_t ctor;
  ant_value_t promise_resolve;
  int index;
} promise_any_iter_ctx_t;

static iter_action_t promise_any_iter_cb(
  ant_t *js, ant_value_t value, void *ctx, ant_value_t *out
) {
  GC_ROOT_SAVE(root_mark, js);
  promise_any_iter_ctx_t *pctx = (promise_any_iter_ctx_t *)ctx;
  ant_value_t item = promise_call_one(
    js, pctx->promise_resolve,
    pctx->ctor, value
  );

  GC_ROOT_PIN(js, item);
  if (is_err(item)) {
    *out = item;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }

  ant_value_t resolve_obj = mkobj(js, 0);
  if (is_err(resolve_obj)) {
    *out = resolve_obj;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }

  GC_ROOT_PIN(js, resolve_obj);
  set_slot(resolve_obj, SLOT_CFUNC, Ant_Function_Create(builtin_Promise_any_resolve_handler));
  set_slot(resolve_obj, SLOT_SETTLED, js_false);
  set_slot(resolve_obj, SLOT_DATA, pctx->tracker);
  set_slot(resolve_obj, SLOT_AUX, tov((double)pctx->index));

  ant_value_t resolve_fn = js_obj_to_func(js, resolve_obj);
  GC_ROOT_PIN(js, resolve_fn);

  ant_value_t reject_obj = mkobj(js, 0);
  if (is_err(reject_obj)) {
    *out = reject_obj;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }

  GC_ROOT_PIN(js, reject_obj);
  set_slot(reject_obj, SLOT_CFUNC, Ant_Function_Create(builtin_Promise_any_reject_handler));
  set_slot(reject_obj, SLOT_DATA, resolve_fn);

  ant_value_t reject_fn = js_obj_to_func(js, reject_obj);
  GC_ROOT_PIN(js, reject_fn);

  int remaining = (int)tod(get_slot(pctx->tracker, SLOT_MAP)) + 1;
  set_slot(pctx->tracker, SLOT_MAP, tov((double)remaining));

  ant_value_t then_result = promise_invoke_then(
    js, item, resolve_fn, reject_fn
  );
  if (is_err(then_result)) {
    *out = then_result;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }

  pctx->index++;
  GC_ROOT_RESTORE(js, root_mark);

  return ITER_CONTINUE;
}

static ant_value_t builtin_Promise_any(ant_params_t) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t ctor = js->this_val;
  GC_ROOT_PIN(js, ctor);

  ant_value_t result_promise = js_mkundef();
  ant_value_t result_resolve = js_mkundef();
  ant_value_t result_reject = js_mkundef();

  ant_value_t capability_result = new_promise_capability(
    js, ctor, &result_promise,
    &result_resolve, &result_reject
  );

  GC_ROOT_PIN(js, result_promise);
  GC_ROOT_PIN(js, result_resolve);
  GC_ROOT_PIN(js, result_reject);
  if (is_err(capability_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return capability_result;
  }

  ant_value_t promise_resolve = promise_get_resolve(js, ctor);
  GC_ROOT_PIN(js, promise_resolve);
  if (is_err(promise_resolve)) {
    ant_value_t result = promise_reject_abrupt(
      js, result_promise,
      result_reject, promise_resolve
    );
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }

  ant_value_t iterable = nargs > 0 ? args[0] : js_mkundef();
  GC_ROOT_PIN(js, iterable);

  ant_value_t tracker = mkobj(js, 0);
  GC_ROOT_PIN(js, tracker);
  ant_value_t errors = mkarr(js);
  GC_ROOT_PIN(js, errors);

  js_set_slot_wb(js, tracker, SLOT_DATA, result_resolve);
  js_set_slot_wb(js, tracker, SLOT_AUX, result_reject);
  js_set_slot_wb(js, tracker, SLOT_ENTRIES, errors);
  set_slot(tracker, SLOT_MAP, tov(1.0));
  set_slot(tracker, SLOT_SETTLED, js_false);

  promise_any_iter_ctx_t ctx = {
    .tracker = tracker,
    .ctor = ctor,
    .promise_resolve = promise_resolve,
    .index = 0
  };
  ant_value_t iter_result = iter_foreach(
    js, iterable,
    promise_any_iter_cb, &ctx
  );

  if (is_err(iter_result)) {
    ant_value_t result = promise_reject_abrupt(
      js, result_promise,
      result_reject, iter_result
    );
    GC_ROOT_RESTORE(js, root_mark);
    return result;
  }

  int len = ctx.index;
  ant_offset_t doff = get_dense_buf(errors);
  if (doff) {
    if ((ant_offset_t)len > dense_capacity(doff)) doff = dense_grow(js, errors, (ant_offset_t)len);
    if (doff) array_len_set(js, errors, (ant_offset_t)len);
  }

  int remaining = (int)tod(get_slot(tracker, SLOT_MAP)) - 1;
  set_slot(tracker, SLOT_MAP, tov((double)remaining));

  if (remaining == 0) {
    set_slot(tracker, SLOT_SETTLED, js_true);
    ant_value_t reject_result = promise_call_one(
      js, result_reject, js_mkundef(),
      mk_aggregate_error(js, errors)
    );

    if (is_err(reject_result)) {
      GC_ROOT_RESTORE(js, root_mark);
      return reject_result;
    }
  }

  GC_ROOT_RESTORE(js, root_mark);
  return result_promise;
}

bool js_chain_may_override_has_instance(ant_t *js, ant_value_t obj, bool guard_absence) {
  ant_offset_t sym_off = (ant_offset_t)vdata(js->sym.hasInstance_sym);
  ant_object_t *function_proto = js_obj_ptr(js->sym.function_proto);
  
  for (int depth = 0; depth < MAX_PROTO_CHAIN_DEPTH && is_object_type(obj); depth++) {
    ant_value_t o = js_as_obj(obj);
    ant_object_t *ptr = js_obj_ptr(o);
    if (ptr && ptr == function_proto) return false;
    if (!ptr || !ptr->shape || is_proxy(o) || ant_shape_lookup_symbol(ptr->shape, sym_off) >= 0) return true;
    if (ptr->flags.is_exotic && lookup_sym_descriptor(js, o, sym_off)) return true;
    if (guard_absence) ant_object_guard_absence(ptr);
    obj = ptr->proto;
  }
  
  return is_object_type(obj);
}

static bool instanceof_via_has_instance(ant_t *js, ant_value_t l, ant_value_t r, ant_value_t *out) {
  ant_value_t has_instance = js_get_sym(js, r, js->sym.hasInstance_sym);
  uint8_t hit = vtype(has_instance);
  
  if (is_err(has_instance)) *out = has_instance;
  else if (hit == kTypeFunction || hit == kTypeBuiltin) {
    ant_value_t args[1] = { l };
    ant_value_t result = sv_vm_call(js->vm, js, has_instance, r, args, 1, NULL, js_mkundef());
    *out = is_err(result) ? result : js_bool(js_truthy(js, result));
  } else if (hit != kTypeUndefined && hit != kTypeNull)
    *out = js_mkerr_typed(js, JS_ERR_TYPE, "Symbol.hasInstance is not callable");
  else return false;
  
  return true;
}

static ant_value_t proxy_ordinary_has_instance(ant_t *js, ant_value_t l, ant_value_t r, uint8_t ltype) {
  if (ltype != kTypeObject && ltype != kTypeArray && ltype != kTypeFunction && ltype != kTypePromise && ltype != kTypeGenerator)
    return js_false;
  
  ant_value_t proto_val = proxy_get(js, r, "prototype", 9);
  if (is_err(proto_val)) return proto_val;
  
  uint8_t pt = vtype(proto_val);
  if (pt != kTypeObject && pt != kTypeArray && pt != kTypeFunction)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Function has non-object prototype in instanceof check");
  
  ant_value_t current = get_proto(js, l);
  return mkval(kTypeBool, proto_chain_contains_cycle_safe(js, current, proto_val) ? 1 : 0);
}

static ant_value_t handle_proxy_instanceof(ant_t *js, ant_value_t l, ant_value_t r, uint8_t ltype) {
  ant_value_t handled;
  if (instanceof_via_has_instance(js, l, r, &handled)) return handled;
  
  uint8_t ttype = vtype(proxy_read_target(js, r));
  if (ttype != kTypeFunction && ttype != kTypeBuiltin)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Right-hand side of 'instanceof' is not callable");
  return proxy_ordinary_has_instance(js, l, r, ltype);
}

static ant_value_t handle_cfunc_instanceof(ant_t *js, ant_value_t l, ant_value_t r, uint8_t ltype) {
  ant_cfunc_t fn = js_as_cfunc(r);
    
  if (fn == builtin_Array) {
    bool result = false;
    ant_value_t is_array_res = js_is_array_value_checked(js, l, &result);
    if (is_err(is_array_res)) return is_array_res;
    return mkval(kTypeBool, result ? 1 : 0);
  }
  
  if (fn == builtin_Object)
    return mkval(kTypeBool, ltype == kTypeObject ? 1 : 0);
  
  if (fn == builtin_Function)
    return mkval(kTypeBool, (ltype == kTypeFunction || ltype == kTypeBuiltin) ? 1 : 0);
  
  if (fn == builtin_Promise)
    return mkval(kTypeBool, ltype == kTypePromise ? 1 : 0);
  
  return mkval(kTypeBool, 0);
}

static bool proto_chain_contains_cycle_safe(ant_t *js, ant_value_t start, ant_value_t target) {
  if (!is_object_type(start) || !is_object_type(target)) return false;

  bool found = false;
  ant_value_t slow = start;
  ant_value_t fast = start;

  while (is_object_type(slow)) {
    if (same_object_identity(slow, target)) {
      found = true;
      break;
    }
    slow = get_proto(js, slow);

    if (is_object_type(fast)) fast = get_proto(js, fast);
    if (is_object_type(fast)) fast = get_proto(js, fast);

    if (
      is_object_type(slow) &&
      is_object_type(fast) &&
      same_object_identity(slow, fast)
    ) break;
  }

  return found;
}

bool js_instance_of_proto(ant_t *js, ant_value_t obj, ant_value_t ctor_proto) {
  return proto_chain_contains_cycle_safe(js, get_proto(js, obj), ctor_proto);
}

static ant_value_t walk_prototype_chain(ant_t *js, ant_value_t l, ant_value_t ctor_proto) {
  ant_value_t current = is_proxy(l) ? proxy_get_prototype_of(js, l) : get_proto(js, l);
  if (is_err(current)) return current;
  return mkval(kTypeBool, proto_chain_contains_cycle_safe(js, current, ctor_proto) ? 1 : 0);
}

static ant_value_t ordinary_has_instance(ant_t *js, ant_value_t l, ant_value_t r) {
  if (vtype(l) == kTypeBuiltin) l = js_cfunc_promote(js, l);
  if (is_err(l)) return l;
  uint8_t ltype = vtype(l);
  if (is_proxy(r)) {
    uint8_t ttype = vtype(proxy_read_target(js, r));
    return ttype == kTypeFunction || ttype == kTypeBuiltin ? proxy_ordinary_has_instance(js, l, r, ltype) : js_false;
  }
  if (vtype(r) == kTypeBuiltin) return handle_cfunc_instanceof(js, l, r, ltype);
  if (vtype(r) != kTypeFunction) return js_false;

  ant_value_t bound_target = js_resolve_bound_target(r);
  if (bound_target != r) return do_instanceof(js, l, bound_target);

  if (ltype != kTypeObject && ltype != kTypeArray && ltype != kTypeFunction && ltype != kTypePromise && ltype != kTypeGenerator)
    return js_false;
  
  ant_prop_loc_t proto_off = lkp_interned(js_func_obj(r), js->intern.prototype);
  ant_value_t ctor_proto = proto_off.obj ? js_prop_load(proto_off) : js_get(js, r, "prototype");
  if (is_err(ctor_proto)) return ctor_proto;
  
  uint8_t pt = vtype(ctor_proto);
  if (pt != kTypeObject && pt != kTypeArray && pt != kTypeFunction)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Function has non-object prototype in instanceof check");
  
  return walk_prototype_chain(js, l, ctor_proto);
}

static ant_value_t builtin_function_has_instance(ant_params_t) {
  return ordinary_has_instance(js, nargs > 0 ? args[0] : js_mkundef(), js->this_val);
}

void js_init_function_has_instance(ant_t *js) {
  static const ant_cfunc_meta_t meta = { builtin_function_has_instance, "[Symbol.hasInstance]", 1, 0 };
  mkprop_exact_attrs(js, js->sym.function_proto, js->sym.hasInstance_sym, js_mkfun_meta(&meta), 0);
}

ant_value_t do_instanceof(ant_t *js, ant_value_t l, ant_value_t r) {
  uint8_t ltype = vtype(l);
  uint8_t rtype = vtype(r);
  
  if (rtype != kTypeFunction && rtype != kTypeBuiltin) {
    if (is_proxy(r)) return handle_proxy_instanceof(js, l, r, ltype);
    ant_value_t handled;
    if (is_object_type(r) && instanceof_via_has_instance(js, l, r, &handled)) return handled;
    return js_mkerr_typed(js, JS_ERR_TYPE, "Right-hand side of 'instanceof' is not callable");
  }
  
  if (rtype == kTypeBuiltin) {
    return handle_cfunc_instanceof(js, l, r, ltype);
  }

  ant_value_t func_obj = js_func_obj(r);
  bool use_slow_has_instance = js_chain_may_override_has_instance(js, func_obj, false);

  ant_value_t handled;
  if (use_slow_has_instance && instanceof_via_has_instance(js, l, r, &handled)) return handled;
  return ordinary_has_instance(js, l, r);
}


static inline bool try_array_key_membership(
  ant_t *js,
  ant_value_t array,
  const char *name,
  ant_offset_t name_len,
  ant_value_t *result
) {
  if (is_length_key(name, name_len) && real_array_obj_ptr(array)) {
    *result = js_true;
    return true;
  }

  unsigned long index;
  ant_offset_t array_len = get_array_length(js, array);
  if (!parse_array_index(name, name_len, array_len, &index)) return false;

  if (!arr_has(js, array, (ant_offset_t)index)) return false;
  *result = js_true;
  
  return true;
}

static inline bool array_index_in(ant_t *js, ant_value_t arr, double d, ant_value_t *out) {
  ant_object_t *dense = array_dense_source(js, arr);
  
  if (!dense || ant_shape_may_have_index_keys(dense->shape)) return false;
  if (!(d >= 0 && d < 4294967295.0) || d != (double)(uint32_t)d) return false;
  
  uint32_t idx = (uint32_t)d;
  *out = js_bool(
    idx < dense_iterable_length(js, arr) && idx < get_array_length(js, arr) &&
    !is_empty_slot(dense_get(get_dense_buf(arr), idx))
  );
  
  return true;
}

static ant_value_t in_lookup(ant_t *js, ant_value_t r, property_key_view_t *key) {
  if (!is_object_type(r)) return js_mkerr_typed(
    js, JS_ERR_TYPE, 
    "Cannot use 'in' operator to search for '%.*s' in non-object", 
    (int)key->length, key->bytes
  );

  ant_value_t array_result;
  if (
    !key->is_symbol && vtype(r) == kTypeArray && 
    try_array_key_membership(js, r, key->bytes, (ant_offset_t)key->length, &array_result)
  ) return array_result;
  
  ant_value_t cur = r;
  uint8_t cur_t = vtype(cur);
  
  proto_walk_overflow_guard_t guard;
  proto_walk_overflow_guard_init(&guard);

  do {
    ant_object_t *ptr = js_obj_ptr(js_as_obj(cur));
    if (ptr->flags.is_exotic && is_proxy(js_as_obj(cur))) {
      ant_value_t result = key->is_symbol
        ? proxy_has_val(js, js_as_obj(cur), key->js_key)
        : proxy_has(js, js_as_obj(cur), key->bytes, (ant_offset_t)key->length);
      return is_err(result) ? result : js_bool(js_truthy(js, result));
    }
    
    ant_value_t exotic = key->is_symbol ? js_mkundef() : exotic_own_get(js, js_as_obj(cur), ptr, key->bytes, key->length);
    if (vtype(exotic) != kTypeUndefined) return js_bool(exotic != T_EMPTY);
    
    ant_value_t own = object_has_own_key(js, js_object_view(cur), key);
    if (own != js_false) return own;
  } while (
    proto_walk_next(js, &cur, &cur_t, PROTO_WALK_F_LOOKUP) &&
    !proto_walk_overflow_guard_hit_cycle(js, &guard, cur, cur_t, PROTO_WALK_F_LOOKUP)
  );

  return js_false;
}

ant_value_t js_in_str(ant_t *js, ant_value_t r, const char *key, size_t key_len) {
  if (vtype(r) == kTypeBuiltin) r = js_cfunc_promote(js, r);
  if (is_err(r)) return r;
  
  property_key_view_t view = { .js_key = js_mkundef(), .bytes = key, .length = key_len };
  return in_lookup(js, r, &view);
}

ant_value_t do_in(ant_t *js, ant_value_t l, ant_value_t r) {
  ant_offset_t prop_len;
  const char *prop_name;
  
  char num_buf[32];
  ant_value_t indexed;
  
  if (
    vtype(l) == kTypeNumber && vtype(r) == kTypeArray && 
    array_index_in(js, r, tod(l), &indexed)
  ) return indexed;

  if (vtype(r) == kTypeBuiltin) r = js_cfunc_promote(js, r);
  if (is_err(r)) return r;

  ant_value_t key = vtype(l) == kTypeString ? l : js_to_primitive(js, l, 1);
  if (is_err(key)) return key;
  bool is_sym = (vtype(key) == kTypeSymbol);
  
  if (vtype(key) == kTypeString) {
    ant_offset_t prop_off = vstr(js, key, &prop_len);
    prop_name = (char *)(uintptr_t)(prop_off);
  } else if (is_sym) {
    const char *d = js_sym_desc(key);
    prop_name = d ? d : "symbol";
    prop_len = (ant_offset_t)strlen(prop_name);
  } else if (vtype(key) == kTypeNumber) {
    prop_len = (ant_offset_t)js_number_to_chars(key, num_buf, sizeof(num_buf));
    prop_name = num_buf;
  } else {
    ant_value_t key_str = js_tostring_val(js, key);
    if (is_err(key_str)) return key_str;
    ant_offset_t prop_off = vstr(js, key_str, &prop_len);
    prop_name = (char *)(uintptr_t)(prop_off);
  }

  property_key_view_t view = { 
    .js_key = key, 
    .bytes = prop_name, 
    .length = prop_len, 
    .is_symbol = is_sym,
  };
  
  return in_lookup(js, r, &view);
}

static ant_value_t builtin_import_tla_resolve(ant_params_t) {
  ant_value_t me = js->current_func;
  return get_slot(me, SLOT_DATA);
}

static inline bool js_has_module_filename(const char *filename) {
  return filename && filename[0];
}

static ant_value_t js_get_import_func(ant_t *js) {
  ant_value_t glob = js_glob(js);
  ant_prop_loc_t import_off = lkp(js, glob, "import", 6);
  if (!import_off.obj) return js_mkundef();
  return js_prop_load(import_off);
}

static ant_value_t js_get_module_ctx_import_meta(ant_t *js, ant_value_t module_ctx) {
  if (!is_object_type(module_ctx)) return js_mkundef();
  return js_get(js, module_ctx, "meta");
}

static const char *js_get_module_ctx_filename(ant_t *js, ant_value_t module_ctx) {
  if (!is_object_type(module_ctx)) return NULL;

  ant_value_t filename = js_get(js, module_ctx, "filename");
  if (vtype(filename) != kTypeString) return NULL;
  return js_getstr(js, filename, NULL);
}

static ant_value_t js_module_ctx_from_func(ant_value_t func) {
  if (vtype(func) != kTypeFunction) return js_mkundef();
  ant_value_t module_ctx = get_slot(js_func_obj(func), SLOT_MODULE_CTX);
  return is_object_type(module_ctx) ? module_ctx : js_mkundef();
}

static ant_value_t js_get_execution_module_ctx(ant_t *js) {
  sv_vm_t *vm = js->vm;
  if (vm && vm->fp >= 0) {
    ant_value_t module_ctx = js_module_ctx_from_func(vm->frames[vm->fp].callee);
    if (is_object_type(module_ctx)) return module_ctx;
  }

  return js_module_eval_active_ctx(js);
}

static ant_value_t js_get_import_owner_module_ctx(ant_t *js) {
  ant_value_t module_ctx = js_module_ctx_from_func(js_getcurrentfunc(js));
  if (is_object_type(module_ctx)) return module_ctx;
  return js_get_execution_module_ctx(js);
}

static const char *js_get_execution_module_filename(ant_t *js) {
  const char *filename = js_get_module_ctx_filename(js, js_get_execution_module_ctx(js));
  if (js_has_module_filename(filename)) return filename;
  return js->filename;
}

ant_value_t js_builtin_import(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "import() requires a string specifier");

  ant_value_t module_ctx = js_get_import_owner_module_ctx(js);
  const char *base_path = js_get_module_ctx_filename(js, module_ctx);

  if (!js_has_module_filename(base_path))
    base_path = js_get_execution_module_filename(js);

  ant_value_t attrs = js_mkundef();
  if (nargs >= 2 && vtype(args[1]) != kTypeUndefined) {
    if (!is_object_type(args[1])) {
      ant_value_t err = js_take_thrown(js, js_mkerr_typed(js, JS_ERR_TYPE, "The second argument to import() must be an object"));
      return promise_reject_with_ctor(js, js->sym.promise_ctor, err);
    }

    attrs = js_get(js, args[1], "with");
    if (is_err(attrs)) {
      ant_value_t reject_val = js_take_thrown(js, attrs);
      return promise_reject_with_ctor(js, js->sym.promise_ctor, reject_val);
    }

    if (vtype(attrs) != kTypeUndefined && !is_object_type(attrs)) {
      ant_value_t err = js_take_thrown(js, js_mkerr_typed(js, JS_ERR_TYPE, "The 'with' option must be an object"));
      return promise_reject_with_ctor(js, js->sym.promise_ctor, err);
    }

    if (is_object_type(attrs)) {
      GC_ROOT_SAVE(root_mark, js);
      GC_ROOT_PIN(js, attrs);
      ant_value_t keys = is_proxy(attrs)
        ? proxy_enum(js, attrs, OBJ_ENUM_KEYS)
        : js_own_property_keys(js, attrs, false, true);
      GC_ROOT_PIN(js, keys);
      if (is_err(keys)) {
        ant_value_t reason = js_take_thrown(js, keys);
        GC_ROOT_RESTORE(js, root_mark);
        return promise_reject_with_ctor(js, js->sym.promise_ctor, reason);
      }

      ant_value_t key = js_mkundef();
      GC_ROOT_PIN(js, key);

      ant_offset_t key_count = js_arr_len(js, keys);
      for (ant_offset_t i = 0; i < key_count; i++) {
        key = js_arr_get(js, keys, i);
        if (vtype(key) != kTypeString) continue;

        ant_value_t val = js_get(js, attrs, js_getstr(js, key, NULL));
        if (is_err(val)) {
          ant_value_t reject_val = js_take_thrown(js, val);
          GC_ROOT_RESTORE(js, root_mark);
          return promise_reject_with_ctor(js, js->sym.promise_ctor, reject_val);
        }
        if (vtype(val) != kTypeString) {
          ant_value_t err = js_take_thrown(js, js_mkerr_typed(js, JS_ERR_TYPE, "Import attribute values must be strings"));
          GC_ROOT_RESTORE(js, root_mark);
          return promise_reject_with_ctor(js, js->sym.promise_ctor, err);
        }
      }
      GC_ROOT_RESTORE(js, root_mark);
    }
  }

  ant_value_t tla_promise = js_mkundef();
  ant_value_t ns = js_esm_import_dynamic_ex(js, args[0], base_path, attrs, &tla_promise);

  if (is_err(ns)) {
    ant_value_t reject_val = js_take_thrown(js, ns);
    return promise_reject_with_ctor(js, js->sym.promise_ctor, reject_val);
  }

  if (vtype(tla_promise) == kTypePromise) {
    ant_value_t resolve_fn = make_data_cfunc(js, ns, builtin_import_tla_resolve);
    ant_value_t then_args[] = { resolve_fn };
    return sv_call_native_this(js, builtin_promise_then, tla_promise, then_args, 1);
  }

  return js_promise_assimilate_awaitable(js, ns);
}

static void js_set_import_module_ctx(ant_t *js, ant_value_t module_ctx) {
  if (!is_object_type(module_ctx)) return;
  ant_value_t import_fn = js_get_import_func(js);
  if (vtype(import_fn) != kTypeFunction) return;
  js_set_slot_wb(js, js_func_obj(import_fn), SLOT_MODULE_CTX, module_ctx);
}

static ant_value_t js_get_current_import_meta(ant_t *js) {
  ant_value_t import_meta = js_get_module_ctx_import_meta(js, js_get_execution_module_ctx(js));
  if (vtype(import_meta) == kTypeObject) return import_meta;
  return js->modules.import_meta;
}

static bool js_try_import_meta(ant_t *js, ant_value_t func_obj, ant_value_t *out) {
  ant_value_t cfunc = js_get_slot(func_obj, SLOT_CFUNC);
  if (vtype(cfunc) != kTypeBuiltin) return false;
  if (!js_cfunc_same_entrypoint(cfunc, js_builtin_import)) return false;

  ant_value_t meta = js_get_module_ctx_import_meta(js, js_get_slot(func_obj, SLOT_MODULE_CTX));
  if (vtype(meta) == kTypeUndefined) meta = js_get_current_import_meta(js);
  if (vtype(meta) == kTypeUndefined) return false;

  *out = meta;
  return true;
}

static ant_value_t builtin_import_meta_resolve(ant_params_t) {
  if (nargs < 1) return js_mkerr(js, "import.meta.resolve() requires a string specifier");

  const char *base_path = NULL;
  ant_value_t import_meta = js_getthis(js);
  ant_value_t module_ctx = js_mkundef();

  if (is_object_type(import_meta)) module_ctx = js_get_slot(import_meta, SLOT_MODULE_CTX);

  if (!is_object_type(module_ctx))
    module_ctx = js_get_import_owner_module_ctx(js);

  base_path = js_get_module_ctx_filename(js, module_ctx);
  if (!js_has_module_filename(base_path))
    base_path = js_get_execution_module_filename(js);
  
  return js_esm_resolve_specifier(js, args[0], base_path);
}

static inline void js_set_import_meta_special_dirname(
  ant_t *js,
  ant_value_t import_meta,
  const char *filename,
  bool is_builtin
) {
  char *filename_copy = strdup(filename);
  if (!filename_copy) return;

  char *last_slash = strrchr(filename_copy, '/');
  char *scheme_end = strstr(filename_copy, "://");

  if ((is_builtin && last_slash) || (!is_builtin && last_slash && scheme_end && last_slash > scheme_end + 2)) {
    *last_slash = '\0';
    ant_value_t dirname_val = js_mkstr(js, filename_copy, strlen(filename_copy));
    if (!is_err(dirname_val)) setprop_cstr(js, import_meta, "dirname", 7, dirname_val);
  }

  free(filename_copy);
}

static inline void js_set_import_meta_path_dirname(
  ant_t *js,
  ant_value_t import_meta,
  const char *filename
) {
  char *filename_copy = strdup(filename);
  if (!filename_copy) return;

  char *dir = dirname(filename_copy);
  if (dir) {
    ant_value_t dirname_val = js_mkstr(js, dir, strlen(dir));
    if (!is_err(dirname_val)) setprop_cstr(js, import_meta, "dirname", 7, dirname_val);
  }

  free(filename_copy);
}

static ant_value_t js_create_import_meta_for_context(
  ant_t *js,
  ant_value_t module_ctx,
  const char *filename,
  bool is_main
) {
  if (!filename) return js_mkundef();

  ant_value_t import_meta = mkobj(js, 0);
  if (is_err(import_meta)) return import_meta;
  js_set_slot_wb(js, import_meta, SLOT_MODULE_CTX, module_ctx);

  bool is_url = esm_is_url(filename) || esm_is_data_url(filename);
  bool is_builtin = esm_has_builtin_scheme(filename);

  ant_value_t url_val = (is_url || is_builtin)
    ? js_mkstr(js, filename, strlen(filename))
    : js_esm_make_file_url(js, filename);
  if (!is_err(url_val)) setprop_cstr(js, import_meta, "url", 3, url_val);

  ant_value_t filename_val = js_get(js, module_ctx, "filename");
  if (vtype(filename_val) == kTypeString) {
    setprop_cstr(js, import_meta, "filename", 8, filename_val);
    setprop_cstr(js, import_meta, "path", 4, filename_val);
  }

  if (is_url || is_builtin) js_set_import_meta_special_dirname(js, import_meta, filename, is_builtin);
  else js_set_import_meta_path_dirname(js, import_meta, filename);

  ant_value_t dirname_val = js_get(js, import_meta, "dirname");
  if (vtype(dirname_val) == kTypeString)
    setprop_cstr(js, import_meta, "dir", 3, dirname_val);

  char *filename_copy = strdup(filename);
  if (filename_copy) {
    char *file = (is_url || is_builtin) ? strrchr(filename_copy, '/') : NULL;
    file = (is_url || is_builtin) ? (file ? file + 1 : filename_copy) : basename(filename_copy);
    if (file) {
      ant_value_t file_val = js_mkstr(js, file, strlen(file));
      if (!is_err(file_val)) setprop_cstr(js, import_meta, "file", 4, file_val);
    }
    free(filename_copy);
  }

  setprop_cstr(js, import_meta, "env", 3, js->builtins.process_env);
  setprop_cstr(js, import_meta, "main", 4, is_main ? js_true : js_false);

  ant_value_t resolve_fn = js_heavy_mkfun(js, builtin_import_meta_resolve, js_mkundef());
  if (vtype(resolve_fn) == kTypeFunction)
    js_set_slot_wb(js, js_func_obj(resolve_fn), SLOT_MODULE_CTX, module_ctx);
  setprop_cstr(js, import_meta, "resolve", 7, resolve_fn);

  return import_meta;
}

ant_value_t js_create_module_context(ant_t *js, const char *filename, bool is_main) {
  GC_ROOT_SAVE(root_mark, js);
  if (!js_has_module_filename(filename)) {
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkundef();
  }

  ant_value_t module_ctx = Ant_Object_CreateBare(js);
  if (is_err(module_ctx)) {
    GC_ROOT_RESTORE(js, root_mark);
    return module_ctx;
  }
  GC_ROOT_PIN(js, module_ctx);

  ant_value_t filename_val = js_mkstr(js, filename, strlen(filename));
  if (is_err(filename_val)) {
    GC_ROOT_RESTORE(js, root_mark);
    return filename_val;
  }
  
  GC_ROOT_PIN(js, filename_val);
  setprop_cstr(js, module_ctx, "filename", 8, filename_val);
  setprop_cstr(js, module_ctx, "displayName", 11, filename_val);

  ant_value_t import_meta = js_create_import_meta_for_context(js, module_ctx, filename, is_main);
  if (is_err(import_meta)) {
    GC_ROOT_RESTORE(js, root_mark);
    return import_meta;
  }
  
  GC_ROOT_PIN(js, import_meta);
  setprop_cstr(js, module_ctx, "meta", 4, import_meta);
  GC_ROOT_RESTORE(js, root_mark);
  
  return module_ctx;
}

ant_value_t js_create_import_meta(ant_t *js, const char *filename, bool is_main) {
  ant_value_t module_ctx = js_create_module_context(js, filename, is_main);
  if (is_err(module_ctx)) return module_ctx;
  return js_get_module_ctx_import_meta(js, module_ctx);
}

ant_value_t js_get_module_import_binding(ant_t *js) {
  GC_ROOT_SAVE(root_mark, js);
  
  ant_value_t module_ctx = js_get_execution_module_ctx(js);
  ant_value_t import_meta = js_get_module_ctx_import_meta(js, module_ctx);

  GC_ROOT_PIN(js, module_ctx);
  GC_ROOT_PIN(js, import_meta);

  if (!is_object_type(module_ctx) || vtype(import_meta) != kTypeObject) {
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkundef();
  }

  ant_value_t import_obj = Ant_Object_CreateBare(js);
  if (is_err(import_obj)) {
    GC_ROOT_RESTORE(js, root_mark);
    return import_obj;
  }

  GC_ROOT_PIN(js, import_obj);
  ant_value_t function_proto = js->sym.function_proto;
  GC_ROOT_PIN(js, function_proto);

  if (is_object_type(function_proto)) js_set_proto_wb(js, import_obj, function_proto);
  set_slot(import_obj, SLOT_CFUNC, Ant_Function_Create(js_builtin_import));
  
  js_set_slot_wb(js, import_obj, SLOT_MODULE_CTX, module_ctx);
  setprop_cstr(js, import_obj, "meta", 4, import_meta);

  ant_value_t import_fn = js_obj_to_func(js, import_obj);
  GC_ROOT_RESTORE(js, root_mark);
  
  return import_fn;
}

void js_setup_import_meta(ant_t *js, const char *filename) {
  if (!filename) return;

  ant_value_t module_ctx = js_create_module_context(js, filename, true);
  ant_value_t import_meta = js_get_module_ctx_import_meta(js, module_ctx);
  if (is_err(import_meta) || vtype(import_meta) == kTypeUndefined) return;

  js_set_import_module_ctx(js, module_ctx);
  js->modules.import_meta = import_meta;
}

void js_module_eval_ctx_push(ant_t *js, ant_module_t *ctx) {
  if (!js || !ctx) return;

  ctx->prev = js->modules.module_stack;
  ctx->prev_import_meta_prop = js->modules.import_meta;
  js->modules.module_stack = ctx;

  ant_value_t import_meta = js_get_module_ctx_import_meta(js, ctx->module_ctx);
  if (vtype(import_meta) != kTypeUndefined) js->modules.import_meta = import_meta;
}

void js_module_eval_ctx_pop(ant_t *js, ant_module_t *ctx) {
  if (!js || !ctx) return;

  if (js->modules.module_stack == ctx) {
    js->modules.import_meta = ctx->prev_import_meta_prop;
    js->modules.module_stack = ctx->prev;
  }
}

static ant_proxy_state_t *get_proxy_data(ant_value_t obj) {
  if (vtype(obj) != kTypeObject) return NULL;
  ant_object_t *ptr = js_obj_ptr(obj);
  return ptr ? ant_object_proxy_state(ptr) : NULL;
}

bool is_proxy(ant_value_t obj) {
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || !ptr->flags.is_exotic) return false;
  return get_proxy_data(obj) != NULL;
}

bool js_proxy_is_host(ant_value_t obj) {
  ant_proxy_state_t *data = get_proxy_data(obj);
  return data && data->host;
}

static ant_value_t proxy_host_load(ant_t *js, ant_value_t target, ant_prop_loc_t loc, const char *key, size_t key_len) {
  const ant_shape_prop_t *meta = prop_shape_meta(loc);
  if (meta && meta->has_getter) return js_getprop_fallback_len(js, target, key, key_len);
  return js_prop_load(loc);
}

static ant_value_t proxy_host_get(ant_t *js, ant_proxy_state_t *data, const char *key, size_t key_len) {
  ant_value_t target = data->target;
  ant_prop_loc_t loc = lkp(js, target, key, key_len);
  if (loc.obj) return proxy_host_load(js, target, loc, key, key_len);
  
  ant_value_t value = data->host->get(js, target, key, key_len);
  if (vtype(value) != kTypeUndefined) return value;
  
  ant_value_t proto = js_get_proto(js, target);
  if (!is_object_type(proto)) return js_mkundef();
  
  ant_prop_loc_t proto_loc = lkp_proto(js, proto, key, key_len);
  return proto_loc.obj ? proxy_host_load(js, target, proto_loc, key, key_len) : js_mkundef();
}

bool js_try_host_proxy_get(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t *out) {
  ant_proxy_state_t *data = get_proxy_data(obj);
  if (!data || !data->host || data->revoked) return false;
  *out = proxy_host_get(js, data, key, key_len);
  return true;
}

static ant_value_t proxy_host_desc(ant_t *js, ant_proxy_state_t *data, ant_value_t key_val) {
  size_t key_len;
  const char *key = js_getstr(js, key_val, &key_len);
  ant_value_t target = data->target;
  
  if (!key || lkp(js, target, key, key_len).obj) {
    ant_value_t args[2] = { target, key_val };
    return builtin_object_getOwnPropertyDescriptor(js, args, 2, js_mkundef());
  }
  
  ant_value_t value = data->host->get(js, target, key, key_len);
  if (vtype(value) == kTypeUndefined) return value;
  
  return js_from_data_descriptor(js, value, true, true, true);
}

bool is_array_value(ant_value_t value) {
  while (is_proxy(value)) {
    ant_proxy_state_t *data = get_proxy_data(value);
    if (!data || data->revoked) return false;
    value = data->target;
  }
  return real_array_obj_ptr(value) != NULL;
}

ant_value_t js_is_array_value_checked(ant_t *js, ant_value_t value, bool *out) {
  while (is_proxy(value)) {
    ant_proxy_state_t *data = get_proxy_data(value);
    if (!data || data->revoked)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot perform IsArray on a revoked proxy");
    value = data->target;
  }
  *out = real_array_obj_ptr(value) != NULL;
  return js_mkundef();
}

bool js_is_constructor(ant_value_t value) {
  ant_value_t slow = value;
  ant_value_t fast = value;

  while (true) {
    uint8_t t = vtype(slow);
    if (t == kTypeFunction) {
      ant_object_t *obj = js_obj_ptr(js_func_obj(slow));
      return obj && obj->flags.is_constructor;
    }
    if (t == kTypeBuiltin) {
      const ant_cfunc_meta_t *meta = js_as_cfunc_meta(slow);
      return meta && (meta->flags & CFUNC_IS_CONSTRUCTOR) != 0;
    }
    if (t != kTypeObject) return false;

    ant_object_t *obj = js_obj_ptr(slow);
    if (!obj) return false;
    if (obj->flags.is_constructor) return true;
    if (!obj->flags.is_exotic) return false;

    ant_proxy_state_t *data = get_proxy_data(slow);
    if (!data) return false;
    slow = data->target;

    for (int i = 0; i < 2; i++) {
      if (vtype(fast) != kTypeObject) { fast = js_mknull(); break; }
      ant_object_t *fobj = js_obj_ptr(fast);
      if (!fobj || !fobj->flags.is_exotic) { fast = js_mknull(); break; }
      ant_proxy_state_t *fdata = get_proxy_data(fast);
      if (!fdata) { fast = js_mknull(); break; }
      fast = fdata->target;
    }

    if (same_object_identity(slow, fast)) return false;
  }
}

static ant_value_t proxy_read_target(ant_t *js, ant_value_t obj) {
  ant_proxy_state_t *data = get_proxy_data(obj);
  return data ? data->target : obj;
}

static inline TypedArrayData *array_method_typedarray_data(ant_value_t value, uint8_t value_type) {
  if (value_type == kTypeTypedArray) return (TypedArrayData *)js_gettypedarray(value);
  if (value_type == kTypeObject) return (TypedArrayData *)js_get_native(value, BUFFER_TYPEDARRAY_NATIVE_TAG);
  return NULL;
}

static ant_value_t proxy_aware_length(ant_t *js, ant_value_t obj, ant_offset_t *out_len) {
  *out_len = 0;
  uint8_t obj_type = vtype(obj);
  if (is_proxy(obj)) {
    ant_value_t len_val = proxy_get(js, obj, "length", 6);
    if (is_err(len_val)) return len_val;
    if (vtype(len_val) == kTypeNumber) *out_len = (ant_offset_t)tod(len_val);
    return js_mkundef();
  }
  TypedArrayData *ta = obj_type == kTypeArray ? NULL : array_method_typedarray_data(obj, obj_type);
  if (ta) {
    *out_len = (ant_offset_t)ta->length;
    return js_mkundef();
  }
  return array_like_length_checked(js, obj, out_len);
}

static ant_value_t proxy_aware_get_elem(ant_t *js, ant_value_t obj, const char *key, size_t key_len) {
  if (is_proxy(obj)) return proxy_get(js, obj, key, key_len);
  ant_prop_loc_t off = lkp(js, obj, key, key_len);
  return off.obj ? js_prop_load(off) : js_mkundef();
}

static ant_value_t proxy_delete_index(ant_t *js, ant_value_t obj, ant_offset_t idx) {
  char idxstr[16];
  size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (uint64_t)idx);
  return proxy_delete(js, obj, idxstr, idxlen);
}

static inline bool array_method_dense_element(ant_value_t arr, ant_offset_t idx, ant_value_t *out) {
  return array_dense_read(array_obj_ptr(arr), idx, out);
}

static inline ant_object_t *array_method_elements(ant_value_t arr) {
  return is_proxy(arr) ? NULL : array_obj_ptr(arr);
}

static inline ant_value_t array_method_element(
  ant_t *js, ant_value_t arr, ant_object_t *elems, ant_offset_t idx, ant_value_t *out
) {
  if (array_dense_read(elems, idx, out)) return js_true;
  
  ant_value_t has = array_method_has_index(js, arr, idx);
  if (has != js_true) return has;
  
  *out = array_method_get_index(js, arr, idx);
  return is_err(*out) ? *out : js_true;
}

static ant_value_t array_method_has_index(ant_t *js, ant_value_t arr, ant_offset_t idx) {
  ant_value_t dense;
  if (array_method_dense_element(arr, idx, &dense)) return js_true;
  if (is_proxy(arr)) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (uint64_t)idx);
    return proxy_has(js, arr, idxstr, idxlen);
  }
  
  uint8_t arr_type = vtype(arr);
  if (arr_type != kTypeArray) {
    TypedArrayData *ta = array_method_typedarray_data(arr, arr_type);
    if (ta) return js_bool((size_t)idx < ta->length);
    return do_in(js, tov((double)idx), arr);
  }
  
  return js_bool(arr_has(js, arr, idx));
}

static ant_value_t array_method_get_index(ant_t *js, ant_value_t arr, ant_offset_t idx) {
  ant_value_t dense;
  if (array_method_dense_element(arr, idx, &dense)) return dense;
  if (is_proxy(arr)) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (uint64_t)idx);
    return proxy_get(js, arr, idxstr, idxlen);
  }
  
  uint8_t arr_type = vtype(arr);
  if (arr_type != kTypeArray) {
    TypedArrayData *ta = array_method_typedarray_data(arr, arr_type);
    ant_value_t item = js_mkundef();
    if (ta && buffer_typedarray_data_read_index(js, ta, (size_t)idx, &item)) return item;
    if (!ta) {
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (uint64_t)idx);
      return js_getprop_fallback_len(js, arr, idxstr, idxlen);
    }
  }
  
  return arr_get(js, arr, idx);
}

static ant_value_t throw_proxy_error(ant_t *js, const char *message) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "%s", message);
}

static ant_value_t proxy_get_method(ant_t *js, ant_value_t handler, const char *name) {
  ant_value_t trap = js_getprop_fallback(js, handler, name);
  if (is_err(trap)) return trap;
  if (vtype(trap) == kTypeUndefined || vtype(trap) == kTypeNull) return js_mkundef();
  if (!is_callable(trap))
    return js_mkerr_typed(js, JS_ERR_TYPE, "'%s' proxy trap is not callable", name);
  return trap;
}

static bool proxy_target_is_extensible(ant_value_t obj) {
  uint8_t t = vtype(obj);
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) return false;

  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  if (!ptr) return false;
  if (ptr->flags.frozen || ptr->flags.sealed) return false;
  return ptr->flags.extensible != 0;
}

static bool proxy_target_prop_is_nonconfig(ant_t *js, ant_value_t target, ant_prop_loc_t prop_off) {
  (void)js; (void)target;
  return is_nonconfig_prop(prop_off);
}

static bool proxy_target_prop_is_const(ant_t *js, ant_value_t target, ant_prop_loc_t prop_off) {
  (void)js; (void)target;
  return is_const_prop(prop_off);
}

static ant_value_t proxy_get_with_receiver(ant_t *js, ant_value_t proxy, const char *key, size_t key_len, ant_value_t receiver) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return js_mkundef();
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'get' on a proxy that has been revoked");
  if (data->host) return proxy_host_get(js, data, key, key_len);
  
  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  
  ant_value_t get_trap = proxy_get_method(js, handler, "get");
  if (is_err(get_trap)) return get_trap;
  if (vtype(get_trap) != kTypeUndefined) {
      ant_value_t key_val = js_mkstr(js, key, key_len);
      
      ant_value_t args[3] = { target, key_val, receiver };
      ant_value_t result = sv_vm_call(js->vm, js, get_trap, js_mkundef(), args, 3, NULL, js_mkundef());
      if (is_err(result)) return result;

      ant_prop_loc_t prop_off = lkp(js, target, key, key_len);
      if (prop_off.obj && proxy_target_prop_is_nonconfig(js, target, prop_off) &&
          proxy_target_prop_is_const(js, target, prop_off)) {
        ant_value_t target_value = js_prop_load(prop_off);
        if (!strict_eq_values(js, result, target_value))
          return js_mkerr_typed(js, JS_ERR_TYPE, "'get' on proxy: trap returned invalid value for non-configurable, non-writable property");
      }

      prop_meta_t meta;
      bool has_meta = lookup_string_prop_meta(js, js_as_obj(target), key, key_len, &meta);
      if (has_meta && !meta.configurable) {
        if (!meta.has_getter && !meta.has_setter && !meta.writable && prop_off.obj) {
          ant_value_t target_value = js_prop_load(prop_off);
          if (!strict_eq_values(js, result, target_value))
            return js_mkerr_typed(js, JS_ERR_TYPE, "'get' on proxy: trap returned invalid value for non-configurable, non-writable property");
        }
        if ((meta.has_getter || meta.has_setter) && !meta.has_getter && vtype(result) != kTypeUndefined)
          return js_mkerr_typed(js, JS_ERR_TYPE, "'get' on proxy: trap returned non-undefined for property with undefined getter");
      }

      return result;
  }
  
  char key_buf[256];
  char *owned_key = NULL;
  char *key_cstr = key_buf;
  if (key_len >= sizeof(key_buf)) {
    owned_key = calloc(1, key_len + 1);
    if (!owned_key) return js_mkerr(js, "oom");
    key_cstr = owned_key;
  }
  memcpy(key_cstr, key, key_len);
  key_cstr[key_len] = '\0';

  ant_value_t result = js_getprop_super(js, target, receiver, key_cstr);
  free(owned_key);
  return result;
}

static ant_value_t proxy_get(ant_t *js, ant_value_t proxy, const char *key, size_t key_len) {
  return proxy_get_with_receiver(js, proxy, key, key_len, proxy);
}

static ant_value_t proxy_set_with_receiver(ant_t *js, ant_value_t proxy, const char *key, size_t key_len, ant_value_t value, ant_value_t receiver) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  
  if (!data) return js_mkundef();
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'set' on a proxy that has been revoked");
  
  if (data->host && same_object_identity(receiver, proxy))
    return data->host->set(js, data->target, key, key_len, value);
  
  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  
  ant_value_t set_trap = proxy_get_method(js, handler, "set");
  if (is_err(set_trap)) return set_trap;
  if (vtype(set_trap) != kTypeUndefined) {
      ant_value_t key_val = js_mkstr(js, key, key_len);
      ant_value_t args[4] = { target, key_val, value, receiver };
      ant_value_t result = sv_vm_call(js->vm, js, set_trap, js_mkundef(), args, 4, NULL, js_mkundef());
      if (is_err(result)) return result;
      if (!js_truthy(js, result)) return js_false;
      {
        ant_prop_loc_t prop_off = lkp(js, target, key, key_len);
        if (prop_off.obj && proxy_target_prop_is_nonconfig(js, target, prop_off) &&
            proxy_target_prop_is_const(js, target, prop_off)) {
          ant_value_t target_value = js_prop_load(prop_off);
          if (!strict_eq_values(js, value, target_value))
            return js_mkerr_typed(js, JS_ERR_TYPE, "'set' on proxy: trap returned truthy for non-configurable, non-writable property with different value");
        }

        prop_meta_t meta;
        bool has_meta = lookup_string_prop_meta(js, js_as_obj(target), key, key_len, &meta);
        if (has_meta && !meta.configurable) {
          if (!meta.has_getter && !meta.has_setter && !meta.writable && prop_off.obj) {
            ant_value_t target_value = js_prop_load(prop_off);
            if (!strict_eq_values(js, value, target_value))
              return js_mkerr_typed(js, JS_ERR_TYPE, "'set' on proxy: trap returned truthy for non-configurable, non-writable property with different value");
          }
          if ((meta.has_getter || meta.has_setter) && !meta.has_setter)
            return js_mkerr_typed(js, JS_ERR_TYPE, "'set' on proxy: trap returned truthy for property with undefined setter");
        }
      }
      return js_true;
  }
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, proxy);
  GC_ROOT_PIN(js, receiver);
  GC_ROOT_PIN(js, value);
  ant_value_t key_str = js_mkstr(js, key, key_len);
  GC_ROOT_PIN(js, key_str);
  if (is_err(key_str)) {
    GC_ROOT_RESTORE(js, root_mark);
    return key_str;
  }

  ant_value_t own_desc = proxy_get_own_property_descriptor(js, proxy, key_str);
  GC_ROOT_PIN(js, own_desc);
  if (is_err(own_desc)) {
    GC_ROOT_RESTORE(js, root_mark);
    return own_desc;
  }

  ant_value_t desc = Ant_Object_CreateBare(js);
  GC_ROOT_PIN(js, desc);
  if (is_err(desc)) {
    GC_ROOT_RESTORE(js, root_mark);
    return desc;
  }
  js_setprop(js, desc, js_mkstr(js, "value", 5), value);
  if (vtype(own_desc) == kTypeUndefined) {
    js_setprop(js, desc, js_mkstr(js, "writable", 8), js_true);
    js_setprop(js, desc, js_mkstr(js, "enumerable", 10), js_true);
    js_setprop(js, desc, js_mkstr(js, "configurable", 12), js_true);
  }

  ant_value_t define_trap = proxy_get_method(js, handler, "defineProperty");
  if (is_err(define_trap)) {
    GC_ROOT_RESTORE(js, root_mark);
    return define_trap;
  }
  if (vtype(define_trap) == kTypeUndefined) {
    ant_value_t set_result;
    if (same_object_identity(receiver, proxy)) {
      bool declined = false;
      set_result = setprop_impl(js, target, key_str, value, false, &declined);
      if (!is_err(set_result) && declined) {
        GC_ROOT_RESTORE(js, root_mark);
        return js_false;
      }
    } else {
      if (!is_object_type(receiver)) {
        GC_ROOT_RESTORE(js, root_mark);
        return js_false;
      }

      ant_prop_loc_t existing = lkp(js, receiver, key, key_len);
      if (existing.obj) {
        if (is_const_prop(existing)) {
          GC_ROOT_RESTORE(js, root_mark);
          return js_false;
        }
        js_prop_store(js, existing, value);
        array_define_or_set_index(js, receiver, key, key_len);
        GC_ROOT_RESTORE(js, root_mark);
        return js_true;
      }

      ant_value_t extensible = check_object_extensibility(js, receiver);
      if (is_err(extensible)) {
        GC_ROOT_RESTORE(js, root_mark);
        return extensible;
      }
      if (extensible == js_false) {
        GC_ROOT_RESTORE(js, root_mark);
        return js_false;
      }

      const char *interned_key = intern_string(key, key_len);
      set_result = interned_key
        ? mkprop_interned_exact(js, receiver, interned_key, value, 0)
        : js_mkerr(js, "oom");
      if (!is_err(set_result)) array_define_or_set_index(js, receiver, key, key_len);
    }
    GC_ROOT_RESTORE(js, root_mark);
    if (is_err(set_result)) return set_result;
    return js_true;
  }

  ant_value_t define_result = proxy_define_property(js, proxy, key_str, desc);
  GC_ROOT_RESTORE(js, root_mark);
  if (is_err(define_result)) return define_result;
  return js_bool(js_truthy(js, define_result));
}

static ant_value_t proxy_set(ant_t *js, ant_value_t proxy, const char *key, size_t key_len, ant_value_t value) {
  return proxy_set_with_receiver(js, proxy, key, key_len, value, proxy);
}

static ant_value_t proxy_set_rejected(ant_t *js, const char *key, size_t key_len, ant_value_t value, bool *declined) {
  if (!sv_is_strict_context(js)) return set_declined(declined, value);
  return js_mkerr_typed(js, JS_ERR_TYPE, "'set' on proxy: trap returned falsish for property '%.*s'", (int)key_len, key);
}

[[gnu::noinline, gnu::cold]]
static ant_value_t proxy_set_rejected_sym(ant_t *js, ant_value_t sym, ant_value_t value, bool *declined) {
  char desc[128];
  size_t desc_len = tostr(js, sym, desc, sizeof(desc));
  return proxy_set_rejected(js, desc, desc_len < sizeof(desc) ? desc_len : sizeof(desc) - 1, value, declined);
}

static ant_value_t proxy_set_sym(
  ant_t *js, ant_value_t proxy, ant_value_t sym, ant_value_t value,
  ant_value_t receiver, ant_value_t *target_out
) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  *target_out = js_mkundef();
  if (!data) return js_mkundef();
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'set' on a proxy that has been revoked");
  if (data->host) return js_mkerr_typed(js, JS_ERR_TYPE, "%s", "Cannot convert a Symbol value to a string");
  
  ant_value_t target = data->target;
  *target_out = target;
  ant_value_t set_trap = proxy_get_method(js, data->handler, "set");
  if (is_err(set_trap) || vtype(set_trap) == kTypeUndefined) return set_trap;
  
  ant_value_t args[4] = { target, sym, value, receiver };
  ant_value_t result = sv_vm_call(js->vm, js, set_trap, js_mkundef(), args, 4, NULL, js_mkundef());
  if (is_err(result)) return result;
  if (!js_truthy(js, result)) return js_false;
  
  ant_offset_t sym_off = (ant_offset_t)vdata(sym);
  prop_meta_t meta;
  if (lookup_symbol_prop_meta(js, js_as_obj(target), sym_off, &meta) && !meta.configurable) {
    if (!meta.has_getter && !meta.has_setter && !meta.writable) {
      ant_prop_loc_t own = lkp_sym(js_as_obj(target), sym_off);
      if (own.obj && !strict_eq_values(js, value, js_prop_load(own)))
        return js_mkerr_typed(js, JS_ERR_TYPE, "'set' on proxy: trap returned truthy for non-configurable, non-writable property with different value");
    }
    if (meta.has_getter && !meta.has_setter)
      return js_mkerr_typed(js, JS_ERR_TYPE, "'set' on proxy: trap returned truthy for property with undefined setter");
  }
  
  return js_true;
}

ant_value_t js_proxy_set(ant_t *js, ant_value_t proxy, const char *key, size_t key_len, ant_value_t value) {
  return proxy_set(js, proxy, key, key_len, value);
}

ant_value_t js_proxy_set_sym(ant_t *js, ant_value_t proxy, ant_value_t sym, ant_value_t value) {
  ant_value_t target = js_mkundef();
  
  ant_value_t result = proxy_set_sym(js, proxy, sym, value, proxy, &target);
  if (is_err(result) || result == js_true || result == js_false) return result;
  
  bool declined = false;
  ant_value_t stored = setprop_impl(js, target, sym, value, false, &declined);
  if (is_err(stored)) return stored;
  
  return js_bool(!declined);
}

ant_value_t js_set_reporting(ant_t *js, ant_value_t target, ant_value_t key, ant_value_t value) {
  sv_vm_t *vm = js->vm;
  int saved_fp = vm->jit_mode_fp;
  bool saved_strict = vm->jit_mode_strict;
  
  vm->jit_mode_fp = vm->fp;
  vm->jit_mode_strict = false;
  bool declined = false;
  
  ant_value_t stored = setprop_impl(js, target, key, value, false, &declined);
  vm->jit_mode_fp = saved_fp;
  vm->jit_mode_strict = saved_strict;
  
  if (is_err(stored)) return stored;
  return js_bool(!declined);
}

static ant_value_t proxy_has(ant_t *js, ant_value_t proxy, const char *key, size_t key_len) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  
  if (!data) return js_false;
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'has' on a proxy that has been revoked");
  
  if (data->host) return js_bool(
    lkp_proto(js, data->target, key, key_len).obj ||
    vtype(data->host->get(js, data->target, key, key_len)) != kTypeUndefined
  );
  
  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  
  ant_value_t has_trap = proxy_get_method(js, handler, "has");
  if (is_err(has_trap)) return has_trap;
  if (vtype(has_trap) != kTypeUndefined) {
      ant_value_t key_val = js_mkstr(js, key, key_len);
      ant_value_t args[2] = { target, key_val };
      ant_value_t result = sv_vm_call(js->vm, js, has_trap, js_mkundef(), args, 2, NULL, js_mkundef());
      if (is_err(result)) return result;

      if (!js_truthy(js, result)) {
        ant_prop_loc_t prop_off = lkp(js, target, key, key_len);
        prop_meta_t meta;
        bool has_meta = lookup_string_prop_meta(js, js_as_obj(target), key, key_len, &meta);
        bool has_own = (prop_off.obj) || has_meta;

        if ((prop_off.obj && proxy_target_prop_is_nonconfig(js, target, prop_off)) || (has_meta && !meta.configurable))
          return js_mkerr_typed(js, JS_ERR_TYPE, "'has' on proxy: trap returned falsy for non-configurable property");

        if (has_own && !proxy_target_is_extensible(target))
          return js_mkerr_typed(js, JS_ERR_TYPE, "'has' on proxy: trap returned falsy for existing property on non-extensible target");
      }

      return result;
  }
  
  char key_buf[256];
  size_t len = key_len < sizeof(key_buf) - 1 ? key_len : sizeof(key_buf) - 1;
  memcpy(key_buf, key, len);
  key_buf[len] = '\0';
  
  if (vtype(target) == kTypeArray) {
    if (is_length_key(key_buf, len) && real_array_obj_ptr(target)) return js_true;
    unsigned long idx = 0;
    if (parse_array_index(key_buf, len, get_array_length(js, target), &idx))
      return js_bool(arr_has(js, target, (ant_offset_t)idx));
  }

  ant_prop_loc_t off = lkp_proto(js, target, key_buf, len);
  return js_bool(off.obj);
}

ant_value_t js_proxy_prevent_extensions(ant_t *js, ant_value_t proxy) {
  return proxy_prevent_extensions(js, proxy);
}

ant_value_t js_proxy_has(ant_t *js, ant_value_t proxy, const char *key, size_t key_len) {
  return proxy_has(js, proxy, key, key_len);
}

static ant_value_t proxy_delete(ant_t *js, ant_value_t proxy, const char *key, size_t key_len) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  
  if (!data) return js_true;
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'deleteProperty' on a proxy that has been revoked");
  
  if (data->host) {
    ant_value_t result = js_delete_prop_ordinary(js, data->target, key, key_len);
    if (is_err(result) || !js_truthy(js, result)) return result;
    return data->host->del(js, data->target, key, key_len);
  }
  
  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  
  ant_value_t delete_trap = proxy_get_method(js, handler, "deleteProperty");
  if (is_err(delete_trap)) return delete_trap;
  if (vtype(delete_trap) != kTypeUndefined) {
      ant_value_t key_val = js_mkstr(js, key, key_len);
      ant_value_t args[2] = { target, key_val };
      ant_value_t result = sv_vm_call(js->vm, js, delete_trap, js_mkundef(), args, 2, NULL, js_mkundef());
      if (is_err(result)) return result;
      if (js_truthy(js, result)) {
        ant_prop_loc_t prop_off = lkp(js, target, key, key_len);
        if (prop_off.obj && is_nonconfig_prop(prop_off))
          return js_mkerr_typed(js, JS_ERR_TYPE, "'deleteProperty' on proxy: trap returned truthy for non-configurable property");
        prop_meta_t meta;
        if (lookup_string_prop_meta(js, js_as_obj(target), key, key_len, &meta) && !meta.configurable)
          return js_mkerr_typed(js, JS_ERR_TYPE, "'deleteProperty' on proxy: trap returned truthy for non-configurable property");
      }
      return result;
  }
  
  return js_delete_prop_ordinary(js, target, key, key_len);
}

static ant_value_t proxy_get_val(ant_t *js, ant_value_t proxy, ant_value_t key_val) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return js_mkundef();
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'get' on a proxy that has been revoked");

  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  ant_value_t prop_key = key_val;

  if (vtype(prop_key) != kTypeSymbol) {
  if (is_object_type(prop_key)) {
    prop_key = js_to_primitive(js, prop_key, 1);
    if (is_err(prop_key)) return prop_key;
  }
  
  if (vtype(prop_key) != kTypeSymbol) {
    prop_key = js_tostring_val(js, prop_key);
    if (is_err(prop_key)) return prop_key;
  }}

  if (data->host && vtype(prop_key) == kTypeString) {
    size_t key_len = 0;
    const char *key = js_getstr(js, prop_key, &key_len);
    return key ? proxy_host_get(js, data, key, key_len) : js_mkerr(js, "out of memory");
  }

  ant_value_t get_trap = proxy_get_method(js, handler, "get");
  if (is_err(get_trap)) return get_trap;
  if (vtype(get_trap) != kTypeUndefined) {
    ant_value_t args[3] = { target, prop_key, proxy };
    return sv_vm_call(js->vm, js, get_trap, js_mkundef(), args, 3, NULL, js_mkundef());
  }

  if (vtype(prop_key) == kTypeSymbol) return js_get_sym_with_receiver(js, target, prop_key, proxy);

  ant_offset_t key_len = 0;
  ant_offset_t key_off = vstr(js, prop_key, &key_len);
  const char *key_ptr = (const char *)(uintptr_t)key_off;
  
  return proxy_get(js, proxy, key_ptr, (size_t)key_len);
}

static ant_value_t proxy_get_prototype_of(ant_t *js, ant_value_t proxy) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return js_mknull();
  
  if (data->revoked)
    return throw_proxy_error(js, "Cannot perform 'getPrototypeOf' on a proxy that has been revoked");

  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  ant_value_t trap = proxy_get_method(js, handler, "getPrototypeOf");
  if (is_err(trap)) return trap;
  if (vtype(trap) != kTypeUndefined) {
    ant_value_t args[1] = { target };
    ant_value_t result = sv_vm_call(js->vm, js, trap, js_mkundef(), args, 1, NULL, js_mkundef());
    if (is_err(result)) return result;
    if (!is_object_type(result) && vtype(result) != kTypeNull)
      return js_mkerr_typed(js, JS_ERR_TYPE, "'getPrototypeOf' on proxy: trap returned neither object nor null");
    if (!proxy_target_is_extensible(target)) {
      ant_value_t target_proto = get_proto(js, target);
      if (!same_object_identity(result, target_proto))
        return js_mkerr_typed(js, JS_ERR_TYPE, "'getPrototypeOf' on proxy: trap returned incompatible prototype for non-extensible target");
    }
    return result;
  }

  return get_proto(js, target);
}

static ant_value_t proxy_set_prototype_of(ant_t *js, ant_value_t proxy, ant_value_t proto) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return js_false;
  if (data->revoked)
    return throw_proxy_error(js, "Cannot perform 'setPrototypeOf' on a proxy that has been revoked");

  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  ant_value_t trap = proxy_get_method(js, handler, "setPrototypeOf");
  if (is_err(trap)) return trap;

  if (vtype(trap) != kTypeUndefined) {
    ant_value_t args[2] = { target, proto };
    ant_value_t result = sv_vm_call(js->vm, js, trap, js_mkundef(), args, 2, NULL, js_mkundef());
    if (is_err(result)) return result;
    if (js_truthy(js, result) && !proxy_target_is_extensible(target)) {
      ant_value_t current = get_proto(js, target);
      if (!same_object_identity(current, proto))
        return js_mkerr_typed(js, JS_ERR_TYPE, "'setPrototypeOf' on proxy: trap returned truthy for incompatible prototype on non-extensible target");
    }
    return js_bool(js_truthy(js, result));
  }

  set_proto(js, target, proto);
  return js_true;
}

static ant_value_t proxy_is_extensible(ant_t *js, ant_value_t proxy) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return js_false;
  if (data->revoked)
    return throw_proxy_error(js, "Cannot perform 'isExtensible' on a proxy that has been revoked");

  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  bool target_extensible = proxy_target_is_extensible(target);
  ant_value_t trap = proxy_get_method(js, handler, "isExtensible");
  if (is_err(trap)) return trap;
  if (vtype(trap) == kTypeUndefined) return js_bool(target_extensible);

  ant_value_t args[1] = { target };
  ant_value_t result = sv_vm_call(js->vm, js, trap, js_mkundef(), args, 1, NULL, js_mkundef());
  if (is_err(result)) return result;
  bool trap_extensible = js_truthy(js, result);
  if (trap_extensible != target_extensible)
    return js_mkerr_typed(js, JS_ERR_TYPE, "'isExtensible' on proxy: trap result does not reflect target extensibility");
  return js_bool(trap_extensible);
}

static ant_value_t proxy_prevent_extensions(ant_t *js, ant_value_t proxy) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return js_false;

  if (data->revoked)
    return throw_proxy_error(js, "Cannot perform 'preventExtensions' on a proxy that has been revoked");
  
  if (data->host) return js_false;

  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  ant_value_t trap = proxy_get_method(js, handler, "preventExtensions");
  if (is_err(trap)) return trap;

  if (vtype(trap) != kTypeUndefined) {
    ant_value_t args[1] = { target };
    ant_value_t result = sv_vm_call(js->vm, js, trap, js_mkundef(), args, 1, NULL, js_mkundef());
    if (is_err(result)) return result;
    bool ok = js_truthy(js, result);
    if (ok && proxy_target_is_extensible(target))
      return js_mkerr_typed(js, JS_ERR_TYPE, "'preventExtensions' on proxy: trap returned truthy but target is extensible");
    return js_bool(ok);
  }

  ant_object_t *ptr = js_obj_ptr(js_as_obj(target));
  if (ptr) ptr->flags.extensible = 0;
  return js_true;
}

static ant_value_t proxy_has_val(ant_t *js, ant_value_t proxy, ant_value_t key_val) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return js_false;
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'has' on a proxy that has been revoked");

  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  ant_value_t prop_key = key_val;

  if (vtype(prop_key) != kTypeSymbol) {
  if (is_object_type(prop_key)) {
    prop_key = js_to_primitive(js, prop_key, 1);
    if (is_err(prop_key)) return prop_key;
  }
  
  if (vtype(prop_key) != kTypeSymbol) {
    prop_key = js_tostring_val(js, prop_key);
    if (is_err(prop_key)) return prop_key;
  }}

  ant_value_t has_trap = proxy_get_method(js, handler, "has");
  if (is_err(has_trap)) return has_trap;
  if (vtype(has_trap) != kTypeUndefined) {
    ant_value_t args[2] = { target, prop_key };
    return sv_vm_call(js->vm, js, has_trap, js_mkundef(), args, 2, NULL, js_mkundef());
  }

  if (vtype(prop_key) == kTypeSymbol) {
    ant_prop_loc_t off = lkp_sym_proto(js, target, (ant_offset_t)vdata(prop_key));
    return js_bool(off.obj);
  }

  ant_offset_t key_len = 0;
  ant_offset_t key_off = vstr(js, prop_key, &key_len);
  const char *key_ptr = (const char *)(uintptr_t)key_off;
  
  return proxy_has(js, proxy, key_ptr, (size_t)key_len);
}

static bool descriptor_has_own_key(ant_t *js, ant_value_t desc, const char *key, size_t key_len) {
  return vtype(desc) == kTypeObject && lkp(js, desc, key, key_len).obj;
}

static bool descriptor_bool_value(ant_t *js, ant_value_t desc, const char *key, size_t key_len, bool default_value) {
  if (vtype(desc) != kTypeObject) return default_value;
  ant_prop_loc_t off = lkp(js, desc, key, key_len);
  if (!off.obj) return default_value;
  return js_truthy(js, js_prop_load(off));
}

static inline bool descriptor_is_accessor(ant_t *js, ant_value_t desc) {
  return descriptor_has_own_key(js, desc, "get", 3) || descriptor_has_own_key(js, desc, "set", 3);
}

static ant_value_t own_property_descriptor(ant_t *js, ant_value_t obj, ant_value_t key) {
  ant_value_t args[2] = { obj, key };
  return builtin_object_getOwnPropertyDescriptor(js, args, 2, js_mkundef());
}

static inline ant_value_t set_receiver_write_target(ant_t *js, ant_value_t receiver) {
  return vtype(receiver) == kTypeBuiltin ? js_cfunc_promote(js, receiver) : receiver;
}

static ant_value_t set_receiver_own_target(ant_t *js, ant_value_t receiver) {
  for (int depth = 0; depth < MAX_PROTO_CHAIN_DEPTH && is_object_type(receiver); depth++) {
    ant_value_t obj = js_as_obj(receiver);
    if (!is_proxy(obj)) return obj;
    
    ant_proxy_state_t *data = get_proxy_data(obj);
    if (!data || data->revoked || data->host || !is_object_type(data->handler)) return js_mkundef();
    
    if (data->plain_handler_epoch != js->ic.epoch) {
      if (
        !chain_lacks_plain_keys(data->handler, 
        js->intern.get_own_property_descriptor, js->intern.define_property)
      ) return js_mkundef();
      
      for (ant_value_t cur = data->handler; is_object_type(cur);) {
        ant_object_t *ptr = js_obj_ptr(js_as_obj(cur));
        ant_object_guard_absence(ptr);
        cur = ptr->proto;
      }
      
      data->plain_handler_epoch = js->ic.epoch;
    }
    
    receiver = data->target;
  }
  
  return js_mkundef();
}

static ant_value_t set_with_receiver_fast(ant_t *js, ant_value_t target, ant_value_t key, ant_value_t value, ant_value_t receiver) {
  if (vtype(key) != kTypeString) return T_EMPTY;
  
  size_t key_len = 0;
  const char *key_str = js_getstr(js, key, &key_len);
  if (!key_str || is_length_key(key_str, key_len) || is_array_index(key_str, (ant_offset_t)key_len)) return T_EMPTY;
  
  const char *interned = intern_find(key_str, key_len);
  ant_object_t *holder = NULL;
  int32_t slot = -1;
  bool chain_end = false;
  
  ant_value_t cur = target;
  for (int depth = 0; depth < MAX_PROTO_CHAIN_DEPTH && !holder && !chain_end; depth++) {
    ant_object_t *ptr = js_obj_ptr(js_as_obj(cur));
    if (!is_plain_set_target(ptr)) return T_EMPTY;
    
    slot = interned ? ant_shape_lookup_interned(ptr->shape, interned) : -1;
    if (slot >= 0) holder = ptr;
    else if (vtype(ptr->proto) == kTypeNull) chain_end = true;
    else if (is_object_type(ptr->proto)) cur = ptr->proto;
    else return T_EMPTY;
  }
  
  if (!holder && !chain_end) return T_EMPTY;
  if (holder) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(holder->shape, (uint32_t)slot);
    if (!prop || (uint32_t)slot >= holder->prop_count) return T_EMPTY;
    
    if (prop->has_getter || prop->has_setter) {
      if (!prop->has_setter || !is_callable(prop->setter)) return js_false;
      ant_value_t called = sv_vm_call(js->vm, js, prop->setter, receiver, &value, 1, NULL, js_mkundef());
      return is_err(called) ? called : js_true;
    }
    
    if (!(ant_shape_get_attrs(holder->shape, (uint32_t)slot) & ANT_PROP_ATTR_WRITABLE)) return js_false;
  }
  
  ant_value_t write = set_receiver_write_target(js, receiver);
  if (is_err(write)) return write;
  if (!is_object_type(write)) return js_false;
  ant_value_t own = set_receiver_own_target(js, write);
  ant_object_t *own_ptr = is_object_type(own) ? js_obj_ptr(own) : NULL;
  if (!is_plain_set_target(own_ptr)) return T_EMPTY;
  
  int32_t own_slot = interned ? ant_shape_lookup_interned(own_ptr->shape, interned) : -1;
  if (own_slot >= 0) {
    if ((uint32_t)own_slot >= own_ptr->prop_count) return T_EMPTY;
    if (!own_writable_data_slot(own_ptr, own_slot)) return js_false;
    store_own_data_slot(js, own_ptr, (uint32_t)own_slot, interned, value);
    return js_true;
  }
  
  if (!own_ptr->flags.extensible) return js_false;
  const char *added_key = interned ? interned : intern_string(key_str, key_len);
  if (!added_key) return js_mkerr(js, "oom");
  
  ant_value_t added = mkprop_interned_exact(js, own, added_key, value, 0);
  return is_err(added) ? added : js_true;
}

static ant_value_t set_with_receiver(ant_t *js, ant_value_t target, ant_value_t key, ant_value_t value, ant_value_t receiver) {
  ant_value_t cur = target;
  ant_value_t desc = js_mkundef();
  
  for (int depth = 0; depth < MAX_PROTO_CHAIN_DEPTH; depth++) {
    ant_value_t obj = js_as_obj(cur);
    if (is_proxy(obj) && vtype(key) == kTypeSymbol) {
      ant_value_t proxy_target = js_mkundef();
      ant_value_t result = proxy_set_sym(js, obj, key, value, receiver, &proxy_target);
      if (vtype(result) != kTypeUndefined) return result;
      cur = proxy_target;
      continue;
    }
    
    if (is_proxy(obj)) {
      size_t key_len = 0;
      const char *key_str = js_getstr(js, key, &key_len);
      return proxy_set_with_receiver(js, obj, key_str, key_len, value, receiver);
    }
    
    if (vtype(key) == kTypeString) {
      size_t key_len = 0;
      const char *key_str = js_getstr(js, key, &key_len);
      ant_object_t *ptr = js_obj_ptr(obj);
      if (key_str && ptr && exotic_own_get(js, obj, ptr, key_str, key_len) == T_EMPTY) return js_true;
    }
    
    desc = own_property_descriptor(js, cur, key);
    if (is_err(desc) || vtype(desc) != kTypeUndefined) break;
    
    ant_value_t proto = js_get_proto(js, cur);
    if (!is_object_type(proto)) break;
    cur = proto;
  }
  
  if (is_err(desc)) return desc;
  if (descriptor_is_accessor(js, desc)) {
    ant_value_t setter = js_get(js, desc, "set");
    if (!is_callable(setter)) return js_false;
    
    ant_value_t called = sv_vm_call(js->vm, js, setter, receiver, &value, 1, NULL, js_mkundef());
    return is_err(called) ? called : js_true;
  }
  
  if (vtype(desc) != kTypeUndefined && !descriptor_bool_value(js, desc, "writable", 8, false)) return js_false;
  ant_value_t write = set_receiver_write_target(js, receiver);
  if (is_err(write)) return write;
  if (!is_object_type(write)) return js_false;
  
  ant_value_t existing = own_property_descriptor(js, write, key);
  if (is_err(existing)) return existing;
  
  bool fresh = vtype(existing) == kTypeUndefined;
  if (!fresh && (descriptor_is_accessor(js, existing) || !descriptor_bool_value(js, existing, "writable", 8, false)))
    return js_false;
  
  ant_value_t update = Ant_Object_CreateBare(js);
  if (is_err(update)) return update;
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, update);
  
  js_set(js, update, "value", value);
  if (fresh) {
    js_set(js, update, "writable", js_true);
    js_set(js, update, "enumerable", js_true);
    js_set(js, update, "configurable", js_true);
  }
  
  GC_ROOT_PIN(js, write);
  ant_value_t result = js_define_property(js, write, key, update, true);
  GC_ROOT_RESTORE(js, root_mark);
  
  return result;
}

ant_value_t js_set_with_receiver(ant_t *js, ant_value_t target, ant_value_t key, ant_value_t value, ant_value_t receiver) {
  if (receiver == target) return js_set_reporting(js, target, key, value);
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, target);
  GC_ROOT_PIN(js, key);
  GC_ROOT_PIN(js, value);
  GC_ROOT_PIN(js, receiver);
  
  ant_value_t result = set_with_receiver_fast(js, target, key, value, receiver);
  if (result == T_EMPTY) result = set_with_receiver(js, target, key, value, receiver);
  GC_ROOT_RESTORE(js, root_mark);
  
  return result;
}

typedef struct {
  bool accessor;
  bool enumerable;
  bool configurable;
  bool writable;
  ant_value_t first;
  ant_value_t second;
} proxy_full_desc_t;

static ant_value_t proxy_to_complete_descriptor(ant_t *js, ant_value_t obj, proxy_full_desc_t *out) {
  define_desc_raw_t raw = {0};
  ant_value_t err = define_desc_read(js, obj, &raw);
  if (is_err(err)) return err;
  
  bool accessor = raw.has[DESC_GET] || raw.has[DESC_SET];
  if (accessor && (raw.has[DESC_VALUE] || raw.has[DESC_WRITABLE])) return js_mkerr_typed(
    js, JS_ERR_TYPE, "Invalid property descriptor. Cannot both specify accessors and a value or writable attribute"
  );
  
  *out = (proxy_full_desc_t){
    .accessor = accessor,
    .enumerable = raw.has[DESC_ENUMERABLE] && js_truthy(js, raw.val[DESC_ENUMERABLE]),
    .configurable = raw.has[DESC_CONFIGURABLE] && js_truthy(js, raw.val[DESC_CONFIGURABLE]),
    .writable = raw.has[DESC_WRITABLE] && js_truthy(js, raw.val[DESC_WRITABLE]),
    .first = raw.has[accessor ? DESC_GET : DESC_VALUE] ? raw.val[accessor ? DESC_GET : DESC_VALUE] : js_mkundef(),
    .second = raw.has[DESC_SET] ? raw.val[DESC_SET] : js_mkundef(),
  };
  
  return js_mkundef();
}

static bool proxy_desc_compatible(ant_t *js, bool extensible, const proxy_full_desc_t *desc, const proxy_full_desc_t *current, bool exists) {
  if (!exists) return extensible;
  if (current->configurable) return true;
  if (desc->configurable || desc->enumerable != current->enumerable) return false;
  if (desc->accessor != current->accessor) return false;
  
  if (current->accessor) return
    same_value_values(js, desc->first, current->first) &&
    same_value_values(js, desc->second, current->second);
    
  if (current->writable) return true;
  return !desc->writable && same_value_values(js, desc->first, current->first);
}

static ant_value_t proxy_get_own_property_descriptor(ant_t *js, ant_value_t proxy, ant_value_t key_val) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return js_mkundef();
  
  if (data->revoked)
    return throw_proxy_error(js, "Cannot perform 'getOwnPropertyDescriptor' on a proxy that has been revoked");
  
  if (data->host && vtype(key_val) == kTypeString) return proxy_host_desc(js, data, key_val);

  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  ant_value_t trap = proxy_get_method(js, handler, "getOwnPropertyDescriptor");
  
  if (is_err(trap)) return trap;
  if (vtype(trap) != kTypeUndefined) {
    ant_value_t args[2] = { target, key_val };
    ant_value_t result = sv_vm_call(js->vm, js, trap, js_mkundef(), args, 2, NULL, js_mkundef());
    if (is_err(result)) return result;
    if (vtype(result) != kTypeUndefined && vtype(result) != kTypeObject)
      return js_mkerr_typed(js, JS_ERR_TYPE, "'getOwnPropertyDescriptor' on proxy: trap returned neither object nor undefined");

    ant_value_t target_args[2] = { target, key_val };
    ant_value_t target_desc = builtin_object_getOwnPropertyDescriptor(js, target_args, 2, js_mkundef());
    if (is_err(target_desc)) return target_desc;
    bool target_exists = vtype(target_desc) != kTypeUndefined;
    
    GC_ROOT_SAVE(mark, js);
    GC_ROOT_PIN(js, result);
    GC_ROOT_PIN(js, target_desc);
    
    proxy_full_desc_t current = {0};
    if (target_exists) {
      ant_value_t read = proxy_to_complete_descriptor(js, target_desc, &current);
      if (is_err(read)) { GC_ROOT_RESTORE(js, mark); return read; }
      GC_ROOT_PIN(js, current.first);
      GC_ROOT_PIN(js, current.second);
    }
    bool target_extensible = proxy_target_is_extensible(target);

    if (vtype(result) == kTypeUndefined) {
      GC_ROOT_RESTORE(js, mark);
      if (target_exists && (!current.configurable || !target_extensible))
        return js_mkerr_typed(js, JS_ERR_TYPE, "'getOwnPropertyDescriptor' on proxy: trap reported existing property as undefined");
      return result;
    }

    proxy_full_desc_t desc = {0};
    ant_value_t read = proxy_to_complete_descriptor(js, result, &desc);
    
    if (is_err(read)) { 
      GC_ROOT_RESTORE(js, mark); 
      return read;
    }
    
    GC_ROOT_PIN(js, desc.first);
    GC_ROOT_PIN(js, desc.second);
    
    const char *invalid = NULL;
    if (!proxy_desc_compatible(js, target_extensible, &desc, &current, target_exists))
      invalid = "trap returned descriptor incompatible with the target property";
    else if (!desc.configurable && (!target_exists || current.configurable))
      invalid = "trap reported non-configurability for a configurable or missing property";
    else if (!desc.configurable && !desc.accessor && !desc.writable && target_exists && current.writable)
      invalid = "trap reported non-configurable and writable property as non-writable";
    
    ant_value_t completed = invalid
      ? js_mkerr_typed(js, JS_ERR_TYPE, "'getOwnPropertyDescriptor' on proxy: %s", invalid)
      : desc.accessor
      ? js_from_accessor_descriptor(js, desc.first, desc.second, desc.enumerable, desc.configurable)
      : js_from_data_descriptor(js, desc.first, desc.writable, desc.enumerable, desc.configurable);
    GC_ROOT_RESTORE(js, mark);
    
    return completed;
  }

  ant_value_t args[2] = { target, key_val };
  return builtin_object_getOwnPropertyDescriptor(js, args, 2, js_mkundef());
}

static ant_value_t proxy_has_own(ant_t *js, ant_value_t proxy, ant_value_t key_val) {
  ant_value_t desc = proxy_get_own_property_descriptor(js, proxy, key_val);
  if (is_err(desc)) return desc;
  return js_bool(vtype(desc) != kTypeUndefined);
}

static ant_value_t proxy_host_define(
  ant_t *js, ant_proxy_state_t *data, const char *key, size_t key_len, ant_value_t descriptor
) {
  bool plain_data =
    descriptor_has_own_key(js, descriptor, "value", 5) &&
    descriptor_bool_value(js, descriptor, "configurable", 12, false) &&
    descriptor_bool_value(js, descriptor, "writable", 8, false) &&
    descriptor_bool_value(js, descriptor, "enumerable", 10, false);
  
  if (!plain_data) return js_mkerr_typed(
    js, JS_ERR_TYPE, 
    "'%s' only accepts a configurable, writable, and enumerable data descriptor", data->host->name
  );
  
  ant_value_t value = js_get(js, descriptor, "value");
  if (is_err(value)) return value;
  
  return data->host->set(js, data->target, key, key_len, value);
}

static ant_value_t proxy_define_property(ant_t *js, ant_value_t proxy, ant_value_t key_val, ant_value_t descriptor) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return js_false;
  
  if (data->revoked)
    return throw_proxy_error(js, "Cannot perform 'defineProperty' on a proxy that has been revoked");

  ant_value_t target = data->target;
  if (data->host && vtype(key_val) == kTypeSymbol) return js_mkerr_typed(js, JS_ERR_TYPE, "%s", "Cannot convert a Symbol value to a string");
  if (data->host && vtype(key_val) == kTypeString) {
    size_t key_len;
    const char *key = js_getstr(js, key_val, &key_len);
    if (key && !lkp(js, target, key, key_len).obj) return proxy_host_define(js, data, key, key_len, descriptor);
  }
  
  ant_value_t handler = data->handler;
  ant_value_t trap = proxy_get_method(js, handler, "defineProperty");
  if (is_err(trap)) return trap;
  if (vtype(trap) != kTypeUndefined) {
    ant_value_t args[3] = { target, key_val, descriptor };
    ant_value_t result = sv_vm_call(js->vm, js, trap, js_mkundef(), args, 3, NULL, js_mkundef());
    if (is_err(result) || !js_truthy(js, result)) return result;

    ant_value_t target_args[2] = { target, key_val };
    ant_value_t target_desc = builtin_object_getOwnPropertyDescriptor(js, target_args, 2, js_mkundef());
    if (is_err(target_desc)) return target_desc;

    bool target_exists = vtype(target_desc) != kTypeUndefined;
    bool target_configurable = target_exists
      ? descriptor_bool_value(js, target_desc, "configurable", 12, false)
      : true;
    bool target_extensible = proxy_target_is_extensible(target);
    bool desc_has_configurable = descriptor_has_own_key(js, descriptor, "configurable", 12);
    bool desc_configurable = descriptor_bool_value(js, descriptor, "configurable", 12, false);

    if (!target_exists && !target_extensible)
      return js_mkerr_typed(js, JS_ERR_TYPE, "'defineProperty' on proxy: trap returned truthy for adding property to non-extensible target");
    if (desc_has_configurable && !desc_configurable && (!target_exists || target_configurable))
      return js_mkerr_typed(js, JS_ERR_TYPE, "'defineProperty' on proxy: trap returned truthy for defining invalid non-configurable property");

    return js_true;
  }

  ant_value_t result = object_define_property(js, target, key_val, descriptor);
  if (is_err(result)) return result;
  
  return js_true;
}

static ant_value_t proxy_delete_val(ant_t *js, ant_value_t proxy, ant_value_t key_val) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return js_true;
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'deleteProperty' on a proxy that has been revoked");

  ant_value_t target = data->target;
  ant_value_t handler = data->handler;

  ant_value_t delete_trap = proxy_get_method(js, handler, "deleteProperty");
  if (is_err(delete_trap)) return delete_trap;
  if (vtype(delete_trap) != kTypeUndefined) {
      ant_value_t args[2] = { target, key_val };
      ant_value_t result = sv_vm_call(js->vm, js, delete_trap, js_mkundef(), args, 2, NULL, js_mkundef());
      if (is_err(result)) return result;
      if (js_truthy(js, result) && vtype(key_val) == kTypeSymbol) {
        ant_prop_loc_t prop_off = lkp_sym(target, (ant_offset_t)vdata(key_val));
        if (prop_off.obj && is_nonconfig_prop(prop_off))
          return js_mkerr_typed(js, JS_ERR_TYPE, "'deleteProperty' on proxy: trap returned truthy for non-configurable property");
      } return result;
  }
  if (vtype(key_val) == kTypeSymbol) return js_delete_sym_prop(js, target, key_val);
  return js_true;
}

ant_value_t js_proxy_apply(ant_t *js, ant_value_t proxy, ant_value_t this_arg, ant_value_t *args, int argc) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return js_mkerr_typed(js, JS_ERR_TYPE, "object is not a function");
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'apply' on a proxy that has been revoked");

  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  uint8_t target_type = vtype(target);

  if (target_type != kTypeFunction && target_type != kTypeBuiltin && !(target_type == kTypeObject && is_proxy(target)))
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s is not a function", typestr(target_type));

  ant_value_t trap = proxy_get_method(js, handler, "apply");
  if (is_err(trap)) return trap;
  if (vtype(trap) != kTypeUndefined) {
      ant_value_t args_arr = mkarr(js);
      for (int i = 0; i < argc; i++)
        js_arr_push(js, args_arr, args[i]);
      ant_value_t trap_args[3] = { target, this_arg, args_arr };
      return sv_vm_call(js->vm, js, trap, handler, trap_args, 3, NULL, js_mkundef());
  }

  return sv_vm_call(js->vm, js, target, this_arg, args, argc, NULL, js_mkundef());
}

ant_value_t js_proxy_construct(ant_t *js, ant_value_t proxy, ant_value_t *args, int argc, ant_value_t new_target) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  
  if (!data) return js_mkerr_typed(js, JS_ERR_TYPE, "not a constructor");
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'construct' on a proxy that has been revoked");

  ant_value_t target = data->target;
  ant_value_t handler = data->handler;

  if (!js_is_constructor(target))
    return js_mkerr_typed(js, JS_ERR_TYPE, "not a constructor");
  
  ant_value_t trap = proxy_get_method(js, handler, "construct");
  if (is_err(trap)) return trap;
  
  if (vtype(trap) != kTypeUndefined) {
      ant_value_t args_arr = mkarr(js);
      for (int i = 0; i < argc; i++)
        js_arr_push(js, args_arr, args[i]);
      ant_value_t trap_args[3] = { target, args_arr, new_target };
      ant_value_t result = sv_vm_call(js->vm, js, trap, js_mkundef(), trap_args, 3, NULL, js_mkundef());
      if (is_err(result)) return result;
      if (!is_object_type(result))
        return js_mkerr_typed(js, JS_ERR_TYPE, "'construct' on proxy: trap returned non-Object");
      return result;
  }

  if (vtype(target) == kTypeObject && is_proxy(target))
    return js_proxy_construct(js, target, args, argc, new_target);

  ant_value_t effective_new_target = new_target;
  ant_value_t proto;
  
  if (new_target != target) {
    proto = js_getprop_fallback(js, new_target, "prototype");
    if (!is_err(proto) && !is_object_type(proto)) proto = js->sym.object_proto;
  } else proto = sv_prepare_construct_meta(js, target, new_target, &effective_new_target);
  if (is_err(proto)) return proto;

  ant_value_t obj = mkobj(js, 0);
  if (is_object_type(proto)) js_set_proto_init(obj, proto);

  ant_value_t ctor_this = obj;
  ant_value_t result = sv_vm_call(
    js->vm, js, target, obj, args, 
    argc, &ctor_this, effective_new_target
  );
  
  if (is_err(result)) return result;
  
  return is_object_type(result)
    ? result
    : (is_object_type(ctor_this) ? ctor_this : obj);
}

static ant_value_t mkproxy(ant_t *js, ant_value_t target, ant_value_t handler) {
  ant_value_t proxy_obj = mkobj(js, 0);
  ant_object_t *proxy_ptr = js_obj_ptr(proxy_obj);
  if (!proxy_ptr) return js_mkerr(js, "out of memory");

  ant_proxy_state_t *data = (ant_proxy_state_t *)calloc(1, sizeof(ant_proxy_state_t));
  if (!data) return js_mkerr(js, "out of memory");

  data->target = target;
  data->handler = handler;
  data->revoked = false;

  proxy_ptr->flags.is_exotic = 1;
  proxy_ptr->flags.is_callable = is_callable(target) ? 1u : 0u;
  js_mark_constructor(proxy_obj, js_is_constructor(target));
  
  ant_object_sidecar_t *sidecar = ant_object_ensure_sidecar(proxy_ptr);
  if (!sidecar) {
    free(data);
    return js_mkerr(js, "out of memory");
  }
  
  sidecar->proxy_state = data;
  return proxy_obj;
}

ant_value_t js_mkproxy(ant_t *js, ant_value_t target, ant_value_t handler) {
  return mkproxy(js, target, handler);
}

ant_value_t js_mkproxy_host(ant_t *js, ant_value_t target, const ant_host_proxy_ops_t *ops) {
  ANT_ASSERT(ops && ops->name && ops->get && ops->set && ops->del && ops->keys, "host proxies need every hook");
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, target);
  
  ant_value_t handler = mkobj(js, 0);
  GC_ROOT_PIN(js, handler);
  
  ant_value_t proxy = mkproxy(js, target, handler);
  if (!is_err(proxy)) get_proxy_data(proxy)->host = ops;
  
  GC_ROOT_RESTORE(js, root_mark);
  return proxy;
}

static ant_value_t create_proxy_checked(ant_native_params_t, ant_value_t new_target, bool require_new) {
  if (require_new && vtype(new_target) == kTypeUndefined) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Proxy constructor requires 'new'");
  }
  if (nargs < 2) return js_mkerr(js, "Proxy requires two arguments: target and handler");

  ant_value_t target = args[0];
  ant_value_t handler = args[1];

  uint8_t target_type = vtype(target);
  if (target_type != kTypeObject && target_type != kTypeFunction && target_type != kTypeArray) {
    return js_mkerr(js, "Proxy target must be an object");
  }

  uint8_t handler_type = vtype(handler);
  if (handler_type != kTypeObject && handler_type != kTypeFunction) {
    return js_mkerr(js, "Proxy handler must be an object");
  }

  return mkproxy(js, target, handler);
}

static ant_value_t builtin_Proxy(ant_params_t) {
  return create_proxy_checked(js, args, nargs, call_new_target, true);
}

static ant_value_t proxy_revoke_fn(ant_params_t) {
  (void)args; (void)nargs;
  ant_value_t func = js->current_func;
  ant_value_t ref_slot = get_slot(func, SLOT_PROXY_REF);
  
  if (vtype(ref_slot) != kTypeUndefined && vdata(ref_slot) != 0) {
    ant_proxy_state_t *data = get_proxy_data(ref_slot);
    if (data) data->revoked = true;
  }
  
  return js_mkundef();
}

static ant_value_t builtin_Proxy_revocable(ant_params_t) {
  ant_value_t proxy = create_proxy_checked(js, args, nargs, js_mkundef(), false);
  if (is_err(proxy)) return proxy;
  
  ant_value_t revoke_obj = mkobj(js, 0);
  set_slot(revoke_obj, SLOT_CFUNC, Ant_Function_Create(proxy_revoke_fn));
  set_slot(revoke_obj, SLOT_PROXY_REF, proxy);
  
  ant_value_t revoke_func = js_obj_to_func(js, revoke_obj);
  ant_value_t result = Ant_Object_Create(js);
  
  js_setprop(js, result, js_mkstr(js, "proxy", 5), proxy);
  js_setprop(js, result, js_mkstr(js, "revoke", 6), revoke_func);
  
  return result;
}

static inline void isolate_arenas_destroy(ant_t *js) {
  fixed_arena_destroy(&js->upvalue_arena);
  fixed_arena_destroy(&js->closure_arena);
  fixed_arena_destroy(&js->obj_arena);
}

static inline bool isolate_arenas_init(ant_t *js) {
  if (
    !fixed_arena_init(&js->obj_arena, sizeof(ant_object_t), offsetof(ant_object_t, mark_epoch), ANT_ARENA_MAX)           ||
    !fixed_arena_init(&js->closure_arena, sizeof(sv_closure_t), offsetof(sv_closure_t, gc_epoch), ANT_CLOSURE_ARENA_MAX) ||
    !fixed_arena_init(&js->upvalue_arena, sizeof(sv_upvalue_t), offsetof(sv_upvalue_t, gc_epoch), ANT_UPVALUE_ARENA_MAX)
  ) {
    isolate_arenas_destroy(js);
    return false;
  }

  return true;
}

static void init_function_kind(
  ant_t *js, ant_value_t function_proto, ant_value_t function_ctor, internal_slot_t slot,
  ant_value_t cfunc, ant_value_t name, ant_value_t instance_proto, bool is_async
) {
  ant_value_t kind_proto = Ant_Object_CreateBare(js);
  set_proto(js, kind_proto, function_proto);
  if (is_async) set_slot(kind_proto, SLOT_ASYNC, js_true);
  set_slot(js_glob(js), slot, kind_proto);
  
  ant_value_t ctor_obj = mkobj(js, 0);
  set_proto(js, ctor_obj, function_ctor);
  set_slot(ctor_obj, SLOT_CFUNC, cfunc);
  
  mkprop(js, ctor_obj, js->length_str, tov(1.0), ANT_PROP_ATTR_CONFIGURABLE);
  mkprop(js, ctor_obj, ANT_STRING("name"), name, ANT_PROP_ATTR_CONFIGURABLE);
  mkprop_exact_attrs(js, ctor_obj, ANT_STRING("prototype"), kind_proto, 0);
  
  if (is_object_type(instance_proto)) {
    mkprop(js, kind_proto, ANT_STRING("prototype"), instance_proto, ANT_PROP_ATTR_CONFIGURABLE);
    mkprop(js, instance_proto, ANT_STRING("constructor"), kind_proto, ANT_PROP_ATTR_CONFIGURABLE);
  }
  
  mkprop(js, kind_proto, ANT_STRING("constructor"), js_obj_to_func(js, ctor_obj), ANT_PROP_ATTR_CONFIGURABLE);
}

static void realm_init_core(ant_t *js) {
  js->global = mkobj(js, 0);
  js->global_this = js->global;
  js->global_sandbox = js_mkundef();

  ant_value_t glob = js->global;
  ant_value_t object_proto = Ant_Object_CreateBare(js);
  set_proto(js, object_proto, js_mknull());
  
  defmethod(js, object_proto, "toString", 8, Ant_Function_Create(builtin_object_toString));
  defmethod(js, object_proto, "valueOf", 7, Ant_Function_Create(builtin_object_valueOf));
  defmethod(js, object_proto, "toLocaleString", 14, Ant_Function_Create(builtin_object_toLocaleString));
  defmethod(js, object_proto, "hasOwnProperty", 14, Ant_Function_CreateWithArity(builtin_object_hasOwnProperty, 1));
  defmethod(js, object_proto, "isPrototypeOf", 13, Ant_Function_CreateWithArity(builtin_object_isPrototypeOf, 1));
  defmethod(js, object_proto, "propertyIsEnumerable", 20, Ant_Function_CreateWithArity(builtin_object_propertyIsEnumerable, 1));
  defmethod(js, object_proto, "__defineGetter__", 16, Ant_Function_CreateWithArity(builtin_object___defineGetter__, 2));
  defmethod(js, object_proto, "__defineSetter__", 16, Ant_Function_CreateWithArity(builtin_object___defineSetter__, 2));
  defmethod(js, object_proto, "__lookupGetter__", 16, Ant_Function_CreateWithArity(builtin_object___lookupGetter__, 1));
  defmethod(js, object_proto, "__lookupSetter__", 16, Ant_Function_CreateWithArity(builtin_object___lookupSetter__, 1));
  
  ant_value_t proto_getter = Ant_Function_Create(builtin_proto_getter);
  ant_value_t proto_setter = Ant_Function_Create(builtin_proto_setter);
  js_set_accessor_desc(
    js, object_proto, "__proto__", sizeof("__proto__") - 1,
    proto_getter, proto_setter, JS_DESC_C
  );
  
  ant_value_t function_proto_obj = Ant_Object_CreateBare(js);
  set_proto(js, function_proto_obj, object_proto);
  set_slot(function_proto_obj, SLOT_CFUNC, Ant_Function_Create(builtin_function_empty));
  
  defmethod(js, function_proto_obj, "call", 4, Ant_Function_CreateWithArity(builtin_function_call, 1));
  defmethod(js, function_proto_obj, "apply", 5, Ant_Function_CreateWithArity(builtin_function_apply, 2));
  defmethod(js, function_proto_obj, "bind", 4, Ant_Function_CreateWithArity(builtin_function_bind, 1));
  defmethod(js, function_proto_obj, "toString", 8, Ant_Function_Create(builtin_function_toString));
  
  ant_value_t function_proto = js_obj_to_func(js, function_proto_obj);
  set_slot(glob, SLOT_FUNC_PROTO, function_proto);
  js_define_function_length(js, function_proto_obj, 0);
  js_set_function_name(js, function_proto, "", 0);
  
  ant_value_t array_proto = alloc_array_with_proto(js, object_proto);
  
  js->sym.array_push_fn = defmethod(js, array_proto, "push", 4, Ant_Function_CreateWithArity(builtin_array_push, 1));
  defmethod(js, array_proto, "pop", 3, Ant_Function_Create(builtin_array_pop));
  defmethod(js, array_proto, "slice", 5, Ant_Function_CreateWithArity(builtin_array_slice, 2));
  defmethod(js, array_proto, "join", 4, Ant_Function_CreateWithArity(builtin_array_join, 1));
  defmethod(js, array_proto, "includes", 8, Ant_Function_CreateWithArity(builtin_array_includes, 1));
  defmethod(js, array_proto, "every", 5, Ant_Function_CreateWithArity(builtin_array_every, 1));
  defmethod(js, array_proto, "reverse", 7, Ant_Function_Create(builtin_array_reverse));
  defmethod(js, array_proto, "map", 3, Ant_Function_CreateWithArity(builtin_array_map, 1));
  defmethod(js, array_proto, "filter", 6, Ant_Function_CreateWithArity(builtin_array_filter, 1));
  defmethod(js, array_proto, "reduce", 6, Ant_Function_CreateWithArity(builtin_array_reduce, 1));
  defmethod(js, array_proto, "flat", 4, Ant_Function_Create(builtin_array_flat));
  defmethod(js, array_proto, "concat", 6, Ant_Function_CreateWithArity(builtin_array_concat, 1));
  defmethod(js, array_proto, "at", 2, Ant_Function_CreateWithArity(builtin_array_at, 1));
  defmethod(js, array_proto, "fill", 4, Ant_Function_CreateWithArity(builtin_array_fill, 1));
  defmethod(js, array_proto, "find", 4, Ant_Function_CreateWithArity(builtin_array_find, 1));
  defmethod(js, array_proto, "findIndex", 9, Ant_Function_CreateWithArity(builtin_array_findIndex, 1));
  defmethod(js, array_proto, "findLast", 8, Ant_Function_CreateWithArity(builtin_array_findLast, 1));
  defmethod(js, array_proto, "findLastIndex", 13, Ant_Function_CreateWithArity(builtin_array_findLastIndex, 1));
  defmethod(js, array_proto, "flatMap", 7, Ant_Function_CreateWithArity(builtin_array_flatMap, 1));
  defmethod(js, array_proto, "forEach", 7, Ant_Function_CreateWithArity(builtin_array_forEach, 1));
  defmethod(js, array_proto, "indexOf", 7, Ant_Function_CreateWithArity(builtin_array_indexOf, 1));
  defmethod(js, array_proto, "lastIndexOf", 11, Ant_Function_CreateWithArity(builtin_array_lastIndexOf, 1));
  defmethod(js, array_proto, "reduceRight", 11, Ant_Function_CreateWithArity(builtin_array_reduceRight, 1));
  defmethod(js, array_proto, "shift", 5, Ant_Function_Create(builtin_array_shift));
  defmethod(js, array_proto, "unshift", 7, Ant_Function_CreateWithArity(builtin_array_unshift, 1));
  defmethod(js, array_proto, "some", 4, Ant_Function_CreateWithArity(builtin_array_some, 1));
  defmethod(js, array_proto, "sort", 4, Ant_Function_CreateWithArity(builtin_array_sort, 1));
  defmethod(js, array_proto, "splice", 6, Ant_Function_CreateWithArity(builtin_array_splice, 2));
  defmethod(js, array_proto, "copyWithin", 10, Ant_Function_CreateWithArity(builtin_array_copyWithin, 2));
  defmethod(js, array_proto, "toReversed", 10, Ant_Function_Create(builtin_array_toReversed));
  defmethod(js, array_proto, "toSorted", 8, Ant_Function_CreateWithArity(builtin_array_toSorted, 1));
  defmethod(js, array_proto, "toSpliced", 9, Ant_Function_CreateWithArity(builtin_array_toSpliced, 2));
  defmethod(js, array_proto, "with", 4, Ant_Function_CreateWithArity(builtin_array_with, 2));
  defmethod(js, array_proto, "keys", 4, Ant_Function_Create(builtin_array_keys));
  js->sym.array_values_fn = defmethod(js, array_proto, "values", 6, Ant_Function_Create(builtin_array_values));
  defmethod(js, array_proto, "entries", 7, Ant_Function_Create(builtin_array_entries));
  defmethod(js, array_proto, "toString", 8, Ant_Function_Create(builtin_array_toString));
  defmethod(js, array_proto, "toLocaleString", 14, Ant_Function_Create(builtin_array_toLocaleString));
  
  ant_value_t string_proto = Ant_Object_CreateBare(js);
  set_proto(js, string_proto, object_proto);
  set_slot(string_proto, SLOT_PRIMITIVE, js_mkstr(js, "", 0));
  
  defmethod(js, string_proto, "indexOf", 7, Ant_Function_CreateWithArity(builtin_string_indexOf, 1));
  defmethod(js, string_proto, "substring", 9, Ant_Function_CreateWithArity(builtin_string_substring, 2));
  defmethod(js, string_proto, "substr", 6, Ant_Function_CreateWithArity(builtin_string_substr, 2));
  defmethod(js, string_proto, "split", 5, Ant_Function_CreateWithArity(builtin_string_split, 2));
  defmethod(js, string_proto, "slice", 5, Ant_Function_CreateWithArity(builtin_string_slice, 2));
  defmethod(js, string_proto, "includes", 8, Ant_Function_CreateWithArity(builtin_string_includes, 1));
  defmethod(js, string_proto, "startsWith", 10, Ant_Function_CreateWithArity(builtin_string_startsWith, 1));
  defmethod(js, string_proto, "endsWith", 8, Ant_Function_CreateWithArity(builtin_string_endsWith, 1));
  defmethod(js, string_proto, "template", 8, Ant_Function_Create(builtin_string_template));
  defmethod(js, string_proto, "charCodeAt", 10, Ant_Function_CreateWithArity(builtin_string_charCodeAt, 1));
  defmethod(js, string_proto, "codePointAt", 11, Ant_Function_CreateWithArity(builtin_string_codePointAt, 1));
  defmethod(js, string_proto, "toLowerCase", 11, Ant_Function_Create(builtin_string_toLowerCase));
  defmethod(js, string_proto, "toUpperCase", 11, Ant_Function_Create(builtin_string_toUpperCase));
  defmethod(js, string_proto, "toLocaleLowerCase", 17, Ant_Function_Create(builtin_string_toLowerCase));
  defmethod(js, string_proto, "toLocaleUpperCase", 17, Ant_Function_Create(builtin_string_toUpperCase));
  defmethod(js, string_proto, "trim", 4, Ant_Function_Create(builtin_string_trim));
  defmethod(js, string_proto, "trimStart", 9, Ant_Function_Create(builtin_string_trimStart));
  defmethod(js, string_proto, "trimEnd", 7, Ant_Function_Create(builtin_string_trimEnd));
  defalias_of(js, string_proto, "trimLeft", "trimStart");
  defalias_of(js, string_proto, "trimRight", "trimEnd");
  defmethod(js, string_proto, "repeat", 6, Ant_Function_CreateWithArity(builtin_string_repeat, 1));
  defmethod(js, string_proto, "padStart", 8, Ant_Function_CreateWithArity(builtin_string_padStart, 1));
  defmethod(js, string_proto, "padEnd", 6, Ant_Function_CreateWithArity(builtin_string_padEnd, 1));
  defmethod(js, string_proto, "charAt", 6, Ant_Function_CreateWithArity(builtin_string_charAt, 1));
  defmethod(js, string_proto, "at", 2, Ant_Function_CreateWithArity(builtin_string_at, 1));
  defmethod(js, string_proto, "lastIndexOf", 11, Ant_Function_CreateWithArity(builtin_string_lastIndexOf, 1));
  defmethod(js, string_proto, "concat", 6, Ant_Function_CreateWithArity(builtin_string_concat, 1));
  defmethod(js, string_proto, "localeCompare", 13, Ant_Function_CreateWithArity(builtin_string_localeCompare, 1));
  defmethod(js, string_proto, "normalize", 9, Ant_Function_Create(builtin_string_normalize));
  defmethod(js, string_proto, "valueOf", 7, Ant_Function_Create(builtin_string_valueOf));
  defmethod(js, string_proto, "toString", 8, Ant_Function_Create(builtin_string_toString));
  defmethod(js, string_proto, "anchor", 6, Ant_Function_CreateWithArity(builtin_string_anchor, 1));
  defmethod(js, string_proto, "big", 3, Ant_Function_Create(builtin_string_big));
  defmethod(js, string_proto, "bold", 4, Ant_Function_Create(builtin_string_bold));
  defmethod(js, string_proto, "fixed", 5, Ant_Function_Create(builtin_string_fixed));
  defmethod(js, string_proto, "fontcolor", 9, Ant_Function_CreateWithArity(builtin_string_fontcolor, 1));
  defmethod(js, string_proto, "fontsize", 8, Ant_Function_CreateWithArity(builtin_string_fontsize, 1));
  defmethod(js, string_proto, "italics", 7, Ant_Function_Create(builtin_string_italics));
  defmethod(js, string_proto, "link", 4, Ant_Function_CreateWithArity(builtin_string_link, 1));
  defmethod(js, string_proto, "small", 5, Ant_Function_Create(builtin_string_small));
  defmethod(js, string_proto, "strike", 6, Ant_Function_Create(builtin_string_strike));
  defmethod(js, string_proto, "sub", 3, Ant_Function_Create(builtin_string_sub));
  defmethod(js, string_proto, "sup", 3, Ant_Function_Create(builtin_string_sup));

  ant_value_t number_proto = Ant_Object_CreateBare(js);
  set_proto(js, number_proto, object_proto);
  set_slot(number_proto, SLOT_PRIMITIVE, tov(0.0));
  
  js->sym.number_to_string_fn = defmethod(js, number_proto, "toString", 8, Ant_Function_CreateWithArity(builtin_number_toString, 1));
  defmethod(js, number_proto, "toFixed", 7, Ant_Function_CreateWithArity(builtin_number_toFixed, 1));
  defmethod(js, number_proto, "toPrecision", 11, Ant_Function_CreateWithArity(builtin_number_toPrecision, 1));
  defmethod(js, number_proto, "toExponential", 13, Ant_Function_CreateWithArity(builtin_number_toExponential, 1));
  defmethod(js, number_proto, "valueOf", 7, Ant_Function_Create(builtin_number_valueOf));
  defmethod(js, number_proto, "toLocaleString", 14, Ant_Function_Create(builtin_number_toLocaleString));
  
  ant_value_t boolean_proto = Ant_Object_CreateBare(js);
  set_proto(js, boolean_proto, object_proto);
  set_slot(boolean_proto, SLOT_PRIMITIVE, js_false);
  
  defmethod(js, boolean_proto, "valueOf", 7, Ant_Function_Create(builtin_boolean_valueOf));
  defmethod(js, boolean_proto, "toString", 8, Ant_Function_Create(builtin_boolean_toString));
  
  ant_value_t error_proto = Ant_Object_CreateBare(js);
  set_proto(js, error_proto, object_proto);
  
  mkprop_bytes(js, error_proto, "name", 4, ANT_STRING("Error"), JS_DESC_W | JS_DESC_C);
  mkprop_bytes(js, error_proto, "message", 7, js_mkstr(js, "", 0), JS_DESC_W | JS_DESC_C);
  defmethod(js, error_proto, "toString", 8, Ant_Function_Create(builtin_Error_toString));
  js->builtins.error_protos[JS_ERR_GENERIC] = error_proto;
  
  ant_value_t err_ctor_obj = mkobj(js, 0);
  set_proto(js, err_ctor_obj, function_proto);
  set_slot(err_ctor_obj, SLOT_CFUNC, Ant_Function_CreateWithArity(builtin_Error, 1));
  js_setprop_readonly_nonconfigurable(js, err_ctor_obj, "prototype", 9, error_proto);
  js_set_function_name(js, err_ctor_obj, "Error", sizeof("Error") - 1);
  
  ant_value_t err_ctor_func = js_obj_to_func(js, err_ctor_obj);
  js_set_global_builtin(js, "Error", err_ctor_func);
  js->builtins.error_ctor = err_ctor_func;
  defhidden(js, error_proto, "constructor", 11, err_ctor_func);
  defmethod(js, err_ctor_func, "isError", 7, Ant_Function_CreateWithArity(builtin_error_isError, 1));
  defmethod(js, err_ctor_func, "captureStackTrace", 17, Ant_Function_CreateWithArity(builtin_error_captureStackTrace, 2));
  js_setprop(js, err_ctor_func, ANT_STRING("stackTraceLimit"), js_mknum(10));
  
  #define REGISTER_ERROR_SUBTYPE(err_type, name_str) do { \
    ant_value_t ctor_func = Ant_Error_CreateConstructor(js, name_str, &js->builtins.error_protos[err_type]); \
    js_set_global_builtin(js, name_str, ctor_func); \
  } while(0)
  
  REGISTER_ERROR_SUBTYPE(JS_ERR_EVAL, "EvalError");
  REGISTER_ERROR_SUBTYPE(JS_ERR_RANGE, "RangeError");
  REGISTER_ERROR_SUBTYPE(JS_ERR_REFERENCE, "ReferenceError");
  REGISTER_ERROR_SUBTYPE(JS_ERR_SYNTAX, "SyntaxError");
  REGISTER_ERROR_SUBTYPE(JS_ERR_TYPE, "TypeError");
  REGISTER_ERROR_SUBTYPE(JS_ERR_URI, "URIError");
  REGISTER_ERROR_SUBTYPE(JS_ERR_INTERNAL, "InternalError");
  
  #undef REGISTER_ERROR_SUBTYPE
  
  ant_value_t aggregate_proto = Ant_Object_CreateBare(js);
  set_proto(js, aggregate_proto, error_proto);
  mkprop_bytes(js, aggregate_proto, "name", 4, ANT_STRING("AggregateError"), JS_DESC_W | JS_DESC_C);
  js->builtins.error_protos[JS_ERR_AGGREGATE] = aggregate_proto;
  
  ant_value_t aggregate_ctor = mkobj(js, 0);
  set_proto(js, aggregate_ctor, err_ctor_func);
  set_slot(aggregate_ctor, SLOT_CFUNC, Ant_Function_CreateWithArity(builtin_AggregateError, 2));
  js_setprop_readonly_nonconfigurable(js, aggregate_ctor, "prototype", 9, aggregate_proto);
  js_set_function_name(js, aggregate_ctor, "AggregateError", sizeof("AggregateError") - 1);
  
  ant_value_t aggregate_ctor_func = js_obj_to_func(js, aggregate_ctor);
  defhidden(js, aggregate_proto, "constructor", 11, aggregate_ctor_func);
  js_set_global_builtin(js, "AggregateError", aggregate_ctor_func);

  ant_value_t suppressed_proto = Ant_Object_CreateBare(js);
  set_proto(js, suppressed_proto, error_proto);
  mkprop_bytes(js, suppressed_proto, "name", 4, ANT_STRING("SuppressedError"), JS_DESC_W | JS_DESC_C);
  js->builtins.suppressed_error_proto = suppressed_proto;
  
  ant_value_t suppressed_ctor = mkobj(js, 0);
  set_proto(js, suppressed_ctor, err_ctor_func);
  set_slot(suppressed_ctor, SLOT_CFUNC, Ant_Function_CreateWithArity(builtin_SuppressedError, 3));
  js_setprop_readonly_nonconfigurable(js, suppressed_ctor, "prototype", 9, suppressed_proto);
  js_set_function_name(js, suppressed_ctor, "SuppressedError", sizeof("SuppressedError") - 1);
  
  ant_value_t suppressed_ctor_func = js_obj_to_func(js, suppressed_ctor);
  defhidden(js, suppressed_proto, "constructor", 11, suppressed_ctor_func);
  js_set_global_builtin(js, "SuppressedError", suppressed_ctor_func);

  ant_value_t disposable_stack_proto = Ant_Object_CreateBare(js);
  set_proto(js, disposable_stack_proto, object_proto);
  js->builtins.disposable_stack_proto = disposable_stack_proto;
  defmethod(js, disposable_stack_proto, "use", 3, Ant_Function_CreateWithArity(builtin_DisposableStack_use, 1));
  defmethod(js, disposable_stack_proto, "adopt", 5, Ant_Function_CreateWithArity(builtin_DisposableStack_adopt, 2));
  defmethod(js, disposable_stack_proto, "defer", 5, Ant_Function_CreateWithArity(builtin_DisposableStack_defer, 1));
  defmethod(js, disposable_stack_proto, "move", 4, Ant_Function_Create(builtin_DisposableStack_move));
  defmethod(js, disposable_stack_proto, "dispose", 7, Ant_Function_Create(builtin_DisposableStack_dispose));

  ant_value_t disposable_stack_ctor = mkobj(js, 0);
  set_proto(js, disposable_stack_ctor, function_proto);
  set_slot(disposable_stack_ctor, SLOT_CFUNC, Ant_Function_Create(builtin_DisposableStack));
  js_setprop_readonly_nonconfigurable(js, disposable_stack_ctor, "prototype", 9, disposable_stack_proto);
  js_set_function_name(js, disposable_stack_ctor, "DisposableStack", sizeof("DisposableStack") - 1);
  
  ant_value_t disposable_stack_ctor_func = js_obj_to_func(js, disposable_stack_ctor);
  defhidden(js, disposable_stack_proto, "constructor", 11, disposable_stack_ctor_func);
  js_set_global_builtin(js, "DisposableStack", disposable_stack_ctor_func);

  ant_value_t async_disposable_stack_proto = Ant_Object_CreateBare(js);
  set_proto(js, async_disposable_stack_proto, object_proto);
  js->builtins.async_disposable_stack_proto = async_disposable_stack_proto;
  defmethod(js, async_disposable_stack_proto, "use", 3, Ant_Function_CreateWithArity(builtin_AsyncDisposableStack_use, 1));
  defmethod(js, async_disposable_stack_proto, "adopt", 5, Ant_Function_CreateWithArity(builtin_AsyncDisposableStack_adopt, 2));
  defmethod(js, async_disposable_stack_proto, "defer", 5, Ant_Function_CreateWithArity(builtin_AsyncDisposableStack_defer, 1));
  defmethod(js, async_disposable_stack_proto, "move", 4, Ant_Function_Create(builtin_AsyncDisposableStack_move));
  defmethod(js, async_disposable_stack_proto, "disposeAsync", 12, Ant_Function_Create(builtin_AsyncDisposableStack_disposeAsync));

  ant_value_t async_disposable_stack_ctor = mkobj(js, 0);
  set_proto(js, async_disposable_stack_ctor, function_proto);
  set_slot(async_disposable_stack_ctor, SLOT_CFUNC, Ant_Function_Create(builtin_AsyncDisposableStack));
  js_setprop_readonly_nonconfigurable(js, async_disposable_stack_ctor, "prototype", 9, async_disposable_stack_proto);
  js_set_function_name(js, async_disposable_stack_ctor, "AsyncDisposableStack", sizeof("AsyncDisposableStack") - 1);
  
  ant_value_t async_disposable_stack_ctor_func = js_obj_to_func(js, async_disposable_stack_ctor);
  defhidden(js, async_disposable_stack_proto, "constructor", 11, async_disposable_stack_ctor_func);
  js_set_global_builtin(js, "AsyncDisposableStack", async_disposable_stack_ctor_func);
  
  ant_value_t promise_proto = Ant_Object_CreateBare(js);
  set_proto(js, promise_proto, object_proto);
  js->sym.promise_then = defmethod(js, promise_proto, "then", 4, Ant_Function_CreateWithArity(builtin_promise_then, 2));
  defmethod(js, promise_proto, "catch", 5, Ant_Function_CreateWithArity(builtin_promise_catch, 1));
  defmethod(js, promise_proto, "finally", 7, Ant_Function_CreateWithArity(builtin_promise_finally, 1));
  
  ant_value_t obj_func_obj = mkobj(js, 0);
  set_proto(js, obj_func_obj, function_proto);
  set_slot(obj_func_obj, SLOT_BUILTIN, tov(BUILTIN_OBJECT));
  js_mark_constructor(obj_func_obj, true);
  
  defmethod(js, obj_func_obj, "keys", 4, Ant_Function_CreateWithArity(builtin_object_keys, 1));
  defmethod(js, obj_func_obj, "values", 6, Ant_Function_CreateWithArity(builtin_object_values, 1));
  defmethod(js, obj_func_obj, "entries", 7, Ant_Function_CreateWithArity(builtin_object_entries, 1));
  defmethod(js, obj_func_obj, "is", 2, Ant_Function_CreateWithArity(builtin_object_is, 2));
  defmethod(js, obj_func_obj, "getPrototypeOf", 14, Ant_Function_CreateWithArity(builtin_object_getPrototypeOf, 1));
  defmethod(js, obj_func_obj, "setPrototypeOf", 14, Ant_Function_CreateWithArity(builtin_object_setPrototypeOf, 2));
  defmethod(js, obj_func_obj, "create", 6, Ant_Function_CreateWithArity(builtin_object_create, 2));
  defmethod(js, obj_func_obj, "hasOwn", 6, Ant_Function_CreateWithArity(builtin_object_hasOwn, 2));
  defmethod(js, obj_func_obj, "groupBy", 7, Ant_Function_CreateWithArity(builtin_object_groupBy, 2));
  defmethod(js, obj_func_obj, "defineProperty", 14, Ant_Function_CreateWithArity(builtin_object_defineProperty, 3));
  defmethod(js, obj_func_obj, "defineProperties", 16, Ant_Function_CreateWithArity(builtin_object_defineProperties, 2));
  defmethod(js, obj_func_obj, "assign", 6, Ant_Function_CreateWithArity(builtin_object_assign, 2));
  defmethod(js, obj_func_obj, "freeze", 6, Ant_Function_CreateWithArity(builtin_object_freeze, 1));
  defmethod(js, obj_func_obj, "isFrozen", 8, Ant_Function_CreateWithArity(builtin_object_isFrozen, 1));
  defmethod(js, obj_func_obj, "seal", 4, Ant_Function_CreateWithArity(builtin_object_seal, 1));
  defmethod(js, obj_func_obj, "isSealed", 8, Ant_Function_CreateWithArity(builtin_object_isSealed, 1));
  defmethod(js, obj_func_obj, "fromEntries", 11, Ant_Function_CreateWithArity(builtin_object_fromEntries, 1));
  defmethod(js, obj_func_obj, "getOwnPropertyDescriptor", 24, Ant_Function_CreateWithArity(builtin_object_getOwnPropertyDescriptor, 2));
  defmethod(js, obj_func_obj, "getOwnPropertyDescriptors", 25, Ant_Function_CreateWithArity(builtin_object_getOwnPropertyDescriptors, 1));
  defmethod(js, obj_func_obj, "getOwnPropertyNames", 19, Ant_Function_CreateWithArity(builtin_object_getOwnPropertyNames, 1));
  defmethod(js, obj_func_obj, "getOwnPropertySymbols", 21, Ant_Function_CreateWithArity(builtin_object_getOwnPropertySymbols, 1));
  defmethod(js, obj_func_obj, "isExtensible", 12, Ant_Function_CreateWithArity(builtin_object_isExtensible, 1));
  defmethod(js, obj_func_obj, "preventExtensions", 17, Ant_Function_CreateWithArity(builtin_object_preventExtensions, 1));
  
  js_define_function_length(js, obj_func_obj, 1);
  js_set_function_name(js, obj_func_obj, "Object", sizeof("Object") - 1);
  js_setprop_readonly_nonconfigurable(js, obj_func_obj, "prototype", 9, object_proto);
  ant_value_t obj_func = js_obj_to_func(js, obj_func_obj);
  js_set_global_builtin(js, "Object", obj_func);
  
  ant_value_t func_ctor_obj = mkobj(js, 0);
  set_proto(js, func_ctor_obj, function_proto);
  set_slot(func_ctor_obj, SLOT_CFUNC, Ant_Function_CreateWithArity(builtin_Function, 1));
  js_setprop_readonly_nonconfigurable(js, func_ctor_obj, "prototype", 9, function_proto);
  js_set_function_name(js, func_ctor_obj, "Function", sizeof("Function") - 1);
  ant_value_t func_ctor_func = js_obj_to_func(js, func_ctor_obj);
  js_set_global_builtin(js, "Function", func_ctor_func);
  
  js->sym.generator_proto = Ant_Object_CreateBare(js);
  js->sym.async_generator_proto = Ant_Object_CreateBare(js);
  
  init_function_kind(js, function_proto, func_ctor_func, SLOT_ASYNC_PROTO, Ant_Function_Create(builtin_AsyncFunction), ANT_STRING("AsyncFunction"), js_mkundef(), true);
  init_function_kind(js, function_proto, func_ctor_func, SLOT_GENERATOR_PROTO, Ant_Function_Create(builtin_GeneratorFunction), ANT_STRING("GeneratorFunction"), js->sym.generator_proto, false);
  init_function_kind(js, function_proto, func_ctor_func, SLOT_ASYNC_GENERATOR_PROTO, Ant_Function_Create(builtin_AsyncGeneratorFunction), ANT_STRING("AsyncGeneratorFunction"), js->sym.async_generator_proto, true);
  
  ant_value_t str_ctor_obj = mkobj(js, 0);
  set_proto(js, str_ctor_obj, function_proto);
  set_slot(str_ctor_obj, SLOT_CFUNC, Ant_Function_CreateWithArity(builtin_String, 1));
  js_setprop_readonly_nonconfigurable(js, str_ctor_obj, "prototype", 9, string_proto);
  defmethod(js, str_ctor_obj, "fromCharCode", 12, Ant_Function_CreateWithArity(builtin_string_fromCharCode, 1));
  defmethod(js, str_ctor_obj, "fromCodePoint", 13, Ant_Function_CreateWithArity(builtin_string_fromCodePoint, 1));
  defmethod(js, str_ctor_obj, "raw", 3, Ant_Function_CreateWithArity(builtin_string_raw, 1));
  js_set_function_name(js, str_ctor_obj, "String", sizeof("String") - 1);
  
  ant_value_t str_ctor_func = js_obj_to_func(js, str_ctor_obj);
  js->sym.string_ctor = str_ctor_func;
  js_set_global_builtin(js, "String", str_ctor_func);
  
  ant_value_t number_ctor_obj = mkobj(js, 0);
  set_proto(js, number_ctor_obj, function_proto);
  
  set_slot(number_ctor_obj, SLOT_CFUNC, Ant_Function_CreateWithArity(builtin_Number, 1));
  defmethod(js, number_ctor_obj, "isNaN", 5, Ant_Function_CreateWithArity(builtin_Number_isNaN, 1));
  defmethod(js, number_ctor_obj, "isFinite", 8, Ant_Function_CreateWithArity(builtin_Number_isFinite, 1));
  defmethod(js, number_ctor_obj, "isInteger", 9, Ant_Function_CreateWithArity(builtin_Number_isInteger, 1));
  defmethod(js, number_ctor_obj, "isSafeInteger", 13, Ant_Function_CreateWithArity(builtin_Number_isSafeInteger, 1));
  ant_value_t number_parse_int = defmethod(js, number_ctor_obj, "parseInt", 8, Ant_Function_CreateWithArity(builtin_parseInt, 2));
  ant_value_t number_parse_float = defmethod(js, number_ctor_obj, "parseFloat", 10, Ant_Function_CreateWithArity(builtin_parseFloat, 1));
  
  js_setprop_readonly_nonconfigurable(js, number_ctor_obj, "MAX_VALUE", 9, tov(1.7976931348623157e+308));
  js_setprop_readonly_nonconfigurable(js, number_ctor_obj, "MIN_VALUE", 9, tov(5e-324));
  js_setprop_readonly_nonconfigurable(js, number_ctor_obj, "MAX_SAFE_INTEGER", 16, tov(9007199254740991.0));
  js_setprop_readonly_nonconfigurable(js, number_ctor_obj, "MIN_SAFE_INTEGER", 16, tov(-9007199254740991.0));
  js_setprop_readonly_nonconfigurable(js, number_ctor_obj, "POSITIVE_INFINITY", 17, tov(JS_INF));
  js_setprop_readonly_nonconfigurable(js, number_ctor_obj, "NEGATIVE_INFINITY", 17, tov(JS_NEG_INF));
  js_setprop_readonly_nonconfigurable(js, number_ctor_obj, "NaN", 3, tov(JS_NAN));
  js_setprop_readonly_nonconfigurable(js, number_ctor_obj, "EPSILON", 7, tov(2.220446049250313e-16));
  
  js_setprop_readonly_nonconfigurable(js, number_ctor_obj, "prototype", 9, number_proto);
  js_set_function_name(js, number_ctor_obj, "Number", sizeof("Number") - 1);
  ant_value_t number_ctor_func = js_obj_to_func(js, number_ctor_obj);
  js_set_global_builtin(js, "Number", number_ctor_func);
  
  ant_value_t bool_ctor_obj = mkobj(js, 0);
  set_proto(js, bool_ctor_obj, function_proto);
  set_slot(bool_ctor_obj, SLOT_CFUNC, Ant_Function_CreateWithArity(builtin_Boolean, 1));
  js_setprop_readonly_nonconfigurable(js, bool_ctor_obj, "prototype", 9, boolean_proto);
  js_set_function_name(js, bool_ctor_obj, "Boolean", sizeof("Boolean") - 1);
  ant_value_t bool_ctor_func = js_obj_to_func(js, bool_ctor_obj);
  js_set_global_builtin(js, "Boolean", bool_ctor_func);
  
  ant_value_t arr_ctor_obj = mkobj(js, 0);
  set_proto(js, arr_ctor_obj, function_proto);
  set_slot(arr_ctor_obj, SLOT_CFUNC, Ant_Function_CreateWithArity(builtin_Array, 1));
  js_setprop_readonly_nonconfigurable(js, arr_ctor_obj, "prototype", 9, array_proto);
  defmethod(js, arr_ctor_obj, "isArray", 7, Ant_Function_CreateWithArity(builtin_Array_isArray, 1));
  defmethod(js, arr_ctor_obj, "isTemplateObject", 16, Ant_Function_Create(builtin_Array_isTemplateObject));
  defmethod(js, arr_ctor_obj, "from", 4, Ant_Function_CreateWithArity(builtin_Array_from, 1));
  defmethod(js, arr_ctor_obj, "fromAsync", 9, Ant_Function_CreateWithArity(builtin_Array_fromAsync, 1));
  defmethod(js, arr_ctor_obj, "of", 2, Ant_Function_Create(builtin_Array_of));
  js_set_function_name(js, arr_ctor_obj, "Array", sizeof("Array") - 1);
  
  ant_value_t arr_ctor_func = js_obj_to_func(js, arr_ctor_obj);
  js->sym.array_ctor = arr_ctor_func;
  js_set_global_builtin(js, "Array", arr_ctor_func);
  
  ant_value_t proxy_ctor_obj = mkobj(js, 0);
  set_proto(js, proxy_ctor_obj, function_proto);
  set_slot(proxy_ctor_obj, SLOT_CFUNC, Ant_Function_Create(builtin_Proxy));
  js_define_function_length(js, proxy_ctor_obj, 2);
  js_mark_constructor(proxy_ctor_obj, true);
  defmethod(js, proxy_ctor_obj, "revocable", 9, Ant_Function_CreateWithArity(builtin_Proxy_revocable, 2));
  js_set_function_name(js, proxy_ctor_obj, "Proxy", sizeof("Proxy") - 1);
  js_set_global_builtin(js, "Proxy", js_obj_to_func(js, proxy_ctor_obj));
  
  ant_value_t p_ctor_obj = mkobj(js, 0);
  set_proto(js, p_ctor_obj, function_proto);
  set_slot(p_ctor_obj, SLOT_CFUNC, Ant_Function_CreateWithArity(builtin_Promise, 1));
  
  js->sym.promise_resolve = defmethod(js, p_ctor_obj, "resolve", 7, Ant_Function_CreateWithArity(builtin_Promise_resolve, 1));
  defmethod(js, p_ctor_obj, "reject", 6, Ant_Function_CreateWithArity(builtin_Promise_reject, 1));
  defmethod(js, p_ctor_obj, "try", 3, Ant_Function_CreateWithArity(builtin_Promise_try, 1));
  defmethod(js, p_ctor_obj, "withResolvers", 13, Ant_Function_Create(builtin_Promise_withResolvers));
  defmethod(js, p_ctor_obj, "all", 3, Ant_Function_CreateWithArity(builtin_Promise_all, 1));
  defmethod(js, p_ctor_obj, "allSettled", 10, Ant_Function_CreateWithArity(builtin_Promise_allSettled, 1));
  defmethod(js, p_ctor_obj, "race", 4, Ant_Function_CreateWithArity(builtin_Promise_race, 1));
  defmethod(js, p_ctor_obj, "any", 3, Ant_Function_CreateWithArity(builtin_Promise_any, 1));
  
  js_setprop_readonly_nonconfigurable(js, p_ctor_obj, "prototype", 9, promise_proto);
  ant_value_t p_ctor_func = js_obj_to_func(js, p_ctor_obj);
  defhidden(js, promise_proto, "constructor", 11, p_ctor_func);
  js_set_function_name(js, p_ctor_obj, "Promise", sizeof("Promise") - 1);
  js_set_global_builtin(js, "Promise", p_ctor_func);
  
  defalias(js, glob, "parseInt", 8, number_parse_int);
  defalias(js, glob, "parseFloat", 10, number_parse_float);
  defmethod(js, glob, "eval", 4, js_builtin_eval(js));
  defmethod(js, glob, "isNaN", 5, Ant_Function_CreateWithArity(builtin_global_isNaN, 1));
  defmethod(js, glob, "isFinite", 8, Ant_Function_CreateWithArity(builtin_global_isFinite, 1));
  defmethod(js, glob, "btoa", 4, Ant_Function_CreateWithArity(builtin_btoa, 1));
  js_set_descriptor(js, glob, "btoa", 4, JS_DESC_W | JS_DESC_E | JS_DESC_C);
  defmethod(js, glob, "atob", 4, Ant_Function_CreateWithArity(builtin_atob, 1));
  js_set_descriptor(js, glob, "atob", 4, JS_DESC_W | JS_DESC_E | JS_DESC_C);
  
  js_setprop(js, glob, js_mkstr(js, "NaN", 3), tov(JS_NAN));
  js_set_descriptor(js, glob, "NaN", 3, 0);
  js_setprop(js, glob, js_mkstr(js, "Infinity", 8), tov(JS_INF));
  js_set_descriptor(js, glob, "Infinity", 8, 0);
  js_setprop(js, glob, js_mkstr(js, "undefined", 9), js_mkundef());
  js_set_descriptor(js, glob, "undefined", 9, 0);
  
  ant_value_t import_obj = mkobj(js, 0);
  set_proto(js, import_obj, function_proto);
  
  set_slot(import_obj, SLOT_CFUNC, Ant_Function_Create(js_builtin_import));
  js_set_global_builtin(js, "import", js_obj_to_func(js, import_obj));
  
  defhidden(js, object_proto, "constructor", 11, obj_func);
  defhidden(js, function_proto, "constructor", 11, func_ctor_func);
  defhidden(js, array_proto, "constructor", 11, arr_ctor_func);
  defhidden(js, string_proto, "constructor", 11, str_ctor_func);
  defhidden(js, number_proto, "constructor", 11, number_ctor_func);
  defhidden(js, boolean_proto, "constructor", 11, bool_ctor_func);
  set_proto(js, glob, object_proto);
  
  js->sym.object_proto = object_proto;
  js->sym.array_proto = array_proto;
  js->sym.function_proto = function_proto;
  js->sym.string_proto = string_proto;
  js->sym.number_proto = number_proto;
  js->sym.boolean_proto = boolean_proto;
  js->sym.promise_ctor = p_ctor_func;
  js->sym.promise_proto = promise_proto;
  js->promise_resolve_lookup_protector_invalid = false;

  ant_protector_holder_mark(glob);
  ant_protector_holder_mark(array_proto);
  ant_protector_holder_mark(js->sym.array_ctor);
  ant_protector_holder_mark(p_ctor_func);
  ant_protector_holder_mark(promise_proto);
}

static ant_t *isolate_init(void *buf, size_t len) {
  #if UINTPTR_MAX > 9007199254740991ULL
  ANT_ASSERT(
    (uintptr_t)buf <= ((1ULL << 53) - 1),
    "pointer exceeds 53-bit double-precision integer limit"
  );
  #endif
  
  ant_t *js = NULL;
  
  if (len < sizeof(*js)) return js;
  memset(buf, 0, len);
  
  js = (ant_t *)buf;
  js->stringify_root.max_depth = ANT_INSPECT_MAX_DEPTH;
  Ant_List_Insert(&js->stringify, &js->stringify_root);
  
  js->exception = js_mkundef();
  js->exception_oom = js_mkundef();
  js_init_intern_cache(js);
  
  js->pool.rope.block_size = ANT_POOL_ROPE_BLOCK_SIZE;
  js->rope_gc.young.block_size = ANT_POOL_ROPE_BLOCK_SIZE;
  js->rope_gc.old.block_size = ANT_POOL_ROPE_BLOCK_SIZE;
  js->gc_use_nursery_major_floor = true;
  
  js->ic.epoch = 1;
  js->ic.obj_epoch = 1;
  gc_state_init(js);
  
  if (!isolate_arenas_init(js)) return NULL;
  if (!Ant_Loop_Init(js)) return NULL;

  js->young_closure_trigger = GC_CLOSURE_NURSERY_THRESHOLD;
  js->c_root_cap = 64;
  js->c_roots = calloc(js->c_root_cap, sizeof(*js->c_roots));
  
  if (!js->c_roots) {
    isolate_arenas_destroy(js);
    return NULL;
  }

  ant_realm_t *main_realm = calloc(1, sizeof(ant_realm_t));
  if (!main_realm) {
    free(js->c_roots);
    isolate_arenas_destroy(js);
    return NULL;
  }
  
  main_realm->is_main = true;
  Ant_List_Insert(&js->realms, main_realm);
  js->realm = js->main_realm = main_realm;

  js->modules.cjs.cache = js_mkundef();
  js->modules.cjs.parent = js_mkundef();
  js->modules.cjs.main = js_mkundef();
  js->modules.cjs.constructor = js_mkundef();
  js->modules.cjs.generation = 0;
  js->modules.hooks = js_mkundef();
  js->modules.import_meta = js_mkundef();
  js->modules.state = NULL;
  js->length_str = ANT_STRING("length");
  
  realm_init_core(js);
  
  js->this_val = js->global;
  js->owns_mem = false;
  js->max_size = 0;
  
  return js;
}

void init_intrinsic_symbols(ant_t *js) {
  mkprop(js, js->global, js->sym.toStringTag_sym, ANT_STRING("global"), ANT_PROP_ATTR_CONFIGURABLE);
  
  mkprop(
    js, js->sym.array_proto, js->sym.iterator_sym, 
    js->sym.array_values_fn, ANT_PROP_ATTR_WRITABLE | ANT_PROP_ATTR_CONFIGURABLE);

  ant_value_t unscopables = Ant_Object_CreateBare(js);
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, unscopables);
  
  js_set(js, unscopables, "at", js_true);
  js_set(js, unscopables, "copyWithin", js_true);
  js_set(js, unscopables, "entries", js_true);
  js_set(js, unscopables, "fill", js_true);
  js_set(js, unscopables, "find", js_true);
  js_set(js, unscopables, "findIndex", js_true);
  js_set(js, unscopables, "findLast", js_true);
  js_set(js, unscopables, "findLastIndex", js_true);
  js_set(js, unscopables, "flat", js_true);
  js_set(js, unscopables, "flatMap", js_true);
  js_set(js, unscopables, "includes", js_true);
  js_set(js, unscopables, "keys", js_true);
  js_set(js, unscopables, "toReversed", js_true);
  js_set(js, unscopables, "toSorted", js_true);
  js_set(js, unscopables, "toSpliced", js_true);
  js_set(js, unscopables, "values", js_true);
  
  mkprop(js, js->sym.array_proto, js->sym.unscopables_sym, unscopables, ANT_PROP_ATTR_CONFIGURABLE);
  GC_ROOT_RESTORE(js, mark);

  mkprop(js, js->sym.promise_proto, js->sym.toStringTag_sym, ANT_STRING("Promise"), ANT_PROP_ATTR_CONFIGURABLE);
  ant_value_t async_proto = js_get_slot(js->global, SLOT_ASYNC_PROTO);
  
  mkprop(js, async_proto, js->sym.toStringTag_sym, ANT_STRING("AsyncFunction"), ANT_PROP_ATTR_CONFIGURABLE);
  js_define_species_getter(js, js->sym.promise_ctor);
  js_define_species_getter(js, js_get(js, js->global, "Array"));
  defalias_sym_of(js, js->builtins.disposable_stack_proto, js->sym.dispose_sym, "dispose");
  defalias_sym_of(js, js->builtins.async_disposable_stack_proto, js->sym.asyncDispose_sym, "disposeAsync");
  js->promise_species_protector_invalid = false;
  js->array_species_protector_invalid = false;
  js->array_iteration_protector_invalid = false;
}

bool js_realm_init_intrinsics(ant_t *js) {
  realm_init_core(js);
  ant_bootstrap_realm_intrinsics(js);
  js_set_global_builtin(js, "globalThis", js->global);
  
  static const char *const not_ecmascript[] = { "Buffer", "atob", "btoa", "AsyncIterator", "InternalError", "import" };
  for (size_t i = 0; i < sizeof(not_ecmascript) / sizeof(*not_ecmascript); i++)
    js_delete_prop_ordinary(js, js->global, not_ecmascript[i], strlen(not_ecmascript[i]));
    
  ant_realm_install_console(js);
  return !Ant_Exception_Pending(js);
}

ant_t *ant_create() {
  ant_t *js = (ant_t *)calloc(1, sizeof(*js));
  if (js == NULL) return NULL;
  
  if (isolate_init(js, sizeof(*js)) == NULL) {
    free(js);
    return NULL;
  }
  
  js->owns_mem = true;
  js->vm = sv_vm_create(js);

  if (!js->vm) {
    js_destroy(js);
    return NULL;
  }

  ant_value_t oom = Ant_Error_Create(
    js, JS_ERR_INTERNAL | JS_ERR_NO_STACK,
    "out of memory creating exception"
  );

  if (is_err(oom)) {
    js_destroy(js);
    return NULL;
  }

  js->exception_oom = Ant_Exception_CreateRecord(js, oom, js_mkundef());
  if (!is_err(js->exception_oom) || vdata(js->exception_oom) < 2) {
    js_destroy(js);
    return NULL;
  }

  return js;
}

void js_destroy(ant_t *js) {
  if (js == NULL) return;
  js->destroying = true;
  
  cleanup_timer_module(js);
  json_layout_cache_clear(js);
  if (js->index_layout_shape) ant_shape_release(js->index_layout_shape);
  js->index_layout_shape = NULL;
  cleanup_cron_module(js);
  cleanup_abort_module(js);
  cleanup_eventsource_module(js);
  cleanup_websocket_module(js);
  cleanup_dns_module(js);
  
  Ant_Loop_Drain(js);
  gc_weak_cleanup(js);

  if (js->vm) {
    sv_vm_destroy(js->vm);
    js->vm = NULL;
  }
  
  js_esm_cleanup_module_cache(js);
  sv_jit_destroy(js);
  
  sv_code_units_destroy(js);
  sv_ic_shape_refs_cleanup(js);
  sv_ic_polys_cleanup(js);
  
  free(js->ic.gf_mega);
  free(js->gc.mark_stack);
  free(js->gc.func_stack);
  free(js->gc.fb_funcs);
  js->ic.gf_mega = NULL;
  
  code_arenas_destroy(js);
  cleanup_rpc_module();
  cleanup_lmdb_module();
  js_descriptor_registry_cleanup(js);
  cleanup_iterator_module(js);

  sym_registry_entry_t *entry, *tmp;
  HASH_ITER(hh, js->sym.registry, entry, tmp) {
    HASH_DEL(js->sym.registry, entry);
    free(entry);
  }

  ant_object_t *lists[] = { js->objects, js->objects_old, js->permanent_objects };
  for (int i = 0; i < 3; i++) for (ant_object_t *obj = lists[i]; obj;) {
    ant_object_t *next = obj->next;
    gc_object_free(js, obj);
    obj = next;
  }

  js->objects = NULL;
  js->objects_old = NULL;
  js->permanent_objects = NULL;
  
  cleanup_buffer_module(js);
  cleanup_atomics_module(js);
  cleanup_events_module(js);
  cleanup_regex_module(js);
  Ant_Loop_Destroy(js);
  gc_array_storage_cache_destroy(js);
  isolate_arenas_destroy(js);
  
  free(js->c_roots);
  js->c_roots = NULL;
  js->c_root_count = js->c_root_cap = 0;

  ant_realm_destroy_all(js);
  free(js->global_lexicals);
  
  js->global_lexicals = NULL;
  js->global_lexical_count = js->global_lexical_cap = 0;
  free(js->global_lexical_index);
  
  js->global_lexical_index = NULL;
  js->global_lexical_index_cap = 0;

  free(js->permanent_roots);
  js->permanent_roots = NULL;
  js->permanent_root_len = 0;
  js->permanent_root_cap = 0;
  js->permanent_root_traced = 0;

  free(js->remembered_upvalues);
  js->remembered_upvalues = NULL;
  js->remembered_upvalue_len = js->remembered_upvalue_cap = 0;

  free(js->remembered_closures);
  js->remembered_closures = NULL;
  js->remembered_closure_len = js->remembered_closure_cap = 0;

  free(js->rope_gc.remembered_builders);
  js->rope_gc.remembered_builders = NULL;
  js->rope_gc.remembered_builder_len = js->rope_gc.remembered_builder_cap = 0;

  free(js->rope_gc.marks);
  js->rope_gc.marks = NULL;
  js->rope_gc.last_mark = NULL;
  js->rope_gc.mark_count = js->rope_gc.mark_cap = 0;

  free(js->young_closures);
  js->young_closures = NULL;
  js->young_closure_len = js->young_closure_cap = 0;

  free(js->young_upvalues);
  js->young_upvalues = NULL;
  js->young_upvalue_len = js->young_upvalue_cap = 0;

  free(js->remember_set);
  js->remember_set = NULL;
  js->remember_set_len = js->remember_set_cap = 0;

  free(js->pending_rejections.items);
  js->pending_rejections.items = NULL;
  js->pending_rejections.len = js->pending_rejections.cap = 0;

  ant_ptr_pair_free(&js->cfunc_promoted);
  ant_ptr_pair_free(&js->cfunc_named);

  js_pool_destroy(&js->pool.rope);
  js_pool_destroy(&js->rope_gc.young);
  js_pool_destroy(&js->rope_gc.old);
  js_pool_destroy(&js->pool.symbol);
  js_pool_destroy(&js->pool.permanent);
  
  js_class_pool_destroy(&js->pool.bigint);
  js_string_pool_destroy(&js->pool.string);
  gc_strings_epoch_bump();
  gc_strings_destroy(js);
  gc_bigints_destroy(js);

  if (js->owns_mem) free(js);
}

inline double js_getnum(ant_value_t value) { return tod(value); }
inline void js_setstackbase(ant_t *js, void *base) { js->cstk.base = base; js->cstk.main_base = base; js_cstk_refresh_floor(js); }
inline void js_setstacklimit(ant_t *js, size_t max) { js->cstk.limit = max; js_cstk_refresh_floor(js); }
inline void js_set_filename(ant_t *js, const char *filename) { js->filename = filename; }

inline ant_value_t js_mkundef(void) { return mkval(kTypeUndefined, 0); }
inline ant_value_t js_mknull(void) { return mkval(kTypeNull, 0); }
inline ant_value_t js_mknum(double value) { return tov(value); }
inline ant_value_t Ant_Object_CreateBare(ant_t *js) { return mkobj(js, 0); }
inline ant_value_t js_glob(ant_t *js) { return js->global; }

static inline ant_value_t js_cfunc_value(const ant_cfunc_meta_t *stored) {
  return stored ? mkref(kTypeBuiltin, stored) : mkval(kTypeError, 0);
}

ant_value_t js_mkfun_meta(const ant_cfunc_meta_t *meta) {
  if (!meta) return mkval(kTypeBuiltin, 0);
  return js_cfunc_value(ant_cfunc_meta_intern(meta));
}

ant_value_t js_mkfun_site(ant_cfunc_site_t *site) {
  if (__builtin_expect(ant_cfunc_intern_realm != NULL, 0)) return js_cfunc_value(ant_cfunc_meta_intern(&site->meta));
  const ant_cfunc_meta_t *stored = __atomic_load_n(&site->stored, __ATOMIC_ACQUIRE);
  if (stored) return mkref(kTypeBuiltin, stored);
  
  stored = ant_cfunc_meta_intern(&site->meta);
  if (stored) __atomic_store_n(&site->stored, stored, __ATOMIC_RELEASE);
  return js_cfunc_value(stored);
}

ant_value_t js_mkfun_dyn(ant_cfunc_t fn) {
  return js_cfunc_value(ant_cfunc_meta_for_entrypoint(fn));
}

inline ant_value_t js_getthis(ant_t *js) { return js->this_val; }
inline void js_setthis(ant_t *js, ant_value_t val) { js->this_val = val; }
inline ant_value_t js_getcurrentfunc(ant_t *js) { return js->current_func; }

static ant_value_t js_cfunc_name_value(ant_t *js, ant_value_t cfunc) {
  const ant_cfunc_meta_t *meta = js_as_cfunc_meta(cfunc);
  if (!meta || !meta->name) return js_mkundef();
  return js_mkstr(js, meta->name, strlen(meta->name));
}

static ant_value_t js_cfunc_length_value(ant_value_t cfunc) {
  return tov((double)js_cfunc_length(cfunc));
}

static void js_note_function_display_name(ant_t *js, ant_value_t fn, ant_value_t name_val) {
  sv_func_t *func = vtype(fn) == kTypeFunction ? js_func_closure(fn)->func : NULL;
  if (!func || (func->debug->name && func->debug->name[0]) || func->debug->display_name_varies) return;

  size_t len = 0;
  const char *name = js_getstr(js, name_val, &len);
  if (!name || len == 0) return;
  
  const char *interned = intern_string(name, len);
  if (!func->debug->display_name) func->debug->display_name = interned;
  else if (func->debug->display_name != interned) func->debug->display_name_varies = true;
}

static ant_value_t js_define_function_name_value(ant_t *js, ant_value_t fn, ant_value_t name_val) {
  ant_value_t fn_obj = js_as_obj(fn);
  ant_object_t *ptr = js_obj_ptr(fn_obj);
  
  if (!ptr) return js_mkundef();
  if (vtype(name_val) != kTypeString) return js_mkerr(js, "function name must be a string");
  js_note_function_display_name(js, fn, name_val);

  const char *name_key = js->intern.name ? js->intern.name : intern_string("name", 4);
  if (!name_key) return js_mkerr(js, "oom");

  return mkprop_interned_exact(
    js, fn_obj, name_key, name_val, 
    ANT_PROP_ATTR_CONFIGURABLE
  );
}

static ant_value_t js_mkstr_concat2(
  ant_t *js,
  const char *left, size_t left_len,
  const char *right, size_t right_len
) {
  if (!left) left_len = 0;
  if (!right) right_len = 0;

  if (SIZE_MAX - left_len < right_len) return js_mkerr(js, "oom");
  size_t total = left_len + right_len;
  ant_value_t out = js_mkstr(js, NULL, total);
  if (is_err(out)) return out;

  ant_flat_string_t *flat = ant_str_flat_ptr(out);
  if (!flat) return js_mkerr(js, "oom");
  if (left_len) memcpy(flat->bytes, left, left_len);
  if (right_len) memcpy(flat->bytes + left_len, right, right_len);
  
  flat->bytes[total] = '\0';
  str_flat_finish(flat, str_detect_ascii_bytes(flat->bytes, total));
  
  return out;
}

static ant_value_t js_mkstr_symbol_function_name(
  ant_t *js,
  const char *prefix, size_t prefix_len,
  const char *desc
) {
  if (!prefix) prefix_len = 0;
  if (!desc) return js_mkstr_concat2(js, prefix, prefix_len, "", 0);

  size_t desc_len = strlen(desc);
  if (SIZE_MAX - prefix_len < desc_len ||
      SIZE_MAX - prefix_len - desc_len < 2) {
    return js_mkerr(js, "oom");
  }

  size_t total = prefix_len + desc_len + 2;
  ant_value_t out = js_mkstr(js, NULL, total);
  if (is_err(out)) return out;

  ant_flat_string_t *flat = ant_str_flat_ptr(out);
  if (!flat) return js_mkerr(js, "oom");
  if (prefix_len) memcpy(flat->bytes, prefix, prefix_len);
  
  flat->bytes[prefix_len] = '[';
  memcpy(flat->bytes + prefix_len + 1, desc, desc_len);
  
  flat->bytes[total - 1] = ']';
  flat->bytes[total] = '\0';
  str_flat_init_meta(flat, str_detect_ascii_bytes(flat->bytes, total));
  
  return out;
}

ant_value_t js_set_function_name(ant_t *js, ant_value_t fn, const char *name, size_t name_len) {
  if (!name) {
    name = "";
    name_len = 0;
  }

  ant_value_t name_val = js_mkstr(js, name, name_len);
  if (is_err(name_val)) return name_val;
  return js_define_function_name_value(js, fn, name_val);
}

ant_value_t js_set_function_name_prefixed(
  ant_t *js, ant_value_t fn,
  const char *prefix, size_t prefix_len,
  const char *name, size_t name_len
) {
  if (!prefix || prefix_len == 0)
    return js_set_function_name(js, fn, name, name_len);

  if (!name) {
    name = "";
    name_len = 0;
  }

  ant_value_t prefixed = js_mkstr_concat2(js, prefix, prefix_len, name, name_len);
  if (is_err(prefixed)) return prefixed;
  return js_define_function_name_value(js, fn, prefixed);
}

ant_value_t js_set_function_name_from_key(
  ant_t *js, ant_value_t fn,
  ant_value_t key,
  const char *prefix, size_t prefix_len
) {
  if (vtype(key) == kTypeSymbol) {
    const char *desc = js_sym_desc(key);
    ant_value_t symbol_name = js_mkstr_symbol_function_name(js, prefix, prefix_len, desc);
    if (is_err(symbol_name)) return symbol_name;
    return js_define_function_name_value(js, fn, symbol_name);
  }

  ant_value_t key_str = coerce_to_str(js, key);
  if (is_err(key_str)) return key_str;
  if (vtype(key_str) != kTypeString)
    return js_set_function_name_prefixed(js, fn, prefix, prefix_len, "", 0);

  ant_offset_t key_len = 0;
  ant_offset_t key_off = vstr(js, key_str, &key_len);
  
  const char *key_ptr = (const char *)(uintptr_t)key_off;
  return js_set_function_name_prefixed(js, fn, prefix, prefix_len, key_ptr, (size_t)key_len);
}

static bool js_function_name_is_inferable(ant_t *js, ant_value_t fn) {
  if (vtype(fn) != kTypeFunction) return false;
  
  ant_value_t fn_obj = js_func_obj(fn);
  ant_value_t existing = lkp_interned_val(js, fn_obj, js->intern.name);
  
  if (vtype(existing) == kTypeUndefined) return true;
  if (vtype(existing) != kTypeString) return false;

  ant_offset_t len = 0;
  vstr(js, existing, &len);
  
  return len == 0;
}

ant_value_t js_maybe_set_function_name_from_key(
  ant_t *js, ant_value_t fn,
  ant_value_t key,
  const char *prefix, size_t prefix_len
) {
  if (!js_function_name_is_inferable(js, fn)) return js_mkundef();
  return js_set_function_name_from_key(js, fn, key, prefix, prefix_len);
}

static bool js_cfunc_has_prototype(ant_value_t cfunc) {
  const ant_cfunc_meta_t *meta = js_as_cfunc_meta(cfunc);
  return meta && (meta->flags & CFUNC_HAS_PROTOTYPE) != 0;
}

static bool js_cfunc_is_constructor(ant_value_t cfunc) {
  const ant_cfunc_meta_t *meta = js_as_cfunc_meta(cfunc);
  return meta && (meta->flags & CFUNC_IS_CONSTRUCTOR) != 0;
}

static ant_value_t setup_func_prototype_property(ant_t *js, ant_value_t func, bool mark_constructor) {
  ant_value_t func_obj = (vtype(func) == kTypeFunction)
    ? js_func_obj(func) 
    : js_as_obj(func);
  
  ant_value_t proto_obj = mkobj(js, 0);
  if (is_err(proto_obj)) return proto_obj;
  
  ant_value_t object_proto = js->sym.object_proto;
  if (vtype(object_proto) == kTypeObject) js_set_proto_init(proto_obj, object_proto);
  
  ant_value_t res = defhidden(js, proto_obj, "constructor", 11, func);
  if (is_err(res)) return res;
  
  ant_prop_loc_t existing = lkp(js, func_obj, "prototype", 9);
  if (existing.obj) {
    if (is_const_prop(existing)) return js_mkerr(js, "assignment to constant");
    js_prop_store(js, existing, proto_obj);
    js_set_descriptor(js, func_obj, "prototype", 9, JS_DESC_W);
  } else {
    res = mkprop_interned_exact(js, func_obj, js->intern.prototype, proto_obj, ANT_PROP_ATTR_WRITABLE);
    if (is_err(res)) return res;
  }
  
  if (mark_constructor) js_mark_constructor(func_obj, true);
  return js_mkundef();
}

static ant_value_t cfunc_expose_named(ant_t *js, ant_value_t cfunc, const char *name, size_t name_len, uint32_t length) {
  const ant_cfunc_meta_t *base = js_as_cfunc_meta(cfunc);
  if (!base) return cfunc;

  if (
    base->name && base->length == length &&
    strlen(base->name) == name_len &&
    memcmp(base->name, name, name_len) == 0
  ) return cfunc;

  const char *interned = intern_string(name, name_len);
  if (!interned) return js_mkerr(js, "oom");

  ant_value_t cached;
  ant_ptr_pair_map_t *named_map = js_cfunc_named_map(js, base);
  
  if (
    ant_ptr_pair_get(named_map, (uintptr_t)base, (uintptr_t)interned, &cached) &&
    js_as_cfunc_meta(cached)->length == length
  ) return cached;

  ant_cfunc_meta_t named_meta = {
    .fn = base->fn,
    .name = interned,
    .length = length,
    .flags = base->flags,
  };

  const ant_cfunc_meta_t *stored = ant_cfunc_meta_derive(base, &named_meta);
  if (!stored) return js_mkerr(js, "oom");

  ant_value_t named = mkref(kTypeBuiltin, stored);
  if (!ant_ptr_pair_put(named_map, (uintptr_t)base, (uintptr_t)interned, named)) return js_mkerr(js, "oom");
  
  return named;
}

ant_value_t js_cfunc_expose_named(ant_t *js, ant_value_t cfunc, const char *name, size_t name_len) {
  if (vtype(cfunc) != kTypeBuiltin || !name) return cfunc;
  const ant_cfunc_meta_t *base = js_as_cfunc_meta(cfunc);
  return base ? cfunc_expose_named(js, cfunc, name, name_len, base->length) : cfunc;
}

ant_value_t js_cfunc_expose_property(ant_t *js, ant_value_t fn, js_cfunc_role_t role, const char *key, size_t key_len, bool symbol) {
  static const char *const prefixes[] = {
    [JS_CFUNC_VALUE] = "", [JS_CFUNC_GETTER] = "get ", [JS_CFUNC_SETTER] = "set ",
  };

  const ant_cfunc_meta_t *meta = vtype(fn) == kTypeBuiltin ? js_as_cfunc_meta(fn) : NULL;
  if (!meta || meta->name) return fn;

  char name[256];
  int len = !key ? snprintf(name, sizeof(name), "%s", prefixes[role])
    : snprintf(name, sizeof(name), symbol ? "%s[%.*s]" : "%s%.*s", prefixes[role], (int)key_len, key);
  if (len < 0) return fn;

  size_t name_len = (size_t)len < sizeof(name) ? (size_t)len : sizeof(name) - 1;
  return cfunc_expose_named(js, fn, name, name_len, role == JS_CFUNC_SETTER ? 1 : meta->length);
}

static ant_value_t js_expose_cfunc_for_key(ant_t *js, ant_value_t value, const char *key, size_t key_len) {
  if (vtype(value) != kTypeBuiltin) return value;
  return js_cfunc_expose_named(js, value, key, key_len);
}

static ant_value_t js_cfunc_promote_in_realm(ant_t *js, ant_value_t cfunc) {
  const ant_cfunc_meta_t *meta = js_as_cfunc_meta(cfunc);

  ant_value_t fn_obj = mkobj(js, 0);
  set_slot(fn_obj, SLOT_CFUNC, cfunc);

  ant_value_t proto = js_primitive_prototype(js, kTypeBuiltin);
  if (is_object_type(proto)) {
    ant_object_t *obj_ptr = js_obj_ptr(js_as_obj(fn_obj));
    if (obj_ptr) obj_ptr->proto = proto;
  }

  ant_value_t length_result = js_define_function_length(js, fn_obj, (int)js_cfunc_length(cfunc));
  if (is_err(length_result)) return length_result;

  if (meta && meta->name) {
    ant_value_t name_result = js_set_function_name(js, fn_obj, meta->name, strlen(meta->name));
    if (is_err(name_result)) return name_result;
  }

  ant_value_t promoted = js_obj_to_func(js, fn_obj);

  bool is_constructor = js_cfunc_is_constructor(cfunc);
  if (js_cfunc_has_prototype(cfunc)) {
    ant_value_t proto_result = setup_func_prototype_property(
      js, promoted, is_constructor
    );
    if (is_err(proto_result)) return proto_result;
  } else if (is_constructor) js_mark_constructor(promoted, true);

  if (!ant_ptr_pair_put(js_cfunc_promoted_map(js, meta), vdata(cfunc), 0, promoted))
    return js_mkerr(js, "oom");
  
  ant_realm_t *owner = ant_cfunc_meta_realm(meta);
  if (owner) owner->gc_dirty = true;

  return promoted;
}

ant_value_t js_cfunc_promote(ant_t *js, ant_value_t cfunc) {
  ant_value_t cached;
  if (ant_ptr_pair_get(js_cfunc_promoted_map(js, js_as_cfunc_meta(cfunc)), vdata(cfunc), 0, &cached)) return cached;
  
  ant_realm_t *prev = ant_realm_switch(js, ant_realm_of_builtin(js, cfunc));
  ant_value_t promoted = js_cfunc_promote_in_realm(js, cfunc);
  ant_realm_restore(js, prev);
  
  return promoted;
}

static ant_value_t js_heavy_function(ant_t *js, ant_value_t cfunc, ant_value_t data) {
  ant_value_t fn_obj = mkobj(js, 0);
  
  set_slot(fn_obj, SLOT_CFUNC, cfunc);
  set_slot(fn_obj, SLOT_DATA, data);
  
  return js_obj_to_func(js, fn_obj);
}

void js_define_methods(ant_t *js, ant_value_t obj, const ant_cfunc_meta_t *methods, size_t count, uint8_t attrs) {
  for (const ant_cfunc_meta_t *m = methods; m < methods + count; m++)
    mkprop_interned(js, obj, intern_string(m->name, strlen(m->name)), js_mkfun_meta(m), attrs);
}

ant_value_t js_make_ctor_cfunc(ant_t *js, ant_value_t cfunc, ant_value_t proto, uint8_t proto_attrs) {
  const ant_cfunc_meta_t *meta = js_as_cfunc_meta(cfunc);
  if (!meta || !meta->name) return js_mkerr(js, "constructor requires a named cfunc");
  
  ant_value_t obj = Ant_Object_CreateBare(js);
  js_set_slot(obj, SLOT_CFUNC, cfunc);
  js_set_function_name(js, obj, meta->name, strlen(meta->name));
  mkprop_exact_attrs(js, obj, js_mkstr(js, "prototype", 9), proto, proto_attrs);

  ant_value_t fn_val = js_obj_to_func(js, obj);
  defhidden(js, proto, "constructor", 11, fn_val);
  return fn_val;
}

ant_value_t js_heavy_mkfun(ant_t *js, ant_value_t (*fn)(ant_params_t), ant_value_t data) {
  return js_heavy_function(js, js_mkfun_dyn(fn), data);
}

ant_value_t js_heavy_mkfun_cfunc(ant_t *js, ant_value_t cfunc, ant_value_t data) {
  const ant_cfunc_meta_t *meta = js_as_cfunc_meta(cfunc);
  ant_value_t fn = js_heavy_function(js, cfunc, data);
  if (is_err(fn) || !meta) return fn;

  ant_value_t length_result = js_define_function_length(js, fn, (int)meta->length);
  if (is_err(length_result)) return length_result;
  if (!meta->name) return fn;

  ant_value_t name_result = js_set_function_name(js, fn, meta->name, strlen(meta->name));
  return is_err(name_result) ? name_result : fn;
}

ant_value_t js_heavy_mkfun_native(ant_t *js, ant_value_t (*fn)(ant_params_t), void *ptr, uint32_t tag) {
  ant_value_t cfunc = js_mkfun_dyn(fn);
  ant_value_t fn_obj = mkobj(js, 0);
  
  set_slot(fn_obj, SLOT_CFUNC, cfunc);
  js_set_native(fn_obj, ptr, tag);
  
  return js_obj_to_func(js, fn_obj);
}

void js_set(ant_t *js, ant_value_t obj, const char *key, ant_value_t val) {
  size_t key_len = strlen(key);
  val = js_expose_cfunc_for_key(js, val, key, key_len);
  if (is_err(val)) return;
  
  if (vtype(obj) == kTypeObject) {
    ant_prop_loc_t existing = lkp(js, obj, key, key_len);
    if (existing.obj) {
      if (is_const_prop(existing)) {
        js_mkerr(js, "assignment to constant");
        return;
      }
      js_prop_store(js, existing, val);
    } else {
      ant_value_t key_str = js_mkstr(js, key, key_len);
      mkprop(js, obj, key_str, val, 0);
    }
  } else if (vtype(obj) == kTypeFunction) {
    ant_value_t func_obj = js_func_obj(obj);
    ant_prop_loc_t existing = lkp(js, func_obj, key, key_len);
    if (existing.obj) {
      if (is_const_prop(existing)) {
        js_mkerr(js, "assignment to constant");
        return;
      }
      js_prop_store(js, existing, val);
    } else {
      ant_value_t key_str = js_mkstr(js, key, key_len);
      mkprop(js, func_obj, key_str, val, 0);
    }
  }
}

void js_set_exact(ant_t *js, ant_value_t obj, const char *key, ant_value_t val) {
  size_t key_len = strlen(key);
  const char *interned = intern_string(key, key_len);
  
  if (!interned) {
    js_mkerr(js, "oom");
    return;
  }

  if (vtype(obj) == kTypeObject) {
    ant_prop_loc_t existing = lkp(js, obj, key, key_len);
    if (existing.obj) {
      if (is_const_prop(existing)) {
        js_mkerr(js, "assignment to constant");
        return;
      }
      js_prop_store(js, existing, val);
    } else mkprop_interned_exact(js, obj, interned, val, 0);
  } else if (vtype(obj) == kTypeFunction) {
    ant_value_t func_obj = js_func_obj(obj);
    ant_prop_loc_t existing = lkp(js, func_obj, key, key_len);
    if (existing.obj) {
      if (is_const_prop(existing)) {
        js_mkerr(js, "assignment to constant");
        return;
      }
      js_prop_store(js, existing, val);
    } else mkprop_interned_exact(js, func_obj, interned, val, 0);
  }
}

void js_set_tostringtag(ant_t *js, ant_value_t obj, ant_value_t tag) {
  mkprop(js, obj, js->sym.toStringTag_sym, tag, ANT_PROP_ATTR_CONFIGURABLE);
}

void js_set_sym(ant_t *js, ant_value_t obj, ant_value_t sym, ant_value_t val) {
  if (vtype(sym) != kTypeSymbol) return;
  ant_offset_t sym_off = (ant_offset_t)vdata(sym);
  
  const char *desc = js_sym_desc(sym);
  val = js_cfunc_expose_property(js, val, JS_CFUNC_VALUE, desc, desc ? strlen(desc) : 0, true);
  if (is_err(val)) return;
  
  if (vtype(obj) == kTypeFunction) obj = js_func_obj(obj);
  else if (is_object_type(obj)) obj = js_as_obj(obj);
  if (vtype(obj) != kTypeObject && vtype(obj) != kTypeArray) return;
  
  ant_prop_loc_t existing = lkp_sym(obj, sym_off);
  if (existing.obj) {
    if (is_const_prop(existing)) return;
    js_prop_store(js, existing, val);
    ant_symbol_property_mutation_invalidate(js, existing.obj, sym_off);
  } else mkprop(js, obj, sym, val, 0);
}

void js_set_symbol(ant_t *js, ant_value_t obj, const char *key, ant_value_t val) {
  ant_value_t sym = js_mksym_for(js, key);
  if (is_err(sym)) return;
  js_set_sym(js, obj, sym, val);
}

static bool js_try_get_sym_with_receiver(
  ant_t *js, ant_value_t obj, ant_value_t sym,
  ant_value_t receiver, ant_value_t *out
) {
  if (vtype(sym) != kTypeSymbol) return false;
  ant_offset_t sym_off = (ant_offset_t)vdata(sym);

  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) == kTypeFunction)
      return js_try_get_sym_with_receiver(js, promoted, sym, receiver, out);
  }

  if (vtype(obj) == kTypeFunction) obj = js_func_obj(obj);
  uint8_t ot = vtype(obj);
  
  if (!is_object_type(obj)) {
    if (ot == kTypeString || ot == kTypeNumber || ot == kTypeBool || ot == kTypeBigInt || ot == kTypeSymbol || ot == kTypeBuiltin) {
      ant_value_t proto = js_primitive_prototype(js, ot);
      if (!is_object_type(proto)) return false;
      obj = js_as_obj(proto);
    } else return false;
  } else obj = js_as_obj(obj);

  if (is_proxy(obj)) {
    *out = proxy_get_val(js, obj, sym);
    return true;
  }

  ant_value_t cur = obj;
  proto_overflow_guard_t guard;
  proto_overflow_guard_init(&guard);
  
  while (is_object_type(cur)) {
    ant_value_t cur_obj = js_as_obj(cur);
    ant_prop_loc_t off = lkp_sym(cur_obj, sym_off);
    if (off.obj) {
      const ant_shape_prop_t *meta = prop_shape_meta(off);
      if (meta && meta->has_getter) {
        *out = is_callable(meta->getter) ? js_call_getter(js, receiver, meta->getter) : js_mkundef();
        return true;
      }
      *out = (meta && meta->has_setter) ? js_mkundef() : js_prop_load(off);
      return true;
    }

    ant_value_t proto = get_proto(js, cur_obj);
    if (!is_object_type(proto)) break;
    
    cur = js_as_obj(proto);
    if (proto_overflow_guard_hit_cycle(js, &guard, cur)) break;
  }

  return false;
}

ant_value_t js_get_sym_with_receiver(ant_t *js, ant_value_t obj, ant_value_t sym, ant_value_t receiver) {
  ant_value_t result;
  return js_try_get_sym_with_receiver(js, obj, sym, receiver, &result) ? result : js_mkundef();
}

ant_value_t js_get_sym(ant_t *js, ant_value_t obj, ant_value_t sym) {
  return js_get_sym_with_receiver(js, obj, sym, obj);
}

ant_value_t js_get_symbol(ant_t *js, ant_value_t obj, const char *key) {
  ant_value_t sym = js_mksym_for(js, key);
  if (is_err(sym)) return sym;
  return js_get_sym(js, obj, sym);
}

static bool js_try_get_len(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t *out) {
  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) == kTypeFunction && js_try_get_len(js, promoted, key, key_len, out)) return true;
    if (js_cfunc_try_get_own(js, obj, key, key_len, out)) return true;
    ant_prop_loc_t off = lkp_proto(js, obj, key, key_len);
    if (off.obj) {
      *out = js_prop_load_from(js, obj, off);
      return true;
    }
    return false;
  }
  
  if (vtype(obj) == kTypeFunction) {
    if (sv_vm_is_strict(js->vm) &&
        ((key_len == 6 && memcmp(key, "caller", 6) == 0) ||
         (key_len == 9 && memcmp(key, "arguments", 9) == 0))) {
      *out = js_mkerr_typed(
        js, JS_ERR_TYPE,
        "'%.*s' not allowed on functions in strict mode",
        (int)key_len, key
      );
      return true;
    }

    ant_value_t func_obj = js_func_obj(obj);
    if (key_len == 4 && memcmp(key, "meta", 4) == 0 &&
      js_try_import_meta(js, func_obj, out)) return true;

    ant_prop_loc_t off = lkp(js, func_obj, key, key_len);
    if (!off.obj) {
      ant_value_t accessor_result;
      if (try_accessor_getter(js, obj, key, key_len, &accessor_result)) {
        *out = accessor_result;
        return true;
      }
      off = lkp_proto(js, obj, key, key_len);
      if (off.obj) {
        *out = js_prop_load_from(js, obj, off);
        return true;
      }
      return false;
    }

    *out = js_prop_load_from(js, obj, off);
    return true;
  }
  
  if (array_obj_ptr(obj)) {
    if (real_array_obj_ptr(obj) && is_length_key(key, key_len)) {
      *out = tov((double)get_array_length(js, obj));
      return true;
    }
    
    unsigned long idx;
    ant_offset_t arr_len = get_array_length(js, obj);
    
    if (parse_array_index(key, key_len, arr_len, &idx)) {
      if (js_arguments_state(obj)) {
        *out = js_arguments_getter(js, obj, key, key_len);
        return true;
      }
      if (arr_has(js, obj, (ant_offset_t)idx)) {
        *out = arr_get(js, obj, (ant_offset_t)idx);
        return true;
      } 
      
      return false;
    }
    
    ant_value_t arr_obj = js_as_obj(obj);
    ant_prop_loc_t off = lkp(js, arr_obj, key, key_len);
    
    if (!off.obj) {
      ant_value_t accessor_result;
      if (try_accessor_getter(js, arr_obj, key, key_len, &accessor_result)) {
        *out = accessor_result; return true;
      }
      
      return false;
    }
    
    *out = js_prop_load_from(js, obj, off);
    return true;
  }

  uint8_t t = vtype(obj);
  bool is_promise = (t == kTypePromise);
  if (t == kTypeObject && is_proxy(obj)) {
    *out = proxy_get(js, obj, key, key_len);
    return true;
  }
  
  if (is_boxable_primitive_type(t)) {
    if (t == kTypeString && is_length_key(key, key_len)) {
      *out = tov((double)str_utf16_len(js, obj));
      return true;
    }
    
    if (t == kTypeString && js_try_get_string_index(js, obj, key, key_len, out)) return true;

    ant_value_t prim_proto = js_primitive_prototype(js, t);
    if (is_object_type(prim_proto)) {
      ant_prop_loc_t poff = lkp_proto(js, prim_proto, key, key_len);
      if (poff.obj) {
        *out = js_prop_load_from(js, obj, poff);
        return true;
      }
    }

    return false;
  }
  
  if (is_promise || t == kTypeGenerator) obj = js_as_obj(obj);
  else if (t != kTypeObject) return false;
  ant_prop_loc_t off = lkp(js, obj, key, key_len);
  
  if (!off.obj && t == kTypeObject && js_try_get_string_own_exotic(js, obj, key, key_len, out, NULL)) return true;
  if (!off.obj) {
    ant_value_t result = try_dynamic_getter(js, obj, key, key_len);
    if (result == T_EMPTY) { *out = js_mkundef(); return true; }
    if (vtype(result) != kTypeUndefined) { *out = result; return true; }
  }
  
  if (!off.obj) {
    off = lkp_proto(js, obj, key, key_len);
    if (off.obj) {
      *out = js_prop_load_from(js, obj, off);
      return true;
    }
  }
  
  if (!off.obj) {
    ant_value_t accessor_result;
    if (try_accessor_getter(js, obj, key, key_len, &accessor_result)) {
      *out = accessor_result; return true;
    }
    return false;
  }
  
  *out = js_prop_load_from(js, obj, off);
  return true;
}

ant_value_t js_get(ant_t *js, ant_value_t obj, const char *key) {
  return js_getprop_fallback_len(js, obj, key, strlen(key));
}

ant_value_t js_getprop_proto(ant_t *js, ant_value_t obj, const char *key) {
  if (vtype(obj) == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) == kTypeFunction) obj = promoted;
  }
  size_t key_len = strlen(key);
  ant_prop_loc_t off = lkp_proto(js, obj, key, key_len);
  return !off.obj ? js_mkundef() : js_prop_load(off);
}

static bool js_try_get(ant_t *js, ant_value_t obj, const char *key, ant_value_t *out) {
  return js_try_get_len(js, obj, key, strlen(key), out);
}

ant_value_t js_getprop_fallback_len(ant_t *js, ant_value_t obj, const char *name, size_t key_len) {
  ant_value_t val;
  if (js_try_get_len(js, obj, name, key_len, &val)) return val;

  ant_value_t cur = obj;
  uint8_t t = vtype(cur);
  proto_walk_overflow_guard_t guard;
  proto_walk_overflow_guard_init(&guard);

  while (proto_walk_next(js, &cur, &t, PROTO_WALK_F_LOOKUP)) {
    if (is_object_type(cur)) {
      ant_value_t cur_obj = js_as_obj(cur);
      if (is_proxy(cur_obj))
        return proxy_get_with_receiver(js, cur_obj, name, key_len, obj);
      ant_prop_loc_t off = lkp(js, cur_obj, name, key_len);
      if (off.obj) return js_prop_load_from(js, obj, off);
      ant_value_t element;
      if (array_own_value(js, cur_obj, name, key_len, &element)) return element;
      ant_value_t exotic = exotic_own_get(js, cur_obj, (const ant_object_t *)vptr(cur_obj), name, key_len);
      if (vtype(exotic) != kTypeUndefined) return exotic == T_EMPTY ? js_mkundef() : exotic;
    } else if (t == kTypeBuiltin) {
      ant_value_t promoted = js_cfunc_lookup_promoted(js, cur);
      if (vtype(promoted) == kTypeFunction) {
        ant_prop_loc_t off = lkp(js, js_func_obj(promoted), name, key_len);
        if (off.obj) return js_prop_load_from(js, obj, off);
      }
    }
    if (proto_walk_overflow_guard_hit_cycle(js, &guard, cur, t, PROTO_WALK_F_LOOKUP)) break;
  }

  return js_mkundef();
}

ant_value_t js_getprop_fallback(ant_t *js, ant_value_t obj, const char *name) {
  return js_getprop_fallback_len(js, obj, name, strlen(name));
}

ant_value_t js_getprop_super(ant_t *js, ant_value_t super_obj, ant_value_t receiver, const char *name) {
  if (!name) return js_mkundef();

  if (vtype(super_obj) == kTypeArray && (is_length_key(name, strlen(name)) || (name[0] >= '0' && name[0] <= '9'))) {
    ant_value_t own_value;
    if (js_try_get(js, super_obj, name, &own_value)) return own_value;
  }

  if (vtype(super_obj) == kTypeFunction) super_obj = js_func_obj(super_obj);
  if (!is_object_type(super_obj)) return js_mkundef();

  size_t key_len = strlen(name);
  if (is_proxy(super_obj)) return proxy_get(js, super_obj, name, key_len);

  const char *key_intern = intern_string(name, key_len);
  if (!key_intern) return js_mkundef();

  ant_value_t cur = super_obj;
  proto_overflow_guard_t guard;
  proto_overflow_guard_init(&guard);
  while (is_object_type(cur)) {
    ant_value_t cur_obj = js_as_obj(cur);
    ant_object_t *cur_ptr = js_obj_ptr(cur_obj);
    bool handled = false;

    if (cur_ptr && cur_ptr->shape) {
      int32_t slot = ant_shape_lookup_interned(cur_ptr->shape, key_intern);
      if (slot >= 0) {
        const ant_shape_prop_t *prop = ant_shape_prop_at(cur_ptr->shape, (uint32_t)slot);
        if (prop && prop->has_getter) {
          ant_value_t getter = prop->getter;
          if (vtype(getter) == kTypeFunction || vtype(getter) == kTypeBuiltin)
            return sv_vm_call(js->vm, js, getter, receiver, NULL, 0, NULL, js_mkundef());
          return js_mkundef();
        }
        if (prop && prop->has_setter) return js_mkundef();
        handled = true;
      }
    }

    ant_value_t element;
    if (!handled && array_own_value(js, cur_obj, name, key_len, &element)) return element;

    if (!handled && cur_ptr && cur_ptr->flags.is_exotic) {
      ant_value_t exotic = exotic_own_get(js, cur_obj, cur_ptr, name, key_len);
      if (vtype(exotic) != kTypeUndefined) return exotic == T_EMPTY ? js_mkundef() : exotic;
      descriptor_entry_t *desc = lookup_descriptor(js, cur_obj, name, key_len);
      if (desc) {
        if (desc->has_getter) {
          ant_value_t getter = desc->getter;
          if (vtype(getter) == kTypeFunction || vtype(getter) == kTypeBuiltin)
            return sv_vm_call(js->vm, js, getter, receiver, NULL, 0, NULL, js_mkundef());
          return js_mkundef();
        }
        if (desc->has_setter) return js_mkundef();
      }
    }

    ant_prop_loc_t prop_off = lkp_interned(cur_obj, key_intern);
    if (prop_off.obj) return js_prop_load(prop_off);

    ant_value_t proto = get_proto(js, cur_obj);
    if (!is_object_type(proto)) break;
    cur = proto;
    if (proto_overflow_guard_hit_cycle(js, &guard, cur)) break;
  }

  return js_mkundef();
}

typedef struct {
  bool (*callback)(ant_t *js, ant_value_t value, void *udata);
  void *udata;
} js_iter_ctx_t;

static iter_action_t js_iter_cb(ant_t *js, ant_value_t value, void *ctx, ant_value_t *out) {
  js_iter_ctx_t *ictx = (js_iter_ctx_t *)ctx;
  return ictx->callback(js, value, ictx->udata) ? ITER_CONTINUE : ITER_BREAK;
}

bool js_iter(ant_t *js, ant_value_t iterable, bool (*callback)(ant_t *js, ant_value_t value, void *udata), void *udata) {
  js_iter_ctx_t ctx = { .callback = callback, .udata = udata };
  ant_value_t result = iter_foreach(js, iterable, js_iter_cb, &ctx);
  return !is_err(result);
}

char *js_getstr(ant_t *js, ant_value_t value, size_t *len) {
  if (vtype(value) != kTypeString) return NULL;
  ant_offset_t n, off = vstr(js, value, &n);
  if (len != NULL) *len = n;
  return (char *)(uintptr_t)off;
}

void js_merge_obj(ant_t *js, ant_value_t dst, ant_value_t src) {
  if (vtype(dst) != kTypeObject || vtype(src) != kTypeObject) return;
  ant_value_t as_src = js_as_obj(src);
  ant_object_t *src_obj = js_obj_ptr(as_src);
  if (!src_obj || !src_obj->shape) return;

  uint32_t count = ant_shape_count(src_obj->shape);
  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(src_obj->shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING) continue;

    const char *key = prop->key.interned;
    ant_value_t val = (i < src_obj->prop_count) ? ant_object_prop_get_unchecked(src_obj, i) : js_mkundef();
    js_setprop(js, dst, js_mkstr(js, key, intern_length(key)), val);
  }
}

bool js_chkargs(ant_value_t *args, int nargs, const char *spec) {
  int i = 0, ok = 1;
  for (; ok && i < nargs && spec[i]; i++) {
    uint8_t t = vtype(args[i]), c = (uint8_t) spec[i];
    ok = (c == 'b' && t == kTypeBool) || (c == 'd' && t == kTypeNumber) ||
         (c == 's' && t == kTypeString) || (c == 'j');
  }
  if (spec[i] != '\0' || i != nargs) ok = 0;
  return ok;
}

sv_func_t *js_compile_parsed_bytecode(
  ant_t *js, sv_ast_t *program, const char *buf,
  size_t len, int mode_value, ant_value_t eval_env
) {
  sv_compile_mode_t mode = (sv_compile_mode_t)mode_value;

  if (mode != SV_COMPILE_MODULE && mode != SV_COMPILE_REPL && (program->flags & FN_MODULE_SYNTAX)) {
    js_mkerr_typed(js, JS_ERR_SYNTAX, "Cannot use import/export syntax outside a module");
    return NULL;
  }

  if (mode == SV_COMPILE_MODULE) {
    ant_value_t ns = js_module_eval_active_ns(js);
    if (is_object_type(ns)) esm_predeclare_exports(js, program, ns);
  }

  sv_func_t *func = sv_compile(js, program, mode, buf, (ant_offset_t)len, eval_env);
  if (!func) {
    if (!Ant_Exception_Pending(js)) js_mkerr_typed(js, JS_ERR_INTERNAL | JS_ERR_NO_STACK, "Unexpected compile error");
    return NULL;
  }

  return func;
}

static ant_value_t execute_top_level(
  ant_t *js, sv_func_t *func,
  ant_value_t this_val, coroutine_t **async_coro_out
) {
  if (async_coro_out) *async_coro_out = NULL;
  if (sv_dump_bytecode_unlikely) sv_disasm(js, func, js->filename);
  if (func->is_tla) return sv_start_tla(js, func, this_val, async_coro_out);
  return sv_execute_entry(js->vm, func, this_val, NULL, 0);
}

ant_value_t js_execute_compiled_bytecode(ant_t *js, sv_func_t *func, coroutine_t **async_coro_out) {
  return execute_top_level(js, func, js->global_this, async_coro_out);
}

ant_value_t js_execute_compiled_module(ant_t *js, sv_func_t *func) {
  return execute_top_level(js, func, js_mkundef(), NULL);
}

// Runs a REPL entry as a script sharing the global lexical environment.
static ant_value_t js_execute_compiled_repl_bytecode(
  ant_t *js, sv_func_t *func, coroutine_t **async_coro_out
) {
  if (async_coro_out) *async_coro_out = NULL;
  GC_ROOT_SAVE(mark, js);

  ant_value_t compiled = mkref(kTypeFunctionInfo, func);
  GC_ROOT_PIN(js, compiled);

  ant_value_t result = sv_global_declare(js, func);
  if (!is_err(result))
    result = js_execute_compiled_bytecode(js, func, async_coro_out);

  GC_ROOT_RESTORE(js, mark);
  return result;
}

static ant_value_t js_execute_compiled_eval_bytecode(
  ant_t *js, sv_func_t *func,
  ant_value_t this_val, ant_value_t eval_env, ant_value_t new_target
) {
  if (sv_dump_bytecode_unlikely) sv_disasm(js, func, js->filename);
  GC_ROOT_SAVE(mark, js);

  ant_value_t compiled = mkref(kTypeFunctionInfo, func);
  GC_ROOT_PIN(js, compiled);

  ant_value_t result = sv_eval_declare_vars(js, func, eval_env, true);
  if (!is_err(result))
    result = sv_execute_eval_entry(js->vm, func, this_val, eval_env, new_target);

  GC_ROOT_RESTORE(js, mark);
  return result;
}

static inline js_eval_result_t js_eval_bytecode_mode_result(
  ant_t *js, const char *buf, size_t len, 
  sv_compile_mode_t mode, bool parse_strict,
  ant_value_t eval_this, ant_value_t eval_env, ant_value_t new_target
) {
  if (len == (size_t)~0U) len = strlen(buf);

  code_arena_mark_t parse_mark = parse_arena_mark(js);
  sv_parse_goal_t goal =
    mode == SV_COMPILE_MODULE ? SV_PARSE_MODULE :
    mode == SV_COMPILE_REPL ? SV_PARSE_REPL : SV_PARSE_SCRIPT;
  sv_ast_t *program = sv_parse(js, buf, (ant_offset_t)len, goal, parse_strict);

  if (!program) {
    parse_arena_rewind(js, parse_mark);
    ant_value_t value = Ant_Exception_Pending(js)
      ? Ant_Exception_Current(js)
      : js_mkerr_typed(js, JS_ERR_INTERNAL | JS_ERR_NO_STACK, "Unexpected parse error");
    
    return (js_eval_result_t){
      .value = value,
      .async_coro = NULL,
      .kind = JS_EVAL_COMPLETE,
    };
  }

  sv_func_t *func = js_compile_parsed_bytecode(js, program, buf, len, mode, eval_env);
  parse_arena_rewind(js, parse_mark);

  if (!func) {
    ant_value_t value = Ant_Exception_Pending(js)
      ? Ant_Exception_Current(js)
      : js_mkerr_typed(js, JS_ERR_INTERNAL | JS_ERR_NO_STACK, "Unexpected compile error");
    
    return (js_eval_result_t){
      .value = value,
      .async_coro = NULL,
      .kind = JS_EVAL_COMPLETE,
    };
  }

  coroutine_t *async_coro = NULL;
  ant_value_t value;
  
  switch (mode) {
    case SV_COMPILE_EVAL:
    case SV_COMPILE_EVAL_FUNCTION:
      value = js_execute_compiled_eval_bytecode(js, func, eval_this, eval_env, new_target);
      break;
    case SV_COMPILE_REPL:
      value = js_execute_compiled_repl_bytecode(js, func, &async_coro);
      break;
    case SV_COMPILE_MODULE:
      value = js_execute_compiled_module(js, func);
      break;
    case SV_COMPILE_SCRIPT:
    default:
      value = js_execute_compiled_bytecode(js, func, NULL);
      break;
  }

  sv_code_unit_unpin(func->unit);
  
  return (js_eval_result_t){
    .value = value,
    .async_coro = async_coro,
    .kind = async_coro ? JS_EVAL_ASYNC_ENTRY : JS_EVAL_COMPLETE,
  };
}

static inline ant_value_t js_eval_bytecode_mode(
  ant_t *js, const char *buf, size_t len,
  sv_compile_mode_t mode, bool parse_strict,
  ant_value_t eval_this, ant_value_t eval_env, ant_value_t new_target
) {
  return js_eval_bytecode_mode_result(
    js, buf, len, mode, 
    parse_strict, eval_this, eval_env, new_target
  ).value;
}

ant_value_t js_eval_bytecode(ant_t *js, const char *buf, size_t len) {
  return js_eval_bytecode_mode(
    js, buf, len, SV_COMPILE_SCRIPT, 
    false, js_mkundef(), js_mkundef(), js_mkundef()
  );
}

ant_value_t js_eval_bytecode_module(ant_t *js, const char *buf, size_t len) {
  return js_eval_bytecode_mode(
    js, buf, len, SV_COMPILE_MODULE, 
    false, js_mkundef(), js_mkundef(), js_mkundef()
  );
}

ant_value_t js_eval_bytecode_eval(ant_t *js, const char *buf, size_t len) {
  return js_eval_bytecode_mode(
    js, buf, len, SV_COMPILE_EVAL, 
    false, js->global, js->global, js_mkundef()
  );
}

ant_value_t js_eval_bytecode_eval_with_strict(ant_t *js, const char *buf, size_t len, bool inherit_strict) {
  return js_eval_bytecode_mode(
    js, buf, len, SV_COMPILE_EVAL, 
    inherit_strict, js->global, js->global, js_mkundef()
  );
}

ant_value_t js_eval_bytecode_eval_in_env_with_strict(
  ant_t *js, const char *buf, size_t len,
  bool inherit_strict, ant_value_t this_val, ant_value_t eval_env,
  ant_value_t new_target, bool allows_new_target
) {
  return js_eval_bytecode_mode(
    js, buf, len, allows_new_target ? SV_COMPILE_EVAL_FUNCTION : SV_COMPILE_EVAL,
    inherit_strict, this_val, eval_env, new_target
  );
}

ant_value_t js_vm_script_compile(ant_t *js, const char *buf, size_t len) {
  code_arena_mark_t parse_mark = parse_arena_mark(js);
  sv_ast_t *program = sv_parse(js, buf, (ant_offset_t)len, SV_PARSE_SCRIPT, false);
  
  sv_func_t *func = program
    ? js_compile_parsed_bytecode(js, program, buf, len, SV_COMPILE_REPL, js_mkundef())
    : NULL;
  
  parse_arena_rewind(js, parse_mark);

  if (!func) return Ant_Exception_Pending(js)
    ? Ant_Exception_Current(js)
    : js_mkerr_typed(js, JS_ERR_SYNTAX | JS_ERR_NO_STACK, "Unexpected parse error");

  if (func->debug) func->debug->name = func->debug->display_name = "<script>";

  ant_value_t compiled = mkref(kTypeFunctionInfo, func);
  sv_code_unit_unpin(func->unit);
  
  return compiled;
}

ant_value_t js_vm_script_run(ant_t *js, ant_value_t compiled) {
  return js_execute_compiled_repl_bytecode(js, (sv_func_t *)vptr(compiled), NULL);
}

ant_value_t js_function_from_source(ant_t *js, ant_value_t *params_and_body, int nargs) {
  return build_dynamic_function(js, params_and_body, nargs, js_mkundef(), false, false);
}

js_eval_result_t js_eval_bytecode_repl(
  ant_t *js, const char *buf, size_t len
) {
  return js_eval_bytecode_mode_result(
    js, buf, len, SV_COMPILE_REPL,
    false, js_mkundef(), js_mkundef(), js_mkundef()
  );
}

static inline ant_value_t sv_call_slot_cfunc(ant_t *js, ant_value_t func, ant_value_t this_val, ant_value_t *args, int nargs, ant_value_t cfunc_slot, ant_value_t new_target) {
  ant_value_t saved_func = js->current_func;
  js->current_func = func;
  ant_value_t res = sv_call_builtin_this(js, cfunc_slot, this_val, args, nargs, new_target);
  js->current_func = saved_func;
  return res;
}


static inline ant_value_t sv_call_object_builtin(ant_t *js, ant_value_t this_val, ant_value_t *args, int nargs, ant_value_t new_target) {
  ant_value_t saved_this = js->this_val;
  js->this_val = this_val;
  ant_value_t res = sv_invoke_native(js, builtin_Object, args, nargs, new_target);
  js->this_val = saved_this;
  return res;
}

ant_value_t sv_call_native(
  ant_t *js, ant_value_t func, ant_value_t this_val,
  ant_value_t *args, int nargs, ant_value_t new_target
) {
  if (vtype(func) == kTypeBuiltin) return sv_call_builtin_this(js, func, this_val, args, nargs, new_target);
  
  if (vtype(func) == kTypeFunction) {
    ant_object_t *func_obj = js_obj_ptr(js_func_obj(func));
    ant_value_t cfunc_slot = obj_extra_get(func_obj, SLOT_CFUNC);
    
    if (vtype(cfunc_slot) == kTypeBuiltin)
      return sv_call_slot_cfunc(js, func, this_val, args, nargs, cfunc_slot, new_target);
      
    ant_value_t builtin_slot = obj_extra_get(func_obj, SLOT_BUILTIN);
    if (vtype(builtin_slot) == kTypeNumber && (int)tod(builtin_slot) == BUILTIN_OBJECT)
      return sv_call_object_builtin(js, this_val, args, nargs, new_target);
  }

  return js_mkerr_typed(js, JS_ERR_TYPE, "%s is not a function", typestr(vtype(func)));
}

ant_iter_t js_prop_iter_begin(ant_t *js, ant_value_t obj) {
  ant_iter_t iter = {.js = NULL, .obj = NULL, .off = 0};
  
  uint8_t t = vtype(obj);
  if (t != kTypeObject && t != kTypeArray && t != kTypeFunction) return iter;

  iter.obj = js_obj_ptr(js_as_obj(obj));
  if (!iter.obj || !iter.obj->shape) {
    iter.obj = NULL;
    return iter;
  }
  
  iter.js = js;
  return iter;
}

bool js_prop_iter_next(ant_iter_t *iter, const char **key, size_t *key_len, ant_value_t *value) {
  ant_iter_key_t meta = {0};
  
  while (js_prop_iter_next_key(iter, &meta, value)) {
    if (meta.is_symbol) continue;
    if (key) *key = meta.str;
    if (key_len) *key_len = meta.key_len;
    return true;
  }

  return false;
}

bool js_prop_iter_next_key(ant_iter_t *iter, ant_iter_key_t *key_out, ant_value_t *value) {
  if (!iter || !iter->obj) return false;
  ant_object_t *obj = iter->obj;
  
  if (!obj || !obj->shape) return false;
  uint32_t count = ant_shape_count(obj->shape);
  
  while (iter->off < count) {
    uint32_t i = (uint32_t)iter->off++;
    const ant_shape_prop_t *prop = ant_shape_prop_at(obj->shape, i);
    if (!prop) continue;
    if (i >= obj->prop_count) continue;
    if (key_out) {
      key_out->slot = i;
      key_out->is_symbol = (prop->type == ANT_SHAPE_KEY_SYMBOL);
      if (key_out->is_symbol) {
        key_out->str = NULL;
        key_out->key_len = 0;
        key_out->sym_off = prop->key.sym_off;
      } else {
        key_out->str = prop->key.interned;
        key_out->key_len = intern_length(prop->key.interned);
        key_out->sym_off = 0;
      }
    }
    
    if (value) *value = ant_object_prop_get_unchecked(obj, i);
    return true;
  }

  return false;
}

bool js_prop_iter_next_val(ant_iter_t *iter, ant_value_t *key_out, ant_value_t *value) {
  ant_iter_key_t meta = {0};

  if (!iter || !iter->obj) return false;
  if (!js_prop_iter_next_key(iter, &meta, value)) return false;

  if (key_out) {
    if (meta.is_symbol) *key_out = mkval(kTypeSymbol, meta.sym_off);
    else *key_out = js_mkstr(iter->js, meta.str, meta.key_len);
  }

  return true;
}

void js_prop_iter_end(ant_iter_t *iter) {
  if (!iter) return;
  iter->js = NULL;
  iter->obj = NULL;
  iter->off = 0;
}

static void report_unhandled_rejection(ant_t *js, ant_value_t promise, ant_value_t reason) {
  if (process_has_event_listeners(js, "unhandledRejection")) {
    ant_value_t event_args[2] = { reason, promise };
    emit_process_event(js, "unhandledRejection", event_args, 2);
    process_report_uncaught_exception_if_pending(js);
    return;
  }

  if (js_fire_unhandled_rejection(js, promise, reason)) return;

  if (process_has_event_listeners(js, "uncaughtException")) {
    ant_value_t event_args[2] = { reason, js_mkstr(js, "unhandledRejection", 18) };
    emit_process_event(js, "uncaughtException", event_args, 2);
    
    if (Ant_Exception_Pending(js)) {
      js_take_thrown(js, js_mkundef());
      if (!js->uncaught_nonfatal) exit(7);
    }
    
    return;
  }

  print_unhandled_promise_rejection(js, reason);
  if (!js->uncaught_nonfatal) process_exit_with(js, EXIT_FAILURE);
}

void js_check_unhandled_rejections(ant_t *js) {
  size_t keep = 0;
  
  for (size_t i = 0; i < js->pending_rejections.len; i++) {
    ant_value_t p = js->pending_rejections.items[i];
    ant_promise_state_t *pd = get_promise_data(js, p, false);
    if (!pd || pd->has_rejection_handler || pd->unhandled_reported) continue;
    
    if (vtype(pd->trigger_parent) == kTypePromise) {
      ant_promise_state_t *parent = get_promise_data(js, pd->trigger_parent, false);
      if (parent && parent->has_rejection_handler) continue;
    }
    
    if (js->fatal_error) {
      js_throw(js, pd->value);
      print_uncaught_throw(js);
      int status = process_run_exit_handlers(js, EXIT_FAILURE);
      js_destroy(js); exit(status);
    }

    if (js_is_termination(pd->value)) continue;

    GC_ROOT_SAVE(root_mark, js);
    ant_value_t reason = pd->value;
    GC_ROOT_PIN(js, p); GC_ROOT_PIN(js, reason);
    
    pd->unhandled_reported = true;
    report_unhandled_rejection(js, p, reason);
    GC_ROOT_RESTORE(js, root_mark);
  }
  
  js->pending_rejections.len = keep;
}

void js_set_getter(ant_value_t obj, js_getter_fn getter) {
  if (!is_object_type(obj)) return;
  if (vtype(obj) != kTypeObject) obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return;
  ptr->flags.is_exotic = 1;
  ant_exotic_ops_t *ops = obj_ensure_exotic_ops(ptr);
  if (!ops) return;
  ops->getter = getter;
}

void js_set_exotic_ops(ant_value_t obj, const ant_exotic_ops_t *ops) {
  ant_object_t *ptr = is_object_type(obj) ? js_obj_ptr(js_as_obj(obj)) : NULL;
  if (!ptr) return;
  ant_object_sidecar_t *sidecar = ant_object_owned_exotic_ops(ptr) ? ant_object_sidecar(ptr) : NULL;
  if (sidecar) {
    free(sidecar->owned_exotic_ops);
    sidecar->owned_exotic_ops = NULL;
  }
  ptr->flags.is_exotic = 1;
  ptr->exotic_ops = ops;
}

void js_set_definer(ant_value_t obj, js_definer_fn definer) {
  if (!is_object_type(obj)) return;
  if (vtype(obj) != kTypeObject) obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return;
  ptr->flags.is_exotic = 1;
  ant_exotic_ops_t *ops = obj_ensure_exotic_ops(ptr);
  if (!ops) return;
  ops->definer = definer;
}

void js_set_setter(ant_value_t obj, js_setter_fn setter) {
  if (!is_object_type(obj)) return;
  if (vtype(obj) != kTypeObject) obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return;
  ptr->flags.is_exotic = 1;
  ant_exotic_ops_t *ops = obj_ensure_exotic_ops(ptr);
  if (!ops) return;
  ops->setter = setter;
}

void js_set_deleter(ant_value_t obj, js_deleter_fn deleter) {
  if (!is_object_type(obj)) return;
  if (vtype(obj) != kTypeObject) obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return;
  ptr->flags.is_exotic = 1;
  ant_exotic_ops_t *ops = obj_ensure_exotic_ops(ptr);
  if (!ops) return;
  ops->deleter = deleter;
}

void js_set_finalizer(ant_value_t obj, js_finalizer_fn fn) {
  if (!is_object_type(obj)) return;
  if (vtype(obj) != kTypeObject) obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return;
  ptr->finalizer = fn;
}

