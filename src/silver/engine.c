#include "silver/engine.h"
#include "silver/swarm.h"

#include <stdlib.h>
#include "gc.h"
#include "errors.h"

#include "ops/literals.h"
#include "ops/stack.h"
#include "ops/locals.h"
#include "ops/upvalues.h"
#include "ops/globals.h"
#include "ops/property.h"
#include "ops/optional.h"
#include "ops/private.h"
#include "ops/super.h"
#include "ops/arithmetic.h"
#include "ops/comparison.h"
#include "ops/bitwise.h"
#include "ops/unary.h"
#include "ops/control.h"
#include "ops/calls.h"
#include "ops/returns.h"
#include "ops/exceptions.h"
#include "ops/async.h"
#include "ops/iteration.h"
#include "ops/objects.h"
#include "ops/coercion.h"

sv_vm_t *sv_vm_create(ant_t *js, sv_vm_kind_t kind) {
  int stack_size, max_frames;
  sv_vm_limits(kind, &stack_size, &max_frames);

  sv_vm_t *vm = calloc(1, sizeof(*vm));
  if (!vm) return NULL;
  
  vm->js = js;
  vm->stack_size = stack_size;
  vm->stack = calloc((size_t)stack_size, sizeof(jsval_t));
  vm->max_frames = max_frames;
  vm->frames = calloc((size_t)max_frames, sizeof(sv_frame_t));
  if (!vm->stack || !vm->frames) { sv_vm_destroy(vm); return NULL; }
  
  return vm;
}

void sv_vm_destroy(sv_vm_t *vm) {
  if (!vm) return;
  free(vm->stack);
  free(vm->frames);
  free(vm);
}

static bool sv_vm_grow_frames(sv_vm_t *vm) {
  int new_max = vm->max_frames * 2;
  if (new_max > SV_FRAMES_HARD_MAX) new_max = SV_FRAMES_HARD_MAX;
  if (new_max <= vm->max_frames) return false;
  
  sv_frame_t *nf = realloc(vm->frames, (size_t)new_max * sizeof(sv_frame_t));
  if (!nf) return false;
  vm->frames = nf;
  vm->max_frames = new_max;
  
  return true;
}

static bool sv_vm_grow_stack(sv_vm_t *vm) {
  int new_size = vm->stack_size * 2;
  if (new_size > SV_STACK_HARD_MAX) new_size = SV_STACK_HARD_MAX;
  if (new_size <= vm->stack_size) return false;
  
  jsval_t *old = vm->stack;
  jsval_t *ns = realloc(vm->stack, (size_t)new_size * sizeof(jsval_t));
  if (!ns) return false;
  
  ptrdiff_t delta = ns - old;
  vm->stack = ns;
  vm->stack_size = new_size;
  
  if (delta != 0) {
    for (int i = 0; i <= vm->fp; i++) {
      if (vm->frames[i].bp) vm->frames[i].bp += delta;
      if (vm->frames[i].lp) vm->frames[i].lp += delta;
    }
    for (sv_upvalue_t *uv = vm->open_upvalues; uv; uv = uv->next) {
      if (uv->location != &uv->closed)
        uv->location += delta;
    }
  }
  
  return true;
}

bool sv_lookup_srcpos(sv_func_t *func, int bc_offset, uint32_t *line, uint32_t *col) {
  if (!func || !func->srcpos || func->srcpos_count <= 0) return false;
  int best = -1;
  for (int i = 0; i < func->srcpos_count; i++) {
    if ((int)func->srcpos[i].bc_offset <= bc_offset) best = i;
    else break;
  }
  if (best < 0) return false;
  if (line) *line = func->srcpos[best].line;
  if (col) *col = func->srcpos[best].col;
  return true;
}

bool sv_lookup_srcspan(sv_func_t *func, int bc_offset, uint32_t *src_off, uint32_t *src_end) {
  if (!func || !func->srcpos || func->srcpos_count <= 0) return false;
  int best = -1;
  for (int i = 0; i < func->srcpos_count; i++) {
    if ((int)func->srcpos[i].bc_offset <= bc_offset) best = i;
    else break;
  }
  
  if (best < 0) return false;
  uint32_t off = func->srcpos[best].src_off;
  uint32_t end = func->srcpos[best].src_end;
  
  if (end < off) end = off;
  if (src_off) *src_off = off;
  if (src_end) *src_end = end;
  
  return true;
}

static jsoff_t sv_srcpos_to_offset_local(const char *code, jsoff_t clen, uint32_t line, uint32_t col) {
  jsoff_t off = 0;
  uint32_t cur = 1;
  
  while (off < clen && cur < line) {
    if (code[off] == '\n') cur++;
    off++;
  }
  
  if (col > 0) off += col - 1;
  if (off > clen) off = clen;
  return off;
}

void js_set_error_site_from_bc(ant_t *js, sv_func_t *func, int bc_offset, const char *filename) {
  if (!js || !func || !func->source || func->source_len <= 0) return;

  uint32_t src_off = 0, src_end = 0;
  if (sv_lookup_srcspan(func, bc_offset, &src_off, &src_end)) {
    jsoff_t off = (jsoff_t)src_off;
    jsoff_t span_len = (jsoff_t)(src_end > src_off ? (src_end - src_off) : 0);
    if (span_len <= 0 && off < (jsoff_t)func->source_len) span_len = 1;
    
    js_set_error_site(
      js, func->source, (jsoff_t)func->source_len,
      filename ? filename : func->filename, off, span_len
    );
    return;
  }

  uint32_t line = 0, col = 0;
  if (sv_lookup_srcpos(func, bc_offset, &line, &col)) {
    jsoff_t off = sv_srcpos_to_offset_local(func->source, (jsoff_t)func->source_len, line, col);
    js_set_error_site(
      js, func->source, (jsoff_t)func->source_len,
      filename ? filename : func->filename, off, 0
    );
  }
}

void js_set_error_site_from_vm_top(ant_t *js) {
  if (!js || !js->vm || js->vm->fp < 0) return;
  sv_frame_t *frame = &js->vm->frames[js->vm->fp];
  sv_func_t *func = frame->func;
  if (!func) return;
  
  int bc_off = 0;
  if (frame->ip && func->code) bc_off = (int)(frame->ip - func->code);
  js_set_error_site_from_bc(js, func, bc_off, func->filename);
}

void sv_vm_gc_roots(sv_vm_t *vm, void (*op_val)(void *, jsval_t *), void *ctx) {
  if (!vm) return;

  for (int i = 0; i < vm->sp; i++)
    op_val(ctx, &vm->stack[i]);

  for (int i = 0; i <= vm->fp; i++) {
    op_val(ctx, &vm->frames[i].this);
    op_val(ctx, &vm->frames[i].callee);
    op_val(ctx, &vm->frames[i].new_target);
    op_val(ctx, &vm->frames[i].super_val);
    op_val(ctx, &vm->frames[i].with_obj);
  }
  
  for (sv_upvalue_t *uv = vm->open_upvalues; uv; uv = uv->next) {
    if (uv->location != &uv->closed) continue;
    if (uv->gc_epoch == gc_epoch_counter) continue;
    op_val(ctx, &uv->closed);
  }

  for (int i = 0; i <= vm->fp; i++) {
    sv_frame_t *frame = &vm->frames[i];
    for (int j = 0; j < frame->upvalue_count; j++) {
      sv_upvalue_t *uv = frame->upvalues ? frame->upvalues[j] : NULL;
      if (!uv) continue;
      if (uv->location != &uv->closed) continue;
      if (uv->gc_epoch == gc_epoch_counter) continue;
      op_val(ctx, &uv->closed);
    }
  }

  for (int i = 0; i <= vm->fp; i++) {
    if (vm->frames[i].completion.kind != SV_COMPLETION_NONE)
      op_val(ctx, &vm->frames[i].completion.value);
  }
}

void sv_vm_visit_frame_funcs(sv_vm_t *vm, void (*visitor)(void *, sv_func_t *), void *ctx) {
  if (!vm) return;
  for (int i = 0; i <= vm->fp; i++) if (vm->frames[i].func) visitor(ctx, vm->frames[i].func);
}

