#ifndef SILVER_ENGINE_H
#define SILVER_ENGINE_H

#include "silver/vm.h"
#include "internal.h"
#include "gc.h"
#include "runtime.h"
#include "errors.h"
#include "debug.h"
#include "gc/objects.h"

#include <stdbool.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static constexpr int SV_JIT_ARGS_BUF_CAP = 16;

static constexpr uint8_t SV_MAP_TEMPLATE_MAX_SUBSTITUTIONS = 3;
static constexpr uint32_t SV_MAP_TEMPLATE_TABLE_MAGIC = UINT32_C(0x4d54504c);

static constexpr uint8_t SV_CLASS_FLAG_HAS_NAME     = 1u << 0;
static constexpr uint8_t SV_CLASS_FLAG_HAS_HERITAGE = 1u << 1;

typedef enum {
  SV_DEFINE_METHOD_GETTER   = 1u << 0,
  SV_DEFINE_METHOD_SETTER   = 1u << 1,
  SV_DEFINE_METHOD_SET_NAME = 1u << 2,
  SV_DEFINE_METHOD_NON_ENUM = 1u << 3,
} sv_define_method_flags_t;

typedef enum {
  SV_OPF_JIT_ELIGIBLE               = 1u << 0,
  SV_OPF_JIT_INLINEABLE             = 1u << 1,
  SV_OPF_JIT_NEEDS_BAILOUT          = 1u << 2,
  SV_OPF_JIT_NEEDS_INC_LOCAL        = 1u << 3,
  SV_OPF_JIT_NEEDS_ARGS_BUF         = 1u << 4,
  SV_OPF_JIT_NEEDS_TCO_ARGS         = 1u << 5,
  SV_OPF_JIT_NEEDS_ITER_ROOTS       = 1u << 6,
  SV_OPF_JIT_NEEDS_CLOSE_UPVAL      = 1u << 7,
  SV_OPF_JIT_NEEDS_IC_EPOCH         = 1u << 8,
  SV_OPF_JIT_LOCAL_NUMERIC_BAILOUT  = 1u << 9,
  SV_OPF_JIT_BRANCH32               = 1u << 10,
  SV_OPF_JIT_BRANCH8                = 1u << 11,
  SV_OPF_JIT_OSR_BACKEDGE           = 1u << 12,
  SV_OPF_BUILDER_TARGET             = 1u << 13,
  SV_OPF_JIT_INLINE_ARGC            = 1u << 14,
} sv_opcode_flags_t;

typedef enum {
#define OP_DEF(name, size, n_pop, n_push, f) OP_##name,
#include "silver/opcode.h"
  OP__COUNT
} sv_op_t;

typedef enum {
  SV_STABLE_BUILTIN_PROMISE_RESOLVE = 0,
} sv_stable_builtin_t;

extern const char *const sv_op_names[OP__COUNT];

static const uint8_t sv_op_size[OP__COUNT] = {
#define OP_DEF(name, size, n_pop, n_push, f) [OP_##name] = (size),
#include "silver/opcode.h"
};

static const uint16_t sv_op_flags[OP__COUNT] = {
#define OP_FLAG(name, flags) [OP_##name] = (flags),
#include "silver/opcode.h"
};

static const bool sv_op_ic_slots[OP__COUNT] = {
#define OP_IC_SLOT(name) [OP_##name] = true,
#include "silver/opcode.h"
};

static inline bool sv_op_has_ic_slot(sv_op_t op) {
  return (unsigned)op < OP__COUNT && sv_op_ic_slots[op];
}

typedef struct {
  const char *str;
  uint32_t    len;
} sv_atom_t;

typedef enum {
  SV_MAP_TEMPLATE_GET = 0,
  SV_MAP_TEMPLATE_HAS,
} sv_map_template_operation_t;

typedef struct sv_map_template_desc {
  sv_atom_t segments[SV_MAP_TEMPLATE_MAX_SUBSTITUTIONS + 1];
  uint8_t substitution_count;
  uint8_t operation;
} sv_map_template_desc_t;

typedef struct {
  uint32_t count;
  uint32_t magic;
} sv_map_template_table_header_t;


