#ifndef ANT_INTERNAL_H
#define ANT_INTERNAL_H

#include "ant.h"
#include "object.h"
#include "pool.h"
#include "sugar.h"
#include "errors.h"
#include "arena.h"
#include "descriptors.h"
#include "esm/loader.h"

#include <assert.h>
#include <string.h>

// An IEEE 754 double-precision float is a 64-bit value with bits laid out like:
//
// 1 Sign bit
// | 11 Exponent bits
// | |          52 Mantissa (i.e. fraction) bits
// | |          |
// S[Exponent-][Mantissa------------------------------------------]
//
// A NaN is any value where all exponent bits are set and the mantissa is
// non-zero. That means there are a *lot* of bit patterns that all represent
// NaN. NaN tagging takes advantage of this by repurposing those unused
// patterns to encode non-numeric values.
//
// We define a NANBOX_PREFIX as the upper 12 bits all set (0xFFF0...):
//
// 1111 1111 1111 0000 0000 0000 ... 0000
// [sign+exp all 1s  ] [mantissa all 0s  ]
//
// This corresponds to -Infinity in IEEE 754. Any 64-bit value strictly
// greater than this prefix is a tagged (non-numeric) value. Any value less
// than or equal to it is an unmodified double — so numeric math is free.
//
// For tagged values, we carve the remaining 52 mantissa bits into two fields:
//
// 1111 1111 1111 TTTTT DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD
// [-- prefix ---][type][--------------- 47-bit data --------------------]
//
// The 5-bit type tag (bits 51–47) encodes up to 31 distinct types: objects,
// strings, booleans, undefined, null, functions, closures, errors, etc.
// The 47-bit data field holds either a heap offset (for heap-resident types
// like objects and strings) or an immediate value (e.g. 1 for true, 0 for
// false).
//
// Encoding and decoding are simple:
//
//   mkval(type, data) = PREFIX | (type << 47) | (data & 0x7FFFFFFFFFFF)
//   vtype(v)          = is_tagged(v) ? (v >> 47) & 0x1F : T_NUM
//   vdata(v)          = v & 0x7FFFFFFFFFFF
//   is_tagged(v)      = v > PREFIX

#define NANBOX_TYPE_MASK   0x1F
#define NANBOX_TYPE_SHIFT  0x2F
#define NANBOX_HEAP_OFFSET 0x8
#define NANBOX_PREFIX      0xFFF0000000000000ULL
#define NANBOX_DATA_MASK   0x00007FFFFFFFFFFFULL

#define JS_ERR_NO_STACK  (1 << 8)
#define JS_TPFLG(t)      (1u << (t))

#define MAX_STRINGIFY_DEPTH   64
#define MAX_PROTO_CHAIN_DEPTH 256
#define MAX_MULTIREF_OBJS     128
#define MAX_DENSE_INITIAL_CAP 8

#define PROTO_WALK_F_OBJECT_ONLY (1u << 0)
#define PROTO_WALK_F_LOOKUP      (1u << 1)

#define ROPE_MAX_DEPTH         255
#define ROPE_FLATTEN_THRESHOLD (512 * 1024)

#define T_EMPTY                (NANBOX_PREFIX | ((ant_value_t)T_SENTINEL << NANBOX_TYPE_SHIFT) | 0xDEADULL)
#define T_SPECIAL_OBJECT_MASK  (JS_TPFLG(T_OBJ)  | JS_TPFLG(T_ARR))
#define T_NEEDS_PROTO_FALLBACK (JS_TPFLG(T_FUNC) | JS_TPFLG(T_ARR) | JS_TPFLG(T_PROMISE) | JS_TPFLG(T_GENERATOR))
#define T_OBJECT_MASK          (JS_TPFLG(T_OBJ)  | JS_TPFLG(T_ARR) | JS_TPFLG(T_FUNC) | JS_TPFLG(T_PROMISE) | JS_TPFLG(T_GENERATOR))
#define T_NON_NUMERIC_MASK     (JS_TPFLG(T_STR)  | JS_TPFLG(T_ARR) | JS_TPFLG(T_FUNC) | JS_TPFLG(T_CFUNC) | JS_TPFLG(T_OBJ) | JS_TPFLG(T_GENERATOR))