jsval_t sv_call_async_closure_dispatch(
  sv_vm_t *vm, ant_t *js, sv_closure_t *closure,
  jsval_t callee_func, jsval_t super_val,
  jsval_t this_val, jsval_t *args, int argc
) {
  return sv_start_async_closure(vm, js, closure, callee_func, super_val, this_val, args, argc);
}

jsval_t sv_execute_entry_tla(ant_t *js, sv_func_t *func, jsval_t this_val) {
  return sv_start_tla(js, func, this_val);
}

void sv_vm_gc_roots_pending(void (*op_val)(void *, jsval_t *), void *ctx) {
  sv_vm_gc_roots_async(op_val, ctx);
}

static inline jsval_t sv_stage_frame_args(
  sv_vm_t *vm, ant_t *js, sv_func_t *func, jsval_t *args, int argc,
  jsval_t **out_bp, jsval_t **out_lp
) {
  int arg_slots = (argc > func->param_count) ? argc : func->param_count;
  int need = arg_slots + func->max_locals;
  if (vm->sp + need > vm->stack_size) {
    int args_idx = (
      args && args >= vm->stack && args < vm->stack + vm->stack_size)
      ? (int)(args - vm->stack) : -1;
    while (vm->sp + need > vm->stack_size) {
      if (!sv_vm_grow_stack(vm)) return js_mkerr(js, "stack overflow");
    }
    if (args_idx >= 0) args = &vm->stack[args_idx];
  }

  jsval_t *base = &vm->stack[vm->sp];
  *out_bp = base;
  *out_lp = base + arg_slots;

  if (argc > 0 && args)
    memmove(base, args, (size_t)argc * sizeof(jsval_t));
  for (int i = argc; i < arg_slots; i++)
    base[i] = js_mkundef();
  if (func->max_locals > 0)
    for (int i = 0; i < func->max_locals; i++)
      (*out_lp)[i] = js_mkundef();
  vm->sp += need;
  return js_mkundef();
}

static inline jsval_t sv_execute_entry_common(
  sv_vm_t *vm, sv_func_t *func, sv_upvalue_t **upvalues, int upvalue_count,
  jsval_t callee_func, jsval_t super_val,
  jsval_t this_val, jsval_t *args, int argc, jsval_t *out_this
) {
  if (!vm || !vm->js || !func) return mkval(T_ERR, 0);
  ant_t *js = vm->js;
  if (vm->fp + 1 >= vm->max_frames && !sv_vm_grow_frames(vm))
    return js_mkerr_typed(js, JS_ERR_RANGE | JS_ERR_NO_STACK, "Maximum AOT call stack size exceeded");

  int saved_fp = vm->fp;
  vm->fp = saved_fp + 1;
  vm->frames[vm->fp].upvalues = upvalues;
  vm->frames[vm->fp].upvalue_count = upvalue_count;
  vm->frames[vm->fp].callee = callee_func;

  jsval_t result = sv_execute_frame(vm, func, this_val, super_val, args, argc);
  if (out_this) *out_this = vm->frames[vm->fp].this;

  vm->fp = saved_fp;

  return result;
}

jsval_t sv_execute_entry(
  sv_vm_t *vm, sv_func_t *func, jsval_t this_val, jsval_t *args, int argc
) {
  return sv_execute_entry_common(vm, func, NULL, 0, js_mkundef(), js_mkundef(), this_val, args, argc, NULL);
}

jsval_t sv_execute_closure_entry(
  sv_vm_t *vm, sv_closure_t *closure, jsval_t callee_func, jsval_t super_val,
  jsval_t this_val, jsval_t *args, int argc, jsval_t *out_this
) {
  if (!closure || !closure->func) return mkval(T_ERR, 0);
  return sv_execute_entry_common(
    vm, closure->func, closure->upvalues, closure->func->upvalue_count, callee_func,
    super_val, this_val, args, argc, out_this
  );
}

