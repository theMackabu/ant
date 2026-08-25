#ifndef ANT_INTERNAL_H
#define ANT_INTERNAL_H

#include "ant.h"
#include "value.h"
#include "object.h"
#include "pool.h"
#include "sugar.h"
#include "errors.h"
#include "arena.h"
#include "silver/ast.h"
#include "descriptors.h"
#include "esm/loader.h"

#include <assert.h>
#include <string.h>

static constexpr uint32_t ANT_RUNTIME_WEB = 1u << 0;
static constexpr uint32_t PROTO_WALK_F_OBJECT_ONLY = 1u << 0;
static constexpr uint32_t PROTO_WALK_F_LOOKUP      = 1u << 1;

static constexpr int JS_ERR_NO_STACK          = 1 << 8;
static constexpr int MAX_STRINGIFY_DEPTH      = 64;
static constexpr int MAX_PROTO_CHAIN_DEPTH    = 256;
static constexpr int MAX_MULTIREF_OBJS        = 128;
static constexpr int MAX_DENSE_INITIAL_CAP    = 8;
static constexpr int STR_SHORT_CONS_THRESHOLD = 13;

static inline bool ant_value_stack_push_with_spill(
  ant_value_t **stack, size_t *sp, size_t *cap,
  ant_value_t *local, ant_value_t value
) {
  if (*sp == *cap) {
    size_t next_cap = *cap * 2u;
    
    ant_value_t *next = *stack == local
      ? (ant_value_t *)malloc(next_cap * sizeof(*next))
      : (ant_value_t *)realloc(*stack, next_cap * sizeof(*next));
      
    if (!next) return false;
    if (*stack == local) memcpy(next, local, *sp * sizeof(*next));
    
    *stack = next;
    *cap = next_cap;
  }
  
  (*stack)[(*sp)++] = value;
  return true;
}

enum: uint64_t {
  STR_META_ASCII_SHIFT = 56,
  STR_META_VALID_SHIFT = 58,
  STR_BUILDER_TAIL_CAP = 256,
};

enum {
  STR_UTF_UNKNOWN = 0,
  STR_UTF_VALID = 1,
  STR_UTF_INVALID = 2,
  STR_UTF_INVALID_SAME_LENGTH = 3,
};

enum: uint64_t {
  STR_HEAP_TAG_MASK    = 0x3,
  STR_HEAP_TAG_FLAT    = 0x0,
  STR_HEAP_TAG_ROPE    = 0x1,
  STR_HEAP_TAG_BUILDER = 0x2,
};

static constexpr uint64_t STR_META_FIELD_MASK   = 0x3;
static constexpr uint64_t STR_META_UTF16_MASK   = (UINT64_C(1) << STR_META_ASCII_SHIFT) - 1;
static constexpr uint64_t STR_META_ASCII_MASK   = STR_META_FIELD_MASK << STR_META_ASCII_SHIFT;
static constexpr uint64_t STR_META_VALID_MASK   = STR_META_FIELD_MASK << STR_META_VALID_SHIFT;
static constexpr uint64_t STR_META_STATE_MASK   = ~STR_META_UTF16_MASK;
static constexpr uint64_t STR_UTF16_LEN_UNKNOWN = STR_META_UTF16_MASK;

#define T_FLAG_FIND(t) (1u << (t))
#define T_MASK_HOLD ()
#define T_MASK_EXPAND(...) T_MASK_E1(T_MASK_E1(T_MASK_E1(T_MASK_E1(__VA_ARGS__))))
#define T_MASK_E1(...)     T_MASK_E2(T_MASK_E2(T_MASK_E2(T_MASK_E2(__VA_ARGS__))))
#define T_MASK_E2(...)     __VA_ARGS__
#define T_MASK_STEP(a, ...) \
  T_FLAG_FIND(a) __VA_OPT__(| T_MASK_AGAIN T_MASK_HOLD (__VA_ARGS__))
#define T_MASK_AGAIN() T_MASK_STEP
#define T_MASK(...) (T_MASK_EXPAND(T_MASK_STEP(__VA_ARGS__)))

static_assert(T_MASK(kTypeObject) == T_FLAG_FIND(kTypeObject));

#define is_non_numeric(v)    ((1u << vtype(v)) & T_NON_NUMERIC_MASK)
#define is_object_type(v)    ((1u << vtype(v)) & T_OBJECT_MASK)
#define is_special_object(v) ((1u << vtype(v)) & T_SPECIAL_OBJECT_MASK)

static constexpr uint32_t T_SPECIAL_OBJECT_MASK = 
  T_MASK(kTypeObject, kTypeArray);
  
static constexpr uint32_t T_BOXABLE_PRIMITIVE_MASK = 
  T_MASK(kTypeString, kTypeNumber, kTypeBool, kTypeBigInt, kTypeSymbol);
  