#define is_non_numeric(v)    ((1u << vtype(v)) & T_NON_NUMERIC_MASK)
#define is_object_type(v)    ((1u << vtype(v)) & T_OBJECT_MASK)
#define is_special_object(v) ((1u << vtype(v)) & T_SPECIAL_OBJECT_MASK)

enum {
  // heap-resident
  T_OBJ = 0,
  T_STR = 1,
  T_ARR = 2,

  // objects
  T_FUNC,
  T_CFUNC,
  T_CLOSURE,
  T_PROMISE,
  T_GENERATOR,

  // primitives
  T_UNDEF,
  T_NULL,
  T_BOOL,
  T_NUM,
  T_BIGINT,
  T_SYMBOL,

  // internal
  T_ERR,
  T_TYPEDARRAY,
  T_FFI,
  T_NTARG,

  // collections 
  T_MAP,
  T_SET,
  T_WEAKMAP,
  T_WEAKSET,

  T_SENTINEL = NANBOX_TYPE_MASK
};

typedef struct {
  const char *src;
  const char *filename;
  ant_offset_t src_len;
  ant_offset_t off;
  ant_offset_t span_len;
  bool valid;
} js_error_site_t;

struct ant_isolate_t {
  sv_vm_t *vm;
  
  ant_module_t *module;
  ant_object_t *objects;
  ant_object_t *permanent_objects;
  
  ant_fixed_arena_t obj_arena;
  ant_prop_ref_t *prop_refs;

  ant_fixed_arena_t closure_arena;
  ant_fixed_arena_t upvalue_arena;
  
  ant_value_t **c_roots;
  size_t c_root_count;
  size_t c_root_cap;
  struct gc_temp_root_scope *temp_roots;

  const char *code;
  const char *filename;

  #ifdef ANT_JIT
  void *jit_ctx;
  #endif
  
  ant_value_t global;
  ant_value_t this_val;
  ant_value_t new_target;
  ant_value_t current_func;
  ant_value_t length_str;
  
  struct {
    const char *length;
    const char *buffer;
    const char *prototype;
    const char *constructor;
    const char *name;
    const char *message;
    const char *done;
    const char *value;
    const char *get;
    const char *set;
    const char *arguments;
    const char *callee;
    const char *idx[10];
  } intern;
  
  ant_value_t thrown_value;
  ant_value_t thrown_stack;

  struct {
    void *base;
    void *main_base;
    void *main_lo;
    size_t limit;
  } cstk;

  struct {
    uint64_t counter;
    struct sym_registry_entry *registry;
    
    ant_value_t object_proto;
    ant_value_t array_proto;
    ant_value_t iterator_proto;
    ant_value_t array_iterator_proto;
    ant_value_t string_iterator_proto;
    ant_value_t generator_proto;
  } sym;
  
  ant_offset_t max_size;
  ant_offset_t prop_refs_len;
  ant_offset_t prop_refs_cap;
  js_error_site_t errsite;

  struct {
    ant_pool_t rope;
    ant_pool_t symbol;
    ant_pool_t permanent;
    ant_class_pool_t bigint;
    ant_string_pool_t string;
  } pool;

  struct {
    size_t closures;
    size_t upvalues;
  } alloc_bytes;
  
  size_t gc_last_live;
  size_t gc_pool_alloc;
  size_t gc_pool_last_live;

  ant_object_t *objects_old;
  size_t old_live_count;
  size_t minor_gc_count;

  ant_object_t **remember_set;
  size_t remember_set_len;
  size_t remember_set_cap;

  #ifdef ANT_JIT
  uint32_t jit_active_depth;
  #endif

  uint32_t vm_exec_depth;
  bool microtasks_draining;
  struct coroutine *active_async_coro;

  struct {
    ant_value_t *items;
    size_t len;
    size_t cap;
  } pending_rejections;

  struct {
    uintptr_t *cfunc_ptr;
    ant_value_t *promoted;
    uint8_t len;
    uint8_t cap;
  } cfunc_promote_cache;

  bool owns_mem;
  bool fatal_error;
  bool thrown_exists;
};

enum {
  STR_ASCII_UNKNOWN = 0,
  STR_ASCII_YES = 1,
  STR_ASCII_NO = 2,
};

typedef struct {
  ant_offset_t len;
  uint8_t is_ascii;
  char bytes[];
} ant_flat_string_t;

typedef struct {
  const char *ptr;
  size_t len;
  bool needs_free;
} js_cstr_t;

