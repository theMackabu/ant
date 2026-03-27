#ifndef ANT_INTERNAL_H
#define ANT_INTERNAL_H

#include "ant.h"
#include "object.h"
#include "pool.h"
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

#define JS_ERR_NO_STACK      (1 << 8)
#define JS_TYPE_FLAG(t)      (1u << (t))

#define MAX_STRINGIFY_DEPTH   64
#define MAX_PROTO_CHAIN_DEPTH 256
#define MAX_MULTIREF_OBJS     128
#define MAX_DENSE_INITIAL_CAP 8

#define PROTO_WALK_F_OBJECT_ONLY (1u << 0)
#define PROTO_WALK_F_LOOKUP      (1u << 1)

#define ROPE_MAX_DEPTH         64
#define ROPE_FLATTEN_THRESHOLD (32 * 1024)

#define T_EMPTY                (NANBOX_PREFIX | ((ant_value_t)T_SENTINEL << NANBOX_TYPE_SHIFT) | 0xDEADULL)
#define T_SPECIAL_OBJECT_MASK  (JS_TYPE_FLAG(T_OBJ)  | JS_TYPE_FLAG(T_ARR))
#define T_NEEDS_PROTO_FALLBACK (JS_TYPE_FLAG(T_FUNC) | JS_TYPE_FLAG(T_ARR) | JS_TYPE_FLAG(T_PROMISE))
#define T_OBJECT_MASK          (JS_TYPE_FLAG(T_OBJ)  | JS_TYPE_FLAG(T_ARR) | JS_TYPE_FLAG(T_FUNC) | JS_TYPE_FLAG(T_PROMISE))
#define T_NON_NUMERIC_MASK     (JS_TYPE_FLAG(T_STR)  | JS_TYPE_FLAG(T_ARR) | JS_TYPE_FLAG(T_FUNC) | JS_TYPE_FLAG(T_CFUNC) | JS_TYPE_FLAG(T_OBJ))

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

  const char *code;
  const char *filename;

  #ifdef ANT_JIT
  void *jit_ctx;
  #endif

  ant_value_t global;
  ant_value_t object;
  ant_value_t this_val;
  ant_value_t new_target;
  ant_value_t current_func;
  ant_value_t length_str;
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
    
    ant_value_t iterator_proto;
    ant_value_t array_iterator_proto;
    ant_value_t string_iterator_proto;
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
    ant_class_pool_t string;
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

  struct {
    ant_value_t *items;
    size_t len;
    size_t cap;
  } pending_rejections;

  bool owns_mem;
  bool fatal_error;
  bool thrown_exists;
};

typedef struct {
  ant_offset_t len;
  char bytes[];
} ant_flat_string_t;

typedef struct {
  ant_offset_t len;
  uint8_t depth;
  ant_value_t left;
  ant_value_t right;
  ant_value_t cached;
} ant_rope_heap_t;

typedef struct {
  const char *ptr;
  size_t len;
  bool needs_free;
} js_cstr_t;

typedef struct {
  size_t count;
  size_t bytes;
} js_intern_stats_t;

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
ant_value_t js_create_import_meta(ant_t *js, const char *filename, bool is_main);
ant_value_t js_instance_proto_from_new_target(ant_t *js, ant_value_t fallback_proto);

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

ant_value_t mkarr(ant_t *js);
ant_value_t mkval(uint8_t type, uint64_t data);
ant_value_t mkobj(ant_t *js, ant_offset_t parent);
ant_value_t js_mkobj_with_inobj_limit(ant_t *js, uint8_t inobj_limit);
ant_value_t rope_flatten(ant_t *js, ant_value_t rope);

ant_value_t js_for_in_keys(ant_t *js, ant_value_t obj);
ant_value_t js_delete_prop(ant_t *js, ant_value_t obj, const char *key, size_t len);
ant_value_t js_delete_sym_prop(ant_t *js, ant_value_t obj, ant_value_t sym);

bool is_proxy(ant_value_t obj);
bool strict_eq_values(ant_t *js, ant_value_t l, ant_value_t r);

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

void js_module_eval_ctx_push(ant_t *js, ant_module_t *ctx);
void js_module_eval_ctx_pop(ant_t *js, ant_module_t *ctx);

static inline ant_value_t js_module_eval_active_ns(ant_t *js) {
  ant_module_t *ctx = js->module;
  return ctx ? ctx->module_ns : js_mkundef();
}

static inline ant_value_t js_module_eval_active_import_meta(ant_t *js) {
  ant_module_t *ctx = js->module;
  return ctx ? ctx->import_meta : js_mkundef();
}

static inline const char *js_module_eval_active_filename(ant_t *js) {
  ant_module_t *ctx = js->module;
  return ctx ? ctx->filename : js->filename;
}

static inline const char *js_module_eval_active_parent_path(ant_t *js) {
  ant_module_t *ctx = js->module;
  return ctx ? ctx->parent_path : NULL;
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
