#ifndef SILVER_ENGINE_H
#define SILVER_ENGINE_H

#include "silver/vm.h"
#include "internal.h"
#include "runtime.h"
#include "errors.h"
#include "gc/objects.h"
#include "modules/timer.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef enum {
#define OP_DEF(name, size, n_pop, n_push, f) OP_##name,
#include "silver/opcode.h"
  OP__COUNT
} sv_op_t;

static const uint8_t sv_op_size[OP__COUNT] = {
#define OP_DEF(name, size, n_pop, n_push, f) [OP_##name] = (size),
#include "silver/opcode.h"
};

typedef struct {
  const char *str;
  uint32_t    len;
} sv_atom_t;

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
  ant_shape_t *add_from_shape;
  ant_shape_t *add_to_shape;
  uint32_t add_slot;
  uint32_t add_epoch;
  bool cached_is_own;
} sv_ic_entry_t;

typedef struct {
  uint32_t bc_off;
  ant_shape_t *shared_shape;
} sv_obj_site_cache_t;

#define SV_GF_IC_AUX_WARMUP_MASK    ((uintptr_t)0xFFu)
#define SV_GF_IC_AUX_MISS_MASK      ((uintptr_t)0xFF00u)
#define SV_GF_IC_AUX_ACTIVE_BIT     ((uintptr_t)0x10000u)
#define SV_GF_IC_AUX_MISS_SHIFT     8u

#define SV_GF_IC_WARMUP_ENABLE      16u
#define SV_GF_IC_MISS_DISABLE       4u

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

#ifdef ANT_JIT
typedef struct {
  uint16_t    bc_off;
  uint8_t     miss_count;
  uint8_t     disabled;
  sv_func_t  *target;
} sv_call_target_fb_t;
#endif

struct sv_func {
  uint8_t *code;
  int code_len;
  
  ant_value_t *constants;
  int const_count;
  
  struct sv_func **child_funcs;
  int child_func_count;
  
  uint32_t *gc_const_slots;
  int gc_const_slot_count;
  
  sv_atom_t *atoms;
  int atom_count;

  sv_ic_entry_t *ic_slots;
  uint16_t ic_count;
  
  sv_obj_site_cache_t *obj_sites;
  uint16_t obj_site_count;
  
  sv_upval_desc_t *upval_descs;
  int max_locals;
  int max_stack;
  
  sv_type_info_t *local_types;
  int local_type_count;
  
  int param_count;
  int upvalue_count;
  
  bool is_strict;
  bool is_arrow;
  bool is_async;
  bool has_await;
  bool is_generator;
  bool is_method;
  bool is_static;
  bool is_tla;
  uint64_t gc_epoch;
  
  const char *name;
  const char *filename;
  
  sv_srcpos_t *srcpos;
  int srcpos_count;
  int source_line;
  
  const char *source;
  int source_len;
  int source_start;
  int source_end;
  
#ifdef ANT_JIT
  void *jit_code;
  
  uint32_t call_count;
  uint32_t back_edge_count;
  
  bool jit_compile_failed;
  bool jit_compiling;
  uint32_t tfb_version;
  uint32_t jit_compiled_tfb_ver;
  uint8_t *type_feedback;
  uint8_t *local_type_feedback;
  uint64_t ctor_prop_samples;
  uint64_t ctor_prop_hist[17];
  uint8_t ctor_inobj_limit;
  uint8_t ctor_inobj_frozen;

  sv_call_target_fb_t *call_target_fb;
  uint8_t call_target_fb_count;
#endif
};

typedef enum {
  SV_COMPLETION_NONE = 0,
  SV_COMPLETION_THROW = 1,
  SV_COMPLETION_RETURN = 2,
} sv_completion_kind_t;

typedef struct {
  sv_completion_kind_t kind;
  ant_value_t              value;
} sv_completion_t;

typedef enum {
  SV_RESUME_NEXT = 0,
  SV_RESUME_THROW = 1,
  SV_RESUME_RETURN = 2,
} sv_resume_kind_t;

typedef struct 
  sv_upvalue sv_upvalue_t;

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
  int handler_base;
  int handler_top;
  int argc;
  
  sv_completion_t completion;
  sv_upvalue_t **upvalues;
  int upvalue_count;
  
  ant_value_t with_obj;
  ant_value_t arguments_obj;
} sv_frame_t;

