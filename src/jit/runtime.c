#include <time.h>

#include "gc/roots.h"
#include "jit_internal.h"

#include "silver/call.h"
#include "silver/glue.h"
#include "silver/feedback.h"

void *jit_helper_tier_up(ant_t *js, sv_func_t *func, sv_closure_t *closure) {
  if (!func->jit_code_cold) return NULL;
  return (void *)sv_jit_tier_up(js, func, closure);
}

static int64_t jit_promote_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void jit_helper_promote_start(int64_t *slot) {
  slot[2] = jit_promote_now();
}

int64_t jit_helper_promote_due(int64_t *slot) {
  int64_t now = jit_promote_now();
  slot[0] += now - slot[2];
  slot[2] = now;
  return slot[0] >= slot[1] ? 0 : 1;
}

void jit_load_externals_once(sv_jit_ctx_t *jc) {
  if (jc == NULL || jc->externals_loaded) return;
#define LOAD_EXT(name)                           \
  do {                                           \
    MIR_load_external(jc->ctx, #name, name);     \
    MIR_load_external(jc->ctx_hot, #name, name); \
  } while (0)
  LOAD_EXT(jit_helper_add);
  LOAD_EXT(jit_helper_sub);
  LOAD_EXT(jit_helper_mul);
  LOAD_EXT(jit_helper_div);
  LOAD_EXT(jit_helper_mod);
  LOAD_EXT(jit_helper_str_read_value);
  LOAD_EXT(jit_helper_str_append_local);
  LOAD_EXT(jit_helper_str_append_local_snapshot);
  LOAD_EXT(jit_helper_str_flush_local);
  LOAD_EXT(jit_helper_lt);
  LOAD_EXT(jit_helper_le);
  LOAD_EXT(jit_helper_gt);
  LOAD_EXT(jit_helper_ge);
  LOAD_EXT(jit_helper_call);
  LOAD_EXT(jit_helper_call_method);
  LOAD_EXT(jit_helper_call_array_includes);
  LOAD_EXT(jit_helper_call_char_code_at);
  LOAD_EXT(jit_helper_call_string_intrinsic);
  LOAD_EXT(jit_helper_call_map_template);
  LOAD_EXT(jit_helper_map_template_fast);
  LOAD_EXT(jit_helper_map_numeric_pair_fast);
  LOAD_EXT(jit_helper_regexp_exec_truthy);
  LOAD_EXT(jit_helper_call_stable_builtin);
  LOAD_EXT(jit_helper_load_stable_builtin);
  LOAD_EXT(jit_helper_apply);
  LOAD_EXT(jit_helper_forward_arguments);
  LOAD_EXT(jit_helper_call_call);
  LOAD_EXT(jit_helper_call_call_slot);
  LOAD_EXT(jit_helper_rest);
  LOAD_EXT(jit_helper_special_obj);
  LOAD_EXT(jit_helper_strict_arguments);
  LOAD_EXT(jit_helper_for_of);
  LOAD_EXT(jit_helper_iter_next);
  LOAD_EXT(jit_helper_destructure_close);
  LOAD_EXT(jit_helper_destructure_next);
  LOAD_EXT(jit_helper_get_global);
  LOAD_EXT(jit_helper_get_eval_global);
  LOAD_EXT(jit_helper_put_eval_global);
  LOAD_EXT(jit_helper_delete_eval_var);
  LOAD_EXT(jit_helper_get_field);
  LOAD_EXT(jit_helper_get_field_inline);
  LOAD_EXT(jit_helper_import_default);
  LOAD_EXT(jit_helper_import_named);
  LOAD_EXT(jit_helper_export);
  LOAD_EXT(jit_helper_to_propkey);
  LOAD_EXT(js_template_to_string);
  LOAD_EXT(jit_helper_bailout_resume);
  LOAD_EXT(jit_helper_promote_resume);
  LOAD_EXT(jit_helper_promote_start);
  LOAD_EXT(jit_helper_promote_due);
  LOAD_EXT(jit_helper_close_upval);
  LOAD_EXT(jit_helper_upval_barrier);
  LOAD_EXT(jit_helper_adopt_open_upvalues);
  LOAD_EXT(jit_helper_take_open_upvalues);
  LOAD_EXT(jit_helper_take_open_upvalues_rebase);
  LOAD_EXT(jit_helper_closure);
  LOAD_EXT(jit_helper_in);
  LOAD_EXT(jit_helper_instanceof);
  LOAD_EXT(jit_helper_call_is_proto);
  LOAD_EXT(jit_helper_get_length);
  LOAD_EXT(jit_helper_get_length_inline);
  LOAD_EXT(jit_helper_define_field);
  LOAD_EXT(jit_helper_define_method_comp);
  LOAD_EXT(jit_helper_seq);
  LOAD_EXT(jit_helper_eq);
  LOAD_EXT(jit_helper_ne);
  LOAD_EXT(jit_helper_sne);
  LOAD_EXT(jit_helper_put_field);
  LOAD_EXT(jit_helper_put_field_ic);
  LOAD_EXT(jit_helper_shape_transition);
  LOAD_EXT(gc_remember_add);
  LOAD_EXT(jit_helper_get_elem);
  LOAD_EXT(jit_helper_get_elem2);
  LOAD_EXT(jit_helper_get_elem_inline);
  LOAD_EXT(jit_helper_put_elem);
  LOAD_EXT(jit_helper_get_private);
  LOAD_EXT(jit_helper_put_private);
  LOAD_EXT(jit_helper_put_global);
  LOAD_EXT(jit_helper_object);
  LOAD_EXT(jit_helper_object_template);
  LOAD_EXT(jit_helper_regexp);
  LOAD_EXT(jit_helper_define_slot);
  LOAD_EXT(jit_helper_array);
  LOAD_EXT(jit_helper_catch_value);
  LOAD_EXT(jit_helper_throw);
  LOAD_EXT(jit_helper_throw_error);
  LOAD_EXT(jit_helper_set_proto);
  LOAD_EXT(jit_helper_band);
  LOAD_EXT(jit_helper_bor);
  LOAD_EXT(jit_helper_bxor);
  LOAD_EXT(jit_helper_bnot);
  LOAD_EXT(jit_helper_shl);
  LOAD_EXT(jit_helper_shr);
  LOAD_EXT(jit_helper_ushr);
  LOAD_EXT(jit_helper_not);
  LOAD_EXT(jit_helper_is_truthy);
  LOAD_EXT(jit_helper_typeof);
  LOAD_EXT(jit_helper_new);
  LOAD_EXT(jit_helper_delete);
  LOAD_EXT(jit_helper_set_name);
  LOAD_EXT(jit_helper_stack_overflow);
  LOAD_EXT(jit_helper_stack_overflow_error);
  LOAD_EXT(jit_helper_tier_up);
  LOAD_EXT(jit_helper_normalize_sloppy_this);
#undef LOAD_EXT
  jc->externals_loaded = true;
}

void sv_jit_init(ant_t *js) {
  if (js->jit_ctx) return;

  ANT_ASSERT(
    ant_object_flag_masks_match_layout(),
    "object flag bitfield layout does not match the JIT masks"
  );

  sv_jit_ctx_t *jc = calloc(1, sizeof(*jc));
  if (!jc) return;

  jc->mir_heap = jit_heap_create();
  jc->ctx = MIR_init2(jit_heap_mir_alloc(jc->mir_heap), NULL);
  
  MIR_gen_init(jc->ctx);
  MIR_gen_set_optimize_level(jc->ctx, 1);

  jc->ctx_hot = MIR_init2(jit_heap_mir_alloc(jc->mir_heap), NULL);
  MIR_gen_init(jc->ctx_hot);
  MIR_gen_set_optimize_level(jc->ctx_hot, 3);

  jit_load_externals_once(jc);
  js->jit_ctx = jc;
}

void jit_release_gen_scratch(sv_jit_ctx_t *jc, MIR_context_t ctx) {
  MIR_gen_finish(ctx);
  MIR_gen_init(ctx);
  MIR_gen_set_optimize_level(ctx, ctx == jc->ctx_hot ? 3 : 1);
  jit_heap_release(jc->mir_heap);
}

void sv_jit_destroy(ant_t *js) {
  sv_jit_ctx_t *jc = js->jit_ctx;
  if (!jc) return;

  MIR_gen_finish(jc->ctx);
  MIR_finish(jc->ctx);
  MIR_gen_finish(jc->ctx_hot);
  MIR_finish(jc->ctx_hot);
  jit_heap_destroy(jc->mir_heap);

  free(jc);
  js->jit_ctx = NULL;
}

static void sv_jit_compile_callees(ant_t *js, sv_func_t *func) {
  sv_call_target_fb_t *fb = func->call_target_fb;
  int count = func->call_target_fb_count;
  for (int i = 0; i < count; i++) {
    if (fb[i].disabled || !fb[i].target) continue;
    sv_func_t *callee = fb[i].target;
    if (callee->jit_code || callee->jit_compile_failed || callee->jit_compiling) continue;
    if (callee->call_count < SV_JIT_THRESHOLD / 2) continue;
    if (!jit_is_eligible(callee)) continue;
    sv_jit_func_t cjit = sv_jit_compile(js, callee, NULL);
    if (cjit) callee->jit_code = (void *)cjit;
  }
}

ant_value_t sv_jit_try_compile_and_call(
  sv_vm_t *vm, ant_t *js,
  sv_closure_t *closure, ant_value_t callee_func,
  sv_call_ctx_t *ctx, ant_value_t *out_this
) {
  sv_func_t *fn = closure->func;

  sv_jit_func_t jit = sv_jit_compile(js, fn, closure);
  if (!jit) {
    fn->call_count = 0;
    fn->back_edge_count = 0;
    return SV_JIT_RETRY_INTERP;
  }

  fn->jit_code = (void *)jit;
  sv_jit_enter(js);
  ant_value_t result = jit(
      vm, ctx->this_val, ctx->new_target,
      ctx->super_val, ctx->args, ctx->argc, closure);
  sv_jit_leave(js);
  if (sv_is_jit_bailout(result)) {
    sv_jit_on_bailout(fn);
    return SV_JIT_RETRY_INTERP;
  }
  sv_call_cleanup(js, ctx);
  if (out_this) *out_this = ctx->this_val;
  return result;
}

sv_jit_func_t sv_jit_tier_up(ant_t *js, sv_func_t *func, sv_closure_t *closure) {
  func->jit_code_cold = false;
  sv_jit_func_t hot = sv_jit_compile_tier(js, func, closure, SV_JIT_TIER_HOT);
  
  if (sv_jit_warn_unlikely) fprintf(
    stderr, "jit: tier-up %s func=%s code_len=%d\n",
    hot ? "compiled" : "compile-failed",
    func->debug->name ? func->debug->name : "<anonymous>", func->code_len
  );
  
  if (hot) func->jit_code = (void *)hot;
  return hot;
}

ant_value_t sv_jit_try_osr(
    sv_vm_t *vm, ant_t *js,
    sv_frame_t *frame, sv_func_t *func,
    int bc_offset) {
  func->jit_loop_hot = true;

  sv_closure_t *closure;
  ant_value_t osr_closure_value = js_mkundef();
  size_t root_mark = 0;
  bool synthetic_closure = false;
  if (vtype(frame->callee) == kTypeFunction)
    closure = js_func_closure(frame->callee);
  else {
    root_mark = gc_root_scope(js);
    if (!gc_push_root(js, &osr_closure_value)) return SV_JIT_RETRY_INTERP;
    synthetic_closure = true;
    closure = js_closure_alloc(js);
    if (!closure) {
      gc_pop_roots(js, root_mark);
      return SV_JIT_RETRY_INTERP;
    }
    osr_closure_value = mkref(kTypeFunction, closure);
    closure->func = func;
    closure->upvalues = frame->upvalues;
    closure->call_flags = SV_CALL_BORROWED_UPVALS;
    closure->js = js;
    closure->bound_this = js_mkundef();
    closure->super_val = js_mkundef();
    closure->module_ctx = js_mkundef();
    closure->gc_epoch = gc_get_epoch();
  }

  sv_jit_func_t jit;
  if (func->jit_code) {
    jit = (sv_jit_func_t)func->jit_code;
  } else {
    sv_jit_tier_t tier = sv_jit_promote_pending(func) ? SV_JIT_TIER_HOT
     : func->code_len > JIT_OSR_COLD_COMPILE_MIN_BYTES 
     ? SV_JIT_TIER_COLD : SV_JIT_TIER_AUTO;
    
    jit = sv_jit_compile_tier(js, func, closure, tier);
    
    if (sv_jit_warn_unlikely) fprintf(
      stderr, "jit: osr %s func=%s code_len=%d threshold=%u tier=%s\n",
      jit ? "compiled" : "compile-failed",
      func->debug->name ? func->debug->name : "<anonymous>",
      func->code_len, func->jit_osr_threshold,
      tier == SV_JIT_TIER_HOT ? "hot" : tier == SV_JIT_TIER_COLD ? "cold" : "auto"
    );
    
    if (!jit) {
      if (synthetic_closure) gc_pop_roots(js, root_mark);
      return SV_JIT_RETRY_INTERP;
    }
    
    func->jit_code = (void *)jit;
    sv_jit_compile_callees(js, func);
  }

  int nl = func->max_locals;
  ant_value_t osr_locals[nl > 0 ? nl : 1];
  for (int i = 0; i < nl; i++)
    osr_locals[i] = frame->lp[i];

  vm->jit_osr.active = true;
  vm->jit_osr.bc_offset = bc_offset;
  vm->jit_osr.locals = osr_locals;
  vm->jit_osr.n_locals = nl;
  vm->jit_osr.lp = frame->lp;
  vm->jit_osr.vstack = frame->lp + nl;
  vm->jit_osr.vstack_sp =
      vm->sp - (int)(vm->jit_osr.vstack - vm->stack);

  func->back_edge_count = 0;
  sv_jit_enter(js);
  ant_value_t result = jit(
      vm, frame->this, frame->new_target, frame->super_val,
      frame->bp, frame->argc, closure);
  vm->jit_osr = (sv_jit_osr_t){0};
  sv_jit_leave(js);
  if (synthetic_closure) gc_pop_roots(js, root_mark);

  if (result == SV_JIT_RETRY_INTERP && sv_jit_warn_unlikely) fprintf(
    stderr, "jit: osr entry-rejected func=%s offset=%d\n",
    func->debug->name ? func->debug->name : "<anonymous>", bc_offset
  );

  if (sv_is_jit_bailout(result)) {
    sv_jit_on_bailout(func);
    return SV_JIT_RETRY_INTERP;
  }

  return result;
}