static inline bool sv_map_template_is_canonical_pair_get(
  const sv_map_template_desc_t *desc
) {
  return 
    desc && desc->operation == SV_MAP_TEMPLATE_GET &&
    desc->substitution_count == 2 &&
    desc->segments[0].len == 0 && desc->segments[2].len == 0;
}

static_assert(
  _Alignof(sv_map_template_desc_t) <= CODE_ARENA_ALIGNMENT,
  "map template descriptor alignment exceeds the code arena guarantee"
);

static_assert(
  sizeof(sv_map_template_table_header_t) % _Alignof(sv_map_template_desc_t) == 0,
  "map template table header must preserve descriptor alignment"
);

static_assert(
  UINT32_MAX <= SIZE_MAX / sizeof(sv_map_template_desc_t),
  "map template descriptor table must fit in size_t"
);

typedef struct {
  uint16_t  index;
  bool      is_local;
  bool      is_const;
} sv_upval_desc_t;

typedef struct {
  uint32_t bc_offset;
  uint32_t line;
  uint32_t col;
  uint32_t src_off;
  uint32_t src_end;
} sv_srcpos_t;

typedef enum {
  SV_TI_UNKNOWN = 0,
  SV_TI_NUM,
  SV_TI_STR,
  SV_TI_ARR,
  SV_TI_OBJ,
  SV_TI_BOOL,
  SV_TI_NULL,
  SV_TI_UNDEF,
} sv_local_type_t;

typedef struct {
  uint8_t type;
} sv_type_info_t;

typedef struct {
  ant_shape_t *cached_shape;
  ant_object_t *cached_holder;
  
  uint32_t cached_index;
  uint32_t epoch;
  uintptr_t cached_aux;
  
  // each IC slot belongs to one bytecode site/op. comparison ICs need both
  // their direct-prototype value and object-lifetime epoch simultaneously.
  union {
    ant_value_t receiver_proto;
    struct {
      ant_shape_t *from_shape;
      ant_shape_t *to_shape;
      uint32_t slot;
      uint32_t epoch;
    } add;
    
    struct {
      ant_value_t receiver_proto;
      uint32_t object_epoch;
    } comparison;
  } guard;
  
  bool cached_is_own;
  uint8_t shape_ref_mask;
  uint32_t prototype_epoch;
#ifdef ANT_WASM_EMBED
  uint32_t wasm32_proto_identity;
  uint8_t wasm32_cacheline_padding[12];
#endif
} sv_ic_entry_t;

static_assert(
  sizeof(sv_ic_entry_t) == 64,
  "IC entries must remain one cache line"
);

enum {
  SV_IC_SHAPE_REF_CACHED   = 1u << 0,
  SV_IC_SHAPE_REF_ADD_FROM = 1u << 1,
  SV_IC_SHAPE_REF_ADD_TO   = 1u << 2,
};

bool sv_ic_shape_ref_register(ant_t *js, ant_shape_t **slot);
void sv_ic_shape_refs_cleanup(ant_t *js);

typedef struct {
  uint32_t bc_off;
  ant_shape_t *shared_shape;
  const uint32_t *key_atoms;
  uint16_t key_count;
  bool shape_build_failed;
  ant_value_t literal_template;
} sv_obj_site_cache_t;

static constexpr uint32_t SV_GF_IC_AUX_MISS_SHIFT = 8u;
static constexpr uint32_t SV_GF_IC_PROTO_ID_SHIFT = 32u;
static constexpr uint32_t SV_GF_IC_WARMUP_ENABLE  = 16u;
static constexpr uint32_t SV_GF_IC_MISS_DISABLE   = 4u;

static constexpr uintptr_t SV_GF_IC_AUX_WARMUP_MASK = (uintptr_t)0xFFu;
static constexpr uintptr_t SV_GF_IC_AUX_MISS_MASK   = (uintptr_t)0xFF00u;
static constexpr uintptr_t SV_GF_IC_AUX_ACTIVE_BIT  = (uintptr_t)0x10000u;

#define SV_GF_IC_AUX_ALL_MASK \
  (SV_GF_IC_AUX_WARMUP_MASK | SV_GF_IC_AUX_MISS_MASK | SV_GF_IC_AUX_ACTIVE_BIT)

