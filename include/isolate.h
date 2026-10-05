#ifndef ANT_ISOLATE_H
#define ANT_ISOLATE_H

#include "arena.h"
#include "errors.h"
#include "pool.h"
#include "gc/objects.h"
#include "silver/code_unit.h"
#include "runtime.h"
#include "primordials.h"
#include "descriptors.h"

#include "esm/loader.h"
#include "modules/json.h"
#include "modules/timer.h"

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

typedef struct {
  const char *name;
  uint32_t len;
  bool is_const;
  ant_value_t value;
} ant_global_lexical_t;

static constexpr int MAX_STRINGIFY_DEPTH   = 64;
static constexpr int MAX_PROTO_CHAIN_DEPTH = 256;
static constexpr int MAX_MULTIREF_OBJS     = 128;
static constexpr int MAX_DENSE_INITIAL_CAP = 8;

struct ant_isolate_t {
  sv_vm_t *vm;
  void *jit_ctx;
  gc_vm_seg_t *vm_segs;

  ant_object_t *objects;
  ant_object_t *permanent_objects;
  
  descriptor_entry_t *desc_registry;
  ant_process_state_t *process_state;
  ant_events_state_t *events_state;
  ant_regex_state_t *regex_state;
  json_layout_cache_t *json_layout_cache;
  server_runtime_t *server_runtimes;

  ant_fixed_arena_t obj_arena;
  ant_fixed_arena_t closure_arena;
  ant_fixed_arena_t upvalue_arena;
  uint32_t prototype_write_epoch;

  bool promise_constructor_protector_invalid;
  bool promise_resolve_lookup_protector_invalid;
  bool promise_species_protector_invalid;
  bool promise_then_protector_invalid;

  struct {
    uint32_t epoch;
    uint32_t obj_epoch;
    uint32_t next_object_identity;
    ant_shape_t ***shape_ref_slots;
    size_t shape_ref_len;
    size_t shape_ref_cap;
    sv_gf_mega_cache_t *gf_mega;
    sv_gf_poly_t *gf_polys;
    sv_pf_poly_t *pf_polys;
  } ic;

  ant_value_t **c_roots;
  size_t c_root_count;
  size_t c_root_cap;

  const char *code;
  const char *filename;

  ant_value_t Ant;
  ant_value_t global;
  ant_value_t this_val;
  ant_value_t current_func;
  ant_value_t length_str;
  ant_value_t ascii_chars[128];

  // TODO: struct
  ant_global_lexical_t *global_lexicals;
  uint32_t global_lexical_count;
  uint32_t global_lexical_cap;
  uint32_t *global_lexical_index;
  uint32_t global_lexical_index_cap;

  struct {
    ant_value_t hooks;
    ant_value_t import_meta;
    struct {
      ant_value_t cache;
      ant_value_t parent;
      ant_value_t main;
      ant_value_t constructor;
      uint64_t generation;
    } cjs;
    ant_esm_state_t *state;
    ant_module_t *module_stack;
  } modules;

  struct {
    const char *length;
    const char *buffer;
    const char *prototype;
    const char *exec;
    const char *replace;
    const char *constructor;
    const char *promise;
    const char *resolve;
    const char *then;
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

  ant_value_t exception;
  ant_value_t exception_oom;

  struct {
    ant_value_t stack[MAX_STRINGIFY_DEPTH];
    int depth;
    int indent;

    ant_value_t multiref_objs[MAX_MULTIREF_OBJS];
    int multiref_ids[MAX_MULTIREF_OBJS];
    int multiref_count;
    int multiref_next_id;
  } stringify;

  struct {
    void *base;
    void *main_base;
    void *main_lo;
    size_t limit;
    void *floor;
  } cstk;

  struct {
    struct sym_registry_entry *registry;

    ant_value_t object_proto;
    ant_value_t array_proto;
    ant_value_t function_proto;
    ant_value_t string_proto;
    ant_value_t number_proto;
    ant_value_t boolean_proto;
    ant_value_t promise_ctor;
    ant_value_t promise_proto;
    ant_value_t promise_resolve;
    ant_value_t promise_then;
    ant_value_t bigint_proto;
    ant_value_t symbol_proto;
    ant_value_t array_values_fn;
    ant_value_t iterator_proto;
    ant_value_t array_iterator_proto;
    ant_value_t string_iterator_proto;
    ant_value_t generator_proto;
    ant_value_t async_generator_proto;
    ant_value_t async_iterator_proto;