typedef enum {
  SV_HANDLER_TRY = 1,
  SV_HANDLER_FINALLY = 2,
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
};

static inline sv_upvalue_t *js_upvalue_alloc(void) {
  return (sv_upvalue_t *)fixed_arena_alloc(&rt->js->upvalue_arena);
}

bool sv_slot_has_open_upvalue(sv_vm_t *vm, ant_value_t *slot);

#define SV_CALL_HAS_BOUND_ARGS   (1u << 0)
#define SV_CALL_HAS_SUPER        (1u << 1)
#define SV_CALL_IS_ARROW         (1u << 2)
#define SV_CALL_IS_DEFAULT_CTOR  (1u << 3)
#define SV_CALL_BORROWED_UPVALS  (1u << 4)

typedef struct sv_closure {
  uint32_t call_flags;
  int bound_argc;
  sv_func_t *func;
  
  sv_upvalue_t **upvalues;
  ant_value_t bound_this;
  ant_value_t *bound_argv;
  ant_value_t bound_args;
  ant_value_t super_val;
  ant_value_t func_obj;
  
  uint64_t gc_epoch;
} sv_closure_t;

static inline sv_closure_t *js_closure_alloc(ant_t *js) {
  sv_closure_t *c = (sv_closure_t *)fixed_arena_alloc(&js->closure_arena);
  if (c) c->gc_epoch = gc_get_epoch();
  return c;
}

static inline sv_closure_t *js_func_closure(ant_value_t func) {
  return (sv_closure_t *)(uintptr_t)vdata(func);
}

static inline ant_value_t js_func_obj(ant_value_t func) {
  return js_func_closure(func)->func_obj;
}

static inline ant_value_t js_as_obj(ant_value_t v) {
  uint8_t t = vtype(v);
  if (t == T_OBJ) return v;
  if (t == T_FUNC) return js_func_obj(v);
  return mkval(T_OBJ, vdata(v));
}

ant_value_t sv_execute_closure_entry(
  sv_vm_t *vm,sv_closure_t *closure,
  ant_value_t callee_func, ant_value_t super_val,
  ant_value_t this_val, ant_value_t *args,
  int argc, ant_value_t *out_this
);

#ifdef ANT_JIT
typedef struct {
  bool active;
  int bc_offset;
  ant_value_t *locals;
  int n_locals;
  ant_value_t *lp;
} sv_jit_osr_t;
#endif

#define SV_TRY_MAX  64
#define SV_TDZ      T_EMPTY
#define SV_HANDLER_MAX (SV_TRY_MAX * 2)

#define SV_FRAMES_HARD_MAX 65536
#define SV_STACK_HARD_MAX  524288

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

#ifdef ANT_JIT
  struct {
    bool active;
    int64_t ip_offset;
    ant_value_t *locals;
    int64_t n_locals;
    ant_value_t *vstack;
    int64_t vstack_sp;
  } jit_resume;

  sv_jit_osr_t jit_osr;
#endif
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
    vtype(v) == T_UNDEF || vtype(v) == T_NULL ||
    (vtype(v) == T_OBJ && vdata(v) == 0);
}

static inline ant_value_t sv_normalize_this_for_frame(ant_t *js, sv_func_t *func, ant_value_t this_val) {
  if (!func || func->is_arrow) return this_val;
  if (func->is_strict) return sv_is_nullish_this(this_val) ? js_mkundef() : this_val;
  return sv_is_nullish_this(this_val) ? js->global : this_val;
}

static inline bool sv_vm_is_strict(const sv_vm_t *vm) {
  if (vm && vm->fp >= 0) {
    const sv_frame_t *f = &vm->frames[vm->fp];
    return f->func && f->func->is_strict;
  }
  return false;
}

static inline sv_vm_t *sv_vm_get_active(ant_t *js) {
  if (!js) return NULL;
  if (js->active_async_coro) {
    if (js->active_async_coro->sv_vm) return js->active_async_coro->sv_vm;
    if (js->active_async_coro->owner_vm) return js->active_async_coro->owner_vm;
  }
  return js->vm;
}

static inline bool sv_is_strict_context(ant_t *js) {
  return sv_vm_is_strict(sv_vm_get_active(js));
}

