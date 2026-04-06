#if defined(__GNUC__) && !defined(__clang__)
  #pragma GCC optimize("O3,inline")
#endif

#include <compat.h> // IWYU pragma: keep

#include "ant.h"
#include "utf8.h"
#include "debug.h"
#include "tokens.h"
#include "common.h"
#include "utils.h"
#include "sugar.h"
#include "base64.h"
#include "runtime.h"
#include "internal.h"
#include "errors.h"
#include "descriptors.h"
#include "shapes.h"

#include "gc.h"
#include "gc/objects.h"
#include "gc/roots.h"

#include "esm/remote.h"
#include "esm/loader.h"
#include "esm/exports.h"
#include "esm/builtin_bundle.h"

#include "silver/lexer.h"
#include "silver/compiler.h"
#include "silver/engine.h"

#include <uv.h>
#include <oxc.h>
#include <assert.h>
#include <pcre2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <utarray.h>
#include <uthash.h>
#include <float.h>
#include <tlsuv/tlsuv.h>
#include <tlsuv/http.h>
#include <minicoro.h>

#ifdef _WIN32
#include <sys/stat.h>
#else
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/resource.h>
#endif

#include "modules/bigint.h"
#include "modules/timer.h"
#include "modules/symbol.h"
#include "modules/ffi.h"
#include "modules/date.h"
#include "modules/buffer.h"
#include "modules/blob.h"
#include "modules/collections.h"
#include "modules/lmdb.h"
#include "modules/regex.h"
#include "modules/globals.h"

#define D(x) ((double)(x))

_Static_assert(sizeof(double) == 8, "NaN-boxing requires 64-bit IEEE 754 doubles");
_Static_assert(sizeof(uint64_t) == 8, "NaN-boxing requires 64-bit integers");
_Static_assert(sizeof(double) == sizeof(uint64_t), "double and uint64_t must have same size");

#if defined(__STDC_IEC_559__) || defined(__GCC_IEC_559)
#elif defined(__FAST_MATH__)
  #error "NaN-boxing is incompatible with -ffast-math"
#elif DBL_MANT_DIG != 53 || DBL_MAX_EXP != 1024
  #error "NaN-boxing requires IEEE 754 binary64 doubles"
#endif

static const char *INTERN_LENGTH = NULL;
static const char *INTERN_BUFFER = NULL;
static const char *INTERN_PROTOTYPE = NULL;
static const char *INTERN_CONSTRUCTOR = NULL;
static const char *INTERN_NAME = NULL;
static const char *INTERN_MESSAGE = NULL;
static const char *INTERN_VALUE = NULL;
static const char *INTERN_GET = NULL;
static const char *INTERN_SET = NULL;

static const char *INTERN_ARGUMENTS = NULL;
static const char *INTERN_CALLEE = NULL;
static const char *INTERN_IDX[10] = {NULL};

typedef struct interned_string {
  uint64_t hash;
  char *str;
  size_t len;
  struct interned_string *next;
} interned_string_t;

typedef struct {
  uint32_t id;
  uint32_t flags;
  const char *key;
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
  intern_buckets = ant_calloc(sizeof(*intern_buckets) * intern_bucket_count);
  if (!intern_buckets) {
    intern_bucket_count = 0;
    return false;
  }
  return true;
}

static bool intern_table_rehash(size_t new_bucket_count) {
  if (!intern_buckets || new_bucket_count < INTERN_BUCKET_MIN) return false;

  interned_string_t **next = ant_calloc(sizeof(*next) * new_bucket_count);
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
  return pd ? (uint32_t)pd->handler_count : 0;
}

static inline bool promise_has_handlers(const ant_promise_state_t *pd) {
  return promise_handler_count(pd) != 0;
}

static bool promise_handler_append(ant_promise_state_t *pd, const promise_handler_t *handler) {
  if (!pd || !handler) return false;

  if (pd->handler_count == 0) {
    pd->inline_handler = *handler;
    pd->handler_count = 1;
    return true;
  }

  if (pd->handler_count == 1) {
    if (!pd->handlers) utarray_new(pd->handlers, &promise_handler_icd);
    if (!pd->handlers) return false;
    utarray_push_back(pd->handlers, &pd->inline_handler);
    utarray_push_back(pd->handlers, handler);
    pd->handler_count = 2;
    return true;
  }

  if (!pd->handlers) utarray_new(pd->handlers, &promise_handler_icd);
  if (!pd->handlers) return false;
  
  utarray_push_back(pd->handlers, handler);
  pd->handler_count++;
  
  return true;
}

static inline promise_handler_t *promise_handler_at(ant_promise_state_t *pd, uint32_t index) {
  if (!pd || index >= pd->handler_count) return NULL;
  if (pd->handler_count == 1) return &pd->inline_handler;
  if (!pd->handlers) return NULL;
  return (promise_handler_t *)utarray_eltptr(pd->handlers, (unsigned int)index);
}

static inline void promise_handlers_clear(ant_promise_state_t *pd) {
  if (!pd) return;
  pd->handler_count = 0;
  pd->inline_handler = (promise_handler_t){ 0 };
  if (pd->handlers) utarray_clear(pd->handlers);
}

ant_value_t tov(double d) {
  union { double d; ant_value_t v; } u = {d};
  if (__builtin_expect(isnan(d), 0)) 
    return (u.v > NANBOX_PREFIX) 
    ? 0x7FF8000000000000ULL : u.v; // canonical NaN
  return u.v;
}

double tod(ant_value_t v) {
  union { ant_value_t v; double d; } u = {v}; return u.d;
}

static bool is_tagged(ant_value_t v) {
  return v > NANBOX_PREFIX;
}

size_t vdata(ant_value_t v) {
  return (size_t)(v & NANBOX_DATA_MASK);
}

ant_object_t *js_obj_ptr(ant_value_t v) {
  if (!is_object_type(v)) return NULL;
  ant_value_t as_obj = js_as_obj(v);
  return (ant_object_t *)(uintptr_t)vdata(as_obj);
}

ant_value_t js_obj_from_ptr(ant_object_t *obj) {
  if (!obj) return js_mkundef();
  return mkval(T_OBJ, (uintptr_t)obj);
}

void js_mark_constructor(ant_value_t value, bool is_constructor) {
  ant_object_t *obj = js_obj_ptr(value);
  if (obj) obj->is_constructor = is_constructor ? 1u : 0u;
}

static inline ant_flat_string_t *str_flat_ptr(ant_value_t value) {
  if (vtype(value) != T_STR || str_is_heap_rope(value)) return NULL;
  return (ant_flat_string_t *)(uintptr_t)vdata(value);
}

static inline ant_rope_heap_t *str_rope_ptr(ant_value_t value) {
  return (ant_rope_heap_t *)(uintptr_t)(vdata(value) & ~1ULL);
}

static inline ant_value_t mkrope_value(ant_rope_heap_t *rope) {
  return mkval(T_STR, ((uintptr_t)rope) | 1ULL);
}

static inline ant_extra_slot_t *obj_extra_slots(ant_object_t *obj) {
  return (ant_extra_slot_t *)obj->extra_slots;
}

static ant_value_t obj_extra_get(ant_object_t *obj, internal_slot_t slot) {
  if (!obj || obj->extra_count == 0) return js_mkundef();
  ant_extra_slot_t *entries = obj_extra_slots(obj);
  for (uint8_t i = 0; i < obj->extra_count; i++) {
    if ((internal_slot_t)entries[i].slot == slot) return entries[i].value;
  }
  return js_mkundef();
}

static bool obj_extra_set(ant_object_t *obj, internal_slot_t slot, ant_value_t value) {
  if (!obj) return false;
  ant_extra_slot_t *entries = obj_extra_slots(obj);
  for (uint8_t i = 0; i < obj->extra_count; i++) {
    if ((internal_slot_t)entries[i].slot == slot) {
      entries[i].value = value;
      return true;
    }
  }

  if (obj->extra_count == UINT8_MAX) return false;
  uint8_t next_count = (uint8_t)(obj->extra_count + 1);
  ant_extra_slot_t *next = realloc(entries, sizeof(*next) * next_count);
  if (!next) return false;

  next[obj->extra_count].slot = (uint8_t)slot;
  next[obj->extra_count].value = value;
  obj->extra_slots = (ant_value_t *)next;
  obj->extra_count = next_count;
  return true;
}

static ant_offset_t propref_make(ant_t *js, ant_object_t *obj, uint32_t slot) {
  if (!js || !obj) return 0;

  if (js->prop_refs_len >= js->prop_refs_cap) {
    ant_offset_t next_cap = js->prop_refs_cap ? js->prop_refs_cap * 2 : 256;
    ant_prop_ref_t *next = realloc(js->prop_refs, sizeof(*next) * next_cap);
    if (!next) return 0;
    js->prop_refs = next;
    js->prop_refs_cap = next_cap;
  }

  ant_offset_t handle = js->prop_refs_len + 1;
  js->prop_refs[js->prop_refs_len++] = (ant_prop_ref_t){
    .obj = obj,
    .slot = slot,
    .valid = true,
  };
  obj->propref_count++;
  return handle;
}

static ant_prop_ref_t *propref_get(ant_t *js, ant_offset_t handle) {
  if (!js || handle == 0 || handle > js->prop_refs_len) return NULL;
  ant_prop_ref_t *ref = &js->prop_refs[handle - 1];
  return ref->valid ? ref : NULL;
}

static inline ant_value_t propref_load(ant_t *js, ant_offset_t handle) {
  ant_prop_ref_t *ref = propref_get(js, handle);
  if (!ref || !ref->obj || ref->slot >= ref->obj->prop_count) return js_mkundef();
  return ant_object_prop_get_unchecked(ref->obj, ref->slot);
}

static inline bool propref_store(ant_t *js, ant_offset_t handle, ant_value_t value) {
  ant_prop_ref_t *ref = propref_get(js, handle);
  if (!ref || !ref->obj || ref->slot >= ref->obj->prop_count) return false;
  ant_object_prop_set_unchecked(ref->obj, ref->slot, value);
  gc_write_barrier(js, ref->obj, value);
  return true;
}

static void propref_adjust_after_swap_delete(ant_t *js, ant_object_t *obj, uint32_t deleted_slot, uint32_t swapped_from) {
  if (!js || !obj || obj->propref_count == 0) return;
  
  for (ant_offset_t i = js->prop_refs_len; i-- > 0;) {
    ant_prop_ref_t *ref = &js->prop_refs[i];
    if (!ref->valid || ref->obj != obj) continue;
    
    if (ref->slot == deleted_slot) {
      ref->valid = false;
      obj->propref_count--;
      if (obj->propref_count == 0) return;
    } else if (ref->slot == swapped_from) ref->slot = deleted_slot;
  }
}

bool js_obj_ensure_prop_capacity(ant_object_t *obj, uint32_t needed) {
  if (!obj) return false;
  uint32_t inobj_limit = ant_object_inobj_limit(obj);
  uint32_t old_count = obj->prop_count;
  if (needed <= obj->prop_count) return true;

  if (needed > inobj_limit) {
    uint32_t overflow_needed = needed - inobj_limit;
    if (overflow_needed > obj->overflow_cap) {
      uint32_t new_cap = obj->overflow_cap ? (uint32_t)obj->overflow_cap * 2 : 4;
      while (new_cap < overflow_needed) new_cap *= 2;
      
      if (new_cap > 255) new_cap = overflow_needed;
      ant_value_t *next = realloc(obj->overflow_prop, sizeof(*next) * new_cap);
      if (!next) return false;
      
      obj->overflow_prop = next;
      obj->overflow_cap = (uint8_t)new_cap;
    }
  } else if (obj->overflow_prop) {
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

static void obj_remove_prop_slot(ant_object_t *obj, uint32_t slot) {
  if (!obj || slot >= obj->prop_count) return;
  uint32_t last = obj->prop_count - 1;
  if (slot != last) {
    ant_object_prop_set_unchecked(obj, slot, ant_object_prop_get_unchecked(obj, last));
  }
  obj->prop_count--;
}

static ant_exotic_ops_t *obj_ensure_exotic_ops(ant_object_t *obj) {
  if (!obj) return NULL;
  if (!obj->exotic_ops) {
    ant_exotic_ops_t *ops = calloc(1, sizeof(*ops));
    if (!ops) return NULL;
    obj->exotic_ops = ops;
  }
  return (ant_exotic_ops_t *)(void *)obj->exotic_ops;
}

static ant_object_t *obj_alloc(ant_t *js, uint8_t type_tag, uint8_t inobj_limit) {
  size_t threshold = GC_HEAP_GROWTH(js->gc_last_live);
  
  if (threshold < 2048) threshold = 2048;
  if (js->obj_arena.live_count >= threshold) gc_run(js);

  ant_object_t *obj = (ant_object_t *)fixed_arena_alloc(&js->obj_arena);
  if (!obj) return NULL;

  obj->type_tag = type_tag;
  obj->proto = js_mkundef();
  obj->shape = ant_shape_new_with_inobj_limit(inobj_limit);
  obj->overflow_prop = NULL;
  obj->overflow_cap = 0;
  obj->prop_count = 0;
  obj->inobj_limit = ant_shape_get_inobj_limit(obj->shape);
  for (uint32_t i = 0; i < ANT_INOBJ_MAX_SLOTS; i++) obj->inobj[i] = js_mkundef();
  obj->extensible = 1;
  obj->frozen = 0;
  obj->sealed = 0;
  obj->is_exotic = 0;
  obj->is_constructor = 0;
  obj->fast_array = 0;
  obj->exotic_ops = NULL;
  obj->exotic_keys = NULL;
  obj->promise_state = NULL;
  obj->proxy_state = NULL;
  obj->u.data.value = js_mkundef();
  obj->extra_slots = NULL;
  obj->extra_count = 0;
  obj->gc_pending_next = NULL;
  obj->gc_pending_rooted = false;
  obj->generation = 0;
  obj->in_remember_set = 0;

  obj->next = js->objects;
  js->objects = obj;
  
  return obj;
}

static ant_value_t get_slot(ant_value_t obj, internal_slot_t slot);
static void set_slot(ant_value_t obj, internal_slot_t slot, ant_value_t value);

static ant_value_t get_proto(ant_t *js, ant_value_t obj);
static void set_proto(ant_t *js, ant_value_t obj, ant_value_t proto);

const char *typestr(uint8_t t) {
  static const char *names[] = {
    [T_UNDEF] = "undefined", [T_NULL] = "object", [T_BOOL] = "boolean",
    [T_NUM] = "number", [T_BIGINT] = "bigint", [T_STR] = "string",
    [T_SYMBOL] = "symbol", [T_OBJ] = "object", [T_ARR] = "object",
    [T_FUNC] = "function", [T_CFUNC] = "function", [T_CLOSURE] = "closure",
    [T_PROMISE] = "object", [T_GENERATOR] = "generator",
    [T_ERR] = "err", [T_TYPEDARRAY] = "typedarray",
    [T_FFI] = "ffi", [T_NTARG] = "ntarg"
  };

  return (t < sizeof(names) / sizeof(names[0])) ? names[t] : "??";
}

uint8_t vtype(ant_value_t v) { 
  return is_tagged(v) ? ((v >> NANBOX_TYPE_SHIFT) & NANBOX_TYPE_MASK) : (uint8_t)T_NUM; 
}

ant_value_t mkval(uint8_t type, uint64_t data) { 
  return NANBOX_PREFIX 
    | ((ant_value_t)(type & NANBOX_TYPE_MASK) << NANBOX_TYPE_SHIFT) 
    | (data & NANBOX_DATA_MASK);
}

ant_value_t js_obj_to_func_ex(ant_value_t obj, uint8_t flags) {
  sv_closure_t *closure = js_closure_alloc(rt->js);
  if (!closure) return mkval(T_ERR, 0);
  closure->func_obj = (vtype(obj) == T_OBJ) ? obj : mkval(T_OBJ, vdata(obj));
  closure->bound_this = js_mkundef();
  closure->bound_args = js_mkundef();
  closure->super_val = js_mkundef();
  closure->call_flags = flags;
  ant_object_t *func_obj = js_obj_ptr(closure->func_obj);
  if (func_obj) {
    if (flags & SV_CALL_IS_DEFAULT_CTOR) {
      func_obj->is_constructor = 1;
    } else if (
      !func_obj->is_constructor &&
      func_obj->shape &&
      INTERN_PROTOTYPE &&
      vtype(obj_extra_get(func_obj, SLOT_CFUNC)) == T_CFUNC
    ) {
      // mark native function objects as constructors when they are
      // created with an explicit .prototype own property.
      if (ant_shape_lookup_interned(func_obj->shape, INTERN_PROTOTYPE) >= 0)
        func_obj->is_constructor = 1;
    }
  }
  return mkval(T_FUNC, (uintptr_t)closure);
}

ant_value_t js_obj_to_func(ant_value_t obj) {
  return js_obj_to_func_ex(obj, 0);
}

ant_value_t js_mktypedarray(void *data) {
  return mkval(T_TYPEDARRAY, (uintptr_t)data);
}

void *js_gettypedarray(ant_value_t val) {
  if (vtype(val) != T_TYPEDARRAY) return NULL;
  return (void *)vdata(val);
}

ant_value_t js_get_slot(ant_value_t obj, internal_slot_t slot) { 
  return get_slot(js_as_obj(obj), slot); 
}

ant_value_t js_mkffi(unsigned int index) {
  return mkval(T_FFI, (uint64_t)index);
}

int js_getffi(ant_value_t val) {
  if (vtype(val) != T_FFI) return -1;
  return (int)vdata(val);
}

typedef enum {
  NTARG_INVALID = 0,
  NTARG_NEW_TARGET = 1
} ntarg_kind_t;

static inline bool is_unboxed_obj(ant_t *js, ant_value_t val, ant_value_t expected_proto) {
  if (vtype(val) != T_OBJ) return false;
  if (vtype(get_slot(val, SLOT_PRIMITIVE)) != T_UNDEF) return false;
  ant_value_t proto = get_slot(val, SLOT_PROTO);
  return vdata(proto) == vdata(expected_proto);
}

uint32_t js_to_uint32(double d) {
  if (!isfinite(d) || d == 0) return 0;
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

static size_t strstring(ant_t *js, ant_value_t value, char *buf, size_t len);
static size_t strkey(ant_t *js, ant_value_t value, char *buf, size_t len);

ant_offset_t vstrlen(ant_t *js, ant_value_t v) { 
  if (str_is_heap_rope(v)) {
    ant_rope_heap_t *rope = str_rope_ptr(v);
    return rope ? rope->len : 0;
  }
  ant_flat_string_t *flat = str_flat_ptr(v);
  return flat ? flat->len : 0;
}

static ant_value_t proxy_read_target(ant_t *js, ant_value_t obj);
static ant_offset_t proxy_aware_length(ant_t *js, ant_value_t obj);
static ant_value_t proxy_aware_get_elem(ant_t *js, ant_value_t obj, const char *key, size_t key_len);

static ant_offset_t get_dense_buf(ant_value_t arr);
static ant_offset_t dense_capacity(ant_offset_t doff);
static ant_offset_t get_array_length(ant_t *js, ant_value_t arr);
static ant_value_t arr_get(ant_t *js, ant_value_t arr, ant_offset_t idx);
static bool arr_has(ant_t *js, ant_value_t arr, ant_offset_t idx);

static bool streq(const char *buf, size_t len, const char *p, size_t n);
static bool parse_func_params(ant_t *js, uint8_t *flags, int *out_count);
static bool try_dynamic_setter(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t value);
static uintptr_t lkp_with_setter(ant_t *js, ant_value_t obj, const char *buf, size_t len, ant_value_t *setter_out, bool *has_setter_out);
static ant_value_t call_proto_accessor(ant_t *js, ant_value_t prim, ant_value_t accessor, bool has_accessor, ant_value_t *arg, int arg_count, bool is_setter);
static ant_value_t get_prototype_for_type(ant_t *js, uint8_t type);

static inline ant_value_t lkp_val(ant_t *js, ant_value_t obj, const char *buf, size_t len);
static inline ant_value_t lkp_sym_proto_val(ant_t *js, ant_value_t obj, ant_offset_t sym_off);
static size_t tostr(ant_t *js, ant_value_t value, char *buf, size_t len);
static size_t strpromise(ant_t *js, ant_value_t value, char *buf, size_t len);

static ant_value_t js_call_valueOf(ant_t *js, ant_value_t value);
static ant_value_t js_call_toString(ant_t *js, ant_value_t value);
static ant_value_t js_call_method(ant_t *js, ant_value_t obj, const char *method, size_t method_len, ant_value_t *args, int nargs);

static inline bool is_slot_prop(ant_offset_t header);
static inline ant_offset_t next_prop(ant_offset_t header);

static ant_value_t builtin_promise_then(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t proxy_get(ant_t *js, ant_value_t proxy, const char *key, size_t key_len);
static ant_value_t proxy_get_val(ant_t *js, ant_value_t proxy, ant_value_t key_val);
static ant_value_t proxy_set(ant_t *js, ant_value_t proxy, const char *key, size_t key_len, ant_value_t value);
static ant_value_t proxy_has(ant_t *js, ant_value_t proxy, const char *key, size_t key_len);
static ant_value_t proxy_has_val(ant_t *js, ant_value_t proxy, ant_value_t key_val);
static ant_value_t proxy_get_own_property_descriptor(ant_t *js, ant_value_t proxy, ant_value_t key_val);
static ant_value_t proxy_has_own(ant_t *js, ant_value_t proxy, ant_value_t key_val);
static ant_value_t proxy_delete(ant_t *js, ant_value_t proxy, const char *key, size_t key_len);
static ant_value_t proxy_delete_val(ant_t *js, ant_value_t proxy, ant_value_t key_val);

static ant_value_t get_ctor_proto(ant_t *js, const char *name, size_t len);
static inline void array_len_set(ant_t *js, ant_value_t obj, ant_offset_t new_len);

typedef struct { ant_value_t handle; bool is_new; } ctor_t;

static ctor_t get_constructor(ant_t *js, const char *name, size_t len) {
  ctor_t ctor;
  
  ctor.handle = get_ctor_proto(js, name, len);
  ctor.is_new = (vtype(js->new_target) != T_UNDEF);
  
  return ctor;
}

ant_value_t unwrap_primitive(ant_t *js, ant_value_t val) {
  if (__builtin_expect(vtype(val) != T_OBJ, 1)) return val;
  ant_value_t prim = get_slot(val, SLOT_PRIMITIVE);
  if (__builtin_expect(vtype(prim) == T_UNDEF, 1)) return val;
  return prim;
}

static ant_value_t to_string_val(ant_t *js, ant_value_t val) {
  uint8_t t = vtype(val);
  if (t == T_STR) return val;
  if (t == T_OBJ) {
    ant_value_t prim = get_slot(val, SLOT_PRIMITIVE);
    if (vtype(prim) == T_STR) return prim;
  }
  return js_call_toString(js, val);
}

bool js_truthy(ant_t *js, ant_value_t v) {
  static const void *dispatch[] = {
    [T_OBJ]     = &&l_true,
    [T_FUNC]    = &&l_true,
    [T_CFUNC]   = &&l_true,
    [T_ARR]     = &&l_true,
    [T_PROMISE] = &&l_true,
    [T_SYMBOL]  = &&l_true,
    [T_BOOL]    = &&l_bool,
    [T_STR]     = &&l_str,
    [T_BIGINT]  = &&l_bigint,
    [T_NUM]     = &&l_num,
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

static size_t cpy(char *dst, size_t dstlen, const char *src, size_t srclen) {
  if (dstlen == 0) return srclen;
  size_t len = srclen < dstlen - 1 ? srclen : dstlen - 1;
  memcpy(dst, src, len); dst[len] = '\0';
  return srclen;
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

static ant_value_t stringify_stack[MAX_STRINGIFY_DEPTH];
static int stringify_depth = 0;
static int stringify_indent = 0;

static ant_value_t multiref_objs[MAX_MULTIREF_OBJS];
static int multiref_ids[MAX_MULTIREF_OBJS];
static int multiref_count = 0;
static int multiref_next_id = 0;

static void scan_refs(ant_t *js, ant_value_t value);

static int find_multiref(ant_value_t obj) {
  for (int i = 0; i < multiref_count; i++) {
    if (multiref_objs[i] == obj) return multiref_ids[i];
  }
  return 0;
}

static bool is_on_stack(ant_value_t obj) {
  for (int i = 0; i < stringify_depth; i++) {
    if (stringify_stack[i] == obj) return true;
  }
  return false;
}

static void mark_multiref(ant_value_t obj) {
  for (int i = 0; i < multiref_count; i++) {
    if (multiref_objs[i] == obj) {
      if (multiref_ids[i] == 0) multiref_ids[i] = ++multiref_next_id;
      return;
    }
  }
  if (multiref_count < MAX_MULTIREF_OBJS) {
    multiref_objs[multiref_count] = obj;
    multiref_ids[multiref_count] = 0;
    multiref_count++;
  }
}

static void scan_obj_refs(ant_t *js, ant_value_t obj) {
  if (is_on_stack(obj)) {
    mark_multiref(obj);
    return;
  }
  
  if (stringify_depth >= MAX_STRINGIFY_DEPTH) return;
  stringify_stack[stringify_depth++] = obj;
  
  ant_object_t *ptr = js_obj_ptr(obj);
  if (ptr && ptr->shape) {
    uint32_t count = ant_shape_count(ptr->shape);
    for (uint32_t i = 0; i < count && i < ptr->prop_count; i++) {
      scan_refs(js, ant_object_prop_get_unchecked(ptr, i));
    }
  }

  ant_value_t proto_val = get_proto(js, obj);
  if (vtype(proto_val) == T_OBJ) scan_refs(js, proto_val);
  
  stringify_depth--;
}

static void scan_arr_refs(ant_t *js, ant_value_t obj) {
  if (is_on_stack(obj)) {
    mark_multiref(obj);
    return;
  }
  
  if (stringify_depth >= MAX_STRINGIFY_DEPTH) return;
  stringify_stack[stringify_depth++] = obj;
  
  ant_object_t *ptr = js_obj_ptr(obj);
  if (ptr && ptr->shape) {
    uint32_t count = ant_shape_count(ptr->shape);
    for (uint32_t i = 0; i < count && i < ptr->prop_count; i++) {
      scan_refs(js, ant_object_prop_get_unchecked(ptr, i));
    }
  }
  
  stringify_depth--;
}

static void scan_func_refs(ant_t *js, ant_value_t value) {
  ant_value_t func_obj = js_func_obj(value);
  
  if (is_on_stack(func_obj)) {
    mark_multiref(func_obj);
    return;
  }
  
  if (stringify_depth >= MAX_STRINGIFY_DEPTH) return;
  stringify_stack[stringify_depth++] = func_obj;
  
  ant_object_t *ptr = js_obj_ptr(func_obj);
  if (ptr && ptr->shape) {
    uint32_t count = ant_shape_count(ptr->shape);
    for (uint32_t i = 0; i < count && i < ptr->prop_count; i++) {
      scan_refs(js, ant_object_prop_get_unchecked(ptr, i));
    }
  }
  
  stringify_depth--;
}

static void scan_refs(ant_t *js, ant_value_t value) {
  switch (vtype(value)) {
    case T_OBJ: scan_obj_refs(js, value); break;
    case T_ARR: scan_arr_refs(js, value); break;
    case T_FUNC: scan_func_refs(js, value); break;
    default: break;
  }
}

static int get_circular_ref(ant_value_t obj) {
  if (is_on_stack(obj)) {
    int ref = find_multiref(obj);
    return ref ? ref : -1;
  }
  return 0;
}

static bool is_circular(ant_value_t obj) {
  return is_on_stack(obj);
}

static int get_self_ref(ant_value_t obj) {
  return find_multiref(obj);
}

static void push_stringify(ant_value_t obj) {
  if (stringify_depth < MAX_STRINGIFY_DEPTH) {
    stringify_stack[stringify_depth++] = obj;
  }
}

static void pop_stringify(void) {
  if (stringify_depth > 0) stringify_depth--;
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
  ant_value_t v = lkp_val(js, obj, key, klen);
  
  GC_ROOT_PIN(js, v);
  if (vtype(v) != T_STR) {
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
    if (t == T_OBJ || t == T_ARR || t == T_FUNC) has_nested = true;
    count++;
  }
  
  if (elem_count) *elem_count = count;
  return count <= 4 && !has_nested;
}

static inline bool is_array_index(const char *key, ant_offset_t klen) {
  if (klen == 0 || (klen > 1 && key[0] == '0')) return false;
  for (ant_offset_t i = 0; i < klen; i++) {
    if (key[i] < '0' || key[i] > '9') return false;
  }
  return true;
}

static inline bool parse_array_index(const char *key, size_t klen, ant_offset_t max_len, unsigned long *out_idx) {
  if (klen == 0 || key[0] < '0' || key[0] > '9') return false;
  unsigned long parsed_idx = 0;
  
  for (size_t i = 0; i < klen; i++) {
    if (key[i] < '0' || key[i] > '9') return false;
    parsed_idx = parsed_idx * 10 + (key[i] - '0');
  }
  
  if (parsed_idx >= max_len) return false;
  *out_idx = parsed_idx;
  return true;
}

#define ANT_ARRAY_INDEX_EXCLUSIVE ((ant_offset_t)UINT32_MAX)

static inline ant_object_t *array_obj_ptr(ant_value_t obj) {
  if (!is_object_type(obj)) return NULL;
  ant_object_t *ptr = js_obj_ptr(obj);
  return (ptr && ptr->type_tag == T_ARR) ? ptr : NULL;
}

static inline void array_define_or_set_index(ant_t *js, ant_value_t obj, const char *key, size_t klen) {
  if (!key) return;
  if (!array_obj_ptr(obj)) return;

  unsigned long idx = 0;
  if (!parse_array_index(key, klen, ANT_ARRAY_INDEX_EXCLUSIVE, &idx)) return;

  ant_offset_t next_len = (ant_offset_t)idx + 1;
  if (next_len > get_array_length(js, obj)) {
    array_len_set(js, obj, next_len);
  }
}

static ant_offset_t get_array_length(ant_t *js, ant_value_t arr) {
  if (!is_object_type(arr)) return 0;

  ant_object_t *arr_ptr = array_obj_ptr(arr);
  if (arr_ptr) return (ant_offset_t)arr_ptr->u.array.len;

  ant_value_t val = lkp_interned_val(js, arr, INTERN_LENGTH);
  if (vtype(val) == T_NUM) return (ant_offset_t) tod(val);
  
  return 0;
}

static ant_value_t get_obj_ctor(ant_t *js, ant_value_t obj) {
  ant_value_t ctor = get_slot(obj, SLOT_CTOR);
  if (vtype(ctor) == T_FUNC) return ctor;
  ant_value_t proto = get_slot(obj, SLOT_PROTO);
  if (vtype(proto) != T_OBJ) return js_mkundef();
  return lkp_interned_val(js, proto, INTERN_CONSTRUCTOR);
}

static const char *get_func_name(ant_t *js, ant_value_t func, ant_offset_t *out_len) {
  if (vtype(func) != T_FUNC) return NULL;
  ant_value_t name = lkp_val(js, js_func_obj(func), "name", 4);
  if (vtype(name) != T_STR) return NULL;
  ant_offset_t str_off = vstr(js, name, out_len);
  return (const char *)(uintptr_t)(str_off);
}

static const char *get_class_name(ant_t *js, ant_value_t obj, ant_offset_t *out_len, const char *skip) {
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

static size_t strarr(ant_t *js, ant_value_t obj, char *buf, size_t len) {
  int ref = get_circular_ref(obj);
  if (ref) return ref > 0 ? (size_t) snprintf(buf, len, "[Circular *%d]", ref) : cpy(buf, len, "[Circular]", 10);
  
  push_stringify(obj);
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  ant_offset_t length = get_array_length(js, obj);
  ant_offset_t d_len = dense_iterable_length(js, obj);
  ant_offset_t iter_len = (d_len >= length) ? length : d_len;
  
  ant_offset_t class_len = 0;
  const char *class_name = get_class_name(js, obj, &class_len, "Array");
  
  int elem_count = 0;
  bool inline_mode = is_small_array(js, obj, &elem_count);
  size_t n = 0;
  
  if (class_name) {
    n += cpy(buf + n, REMAIN(n, len), class_name, class_len);
    n += (size_t) snprintf(buf + n, REMAIN(n, len), "(%u) ", (unsigned) length);
  }
  
  if (length == 0) {
    n += cpy(buf + n, REMAIN(n, len), "[]", 2);
    pop_stringify();
    return n;
  }
  
  n += cpy(buf + n, REMAIN(n, len), inline_mode ? "[ " : "[\n", 2);
  if (!inline_mode) stringify_indent++;
  
  bool printed_first = false;
  for (ant_offset_t i = 0; i < iter_len; i++) {
    if (printed_first) n += cpy(buf + n, REMAIN(n, len), inline_mode ? ", " : ",\n", 2);
    if (!inline_mode) n += add_indent(buf + n, REMAIN(n, len), stringify_indent);
    
    ant_value_t val = arr_get(js, obj, i); bool found = arr_has(js, obj, i);
    n += found ? tostr(js, val, buf + n, REMAIN(n, len)) : cpy(buf + n, REMAIN(n, len), "undefined", 9);
    printed_first = true;
  }
  
  if (ptr && ptr->shape) {
    uint32_t shape_count = ant_shape_count(ptr->shape);
    for (uint32_t i = 0; i < shape_count; i++) {
      const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
      if (!prop || prop->type == ANT_SHAPE_KEY_SYMBOL) continue;
      const char *key = prop->key.interned;
      ant_offset_t klen = (ant_offset_t)strlen(key);
      if (is_length_key(key, klen)) continue;

      if (printed_first) n += cpy(buf + n, REMAIN(n, len), inline_mode ? ", " : ",\n", 2);
      if (!inline_mode) n += add_indent(buf + n, REMAIN(n, len), stringify_indent);

      ant_value_t val = (i < ptr->prop_count) ? ant_object_prop_get_unchecked(ptr, i) : js_mkundef();
      if (is_array_index(key, klen)) {
        n += tostr(js, val, buf + n, REMAIN(n, len));
      } else {
        n += cpy(buf + n, REMAIN(n, len), key, klen);
        n += cpy(buf + n, REMAIN(n, len), ": ", 2);
        n += tostr(js, val, buf + n, REMAIN(n, len));
      }
      printed_first = true;
    }
  }
  
  if (!inline_mode) {
    stringify_indent--;
    n += cpy(buf + n, REMAIN(n, len), "\n", 1);
    n += add_indent(buf + n, REMAIN(n, len), stringify_indent);
  }
  
  n += cpy(buf + n, REMAIN(n, len), inline_mode ? " ]" : "]", inline_mode ? 2 : 1);
  pop_stringify();
  return n;
}

static size_t strdate(ant_t *js, ant_value_t obj, char *buf, size_t len) {
  ant_value_t time_val = js_get_slot(obj, SLOT_DATA);
  if (vtype(time_val) != T_NUM) return cpy(buf, len, "Invalid Date", 12);

  static const date_string_spec_t kSpec = {DATE_STRING_FMT_ISO, DATE_STRING_PART_ALL};
  ant_value_t iso = get_date_string(js, obj, kSpec);
  if (is_err(iso) || vtype(iso) != T_STR) return cpy(buf, len, "Invalid Date", 12);

  ant_offset_t slen;
  ant_offset_t soff = vstr(js, iso, &slen);
  
  return cpy(buf, len, (const char *)(uintptr_t)(soff), slen);
}

static bool is_valid_identifier(const char *str, ant_offset_t slen) {
  if (slen == 0) return false;
  char c = str[0];
  if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$')) return false;
  for (ant_offset_t i = 1; i < slen; i++) {
    c = str[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$')) return false;
  }
  return true;
}

static size_t strkey(ant_t *js, ant_value_t value, char *buf, size_t len) {
  ant_offset_t slen, off = vstr(js, value, &slen);
  const char *str = (const char *)(uintptr_t)(off);
  
  if (is_valid_identifier(str, slen)) {
    return cpy(buf, len, str, slen);
  }
  return strstring(js, value, buf, len);
}

static size_t strkey_interned(ant_t *js, const char *key, size_t klen, char *buf, size_t len) {
  if (is_valid_identifier(key, (ant_offset_t)klen)) {
    return cpy(buf, len, key, klen);
  }
  ant_value_t key_str = js_mkstr(js, key, klen);
  return strstring(js, key_str, buf, len);
}

static bool is_small_object(ant_t *js, ant_value_t obj, int *prop_count) {
  int count = 0;
  bool has_nested = false;

  ant_value_t as_obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(as_obj);
  uintptr_t obj_off = (uintptr_t)vdata(as_obj);
  if (ptr && ptr->shape) {
    uint32_t shape_count = ant_shape_count(ptr->shape);
    for (uint32_t i = 0; i < shape_count; i++) {
      const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
      if (!prop) continue;

      if (prop->type == ANT_SHAPE_KEY_SYMBOL) {
        count++;
        continue;
      }

      if ((ant_shape_get_attrs(ptr->shape, i) & ANT_PROP_ATTR_ENUMERABLE) == 0) continue;

      ant_value_t val = (i < ptr->prop_count) ? ant_object_prop_get_unchecked(ptr, i) : js_mkundef();
      uint8_t t = vtype(val);
      if (t == T_OBJ || t == T_ARR || t == T_FUNC) has_nested = true;
      count++;
    }
  }
  
  if (ptr && ptr->is_exotic) {
    descriptor_entry_t *desc, *tmp;
    HASH_ITER(hh, desc_registry, desc, tmp) {
      if (desc->obj_off != obj_off) continue;
      if (!desc->enumerable) continue;
      if (!desc->has_getter && !desc->has_setter) continue;
      count++;
    }
  }
  
  if (prop_count) *prop_count = count;
  return count <= 4 && !has_nested;
}

// todo: split into smaller functions
static size_t strobj(ant_t *js, ant_value_t obj, char *buf, size_t len) {
  if (is_date_instance(obj)) return strdate(js, obj, buf, len);
  
  int ref = get_circular_ref(obj);
  if (ref) return ref > 0 ? (size_t) snprintf(buf, len, "[Circular *%d]", ref) : cpy(buf, len, "[Circular]", 10);
  
  push_stringify(obj);
  
  size_t n = 0;
  int self_ref = get_self_ref(obj);
  if (self_ref) {
    n += (size_t) snprintf(buf + n, REMAIN(n, len), "<ref *%d> ", self_ref);
  }
  
  ant_value_t tag_sym = get_toStringTag_sym();
  ant_value_t tag_val = (vtype(tag_sym) == T_SYMBOL) ? lkp_sym_proto_val(js, obj, (ant_offset_t)vdata(tag_sym)) : js_mkundef();
  bool is_map = false, is_set = false, is_arraybuffer = false;
  ant_offset_t tlen = 0, toff = 0;
  const char *tag_str = NULL;
  int prop_count = 0;
  bool inline_mode = false;
  
  if (vtype(tag_val) != T_STR) goto print_plain_object;
  
  toff = vstr(js, tag_val, &tlen);
  tag_str = (const char *)(uintptr_t)(toff);
  is_map = (tlen == 3 && memcmp(tag_str, "Map", 3) == 0);
  is_set = (tlen == 3 && memcmp(tag_str, "Set", 3) == 0);
  is_arraybuffer = (tlen >= 11 && memcmp(tag_str + tlen - 11, "ArrayBuffer", 11) == 0);
  
  TypedArrayData *ta = buffer_get_typedarray_data(obj);
  if (ta && ta->buffer) {
    const char *type_name = NULL;
    size_t type_len = 0;
    
    ant_value_t proto = js_get_proto(js, obj);
    ant_value_t buffer_proto = get_ctor_proto(js, "Buffer", 6);
    if (vtype(proto) == T_OBJ && vtype(buffer_proto) == T_OBJ && vdata(proto) == vdata(buffer_proto)) {
      type_name = "Buffer";
      type_len = 6;
    } else if (ta->type <= TYPED_ARRAY_BIGUINT64) {
      type_name = buffer_typedarray_type_name(ta->type);
      type_len = strlen(type_name);
    } else {
      type_name = "TypedArray";
      type_len = 10;
    }
    
    n += cpy(buf + n, REMAIN(n, len), type_name, type_len);
    n += (size_t) snprintf(buf + n, REMAIN(n, len), "(%zu) ", ta->length);
    n += cpy(buf + n, REMAIN(n, len), "[ ", 2);
    
    uint8_t *data = ta->buffer->data + ta->byte_offset;
    
    for (size_t i = 0; i < ta->length && i < 100; i++) {
      if (i > 0) n += cpy(buf + n, REMAIN(n, len), ", ", 2);
      
      switch (ta->type) {
        case TYPED_ARRAY_INT8:
          n += (size_t) snprintf(buf + n, REMAIN(n, len), "%d", (int)((int8_t*)data)[i]);
          break;
        case TYPED_ARRAY_UINT8:
        case TYPED_ARRAY_UINT8_CLAMPED:
          n += (size_t) snprintf(buf + n, REMAIN(n, len), "%u", (unsigned)data[i]);
          break;
        case TYPED_ARRAY_INT16:
          n += (size_t) snprintf(buf + n, REMAIN(n, len), "%d", (int)((int16_t*)data)[i]);
          break;
        case TYPED_ARRAY_UINT16:
          n += (size_t) snprintf(buf + n, REMAIN(n, len), "%u", (unsigned)((uint16_t*)data)[i]);
          break;
        case TYPED_ARRAY_INT32:
          n += (size_t) snprintf(buf + n, REMAIN(n, len), "%d", ((int32_t*)data)[i]);
          break;
        case TYPED_ARRAY_UINT32:
          n += (size_t) snprintf(buf + n, REMAIN(n, len), "%u", ((uint32_t*)data)[i]);
          break;
        case TYPED_ARRAY_FLOAT16:
          n += (size_t) snprintf(
            buf + n, REMAIN(n, len), "%g", half_to_double(((uint16_t*)data)[i])
          );
          break;
        case TYPED_ARRAY_FLOAT32:
          n += (size_t) snprintf(buf + n, REMAIN(n, len), "%g", (double)((float*)data)[i]);
          break;
        case TYPED_ARRAY_FLOAT64:
          n += (size_t) snprintf(buf + n, REMAIN(n, len), "%g", ((double*)data)[i]);
          break;
        case TYPED_ARRAY_BIGINT64:
          n += (size_t) snprintf(buf + n, REMAIN(n, len), "%lldn", (long long)((int64_t*)data)[i]);
          break;
        case TYPED_ARRAY_BIGUINT64:
          n += (size_t) snprintf(buf + n, REMAIN(n, len), "%llun", (unsigned long long)((uint64_t*)data)[i]);
          break;
        default:
          n += (size_t) snprintf(buf + n, REMAIN(n, len), "%u", (unsigned)data[i]);
          break;
      }
    }
    
    if (ta->length > 100) n += cpy(buf + n, REMAIN(n, len), ", ...", 5);
    n += cpy(buf + n, REMAIN(n, len), " ]", 2);
    pop_stringify();
    return n;
  }
  
  if (is_arraybuffer) {
    ArrayBufferData *ab_data = buffer_get_arraybuffer_data(obj);
    if (ab_data) {
      size_t bytelen = ab_data ? ab_data->length : 0;
      
      n += cpy(buf + n, REMAIN(n, len), tag_str, tlen);
      n += cpy(buf + n, REMAIN(n, len), " {\n", 3);
      n += cpy(buf + n, REMAIN(n, len), "  [Uint8Contents]: <", 20);
      
      if (ab_data && ab_data->data && bytelen > 0) {
        for (size_t i = 0; i < bytelen; i++) {
          if (i > 0) n += cpy(buf + n, REMAIN(n, len), " ", 1);
          n += (size_t) snprintf(buf + n, REMAIN(n, len), "%02x", ab_data->data[i]);
        }
      }
      
      n += cpy(buf + n, REMAIN(n, len), ">,\n", 3);
      n += cpy(buf + n, REMAIN(n, len), "  [byteLength]: ", 16);
      n += (size_t) snprintf(buf + n, REMAIN(n, len), "%zu", bytelen);
      n += cpy(buf + n, REMAIN(n, len), "\n}", 2);
      pop_stringify();
      return n;
    }
  }
  
  bool is_dataview = (tlen == 8 && memcmp(tag_str, "DataView", 8) == 0);
  if (is_dataview) {
    DataViewData *dv = buffer_get_dataview_data(obj);
    if (dv && dv->buffer) {
      n += cpy(buf + n, REMAIN(n, len), "DataView {\n", 11);
      n += cpy(buf + n, REMAIN(n, len), "  [byteLength]: ", 16);
      n += (size_t) snprintf(buf + n, REMAIN(n, len), "%zu", dv->byte_length);
      n += cpy(buf + n, REMAIN(n, len), ",\n", 2);
      n += cpy(buf + n, REMAIN(n, len), "  [byteOffset]: ", 16);
      n += (size_t) snprintf(buf + n, REMAIN(n, len), "%zu", dv->byte_offset);
      n += cpy(buf + n, REMAIN(n, len), ",\n", 2);
      n += cpy(buf + n, REMAIN(n, len), "  [buffer]: ArrayBuffer {\n", 26);
      n += cpy(buf + n, REMAIN(n, len), "    [Uint8Contents]: <", 22);
      
      if (dv->buffer->data && dv->buffer->length > 0) {
        for (size_t i = 0; i < dv->buffer->length; i++) {
          if (i > 0) n += cpy(buf + n, REMAIN(n, len), " ", 1);
          n += (size_t) snprintf(buf + n, REMAIN(n, len), "%02x", dv->buffer->data[i]);
        }
      }
      
      n += cpy(buf + n, REMAIN(n, len), ">,\n", 3);
      n += cpy(buf + n, REMAIN(n, len), "    [byteLength]: ", 18);
      n += (size_t) snprintf(buf + n, REMAIN(n, len), "%zu", dv->buffer->length);
      n += cpy(buf + n, REMAIN(n, len), "\n  }\n}", 6);
      pop_stringify();
      return n;
    }
  }
  
  if (is_map) {
    ant_value_t map_val = js_get_slot(obj, SLOT_MAP);
    if (vtype(map_val) == T_UNDEF) goto print_tagged_object;
    
    map_entry_t **map_ptr = (map_entry_t**)(size_t)tod(map_val);
    n += cpy(buf + n, REMAIN(n, len), "Map(", 4);
    
    unsigned int count = 0;
    if (map_ptr && *map_ptr) count = HASH_COUNT(*map_ptr);
    n += (size_t) snprintf(buf + n, REMAIN(n, len), "%u", count);
    n += cpy(buf + n, REMAIN(n, len), ") ", 2);
    
    if (count == 0) {
      n += cpy(buf + n, REMAIN(n, len), "{}", 2);
    } else {
      n += cpy(buf + n, REMAIN(n, len), "{\n", 2);
      stringify_indent++;
      bool first = true;
      if (map_ptr && *map_ptr) {
        map_entry_t *entry, *tmp;
        HASH_ITER(hh, *map_ptr, entry, tmp) {
          if (!first) n += cpy(buf + n, REMAIN(n, len), ",\n", 2);
          first = false;
          n += add_indent(buf + n, REMAIN(n, len), stringify_indent);
          n += tostr(js, entry->key_val, buf + n, REMAIN(n, len));
          n += cpy(buf + n, REMAIN(n, len), " => ", 4);
          n += tostr(js, entry->value, buf + n, REMAIN(n, len));
        }
      }
      stringify_indent--;
      n += cpy(buf + n, REMAIN(n, len), "\n", 1);
      n += add_indent(buf + n, REMAIN(n, len), stringify_indent);
      n += cpy(buf + n, REMAIN(n, len), "}", 1);
    }
    pop_stringify();
    return n;
  }
  
  if (is_set) {
    ant_value_t set_val = js_get_slot(obj, SLOT_SET);
    if (vtype(set_val) == T_UNDEF) goto print_tagged_object;
    
    set_entry_t **set_ptr = (set_entry_t**)(size_t)tod(set_val);
    n += cpy(buf + n, REMAIN(n, len), "Set(", 4);
    
    unsigned int count = 0;
    if (set_ptr && *set_ptr) count = HASH_COUNT(*set_ptr);
    n += (size_t) snprintf(buf + n, REMAIN(n, len), "%u", count);
    n += cpy(buf + n, REMAIN(n, len), ") ", 2);
    
    if (count == 0) {
      n += cpy(buf + n, REMAIN(n, len), "{}", 2);
    } else {
      n += cpy(buf + n, REMAIN(n, len), "{\n", 2);
      stringify_indent++;
      bool first = true;
      if (set_ptr && *set_ptr) {
        set_entry_t *entry, *tmp;
        HASH_ITER(hh, *set_ptr, entry, tmp) {
          if (!first) n += cpy(buf + n, REMAIN(n, len), ",\n", 2);
          first = false;
          n += add_indent(buf + n, REMAIN(n, len), stringify_indent);
          n += tostr(js, entry->value, buf + n, REMAIN(n, len));
        }
      }
      stringify_indent--;
      n += cpy(buf + n, REMAIN(n, len), "\n", 1);
      n += add_indent(buf + n, REMAIN(n, len), stringify_indent);
      n += cpy(buf + n, REMAIN(n, len), "}", 1);
    }
    pop_stringify();
    return n;
  }
  
  if (tag_str) {
  bool is_timeout = (tlen == 7 && memcmp(tag_str, "Timeout", 7) == 0);
  bool is_interval = (tlen == 8 && memcmp(tag_str, "Interval", 8) == 0);
  if (is_timeout || is_interval) {
    ant_value_t id_val = js_get_slot(obj, SLOT_DATA);
    int timer_id = vtype(id_val) == T_NUM ? (int)js_getnum(id_val) : 0;
    n += cpy(buf + n, REMAIN(n, len), tag_str, tlen);
    n += (size_t) snprintf(buf + n, REMAIN(n, len), " (%d) {\n", timer_id);
    goto continue_object_print;
  }
  bool is_blob = (tlen == 4 && memcmp(tag_str, "Blob", 4) == 0);
  bool is_file = (tlen == 4 && memcmp(tag_str, "File", 4) == 0);
  if (is_blob || is_file) {
    blob_data_t *bd = blob_get_data(obj);
    n += cpy(buf + n, REMAIN(n, len), is_file ? "File" : "Blob", 4);
    n += cpy(buf + n, REMAIN(n, len), " { size: ", 9);
    n += (size_t) snprintf(buf + n, REMAIN(n, len), "%zu", bd ? bd->size : 0);
    n += cpy(buf + n, REMAIN(n, len), ", type: '", 9);
    if (bd && bd->type) n += cpy(buf + n, REMAIN(n, len), bd->type, strlen(bd->type));
    n += cpy(buf + n, REMAIN(n, len), "'", 1);
    if (is_file) {
      n += cpy(buf + n, REMAIN(n, len), ", name: '", 9);
      if (bd && bd->name) n += cpy(buf + n, REMAIN(n, len), bd->name, strlen(bd->name));
      n += cpy(buf + n, REMAIN(n, len), "'", 1);
      n += cpy(buf + n, REMAIN(n, len), ", lastModified: ", 16);
      n += (size_t) snprintf(buf + n, REMAIN(n, len), "%" PRId64, bd ? bd->last_modified : 0);
    }
    n += cpy(buf + n, REMAIN(n, len), " }", 2);
    pop_stringify();
    return n;
  }}

print_tagged_object:
  n += cpy(buf + n, REMAIN(n, len), "Object [", 8);
  n += cpy(buf + n, REMAIN(n, len), (const char *)(uintptr_t)(toff), tlen);
  n += cpy(buf + n, REMAIN(n, len), "] {\n", 4);
  goto continue_object_print;
  
print_plain_object:
  inline_mode = is_small_object(js, obj, &prop_count);
  
  ant_value_t proto_val = js_get_proto(js, obj);
  bool is_null_proto = (vtype(proto_val) == T_NULL);
  bool proto_is_null_proto = false;
  const char *class_name = NULL;
  ant_offset_t class_name_len = 0;
  
  do {
    if (is_null_proto) break;
    uint8_t pt = vtype(proto_val);
    if (pt != T_OBJ && pt != T_FUNC) break;
    
    ant_value_t proto_proto = js_get_proto(js, proto_val);
    ant_value_t object_proto = get_ctor_proto(js, "Object", 6);
    proto_is_null_proto = (vtype(proto_proto) == T_NULL) && 
                          (vdata(proto_val) != vdata(object_proto));
    
    class_name = get_class_name(js, obj, &class_name_len, "Object");
  } while (0);
  
  if (prop_count == 0) {
    if (is_null_proto) {
      n += cpy(buf + n, REMAIN(n, len), "[Object: null prototype] {}", 27);
    } else if (class_name && class_name_len > 0) {
      n += cpy(buf + n, REMAIN(n, len), class_name, class_name_len);
      if (proto_is_null_proto) {
        n += cpy(buf + n, REMAIN(n, len), " <[Object: null prototype] {}> {}", 33);
      } else n += cpy(buf + n, REMAIN(n, len), " {}", 3);
    } else if (proto_is_null_proto) {
      n += cpy(buf + n, REMAIN(n, len), "<[Object: null prototype] {}> {}", 32);
    } else n += cpy(buf + n, REMAIN(n, len), "{}", 2);
    pop_stringify();
    return n;
  }
  
  if (is_null_proto) {
    n += cpy(buf + n, REMAIN(n, len), "[Object: null prototype] ", 25);
  } else if (class_name && class_name_len > 0) {
    n += cpy(buf + n, REMAIN(n, len), class_name, class_name_len);
    if (proto_is_null_proto) {
      n += cpy(buf + n, REMAIN(n, len), " <[Object: null prototype] {}> ", 31);
    } else n += cpy(buf + n, REMAIN(n, len), " ", 1);
  } else if (proto_is_null_proto) {
    n += cpy(buf + n, REMAIN(n, len), "<[Object: null prototype] {}> ", 30);
  }
  
  n += cpy(buf + n, REMAIN(n, len), inline_mode ? "{ " : "{\n", 2);
  
continue_object_print:;
  
  if (!inline_mode) stringify_indent++;
  bool first = true;
  
  ant_value_t as_obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(as_obj);
  uintptr_t obj_off = (uintptr_t)vdata(as_obj);
  uint32_t shape_count = (ptr && ptr->shape) ? ant_shape_count(ptr->shape) : 0;

  for (uint32_t i = 0; i < shape_count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop) continue;
    if ((ant_shape_get_attrs(ptr->shape, i) & ANT_PROP_ATTR_ENUMERABLE) == 0) continue;

    ant_value_t val = (i < ptr->prop_count) ? ant_object_prop_get_unchecked(ptr, i) : js_mkundef();

    if (prop->type == ANT_SHAPE_KEY_SYMBOL) {
      ant_offset_t sym_off = prop->key.sym_off;
      if (vtype(tag_sym) == T_SYMBOL && sym_off == (ant_offset_t)vdata(tag_sym)) continue;

      if (ptr && ptr->is_exotic) {
        prop_meta_t meta;
        if (lookup_symbol_prop_meta(as_obj, sym_off, &meta) && !meta.enumerable) continue;
      }

      ant_value_t sym = mkval(T_SYMBOL, sym_off);

      if (!first) n += cpy(buf + n, REMAIN(n, len), inline_mode ? ", " : ",\n", 2);
      first = false;
      if (!inline_mode) n += add_indent(buf + n, REMAIN(n, len), stringify_indent);
      n += cpy(buf + n, REMAIN(n, len), "[", 1);
      n += tostr(js, sym, buf + n, REMAIN(n, len));
      n += cpy(buf + n, REMAIN(n, len), "]: ", 3);
      n += tostr(js, val, buf + n, REMAIN(n, len));
      continue;
    }

    const char *key = prop->key.interned;
    ant_offset_t klen = (ant_offset_t)strlen(key);
    if (ptr && ptr->is_exotic) {
      prop_meta_t meta;
      if (lookup_string_prop_meta(js, as_obj, key, (size_t)klen, &meta) && !meta.enumerable) continue;
    }

    if (prop->has_getter || prop->has_setter) {
      if (!first) n += cpy(buf + n, REMAIN(n, len), inline_mode ? ", " : ",\n", 2);
      first = false;
      if (!inline_mode) n += add_indent(buf + n, REMAIN(n, len), stringify_indent);
      n += strkey_interned(js, key, (size_t)klen, buf + n, REMAIN(n, len));
      n += cpy(buf + n, REMAIN(n, len), ": ", 2);
      if (prop->has_getter && prop->has_setter)
        n += cpy(buf + n, REMAIN(n, len), "[Getter/Setter]", 15);
      else if (prop->has_getter)
        n += cpy(buf + n, REMAIN(n, len), "[Getter]", 8);
      else
        n += cpy(buf + n, REMAIN(n, len), "[Setter]", 8);
      continue;
    }

    if (!first) n += cpy(buf + n, REMAIN(n, len), inline_mode ? ", " : ",\n", 2);
    first = false;
    if (!inline_mode) n += add_indent(buf + n, REMAIN(n, len), stringify_indent);

    bool is_special_global = false;
    if (vtype(val) == T_UNDEF && streq(key, klen, "undefined", 9)) {
      is_special_global = true;
    } else if (vtype(val) == T_NUM) {
      double d = tod(val);
      if (isinf(d) && d > 0 && streq(key, klen, "Infinity", 8)) {
        is_special_global = true;
      } else if (isnan(d) && streq(key, klen, "NaN", 3)) {
        is_special_global = true;
      }
    }

    if (is_special_global) {
      n += tostr(js, val, buf + n, REMAIN(n, len));
    } else {
      n += strkey_interned(js, key, (size_t)klen, buf + n, REMAIN(n, len));
      n += cpy(buf + n, REMAIN(n, len), ": ", 2);
      n += tostr(js, val, buf + n, REMAIN(n, len));
    }
  }
  
  if (ptr && ptr->is_exotic) {
    descriptor_entry_t *desc, *tmp;
    HASH_ITER(hh, desc_registry, desc, tmp) {
      if (desc->obj_off != obj_off) continue;
      if (!desc->enumerable) continue;
      if (!desc->has_getter && !desc->has_setter) continue;

      if (!first) n += cpy(buf + n, REMAIN(n, len), inline_mode ? ", " : ",\n", 2);
      first = false;
      if (!inline_mode) n += add_indent(buf + n, REMAIN(n, len), stringify_indent);
      n += cpy(buf + n, REMAIN(n, len), desc->prop_name, desc->prop_len);
      n += cpy(buf + n, REMAIN(n, len), ": ", 2);

      if (desc->has_getter && desc->has_setter) {
        n += cpy(buf + n, REMAIN(n, len), "[Getter/Setter]", 15);
      } else if (desc->has_getter) {
        n += cpy(buf + n, REMAIN(n, len), "[Getter]", 8);
      } else n += cpy(buf + n, REMAIN(n, len), "[Setter]", 8);
    }
  }
  
  if (!inline_mode) stringify_indent--;
  if (inline_mode) {
    n += cpy(buf + n, REMAIN(n, len), " }", 2);
  } else {
    if (!first) n += cpy(buf + n, REMAIN(n, len), "\n", 1);
    n += add_indent(buf + n, REMAIN(n, len), stringify_indent);
    n += cpy(buf + n, REMAIN(n, len), "}", 1);
  }
  pop_stringify();
  return n;
}

static size_t fix_exponent(char *buf, size_t n) {
  char *e = strchr(buf, 'e');
  if (!e) return n;
  
  char *src = e + 1;
  char *dst = src;
  
  if (*src == '+' || *src == '-') {
    dst++;
    src++;
  }
  
  while (*src == '0' && src[1] != '\0') src++;
  
  if (src != dst) {
    memmove(dst, src, strlen(src) + 1);
    return strlen(buf);
  }
  return n;
}

static size_t strnum(ant_value_t value, char *buf, size_t len) {
  double dv = tod(value);
  
  if (isnan(dv)) return cpy(buf, len, "NaN", 3);
  if (isinf(dv)) return cpy(buf, len, dv > 0 ? "Infinity" : "-Infinity", dv > 0 ? 8 : 9);
  if (dv == 0.0) return cpy(buf, len, "0", 1);
  
  char temp[64];
  int sign = dv < 0 ? 1 : 0;
  double adv = sign ? -dv : dv;
  
  double iv;
  double frac = modf(adv, &iv);
  if (frac == 0.0 && adv < 9007199254740992.0) {
    int result = snprintf(temp, sizeof(temp), "%.0f", dv);
    fix_exponent(temp, (size_t)result);
    return cpy(buf, len, temp, strlen(temp));
  }
  
  for (int prec = 1; prec <= 17; prec++) {
    int n = snprintf(temp, sizeof(temp), "%.*g", prec, dv);
    double parsed = strtod(temp, NULL);
    if (parsed == dv) {
      fix_exponent(temp, (size_t)n);
      return cpy(buf, len, temp, strlen(temp));
    }
    (void)n;
  }
  
  int result = snprintf(temp, sizeof(temp), "%.17g", dv);
  fix_exponent(temp, (size_t)result);
  return cpy(buf, len, temp, strlen(temp));
}

static inline ant_offset_t assert_flat_string_len(ant_t *js, ant_value_t value, const char **out_ptr) {
  (void)js;
  ant_flat_string_t *flat = str_flat_ptr(value);
  assert(flat != NULL);
  ant_offset_t len = flat->len;
  if (out_ptr) *out_ptr = flat->bytes;
  return len;
}

static inline ant_rope_heap_t *assert_rope_ptr(ant_value_t value) {
  assert(vtype(value) == T_STR);
  assert(str_is_heap_rope(value));
  ant_rope_heap_t *ptr = str_rope_ptr(value);
  assert(ptr != NULL);
  return ptr;
}

static inline ant_offset_t rope_len(ant_value_t value) {
  ant_rope_heap_t *ptr = assert_rope_ptr(value);
  return ptr->len;
}

static inline uint8_t rope_depth(ant_value_t value) {
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

static inline void rope_set_cached_flat(ant_value_t rope, ant_value_t flat) {
  ant_rope_heap_t *ptr = assert_rope_ptr(rope);
  ptr->cached = flat;
}

static void rope_flatten_into(ant_t *js, ant_value_t str, char *dest, ant_offset_t *pos) {
  assert(vtype(str) == T_STR);
  
  if (!str_is_heap_rope(str)) {
    const char *sptr;
    ant_offset_t slen = assert_flat_string_len(js, str, &sptr);
    memcpy(dest + *pos, sptr, slen);
    *pos += slen; return;
  }
  
  ant_value_t cached = rope_cached_flat(str);
  if (vtype(cached) == T_STR && !str_is_heap_rope(cached)) {
    const char *cptr;
    ant_offset_t clen = assert_flat_string_len(js, cached, &cptr);
    memcpy(dest + *pos, cptr, clen);
    *pos += clen; return;
  }
  
  ant_value_t stack[ROPE_MAX_DEPTH + 8];
  int sp = 0; stack[sp++] = str;
  
  while (sp > 0) {
    ant_value_t node = stack[--sp];
    assert(vtype(node) == T_STR);
    
    if (!str_is_heap_rope(node)) {
      const char *sptr;
      ant_offset_t slen = assert_flat_string_len(js, node, &sptr);
      memcpy(dest + *pos, sptr, slen);
      *pos += slen; continue;
    }
    
    ant_value_t c = rope_cached_flat(node);
    if (vtype(c) == T_STR && !str_is_heap_rope(c)) {
      const char *cptr;
      ant_offset_t clen = assert_flat_string_len(js, c, &cptr);
      memcpy(dest + *pos, cptr, clen);
      *pos += clen; continue;
    }
    
    if (sp + 2 <= ROPE_MAX_DEPTH + 8) {
      stack[sp++] = rope_right(node);
      stack[sp++] = rope_left(node);
    }
  }
}

ant_value_t rope_flatten(ant_t *js, ant_value_t rope) {
  assert(vtype(rope) == T_STR);
  if (!str_is_heap_rope(rope)) return rope;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, rope);
  
  ant_value_t cached = rope_cached_flat(rope);
  GC_ROOT_PIN(js, cached);
  if (vtype(cached) == T_STR && !str_is_heap_rope(cached)) {
    GC_ROOT_RESTORE(js, root_mark);
    return cached;
  }
  
  ant_offset_t total_len = rope_len(rope);
  char *buf = (char *)ant_calloc(total_len + 1);
  
  if (!buf) {
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkerr(js, "oom");
  }
  
  ant_offset_t pos = 0;
  rope_flatten_into(js, rope, buf, &pos);
  buf[pos] = '\0';
  
  ant_value_t flat = js_mkstr(js, buf, pos);
  GC_ROOT_PIN(js, flat);
  free(buf);
  
  if (!is_err(flat)) {
    rope_set_cached_flat(rope, flat);
  }
  
  GC_ROOT_RESTORE(js, root_mark);
  return flat;
}

ant_offset_t vstr(ant_t *js, ant_value_t value, ant_offset_t *len) {
  if (str_is_heap_rope(value)) {
    ant_value_t flat = rope_flatten(js, value);
    assert(!is_err(flat));
    value = flat;
  }
  
  const char *ptr = NULL;
  ant_offset_t slen = assert_flat_string_len(js, value, &ptr);
  if (len) *len = slen;
  return (ant_offset_t)(uintptr_t)ptr;
}

static size_t strstring(ant_t *js, ant_value_t value, char *buf, size_t len) {
  ant_offset_t slen, off = vstr(js, value, &slen);
  const char *str = (const char *)(uintptr_t)off;
  size_t n = 0;
  n += cpy(buf + n, REMAIN(n, len), "'", 1);
  for (ant_offset_t i = 0; i < slen && n < len - 1; i++) {
    char c = str[i];
    if (c == '\n') { n += cpy(buf + n, REMAIN(n, len), "\\n", 2); }
    else if (c == '\r') { n += cpy(buf + n, REMAIN(n, len), "\\r", 2); }
    else if (c == '\t') { n += cpy(buf + n, REMAIN(n, len), "\\t", 2); }
    else if (c == '\\') { n += cpy(buf + n, REMAIN(n, len), "\\\\", 2); }
    else if (c == '\'') { n += cpy(buf + n, REMAIN(n, len), "\\'", 2); }
    else { if (n < len) buf[n++] = c; }
  }
  n += cpy(buf + n, REMAIN(n, len), "'", 1);
  
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
  interned_string_t *entry = (interned_string_t *)ant_calloc(alloc_size);
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

js_intern_stats_t js_intern_stats(void) {
  return (js_intern_stats_t){ 
    .count = intern_count, 
    .bytes = intern_bytes 
  };
}

bool is_internal_prop(const char *key, ant_offset_t klen) {
  if (klen < 2) return false;
  if (key[0] != '_' || key[1] != '_') return false;
  if (klen == STR_PROTO_LEN && memcmp(key, STR_PROTO, STR_PROTO_LEN) == 0) return false;
  return true;
}

struct func_format {
  const char *prefix;
  size_t prefix_len;
  const char *anon;
  size_t anon_len;
};

static const struct func_format formats[] = {
  [0] = { "[Function: ",      11, "[Function (anonymous)]",      22 },
  [1] = { "[AsyncFunction: ", 16, "[AsyncFunction (anonymous)]", 27 },
};

// todo: make it work with bytecode NAME
static size_t strfunc(ant_t *js, ant_value_t value, char *buf, size_t len) {
  ant_offset_t name_len = 0;
  const char *name = get_func_name(js, value, &name_len);
  
  ant_value_t func_obj = js_func_obj(value);
  ant_value_t code_slot = get_slot(func_obj, SLOT_CODE);
  ant_value_t builtin_slot = get_slot(func_obj, SLOT_BUILTIN);
  ant_value_t async_slot = get_slot(func_obj, SLOT_ASYNC);
  
  bool is_async = (async_slot == js_true);
  bool has_code = (vtype(code_slot) == T_CFUNC);
  
  const struct func_format *fmt = &formats[is_async];
  
  if (vtype(builtin_slot) == T_NUM) {
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
    bool is_native = (vtype(cfunc_slot) == T_CFUNC);
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
    if (pt != T_OBJ && pt != T_FUNC) return n;
    
    ant_value_t ctor = lkp_val(js, proto, "constructor", 11);
    uint8_t ct = vtype(ctor);
    if (ct != T_FUNC && ct != T_CFUNC) return n;
    
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

static size_t tostr(ant_t *js, ant_value_t value, char *buf, size_t len) {
  switch (vtype(value)) {
    case T_UNDEF:   return ANT_COPY(buf, len, "undefined");
    case T_NULL:    return ANT_COPY(buf, len, "null");
    
    case T_BOOL: {
      bool b = vdata(value) & 1;
      return b ? ANT_COPY(buf, len, "true") : ANT_COPY(buf, len, "false");
    }
    
    case T_ARR:     return strarr(js, value, buf, len);
    case T_OBJ:     return strobj(js, value, buf, len);
    case T_STR:     return strstring(js, value, buf, len);
    case T_NUM:     return strnum(value, buf, len);
    case T_BIGINT:  return strbigint(js, value, buf, len);
    case T_PROMISE: return strpromise(js, value, buf, len);
    case T_FUNC:    return strfunc(js, value, buf, len);
    
    case T_CFUNC:   return ANT_COPY(buf, len, "[native code]");
    case T_FFI:     return ANT_COPY(buf, len, "[native code (ffi)]");
    
    case T_ERR: {
      uint64_t data = vdata(value);
      if (data != 0) {
        ant_value_t obj = mkval(T_OBJ, data);
        ant_value_t stack = js_get(js, obj, "stack");
        if (vtype(stack) == T_STR) {
          ant_offset_t slen;
          ant_offset_t off = vstr(js, stack, &slen);
          return cpy(buf, len, (const char *)(uintptr_t)(off), slen);
        }
      }
      return ANT_COPY(buf, len, "Error");
    }
    
    case T_SYMBOL: {
      const char *desc = js_sym_desc(js, value);
      if (desc) return (size_t) snprintf(buf, len, "Symbol(%s)", desc);
      return ANT_COPY(buf, len, "Symbol()");
    }
    
    default:        return (size_t) snprintf(buf, len, "VTYPE%d", vtype(value));
  }
}

static char *tostr_alloc(ant_t *js, ant_value_t value) {
  size_t cap = 64;
  char *buf = ant_calloc(cap);
  size_t n = tostr(js, value, buf, cap);
  if (n >= cap) {
    free(buf);
    buf = ant_calloc(n + 1);
    tostr(js, value, buf, n + 1);
  }
  return buf;
}

js_cstr_t js_to_cstr(ant_t *js, ant_value_t value, char *stack_buf, size_t stack_size) {
  js_cstr_t out = { .ptr = "", .len = 0, .needs_free = false };

  if (is_err(value)) {
    uint64_t data = vdata(value);
    if (data != 0) {
      ant_value_t obj = mkval(T_OBJ, data);
      ant_value_t stack = js_get(js, obj, "stack");
      if (vtype(stack) == T_STR) {
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

  if (vtype(value) == T_STR) {
    size_t len = 0;
    char *str = js_getstr(js, value, &len);
    out.ptr = str ? str : ""; out.len = len;
    return out;
  }

  multiref_count = 0;
  multiref_next_id = 0;
  stringify_depth = 0;
  scan_refs(js, value);

  size_t capacity = stack_size;
  char *buf = stack_buf;
  out.needs_free = false;

  if (!buf || capacity == 0) {
    capacity = 64;
    buf = ant_calloc(capacity);
    if (!buf) return out;
    out.needs_free = true;
  }

  for (;;) {
    stringify_depth = 0;
    stringify_indent = 0;
    size_t len = tostr(js, value, buf, capacity);

    if (len < capacity - 1) {
      out.ptr = buf;
      out.len = len;
      return out;
    }

    size_t new_capacity = capacity * 2;
    char *next = out.needs_free 
      ? ant_realloc(buf, new_capacity) 
      : ant_calloc(new_capacity);
    
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

ant_value_t js_tostring_val(ant_t *js, ant_value_t value) {
  uint8_t t = vtype(value);
  char *buf; size_t len, buflen;
  
  static const void *jump_table[] = {
    [T_OBJ] = &&L_OBJ, [T_STR] = &&L_STR,
    [T_UNDEF] = &&L_UNDEF, [T_NULL] = &&L_NULL, [T_NUM] = &&L_NUM,
    [T_BOOL] = &&L_BOOL, [T_FUNC] = &&L_OBJ, [T_CFUNC] = &&L_DEFAULT, 
    [T_ERR] = &&L_DEFAULT, [T_ARR] = &&L_OBJ,
    [T_PROMISE] = &&L_DEFAULT, [T_TYPEDARRAY] = &&L_DEFAULT,
    [T_BIGINT] = &&L_BIGINT, [T_SYMBOL] = &&L_DEFAULT, 
    [T_GENERATOR] = &&L_DEFAULT, [T_FFI] = &&L_DEFAULT
  };
  
  if (t < sizeof(jump_table) / sizeof(jump_table[0])) goto *jump_table[t];
  goto L_DEFAULT;

  L_STR:   return value;
  L_UNDEF: return js_mkstr(js, "undefined", 9);
  L_NULL:  return js_mkstr(js, "null", 4);
  L_BOOL:  return vdata(value) ? js_mkstr(js, "true", 4) : js_mkstr(js, "false", 5);
  L_OBJ:   return js_call_toString(js, value);
  
  L_NUM: {
    buf = (char *)ant_calloc(32);
    len = strnum(value, buf, 32);
    ant_value_t result = js_mkstr(js, buf, len);
    free(buf); return result;
  }
    
  L_BIGINT: {
    buflen = bigint_digits_len(js, value);
    buf = (char *)ant_calloc(buflen + 2);
    len = strbigint(js, value, buf, buflen + 2);
    ant_value_t result = js_mkstr(js, buf, len);
    free(buf); return result;
  }
    
  L_DEFAULT: {
    buf = tostr_alloc(js, value);
    ant_value_t result = js_mkstr(js, buf, strlen(buf));
    free(buf); return result;
  }
}

const char *js_str(ant_t *js, ant_value_t value) {
  if (is_err(value)) {
    uint64_t data = vdata(value);
    if (data != 0) {
      ant_value_t obj = mkval(T_OBJ, data);
      ant_value_t stack = js_get(js, obj, "stack");
      if (vtype(stack) == T_STR) {
        ant_offset_t slen, off = vstr(js, stack, &slen);
        return (const char *)(uintptr_t)off;
      }
    }
    return "Error";
  }
  
  multiref_count = 0;
  multiref_next_id = 0;
  stringify_depth = 0;
  scan_refs(js, value);
  
  size_t capacity = 4096;
  char *buf = (char *)ant_calloc(capacity);
  if (!buf) return "";
  
  size_t len;
  for (;;) {
    stringify_depth = 0;
    stringify_indent = 0;
    len = tostr(js, value, buf, capacity);
    
    if (len < capacity - 1) break;
    
    capacity *= 2;
    buf = (char *)ant_realloc(buf, capacity);
    if (!buf) return "";
  }
  
  ant_value_t str = js_mkstr(js, buf, len);
  free(buf);
  
  if (is_err(str)) return "";
  ant_offset_t off = vstr(js, str, NULL);
  return (const char *)(uintptr_t)off;
}

static inline ant_offset_t get_dense_buf(ant_value_t arr) {
  ant_object_t *ptr = js_obj_ptr(js_as_obj(arr));
  if (!ptr || !ptr->fast_array || !ptr->u.array.data || ptr->u.array.cap == 0) return 0;
  return (ant_offset_t)(uintptr_t)ptr;
}

static inline ant_object_t *dense_obj(ant_offset_t doff) {
  if (!doff) return NULL;
  ant_object_t *ptr = (ant_object_t *)(uintptr_t)doff;
  if (!ptr || !ptr->fast_array || !ptr->u.array.data || ptr->u.array.cap == 0) return NULL;
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
  gc_write_barrier(js, ptr, val);
}

static ant_offset_t dense_grow(ant_t *js, ant_value_t arr, ant_offset_t needed) {
  ant_object_t *obj = js_obj_ptr(js_as_obj(arr));
  if (!obj) return 0;

  ant_offset_t old_cap = obj->u.array.cap;
  ant_offset_t new_cap = old_cap ? old_cap : MAX_DENSE_INITIAL_CAP;
  
  while (new_cap < needed) new_cap *= 2;
  ant_value_t *next = realloc(obj->u.array.data, sizeof(*next) * (size_t)new_cap);
  if (!next) return 0;

  for (ant_offset_t i = old_cap; i < new_cap; i++) next[i] = T_EMPTY;

  obj->u.array.data = next;
  obj->u.array.cap = (uint32_t)new_cap;
  if (obj->u.array.len > obj->u.array.cap) obj->u.array.len = obj->u.array.cap;
  obj->fast_array = 1;
  return (ant_offset_t)(uintptr_t)obj;
}

// TODO: make get and set dry
static inline ant_value_t arr_get(ant_t *js, ant_value_t arr, ant_offset_t idx) {
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
  
  return lkp_val(js, arr, idxstr, idxlen);
}

static inline void arr_set(ant_t *js, ant_value_t arr, ant_offset_t idx, ant_value_t val) {
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
    
    for (ant_offset_t i = len; i < idx; i++) {
      ant_value_t v = dense_get(doff, i);
      if (!is_empty_slot(v) && vtype(v) == T_UNDEF) dense_set(js, doff, i, T_EMPTY);
    }
    dense_set(js, doff, idx, val);
    array_len_set(js, arr, idx + 1);
    return;
  }
  
  sparse:;
  char idxstr[24];
  size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (uint64_t)idx);
  ant_value_t key = js_mkstr(js, idxstr, idxlen);
  
  js_setprop(js, arr, key, val);
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
  
  return lkp(js, arr, idxstr, idxlen) != 0;
}

static inline void arr_del(ant_t *js, ant_value_t arr, ant_offset_t idx) {
  ant_offset_t semantic_len = get_array_length(js, arr);
  if (idx >= semantic_len) return;
  ant_offset_t doff = get_dense_buf(arr);
  
  if (doff) {
    ant_offset_t len = dense_iterable_length(js, arr);
    if (idx < len) dense_set(js, doff, idx, T_EMPTY);
  }
  
  char idxstr[16];
  size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)idx);
  js_delete_prop(js, arr, idxstr, idxlen);
}

ant_value_t js_mkstr(ant_t *js, const void *ptr, size_t len) {
  ant_flat_string_t *flat = (ant_flat_string_t *)js_type_alloc(
    js, ANT_ALLOC_STRING, sizeof(*flat) + len + 1, _Alignof(ant_flat_string_t)
  );
  if (!flat) return js_mkerr(js, "oom");

  flat->len = (ant_offset_t)len;
  if (ptr && len > 0) memcpy(flat->bytes, ptr, len);
  
  flat->bytes[len] = '\0';
  flat->is_ascii = (ptr || len == 0)
    ? str_detect_ascii_bytes(flat->bytes, len)
    : STR_ASCII_UNKNOWN;

  return mkval(T_STR, (uintptr_t)flat);
}

ant_value_t js_mkstr_permanent(ant_t *js, const void *ptr, size_t len) {
  size_t size = sizeof(ant_flat_string_t) + len + 1;
  size_t align = _Alignof(ant_flat_string_t);
  
  if (js->pool.permanent.block_size == 0)
    js->pool.permanent.block_size = ANT_POOL_STRING_BLOCK_SIZE;
  ant_flat_string_t *flat = (ant_flat_string_t *)pool_alloc_chain(
    &js->pool.permanent.head, NULL, js->pool.permanent.block_size, size, align
  );
  if (!flat) return js_mkerr(js, "oom");

  flat->len = (ant_offset_t)len;
  if (ptr && len > 0) memcpy(flat->bytes, ptr, len);
  
  flat->bytes[len] = '\0';
  flat->is_ascii = (ptr || len == 0)
    ? str_detect_ascii_bytes(flat->bytes, len)
    : STR_ASCII_UNKNOWN;

  return mkval(T_STR, (uintptr_t)flat);
}

static ant_value_t js_mkrope(ant_t *js, ant_value_t left, ant_value_t right, ant_offset_t total_len, uint8_t depth) {
  ant_rope_heap_t *rope = (ant_rope_heap_t *)js_type_alloc(
    js, ANT_ALLOC_ROPE, sizeof(*rope), _Alignof(ant_rope_heap_t)
  );
  if (!rope) return js_mkerr(js, "oom");
  rope->len = total_len;
  rope->depth = depth;
  rope->left = left;
  rope->right = right;
  rope->cached = js_mkundef();
  return mkrope_value(rope);
}


static ant_value_t mkobj_with_inobj_limit(ant_t *js, ant_offset_t parent, uint8_t inobj_limit) {
  (void)parent;
  ant_object_t *obj = obj_alloc(js, T_OBJ, inobj_limit);
  if (!obj) return js_mkerr(js, "oom");
  return mkval(T_OBJ, (uintptr_t)obj);
}

ant_value_t mkobj(ant_t *js, ant_offset_t parent) {
  return mkobj_with_inobj_limit(js, parent, (uint8_t)ANT_INOBJ_MAX_SLOTS);
}

ant_value_t js_mkobj_with_inobj_limit(ant_t *js, uint8_t inobj_limit) {
  return mkobj_with_inobj_limit(js, 0, inobj_limit);
}

ant_value_t mkarr(ant_t *js) {
  ant_object_t *obj = obj_alloc(js, T_ARR, (uint8_t)ANT_INOBJ_MAX_SLOTS);
  if (!obj) return js_mkerr(js, "oom");
  ant_value_t arr = mkval(T_ARR, (uintptr_t)obj);
  ant_value_t array_proto = get_ctor_proto(js, "Array", 5);
  if (vtype(array_proto) == T_OBJ) js_set_proto_init(arr, array_proto);

  obj->u.array.cap = MAX_DENSE_INITIAL_CAP;
  obj->u.array.len = 0;
  obj->u.array.data = malloc(sizeof(*obj->u.array.data) * (size_t)obj->u.array.cap);
  if (obj->u.array.data) {
    for (uint32_t i = 0; i < obj->u.array.cap; i++) obj->u.array.data[i] = T_EMPTY;
    obj->fast_array = 1;
  } else {
    obj->u.array.cap = 0;
    obj->u.array.len = 0;
    obj->fast_array = 0;
  }

  return arr;
}

ant_value_t js_mkarr(ant_t *js) { 
  return mkarr(js); 
}

ant_value_t js_newobj(ant_t *js) {
  ant_value_t obj = mkobj(js, 0);
  ant_value_t proto = get_ctor_proto(js, "Object", 6);
  if (vtype(proto) == T_OBJ) js_set_proto_init(obj, proto);
  return obj;
}

ant_offset_t js_arr_len(ant_t *js, ant_value_t arr) {
  if (!array_obj_ptr(arr)) return 0;
  return get_array_length(js, arr);
}

ant_value_t js_arr_get(ant_t *js, ant_value_t arr, ant_offset_t idx) {
  if (vtype(arr) != T_ARR) return js_mkundef();
  return arr_get(js, arr, idx);
}

static inline bool is_const_prop(ant_t *js, ant_offset_t propoff) {
  ant_prop_ref_t *ref = propref_get(js, propoff);
  if (!ref) return false;
  uint8_t attrs = ant_shape_get_attrs(ref->obj->shape, ref->slot);
  return (attrs & ANT_PROP_ATTR_WRITABLE) == 0;
}

static inline const ant_shape_prop_t *prop_shape_meta(ant_t *js, ant_offset_t propoff) {
  ant_prop_ref_t *ref = propref_get(js, propoff);
  if (!ref || !ref->obj || !ref->obj->shape) return NULL;
  return ant_shape_prop_at(ref->obj->shape, ref->slot);
}

static inline bool is_nonconfig_prop(ant_t *js, ant_offset_t propoff) {
  ant_prop_ref_t *ref = propref_get(js, propoff);
  if (!ref) return false;
  uint8_t attrs = ant_shape_get_attrs(ref->obj->shape, ref->slot);
  return (attrs & ANT_PROP_ATTR_CONFIGURABLE) == 0;
}

static void intern_init(void) {
  if (INTERN_LENGTH) return;
  INTERN_LENGTH = intern_string("length", 6);
  INTERN_BUFFER = intern_string("buffer", 6);
  INTERN_PROTOTYPE = intern_string("prototype", 9);
  INTERN_CONSTRUCTOR = intern_string("constructor", 11);
  INTERN_NAME = intern_string("name", 4);
  INTERN_MESSAGE = intern_string("message", 7);
  INTERN_VALUE = intern_string("value", 5);
  INTERN_GET = intern_string("get", 3);
  INTERN_SET = intern_string("set", 3);
  INTERN_ARGUMENTS = intern_string("arguments", 9);
  INTERN_CALLEE = intern_string("callee", 6);
  INTERN_IDX[0] = intern_string("0", 1);
  INTERN_IDX[1] = intern_string("1", 1);
  INTERN_IDX[2] = intern_string("2", 1);
  INTERN_IDX[3] = intern_string("3", 1);
  INTERN_IDX[4] = intern_string("4", 1);
  INTERN_IDX[5] = intern_string("5", 1);
  INTERN_IDX[6] = intern_string("6", 1);
  INTERN_IDX[7] = intern_string("7", 1);
  INTERN_IDX[8] = intern_string("8", 1);
  INTERN_IDX[9] = intern_string("9", 1);
}

ant_value_t mkprop(ant_t *js, ant_value_t obj, ant_value_t k, ant_value_t v, uint8_t attrs) {
  obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  
  if (!ptr || !ptr->shape) return js_mkerr(js, "invalid object");
  if (!attrs) attrs = ANT_PROP_ATTR_DEFAULT;

  uint32_t slot = 0;
  bool added = false;
  if (vtype(k) == T_SYMBOL) {
    ant_offset_t sym_off = (ant_offset_t)vdata(k);
    int32_t found = ant_shape_lookup_symbol(ptr->shape, sym_off);
    if (found >= 0) {
      slot = (uint32_t)found;
      if (!js_obj_ensure_unique_shape(ptr)) return js_mkerr(js, "oom");
      ant_shape_set_attrs_symbol(ptr->shape, sym_off, attrs);
    } else {
      if (!ant_shape_add_symbol_tr(&ptr->shape, sym_off, attrs, &slot)) {
        return js_mkerr(js, "oom");
      }
      added = true;
    }
  } else {
    ant_offset_t klen = 0;
    ant_offset_t koff = vstr(js, k, &klen);
    const char *p = (const char *)(uintptr_t)(koff);
    const char *interned = intern_string(p, klen);
    if (!interned) return js_mkerr(js, "oom");

    int32_t found = ant_shape_lookup_interned(ptr->shape, interned);
    if (found >= 0) {
      slot = (uint32_t)found;
      if (!js_obj_ensure_unique_shape(ptr)) return js_mkerr(js, "oom");
      ant_shape_set_attrs_interned(ptr->shape, interned, attrs);
    } else {
      if (!ant_shape_add_interned_tr(&ptr->shape, interned, attrs, &slot)) {
        return js_mkerr(js, "oom");
      }
      added = true;
    }
  }

  if (added && !js_obj_ensure_prop_capacity(ptr, ant_shape_count(ptr->shape))) {
    return js_mkerr(js, "oom");
  }

  if (slot >= ptr->prop_count && !js_obj_ensure_prop_capacity(ptr, slot + 1)) {
    return js_mkerr(js, "oom");
  }
  ant_object_prop_set_unchecked(ptr, slot, v);
  gc_write_barrier(js, ptr, v);

  return v;
}

ant_value_t mkprop_interned(ant_t *js, ant_value_t obj, const char *interned_key, ant_value_t v, uint8_t attrs) {
  obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || !ptr->shape || !interned_key) return js_mkerr(js, "invalid object");

  if (!attrs) attrs = ANT_PROP_ATTR_DEFAULT;

  uint32_t slot = 0;
  bool added = false;
  int32_t found = ant_shape_lookup_interned(ptr->shape, interned_key);
  if (found >= 0) {
    slot = (uint32_t)found;
    if (!js_obj_ensure_unique_shape(ptr)) return js_mkerr(js, "oom");
    ant_shape_set_attrs_interned(ptr->shape, interned_key, attrs);
  } else {
    if (!ant_shape_add_interned_tr(&ptr->shape, interned_key, attrs, &slot)) {
      return js_mkerr(js, "oom");
    }
    added = true;
  }

  if (added && !js_obj_ensure_prop_capacity(ptr, ant_shape_count(ptr->shape))) {
    return js_mkerr(js, "oom");
  }
  if (slot >= ptr->prop_count && !js_obj_ensure_prop_capacity(ptr, slot + 1)) {
    return js_mkerr(js, "oom");
  }
  ant_object_prop_set_unchecked(ptr, slot, v);
  gc_write_barrier(js, ptr, v);
  return v;
}

ant_value_t js_mkprop_fast(ant_t *js, ant_value_t obj, const char *key, size_t len, ant_value_t v) {
  const char *interned = intern_string(key, len);
  if (!interned) return js_mkerr(js, "oom");
  return mkprop_interned(js, obj, interned, v, 0);
}

ant_offset_t js_mkprop_fast_off(ant_t *js, ant_value_t obj, const char *key, size_t len, ant_value_t v) {
  const char *interned = intern_string(key, len);
  if (!interned) return 0;
  ant_value_t prop = mkprop_interned(js, obj, interned, v, 0);
  if (is_err(prop)) return 0;
  return lkp_interned(js, obj, interned, len);
}

void js_saveval(ant_t *js, ant_offset_t off, ant_value_t v) {
  bool ok = propref_store(js, off, v);
  assert(ok && "js_saveval expects a valid property handle");
  (void)ok;
}

static void set_slot(ant_value_t obj, internal_slot_t slot, ant_value_t val) {
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || slot < 0 || slot > SLOT_MAX) return;
  if (slot == SLOT_PROTO) {
    ptr->proto = val;
    ant_ic_epoch_bump();
    return;
  }
  if (slot == SLOT_DATA) {
    ptr->u.data.value = val;
    return;
  }
  (void)obj_extra_set(ptr, slot, val);
}

static void set_slot_wb(ant_t *js, ant_value_t obj, internal_slot_t slot, ant_value_t val) {
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || slot < 0 || slot > SLOT_MAX) return;
  if (slot == SLOT_PROTO) {
    ptr->proto = val;
    gc_write_barrier(js, ptr, val);
    ant_ic_epoch_bump();
    return;
  }
  if (slot == SLOT_DATA) {
    ptr->u.data.value = val;
    gc_write_barrier(js, ptr, val);
    return;
  }
  (void)obj_extra_set(ptr, slot, val);
  gc_write_barrier(js, ptr, val);
}

void js_set_slot(ant_value_t obj, internal_slot_t slot, ant_value_t value) {
  set_slot(js_as_obj(obj), slot, value);
}

void js_set_slot_wb(ant_t *js, ant_value_t obj, internal_slot_t slot, ant_value_t value) {
  set_slot_wb(js, js_as_obj(obj), slot, value);
}

static ant_value_t get_slot(ant_value_t obj, internal_slot_t slot) {
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || slot < 0 || slot > SLOT_MAX) return js_mkundef();
  if (slot == SLOT_PROTO) return ptr->proto;
  if (slot == SLOT_DATA) {
    return ptr->u.data.value;
  }
  return obj_extra_get(ptr, slot);
}

static void set_func_code_ptr(ant_t *js, ant_value_t func_obj, const char *code, size_t len) {
  set_slot(func_obj, SLOT_CODE, mkval(T_CFUNC, (size_t)code));
  set_slot(func_obj, SLOT_CODE_LEN, tov((double)len));
}

static void set_func_code(ant_t *js, ant_value_t func_obj, const char *code, size_t len) {
  const char *arena_code = code_arena_alloc(code, len);
  if (!arena_code) return;
  set_func_code_ptr(js, func_obj, arena_code, len);
  
  if (!memmem(code, len, "var", 3)) return;
  
  size_t vars_buf_len;
  char *vars = OXC_get_func_hoisted_vars(code, len, &vars_buf_len);
  
  if (vars) {
    set_slot(func_obj, SLOT_HOISTED_VARS, mkval(T_CFUNC, (size_t)vars));
    set_slot(func_obj, SLOT_HOISTED_VARS_LEN, tov((double)vars_buf_len));
  }
}

static const char *get_func_code(ant_t *js, ant_value_t func_obj, ant_offset_t *len) {
  ant_value_t code_val = get_slot(func_obj, SLOT_CODE);
  ant_value_t len_val = get_slot(func_obj, SLOT_CODE_LEN);
  
  if (vtype(code_val) != T_CFUNC) {
    if (len) *len = 0;
    return NULL;
  }
  
  if (len) *len = (ant_offset_t)tod(len_val);
  return (const char *)vdata(code_val);
}

double js_to_number(ant_t *js, ant_value_t arg) {
  if (vtype(arg) == T_NULL) return 0.0;
  if (vtype(arg) == T_UNDEF) return JS_NAN;
  
  if (vtype(arg) == T_NUM) return tod(arg);
  if (vtype(arg) == T_BOOL) return vdata(arg) ? 1.0 : 0.0;
  if (vtype(arg) == T_BIGINT) return bigint_to_double(js, arg);

  if (vtype(arg) == T_STR) {
    ant_offset_t len, off = vstr(js, arg, &len);
    const char *s = (char *)(uintptr_t)(off), *end;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (!*s) return 0.0;
    double val = strtod(s, (char **)&end);
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') end++;
    return (end == s || *end) ? JS_NAN : val;
  }

  if (vtype(arg) == T_OBJ || vtype(arg) == T_ARR) {
    if (vtype(arg) == T_OBJ) {
      ant_value_t prim = js_call_valueOf(js, arg);
      uint8_t pt = vtype(prim);
      if (pt != T_OBJ && pt != T_ARR && pt != T_FUNC) return js_to_number(js, prim);
    }
    
    ant_value_t str_val = js_tostring_val(js, arg);
    if (is_err(str_val) || vtype(str_val) != T_STR) return JS_NAN;
    return js_to_number(js, str_val);
  }
  
  return JS_NAN;
}

static ant_value_t setup_func_prototype(ant_t *js, ant_value_t func) {
  ant_value_t proto_obj = mkobj(js, 0);
  if (is_err(proto_obj)) return proto_obj;
  
  ant_value_t object_proto = get_ctor_proto(js, "Object", 6);
  if (vtype(object_proto) == T_OBJ) {
    js_set_proto_init(proto_obj, object_proto);
  }
  
  ant_value_t constructor_key = js_mkstr(js, "constructor", 11);
  if (is_err(constructor_key)) return constructor_key;
  
  ant_value_t res = mkprop(js, proto_obj, constructor_key, func, 0);
  if (is_err(res)) return res;
  js_set_descriptor(js, proto_obj, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  ant_value_t prototype_key = js_mkstr(js, "prototype", 9);
  if (is_err(prototype_key)) return prototype_key;
  
  res = js_setprop(js, func, prototype_key, proto_obj);
  if (is_err(res)) return res;
  js_set_descriptor(js, js_as_obj(func), "prototype", 9, JS_DESC_W);

  js_mark_constructor(func, true);
  
  return js_mkundef();
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

static inline bool proto_walk_next(ant_t *js, ant_value_t *cur, uint8_t *t, uint8_t flags) {
  uint8_t ct = *t;

  if (flags & PROTO_WALK_F_OBJECT_ONLY) {
    if (!is_object_type(*cur)) return false;
    ant_value_t next = get_proto(js, *cur);
    uint8_t nt = vtype(next);
    if (nt == T_NULL || nt == T_UNDEF || !is_object_type(next)) return false;
    *cur = next; *t = nt;
    return true;
  }

  if (ct == T_OBJ || ct == T_ARR || ct == T_FUNC || ct == T_PROMISE) {
    ant_value_t as_obj = js_as_obj(*cur);
    ant_value_t proto = get_slot(as_obj, SLOT_PROTO);
    
    uint8_t pt = vtype(proto);
    if (pt == T_OBJ || pt == T_ARR || pt == T_FUNC) {
      *cur = proto;
      *t = pt;
      return true;
    }
    
    if (JS_TYPE_FLAG(ct) & T_NEEDS_PROTO_FALLBACK) {
      ant_value_t fallback = get_prototype_for_type(js, ct);
      uint8_t ft = vtype(fallback);
      if (ft == T_NULL || ft == T_UNDEF) return false;
      *cur = fallback;
      *t = ft;
      return true;
    }
    
    return false;
  }

  if (ct == T_STR || ct == T_NUM || ct == T_BOOL || ct == T_BIGINT || ct == T_SYMBOL) {
    ant_value_t proto = get_prototype_for_type(js, ct);
    uint8_t pt = vtype(proto);
    if (pt == T_NULL || pt == T_UNDEF) return false;
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
  g->fast_t = T_NULL;
}

static inline bool proto_walk_overflow_guard_hit_cycle(
  ant_t *js,
  proto_walk_overflow_guard_t *g,
  ant_value_t cur,
  uint8_t cur_t,
  uint8_t flags
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

ant_value_t js_instance_proto_from_new_target(ant_t *js, ant_value_t fallback_proto) {
  ant_value_t instance_proto = js_mkundef();

  if (vtype(js->new_target) == T_FUNC || vtype(js->new_target) == T_CFUNC) {
    ant_value_t nt_obj = js_as_obj(js->new_target);
    ant_value_t nt_proto = lkp_interned_val(js, nt_obj, INTERN_PROTOTYPE);
    if (is_object_type(nt_proto)) instance_proto = nt_proto;
  }

  if (!is_object_type(instance_proto) && is_object_type(fallback_proto)) 
    instance_proto = fallback_proto;
  
  return instance_proto;
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

static inline bool is_wrapper_ctor_target(ant_t *js, ant_value_t this_val, ant_value_t expected_proto) {
  if (vtype(js->new_target) == T_UNDEF) return false;
  if (vtype(this_val) != T_OBJ) return false;
  if (vtype(get_slot(this_val, SLOT_PRIMITIVE)) != T_UNDEF) return false;
  return proto_chain_contains(js, this_val, expected_proto);
}

ant_value_t get_ctor_species_value(ant_t *js, ant_value_t ctor) {
  if (!is_object_type(ctor) && vtype(ctor) != T_CFUNC) return js_mkundef();
  return js_get_sym(js, ctor, get_species_sym());
}

bool same_ctor_identity(ant_t *js, ant_value_t a, ant_value_t b) {
  if (vtype(a) == vtype(b) && vdata(a) == vdata(b)) return true;
  
  if (vtype(a) == T_FUNC && vtype(b) == T_CFUNC) {
    ant_value_t c = get_slot(a, SLOT_CFUNC);
    return vtype(c) == T_CFUNC && vdata(c) == vdata(b);
  }
  
  if (vtype(a) == T_CFUNC && vtype(b) == T_FUNC) {
    ant_value_t c = get_slot(b, SLOT_CFUNC);
    return vtype(c) == T_CFUNC && vdata(c) == vdata(a);
  }
  
  if (vtype(a) == T_FUNC && vtype(b) == T_FUNC) {
    ant_value_t ca = get_slot(a, SLOT_CFUNC);
    ant_value_t cb = get_slot(b, SLOT_CFUNC);
    if (vtype(ca) == T_CFUNC && vtype(cb) == T_CFUNC && vdata(ca) == vdata(cb)) return true;
  }
  
  return false;
}

static ant_value_t array_constructor_from_receiver(ant_t *js, ant_value_t receiver) {
  if (!is_object_type(receiver)) return js_mkundef();
  
  ant_value_t species_source = receiver;
  if (is_proxy(species_source)) {
    species_source = proxy_read_target(js, species_source);
  }
  
  bool receiver_is_array = (vtype(species_source) == T_ARR);
  if (!receiver_is_array) {
    ant_value_t array_proto = get_ctor_proto(js, "Array", 5);
    if (is_object_type(array_proto) && is_object_type(species_source)) {
      receiver_is_array = proto_chain_contains(js, species_source, array_proto);
    }
  }
  if (!receiver_is_array) return js_mkundef();

  ant_value_t ctor = js_getprop_fallback(js, receiver, "constructor");
  if (is_err(ctor)) return ctor;

  ant_value_t species = get_ctor_species_value(js, ctor);
  if (is_err(species)) return species;
  
  if (vtype(species) == T_NULL) return js_mkundef();
  if (vtype(species) == T_FUNC || vtype(species) == T_CFUNC) return species;
  if (vtype(ctor) != T_FUNC && vtype(ctor) != T_CFUNC) return js_mkundef();
  
  return ctor;
}

static ant_value_t array_alloc_from_ctor_with_length(ant_t *js, ant_value_t ctor, ant_offset_t length_hint) {
  if (vtype(ctor) != T_FUNC && vtype(ctor) != T_CFUNC) {
    return mkarr(js);
  }

  ant_value_t seed = js_mkobj(js);
  if (is_err(seed)) return seed;

  ant_value_t proto = js_get(js, ctor, "prototype");
  if (is_err(proto)) return proto;
  if (is_object_type(proto)) js_set_proto_init(seed, proto);

  ant_value_t ctor_args[1] = { tov((double)length_hint) };
  ant_value_t saved_new_target = js->new_target;
  js->new_target = ctor;
  ant_value_t constructed = sv_vm_call(js->vm, js, ctor, seed, ctor_args, 1, NULL, true);
  js->new_target = saved_new_target;
  if (is_err(constructed)) return constructed;

  ant_value_t result = is_object_type(constructed) ? constructed : seed;
  set_slot(js_as_obj(result), SLOT_CTOR, ctor);
  return result;
}

static inline ant_value_t array_alloc_from_ctor(ant_t *js, ant_value_t ctor) {
  return array_alloc_from_ctor_with_length(js, ctor, 0);
}

static inline ant_value_t array_alloc_like(ant_t *js, ant_value_t receiver) {
  ant_value_t ctor = array_constructor_from_receiver(js, receiver);
  if (is_err(ctor)) return ctor;
  return array_alloc_from_ctor(js, ctor);
}

static ant_value_t validate_array_length(ant_t *js, ant_value_t v) {
  if (vtype(v) != T_NUM) {
    return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid array length");
  }
  double d = tod(v);
  if (d < 0 || d != (uint32_t)d || d >= 4294967296.0) {
    return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid array length");
  }
  return js_mkundef();
}

static inline ant_value_t check_object_extensibility(ant_t *js, ant_value_t obj) {
  obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return js_mkundef();

  if (ptr->frozen) {
    return sv_is_strict_context(js)
      ? js_mkerr(js, "cannot add property to frozen object")
      : js_false;
  }

  if (ptr->sealed) {
    return sv_is_strict_context(js)
      ? js_mkerr(js, "cannot add property to sealed object")
      : js_false;
  }

  if (!ptr->extensible) {
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
    return;
  }

  ant_value_t new_len_val = tov((double)new_len);
  ant_offset_t len_off = lkp_interned(js, obj, INTERN_LENGTH, 6);
  if (len_off != 0) {
    js_saveval(js, len_off, new_len_val);
  } else {
    js_mkprop_fast(js, obj, "length", 6, new_len_val);
  }
}

static ant_value_t js_setprop_array_fast(ant_t *js, ant_value_t obj, ant_value_t k, ant_value_t v, ant_offset_t klen, const char *key) {
  unsigned long idx;
  if (!parse_array_index(key, klen, (ant_offset_t)-1, &idx)) return js_mkundef();
  
  ant_offset_t cur_len = get_array_length(js, obj);
  ant_offset_t doff = get_dense_buf(obj);
  if (doff) {
    ant_offset_t dense_len = dense_iterable_length(js, obj);
    if (idx < dense_len) { dense_set(js, doff, (ant_offset_t)idx, v); return v; }

    ant_offset_t density_limit = dense_len > 0 ? dense_len * 4 : 64;
    if (idx >= density_limit) goto sparse;
    
    ant_value_t extensibility_error = check_object_extensibility(js, obj);
    if (is_err(extensibility_error)) return extensibility_error;
    if (extensibility_error == js_false) return v;
    
    arr_set(js, obj, (ant_offset_t)idx, v);
    return v;
  }
  
  sparse:;
  if (idx < cur_len) return js_mkundef();
  
  ant_value_t extensibility_error = check_object_extensibility(js, obj);
  if (is_err(extensibility_error)) return extensibility_error;
  if (extensibility_error == js_false) return v;
  
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
  if (!out || !is_object_type(cur_obj)) return false;
  if (key_kind == PROP_META_STRING && !key) return false;

  prop_meta_defaults(out);

  ant_object_t *cur_ptr = js_obj_ptr(cur_obj);
  if (!cur_ptr) return false;
  if (key_kind == PROP_META_STRING && key && is_length_key(key, klen) &&
      cur_ptr->type_tag == T_ARR) {
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
      const char *interned_key = intern_string(key, klen);
      if (interned_key) slot = ant_shape_lookup_interned(cur_ptr->shape, interned_key);
    }

    if (slot >= 0) {
      const ant_shape_prop_t *prop = ant_shape_prop_at(cur_ptr->shape, (uint32_t)slot);
      if (!prop) return false;
      prop_meta_from_shape(out, prop);
      return true;
    }
  }

  if (!cur_ptr->is_exotic) return false;

  descriptor_entry_t *desc = NULL;
  if (key_kind == PROP_META_SYMBOL) {
    desc = lookup_sym_descriptor(cur_obj, sym_off);
  } else if (js) {
    desc = lookup_descriptor(cur_obj, key, klen);
  }

  if (!desc) return false;
  prop_meta_from_desc(out, desc);
  return true;
}

bool js_try_get_own_data_prop(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t *out) {
  if (out) *out = js_mkundef();
  if (!key || !out) return false;

  uint8_t t = vtype(obj);
  if (t == T_FUNC) obj = js_func_obj(obj);
  else if (t != T_OBJ && t != T_ARR) return false;

  ant_value_t as_obj = js_as_obj(obj);
  if (is_proxy(as_obj)) return false;

  prop_meta_t meta;
  bool has_meta = lookup_string_prop_meta(js, as_obj, key, key_len, &meta);
  if (has_meta && (meta.has_getter || meta.has_setter)) return false;

  ant_offset_t off = lkp(js, as_obj, key, (ant_offset_t)key_len);
  if (off != 0) {
    *out = propref_load(js, off);
    return true;
  }

  if (array_obj_ptr(as_obj) && is_length_key(key, (ant_offset_t)key_len)) {
    *out = tov((double)get_array_length(js, as_obj));
    return true;
  }

  return false;
}

// TODO: decompose into smaller helpers
ant_value_t js_setprop(ant_t *js, ant_value_t obj, ant_value_t k, ant_value_t v) {
  uint8_t ot = vtype(obj);

  if (ot == T_STR || ot == T_NUM || ot == T_BOOL || ot == T_CFUNC) {
    ant_offset_t klen; ant_offset_t koff = vstr(js, k, &klen);
    const char *key = (char *)(uintptr_t)(koff);
    
    if (ot != T_CFUNC) {
      ant_value_t proto = get_prototype_for_type(js, ot);
      if (is_object_type(proto)) {
        ant_value_t setter = js_mkundef();
        bool has_setter = false;
        lkp_with_setter(js, proto, key, klen, &setter, &has_setter);
        if (has_setter && (vtype(setter) == T_FUNC || vtype(setter) == T_CFUNC)) {
          call_proto_accessor(js, obj, setter, true, &v, 1, true);
          return v;
        }
      }
    }
    
    if (sv_is_strict_context(js))
      return js_mkerr_typed(js, JS_ERR_TYPE,
        "Cannot create property '%.*s' on %s",
        (int)klen, key, typestr(ot));
    return v;
  }

  if (vtype(obj) == T_FUNC) obj = js_func_obj(obj);
  if (vtype(k) == T_SYMBOL) {
    ant_offset_t sym_off = (ant_offset_t)vdata(k);
    ant_value_t cur = obj;
    proto_overflow_guard_t guard;
    proto_overflow_guard_init(&guard);

    while (is_object_type(cur)) {
      ant_value_t cur_obj = js_as_obj(cur);
      prop_meta_t meta;
      if (lookup_symbol_prop_meta(cur_obj, sym_off, &meta)) {
        if (meta.has_setter) {
          ant_value_t setter = meta.setter;
          if (vtype(setter) == T_FUNC || vtype(setter) == T_CFUNC) {
            ant_value_t result = sv_vm_call(sv_vm_get_active(js), js, setter, obj, &v, 1, NULL, false);
            if (is_err(result)) return result;
            return v;
          }
        }
        if (meta.has_getter && !meta.has_setter) {
          if (sv_is_strict_context(js)) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot set property which has only a getter");
          return v;
        }
        if (!meta.has_getter && !meta.has_setter && !meta.writable) {
          if (sv_is_strict_context(js)) return js_mkerr(js, "assignment to read-only property");
          return v;
        }
        break;
      }
      
      ant_value_t proto = get_proto(js, cur_obj);
      if (!is_object_type(proto)) break;
      cur = proto;
      if (proto_overflow_guard_hit_cycle(js, &guard, cur)) break;
    }

    ant_offset_t existing = lkp_sym(js, obj, sym_off);
    
    if (existing > 0) {
      if (is_const_prop(js, existing)) return js_mkerr(js, "assignment to constant");
      js_saveval(js, existing, v);
      return v;
    }
    
    {
      ant_value_t extensibility_error = check_object_extensibility(js, obj);
      if (is_err(extensibility_error)) return extensibility_error;
      if (extensibility_error == js_false) return v;
    }
    
    return mkprop(js, obj, k, v, 0);
  }

  ant_offset_t klen; ant_offset_t koff = vstr(js, k, &klen);
  const char *key = (char *)(uintptr_t)(koff);
  
  if (array_obj_ptr(obj) && !is_proxy(obj) && klen > 0 && key[0] >= '0' && key[0] <= '9') {
    ant_value_t result = js_setprop_array_fast(js, obj, k, v, klen, key);
    if (vtype(result) != T_UNDEF) return result;
  }

  if (array_obj_ptr(obj) && is_length_key(key, klen)) {
    ant_value_t err = validate_array_length(js, v);
    if (is_err(err)) return err;
    ant_offset_t doff = get_dense_buf(obj);
    ant_offset_t new_len_val = (ant_offset_t) tod(v);
    if (doff) {
      ant_offset_t cap = dense_capacity(doff);
      ant_offset_t cur_len = get_array_length(js, obj);
      ant_offset_t clear_to = (cur_len < cap) ? cur_len : cap;
      if (new_len_val < clear_to) {
        for (ant_offset_t i = new_len_val; i < clear_to; i++)
          dense_set(js, doff, i, T_EMPTY);
      }
    }
    array_len_set(js, obj, new_len_val);
    return v;
  }
  
  if (is_proxy(obj)) {
    ant_value_t result = proxy_set(js, obj, key, klen, v);
    if (is_err(result)) return result;
    return v;
  }
  
  if (try_dynamic_setter(js, obj, key, klen, v)) return v;
  ant_offset_t existing = lkp(js, obj, key, klen);
  
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

      if (!found_here && cur_ptr->is_exotic) {
        descriptor_entry_t *desc = lookup_descriptor(cur_obj, key, klen);
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
      if (vtype(proto) != T_OBJ && vtype(proto) != T_FUNC) break;
      cur = proto;
      on_receiver = false;
      if (proto_overflow_guard_hit_cycle(js, &guard, cur)) break;
    }

    if (!found_desc) goto no_descriptor;
    if (!desc_on_receiver && !desc_has_setter && !desc_has_getter && desc_writable) goto no_descriptor;
    
    if (desc_has_setter) {
      ant_value_t setter = desc_setter;
      uint8_t setter_type = vtype(setter);
      if (setter_type == T_FUNC || setter_type == T_CFUNC) {
        js_error_site_t saved_errsite = js->errsite;
        ant_value_t result = sv_vm_call(sv_vm_get_active(js), js, setter, obj, &v, 1, NULL, false);
        js->errsite = saved_errsite;
        if (is_err(result)) return result;
        return v;
      }
    }
    
    if (desc_has_getter && !desc_has_setter) {
      if (sv_is_strict_context(js)) return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot set property which has only a getter");
      return v;
    }
    
    if (!desc_writable) {
      if (sv_is_strict_context(js)) return js_mkerr(js, "assignment to read-only property");
      return v;
    }
    
    if (existing <= 0) goto no_descriptor;
  }
  
no_descriptor:
  if (existing <= 0) goto create_new;
  if (is_const_prop(js, existing)) return js_mkerr(js, "assignment to constant");

  js_saveval(js, existing, v);
  array_define_or_set_index(js, obj, key, (size_t)klen);
  return v;

create_new:
  {
    ant_value_t extensibility_error = check_object_extensibility(js, obj);
    if (is_err(extensibility_error)) return extensibility_error;
    if (extensibility_error == js_false) return v;
  }
  
  ant_value_t result = mkprop(js, obj, k, v, 0);
  if (is_err(result)) return result;
  array_define_or_set_index(js, obj, key, (size_t)klen);
  
  return v;
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

  if (try_dynamic_setter(js, obj, key, klen, v)) return v;
  ant_offset_t existing = lkp(js, obj, key, klen);

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

    if (!has_desc && ptr && ptr->is_exotic) {
      descriptor_entry_t *desc = lookup_descriptor(as_obj, key, klen);
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
        if (setter_type == T_FUNC || setter_type == T_CFUNC) {
          ant_value_t result = sv_vm_call(sv_vm_get_active(js), js, setter, obj, &v, 1, NULL, false);
          if (is_err(result)) return result;
          return v;
        }
      }
    }
  }

  if (existing > 0) {
    if (is_const_prop(js, existing)) return js_mkerr(js, "assignment to constant");
    js_saveval(js, existing, v);
    array_define_or_set_index(js, obj, key, klen);
    return v;
  }

  {
    ant_value_t extensibility_error = check_object_extensibility(js, obj);
    if (is_err(extensibility_error)) return extensibility_error;
    if (extensibility_error == js_false) return v;
  }

  ant_value_t k = js_mkstr(js, key, klen);
  if (is_err(k)) return k;
  ant_value_t created = mkprop(js, obj, k, v, 0);
  if (!is_err(created)) array_define_or_set_index(js, obj, key, klen);
  return is_err(created) ? created : v;
}

ant_value_t setprop_interned(ant_t *js, ant_value_t obj, const char *key, size_t len, ant_value_t v) {
  ant_value_t k = js_mkstr(js, key, len);
  if (is_err(k)) return k;
  return js_setprop(js, obj, k, v);
}

ant_value_t js_setprop_nonconfigurable(ant_t *js, ant_value_t obj, const char *key, size_t keylen, ant_value_t v) {
  ant_value_t k = js_mkstr(js, key, keylen);
  if (is_err(k)) return k;
  ant_value_t result = js_setprop(js, obj, k, v);
  if (is_err(result)) return result;
  
  js_set_descriptor(js, js_as_obj(obj), key, keylen, JS_DESC_W);
  return result;
}

#define SYM_FLAG_GLOBAL      1u
#define SYM_FLAG_WELL_KNOWN  2u

typedef struct sym_registry_entry {
  const char *key;
  ant_value_t sym;
  UT_hash_handle hh;
} sym_registry_entry_t;

ant_value_t js_mksym(ant_t *js, const char *desc) {
  uint32_t id = (uint32_t)(++js->sym.counter);
  size_t desc_len = (desc && *desc) ? strlen(desc) : 0;
  size_t total = sizeof(ant_symbol_heap_t) + (desc_len ? desc_len + 1 : 0);
  ant_symbol_heap_t *sym_ptr = (ant_symbol_heap_t *)js_type_alloc(
    js, ANT_ALLOC_SYMBOL, total, _Alignof(ant_symbol_heap_t)
  );
  if (!sym_ptr) return js_mkerr(js, "oom");

  sym_ptr->id = id;
  sym_ptr->flags = 0;
  sym_ptr->key = NULL;
  sym_ptr->desc_len = (uint32_t)desc_len;
  if (desc_len) {
    memcpy(sym_ptr->desc, desc, desc_len);
    sym_ptr->desc[desc_len] = '\0';
  }

  return mkval(T_SYMBOL, (uintptr_t)sym_ptr);
}

ant_value_t js_mksym_well_known(ant_t *js, const char *desc) {
  ant_value_t sym = js_mksym(js, desc);
  if (is_err(sym)) return sym;
  ant_symbol_heap_t *ptr = (ant_symbol_heap_t *)(uintptr_t)vdata(sym);
  if (ptr) ptr->flags |= SYM_FLAG_WELL_KNOWN;
  return sym;
}

static inline ant_symbol_heap_t *sym_ptr(ant_value_t v) {
  return (ant_symbol_heap_t *)(uintptr_t)vdata(v);
}

static inline uint32_t sym_get_id(ant_t *js, ant_value_t v) {
  (void)js;
  ant_symbol_heap_t *ptr = sym_ptr(v);
  return ptr ? ptr->id : 0;
}

static inline uint32_t sym_get_flags(ant_t *js, ant_value_t v) {
  (void)js;
  ant_symbol_heap_t *ptr = sym_ptr(v);
  return ptr ? ptr->flags : 0;
}

static inline uintptr_t sym_get_key_ptr(ant_t *js, ant_value_t v) {
  (void)js;
  ant_symbol_heap_t *ptr = sym_ptr(v);
  return ptr ? (uintptr_t)ptr->key : 0;
}

static const char *sym_get_desc(ant_t *js, ant_value_t v) {
  (void)js;
  ant_symbol_heap_t *ptr = sym_ptr(v);
  if (!ptr || ptr->desc_len == 0) return NULL;
  return ptr->desc;
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

  sym_registry_entry_t *entry = ant_calloc(sizeof(sym_registry_entry_t));
  if (entry) {
    entry->key = interned;
    entry->sym = sym;
    HASH_ADD_PTR(reg, key, entry);
    js->sym.registry = reg;
  }

  return sym;
}

const char *js_sym_key(ant_value_t sym) {
  if (vtype(sym) != T_SYMBOL) return NULL;
  ant_t *js = rt->js;
  uint32_t flags = sym_get_flags(js, sym);
  if (!(flags & SYM_FLAG_GLOBAL) || (flags & SYM_FLAG_WELL_KNOWN)) return NULL;
  return (const char *)sym_get_key_ptr(js, sym);
}

const inline char *js_sym_desc(ant_t *js, ant_value_t sym) {
  return sym_get_desc(js, sym);
}

static inline bool streq(const char *buf, size_t len, const char *s, size_t n) {
  return len == n && !memcmp(buf, s, n);
}

ant_offset_t lkp_interned(ant_t *js, ant_value_t obj, const char *search_intern, size_t len) {
  obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!search_intern || !ptr || !ptr->shape) return 0;

  int32_t shape_slot = ant_shape_lookup_interned(ptr->shape, search_intern);
  (void)len;
  if (shape_slot < 0) return 0;
  return propref_make(js, ptr, (uint32_t)shape_slot);
}

inline ant_offset_t lkp(ant_t *js, ant_value_t obj, const char *buf, size_t len) {
  const char *search_intern = intern_string(buf, len);
  if (!search_intern) return 0;
  return lkp_interned(js, obj, search_intern, len);
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
  const char *interned = intern_string(buf, len);
  if (!interned) return js_mkundef();
  return lkp_interned_val(js, obj, interned);
}

ant_offset_t lkp_sym(ant_t *js, ant_value_t obj, ant_offset_t sym_off) {
  obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || !ptr->shape) return 0;
  int32_t slot = ant_shape_lookup_symbol(ptr->shape, sym_off);
  if (slot < 0) return 0;
  return propref_make(js, ptr, (uint32_t)slot);
}

ant_offset_t lkp_sym_proto(ant_t *js, ant_value_t obj, ant_offset_t sym_off) {
  ant_value_t cur = obj;
  proto_overflow_guard_t guard;
  proto_overflow_guard_init(&guard);
  while (is_object_type(cur)) {
    obj = cur;
    ant_offset_t off = lkp_sym(js, obj, sym_off);
    if (off != 0) return off;
    ant_value_t proto = get_proto(js, js_as_obj(cur));
    if (!is_object_type(proto)) break;
    cur = proto;
    if (proto_overflow_guard_hit_cycle(js, &guard, cur)) break;
  }
  return 0;
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
  const char *search_intern = intern_string(buf, len);
  
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
          return current_id;
        }
        return propref_make(js, ptr, (uint32_t)slot);
      }
    }

    if (ptr && ptr->is_exotic) {
      descriptor_entry_t *desc = lookup_descriptor(current, buf, len);
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
  const char *search_intern = intern_string(buf, len);
  
  ant_value_t current = obj;
  proto_overflow_guard_t guard;
  proto_overflow_guard_init(&guard);
  while (vtype(current) == T_OBJ || vtype(current) == T_FUNC) {
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
          return current_id;
        }
        return propref_make(js, ptr, (uint32_t)slot);
      }
    }

    if (ptr && ptr->is_exotic) {
      descriptor_entry_t *desc = lookup_descriptor(current, buf, len);
      if (desc && desc->has_setter) {
        *setter_out = desc->setter;
        *has_setter_out = true;
        return current_id;
      }
    }
    
    ant_value_t proto = get_proto(js, current);
    if (vtype(proto) != T_OBJ && vtype(proto) != T_FUNC) break;
    current = proto;
    if (proto_overflow_guard_hit_cycle(js, &guard, current)) break;
  }
  
  return 0;
}

static ant_value_t call_proto_accessor(
  ant_t *js, ant_value_t prim, ant_value_t accessor, bool has_accessor,
  ant_value_t *arg, int arg_count, bool is_setter
) {
  if (!has_accessor || (vtype(accessor) != T_FUNC && vtype(accessor) != T_CFUNC)) 
    return js_mkundef();
  
  js_error_site_t saved_errsite = js->errsite;
  ant_value_t result = sv_vm_call(sv_vm_get_active(js), js, accessor, prim, arg, arg_count, NULL, false);
  
  bool had_throw = js->thrown_exists;
  ant_value_t thrown = js->thrown_value;
  js->errsite = saved_errsite;
  
  if (had_throw) {
    js->thrown_exists = true;
    js->thrown_value = thrown;
  }
  
  if (is_setter) return is_err(result) ? result : (arg ? *arg : js_mkundef());
  return result;
}

ant_value_t js_get_proto(ant_t *js, ant_value_t obj) {
  uint8_t t = vtype(obj);

  if (!is_object_type(obj)) return js_mknull();
  ant_value_t as_obj = js_as_obj(obj);

  ant_object_t *ptr = js_obj_ptr(as_obj);
  ant_value_t proto = ptr ? ptr->proto : js_mkundef();
  if (is_object_type(proto)) return proto;
  
  if (t != T_OBJ) return get_prototype_for_type(js, t);
  return js_mknull();
}

static ant_value_t get_proto(ant_t *js, ant_value_t obj) {
  return js_get_proto(js, obj);
}

void js_set_proto(ant_value_t obj, ant_value_t proto) {
  if (!is_object_type(obj)) return;

  ant_value_t as_obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(as_obj);
  if (!ptr) return;

  ptr->proto = proto;
  ant_ic_epoch_bump();
}

void js_set_proto_init(ant_value_t obj, ant_value_t proto) {
  if (!is_object_type(obj)) return;
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  if (!ptr) return;
  ptr->proto = proto;
}

static void set_proto(ant_t *js, ant_value_t obj, ant_value_t proto) {
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  js_set_proto(obj, proto);
  if (ptr) gc_write_barrier(js, ptr, proto);
}

void js_set_proto_wb(ant_t *js, ant_value_t obj, ant_value_t proto) {
  set_proto(js, obj, proto);
}

ant_value_t js_get_ctor_proto(ant_t *js, const char *name, size_t len) {
  const char *interned = intern_string(name, len);
  ant_value_t ctor = lkp_interned_val(js, js->global, interned);
  if (vtype(ctor) != T_FUNC) return js_mknull();
  ant_value_t ctor_obj = js_as_obj(ctor);
  ant_value_t proto = lkp_interned_val(js, ctor_obj, INTERN_PROTOTYPE);
  return vtype(proto) == T_UNDEF ? js_mknull() : proto;
}

static inline ant_value_t get_ctor_proto(ant_t *js, const char *name, size_t len) {
  return js_get_ctor_proto(js, name, len);
}

static ant_value_t get_prototype_for_type(ant_t *js, uint8_t type) {
switch (type) {
  case T_STR:     return get_ctor_proto(js, "String", 6);
  case T_NUM:     return get_ctor_proto(js, "Number", 6);
  case T_BOOL:    return get_ctor_proto(js, "Boolean", 7);
  case T_ARR:     return get_ctor_proto(js, "Array", 5);
  case T_FUNC:    return get_ctor_proto(js, "Function", 8);
  case T_PROMISE: return get_ctor_proto(js, "Promise", 7);
  case T_OBJ:     return get_ctor_proto(js, "Object", 6);
  case T_BIGINT:  return get_ctor_proto(js, "BigInt", 6);
  case T_SYMBOL:  return get_ctor_proto(js, "Symbol", 6);
  default:        return js_mknull();
}}

ant_offset_t lkp_proto(ant_t *js, ant_value_t obj, const char *key, size_t len) {
  uint8_t t = vtype(obj);
  const char *key_intern = intern_string(key, len);
  if (!key_intern) return 0;

  ant_value_t cur = obj;
  proto_walk_overflow_guard_t guard;
  proto_walk_overflow_guard_init(&guard);
  
  while (true) {
    if (t == T_OBJ || t == T_ARR || t == T_FUNC || t == T_PROMISE) {
      ant_value_t as_obj = js_as_obj(cur);
      ant_offset_t off = lkp_interned(js, as_obj, key_intern, len);
      if (off != 0) return off;
    } else if (t == T_CFUNC) {
      ant_value_t func_proto = get_ctor_proto(js, "Function", 8);
      uint8_t ft = vtype(func_proto);
      if (ft == T_OBJ || ft == T_ARR || ft == T_FUNC) {
        ant_offset_t off = lkp(js, js_as_obj(func_proto), key, len);
        if (off != 0) return off;
      }
      break;
    } else if (t != T_STR && t != T_NUM && t != T_BOOL && t != T_BIGINT && t != T_SYMBOL) break;

    if (!proto_walk_next(js, &cur, &t, PROTO_WALK_F_LOOKUP)) break;
    if (proto_walk_overflow_guard_hit_cycle(js, &guard, cur, t, PROTO_WALK_F_LOOKUP)) break;
  }
  
  return 0;
}

static ant_value_t js_string_from_utf16_code_unit(ant_t *js, uint32_t code_unit) {
  char buf[4];
  size_t out_len = 0;

  if (code_unit >= 0xD800 && code_unit <= 0xDFFF) {
    buf[0] = (char)(0xE0 | (code_unit >> 12));
    buf[1] = (char)(0x80 | ((code_unit >> 6) & 0x3F));
    buf[2] = (char)(0x80 | (code_unit & 0x3F));
    out_len = 3;
  } else out_len = (size_t)utf8_encode(code_unit, buf);

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
  ant_offset_t str_len = (ant_offset_t)utf16_strlen(str_data, byte_len);
  if (!parse_array_index(key, key_len, str_len, &idx)) return false;

  uint32_t code_unit = utf16_code_unit_at(str_data, byte_len, (ant_offset_t)idx);
  if (code_unit == 0xFFFFFFFF) return false;

  *out = js_string_from_utf16_code_unit(js, code_unit);
  return true;
}

static ant_value_t getprop_any(ant_t *js, ant_value_t obj, const char *key, size_t key_len) {
  uint8_t t = vtype(obj);
  
  if (t == T_STR && is_length_key(key, key_len)) {
    ant_offset_t byte_len;
    ant_offset_t str_off = vstr(js, obj, &byte_len);
    return tov(D(utf16_strlen((const char *)(uintptr_t)(str_off), byte_len)));
  }

  if (t == T_STR) {
    ant_value_t indexed = js_mkundef();
    if (js_try_get_string_index(js, obj, key, key_len, &indexed)) return indexed;
  }
  
  if (t == T_STR || t == T_NUM || t == T_BOOL || t == T_BIGINT) {
    ant_offset_t off = lkp_proto(js, obj, key, key_len);
    if (off != 0) return propref_load(js, off);
    return js_mkundef();
  }
  
  if (t == T_OBJ || t == T_ARR || t == T_FUNC) {
    ant_value_t as_obj = js_as_obj(obj);
    ant_offset_t off = lkp(js, as_obj, key, key_len);
    if (off != 0) return propref_load(js, off);
    off = lkp_proto(js, obj, key, key_len);
    if (off != 0) return propref_load(js, off);
  }
  
  return js_mkundef();
}

static ant_value_t try_dynamic_getter(ant_t *js, ant_value_t obj, const char *key, size_t key_len) {
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  if (!ptr || !ptr->is_exotic) return js_mkundef();
  if (!ptr->exotic_ops || !ptr->exotic_ops->getter) return js_mkundef();
  return ptr->exotic_ops->getter(js, obj, key, key_len);
}

static bool try_dynamic_setter(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t value) {
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  if (!ptr || !ptr->is_exotic) return false;
  if (!ptr->exotic_ops || !ptr->exotic_ops->setter) return false;
  return ptr->exotic_ops->setter(js, obj, key, key_len, value);
}

static bool try_dynamic_deleter(ant_t *js, ant_value_t obj, const char *key, size_t key_len) {
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  if (!ptr || !ptr->is_exotic) return false;
  if (!ptr->exotic_ops || !ptr->exotic_ops->deleter) return false;
  return ptr->exotic_ops->deleter(js, obj, key, key_len);
}

static bool try_accessor_getter(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t *out) {
  ant_value_t getter = js_mkundef();
  bool has_getter = false;
  lkp_with_getter(js, obj, key, key_len, &getter, &has_getter);

  ant_value_t result = call_proto_accessor(js, obj, getter, has_getter, NULL, 0, false);
  if (vtype(result) != T_UNDEF) {
    *out = result;
    return true;
  }
  return false;
}

static bool try_accessor_setter(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t val, ant_value_t *out) {
  ant_value_t setter = js_mkundef();
  bool has_setter = false;
  
  lkp_with_setter(js, obj, key, key_len, &setter, &has_setter);
  if (!has_setter) return false;

  ant_value_t result = call_proto_accessor(js, obj, setter, has_setter, &val, 1, true);
  if (is_err(result)) {
    *out = result;
    return true;
  }
  
  *out = val;
  return true;
}

ant_value_t js_propref_load(ant_t *js, ant_offset_t handle) {
  return propref_load(js, handle);
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

static bool string_builder_append(string_builder_t *sb, const char *data, size_t len) {
  if (sb->size + len > sb->capacity) {
    size_t new_capacity = sb->capacity ? sb->capacity * 2 : 256;
    while (new_capacity < sb->size + len) new_capacity *= 2;
    
    char *new_buffer = (char *)ant_calloc(new_capacity);
    if (!new_buffer) return false;
    
    if (sb->size > 0) memcpy(new_buffer, sb->buffer, sb->size);
    if (sb->is_dynamic) free(sb->buffer);
    
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
  if (sb->is_dynamic && sb->buffer) free(sb->buffer);
  return result;
}

ant_offset_t str_len_fast(ant_t *js, ant_value_t str) {
  if (vtype(str) != T_STR) return 0;
  if (str_is_heap_rope(str)) return rope_len(str);
  return assert_flat_string_len(js, str, NULL);
}

ant_value_t do_string_op(ant_t *js, uint8_t op, ant_value_t l, ant_value_t r) {
  if (op == TOK_PLUS) {
    ant_offset_t n1 = str_len_fast(js, l);
    ant_offset_t n2 = str_len_fast(js, r);
    ant_offset_t total_len = n1 + n2;
    
    if (n2 == 0) return l;
    if (n1 == 0) return r;
    
    uint8_t left_depth = (vtype(l) == T_STR && str_is_heap_rope(l)) ? rope_depth(l) : 0;
    uint8_t right_depth = (vtype(r) == T_STR && str_is_heap_rope(r)) ? rope_depth(r) : 0;
    uint8_t new_depth = (left_depth > right_depth ? left_depth : right_depth) + 1;
    
    if (new_depth >= ROPE_MAX_DEPTH || total_len >= ROPE_FLATTEN_THRESHOLD) {
      ant_value_t flat_l = l, flat_r = r;
      if (str_is_heap_rope(l)) flat_l = rope_flatten(js, l);
      if (is_err(flat_l)) return flat_l;
      if (str_is_heap_rope(r)) flat_r = rope_flatten(js, r);
      if (is_err(flat_r)) return flat_r;
      
      ant_offset_t off1, off2, len1, len2;
      off1 = vstr(js, flat_l, &len1);
      off2 = vstr(js, flat_r, &len2);
      
      string_builder_t sb;
      char static_buffer[512];
      string_builder_init(&sb, static_buffer, sizeof(static_buffer));
      
      if (
        !string_builder_append(&sb, (char *)(uintptr_t)(off1), len1) ||
        !string_builder_append(&sb, (char *)(uintptr_t)(off2), len2)
      ) return js_mkerr(js, "string concatenation failed");
      
      return string_builder_finalize(js, &sb);
    }
    
    return js_mkrope(js, l, r, total_len, new_depth);
  }
  
  ant_offset_t n1, off1 = vstr(js, l, &n1);
  ant_offset_t n2, off2 = vstr(js, r, &n2);
  
  if (op == TOK_EQ) {
    bool eq = n1 == n2 &&
      memcmp((const void *)(uintptr_t)off1, (const void *)(uintptr_t)off2, n1) == 0;
    return mkval(T_BOOL, eq ? 1 : 0);
  } else if (op == TOK_NE) {
    bool eq = n1 == n2 &&
      memcmp((const void *)(uintptr_t)off1, (const void *)(uintptr_t)off2, n1) == 0;
    return mkval(T_BOOL, eq ? 0 : 1);
  } else if (op == TOK_LT || op == TOK_LE || op == TOK_GT || op == TOK_GE) {
    ant_offset_t min_len = n1 < n2 ? n1 : n2;
    int cmp = memcmp((const void *)(uintptr_t)off1, (const void *)(uintptr_t)off2, min_len);
    
    if (cmp == 0) {
      if (n1 == n2) {
        return mkval(T_BOOL, (op == TOK_LE || op == TOK_GE) ? 1 : 0);
      } else cmp = (n1 < n2) ? -1 : 1;
    }
    
    switch (op) {
      case TOK_LT: return mkval(T_BOOL, cmp < 0 ? 1 : 0);
      case TOK_LE: return mkval(T_BOOL, cmp <= 0 ? 1 : 0);
      case TOK_GT: return mkval(T_BOOL, cmp > 0 ? 1 : 0);
      case TOK_GE: return mkval(T_BOOL, cmp >= 0 ? 1 : 0);
      default:     return js_mkerr(js, "bad str op");
    }
  } else return js_mkerr(js, "bad str op");
}


typedef enum { ITER_CONTINUE, ITER_BREAK, ITER_ERROR } iter_action_t;
typedef iter_action_t (*iter_callback_t)(ant_t *js, ant_value_t value, void *ctx, ant_value_t *out);

static bool js_try_call_method(ant_t *js, ant_value_t obj, const char *method, size_t method_len, ant_value_t *args, int nargs, ant_value_t *out_result) {
  ant_value_t getter = js_mkundef(); bool has_getter = false;
  uintptr_t off = lkp_with_getter(js, obj, method, method_len, &getter, &has_getter);
  
  ant_value_t fn;
  if (has_getter) {
    fn = call_proto_accessor(js, obj, getter, true, NULL, 0, false);
    if (is_err(fn)) { *out_result = fn; return true; }
  } else if (off != 0) {
    fn = propref_load(js, (ant_offset_t)off);
  } else return false;
  
  uint8_t ft = vtype(fn);
  if (ft != T_FUNC && ft != T_CFUNC) return false;
  
  ant_value_t saved_this = js->this_val;
  js->this_val = obj;
  
  ant_value_t result;
  if (ft == T_CFUNC) result = ((ant_value_t (*)(ant_t *, ant_value_t *, int))vdata(fn))(js, args, nargs);
  else result = sv_vm_call(js->vm, js, fn, obj, args, nargs, NULL, false);
  
  bool had_throw = js->thrown_exists;
  ant_value_t thrown = js->thrown_value;
  
  js->this_val = saved_this;
  if (had_throw) {
    js->thrown_exists = true;
    js->thrown_value = thrown;
  }
  
  *out_result = result;
  return true;
}

static ant_value_t js_call_method(ant_t *js, ant_value_t obj, const char *method, size_t method_len, ant_value_t *args, int nargs) {
  ant_value_t result;
  if (!js_try_call_method(js, obj, method, method_len, args, nargs, &result)) return js_mkundef();
  return result;
}

static ant_value_t js_call_toString(ant_t *js, ant_value_t value) {
  ant_value_t result = js_call_method(js, value, "toString", 8, NULL, 0);
  
  if (is_err(result)) return result;
  if (vtype(result) == T_STR) return result;
  
  uint8_t rtype = vtype(result);
  if (rtype == T_UNDEF) {
    goto fallback;
  }
  
  if (rtype != T_OBJ && rtype != T_ARR && rtype != T_FUNC) {
    char buf[256];
    size_t len = tostr(js, result, buf, sizeof(buf));
    return js_mkstr(js, buf, len);
  }
  
fallback:;
  char buf[4096];
  size_t len = tostr(js, value, buf, sizeof(buf));
  return js_mkstr(js, buf, len);
}

static ant_value_t js_call_valueOf(ant_t *js, ant_value_t value) {
  ant_value_t result = js_call_method(js, value, "valueOf", 7, NULL, 0);
  if (vtype(result) == T_UNDEF) return value;
  return result;
}

static inline bool is_primitive(ant_value_t v) {
  uint8_t t = vtype(v);
  return t == T_STR || t == T_NUM || t == T_BOOL || t == T_NULL || t == T_UNDEF || t == T_SYMBOL || t == T_BIGINT;
}

static ant_value_t try_exotic_to_primitive(ant_t *js, ant_value_t value, int hint) {
  ant_value_t tp_sym = get_toPrimitive_sym();
  if (vtype(tp_sym) != T_SYMBOL) return mkval(T_UNDEF, 0);
  ant_offset_t tp_off = lkp_sym_proto(js, value, (ant_offset_t)vdata(tp_sym));
  if (tp_off == 0) return mkval(T_UNDEF, 0);
  
  ant_value_t tp_fn = propref_load(js, tp_off);
  uint8_t ft = vtype(tp_fn);
  
  if (ft == T_UNDEF) return mkval(T_UNDEF, 0);
  if (ft != T_FUNC && ft != T_CFUNC) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Symbol.toPrimitive is not a function");
  }
  
  const char *hint_str = hint == 1 ? "string" : (hint == 2 ? "number" : "default");
  ant_value_t hint_arg = js_mkstr(js, hint_str, strlen(hint_str));
  ant_value_t result = sv_vm_call(js->vm, js, tp_fn, value, &hint_arg, 1, NULL, false);
  
  if (is_err(result) || is_primitive(result)) return result;
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert object to primitive value");
}

static ant_value_t try_ordinary_to_primitive(ant_t *js, ant_value_t value, int hint) {
  static const char *names[] = {"valueOf", "toString"};
  static const size_t lens[] = {7, 8};
  
  int first = (hint == 1); 
  ant_value_t result;
  
  for (int i = 0; i < 2; i++) {
    int idx = first ^ i;
    if (js_try_call_method(js, value, names[idx], lens[idx], NULL, 0, &result))
      if (is_err(result) || is_primitive(result)) return result;
  }
  
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert object to primitive value");
}

ant_value_t js_to_primitive(ant_t *js, ant_value_t value, int hint) {
  if (is_primitive(value)) return value;
  if (!is_object_type(value)) return value;
  
  ant_value_t result = try_exotic_to_primitive(js, value, hint);
  if (vtype(result) != T_UNDEF) return result;
  
  return try_ordinary_to_primitive(js, value, hint);
}

bool strict_eq_values(ant_t *js, ant_value_t l, ant_value_t r) {
  uint8_t t = vtype(l);
  if (t != vtype(r)) return false;
  if (t == T_STR) {
    ant_offset_t n1, n2, off1 = vstr(js, l, &n1), off2 = vstr(js, r, &n2);
    return n1 == n2 &&
      memcmp((const void *)(uintptr_t)off1, (const void *)(uintptr_t)off2, n1) == 0;
  }
  if (t == T_NUM) return tod(l) == tod(r);
  if (t == T_BIGINT) return bigint_compare(js, l, r) == 0;
  return vdata(l) == vdata(r);
}

ant_value_t coerce_to_str(ant_t *js, ant_value_t v) {
  if (vtype(v) == T_STR) return v;
  
  if (is_object_type(v)) {
    ant_value_t prim = js_to_primitive(js, v, 1);
    if (is_err(prim)) return prim;
    if (vtype(prim) == T_STR) return prim;
    return js_tostring_val(js, prim);
  }
  
  return js_tostring_val(js, v);
}

ant_value_t coerce_to_str_concat(ant_t *js, ant_value_t v) {
  if (vtype(v) == T_STR) return v;
  
  if (is_object_type(v)) {
    ant_value_t prim = js_to_primitive(js, v, 0);
    if (is_err(prim)) return prim;
    if (vtype(prim) == T_STR) return prim;
    return js_tostring_val(js, prim);
  }
  
  return js_tostring_val(js, v);
}

static ant_value_t check_frozen_sealed(ant_t *js, ant_value_t obj, const char *action) {
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  if (!ptr) return js_mkundef();

  if (ptr->frozen) {
    if (sv_is_strict_context(js)) return js_mkerr(js, "cannot %s property of frozen object", action);
    return js_false;
  }
  if (ptr->sealed) {
    if (sv_is_strict_context(js)) return js_mkerr(js, "cannot %s property of sealed object", action);
    return js_false;
  }
  return js_mkundef();
}

ant_value_t js_delete_prop(ant_t *js, ant_value_t obj, const char *key, size_t len) {
  ant_value_t original_obj = obj;
  obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return js_true;
  if (is_proxy(obj)) {
    ant_value_t result = proxy_delete(js, obj, key, len);
    return is_err(result) ? result : js_bool(js_truthy(js, result));
  }

  ant_value_t err = check_frozen_sealed(js, obj, "delete");
  if (vtype(err) != T_UNDEF) return err;

  if (array_obj_ptr(original_obj) && is_length_key(key, len)) {
    if (sv_is_strict_context(js)) return js_mkerr_typed(js, JS_ERR_TYPE, "cannot delete non-configurable property");
    return js_false;
  }

  if (array_obj_ptr(original_obj)) {
    ant_offset_t doff = get_dense_buf(original_obj);
    unsigned long del_idx = 0;
    if (doff && parse_array_index(key, len, get_array_length(js, original_obj), &del_idx)) {
      ant_offset_t dense_len = dense_iterable_length(js, original_obj);
      if ((ant_offset_t)del_idx < dense_len) dense_set(js, doff, (ant_offset_t)del_idx, T_EMPTY);
    }
  }

  const char *interned = intern_string(key, len);
  if (!interned) {
    try_dynamic_deleter(js, obj, key, len);
    return js_true;
  }

  int32_t shape_slot = ant_shape_lookup_interned(ptr->shape, interned);
  if (shape_slot < 0) {
    try_dynamic_deleter(js, obj, key, len);
    return js_true;
  }

  uint8_t attrs = ant_shape_get_attrs(ptr->shape, (uint32_t)shape_slot);
  if ((attrs & ANT_PROP_ATTR_CONFIGURABLE) == 0) {
    if (sv_is_strict_context(js)) return js_mkerr_typed(js, JS_ERR_TYPE, "cannot delete non-configurable property");
    return js_false;
  }

  if (ptr->is_exotic) {
    descriptor_entry_t *desc = lookup_descriptor(obj, key, len);
    if (desc && !desc->configurable) {
      if (sv_is_strict_context(js)) return js_mkerr_typed(js, JS_ERR_TYPE, "cannot delete non-configurable property");
      return js_false;
    }
  }

  uint32_t slot = (uint32_t)shape_slot;
  if (!js_obj_ensure_unique_shape(ptr)) return js_mkerr(js, "oom");
  uint32_t swapped_from = slot;
  if (!ant_shape_remove_slot(ptr->shape, slot, &swapped_from)) return js_true;

  obj_remove_prop_slot(ptr, slot);
  propref_adjust_after_swap_delete(js, ptr, slot, swapped_from);
  return js_true;
}

ant_value_t js_delete_sym_prop(ant_t *js, ant_value_t obj, ant_value_t sym) {
  obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return js_true;
  if (is_proxy(obj)) {
    ant_value_t result = proxy_delete_val(js, obj, sym);
    return is_err(result) ? result : js_bool(js_truthy(js, result));
  }

  ant_value_t err = check_frozen_sealed(js, obj, "delete");
  if (vtype(err) != T_UNDEF) return err;

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
  
  uint32_t swapped_from = slot;
  if (!ant_shape_remove_slot(ptr->shape, slot, &swapped_from)) return js_true;
  
  obj_remove_prop_slot(ptr, slot);
  propref_adjust_after_swap_delete(js, ptr, slot, swapped_from);
  
  return js_true;
}

static ant_value_t iter_call_noargs_with_this(ant_t *js, ant_value_t this_val, ant_value_t method) {
  ant_value_t result = sv_vm_call(js->vm, js, method, this_val, NULL, 0, NULL, false);
  return result;
}

static ant_value_t iter_close_iterator(ant_t *js, ant_value_t iterator) {
  ant_offset_t return_off = lkp_proto(js, iterator, "return", 6);
  if (return_off == 0) return js_mkundef();
  ant_value_t return_method = propref_load(js, return_off);
  if (vtype(return_method) != T_FUNC && vtype(return_method) != T_CFUNC) {
    return js_mkerr(js, "iterator.return is not a function");
  }
  return iter_call_noargs_with_this(js, iterator, return_method);
}

static ant_value_t iter_foreach(ant_t *js, ant_value_t iterable, iter_callback_t cb, void *ctx) {
  ant_value_t iter_sym = get_iterator_sym();
  ant_offset_t iter_prop = (vtype(iter_sym) == T_SYMBOL) ? lkp_sym_proto(js, iterable, (ant_offset_t)vdata(iter_sym)) : 0;
  if (iter_prop == 0) return js_mkerr(js, "not iterable");

  ant_value_t iter_method = propref_load(js, iter_prop);
  ant_value_t iterator = iter_call_noargs_with_this(js, iterable, iter_method);
  if (is_err(iterator)) return iterator;
  
  ant_value_t out = js_mkundef();
  
  while (true) {
    ant_offset_t next_off = lkp_proto(js, iterator, "next", 4);
    if (next_off == 0) { return js_mkerr(js, "iterator.next is not a function"); }
    
    ant_value_t next_method = propref_load(js, next_off);
    if (vtype(next_method) != T_FUNC && vtype(next_method) != T_CFUNC) {
      return js_mkerr(js, "iterator.next is not a function");
    }
    
    ant_value_t result = iter_call_noargs_with_this(js, iterator, next_method);
    if (is_err(result)) { return result; }
    
    ant_offset_t done_off = lkp(js, result, "done", 4);
    ant_value_t done_val = done_off ? propref_load(js, done_off) : js_mkundef();
    if (js_truthy(js, done_val)) break;
    
    ant_offset_t value_off = lkp(js, result, "value", 5);
    ant_value_t value = value_off ? propref_load(js, value_off) : js_mkundef();
    
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

ant_value_t js_symbol_to_string(ant_t *js, ant_value_t sym) {
  const char *desc = js_sym_desc(js, sym);
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

static ant_value_t builtin_String(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t sval;
  
  if (nargs == 0) sval = js_mkstr(js, "", 0);
  else if (vtype(args[0]) == T_STR) sval = args[0];
  
  else if (vtype(args[0]) == T_SYMBOL) {
    sval = js_symbol_to_string(js, args[0]);
    if (is_err(sval)) return sval;
  } else {
    sval = coerce_to_str(js, args[0]);
    if (is_err(sval)) return sval;
  }
  
  ant_value_t string_proto = js_get_ctor_proto(js, "String", 6);
  if (is_wrapper_ctor_target(js, js->this_val, string_proto)) {
    set_slot(js->this_val, SLOT_PRIMITIVE, sval);
    
    ant_offset_t byte_len;
    ant_offset_t str_off = vstr(js, sval, &byte_len);
    const char *str_data = (const char *)(uintptr_t)(str_off);
    
    js_setprop(js, js->this_val, js->length_str, tov((double)utf16_strlen(str_data, byte_len)));
    js_set_descriptor(js, js_as_obj(js->this_val), "length", 6, 0);
  }
  
  return sval;
}

static ant_value_t builtin_Number_isNaN(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return mkval(T_BOOL, 0);
  ant_value_t arg = args[0];
  
  if (vtype(arg) != T_NUM) return mkval(T_BOOL, 0);
  
  double val = tod(arg);
  return mkval(T_BOOL, isnan(val) ? 1 : 0);
}

static ant_value_t builtin_Number_isFinite(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return mkval(T_BOOL, 0);
  ant_value_t arg = args[0];
  
  if (vtype(arg) != T_NUM) return mkval(T_BOOL, 0);
  
  double val = tod(arg);
  return mkval(T_BOOL, isfinite(val) ? 1 : 0);
}

static ant_value_t builtin_global_isNaN(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return mkval(T_BOOL, 1);
  double val = js_to_number(js, args[0]);
  return mkval(T_BOOL, isnan(val) ? 1 : 0);
}

static ant_value_t builtin_global_isFinite(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return mkval(T_BOOL, 0);
  double val = js_to_number(js, args[0]);
  return mkval(T_BOOL, isfinite(val) ? 1 : 0);
}

static ant_value_t builtin_eval(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return js_mkundef();
  ant_value_t code = args[0];
  if (vtype(code) != T_STR) return code;
  ant_offset_t code_len = 0;
  ant_offset_t code_off = vstr(js, code, &code_len);
  const char *code_str = (const char *)(uintptr_t)(code_off);
  return js_eval_bytecode_eval_with_strict(js, code_str, (size_t)code_len, false);
}

static ant_value_t builtin_Number_isInteger(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return mkval(T_BOOL, 0);
  ant_value_t arg = args[0];
  
  if (vtype(arg) != T_NUM) return mkval(T_BOOL, 0);
  
  double val = tod(arg);
  if (!isfinite(val)) return mkval(T_BOOL, 0);
  return mkval(T_BOOL, (val == floor(val)) ? 1 : 0);
}

static ant_value_t builtin_Number_isSafeInteger(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return mkval(T_BOOL, 0);
  ant_value_t arg = args[0];
  
  if (vtype(arg) != T_NUM) return mkval(T_BOOL, 0);
  
  double val = tod(arg);
  if (!isfinite(val)) return mkval(T_BOOL, 0);
  if (val != floor(val)) return mkval(T_BOOL, 0);
  
  return mkval(T_BOOL, (val >= -9007199254740991.0 && val <= 9007199254740991.0) ? 1 : 0);
}

static ant_value_t builtin_Number(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t nval = tov(nargs > 0 ? js_to_number(js, args[0]) : 0.0);
  ant_value_t number_proto = js_get_ctor_proto(js, "Number", 6);
  if (is_wrapper_ctor_target(js, js->this_val, number_proto)) {
    set_slot(js->this_val, SLOT_PRIMITIVE, nval);
  }
  return nval;
}

static ant_value_t builtin_Boolean(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t bval = mkval(T_BOOL, nargs > 0 && js_truthy(js, args[0]) ? 1 : 0);
  ant_value_t boolean_proto = js_get_ctor_proto(js, "Boolean", 7);
  if (is_wrapper_ctor_target(js, js->this_val, boolean_proto)) {
    set_slot(js->this_val, SLOT_PRIMITIVE, bval);
  }
  return bval;
}

static ant_value_t builtin_Object(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0 || vtype(args[0]) == T_NULL || vtype(args[0]) == T_UNDEF) {
    ant_value_t obj_proto = js_get_ctor_proto(js, "Object", 6);
    if (is_unboxed_obj(js, js->this_val, obj_proto)) return js->this_val;
    return js_mkobj(js);
  }
  
  ant_value_t arg = args[0];
  uint8_t t = vtype(arg);
  
  if (t == T_OBJ || t == T_ARR || t == T_FUNC) return arg;
  if (t == T_STR || t == T_NUM || t == T_BOOL || t == T_BIGINT) {
    ant_value_t wrapper = js_mkobj(js);
    if (is_err(wrapper)) return wrapper;
    set_slot(wrapper, SLOT_PRIMITIVE, arg);
    ant_value_t proto = get_prototype_for_type(js, t);
    if (vtype(proto) == T_OBJ) js_set_proto_init(wrapper, proto);
    return wrapper;
  }
  
  return arg;
}

static ant_value_t builtin_function_empty(ant_t *, ant_value_t *, int);

static ant_value_t build_dynamic_function(ant_t *js, ant_value_t *args, int nargs, bool is_async) {
  if (nargs == 0) {
    ant_value_t func_obj = mkobj(js, 0);
    if (is_err(func_obj)) return func_obj;
    
    set_func_code_ptr(js, func_obj, "(){}", 4);
    if (is_async) {
      set_slot(func_obj, SLOT_ASYNC, js_true);
      ant_value_t async_proto = get_slot(js_glob(js), SLOT_ASYNC_PROTO);
      if (vtype(async_proto) == T_FUNC) js_set_proto_init(func_obj, async_proto);
    } else {
      ant_value_t func_proto = get_slot(js_glob(js), SLOT_FUNC_PROTO);
      ant_value_t instance_proto = js_instance_proto_from_new_target(js, func_proto);
      if (is_object_type(instance_proto)) js_set_proto_init(func_obj, instance_proto);
    }
    set_slot(func_obj, SLOT_CFUNC, js_mkfun(builtin_function_empty));
    
    ant_value_t func = js_obj_to_func(func_obj);
    ant_value_t proto_setup = setup_func_prototype(js, func);
    if (is_err(proto_setup)) return proto_setup;
    return func;
  }
  
  size_t total_len = 1;
  
  for (int i = 0; i < nargs - 1; i++) {
    args[i] = coerce_to_str(js, args[i]);
    if (is_err(args[i])) return args[i];
    total_len += vstrlen(js, args[i]);
    if (i < nargs - 2) total_len += 1;
  }
  
  total_len += 2;
  
  ant_value_t body = coerce_to_str(js, args[nargs - 1]);
  if (is_err(body)) return body;
  total_len += vstrlen(js, body);
  total_len += 1;
  
  char *code_buf = (char *)malloc(total_len + 1);
  if (!code_buf) return js_mkerr(js, "oom");
  size_t pos = 0;

  code_buf[pos++] = '(';
  for (int i = 0; i < nargs - 1; i++) {
    ant_offset_t param_len, param_off = vstr(js, args[i], &param_len);
    memcpy(code_buf + pos, (const void *)(uintptr_t)param_off, param_len);
    pos += param_len;
    if (i < nargs - 2) code_buf[pos++] = ',';
  }
  code_buf[pos++] = ')';
  code_buf[pos++] = '{';
  ant_offset_t body_len, body_off = vstr(js, body, &body_len);
  memcpy(code_buf + pos, (const void *)(uintptr_t)body_off, body_len);
  pos += body_len;
  code_buf[pos++] = '}';
  code_buf[pos] = '\0';

  ant_value_t func_obj = mkobj(js, 0);
  if (is_err(func_obj)) { free(code_buf); return func_obj; }

  sv_func_t *compiled = sv_compile_function(js, code_buf, pos, is_async);
  if (!compiled) {
    free(code_buf);
    return js_mkerr_typed(js, JS_ERR_SYNTAX, "invalid function body");
  }

  sv_closure_t *closure = js_closure_alloc(js);
  if (!closure) { free(code_buf); return js_mkerr(js, "oom"); }
  closure->func = compiled;
  closure->bound_this = js_mkundef();
  closure->call_flags = 0;
  closure->func_obj = func_obj;

  size_t params_len = (size_t)(pos - 2) - (size_t)body_len - 2;
  const char *async_prefix = is_async ? "async " : "";
  size_t async_len = is_async ? 6 : 0;
  size_t display_len = async_len + 19 + params_len + 5 + (size_t)body_len + 2;
  char *display = (char *)malloc(display_len + 1);
  if (!display) { free(code_buf); return js_mkerr(js, "oom"); }
  size_t n = 0;
  
  memcpy(display + n, async_prefix, async_len);         n += async_len;
  memcpy(display + n, "function anonymous(", 19);       n += 19;
  memcpy(display + n, code_buf + 1, params_len);        n += params_len;
  memcpy(display + n, "\n) {\n", 5);                    n += 5;
  memcpy(display + n, code_buf + 1 + params_len + 2, (size_t)body_len); n += (size_t)body_len;
  memcpy(display + n, "\n}", 2);                        n += 2;
  display[n] = '\0';
  set_func_code(js, func_obj, display, display_len);
  free(display);
  free(code_buf);
  
  if (is_async) {
    set_slot(func_obj, SLOT_ASYNC, js_true);
    ant_value_t async_proto = get_slot(js_glob(js), SLOT_ASYNC_PROTO);
    if (vtype(async_proto) == T_FUNC) js_set_proto_init(func_obj, async_proto);
  } else {
    ant_value_t func_proto = get_slot(js_glob(js), SLOT_FUNC_PROTO);
    ant_value_t instance_proto = js_instance_proto_from_new_target(js, func_proto);
    if (is_object_type(instance_proto)) js_set_proto_init(func_obj, instance_proto);
  }

  ant_value_t func = mkval(T_FUNC, (uintptr_t)closure);
  ant_value_t proto_setup = setup_func_prototype(js, func);
  if (is_err(proto_setup)) return proto_setup;
  
  return func;
}

static ant_value_t builtin_Function(ant_t *js, ant_value_t *args, int nargs) {
  return build_dynamic_function(js, args, nargs, false);
}

static ant_value_t builtin_AsyncFunction(ant_t *js, ant_value_t *args, int nargs) {
  return build_dynamic_function(js, args, nargs, true);
}

static ant_value_t builtin_function_empty(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkundef();
}

static ant_value_t builtin_function_call(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t func = js->this_val;
  if (vtype(func) != T_FUNC && vtype(func) != T_CFUNC) {
    return js_mkerr(js, "call requires a function");
  }
  
  ant_value_t this_arg = (nargs > 0) ? args[0] : js_mkundef();
  ant_value_t *call_args = NULL;
  
  int call_nargs = (nargs > 1) ? nargs - 1 : 0;
  if (call_nargs > 0) call_args = &args[1];

  return sv_vm_call_explicit_this(js->vm, js, func, this_arg, call_args, call_nargs);
}

static int extract_array_args(ant_t *js, ant_value_t arr, ant_value_t **out_args) {
  int len = (int) get_array_length(js, arr);
  if (len <= 0) return 0;
  
  ant_value_t *args_out = (ant_value_t *)ant_calloc(sizeof(ant_value_t) * len);
  if (!args_out) return 0;
  
  for (int i = 0; i < len; i++) {
    args_out[i] = arr_get(js, arr, (ant_offset_t)i);
  }
  
  *out_args = args_out;
  return len;
}

static ant_value_t builtin_function_toString(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t func = js->this_val;
  uint8_t t = vtype(func);
  
  if (t != T_FUNC && t != T_CFUNC) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Function.prototype.toString requires that 'this' be a Function");
  }
  
  if (t == T_CFUNC) return ANT_STRING("function() { [native code] }");
  
  ant_value_t func_obj = js_func_obj(func);
  ant_value_t cfunc_slot = get_slot(func_obj, SLOT_CFUNC);
  
  if (vtype(cfunc_slot) == T_CFUNC) {
    ant_offset_t name_len = 0;
    const char *name = get_func_name(js, func, &name_len);
    if (name && name_len > 0) {
      size_t total = 9 + name_len + 21 + 1;
      char *buf = ant_calloc(total);
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
  
  if (vtype(code_val) == T_CFUNC && vtype(len_val) == T_NUM) {
    const char *code = (const char *)(uintptr_t)vdata(code_val);
    size_t code_len = (size_t)tod(len_val);
    
    if (code && code_len > 0) {
      ant_value_t async_slot = get_slot(func_obj, SLOT_ASYNC);
      sv_closure_t *closure = js_func_closure(func);
      
      bool is_async = (async_slot == js_true);
      bool is_arrow = (closure->call_flags & SV_CALL_IS_ARROW) != 0;
      
      if (is_arrow) {
        const char *paren_end = memchr(code, ')', code_len);
        if (!paren_end) goto fallback_arrow;
        
        size_t params_len = paren_end - code + 1;
        const char *body = paren_end + 1;
        size_t body_len = code_len - params_len;
        
        size_t len = (is_async ? 6 : 0) + params_len + 4 + body_len + 1;
        char *buf = ant_calloc(len);
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
    if (fn && fn->source && fn->source_end > fn->source_start) {
      int start = fn->source_start;
      int end   = fn->source_end;
      if (start >= 0 && end <= fn->source_len && end > start)
        return js_mkstr(js, fn->source + start, (size_t)(end - start));
    }
  }

  char buf[256];
  size_t len = strfunc(js, func, buf, sizeof(buf));
  return js_mkstr(js, buf, len);
}

static ant_value_t builtin_function_apply(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t func = js->this_val;
  if (vtype(func) != T_FUNC && vtype(func) != T_CFUNC) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Function.prototype.apply requires that 'this' be a Function");
  }
  
  ant_value_t this_arg = (nargs > 0) ? args[0] : js_mkundef();
  ant_value_t *call_args = NULL;
  int call_nargs = 0;
  
  if (nargs > 1) {
    ant_value_t arg_array = args[1];
    uint8_t t = vtype(arg_array);
    if (t == T_ARR || t == T_OBJ) {
      call_nargs = extract_array_args(js, arg_array, &call_args);
    } else if (t != T_UNDEF && t != T_NULL) {}
  }
  
  ant_value_t result = sv_vm_call_explicit_this(js->vm, js, func, this_arg, call_args, call_nargs);
  if (call_args) free(call_args);
  
  return result;
}

static ant_value_t builtin_function_bind(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t func = js->this_val;
  
  if (vtype(func) != T_FUNC && vtype(func) != T_CFUNC) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "bind requires a function");
  }

  ant_value_t this_arg = (nargs > 0) ? args[0] : js_mkundef();
  
  int bound_argc = (nargs > 1) ? nargs - 1 : 0;
  ant_value_t *bound_args = (bound_argc > 0) ? &args[1] : NULL;
  
  int orig_length = 0;
  ant_value_t target_func_obj;
  if (vtype(func) == T_CFUNC) {
    orig_length = 0;
  } else {
    target_func_obj = js_func_obj(func);
    ant_value_t len_val = lkp_interned_val(js, target_func_obj, INTERN_LENGTH);
    if (vtype(len_val) == T_NUM) {
      orig_length = (int) tod(len_val);
    }
  }
  
  int bound_length = orig_length - bound_argc;
  if (bound_length < 0) bound_length = 0;

  if (vtype(func) == T_CFUNC) {
    ant_value_t bound_func = mkobj(js, 0);
    if (is_err(bound_func)) return bound_func;
    
    set_slot(bound_func, SLOT_CFUNC, func);
    
    ant_value_t func_proto = get_slot(js_glob(js), SLOT_FUNC_PROTO);
    if (vtype(func_proto) == T_FUNC) js_set_proto_init(bound_func, func_proto);

    ant_value_t bound = js_obj_to_func_ex(bound_func, bound_argc > 0 ? SV_CALL_HAS_BOUND_ARGS : 0);
    sv_closure_t *bc = js_func_closure(bound);
    bc->bound_this = this_arg;
    if (bound_argc > 0) {
      bc->bound_argv = malloc(sizeof(ant_value_t) * (size_t)bound_argc);
      memcpy(bc->bound_argv, bound_args, sizeof(ant_value_t) * (size_t)bound_argc);
      bc->bound_argc = bound_argc;
    }
    js_setprop(js, bound_func, js->length_str, tov((double) bound_length));
    
    ant_value_t proto_setup = setup_func_prototype(js, bound);
    if (is_err(proto_setup)) return proto_setup;

    js_mark_constructor(bound_func, js_is_constructor(js, func));
    
    return bound;
  }

  ant_value_t func_obj = js_func_obj(func);
  ant_value_t bound_func = mkobj(js, 0);
  if (is_err(bound_func)) return bound_func;

  ant_value_t code_val = get_slot(func_obj, SLOT_CODE);
  if (vtype(code_val) == T_STR || vtype(code_val) == T_CFUNC) {
    set_slot(bound_func, SLOT_CODE, code_val);
    set_slot(bound_func, SLOT_CODE_LEN, get_slot(func_obj, SLOT_CODE_LEN));
  }

  sv_closure_t *orig = js_func_closure(func);
  sv_closure_t *bound_closure = js_closure_alloc(js);
  if (!bound_closure) return js_mkerr(js, "oom");
  
  bound_closure->func = orig->func;
  bound_closure->call_flags = orig->call_flags;
  bound_closure->upvalues = NULL;
  bound_closure->bound_this = this_arg;
  bound_closure->bound_args = js_mkundef();
  bound_closure->super_val = orig->super_val;
  bound_closure->func_obj = bound_func;

  if (orig->func && orig->func->upvalue_count > 0 && orig->upvalues) {
    size_t upvalue_bytes = sizeof(sv_upvalue_t *) * (size_t)orig->func->upvalue_count;
    bound_closure->upvalues = malloc(upvalue_bytes);
    if (!bound_closure->upvalues) return js_mkerr(js, "oom");
    memcpy(bound_closure->upvalues, orig->upvalues, upvalue_bytes);
  }
  
  if (bound_argc > 0)
    bound_closure->call_flags |= SV_CALL_HAS_BOUND_ARGS;

  ant_value_t async_slot = get_slot(func_obj, SLOT_ASYNC);
  if (vtype(async_slot) == T_BOOL && vdata(async_slot) == 1) {
    set_slot(bound_func, SLOT_ASYNC, js_true);
  }

  ant_value_t target_proto = get_proto(js, func);
  if (is_object_type(target_proto)) {
    js_set_proto_init(bound_func, target_proto);
  } else if (vtype(async_slot) == T_BOOL && vdata(async_slot) == 1) {
    ant_value_t async_proto = get_slot(js_glob(js), SLOT_ASYNC_PROTO);
    if (vtype(async_proto) == T_FUNC) js_set_proto_init(bound_func, async_proto);
  } else {
    ant_value_t func_proto = get_slot(js_glob(js), SLOT_FUNC_PROTO);
    if (vtype(func_proto) == T_FUNC) js_set_proto_init(bound_func, func_proto);
  }

  ant_value_t data_slot = get_slot(func_obj, SLOT_DATA);
  if (vtype(data_slot) != T_UNDEF) {
    set_slot(bound_func, SLOT_DATA, data_slot);
  }

  set_slot(bound_func, SLOT_TARGET_FUNC, func);
  
  if (bound_argc > 0) {
    ant_value_t bound_arr = mkarr(js);
    for (int i = 0; i < bound_argc; i++) arr_set(js, bound_arr, (ant_offset_t)i, bound_args[i]);
    bound_closure->bound_args = bound_arr;
    bound_closure->bound_argv = malloc(sizeof(ant_value_t) * (size_t)bound_argc);
    memcpy(bound_closure->bound_argv, bound_args, sizeof(ant_value_t) * (size_t)bound_argc);
    bound_closure->bound_argc = bound_argc;
  }

  ant_value_t cfunc_slot = get_slot(func_obj, SLOT_CFUNC);
  if (vtype(cfunc_slot) == T_CFUNC) {
    set_slot(bound_func, SLOT_CFUNC, cfunc_slot);
  }
  
  js_setprop(js, bound_func, js->length_str, tov((double) bound_length));
  
  ant_value_t bound = mkval(T_FUNC, (uintptr_t)bound_closure);  
  ant_value_t proto_setup = setup_func_prototype(js, bound);
  
  if (is_err(proto_setup)) return proto_setup;
  js_mark_constructor(bound_func, js_is_constructor(js, func));
  
  return bound;
}

static ant_value_t builtin_Array(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = mkarr(js);
  if (is_err(arr)) return arr;
  
  if (nargs == 1 && vtype(args[0]) == T_NUM) {
    ant_value_t err = validate_array_length(js, args[0]);
    if (is_err(err)) return err;
    ant_offset_t new_len = (ant_offset_t)tod(args[0]);
    ant_offset_t doff = get_dense_buf(arr);
    if (doff && new_len <= 1024) {
      if (new_len > dense_capacity(doff)) doff = dense_grow(js, arr, new_len);
    }
    array_len_set(js, arr, new_len);
  } else if (nargs > 0) {
    for (int i = 0; i < nargs; i++) arr_set(js, arr, (ant_offset_t)i, args[i]);
  }

  ant_value_t array_proto = get_ctor_proto(js, "Array", 5);
  ant_value_t instance_proto = js_instance_proto_from_new_target(js, array_proto);
  
  if (is_object_type(instance_proto)) js_set_proto_init(arr, instance_proto);
  if (vtype(js->new_target) == T_FUNC || vtype(js->new_target) == T_CFUNC) {
    set_slot(arr, SLOT_CTOR, js->new_target);
  }
  
  return arr;
}

static ant_value_t builtin_error_captureStackTrace(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || !is_object_type(args[0])) {
    return js_mkerr(js, "argument must be an object");
  }

  ant_value_t target = args[0];
  ant_value_t error_ctor = lkp_val(js, js->global, "Error", 5);
  ant_value_t prep = js_mkundef();

  if (vtype(error_ctor) == T_FUNC || vtype(error_ctor) == T_CFUNC) {
    prep = lkp_val(js, js_func_obj(error_ctor), "prepareStackTrace", 17);
  }

  if (vtype(prep) == T_FUNC || vtype(prep) == T_CFUNC) {
    ant_value_t callsites = js_build_callsite_array(js);
    ant_value_t prep_args[2] = { target, callsites };
    ant_value_t result = sv_vm_call(js->vm, js, prep, js_mkundef(), prep_args, 2, NULL, false);
    if (js->thrown_exists) return js_mkundef();
    js_set(js, target, "stack", result);
    js_set_descriptor(js, js_as_obj(target), "stack", 5, JS_DESC_W | JS_DESC_C);
  } else js_capture_stack(js, target);

  return js_mkundef();
}

static ant_value_t builtin_error_isError(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_false;
  ant_value_t val = args[0];
  if (!is_object_type(val)) return js_false;
  return get_slot(val, SLOT_ERROR_BRAND) == js_true ? js_true : js_false;
}

static ant_value_t builtin_Error(ant_t *js, ant_value_t *args, int nargs) {
  bool is_new = (vtype(js->new_target) != T_UNDEF);
  ant_value_t this_val = js->this_val;
  
  ant_value_t target = is_new ? js->new_target : js->current_func;
  ant_value_t name = ANT_STRING("Error");
  
  if (vtype(target) == T_FUNC) {
    ant_value_t n = lkp_val(js, js_func_obj(target), "name", 4);
    if (vtype(n) != T_UNDEF) name = n;
  }

  if (!is_new) {
    this_val = js_mkobj(js);
    ant_value_t proto = lkp_interned_val(js, js_func_obj(js->current_func), INTERN_PROTOTYPE);
    if (vtype(proto) != T_UNDEF) js_set_proto_init(this_val, proto);
    else js_set_proto_init(this_val, get_ctor_proto(js, "Error", 5));
  }
  
  if (nargs > 0) {
    ant_value_t msg = args[0];
    if (vtype(msg) != T_STR) {
      const char *str = js_str(js, msg);
      msg = js_mkstr(js, str, strlen(str));
    }
    js_mkprop_fast(js, this_val, "message", 7, msg);
  }
  
  if (nargs > 1 && vtype(args[1]) == T_OBJ) {
    ant_offset_t cause_off = lkp(js, args[1], "cause", 5);
    if (cause_off) js_mkprop_fast(js, this_val, "cause", 5, propref_load(js, cause_off));
  }
  
  js_mkprop_fast(js, this_val, "name", 4, name);
  set_slot(this_val, SLOT_ERROR_BRAND, js_true);
  js_capture_stack(js, this_val);

  return this_val;
}

static ant_value_t builtin_Error_toString(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js_getthis(js);
  
  ant_value_t name = js_get(js, this_val, "name");
  if (vtype(name) == T_UNDEF) name = js_mkstr(js, "Error", 5);
  else if (vtype(name) != T_STR) {
    const char *s = js_str(js, name);
    name = js_mkstr(js, s, strlen(s));
  }
  
  ant_value_t msg = js_get(js, this_val, "message");
  if (vtype(msg) == T_UNDEF) msg = js_mkstr(js, "", 0);
  else if (vtype(msg) != T_STR) {
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

static ant_value_t builtin_AggregateError(ant_t *js, ant_value_t *args, int nargs) {
  bool is_new = (vtype(js->new_target) != T_UNDEF);
  ant_value_t this_val = js->this_val;
  
  if (!is_new) {
    this_val = js_mkobj(js);
    ant_offset_t proto_off = lkp_interned(js, js_func_obj(js->current_func), INTERN_PROTOTYPE, 9);
    if (proto_off) js_set_proto_init(this_val, propref_load(js, proto_off));
    else js_set_proto_init(this_val, get_ctor_proto(js, "AggregateError", 14));
  }
  
  ant_value_t errors = nargs > 0 ? args[0] : mkarr(js);
  if (vtype(errors) != T_ARR) errors = mkarr(js);
  js_mkprop_fast(js, this_val, "errors", 6, errors);
  
  if (nargs > 1 && vtype(args[1]) != T_UNDEF) {
    ant_value_t msg = args[1];
    if (vtype(msg) != T_STR) {
      const char *str = js_str(js, msg);
      msg = js_mkstr(js, str, strlen(str));
    }
    js_mkprop_fast(js, this_val, "message", 7, msg);
  }
  
  if (nargs > 2 && vtype(args[2]) == T_OBJ) {
    ant_offset_t cause_off = lkp(js, args[2], "cause", 5);
    if (cause_off) js_mkprop_fast(js, this_val, "cause", 5, propref_load(js, cause_off));
  }
  
  js_mkprop_fast(js, this_val, "name", 4, ANT_STRING("AggregateError"));
  set_slot(this_val, SLOT_ERROR_BRAND, js_true);

  return this_val;
}


typedef ant_value_t (*dynamic_kv_mapper_fn)(
  ant_t *js,
  ant_value_t key,
  ant_value_t val
);

static ant_value_t iterate_dynamic_keys(ant_t *js, ant_value_t obj, dynamic_kv_mapper_fn mapper) {
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || !ptr->exotic_keys || !ptr->exotic_ops || !ptr->exotic_ops->getter) return mkarr(js);
  ant_value_t keys_arr = ptr->exotic_keys(js, obj);
  ant_value_t arr = mkarr(js);
  ant_offset_t len = get_array_length(js, keys_arr);
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t key_val = arr_get(js, keys_arr, i);
    if (vtype(key_val) != T_STR) continue;
    ant_offset_t klen; ant_offset_t str_off = vstr(js, key_val, &klen);
    const char *key = (const char *)(uintptr_t)(str_off);
    ant_value_t val = ptr->exotic_ops->getter(js, obj, key, klen);
    js_arr_push(js, arr, mapper ? mapper(js, key_val, val) : val);
  }
  
  return mkval(T_ARR, vdata(arr));
}

static ant_value_t builtin_object_is(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_false;
  
  ant_value_t x = args[0];
  ant_value_t y = args[1];
  
  uint8_t tx = vtype(x);
  uint8_t ty = vtype(y);
  if (tx != ty) return js_false;
  
  if (tx == T_UNDEF || tx == T_NULL) return js_true;
  
  if (tx == T_NUM) {
    double dx = tod(x);
    double dy = tod(y);
    if (isnan(dx) && isnan(dy)) return js_true;
    if (dx == 0.0 && dy == 0.0) {
      bool x_neg = (1.0 / dx) < 0;
      bool y_neg = (1.0 / dy) < 0;
      return x_neg == y_neg ? js_true : js_false;
    }
    return dx == dy ? js_true : js_false;
  }
  
  if (tx == T_BOOL) return vdata(x) == vdata(y) ? js_true : js_false;
  return strict_eq_values(js, x, y) ? js_true : js_false;
}

enum obj_enum_mode { 
  OBJ_ENUM_KEYS,
  OBJ_ENUM_VALUES,
  OBJ_ENUM_ENTRIES
};

static ant_value_t map_to_entry(ant_t *js, ant_value_t key, ant_value_t val) {
  ant_value_t pair = mkarr(js);
  arr_set(js, pair, 0, key);
  arr_set(js, pair, 1, val);
  return mkval(T_ARR, vdata(pair));
}

static ant_value_t object_enum(ant_t *js, ant_value_t obj, enum obj_enum_mode mode) {
  bool is_arr = (vtype(obj) == T_ARR);
  if (vtype(obj) == T_FUNC) obj = js_func_obj(obj);

  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || !ptr->shape) return mkarr(js);
  if (ptr->is_exotic && ptr->exotic_keys) {
    if (mode == OBJ_ENUM_KEYS && (!ptr->exotic_ops || !ptr->exotic_ops->getter)) return ptr->exotic_keys(js, obj);
    if (ptr->exotic_ops && ptr->exotic_ops->getter) {
      dynamic_kv_mapper_fn mapper = (mode == OBJ_ENUM_ENTRIES) ? map_to_entry : NULL;
      return iterate_dynamic_keys(js, obj, mapper);
    }
  }
  
  ant_value_t arr = mkarr(js);
  ant_offset_t idx = 0;
  
  if (is_arr) {
    ant_offset_t doff = get_dense_buf(obj);
    ant_offset_t dense_len = doff ? dense_iterable_length(js, obj) : 0;
    
    for (ant_offset_t i = 0; i < dense_len; i++) {
      ant_value_t v = dense_get(doff, i);
      if (is_empty_slot(v)) continue;
      char idxstr[16]; size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
      ant_value_t key_val = js_mkstr(js, idxstr, idxlen);
      
      if (mode == OBJ_ENUM_KEYS) arr_set(js, arr, idx, key_val);
      else if (mode == OBJ_ENUM_VALUES) arr_set(js, arr, idx, v);
      else arr_set(js, arr, idx, map_to_entry(js, key_val, v));
      
      idx++;
    }
  }

  uint32_t shape_count = ant_shape_count(ptr->shape);
  for (uint32_t i = 0; i < shape_count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop || prop->type == ANT_SHAPE_KEY_SYMBOL) continue;
    if (i >= ptr->prop_count) continue;

    const char *key = prop->key.interned;
    ant_offset_t klen = (ant_offset_t)strlen(key);
    ant_value_t val = ant_object_prop_get_unchecked(ptr, i);

    if (is_internal_prop(key, klen)) continue;
    if (is_arr && is_array_index(key, klen)) {
    ant_offset_t doff = get_dense_buf(obj);
    if (doff) {
      unsigned long pidx = 0;
      for (ant_offset_t ci = 0; ci < klen; ci++) pidx = pidx * 10 + (key[ci] - '0');
      if (pidx < dense_iterable_length(js, obj)) continue;
    }}
    
    bool should_include = (ant_shape_get_attrs(ptr->shape, i) & ANT_PROP_ATTR_ENUMERABLE) != 0;
    if (should_include && ptr->is_exotic) {
      descriptor_entry_t *desc = lookup_descriptor(js_as_obj(obj), key, (size_t)klen);
      if (desc) should_include = desc->enumerable;
    }
    if (!should_include) continue;

    ant_value_t key_val = js_mkstr(js, key, (size_t)klen);
    if (mode == OBJ_ENUM_KEYS) arr_set(js, arr, idx, key_val);
    else if (mode == OBJ_ENUM_VALUES) arr_set(js, arr, idx, val);
    else arr_set(js, arr, idx, map_to_entry(js, key_val, val));
    
    idx++;
  }
  
  return mkval(T_ARR, vdata(arr));
}

static ant_value_t builtin_object_keys(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return mkarr(js);
  ant_value_t obj = args[0];
  if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR && vtype(obj) != T_FUNC) return mkarr(js);
  
  if (is_proxy(obj)) {
    ant_proxy_state_t *data = get_proxy_data(obj);
    if (!data) return mkarr(js);
    if (data->revoked)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot perform 'ownKeys' on a proxy that has been revoked");
    
    ant_offset_t trap_off = lkp(js, data->handler, "ownKeys", 7);
    if (!trap_off) return object_enum(js, data->target, OBJ_ENUM_KEYS);
    
    ant_value_t trap = propref_load(js, trap_off);
    uint8_t ft = vtype(trap);
    if (ft != T_FUNC && ft != T_CFUNC) return object_enum(js, data->target, OBJ_ENUM_KEYS);
    
    ant_value_t trap_args[1] = { data->target };
    ant_value_t result = sv_vm_call(js->vm, js, trap, data->handler, trap_args, 1, NULL, false);
    if (is_err(result)) return result;
    if (vtype(result) != T_ARR)
      return js_mkerr_typed(js, JS_ERR_TYPE, "ownKeys trap must return an array");
    
    ant_offset_t len = get_array_length(js, result);
    for (ant_offset_t i = 0; i < len; i++) {
      ant_value_t ki = arr_get(js, result, i);
      if (vtype(ki) != T_STR && vtype(ki) != T_SYMBOL)
        return js_mkerr_typed(js, JS_ERR_TYPE, "ownKeys trap result must contain only strings or symbols");
      ant_offset_t ki_len; ant_offset_t ki_off = vstr(js, ki, &ki_len);
      for (ant_offset_t j = 0; j < i; j++) {
        ant_value_t kj = arr_get(js, result, j);
        ant_offset_t kj_len; ant_offset_t kj_off = vstr(js, kj, &kj_len);
        if (ki_len == kj_len &&
            memcmp((const void *)(uintptr_t)ki_off, (const void *)(uintptr_t)kj_off, ki_len) == 0)
          return js_mkerr_typed(js, JS_ERR_TYPE, "ownKeys trap result must not contain duplicate entries");
      }
    }
    return result;
  }
  
  return object_enum(js, obj, OBJ_ENUM_KEYS);
}

static ant_value_t for_in_keys_add(ant_t *js, ant_value_t out, ant_value_t seen, ant_value_t key) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, out);
  GC_ROOT_PIN(js, seen);
  GC_ROOT_PIN(js, key);
  
  if (vtype(key) != T_STR) return js_mkundef();

  ant_offset_t key_len = 0;
  ant_offset_t key_off = vstr(js, key, &key_len);
  const char *key_ptr = (const char *)(uintptr_t)(key_off);

  if (is_internal_prop(key_ptr, key_len)) goto done;
  if (lkp(js, seen, key_ptr, key_len) != 0) goto done;

  ant_value_t mark = setprop_cstr(js, seen, key_ptr, key_len, js_true);
  if (is_err(mark)) {
    GC_ROOT_RESTORE(js, root_mark);
    return mark;
  }

  if (vtype(out) == T_ARR) js_arr_push(js, out, key);
done:
  GC_ROOT_RESTORE(js, root_mark);
  return js_mkundef();
}

static ant_value_t for_in_keys_add_string_indices(ant_t *js, ant_value_t out, ant_value_t seen, ant_value_t str) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, out);
  GC_ROOT_PIN(js, seen);
  GC_ROOT_PIN(js, str);
  
  ant_offset_t slen = vstrlen(js, str);
  for (ant_offset_t i = 0; i < slen; i++) {
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

static inline ant_value_t for_in_keys_collect_chain(
  ant_t *js, ant_value_t out, ant_value_t seen, ant_value_t obj
) {
  ant_value_t cur = obj;
  GC_ROOT_PIN(js, cur);

  for (int depth = 0; is_object_type(cur) && depth < MAX_PROTO_CHAIN_DEPTH; depth++) {
    GC_ROOT_SAVE(iter_mark, js);
    ant_value_t as_cur = (vtype(cur) == T_FUNC) ? js_func_obj(cur) : cur;
    ant_object_t *cur_ptr = js_obj_ptr(as_cur);
    ant_value_t key, r, proto;
    ant_offset_t doff, dense_len, klen;
    bool is_arr;

    if (!cur_ptr) goto next_proto;
    if (!cur_ptr->is_exotic || !cur_ptr->exotic_keys) goto shape_props;

    {
      ant_value_t ekeys = cur_ptr->exotic_keys(js, as_cur);
      GC_ROOT_PIN(js, ekeys);
      if (vtype(ekeys) != T_ARR) goto next_proto;
      ant_offset_t elen = js_arr_len(js, ekeys);
      for (ant_offset_t i = 0; i < elen; i++) {
        key = js_arr_get(js, ekeys, i);
        GC_ROOT_PIN(js, key);
        r = for_in_keys_add(js, out, seen, key);
        if (is_err(r)) goto err;
      }
    }
    goto next_proto;

shape_props:
    if (!cur_ptr->shape) goto next_proto;
    is_arr = (vtype(as_cur) == T_ARR);
    if (!is_arr) goto shape_iter;

    doff = get_dense_buf(as_cur);
    dense_len = doff ? dense_iterable_length(js, as_cur) : 0;
    for (ant_offset_t i = 0; i < dense_len; i++) {
      if (is_empty_slot(dense_get(doff, i))) continue;
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
      key = js_mkstr(js, idxstr, idxlen);
      GC_ROOT_PIN(js, key);
      r = for_in_keys_add(js, out, seen, key);
      if (is_err(r)) goto err;
    }

shape_iter:
    for (uint32_t i = 0; i < ant_shape_count(cur_ptr->shape); i++) {
      const ant_shape_prop_t *prop = ant_shape_prop_at(cur_ptr->shape, i);
      if (!prop || prop->type == ANT_SHAPE_KEY_SYMBOL) continue;
      if (i >= cur_ptr->prop_count) continue;

      const char *kstr = prop->key.interned;
      klen = (ant_offset_t)strlen(kstr);
      if (is_internal_prop(kstr, klen)) continue;

      if (is_arr && is_array_index(kstr, klen)) {
      doff = get_dense_buf(as_cur);
      if (doff) {
        unsigned long pidx = 0;
        for (ant_offset_t ci = 0; ci < klen; ci++) pidx = pidx * 10 + (kstr[ci] - '0');
        if (pidx < (unsigned long)dense_iterable_length(js, as_cur)) continue;
      }}

      bool enumerable = (ant_shape_get_attrs(cur_ptr->shape, i) & ANT_PROP_ATTR_ENUMERABLE) != 0;
      if (cur_ptr->is_exotic) {
        descriptor_entry_t *desc = lookup_descriptor(js_as_obj(as_cur), kstr, (size_t)klen);
        if (desc) enumerable = desc->enumerable;
      }

      key = js_mkstr(js, kstr, (size_t)klen);
      GC_ROOT_PIN(js, key);
      r = for_in_keys_add(js, enumerable ? out : js_mkundef(), seen, key);
      if (is_err(r)) goto err;
    }

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
  
  if (t == T_NULL || t == T_UNDEF) goto done;

  ant_value_t seen = mkobj(js, 0);
  GC_ROOT_PIN(js, seen);

  if (t == T_STR) {
    result = for_in_keys_add_string_indices(js, out, seen, obj);
    if (is_err(result)) goto done;
    result = out;
    goto done;
  }

  if (t == T_OBJ) {
    ant_value_t prim = get_slot(obj, SLOT_PRIMITIVE);
    GC_ROOT_PIN(js, prim);
    if (vtype(prim) == T_STR) {
      result = for_in_keys_add_string_indices(js, out, seen, prim);
      if (is_err(result)) goto done;
      result = out;
    }
  }

  if (t != T_OBJ && t != T_ARR && t != T_FUNC) goto done;
  result = for_in_keys_collect_chain(js, out, seen, obj);

done:
  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static ant_value_t builtin_object_values(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return mkarr(js);
  ant_value_t obj = args[0];
  if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR && vtype(obj) != T_FUNC) return mkarr(js);
  return object_enum(js, obj, OBJ_ENUM_VALUES);
}

static ant_value_t builtin_object_entries(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return mkarr(js);
  ant_value_t obj = args[0];
  if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR && vtype(obj) != T_FUNC) return mkarr(js);
  return object_enum(js, obj, OBJ_ENUM_ENTRIES);
}

static ant_value_t builtin_object_getPrototypeOf(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return js_mkerr(js, "Object.getPrototypeOf requires an argument");
  ant_value_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t == T_STR || t == T_NUM || t == T_BOOL || t == T_BIGINT) return get_prototype_for_type(js, t);
  if (is_object_type(obj)) return get_proto(js, obj);
  
  return js_mknull();
}

static ant_value_t builtin_object_setPrototypeOf(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "Object.setPrototypeOf requires 2 arguments");
  
  ant_value_t obj = args[0];
  ant_value_t proto = args[1];
  
  uint8_t t = vtype(obj);
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) {
    return js_mkerr(js, "Object.setPrototypeOf: first argument must be an object");
  }
  
  uint8_t pt = vtype(proto);
  if (pt != T_OBJ && pt != T_ARR && pt != T_FUNC && pt != T_NULL) {
    return js_mkerr(js, "Object.setPrototypeOf: prototype must be an object or null");
  }
  
  if (pt != T_NULL && proto_chain_contains(js, proto, obj))
    return js_mkerr(js, "Cyclic __proto__ value");
  
  set_proto(js, obj, proto);
  return obj;
}

static ant_value_t builtin_proto_getter(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js->this_val;
  uint8_t t = vtype(this_val);
  
  if (t == T_UNDEF || t == T_NULL) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot read property '__proto__' of %s", typestr(t));
  }
  
  if (t == T_OBJ || t == T_ARR || t == T_FUNC) {
    return get_proto(js, this_val);
  }
  
  return get_prototype_for_type(js, t);
}

static ant_value_t builtin_proto_setter(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_val = js->this_val;
  uint8_t t = vtype(this_val);
  
  if (t == T_UNDEF || t == T_NULL) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot set property '__proto__' of %s", typestr(t));
  }
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) {
    return js_mkundef();
  }
  
  if (nargs == 0) return js_mkundef();
  
  ant_value_t proto = args[0];
  uint8_t pt = vtype(proto);
  
  if (pt != T_OBJ && pt != T_ARR && pt != T_FUNC && pt != T_NULL) {
    return js_mkundef();
  }
  
  if (pt != T_NULL && proto_chain_contains(js, proto, this_val))
    return js_mkundef();
  
  set_proto(js, this_val, proto);
  return js_mkundef();
}

static ant_value_t builtin_object_create(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return js_mkerr(js, "Object.create requires a prototype argument");
  
  ant_value_t proto = args[0];
  uint8_t pt = vtype(proto);
  
  if (pt != T_OBJ && pt != T_ARR && pt != T_FUNC && pt != T_NULL) {
    return js_mkerr(js, "Object.create: prototype must be an object or null");
  }
  
  ant_value_t obj = js_mkobj(js);
  if (pt == T_NULL) {
    js_set_proto_init(obj, js_mknull());
  } else js_set_proto_init(obj, proto);

  if (nargs >= 2 && vtype(args[1]) == T_OBJ) {
    ant_value_t props = args[1];
    ant_iter_t iter = js_prop_iter_begin(js, props);
    
    const char *key = NULL;
    size_t klen = 0;
    ant_value_t descriptor = js_mkundef();

    while (js_prop_iter_next(&iter, &key, &klen, &descriptor)) {
      if (vtype(descriptor) != T_OBJ) continue;
      ant_offset_t val_off = lkp(js, descriptor, "value", 5);
      if (val_off == 0) continue;
      ant_value_t val = propref_load(js, val_off);
      ant_value_t key_str = js_mkstr(js, key, klen);
      js_setprop(js, obj, key_str, val);
    }
    
    js_prop_iter_end(&iter);
  }

  return obj;
}

static ant_value_t builtin_object_hasOwn(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return mkval(T_BOOL, 0);
  
  ant_value_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t == T_NULL || t == T_UNDEF) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert undefined or null to object");
  }
  
  ant_value_t key = args[1];
  if (vtype(key) != T_STR) {
    key = js_tostring_val(js, key);
    if (is_err(key)) return key;
  }
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return mkval(T_BOOL, 0);
  ant_value_t as_obj = js_as_obj(obj);
  if (is_proxy(as_obj)) return proxy_has_own(js, as_obj, key);
  
  ant_offset_t key_len, key_off = vstr(js, key, &key_len);
  const char *key_str = (char *)(uintptr_t)(key_off);
  
  ant_offset_t off = lkp(js, as_obj, key_str, key_len);
  return mkval(T_BOOL, off != 0 ? 1 : 0);
}

static ant_value_t builtin_object_groupBy(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr_typed(js, JS_ERR_TYPE, "Object.groupBy requires 2 arguments");
  
  ant_value_t items = args[0];
  ant_value_t callback = args[1];
  
  if (vtype(callback) != T_FUNC && vtype(callback) != T_CFUNC)
    return js_mkerr_typed(js, JS_ERR_TYPE, "callback is not a function");
  
  ant_value_t result = js_mkobj(js);
  js_set_proto_init(result, js_mknull());

  ant_offset_t len = get_array_length(js, items);
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t val = arr_get(js, items, i);
    ant_value_t cb_args[2] = { val, tov((double)i) };
    ant_value_t key = sv_vm_call(js->vm, js, callback, js_mkundef(), cb_args, 2, NULL, false);
    if (is_err(key)) return key;
    
    ant_value_t key_str = js_tostring_val(js, key);
    if (is_err(key_str)) return key_str;
    
    ant_offset_t klen;
    ant_offset_t koff = vstr(js, key_str, &klen);
    const char *kptr = (char *)(uintptr_t)(koff);
    
    ant_offset_t grp_off = lkp(js, result, kptr, klen);
    ant_value_t group;
    if (grp_off) {
      group = propref_load(js, grp_off);
    } else {
      group = mkarr(js);
      js_setprop(js, result, key_str, group);
    }
    js_arr_push(js, group, val);
  }
  
  return result;
}

// TODO: decompose this huge function into small pieces
static ant_value_t builtin_object_defineProperty(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 3) return js_mkerr(js, "Object.defineProperty requires 3 arguments");
  
  ant_value_t obj = args[0];
  ant_value_t prop = args[1];
  ant_value_t descriptor = args[2];
  uint8_t t = vtype(obj);
  
  if (t == T_CFUNC) {
    ant_value_t fn_obj = mkobj(js, 0);
    set_slot(fn_obj, SLOT_CFUNC, obj);
    obj = js_obj_to_func(fn_obj);
    args[0] = obj;
    t = T_FUNC;
  }
  
  if (!is_object_type(obj)) {
    return js_mkerr(js, "Object.defineProperty called on non-object");
  }
  
  bool sym_key = (vtype(prop) == T_SYMBOL);
  if (!sym_key && vtype(prop) != T_STR) {
    char buf[64];
    size_t len = tostr(js, prop, buf, sizeof(buf));
    prop = js_mkstr(js, buf, len);
  }
  
  if (vtype(descriptor) != T_OBJ) {
    return js_mkerr(js, "Property descriptor must be an object");
  }
  
  ant_value_t as_obj = js_as_obj(obj);
  
  ant_offset_t prop_len = 0;
  const char *prop_str = NULL;
  ant_offset_t sym_off = 0;
  
  if (sym_key) {
    sym_off = (ant_offset_t)vdata(prop);
    const char *desc = js_sym_desc(js, prop);
    prop_str = desc ? desc : "symbol";
    prop_len = (ant_offset_t)strlen(prop_str);
  } else {
    ant_offset_t prop_off = vstr(js, prop, &prop_len);
    prop_str = (char *)(uintptr_t)(prop_off);
    if (streq(prop_str, prop_len, STR_PROTO, STR_PROTO_LEN)) {
      return js_mkerr(js, "Cannot define " STR_PROTO " property");
    }
  }
  
  bool has_value = false, has_get = false, has_set = false;
  bool has_writable = false, has_enumerable = false, has_configurable = false;
  ant_value_t value = js_mkundef();
  bool writable = false, enumerable = false, configurable = false;
  
  ant_offset_t value_off = lkp(js, descriptor, "value", 5);
  if (value_off != 0) {
    has_value = true;
    value = propref_load(js, value_off);
  }
  
  ant_offset_t get_off = lkp_interned(js, descriptor, INTERN_GET, 3);
  if (get_off != 0) {
    has_get = true;
    ant_value_t getter = propref_load(js, get_off);
    if (vtype(getter) != T_FUNC && vtype(getter) != T_UNDEF) {
      return js_mkerr(js, "Getter must be a function");
    }
  }
  
  ant_offset_t set_off = lkp_interned(js, descriptor, INTERN_SET, 3);
  if (set_off != 0) {
    has_set = true;
    ant_value_t setter = propref_load(js, set_off);
    if (vtype(setter) != T_FUNC && vtype(setter) != T_UNDEF) {
      return js_mkerr(js, "Setter must be a function");
    }
  }
  
  ant_offset_t writable_off = lkp(js, descriptor, "writable", 8);
  if (writable_off != 0) {
    has_writable = true;
    ant_value_t w_val = propref_load(js, writable_off);
    writable = js_truthy(js, w_val);
  }
  
  ant_offset_t enumerable_off = lkp(js, descriptor, "enumerable", 10);
  if (enumerable_off != 0) {
    has_enumerable = true;
    ant_value_t e_val = propref_load(js, enumerable_off);
    enumerable = js_truthy(js, e_val);
  }
  
  ant_offset_t configurable_off = lkp(js, descriptor, "configurable", 12);
  if (configurable_off != 0) {
    has_configurable = true;
    ant_value_t c_val = propref_load(js, configurable_off);
    configurable = js_truthy(js, c_val);
  }

  if ((has_value || has_writable) && (has_get || has_set)) {
    return js_mkerr(js, "Invalid property descriptor. Cannot both specify accessors and a value or writable attribute");
  }

  ant_object_t *arr_ptr = (!sym_key && is_length_key(prop_str, prop_len))
    ? array_obj_ptr(as_obj)
    : NULL;
  if (arr_ptr) {
    if (has_get || has_set) return js_mkerr(js, "Invalid property descriptor. Cannot use accessors for array length");
    if ((has_enumerable && enumerable) || (has_configurable && configurable))
      return js_mkerr(js, "Cannot redefine array length attributes");

    ant_offset_t new_len = (ant_offset_t)arr_ptr->u.array.len;
    if (has_value) {
      ant_value_t len_err = validate_array_length(js, value);
      if (is_err(len_err)) return len_err;
      new_len = (ant_offset_t)tod(value);
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

    array_len_set(js, as_obj, new_len);
    return obj;
  }
  
  ant_offset_t existing_off = sym_key ? lkp_sym(js, as_obj, sym_off) : lkp(js, as_obj, prop_str, prop_len);
  prop_meta_t existing_sym_meta;
  bool has_existing_sym_meta = sym_key && lookup_symbol_prop_meta(as_obj, sym_off, &existing_sym_meta);
  bool has_existing_prop = (existing_off > 0) || has_existing_sym_meta;
  ant_object_t *obj_ptr = js_obj_ptr(as_obj);
  
  if (!has_existing_prop) {
    if (obj_ptr && obj_ptr->frozen)
      return js_mkerr(js, "Cannot define property %.*s, object is not extensible", (int)prop_len, prop_str);
    if (obj_ptr && obj_ptr->sealed)
      return js_mkerr(js, "Cannot define property %.*s, object is not extensible", (int)prop_len, prop_str);
    if (obj_ptr && !obj_ptr->extensible)
      return js_mkerr(js, "Cannot define property %.*s, object is not extensible", (int)prop_len, prop_str);
  }
  
  if (has_existing_sym_meta) {
    if (!has_writable) writable = existing_sym_meta.writable;
    if (!has_enumerable) enumerable = existing_sym_meta.enumerable;
    if (!has_configurable) configurable = existing_sym_meta.configurable;
  } else if (existing_off > 0) {
    ant_prop_ref_t *existing_ref = propref_get(js, existing_off);
    if (existing_ref && existing_ref->obj && existing_ref->obj->shape) {
      const ant_shape_prop_t *existing_prop =
        ant_shape_prop_at(existing_ref->obj->shape, existing_ref->slot);
      if (existing_prop) {
        uint8_t existing_attrs = existing_prop->attrs;
        if (!has_writable) writable = (existing_attrs & ANT_PROP_ATTR_WRITABLE) != 0;
        if (!has_enumerable) enumerable = (existing_attrs & ANT_PROP_ATTR_ENUMERABLE) != 0;
        if (!has_configurable) configurable = (existing_attrs & ANT_PROP_ATTR_CONFIGURABLE) != 0;
      }
    }
  }

  if (has_get || has_set) {
    int desc_flags = 
      (enumerable ? JS_DESC_E : 0) |
      (configurable ? JS_DESC_C : 0);
    
    ant_value_t getter = has_get ? propref_load(js, get_off) : js_mkundef();
    ant_value_t setter = has_set ? propref_load(js, set_off) : js_mkundef();

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

    if (!sym_key) js_set_descriptor(js, as_obj, prop_str, prop_len, desc_flags);
    
    if (existing_off > 0) {
      bool is_frozen = obj_ptr ? obj_ptr->frozen : false;
      bool is_nonconfig = is_nonconfig_prop(js, existing_off) || is_frozen;
      bool is_readonly = is_const_prop(js, existing_off) || is_frozen;
      
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
      
      if (is_nonconfig && is_readonly && has_value)
        return js_mkerr(js, "Cannot assign to read-only property '%.*s'", (int)prop_len, prop_str);
      if (has_value) js_saveval(js, existing_off, value);

      ant_prop_ref_t *ref = propref_get(js, existing_off);
      if (ref && ref->obj) {
        if (!js_obj_ensure_unique_shape(ref->obj)) return js_mkerr(js, "oom");
        if (sym_key) ant_shape_set_attrs_symbol(ref->obj->shape, sym_off, attrs);
        else ant_shape_set_attrs_interned(ref->obj->shape, intern_string(prop_str, prop_len), attrs);
        ant_shape_clear_accessor_slot(ref->obj->shape, ref->slot);
      }
    } else {
      if (!has_value) value = js_mkundef();      
      ant_value_t prop_key = sym_key ? prop : js_mkstr(js, prop_str, prop_len);
      uint8_t prop_attrs = ANT_PROP_ATTR_ENUMERABLE
        | (writable ? ANT_PROP_ATTR_WRITABLE : 0)
        | (configurable ? ANT_PROP_ATTR_CONFIGURABLE : 0);
      mkprop(js, as_obj, prop_key, value, prop_attrs);

      if (obj_ptr && obj_ptr->shape) {
        if (!js_obj_ensure_unique_shape(obj_ptr)) return js_mkerr(js, "oom");
        if (sym_key) {
          ant_shape_set_attrs_symbol(obj_ptr->shape, sym_off, attrs);
          int32_t slot = ant_shape_lookup_symbol(obj_ptr->shape, sym_off);
          if (slot >= 0) ant_shape_clear_accessor_slot(obj_ptr->shape, (uint32_t)slot);
        } else {
          const char *interned = intern_string(prop_str, prop_len);
          if (interned) {
            ant_shape_set_attrs_interned(obj_ptr->shape, interned, attrs);
            int32_t slot = ant_shape_lookup_interned(obj_ptr->shape, interned);
            if (slot >= 0) ant_shape_clear_accessor_slot(obj_ptr->shape, (uint32_t)slot);
          }
        }
      }
    }
  }

  if (!sym_key) array_define_or_set_index(js, as_obj, prop_str, (size_t)prop_len);

  return obj;
}

typedef struct {
  bool thrown_exists;
  ant_value_t thrown_value;
  ant_value_t thrown_stack;
} js_exception_state_t;

static inline js_exception_state_t js_save_exception(ant_t *js) {
  js_exception_state_t saved = {
    .thrown_exists = js->thrown_exists,
    .thrown_value = js_mkundef(),
    .thrown_stack = js_mkundef(),
  };
  if (saved.thrown_exists) {
    saved.thrown_value = js->thrown_value;
    saved.thrown_stack = js->thrown_stack;
  }
  return saved;
}

static inline void js_restore_exception(ant_t *js, const js_exception_state_t *saved) {
  js->thrown_exists = saved->thrown_exists;
  js->thrown_value = saved->thrown_value;
  js->thrown_stack = saved->thrown_stack;
}

ant_value_t js_define_property(ant_t *js, ant_value_t obj, ant_value_t prop, ant_value_t descriptor, bool reflect_mode) {
  js_exception_state_t saved = js_save_exception(js);
  ant_value_t args[3] = { obj, prop, descriptor };
  ant_value_t result = builtin_object_defineProperty(js, args, 3);

  if (!reflect_mode) return result;
  if (is_err(result)) {
    js_restore_exception(js, &saved);
    return js_false;
  }
  return js_true;
}

static ant_value_t builtin_object_defineProperties(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "Object.defineProperties requires 2 arguments");
  
  ant_value_t obj = args[0];
  ant_value_t props = args[1];
  
  uint8_t t = vtype(obj);
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) {
    return js_mkerr(js, "Object.defineProperties called on non-object");
  }
  
  if (vtype(props) != T_OBJ) {
    return js_mkerr(js, "Property descriptors must be an object");
  }
  
  ant_iter_t iter = js_prop_iter_begin(js, props);
  const char *key = NULL;
  size_t key_len = 0;
  ant_value_t descriptor = js_mkundef();

  while (js_prop_iter_next(&iter, &key, &key_len, &descriptor)) {
    if (is_internal_prop(key, key_len)) continue;
    ant_value_t prop_key = js_mkstr(js, key, key_len);
    ant_value_t define_args[3] = { obj, prop_key, descriptor };
    ant_value_t result = builtin_object_defineProperty(js, define_args, 3);
    if (is_err(result)) {
      js_prop_iter_end(&iter);
      return result;
    }
  }
  js_prop_iter_end(&iter);
  
  return obj;
}

static inline bool is_enumerable_prop(
  ant_t *js, ant_value_t source, ant_object_t *source_ptr,
  ant_value_t prop_key, uint32_t slot
) {
  if (vtype(prop_key) == T_STR) {
  size_t klen = 0;
  
  const char *kstr = js_getstr(js, prop_key, &klen);
  if (is_internal_prop(kstr, klen)) return false;
  
  if (!source_ptr || source_ptr->is_exotic) {
    descriptor_entry_t *desc = lookup_descriptor(js_as_obj(source), kstr, klen);
    return !desc || desc->enumerable;
  }}
  
  return (ant_shape_get_attrs(source_ptr->shape, slot) & ANT_PROP_ATTR_ENUMERABLE) != 0;
}

static ant_value_t builtin_object_assign(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return js_mkerr(js, "Object.assign requires at least 1 argument");
  
  ant_value_t target = args[0];
  uint8_t t = vtype(target);
  
  if (t == T_NULL || t == T_UNDEF) {
    return js_mkerr(js, "Cannot convert undefined or null to object");
  }
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) {
    target = js_mkobj(js);
  }
  
  ant_value_t as_obj = js_as_obj(target);
  
  for (int i = 1; i < nargs; i++) {
    ant_value_t source = args[i];
    uint8_t st = vtype(source);
    
    if (st == T_NULL || st == T_UNDEF) continue;
    if (st != T_OBJ && st != T_ARR && st != T_FUNC) continue;
    
    ant_iter_t iter = js_prop_iter_begin(js, source);
    ant_object_t *source_ptr = js_obj_ptr(js_as_obj(source));
    
    ant_value_t prop_key = js_mkundef();
    ant_value_t val = js_mkundef();
    
    while (js_prop_iter_next_val(&iter, &prop_key, &val)) if (
      is_enumerable_prop(js, source, source_ptr, prop_key, (uint32_t)(iter.off - 1))
    ) js_setprop(js, as_obj, prop_key, val);
    
    js_prop_iter_end(&iter);
  }
  
  return target;
}

static ant_value_t builtin_object_freeze(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return js_mkundef();
  
  ant_value_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return obj;
  ant_value_t as_obj = js_as_obj(obj);

  ant_object_t *ptr = js_obj_ptr(as_obj);
  if (!ptr || !ptr->shape) return obj;
  if (!js_obj_ensure_unique_shape(ptr)) return js_mkerr(js, "oom");

  uint32_t count = ant_shape_count(ptr->shape);
  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop) continue;

    uint8_t attrs = ant_shape_get_attrs(ptr->shape, i);
    attrs &= (uint8_t)~ANT_PROP_ATTR_WRITABLE;
    attrs &= (uint8_t)~ANT_PROP_ATTR_CONFIGURABLE;

    if (prop->type == ANT_SHAPE_KEY_STRING) {
      const char *key = prop->key.interned;
      size_t klen = strlen(key);
      if (is_internal_prop(key, klen)) continue;
      ant_shape_set_attrs_interned(ptr->shape, key, attrs);
    } else {
      ant_shape_set_attrs_symbol(ptr->shape, prop->key.sym_off, attrs);
    }
  }
  
  ptr->frozen = 1;
  return obj;
}

static ant_value_t builtin_object_isFrozen(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return js_true;
  
  ant_value_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return js_true;
  ant_value_t as_obj = js_as_obj(obj);
  
  ant_object_t *ptr = js_obj_ptr(as_obj);
  return js_bool(ptr && ptr->frozen);
}

static ant_value_t builtin_object_seal(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return js_mkundef();
  
  ant_value_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return obj;
  ant_value_t as_obj = js_as_obj(obj);
  
  ant_object_t *ptr = js_obj_ptr(as_obj);
  if (!ptr || !ptr->shape) return obj;
  if (!js_obj_ensure_unique_shape(ptr)) return js_mkerr(js, "oom");
  ptr->sealed = 1;

  uint32_t count = ant_shape_count(ptr->shape);
  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop) continue;

    uint8_t attrs = ant_shape_get_attrs(ptr->shape, i);
    attrs &= (uint8_t)~ANT_PROP_ATTR_CONFIGURABLE;

    if (prop->type == ANT_SHAPE_KEY_STRING) {
      const char *key = prop->key.interned;
      size_t klen = strlen(key);
      if (is_internal_prop(key, klen)) continue;
      ant_shape_set_attrs_interned(ptr->shape, key, attrs);
    } else {
      ant_shape_set_attrs_symbol(ptr->shape, prop->key.sym_off, attrs);
    }
  }
  
  return obj;
}

static ant_value_t builtin_object_isSealed(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return js_true;
  
  ant_value_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return js_true;
  ant_value_t as_obj = js_as_obj(obj);
  
  ant_object_t *ptr = js_obj_ptr(as_obj);
  if (ptr && (ptr->sealed || ptr->frozen)) return js_true;
  
  return js_false;
}

static ant_value_t builtin_object_fromEntries(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return js_mkerr(js, "Object.fromEntries requires an iterable argument");
  
  ant_value_t iterable = args[0];
  uint8_t t = vtype(iterable);
  
  if (t != T_ARR && t != T_OBJ) {
    return js_mkerr(js, "Object.fromEntries requires an iterable");
  }
  
  ant_value_t result = js_mkobj(js);
  ant_offset_t len = get_array_length(js, iterable);
  if (len == 0) return result;
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t entry = arr_get(js, iterable, i);
    if (vtype(entry) != T_ARR && vtype(entry) != T_OBJ) continue;
    
    ant_value_t key = arr_get(js, entry, 0);
    if (is_undefined(key)) continue;
    ant_value_t val = arr_get(js, entry, 1);
    
    if (vtype(key) != T_STR) {
      char buf[64];
      size_t n = tostr(js, key, buf, sizeof(buf));
      key = js_mkstr(js, buf, n);
    }
    
    js_setprop(js, result, key, val);
  }
  
  return result;
}

static ant_value_t builtin_object_getOwnPropertyDescriptor(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  
  ant_value_t obj = args[0];
  ant_value_t key = args[1];
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return js_mkundef();
  
  const char *key_str;
  ant_offset_t key_len;
  
  bool is_sym = (vtype(key) == T_SYMBOL);
  if (is_sym) {
    const char *d = js_sym_desc(js, key);
    key_str = d ? d : "symbol";
    key_len = (ant_offset_t)strlen(key_str);
  } else if (vtype(key) == T_STR) {
    ant_offset_t key_off = vstr(js, key, &key_len);
    key_str = (char *)(uintptr_t)(key_off);
  } else {
    char buf[64];
    size_t n = tostr(js, key, buf, sizeof(buf));
    key = js_mkstr(js, buf, n);
    ant_offset_t key_off = vstr(js, key, &key_len);
    key_str = (char *)(uintptr_t)(key_off);
  }
  
  ant_value_t as_obj = js_as_obj(obj);
  if (is_proxy(as_obj)) {
    return proxy_get_own_property_descriptor(js, as_obj, key);
  }

  ant_offset_t sym_off = is_sym ? (ant_offset_t)vdata(key) : 0;
  prop_meta_t sym_meta; prop_meta_t str_meta;
  
  bool has_sym_meta = is_sym ? lookup_symbol_prop_meta(as_obj, sym_off, &sym_meta) : false;
  bool has_str_meta = is_sym ? false : lookup_string_prop_meta(js, as_obj, key_str, (size_t)key_len, &str_meta);

  ant_offset_t prop_off = is_sym ? lkp_sym(js, as_obj, sym_off) : lkp(js, as_obj, key_str, key_len);
  if (prop_off == 0 && !(is_sym ? has_sym_meta : has_str_meta)) {
    return js_mkundef();
  }

  bool has_getter = false;
  bool has_setter = false;
  
  ant_value_t result = js_mkobj(js);
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

  if (has_getter || has_setter) {
    if (has_getter) js_setprop(js, result, js_mkstr(js, "get", 3), getter);
    if (has_setter) js_setprop(js, result, js_mkstr(js, "set", 3), setter);
    js_setprop(js, result, js_mkstr(js, "enumerable", 10), js_bool(enumerable));
    js_setprop(js, result, js_mkstr(js, "configurable", 12), js_bool(configurable));
  } else {
    ant_value_t prop_val = js_mkundef();
    bool has_value_out = false;
    if (prop_off != 0) {
      prop_val = propref_load(js, prop_off);
      has_value_out = true;
    } else if (!is_sym && is_length_key(key_str, key_len) && array_obj_ptr(as_obj)) {
      prop_val = tov((double)get_array_length(js, as_obj));
      has_value_out = true;
    }
    if (has_value_out) js_setprop(js, result, js_mkstr(js, "value", 5), prop_val);
    js_setprop(js, result, js_mkstr(js, "writable", 8), js_bool(writable));
    js_setprop(js, result, js_mkstr(js, "enumerable", 10), js_bool(enumerable));
    js_setprop(js, result, js_mkstr(js, "configurable", 12), js_bool(configurable));
  }
  
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

static ant_value_t builtin_object_getOwnPropertyNames(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return mkarr(js);
  ant_value_t obj = args[0];
  
  if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR && vtype(obj) != T_FUNC) return mkarr(js);
  if (vtype(obj) == T_FUNC) obj = js_func_obj(obj);
  
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || !ptr->shape) return mkarr(js);
  bool is_arr_obj = (vtype(obj) == T_ARR);
  
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
    ant_offset_t klen = (ant_offset_t)strlen(key);

    if (is_internal_prop(key, klen)) continue;
    if (is_arr_obj && own_prop_names_is_dense_shadow(js, obj, key, klen)) continue;
    arr_set(js, arr, idx++, js_mkstr(js, key, (size_t)klen));
  }
  
  if (is_arr_obj) arr_set(js, arr, idx++, js->length_str);
  return mkval(T_ARR, vdata(arr));
}

static ant_value_t builtin_object_getOwnPropertySymbols(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return mkarr(js);
  ant_value_t obj = args[0];
  
  uint8_t t = vtype(obj);
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return mkarr(js);
  if (t == T_FUNC) obj = js_func_obj(obj);

  ant_object_t *ptr = js_obj_ptr(obj);
  ant_value_t arr = mkarr(js);
  ant_offset_t idx = 0;
  if (!ptr || !ptr->shape) return mkval(T_ARR, vdata(arr));

  uint32_t count = ant_shape_count(ptr->shape);
  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_SYMBOL) continue;
    arr_set(js, arr, idx++, mkval(T_SYMBOL, prop->key.sym_off));
  }

  return mkval(T_ARR, vdata(arr));
}

static ant_value_t builtin_object_isExtensible(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return js_true;
  
  ant_value_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return js_true;
  ant_value_t as_obj = js_as_obj(obj);
  
  ant_object_t *ptr = js_obj_ptr(as_obj);
  if (!ptr) return js_true;
  if (ptr->frozen || ptr->sealed) return js_false;
  return js_bool(ptr->extensible);
}

static ant_value_t builtin_object_preventExtensions(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return js_mkundef();
  
  ant_value_t obj = args[0];
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return obj;
  ant_value_t as_obj = js_as_obj(obj);
  
  ant_object_t *ptr = js_obj_ptr(as_obj);
  if (ptr) ptr->extensible = 0;
  return obj;
}

static ant_value_t builtin_object_hasOwnProperty(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return mkval(T_BOOL, 0);
  
  ant_value_t obj = js->this_val;
  ant_value_t key = args[0];
  
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return mkval(T_BOOL, 0);
  ant_value_t as_obj = js_as_obj(obj);
  
  if (is_proxy(as_obj)) return proxy_has_own(js, as_obj, key);
  bool is_arr_obj = array_obj_ptr(as_obj) != NULL;

  if (vtype(key) == T_SYMBOL) {
    ant_offset_t sym_off = (ant_offset_t)vdata(key);
    ant_offset_t off = lkp_sym(js, as_obj, sym_off);
    if (off != 0) return mkval(T_BOOL, 1);
    prop_meta_t meta;
    return mkval(T_BOOL, lookup_symbol_prop_meta(as_obj, sym_off, &meta) ? 1 : 0);
  }

  const char *key_str = NULL;
  ant_offset_t key_len = 0;
  if (vtype(key) != T_STR) {
    char buf[64];
    size_t n = tostr(js, key, buf, sizeof(buf));
    key = js_mkstr(js, buf, n);
  }
  ant_offset_t key_off = vstr(js, key, &key_len);
  key_str = (char *)(uintptr_t)(key_off);

  if (is_arr_obj && is_length_key(key_str, key_len)) return mkval(T_BOOL, 1);
  if (is_arr_obj && is_array_index(key_str, key_len)) {
    unsigned long idx;
    if (parse_array_index(key_str, key_len, get_array_length(js, as_obj), &idx)) {
      return mkval(T_BOOL, arr_has(js, as_obj, (ant_offset_t)idx) ? 1 : 0);
    }
  }

  ant_offset_t off = lkp(js, as_obj, key_str, key_len);
  if (off != 0) return mkval(T_BOOL, 1);
  ant_object_t *ptr = js_obj_ptr(as_obj);
  if (ptr && ptr->is_exotic) {
    descriptor_entry_t *desc = lookup_descriptor(as_obj, key_str, key_len);
    return mkval(T_BOOL, (desc && (desc->has_getter || desc->has_setter)) ? 1 : 0);
  }
  return mkval(T_BOOL, 0);
}

static bool proto_chain_contains_cycle_safe(ant_t *js, ant_value_t start, ant_value_t target);

bool js_is_prototype_of(ant_t *js, ant_value_t proto_obj, ant_value_t obj) {
  uint8_t obj_type = vtype(obj);
  if (obj_type != T_OBJ && obj_type != T_ARR && obj_type != T_FUNC) return false;
  uint8_t proto_type = vtype(proto_obj);
  if (proto_type != T_OBJ && proto_type != T_ARR && proto_type != T_FUNC) return false;
  ant_value_t current = get_proto(js, obj);
  return proto_chain_contains_cycle_safe(js, current, proto_obj);
}

ant_value_t builtin_object_isPrototypeOf(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return mkval(T_BOOL, 0);
  return mkval(T_BOOL, js_is_prototype_of(js, js->this_val, args[0]) ? 1 : 0);
}

static ant_value_t builtin_object_propertyIsEnumerable(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return mkval(T_BOOL, 0);
  
  ant_value_t obj = js->this_val;
  ant_value_t key = args[0];
  
  uint8_t t = vtype(obj);
  
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return mkval(T_BOOL, 0);
  ant_value_t as_obj = js_as_obj(obj);
  bool is_arr_obj = array_obj_ptr(as_obj) != NULL;

  if (vtype(key) == T_SYMBOL) {
    ant_offset_t sym_off = (ant_offset_t)vdata(key);
    ant_offset_t off = lkp_sym(js, as_obj, sym_off);
    if (off == 0) return mkval(T_BOOL, 0);
    prop_meta_t meta;
    if (lookup_symbol_prop_meta(as_obj, sym_off, &meta))
      return mkval(T_BOOL, meta.enumerable ? 1 : 0);
    return mkval(T_BOOL, 1);
  }

  const char *key_str = NULL;
  ant_offset_t key_len = 0;
  if (vtype(key) != T_STR) {
    char buf[64];
    size_t n = tostr(js, key, buf, sizeof(buf));
    key = js_mkstr(js, buf, n);
  }
  ant_offset_t key_off = vstr(js, key, &key_len);
  key_str = (char *)(uintptr_t)(key_off);

  if (is_arr_obj && is_length_key(key_str, key_len)) {
    return mkval(T_BOOL, 0);
  }
  
  if (is_arr_obj) {
    unsigned long idx;
    if (parse_array_index(key_str, key_len, get_array_length(js, as_obj), &idx)) {
      return mkval(T_BOOL, arr_has(js, as_obj, (ant_offset_t)idx) ? 1 : 0);
    }
  }
  
  ant_offset_t off = lkp(js, as_obj, key_str, key_len);
  if (off == 0) return mkval(T_BOOL, 0);

  const ant_shape_prop_t *prop_meta = prop_shape_meta(js, off);
  if (prop_meta && !js_obj_ptr(as_obj)->is_exotic) {
    bool enumerable = (prop_meta->attrs & ANT_PROP_ATTR_ENUMERABLE) != 0;
    return mkval(T_BOOL, enumerable ? 1 : 0);
  }

  ant_object_t *ptr = js_obj_ptr(as_obj);
  if (ptr && ptr->is_exotic) {
    prop_meta_t meta;
    if (lookup_string_prop_meta(js, as_obj, key_str, (size_t)key_len, &meta))
      return mkval(T_BOOL, meta.enumerable ? 1 : 0);
  }
  return mkval(T_BOOL, 1);
}

static ant_value_t builtin_object_toString(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t obj = js->this_val;
  
  uint8_t t = vtype(obj);
  
  const char *tag = NULL;
  ant_offset_t tag_len = 0;

  ant_value_t tag_sym = get_toStringTag_sym();
  if (vtype(tag_sym) == T_SYMBOL) {
    ant_offset_t sym_off = (ant_offset_t)vdata(tag_sym);
    ant_offset_t tag_off = 0;
    if (is_object_type(obj)) {
      tag_off = lkp_sym_proto(js, obj, sym_off);
    } else {
      ant_value_t proto = get_prototype_for_type(js, t);
      if (is_object_type(proto)) {
        tag_off = lkp_sym_proto(js, proto, sym_off);
      }
    }
    if (tag_off != 0) {
      ant_value_t tag_val = propref_load(js, tag_off);
      if (vtype(tag_val) == T_STR) {
        ant_offset_t str_off = vstr(js, tag_val, &tag_len);
        tag = (const char *)(uintptr_t)(str_off);
      }
    }
  }
  
  if (!tag) {
  if (is_object_type(obj) && get_slot(obj, SLOT_ERROR_BRAND) == js_true) {
    tag = "Error"; tag_len = 5;
  } else switch (t) {
    case T_UNDEF:   tag = "Undefined"; tag_len = 9; break;
    case T_NULL:    tag = "Null";      tag_len = 4; break;
    case T_BOOL:    tag = "Boolean";   tag_len = 7; break;
    case T_NUM:     tag = "Number";    tag_len = 6; break;
    case T_STR:     tag = "String";    tag_len = 6; break;
    case T_ARR:     tag = "Array";     tag_len = 5; break;
    case T_FUNC:    tag = "Function";  tag_len = 8; break;
    case T_CFUNC:   tag = "Function";  tag_len = 8; break;
    case T_ERR:     tag = "Error";     tag_len = 5; break;
    case T_BIGINT:  tag = "BigInt";    tag_len = 6; break;
    case T_PROMISE: tag = "Promise";   tag_len = 7; break;
    case T_OBJ:     tag = "Object";    tag_len = 6; break;
    default:        tag = "Unknown";   tag_len = 7; break;
  }}

  char static_buf[64];
  string_builder_t sb;
  
  string_builder_init(&sb, static_buf, sizeof(static_buf));
  string_builder_append(&sb, "[object ", 8);
  string_builder_append(&sb, tag, tag_len);
  string_builder_append(&sb, "]", 1);
  
  return string_builder_finalize(js, &sb);
}

static ant_value_t builtin_object_valueOf(ant_t *js, ant_value_t *args, int nargs) {
  return js->this_val;
}

static ant_value_t builtin_object_toLocaleString(ant_t *js, ant_value_t *args, int nargs) {
  return js_call_toString(js, js->this_val);
}

static inline ant_value_t require_callback(ant_t *js, ant_value_t *args, int nargs, const char *name) {
  if (nargs == 0 || !is_callable(args[0]))
    return js_mkerr(js, "%s requires a function argument", name);
  return args[0];
}

static ant_value_t array_shallow_copy(ant_t *js, ant_value_t arr, ant_offset_t len) {
  ant_value_t result = mkarr(js);
  if (is_err(result)) return result;
  
  ant_offset_t doff = get_dense_buf(arr);
  if (doff) {
    for (ant_offset_t i = 0; i < len; i++) {
      ant_value_t v = dense_get(doff, i);
      arr_set(js, result, i, v);
    }
    return result;
  }
  
  ant_iter_t iter = js_prop_iter_begin(js, arr);
  const char *key;
  size_t key_len;
  ant_value_t val;
  
  while (js_prop_iter_next(&iter, &key, &key_len, &val)) {
    if (key_len == 0 || key[0] > '9' || key[0] < '0') continue;
    js_mkprop_fast(js, result, key, key_len, val);
  }
  
  js_prop_iter_end(&iter);
  array_len_set(js, result, len);
  return result;
}

static ant_value_t builtin_array_push(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "push called on non-array");
  }

  if (is_proxy(arr)) {
    ant_offset_t off = lkp_interned(js, arr, INTERN_LENGTH, 6);
    ant_offset_t len = 0;
    if (off != 0) {
      ant_value_t len_val = propref_load(js, off);
      if (vtype(len_val) == T_NUM) len = (ant_offset_t) tod(len_val);
    }
    for (int i = 0; i < nargs; i++) {
      char idxstr[16];
      size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)len);
      ant_value_t key = js_mkstr(js, idxstr, idxlen);
      js_setprop(js, arr, key, args[i]);
      len++;
    }
    
    ant_value_t len_val = tov((double) len);
    js_setprop(js, arr, js->length_str, len_val);
    return len_val;
  }

  ant_offset_t len = get_array_length(js, arr);
  
  ant_offset_t doff = get_dense_buf(arr);
  if (doff) {
    for (int i = 0; i < nargs; i++) {
      ant_offset_t cap = dense_capacity(doff);
      if (len >= cap) {
        doff = dense_grow(js, arr, len + 1);
        if (doff == 0) return js_mkerr(js, "oom");
      }
      dense_set(js, doff, len, args[i]);
      len++;
    }
    array_len_set(js, arr, len);
    return tov((double) len);
  }
  
  for (int i = 0; i < nargs; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)len);
    js_mkprop_fast(js, arr, idxstr, idxlen, args[i]); len++;
  }

  ant_value_t new_len = tov((double) len);
  array_len_set(js, arr, len);

  return new_len;
}

void js_arr_push(ant_t *js, ant_value_t arr, ant_value_t val) {
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) return;
  
  ant_offset_t doff = get_dense_buf(arr);
  if (doff) {
    ant_offset_t len = get_array_length(js, arr);
    ant_offset_t cap = dense_capacity(doff);
    if (len >= cap) {
      doff = dense_grow(js, arr, len + 1);
      if (doff == 0) return;
    }
    dense_set(js, doff, len, val);
    array_len_set(js, arr, len + 1);
    return;
  }
  
  ant_offset_t len = get_array_length(js, arr);
  
  char idxstr[16];
  size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)len);
  js_mkprop_fast(js, arr, idxstr, idxlen, val);
  array_len_set(js, arr, len + 1);
}

static ant_value_t builtin_array_pop(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;

  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "pop called on non-array");
  }

  if (is_proxy(arr)) {
    ant_offset_t len = proxy_aware_length(js, arr);
    if (len == 0) {
      js_setprop(js, arr, js->length_str, tov(0.0));
      return js_mkundef();
    }
    len--;
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)len);
    ant_value_t result = proxy_aware_get_elem(js, arr, idxstr, idxlen);
    js_setprop(js, arr, js->length_str, tov((double) len));
    return result;
  }

  ant_offset_t doff = get_dense_buf(arr);
  if (doff) {
    ant_offset_t len = get_array_length(js, arr);
    ant_offset_t dense_len = dense_iterable_length(js, arr);
    if (len == 0) return js_mkundef();
    if (len != dense_len) goto pop_slow;
    len--;
    ant_value_t result = (len < dense_len) ? dense_get(doff, len) : js_mkundef();
    if (is_empty_slot(result)) result = js_mkundef();
    if (len < dense_len) {
      dense_set(js, doff, len, T_EMPTY);
    }
    array_len_set(js, arr, len);
    return result;
  }

  pop_slow:
  ant_offset_t len = get_array_length(js, arr);

  if (len == 0) return js_mkundef();
  len--; char idxstr[16];
  size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)len);

  ant_offset_t elem_off = 0;

  if (elem_off == 0) elem_off = lkp(js, arr, idxstr, idxlen);
  ant_value_t result = js_mkundef();
  if (elem_off != 0) result = propref_load(js, elem_off);

  array_len_set(js, arr, len);

  return result;
}

static ant_value_t builtin_array_slice(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "slice called on non-array");
  }
  
  ant_offset_t len = get_array_length(js, arr);
  
  ant_offset_t start = 0, end = len;
  double dlen = D(len);
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    double d = tod(args[0]);
    if (d < 0) {
      start = (ant_offset_t) (d + dlen < 0 ? 0 : d + dlen);
    } else start = (ant_offset_t) (d > dlen ? dlen : d);
  }
  
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    double d = tod(args[1]);
    if (d < 0) {
      end = (ant_offset_t) (d + dlen < 0 ? 0 : d + dlen);
    } else {
      end = (ant_offset_t) (d > dlen ? dlen : d);
    }
  }
  
  if (start > end) start = end;
  ant_value_t result = array_alloc_like(js, arr);
  if (is_err(result)) return result;
  ant_offset_t result_idx = 0;
  
  for (ant_offset_t i = start; i < end; i++) {
    ant_value_t elem = arr_get(js, arr, i);
    arr_set(js, result, result_idx, elem);
    result_idx++;
  }
  
  return result;
}

static ant_value_t builtin_array_join(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "join called on non-array");
  }
  const char *sep = ",";
  ant_offset_t sep_len = 1;
  
  if (nargs >= 1) {
    if (vtype(args[0]) == T_STR) {
      sep_len = 0;
      ant_offset_t sep_off = vstr(js, args[0], &sep_len);
      sep = (const char *)(uintptr_t)(sep_off);
    } else if (vtype(args[0]) != T_UNDEF) {
      const char *sep_str = js_str(js, args[0]);
      sep = sep_str;
      sep_len = (ant_offset_t) strlen(sep_str);
    }
  }
  
  ant_offset_t len = get_array_length(js, arr);
  
  if (len == 0) return js_mkstr(js, "", 0);
  
  size_t capacity = 1024;
  size_t result_len = 0;
  char *result = (char *)ant_calloc(capacity);
  if (!result) return js_mkerr(js, "oom");
  
  for (ant_offset_t i = 0; i < len; i++) {
    if (i > 0) {
      if (result_len + sep_len >= capacity) {
        capacity = (result_len + sep_len + 1) * 2;
        char *new_result = (char *)ant_realloc(result, capacity);
        if (!new_result) return js_mkerr(js, "oom");
        result = new_result;
      }
      memcpy(result + result_len, sep, sep_len);
      result_len += sep_len;
    }
    
    {
      ant_value_t elem = arr_get(js, arr, i);
      uint8_t et = vtype(elem);
      if (et == T_NULL || et == T_UNDEF) continue;
      
      const char *elem_str = NULL;
      size_t elem_len = 0;
      char numstr[64];
      ant_value_t str_val = js_mkundef();
      
      if (et == T_STR) {
        ant_offset_t soff, slen;
        soff = vstr(js, elem, &slen);
        elem_str = (const char *)(uintptr_t)(soff);
        elem_len = slen;
      } else if (et == T_NUM) {
        snprintf(numstr, sizeof(numstr), "%g", tod(elem));
        elem_str = numstr;
        elem_len = strlen(numstr);
      } else if (et == T_BOOL) {
        elem_str = vdata(elem) ? "true" : "false";
        elem_len = strlen(elem_str);
      } else if (et == T_ARR || et == T_OBJ || et == T_FUNC || et == T_BIGINT) {
        str_val = to_string_val(js, elem);
        
        if (is_err(str_val)) {
          free(result);
          return str_val;
        }
        
        if (vtype(str_val) == T_STR) {
          ant_offset_t soff, slen;
          soff = vstr(js, str_val, &slen);
          elem_str = (const char *)(uintptr_t)(soff);
          elem_len = slen;
        }
      }

      
      if (elem_str && elem_len > 0) {
        if (result_len + elem_len >= capacity) {
          capacity = (result_len + elem_len + 1) * 2;
          char *new_result = (char *)ant_realloc(result, capacity);
          if (!new_result) { free(result); return js_mkerr(js, "oom"); }
          result = new_result;
        }
        memcpy(result + result_len, elem_str, elem_len);
        result_len += elem_len;
      }
    }
  }
  
  ant_value_t ret = js_mkstr(js, result, result_len);
  free(result); return ret;
}

static ant_value_t builtin_array_includes(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "includes called on non-array");
  
  if (nargs == 0) return mkval(T_BOOL, 0);
  ant_value_t search = args[0];
  
  ant_offset_t len = get_array_length(js, arr);
  if (len == 0) return mkval(T_BOOL, 0);
  
  ant_offset_t start = 0;
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    int s = (int) tod(args[1]);
    if (s < 0) s = (int)len + s;
    if (s < 0) s = 0;
    start = (ant_offset_t) s;
  }
  
  for (ant_offset_t i = start; i < len; i++) {
    ant_value_t val = arr_get(js, arr, i);
    if (vtype(val) == T_NUM && vtype(search) == T_NUM && isnan(tod(val)) && isnan(tod(search))) return mkval(T_BOOL, 1);
    if (strict_eq_values(js, val, search)) return mkval(T_BOOL, 1);
  }
  
  return mkval(T_BOOL, 0);
}

static ant_value_t builtin_array_every(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "every called on non-array");
  
  ant_value_t callback = require_callback(js, args, nargs, "every");
  if (is_err(callback)) return callback;
  ant_value_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();
  
  ant_offset_t len = get_array_length(js, arr);
  
  for (ant_offset_t i = 0; i < len; i++) {
    if (!arr_has(js, arr, i)) continue;
    ant_value_t val = arr_get(js, arr, i);
    ant_value_t call_args[3] = { val, tov((double)i), arr };
    ant_value_t result = sv_vm_call(js->vm, js, callback, this_arg, call_args, 3, NULL, false);
    if (is_err(result)) return result;
    if (!js_truthy(js, result)) return mkval(T_BOOL, 0);
  }
  
  return mkval(T_BOOL, 1);
}

static ant_value_t builtin_array_forEach(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "forEach called on non-array");
  
  ant_value_t callback = require_callback(js, args, nargs, "forEach");
  if (is_err(callback)) return callback;
  ant_value_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();
  
  ant_offset_t len = get_array_length(js, arr);
  
  for (ant_offset_t i = 0; i < len; i++) {
    if (!arr_has(js, arr, i)) continue;
    ant_value_t val = arr_get(js, arr, i);
    ant_value_t call_args[3] = { val, tov((double)i), arr };
    ant_value_t result = sv_vm_call(js->vm, js, callback, this_arg, call_args, 3, NULL, false);
    if (is_err(result)) return result;
  }
  
  return js_mkundef();
}

static ant_value_t builtin_array_reverse(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t arr = js->this_val;

  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "reverse called on non-array");

  if (is_proxy(arr)) {
    ant_offset_t len = proxy_aware_length(js, arr);
    if (len <= 1) return arr;
    ant_value_t read_from = proxy_read_target(js, arr);
    ant_offset_t lower = 0;
    while (lower < len / 2) {
      ant_offset_t upper_idx = len - lower - 1;
      bool lower_exists = arr_has(js, read_from, lower);
      bool upper_exists = arr_has(js, read_from, upper_idx);
      ant_value_t lower_val = lower_exists ? arr_get(js, read_from, lower) : js_mkundef();
      ant_value_t upper_val = upper_exists ? arr_get(js, read_from, upper_idx) : js_mkundef();
      if (lower_exists && upper_exists) {
        char s1[16]; size_t l1 = uint_to_str(s1, sizeof(s1), (unsigned)lower);
        js_setprop(js, arr, js_mkstr(js, s1, l1), upper_val);
        char s2[16]; size_t l2 = uint_to_str(s2, sizeof(s2), (unsigned)upper_idx);
        js_setprop(js, arr, js_mkstr(js, s2, l2), lower_val);
      } else if (upper_exists) {
        char s[16]; size_t l = uint_to_str(s, sizeof(s), (unsigned)lower);
        js_setprop(js, arr, js_mkstr(js, s, l), upper_val);
      } else if (lower_exists) {
        char s[16]; size_t l = uint_to_str(s, sizeof(s), (unsigned)upper_idx);
        js_setprop(js, arr, js_mkstr(js, s, l), lower_val);
      } lower++;
    } return arr;
  }

  ant_offset_t len = get_array_length(js, arr);
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

static ant_value_t builtin_array_map(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "map called on non-array");
  
  ant_value_t callback = require_callback(js, args, nargs, "map");
  if (is_err(callback)) return callback;
  ant_value_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();
  
  ant_offset_t len = get_array_length(js, arr);
  ant_value_t result = array_alloc_like(js, arr);
  if (is_err(result)) return result;
  
  for (ant_offset_t i = 0; i < len; i++) {
    if (!arr_has(js, arr, i)) continue;
    ant_value_t val = arr_get(js, arr, i);
    ant_value_t call_args[3] = { val, tov((double)i), arr };
    ant_value_t mapped = sv_vm_call(js->vm, js, callback, this_arg, call_args, 3, NULL, false);
    if (is_err(mapped)) return mapped;
    arr_set(js, result, i, mapped);
  }
  
  return result;
}

static ant_value_t builtin_array_filter(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "filter called on non-array");
  
  ant_value_t callback = require_callback(js, args, nargs, "filter");
  if (is_err(callback)) return callback;
  ant_value_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();
  
  ant_offset_t len = get_array_length(js, arr);
  ant_value_t result = array_alloc_like(js, arr);
  if (is_err(result)) return result;
  
  ant_offset_t result_idx = 0;
  
  for (ant_offset_t i = 0; i < len; i++) {
    if (!arr_has(js, arr, i)) continue;
    ant_value_t val = arr_get(js, arr, i);
    ant_value_t call_args[3] = { val, tov((double)i), arr };
    ant_value_t test = sv_vm_call(js->vm, js, callback, this_arg, call_args, 3, NULL, false);
    
    if (is_err(test)) return test;
    if (js_truthy(js, test)) { arr_set(js, result, result_idx, val); result_idx++; }
  }
  
  return result;
}

static ant_value_t builtin_array_reduce(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "reduce called on non-array");
  
  ant_value_t callback = require_callback(js, args, nargs, "reduce");
  if (is_err(callback)) return callback;
  bool has_initial = (nargs >= 2);
  
  ant_offset_t len = get_array_length(js, arr);
  
  ant_value_t accumulator = has_initial ? args[1] : js_mkundef();
  bool first = !has_initial;
  
  for (ant_offset_t i = 0; i < len; i++) {
    if (!arr_has(js, arr, i)) continue;
    ant_value_t val = arr_get(js, arr, i);
    if (first) { accumulator = val; first = false; continue; }
    ant_value_t call_args[4] = { accumulator, val, tov((double)i), arr };
    accumulator = sv_vm_call(js->vm, js, callback, js_mkundef(), call_args, 4, NULL, false);
    if (is_err(accumulator)) return accumulator;
  }
  
  if (first) return js_mkerr(js, "reduce of empty array with no initial value");
  return accumulator;
}

static inline void flat_helper(ant_t *js, ant_value_t arr, ant_value_t result, ant_offset_t *result_idx, int depth) {
  ant_offset_t len = get_array_length(js, arr);
  if (len == 0) return;
  
  for (ant_offset_t i = 0; i < len; i++) {
    if (!arr_has(js, arr, i)) continue;
    ant_value_t val = arr_get(js, arr, i);
    
    if (depth > 0 && vtype(val) == T_ARR) flat_helper(js, val, result, result_idx, depth - 1);
    else { arr_set(js, result, *result_idx, val); (*result_idx)++; }
  }
}

static ant_value_t builtin_array_flat(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "flat called on non-array");
  }
  
  int depth = 1;
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    depth = (int) tod(args[0]);
    if (depth < 0) depth = 0;
  }
  
  ant_value_t result = array_alloc_like(js, arr);
  if (is_err(result)) return result;
  ant_offset_t result_idx = 0;
  
  flat_helper(js, arr, result, &result_idx, depth);
  return result;
}

static ant_value_t builtin_array_concat(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "concat called on non-array");
  }
  
  ant_value_t result = array_alloc_like(js, arr);
  if (is_err(result)) return result;
  
  ant_offset_t result_idx = 0;
  for (int a = -1; a < nargs; a++) {
    ant_value_t arg = (a < 0) ? arr : args[a];
    bool spreadable = false;
    
    if (vtype(arg) == T_ARR || vtype(arg) == T_OBJ) {
      bool array_default_spreadable = (vtype(arg) == T_ARR);
      if (!array_default_spreadable && is_proxy(arg)) {
        ant_value_t target = proxy_read_target(js, arg);
        array_default_spreadable = (vtype(target) == T_ARR);
      }
      
      ant_value_t spread_val = js_get_sym(js, arg, get_isConcatSpreadable_sym());
      if (is_err(spread_val)) return spread_val;
      if (vtype(spread_val) == T_UNDEF) spreadable = array_default_spreadable;
      else spreadable = js_truthy(js, spread_val);
    }
    
    if (spreadable) {
      ant_offset_t arg_len = 0;
      ant_value_t len_val = js_get(js, arg, "length");
      if (is_err(len_val)) return len_val;
      if (vtype(len_val) == T_NUM && tod(len_val) > 0) arg_len = (ant_offset_t)tod(len_val);
      
      for (ant_offset_t i = 0; i < arg_len; i++) {
        char idxstr[32];
        uint_to_str(idxstr, sizeof(idxstr), (uint64_t)i);
        ant_value_t elem = js_get(js, arg, idxstr);
        if (is_err(elem)) return elem;
        arr_set(js, result, result_idx, elem);
        result_idx++;
      }
    } else {
      arr_set(js, result, result_idx, arg);
      result_idx++;
    }
  }
  
  return result;
}

static ant_value_t builtin_array_at(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "at called on non-array");
  }
  
  if (nargs == 0 || vtype(args[0]) != T_NUM) return js_mkundef();
  
  ant_offset_t len = get_array_length(js, arr);
  
  int idx = (int) tod(args[0]);
  if (idx < 0) idx = (int)len + idx;
  if (idx < 0 || (ant_offset_t)idx >= len) return js_mkundef();
  
  return arr_get(js, arr, (ant_offset_t)idx);
}

static ant_value_t builtin_array_fill(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "fill called on non-array");
  }

  ant_value_t value = nargs >= 1 ? args[0] : js_mkundef();

  ant_offset_t len = proxy_aware_length(js, arr);
  
  ant_offset_t start = 0, end = len;
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    int s = (int) tod(args[1]);
    if (s < 0) s = (int)len + s;
    if (s < 0) s = 0;
    start = (ant_offset_t) s;
  }
  if (nargs >= 3 && vtype(args[2]) == T_NUM) {
    int e = (int) tod(args[2]);
    if (e < 0) e = (int)len + e;
    if (e < 0) e = 0;
    end = (ant_offset_t) e;
  }
  if (start > len) start = len;
  if (end > len) end = len;
  
  for (ant_offset_t i = start; i < end; i++) {
    arr_set(js, arr, i, value);
  }
  
  return arr;
}

static ant_value_t array_find_impl(ant_t *js, ant_value_t *args, int nargs, bool return_index, const char *name) {
  ant_value_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "%s called on non-array", name);
  
  ant_value_t callback = require_callback(js, args, nargs, name);
  if (is_err(callback)) return callback;
  ant_value_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();
  
  ant_offset_t len = get_array_length(js, arr);
  if (len == 0) return return_index ? tov(-1) : js_mkundef();
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t val = arr_get(js, arr, i);
    
    ant_value_t call_args[3] = { val, tov((double)i), arr };
    ant_value_t result = sv_vm_call(js->vm, js, callback, this_arg, call_args, 3, NULL, false);
    
    if (is_err(result)) return result;
    if (js_truthy(js, result)) return return_index ? tov((double)i) : val;
  }
  
  return return_index ? tov(-1) : js_mkundef();
}

static ant_value_t builtin_array_find(ant_t *js, ant_value_t *args, int nargs) {
  return array_find_impl(js, args, nargs, false, "find");
}

static ant_value_t builtin_array_findIndex(ant_t *js, ant_value_t *args, int nargs) {
  return array_find_impl(js, args, nargs, true, "findIndex");
}

static ant_value_t array_find_last_impl(ant_t *js, ant_value_t *args, int nargs, bool return_index, const char *name) {
  ant_value_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "%s called on non-array", name);
  
  ant_value_t callback = require_callback(js, args, nargs, name);
  if (is_err(callback)) return callback;
  ant_value_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();
  
  ant_offset_t len = get_array_length(js, arr);
  if (len == 0) return return_index ? tov(-1) : js_mkundef();
  
  for (ant_offset_t i = len; i > 0; i--) {
    ant_value_t val = arr_get(js, arr, i - 1);
    
    ant_value_t call_args[3] = { val, tov((double)(i - 1)), arr };
    ant_value_t result = sv_vm_call(js->vm, js, callback, this_arg, call_args, 3, NULL, false);
    
    if (is_err(result)) return result;
    if (js_truthy(js, result)) return return_index ? tov((double)(i - 1)) : val;
  }
  
  return return_index ? tov(-1) : js_mkundef();
}

static ant_value_t builtin_array_findLast(ant_t *js, ant_value_t *args, int nargs) {
  return array_find_last_impl(js, args, nargs, false, "findLast");
}

static ant_value_t builtin_array_findLastIndex(ant_t *js, ant_value_t *args, int nargs) {
  return array_find_last_impl(js, args, nargs, true, "findLastIndex");
}

static ant_value_t builtin_array_flatMap(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "flatMap called on non-array");
  }
  if (nargs == 0 || vtype(args[0]) != T_FUNC) {
    return js_mkerr(js, "flatMap requires a function argument");
  }
  
  ant_value_t callback = args[0];
  ant_value_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();
  ant_offset_t len = get_array_length(js, arr);
  
  ant_value_t result = mkarr(js);
  if (is_err(result)) return result;
  ant_offset_t result_idx = 0;
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t elem = arr_get(js, arr, i);
    ant_value_t call_args[3] = { elem, tov((double)i), arr };
    ant_value_t mapped = sv_vm_call(js->vm, js, callback, this_arg, call_args, 3, NULL, false);
    if (is_err(mapped)) return mapped;
    if (vtype(mapped) == T_ARR) flat_helper(js, mapped, result, &result_idx, 0);
    else arr_set(js, result, result_idx++, mapped);
  }
  
  return mkval(T_ARR, vdata(result));
}

static const char *js_tostring(ant_t *js, ant_value_t v) {
  if (vtype(v) == T_STR) {
    ant_offset_t slen, off = vstr(js, v, &slen);
    return (const char *)(uintptr_t)(off);
  }
  return js_str(js, v);
}

static int js_compare_values(ant_t *js, ant_value_t a, ant_value_t b, ant_value_t compareFn) {
  uint8_t t = vtype(compareFn);
  if (t == T_FUNC || t == T_CFUNC) {
    ant_value_t call_args[2] = { a, b };
    ant_value_t result = sv_vm_call(js->vm, js, compareFn, js_mkundef(), call_args, 2, NULL, false);
    if (vtype(result) == T_NUM) return (int)tod(result);
    return 0;
  }
  
  if (vtype(a) == T_STR && vtype(b) == T_STR) {
    ant_offset_t len_a, len_b;
    const char *sa = (const char *)(uintptr_t)(vstr(js, a, &len_a));
    const char *sb = (const char *)(uintptr_t)(vstr(js, b, &len_b));
    return strcmp(sa, sb);
  }
  
  const char *sa = js_tostring(js, a);
  size_t len = strlen(sa);
  
  char *copy = alloca(len + 1);
  memcpy(copy, sa, len + 1);
  
  return strcmp(copy, js_tostring(js, b));
}

static ant_value_t builtin_array_indexOf(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "indexOf called on non-array");
  }
  if (nargs == 0) return tov(-1);
  
  ant_value_t search = args[0];
  ant_offset_t len = get_array_length(js, arr);
  
  ant_offset_t start = 0;
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    int s = (int) tod(args[1]);
    if (s < 0) s = (int)len + s;
    if (s < 0) s = 0;
    start = (ant_offset_t) s;
  }
  
  for (ant_offset_t i = start; i < len; i++) {
    ant_value_t elem = arr_get(js, arr, i);
    if (vtype(elem) == T_UNDEF && !arr_has(js, arr, i)) continue;
    if (strict_eq_values(js, elem, search)) return tov((double)i);
  }
  return tov(-1);
}

static ant_value_t builtin_array_lastIndexOf(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "lastIndexOf called on non-array");
  }
  if (nargs == 0) return tov(-1);
  
  ant_value_t search = args[0];
  ant_offset_t len = get_array_length(js, arr);
  
  int start = (int)len - 1;
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    start = (int) tod(args[1]);
    if (start < 0) start = (int)len + start;
  }
  if (start >= (int)len) start = (int)len - 1;
  
  for (int i = start; i >= 0; i--) {
    ant_value_t elem = arr_get(js, arr, (ant_offset_t)i);
    if (vtype(elem) == T_UNDEF && !arr_has(js, arr, (ant_offset_t)i)) continue;
    if (strict_eq_values(js, elem, search)) return tov((double)i);
  }
  return tov(-1);
}

static ant_value_t builtin_array_reduceRight(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "reduceRight called on non-array");
  }
  if (nargs == 0 || vtype(args[0]) != T_FUNC) {
    return js_mkerr(js, "reduceRight requires a function argument");
  }
  
  ant_value_t callback = args[0];
  ant_offset_t len = get_array_length(js, arr);
  
  int start_idx = (int)len - 1;
  ant_value_t accumulator;
  
  if (nargs >= 2) {
    accumulator = args[1];
  } else {
    if (len == 0) return js_mkerr(js, "reduceRight of empty array with no initial value");
    accumulator = arr_get(js, arr, len - 1);
    start_idx = (int)len - 2;
  }
  
  for (int i = start_idx; i >= 0; i--) {
    ant_value_t elem = arr_get(js, arr, (ant_offset_t)i);
    ant_value_t call_args[4] = { accumulator, elem, tov((double)i), arr };
    accumulator = sv_vm_call(js->vm, js, callback, js_mkundef(), call_args, 4, NULL, false);
    if (is_err(accumulator)) return accumulator;
  }
  
  return accumulator;
}

static ant_value_t builtin_array_shift(ant_t *js, ant_value_t *args, int nargs) {
  (void) args;
  (void) nargs;
  ant_value_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "shift called on non-array");
  }

  ant_offset_t len = proxy_aware_length(js, arr);
  if (len == 0) return js_mkundef();

  ant_offset_t doff = get_dense_buf(arr);
  if (doff && !is_proxy(arr)) {
    ant_offset_t d_len = dense_iterable_length(js, arr);
    if (len != d_len) goto shift_slow;
    if (d_len == 0) return js_mkundef();
    ant_value_t *d = dense_data(doff);
    if (!d) return js_mkundef();
    ant_value_t first = dense_get(doff, 0);
    if (is_empty_slot(first)) first = js_mkundef();
    memmove(&d[0], &d[1], sizeof(ant_value_t) * (size_t)(d_len - 1));
    dense_set(js, doff, d_len - 1, T_EMPTY);
    array_len_set(js, arr, len - 1);
    return first;
  }

  shift_slow:
  ant_value_t read_from = is_proxy(arr) ? proxy_read_target(js, arr) : arr;
  ant_value_t first = arr_get(js, read_from, 0);

  for (ant_offset_t i = 1; i < len; i++) {
    if (arr_has(js, read_from, i)) {
      ant_value_t elem = arr_get(js, read_from, i);
      char dst[16];
      size_t dstlen = uint_to_str(dst, sizeof(dst), (unsigned)(i - 1));
      js_setprop(js, arr, js_mkstr(js, dst, dstlen), elem);
    }
  }

  if (array_obj_ptr(arr)) array_len_set(js, arr, len - 1);
  else js_setprop(js, arr, js->length_str, tov((double)(len - 1)));
  
  return first;
}

static ant_value_t builtin_array_unshift(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "unshift called on non-array");
  }

  ant_offset_t len = proxy_aware_length(js, arr);

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

static ant_value_t builtin_array_some(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "some called on non-array");
  
  ant_value_t callback = require_callback(js, args, nargs, "some");
  if (is_err(callback)) return callback;
  ant_value_t this_arg = (nargs >= 2) ? args[1] : js_mkundef();
  
  ant_offset_t len = get_array_length(js, arr);
  if (len == 0) return mkval(T_BOOL, 0);
  
  for (ant_offset_t i = 0; i < len; i++) {
    if (!arr_has(js, arr, i)) continue;
    ant_value_t val = arr_get(js, arr, i);
    
    ant_value_t call_args[3] = { val, tov((double)i), arr };
    ant_value_t result = sv_vm_call(js->vm, js, callback, this_arg, call_args, 3, NULL, false);
    
    if (is_err(result)) return result;
    if (js_truthy(js, result)) return mkval(T_BOOL, 1);
  }
  
  return mkval(T_BOOL, 0);
}

static ant_value_t builtin_array_sort(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;
  ant_value_t compareFn = js_mkundef();
  ant_value_t *vals = NULL, *keys = NULL, *temp_vals = NULL, *temp_keys = NULL;
  ant_offset_t count = 0, undef_count = 0, len = 0;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "sort called on non-array");
  
  if (nargs >= 1) {
    uint8_t t = vtype(args[0]);
    if (t == T_FUNC || t == T_CFUNC) compareFn = args[0];
    else if (t != T_UNDEF) return js_mkerr_typed(js, JS_ERR_TYPE, "compareFn must be a function or undefined");
  }
  
  len = get_array_length(js, arr);
  if (len == 0) return arr;
  
  ant_offset_t doff = get_dense_buf(arr);
  if (doff) {
    vals = malloc(len * sizeof(ant_value_t));
    if (!vals) goto oom;
    for (ant_offset_t i = 0; i < len; i++) {
      ant_value_t v = dense_get(doff, i);
      if (is_empty_slot(v) || vtype(v) == T_UNDEF) undef_count++;
      else vals[count++] = v;
    }
  } else {
    vals = malloc(len * sizeof(ant_value_t));
    if (!vals) goto oom;

    for (ant_offset_t i = 0; i < len; i++) {
      if (!arr_has(js, arr, i)) continue;
      ant_value_t v = arr_get(js, arr, i);
      if (vtype(v) == T_UNDEF) undef_count++;
      else vals[count++] = v;
    }
  }
  if (count <= 1) goto writeback;
  
  bool use_keys = (vtype(compareFn) == T_UNDEF);
  if (use_keys) {
    keys = malloc(count * sizeof(ant_value_t));
    if (!keys) goto oom;
    for (ant_offset_t i = 0; i < count; i++) {
      const char *s = js_tostring(js, vals[i]);
      keys[i] = js_mkstr(js, s, strlen(s));
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
        if (use_keys) {
          ant_offset_t len_a, len_b;
          const char *sa = (const char *)(uintptr_t)(vstr(js, keys[i], &len_a));
          const char *sb = (const char *)(uintptr_t)(vstr(js, keys[j], &len_b));
          cmp = strcmp(sa, sb);
        } else {
          cmp = js_compare_values(js, vals[i], vals[j], compareFn);
        }
        if (cmp <= 0) {
          temp_vals[k] = vals[i];
          if (use_keys) temp_keys[k] = keys[i];
          k++; i++;
        } else {
          temp_vals[k] = vals[j];
          if (use_keys) temp_keys[k] = keys[j];
          k++; j++;
        }
      }
      while (i < mid) {
        temp_vals[k] = vals[i];
        if (use_keys) temp_keys[k] = keys[i];
        k++; i++;
      }
      while (j < right) {
        temp_vals[k] = vals[j];
        if (use_keys) temp_keys[k] = keys[j];
        k++; j++;
      }
      
      memcpy(&vals[left], temp_vals, k * sizeof(ant_value_t));
      if (use_keys) memcpy(&keys[left], temp_keys, k * sizeof(ant_value_t));
    }
  }
  
writeback:
  if (doff) {
    for (ant_offset_t i = 0; i < count; i++) dense_set(js, doff, i, vals[i]);
    for (ant_offset_t i = count; i < count + undef_count; i++) dense_set(js, doff, i, js_mkundef());
    for (ant_offset_t i = count + undef_count; i < len; i++) dense_set(js, doff, i, T_EMPTY);
  } else {
    ant_offset_t out = 0;
    for (; out < count; out++) arr_set(js, arr, out, vals[out]);
    for (ant_offset_t i = 0; i < undef_count; i++, out++) arr_set(js, arr, out, js_mkundef());
    for (; out < len; out++) arr_del(js, arr, out);
  }
  
  free(temp_keys);
  free(temp_vals);
  free(keys);
  free(vals);
  return arr;
  
oom:
  free(temp_keys);
  free(temp_vals);
  free(keys);
  free(vals);
  return js_mkerr(js, "out of memory");
}

static ant_value_t builtin_array_splice(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "splice called on non-array");
  }

  ant_offset_t len = proxy_aware_length(js, arr);
  ant_value_t read_from = is_proxy(arr) ? proxy_read_target(js, arr) : arr;

  int start = 0;
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    start = (int) tod(args[0]);
    if (start < 0) start = (int)len + start;
    if (start < 0) start = 0;
    if (start > (int)len) start = (int)len;
  }

  int deleteCount = (int)len - start;
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    deleteCount = (int) tod(args[1]);
    if (deleteCount < 0) deleteCount = 0;
    if (deleteCount > (int)len - start) deleteCount = (int)len - start;
  }

  int insertCount = nargs > 2 ? nargs - 2 : 0;

  ant_value_t removed = array_alloc_like(js, arr);
  if (is_err(removed)) return removed;

  ant_offset_t doff = get_dense_buf(arr);
  if (doff && !is_proxy(arr)) {
    ant_offset_t d_len = dense_iterable_length(js, arr);
    if (d_len != len) goto splice_slow;
    for (int i = 0; i < deleteCount; i++) {
      ant_value_t elem = arr_get(js, arr, (ant_offset_t)(start + i));
      arr_set(js, removed, (ant_offset_t)i, elem);
    }

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
      if (move_count > 0) memmove(&d[move_dest], &d[move_start], sizeof(ant_value_t) * (size_t)move_count);
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

  splice_slow:
  for (int i = 0; i < deleteCount; i++) {
    char src[16], dst[16];
    snprintf(src, sizeof(src), "%u", (unsigned)(start + i));
    snprintf(dst, sizeof(dst), "%u", (unsigned) i);
    ant_offset_t elem_off = lkp(js, read_from, src, strlen(src));
    if (elem_off != 0) {
      ant_value_t elem = propref_load(js, elem_off);
      ant_value_t key = js_mkstr(js, dst, strlen(dst));
      js_setprop(js, removed, key, elem);
    }
  }

  js_setprop(js, removed, js->length_str, tov((double) deleteCount));
  int shift = insertCount - deleteCount;
  
  if (shift > 0) {
    for (int i = (int)len - 1; i >= start + deleteCount; i--) {
      char src[16], dst[16];
      snprintf(src, sizeof(src), "%u", (unsigned) i);
      snprintf(dst, sizeof(dst), "%u", (unsigned)(i + shift));
      ant_offset_t elem_off = lkp(js, read_from, src, strlen(src));
      ant_value_t elem = elem_off ? propref_load(js, elem_off) : js_mkundef();
      ant_value_t key = js_mkstr(js, dst, strlen(dst));
      js_setprop(js, arr, key, elem);
    }
  } else if (shift < 0) {
    for (int i = start + deleteCount; i < (int)len; i++) {
      char src[16], dst[16];
      snprintf(src, sizeof(src), "%u", (unsigned) i);
      snprintf(dst, sizeof(dst), "%u", (unsigned)(i + shift));
      ant_offset_t elem_off = lkp(js, read_from, src, strlen(src));
      ant_value_t elem = elem_off ? propref_load(js, elem_off) : js_mkundef();
      ant_value_t key = js_mkstr(js, dst, strlen(dst));
      js_setprop(js, arr, key, elem);
    }
  }
  
  for (int i = 0; i < insertCount; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)(start + i));
    ant_value_t key = js_mkstr(js, idxstr, idxlen);
    js_setprop(js, arr, key, args[2 + i]);
  }
  
  if (array_obj_ptr(arr)) array_len_set(js, arr, (ant_offset_t)((int)len + shift));
  else js_setprop(js, arr, js->length_str, tov((double)((int)len + shift)));
  
  return removed;
}

static ant_value_t builtin_array_copyWithin(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "copyWithin called on non-array");
  }

  ant_offset_t len = proxy_aware_length(js, arr);
  ant_value_t read_from = is_proxy(arr) ? proxy_read_target(js, arr) : arr;

  int target = 0, start = 0, end = (int)len;
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    target = (int) tod(args[0]);
    if (target < 0) target = (int)len + target;
    if (target < 0) target = 0;
  }
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    start = (int) tod(args[1]);
    if (start < 0) start = (int)len + start;
    if (start < 0) start = 0;
  }
  if (nargs >= 3 && vtype(args[2]) == T_NUM) {
    end = (int) tod(args[2]);
    if (end < 0) end = (int)len + end;
    if (end < 0) end = 0;
  }
  
  if (end > (int)len) end = (int)len;
  int count = end - start;
  if (count > (int)len - target) count = (int)len - target;
  if (count <= 0) return arr;

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

  ant_value_t *temp = (ant_value_t *)malloc(count * sizeof(ant_value_t));
  for (int i = 0; i < count; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)(start + i));
    ant_offset_t elem_off = lkp(js, read_from, idxstr, idxlen);
    temp[i] = elem_off ? propref_load(js, elem_off) : js_mkundef();
  }
  
  for (int i = 0; i < count; i++) {
    char idxstr[16];
    size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)(target + i));
    ant_value_t key = js_mkstr(js, idxstr, idxlen);
    js_setprop(js, arr, key, temp[i]);
  }
  
  free(temp);
  return arr;
}

static ant_value_t builtin_array_toSorted(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "toSorted called on non-array");
  
  ant_value_t result = array_shallow_copy(js, arr, get_array_length(js, arr));
  if (is_err(result)) return result;
  
  ant_value_t saved_this = js->this_val;
  js->this_val = result;
  ant_value_t sorted = builtin_array_sort(js, args, nargs);
  js->this_val = saved_this;
  
  if (is_err(sorted)) return sorted;
  return mkval(T_ARR, vdata(result));
}

static ant_value_t builtin_array_toReversed(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "toReversed called on non-array");
  
  ant_value_t result = array_shallow_copy(js, arr, get_array_length(js, arr));
  if (is_err(result)) return result;
  
  ant_value_t saved_this = js->this_val;
  js->this_val = result;
  ant_value_t reversed = builtin_array_reverse(js, NULL, 0);
  js->this_val = saved_this;
  
  if (is_err(reversed)) return reversed;
  return mkval(T_ARR, vdata(result));
}

static ant_value_t builtin_array_toSpliced(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ)
    return js_mkerr(js, "toSpliced called on non-array");
  
  ant_value_t result = array_shallow_copy(js, arr, get_array_length(js, arr));
  if (is_err(result)) return result;
  
  ant_value_t saved_this = js->this_val;
  js->this_val = result;
  builtin_array_splice(js, args, nargs);
  js->this_val = saved_this;
  
  return mkval(T_ARR, vdata(result));
}

static ant_value_t builtin_array_with(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "with called on non-array");
  }
  
  if (nargs < 2) return js_mkerr(js, "with requires index and value arguments");
  
  ant_offset_t len = get_array_length(js, arr);
  
  int idx = (int) tod(args[0]);
  if (idx < 0) idx = (int)len + idx;
  if (idx < 0 || (ant_offset_t)idx >= len) return js_mkerr(js, "Invalid index");
  
  ant_value_t result = mkarr(js);
  if (is_err(result)) return result;
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t elem = ((ant_offset_t)idx == i) ? args[1] : arr_get(js, arr, i);
    arr_set(js, result, i, elem);
  }
  
  return mkval(T_ARR, vdata(result));
}

static ant_value_t builtin_array_keys(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->this_val) != T_ARR && vtype(js->this_val) != T_OBJ)
    return js_mkerr(js, "keys called on non-array");
  return make_array_iterator(js, js->this_val, ARR_ITER_KEYS);
}

static ant_value_t builtin_array_values(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->this_val) != T_ARR && vtype(js->this_val) != T_OBJ)
    return js_mkerr(js, "values called on non-array");
  return make_array_iterator(js, js->this_val, ARR_ITER_VALUES);
}

static ant_value_t builtin_array_entries(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->this_val) != T_ARR && vtype(js->this_val) != T_OBJ)
    return js_mkerr(js, "entries called on non-array");
  return make_array_iterator(js, js->this_val, ARR_ITER_ENTRIES);
}

static ant_value_t builtin_array_toString(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t arr = js->this_val;
  
  ant_value_t join_result;
  if (js_try_call_method(js, arr, "join", 4, NULL, 0, &join_result)) {
    if (is_err(join_result)) return join_result;
    return join_result;
  }
  
  return builtin_object_toString(js, args, nargs);
}

static ant_value_t builtin_array_toLocaleString(ant_t *js, ant_value_t *args, int nargs) {
  (void) args;
  (void) nargs;
  ant_value_t arr = js->this_val;
  if (vtype(arr) != T_ARR) return js_mkerr(js, "toLocaleString called on non-array");
  
  ant_offset_t len = get_array_length(js, arr);
  if (len == 0) return js_mkstr(js, "", 0);
  
  char *result = NULL;
  size_t result_len = 0, result_cap = 256;
  result = (char *)ant_calloc(result_cap);
  if (!result) return js_mkerr(js, "oom");
  
  for (ant_offset_t i = 0; i < len; i++) {
    if (i > 0) {
      if (result_len + 1 >= result_cap) {
        result_cap *= 2;
        char *new_result = (char *)ant_calloc(result_cap);
        if (!new_result) { free(result); return js_mkerr(js, "oom"); }
        memcpy(new_result, result, result_len);
        free(result);
        result = new_result;
      }
      result[result_len++] = ',';
    }
    
    if (!arr_has(js, arr, i)) continue;
    ant_value_t elem = arr_get(js, arr, i);
    if (vtype(elem) == T_NULL || vtype(elem) == T_UNDEF) continue;
    
    char buf[64];
    size_t elem_len = tostr(js, elem, buf, sizeof(buf));
    
    if (result_len + elem_len >= result_cap) {
      while (result_len + elem_len >= result_cap) result_cap *= 2;
      char *new_result = (char *)ant_calloc(result_cap);
      if (!new_result) { free(result); return js_mkerr(js, "oom"); }
      memcpy(new_result, result, result_len);
      free(result);
      result = new_result;
    }
    memcpy(result + result_len, buf, elem_len);
    result_len += elem_len;
  }
  
  ant_value_t ret = js_mkstr(js, result, result_len);
  free(result);
  return ret;
}

static ant_value_t builtin_Array_isArray(ant_t *js, ant_value_t *args, int nargs) {
  (void) js;
  if (nargs == 0) return mkval(T_BOOL, 0);
  return mkval(T_BOOL, vtype(args[0]) == T_ARR ? 1 : 0);
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
    elem = sv_vm_call(js->vm, js, fctx->mapFn, fctx->mapThis, call_args, 2, NULL, false);
    if (is_err(elem)) { *out = elem; return ITER_ERROR; }
  }

  if (vtype(fctx->write_target) == T_ARR) arr_set(js, fctx->write_target, fctx->index, elem);
  else {
    char idxstr[16]; size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)fctx->index);
    js_setprop(js, fctx->write_target, js_mkstr(js, idxstr, idxlen), elem);
  }

  fctx->index++;
  return ITER_CONTINUE;
}

static ant_value_t builtin_Array_from(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return mkarr(js);

  ant_value_t src = args[0];
  ant_value_t mapFn = (nargs >= 2 && is_callable(args[1])) ? args[1] : js_mkundef();
  ant_value_t mapThis = (nargs >= 3) ? args[2] : js_mkundef();

  ant_value_t ctor = js->this_val;
  bool use_ctor = (vtype(ctor) == T_FUNC || vtype(ctor) == T_CFUNC);
  ant_value_t result = use_ctor ? array_alloc_from_ctor_with_length(js, ctor, 0) : mkarr(js);
  if (is_err(result)) return result;

  bool result_is_proxy = is_proxy(result);
  ant_value_t write_target = result_is_proxy ? proxy_read_target(js, result) : result;
  ant_value_t iter_sym = get_iterator_sym();

  if (vtype(src) == T_STR) {
    if (str_is_heap_rope(src)) { src = rope_flatten(js, src); if (is_err(src)) return src; }
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
    if (vtype(result) != T_ARR) js_setprop(js, result, js->length_str, tov((double)ctx.index));
  } else if (vtype(src) == T_ARR) {
    ant_offset_t iter_off = (vtype(iter_sym) == T_SYMBOL) ? lkp_sym_proto(js, src, (ant_offset_t)vdata(iter_sym)) : 0;
    bool default_iter = iter_off != 0 && vtype(propref_load(js, iter_off)) == T_CFUNC;

    if (default_iter) {
      array_from_iter_ctx_t ctx = { write_target, result, mapFn, mapThis, 0 };
      ant_offset_t len = get_array_length(js, src);
      for (ant_offset_t i = 0; i < len; i++) {
        ant_value_t unused;
        iter_action_t act = array_from_iter_cb(js, arr_get(js, src, i), &ctx, &unused);
        if (act == ITER_ERROR) return unused;
      }
      if (vtype(result) != T_ARR) js_setprop(js, result, js->length_str, tov((double)len));
    } else {
      array_from_iter_ctx_t ctx = { write_target, result, mapFn, mapThis, 0 };
      ant_value_t iter_result = iter_foreach(js, src, array_from_iter_cb, &ctx);
      if (is_err(iter_result)) return iter_result;
      if (vtype(result) != T_ARR) js_setprop(js, result, js->length_str, tov((double)ctx.index));
    }
  } else {
    ant_offset_t iter_prop = (vtype(iter_sym) == T_SYMBOL) ? lkp_sym_proto(js, src, (ant_offset_t)vdata(iter_sym)) : 0;

    if (iter_prop != 0) {
      array_from_iter_ctx_t ctx = { write_target, result, mapFn, mapThis, 0 };
      ant_value_t iter_result = iter_foreach(js, src, array_from_iter_cb, &ctx);
      if (is_err(iter_result)) return iter_result;
      if (vtype(result) != T_ARR) js_setprop(js, result, js->length_str, tov((double)ctx.index));
    } else if (vtype(src) == T_OBJ) {
      array_from_iter_ctx_t ctx = { write_target, result, mapFn, mapThis, 0 };
      ant_offset_t len = get_array_length(js, src);
      for (ant_offset_t i = 0; i < len; i++) {
        ant_value_t unused;
        iter_action_t act = array_from_iter_cb(js, arr_get(js, src, i), &ctx, &unused);
        if (act == ITER_ERROR) return unused;
      }
      if (vtype(result) != T_ARR) js_setprop(js, result, js->length_str, tov((double)len));
    }
  }

  if (!use_ctor) return mkval(T_ARR, vdata(result));
  return result;
}

static ant_value_t builtin_Array_of(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t ctor = js->this_val;
  bool use_ctor = (vtype(ctor) == T_FUNC || vtype(ctor) == T_CFUNC);
  ant_value_t arr = use_ctor ? array_alloc_from_ctor_with_length(js, ctor, (ant_offset_t)nargs) : mkarr(js);
  if (is_err(arr)) return arr;

  bool arr_is_proxy = is_proxy(arr);
  ant_value_t write_target = arr_is_proxy ? proxy_read_target(js, arr) : arr;

  for (int i = 0; i < nargs; i++) {
    if (vtype(write_target) == T_ARR) arr_set(js, write_target, (ant_offset_t)i, args[i]);
    else {
      char idxstr[16]; size_t idxlen = uint_to_str(idxstr, sizeof(idxstr), (unsigned)i);
      js_setprop(js, write_target, js_mkstr(js, idxstr, idxlen), args[i]);
    }
  }

  if (vtype(arr) != T_ARR) js_setprop(js, arr, js->length_str, tov((double) nargs));
  if (!use_ctor) return mkval(T_ARR, vdata(arr));

  return arr;
}

static ant_value_t builtin_string_indexOf(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "indexOf called on non-string");
  if (nargs == 0) return tov(-1);

  ant_value_t search = args[0];
  if (vtype(search) != T_STR) return tov(-1);

  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  ant_offset_t search_len, search_off = vstr(js, search, &search_len);
  
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  const char *search_ptr = (char *)(uintptr_t)(search_off);
  size_t utf16_len = utf16_strlen(str_ptr, str_len);

  ant_offset_t start_utf16 = 0;
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    double pos = tod(args[1]);
    if (pos < 0) pos = 0;
    if (pos > D(utf16_len)) pos = D(utf16_len);
    start_utf16 = (ant_offset_t) pos;
  }
  
  if (search_len == 0) return tov(D(start_utf16));

  size_t byte_start = 0;
  if (start_utf16 > 0) {
    int off = utf16_index_to_byte_offset(str_ptr, str_len, start_utf16, NULL);
    if (off < 0) return tov(-1);
    byte_start = (size_t)off;
  }

  if (byte_start + search_len > (size_t)str_len) return tov(-1);

  for (size_t i = byte_start; i <= (size_t)str_len - search_len; i++) {
    if (memcmp(str_ptr + i, search_ptr, search_len) == 0)
      return tov(D(byte_offset_to_utf16(str_ptr, i)));
  }
  return tov(-1);
}

static ant_value_t builtin_string_substring(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "substring called on non-string");
  ant_offset_t byte_len, str_off = vstr(js, str, &byte_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  size_t utf16_len = utf16_strlen(str_ptr, byte_len);
  ant_offset_t start = 0, end = (ant_offset_t)utf16_len;
  double dstr_len2 = D(utf16_len);
  
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    double d = tod(args[0]);
    start = (ant_offset_t) (d < 0 ? 0 : (d > dstr_len2 ? dstr_len2 : d));
  }
  
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    double d = tod(args[1]);
    end = (ant_offset_t) (d < 0 ? 0 : (d > dstr_len2 ? dstr_len2 : d));
  }
  
  if (start > end) {
    ant_offset_t tmp = start;
    start = end;
    end = tmp;
  }
  
  size_t byte_start, byte_end;
  utf16_range_to_byte_range(str_ptr, byte_len, start, end, &byte_start, &byte_end);
  return js_mkstr(js, str_ptr + byte_start, byte_end - byte_start);
}

static ant_value_t builtin_string_substr(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "substr called on non-string");
  ant_offset_t byte_len, str_off = vstr(js, str, &byte_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  size_t utf16_len = utf16_strlen(str_ptr, byte_len);
  
  if (nargs < 1) return js_mkstr(js, str_ptr, byte_len);
  
  double d_start = tod(args[0]);
  ant_offset_t start;
  if (d_start < 0) {
    start = (ant_offset_t)((double)utf16_len + d_start);
    if ((int)start < 0) start = 0;
  } else {
    start = (ant_offset_t)d_start;
  }
  if (start > (ant_offset_t)utf16_len) start = (ant_offset_t)utf16_len;
  
  ant_offset_t len = (ant_offset_t)utf16_len - start;
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    double d = tod(args[1]);
    if (d < 0) d = 0;
    len = (ant_offset_t)d;
  }
  if (start + len > (ant_offset_t)utf16_len) len = (ant_offset_t)utf16_len - start;
  
  size_t byte_start, byte_end;
  utf16_range_to_byte_range(str_ptr, byte_len, start, start + len, &byte_start, &byte_end);
  return js_mkstr(js, str_ptr + byte_start, byte_end - byte_start);
}

static ant_value_t string_split_impl(ant_t *js, ant_value_t str, ant_value_t *args, int nargs) {
  if (vtype(str) != T_STR) return js_mkerr(js, "split called on non-string");
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  ant_value_t arr = mkarr(js);

  if (is_err(arr)) return arr;

  uint32_t limit = UINT32_MAX;
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    double d = tod(args[1]);
    if (d >= 0 && d <= UINT32_MAX) {
      limit = (uint32_t)d;
    }
  }
  if (limit == 0) {
    return mkval(T_ARR, vdata(arr));
  }
  if (nargs == 0) goto return_whole;

  ant_value_t sep_arg = args[0];
  if (vtype(sep_arg) == T_OBJ) {
    ant_offset_t source_off = lkp(js, sep_arg, "source", 6);
    if (source_off == 0) goto return_whole;
    ant_value_t source_val = propref_load(js, source_off);
    if (vtype(source_val) != T_STR) goto return_whole;

    ant_offset_t plen, poff = vstr(js, source_val, &plen);
    const char *pattern_ptr = (char *)(uintptr_t)(poff);

    if (plen == 0 || (plen == 4 && memcmp(pattern_ptr, "(?:)", 4) == 0)) {
      ant_offset_t idx = 0;
      for (ant_offset_t i = 0; i < str_len && idx < limit; i++) {
        ant_value_t part = js_mkstr(js, str_ptr + i, 1);
        arr_set(js, arr, idx, part);
        idx++;
      }
      return mkval(T_ARR, vdata(arr));
    }

    char pcre2_pattern[512];
    size_t pcre2_len = js_to_pcre2_pattern(pattern_ptr, plen, pcre2_pattern, sizeof(pcre2_pattern), false);

    uint32_t options = PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_UNSET_BACKREF;
    int errcode;
    PCRE2_SIZE erroffset;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pcre2_pattern, pcre2_len, options, &errcode, &erroffset, NULL);
    if (re == NULL) goto return_whole;

    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(re, NULL);
    uint32_t capture_count;
    pcre2_pattern_info(re, PCRE2_INFO_CAPTURECOUNT, &capture_count);

    if (str_len == 0) {
      int rc = pcre2_match(re, (PCRE2_SPTR)str_ptr, 0, 0, 0, match_data, NULL);
      if (rc >= 0) {
        pcre2_match_data_free(match_data);
        pcre2_code_free(re);
        return mkval(T_ARR, vdata(arr));
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

      ant_value_t part = js_mkstr(js, str_ptr + segment_start, match_start - segment_start);
      arr_set(js, arr, idx, part);
      idx++;

      for (uint32_t i = 1; i <= capture_count && idx < limit; i++) {
        PCRE2_SIZE cap_start = ovector[2*i];
        PCRE2_SIZE cap_end = ovector[2*i+1];
        if (cap_start == PCRE2_UNSET) {
          arr_set(js, arr, idx, js_mkundef());
        } else {
          part = js_mkstr(js, str_ptr + cap_start, cap_end - cap_start);
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
      arr_set(js, arr, 0, js_mkstr(js, str_ptr, str_len));
      return mkval(T_ARR, vdata(arr));
    }

    if (idx < limit) {
      ant_value_t part = js_mkstr(js, str_ptr + segment_start, str_len - segment_start);
      arr_set(js, arr, idx, part);
      idx++;
    }

    pcre2_match_data_free(match_data);
    pcre2_code_free(re);
    return mkval(T_ARR, vdata(arr));
  }

  if (vtype(sep_arg) != T_STR) goto return_whole;

  ant_offset_t sep_len, sep_off = vstr(js, sep_arg, &sep_len);
  const char *sep_ptr = (char *)(uintptr_t)(sep_off);
  ant_offset_t idx = 0, start = 0;

  if (sep_len == 0) {
    for (ant_offset_t i = 0; i < str_len && idx < limit; i++) {
      ant_value_t part = js_mkstr(js, str_ptr + i, 1);
      arr_set(js, arr, idx, part);
      idx++;
    }
    return mkval(T_ARR, vdata(arr));
  }

  for (ant_offset_t i = 0; i + sep_len <= str_len && idx < limit; i++) {
    if (memcmp(str_ptr + i, sep_ptr, sep_len) != 0) continue;
    ant_value_t part = js_mkstr(js, str_ptr + start, i - start);
    arr_set(js, arr, idx, part);
    idx++;
    start = i + sep_len;
    i += sep_len - 1;
  }
  if (idx < limit && start <= str_len) {
    ant_value_t part = js_mkstr(js, str_ptr + start, str_len - start);
    arr_set(js, arr, idx, part);
    idx++;
  }

  return mkval(T_ARR, vdata(arr));

return_whole:
  if (limit > 0) {
    arr_set(js, arr, 0, str);
  }
  return mkval(T_ARR, vdata(arr));
}

static ant_value_t builtin_string_split(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "split called on non-string");

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
      js, args[0], get_split_sym(), args[0], call_args, call_nargs, &called
    );
    if (is_err(dispatched)) return dispatched;
    if (called) return dispatched;
  }

  return string_split_impl(js, str, args, nargs);
}

static ant_value_t builtin_string_slice(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_unwrapped = unwrap_primitive(js, js->this_val);
  ant_value_t str = js_tostring_val(js, this_unwrapped);
  if (is_err(str)) return str;
  
  ant_offset_t byte_len, str_off = vstr(js, str, &byte_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  size_t utf16_len = utf16_strlen(str_ptr, byte_len);
  ant_offset_t start = 0, end = (ant_offset_t)utf16_len;
  double dstr_len = D(utf16_len);
  
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    double d = tod(args[0]);
    if (d < 0) {
      start = (ant_offset_t) (d + dstr_len < 0 ? 0 : d + dstr_len);
    } else start = (ant_offset_t) (d > dstr_len ? dstr_len : d);
  }
  
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    double d = tod(args[1]);
    if (d < 0) {
      end = (ant_offset_t) (d + dstr_len < 0 ? 0 : d + dstr_len);
    } else end = (ant_offset_t) (d > dstr_len ? dstr_len : d);
  }
  
  if (start > end) start = end;
  size_t byte_start, byte_end;
  utf16_range_to_byte_range(str_ptr, byte_len, start, end, &byte_start, &byte_end);
  return js_mkstr(js, str_ptr + byte_start, byte_end - byte_start);
}

static ant_value_t builtin_string_includes(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "includes called on non-string");
  if (nargs == 0) return mkval(T_BOOL, 0);
  ant_value_t search = args[0];
  
  if (is_object_type(search)) {
    ant_value_t maybe_err = reject_regexp_arg(js, search, "includes");
    if (is_err(maybe_err)) return maybe_err;
  }
  search = js_tostring_val(js, search);
  if (is_err(search)) return search;
  
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  ant_offset_t search_len, search_off = vstr(js, search, &search_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  const char *search_ptr = (char *)(uintptr_t)(search_off);
  
  ant_offset_t start = 0;
  if (nargs >= 2) {
    double pos = tod(args[1]);
    if (isnan(pos) || pos < 0) pos = 0;
    if (pos > D(str_len)) return mkval(T_BOOL, 0);
    start = (ant_offset_t) pos;
  }
  
  if (search_len == 0) return mkval(T_BOOL, 1);
  if (start + search_len > str_len) return mkval(T_BOOL, 0);
  for (ant_offset_t i = start; i <= str_len - search_len; i++) {
    if (memcmp(str_ptr + i, search_ptr, search_len) == 0) return mkval(T_BOOL, 1);
  }
  
  return mkval(T_BOOL, 0);
}

static ant_value_t builtin_string_startsWith(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "startsWith called on non-string");
  if (nargs == 0) return mkval(T_BOOL, 0);
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
  
  if (search_len > str_len) return mkval(T_BOOL, 0);
  if (search_len == 0) return mkval(T_BOOL, 1);
  
  return mkval(T_BOOL, memcmp(str_ptr, search_ptr, search_len) == 0 ? 1 : 0);
}

static ant_value_t builtin_string_endsWith(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "endsWith called on non-string");
  if (nargs == 0) return mkval(T_BOOL, 0);
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
  
  if (search_len > str_len) return mkval(T_BOOL, 0);
  if (search_len == 0) return mkval(T_BOOL, 1);
  
  return mkval(T_BOOL, memcmp(str_ptr + str_len - search_len, search_ptr, search_len) == 0 ? 1 : 0);
}

static ant_value_t builtin_string_template(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "template called on non-string");
  if (nargs < 1 || vtype(args[0]) != T_OBJ) return str;
  
  ant_value_t data = args[0];
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  
  size_t result_cap = str_len + 256;
  size_t result_len = 0;
  char *result = (char *)ant_calloc(result_cap);
  if (!result) return js_mkerr(js, "oom");
  ant_offset_t i = 0;

#define ENSURE_CAP(need) do { \
  if (result_len + (need) >= result_cap) { \
    result_cap = (result_len + (need) + 1) * 2; \
    char *nr = (char *)ant_realloc(result, result_cap); \
    if (!nr) return js_mkerr(js, "oom"); \
    result = nr; \
  } \
} while(0)
  
  while (i < str_len) {
    if (i < str_len - 3 && str_ptr[i] == '{' && str_ptr[i + 1] == '{') {
      ant_offset_t start = i + 2;
      ant_offset_t end = start;
      while (end < str_len - 1 && !(str_ptr[end] == '}' && str_ptr[end + 1] == '}')) {
        end++;
      }
      if (end < str_len - 1 && str_ptr[end] == '}' && str_ptr[end + 1] == '}') {
        ant_offset_t key_len = end - start;
        ant_offset_t prop_off = lkp(js, data, str_ptr + start, key_len);
        
        if (prop_off != 0) {
          ant_value_t value = propref_load(js, prop_off);
          if (vtype(value) == T_STR) {
            ant_offset_t val_len, val_off = vstr(js, value, &val_len);
            ENSURE_CAP(val_len);
            memcpy(result + result_len, (const void *)(uintptr_t)val_off, val_len);
            result_len += val_len;
          } else if (vtype(value) == T_NUM) {
            char numstr[32];
            snprintf(numstr, sizeof(numstr), "%g", tod(value));
            size_t num_len = strlen(numstr);
            ENSURE_CAP(num_len);
            memcpy(result + result_len, numstr, num_len);
            result_len += num_len;
          } else if (vtype(value) == T_BOOL) {
            const char *boolstr = vdata(value) ? "true" : "false";
            size_t bool_len = strlen(boolstr);
            ENSURE_CAP(bool_len);
            memcpy(result + result_len, boolstr, bool_len);
            result_len += bool_len;
          }
        }
        i = end + 2;
        continue;
      }
    }
    ENSURE_CAP(1);
    result[result_len++] = str_ptr[i++];
  }
  ant_value_t ret = js_mkstr(js, result, result_len);
  free(result);
  return ret;
#undef ENSURE_CAP
}

static ant_value_t builtin_string_charCodeAt(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "charCodeAt called on non-string");
  
  double idx_d = nargs < 1 ? 0.0 : js_to_number(js, args[0]);
  if (isnan(idx_d)) idx_d = 0.0;
  if (isinf(idx_d) || idx_d > (double)LONG_MAX) return tov(JS_NAN);
  
  long idx_l = (long) idx_d;
  if (idx_l < 0) return tov(JS_NAN);
  
  ant_offset_t byte_len; ant_offset_t str_off = vstr(js, str, &byte_len);
  const char *str_data = (const char *)(uintptr_t)(str_off);
  
  uint32_t code_unit = utf16_code_unit_at(str_data, byte_len, idx_l);
  if (code_unit == 0xFFFFFFFF) return tov(JS_NAN);
  
  return tov((double) code_unit);
}

static ant_value_t builtin_string_codePointAt(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "codePointAt called on non-string");
  
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

static ant_value_t builtin_string_toLowerCase(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "toLowerCase called on non-string");
  
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

static ant_value_t builtin_string_toUpperCase(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "toUpperCase called on non-string");
  
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

static ant_value_t builtin_string_trim(ant_t *js, ant_value_t *args, int nargs) {
  (void) args; (void) nargs;
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "trim called on non-string");
  
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  
  ant_offset_t start = 0, end = str_len;
  while (start < end && is_space(str_ptr[start])) start++;
  while (end > start && is_space(str_ptr[end - 1])) end--;
  
  return js_mkstr(js, str_ptr + start, end - start);
}

static ant_value_t builtin_string_trimStart(ant_t *js, ant_value_t *args, int nargs) {
  (void) args; (void) nargs;
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "trimStart called on non-string");
  
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  
  ant_offset_t start = 0;
  while (start < str_len && is_space(str_ptr[start])) start++;
  
  return js_mkstr(js, str_ptr + start, str_len - start);
}

static ant_value_t builtin_string_trimEnd(ant_t *js, ant_value_t *args, int nargs) {
  (void) args; (void) nargs;
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "trimEnd called on non-string");
  
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  
  ant_offset_t end = str_len;
  while (end > 0 && is_space(str_ptr[end - 1])) end--;
  
  return js_mkstr(js, str_ptr, end);
}

static ant_value_t builtin_string_repeat(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "repeat called on non-string");
  if (nargs < 1 || vtype(args[0]) != T_NUM) return js_mkerr(js, "repeat count required");
  
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
  
  for (ant_offset_t i = 0; i < count; i++) {
    memcpy(result_ptr + i * str_len, str_ptr, str_len);
  }
  str_set_ascii_state(
    result_ptr,
    str_is_ascii(str_ptr) ? STR_ASCII_YES : STR_ASCII_NO
  );
  
  return result;
}

static ant_value_t builtin_string_padStart(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "padStart called on non-string");
  if (nargs < 1 || vtype(args[0]) != T_NUM) return str;
  
  ant_offset_t target_len = (ant_offset_t)tod(args[0]);
  if (target_len <= 0) return str;
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  size_t str_utf16_len = utf16_strlen(str_ptr, (size_t)str_len);
  
  if ((size_t)target_len <= str_utf16_len) return str;
  
  ant_value_t pad_val = js_mkstr(js, " ", 1);
  if (nargs >= 2 && vtype(args[1]) != T_UNDEF) {
    pad_val = coerce_to_str(js, args[1]);
    if (is_err(pad_val)) return pad_val;
  }
  ant_offset_t pad_len, pad_off = vstr(js, pad_val, &pad_len);
  const char *pad_str = (char *)(uintptr_t)(pad_off);
  size_t pad_utf16_len = utf16_strlen(pad_str, (size_t)pad_len);
  
  if (pad_utf16_len == 0) return str;
  
  size_t fill_utf16_len = (size_t)target_len - str_utf16_len;
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
  str_set_ascii_state(
    result_ptr,
    (str_is_ascii(pad_str) && str_is_ascii(str_ptr)) ? STR_ASCII_YES : STR_ASCII_NO
  );
  
  return result;
}

static ant_value_t builtin_string_padEnd(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "padEnd called on non-string");
  if (nargs < 1 || vtype(args[0]) != T_NUM) return str;
  
  ant_offset_t target_len = (ant_offset_t)tod(args[0]);
  if (target_len <= 0) return str;
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  size_t str_utf16_len = utf16_strlen(str_ptr, (size_t)str_len);
  
  if ((size_t)target_len <= str_utf16_len) return str;
  
  ant_value_t pad_val = js_mkstr(js, " ", 1);
  if (nargs >= 2 && vtype(args[1]) != T_UNDEF) {
    pad_val = coerce_to_str(js, args[1]);
    if (is_err(pad_val)) return pad_val;
  }
  ant_offset_t pad_len, pad_off = vstr(js, pad_val, &pad_len);
  const char *pad_str = (char *)(uintptr_t)(pad_off);
  size_t pad_utf16_len = utf16_strlen(pad_str, (size_t)pad_len);
  
  if (pad_utf16_len == 0) return str;
  
  size_t fill_utf16_len = (size_t)target_len - str_utf16_len;
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
  str_set_ascii_state(
    result_ptr,
    (str_is_ascii(str_ptr) && str_is_ascii(pad_str)) ? STR_ASCII_YES : STR_ASCII_NO
  );
  
  return result;
}

static ant_value_t builtin_string_charAt(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "charAt called on non-string");
  
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

static ant_value_t builtin_string_at(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "at called on non-string");
  
  double idx_d = nargs < 1 ? 0.0 : js_to_number(js, args[0]);
  if (isnan(idx_d) || isinf(idx_d)) return js_mkundef();

  ant_offset_t byte_len; ant_offset_t str_off = vstr(js, str, &byte_len);
  const char *str_data = (const char *)(uintptr_t)(str_off);
  size_t utf16_len = utf16_strlen(str_data, byte_len);
  
  long idx = (long) idx_d;
  if (idx < 0) idx += (long) utf16_len;
  if (idx < 0 || idx >= (long) utf16_len) return js_mkundef();
  
  uint32_t code_unit = utf16_code_unit_at(str_data, byte_len, idx);
  if (code_unit == 0xFFFFFFFF) return js_mkundef();
  
  return js_string_from_utf16_code_unit(js, code_unit);
}

static ant_value_t builtin_string_localeCompare(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "localeCompare called on non-string");
  if (nargs < 1) return tov(0);
  
  ant_value_t that = args[0];
  if (vtype(that) != T_STR) {
    char buf[64];
    size_t n = tostr(js, that, buf, sizeof(buf));
    that = js_mkstr(js, buf, n);
  }
  
  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  ant_offset_t that_len, that_off = vstr(js, that, &that_len);
  const char *str_ptr = (char *)(uintptr_t)(str_off);
  const char *that_ptr = (char *)(uintptr_t)(that_off);
  
  int result = strcoll(str_ptr, that_ptr);
  if (result < 0) return tov(-1);
  if (result > 0) return tov(1);
  return tov(0);
}

static ant_value_t builtin_string_lastIndexOf(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "lastIndexOf called on non-string");
  if (nargs == 0) return tov(-1);

  ant_value_t search = args[0];
  if (vtype(search) != T_STR) return tov(-1);

  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  ant_offset_t search_len, search_off = vstr(js, search, &search_len);
  
  ant_offset_t max_start = str_len;
  double dstr_len = D(str_len);
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    double pos = tod(args[1]);
    if (isnan(pos)) pos = dstr_len;
    if (pos < 0) pos = 0;
    if (pos > dstr_len) pos = dstr_len;
    max_start = (ant_offset_t) pos;
  }
  
  if (search_len == 0) return tov((double) (max_start > str_len ? str_len : max_start));
  if (search_len > str_len) return tov(-1);

  const char *str_ptr = (char *)(uintptr_t)(str_off);
  const char *search_ptr = (char *)(uintptr_t)(search_off);

  ant_offset_t start = (max_start + search_len > str_len) ? str_len - search_len : max_start;
  for (ant_offset_t i = start + 1; i > 0; i--) {
    if (memcmp(str_ptr + i - 1, search_ptr, search_len) == 0) return tov((double)(i - 1));
  }
  return tov(-1);
}

static ant_value_t builtin_string_concat(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_unwrapped = unwrap_primitive(js, js->this_val);
  ant_value_t str = js_tostring_val(js, this_unwrapped);
  if (is_err(str)) return str;

  ant_offset_t total_len;
  ant_offset_t base_off = vstr(js, str, &total_len);
  
  ant_value_t *str_args = NULL;
  if (nargs > 0) {
    str_args = (ant_value_t *)ant_calloc(nargs * sizeof(ant_value_t));
    if (!str_args) return js_mkerr(js, "oom");
    for (int i = 0; i < nargs; i++) {
      str_args[i] = js_tostring_val(js, args[i]);
      if (is_err(str_args[i])) { 
        free(str_args);
        return str_args[i];
      }
      ant_offset_t arg_len;
      vstr(js, str_args[i], &arg_len);
      total_len += arg_len;
    }
  }

  char *result = (char *)ant_calloc(total_len + 1);
  if (!result) { 
    if (str_args) free(str_args);
    return js_mkerr(js, "oom");
  }

  ant_offset_t base_len;
  base_off = vstr(js, str, &base_len);
  memcpy(result, (const void *)(uintptr_t)base_off, base_len);
  ant_offset_t pos = base_len;

  for (int i = 0; i < nargs; i++) {
    ant_offset_t arg_len, arg_off = vstr(js, str_args[i], &arg_len);
    memcpy(result + pos, (const void *)(uintptr_t)arg_off, arg_len);
    pos += arg_len;
  }
  result[pos] = '\0';

  ant_value_t ret = js_mkstr(js, result, pos);
  free(result); if (str_args) free(str_args);
  
  return ret;
}

static ant_value_t builtin_string_normalize(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "normalize called on non-string");

  ant_offset_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (const char *)(uintptr_t)(str_off);

  if (str_len == 0) return js_mkstr(js, "", 0);
  utf8proc_option_t opts = UTF8PROC_COMPOSE | UTF8PROC_STABLE;

  if (nargs >= 1 && vtype(args[0]) != T_UNDEF) {
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

static ant_value_t builtin_string_fromCharCode(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return js_mkstr(js, "", 0);

  char *buf = (char *)ant_calloc(nargs + 1);
  if (!buf) return js_mkerr(js, "oom");

  for (int i = 0; i < nargs; i++) {
    if (vtype(args[i]) != T_NUM) { buf[i] = 0; continue; }
    int code = (int) tod(args[i]);
    buf[i] = (char)(code & 0xFF);
  }
  buf[nargs] = '\0';

  ant_value_t ret = js_mkstr(js, buf, nargs);
  free(buf);
  return ret;
}

static ant_value_t builtin_string_fromCodePoint(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return js_mkstr(js, "", 0);

  char *buf = (char *)ant_calloc(nargs * 4 + 1);
  if (!buf) return js_mkerr(js, "oom");

  size_t len = 0;
  for (int i = 0; i < nargs; i++) {
    if (vtype(args[i]) != T_NUM) continue;
    double d = tod(args[i]);
    if (d < 0 || d > 0x10FFFF || d != floor(d)) {
      free(buf);
      return js_mkerr_typed(js, JS_ERR_RANGE, "Invalid code point");
    }
    uint32_t cp = (uint32_t)d;
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
  buf[len] = '\0';

  ant_value_t ret = js_mkstr(js, buf, len);
  free(buf);
  return ret;
}

static bool string_builder_append_value(
  ant_t *js, char **buf,
  size_t *len, size_t *cap,
  ant_value_t value, ant_value_t *err
) {
  ant_value_t s = js_tostring_val(js, value);
  if (is_err(s)) {
    if (err) *err = s;
    return false;
  }

  ant_offset_t slen = 0;
  ant_offset_t soff = vstr(js, s, &slen);
  
  size_t need = *len + (size_t)slen + 1;
  if (need > *cap) {
    size_t next = (*cap == 0) ? 64 : *cap;
    while (next < need) next *= 2;
    char *grown = (char *)realloc(*buf, next);
    if (!grown) {
      if (err) *err = js_mkerr(js, "oom");
      return false;
    }
    *buf = grown;
    *cap = next;
  }

  if (slen > 0) memcpy(*buf + *len, (const void *)(uintptr_t)soff, (size_t)slen);
  *len += (size_t)slen;
  (*buf)[*len] = '\0';
  return true;
}

static ant_value_t builtin_string_raw(ant_t *js, ant_value_t *args, int nargs) {
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

  char *buf = NULL;
  size_t len = 0; size_t cap = 0;
  ant_value_t err = js_mkundef();

  for (size_t i = 0; i < literal_count; i++) {
    ant_value_t chunk = js_mkundef();
    if (vtype(raw) == T_ARR) chunk = js_arr_get(js, raw, (ant_offset_t)i);
    else {
      char key[32];
      snprintf(key, sizeof(key), "%zu", i);
      chunk = js_get(js, raw, key);
    }

    if (!string_builder_append_value(js, &buf, &len, &cap, chunk, &err)) {
      free(buf);
      return is_err(err) ? err : js_mkerr(js, "oom");
    }

    if (i + 1 < literal_count && (int)(i + 1) < nargs) {
      if (!string_builder_append_value(js, &buf, &len, &cap, args[i + 1], &err)) {
        free(buf); return is_err(err) ? err : js_mkerr(js, "oom");
      }
    }
  }

  ant_value_t out = js_mkstr(js, buf ? buf : "", len);
  free(buf);
  
  return out;
}

static ant_value_t builtin_number_toString(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t num = unwrap_primitive(js, js->this_val);
  if (vtype(num) != T_NUM) return js_mkerr(js, "toString called on non-number");
  
  int radix = 10;
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    radix = (int)tod(args[0]);
    if (radix < 2 || radix > 36) {
      return js_mkerr(js, "radix must be between 2 and 36");
    }
  }
  
  if (radix == 10) {
    char buf[64];
    size_t len = strnum(num, buf, sizeof(buf));
    return js_mkstr(js, buf, len);
  }
  
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

static ant_value_t builtin_number_toFixed(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t num = unwrap_primitive(js, js->this_val);
  if (vtype(num) != T_NUM) return js_mkerr(js, "toFixed called on non-number");
  
  double d = tod(num);
  if (isnan(d)) return js_mkstr(js, "NaN", 3);
  if (isinf(d)) return d > 0 ? js_mkstr(js, "Infinity", 8) : js_mkstr(js, "-Infinity", 9);
  
  int digits = 0;
  if (nargs >= 1 && vtype(args[0]) != T_UNDEF) {
    digits = (int) tod(args[0]);
    if (digits < 0 || digits > 100) {
      return js_mkerr_typed(js, JS_ERR_RANGE, "toFixed() digits argument must be between 0 and 100");
    }
  }
  
  bool negative = d < 0;
  if (negative) d = -d;
  
  if (d >= 1e21) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.0f", negative ? -d : d);
    return js_mkstr(js, buf, strlen(buf));
  }
  
  double scale = pow(10, digits);
  double scaled = d * scale;
  double rounded = floor(scaled + 0.5);
  
  char digit_buf[128];
  snprintf(digit_buf, sizeof(digit_buf), "%.0f", rounded);
  int digit_len = (int)strlen(digit_buf);
  
  while (digit_len < digits + 1) {
    memmove(digit_buf + 1, digit_buf, digit_len + 1);
    digit_buf[0] = '0';
    digit_len++;
  }
  
  char buf[128];
  int pos = 0;
  
  if (negative && rounded != 0) buf[pos++] = '-';
  int int_digits = digit_len - digits;
  if (int_digits <= 0) int_digits = 1;
  
  for (int i = 0; i < int_digits; i++) {
    buf[pos++] = digit_buf[i];
  }
  
  if (digits > 0) {
    buf[pos++] = '.';
    for (int i = int_digits; i < digit_len; i++) {
      buf[pos++] = digit_buf[i];
    }
  }
  
  buf[pos] = '\0';
  return js_mkstr(js, buf, pos);
}

static ant_value_t builtin_number_toPrecision(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t num = unwrap_primitive(js, js->this_val);
  if (vtype(num) != T_NUM) return js_mkerr(js, "toPrecision called on non-number");
  
  double d = tod(num);
  if (isnan(d)) return js_mkstr(js, "NaN", 3);
  if (isinf(d)) return d > 0 ? js_mkstr(js, "Infinity", 8) : js_mkstr(js, "-Infinity", 9);
  
  if (nargs < 1 || vtype(args[0]) == T_UNDEF) {
    char buf[64];
    size_t len = strnum(num, buf, sizeof(buf));
    return js_mkstr(js, buf, len);
  }
  
  int precision = (int) tod(args[0]);
  if (precision < 1 || precision > 100) {
    return js_mkerr_typed(js, JS_ERR_RANGE, "toPrecision() argument must be between 1 and 100");
  }
  
  bool negative = d < 0;
  if (negative) d = -d;
  
  if (d == 0) {
    char buf[128];
    int pos = 0;
    if (negative) buf[pos++] = '-';
    buf[pos++] = '0';
    if (precision > 1) {
      buf[pos++] = '.';
      for (int i = 1; i < precision; i++) buf[pos++] = '0';
    }
    buf[pos] = '\0';
    return js_mkstr(js, buf, pos);
  }
  
  int exp = (int) floor(log10(d));
  bool use_exp = (exp < -(precision - 1) - 1) || (exp >= precision);
  
  if (use_exp) {
    double mantissa = d / pow(10, exp);
    double scale = pow(10, precision - 1);
    double rounded = floor(mantissa * scale + 0.5);
    
    if (rounded >= scale * 10) {
      rounded /= 10;
      exp++;
    }
    
    char digit_buf[32];
    snprintf(digit_buf, sizeof(digit_buf), "%.0f", rounded);
    int digit_len = (int)strlen(digit_buf);
    
    char buf[128];
    int pos = 0;
    if (negative) buf[pos++] = '-';
    buf[pos++] = digit_buf[0];
    if (precision > 1) {
      buf[pos++] = '.';
      for (int i = 1; i < precision; i++) {
        buf[pos++] = (i < digit_len) ? digit_buf[i] : '0';
      }
    }
    buf[pos++] = 'e';
    buf[pos++] = (exp >= 0) ? '+' : '-';
    if (exp < 0) exp = -exp;
    snprintf(buf + pos, sizeof(buf) - pos, "%d", exp);
    return js_mkstr(js, buf, strlen(buf));
  } else {
    int digits_after_point = precision - exp - 1;
    if (digits_after_point < 0) digits_after_point = 0;
    
    double scale = pow(10, digits_after_point);
    double rounded = floor(d * scale + 0.5);
    
    char digit_buf[64];
    snprintf(digit_buf, sizeof(digit_buf), "%.0f", rounded);
    int digit_len = (int)strlen(digit_buf);
    
    while (digit_len < digits_after_point + 1) {
      memmove(digit_buf + 1, digit_buf, digit_len + 1);
      digit_buf[0] = '0';
      digit_len++;
    }
    
    char buf[128];
    int pos = 0;
    if (negative) buf[pos++] = '-';
    
    int int_digits = digit_len - digits_after_point;
    for (int i = 0; i < int_digits; i++) {
      buf[pos++] = digit_buf[i];
    }
    
    if (digits_after_point > 0) {
      buf[pos++] = '.';
      for (int i = int_digits; i < digit_len; i++) {
        buf[pos++] = digit_buf[i];
      }
    }
    
    buf[pos] = '\0';
    return js_mkstr(js, buf, pos);
  }
}

static ant_value_t builtin_number_toExponential(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t num = unwrap_primitive(js, js->this_val);
  if (vtype(num) != T_NUM) return js_mkerr(js, "toExponential called on non-number");
  
  double d = tod(num);
  if (isnan(d)) return js_mkstr(js, "NaN", 3);
  if (isinf(d)) return d > 0 ? js_mkstr(js, "Infinity", 8) : js_mkstr(js, "-Infinity", 9);
  
  int digits = -1;
  if (nargs >= 1 && vtype(args[0]) != T_UNDEF) {
    digits = (int) tod(args[0]);
    if (digits < 0 || digits > 100) {
      return js_mkerr_typed(js, JS_ERR_RANGE, "toExponential() argument must be between 0 and 100");
    }
  }
  
  bool negative = d < 0;
  if (negative) d = -d;
  
  int exp = 0;
  if (d != 0) {
    exp = (int) floor(log10(d));
    double test = d / pow(10, exp);
    if (test >= 10) { exp++; test /= 10; }
    if (test < 1) { exp--; test *= 10; }
  }
  
  if (digits < 0) {
    char temp[32];
    snprintf(temp, sizeof(temp), "%.15g", d);
    int sig = 0;
    for (int i = 0; temp[i] && temp[i] != 'e' && temp[i] != 'E'; i++) {
      if (temp[i] == '.') continue;
      if (temp[i] >= '0' && temp[i] <= '9') if (temp[i] != '0' || sig > 0) sig++;
    }
    digits = sig > 0 ? sig - 1 : 0;
    if (digits > 20) digits = 20;
  }
  
  double mantissa = d / pow(10, exp);
  double scale = pow(10, digits);
  double scaled = mantissa * scale;
  double rounded = floor(scaled + 0.5);
  
  if (rounded >= scale * 10) {
    rounded /= 10;
    exp++;
  }
  
  char buf[64];
  int pos = 0;
  
  if (negative) buf[pos++] = '-';
  
  char digit_buf[32];
  snprintf(digit_buf, sizeof(digit_buf), "%.0f", rounded);
  int digit_len = (int)strlen(digit_buf);
  
  while (digit_len < digits + 1) {
    memmove(digit_buf + 1, digit_buf, digit_len + 1);
    digit_buf[0] = '0';
    digit_len++;
  }
  
  buf[pos++] = digit_buf[0];
  
  if (digits > 0) {
    buf[pos++] = '.';
    for (int i = 1; i <= digits; i++) {
      buf[pos++] = (i < digit_len) ? digit_buf[i] : '0';
    }
  }
  
  buf[pos++] = 'e';
  buf[pos++] = (exp >= 0) ? '+' : '-';
  if (exp < 0) exp = -exp;
  snprintf(buf + pos, sizeof(buf) - pos, "%d", exp);
  
  return js_mkstr(js, buf, strlen(buf));
}

static ant_value_t builtin_number_valueOf(ant_t *js, ant_value_t *args, int nargs) {
  (void) args; (void) nargs;
  ant_value_t num = unwrap_primitive(js, js->this_val);
  if (vtype(num) != T_NUM) return js_mkerr(js, "valueOf called on non-number");
  return num;
}

static ant_value_t builtin_number_toLocaleString(ant_t *js, ant_value_t *args, int nargs) {
  (void) args; (void) nargs;
  ant_value_t num = unwrap_primitive(js, js->this_val);
  if (vtype(num) != T_NUM) return js_mkerr(js, "toLocaleString called on non-number");
  double d = tod(num);
  char raw[64];
  strnum(num, raw, sizeof(raw));
  if (!isfinite(d) || strchr(raw, 'e') || strchr(raw, 'E'))
    return js_mkstr(js, raw, strlen(raw));
  char *dot = strchr(raw, '.');
  size_t int_len = dot ? (size_t)(dot - raw) : strlen(raw);
  size_t start = (raw[0] == '-') ? 1 : 0;
  size_t frac_len = dot ? strlen(dot) : 0;
  char buf[128];
  size_t pos = 0;
  if (start) buf[pos++] = '-';
  for (size_t i = start; i < int_len; i++) {
    buf[pos++] = raw[i];
    size_t remaining = int_len - 1 - i;
    if (remaining > 0 && remaining % 3 == 0) buf[pos++] = ',';
  }
  if (frac_len) memcpy(buf + pos, dot, frac_len);
  pos += frac_len;
  buf[pos] = '\0';
  return js_mkstr(js, buf, pos);
}

static ant_value_t builtin_string_valueOf(ant_t *js, ant_value_t *args, int nargs) {
  (void) args; (void) nargs;
  ant_value_t str = to_string_val(js, js->this_val);
  if (vtype(str) != T_STR) return js_mkerr(js, "valueOf called on non-string");
  return str;
}

static ant_value_t builtin_string_toString(ant_t *js, ant_value_t *args, int nargs) {
  return builtin_string_valueOf(js, args, nargs);
}

static ant_value_t builtin_boolean_valueOf(ant_t *js, ant_value_t *args, int nargs) {
  (void) args; (void) nargs;
  ant_value_t b = unwrap_primitive(js, js->this_val);
  if (vtype(b) != T_BOOL) return js_mkerr(js, "valueOf called on non-boolean");
  return b;
}

static ant_value_t builtin_boolean_toString(ant_t *js, ant_value_t *args, int nargs) {
  (void) args; (void) nargs;
  ant_value_t b = unwrap_primitive(js, js->this_val);
  if (vtype(b) != T_BOOL) return js_mkerr(js, "toString called on non-boolean");
  return vdata(b) ? js_mkstr(js, "true", 4) : js_mkstr(js, "false", 5);
}

static ant_value_t builtin_parseInt(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return tov(JS_NAN);
  
  ant_value_t str_val = args[0];
  if (vtype(str_val) != T_STR) {
    const char *str = js_str(js, str_val);
    str_val = js_mkstr(js, str, strlen(str));
  }
  
  ant_offset_t str_len, str_off = vstr(js, str_val, &str_len);
  const char *str = (char *)(uintptr_t)(str_off);
  
  int radix = 0;
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
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

static ant_value_t builtin_parseFloat(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return tov(JS_NAN);
  
  ant_value_t str_val = args[0];
  if (vtype(str_val) != T_STR) {
    const char *str = js_str(js, str_val);
    str_val = js_mkstr(js, str, strlen(str));
  }
  
  ant_offset_t str_len, str_off = vstr(js, str_val, &str_len);
  const char *str = (char *)(uintptr_t)(str_off);
  
  ant_offset_t i = 0;
  while (i < str_len && is_space(str[i])) i++;
  
  if (i >= str_len) return tov(JS_NAN);
  
  char *end;
  double result = strtod(&str[i], &end);
  
  if (end == &str[i]) return tov(JS_NAN);
  
  return tov(result);
}

static ant_value_t builtin_btoa(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "btoa requires 1 argument");
  
  ant_value_t str_val = args[0];
  if (vtype(str_val) != T_STR) {
    const char *str = js_str(js, str_val);
    str_val = js_mkstr(js, str, strlen(str));
  }
  
  ant_offset_t str_len, str_off = vstr(js, str_val, &str_len);
  const char *str = (char *)(uintptr_t)(str_off);
  
  size_t out_len;
  char *out = ant_base64_encode((const uint8_t *)str, str_len, &out_len);
  if (!out) return js_mkerr(js, "out of memory");
  
  ant_value_t result = js_mkstr(js, out, out_len);
  free(out);
  
  return result;
}

static ant_value_t builtin_atob(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "atob requires 1 argument");
  
  ant_value_t str_val = args[0];
  if (vtype(str_val) != T_STR) {
    const char *str = js_str(js, str_val);
    str_val = js_mkstr(js, str, strlen(str));
  }
  
  ant_offset_t str_len, str_off = vstr(js, str_val, &str_len);
  const char *str = (char *)(uintptr_t)(str_off);
  if (str_len == 0) return js_mkstr(js, "", 0);
  
  size_t out_len;
  uint8_t *out = ant_base64_decode(str, str_len, &out_len);
  if (!out) return js_mkerr(js, "atob: invalid base64 string");
  
  ant_value_t result = js_mkstr(js, (char *)out, out_len);
  free(out);
  
  return result;
}

static ant_value_t builtin_resolve_internal(ant_t *js, ant_value_t *args, int nargs);
static ant_value_t builtin_reject_internal(ant_t *js, ant_value_t *args, int nargs);

static size_t strpromise(ant_t *js, ant_value_t value, char *buf, size_t len) {
  uint32_t pid = get_promise_id(js, value);
  ant_promise_state_t *pd = get_promise_data(js, value, false);
  
  const char *content;
  char *allocated = NULL;
  
  if (!pd || pd->state == 0) {
    content = "<pending>";
  } else if (pd->state == 2) {
    char *val = tostr_alloc(js, pd->value);
    allocated = ant_calloc(strlen(val) + 12);
    sprintf(allocated, "<rejected> %s", val);
    free(val);
    content = allocated;
  } else { content = allocated = tostr_alloc(js, pd->value); }
  
  uint32_t trigger_pid = 0;
  if (pd && vtype(pd->trigger_parent) == T_PROMISE) {
    trigger_pid = get_promise_id(js, pd->trigger_parent);
  }
  size_t result = trigger_pid
    ? (size_t)snprintf(buf, len, "Promise {\n  %s,\n  Symbol(async_id): %u,\n  Symbol(trigger_async_id): %u\n}", content, pid, trigger_pid)
    : (size_t)snprintf(buf, len, "Promise {\n  %s,\n  Symbol(async_id): %u\n}", content, pid);
  
  if (allocated) free(allocated);
  return result;
}

static ant_promise_state_t *get_promise_data(ant_t *js, ant_value_t promise, bool create) {
  if (vtype(promise) != T_PROMISE) return NULL;
  ant_object_t *obj = js_obj_ptr(js_as_obj(promise));
  if (!obj) return NULL;
  if (obj->promise_state) return obj->promise_state;
  if (!create) return NULL;

  ant_promise_state_t *entry = (ant_promise_state_t *)calloc(1, sizeof(*entry));
  if (!entry) return NULL;
  entry->promise_id = next_promise_id++;
  entry->trigger_parent = js_mkundef();
  entry->inline_handler = (promise_handler_t){ 0 };
  entry->handlers = NULL;
  entry->handler_count = 0;
  entry->state = 0;
  entry->value = js_mkundef();
  entry->trigger_queued = false;
  entry->has_rejection_handler = false;
  entry->processing = false;
  entry->unhandled_reported = false;
  obj->promise_state = entry;
  obj->type_tag = T_PROMISE;
  return entry;
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
  ant_value_t (*fn)(ant_t *, ant_value_t *, int)
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
  set_slot(obj, SLOT_CFUNC, js_mkfun(fn));

  ant_value_t func = js_obj_to_func(obj);
  GC_ROOT_RESTORE(js, root_mark);
  return func;
}

ant_value_t js_mkpromise(ant_t *js) {
  ant_value_t obj = mkobj(js, 0);
  if (is_err(obj)) return obj;
  if (!get_promise_data(js, mkval(T_PROMISE, vdata(obj)), true))
    return js_mkerr(js, "out of memory");

  ant_value_t promise_ctor = js_get(js, js_glob(js), "Promise");
  if (vtype(promise_ctor) == T_FUNC || vtype(promise_ctor) == T_CFUNC) {
    set_slot(obj, SLOT_CTOR, promise_ctor);
  }

  ant_value_t promise_proto = get_ctor_proto(js, "Promise", 7);
  if (is_object_type(promise_proto)) {
    js_set_proto_init(obj, promise_proto);
  }
  
  return mkval(T_PROMISE, vdata(obj));
}

ant_value_t js_promise_then(ant_t *js, ant_value_t promise, ant_value_t on_fulfilled, ant_value_t on_rejected) {
  ant_value_t args_then[2] = { on_fulfilled, on_rejected };
  ant_value_t saved_this = js->this_val;
  
  js->this_val = promise;
  ant_value_t result = builtin_promise_then(js, args_then, 2);
  js->this_val = saved_this;
  
  return result;
}

static inline ant_value_t js_get_thenable_then(ant_t *js, ant_value_t value) {
  if (!is_object_type(value)) return js_mkundef();
  return js_getprop_fallback(js, value, "then");
}

js_await_result_t js_promise_await_coroutine(ant_t *js, ant_value_t promise, coroutine_t *coro) {
  js_await_result_t result = {
    .state = JS_AWAIT_PENDING,
    .value = js_mkundef(),
  };

  if (vtype(promise) != T_PROMISE || !coro) return result;
  ant_promise_state_t *pd = get_promise_data(js, promise, false);
  
  if (!pd) {
    result.state = JS_AWAIT_FULFILLED;
    return result;
  }

  // if (pd->state == 1) {
  //   result.state = JS_AWAIT_FULFILLED;
  //   result.value = pd->value;
  //     return result;
  // }
  //
  // if (pd->state == 2) {
  //   if (pd->unhandled_reported) js_fire_rejection_handled(js, promise, pd->value);
  //   pd->has_rejection_handler = true;
  //   pd->unhandled_reported = false;
  //   result.state = JS_AWAIT_REJECTED;
  //   result.value = pd->value;
  //   return result;
  // }

  promise_handler_t h = { js_mkundef(), js_mkundef(), js_mkundef(), coro };
  if (!promise_handler_append(pd, &h)) {
    result.state = JS_AWAIT_REJECTED;
    result.value = js_mkerr(js, "out of memory");
    return result;
  }

  if (pd->unhandled_reported) js_fire_rejection_handled(js, promise, pd->value);
  pd->has_rejection_handler = true;
  pd->unhandled_reported = false;

  if (pd->state == 0) gc_root_pending_promise(js_obj_ptr(js_as_obj(promise)));
  else queue_promise_trigger(js, promise);

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
  
  gc_root_pending_promise(pobj);
  pd->processing = true;
  
  for (uint32_t i = 0; i < len; i++) {
    promise_handler_t *h = promise_handler_at(pd, i);
    if (!h) continue;
    
    if (h->await_coro) {
      settle_and_resume_coroutine(js, h->await_coro, val, state != 1);
      continue;
    }
    
    ant_value_t handler = (state == 1) ? h->onFulfilled : h->onRejected;
    if (vtype(handler) != T_FUNC && vtype(handler) != T_CFUNC) {
      if (state == 1) js_resolve_promise(js, h->nextPromise, val);
      else js_reject_promise(js, h->nextPromise, val);
      continue;
    }
    
    ant_value_t res = js_mkundef();
    if (vtype(handler) == T_CFUNC) {
      ant_value_t (*fn)(ant_t *, ant_value_t *, int) = (ant_value_t(*)(ant_t *, ant_value_t *, int))vdata(handler);
      res = fn(js, &val, 1);
    } else {
      ant_value_t call_args[] = { val };
      res = sv_vm_call(js->vm, js, handler, js_mkundef(), call_args, 1, NULL, false);
    }
    
    if (!is_err(res)) {
      js_resolve_promise(js, h->nextPromise, res);
      continue;
    }
    
    ant_value_t reject_val = js->thrown_value;
    if (vtype(reject_val) == T_UNDEF) reject_val = res;
    
    js->thrown_exists = false;
    js->thrown_value = js_mkundef();
    js->thrown_stack = js_mkundef();
    js_reject_promise(js, h->nextPromise, reject_val);
  }

  pd->processing = false;
  promise_handlers_clear(pd);
  gc_unroot_pending_promise(js_obj_ptr(promise));
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

  if (vtype(val) == T_PROMISE) {
    if (vdata(js_as_obj(val)) == vdata(js_as_obj(p))) {
      ant_value_t err = js_mkerr(js, "TypeError: Chaining cycle");
      GC_ROOT_PIN(js, err);
      js_reject_promise(js, p, err);
      GC_ROOT_RESTORE(js, root_mark);
      return;
    }

    ant_promise_state_t *src_pd = get_promise_data(js, val, false);
    if (!src_pd) {
      pd->state = 1;
      pd->value = val;
      gc_write_barrier(js, js_obj_ptr(js_as_obj(p)), val);
      GC_ROOT_RESTORE(js, root_mark);
      return;
    }

    pd->trigger_parent = val;

    promise_handler_t h = { js_mkundef(), js_mkundef(), p, NULL };
    if (!promise_handler_append(src_pd, &h)) {
      pd->state = 2;
      pd->value = js_mkerr(js, "out of memory");
      gc_write_barrier(js, js_obj_ptr(js_as_obj(p)), pd->value);
      GC_ROOT_RESTORE(js, root_mark);
      return;
    }
    
    gc_write_barrier(js, js_obj_ptr(js_as_obj(val)), p);
    if (src_pd->unhandled_reported) js_fire_rejection_handled(js, val, src_pd->value);
    
    src_pd->has_rejection_handler = true;
    src_pd->unhandled_reported = false;

    if (src_pd->state == 0) gc_root_pending_promise(js_obj_ptr(js_as_obj(val)));
    else queue_promise_trigger(js, val);
    
    GC_ROOT_RESTORE(js, root_mark);
    
    return;
  }

  if (is_object_type(val)) {
    ant_value_t res_fn = make_data_cfunc(js, p, builtin_resolve_internal);
    GC_ROOT_PIN(js, res_fn);
    
    if (is_err(res_fn)) {
      js_reject_promise(js, p, res_fn);
      GC_ROOT_RESTORE(js, root_mark);
      return;
    }
    
    ant_value_t rej_fn = make_data_cfunc(js, p, builtin_reject_internal);
    GC_ROOT_PIN(js, rej_fn);
    
    if (is_err(rej_fn)) {
      js_reject_promise(js, p, rej_fn);
      GC_ROOT_RESTORE(js, root_mark);
      return;
    }
    
    ant_value_t then_prop = js_get_thenable_then(js, val);
    GC_ROOT_PIN(js, then_prop);
    
    if (vtype(then_prop) == T_FUNC || vtype(then_prop) == T_CFUNC) {
      ant_value_t call_args[] = { res_fn, rej_fn };
      sv_vm_call(js->vm, js, then_prop, val, call_args, 2, NULL, false);
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

void js_reject_promise(ant_t *js, ant_value_t p, ant_value_t val) {
  if (vtype(val) == T_ERR) {
    if (vdata(val) != 0) val = mkval(T_OBJ, vdata(val));
    else if (js->thrown_exists && is_object_type(js->thrown_value)) val = js->thrown_value;
    else val = js_make_error_silent(js, JS_ERR_INTERNAL, "unknown error");
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

static ant_value_t builtin_resolve_internal(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t me = js->current_func;
  ant_value_t p = get_slot(me, SLOT_DATA);
  if (vtype(p) != T_PROMISE) return js_mkundef();
  js_resolve_promise(js, p, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static ant_value_t builtin_reject_internal(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t me = js->current_func;
  ant_value_t p = get_slot(me, SLOT_DATA);
  if (vtype(p) != T_PROMISE) return js_mkundef();
  js_reject_promise(js, p, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static ant_value_t builtin_Promise(ant_t *js, ant_value_t *args, int nargs) {
  if (vtype(js->new_target) == T_UNDEF) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Promise constructor cannot be invoked without 'new'");
  }
  
  if (nargs == 0 || (vtype(args[0]) != T_FUNC && vtype(args[0]) != T_CFUNC)) {
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

  ant_value_t new_target = js->new_target;
  ant_value_t p_obj = js_as_obj(p);

  ant_value_t promise_proto = get_ctor_proto(js, "Promise", 7);
  ant_value_t instance_proto = js_instance_proto_from_new_target(js, promise_proto);
  GC_ROOT_PIN(js, instance_proto);

  if (vtype(new_target) == T_FUNC || vtype(new_target) == T_CFUNC) set_slot(p_obj, SLOT_CTOR, new_target);
  if (is_object_type(instance_proto)) js_set_proto_init(p_obj, instance_proto);

  ant_value_t res_fn = make_data_cfunc(js, p, builtin_resolve_internal);
  GC_ROOT_PIN(js, res_fn);
  if (is_err(res_fn)) {
    GC_ROOT_RESTORE(js, root_mark);
    return res_fn;
  }

  ant_value_t rej_fn = make_data_cfunc(js, p, builtin_reject_internal);
  GC_ROOT_PIN(js, rej_fn);
  if (is_err(rej_fn)) {
    GC_ROOT_RESTORE(js, root_mark);
    return rej_fn;
  }

  ant_value_t exec_args[] = { res_fn, rej_fn };
  sv_vm_call(js->vm, js, executor, js_mkundef(), exec_args, 2, NULL, false);

  GC_ROOT_RESTORE(js, root_mark);
  return p;
}

static ant_value_t builtin_Promise_resolve(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t val = nargs > 0 ? args[0] : js_mkundef();
  if (vtype(val) == T_PROMISE) return val;
  ant_value_t p = js_mkpromise(js);
  js_resolve_promise(js, p, val);
  return p;
}

static ant_value_t builtin_Promise_reject(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t val = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t p = js_mkpromise(js);
  js_reject_promise(js, p, val);
  return p;
}

static ant_value_t promise_species_noop_executor(ant_t *js, ant_value_t *args, int nargs) {
  return js_mkundef();
}

static inline bool is_same_heap_value(ant_value_t a, ant_value_t b) {
  return vtype(a) == vtype(b) && vdata(a) == vdata(b);
}

static inline bool is_constructor_value(ant_value_t value) {
  return vtype(value) == T_FUNC || vtype(value) == T_CFUNC;
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
  if (vtype(parent_proto) == T_OBJ) js_set_proto_init(next_obj, parent_proto);

  ant_value_t parent_ctor = get_slot(parent_obj, SLOT_CTOR);
  if (is_constructor_value(parent_ctor)) set_slot(next_obj, SLOT_CTOR, parent_ctor);
}

static ant_value_t builtin_promise_then(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t p = js->this_val;
  if (vtype(p) != T_PROMISE) return js_mkerr(js, "not a promise");

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, p);

  ant_value_t promise_ctor = js_get(js, js_glob(js), "Promise");
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
  if (vtype(ctor) == T_UNDEF) ctor = get_slot(p_obj, SLOT_CTOR);

  ant_value_t species = get_ctor_species_value(js, ctor);
  GC_ROOT_PIN(js, species);
  if (is_err(species)) {
    GC_ROOT_RESTORE(js, root_mark);
    return species;
  }

  if (vtype(species) == T_FUNC || vtype(species) == T_CFUNC) {
    species_ctor = species;
  } else if (vtype(species) == T_NULL) {
    species_ctor = promise_ctor;
  } else if (vtype(species) != T_UNDEF) {
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

    if (vtype(onRejected) == T_FUNC || vtype(onRejected) == T_CFUNC) {
      if (pd->unhandled_reported) js_fire_rejection_handled(js, p, pd->value);
      pd->has_rejection_handler = true;
      pd->unhandled_reported = false;
    }

    if (pd->state == 0)
      gc_root_pending_promise(js_obj_ptr(p));
  }

  if (pd && pd->state != 0) queue_promise_trigger(js, p);
  GC_ROOT_RESTORE(js, root_mark);
  
  return nextP;
}

static ant_value_t builtin_promise_catch(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t args_then[] = { js_mkundef(), nargs > 0 ? args[0] : js_mkundef() };
  return builtin_promise_then(js, args_then, 2);
}

static ant_value_t finally_value_thunk(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t me = js->current_func;
  return get_slot(me, SLOT_DATA);
}

static ant_value_t finally_thrower(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t me = js->current_func;
  ant_value_t reason = get_slot(me, SLOT_DATA);
  ant_value_t rejected = js_mkpromise(js);
  js_reject_promise(js, rejected, reason);
  return rejected;
}

static ant_value_t finally_identity_reject(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t reason = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t rejected = js_mkpromise(js);
  js_reject_promise(js, rejected, reason);
  return rejected;
}

static ant_value_t finally_fulfilled_wrapper(ant_t *js, ant_value_t *args, int nargs) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t me = js->current_func;
  ant_value_t callback = get_slot(me, SLOT_DATA);
  ant_value_t value = nargs > 0 ? args[0] : js_mkundef();
  GC_ROOT_PIN(js, callback);
  GC_ROOT_PIN(js, value);

  ant_value_t result = js_mkundef();
  if (vtype(callback) == T_FUNC || vtype(callback) == T_CFUNC) {
    result = sv_vm_call(js->vm, js, callback, js_mkundef(), NULL, 0, NULL, false);
    if (is_err(result)) {
      GC_ROOT_RESTORE(js, root_mark);
      return result;
    }
  }
  GC_ROOT_PIN(js, result);

  if (vtype(result) == T_PROMISE) {
    ant_value_t thunk_fn = make_data_cfunc(js, value, finally_value_thunk);
    GC_ROOT_PIN(js, thunk_fn);
    
    if (is_err(thunk_fn)) {
      GC_ROOT_RESTORE(js, root_mark);
      return thunk_fn;
    }

    ant_value_t identity_rej_fn = js_mkfun(finally_identity_reject);
    ant_value_t ret = js_promise_then(js, result, thunk_fn, identity_rej_fn);
    GC_ROOT_RESTORE(js, root_mark);
    
    return ret;
  }

  if (is_object_type(result)) {
  ant_value_t then_fn = js_get_thenable_then(js, result);
  GC_ROOT_PIN(js, then_fn);
  
  if (vtype(then_fn) == T_FUNC || vtype(then_fn) == T_CFUNC) {
    ant_value_t thunk_fn = make_data_cfunc(js, value, finally_value_thunk);
    GC_ROOT_PIN(js, thunk_fn);
    
    if (is_err(thunk_fn)) {
      GC_ROOT_RESTORE(js, root_mark);
      return thunk_fn;
    }
    
    ant_value_t identity_rej_fn = js_mkfun(finally_identity_reject);
    ant_value_t call_args[] = { thunk_fn, identity_rej_fn };
    ant_value_t ret = sv_vm_call(js->vm, js, then_fn, result, call_args, 2, NULL, false);
    GC_ROOT_RESTORE(js, root_mark);
    
    return ret;
  }}

  GC_ROOT_RESTORE(js, root_mark);
  return value;
}

static ant_value_t finally_rejected_wrapper(ant_t *js, ant_value_t *args, int nargs) {
  GC_ROOT_SAVE(root_mark, js);
  
  ant_value_t me = js->current_func;
  ant_value_t callback = get_slot(me, SLOT_DATA);
  ant_value_t reason = nargs > 0 ? args[0] : js_mkundef();
  
  GC_ROOT_PIN(js, callback);
  GC_ROOT_PIN(js, reason);

  ant_value_t result = js_mkundef();
  if (vtype(callback) == T_FUNC || vtype(callback) == T_CFUNC) {
    result = sv_vm_call(js->vm, js, callback, js_mkundef(), NULL, 0, NULL, false);
    if (is_err(result)) {
      GC_ROOT_RESTORE(js, root_mark);
      return result;
    }
  }
  GC_ROOT_PIN(js, result);

  if (vtype(result) == T_PROMISE) {
    ant_value_t thrower_fn = make_data_cfunc(js, reason, finally_thrower);
    GC_ROOT_PIN(js, thrower_fn);
    if (is_err(thrower_fn)) {
      GC_ROOT_RESTORE(js, root_mark);
      return thrower_fn;
    }
    ant_value_t identity_rej_fn = js_mkfun(finally_identity_reject);
    ant_value_t ret = js_promise_then(js, result, thrower_fn, identity_rej_fn);
    GC_ROOT_RESTORE(js, root_mark);
    return ret;
  }

  if (is_object_type(result)) {
  ant_value_t then_prop = js_get_thenable_then(js, result);
  GC_ROOT_PIN(js, then_prop);
  
  if (vtype(then_prop) == T_FUNC || vtype(then_prop) == T_CFUNC) {
    ant_value_t thrower_fn = make_data_cfunc(js, reason, finally_thrower);
    GC_ROOT_PIN(js, thrower_fn);
    
    if (is_err(thrower_fn)) {
      GC_ROOT_RESTORE(js, root_mark);
      return thrower_fn;
    }
    
    ant_value_t identity_rej_fn = js_mkfun(finally_identity_reject);
    ant_value_t call_args[] = { thrower_fn, identity_rej_fn };
    ant_value_t ret = sv_vm_call(js->vm, js, then_prop, result, call_args, 2, NULL, false);
    GC_ROOT_RESTORE(js, root_mark);
    
    return ret;
  }}

  ant_value_t rejected = js_mkpromise(js);
  GC_ROOT_PIN(js, rejected);
  js_reject_promise(js, rejected, reason);
  GC_ROOT_RESTORE(js, root_mark);
  
  return rejected;
}

static ant_value_t builtin_promise_finally(ant_t *js, ant_value_t *args, int nargs) {
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
  ant_value_t ret = builtin_promise_then(js, args_then, 2);
  GC_ROOT_RESTORE(js, root_mark);
  
  return ret;
}

static ant_value_t builtin_Promise_try(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs == 0) return builtin_Promise_resolve(js, args, 0);
  
  ant_value_t fn = args[0];
  ant_value_t *call_args = nargs > 1 ? &args[1] : NULL;
  int call_nargs = nargs > 1 ? nargs - 1 : 0;
  ant_value_t res = sv_vm_call(js->vm, js, fn, js_mkundef(), call_args, call_nargs, NULL, false);
  
  if (is_err(res)) {
    ant_value_t reject_val = js->thrown_value;
    if (vtype(reject_val) == T_UNDEF) reject_val = res;
    js->thrown_exists = false;
    js->thrown_value = js_mkundef();
    js->thrown_stack = js_mkundef();
    ant_value_t rej_args[] = { reject_val };
    return builtin_Promise_reject(js, rej_args, 1);
  }
  
  ant_value_t res_args[] = { res };
  return builtin_Promise_resolve(js, res_args, 1);
}

static ant_value_t builtin_Promise_withResolvers(ant_t *js, ant_value_t *args, int nargs) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t p = js_mkpromise(js);
  GC_ROOT_PIN(js, p);

  ant_value_t res_fn = make_data_cfunc(js, p, builtin_resolve_internal);
  GC_ROOT_PIN(js, res_fn);
  if (is_err(res_fn)) {
    GC_ROOT_RESTORE(js, root_mark);
    return res_fn;
  }

  ant_value_t rej_fn = make_data_cfunc(js, p, builtin_reject_internal);
  GC_ROOT_PIN(js, rej_fn);
  if (is_err(rej_fn)) {
    GC_ROOT_RESTORE(js, root_mark);
    return rej_fn;
  }

  ant_value_t result = js_newobj(js);
  GC_ROOT_PIN(js, result);
  js_setprop(js, result, js_mkstr(js, "promise", 7), p);
  js_setprop(js, result, js_mkstr(js, "resolve", 7), res_fn);
  js_setprop(js, result, js_mkstr(js, "reject", 6), rej_fn);

  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static ant_value_t mkpromise_with_ctor(ant_t *js, ant_value_t ctor) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, ctor);
  ant_value_t p = js_mkpromise(js);
  GC_ROOT_PIN(js, p);
  if (vtype(ctor) != T_FUNC && vtype(ctor) != T_CFUNC) {
    GC_ROOT_RESTORE(js, root_mark);
    return p;
  }

  ant_value_t proto = js_get(js, ctor, "prototype");
  if (is_err(proto)) {
    GC_ROOT_RESTORE(js, root_mark);
    return proto;
  }
  if (is_object_type(proto)) {
    ant_value_t p_obj = js_as_obj(p);
    set_slot(p_obj, SLOT_CTOR, ctor);
    js_set_proto_init(p_obj, proto);
  }
  GC_ROOT_RESTORE(js, root_mark);
  return p;
}

static ant_value_t builtin_Promise_all_resolve_handler(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t me = js->current_func;
  ant_value_t tracker = js_get(js, me, "tracker");
  ant_value_t index_val = js_get(js, me, "index");
  
  int index = (int)tod(index_val);
  ant_value_t value = nargs > 0 ? args[0] : js_mkundef();
  
  ant_value_t results = js_get(js, tracker, "results");
  arr_set(js, results, (ant_offset_t)index, value);
  
  ant_value_t remaining_val = js_get(js, tracker, "remaining");
  int remaining = (int)tod(remaining_val) - 1;
  js_setprop(js, tracker, js_mkstr(js, "remaining", 9), tov((double)remaining));
  
  if (remaining == 0) {
    ant_value_t result_promise = get_slot(tracker, SLOT_DATA);
    js_resolve_promise(js, result_promise, mkval(T_ARR, vdata(results)));
  }
  
  return js_mkundef();
}

static ant_value_t builtin_Promise_all_reject_handler(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t me = js->current_func;
  ant_value_t tracker = js_get(js, me, "tracker");
  ant_value_t result_promise = get_slot(tracker, SLOT_DATA);
  
  ant_value_t reason = nargs > 0 ? args[0] : js_mkundef();
  js_reject_promise(js, result_promise, reason);
  
  return js_mkundef();
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
  ant_value_t results = js_get(js, tracker, "results");
  ant_value_t result = promise_all_settled_make_result(js, fulfilled, value);
  ant_value_t remaining_val = 0;
  int remaining = 0;

  if (is_err(result)) return result;
  arr_set(js, results, (ant_offset_t)index, result);

  remaining_val = js_get(js, tracker, "remaining");
  remaining = (int)tod(remaining_val) - 1;
  js_setprop(js, tracker, js_mkstr(js, "remaining", 9), tov((double)remaining));

  if (remaining == 0) {
    ant_value_t result_promise = get_slot(tracker, SLOT_DATA);
    js_resolve_promise(js, result_promise, mkval(T_ARR, vdata(results)));
  }

  return js_mkundef();
}

static ant_value_t builtin_Promise_allSettled_resolve_handler(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t me = js->current_func;
  ant_value_t tracker = js_get(js, me, "tracker");
  ant_value_t index_val = js_get(js, me, "index");
  int index = (int)tod(index_val);
  ant_value_t value = nargs > 0 ? args[0] : js_mkundef();

  return promise_all_settled_store_result(js, tracker, index, true, value);
}

static ant_value_t builtin_Promise_allSettled_reject_handler(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t me = js->current_func;
  ant_value_t tracker = js_get(js, me, "tracker");
  ant_value_t index_val = js_get(js, me, "index");
  int index = (int)tod(index_val);
  ant_value_t reason = nargs > 0 ? args[0] : js_mkundef();

  return promise_all_settled_store_result(js, tracker, index, false, reason);
}

typedef struct {
  ant_value_t tracker;
  int index;
} promise_all_iter_ctx_t;

// TODO: move Promise combinator bookkeeping off JS-visible properties and into slots/native state
static iter_action_t promise_all_iter_cb(ant_t *js, ant_value_t value, void *ctx, ant_value_t *out) {
  GC_ROOT_SAVE(root_mark, js);
  promise_all_iter_ctx_t *pctx = (promise_all_iter_ctx_t *)ctx;
  ant_value_t item = value;
  GC_ROOT_PIN(js, item);

  if (vtype(item) != T_PROMISE) {
    ant_value_t wrap_args[] = { item };
    item = builtin_Promise_resolve(js, wrap_args, 1);
    GC_ROOT_PIN(js, item);
  }

  ant_value_t resolve_obj = mkobj(js, 0);
  if (is_err(resolve_obj)) {
    *out = resolve_obj;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }
  GC_ROOT_PIN(js, resolve_obj);
  set_slot(resolve_obj, SLOT_CFUNC, js_mkfun(builtin_Promise_all_resolve_handler));
  js_setprop(js, resolve_obj, js_mkstr(js, "index", 5), tov((double)pctx->index));
  js_setprop(js, resolve_obj, js_mkstr(js, "tracker", 7), pctx->tracker);
  ant_value_t resolve_fn = js_obj_to_func(resolve_obj);
  GC_ROOT_PIN(js, resolve_fn);

  ant_value_t reject_obj = mkobj(js, 0);
  if (is_err(reject_obj)) {
    *out = reject_obj;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }
  GC_ROOT_PIN(js, reject_obj);
  set_slot(reject_obj, SLOT_CFUNC, js_mkfun(builtin_Promise_all_reject_handler));
  js_setprop(js, reject_obj, js_mkstr(js, "tracker", 7), pctx->tracker);
  ant_value_t reject_fn = js_obj_to_func(reject_obj);
  GC_ROOT_PIN(js, reject_fn);

  ant_value_t then_args[] = { resolve_fn, reject_fn };
  ant_value_t saved_this = js->this_val;
  GC_ROOT_PIN(js, saved_this);
  js->this_val = item;
  ant_value_t then_result = builtin_promise_then(js, then_args, 2);
  js->this_val = saved_this;
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
  ant_value_t item = value;
  GC_ROOT_PIN(js, item);

  if (vtype(item) != T_PROMISE) {
    ant_value_t wrap_args[] = { item };
    item = builtin_Promise_resolve(js, wrap_args, 1);
    GC_ROOT_PIN(js, item);
  }

  ant_value_t resolve_obj = mkobj(js, 0);
  if (is_err(resolve_obj)) {
    *out = resolve_obj;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }
  GC_ROOT_PIN(js, resolve_obj);
  set_slot(resolve_obj, SLOT_CFUNC, js_mkfun(builtin_Promise_allSettled_resolve_handler));
  js_setprop(js, resolve_obj, js_mkstr(js, "index", 5), tov((double)pctx->index));
  js_setprop(js, resolve_obj, js_mkstr(js, "tracker", 7), pctx->tracker);
  ant_value_t resolve_fn = js_obj_to_func(resolve_obj);
  GC_ROOT_PIN(js, resolve_fn);

  ant_value_t reject_obj = mkobj(js, 0);
  if (is_err(reject_obj)) {
    *out = reject_obj;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }
  GC_ROOT_PIN(js, reject_obj);
  set_slot(reject_obj, SLOT_CFUNC, js_mkfun(builtin_Promise_allSettled_reject_handler));
  js_setprop(js, reject_obj, js_mkstr(js, "index", 5), tov((double)pctx->index));
  js_setprop(js, reject_obj, js_mkstr(js, "tracker", 7), pctx->tracker);
  ant_value_t reject_fn = js_obj_to_func(reject_obj);
  GC_ROOT_PIN(js, reject_fn);

  ant_value_t then_args[] = { resolve_fn, reject_fn };
  ant_value_t saved_this = js->this_val;
  GC_ROOT_PIN(js, saved_this);
  js->this_val = item;
  ant_value_t then_result = builtin_promise_then(js, then_args, 2);
  js->this_val = saved_this;
  if (is_err(then_result)) {
    *out = then_result;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }

  pctx->index++;
  GC_ROOT_RESTORE(js, root_mark);
  return ITER_CONTINUE;
}

static ant_value_t builtin_Promise_all(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Promise.all requires an iterable");

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t iterable = args[0];
  GC_ROOT_PIN(js, iterable);
  uint8_t t = vtype(iterable);
  if (t != T_ARR && t != T_OBJ) {
    ant_value_t err = js_mkerr(js, "Promise.all requires an iterable");
    GC_ROOT_RESTORE(js, root_mark);
    return err;
  }

  ant_value_t ctor = js->this_val;
  GC_ROOT_PIN(js, ctor);
  if (vtype(ctor) != T_FUNC && vtype(ctor) != T_CFUNC) ctor = js_mkundef();

  ant_value_t result_promise = mkpromise_with_ctor(js, ctor);
  GC_ROOT_PIN(js, result_promise);
  if (is_err(result_promise)) {
    GC_ROOT_RESTORE(js, root_mark);
    return result_promise;
  }

  ant_value_t tracker = mkobj(js, 0);
  GC_ROOT_PIN(js, tracker);
  ant_value_t results = mkarr(js);
  GC_ROOT_PIN(js, results);

  js_setprop(js, tracker, js_mkstr(js, "remaining", 9), tov(0.0));
  js_setprop(js, tracker, js_mkstr(js, "results", 7), results);
  set_slot(tracker, SLOT_DATA, result_promise);

  promise_all_iter_ctx_t ctx = { .tracker = tracker, .index = 0 };
  ant_value_t iter_result = iter_foreach(js, iterable, promise_all_iter_cb, &ctx);

  if (is_err(iter_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return iter_result;
  }

  int len = ctx.index;
  {
    ant_offset_t doff = get_dense_buf(results);
    if (doff) {
      if ((ant_offset_t)len > dense_capacity(doff)) doff = dense_grow(js, results, (ant_offset_t)len);
      if (doff) array_len_set(js, results, (ant_offset_t)len);
    }
  }

  if (len == 0) {
    js_resolve_promise(js, result_promise, mkval(T_ARR, vdata(results)));
    GC_ROOT_RESTORE(js, root_mark);
    return result_promise;
  }

  js_setprop(js, tracker, js_mkstr(js, "remaining", 9), tov((double)len));
  GC_ROOT_RESTORE(js, root_mark);
  return result_promise;
}

static ant_value_t builtin_Promise_allSettled(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Promise.allSettled requires an iterable");

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t iterable = args[0];
  GC_ROOT_PIN(js, iterable);
  uint8_t t = vtype(iterable);
  if (t != T_ARR && t != T_OBJ) {
    ant_value_t err = js_mkerr(js, "Promise.allSettled requires an iterable");
    GC_ROOT_RESTORE(js, root_mark);
    return err;
  }

  ant_value_t ctor = js->this_val;
  GC_ROOT_PIN(js, ctor);
  if (vtype(ctor) != T_FUNC && vtype(ctor) != T_CFUNC) ctor = js_mkundef();

  ant_value_t result_promise = mkpromise_with_ctor(js, ctor);
  GC_ROOT_PIN(js, result_promise);
  if (is_err(result_promise)) {
    GC_ROOT_RESTORE(js, root_mark);
    return result_promise;
  }

  ant_value_t tracker = mkobj(js, 0);
  GC_ROOT_PIN(js, tracker);
  ant_value_t results = mkarr(js);
  GC_ROOT_PIN(js, results);

  js_setprop(js, tracker, js_mkstr(js, "remaining", 9), tov(0.0));
  js_setprop(js, tracker, js_mkstr(js, "results", 7), results);
  set_slot(tracker, SLOT_DATA, result_promise);

  promise_all_iter_ctx_t ctx = { .tracker = tracker, .index = 0 };
  ant_value_t iter_result = iter_foreach(js, iterable, promise_all_settled_iter_cb, &ctx);

  if (is_err(iter_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return iter_result;
  }

  int len = ctx.index;
  ant_offset_t doff = get_dense_buf(results);
  if (doff) {
    if ((ant_offset_t)len > dense_capacity(doff)) doff = dense_grow(js, results, (ant_offset_t)len);
    if (doff) array_len_set(js, results, (ant_offset_t)len);
  }

  if (len == 0) {
    js_resolve_promise(js, result_promise, mkval(T_ARR, vdata(results)));
    GC_ROOT_RESTORE(js, root_mark);
    return result_promise;
  }

  js_setprop(js, tracker, js_mkstr(js, "remaining", 9), tov((double)len));
  GC_ROOT_RESTORE(js, root_mark);
  return result_promise;
}

typedef struct {
  ant_value_t result_promise;
  ant_value_t resolve_fn;
  ant_value_t reject_fn;
  bool settled;
} promise_race_iter_ctx_t;

static iter_action_t promise_race_iter_cb(ant_t *js, ant_value_t value, void *ctx, ant_value_t *out) {
  GC_ROOT_SAVE(root_mark, js);
  promise_race_iter_ctx_t *pctx = (promise_race_iter_ctx_t *)ctx;
  ant_value_t item = value;
  GC_ROOT_PIN(js, item);

  if (vtype(item) != T_PROMISE) {
    js_resolve_promise(js, pctx->result_promise, item);
    pctx->settled = true;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_BREAK;
  }

  ant_promise_state_t *pd = get_promise_data(js, item, false);
  if (pd) {
  if (pd->state == 1) {
    js_resolve_promise(js, pctx->result_promise, pd->value);
    pctx->settled = true;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_BREAK;
  } else if (pd->state == 2) {
    js_reject_promise(js, pctx->result_promise, pd->value);
    pctx->settled = true;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_BREAK;
  }}

  ant_value_t then_args[] = { pctx->resolve_fn, pctx->reject_fn };
  ant_value_t saved_this = js->this_val;
  GC_ROOT_PIN(js, saved_this);
  js->this_val = item;
  ant_value_t then_result = builtin_promise_then(js, then_args, 2);
  js->this_val = saved_this;
  if (is_err(then_result)) {
    *out = then_result;
    GC_ROOT_RESTORE(js, root_mark);
    return ITER_ERROR;
  }

  GC_ROOT_RESTORE(js, root_mark);
  return ITER_CONTINUE;
}

static ant_value_t builtin_Promise_race(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Promise.race requires an iterable");

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t iterable = args[0];
  GC_ROOT_PIN(js, iterable);
  uint8_t t = vtype(iterable);
  if (t != T_ARR && t != T_OBJ) {
    ant_value_t err = js_mkerr(js, "Promise.race requires an iterable");
    GC_ROOT_RESTORE(js, root_mark);
    return err;
  }

  ant_value_t ctor = js->this_val;
  GC_ROOT_PIN(js, ctor);
  if (vtype(ctor) != T_FUNC && vtype(ctor) != T_CFUNC) ctor = js_mkundef();
  ant_value_t result_promise = mkpromise_with_ctor(js, ctor);
  GC_ROOT_PIN(js, result_promise);
  if (is_err(result_promise)) {
    GC_ROOT_RESTORE(js, root_mark);
    return result_promise;
  }

  ant_value_t resolve_fn = make_data_cfunc(js, result_promise, builtin_resolve_internal);
  GC_ROOT_PIN(js, resolve_fn);
  if (is_err(resolve_fn)) {
    GC_ROOT_RESTORE(js, root_mark);
    return resolve_fn;
  }

  ant_value_t reject_fn = make_data_cfunc(js, result_promise, builtin_reject_internal);
  GC_ROOT_PIN(js, reject_fn);
  if (is_err(reject_fn)) {
    GC_ROOT_RESTORE(js, root_mark);
    return reject_fn;
  }

  promise_race_iter_ctx_t ctx = {
    .result_promise = result_promise,
    .resolve_fn = resolve_fn,
    .reject_fn = reject_fn,
    .settled = false
  };

  ant_value_t iter_result = iter_foreach(js, iterable, promise_race_iter_cb, &ctx);
  if (is_err(iter_result)) {
    GC_ROOT_RESTORE(js, root_mark);
    return iter_result;
  }

  GC_ROOT_RESTORE(js, root_mark);
  return result_promise;
}

static ant_value_t mk_aggregate_error(ant_t *js, ant_value_t errors) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, errors);
  ant_value_t args[] = { errors, js_mkstr(js, "All promises were rejected", 26) };
  ant_offset_t off = lkp(js, js_glob(js), "AggregateError", 14);
  ant_value_t ctor = off ? propref_load(js, off) : js_mkundef();
  GC_ROOT_PIN(js, ctor);
  ant_value_t ret = sv_vm_call(js->vm, js, ctor, js_mkundef(), args, 2, NULL, false);
  GC_ROOT_RESTORE(js, root_mark);
  return ret;
}

static bool promise_any_try_resolve(ant_t *js, ant_value_t tracker, ant_value_t value) {
  if (js_truthy(js, js_get(js, tracker, "resolved"))) return false;
  js_set(js, tracker, "resolved", js_true);
  js_resolve_promise(js, get_slot(tracker, SLOT_DATA), value);
  return true;
}

static void promise_any_record_rejection(ant_t *js, ant_value_t tracker, int index, ant_value_t reason) {
  ant_value_t errors = js_get(js, tracker, "errors");
  arr_set(js, errors, (ant_offset_t)index, reason);
  
  int remaining = (int)tod(js_get(js, tracker, "remaining")) - 1;
  js_set(js, tracker, "remaining", tov((double)remaining));
  
  if (remaining == 0) js_reject_promise(js, get_slot(tracker, SLOT_DATA), mk_aggregate_error(js, errors));
}

static ant_value_t builtin_Promise_any_resolve_handler(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t tracker = js_get(js, js->this_val, "tracker");
  promise_any_try_resolve(js, tracker, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static ant_value_t builtin_Promise_any_reject_handler(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t tracker = js_get(js, js->this_val, "tracker");
  if (js_truthy(js, js_get(js, tracker, "resolved"))) return js_mkundef();
  
  int index = (int)tod(js_get(js, js->this_val, "index"));
  promise_any_record_rejection(js, tracker, index, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static ant_value_t builtin_Promise_any(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "Promise.any requires an array");

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t arr = args[0];
  GC_ROOT_PIN(js, arr);
  if (vtype(arr) != T_ARR) {
    ant_value_t err = js_mkerr(js, "Promise.any requires an array");
    GC_ROOT_RESTORE(js, root_mark);
    return err;
  }

  int len = (int)get_array_length(js, arr);

  if (len == 0) {
    ant_value_t reject_args[] = { mk_aggregate_error(js, mkarr(js)) };
    ant_value_t ret = builtin_Promise_reject(js, reject_args, 1);
    GC_ROOT_RESTORE(js, root_mark);
    return ret;
  }

  ant_value_t result_promise = js_mkpromise(js);
  GC_ROOT_PIN(js, result_promise);
  ant_value_t tracker = mkobj(js, 0);
  GC_ROOT_PIN(js, tracker);
  ant_value_t errors = mkarr(js);
  GC_ROOT_PIN(js, errors);

  set_slot(tracker, SLOT_DATA, result_promise);

  js_setprop(js, tracker, js_mkstr(js, "remaining", 9), tov((double)len));
  js_setprop(js, tracker, js_mkstr(js, "errors", 6), errors);
  js_setprop(js, tracker, js_mkstr(js, "resolved", 8), js_false);
  
  {
    ant_offset_t doff = get_dense_buf(errors);
    if (doff) {
      if ((ant_offset_t)len > dense_capacity(doff)) doff = dense_grow(js, errors, (ant_offset_t)len);
      if (doff) array_len_set(js, errors, (ant_offset_t)len);
    }
  }

  for (int i = 0; i < len; i++) {
    ant_value_t item = arr_get(js, arr, (ant_offset_t)i);
    GC_ROOT_PIN(js, item);
    if (vtype(item) != T_PROMISE) {
      promise_any_try_resolve(js, tracker, item);
      GC_ROOT_RESTORE(js, root_mark);
      return result_promise;
    }

    ant_promise_state_t *pd = get_promise_data(js, item, false);
    if (pd) {
      pd->has_rejection_handler = true;
      pd->unhandled_reported = false;
      
      if (pd->state == 1) {
        promise_any_try_resolve(js, tracker, pd->value);
        GC_ROOT_RESTORE(js, root_mark);
        return result_promise;
      } else if (pd->state == 2) {
        promise_any_record_rejection(js, tracker, i, pd->value);
        continue;
      }
    }

    ant_value_t resolve_obj = mkobj(js, 0);
    if (is_err(resolve_obj)) {
      GC_ROOT_RESTORE(js, root_mark);
      return resolve_obj;
    }
    GC_ROOT_PIN(js, resolve_obj);
    set_slot(resolve_obj, SLOT_CFUNC, js_mkfun(builtin_Promise_any_resolve_handler));
    js_setprop(js, resolve_obj, js_mkstr(js, "tracker", 7), tracker);

    ant_value_t reject_obj = mkobj(js, 0);
    if (is_err(reject_obj)) {
      GC_ROOT_RESTORE(js, root_mark);
      return reject_obj;
    }
    GC_ROOT_PIN(js, reject_obj);
    set_slot(reject_obj, SLOT_CFUNC, js_mkfun(builtin_Promise_any_reject_handler));
    js_setprop(js, reject_obj, js_mkstr(js, "index", 5), tov((double)i));
    js_setprop(js, reject_obj, js_mkstr(js, "tracker", 7), tracker);

    ant_value_t resolve_fn = js_obj_to_func(resolve_obj);
    GC_ROOT_PIN(js, resolve_fn);
    ant_value_t reject_fn = js_obj_to_func(reject_obj);
    GC_ROOT_PIN(js, reject_fn);
    ant_value_t then_args[] = { resolve_fn, reject_fn };
    ant_value_t saved_this = js->this_val;
    GC_ROOT_PIN(js, saved_this);
    js->this_val = item;
    ant_value_t then_result = builtin_promise_then(js, then_args, 2);
    js->this_val = saved_this;
    if (is_err(then_result)) {
      GC_ROOT_RESTORE(js, root_mark);
      return then_result;
    }
  }

  GC_ROOT_RESTORE(js, root_mark);
  return result_promise;
}

static ant_value_t handle_proxy_instanceof(ant_t *js, ant_value_t l, ant_value_t r, uint8_t ltype) {
  ant_value_t target = proxy_read_target(js, r);
  uint8_t ttype = vtype(target);
  
  if (ttype != T_FUNC && ttype != T_CFUNC) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Right-hand side of 'instanceof' is not callable");
  }
  
  {
    ant_value_t has_instance = js_get_sym(js, r, get_hasInstance_sym());
    if (is_err(has_instance)) return has_instance;
    uint8_t hit = vtype(has_instance);
    if (hit == T_FUNC || hit == T_CFUNC) {
      ant_value_t args[1] = { l };
      ant_value_t result = sv_vm_call(js->vm, js, has_instance, r, args, 1, NULL, false);
      if (is_err(result)) return result;
      return js_bool(js_truthy(js, result));
    }
    if (hit != T_UNDEF && hit != T_NULL) {
      return js_mkerr_typed(js, JS_ERR_TYPE, "Symbol.hasInstance is not callable");
    }
  }
  
  ant_value_t proto_val = proxy_get(js, r, "prototype", 9);
  uint8_t pt = vtype(proto_val);
  
  if (pt != T_OBJ && pt != T_ARR && pt != T_FUNC) {
    return mkval(T_BOOL, 0);
  }
  
  if (ltype != T_OBJ && ltype != T_ARR && ltype != T_FUNC && ltype != T_PROMISE) {
    return mkval(T_BOOL, 0);
  }
  
  ant_value_t current = get_proto(js, l);
  return mkval(T_BOOL, proto_chain_contains_cycle_safe(js, current, proto_val) ? 1 : 0);
}

static ant_value_t handle_cfunc_instanceof(ant_value_t l, ant_value_t r, uint8_t ltype) {
  ant_value_t (*fn)(ant_t *, ant_value_t *, int) = (ant_value_t(*)(ant_t *, ant_value_t *, int)) vdata(r);
  
  if (fn == builtin_Object) return mkval(T_BOOL, ltype == T_OBJ ? 1 : 0);
  if (fn == builtin_Function) return mkval(T_BOOL, (ltype == T_FUNC || ltype == T_CFUNC) ? 1 : 0);
  if (fn == builtin_String) return mkval(T_BOOL, ltype == T_STR ? 1 : 0);
  if (fn == builtin_Number) return mkval(T_BOOL, ltype == T_NUM ? 1 : 0);
  if (fn == builtin_Boolean) return mkval(T_BOOL, ltype == T_BOOL ? 1 : 0);
  if (fn == builtin_Array) return mkval(T_BOOL, ltype == T_ARR ? 1 : 0);
  if (fn == builtin_Promise) return mkval(T_BOOL, ltype == T_PROMISE ? 1 : 0);
  
  return mkval(T_BOOL, 0);
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

static ant_value_t walk_prototype_chain(ant_t *js, ant_value_t l, ant_value_t ctor_proto) {
  ant_value_t current = get_proto(js, l);
  return mkval(T_BOOL, proto_chain_contains_cycle_safe(js, current, ctor_proto) ? 1 : 0);
}

static inline ant_object_t *cached_function_proto_obj(ant_t *js) {
  static ant_object_t *cached = NULL;
  if (cached) return cached;
  ant_value_t proto = get_ctor_proto(js, "Function", 8);
  if (!is_object_type(proto)) return NULL;
  cached = js_obj_ptr(js_as_obj(proto));
  return cached;
}

ant_value_t do_instanceof(ant_t *js, ant_value_t l, ant_value_t r) {
  uint8_t ltype = vtype(l);
  uint8_t rtype = vtype(r);
  
  if (rtype != T_FUNC && rtype != T_CFUNC) {
    if (is_proxy(r)) return handle_proxy_instanceof(js, l, r, ltype);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Right-hand side of 'instanceof' is not callable");
  }
  
  if (rtype == T_CFUNC) {
    return handle_cfunc_instanceof(l, r, ltype);
  }

  ant_value_t func_obj = js_func_obj(r);
  ant_offset_t has_instance_sym_off = (ant_offset_t)vdata(get_hasInstance_sym());
  bool use_slow_has_instance = false;
  ant_offset_t own_has_instance = lkp_sym(js, func_obj, has_instance_sym_off);
  if (own_has_instance != 0) {
    const ant_shape_prop_t *prop_meta = prop_shape_meta(js, own_has_instance);
    if (prop_meta && (prop_meta->has_getter || prop_meta->has_setter)) {
      use_slow_has_instance = true;
    } else {
      ant_value_t has_instance = propref_load(js, own_has_instance);
      uint8_t hit = vtype(has_instance);
      if (hit == T_FUNC || hit == T_CFUNC) {
        ant_value_t args[1] = { l };
        ant_value_t result = sv_vm_call(js->vm, js, has_instance, r, args, 1, NULL, false);
        if (is_err(result)) return result;
        return js_bool(js_truthy(js, result));
      }
      if (hit != T_UNDEF && hit != T_NULL) {
        return js_mkerr_typed(js, JS_ERR_TYPE, "Symbol.hasInstance is not callable");
      }
    }
  } else {
    ant_object_t *func_proto_ptr = cached_function_proto_obj(js);
    if (func_proto_ptr && func_proto_ptr->shape &&
        ant_shape_lookup_symbol(func_proto_ptr->shape, has_instance_sym_off) >= 0) {
      use_slow_has_instance = true;
    } else if (func_proto_ptr && func_proto_ptr->is_exotic) {
      ant_value_t func_proto_obj = mkval(T_OBJ, (uintptr_t)func_proto_ptr);
      if (lookup_sym_descriptor(func_proto_obj, has_instance_sym_off))
        use_slow_has_instance = true;
    }
  }

  if (use_slow_has_instance) {
    ant_value_t has_instance = js_get_sym(js, r, get_hasInstance_sym());
    if (is_err(has_instance)) return has_instance;
    uint8_t hit = vtype(has_instance);
    if (hit == T_FUNC || hit == T_CFUNC) {
      ant_value_t args[1] = { l };
      ant_value_t result = sv_vm_call(js->vm, js, has_instance, r, args, 1, NULL, false);
      if (is_err(result)) return result;
      return js_bool(js_truthy(js, result));
    }
    if (hit != T_UNDEF && hit != T_NULL) {
      return js_mkerr_typed(js, JS_ERR_TYPE, "Symbol.hasInstance is not callable");
    }
  }

  ant_offset_t proto_off = lkp_interned(js, func_obj, INTERN_PROTOTYPE, 9);
  if (proto_off == 0) return mkval(T_BOOL, 0);
  
  ant_value_t ctor_proto = propref_load(js, proto_off);
  uint8_t pt = vtype(ctor_proto);
  if (pt != T_OBJ && pt != T_ARR && pt != T_FUNC) return mkval(T_BOOL, 0);
  
  if (ltype == T_STR || ltype == T_NUM || ltype == T_BOOL) {
    ant_value_t type_proto = get_prototype_for_type(js, ltype);
    return mkval(T_BOOL, vdata(ctor_proto) == vdata(type_proto) ? 1 : 0);
  }
  
  if (ltype != T_OBJ && ltype != T_ARR && ltype != T_FUNC && ltype != T_PROMISE) {
    return mkval(T_BOOL, 0);
  }
  
  return walk_prototype_chain(js, l, ctor_proto);
}

ant_value_t do_in(ant_t *js, ant_value_t l, ant_value_t r) {
  ant_offset_t prop_len;
  const char *prop_name;
  char num_buf[32];
  
  ant_value_t key = js_to_primitive(js, l, 1);
  if (is_err(key)) return key;
  
  bool is_sym = (vtype(key) == T_SYMBOL);
  
  if (is_sym) {
    const char *d = js_sym_desc(js, key);
    prop_name = d ? d : "symbol";
    prop_len = (ant_offset_t)strlen(prop_name);
  } else if (vtype(key) == T_NUM) {
    prop_len = (ant_offset_t)strnum(key, num_buf, sizeof(num_buf));
    prop_name = num_buf;
  } else {
    ant_value_t key_str = js_tostring_val(js, key);
    if (is_err(key_str)) return key_str;
    ant_offset_t prop_off = vstr(js, key_str, &prop_len);
    prop_name = (char *)(uintptr_t)(prop_off);
  }
  
  if (!is_object_type(r)) {
    if (vtype(r) == T_CFUNC) return mkval(T_BOOL, 0);
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot use 'in' operator to search for '%.*s' in non-object", (int)prop_len, prop_name);
  }
  
  if (is_proxy(r)) {
    ant_value_t result = is_sym ? proxy_has_val(js, r, key) : proxy_has(js, r, prop_name, prop_len);
    if (is_err(result)) return result;
    return js_bool(js_truthy(js, result));
  }
  
  if (!is_sym && vtype(r) == T_ARR) {
    unsigned long idx;
    ant_offset_t arr_len = get_array_length(js, r);
    if (parse_array_index(prop_name, prop_len, arr_len, &idx)) return mkval(T_BOOL, arr_has(js, r, (ant_offset_t)idx) ? 1 : 0);
    if (is_length_key(prop_name, prop_len)) return mkval(T_BOOL, 1);
  }
  
  ant_offset_t found = is_sym ? lkp_sym_proto(js, r, (ant_offset_t)vdata(key)) : lkp_proto(js, r, prop_name, prop_len);
  return mkval(T_BOOL, found != 0 ? 1 : 0);
}

static ant_value_t builtin_import_tla_resolve(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t me = js->current_func;
  return get_slot(me, SLOT_DATA);
}

static inline bool js_has_module_filename(const char *filename) {
  return filename && filename[0];
}

static ant_value_t js_get_import_func(ant_t *js) {
  ant_value_t glob = js_glob(js);
  ant_offset_t import_off = lkp(js, glob, "import", 6);
  if (import_off == 0) return js_mkundef();
  return propref_load(js, import_off);
}

static ant_value_t js_get_module_ctx_import_meta(ant_t *js, ant_value_t module_ctx) {
  if (!is_object_type(module_ctx)) return js_mkundef();
  return js_get(js, module_ctx, "meta");
}

static const char *js_get_module_ctx_filename(ant_t *js, ant_value_t module_ctx) {
  if (!is_object_type(module_ctx)) return NULL;

  ant_value_t filename = js_get(js, module_ctx, "filename");
  if (vtype(filename) != T_STR) return NULL;
  return js_getstr(js, filename, NULL);
}

static ant_value_t js_module_ctx_from_func(ant_value_t func) {
  if (vtype(func) != T_FUNC) return js_mkundef();
  ant_value_t module_ctx = get_slot(js_func_obj(func), SLOT_MODULE_CTX);
  return is_object_type(module_ctx) ? module_ctx : js_mkundef();
}

static ant_value_t js_get_execution_module_ctx(ant_t *js) {
  sv_vm_t *vm = sv_vm_get_active(js);
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

ant_value_t js_builtin_import(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "import() requires a string specifier");
  
  ant_value_t module_ctx = js_get_import_owner_module_ctx(js);
  const char *base_path = js_get_module_ctx_filename(js, module_ctx);
  
  if (!js_has_module_filename(base_path))
    base_path = js_get_execution_module_filename(js);
    
  ant_value_t tla_promise = js_mkundef();
  ant_value_t ns = js_esm_import_dynamic(js, args[0], base_path, &tla_promise);
  
  if (is_err(ns)) return builtin_Promise_reject(js, &ns, 1);

  if (vtype(tla_promise) == T_PROMISE) {
    ant_value_t resolve_fn = make_data_cfunc(js, ns, builtin_import_tla_resolve);
    ant_value_t saved = js->this_val;
    
    js->this_val = tla_promise;
    ant_value_t then_args[] = { resolve_fn };
    
    ant_value_t result = builtin_promise_then(js, then_args, 1);
    js->this_val = saved;
    
    return result;
  }

  ant_value_t promise_args[] = { ns };
  return builtin_Promise_resolve(js, promise_args, 1);
}

static ant_value_t js_get_import_meta_prop(ant_t *js) {
  ant_value_t import_fn = js_get_import_func(js);
  if (vtype(import_fn) != T_FUNC) return js_mkundef();
  return js_get(js, js_func_obj(import_fn), "meta");
}

static void js_set_import_meta_prop(ant_t *js, ant_value_t import_meta) {
  ant_value_t import_fn = js_get_import_func(js);
  if (vtype(import_fn) != T_FUNC) return;
  setprop_cstr(js, js_func_obj(import_fn), "meta", 4, import_meta);
}

static void js_set_import_module_ctx(ant_t *js, ant_value_t module_ctx) {
  if (!is_object_type(module_ctx)) return;
  ant_value_t import_fn = js_get_import_func(js);
  if (vtype(import_fn) != T_FUNC) return;
  js_set_slot_wb(js, js_func_obj(import_fn), SLOT_MODULE_CTX, module_ctx);
}

static ant_value_t js_get_current_import_meta(ant_t *js) {
  ant_value_t import_meta = js_get_module_ctx_import_meta(
    js,
    js_get_execution_module_ctx(js)
  );
  if (vtype(import_meta) == T_OBJ) return import_meta;
  return js_get_import_meta_prop(js);
}

static ant_value_t builtin_import_meta_resolve(ant_t *js, ant_value_t *args, int nargs) {
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

  bool is_url = esm_is_url(filename);
  bool is_builtin = esm_has_builtin_scheme(filename);

  ant_value_t url_val = (is_url || is_builtin)
    ? js_mkstr(js, filename, strlen(filename))
    : js_esm_make_file_url(js, filename);
  if (!is_err(url_val)) setprop_cstr(js, import_meta, "url", 3, url_val);

  ant_value_t filename_val = js_get(js, module_ctx, "filename");
  if (vtype(filename_val) == T_STR)
    setprop_cstr(js, import_meta, "filename", 8, filename_val);

  if (is_url || is_builtin) js_set_import_meta_special_dirname(js, import_meta, filename, is_builtin);
  else js_set_import_meta_path_dirname(js, import_meta, filename);

  setprop_cstr(js, import_meta, "main", 4, is_main ? js_true : js_false);

  ant_value_t resolve_fn = js_heavy_mkfun(js, builtin_import_meta_resolve, js_mkundef());
  if (vtype(resolve_fn) == T_FUNC)
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

  ant_value_t module_ctx = js_mkobj(js);
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

  if (!is_object_type(module_ctx) || vtype(import_meta) != T_OBJ) {
    GC_ROOT_RESTORE(js, root_mark);
    return js_mkundef();
  }

  ant_value_t import_obj = js_mkobj(js);
  if (is_err(import_obj)) {
    GC_ROOT_RESTORE(js, root_mark);
    return import_obj;
  }

  GC_ROOT_PIN(js, import_obj);
  ant_value_t function_proto = js_get_slot(js_glob(js), SLOT_FUNC_PROTO);

  if (vtype(function_proto) == T_UNDEF)
    function_proto = js_get_ctor_proto(js, "Function", 8);
  GC_ROOT_PIN(js, function_proto);

  if (is_object_type(function_proto)) js_set_proto_wb(js, import_obj, function_proto);
  set_slot(import_obj, SLOT_CFUNC, js_mkfun(js_builtin_import));
  
  js_set_slot_wb(js, import_obj, SLOT_MODULE_CTX, module_ctx);
  setprop_cstr(js, import_obj, "meta", 4, import_meta);

  ant_value_t import_fn = js_obj_to_func(import_obj);
  GC_ROOT_RESTORE(js, root_mark);
  
  return import_fn;
}

void js_setup_import_meta(ant_t *js, const char *filename) {
  if (!filename) return;

  ant_value_t module_ctx = js_create_module_context(js, filename, true);
  ant_value_t import_meta = js_get_module_ctx_import_meta(js, module_ctx);
  if (is_err(import_meta) || vtype(import_meta) == T_UNDEF) return;

  js_set_import_module_ctx(js, module_ctx);
  js_set_import_meta_prop(js, import_meta);
}

void js_module_eval_ctx_push(ant_t *js, ant_module_t *ctx) {
  if (!js || !ctx) return;

  ctx->prev = js->module;
  ctx->prev_import_meta_prop = js_get_import_meta_prop(js);
  js->module = ctx;

  ant_value_t import_meta = js_get_module_ctx_import_meta(js, ctx->module_ctx);
  if (vtype(import_meta) != T_UNDEF) js_set_import_meta_prop(js, import_meta);
}

void js_module_eval_ctx_pop(ant_t *js, ant_module_t *ctx) {
  if (!js || !ctx) return;

  if (js->module == ctx) {
    js_set_import_meta_prop(js, ctx->prev_import_meta_prop);
    js->module = ctx->prev;
  }
}

static ant_proxy_state_t *get_proxy_data(ant_value_t obj) {
  if (vtype(obj) != T_OBJ) return NULL;
  ant_object_t *ptr = js_obj_ptr(obj);
  return ptr ? ptr->proxy_state : NULL;
}

bool is_proxy(ant_value_t obj) {
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr || !ptr->is_exotic) return false;
  return get_proxy_data(obj) != NULL;
}

static bool js_is_constructor_impl(ant_t *js, ant_value_t value) {
  (void)js;
  ant_value_t slow = value;
  ant_value_t fast = value;

  while (true) {
    uint8_t t = vtype(slow);
    if (t == T_FUNC) {
      ant_object_t *obj = js_obj_ptr(js_func_obj(slow));
      return obj && obj->is_constructor;
    }
    if (t != T_OBJ) return false;

    ant_object_t *obj = js_obj_ptr(slow);
    if (!obj) return false;
    if (obj->is_constructor) return true;
    if (!obj->is_exotic) return false;

    ant_proxy_state_t *data = get_proxy_data(slow);
    if (!data) return false;
    slow = data->target;

    for (int i = 0; i < 2; i++) {
      if (vtype(fast) != T_OBJ) { fast = js_mknull(); break; }
      ant_object_t *fobj = js_obj_ptr(fast);
      if (!fobj || !fobj->is_exotic) { fast = js_mknull(); break; }
      ant_proxy_state_t *fdata = get_proxy_data(fast);
      if (!fdata) { fast = js_mknull(); break; }
      fast = fdata->target;
    }

    if (same_object_identity(slow, fast)) return false;
  }
}

bool js_is_constructor(ant_t *js, ant_value_t value) {
  return js_is_constructor_impl(js, value);
}

static ant_value_t proxy_read_target(ant_t *js, ant_value_t obj) {
  ant_proxy_state_t *data = get_proxy_data(obj);
  return data ? data->target : obj;
}

static ant_offset_t proxy_aware_length(ant_t *js, ant_value_t obj) {
  ant_value_t src = is_proxy(obj) ? proxy_read_target(js, obj) : obj;
  if (vtype(src) == T_ARR) return get_array_length(js, src);
  ant_offset_t off = lkp_interned(js, src, INTERN_LENGTH, 6);
  if (off == 0) return 0;
  ant_value_t len_val = propref_load(js, off);
  return vtype(len_val) == T_NUM ? (ant_offset_t)tod(len_val) : 0;
}

static ant_value_t proxy_aware_get_elem(ant_t *js, ant_value_t obj, const char *key, size_t key_len) {
  ant_value_t src = is_proxy(obj) ? proxy_read_target(js, obj) : obj;
  ant_offset_t off = lkp(js, src, key, key_len);
  return off ? propref_load(js, off) : js_mkundef();
}

static ant_value_t throw_proxy_error(ant_t *js, const char *message) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "%s", message);
}

static bool proxy_target_is_extensible(ant_value_t obj) {
  uint8_t t = vtype(obj);
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return false;

  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  if (!ptr) return false;
  if (ptr->frozen || ptr->sealed) return false;
  return ptr->extensible != 0;
}

static bool proxy_target_prop_is_nonconfig(ant_t *js, ant_value_t target, ant_offset_t prop_off) {
  (void)target;
  return is_nonconfig_prop(js, prop_off);
}

static bool proxy_target_prop_is_const(ant_t *js, ant_value_t target, ant_offset_t prop_off) {
  (void)target;
  return is_const_prop(js, prop_off);
}

static ant_value_t proxy_get(ant_t *js, ant_value_t proxy, const char *key, size_t key_len) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return js_mkundef();
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'get' on a proxy that has been revoked");
  
  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  
  ant_offset_t get_trap_off = vtype(handler) == T_OBJ 
    ? lkp_interned(js, handler, INTERN_GET, 3) 
    : 0;
  
  if (get_trap_off != 0) {
    ant_value_t get_trap = propref_load(js, get_trap_off);
    if (vtype(get_trap) == T_FUNC || vtype(get_trap) == T_CFUNC) {
      ant_value_t key_val = js_mkstr(js, key, key_len);
      
      ant_value_t args[3] = { target, key_val, proxy };
      ant_value_t result = sv_vm_call(js->vm, js, get_trap, js_mkundef(), args, 3, NULL, false);
      if (is_err(result)) return result;

      ant_offset_t prop_off = lkp(js, target, key, key_len);
      if (prop_off != 0 && proxy_target_prop_is_nonconfig(js, target, prop_off) &&
          proxy_target_prop_is_const(js, target, prop_off)) {
        ant_value_t target_value = propref_load(js, prop_off);
        if (!strict_eq_values(js, result, target_value))
          return js_mkerr_typed(js, JS_ERR_TYPE, "'get' on proxy: trap returned invalid value for non-configurable, non-writable property");
      }

      prop_meta_t meta;
      bool has_meta = lookup_string_prop_meta(js, js_as_obj(target), key, key_len, &meta);
      if (has_meta && !meta.configurable) {
        if (!meta.has_getter && !meta.has_setter && !meta.writable && prop_off != 0) {
          ant_value_t target_value = propref_load(js, prop_off);
          if (!strict_eq_values(js, result, target_value))
            return js_mkerr_typed(js, JS_ERR_TYPE, "'get' on proxy: trap returned invalid value for non-configurable, non-writable property");
        }
        if ((meta.has_getter || meta.has_setter) && !meta.has_getter && vtype(result) != T_UNDEF)
          return js_mkerr_typed(js, JS_ERR_TYPE, "'get' on proxy: trap returned non-undefined for property with undefined getter");
      }

      return result;
    }
  }
  
  char key_buf[256];
  size_t len = key_len < sizeof(key_buf) - 1 ? key_len : sizeof(key_buf) - 1;
  memcpy(key_buf, key, len);
  key_buf[len] = '\0';
  
  ant_offset_t off = lkp(js, target, key_buf, len);
  if (off != 0) return propref_load(js, off);
  
  ant_offset_t proto_off = lkp_proto(js, target, key_buf, len);
  if (proto_off != 0) return propref_load(js, proto_off);
  
  return js_mkundef();
}

static ant_value_t proxy_set(ant_t *js, ant_value_t proxy, const char *key, size_t key_len, ant_value_t value) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return js_mkundef();
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'set' on a proxy that has been revoked");
  
  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  
  ant_offset_t set_trap_off = vtype(handler) == T_OBJ ? lkp_interned(js, handler, INTERN_SET, 3) : 0;
  if (set_trap_off != 0) {
    ant_value_t set_trap = propref_load(js, set_trap_off);
    if (vtype(set_trap) == T_FUNC || vtype(set_trap) == T_CFUNC) {
      ant_value_t key_val = js_mkstr(js, key, key_len);
      ant_value_t args[4] = { target, key_val, value, proxy };
      ant_value_t result = sv_vm_call(js->vm, js, set_trap, js_mkundef(), args, 4, NULL, false);
      if (is_err(result)) return result;
      if (js_truthy(js, result)) {
        ant_offset_t prop_off = lkp(js, target, key, key_len);
        if (prop_off != 0 && proxy_target_prop_is_nonconfig(js, target, prop_off) &&
            proxy_target_prop_is_const(js, target, prop_off)) {
          ant_value_t target_value = propref_load(js, prop_off);
          if (!strict_eq_values(js, value, target_value))
            return js_mkerr_typed(js, JS_ERR_TYPE, "'set' on proxy: trap returned truthy for non-configurable, non-writable property with different value");
        }

        prop_meta_t meta;
        bool has_meta = lookup_string_prop_meta(js, js_as_obj(target), key, key_len, &meta);
        if (has_meta && !meta.configurable) {
          if (!meta.has_getter && !meta.has_setter && !meta.writable && prop_off != 0) {
            ant_value_t target_value = propref_load(js, prop_off);
            if (!strict_eq_values(js, value, target_value))
              return js_mkerr_typed(js, JS_ERR_TYPE, "'set' on proxy: trap returned truthy for non-configurable, non-writable property with different value");
          }
          if ((meta.has_getter || meta.has_setter) && !meta.has_setter)
            return js_mkerr_typed(js, JS_ERR_TYPE, "'set' on proxy: trap returned truthy for property with undefined setter");
        }
      }
      return js_true;
    }
  }
  
  ant_value_t key_str = js_mkstr(js, key, key_len);
  js_setprop(js, target, key_str, value);
  return js_true;
}

static ant_value_t proxy_has(ant_t *js, ant_value_t proxy, const char *key, size_t key_len) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return js_false;
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'has' on a proxy that has been revoked");
  
  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  
  ant_offset_t has_trap_off = vtype(handler) == T_OBJ ? lkp(js, handler, "has", 3) : 0;
  if (has_trap_off != 0) {
    ant_value_t has_trap = propref_load(js, has_trap_off);
    if (vtype(has_trap) == T_FUNC || vtype(has_trap) == T_CFUNC) {
      ant_value_t key_val = js_mkstr(js, key, key_len);
      ant_value_t args[2] = { target, key_val };
      ant_value_t result = sv_vm_call(js->vm, js, has_trap, js_mkundef(), args, 2, NULL, false);
      if (is_err(result)) return result;

      if (!js_truthy(js, result)) {
        ant_offset_t prop_off = lkp(js, target, key, key_len);
        prop_meta_t meta;
        bool has_meta = lookup_string_prop_meta(js, js_as_obj(target), key, key_len, &meta);
        bool has_own = (prop_off != 0) || has_meta;

        if ((prop_off != 0 && proxy_target_prop_is_nonconfig(js, target, prop_off)) || (has_meta && !meta.configurable))
          return js_mkerr_typed(js, JS_ERR_TYPE, "'has' on proxy: trap returned falsy for non-configurable property");

        if (has_own && !proxy_target_is_extensible(target))
          return js_mkerr_typed(js, JS_ERR_TYPE, "'has' on proxy: trap returned falsy for existing property on non-extensible target");
      }

      return result;
    }
  }
  
  char key_buf[256];
  size_t len = key_len < sizeof(key_buf) - 1 ? key_len : sizeof(key_buf) - 1;
  memcpy(key_buf, key, len);
  key_buf[len] = '\0';
  
  ant_offset_t off = lkp_proto(js, target, key_buf, len);
  return js_bool(off != 0);
}

static ant_value_t proxy_delete(ant_t *js, ant_value_t proxy, const char *key, size_t key_len) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return js_true;
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'deleteProperty' on a proxy that has been revoked");
  
  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  
  ant_offset_t delete_trap_off = vtype(handler) == T_OBJ ? lkp(js, handler, "deleteProperty", 14) : 0;
  if (delete_trap_off != 0) {
    ant_value_t delete_trap = propref_load(js, delete_trap_off);
    if (vtype(delete_trap) == T_FUNC || vtype(delete_trap) == T_CFUNC) {
      ant_value_t key_val = js_mkstr(js, key, key_len);
      ant_value_t args[2] = { target, key_val };
      ant_value_t result = sv_vm_call(js->vm, js, delete_trap, js_mkundef(), args, 2, NULL, false);
      if (is_err(result)) return result;
      if (js_truthy(js, result)) {
        ant_offset_t prop_off = lkp(js, target, key, key_len);
        if (prop_off != 0 && is_nonconfig_prop(js, prop_off))
          return js_mkerr_typed(js, JS_ERR_TYPE, "'deleteProperty' on proxy: trap returned truthy for non-configurable property");
        prop_meta_t meta;
        if (lookup_string_prop_meta(js, js_as_obj(target), key, key_len, &meta) && !meta.configurable)
          return js_mkerr_typed(js, JS_ERR_TYPE, "'deleteProperty' on proxy: trap returned truthy for non-configurable property");
      }
      return result;
    }
  }
  
  ant_value_t key_str = js_mkstr(js, key, key_len);
  js_setprop(js, target, key_str, js_mkundef());
  return js_true;
}

static ant_value_t proxy_get_val(ant_t *js, ant_value_t proxy, ant_value_t key_val) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return js_mkundef();
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'get' on a proxy that has been revoked");

  ant_value_t target = data->target;
  ant_value_t handler = data->handler;

  ant_offset_t get_trap_off = vtype(handler) == T_OBJ
    ? lkp_interned(js, handler, INTERN_GET, 3) : 0;
  if (get_trap_off != 0) {
    ant_value_t get_trap = propref_load(js, get_trap_off);
    if (vtype(get_trap) == T_FUNC || vtype(get_trap) == T_CFUNC) {
      ant_value_t args[3] = { target, key_val, proxy };
      return sv_vm_call(js->vm, js, get_trap, js_mkundef(), args, 3, NULL, false);
    }
  }

  if (vtype(key_val) == T_SYMBOL) {
    ant_offset_t off = lkp_sym_proto(js, target, (ant_offset_t)vdata(key_val));
    return off != 0 ? propref_load(js, off) : js_mkundef();
  }

  return proxy_get(js, proxy, "", 0);
}

static ant_value_t proxy_has_val(ant_t *js, ant_value_t proxy, ant_value_t key_val) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return js_false;
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'has' on a proxy that has been revoked");

  ant_value_t target = data->target;
  ant_value_t handler = data->handler;

  ant_offset_t has_trap_off = vtype(handler) == T_OBJ ? lkp(js, handler, "has", 3) : 0;
  if (has_trap_off != 0) {
    ant_value_t has_trap = propref_load(js, has_trap_off);
    if (vtype(has_trap) == T_FUNC || vtype(has_trap) == T_CFUNC) {
      ant_value_t args[2] = { target, key_val };
      return sv_vm_call(js->vm, js, has_trap, js_mkundef(), args, 2, NULL, false);
    }
  }

  if (vtype(key_val) == T_SYMBOL) {
    ant_offset_t off = lkp_sym_proto(js, target, (ant_offset_t)vdata(key_val));
    return js_bool(off != 0);
  }
  return js_false;
}

static ant_value_t proxy_get_own_property_descriptor(ant_t *js, ant_value_t proxy, ant_value_t key_val) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return js_mkundef();
  
  if (data->revoked)
    return throw_proxy_error(js, "Cannot perform 'getOwnPropertyDescriptor' on a proxy that has been revoked");

  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  ant_offset_t trap_off = vtype(handler) == T_OBJ ? lkp(js, handler, "getOwnPropertyDescriptor", 24) : 0;
    
  if (trap_off != 0) {
    ant_value_t trap = propref_load(js, trap_off);
    if (vtype(trap) == T_FUNC || vtype(trap) == T_CFUNC) {
      ant_value_t args[2] = { target, key_val };
      ant_value_t result = sv_vm_call(js->vm, js, trap, js_mkundef(), args, 2, NULL, false);
      if (is_err(result)) return result;
      if (vtype(result) == T_UNDEF || vtype(result) == T_OBJ) return result;
      return js_mkerr_typed(js, JS_ERR_TYPE, "'getOwnPropertyDescriptor' on proxy: trap returned neither object nor undefined");
    }
  }

  ant_value_t args[2] = { target, key_val };
  return builtin_object_getOwnPropertyDescriptor(js, args, 2);
}

static ant_value_t proxy_has_own(ant_t *js, ant_value_t proxy, ant_value_t key_val) {
  ant_value_t desc = proxy_get_own_property_descriptor(js, proxy, key_val);
  if (is_err(desc)) return desc;
  return js_bool(vtype(desc) != T_UNDEF);
}

static ant_value_t proxy_delete_val(ant_t *js, ant_value_t proxy, ant_value_t key_val) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return js_true;
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'deleteProperty' on a proxy that has been revoked");

  ant_value_t target = data->target;
  ant_value_t handler = data->handler;

  ant_offset_t delete_trap_off = vtype(handler) == T_OBJ ? lkp(js, handler, "deleteProperty", 14) : 0;
  if (delete_trap_off != 0) {
    ant_value_t delete_trap = propref_load(js, delete_trap_off);
    if (vtype(delete_trap) == T_FUNC || vtype(delete_trap) == T_CFUNC) {
      ant_value_t args[2] = { target, key_val };
      ant_value_t result = sv_vm_call(js->vm, js, delete_trap, js_mkundef(), args, 2, NULL, false);
      if (is_err(result)) return result;
      if (js_truthy(js, result) && vtype(key_val) == T_SYMBOL) {
        ant_offset_t prop_off = lkp_sym(js, target, (ant_offset_t)vdata(key_val));
        if (prop_off != 0 && is_nonconfig_prop(js, prop_off))
          return js_mkerr_typed(js, JS_ERR_TYPE, "'deleteProperty' on proxy: trap returned truthy for non-configurable property");
      }
      return result;
    }
  }

  if (vtype(key_val) == T_SYMBOL) {
    return js_delete_sym_prop(js, target, key_val);
  }
  return js_true;
}

ant_value_t js_proxy_apply(ant_t *js, ant_value_t proxy, ant_value_t this_arg, ant_value_t *args, int argc) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return js_mkerr_typed(js, JS_ERR_TYPE, "object is not a function");
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'apply' on a proxy that has been revoked");

  ant_value_t target = data->target;
  ant_value_t handler = data->handler;
  uint8_t target_type = vtype(target);

  if (target_type != T_FUNC && target_type != T_CFUNC && !(target_type == T_OBJ && is_proxy(target)))
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s is not a function", typestr(target_type));

  ant_offset_t trap_off = vtype(handler) == T_OBJ ? lkp(js, handler, "apply", 5) : 0;
  if (trap_off != 0) {
    ant_value_t trap = propref_load(js, trap_off);
    if (vtype(trap) == T_FUNC || vtype(trap) == T_CFUNC) {
      ant_value_t args_arr = mkarr(js);
      for (int i = 0; i < argc; i++)
        js_arr_push(js, args_arr, args[i]);
      ant_value_t trap_args[3] = { target, this_arg, args_arr };
      return sv_vm_call(js->vm, js, trap, handler, trap_args, 3, NULL, false);
    }
  }

  return sv_vm_call(js->vm, js, target, this_arg, args, argc, NULL, false);
}

ant_value_t js_proxy_construct(ant_t *js, ant_value_t proxy, ant_value_t *args, int argc, ant_value_t new_target) {
  ant_proxy_state_t *data = get_proxy_data(proxy);
  if (!data) return js_mkerr_typed(js, JS_ERR_TYPE, "not a constructor");
  if (data->revoked) return throw_proxy_error(js, "Cannot perform 'construct' on a proxy that has been revoked");

  ant_value_t target = data->target;
  ant_value_t handler = data->handler;

  if (!js_is_constructor(js, target))
    return js_mkerr_typed(js, JS_ERR_TYPE, "not a constructor");
  if (vtype(target) == T_OBJ && is_proxy(target))
    return js_proxy_construct(js, target, args, argc, new_target);
  if (vtype(target) != T_FUNC)
    return js_mkerr_typed(js, JS_ERR_TYPE, "not a constructor");

  ant_offset_t trap_off = vtype(handler) == T_OBJ ? lkp(js, handler, "construct", 9) : 0;
  if (trap_off != 0) {
    ant_value_t trap = propref_load(js, trap_off);
    if (vtype(trap) == T_FUNC || vtype(trap) == T_CFUNC) {
      ant_value_t args_arr = mkarr(js);
      for (int i = 0; i < argc; i++)
        js_arr_push(js, args_arr, args[i]);
      ant_value_t trap_args[3] = { target, args_arr, new_target };
      ant_value_t result = sv_vm_call(js->vm, js, trap, js_mkundef(), trap_args, 3, NULL, false);
      if (is_err(result)) return result;
      if (!is_object_type(result))
        return js_mkerr_typed(js, JS_ERR_TYPE, "'construct' on proxy: trap returned non-Object");
      return result;
    }
  }

  ant_value_t obj = mkobj(js, 0);
  ant_value_t proto = js_getprop_fallback(js, target, "prototype");
  if (is_object_type(proto)) js_set_proto_init(obj, proto);
  ant_value_t saved = js->new_target;
  js->new_target = new_target;
  ant_value_t ctor_this = obj;
  ant_value_t result = sv_vm_call(js->vm, js, target, obj, args, argc, &ctor_this, true);
  js->new_target = saved;
  if (is_err(result)) return result;
  return is_object_type(result) ? result : (is_object_type(ctor_this) ? ctor_this : obj);
}

static ant_value_t mkproxy(ant_t *js, ant_value_t target, ant_value_t handler) {
  ant_value_t proxy_obj = mkobj(js, 0);
  ant_object_t *proxy_ptr = js_obj_ptr(proxy_obj);
  if (!proxy_ptr) return js_mkerr(js, "out of memory");

  ant_proxy_state_t *data = (ant_proxy_state_t *)ant_calloc(sizeof(ant_proxy_state_t));
  if (!data) return js_mkerr(js, "out of memory");

  data->target = target;
  data->handler = handler;
  data->revoked = false;

  proxy_ptr->is_exotic = 1;
  js_mark_constructor(proxy_obj, js_is_constructor(js, target));
  proxy_ptr->proxy_state = data;
  return proxy_obj;
}

static ant_value_t create_proxy_checked(ant_t *js, ant_value_t *args, int nargs, bool require_new) {
  if (require_new && vtype(js->new_target) == T_UNDEF) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Proxy constructor requires 'new'");
  }
  if (nargs < 2) return js_mkerr(js, "Proxy requires two arguments: target and handler");

  ant_value_t target = args[0];
  ant_value_t handler = args[1];

  uint8_t target_type = vtype(target);
  if (target_type != T_OBJ && target_type != T_FUNC && target_type != T_ARR) {
    return js_mkerr(js, "Proxy target must be an object");
  }

  uint8_t handler_type = vtype(handler);
  if (handler_type != T_OBJ && handler_type != T_FUNC) {
    return js_mkerr(js, "Proxy handler must be an object");
  }

  return mkproxy(js, target, handler);
}

static ant_value_t builtin_Proxy(ant_t *js, ant_value_t *args, int nargs) {
  return create_proxy_checked(js, args, nargs, true);
}

static ant_value_t proxy_revoke_fn(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t func = js->current_func;
  ant_value_t ref_slot = get_slot(func, SLOT_PROXY_REF);
  
  if (vtype(ref_slot) != T_UNDEF && vdata(ref_slot) != 0) {
    ant_proxy_state_t *data = get_proxy_data(ref_slot);
    if (data) data->revoked = true;
  }
  
  return js_mkundef();
}

static ant_value_t builtin_Proxy_revocable(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t proxy = create_proxy_checked(js, args, nargs, false);
  if (is_err(proxy)) return proxy;
  
  ant_value_t revoke_obj = mkobj(js, 0);
  set_slot(revoke_obj, SLOT_CFUNC, js_mkfun(proxy_revoke_fn));
  set_slot(revoke_obj, SLOT_PROXY_REF, proxy);
  
  ant_value_t revoke_func = js_obj_to_func(revoke_obj);
  
  ant_value_t result = mkobj(js, 0);
  js_setprop(js, result, js_mkstr(js, "proxy", 5), proxy);
  js_setprop(js, result, js_mkstr(js, "revoke", 6), revoke_func);
  
  return result;
}

ant_t *js_create(void *buf, size_t len) {
  assert(
    (uintptr_t)buf <= ((1ULL << 53) - 1) &&
    "ANT_PTR: pointer exceeds 53-bit double-precision integer limit"
  );
  
  intern_init();
  ant_t *js = NULL;
  
  if (len < sizeof(*js)) return js;
  memset(buf, 0, len);
  
  js = (ant_t *) buf;
  rt->js = js;
  if (!fixed_arena_init(&js->obj_arena, sizeof(ant_object_t), offsetof(ant_object_t, mark_epoch), ANT_ARENA_MAX)) return NULL;
  if (!fixed_arena_init(&js->closure_arena, sizeof(sv_closure_t), offsetof(sv_closure_t, gc_epoch), ANT_CLOSURE_ARENA_MAX)) {
    fixed_arena_destroy(&js->obj_arena);
    return NULL;
  }
  if (!fixed_arena_init(&js->upvalue_arena, sizeof(sv_upvalue_t), offsetof(sv_upvalue_t, gc_epoch), ANT_CLOSURE_ARENA_MAX)) {
    fixed_arena_destroy(&js->closure_arena);
    fixed_arena_destroy(&js->obj_arena);
    return NULL;
  }
  js->c_root_cap = 64;
  js->c_roots = calloc(js->c_root_cap, sizeof(*js->c_roots));
  if (!js->c_roots) {
    fixed_arena_destroy(&js->upvalue_arena);
    fixed_arena_destroy(&js->closure_arena);
    fixed_arena_destroy(&js->obj_arena);
    return NULL;
  }
  js->global = mkobj(js, 0);
  js->this_val = js->global;
  js->new_target = js_mkundef();
  js->length_str = ANT_STRING("length");

  ant_value_t glob = js->global;
  ant_value_t object_proto = js_mkobj(js);
  set_proto(js, object_proto, js_mknull());
  
  defmethod(js, object_proto, "toString", 8, js_mkfun(builtin_object_toString));
  defmethod(js, object_proto, "valueOf", 7, js_mkfun(builtin_object_valueOf));
  defmethod(js, object_proto, "toLocaleString", 14, js_mkfun(builtin_object_toLocaleString));
  defmethod(js, object_proto, "hasOwnProperty", 14, js_mkfun(builtin_object_hasOwnProperty));
  defmethod(js, object_proto, "isPrototypeOf", 13, js_mkfun(builtin_object_isPrototypeOf));
  defmethod(js, object_proto, "propertyIsEnumerable", 20, js_mkfun(builtin_object_propertyIsEnumerable));
  
  ant_value_t proto_getter = js_mkfun(builtin_proto_getter);
  ant_value_t proto_setter = js_mkfun(builtin_proto_setter);
  js_set_accessor_desc(js, object_proto, STR_PROTO, STR_PROTO_LEN, proto_getter, proto_setter, JS_DESC_C);
  
  ant_value_t function_proto_obj = js_mkobj(js);
  set_proto(js, function_proto_obj, object_proto);
  set_slot(function_proto_obj, SLOT_CFUNC, js_mkfun(builtin_function_empty));
  
  defmethod(js, function_proto_obj, "call", 4, js_mkfun(builtin_function_call));
  defmethod(js, function_proto_obj, "apply", 5, js_mkfun(builtin_function_apply));
  defmethod(js, function_proto_obj, "bind", 4, js_mkfun(builtin_function_bind));
  defmethod(js, function_proto_obj, "toString", 8, js_mkfun(builtin_function_toString));
  
  ant_value_t function_proto = js_obj_to_func(function_proto_obj);
  set_slot(glob, SLOT_FUNC_PROTO, function_proto);
  
  ant_value_t array_proto = js_mkobj(js);
  set_proto(js, array_proto, object_proto);
  
  defmethod(js, array_proto, "push", 4, js_mkfun(builtin_array_push));
  defmethod(js, array_proto, "pop", 3, js_mkfun(builtin_array_pop));
  defmethod(js, array_proto, "slice", 5, js_mkfun(builtin_array_slice));
  defmethod(js, array_proto, "join", 4, js_mkfun(builtin_array_join));
  defmethod(js, array_proto, "includes", 8, js_mkfun(builtin_array_includes));
  defmethod(js, array_proto, "every", 5, js_mkfun(builtin_array_every));
  defmethod(js, array_proto, "reverse", 7, js_mkfun(builtin_array_reverse));
  defmethod(js, array_proto, "map", 3, js_mkfun(builtin_array_map));
  defmethod(js, array_proto, "filter", 6, js_mkfun(builtin_array_filter));
  defmethod(js, array_proto, "reduce", 6, js_mkfun(builtin_array_reduce));
  defmethod(js, array_proto, "flat", 4, js_mkfun(builtin_array_flat));
  defmethod(js, array_proto, "concat", 6, js_mkfun(builtin_array_concat));
  defmethod(js, array_proto, "at", 2, js_mkfun(builtin_array_at));
  defmethod(js, array_proto, "fill", 4, js_mkfun(builtin_array_fill));
  defmethod(js, array_proto, "find", 4, js_mkfun(builtin_array_find));
  defmethod(js, array_proto, "findIndex", 9, js_mkfun(builtin_array_findIndex));
  defmethod(js, array_proto, "findLast", 8, js_mkfun(builtin_array_findLast));
  defmethod(js, array_proto, "findLastIndex", 13, js_mkfun(builtin_array_findLastIndex));
  defmethod(js, array_proto, "flatMap", 7, js_mkfun(builtin_array_flatMap));
  defmethod(js, array_proto, "forEach", 7, js_mkfun(builtin_array_forEach));
  defmethod(js, array_proto, "indexOf", 7, js_mkfun(builtin_array_indexOf));
  defmethod(js, array_proto, "lastIndexOf", 11, js_mkfun(builtin_array_lastIndexOf));
  defmethod(js, array_proto, "reduceRight", 11, js_mkfun(builtin_array_reduceRight));
  defmethod(js, array_proto, "shift", 5, js_mkfun(builtin_array_shift));
  defmethod(js, array_proto, "unshift", 7, js_mkfun(builtin_array_unshift));
  defmethod(js, array_proto, "some", 4, js_mkfun(builtin_array_some));
  defmethod(js, array_proto, "sort", 4, js_mkfun(builtin_array_sort));
  defmethod(js, array_proto, "splice", 6, js_mkfun(builtin_array_splice));
  defmethod(js, array_proto, "copyWithin", 10, js_mkfun(builtin_array_copyWithin));
  defmethod(js, array_proto, "toReversed", 10, js_mkfun(builtin_array_toReversed));
  defmethod(js, array_proto, "toSorted", 8, js_mkfun(builtin_array_toSorted));
  defmethod(js, array_proto, "toSpliced", 9, js_mkfun(builtin_array_toSpliced));
  defmethod(js, array_proto, "with", 4, js_mkfun(builtin_array_with));
  defmethod(js, array_proto, "keys", 4, js_mkfun(builtin_array_keys));
  defmethod(js, array_proto, "values", 6, js_mkfun(builtin_array_values));
  defmethod(js, array_proto, "entries", 7, js_mkfun(builtin_array_entries));
  defmethod(js, array_proto, "toString", 8, js_mkfun(builtin_array_toString));
  defmethod(js, array_proto, "toLocaleString", 14, js_mkfun(builtin_array_toLocaleString));
  
  ant_value_t string_proto = js_mkobj(js);
  set_proto(js, string_proto, object_proto);
  
  defmethod(js, string_proto, "indexOf", 7, js_mkfun(builtin_string_indexOf));
  defmethod(js, string_proto, "substring", 9, js_mkfun(builtin_string_substring));
  defmethod(js, string_proto, "substr", 6, js_mkfun(builtin_string_substr));
  defmethod(js, string_proto, "split", 5, js_mkfun(builtin_string_split));
  defmethod(js, string_proto, "slice", 5, js_mkfun(builtin_string_slice));
  defmethod(js, string_proto, "includes", 8, js_mkfun(builtin_string_includes));
  defmethod(js, string_proto, "startsWith", 10, js_mkfun(builtin_string_startsWith));
  defmethod(js, string_proto, "endsWith", 8, js_mkfun(builtin_string_endsWith));
  defmethod(js, string_proto, "template", 8, js_mkfun(builtin_string_template));
  defmethod(js, string_proto, "charCodeAt", 10, js_mkfun(builtin_string_charCodeAt));
  defmethod(js, string_proto, "codePointAt", 11, js_mkfun(builtin_string_codePointAt));
  defmethod(js, string_proto, "toLowerCase", 11, js_mkfun(builtin_string_toLowerCase));
  defmethod(js, string_proto, "toUpperCase", 11, js_mkfun(builtin_string_toUpperCase));
  defmethod(js, string_proto, "toLocaleLowerCase", 17, js_mkfun(builtin_string_toLowerCase));
  defmethod(js, string_proto, "toLocaleUpperCase", 17, js_mkfun(builtin_string_toUpperCase));
  defmethod(js, string_proto, "trim", 4, js_mkfun(builtin_string_trim));
  defmethod(js, string_proto, "trimStart", 9, js_mkfun(builtin_string_trimStart));
  defmethod(js, string_proto, "trimEnd", 7, js_mkfun(builtin_string_trimEnd));
  defmethod(js, string_proto, "repeat", 6, js_mkfun(builtin_string_repeat));
  defmethod(js, string_proto, "padStart", 8, js_mkfun(builtin_string_padStart));
  defmethod(js, string_proto, "padEnd", 6, js_mkfun(builtin_string_padEnd));
  defmethod(js, string_proto, "charAt", 6, js_mkfun(builtin_string_charAt));
  defmethod(js, string_proto, "at", 2, js_mkfun(builtin_string_at));
  defmethod(js, string_proto, "lastIndexOf", 11, js_mkfun(builtin_string_lastIndexOf));
  defmethod(js, string_proto, "concat", 6, js_mkfun(builtin_string_concat));
  defmethod(js, string_proto, "localeCompare", 13, js_mkfun(builtin_string_localeCompare));
  defmethod(js, string_proto, "normalize", 9, js_mkfun(builtin_string_normalize));
  defmethod(js, string_proto, "valueOf", 7, js_mkfun(builtin_string_valueOf));
  defmethod(js, string_proto, "toString", 8, js_mkfun(builtin_string_toString));

  ant_value_t number_proto = js_mkobj(js);
  set_proto(js, number_proto, object_proto);
  
  defmethod(js, number_proto, "toString", 8, js_mkfun(builtin_number_toString));
  defmethod(js, number_proto, "toFixed", 7, js_mkfun(builtin_number_toFixed));
  defmethod(js, number_proto, "toPrecision", 11, js_mkfun(builtin_number_toPrecision));
  defmethod(js, number_proto, "toExponential", 13, js_mkfun(builtin_number_toExponential));
  defmethod(js, number_proto, "valueOf", 7, js_mkfun(builtin_number_valueOf));
  defmethod(js, number_proto, "toLocaleString", 14, js_mkfun(builtin_number_toLocaleString));
  
  ant_value_t boolean_proto = js_mkobj(js);
  set_proto(js, boolean_proto, object_proto);
  
  defmethod(js, boolean_proto, "valueOf", 7, js_mkfun(builtin_boolean_valueOf));
  defmethod(js, boolean_proto, "toString", 8, js_mkfun(builtin_boolean_toString));
  
  ant_value_t error_proto = js_mkobj(js);
  set_proto(js, error_proto, object_proto);
  
  js_setprop(js, error_proto, ANT_STRING("name"), ANT_STRING("Error"));
  js_setprop(js, error_proto, ANT_STRING("message"), js_mkstr(js, "", 0));
  defmethod(js, error_proto, "toString", 8, js_mkfun(builtin_Error_toString));
  
  ant_value_t err_ctor_obj = mkobj(js, 0);
  set_proto(js, err_ctor_obj, function_proto);
  set_slot(err_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_Error));
  js_setprop_nonconfigurable(js, err_ctor_obj, "prototype", 9, error_proto);
  js_setprop(js, err_ctor_obj, ANT_STRING("name"), ANT_STRING("Error"));
  
  ant_value_t err_ctor_func = js_obj_to_func(err_ctor_obj);
  js_setprop(js, glob, ANT_STRING("Error"), err_ctor_func);
  js_setprop(js, error_proto, js_mkstr(js, "constructor", 11), err_ctor_func);
  js_set_descriptor(js, error_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  defmethod(js, err_ctor_func, "isError", 7, js_mkfun(builtin_error_isError));
  defmethod(js, err_ctor_func, "captureStackTrace", 17, js_mkfun(builtin_error_captureStackTrace));
  js_setprop(js, err_ctor_func, ANT_STRING("stackTraceLimit"), js_mknum(10));
  
  #define REGISTER_ERROR_SUBTYPE(name_str) do { \
    ant_value_t proto = js_mkobj(js); \
    set_proto(js, proto, error_proto); \
    js_setprop(js, proto, ANT_STRING("name"), ANT_STRING(name_str)); \
    ant_value_t ctor = mkobj(js, 0); \
    set_proto(js, ctor, function_proto); \
    set_slot(ctor, SLOT_CFUNC, js_mkfun(builtin_Error)); \
    js_setprop_nonconfigurable(js, ctor, "prototype", 9, proto); \
    js_setprop(js, ctor, ANT_STRING("name"), ANT_STRING(name_str)); \
    ant_value_t ctor_func = js_obj_to_func(ctor); \
    js_setprop(js, proto, ANT_STRING("constructor"), ctor_func); \
    js_set_descriptor(js, proto, "constructor", 11, JS_DESC_W | JS_DESC_C); \
    js_setprop(js, glob, ANT_STRING(name_str), ctor_func); \
  } while(0)
  
  REGISTER_ERROR_SUBTYPE("EvalError");
  REGISTER_ERROR_SUBTYPE("RangeError");
  REGISTER_ERROR_SUBTYPE("ReferenceError");
  REGISTER_ERROR_SUBTYPE("SyntaxError");
  REGISTER_ERROR_SUBTYPE("TypeError");
  REGISTER_ERROR_SUBTYPE("URIError");
  REGISTER_ERROR_SUBTYPE("InternalError");
  
  #undef REGISTER_ERROR_SUBTYPE
  
  ant_value_t proto = js_mkobj(js);
  set_proto(js, proto, error_proto);
  js_setprop(js, proto, ANT_STRING("name"), ANT_STRING("AggregateError"));
  ant_value_t ctor = mkobj(js, 0);
  set_proto(js, ctor, function_proto);
  set_slot(ctor, SLOT_CFUNC, js_mkfun(builtin_AggregateError));
  js_setprop_nonconfigurable(js, ctor, "prototype", 9, proto);
  js_setprop(js, ctor, ANT_STRING("name"), ANT_STRING("AggregateError"));
  js_setprop(js, proto, ANT_STRING("constructor"), js_obj_to_func(ctor));
  js_set_descriptor(js, proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  js_setprop(js, glob, ANT_STRING("AggregateError"), js_obj_to_func(ctor));
  
  ant_value_t promise_proto = js_mkobj(js);
  set_proto(js, promise_proto, object_proto);
  defmethod(js, promise_proto, "then", 4, js_mkfun(builtin_promise_then));
  defmethod(js, promise_proto, "catch", 5, js_mkfun(builtin_promise_catch));
  defmethod(js, promise_proto, "finally", 7, js_mkfun(builtin_promise_finally));
  
  ant_value_t obj_func_obj = mkobj(js, 0);
  set_proto(js, obj_func_obj, function_proto);
  set_slot(obj_func_obj, SLOT_BUILTIN, tov(BUILTIN_OBJECT));
  js_mark_constructor(obj_func_obj, true);
  
  defmethod(js, obj_func_obj, "keys", 4, js_mkfun(builtin_object_keys));
  defmethod(js, obj_func_obj, "values", 6, js_mkfun(builtin_object_values));
  defmethod(js, obj_func_obj, "entries", 7, js_mkfun(builtin_object_entries));
  defmethod(js, obj_func_obj, "is", 2, js_mkfun(builtin_object_is));
  defmethod(js, obj_func_obj, "getPrototypeOf", 14, js_mkfun(builtin_object_getPrototypeOf));
  defmethod(js, obj_func_obj, "setPrototypeOf", 14, js_mkfun(builtin_object_setPrototypeOf));
  defmethod(js, obj_func_obj, "create", 6, js_mkfun(builtin_object_create));
  defmethod(js, obj_func_obj, "hasOwn", 6, js_mkfun(builtin_object_hasOwn));
  defmethod(js, obj_func_obj, "groupBy", 7, js_mkfun(builtin_object_groupBy));
  defmethod(js, obj_func_obj, "defineProperty", 14, js_mkfun(builtin_object_defineProperty));
  defmethod(js, obj_func_obj, "defineProperties", 16, js_mkfun(builtin_object_defineProperties));
  defmethod(js, obj_func_obj, "assign", 6, js_mkfun(builtin_object_assign));
  defmethod(js, obj_func_obj, "freeze", 6, js_mkfun(builtin_object_freeze));
  defmethod(js, obj_func_obj, "isFrozen", 8, js_mkfun(builtin_object_isFrozen));
  defmethod(js, obj_func_obj, "seal", 4, js_mkfun(builtin_object_seal));
  defmethod(js, obj_func_obj, "isSealed", 8, js_mkfun(builtin_object_isSealed));
  defmethod(js, obj_func_obj, "fromEntries", 11, js_mkfun(builtin_object_fromEntries));
  defmethod(js, obj_func_obj, "getOwnPropertyDescriptor", 24, js_mkfun(builtin_object_getOwnPropertyDescriptor));
  defmethod(js, obj_func_obj, "getOwnPropertyNames", 19, js_mkfun(builtin_object_getOwnPropertyNames));
  defmethod(js, obj_func_obj, "getOwnPropertySymbols", 21, js_mkfun(builtin_object_getOwnPropertySymbols));
  defmethod(js, obj_func_obj, "isExtensible", 12, js_mkfun(builtin_object_isExtensible));
  defmethod(js, obj_func_obj, "preventExtensions", 17, js_mkfun(builtin_object_preventExtensions));
  
  js_setprop(js, obj_func_obj, ANT_STRING("name"), ANT_STRING("Object"));
  js_setprop_nonconfigurable(js, obj_func_obj, "prototype", 9, object_proto);
  ant_value_t obj_func = js_obj_to_func(obj_func_obj);
  js_setprop(js, glob, js_mkstr(js, "Object", 6), obj_func);
  
  ant_value_t func_ctor_obj = mkobj(js, 0);
  set_proto(js, func_ctor_obj, function_proto);
  set_slot(func_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_Function));
  js_setprop_nonconfigurable(js, func_ctor_obj, "prototype", 9, function_proto);
  js_setprop(js, func_ctor_obj, js->length_str, tov(1.0));
  js_set_descriptor(js, func_ctor_obj, "length", 6, JS_DESC_C);
  js_setprop(js, func_ctor_obj, ANT_STRING("name"), ANT_STRING("Function"));
  ant_value_t func_ctor_func = js_obj_to_func(func_ctor_obj);
  js_setprop(js, glob, js_mkstr(js, "Function", 8), func_ctor_func);
  
  ant_value_t async_func_proto_obj = js_mkobj(js);
  set_proto(js, async_func_proto_obj, function_proto);
  set_slot(async_func_proto_obj, SLOT_ASYNC, js_true);
  ant_value_t async_func_proto = js_obj_to_func(async_func_proto_obj);
  set_slot(glob, SLOT_ASYNC_PROTO, async_func_proto);
  
  ant_value_t async_func_ctor_obj = mkobj(js, 0);
  set_proto(js, async_func_ctor_obj, function_proto);
  set_slot(async_func_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_AsyncFunction));
  js_setprop_nonconfigurable(js, async_func_ctor_obj, "prototype", 9, async_func_proto);
  js_setprop(js, async_func_ctor_obj, js->length_str, tov(1.0));
  js_set_descriptor(js, async_func_ctor_obj, "length", 6, JS_DESC_C);
  js_setprop(js, async_func_ctor_obj, ANT_STRING("name"), ANT_STRING("AsyncFunction"));
  ant_value_t async_func_ctor = js_obj_to_func(async_func_ctor_obj);
  
  js_setprop(js, async_func_proto_obj, js_mkstr(js, "constructor", 11), async_func_ctor);
  js_set_descriptor(js, async_func_proto_obj, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  ant_value_t str_ctor_obj = mkobj(js, 0);
  set_proto(js, str_ctor_obj, function_proto);
  set_slot(str_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_String));
  js_setprop_nonconfigurable(js, str_ctor_obj, "prototype", 9, string_proto);
  defmethod(js, str_ctor_obj, "fromCharCode", 12, js_mkfun(builtin_string_fromCharCode));
  defmethod(js, str_ctor_obj, "fromCodePoint", 13, js_mkfun(builtin_string_fromCodePoint));
  defmethod(js, str_ctor_obj, "raw", 3, js_mkfun(builtin_string_raw));
  js_setprop(js, str_ctor_obj, ANT_STRING("name"), ANT_STRING("String"));
  ant_value_t str_ctor_func = js_obj_to_func(str_ctor_obj);
  js_setprop(js, glob, js_mkstr(js, "String", 6), str_ctor_func);
  
  ant_value_t number_ctor_obj = mkobj(js, 0);
  set_proto(js, number_ctor_obj, function_proto);
  
  set_slot(number_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_Number));
  defmethod(js, number_ctor_obj, "isNaN", 5, js_mkfun(builtin_Number_isNaN));
  defmethod(js, number_ctor_obj, "isFinite", 8, js_mkfun(builtin_Number_isFinite));
  defmethod(js, number_ctor_obj, "isInteger", 9, js_mkfun(builtin_Number_isInteger));
  defmethod(js, number_ctor_obj, "isSafeInteger", 13, js_mkfun(builtin_Number_isSafeInteger));
  defmethod(js, number_ctor_obj, "parseInt", 8, js_mkfun(builtin_parseInt));
  defmethod(js, number_ctor_obj, "parseFloat", 10, js_mkfun(builtin_parseFloat));
  
  js_setprop(js, number_ctor_obj, js_mkstr(js, "MAX_VALUE", 9), tov(1.7976931348623157e+308));
  js_setprop(js, number_ctor_obj, js_mkstr(js, "MIN_VALUE", 9), tov(5e-324));
  js_setprop(js, number_ctor_obj, js_mkstr(js, "MAX_SAFE_INTEGER", 16), tov(9007199254740991.0));
  js_setprop(js, number_ctor_obj, js_mkstr(js, "MIN_SAFE_INTEGER", 16), tov(-9007199254740991.0));
  js_setprop(js, number_ctor_obj, js_mkstr(js, "POSITIVE_INFINITY", 17), tov(JS_INF));
  js_setprop(js, number_ctor_obj, js_mkstr(js, "NEGATIVE_INFINITY", 17), tov(JS_NEG_INF));
  js_setprop(js, number_ctor_obj, js_mkstr(js, "NaN", 3), tov(JS_NAN));
  js_setprop(js, number_ctor_obj, js_mkstr(js, "EPSILON", 7), tov(2.220446049250313e-16));
  
  js_setprop_nonconfigurable(js, number_ctor_obj, "prototype", 9, number_proto);
  js_setprop(js, number_ctor_obj, ANT_STRING("name"), ANT_STRING("Number"));
  ant_value_t number_ctor_func = js_obj_to_func(number_ctor_obj);
  js_setprop(js, glob, js_mkstr(js, "Number", 6), number_ctor_func);
  
  ant_value_t bool_ctor_obj = mkobj(js, 0);
  set_proto(js, bool_ctor_obj, function_proto);
  set_slot(bool_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_Boolean));
  js_setprop_nonconfigurable(js, bool_ctor_obj, "prototype", 9, boolean_proto);
  js_setprop(js, bool_ctor_obj, ANT_STRING("name"), ANT_STRING("Boolean"));
  ant_value_t bool_ctor_func = js_obj_to_func(bool_ctor_obj);
  js_setprop(js, glob, js_mkstr(js, "Boolean", 7), bool_ctor_func);
  
  ant_value_t arr_ctor_obj = mkobj(js, 0);
  set_proto(js, arr_ctor_obj, function_proto);
  set_slot(arr_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_Array));
  js_setprop_nonconfigurable(js, arr_ctor_obj, "prototype", 9, array_proto);
  defmethod(js, arr_ctor_obj, "isArray", 7, js_mkfun(builtin_Array_isArray));
  defmethod(js, arr_ctor_obj, "from", 4, js_mkfun(builtin_Array_from));
  defmethod(js, arr_ctor_obj, "of", 2, js_mkfun(builtin_Array_of));
  js_setprop(js, arr_ctor_obj, js->length_str, tov(1.0));
  js_set_descriptor(js, arr_ctor_obj, "length", 6, JS_DESC_C);
  js_setprop(js, arr_ctor_obj, ANT_STRING("name"), ANT_STRING("Array"));
  ant_value_t arr_ctor_func = js_obj_to_func(arr_ctor_obj);
  js_setprop(js, glob, js_mkstr(js, "Array", 5), arr_ctor_func);
  
  ant_value_t proxy_ctor_obj = mkobj(js, 0);
  set_proto(js, proxy_ctor_obj, function_proto);
  set_slot(proxy_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_Proxy));
  js_mark_constructor(proxy_ctor_obj, true);
  defmethod(js, proxy_ctor_obj, "revocable", 9, js_mkfun(builtin_Proxy_revocable));
  js_setprop(js, proxy_ctor_obj, ANT_STRING("name"), ANT_STRING("Proxy"));
  js_setprop(js, glob, js_mkstr(js, "Proxy", 5), js_obj_to_func(proxy_ctor_obj));
  
  ant_value_t p_ctor_obj = mkobj(js, 0);
  set_proto(js, p_ctor_obj, function_proto);
  set_slot(p_ctor_obj, SLOT_CFUNC, js_mkfun(builtin_Promise));
  
  defmethod(js, p_ctor_obj, "resolve", 7, js_mkfun(builtin_Promise_resolve));
  defmethod(js, p_ctor_obj, "reject", 6, js_mkfun(builtin_Promise_reject));
  defmethod(js, p_ctor_obj, "try", 3, js_mkfun(builtin_Promise_try));
  defmethod(js, p_ctor_obj, "withResolvers", 13, js_mkfun(builtin_Promise_withResolvers));
  defmethod(js, p_ctor_obj, "all", 3, js_mkfun(builtin_Promise_all));
  defmethod(js, p_ctor_obj, "allSettled", 10, js_mkfun(builtin_Promise_allSettled));
  defmethod(js, p_ctor_obj, "race", 4, js_mkfun(builtin_Promise_race));
  defmethod(js, p_ctor_obj, "any", 3, js_mkfun(builtin_Promise_any));
  
  js_setprop_nonconfigurable(js, p_ctor_obj, "prototype", 9, promise_proto);
  js_setprop(js, p_ctor_obj, ANT_STRING("name"), ANT_STRING("Promise"));
  js_setprop(js, glob, js_mkstr(js, "Promise", 7), js_obj_to_func(p_ctor_obj));
  
  defmethod(js, glob, "parseInt", 8, js_mkfun(builtin_parseInt));
  defmethod(js, glob, "parseFloat", 10, js_mkfun(builtin_parseFloat));
  defmethod(js, glob, "eval", 4, js_mkfun(builtin_eval));
  defmethod(js, glob, "isNaN", 5, js_mkfun(builtin_global_isNaN));
  defmethod(js, glob, "isFinite", 8, js_mkfun(builtin_global_isFinite));
  defmethod(js, glob, "btoa", 4, js_mkfun(builtin_btoa));
  defmethod(js, glob, "atob", 4, js_mkfun(builtin_atob));
  
  js_setprop(js, glob, js_mkstr(js, "NaN", 3), tov(JS_NAN));
  js_set_descriptor(js, glob, "NaN", 3, 0);
  js_setprop(js, glob, js_mkstr(js, "Infinity", 8), tov(JS_INF));
  js_set_descriptor(js, glob, "Infinity", 8, 0);
  js_setprop(js, glob, js_mkstr(js, "undefined", 9), js_mkundef());
  js_set_descriptor(js, glob, "undefined", 9, 0);
  
  ant_value_t import_obj = mkobj(js, 0);
  set_proto(js, import_obj, function_proto);
  
  set_slot(import_obj, SLOT_CFUNC, js_mkfun(js_builtin_import));
  js_setprop(js, glob, js_mkstr(js, "import", 6), js_obj_to_func(import_obj));
  
  js_setprop(js, object_proto, js_mkstr(js, "constructor", 11), obj_func);
  js_set_descriptor(js, object_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  js_setprop(js, function_proto, js_mkstr(js, "constructor", 11), func_ctor_func);
  js_set_descriptor(js, js_as_obj(function_proto), "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  js_setprop(js, array_proto, js_mkstr(js, "constructor", 11), arr_ctor_func);
  js_set_descriptor(js, array_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  js_setprop(js, string_proto, js_mkstr(js, "constructor", 11), str_ctor_func);
  js_set_descriptor(js, string_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  js_setprop(js, number_proto, js_mkstr(js, "constructor", 11), number_ctor_func);
  js_set_descriptor(js, number_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  js_setprop(js, boolean_proto, js_mkstr(js, "constructor", 11), bool_ctor_func);
  js_set_descriptor(js, boolean_proto, "constructor", 11, JS_DESC_W | JS_DESC_C);
  
  set_proto(js, glob, object_proto);
  
  js->object = object_proto;
  js->owns_mem = false;
  js->max_size = 0;
  
  return js;
}

ant_t *js_create_dynamic() {
  ant_t *js = (ant_t *)calloc(1, sizeof(*js));
  if (js == NULL) return NULL;
  if (js_create(js, sizeof(*js)) == NULL) {
    free(js);
    return NULL;
  }
  js->owns_mem = true;
  js->vm = sv_vm_create(js, SV_VM_MAIN);
  return js;
}

void js_destroy(ant_t *js) {
  if (js == NULL) return;
  
  if (js->vm) {
    sv_vm_destroy(js->vm);
    js->vm = NULL;
  }
  
  js_esm_cleanup_module_cache();
  code_arena_reset();
  cleanup_lmdb_module();

  ant_object_t *lists[] = { js->objects, js->objects_old, js->permanent_objects };
  for (int i = 0; i < 3; i++) for (ant_object_t *obj = lists[i]; obj;) {
    ant_object_t *next = obj->next;
    gc_object_free(js, obj);
    obj = next;
  }

  js->objects = NULL;
  js->objects_old = NULL;
  js->permanent_objects = NULL;
  
  cleanup_buffer_module();
  fixed_arena_destroy(&js->obj_arena);
  fixed_arena_destroy(&js->closure_arena);
  fixed_arena_destroy(&js->upvalue_arena);

  free(js->prop_refs);
  js->prop_refs = NULL;
  js->prop_refs_len = js->prop_refs_cap = 0;
  
  free(js->c_roots);
  js->c_roots = NULL;
  js->c_root_count = js->c_root_cap = 0;
  
  free(js->pending_rejections.items);
  js->pending_rejections.items = NULL;
  js->pending_rejections.len = js->pending_rejections.cap = 0;

  js_pool_destroy(&js->pool.rope);
  js_pool_destroy(&js->pool.symbol);
  js_pool_destroy(&js->pool.permanent);
  
  js_class_pool_destroy(&js->pool.bigint);
  js_class_pool_destroy(&js->pool.string);

  destroy_runtime(js);
  if (js->owns_mem) free(js);
}

inline double js_getnum(ant_value_t value) { return tod(value); }
inline void js_setstackbase(ant_t *js, void *base) { js->cstk.base = base; js->cstk.main_base = base; }
inline void js_setstacklimit(ant_t *js, size_t max) { js->cstk.limit = max; }
inline void js_set_filename(ant_t *js, const char *filename) { js->filename = filename; }

inline ant_value_t js_mkundef(void) { return mkval(T_UNDEF, 0); }
inline ant_value_t js_mknull(void) { return mkval(T_NULL, 0); }
inline ant_value_t js_mknum(double value) { return tov(value); }
inline ant_value_t js_mkobj(ant_t *js) { return mkobj(js, 0); }
inline ant_value_t js_glob(ant_t *js) { return js->global; }
inline ant_value_t js_mkfun(ant_value_t (*fn)(ant_t *, ant_value_t *, int)) { return mkval(T_CFUNC, (size_t) (void *) fn); }

inline ant_value_t js_getthis(ant_t *js) { return js->this_val; }
inline void js_setthis(ant_t *js, ant_value_t val) { js->this_val = val; }
inline ant_value_t js_getcurrentfunc(ant_t *js) { return js->current_func; }

ant_value_t js_heavy_mkfun(ant_t *js, ant_value_t (*fn)(ant_t *, ant_value_t *, int), ant_value_t data) {
  ant_value_t cfunc = js_mkfun(fn);
  ant_value_t fn_obj = mkobj(js, 0);
  
  set_slot(fn_obj, SLOT_CFUNC, cfunc);
  set_slot(fn_obj, SLOT_DATA, data);
  
  return js_obj_to_func(fn_obj);
}

void js_set(ant_t *js, ant_value_t obj, const char *key, ant_value_t val) {
  size_t key_len = strlen(key);
  
  if (vtype(obj) == T_OBJ) {
    ant_offset_t existing = lkp(js, obj, key, key_len);
    if (existing > 0) {
      if (is_const_prop(js, existing)) {
        js_mkerr(js, "assignment to constant");
        return;
      }
      js_saveval(js, existing, val);
    } else {
      ant_value_t key_str = js_mkstr(js, key, key_len);
      mkprop(js, obj, key_str, val, 0);
    }
  } else if (vtype(obj) == T_FUNC) {
    ant_value_t func_obj = js_func_obj(obj);
    ant_offset_t existing = lkp(js, func_obj, key, key_len);
    if (existing > 0) {
      if (is_const_prop(js, existing)) {
        js_mkerr(js, "assignment to constant");
        return;
      }
      js_saveval(js, existing, val);
    } else {
      ant_value_t key_str = js_mkstr(js, key, key_len);
      mkprop(js, func_obj, key_str, val, 0);
    }
  }
}

void js_set_sym(ant_t *js, ant_value_t obj, ant_value_t sym, ant_value_t val) {
  if (vtype(sym) != T_SYMBOL) return;
  ant_offset_t sym_off = (ant_offset_t)vdata(sym);
  
  if (vtype(obj) == T_FUNC) obj = js_func_obj(obj);
  if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR) return;
  
  ant_offset_t existing = lkp_sym(js, obj, sym_off);
  if (existing > 0) {
    if (is_const_prop(js, existing)) return;
    js_saveval(js, existing, val);
  } else mkprop(js, obj, sym, val, 0);
}

ant_value_t js_get_sym_with_receiver(ant_t *js, ant_value_t obj, ant_value_t sym, ant_value_t receiver) {
  if (vtype(sym) != T_SYMBOL) return js_mkundef();
  ant_offset_t sym_off = (ant_offset_t)vdata(sym);

  if (vtype(obj) == T_FUNC) obj = js_func_obj(obj);
  uint8_t ot = vtype(obj);
  
  if (!is_object_type(obj)) {
    if (ot == T_STR || ot == T_NUM || ot == T_BOOL || ot == T_BIGINT || ot == T_SYMBOL) {
      ant_value_t proto = get_prototype_for_type(js, ot);
      if (!is_object_type(proto)) return js_mkundef();
      obj = js_as_obj(proto);
    } else return js_mkundef();
  } else obj = js_as_obj(obj);

  if (is_proxy(obj)) 
    return proxy_get_val(js, obj, sym);

  ant_value_t cur = obj;
  proto_overflow_guard_t guard;
  proto_overflow_guard_init(&guard);
  
  while (is_object_type(cur)) {
    ant_value_t cur_obj = js_as_obj(cur);
    prop_meta_t meta;
    if (lookup_symbol_prop_meta(cur_obj, sym_off, &meta)) {
      if (meta.has_getter) {
        ant_value_t g = meta.getter;
        if (vtype(g) == T_FUNC || vtype(g) == T_CFUNC)
          return sv_vm_call(js->vm, js, g, receiver, NULL, 0, NULL, false);
        return js_mkundef();
      }
      if (meta.has_setter && !meta.has_getter) return js_mkundef();
      break;
    }

    ant_value_t proto = get_proto(js, cur_obj);
    if (!is_object_type(proto)) break;
    cur = js_as_obj(proto);
    if (proto_overflow_guard_hit_cycle(js, &guard, cur)) break;
  }
  
  ant_offset_t off = lkp_sym_proto(js, obj, sym_off);
  if (off == 0) return js_mkundef();
  return propref_load(js, off);
}

ant_value_t js_get_sym(ant_t *js, ant_value_t obj, ant_value_t sym) {
  return js_get_sym_with_receiver(js, obj, sym, obj);
}

static bool js_try_get(ant_t *js, ant_value_t obj, const char *key, ant_value_t *out) {
  size_t key_len = strlen(key);
  
  if (vtype(obj) == T_FUNC) {
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
    ant_value_t import_meta = js_get_module_ctx_import_meta(js, js_get_slot(func_obj, SLOT_MODULE_CTX));
    if (vtype(import_meta) == T_UNDEF) import_meta = js_get_current_import_meta(js);
    if (key_len == 4 && memcmp(key, "meta", 4) == 0 && vtype(import_meta) != T_UNDEF) {
      ant_value_t cfunc = js_get_slot(func_obj, SLOT_CFUNC);
      if (vtype(cfunc) == T_CFUNC && js_as_cfunc(cfunc) == js_builtin_import) {
        *out = import_meta;
        return true;
      }
    }
    ant_offset_t off = lkp(js, func_obj, key, key_len);
    if (off == 0) {
      ant_value_t accessor_result;
      if (try_accessor_getter(js, obj, key, key_len, &accessor_result)) {
        *out = accessor_result;
        return true;
      }
      return false;
    }

    const ant_shape_prop_t *prop_meta = prop_shape_meta(js, off);
    if (prop_meta && prop_meta->has_getter) {
      ant_value_t accessor_result;
      if (try_accessor_getter(js, obj, key, key_len, &accessor_result)) {
        *out = accessor_result;
        return true;
      }
    }

    *out = propref_load(js, off);
    return true;
  }
  
  if (array_obj_ptr(obj)) {
    if (((key_len == 6 && memcmp(key, "callee", 6) == 0) ||
         (key_len == 6 && memcmp(key, "caller", 6) == 0)) &&
        vtype(get_slot(obj, SLOT_STRICT_ARGS)) != T_UNDEF) {
      *out = js_mkerr_typed(js, JS_ERR_TYPE,
                            "'%.*s' not allowed on strict arguments",
                            (int)key_len, key);
      return true;
    }

    if (is_length_key(key, key_len)) {
      *out = tov((double)get_array_length(js, obj));
      return true;
    }
    unsigned long idx;
    ant_offset_t arr_len = get_array_length(js, obj);
    if (parse_array_index(key, key_len, arr_len, &idx)) {
      if (arr_has(js, obj, (ant_offset_t)idx)) {
        *out = arr_get(js, obj, (ant_offset_t)idx);
        return true;
      } return false;
    }
    
    ant_value_t arr_obj = js_as_obj(obj);
    ant_offset_t off = lkp(js, arr_obj, key, key_len);
    if (off == 0) {
      ant_value_t accessor_result;
      if (try_accessor_getter(js, arr_obj, key, key_len, &accessor_result)) {
        *out = accessor_result; return true;
      } return false;
    }
    
    const ant_shape_prop_t *prop_meta = prop_shape_meta(js, off);
    if (prop_meta && prop_meta->has_getter) {
      ant_value_t accessor_result;
      if (try_accessor_getter(js, arr_obj, key, key_len, &accessor_result)) {
        *out = accessor_result; return true;
      }
    }
    
    *out = propref_load(js, off);
    return true;
  }

  uint8_t t = vtype(obj);
  bool is_promise = (t == T_PROMISE);
  if (t == T_OBJ && is_proxy(obj)) {
    *out = proxy_get(js, obj, key, key_len);
    return true;
  }
  
  if (t == T_STR || t == T_NUM || t == T_BOOL) {
    if (t == T_STR && is_length_key(key, key_len)) {
      ant_offset_t byte_len = 0; ant_offset_t str_off = vstr(js, obj, &byte_len);
      const char *str_data = (const char *)(uintptr_t)(str_off);
      *out = tov((double)utf16_strlen(str_data, byte_len));
      return true;
    }
    
    if (t == T_STR && js_try_get_string_index(js, obj, key, key_len, out)) return true;
    ant_value_t boxed = mkobj(js, 0);
    
    js_set_slot(js_as_obj(boxed), SLOT_PRIMITIVE, obj);
    obj = boxed; t = T_OBJ;
  }
  
  if (is_promise) obj = js_as_obj(obj);
  else if (t != T_OBJ) return false;
  ant_offset_t off = lkp(js, obj, key, key_len);
  
  if (off == 0) {
    ant_value_t result = try_dynamic_getter(js, obj, key, key_len);
    if (vtype(result) != T_UNDEF) { *out = result; return true; }
  }
  
  if (off == 0 && is_promise) {
    ant_value_t promise_proto = get_ctor_proto(js, "Promise", 7);
    if (vtype(promise_proto) != T_UNDEF && vtype(promise_proto) != T_NULL) {
      off = lkp(js, promise_proto, key, key_len);
      if (off != 0) { *out = propref_load(js, off); return true; }
    }
  }
  
  if (off == 0) {
    ant_value_t accessor_result;
    if (try_accessor_getter(js, obj, key, key_len, &accessor_result)) {
      *out = accessor_result; return true;
    }
    return false;
  }
  
  const ant_shape_prop_t *prop_meta = prop_shape_meta(js, off);
  if (prop_meta && prop_meta->has_getter) {
    ant_value_t accessor_result;
    if (try_accessor_getter(js, obj, key, key_len, &accessor_result)) {
      *out = accessor_result; return true;
    }
  }
  
  *out = propref_load(js, off);
  return true;
}

ant_value_t js_get(ant_t *js, ant_value_t obj, const char *key) {
  ant_value_t val;
  if (js_try_get(js, obj, key, &val)) return val;
  return js_mkundef();
}

ant_value_t js_getprop_proto(ant_t *js, ant_value_t obj, const char *key) {
  size_t key_len = strlen(key);
  ant_offset_t off = lkp_proto(js, obj, key, key_len);
  return off == 0 ? js_mkundef() : propref_load(js, off);
}

ant_value_t js_getprop_fallback(ant_t *js, ant_value_t obj, const char *name) {
  ant_value_t val;
  if (js_try_get(js, obj, name, &val)) return val;
  return js_getprop_proto(js, obj, name);
}

ant_value_t js_getprop_super(ant_t *js, ant_value_t super_obj, ant_value_t receiver, const char *name) {
  if (!name) return js_mkundef();

  if (vtype(super_obj) == T_FUNC) super_obj = js_func_obj(super_obj);
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
          if (vtype(getter) == T_FUNC || vtype(getter) == T_CFUNC)
            return sv_vm_call(js->vm, js, getter, receiver, NULL, 0, NULL, false);
          return js_mkundef();
        }
        if (prop && prop->has_setter) return js_mkundef();
        handled = true;
      }
    }

    if (!handled && cur_ptr && cur_ptr->is_exotic) {
      descriptor_entry_t *desc = lookup_descriptor(cur_obj, name, key_len);
      if (desc) {
        if (desc->has_getter) {
          ant_value_t getter = desc->getter;
          if (vtype(getter) == T_FUNC || vtype(getter) == T_CFUNC)
            return sv_vm_call(js->vm, js, getter, receiver, NULL, 0, NULL, false);
          return js_mkundef();
        }
        if (desc->has_setter) return js_mkundef();
      }
    }

    ant_offset_t prop_off = lkp_interned(js, cur_obj, key_intern, key_len);
    if (prop_off != 0) return propref_load(js, prop_off);

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
  (void)js;
  if (vtype(value) != T_STR) return NULL;
  ant_offset_t n, off = vstr(js, value, &n);
  if (len != NULL) *len = n;
  return (char *)(uintptr_t)off;
}

void js_merge_obj(ant_t *js, ant_value_t dst, ant_value_t src) {
  if (vtype(dst) != T_OBJ || vtype(src) != T_OBJ) return;
  ant_value_t as_src = js_as_obj(src);
  ant_object_t *src_obj = js_obj_ptr(as_src);
  if (!src_obj || !src_obj->shape) return;

  uint32_t count = ant_shape_count(src_obj->shape);
  for (uint32_t i = 0; i < count; i++) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(src_obj->shape, i);
    if (!prop || prop->type != ANT_SHAPE_KEY_STRING) continue;

    const char *key = prop->key.interned;
    ant_value_t val = (i < src_obj->prop_count) ? ant_object_prop_get_unchecked(src_obj, i) : js_mkundef();
    js_setprop(js, dst, js_mkstr(js, key, strlen(key)), val);
  }
}

bool js_chkargs(ant_value_t *args, int nargs, const char *spec) {
  int i = 0, ok = 1;
  for (; ok && i < nargs && spec[i]; i++) {
    uint8_t t = vtype(args[i]), c = (uint8_t) spec[i];
    ok = (c == 'b' && t == T_BOOL) || (c == 'd' && t == T_NUM) ||
         (c == 's' && t == T_STR) || (c == 'j');
  }
  if (spec[i] != '\0' || i != nargs) ok = 0;
  return ok;
}

static ant_value_t js_eval_bytecode_mode(ant_t *js, const char *buf, size_t len, sv_compile_mode_t mode, bool parse_strict) {
  if (len == (size_t)~0U) len = strlen(buf);
  sv_ast_t *program = sv_parse(js, buf, (ant_offset_t)len, parse_strict);

  if (!program) {
    if (js->thrown_exists) return mkval(T_ERR, 0);
    return js_mkerr_typed(js, JS_ERR_INTERNAL | JS_ERR_NO_STACK, "Unexpected parse error");
  }

  if (mode == SV_COMPILE_MODULE) {
    ant_value_t ns = js_module_eval_active_ns(js);
    if (is_object_type(ns)) esm_predeclare_exports(js, program, ns);
  }

  sv_func_t *func = sv_compile(js, program, mode, buf, (ant_offset_t)len);
  if (!func) {
    if (js->thrown_exists) return mkval(T_ERR, 0);
    return js_mkerr_typed(js, JS_ERR_INTERNAL | JS_ERR_NO_STACK, "Unexpected compile error");
  }
  
  js_clear_error_site(js);   
  ant_value_t result;
  // TODO: this-newtarget-frame-migration
  ant_value_t saved_this = js->this_val;

  if (sv_dump_bytecode_unlikely) sv_disasm(js, func, js->filename);
  if (func->is_tla) result = sv_execute_entry_tla(js, func, js->this_val);
  else result = sv_execute_entry(sv_vm_get_active(js), func, js->this_val, NULL, 0);

  js->this_val = saved_this;
  return result;
}

ant_value_t js_eval_bytecode(ant_t *js, const char *buf, size_t len) {
  return js_eval_bytecode_mode(js, buf, len, SV_COMPILE_SCRIPT, false);
}

ant_value_t js_eval_bytecode_module(ant_t *js, const char *buf, size_t len) {
  return js_eval_bytecode_mode(js, buf, len, SV_COMPILE_MODULE, false);
}

ant_value_t js_eval_bytecode_eval(ant_t *js, const char *buf, size_t len) {
  return js_eval_bytecode_mode(js, buf, len, SV_COMPILE_EVAL, false);
}

ant_value_t js_eval_bytecode_eval_with_strict(ant_t *js, const char *buf, size_t len, bool inherit_strict) {
  return js_eval_bytecode_mode(js, buf, len, SV_COMPILE_EVAL, inherit_strict);
}

ant_value_t js_eval_bytecode_repl(ant_t *js, const char *buf, size_t len) {
  return js_eval_bytecode_mode(js, buf, len, SV_COMPILE_REPL, false);
}

ant_value_t inline sv_call_cfunc(ant_params_t, ant_bind_t) {
  ant_value_t saved_this = js->this_val;
  js->this_val = this_val;
  ant_value_t res = js_as_cfunc(func)(js, args, nargs);
  js->this_val = saved_this;
  return res;
}

ant_value_t inline sv_call_slot_cfunc(ant_params_t, ant_bind_t, ant_value_t cfunc_slot) {
  ant_value_t saved_func = js->current_func;
  ant_value_t saved_this = js->this_val;
  js->current_func = func;
  js->this_val = this_val;
  ant_value_t res = js_as_cfunc(cfunc_slot)(js, args, nargs);
  js->current_func = saved_func;
  js->this_val = saved_this;
  return res;
}


ant_value_t inline sv_call_object_builtin(ant_params_t, ant_value_t this_val) {
  ant_value_t saved_this = js->this_val;
  js->this_val = this_val;
  ant_value_t res = builtin_Object(js, args, nargs);
  js->this_val = saved_this;
  return res;
}

ant_value_t sv_call_native(
  ant_t *js, ant_value_t func, ant_value_t this_val,
  ant_value_t *args, int nargs
) {
  if (vtype(func) == T_CFUNC) return sv_call_cfunc(js, args, nargs, func, this_val);
  if (vtype(func) == T_FFI) return ffi_call_by_index(js, (unsigned int)vdata(func), args, nargs);
  
  if (vtype(func) == T_FUNC) {
    ant_value_t func_obj = js_func_obj(func);
    ant_value_t cfunc_slot = get_slot(func_obj, SLOT_CFUNC);
    
    if (vtype(cfunc_slot) == T_CFUNC) 
      return sv_call_slot_cfunc(js, args, nargs, func, this_val, cfunc_slot);
      
    ant_value_t builtin_slot = get_slot(func_obj, SLOT_BUILTIN);
    if (vtype(builtin_slot) == T_NUM && (int)tod(builtin_slot) == BUILTIN_OBJECT) 
      return sv_call_object_builtin(js, args, nargs, this_val);
  }

  return js_mkerr_typed(js, JS_ERR_TYPE, "%s is not a function", typestr(vtype(func)));
}

typedef struct {
  ant_t *js;
  ant_object_t *obj;
  uint32_t index;
} prop_iter_ctx_t;

ant_iter_t js_prop_iter_begin(ant_t *js, ant_value_t obj) {
  ant_iter_t iter = {.ctx = NULL, .off = 0};
  uint8_t t = vtype(obj);
  if (t != T_OBJ && t != T_ARR && t != T_FUNC) return iter;

  prop_iter_ctx_t *ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return iter;
  
  ctx->js = js;
  ctx->obj = js_obj_ptr(js_as_obj(obj));
  ctx->index = 0;
  
  if (!ctx->obj || !ctx->obj->shape) {
    free(ctx);
    return iter;
  }
  
  iter.ctx = ctx;
  return iter;
}

bool js_prop_iter_next(ant_iter_t *iter, const char **key, size_t *key_len, ant_value_t *value) {
  if (!iter || !iter->ctx) return false;
  prop_iter_ctx_t *ctx = (prop_iter_ctx_t *)iter->ctx;
  
  ant_object_t *obj = ctx->obj;
  if (!obj || !obj->shape) return false;

  uint32_t count = ant_shape_count(obj->shape);
  while (ctx->index < count) {
    uint32_t i = ctx->index++;
    const ant_shape_prop_t *prop = ant_shape_prop_at(obj->shape, i);
    if (!prop) continue;
    if (prop->type == ANT_SHAPE_KEY_SYMBOL) continue;
    if (i >= obj->prop_count) continue;

    if (key) {
      *key = prop->key.interned;
      if (key_len) *key_len = strlen(prop->key.interned);
    }
    
    if (value) *value = ant_object_prop_get_unchecked(obj, i);
    iter->off = i + 1;
    
    return true;
  }

  return false;
}

bool js_prop_iter_next_val(ant_iter_t *iter, ant_value_t *key_out, ant_value_t *value) {
  if (!iter || !iter->ctx) return false;
  prop_iter_ctx_t *ctx = (prop_iter_ctx_t *)iter->ctx;
  
  ant_object_t *obj = ctx->obj;
  if (!obj || !obj->shape) return false;
  uint32_t count = ant_shape_count(obj->shape);
  
  while (ctx->index < count) {
    uint32_t i = ctx->index++;
    const ant_shape_prop_t *prop = ant_shape_prop_at(obj->shape, i);
    
    if (!prop) continue;
    if (i >= obj->prop_count) continue;

    if (key_out) {
      if (prop->type == ANT_SHAPE_KEY_SYMBOL) *key_out = mkval(T_SYMBOL, prop->key.sym_off);
      else *key_out = js_mkstr(ctx->js, prop->key.interned, strlen(prop->key.interned));
    }
    
    if (value) *value = ant_object_prop_get_unchecked(obj, i);
    iter->off = i + 1;
    
    return true;
  }

  return false;
}

void js_prop_iter_end(ant_iter_t *iter) {
  if (!iter) return;
  free(iter->ctx);
  iter->off = 0;
  iter->ctx = NULL;
}

void js_check_unhandled_rejections(ant_t *js) {
  size_t keep = 0;
  
  for (size_t i = 0; i < js->pending_rejections.len; i++) {
    ant_value_t p = js->pending_rejections.items[i];
    ant_promise_state_t *pd = get_promise_data(js, p, false);
    if (!pd || pd->has_rejection_handler || pd->unhandled_reported) continue;
    
    if (vtype(pd->trigger_parent) == T_PROMISE) {
      ant_promise_state_t *parent = get_promise_data(js, pd->trigger_parent, false);
      if (parent && parent->has_rejection_handler) continue;
    }
    
    if (js->fatal_error) {
      js->thrown_exists = true;
      js->thrown_value = pd->value;
      print_uncaught_throw(js);
      js_destroy(js); exit(1);
    }

    GC_ROOT_SAVE(root_mark, js);
    ant_value_t reason = pd->value;
    GC_ROOT_PIN(js, p); GC_ROOT_PIN(js, reason);
    
    if (!js_fire_unhandled_rejection(js, p, reason))
      print_unhandled_promise_rejection(js, reason);
      
    GC_ROOT_RESTORE(js, root_mark);
    pd->unhandled_reported = true;
  }
  
  js->pending_rejections.len = keep;
}

void js_set_getter(ant_value_t obj, js_getter_fn getter) {
  if (!is_object_type(obj)) return;
  if (vtype(obj) != T_OBJ) obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return;
  ptr->is_exotic = 1;
  ant_exotic_ops_t *ops = obj_ensure_exotic_ops(ptr);
  if (!ops) return;
  ops->getter = getter;
}

void js_set_setter(ant_value_t obj, js_setter_fn setter) {
  if (!is_object_type(obj)) return;
  if (vtype(obj) != T_OBJ) obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return;
  ptr->is_exotic = 1;
  ant_exotic_ops_t *ops = obj_ensure_exotic_ops(ptr);
  if (!ops) return;
  ops->setter = setter;
}

void js_set_deleter(ant_value_t obj, js_deleter_fn deleter) {
  if (!is_object_type(obj)) return;
  if (vtype(obj) != T_OBJ) obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return;
  ptr->is_exotic = 1;
  ant_exotic_ops_t *ops = obj_ensure_exotic_ops(ptr);
  if (!ops) return;
  ops->deleter = deleter;
}

void js_set_finalizer(ant_value_t obj, js_finalizer_fn fn) {
  if (!is_object_type(obj)) return;
  if (vtype(obj) != T_OBJ) obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return;
  ptr->finalizer = fn;
}

void js_set_keys(ant_value_t obj, js_keys_fn keys) {
  if (!is_object_type(obj)) return;
  if (vtype(obj) != T_OBJ) obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(obj);
  if (!ptr) return;
  ptr->is_exotic = 1;
  ptr->exotic_keys = keys;
}