static inline uint8_t sv_gf_ic_warmup(uintptr_t aux) {
  return (uint8_t)(aux & SV_GF_IC_AUX_WARMUP_MASK);
}

static inline uint8_t sv_gf_ic_miss_streak(uintptr_t aux) {
  return (uint8_t)((aux & SV_GF_IC_AUX_MISS_MASK) >> SV_GF_IC_AUX_MISS_SHIFT);
}

static inline bool sv_gf_ic_active(uintptr_t aux) {
  return (aux & SV_GF_IC_AUX_ACTIVE_BIT) != 0;
}

static inline uintptr_t sv_gf_ic_pack_aux(uint8_t warmup, uint8_t miss_streak, bool active) {
  uintptr_t aux = ((uintptr_t)warmup & SV_GF_IC_AUX_WARMUP_MASK) |
                  ((uintptr_t)miss_streak << SV_GF_IC_AUX_MISS_SHIFT);
  if (active) aux |= SV_GF_IC_AUX_ACTIVE_BIT;
  return aux;
}

bool sv_lookup_srcpos(sv_func_t *func, int bc_offset, uint32_t *line, uint32_t *col);
bool sv_lookup_srcspan(sv_func_t *func, int bc_offset, uint32_t *src_off, uint32_t *src_end);

static constexpr uint32_t SV_TFB_CTOR_PROP_BINS = 17;
static constexpr uint32_t SV_TFB_CTOR_PROP_OVERFLOW_FROM = SV_TFB_CTOR_PROP_BINS - 1;

typedef struct {
  uint16_t    bc_off;
  uint8_t     miss_count;
  uint8_t     disabled;
  sv_func_t  *target;
} sv_call_target_fb_t;

typedef struct {
  uint64_t samples;
  uint64_t hist[SV_TFB_CTOR_PROP_BINS];
  uint8_t  inobj_limit;
  uint8_t  inobj_frozen;
} sv_ctor_prop_fb_t;

typedef struct {
  uint8_t *type_feedback;
  sv_ctor_prop_fb_t ctor_prop_fb;
} sv_func_sidecar_t;

static_assert(
  _Alignof(sv_func_sidecar_t) > ant_sidecar,
  "function sidecar pointer uses low-bit tag"
);

typedef struct {
  const char *name;
  uint32_t len;
  uint16_t index;
  uint8_t kind;
  bool is_const;
  const char *import_name;
  uint32_t import_len;
} sv_runtime_binding_t;

typedef struct {
  const sv_runtime_binding_t *bindings;
  uint32_t count;
} sv_eval_scope_t;

enum {
  SV_EVAL_BIND_PARAM = 0,
  SV_EVAL_BIND_LOCAL = 1,
  SV_EVAL_BIND_UPVALUE = 2,
  SV_EVAL_BIND_KIND_MASK = 3,
  SV_EVAL_BIND_LEXICAL = 4,
  SV_EVAL_BIND_CATCH = 8,
  SV_EVAL_BIND_IMPORT_DEFAULT = 16,
  SV_EVAL_BIND_IMPORT_NAMED = 32,
};

typedef struct {
  const char *str;
  uint32_t len;
  bool annex_b;
} sv_eval_decl_t;

typedef struct {
  sv_eval_scope_t *eval_scopes;
  uint32_t eval_scope_count;
  sv_eval_decl_t *eval_vars;
  uint32_t eval_var_count;
  sv_type_info_t local_types[];
} sv_func_metadata_t;

static_assert(
  _Alignof(sv_func_metadata_t) <= CODE_ARENA_ALIGNMENT,
  "function metadata alignment exceeds the code arena guarantee"
);

typedef struct {
  const char *name;
  const char *filename;
  sv_srcpos_t *srcpos;
  const char *source;

  int srcpos_count;
  int source_line;
  int source_len;
  int source_start;
  int source_end;
} sv_func_debug_t;

struct sv_func {
  uint8_t *code;
  ant_value_t *constants;

  struct sv_func **child_funcs;
  struct sv_func *parent;
  uint32_t *gc_const_slots;

  sv_atom_t *atoms;
  sv_ic_entry_t *ic_slots;
  sv_obj_site_cache_t *obj_sites;
  sv_upval_desc_t *upval_descs;
  sv_func_debug_t *debug;