static inline ant_value_t sv_vm_get_new_target(const sv_vm_t *vm, ant_t *js) {
  if (vm && vm->fp >= 0) return vm->frames[vm->fp].new_target;
  return js->new_target;
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
  if (vtype(frame->arguments_obj) != T_UNDEF)
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

static inline void sv_vm_maybe_checkpoint_microtasks(ant_t *js) {
  if (!js || js->microtasks_draining || js->vm_exec_depth != 0) return;
  js_maybe_drain_microtasks(js);
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

typedef struct {
  ant_value_t this_val;
  ant_value_t super_val;
  ant_value_t *args;
  int argc;
  ant_value_t *alloc;
} sv_call_ctx_t;

typedef enum {
  SV_CALL_MODE_NORMAL = 0,
  SV_CALL_MODE_EXPLICIT_THIS,
  SV_CALL_MODE_CONSTRUCT,
} sv_call_mode_t;

typedef enum {
  SV_CALL_EXEC_NATIVE = 0,
  SV_CALL_EXEC_PROXY_APPLY,
  SV_CALL_EXEC_PROXY_CONSTRUCT,
  SV_CALL_EXEC_DEFAULT_CTOR,
  SV_CALL_EXEC_CLOSURE,
} sv_call_exec_kind_t;

typedef struct {
  sv_call_exec_kind_t kind;
  ant_value_t func;
  sv_closure_t *closure;
  sv_call_ctx_t ctx;
} sv_call_plan_t;

static inline ant_value_t *sv_prepend_bound_args(
  sv_closure_t *closure, ant_value_t *args, int argc, int *out_total
) {
  int total = closure->bound_argc + argc;
  ant_value_t *combined = malloc(sizeof(ant_value_t) * (size_t)total);
  
  if (!combined) { *out_total = argc; return NULL; }
  memcpy(combined, closure->bound_argv, sizeof(ant_value_t) * (size_t)closure->bound_argc);
  memcpy(combined + closure->bound_argc, args, sizeof(ant_value_t) * (size_t)argc);
  
  *out_total = total;
  return combined;
}

static inline bool sv_call_mode_is_construct(sv_call_mode_t mode) {
  return mode == SV_CALL_MODE_CONSTRUCT;
}

static inline ant_value_t sv_call_normalize_this(ant_t *js, ant_value_t this_val, sv_call_mode_t mode) {
  if (mode == SV_CALL_MODE_NORMAL && sv_is_nullish_this(this_val)) return js->global;
  return this_val;
}

static inline ant_value_t sv_call_resolve_bound(
  ant_t *js, sv_closure_t *closure,
  sv_call_ctx_t *ctx, sv_call_mode_t mode
) {
  uint32_t flags = closure->call_flags;

  if (flags & SV_CALL_IS_ARROW) ctx->this_val = closure->bound_this;
  else if (!sv_call_mode_is_construct(mode) && vtype(closure->bound_this) != T_UNDEF)
    ctx->this_val = closure->bound_this;

  if ((flags & SV_CALL_HAS_BOUND_ARGS) && closure->bound_argc > 0) {
    int total;
    ant_value_t *combined = sv_prepend_bound_args(closure, ctx->args, ctx->argc, &total);
    if (!combined) return js_mkerr(js, "out of memory");
    ctx->args  = combined;
    ctx->argc  = total;
    ctx->alloc = combined;
  }

  if (flags & SV_CALL_HAS_SUPER) ctx->super_val = closure->super_val;
  return js_mkundef();
}

static inline void sv_call_cleanup(ant_t *js, sv_call_ctx_t *ctx) {
  if (ctx->alloc) { free(ctx->alloc); ctx->alloc = NULL; }
}

static inline ant_value_t sv_call_default_ctor(
  sv_vm_t *vm, ant_t *js, sv_closure_t *closure,
  sv_call_ctx_t *ctx, ant_value_t *out_this
);

static inline ant_value_t sv_call_resolve_closure(
  sv_vm_t *vm, ant_t *js, sv_closure_t *closure,
  ant_value_t callee_func, sv_call_ctx_t *ctx, ant_value_t *out_this
);

static inline ant_value_t sv_prepare_call(
  sv_vm_t *vm, ant_t *js, ant_value_t func,
  ant_value_t this_val, ant_value_t *args, int argc,
  ant_value_t *out_this, sv_call_mode_t mode, sv_call_plan_t *plan
) {
  bool is_construct_call = sv_call_mode_is_construct(mode);

  plan->kind = SV_CALL_EXEC_NATIVE;
  plan->func = func;
  plan->closure = NULL;
  
  plan->ctx = (sv_call_ctx_t){
    .this_val = this_val,
    .super_val = js_mkundef(),
    .args = args,
    .argc = argc,
    .alloc = NULL,
  };

  if (!is_construct_call) js->new_target = js_mkundef();
  if (out_this) *out_this = this_val;

  if (is_construct_call && vtype(func) == T_OBJ && is_proxy(func)) {
    plan->kind = SV_CALL_EXEC_PROXY_CONSTRUCT;
    return js_mkundef();
  }

  if (is_construct_call && !js_is_constructor(js, func))
    return js_mkerr_typed(js, JS_ERR_TYPE, "not a constructor");

  if (!is_construct_call && vtype(func) == T_OBJ && is_proxy(func)) {
    plan->kind = SV_CALL_EXEC_PROXY_APPLY;
    return js_mkundef();
  }

  if (vtype(func) == T_CFUNC) {
    plan->ctx.this_val = sv_call_normalize_this(js, this_val, mode);
    if (out_this) *out_this = plan->ctx.this_val;
    return js_mkundef();
  }

  if (vtype(func) != T_FUNC)
    return js_mkerr_typed(js, JS_ERR_TYPE, "%s is not a function", typestr(vtype(func)));

  sv_closure_t *closure = js_func_closure(func);
  plan->closure = closure;

  ant_value_t err = sv_call_resolve_bound(js, closure, &plan->ctx, mode);
  if (is_err(err)) return err;

  if (is_construct_call) plan->ctx.this_val = this_val;
  if (out_this) *out_this = plan->ctx.this_val;

  if (closure->call_flags & SV_CALL_IS_DEFAULT_CTOR) {
    plan->kind = SV_CALL_EXEC_DEFAULT_CTOR;
    return js_mkundef();
  }

  if (closure->func != NULL) {
    plan->kind = SV_CALL_EXEC_CLOSURE;
    return js_mkundef();
  }

  return js_mkundef();
}

static inline ant_value_t sv_execute_call_plan(
  sv_vm_t *vm, ant_t *js, sv_call_plan_t *plan, ant_value_t *out_this
) {
  switch (plan->kind) {
  case SV_CALL_EXEC_PROXY_APPLY: return js_proxy_apply(
    js, plan->func, plan->ctx.this_val, plan->ctx.args, plan->ctx.argc
  );
  
  case SV_CALL_EXEC_PROXY_CONSTRUCT: return js_proxy_construct(
    js, plan->func, plan->ctx.args, plan->ctx.argc, sv_vm_get_new_target(vm, js)
  );
  
  case SV_CALL_EXEC_DEFAULT_CTOR: return sv_call_default_ctor(
    vm, js, plan->closure, &plan->ctx, out_this
  );
  
  case SV_CALL_EXEC_CLOSURE: return sv_call_resolve_closure(
    vm, js, plan->closure, plan->func, &plan->ctx, out_this
  );
  
  case SV_CALL_EXEC_NATIVE: {
    ant_value_t result = sv_call_native(
      js, plan->func, plan->ctx.this_val, plan->ctx.args, plan->ctx.argc
    );
    sv_call_cleanup(js, &plan->ctx);
    return result;
  }}

  return js_mkerr(js, "invalid call plan");
}

static inline bool sv_check_c_stack_overflow(ant_t *js) {
  volatile char marker;
  if (js->cstk.limit == 0 || js->cstk.base == NULL) return false;
  
  uintptr_t base = (uintptr_t)js->cstk.base;
  uintptr_t curr = (uintptr_t)&marker;
  
  size_t used = (base > curr) ? (base - curr) : (curr - base);
  return used > js->cstk.limit;
}

static inline ant_value_t sv_vm_call(
  sv_vm_t *vm, ant_t *js, ant_value_t func,
  ant_value_t this_val, ant_value_t *args, int argc,
  ant_value_t *out_this, bool is_construct_call
) {
  if (sv_check_c_stack_overflow(js))
    return js_mkerr_typed(js, JS_ERR_RANGE | JS_ERR_NO_STACK, "Maximum call stack size exceeded");

  sv_call_mode_t mode = is_construct_call 
    ? SV_CALL_MODE_CONSTRUCT 
    : SV_CALL_MODE_NORMAL;
  
  sv_call_plan_t plan;
  ant_value_t err = sv_prepare_call(
    vm, js, func, this_val, args, argc,
    out_this, mode, &plan
  );
  
  if (is_err(err)) return err;
  ant_value_t result = sv_execute_call_plan(vm, js, &plan, out_this);
  sv_vm_maybe_checkpoint_microtasks(js);
  
  return result;
}

static inline ant_value_t sv_vm_call_explicit_this(
  sv_vm_t *vm, ant_t *js, ant_value_t func,
  ant_value_t this_val, ant_value_t *args, int argc
) {
  if (sv_check_c_stack_overflow(js))
    return js_mkerr_typed(js, JS_ERR_RANGE | JS_ERR_NO_STACK, "Maximum call stack size exceeded");

  sv_call_plan_t plan;
  ant_value_t err = sv_prepare_call(
    vm, js, func, this_val, args, argc, NULL,
    SV_CALL_MODE_EXPLICIT_THIS, &plan
  );
  
  if (is_err(err)) return err;
  ant_value_t result = sv_execute_call_plan(vm, js, &plan, NULL);
  sv_vm_maybe_checkpoint_microtasks(js);
  
  return result;
}

static inline ant_value_t sv_call_default_ctor(
  sv_vm_t *vm, ant_t *js, sv_closure_t *closure,
  sv_call_ctx_t *ctx, ant_value_t *out_this
) {
  if (vtype(js->new_target) == T_UNDEF) {
  sv_call_cleanup(js, ctx);
  return js_mkerr_typed(
    js, JS_ERR_TYPE, 
    "Class constructor cannot be invoked without 'new'"
  );}

  ant_value_t super_ctor = closure->super_val;
  uint8_t st = vtype(super_ctor);
  
  if (st == T_FUNC || st == T_CFUNC) {
    ant_value_t super_this = ctx->this_val;
    ant_value_t result = sv_vm_call(
      vm, js, super_ctor, ctx->this_val,
      ctx->args, ctx->argc, &super_this, true
    );
    
    if (out_this) *out_this = super_this;
    sv_call_cleanup(js, ctx);
    
    return result;
  }

  sv_call_cleanup(js, ctx);
  return js_mkundef();
}

ant_value_t sv_call_async_closure_dispatch(
  sv_vm_t *vm, ant_t *js, sv_closure_t *closure,
  ant_value_t callee_func, ant_value_t super_val,
  ant_value_t this_val, ant_value_t *args, int argc
);

ant_value_t sv_call_generator_closure_dispatch(
  sv_vm_t *vm, ant_t *js, sv_closure_t *closure,
  ant_value_t callee_func, ant_value_t super_val,
  ant_value_t this_val, ant_value_t *args, int argc
);

static inline ant_value_t sv_call_async_closure(
  sv_vm_t *vm, ant_t *js, sv_closure_t *closure,
  ant_value_t callee_func, sv_call_ctx_t *ctx
) {
  ant_value_t result = sv_call_async_closure_dispatch(
    vm, js, closure, callee_func,
    ctx->super_val, ctx->this_val, ctx->args, ctx->argc
  );
  sv_call_cleanup(js, ctx);
  return result;
}

static inline ant_value_t sv_call_generator_closure(
  sv_vm_t *vm, ant_t *js, sv_closure_t *closure,
  ant_value_t callee_func, sv_call_ctx_t *ctx
) {
  ant_value_t result = sv_call_generator_closure_dispatch(
    vm, js, closure, callee_func,
    ctx->super_val, ctx->this_val, ctx->args, ctx->argc
  );
  sv_call_cleanup(js, ctx);
  return result;
}

static inline ant_value_t sv_call_closure(
  sv_vm_t *vm, ant_t *js, sv_closure_t *closure,
  ant_value_t callee_func, sv_call_ctx_t *ctx, ant_value_t *out_this
) {
  ant_value_t result = sv_execute_closure_entry(
    vm, closure, callee_func, ctx->super_val,
    ctx->this_val, ctx->args, ctx->argc, out_this
  );
  sv_call_cleanup(js, ctx);
  return result;
}

#ifdef ANT_JIT

#define SV_TFB_NUM   (1 << 0)
#define SV_TFB_STR   (1 << 1)
#define SV_TFB_BOOL  (1 << 2)
#define SV_TFB_OTHER (1 << 3)
#define SV_TFB_CTOR_PROP_BINS 17
#define SV_TFB_CTOR_PROP_OVERFLOW_FROM (SV_TFB_CTOR_PROP_BINS - 1)
#define SV_TFB_INOBJ_SLACK_ALLOCATIONS 32
#define SV_TFB_INOBJ_P90_NUMERATOR 9
#define SV_TFB_INOBJ_P90_DENOMINATOR 10

#define SV_JIT_THRESHOLD       100
#define SV_JIT_RECOMPILE_DELAY 50
#define SV_TFB_ALLOC_THRESHOLD 2

#define SV_CALL_FB_MAX_SLOTS   32
#define SV_CALL_FB_MISS_DISABLE 4

#define SV_JIT_RETRY_INTERP    mkval(T_ERR, 1)
#define SV_JIT_MAGIC           0xBA110ULL

#define SV_JIT_BAILOUT \
  (NANBOX_PREFIX \
    | ((ant_value_t)T_SENTINEL << NANBOX_TYPE_SHIFT) \
    | SV_JIT_MAGIC)
  
static inline bool sv_is_jit_bailout(ant_value_t v) { 
  return v == SV_JIT_BAILOUT;
}

static inline void sv_jit_enter(ant_t *js) {
  if (js) js->jit_active_depth++;
}

static inline void sv_jit_leave(ant_t *js) {
  if (js && js->jit_active_depth > 0) js->jit_active_depth--;
}

static inline void sv_jit_on_bailout(sv_func_t *fn) {
  fn->jit_code = NULL;
  fn->back_edge_count = 0;
  fn->call_count = SV_JIT_THRESHOLD - SV_JIT_RECOMPILE_DELAY;
}

typedef ant_value_t (*sv_jit_func_t)(
  sv_vm_t *,
  ant_value_t,
  ant_value_t,
  ant_value_t,
  ant_value_t *,
  int, sv_closure_t *
);

ant_value_t sv_jit_try_compile_and_call(sv_vm_t *vm, ant_t *js,
  sv_closure_t *closure, ant_value_t callee_func,
  sv_call_ctx_t *ctx, ant_value_t *out_this
);

static inline uint8_t sv_tfb_classify(ant_value_t v) {
  if (vtype(v) == T_NUM) return SV_TFB_NUM;
  if (vtype(v) == T_STR) return SV_TFB_STR;
  if (vtype(v) == T_BOOL) return SV_TFB_BOOL;
  return SV_TFB_OTHER;
}

static inline void sv_tfb_record2(sv_func_t *func, uint8_t *ip, ant_value_t l, ant_value_t r) {
if (func->type_feedback) {
  int off = (int)(ip - func->code);
  uint8_t old = func->type_feedback[off];
  uint8_t neu = old | sv_tfb_classify(l) | sv_tfb_classify(r);
  if (neu != old) { func->type_feedback[off] = neu; func->tfb_version++; }
}}

static inline void sv_tfb_record1(sv_func_t *func, uint8_t *ip, ant_value_t v) {
if (func->type_feedback) {
  int off = (int)(ip - func->code);
  uint8_t old = func->type_feedback[off];
  uint8_t neu = old | sv_tfb_classify(v);
  if (neu != old) { func->type_feedback[off] = neu; func->tfb_version++; }
}}

static inline void sv_tfb_ensure(sv_func_t *fn) {
  if (!fn->type_feedback && fn->code_len > 0)
    fn->type_feedback = calloc((size_t)fn->code_len, 1);
  if (!fn->local_type_feedback && fn->max_locals > 0)
    fn->local_type_feedback = calloc((size_t)fn->max_locals, 1);
}

static inline void sv_tfb_record_call_target(sv_func_t *func, int bc_off, sv_func_t *callee) {
  if (!callee) return;
  sv_call_target_fb_t *fb = func->call_target_fb;
  int count = func->call_target_fb_count;
  for (int i = 0; i < count; i++) {
    if (fb[i].bc_off != (uint16_t)bc_off) continue;
    if (fb[i].disabled) return;
    if (fb[i].target == callee) return;
    if (fb[i].target == NULL) { fb[i].target = callee; return; }
    fb[i].miss_count++;
    if (fb[i].miss_count >= SV_CALL_FB_MISS_DISABLE) {
      fb[i].disabled = 1;
      fb[i].target = NULL;
    } else {
      fb[i].target = callee;
    }
    func->tfb_version++;
    return;
  }
  if (count >= SV_CALL_FB_MAX_SLOTS) return;
  if (!fb) {
    fb = calloc(SV_CALL_FB_MAX_SLOTS, sizeof(sv_call_target_fb_t));
    if (!fb) return;
    func->call_target_fb = fb;
  }
  fb[count].bc_off = (uint16_t)bc_off;
  fb[count].target = callee;
  fb[count].miss_count = 0;
  fb[count].disabled = 0;
  func->call_target_fb_count = (uint8_t)(count + 1);
}

static inline sv_func_t *sv_tfb_get_call_target(sv_func_t *func, int bc_off) {
  sv_call_target_fb_t *fb = func->call_target_fb;
  int count = func->call_target_fb_count;
  for (int i = 0; i < count; i++) {
    if (fb[i].bc_off == (uint16_t)bc_off && !fb[i].disabled)
      return fb[i].target;
  }
  return NULL;
}

static inline void sv_tfb_record_local(sv_func_t *func, int idx, ant_value_t v) {
  if (func->local_type_feedback && idx >= 0 && idx < func->max_locals) {
    uint8_t old = func->local_type_feedback[idx];
    uint8_t neu = old | sv_tfb_classify(v);
    if (neu != old) { func->local_type_feedback[idx] = neu; func->tfb_version++; }
  }
}

static inline uint8_t sv_tfb_clamp_inobj_limit(uint32_t limit) {
  return (limit > ANT_INOBJ_MAX_SLOTS) ? (uint8_t)ANT_INOBJ_MAX_SLOTS : (uint8_t)limit;
}

static inline uint8_t sv_tfb_infer_inobj_limit(const sv_func_t *func, uint64_t samples) {
  if (!func || samples == 0) return (uint8_t)ANT_INOBJ_MAX_SLOTS;

  uint64_t target = (
    (samples * SV_TFB_INOBJ_P90_NUMERATOR)
    + (SV_TFB_INOBJ_P90_DENOMINATOR - 1)
  ) / SV_TFB_INOBJ_P90_DENOMINATOR;
  if (target == 0) target = 1;

  uint64_t seen = 0;
  for (uint32_t i = 0; i < SV_TFB_CTOR_PROP_BINS; i++) {
    seen += func->ctor_prop_hist[i];
    if (seen < target) continue;

    if (i >= SV_TFB_CTOR_PROP_OVERFLOW_FROM) return (uint8_t)ANT_INOBJ_MAX_SLOTS;
    return sv_tfb_clamp_inobj_limit(i);
  }

  return (uint8_t)ANT_INOBJ_MAX_SLOTS;
}

static inline void sv_tfb_record_ctor_prop_count(ant_value_t ctor_func, ant_value_t instance) {
  if (vtype(ctor_func) != T_FUNC) return;
  if (!is_object_type(instance)) return;
  sv_closure_t *closure = js_func_closure(ctor_func);
  if (!closure || !closure->func) return;
  ant_object_t *obj = js_obj_ptr(js_as_obj(instance));
  if (!obj) return;

  sv_func_t *func = closure->func;
  uint32_t count = obj->prop_count;
  uint32_t bin = (count < SV_TFB_CTOR_PROP_OVERFLOW_FROM)
    ? count
    : SV_TFB_CTOR_PROP_OVERFLOW_FROM;
  func->ctor_prop_hist[bin]++;
  uint64_t samples = ++func->ctor_prop_samples;
  if (!func->ctor_inobj_frozen && samples >= SV_TFB_INOBJ_SLACK_ALLOCATIONS) {
    func->ctor_inobj_limit = sv_tfb_infer_inobj_limit(func, samples);
    func->ctor_inobj_frozen = 1;
  }
}

static inline uint8_t sv_tfb_ctor_inobj_limit(ant_value_t ctor_func) {
  if (vtype(ctor_func) != T_FUNC) return (uint8_t)ANT_INOBJ_MAX_SLOTS;
  sv_closure_t *closure = js_func_closure(ctor_func);
  if (!closure || !closure->func) return (uint8_t)ANT_INOBJ_MAX_SLOTS;

  sv_func_t *func = closure->func;
  if (!func->ctor_inobj_frozen) return (uint8_t)ANT_INOBJ_MAX_SLOTS;
  return sv_tfb_clamp_inobj_limit(func->ctor_inobj_limit);
}

static inline bool sv_tfb_ctor_inobj_limit_frozen(ant_value_t ctor_func) {
  if (vtype(ctor_func) != T_FUNC) return false;
  sv_closure_t *closure = js_func_closure(ctor_func);
  if (!closure || !closure->func) return false;
  return closure->func->ctor_inobj_frozen != 0;
}

static inline uint32_t sv_tfb_ctor_inobj_slack_remaining(ant_value_t ctor_func) {
  if (vtype(ctor_func) != T_FUNC) return SV_TFB_INOBJ_SLACK_ALLOCATIONS;
  sv_closure_t *closure = js_func_closure(ctor_func);
  if (!closure || !closure->func) return SV_TFB_INOBJ_SLACK_ALLOCATIONS;
  sv_func_t *func = closure->func;
  if (func->ctor_inobj_frozen || func->ctor_prop_samples >= SV_TFB_INOBJ_SLACK_ALLOCATIONS) return 0;
  return (uint32_t)(SV_TFB_INOBJ_SLACK_ALLOCATIONS - func->ctor_prop_samples);
}
#endif

#ifndef ANT_JIT
static inline void sv_tfb_record_ctor_prop_count(ant_value_t ctor_func, ant_value_t instance) {
  (void)ctor_func;
  (void)instance;
}

static inline uint8_t sv_tfb_ctor_inobj_limit(ant_value_t ctor_func) {
  (void)ctor_func;
  return (uint8_t)ANT_INOBJ_MAX_SLOTS;
}

static inline bool sv_tfb_ctor_inobj_limit_frozen(ant_value_t ctor_func) {
  (void)ctor_func;
  return false;
}

static inline uint32_t sv_tfb_ctor_inobj_slack_remaining(ant_value_t ctor_func) {
  (void)ctor_func;
  return 0;
}
#endif

static inline ant_value_t sv_call_resolve_closure(
  sv_vm_t *vm, ant_t *js, sv_closure_t *closure,
  ant_value_t callee_func, sv_call_ctx_t *ctx, ant_value_t *out_this
) {
  if (closure->func->is_generator)
    return sv_call_generator_closure(vm, js, closure, callee_func, ctx);
  if (closure->func->is_async)
    return sv_call_async_closure(vm, js, closure, callee_func, ctx);
#ifdef ANT_JIT
  if (!closure->func->is_generator) {
    sv_func_t *fn = closure->func;
    if (fn->jit_code) {
      sv_jit_enter(js);
      ant_value_t result = ((sv_jit_func_t)fn->jit_code)(
        vm, ctx->this_val, js->new_target,
        ctx->super_val, ctx->args, ctx->argc, closure
      );
      sv_jit_leave(js);
      if (sv_is_jit_bailout(result)) {
        sv_jit_on_bailout(fn);
      } else { sv_call_cleanup(js, ctx); return result; }
    }
    {
      uint32_t cc = ++fn->call_count;
      if (__builtin_expect(cc == SV_TFB_ALLOC_THRESHOLD, 0))
        sv_tfb_ensure(fn);
      if (cc > SV_JIT_THRESHOLD) {
        ant_value_t result = sv_jit_try_compile_and_call(vm, js, closure, callee_func, ctx, out_this);
        if (result != SV_JIT_RETRY_INTERP) return result;
      }
    }
  }
#endif
  return sv_call_closure(vm, js, closure, callee_func, ctx, out_this);
}

#endif