typedef struct {
  size_t count;
  size_t bytes;
} js_intern_stats_t;

typedef struct {
  bool has_getter;
  bool has_setter;
  bool writable;
  bool enumerable;
  bool configurable;
  ant_value_t getter;
  ant_value_t setter;
} prop_meta_t;

typedef enum {
  PROP_META_STRING = 0,
  PROP_META_SYMBOL = 1,
} prop_meta_key_t;

typedef ant_value_t 
  (*js_cfunc_fn_t)(ant_t *, ant_value_t *, int);

static inline js_cfunc_fn_t js_as_cfunc(ant_value_t fn_val) {
  return (js_cfunc_fn_t)(uintptr_t)vdata(fn_val);
}

static inline bool is_err(ant_value_t v) { 
  return vtype(v) == T_ERR; 
}

static inline bool is_null(ant_value_t v) { 
  return vtype(v) == T_NULL; 
}

static inline bool is_undefined(ant_value_t v) { 
  return vtype(v) == T_UNDEF; 
}

static inline bool is_empty_slot(ant_value_t v) { 
  return v == T_EMPTY; 
}

static inline bool is_callable(ant_value_t v) {
  uint8_t t = vtype(v);
  return t == T_FUNC || t == T_CFUNC;
}

bool is_internal_prop(const char *key, ant_offset_t klen);
size_t uint_to_str(char *buf, size_t bufsize, uint64_t val);
int extract_array_args(ant_t *js, ant_value_t arr, ant_value_t **out_args);

ant_value_t tov(double d);
double tod(ant_value_t v);
double js_to_number(ant_t *js, ant_value_t arg);

bool js_obj_ensure_prop_capacity(ant_object_t *obj, uint32_t needed);
bool js_obj_ensure_unique_shape(ant_object_t *obj);

ant_value_t js_propref_load(ant_t *js, ant_offset_t handle);
ant_value_t mkprop(ant_t *js, ant_value_t obj, ant_value_t k, ant_value_t v, uint8_t attrs);
ant_value_t mkprop_interned(ant_t *js, ant_value_t obj, const char *interned_key, ant_value_t v, uint8_t attrs);
ant_value_t setprop_cstr(ant_t *js, ant_value_t obj, const char *key, size_t len, ant_value_t v);
ant_value_t setprop_interned(ant_t *js, ant_value_t obj, const char *key, size_t len, ant_value_t v);
ant_value_t js_define_property(ant_t *js, ant_value_t obj, ant_value_t prop, ant_value_t descriptor, bool reflect_mode);

ant_value_t js_define_own_prop(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t v);
ant_value_t js_instance_proto_from_new_target(ant_t *js, ant_value_t fallback_proto);

ant_value_t js_get_module_import_binding(ant_t *js);
ant_value_t js_builtin_import(ant_t *js, ant_value_t *args, int nargs);
ant_value_t js_create_import_meta(ant_t *js, const char *filename, bool is_main);
ant_value_t js_create_module_context(ant_t *js, const char *filename, bool is_main);

ant_value_t coerce_to_str(ant_t *js, ant_value_t v);
ant_value_t coerce_to_str_concat(ant_t *js, ant_value_t v);
ant_value_t get_ctor_species_value(ant_t *js, ant_value_t ctor);

bool proto_chain_contains(ant_t *js, ant_value_t obj, ant_value_t proto_target);
bool same_ctor_identity(ant_t *js, ant_value_t a, ant_value_t b);

js_intern_stats_t js_intern_stats(void);
const char *intern_string(const char *str, size_t len);
js_cstr_t js_to_cstr(ant_t *js, ant_value_t value, char *stack_buf, size_t stack_size);

ant_value_t  lkp_interned_val(ant_t *js, ant_value_t obj, const char *search_intern);
ant_offset_t lkp_interned(ant_t *js, ant_value_t obj, const char *search_intern, size_t len);

ant_offset_t lkp(ant_t *js, ant_value_t obj, const char *buf, size_t len);
ant_offset_t lkp_proto(ant_t *js, ant_value_t obj, const char *buf, size_t len);

ant_offset_t lkp_sym(ant_t *js, ant_value_t obj, ant_offset_t sym_off);
ant_offset_t lkp_sym_proto(ant_t *js, ant_value_t obj, ant_offset_t sym_off);

