#ifndef SILVER_ENGINE_H
#define SILVER_ENGINE_H

#include "silver/vm.h"
#include "internal.h"
#include "errors.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

bool sv_lookup_srcpos(sv_func_t *func, int bc_offset, uint32_t *line, uint32_t *col);
bool sv_lookup_srcspan(sv_func_t *func, int bc_offset, uint32_t *src_off, uint32_t *src_end);

struct sv_func {
  uint8_t *code;
  int code_len;
  
  ant_value_t *constants;
  int const_count;
  
  sv_atom_t *atoms;
  int atom_count;
  
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
  bool is_generator;
  bool is_method;
  bool is_tla;
  uint32_t gc_epoch;
  
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
  uint32_t tfb_version;
  uint32_t jit_compiled_tfb_ver;
  uint8_t *type_feedback;
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

typedef struct 
  sv_upvalue sv_upvalue_t;

typedef struct {
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
  uint32_t gc_epoch;
};

#define SV_CALL_HAS_BOUND_ARGS   (1u << 0)
#define SV_CALL_HAS_SUPER        (1u << 1)
#define SV_CALL_IS_ARROW         (1u << 2)
#define SV_CALL_IS_DEFAULT_CTOR  (1u << 3)

typedef struct sv_closure {
  sv_func_t *func;
  sv_upvalue_t **upvalues;
  ant_value_t bound_this;
  ant_value_t *bound_argv;
  int bound_argc;
  ant_value_t bound_args;
  ant_value_t super_val;
  ant_value_t func_obj;
  uint32_t gc_epoch;
  uint8_t call_flags;
} sv_closure_t;

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

static inline void sv_frame_set_arg_value(sv_frame_t *frame, uint16_t idx, ant_value_t val) {
  int arg_slots = sv_frame_arg_slots(frame);
  if (!frame || !frame->bp || (int)idx >= arg_slots) return;
  frame->bp[idx] = val;
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

static inline ant_value_t sv_vm_call(
  sv_vm_t *vm, ant_t *js, ant_value_t func,
  ant_value_t this_val, ant_value_t *args, int argc,
  ant_value_t *out_this, bool is_construct_call
);

typedef struct {
  ant_value_t  this_val;
  ant_value_t  super_val;
  ant_value_t *args;
  int      argc;
  ant_value_t *alloc;
} sv_call_ctx_t;

static inline ant_value_t sv_call_cfunc(
  ant_t *js, ant_value_t func,
  ant_value_t this_val, ant_value_t *args, int argc
) {
  js->this_val = this_val;
  return js_as_cfunc(func)(js, args, argc);
}

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

static inline ant_value_t sv_call_resolve_bound(ant_t *js, sv_closure_t *closure, sv_call_ctx_t *ctx) {
  uint8_t flags = closure->call_flags;

  if (flags & SV_CALL_IS_ARROW) ctx->this_val = closure->bound_this;
  else if (vtype(closure->bound_this) != T_UNDEF) ctx->this_val = closure->bound_this;

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
) {
  if (vtype(js->new_target) == T_UNDEF) {
    sv_call_cleanup(js, ctx);
    return js_mkerr_typed(
      js, JS_ERR_TYPE, 
      "Class constructor cannot be invoked without 'new'"
    );
  }

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

#define SV_JIT_THRESHOLD       100
#define SV_JIT_RECOMPILE_DELAY 50
#define SV_TFB_ALLOC_THRESHOLD 2
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

typedef ant_value_t (*sv_jit_func_t)
  (sv_vm_t *, ant_value_t, ant_value_t *, int, sv_closure_t *);

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
}
#endif

static inline ant_value_t sv_call_resolve_closure(
  sv_vm_t *vm, ant_t *js, sv_closure_t *closure,
  ant_value_t callee_func, sv_call_ctx_t *ctx, ant_value_t *out_this
) {
  if (closure->func->is_async)
    return sv_call_async_closure(vm, js, closure, callee_func, ctx);
#ifdef ANT_JIT
  if (!closure->func->is_generator) {
    sv_func_t *fn = closure->func;
    if (fn->jit_code) {
      sv_jit_enter(js);
      ant_value_t result = ((sv_jit_func_t)fn->jit_code)(
        vm, ctx->this_val, ctx->args, ctx->argc, closure
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

static inline ant_value_t sv_vm_call(
  sv_vm_t *vm, ant_t *js, ant_value_t func,
  ant_value_t this_val, ant_value_t *args, int argc,
  ant_value_t *out_this, bool is_construct_call
) {
  if (!is_construct_call) js->new_target = js_mkundef();
  if (out_this) *out_this = this_val;

  if (is_construct_call && vtype(func) == T_OBJ && is_proxy(js, func))
    return js_proxy_construct(js, func, args, argc, sv_vm_get_new_target(vm, js));
  if (is_construct_call && !js_is_constructor(js, func))
    return js_mkerr_typed(js, JS_ERR_TYPE, "not a constructor");

  if (!is_construct_call && vtype(func) == T_OBJ && is_proxy(js, func))
    return js_proxy_apply(js, func, this_val, args, argc);

  if (vtype(func) == T_CFUNC) {
    ant_value_t cfunc_this = sv_is_nullish_this(this_val) ? js->global : this_val;
    return sv_call_cfunc(js, func, cfunc_this, args, argc);
  }

  if (vtype(func) != T_FUNC)
    return sv_call_native(js, func, this_val, args, argc);

  sv_closure_t *closure = js_func_closure(func);

  sv_call_ctx_t ctx = {
    .this_val = this_val, .super_val = js_mkundef(),
    .args = args, .argc = argc, .alloc = NULL,
  };

  ant_value_t err = sv_call_resolve_bound(js, closure, &ctx);
  if (is_err(err)) return err;

  if (is_construct_call) ctx.this_val = this_val;
  if (out_this) *out_this = ctx.this_val;

  if (closure->call_flags & SV_CALL_IS_DEFAULT_CTOR)
    return sv_call_default_ctor(vm, js, closure, &ctx, out_this);

  if (closure->func != NULL)
    return sv_call_resolve_closure(vm, js, closure, func, &ctx, out_this);

  ant_value_t result = sv_call_native(js, func, ctx.this_val, ctx.args, ctx.argc);
  sv_call_cleanup(js, &ctx);
  
  return result;
}

#endif