  union {
    sv_type_info_t *local_types;
    sv_func_metadata_t *metadata;
  } type_data;

  void *jit_code;
  uint8_t *type_feedback;
  uint8_t *local_type_feedback;
  
  sv_call_target_fb_t *call_target_fb;
  uint64_t gc_epoch;

  int code_len;
  int const_count;
  int child_func_count;
  int gc_const_slot_count;
  int atom_count;
  int max_locals;
  int max_stack;
  int local_type_count;
  int upvalue_count;

  uint32_t obj_site_count;
  uint16_t ic_count;
  uint16_t param_count;
  uint16_t function_length;

  bool is_strict: 1;
  bool is_arrow: 1;
  bool is_async: 1;
  bool has_await: 1;
  bool is_generator: 1;
  bool is_method: 1;
  bool is_static: 1;
  bool is_tla: 1;
  bool is_derived_ctor: 1;
  bool has_dynamic_eval: 1;
  bool needs_eval_env: 1;
  bool is_eval: 1;
  bool is_curried_step: 1;
  bool is_fusable_leaf: 1;

  bool jit_compile_failed: 1;
  bool jit_compiling: 1;
  bool jit_loop_hot: 1;
  bool has_map_templates: 1;

  uint32_t call_count;
  uint32_t back_edge_count;
  uint32_t jit_bailout_tfb_ver;
  uint32_t tfb_version;
  uint32_t jit_compiled_tfb_ver;

  uint8_t jit_bailout_count;
  uint8_t call_target_fb_count;
};

static inline const sv_map_template_desc_t *sv_map_template_desc_at(
  const sv_func_t *func, uint32_t index
) {
  if (!func || !func->has_map_templates || !func->code) return NULL;

  const sv_map_template_table_header_t *header =
    (const sv_map_template_table_header_t *)(const void *)(
      func->code - sizeof(sv_map_template_table_header_t));
  if (header->magic != SV_MAP_TEMPLATE_TABLE_MAGIC ||
      header->count == 0 || index >= header->count)
    return NULL;

  size_t table_size =
    (size_t)header->count * sizeof(sv_map_template_desc_t);
  
  const sv_map_template_desc_t *table =
    (const sv_map_template_desc_t *)(const void *)(
    (const uint8_t *)header - table_size);
  
  const sv_map_template_desc_t *desc = &table[index];
  if (desc->substitution_count == 0 ||
      desc->substitution_count > SV_MAP_TEMPLATE_MAX_SUBSTITUTIONS ||
      desc->operation > SV_MAP_TEMPLATE_HAS)
    return NULL;

  for (uint8_t i = 0; i <= desc->substitution_count; i++)
    if (desc->segments[i].len > 0 && !desc->segments[i].str) return NULL;
  return desc;
}

static inline sv_obj_site_cache_t *sv_obj_site_for_offset(
  sv_func_t *func,
  uint32_t bc_off
) {
  if (!func || !func->obj_sites || func->obj_site_count == 0) return NULL;

  uint32_t lo = 0;
  uint32_t hi = func->obj_site_count;
  
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    uint32_t site_off = func->obj_sites[mid].bc_off;
    if (site_off < bc_off) lo = mid + 1;
    else hi = mid;
  }
  
  if (lo >= func->obj_site_count || func->obj_sites[lo].bc_off != bc_off) return NULL;
  return &func->obj_sites[lo];
}

static inline sv_func_metadata_t *sv_func_metadata(sv_func_t *func) {
  if (!func || !func->has_dynamic_eval) return NULL;
  ANT_ASSERT(
    func->type_data.metadata != NULL,
    "dynamic-eval function must have metadata"
  );
  return func->type_data.metadata;
}

static inline sv_type_info_t *sv_func_local_types(sv_func_t *func) {
  if (!func) return NULL;
  if (!func->has_dynamic_eval) return func->type_data.local_types;
  sv_func_metadata_t *metadata = sv_func_metadata(func);
  return metadata ? metadata->local_types : NULL;
}

static inline const sv_eval_scope_t *sv_func_eval_scope(sv_func_t *func, uint32_t index) {
  sv_func_metadata_t *metadata = sv_func_metadata(func);
  ANT_ASSERT(metadata != NULL, "eval opcode requires function metadata");
  ANT_ASSERT(
    index < metadata->eval_scope_count,
    "eval opcode scope index is outside function metadata"
  );
  return &metadata->eval_scopes[index];
}