ant_offset_t vstr(ant_t *js, ant_value_t value, ant_offset_t *len);
ant_offset_t vstrlen(ant_t *js, ant_value_t value);
ant_offset_t str_len_fast(ant_t *js, ant_value_t str);

ant_value_t mkval(uint8_t type, uint64_t data);
ant_value_t mkobj(ant_t *js, ant_offset_t parent);
ant_value_t js_mkobj_with_inobj_limit(ant_t *js, uint8_t inobj_limit);
ant_value_t rope_flatten(ant_t *js, ant_value_t rope);

ant_value_t js_for_in_keys(ant_t *js, ant_value_t obj);
ant_value_t js_delete_prop(ant_t *js, ant_value_t obj, const char *key, size_t len);
ant_value_t js_delete_sym_prop(ant_t *js, ant_value_t obj, ant_value_t sym);

bool is_proxy(ant_value_t obj);
bool strict_eq_values(ant_t *js, ant_value_t l, ant_value_t r);
bool js_deep_equal(ant_t *js, ant_value_t a, ant_value_t b, bool strict);

ant_value_t js_proxy_apply(ant_t *js, ant_value_t proxy, ant_value_t this_arg, ant_value_t *args, int argc);
ant_value_t js_proxy_construct(ant_t *js, ant_value_t proxy, ant_value_t *args, int argc, ant_value_t new_target);
ant_value_t sv_call_native(ant_t *js, ant_value_t func, ant_value_t this_val, ant_value_t *args, int nargs);

const char *typestr(uint8_t t);
ant_value_t unwrap_primitive(ant_t *js, ant_value_t val);
ant_value_t do_string_op(ant_t *js, uint8_t op, ant_value_t l, ant_value_t r);
ant_value_t js_to_primitive(ant_t *js, ant_value_t value, int hint);

ant_value_t do_instanceof(ant_t *js, ant_value_t l, ant_value_t r);
ant_value_t do_in(ant_t *js, ant_value_t l, ant_value_t r);

bool js_is_prototype_of(ant_t *js, ant_value_t proto_obj, ant_value_t obj);
ant_value_t builtin_object_isPrototypeOf(ant_t *js, ant_value_t *args, int nargs);
ant_value_t builtin_object_freeze(ant_t *js, ant_value_t *args, int nargs);

void js_module_eval_ctx_push(ant_t *js, ant_module_t *ctx);
void js_module_eval_ctx_pop(ant_t *js, ant_module_t *ctx);

bool lookup_prop_meta(
  ant_t *js, ant_value_t cur_obj,
  prop_meta_key_t key_kind, 
  const char *key, size_t klen,
  ant_offset_t sym_off, prop_meta_t *out
);

static inline ant_value_t js_module_eval_active_ns(ant_t *js) {
  ant_module_t *ctx = js->module;
  return ctx ? ctx->module_ns : js_mkundef();
}

static inline ant_value_t js_module_eval_active_ctx(ant_t *js) {
  ant_module_t *ctx = js->module;
  return ctx ? ctx->module_ctx : js_mkundef();
}

static inline ant_value_t js_module_eval_active_import_meta(ant_t *js) {
  ant_value_t module_ctx = js_module_eval_active_ctx(js);
  return is_object_type(module_ctx) ? js_get(js, module_ctx, "meta") : js_mkundef();
}

static inline const char *js_module_eval_active_filename(ant_t *js) {
  ant_value_t module_ctx = js_module_eval_active_ctx(js);
  if (is_object_type(module_ctx)) {
    ant_value_t filename = js_get(js, module_ctx, "filename");
    if (vtype(filename) == T_STR) return js_getstr(js, filename, NULL);
  }
  return js->filename;
}

static inline ant_module_format_t js_module_eval_active_format(ant_t *js) {
  ant_module_t *ctx = js->module;
  return ctx ? ctx->format : MODULE_EVAL_FORMAT_UNKNOWN;
}

static inline bool is_length_key(const char *key, size_t len) {
  return len == 6 && !memcmp(key, "length", 6);
}

static inline bool str_is_heap_rope(ant_value_t value) {
  return vtype(value) == T_STR && ((vdata(value) & 1ULL) != 0);
}

static inline int js_brand_id(ant_value_t obj) {
  if (!is_object_type(obj)) return BRAND_NONE;
  ant_value_t brand = js_get_slot(obj, SLOT_BRAND);
  return vtype(brand) == T_NUM ? (int)js_getnum(brand) : BRAND_NONE;
}