static constexpr uint32_t T_NEEDS_PROTO_FALLBACK = 
  T_MASK(kTypeFunction, kTypeArray, kTypePromise, kTypeGenerator);
  
static constexpr uint32_t T_OBJECT_MASK = 
  T_MASK(kTypeObject, kTypeArray, kTypeFunction, kTypePromise, kTypeGenerator);
  
static constexpr uint32_t T_NON_NUMERIC_MASK = 
  T_MASK(kTypeString, kTypeArray, kTypeFunction, kTypeBuiltin, kTypeObject, kTypeGenerator);

static_assert(
  T_NON_NUMERIC_MASK == (
    T_FLAG_FIND(kTypeString)   |
    T_FLAG_FIND(kTypeArray)    |
    T_FLAG_FIND(kTypeFunction) |
    T_FLAG_FIND(kTypeBuiltin)  |
    T_FLAG_FIND(kTypeObject)   |
    T_FLAG_FIND(kTypeGenerator)),
  "T_MASK variadic expansion"
);

enum: ant_value_t {
  T_EMPTY             = ANT_SENTINEL_TAG | 0xDEADULL,
  SV_JIT_BAILOUT      = ANT_SENTINEL_TAG | 0xBA110ULL,
  SV_AITER_ARRAY_TAG  = ANT_SENTINEL_TAG | 0xFA1ULL,
  SV_AITER_AWAIT_MARK = ANT_SENTINEL_TAG | 0xFA2ULL,
};

typedef struct {
  const char *src;
  const char *filename;
  ant_offset_t src_len;
  ant_offset_t off;
  ant_offset_t span_len;
  bool valid;
} js_error_site_t;

typedef struct {
  ant_object_t *base;
  ant_object_t *proto;
  ant_shape_t *base_shape;
  ant_shape_t *proto_shape;
  uint32_t object_epoch;
} ant_with_unscopables_cache_t;

typedef struct {
  ant_object_t *object;
  ant_shape_t *shape;
  ant_value_t proto;
  uint32_t own_valueof_data_slot;
  uint32_t ic_epoch;
} ant_to_primitive_cache_t;

struct ant_isolate_t {
  sv_vm_t *vm;
  void *jit_ctx;

  ant_object_t *objects;
  ant_object_t *permanent_objects;
  ant_process_state_t *process_state;
  ant_events_state_t *events_state;
  ant_regex_state_t *regex_state;

  ant_fixed_arena_t obj_arena;
  ant_fixed_arena_t closure_arena;
  ant_fixed_arena_t upvalue_arena;

  uint32_t next_ic_object_identity;
  uint32_t prototype_write_epoch;

  ant_shape_t ***ic_shape_ref_slots;
  size_t ic_shape_ref_len;
  size_t ic_shape_ref_cap;
  
  ant_value_t **c_roots;
  size_t c_root_count;
  size_t c_root_cap;
  
  struct gc_temp_root_scope *temp_roots;
  struct coroutine *retired_coroutines;

  const char *code;
  const char *filename;
  
  ant_value_t Ant;
  ant_value_t global;
  ant_value_t this_val;
  ant_value_t new_target;
  ant_value_t current_func;
  ant_value_t length_str;
  
  struct {
    ant_value_t hooks;
    ant_value_t import_meta;
    ant_esm_state_t *state;
    ant_module_t *module_stack;
  } esm;
  
  struct {
    const char *length;
    const char *buffer;
    const char *prototype;
    const char *exec;
    const char *replace;
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
    void *floor;
  } cstk;

  // TODO: rename to uppercase
  struct {
    struct sym_registry_entry *registry;

    ant_value_t object_proto;
    ant_value_t array_proto;
    ant_value_t function_proto;
    ant_value_t string_proto;
    ant_value_t number_proto;
    ant_value_t boolean_proto;
    ant_value_t promise_proto;
    ant_value_t bigint_proto;
    ant_value_t symbol_proto;
    ant_value_t array_values_fn;
    ant_value_t iterator_proto;
    ant_value_t array_iterator_proto;
    ant_value_t string_iterator_proto;
    ant_value_t generator_proto;
    ant_value_t async_generator_proto;
    ant_value_t async_iterator_proto;
  } sym;

  struct {
    #define ANT_BUILTIN(name) ant_value_t name;
    #define ANT_BUILTIN_ARR(name, n) ant_value_t name[n];
    #include "isolate_values.h"
  } builtins;

  struct {
    #define ANT_MUTABLE_ROOT(name) ant_value_t name;
    #define ANT_MUTABLE_ROOT_ARR(name, n) ant_value_t name[n];
    #include "isolate_values.h"
  } mutable_roots;
  