typedef enum {
  SV_COMPLETION_NONE = 0,
  SV_COMPLETION_THROW = 1,
  SV_COMPLETION_RETURN = 2,
  SV_COMPLETION_JUMP = 3,
} sv_completion_kind_t;

typedef struct {
  sv_completion_kind_t kind;
  ant_value_t value;
  uint8_t *jump_ip;
  uint16_t jump_finallies;
  uint16_t jump_pops;
} sv_completion_t;

typedef enum {
  SV_RESUME_NEXT = 0,
  SV_RESUME_THROW = 1,
  SV_RESUME_RETURN = 2,
} sv_resume_kind_t;

typedef struct sv_frame {
  uint8_t *ip;
  ant_value_t *bp;
  ant_value_t *lp;
  
  sv_func_t *func;
  ant_value_t callee;
  ant_value_t this;
  ant_value_t new_target;
  ant_value_t super_val;
  
  int prev_sp;
  int argc;
  
  sv_completion_t completion;
  sv_upvalue_t **upvalues;

  int upvalue_count;
  uint16_t handler_base;
  uint16_t handler_top;
  
  ant_value_t with_obj;
  ant_value_t arguments_obj;
  ant_value_t eval_env;
} sv_frame_t;

static inline ant_value_t sv_frame_eval_env(ant_t *js, const sv_frame_t *frame) {
  return frame && is_object_type(frame->eval_env) ? frame->eval_env : js->global;
}

typedef enum {
  SV_HANDLER_TRY = 1,
  SV_HANDLER_FINALLY = 2,
  SV_HANDLER_TRY_FINALLY = 3,
} sv_handler_kind_t;

typedef struct {
  uint8_t *ip;
  int      saved_sp;
  uint8_t  kind;
} sv_handler_t;

struct sv_upvalue {
  ant_value_t *location;
  ant_value_t closed;
  struct sv_upvalue *next;
  uint64_t gc_epoch;
  uint8_t in_remember_set;
};

typedef struct sv_activation {
  int frame_count;
  int stack_count;
  int handler_count;
  size_t capacity;
  sv_upvalue_t *open_upvalues;
  sv_frame_t *frames;
  ant_value_t *slots;
  sv_handler_t *handlers;
} sv_activation_t;

sv_activation_t *sv_activation_capture(
  sv_vm_t *vm, int entry_fp, 
  sv_activation_t *reuse
);

bool sv_activation_install(sv_vm_t *vm, sv_activation_t *act);
void sv_activation_seal(ant_t *js, sv_activation_t *act);
void sv_activation_discard(sv_vm_t *vm, int entry_fp);

static inline void gc_upvalue_write_barrier(ant_t *js, sv_upvalue_t *uv, ant_value_t new_val) {
  if (uv->in_remember_set || uv->gc_epoch == 0) return;
  if (!is_tagged(new_val) || !gc_value_is_heap_ref(new_val)) return;
  if (uv->location == &uv->closed || gc_value_ref_is_young(new_val))
    gc_remember_upvalue(js, uv);
}

static inline void gc_upvalue_capture_barrier(ant_t *js, sv_upvalue_t *uv) {
  if (uv->in_remember_set || uv->gc_epoch == 0) return;
  ant_value_t value = *uv->location;
  if (gc_value_is_heap_ref(value) && gc_value_ref_is_young(value))
    gc_remember_upvalue(js, uv);
}

static inline sv_upvalue_t *js_upvalue_alloc(ant_t *js) {
  sv_upvalue_t *uv = (sv_upvalue_t *)fixed_arena_alloc(&js->upvalue_arena);
  if (uv) {
    if (js->young_upvalue_len < js->young_upvalue_cap)
      js->young_upvalues[js->young_upvalue_len++] = uv;
    else gc_track_young_upvalue_slow(js, uv);
  }
  return uv;
}