    #define ANT_SYMBOL(name, _desc) ant_value_t name##_sym;
    #include "symbol_list.h"
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

  struct {
    struct ant_iterator_entry *entries;
    size_t len;
    size_t cap;
  } iterators;

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
  
  struct {
    size_t last_live;
    size_t pool_alloc;
    size_t code_alloc;
    size_t closure_alloc;
    size_t closure_at_minor;
    size_t closure_wm_at_major;
    size_t closure_wm_minor_tried;
    size_t pool_last_live;
    uint64_t alloc_since_major;
    size_t arrays_at_collect;
    size_t alloc_limit;
    size_t array_limit;
    size_t array_major_limit;
    size_t idle_retry_at;

    size_t tick;
    size_t nursery_threshold;
    uint32_t major_every_n;
    uint32_t major_live_growth_x256;
    uint32_t major_pool_growth_x256;
    uint32_t minor_surv_ewma;
    uint32_t major_recl_ewma;
    uint32_t major_time_share_ewma;
    uint32_t major_work_share_ewma;
    uint64_t last_major_end_ns;
    uint64_t minor_cost_ns;
    uint64_t major_cost_ns;
  
    uint64_t epoch;
    uint8_t obj_epoch;
    bool minor;
    ant_object_t **mark_stack;
    size_t mark_sp;
    size_t mark_cap;
  
    gc_func_mark_profile_t func_profile;
    sv_func_t **func_stack;
    size_t func_sp;
    size_t func_cap;
    bool func_draining;
  
    sv_func_t **fb_funcs;
    size_t fb_len;
    size_t fb_cap;
  
    struct gc_string_block_mark *string_marks;
    size_t string_mark_len, string_mark_cap;
  
    struct gc_large_string_mark *large_string_marks;
    size_t large_string_mark_len, large_string_mark_cap;
  
    struct gc_bigint_block *bigint_blocks;
    size_t bigint_block_len, bigint_block_cap;
  } gc;

  ant_object_t *objects_old;
  ant_object_t *pending_promises;

  size_t old_live_count;
  size_t minor_gc_count;

  ant_object_t **remember_set;
  size_t remember_set_len;
  size_t remember_set_cap;

  ant_object_t **permanent_roots;
  size_t permanent_root_len;
  size_t permanent_root_cap;
  size_t permanent_root_traced;

  sv_code_units_t code_units;

  struct {
    ant_code_arena_t code;
    ant_code_arena_t parse;
    struct code_intern *interns;
  } arenas;

  size_t remembered_upvalue_len;
  size_t remembered_upvalue_cap;
  struct sv_upvalue **remembered_upvalues;

  size_t remembered_closure_len;
  size_t remembered_closure_cap;
  struct sv_closure **remembered_closures;
  
  size_t remembered_coroutine_len;
  size_t remembered_coroutine_cap;
  struct coroutine **remembered_coroutines;

  struct sv_closure **young_closures;
  size_t young_closure_len;
  size_t young_closure_cap;

  struct sv_upvalue **young_upvalues;
  size_t young_upvalue_len;
  size_t young_upvalue_cap;

  size_t young_closure_trigger;
  size_t gc_closure_promoted_since_major;

  bool gc_running;
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

#ifdef ANT_WASM_EMBED
  uint32_t wasm_interrupt_ticks;
  bool wasm_interrupt_enabled;
#endif

  ant_timer_state_t timers;
  bool microtasks_draining;
  
  struct coroutine *active_async_coro;
  struct gc_temp_root_scope *temp_roots;

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
  bool uncaught_nonfatal;

  struct {
    ant_pool_t young;
    ant_pool_t old;

    size_t young_alloc;
    struct gc_rope_mark *marks;
    struct gc_rope_mark *last_mark;

    size_t mark_count;
    size_t mark_cap;

    uint32_t mark_epoch;
    bool minor_marking;
    bool conservative_marking;

    ant_string_builder_t **remembered_builders;
    size_t remembered_builder_len;
    size_t remembered_builder_cap;
  } rope_gc;
  
  ant_value_t primordials;
  ant_value_t primordial_values[ANT_PRIMORDIAL_COUNT];
};

#endif
