#ifndef ANT_INTERNAL_H
#define ANT_INTERNAL_H

#include "ant.h"
#include "gc.h"

#include <assert.h>
#include <string.h>

typedef struct sv_vm sv_vm_t;
typedef struct sv_func sv_func_t;

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
  T_OBJ  = 0,
  T_PROP = 1,
  T_STR  = 2,

  // objects
  T_ARR = 3,
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

  T_SENTINEL = 31
};

typedef struct {
  const char *src;
  const char *filename;
  ant_offset_t src_len;
  ant_offset_t off;
  ant_offset_t span_len;
  bool valid;
} js_error_site_t;

struct ant {
  sv_vm_t *vm;
  
  #ifdef ANT_JIT
  void *jit_ctx;
  #endif

  const char *code;
  const char *filename;
  
  uint64_t sym_counter;
  js_error_site_t errsite;

  ant_value_t global;
  ant_value_t object;
  ant_value_t this_val;
  ant_value_t new_target;
  ant_value_t current_func;

  ant_value_t module_ns;
  ant_value_t import_meta;
  ant_value_t length_str;

  uint8_t *mem;
  ant_offset_t size;
  ant_offset_t brk;
  
  struct {
    void *base;
    void *main_base;
    void *main_lo;
    size_t limit;
  } cstk;

  ant_offset_t max_size;
  ant_value_t thrown_value;
  ant_value_t thrown_stack;
  ant_offset_t gc_alloc_since;

  ant_value_t *gc_roots;
  ant_handle_t gc_roots_len;
  ant_handle_t gc_roots_cap;

  bool owns_mem;
  bool needs_gc;
  
  bool fatal_error;
  bool thrown_exists;
};

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

static inline ant_offset_t loadoff(ant_t *js, ant_offset_t off) {
  assert(off + sizeof(ant_offset_t) <= js->brk); ant_offset_t val;
  memcpy(&val, &js->mem[off], sizeof(val)); return val;
}

static inline ant_value_t loadval(ant_t *js, ant_offset_t off) { 
  return *(ant_value_t *)(&js->mem[off]);
}

static inline bool is_arr_off(ant_t *js, ant_offset_t off) { 
  return (loadoff(js, off) & ARRMASK) != 0; 
}

bool is_internal_prop(const char *key, ant_offset_t klen);
size_t uint_to_str(char *buf, size_t bufsize, uint64_t val);

void js_gc_reserve_roots(GC_RESERVE_ARGS);
void js_gc_update_roots(GC_UPDATE_ARGS);
void js_gc_visit_frame_funcs(ant_t *js, void (*visitor)(void *, sv_func_t *), void *ctx);

ant_offset_t esize(ant_offset_t w);
ant_value_t tov(double d);

double tod(ant_value_t v);
double js_to_number(ant_t *js, ant_value_t arg);

ant_value_t resolveprop(ant_t *js, ant_value_t v);
ant_value_t mkprop(ant_t *js, ant_value_t obj, ant_value_t k, ant_value_t v, ant_offset_t flags);
ant_value_t setprop_cstr(ant_t *js, ant_value_t obj, const char *key, size_t len, ant_value_t v);
ant_value_t setprop_interned(ant_t *js, ant_value_t obj, const char *key, size_t len, ant_value_t v);
ant_value_t js_define_own_prop(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t v);

ant_value_t coerce_to_str(ant_t *js, ant_value_t v);
ant_value_t coerce_to_str_concat(ant_t *js, ant_value_t v);
ant_value_t get_ctor_species_value(ant_t *js, ant_value_t ctor);

bool is_rope(ant_t *js, ant_value_t value);
bool proto_chain_contains(ant_t *js, ant_value_t obj, ant_value_t proto_target);
bool same_ctor_identity(ant_t *js, ant_value_t a, ant_value_t b);

js_intern_stats_t js_intern_stats(void);
js_cstr_t js_to_cstr(ant_t *js, ant_value_t value, char *stack_buf, size_t stack_size);
ant_value_t js_instance_proto_from_new_target(ant_t *js, ant_value_t fallback_proto);

ant_offset_t lkp(ant_t *js, ant_value_t obj, const char *buf, size_t len);
ant_offset_t lkp_proto(ant_t *js, ant_value_t obj, const char *buf, size_t len);
ant_offset_t lkp_sym(ant_t *js, ant_value_t obj, ant_offset_t sym_off);
ant_offset_t lkp_sym_proto(ant_t *js, ant_value_t obj, ant_offset_t sym_off);
ant_offset_t vstr(ant_t *js, ant_value_t value, ant_offset_t *len);
ant_offset_t vstrlen(ant_t *js, ant_value_t value);
ant_value_t rope_flatten(ant_t *js, ant_value_t rope);
ant_offset_t str_len_fast(ant_t *js, ant_value_t str);

ant_value_t mkarr(ant_t *js);
ant_value_t mkval(uint8_t type, uint64_t data);
ant_value_t mkobj(ant_t *js, ant_offset_t parent);

ant_value_t js_for_in_keys(ant_t *js, ant_value_t obj);
ant_value_t js_delete_prop(ant_t *js, ant_value_t obj, const char *key, size_t len);
ant_value_t js_delete_sym_prop(ant_t *js, ant_value_t obj, ant_value_t sym);

bool is_proxy(ant_t *js, ant_value_t obj);
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

ant_value_t bigint_add(ant_t *js, ant_value_t a, ant_value_t b);
ant_value_t bigint_sub(ant_t *js, ant_value_t a, ant_value_t b);
ant_value_t bigint_mul(ant_t *js, ant_value_t a, ant_value_t b);
ant_value_t bigint_div(ant_t *js, ant_value_t a, ant_value_t b);
ant_value_t bigint_mod(ant_t *js, ant_value_t a, ant_value_t b);
ant_value_t bigint_neg(ant_t *js, ant_value_t a);
ant_value_t bigint_exp(ant_t *js, ant_value_t base, ant_value_t exp);
int bigint_compare(ant_t *js, ant_value_t a, ant_value_t b);

ant_value_t bigint_shift_left(ant_t *js, ant_value_t value, uint64_t shift);
ant_value_t bigint_shift_right(ant_t *js, ant_value_t value, uint64_t shift);
ant_value_t bigint_shift_right_logical(ant_t *js, ant_value_t value, uint64_t shift);
ant_value_t bigint_asint_bits(ant_t *js, ant_value_t arg, uint64_t *bits_out);

#endif