#define SV_CALL_HAS_BOUND_ARGS   (1u << 0)
#define SV_CALL_HAS_SUPER        (1u << 1)
#define SV_CALL_IS_ARROW         (1u << 2)
#define SV_CALL_IS_DEFAULT_CTOR  (1u << 3)
#define SV_CALL_BORROWED_UPVALS  (1u << 4)
#define SV_CALL_HAS_EVAL_ENV     (1u << 5)
#define SV_CALL_HAS_BOUND_THIS   (1u << 6)
#define SV_CALL_IS_UNCURRY       (1u << 7)

static constexpr char SV_CLASS_CTOR_CALL_ERROR[] =
  "Class constructor cannot be invoked without 'new'";

#define SV_CLOSURE_INLINE_UPVALS 4

typedef struct sv_closure {
  uint32_t call_flags;
  int bound_argc;
  
  sv_func_t *func;
  sv_upvalue_t **upvalues;
  sv_upvalue_t *inline_upvals[SV_CLOSURE_INLINE_UPVALS];
  
  ant_value_t bound_this;
  ant_value_t super_val;
  ant_value_t func_obj;

  union {
    struct {
      ant_value_t *argv;
      ant_value_t args_arr;
    } bound;
    struct {
      const char *name;
      uint32_t len;
    } pending;
  } u;

  ant_t *js;
  ant_value_t module_ctx;
  
  uint8_t in_remember_set;
  uint8_t generation;
  uint64_t gc_epoch;
} sv_closure_t;

static inline bool sv_closure_has_lexical_this(const sv_closure_t *closure) {
  return 
    closure->func->is_arrow || 
    (closure->call_flags & SV_CALL_HAS_BOUND_THIS);
}

static inline void js_closure_alloc_prepare(ant_t *js) {
  js->gc_closure_alloc++;
  if (js->young_closure_len >= js->young_closure_trigger) gc_pressure(js);
}

static inline sv_closure_t *js_closure_alloc_finish(
  ant_t *js, sv_closure_t *c
) {
  c->gc_epoch = gc_get_epoch();
  c->js = js;
  c->func_obj = 0;
  c->module_ctx = mkval(kTypeUndefined, 0);
  c->u.pending.name = NULL;
  c->u.pending.len = 0;
  c->in_remember_set = 0;
  c->generation = 0;
  
  if (js->young_closure_len < js->young_closure_cap)
    js->young_closures[js->young_closure_len++] = c;
  else gc_track_young_closure_slow(js, c);
  
  return c;
}

static inline sv_closure_t *js_closure_alloc(ant_t *js) {
  js_closure_alloc_prepare(js);
  sv_closure_t *c = (sv_closure_t *)fixed_arena_alloc(&js->closure_arena);
  if (!c) return NULL;
  return js_closure_alloc_finish(js, c);
}

static inline sv_closure_t *js_closure_alloc_hot(ant_t *js) {
  js_closure_alloc_prepare(js);
  sv_closure_t *c = (sv_closure_t *)fixed_arena_alloc_uninit(&js->closure_arena);
  
  if (!c) return NULL;
  c->call_flags = 0;
  c->upvalues = NULL;
  
  return js_closure_alloc_finish(js, c);
}

static inline sv_closure_t *js_func_closure(ant_value_t func) {
  return (sv_closure_t *)vptr(func);
}

ant_value_t sv_closure_materialize_func_obj(ant_t *js, sv_closure_t *c, ant_value_t func_val);

static inline ant_value_t js_func_obj(ant_value_t func) {
  sv_closure_t *c = js_func_closure(func);
  if (__builtin_expect(c->func_obj == 0, 0))
    return sv_closure_materialize_func_obj(c->js, c, func);
  return c->func_obj;
}

static inline ant_value_t sv_closure_eval_env(const sv_closure_t *closure) {
  if (!closure || !(closure->call_flags & SV_CALL_HAS_EVAL_ENV) ||
      !is_object_type(closure->func_obj)) return js_mkundef();
  ant_value_t env = js_get_slot(closure->func_obj, SLOT_EVAL_ENV);
  return is_object_type(env) ? env : js_mkundef();
}

static inline ant_value_t js_as_obj(ant_value_t v) {
  uint8_t t = vtype(v);
  if (t == kTypeObject) return v;
  if (t == kTypeFunction) return js_func_obj(v);
  return mkval(kTypeObject, vdata(v));
}