static inline bool js_check_brand(ant_value_t obj, int brand) {
  return js_brand_id(obj) == brand;
}

static inline bool lookup_symbol_prop_meta(ant_value_t cur_obj, ant_offset_t sym_off, prop_meta_t *out) {
  return lookup_prop_meta(NULL, cur_obj, PROP_META_SYMBOL, NULL, 0, sym_off, out);
}

static inline bool lookup_string_prop_meta(ant_t *js, ant_value_t cur_obj, const char *key, size_t klen, prop_meta_t *out) {
  return lookup_prop_meta(js, cur_obj, PROP_META_STRING, key, klen, 0, out);
}

static inline ant_value_t defmethod(ant_t *js, ant_value_t obj, const char *name, size_t len, ant_value_t fn) {
  const char *interned = intern_string(name, len);
  if (!interned) return js_mkerr(js, "oom");
  
  return mkprop_interned(
    js, obj, interned, fn,
    ANT_PROP_ATTR_WRITABLE | ANT_PROP_ATTR_CONFIGURABLE
  );
}

static inline ant_flat_string_t *str_flat_from_bytes(const char *str) {
  return (ant_flat_string_t *)((char *)str - offsetof(ant_flat_string_t, bytes));
}

static inline ant_flat_string_t *large_string_flat_ptr(ant_large_string_alloc_t *alloc) {
  return alloc ? (ant_flat_string_t *)&alloc->len : NULL;
}

static inline ant_large_string_alloc_t *large_string_alloc_from_flat(ant_flat_string_t *flat) {
  return flat ? (ant_large_string_alloc_t *)((char *)flat - offsetof(ant_large_string_alloc_t, len)) : NULL;
}

static inline uint8_t str_detect_ascii_bytes(const char *str, size_t len) {
  const unsigned char *s = (const unsigned char *)str;
  for (size_t i = 0; i < len; i++) {
    if (s[i] >= 0x80) return STR_ASCII_NO;
  }
  return STR_ASCII_YES;
}

static inline void str_set_ascii_state(const char *str, uint8_t state) {
  ant_flat_string_t *flat = str_flat_from_bytes(str);
  flat->is_ascii = state;
}

static inline bool str_is_ascii(const char *str) {
  ant_flat_string_t *flat = str_flat_from_bytes(str);
  if (flat->is_ascii == STR_ASCII_UNKNOWN) {
    flat->is_ascii = str_detect_ascii_bytes(flat->bytes, (size_t)flat->len);
  }
  return flat->is_ascii == STR_ASCII_YES;
}

static inline void js_set_module_default(ant_t *js, ant_value_t lib, ant_value_t ctor_fn, const char *name) {
  js_set(js, ctor_fn, name, ctor_fn);
  js_set(js, lib, name, ctor_fn);
  js_set(js, lib, "default", ctor_fn);
  js_set(js, ctor_fn, "default", ctor_fn);
  js_set_slot_wb(js, lib, SLOT_DEFAULT, ctor_fn);
}

ant_value_t js_cfunc_promote(ant_t *js, ant_value_t cfunc);

static inline ant_value_t js_cfunc_lookup_promoted(ant_t *js, ant_value_t cfunc) {
  uintptr_t ptr = vdata(cfunc);
  for (uint8_t i = 0; i < js->cfunc_promote_cache.len; i++) {
    if (js->cfunc_promote_cache.cfunc_ptr[i] == ptr)
      return js->cfunc_promote_cache.promoted[i];
  }
  return cfunc;
}

static inline ant_value_t js_make_ctor(ant_t *js, ant_cfunc_t fn, ant_value_t proto, const char *name, size_t nlen) {
  ant_value_t obj = js_mkobj(js);
  js_set_slot(obj, SLOT_CFUNC, js_mkfun(fn));
  js_mkprop_fast(js, obj, "prototype", 9, proto);
  js_mkprop_fast(js, obj, "name", 4, js_mkstr(js, name, nlen));
  js_set_descriptor(js, obj, "name", 4, 0);

  ant_value_t fn_val = js_obj_to_func(obj);
  js_set(js, proto, "constructor", fn_val);
  js_set_descriptor(js, proto, "constructor", 11, JS_DESC_W | JS_DESC_C);

  return fn_val;
}

#endif