  ant_offset_t max_size;
  js_error_site_t errsite;
  double perf_time_origin_ms;

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
    size_t arrays;
  } alloc_bytes;
  
  size_t gc_last_live;
  size_t gc_pool_alloc;
  size_t gc_closure_alloc;
  size_t gc_closure_at_minor;
  size_t gc_closure_wm_at_major;
  size_t gc_closure_wm_minor_tried;
  size_t gc_pool_last_live;

  ant_object_t *objects_old;
  ant_object_t *pending_promises;
  
  size_t old_live_count;
  size_t minor_gc_count;

  ant_object_t **remember_set;
  size_t remember_set_len;
  size_t remember_set_cap;

  struct {
    sv_func_t *func;
    uint32_t slot;
  } *remembered_func_consts;

  size_t remembered_func_const_len;
  size_t remembered_func_const_cap;

  size_t remembered_upvalue_len;
  size_t remembered_upvalue_cap;
  struct sv_upvalue **remembered_upvalues;

  size_t remembered_closure_len;
  size_t remembered_closure_cap;
  struct sv_closure **remembered_closures;

  struct sv_closure **young_closures;
  size_t young_closure_len;
  size_t young_closure_cap;
  
  struct sv_upvalue **young_upvalues;
  size_t young_upvalue_len;
  size_t young_upvalue_cap;
  
  size_t young_closure_trigger;
  size_t gc_closure_promoted_since_major;

  bool gc_remember_overflow;
  bool gc_objects_running;
  bool gc_use_nursery_major_floor;

  struct {
    ant_object_t **collections;
    size_t collection_len;
    size_t collection_cap;
    
    ant_value_t *kept_alive;
    size_t kept_alive_len;
    size_t kept_alive_cap;
    
    struct {
      ant_object_t *owner;
      ant_value_t key;
      ant_value_t value;
      uint8_t kind;
    } *minor_edges;
    
    size_t minor_edge_len;
    size_t minor_edge_cap;
    
    void *pending;
    void (*mark)(ant_t *js, ant_value_t value);
    bool (*key_alive)(ant_t *js, ant_value_t key);
    
    bool registry_overflow;
    bool minor_edge_overflow;
    bool pending_active;
    bool pending_oom;
    bool kept_alive_overflow;
  } weak_gc;

  uint32_t jit_active_depth;
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

  struct {
    const ant_cfunc_meta_t **base_meta;
    const char **name_ptr;
    ant_value_t *named;
    uint16_t len;
    uint16_t cap;
  } cfunc_name_cache;

  struct {
    ant_with_unscopables_cache_t with_unscopables_absent;
    ant_to_primitive_cache_t to_primitive_absent;
  } runtime_cache;

  struct {
    char **argv;
    const char *ls_fp;
    int argc;
    int pid;
    unsigned int flags;
  } runtime;

  bool owns_mem;
  bool fatal_error;
  bool thrown_exists;

  struct {
    ant_pool_t young;
    ant_pool_t old;
    
    size_t young_alloc;
    struct gc_rope_mark *marks;
    
    size_t mark_count;
    size_t mark_cap;
    
    uint32_t mark_epoch;
    bool minor_marking;
    bool conservative_marking;
    
    ant_string_builder_t **remembered_builders;
    size_t remembered_builder_len;
    size_t remembered_builder_cap;
  } rope_gc;
};

static inline void ant_prototype_write_epoch_bump(ant_t *js) {
  if (++js->prototype_write_epoch == 0) {
    ant_ic_epoch_bump();
    js->prototype_write_epoch = 1;
  }
}

static inline void ant_prototype_property_write_invalidate(ant_t *js, ant_object_t *holder, const char *key) {
  if (!js || !holder || !key) return;
  if (key == js->intern.prototype) ant_prototype_write_epoch_bump(js);
}

enum {
  STR_ASCII_UNKNOWN = 0,
  STR_ASCII_YES = 1,
  STR_ASCII_NO = 2,
};

typedef struct {
  ant_offset_t len;
  uint64_t meta;
  char bytes[];
} ant_flat_string_t;

static_assert(
  sizeof(ant_flat_string_t) == 16,
  "flat string header must remain 16 bytes"
);

static_assert(
  offsetof(ant_flat_string_t, bytes) == sizeof(ant_flat_string_t),
  "flat string bytes must follow packed metadata"
);

static_assert(
  offsetof(ant_large_string_alloc_t, meta) - offsetof(ant_large_string_alloc_t, len) ==
    offsetof(ant_flat_string_t, meta),
  "large and pooled string metadata layouts must match"
);

static_assert(
  offsetof(ant_large_string_alloc_t, bytes) - offsetof(ant_large_string_alloc_t, len) ==
    offsetof(ant_flat_string_t, bytes),
  "large and pooled string byte layouts must match"
);