typedef ant_value_t (*sv_jit_func_t)(
  sv_vm_t *,
  ant_value_t,
  ant_value_t,
  ant_value_t,
  ant_value_t *,
  int, sv_closure_t *
);

ant_value_t sv_execute_closure_entry(
  sv_vm_t *vm,sv_closure_t *closure,
  ant_value_t callee_func, ant_value_t super_val,
  ant_value_t new_target, ant_value_t this_val,
  ant_value_t *args, int argc, ant_value_t *out_this
);

ant_value_t sv_execute_eval_entry(
  sv_vm_t *vm, sv_func_t *func, ant_value_t this_val, 
  ant_value_t eval_env, ant_value_t new_target
);

ant_value_t sv_call_compiled_zero_upvalues(
  ant_t *js, sv_func_t *func,
  ant_value_t this_val, ant_value_t *args, int argc
);

typedef struct {
  bool active;
  int bc_offset;
  ant_value_t *locals;
  int n_locals;
  ant_value_t *lp;
  ant_value_t *vstack;
  int vstack_sp;
} sv_jit_osr_t;

#define SV_TRY_MAX  64
#define SV_TDZ      T_EMPTY
#define SV_HANDLER_MAX (SV_TRY_MAX * 2)

static_assert(SV_HANDLER_MAX <= UINT16_MAX,
  "frame handler indexes must fit in uint16_t");

#ifdef ANT_WASM_EMBED
#define SV_FRAMES_HARD_MAX 16384
#define SV_STACK_HARD_MAX  131072
#else
#define SV_FRAMES_HARD_MAX 65536
#define SV_STACK_HARD_MAX  524288
#endif

typedef struct sv_native_frame {
  struct sv_native_frame *caller;
  ant_value_t new_target;
} sv_native_frame_t;

struct sv_vm {
  ant_t *js;

  ant_value_t *stack;
  int sp;
  int stack_size;

  sv_frame_t *frames;
  int fp;
  int max_frames;

  sv_handler_t handler_stack[SV_HANDLER_MAX];
  sv_upvalue_t *open_upvalues;
  int handler_depth;
  
  // TODO: move to nested struct
  bool suspended;
  bool suspended_resume_pending;
  bool suspended_resume_is_error;
  sv_resume_kind_t suspended_resume_kind;
  
  int suspended_entry_fp;
  int suspended_saved_fp;
  ant_value_t suspended_resume_value;

  struct {
    bool active;
    int64_t ip_offset;
    ant_value_t *params;
    int64_t n_params;
    ant_value_t *locals;
    int64_t n_locals;
    ant_value_t *vstack;
    int64_t vstack_sp;
  } jit_resume;

  sv_jit_osr_t jit_osr;
  sv_native_frame_t *native_frame;
};

static inline uint8_t sv_get_u8(const uint8_t *ip)  { return ip[0]; }
static inline int8_t  sv_get_i8(const uint8_t *ip)  { return (int8_t)ip[0]; }

static inline uint16_t sv_get_u16(const uint8_t *ip) {
  uint16_t v; memcpy(&v, ip, 2); return v;
}

static inline int16_t sv_get_i16(const uint8_t *ip) {
  int16_t v; memcpy(&v, ip, 2); return v;
}

static inline uint32_t sv_get_u32(const uint8_t *ip) {
  uint32_t v; memcpy(&v, ip, 4); return v;
}

static inline int32_t sv_get_i32(const uint8_t *ip) {
  int32_t v; memcpy(&v, ip, 4); return v;
}

static inline const char *sv_atom_cstr(sv_atom_t *a, char *buf, size_t bufsz) {
  size_t n = a->len < bufsz - 1 ? a->len : bufsz - 1;
  memcpy(buf, a->str, n);
  buf[n] = '\0';
  return buf;
}

static inline bool sv_frame_is_strict(const sv_frame_t *frame) {
  return frame && frame->func && frame->func->is_strict;
}

static inline bool sv_slot_in_range(
  const ant_value_t *base, size_t count, 
  const ant_value_t *slot
) {
  if (!base || !slot || count == 0) return false;

  uintptr_t lo = (uintptr_t)base;
  uintptr_t hi = lo + count * sizeof(*base);
  uintptr_t addr = (uintptr_t)slot;
  return addr >= lo && addr < hi;
}