jsval_t sv_execute_frame(sv_vm_t *vm, sv_func_t *func, jsval_t this, jsval_t super_val, jsval_t *args, int argc) {
  ant_t *js = vm->js;
  uint8_t *ip = func->code;
  int entry_fp = vm->fp;
  jsval_t vm_result = js_mkundef();

  // TODO: shorthand?
  sv_frame_t *frame = &vm->frames[vm->fp];
  frame->ip = ip;
  frame->func = func;
  frame->this = sv_normalize_this_for_frame(js, func, this);
  frame->new_target = js->new_target;
  frame->super_val = super_val;
  frame->prev_sp = vm->sp;
  frame->handler_base = vm->handler_depth;
  frame->handler_top = vm->handler_depth;
  frame->argc = argc;
  frame->completion.kind = SV_COMPLETION_NONE;
  frame->completion.value = js_mkundef();
  frame->with_obj = js_mkundef();
  
  jsval_t *entry_bp = NULL;
  jsval_t *entry_lp = NULL;
  jsval_t stage_err = sv_stage_frame_args(vm, js, func, args, argc, &entry_bp, &entry_lp);
  if (is_err(stage_err)) {
    vm_result = stage_err;
    goto sv_leave;
  }
  #ifdef ANT_JIT
  if (vm->jit_resume.active) {
    ip = func->code + vm->jit_resume.ip_offset;
    frame->ip = ip;
    int64_t rl = 
      vm->jit_resume.n_locals < func->max_locals
      ? vm->jit_resume.n_locals : func->max_locals;
    for (int64_t i = 0; i < rl; i++)
      entry_lp[i] = vm->jit_resume.locals[i];
  }
  #endif
  frame->bp = entry_bp;
  frame->lp = entry_lp;

  jsval_t *bp = frame->bp;
  jsval_t *lp = frame->lp;

  #ifdef ANT_JIT
  if (vm->jit_resume.active) {
    for (int64_t i = 0; i < vm->jit_resume.vstack_sp; i++)
      vm->stack[vm->sp++] = vm->jit_resume.vstack[i];

    int resume_off = (int)vm->jit_resume.ip_offset;
    uint8_t *scan = func->code;
    uint8_t *scan_end = func->code + resume_off;
    typedef struct { uint8_t *catch_ip; int saved_sp; } pending_h;
    
    pending_h h_stack[SV_TRY_MAX];
    int enclosing_count = 0;
    int total_depth = 0;
    bool is_enclosing[SV_TRY_MAX];
    
    while (scan < scan_end) {
      sv_op_t scan_op = (sv_op_t)*scan;
      int scan_sz = sv_op_size[scan_op];
      if (scan_sz == 0) break;
      if (scan_op == OP_TRY_PUSH && total_depth < SV_TRY_MAX) {
        int32_t off = sv_get_i32(scan + 1);
        int catch_off = (int)(scan - func->code) + scan_sz + off;
        is_enclosing[total_depth] = (catch_off > resume_off);
        if (is_enclosing[total_depth]) {
          h_stack[enclosing_count].catch_ip = func->code + catch_off;
          h_stack[enclosing_count].saved_sp = vm->sp;
          enclosing_count++;
        }
        total_depth++;
      } else if (scan_op == OP_TRY_POP && total_depth > 0) {
        total_depth--;
        if (is_enclosing[total_depth]) enclosing_count--;
      }
      scan += scan_sz;
    }
    
    for (int i = 0; i < enclosing_count; i++) {
      if (vm->handler_depth < SV_HANDLER_MAX) {
        sv_handler_t *h = &vm->handler_stack[vm->handler_depth++];
        h->kind = SV_HANDLER_TRY;
        h->ip = h_stack[i].catch_ip;
        h->saved_sp = h_stack[i].saved_sp;
        frame->handler_top = vm->handler_depth;
      }
    }

    vm->jit_resume.active = false;
  }
  #endif

  // TODO: generate with X-macro
  #define OP(name) [OP_##name] = &&L_##name
  static const void *dispatch[OP__COUNT] = {
    OP(INVALID),

    OP(CONST), OP(CONST_I8), OP(CONST8),
    OP(UNDEF), OP(NULL), OP(TRUE), OP(FALSE),
    OP(THIS), OP(GLOBAL), OP(OBJECT), OP(ARRAY), OP(REGEXP), OP(CLOSURE),

    OP(POP), OP(DUP), OP(DUP2), OP(SWAP),
    OP(ROT3L), OP(ROT3R), OP(NIP), OP(NIP2), OP(INSERT2), OP(INSERT3),
    OP(SWAP_UNDER), OP(ROT4_UNDER),

    OP(GET_LOCAL), OP(PUT_LOCAL), OP(SET_LOCAL),
    OP(GET_LOCAL8), OP(PUT_LOCAL8), OP(SET_LOCAL8),
    OP(SET_LOCAL_UNDEF), OP(GET_LOCAL_CHK), OP(PUT_LOCAL_CHK),

    OP(GET_ARG), OP(PUT_ARG), OP(SET_ARG), OP(REST),
    OP(GET_UPVAL), OP(PUT_UPVAL), OP(SET_UPVAL), OP(CLOSE_UPVAL),
    OP(GET_GLOBAL), OP(GET_GLOBAL_UNDEF), OP(PUT_GLOBAL),

    OP(GET_FIELD), OP(GET_FIELD2), OP(PUT_FIELD),
    OP(GET_ELEM), OP(GET_ELEM2), OP(PUT_ELEM),
    OP(DEFINE_FIELD), OP(GET_LENGTH),

    OP(GET_FIELD_OPT), OP(GET_ELEM_OPT),
    OP(GET_PRIVATE), OP(PUT_PRIVATE), OP(DEF_PRIVATE),
    OP(GET_SUPER), OP(GET_SUPER_VAL), OP(PUT_SUPER_VAL),

    OP(ADD), OP(SUB), OP(MUL), OP(DIV), OP(MOD), OP(EXP),
    OP(NEG), OP(UPLUS),
    OP(INC), OP(DEC), OP(POST_INC), OP(POST_DEC),
    OP(INC_LOCAL), OP(DEC_LOCAL), OP(ADD_LOCAL),

    OP(EQ), OP(NE), OP(SEQ), OP(SNE),
    OP(LT), OP(LE), OP(GT), OP(GE),
    OP(INSTANCEOF), OP(IN),
    OP(IS_NULLISH), OP(IS_UNDEF_OR_NULL),

    OP(BAND), OP(BOR), OP(BXOR), OP(BNOT),
    OP(SHL), OP(SHR), OP(USHR),
    OP(NOT), OP(TYPEOF), OP(VOID), OP(DELETE), OP(DELETE_VAR),

    OP(JMP), OP(JMP_FALSE), OP(JMP_TRUE),
    OP(JMP_FALSE_PEEK), OP(JMP_TRUE_PEEK), OP(JMP_NOT_NULLISH),
    OP(JMP8), OP(JMP_FALSE8), OP(JMP_TRUE8),

    OP(CALL), OP(CALL_METHOD),
    OP(TAIL_CALL), OP(TAIL_CALL_METHOD),
    OP(NEW), OP(APPLY), OP(EVAL),
    OP(RETURN), OP(RETURN_UNDEF), OP(RETURN_ASYNC),
    OP(CHECK_CTOR), OP(CHECK_CTOR_RET), OP(HALT),

    OP(THROW), OP(THROW_ERROR),
    OP(TRY_PUSH), OP(TRY_POP),
    OP(CATCH), OP(FINALLY), OP(FINALLY_RET), OP(NIP_CATCH),

    OP(FOR_IN), OP(FOR_OF), OP(FOR_AWAIT_OF),
    OP(ITER_NEXT), OP(ITER_GET_VALUE),
    OP(ITER_CLOSE), OP(ITER_CALL), OP(AWAIT_ITER_NEXT),

    OP(AWAIT), OP(YIELD), OP(YIELD_STAR), OP(INITIAL_YIELD),
    OP(SPREAD),

    OP(DEFINE_METHOD), OP(DEFINE_METHOD_COMP),
    OP(SET_NAME), OP(SET_NAME_COMP),
    OP(SET_PROTO), OP(SET_HOME_OBJ),
    OP(APPEND), OP(COPY_DATA_PROPS),

    OP(DEFINE_CLASS), OP(DEFINE_CLASS_COMP), OP(ADD_BRAND),
    OP(TO_OBJECT), OP(TO_PROPKEY), OP(IS_UNDEF), OP(IS_NULL),

    OP(IMPORT), OP(IMPORT_SYNC), OP(IMPORT_DEFAULT), OP(EXPORT), OP(EXPORT_ALL),
    OP(ENTER_WITH), OP(EXIT_WITH),

    OP(WITH_GET_VAR), OP(WITH_PUT_VAR), OP(WITH_DEL_VAR),
    OP(SPECIAL_OBJ), OP(EMPTY), OP(DEBUGGER), OP(NOP), OP(PUT_CONST),
    OP(LABEL), OP(LINE_NUM), OP(COL_NUM),
  };
  #undef OP
  
  jsval_t sv_err;
  #define VM_CHECK(expr) do {             \
    frame->ip = ip;                     \
    sv_err = (expr);                    \
    if (is_err(sv_err)) goto sv_throw;  \
  } while (0)
  jsval_t  tc_this = js_mkundef();

  #define DISPATCH()  goto *dispatch[*ip]
  #define NEXT(n)     do { ip += (n); DISPATCH(); } while (0)

  #ifdef ANT_JIT
  #define JIT_OSR_BACK_EDGE() do {                                            \
    if (!func->jit_compile_failed) {                                        \
      if (!func->type_feedback) sv_tfb_ensure(func);                        \
      if (++func->back_edge_count >= SV_JIT_OSR_THRESHOLD) {                \
      jsval_t osr_r = sv_jit_try_osr(                                       \
        vm, js, frame, func,                                                \
         (int)(ip - func->code));                                           \
      if (osr_r != SV_JIT_RETRY_INTERP) {                                   \
        if (is_err(osr_r)) { sv_err = osr_r; goto sv_throw; }               \
        vm->sp = frame->prev_sp;                                            \
        if (vm->fp <= entry_fp) {                                           \
          vm_result = osr_r;                                                \
          goto sv_leave;                                                    \
        }                                                                   \
        vm->fp--;                                                           \
        frame = &vm->frames[vm->fp];                                        \
        func = frame->func;                                                 \
        bp = frame->bp;                                                     \
        lp = frame->lp;                                                     \
        ip = frame->ip;                                                     \
        vm->stack[vm->sp++] = osr_r;                                        \
        DISPATCH();                                                         \
      }                                                                     \
      }                                                                     \
    }                                                                       \
  } while (0)
  #else
  #define JIT_OSR_BACK_EDGE() ((void)0)
  #endif
  DISPATCH();

  L_CONST:     { sv_op_const(vm, func, ip);   NEXT(5); }
  L_CONST_I8:  { sv_op_const_i8(vm, ip);      NEXT(2); }
  L_CONST8:    { sv_op_const8(vm, func, ip);  NEXT(2); }
  L_UNDEF:     { sv_op_undef(vm);             NEXT(1); }
  L_NULL:      { sv_op_null(vm);              NEXT(1); }
  L_TRUE:      { sv_op_true(vm);              NEXT(1); }
  L_FALSE:     { sv_op_false(vm);             NEXT(1); }
  L_THIS:      { sv_op_this(vm, frame);       NEXT(1); }
  L_GLOBAL:    { sv_op_global(vm, js);        NEXT(1); }
  L_OBJECT:    { sv_op_object(vm, js);        NEXT(1); }
  L_ARRAY:     { sv_op_array(vm, js, ip);     NEXT(3); }
  
  L_REGEXP:   { sv_op_regexp(vm, js);                    NEXT(1); }
  L_CLOSURE:  { sv_op_closure(vm, js, frame, func, ip);  NEXT(5); }

  L_POP:      { sv_op_pop(vm);      NEXT(1); }
  L_DUP:      { sv_op_dup(vm);      NEXT(1); }
  L_DUP2:     { sv_op_dup2(vm);     NEXT(1); }
  L_SWAP:     { sv_op_swap(vm);     NEXT(1); }
  L_ROT3L:    { sv_op_rot3l(vm);    NEXT(1); }
  L_ROT3R:    { sv_op_rot3r(vm);    NEXT(1); }
  L_NIP:      { sv_op_nip(vm);      NEXT(1); }
  L_NIP2:     { sv_op_nip2(vm);     NEXT(1); }
  L_INSERT2:  { sv_op_insert2(vm);  NEXT(1); }
  L_INSERT3:  { sv_op_insert3(vm);  NEXT(1); }
  
  L_SWAP_UNDER:  { sv_op_swap_under(vm);   NEXT(1); }
  L_ROT4_UNDER:  { sv_op_rot4_under(vm);   NEXT(1); }

  L_GET_LOCAL:        { sv_op_get_local(vm, lp, ip);    NEXT(3); }
  L_PUT_LOCAL:        { sv_op_put_local(vm, lp, ip);    NEXT(3); }
  L_SET_LOCAL:        { sv_op_set_local(vm, lp, ip);    NEXT(3); }
  L_GET_LOCAL8:       { sv_op_get_local8(vm, lp, ip);   NEXT(2); }
  L_PUT_LOCAL8:       { sv_op_put_local8(vm, lp, ip);   NEXT(2); }
  L_SET_LOCAL8:       { sv_op_set_local8(vm, lp, ip);   NEXT(2); }
  L_SET_LOCAL_UNDEF:  { sv_op_set_local_undef(lp, ip);  NEXT(3); }
  
  L_GET_LOCAL_CHK:  { VM_CHECK(sv_op_get_local_chk(vm, lp, js, func, ip));  NEXT(7); }
  L_PUT_LOCAL_CHK:  { VM_CHECK(sv_op_put_local_chk(vm, lp, js, func, ip));  NEXT(7); }

  L_GET_ARG:  { sv_op_get_arg(vm, frame, ip);   NEXT(3); }
  L_PUT_ARG:  { sv_op_put_arg(vm, frame, ip);   NEXT(3); }
  L_SET_ARG:  { sv_op_set_arg(vm, frame, ip);   NEXT(3); }
  L_REST:     { sv_op_rest(vm, frame, js, ip);  NEXT(3); }

  L_GET_UPVAL:    { VM_CHECK(sv_op_get_upval(vm, frame, js, ip));  NEXT(3); }
  L_PUT_UPVAL:    { sv_op_put_upval(vm, frame, ip);                NEXT(3); }
  L_SET_UPVAL:    { sv_op_set_upval(vm, frame, ip);                NEXT(3); }
  L_CLOSE_UPVAL:  { sv_op_close_upval(vm, frame, ip);              NEXT(3); }

  L_GET_GLOBAL:        { VM_CHECK(sv_op_get_global(vm, js, func, ip));         NEXT(5); }
  L_GET_GLOBAL_UNDEF:  { sv_op_get_global_undef(vm, js, func, ip);             NEXT(5); }
  L_PUT_GLOBAL:        { VM_CHECK(sv_op_put_global(vm, js, frame, func, ip));  NEXT(5); }

  L_GET_FIELD:     { VM_CHECK(sv_op_get_field(vm, js, func, ip));   NEXT(5); }
  L_GET_FIELD2:    { VM_CHECK(sv_op_get_field2(vm, js, func, ip));  NEXT(5); }
  L_PUT_FIELD:     { VM_CHECK(sv_op_put_field(vm, js, func, ip));   NEXT(5); }
  L_GET_ELEM:      { VM_CHECK(sv_op_get_elem(vm, js, func, ip));    NEXT(1); }
  L_GET_ELEM2:     { VM_CHECK(sv_op_get_elem2(vm, js, func, ip));   NEXT(1); }
  L_PUT_ELEM:      { VM_CHECK(sv_op_put_elem(vm, js));              NEXT(1); }
  L_DEFINE_FIELD:  { sv_op_define_field(vm, js, func, ip);          NEXT(5); }
  L_GET_LENGTH:    { VM_CHECK(sv_op_get_length(vm, js));            NEXT(1); }

  L_GET_FIELD_OPT:  { VM_CHECK(sv_op_get_field_opt(vm, js, func, ip));  NEXT(5); }
  L_GET_ELEM_OPT:   { VM_CHECK(sv_op_get_elem_opt(vm, js, func, ip));   NEXT(1); }

  L_GET_PRIVATE:  { sv_op_get_private(vm, js);  NEXT(1); }
  L_PUT_PRIVATE:  { sv_op_put_private(vm, js);  NEXT(1); }
  L_DEF_PRIVATE:  { sv_op_def_private(vm, js);  NEXT(1); }

  L_GET_SUPER:      { sv_op_get_super(vm, js);      NEXT(1); }
  L_GET_SUPER_VAL:  { sv_op_get_super_val(vm, js);  NEXT(1); }
  L_PUT_SUPER_VAL:  { sv_op_put_super_val(vm, js);  NEXT(1); }

  L_ADD: {
    jsval_t r = vm->stack[vm->sp - 1], l = vm->stack[vm->sp - 2];
    sv_tfb_record2(func, ip, l, r);
    if (__builtin_expect(vtype(l) == T_NUM && vtype(r) == T_NUM, 1)) {
      vm->sp--; vm->stack[vm->sp - 1] = tov(tod(l) + tod(r)); NEXT(1);
    }
    VM_CHECK(sv_op_add(vm, js)); NEXT(1);
  }
  
  L_SUB: {
    jsval_t r = vm->stack[vm->sp - 1], l = vm->stack[vm->sp - 2];
    sv_tfb_record2(func, ip, l, r);
    if (__builtin_expect(vtype(l) == T_NUM && vtype(r) == T_NUM, 1)) {
      vm->sp--; vm->stack[vm->sp - 1] = tov(tod(l) - tod(r)); NEXT(1);
    }
    VM_CHECK(sv_op_sub(vm, js)); NEXT(1);
  }
  
  L_MUL:        { sv_tfb_record2(func, ip, vm->stack[vm->sp-2], vm->stack[vm->sp-1]); VM_CHECK(sv_op_mul(vm, js));                NEXT(1); }
  L_DIV:        { sv_tfb_record2(func, ip, vm->stack[vm->sp-2], vm->stack[vm->sp-1]); VM_CHECK(sv_op_div(vm, js));                NEXT(1); }
  L_MOD:        { sv_tfb_record2(func, ip, vm->stack[vm->sp-2], vm->stack[vm->sp-1]); VM_CHECK(sv_op_mod(vm, js));                NEXT(1); }
  L_NEG:        { sv_tfb_record1(func, ip, vm->stack[vm->sp-1]); VM_CHECK(sv_op_neg(vm, js));                                     NEXT(1); }
  L_ADD_LOCAL:  { sv_tfb_record2(func, ip, lp[sv_get_u8(ip+1)], vm->stack[vm->sp-1]); VM_CHECK(sv_op_add_local(vm, lp, js, ip));  NEXT(2); }

  L_EXP:        { VM_CHECK(sv_op_exp(vm, js));    NEXT(1); }
  L_UPLUS:      { VM_CHECK(sv_op_uplus(vm, js));  NEXT(1); }
  L_INC:        { sv_op_inc(vm);                  NEXT(1); }
  L_DEC:        { sv_op_dec(vm);                  NEXT(1); }
  L_POST_INC:   { sv_op_post_inc(vm);             NEXT(1); }
  L_POST_DEC:   { sv_op_post_dec(vm);             NEXT(1); }
  L_INC_LOCAL:  { sv_op_inc_local(vm, lp, ip);    NEXT(2); }
  L_DEC_LOCAL:  { sv_op_dec_local(vm, lp, ip);    NEXT(2); }

  L_EQ:   { sv_op_eq(vm, js);   NEXT(1); }
  L_NE:   { sv_op_ne(vm, js);   NEXT(1); }
  L_SEQ:  { sv_op_seq(vm, js);  NEXT(1); }
  L_SNE:  { sv_op_sne(vm, js);  NEXT(1); }
  
  L_LT: {
    jsval_t r = vm->stack[vm->sp - 1], l = vm->stack[vm->sp - 2];
    sv_tfb_record2(func, ip, l, r);
    if (__builtin_expect(vtype(l) == T_NUM && vtype(r) == T_NUM, 1)) {
      vm->sp--; vm->stack[vm->sp - 1] = mkval(T_BOOL, tod(l) < tod(r)); NEXT(1);
    }
    VM_CHECK(sv_op_lt(vm, js)); NEXT(1);
  }
  
  L_LE:  { sv_tfb_record2(func, ip, vm->stack[vm->sp-2], vm->stack[vm->sp-1]); VM_CHECK(sv_op_le(vm, js));  NEXT(1); }
  L_GT:  { sv_tfb_record2(func, ip, vm->stack[vm->sp-2], vm->stack[vm->sp-1]); VM_CHECK(sv_op_gt(vm, js));  NEXT(1); }
  L_GE:  { sv_tfb_record2(func, ip, vm->stack[vm->sp-2], vm->stack[vm->sp-1]); VM_CHECK(sv_op_ge(vm, js));  NEXT(1); }
  
  L_INSTANCEOF:        { VM_CHECK(sv_op_instanceof(vm, js));  NEXT(1); }
  L_IN:                { VM_CHECK(sv_op_in(vm, js));          NEXT(1); }
  L_IS_NULLISH:        { sv_op_is_nullish(vm);                NEXT(1); }
  L_IS_UNDEF_OR_NULL:  { sv_op_is_undef_or_null(vm);          NEXT(1); }

  L_BAND:  { VM_CHECK(sv_op_band(vm, js));  NEXT(1); }
  L_BOR:   { VM_CHECK(sv_op_bor(vm, js));   NEXT(1); }
  L_BXOR:  { VM_CHECK(sv_op_bxor(vm, js));  NEXT(1); }
  L_BNOT:  { sv_op_bnot(vm, js);            NEXT(1); }
  L_SHL:   { VM_CHECK(sv_op_shl(vm, js));   NEXT(1); }
  L_SHR:   { VM_CHECK(sv_op_shr(vm, js));   NEXT(1); }
  L_USHR:  { VM_CHECK(sv_op_ushr(vm, js));  NEXT(1); }

  L_NOT:         { sv_op_not(vm, js);                   NEXT(1); }
  L_TYPEOF:      { sv_op_typeof(vm, js);                NEXT(1); }
  L_VOID:        { sv_op_void(vm);                      NEXT(1); }
  L_DELETE:      { VM_CHECK(sv_op_delete(vm, js));      NEXT(1); }
  L_DELETE_VAR:  { sv_op_delete_var(vm, js, func, ip);  NEXT(5); }

  L_JMP: { 
    uint8_t *prev = ip; ip = sv_op_jmp(ip);
    if (ip <= prev) {
      if (js->needs_gc) js_gc_maybe(js);
      JIT_OSR_BACK_EDGE();
    }
    DISPATCH(); 
  }
  
  L_JMP_FALSE: {
    uint8_t *prev = ip;
    jsval_t v = vm->stack[--vm->sp];
    if (__builtin_expect(vtype(v) == T_BOOL, 1)) {
      ip = vdata(v) 
        ? ip + sv_op_size[OP_JMP_FALSE]
        : ip + sv_op_size[OP_JMP_FALSE] + sv_get_i32(ip + 1);
    } else {
      ip = js_truthy(js, v) 
        ? ip + sv_op_size[OP_JMP_FALSE]
        : ip + sv_op_size[OP_JMP_FALSE] + sv_get_i32(ip + 1);
    }
    if (ip <= prev) {
      if (js->needs_gc) js_gc_maybe(js);
      JIT_OSR_BACK_EDGE();
    }
    DISPATCH();
  }
  
  L_JMP_TRUE: {
    uint8_t *prev = ip;
    jsval_t v = vm->stack[--vm->sp];
    if (__builtin_expect(vtype(v) == T_BOOL, 1)) {
      ip = vdata(v) 
        ? ip + sv_op_size[OP_JMP_TRUE] + sv_get_i32(ip + 1)
        : ip + sv_op_size[OP_JMP_TRUE];
    } else {
      ip = js_truthy(js, v) 
        ? ip + sv_op_size[OP_JMP_TRUE] + sv_get_i32(ip + 1)
        : ip + sv_op_size[OP_JMP_TRUE];
    }
    if (ip <= prev) {
      if (js->needs_gc) js_gc_maybe(js);
      JIT_OSR_BACK_EDGE();
    }
    DISPATCH();
  }
  
  L_JMP_FALSE_PEEK: { 
    uint8_t *prev = ip; ip = sv_op_jmp_false_peek(vm, js, ip);
    if (ip <= prev) {
      if (js->needs_gc) js_gc_maybe(js);
      JIT_OSR_BACK_EDGE();
    }
    DISPATCH();
  }
  
  L_JMP_TRUE_PEEK: { 
    uint8_t *prev = ip; ip = sv_op_jmp_true_peek(vm, js, ip);
    if (ip <= prev) {
      if (js->needs_gc) js_gc_maybe(js);
      JIT_OSR_BACK_EDGE();
    }
    DISPATCH();
  }
  
  L_JMP_NOT_NULLISH: { 
    uint8_t *prev = ip; ip = sv_op_jmp_not_nullish(vm, ip);
    if (ip <= prev) {
      if (js->needs_gc) js_gc_maybe(js);
      JIT_OSR_BACK_EDGE();
    }
    DISPATCH();
  }
   
  L_JMP8: {
    uint8_t *prev = ip; ip = sv_op_jmp8(ip);
    if (ip <= prev) {
      if (js->needs_gc) js_gc_maybe(js);
      JIT_OSR_BACK_EDGE();
    }
    DISPATCH();
  }
  
  L_JMP_FALSE8: { 
    uint8_t *prev = ip; ip = sv_op_jmp_false8(vm, js, ip);
    if (ip <= prev) {
      if (js->needs_gc) js_gc_maybe(js);
      JIT_OSR_BACK_EDGE();
    }
    DISPATCH();
  }
  
  L_JMP_TRUE8:  { 
    uint8_t *prev = ip; ip = sv_op_jmp_true8(vm, js, ip);
    if (ip <= prev) {
      if (js->needs_gc) js_gc_maybe(js);
      JIT_OSR_BACK_EDGE();
    }
    DISPATCH();
  }

  // TODO: make the methods below DRY
  L_CALL: {
    uint16_t call_argc = sv_get_u16(ip + 1);
    jsval_t *call_args = &vm->stack[vm->sp - call_argc];
    jsval_t call_func = vm->stack[vm->sp - call_argc - 1];
    
    bool is_super_call =
      (vtype(frame->super_val) != T_UNDEF && call_func == frame->super_val);
    jsval_t call_this = is_super_call ? frame->this : js_mkundef();

    if (!is_super_call && vtype(frame->new_target) == T_UNDEF && vtype(call_func) == T_FUNC) {
      sv_closure_t *closure = js_func_closure(call_func);
      if (closure->func != NULL) {
        if (closure->call_flags & (SV_CALL_HAS_BOUND_ARGS | SV_CALL_HAS_SUPER))
          goto call_fallback;
        if (closure->func->is_async) goto call_fallback;
        #ifdef ANT_JIT
        {
          sv_func_t *callee = closure->func;
          if (callee->jit_code) {
            jsval_t jit_this = (
              callee->is_arrow || vtype(closure->bound_this) != T_UNDEF)
              ? closure->bound_this : js_mkundef();
            frame->ip = ip + 3;
            jsval_t jit_result = ((sv_jit_func_t)callee->jit_code)(
              vm, jit_this, call_args, (int)call_argc, closure);
            if (sv_is_jit_bailout(jit_result)) {
              sv_jit_on_bailout(callee);
              goto call_fallback;
            }
            vm->sp -= call_argc + 1;
            if (is_err(jit_result)) { sv_err = jit_result; goto sv_throw; }
            vm->stack[vm->sp++] = jit_result;
            ip = frame->ip;
            DISPATCH();
          }
          if (!callee->is_generator) {
            uint32_t cc = ++callee->call_count;
            if (__builtin_expect(cc == SV_TFB_ALLOC_THRESHOLD, 0))
              sv_tfb_ensure(callee);
            if (cc > SV_JIT_THRESHOLD) {
            sv_jit_func_t jit_fn = sv_jit_compile(js, callee, closure);
            if (jit_fn) {
              callee->jit_code = (void *)jit_fn;
              jsval_t jit_this = (
                callee->is_arrow || vtype(closure->bound_this) != T_UNDEF)
                 ? closure->bound_this : js_mkundef();
              frame->ip = ip + 3;
              jsval_t jit_result = jit_fn(vm, jit_this, call_args, (int)call_argc, closure);
              if (sv_is_jit_bailout(jit_result)) {
                sv_jit_on_bailout(callee);
                goto call_fallback;
              }
              vm->sp -= call_argc + 1;
              if (is_err(jit_result)) { sv_err = jit_result; goto sv_throw; }
              vm->stack[vm->sp++] = jit_result;
              ip = frame->ip;
              DISPATCH();
            } else {
              callee->call_count = 0;
              callee->back_edge_count = 0;
            }}
          }
        }
        #endif
        if (vm->fp + 1 >= vm->max_frames && !sv_vm_grow_frames(vm)) {
          sv_err = js_mkerr_typed(js, JS_ERR_RANGE | JS_ERR_NO_STACK, "Maximum AOT call stack size exceeded");
          goto sv_throw;
        }
        if (closure->func->is_arrow || vtype(closure->bound_this) != T_UNDEF) call_this = closure->bound_this;
        frame = &vm->frames[vm->fp];
        frame->ip = ip + 3;
        vm->sp -= call_argc + 1;
        vm->fp++;
        frame = &vm->frames[vm->fp];
        func = closure->func;
        frame->func = func;
        frame->callee = call_func;
        frame->this = sv_normalize_this_for_frame(js, func, call_this);
        frame->new_target = js_mkundef();
        frame->super_val = js_mkundef();
        frame->prev_sp = vm->sp;
        frame->handler_base = vm->handler_depth;
        frame->handler_top = vm->handler_depth;
        frame->argc = call_argc;
        jsval_t *call_bp = NULL;
        jsval_t *call_lp = NULL;
        jsval_t call_stage_err = sv_stage_frame_args(
          vm, js, func, call_args, (int)call_argc, &call_bp, &call_lp
        );
        if (is_err(call_stage_err)) {
          vm->fp--;
          vm->sp += call_argc + 1;
          sv_err = call_stage_err;
          goto sv_throw;
        }
        frame->bp = call_bp;
        frame->lp = call_lp;
        frame->upvalues = closure->upvalues;
        frame->upvalue_count = closure->func->upvalue_count;
        bp = frame->bp;
        lp = frame->lp;
        
        ip = func->code;
        DISPATCH();
      }
    }
    call_fallback:;
    frame->ip = ip;
    jsval_t call_result = sv_vm_call(
      vm, js, call_func, call_this, call_args, call_argc, NULL, false);
    vm->sp -= call_argc + 1;
    if (is_err(call_result)) { sv_err = call_result; goto sv_throw; }
    if (is_super_call && is_object_type(call_result))
      frame->this = call_result;
    vm->stack[vm->sp++] = call_result;
    NEXT(3);
  }

  L_CALL_METHOD: {
    uint16_t call_argc = sv_get_u16(ip + 1);
    jsval_t *call_args = &vm->stack[vm->sp - call_argc];
    jsval_t call_func = vm->stack[vm->sp - call_argc - 1];
    jsval_t call_this = vm->stack[vm->sp - call_argc - 2];
    
    bool is_super_call =
      (vtype(frame->super_val) != T_UNDEF && call_func == frame->super_val);

    if (!is_super_call && vtype(frame->new_target) == T_UNDEF &&
        vtype(call_func) == T_FUNC) {
      sv_closure_t *closure = js_func_closure(call_func);
      if (closure->func != NULL) {
        if (closure->call_flags & (SV_CALL_HAS_BOUND_ARGS | SV_CALL_HAS_SUPER))
          goto call_method_fallback;
        if (closure->func->is_async) goto call_method_fallback;
        #ifdef ANT_JIT
        {
          sv_func_t *callee = closure->func;
          if (callee->jit_code) {
            jsval_t jit_this = (
              callee->is_arrow || vtype(closure->bound_this) != T_UNDEF)
              ? closure->bound_this : call_this;
            frame->ip = ip + 3;
            jsval_t jit_result = ((sv_jit_func_t)callee->jit_code)(
              vm, jit_this, call_args, (int)call_argc, closure);
            if (sv_is_jit_bailout(jit_result)) {
              sv_jit_on_bailout(callee);
              goto call_method_fallback;
            }
            vm->sp -= call_argc + 2;
            if (is_err(jit_result)) { sv_err = jit_result; goto sv_throw; }
            vm->stack[vm->sp++] = jit_result;
            ip = frame->ip;
            DISPATCH();
          }
          if (!callee->is_generator) {
            uint32_t cc = ++callee->call_count;
            if (__builtin_expect(cc == SV_TFB_ALLOC_THRESHOLD, 0))
              sv_tfb_ensure(callee);
            if (cc > SV_JIT_THRESHOLD) {
            sv_jit_func_t jit_fn = sv_jit_compile(js, callee, closure);
            if (jit_fn) {
              callee->jit_code = (void *)jit_fn;
              jsval_t jit_this = (
                callee->is_arrow || vtype(closure->bound_this) != T_UNDEF)
                ? closure->bound_this : call_this;
              frame->ip = ip + 3;
              jsval_t jit_result = jit_fn(vm, jit_this, call_args, (int)call_argc, closure);
              if (sv_is_jit_bailout(jit_result)) {
                sv_jit_on_bailout(callee);
                goto call_method_fallback;
              }
              vm->sp -= call_argc + 2;
              if (is_err(jit_result)) { sv_err = jit_result; goto sv_throw; }
              vm->stack[vm->sp++] = jit_result;
              ip = frame->ip;
              DISPATCH();
            } else {
              callee->call_count = 0;
              callee->back_edge_count = 0;
            }}
          }
        }
        #endif
        if (vm->fp + 1 >= vm->max_frames && !sv_vm_grow_frames(vm)) {
          sv_err = js_mkerr_typed(js, JS_ERR_RANGE | JS_ERR_NO_STACK, "Maximum AOT call stack size exceeded");
          goto sv_throw;
        }
        if (closure->func->is_arrow || vtype(closure->bound_this) != T_UNDEF)
          call_this = closure->bound_this;
        frame = &vm->frames[vm->fp];
        frame->ip = ip + 3;
        vm->sp -= call_argc + 2;
        vm->fp++;
        frame = &vm->frames[vm->fp];
        func = closure->func;
        frame->func = func;
        frame->callee = call_func;
        frame->this = sv_normalize_this_for_frame(js, func, call_this);
        frame->new_target = js_mkundef();
        frame->super_val = js_mkundef();
        frame->prev_sp = vm->sp;
        frame->handler_base = vm->handler_depth;
        frame->handler_top = vm->handler_depth;
        frame->argc = call_argc;
        jsval_t *call_bp = NULL;
        jsval_t *call_lp = NULL;
        jsval_t call_stage_err = sv_stage_frame_args(
          vm, js, func, call_args, (int)call_argc, &call_bp, &call_lp
        );
        if (is_err(call_stage_err)) {
          vm->fp--;
          vm->sp += call_argc + 2;
          sv_err = call_stage_err;
          goto sv_throw;
        }
        frame->bp = call_bp;
        frame->lp = call_lp;
        frame->upvalues = closure->upvalues;
        frame->upvalue_count = closure->func->upvalue_count;
        bp = frame->bp;
        lp = frame->lp;
        ip = func->code;
        DISPATCH();
      }
    }
    call_method_fallback:;
    frame->ip = ip;
    if (is_super_call) js->new_target = frame->new_target;
    jsval_t call_result = sv_vm_call(
      vm, js, call_func, call_this, call_args, call_argc, NULL, is_super_call);
    vm->sp -= call_argc + 2;
    if (is_err(call_result)) { sv_err = call_result; goto sv_throw; }
    if (is_super_call && is_object_type(call_result))
      frame->this = call_result;
    vm->stack[vm->sp++] = call_result;
    NEXT(3);
  }

  L_TAIL_CALL: {
    uint16_t call_argc = sv_get_u16(ip + 1);
    jsval_t call_func = vm->stack[vm->sp - call_argc - 1];
    tc_this = js_mkundef();

    if (vm->handler_depth == frame->handler_base &&
        vtype(frame->new_target) == T_UNDEF &&
        vtype(call_func) == T_FUNC) {
      sv_closure_t *closure = js_func_closure(call_func);
      if (closure->func != NULL) {
        if (!closure->func->is_async &&
            !(closure->call_flags & (SV_CALL_HAS_BOUND_ARGS | SV_CALL_HAS_SUPER))) {
          if (closure->func->is_arrow || vtype(closure->bound_this) != T_UNDEF)
            tc_this = closure->bound_this;
          goto tail_call_inline;
        }
      }
    }
    jsval_t *call_args = &vm->stack[vm->sp - call_argc];
    frame->ip = ip;
    jsval_t call_result = sv_vm_call(
      vm, js, call_func, tc_this, call_args, call_argc, NULL, false);
    vm->sp -= call_argc + 1;
    if (is_err(call_result)) { sv_err = call_result; goto sv_throw; }
    vm->stack[vm->sp++] = call_result;
    goto L_RETURN;
  }

  L_TAIL_CALL_METHOD: {
    uint16_t call_argc = sv_get_u16(ip + 1);
    jsval_t call_func = vm->stack[vm->sp - call_argc - 1];
    tc_this = vm->stack[vm->sp - call_argc - 2];

    if (vm->handler_depth == frame->handler_base &&
        vtype(frame->new_target) == T_UNDEF &&
        vtype(call_func) == T_FUNC) {
      sv_closure_t *closure = js_func_closure(call_func);
      if (closure->func != NULL) {
        if (!closure->func->is_async &&
            !(closure->call_flags & (SV_CALL_HAS_BOUND_ARGS | SV_CALL_HAS_SUPER))) {
          if (closure->func->is_arrow || vtype(closure->bound_this) != T_UNDEF)
            tc_this = closure->bound_this;
          goto tail_call_inline;
        }
      }
    }
    jsval_t *call_args = &vm->stack[vm->sp - call_argc];
    jsval_t call_result = sv_vm_call(
      vm, js, call_func, tc_this, call_args, call_argc, NULL, false);
    vm->sp -= call_argc + 2;
    if (is_err(call_result)) { sv_err = call_result; goto sv_throw; }
    vm->stack[vm->sp++] = call_result;
    goto L_RETURN;
  }

  tail_call_inline: {
    uint16_t call_argc = sv_get_u16(ip + 1);
    jsval_t *call_args = &vm->stack[vm->sp - call_argc];
    jsval_t call_func = vm->stack[vm->sp - call_argc - 1];
    sv_closure_t *closure = js_func_closure(call_func);
    if (frame->bp) sv_close_upvalues_from_slot(vm, frame->bp);
    vm->sp = frame->prev_sp;
    int arg_slots = (
      (int)call_argc > closure->func->param_count)
      ? (int)call_argc : closure->func->param_count;
    int need = arg_slots + closure->func->max_locals;
    jsval_t *base = &vm->stack[vm->sp];
    memmove(base, call_args, (size_t)call_argc * sizeof(jsval_t));
    for (int i = (int)call_argc; i < arg_slots; i++) base[i] = js_mkundef();
    jsval_t *new_lp = base + arg_slots;
    for (int i = 0; i < closure->func->max_locals; i++) new_lp[i] = js_mkundef();
    vm->sp = frame->prev_sp + need;
    func = closure->func;
    frame->func = func;
    frame->callee = call_func;
    frame->this = sv_normalize_this_for_frame(js, func, tc_this);
    frame->new_target = js_mkundef();
    frame->super_val = js_mkundef();
    frame->argc = call_argc;
    frame->handler_base = vm->handler_depth;
    frame->handler_top = vm->handler_depth;
    frame->bp = base;
    frame->lp = new_lp;
    frame->upvalues = closure->upvalues;
    frame->upvalue_count = closure->func->upvalue_count;
    bp = frame->bp;
    lp = frame->lp;
    ip = func->code;
    DISPATCH();
  }
  
  L_NEW:    { VM_CHECK(sv_op_new(vm, js, ip));          NEXT(3); }
  L_APPLY:  { VM_CHECK(sv_op_apply(vm, js, ip));        NEXT(3); }
  L_EVAL:   { VM_CHECK(sv_op_eval(vm, js, frame, ip));  NEXT(5); }

  // TODO: make the methods below DRY
  L_RETURN: {
    jsval_t r = vm->stack[--vm->sp];
    if (__builtin_expect(vm->handler_depth != frame->handler_base, 0)) {
      uint8_t *finally_ip = sv_vm_unwind_for_return(vm, r);
      if (finally_ip) {
        frame = &vm->frames[vm->fp];
        func = frame->func;
        bp = frame->bp;
        lp = frame->lp;
        ip = finally_ip;
        DISPATCH();
      }
      vm->handler_depth = frame->handler_base;
      frame->handler_top = frame->handler_base;
    }
    vm->sp = frame->prev_sp;
    if (vm->fp <= entry_fp) {
      vm_result = r;
      goto sv_leave;
    }
    vm->fp--;
    frame = &vm->frames[vm->fp];
    func = frame->func;
    bp = frame->bp;
    lp = frame->lp;
    ip = frame->ip;
    vm->stack[vm->sp++] = r;
    DISPATCH();
  }

  L_RETURN_UNDEF: {
    jsval_t r = js_mkundef();
    if (__builtin_expect(vm->handler_depth != frame->handler_base, 0)) {
      uint8_t *finally_ip = sv_vm_unwind_for_return(vm, r);
      if (finally_ip) {
        frame = &vm->frames[vm->fp];
        func = frame->func;
        bp = frame->bp;
        lp = frame->lp;
        ip = finally_ip;
        DISPATCH();
      }
      vm->handler_depth = frame->handler_base;
      frame->handler_top = frame->handler_base;
    }
    vm->sp = frame->prev_sp;
    if (vm->fp <= entry_fp) {
      vm_result = r;
      goto sv_leave;
    }
    vm->fp--;
    frame = &vm->frames[vm->fp];
    func = frame->func;
    bp = frame->bp;
    lp = frame->lp;
    ip = frame->ip;
    vm->stack[vm->sp++] = r;
    DISPATCH();
  }

  L_RETURN_ASYNC: {
    jsval_t r = vm->stack[--vm->sp];
    if (__builtin_expect(vm->handler_depth != frame->handler_base, 0)) {
      uint8_t *finally_ip = sv_vm_unwind_for_return(vm, r);
      if (finally_ip) {
        frame = &vm->frames[vm->fp];
        func = frame->func;
        bp = frame->bp;
        lp = frame->lp;
        ip = finally_ip;
        DISPATCH();
      }
      vm->handler_depth = frame->handler_base;
      frame->handler_top = frame->handler_base;
    }
    vm->sp = frame->prev_sp;
    if (vm->fp <= entry_fp) {
      vm_result = r;
      goto sv_leave;
    }
    vm->fp--;
    frame = &vm->frames[vm->fp];
    func = frame->func;
    bp = frame->bp;
    lp = frame->lp;
    ip = frame->ip;
    vm->stack[vm->sp++] = r;
    DISPATCH();
  }

  L_CHECK_CTOR:      { VM_CHECK(sv_op_check_ctor(vm, js));  NEXT(1); }
  L_CHECK_CTOR_RET:  { sv_op_check_ctor_ret(vm, frame);     NEXT(1); }
  
  L_HALT: {
    vm_result = sv_op_halt(vm, frame);
    goto sv_leave;
  }

  L_THROW:       { sv_err = sv_op_throw(vm);                      goto sv_throw; }
  L_THROW_ERROR: { sv_err = sv_op_throw_error(vm, js, func, ip);  goto sv_throw; }
  
  L_TRY_PUSH:    { sv_op_try_push(vm, ip);               NEXT(5); }
  L_TRY_POP:     { sv_op_try_pop(vm);                    NEXT(1); }
  L_CATCH:       { sv_op_catch(vm, sv_err, ip);          NEXT(5); }
  L_FINALLY:     { VM_CHECK(sv_op_finally(vm, js, ip));  NEXT(5); }
  
  L_FINALLY_RET: {
    uint8_t *resume_ip = NULL;
    jsval_t completion = js_mkundef();
    sv_finally_ret_t action = sv_op_finally_ret(vm, js, &resume_ip, &completion);
    if (action == SV_FINALLY_RET_ERROR || action == SV_FINALLY_RET_THROW) {
      sv_err = completion;
      goto sv_throw;
    }
    if (action == SV_FINALLY_RET_RETURN) {
      vm->handler_depth = frame->handler_base;
      frame->handler_top = frame->handler_base;
      vm->sp = frame->prev_sp;
      if (vm->fp <= entry_fp) {
        vm_result = completion;
        goto sv_leave;
      }
      vm->fp--;
      frame = &vm->frames[vm->fp];
      func = frame->func;
      bp = frame->bp;
      lp = frame->lp;
      
      ip = frame->ip;
      vm->stack[vm->sp++] = completion;
      DISPATCH();
    }
    ip = resume_ip;
    DISPATCH();
  }
  
  L_NIP_CATCH:        { sv_op_nip_catch(vm);                      NEXT(1); }
  L_FOR_IN:           { VM_CHECK(sv_op_for_in(vm, js));           NEXT(1); }
  L_FOR_OF:           { VM_CHECK(sv_op_for_of(vm, js));           NEXT(1); }
  L_FOR_AWAIT_OF:     { VM_CHECK(sv_op_for_await_of(vm, js));     NEXT(1); }
  L_ITER_NEXT:        { VM_CHECK(sv_op_iter_next(vm, js));        NEXT(1); }
  L_ITER_GET_VALUE:   { sv_op_iter_get_value(vm, js);             NEXT(1); }
  L_ITER_CLOSE:       { sv_op_iter_close(vm, js);                 NEXT(1); }
  L_ITER_CALL:        { VM_CHECK(sv_op_iter_call(vm, js, ip));    NEXT(2); }
  L_AWAIT_ITER_NEXT:  { VM_CHECK(sv_op_await_iter_next(vm, js));  NEXT(1); }

  L_AWAIT: {
    jsval_t await_val = vm->stack[--vm->sp];
    jsval_t result = sv_await_value(js, await_val);
    if (is_err(result)) { sv_err = result; goto sv_throw; }
    vm->stack[vm->sp++] = result;
    NEXT(1);
  }
  
  // TODO: implement
  L_YIELD:          { NEXT(1); }
  L_YIELD_STAR:     { NEXT(1); }
  L_INITIAL_YIELD:  { NEXT(1); }

  L_SPREAD:              { VM_CHECK(sv_op_spread(vm, js));         NEXT(1); }
  L_DEFINE_METHOD:       { sv_op_define_method(vm, js, func, ip);  NEXT(6); }
  L_DEFINE_METHOD_COMP:  { sv_op_define_method_comp(vm, js, ip);   NEXT(2); }
  L_SET_NAME:            { sv_op_set_name(vm, js, func, ip);       NEXT(5); }
  L_SET_NAME_COMP:       { sv_op_set_name_comp(vm, js);            NEXT(1); }
  L_SET_PROTO:           { sv_op_set_proto(vm, js);                NEXT(1); }
  L_SET_HOME_OBJ:        { sv_op_set_home_obj(vm, js);             NEXT(1); }
  L_APPEND:              { sv_op_append(vm, js);                   NEXT(1); }
  L_COPY_DATA_PROPS:     { sv_op_copy_data_props(vm, js, ip);      NEXT(2); }

  L_DEFINE_CLASS:       { sv_op_define_class(vm, js, func, ip);       NEXT(6); }
  L_DEFINE_CLASS_COMP:  { sv_op_define_class_comp(vm, js, func, ip);  NEXT(6); }
  L_ADD_BRAND:          { sv_op_add_brand(vm);                        NEXT(1); }

  L_TO_OBJECT:   { VM_CHECK(sv_op_to_object(vm, js));  NEXT(1); }
  L_TO_PROPKEY:  { sv_op_to_propkey(vm, js);           NEXT(1); }
  L_IS_UNDEF:    { sv_op_is_undef(vm);                 NEXT(1); }
  L_IS_NULL:     { sv_op_is_null(vm);                  NEXT(1); }

  L_IMPORT:          { VM_CHECK(sv_op_import(vm, js));            NEXT(1); }
  L_IMPORT_SYNC:     { VM_CHECK(sv_op_import_sync(vm, js));       NEXT(1); }
  L_IMPORT_DEFAULT:  { sv_op_import_default(vm, js);              NEXT(1); }
  L_EXPORT:          { VM_CHECK(sv_op_export(vm, js, func, ip));  NEXT(5); }
  L_EXPORT_ALL:      { VM_CHECK(sv_op_export_all(vm, js));        NEXT(1); }

  L_ENTER_WITH:   { VM_CHECK(sv_op_enter_with(vm, js, frame));  NEXT(1); }
  L_EXIT_WITH:    { sv_op_exit_with(vm, frame);                 NEXT(1); }

  L_WITH_GET_VAR:  { VM_CHECK(sv_op_with_get_var(vm, js, frame, func, ip));  NEXT(8); }
  L_WITH_PUT_VAR:  { sv_op_with_put_var(vm, js, frame, func, ip);            NEXT(8); }
  L_WITH_DEL_VAR:  { sv_op_with_del_var(vm, js, frame, func, ip);            NEXT(5); }

  L_SPECIAL_OBJ:  { sv_op_special_obj(vm, js, frame, ip);                       NEXT(2); }
  L_EMPTY:        { vm->stack[vm->sp++] = T_EMPTY;                              NEXT(1); }
  L_PUT_CONST:    { func->constants[sv_get_u32(ip + 1)] = vm->stack[--vm->sp];  NEXT(5); }
  
  L_DEBUGGER:  { NEXT(1); }
  L_NOP:       { NEXT(1); }

  L_LABEL:
  L_LINE_NUM:
  L_COL_NUM:
  L_INVALID:
    sv_err = js_mkerr(js, "invalid opcode %d", (int)*ip);
    goto sv_throw;

  sv_throw: {
    uint8_t *catch_ip = sv_vm_throw(vm, sv_err, entry_fp);
    if (catch_ip) {
      frame = &vm->frames[vm->fp];
      func = frame->func;
      bp = frame->bp;
      lp = frame->lp;
      
      ip = catch_ip;
      DISPATCH();
    }
    if (!is_err(sv_err)) {
      vm_result = js_throw(js, sv_err);
      goto sv_leave;
    }
    vm_result = sv_err;
    goto sv_leave;
  }

  sv_leave:
  for (int f = vm->fp; f >= entry_fp; f--) {
    jsval_t *drop_bp = vm->frames[f].bp;
    if (drop_bp) sv_close_upvalues_from_slot(vm, drop_bp);
  }
  vm->fp = entry_fp;
  vm->sp = vm->frames[entry_fp].prev_sp;
  vm->handler_depth = vm->frames[entry_fp].handler_base;

  return vm_result;

  #undef DISPATCH
  #undef NEXT
  #undef VM_CHECK

  // TODO: use entry_bp/frame->bp
  //       and sv_op_size values
  (void)bp;
  (void)sv_op_size;
}