typedef struct ant_builder_chunk {
  struct ant_builder_chunk *next;
  ant_value_t value;
} ant_builder_chunk_t;

struct ant_string_builder {
  ant_offset_t len;
  ant_value_t snapshot;
  ant_builder_chunk_t *head;
  ant_builder_chunk_t *chunk_tail;
  ant_value_t cached;
  uint16_t tail_len;
  uint8_t ascii_state;
  uint8_t in_remember_set;
  char tail[STR_BUILDER_TAIL_CAP];
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

static inline bool is_err(ant_value_t v) { 
  return vtype(v) == kTypeError;
}

static inline bool is_null(ant_value_t v) { 
  return vtype(v) == kTypeNull;
}

static inline bool is_undefined(ant_value_t v) { 
  return vtype(v) == kTypeUndefined;
}

static inline bool is_empty_slot(ant_value_t v) {
  return v == T_EMPTY;
}

static inline void js_cstk_refresh_floor(ant_t *js) {
  uintptr_t base = (uintptr_t)js->cstk.base;
  js->cstk.floor = (js->cstk.base != NULL && js->cstk.limit != 0 && base > js->cstk.limit) 
    ? (void *)(base - js->cstk.limit) : NULL;
}

static inline bool is_callable(ant_value_t v) {
  uint8_t t = vtype(v);
  if (t == kTypeFunction || t == kTypeBuiltin) return true;
  if (t != kTypeObject) return false;
  ant_object_t *obj = js_obj_ptr(v);
  return obj && obj->flags.is_callable;
}

static inline const ant_cfunc_meta_t *js_as_cfunc_meta(ant_value_t fn_val) {
  return (const ant_cfunc_meta_t *)ant_cage_decode(vdata(fn_val));
}

static inline ant_cfunc_t js_as_cfunc(ant_value_t fn_val) {
  const ant_cfunc_meta_t *meta = js_as_cfunc_meta(fn_val);
  return meta ? meta->fn : NULL;
}

static inline uint32_t js_cfunc_length(ant_value_t fn_val) {
  const ant_cfunc_meta_t *meta = js_as_cfunc_meta(fn_val);
  return meta ? meta->length : 0;
}

static inline bool js_cfunc_same_entrypoint(ant_value_t fn_val, ant_cfunc_t fn) {
  const ant_cfunc_meta_t *meta = js_as_cfunc_meta(fn_val);
  return meta && meta->fn == fn;
}

size_t uint_to_str(char *buf, size_t bufsize, uint64_t val);
ant_value_t extract_array_args(ant_t *js, ant_value_t arr, ant_value_t **out_args, int *out_count);
ant_value_t js_proxy_has(ant_t *js, ant_value_t proxy, const char *key, size_t key_len);

double js_to_number(ant_t *js, ant_value_t arg);
double js_parse_int_value(ant_t *js, ant_value_t arg);
double js_parse_float_value(ant_t *js, ant_value_t arg);

bool js_obj_ensure_prop_capacity(ant_object_t *obj, uint32_t needed);
bool js_obj_ensure_unique_shape(ant_object_t *obj);

ant_value_t js_template_to_string(ant_t *js, ant_value_t v);
ant_value_t js_define_property(ant_t *js, ant_value_t obj, ant_value_t prop, ant_value_t descriptor, bool reflect_mode);

ant_value_t mkprop(ant_t *js, ant_value_t obj, ant_value_t k, ant_value_t v, uint8_t attrs);
ant_value_t mkprop_exact_attrs(ant_t *js, ant_value_t obj, ant_value_t k, ant_value_t v, uint8_t attrs);
ant_value_t mkprop_interned(ant_t *js, ant_value_t obj, const char *interned_key, ant_value_t v, uint8_t attrs);
ant_value_t mkprop_interned_exact(ant_t *js, ant_value_t obj, const char *interned_key, ant_value_t v, uint8_t attrs);
ant_value_t mkprop_append_fast(ant_t *js, ant_value_t obj, const char *key, size_t len, ant_value_t v);

ant_value_t setprop_cstr(ant_t *js, ant_value_t obj, const char *key, size_t len, ant_value_t v);
ant_value_t setprop_interned(ant_t *js, ant_value_t obj, const char *key, size_t len, ant_value_t v);

// TODO: move into builder.c
typedef struct {
  ant_t *js;
  char *buf;
  size_t len;
  size_t n;
  bool growable;
  bool inline_mode;
  bool first;
  bool closed;
  bool did_indent;
} js_inspect_builder_t;

void js_inspect_builder_init_fixed(js_inspect_builder_t *builder, ant_t *js, char *buf, size_t len, size_t initial_n);
bool js_inspect_builder_init_dynamic(js_inspect_builder_t *builder, ant_t *js, size_t initial_cap);
void js_inspect_builder_dispose(js_inspect_builder_t *builder);
bool js_inspect_tagged_header(js_inspect_builder_t *builder, const char *tag, size_t tag_len);
bool js_inspect_object_body(js_inspect_builder_t *builder, ant_value_t obj);
bool js_inspect_close(js_inspect_builder_t *builder);

__attribute__((format(printf, 2, 3)))
bool js_inspect_header(js_inspect_builder_t *builder, const char *fmt, ...);

__attribute__((format(printf, 3, 4)))
bool js_inspect_header_for(js_inspect_builder_t *builder, ant_value_t obj, const char *fmt, ...);

ant_value_t js_inspect_builder_result(js_inspect_builder_t *builder);
ant_value_t js_define_own_prop(ant_t *js, ant_value_t obj, const char *key, size_t klen, ant_value_t v);
ant_value_t js_instance_proto_from_new_target(ant_t *js, ant_value_t fallback_proto);
ant_value_t js_construct_native(ant_t *js, ant_cfunc_t ctor, ant_value_t *args, int nargs);

ant_value_t js_get_module_import_binding(ant_t *js);
ant_value_t js_builtin_import(ant_t *js, ant_value_t *args, int nargs);
ant_value_t js_create_import_meta(ant_t *js, const char *filename, bool is_main);
ant_value_t js_create_module_context(ant_t *js, const char *filename, bool is_main);
ant_value_t js_create_arguments_object(ant_t *js, sv_vm_t *vm, ant_value_t callee, sv_frame_t *frame, int argc, int mapped_count, bool is_strict);

void js_arguments_detach(ant_t *js, ant_value_t obj);
void js_arguments_sync_slot(ant_t *js, ant_value_t obj, uint32_t idx, ant_value_t value);
void js_arguments_rebind_frame(ant_t *js, ant_value_t obj, int frame_index);
void js_arguments_bind_direct(ant_t *js, ant_value_t obj, struct sv_frame *frame);

ant_value_t coerce_to_str(ant_t *js, ant_value_t v);
ant_value_t coerce_to_str_concat(ant_t *js, ant_value_t v);
ant_value_t get_ctor_species_value(ant_t *js, ant_value_t ctor);

bool proto_chain_contains(ant_t *js, ant_value_t obj, ant_value_t proto_target);
bool same_ctor_identity(ant_t *js, ant_value_t a, ant_value_t b);

js_intern_stats_t js_intern_stats(void);
const char *intern_string(const char *str, size_t len);
const char *intern_find(const char *str, size_t len);

js_cstr_t js_to_cstr(ant_t *js, ant_value_t value, char *stack_buf, size_t stack_size);
js_cstr_t js_inspect_cstr(ant_t *js, ant_value_t value, char *stack_buf, size_t stack_size);

ant_value_t lkp_interned_val(ant_t *js, ant_value_t obj, const char *search_intern);
ant_prop_loc_t lkp_interned(ant_value_t obj, const char *search_intern);

ant_prop_loc_t lkp(ant_t *js, ant_value_t obj, const char *buf, size_t len);
ant_prop_loc_t lkp_proto(ant_t *js, ant_value_t obj, const char *buf, size_t len);

ant_prop_loc_t lkp_sym(ant_value_t obj, ant_offset_t sym_off);
ant_prop_loc_t lkp_sym_proto(ant_t *js, ant_value_t obj, ant_offset_t sym_off);

ant_offset_t vstr(ant_t *js, ant_value_t value, ant_offset_t *len);
ant_offset_t vstrlen(ant_t *js, ant_value_t value);
ant_offset_t str_len_fast(ant_t *js, ant_value_t str);
ant_offset_t str_utf16_len(ant_t *js, ant_value_t str);

ant_value_t mkobj(ant_t *js, ant_offset_t parent);
ant_value_t js_mkobj_with_inobj_limit(ant_t *js, uint8_t inobj_limit);
ant_value_t rope_flatten(ant_t *js, ant_value_t rope);
ant_value_t str_materialize(ant_t *js, ant_value_t value);

ant_value_t js_for_in_keys(ant_t *js, ant_value_t obj);
ant_value_t js_own_property_keys(ant_t *js, ant_value_t obj, bool include_symbols, bool enumerable_only);
ant_value_t js_delete_prop(ant_t *js, ant_value_t obj, const char *key, size_t len);
ant_value_t js_delete_sym_prop(ant_t *js, ant_value_t obj, ant_value_t sym);

ant_value_t js_cfunc_promote(ant_t *js, ant_value_t cfunc);
ant_value_t js_cfunc_expose_named(ant_t *js, ant_value_t cfunc, const char *name, size_t name_len);
ant_value_t js_set_function_name(ant_t *js, ant_value_t fn, const char *name, size_t name_len);

ant_value_t js_set_function_name_prefixed(
  ant_t *js, ant_value_t fn,
  const char *prefix, size_t prefix_len,
  const char *name, size_t name_len
);

ant_value_t js_set_function_name_from_key(
  ant_t *js, ant_value_t fn,
  ant_value_t key,
  const char *prefix, size_t prefix_len
);

ant_value_t js_maybe_set_function_name_from_key(
  ant_t *js, ant_value_t fn,
  ant_value_t key,
  const char *prefix, size_t prefix_len
);

sv_func_t *js_compile_parsed_bytecode(
  ant_t *js, struct sv_ast *program,
  const char *buf, size_t len, int mode
);

bool is_proxy(ant_value_t obj);
bool is_array_value(ant_value_t value);
bool strict_eq_values(ant_t *js, ant_value_t l, ant_value_t r);
bool same_value_values(ant_t *js, ant_value_t l, ant_value_t r);
bool js_deep_equal(ant_t *js, ant_value_t a, ant_value_t b, bool strict);
bool utf8_validate_bytes(const char *str, size_t byte_len);

ant_value_t js_eval_bytecode_eval_in_env_with_strict(
  ant_t *js, const char *buf, size_t len,
  bool inherit_strict, ant_value_t this_val, ant_value_t eval_env
);

ant_value_t js_primitive_prototype(ant_t *js, uint8_t type);
ant_value_t js_normalize_sloppy_this(ant_t *js, ant_value_t value);
ant_value_t js_resolve_bound_target(ant_value_t value);
ant_value_t js_resolve_bound_target_known_bound(ant_value_t value);
ant_value_t js_execute_compiled_bytecode(ant_t *js, sv_func_t *func, js_async_entry_t **async_entry_out);
ant_value_t js_proxy_apply(ant_t *js, ant_value_t proxy, ant_value_t this_arg, ant_value_t *args, int argc);
ant_value_t js_proxy_construct(ant_t *js, ant_value_t proxy, ant_value_t *args, int argc, ant_value_t new_target);
ant_value_t sv_call_native(ant_t *js, ant_value_t func, ant_value_t this_val, ant_value_t *args, int nargs);

const char *typestr(ant_value_type_t t);
ant_value_t unwrap_primitive(ant_t *js, ant_value_t val);
ant_value_t do_string_op(ant_t *js, uint8_t op, ant_value_t l, ant_value_t r);
ant_value_t js_to_primitive(ant_t *js, ant_value_t value, int hint);
ant_value_t js_is_array_value_checked(ant_t *js, ant_value_t value, bool *out);

ant_value_t do_instanceof(ant_t *js, ant_value_t l, ant_value_t r);
ant_value_t do_in(ant_t *js, ant_value_t l, ant_value_t r);

bool js_is_prototype_of(ant_t *js, ant_value_t proto_obj, ant_value_t obj);
ant_value_t builtin_object_isPrototypeOf(ant_t *js, ant_value_t *args, int nargs);
ant_value_t builtin_object_freeze(ant_t *js, ant_value_t *args, int nargs);

bool js_is_array_includes_builtin(ant_value_t func);
ant_value_t js_array_includes_call(ant_t *js, ant_value_t this_val, ant_value_t *args, int nargs);
ant_value_t builtin_array_includes(ant_t *js, ant_value_t *args, int nargs);

void js_module_eval_ctx_push(ant_t *js, ant_module_t *ctx);
void js_module_eval_ctx_pop(ant_t *js, ant_module_t *ctx);

bool lookup_prop_meta(
  ant_t *js, ant_value_t cur_obj,
  prop_meta_key_t key_kind, 
  const char *key, size_t klen,
  ant_offset_t sym_off, prop_meta_t *out
);

size_t intern_length(const char *interned);
size_t utf8_export_length_slow(const char *str, size_t str_len);

size_t utf8_export_into(
  const char *str, size_t str_len, uint8_t *dst, 
  size_t dst_len, size_t *out_read_units
);

static inline bool is_boxable_primitive_type(uint8_t type) {
  return (T_FLAG_FIND(type) & T_BOXABLE_PRIMITIVE_MASK) != 0;
}

static inline ant_module_t *js_active_tla_module_ctx(ant_t *js) {
  if (!js) return NULL;
  for (coroutine_t *coro = js->active_async_coro; coro; coro = coro->active_parent)
    if (coro->module_eval_ctx) return coro->module_eval_ctx;
  return NULL;
}

static inline void js_module_ctx_link_namespace(ant_t *js, ant_value_t module_ctx, ant_value_t ns) {
  if (!is_object_type(module_ctx) || !is_object_type(ns)) return;
  js_set_slot_wb(js, ns, SLOT_MODULE_CTX, module_ctx);
  js_set_slot_wb(js, module_ctx, SLOT_DATA, ns);
}

static inline ant_value_t js_module_ctx_namespace(ant_value_t module_ctx) {
  if (!is_object_type(module_ctx)) return js_mkundef();
  ant_value_t ns = js_get_slot(module_ctx, SLOT_DATA);
  return is_object_type(ns) ? ns : js_mkundef();
}

static inline ant_value_t js_current_func_module_ns(ant_t *js) {
  if (!js || !is_object_type(js->current_func)) return js_mkundef();
  return js_module_ctx_namespace(js_get_slot(js->current_func, SLOT_MODULE_CTX));
}

static inline ant_value_t js_module_eval_active_ns(ant_t *js) {
  ant_module_t *ctx = js->esm.module_stack;
  if (ctx) return ctx->module_ns;
  ctx = js->active_async_coro ? js_active_tla_module_ctx(js) : NULL;
  if (ctx) return ctx->module_ns;
  return js_current_func_module_ns(js);
}

static inline ant_value_t js_module_eval_active_ctx(ant_t *js) {
  ant_module_t *ctx = js->esm.module_stack;
  if (ctx) return ctx->module_ctx;
  ctx = js->active_async_coro ? js_active_tla_module_ctx(js) : NULL;
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
    if (vtype(filename) == kTypeString) return js_getstr(js, filename, NULL);
  }
  return js->filename;
}