static inline bool sv_slot_in_vm_stack(const sv_vm_t *vm, const ant_value_t *slot) {
  return vm && sv_slot_in_range(vm->stack, (size_t)vm->stack_size, slot);
}

static inline bool sv_is_nullish_this(ant_value_t v) {
  return 
    vtype(v) == kTypeUndefined || vtype(v) == kTypeNull ||
    (vtype(v) == kTypeObject && vdata(v) == 0);
}

static inline ant_value_t sv_normalize_this_for_frame(ant_t *js, sv_func_t *func, ant_value_t this_val) {
  if (!func || func->is_arrow) return this_val;
  if (func->is_strict) return this_val;
  uint8_t type = vtype(this_val);
  if (type == kTypeUndefined || type == kTypeNull) return js->global;
  if (is_object_type(this_val) || type == kTypeBuiltin) return this_val;
  return js_normalize_sloppy_this(js, this_val);
}

static inline bool sv_vm_is_strict(const sv_vm_t *vm) {
  if (vm && vm->fp >= 0) {
    const sv_frame_t *f = &vm->frames[vm->fp];
    return f->func && f->func->is_strict;
  }
  return false;
}

static inline bool sv_is_strict_context(ant_t *js) {
  return sv_vm_is_strict(js->vm);
}

static inline ant_value_t sv_vm_get_new_target(const sv_vm_t *vm) {
  if (vm && vm->fp >= 0) return vm->frames[vm->fp].new_target;
  return js_mkundef();
}

static inline ant_value_t sv_vm_get_super_val(const sv_vm_t *vm) {
  if (vm && vm->fp >= 0) return vm->frames[vm->fp].super_val;
  return js_mkundef();
}

static inline int sv_frame_arg_slots(const sv_frame_t *frame) {
  if (!frame || !frame->func) return 0;
  return frame->argc > frame->func->param_count ? frame->argc : frame->func->param_count;
}

static inline ant_value_t sv_frame_get_arg_value(const sv_frame_t *frame, uint16_t idx) {
  int arg_slots = sv_frame_arg_slots(frame);
  if (!frame || !frame->bp || (int)idx >= arg_slots) return js_mkundef();
  return frame->bp[idx];
}

static inline void sv_frame_set_arg_value(ant_t *js, sv_frame_t *frame, uint16_t idx, ant_value_t val) {
  int arg_slots = sv_frame_arg_slots(frame);
  if (!frame || !frame->bp || (int)idx >= arg_slots) return;
  frame->bp[idx] = val;
  if (vtype(frame->arguments_obj) != kTypeUndefined)
    js_arguments_sync_slot(js, frame->arguments_obj, idx, val);
}

static inline ant_value_t *sv_frame_slot_ptr(sv_frame_t *frame, uint16_t slot_idx) {
  if (!frame || !frame->func) return NULL;
  int param_count = frame->func->param_count;
  if ((int)slot_idx < param_count) {
    int arg_slots = sv_frame_arg_slots(frame);
    if ((int)slot_idx >= arg_slots || !frame->bp) return NULL;
    return &frame->bp[slot_idx];
  }
  if (!frame->lp) return NULL;
  return &frame->lp[slot_idx - param_count];
}

static inline uint16_t sv_frame_total_slots(const sv_frame_t *frame) {
  if (!frame || !frame->func) return 0;
  int total = frame->func->param_count + frame->func->max_locals;
  return total > 0 ? (uint16_t)total : 0;
}

ant_value_t sv_string_builder_read_value(
  ant_t *js, ant_value_t value
);

ant_value_t sv_string_builder_flush_slot(
  sv_vm_t *vm, ant_t *js, 
  sv_frame_t *frame, uint16_t slot_idx
);

ant_value_t sv_string_builder_append_slot(
  sv_vm_t *vm, ant_t *js, sv_frame_t *frame,
  sv_func_t *func, uint16_t slot_idx, ant_value_t rhs
);

ant_value_t sv_string_builder_append_snapshot_slot(
  sv_vm_t *vm, ant_t *js, sv_frame_t *frame,
  sv_func_t *func, uint16_t slot_idx, ant_value_t lhs, ant_value_t rhs
);

#endif