static inline ant_module_format_t js_module_eval_active_format(ant_t *js) {
  ant_module_t *ctx = js->esm.module_stack;
  if (ctx) return ctx->format;
  ctx = js->active_async_coro ? js_active_tla_module_ctx(js) : NULL;
  return ctx ? ctx->format : MODULE_EVAL_FORMAT_UNKNOWN;
}

static inline bool is_length_key(const char *key, size_t len) {
  return len == 6 && !memcmp(key, "length", 6);
}

// TODO: move strings helpers to strings.h
static inline bool str_is_heap_rope(ant_value_t value) {
  return vtype(value) == kTypeString && ((vdata(value) & STR_HEAP_TAG_MASK) == STR_HEAP_TAG_ROPE);
}

static inline bool str_is_heap_builder(ant_value_t value) {
  return vtype(value) == kTypeString && ((vdata(value) & STR_HEAP_TAG_MASK) == STR_HEAP_TAG_BUILDER);
}

static inline ant_rope_heap_t *ant_str_rope_ptr(ant_value_t value) {
  return (ant_rope_heap_t *)vptr_masked(value, STR_HEAP_TAG_MASK);
}

static inline ant_string_builder_t *ant_str_builder_ptr(ant_value_t value) {
  return (ant_string_builder_t *)vptr_masked(value, STR_HEAP_TAG_MASK);
}

static inline ant_value_t ant_mkrope_value(ant_rope_heap_t *rope) {
  return mkref_tagged(kTypeString, rope, STR_HEAP_TAG_ROPE);
}

static inline ant_value_t ant_mkbuilder_value(ant_string_builder_t *builder) {
  return mkref_tagged(kTypeString, builder, STR_HEAP_TAG_BUILDER);
}

static inline int js_brand_id(ant_value_t obj) {
  if (!is_object_type(obj)) return BRAND_NONE;
  ant_value_t brand = js_get_slot(obj, SLOT_BRAND);
  return vtype(brand) == kTypeNumber ? (int)js_getnum(brand) : BRAND_NONE;
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

static inline ant_value_t defalias(ant_t *js, ant_value_t obj, const char *name, size_t len, ant_value_t fn) {
  const char *interned = intern_string(name, len);
  if (!interned) return js_mkerr(js, "oom");

  return mkprop_interned_exact(
    js, obj, interned, fn,
    ANT_PROP_ATTR_WRITABLE | ANT_PROP_ATTR_CONFIGURABLE
  );
}

static inline void js_set_global_builtin(
  ant_t *js,
  const char *name,
  ant_value_t value
) {
  ant_value_t global = js->global;
  size_t name_len = strlen(name);
  js_set(js, global, name, value);
  js_set_descriptor(js, global, name, name_len, JS_DESC_W | JS_DESC_C);
}

static inline ant_flat_string_t *str_flat_from_bytes(const char *str) {
  return (ant_flat_string_t *)((char *)str - offsetof(ant_flat_string_t, bytes));
}

static inline ant_flat_string_t *ant_str_flat_ptr(ant_value_t value) {
  if (vtype(value) != kTypeString) return NULL;
  if ((vdata(value) & STR_HEAP_TAG_MASK) != STR_HEAP_TAG_FLAT) return NULL;
  return (ant_flat_string_t *)vptr(value);
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

static inline uint8_t str_flat_ascii_state(const ant_flat_string_t *flat) {
  if (!flat) return STR_ASCII_UNKNOWN;
  return (uint8_t)((flat->meta & STR_META_ASCII_MASK) >> STR_META_ASCII_SHIFT);
}

static inline uint8_t str_flat_utf_valid_state(const ant_flat_string_t *flat) {
  if (!flat) return STR_UTF_UNKNOWN;
  return (uint8_t)((flat->meta & STR_META_VALID_MASK) >> STR_META_VALID_SHIFT);
}

static inline void str_flat_set_utf_valid_state(ant_flat_string_t *flat, uint8_t state) {
  if (!flat) return;
  flat->meta = (flat->meta & ~STR_META_VALID_MASK) | (
    ((uint64_t)state << STR_META_VALID_SHIFT) & STR_META_VALID_MASK);
}

static inline ant_offset_t str_flat_cached_utf16_len(const ant_flat_string_t *flat) {
  return flat ? (ant_offset_t)(flat->meta & STR_META_UTF16_MASK) : STR_UTF16_LEN_UNKNOWN;
}

static inline void str_flat_init_meta(ant_flat_string_t *flat, uint8_t ascii_state) {
  if (!flat) return;
  flat->meta = ((uint64_t)ascii_state << STR_META_ASCII_SHIFT) | STR_UTF16_LEN_UNKNOWN;
}

static inline void str_flat_set_utf16_len(ant_flat_string_t *flat, ant_offset_t len) {
  if (!flat) return;
  flat->meta = (flat->meta & STR_META_STATE_MASK) | (len & STR_META_UTF16_MASK);
}

static inline void str_set_ascii_state(const char *str, uint8_t state) {
  ant_flat_string_t *flat = str_flat_from_bytes(str);
  str_flat_init_meta(flat, state);
}

static inline bool str_is_ascii(const char *str) {
  ant_flat_string_t *flat = str_flat_from_bytes(str);
  uint8_t state = str_flat_ascii_state(flat);
  if (state == STR_ASCII_UNKNOWN) {
    state = str_detect_ascii_bytes(flat->bytes, (size_t)flat->len);
    str_flat_init_meta(flat, state);
  }
  return state == STR_ASCII_YES;
}

static inline bool str_is_valid_utf8(const char *str) {
  if (str_is_ascii(str)) return true;

  ant_flat_string_t *flat = str_flat_from_bytes(str);
  uint8_t state = str_flat_utf_valid_state(flat);
  
  if (state == STR_UTF_UNKNOWN) {
    state = utf8_validate_bytes(flat->bytes, (size_t)flat->len) 
      ? STR_UTF_VALID : STR_UTF_INVALID;
    str_flat_set_utf_valid_state(flat, state);
  }
  
  return state == STR_UTF_VALID;
}

static inline size_t utf8_export_length(const char *str, size_t str_len) {
  if (str_len == 0) return 0;
  
  uint8_t state = str_flat_utf_valid_state(str_flat_from_bytes(str));
  if (state == STR_UTF_VALID || state == STR_UTF_INVALID_SAME_LENGTH) return str_len;  
  if (state == STR_UTF_UNKNOWN && str_is_ascii(str)) return str_len;
  
  return utf8_export_length_slow(str, str_len);
}

static inline void js_set_module_default(ant_t *js, ant_value_t lib, ant_value_t ctor_fn, const char *name) {
  js_set(js, ctor_fn, name, ctor_fn);
  js_set(js, lib, name, ctor_fn);
  js_set(js, lib, "default", ctor_fn);
  js_set(js, ctor_fn, "default", ctor_fn);
  js_set_slot_wb(js, lib, SLOT_DEFAULT, ctor_fn);
}

static inline ant_value_t js_cfunc_lookup_promoted(ant_t *js, ant_value_t cfunc) {
  uintptr_t ptr = vdata(cfunc);
  for (uint8_t i = 0; i < js->cfunc_promote_cache.len; i++) if (
    js->cfunc_promote_cache.cfunc_ptr[i] == ptr
  ) return js->cfunc_promote_cache.promoted[i];
  return cfunc;
}

static inline ant_value_t js_make_ctor(ant_t *js, ant_cfunc_t fn, ant_value_t proto, const char *name, size_t nlen) {
  ant_value_t obj = js_mkobj(js);
  js_set_slot(obj, SLOT_CFUNC, js_mkfun_dyn(fn));
  js_mkprop_fast(js, obj, "prototype", 9, proto);
  js_mkprop_fast(js, obj, "name", 4, js_mkstr(js, name, nlen));
  js_set_descriptor(js, obj, "name", 4, 0);

  ant_value_t fn_val = js_obj_to_func(js, obj);
  js_set(js, proto, "constructor", fn_val);
  js_set_descriptor(js, proto, "constructor", 11, JS_DESC_W | JS_DESC_C);

  return fn_val;
}

#endif
