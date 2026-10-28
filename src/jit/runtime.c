#include <math.h>
#include <time.h>

#include "gc/roots.h"
#include "gc/objects.h"
#include "interrupt.h"
#include "jit_internal.h"
#include "modules/regex.h"

#include "silver/call.h"
#include "silver/glue.h"
#include "silver/feedback.h"

void *jit_helper_tier_up(ant_t *js, sv_func_t *func, sv_closure_t *closure) {
  bool due = func->jit_code_cold || func->jit_code_retier;
  sv_jit_func_t hot = due ? sv_jit_tier_up(js, func, closure) : NULL;
  if (!hot) func->call_count = 0;
  return (void *)hot;
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
  LOAD_EXT(fmod);
  LOAD_EXT(jit_helper_str_read_value);
  LOAD_EXT(jit_helper_str_append_local);
  LOAD_EXT(jit_helper_str_append_local_snapshot);
  LOAD_EXT(jit_helper_str_flush_local);
  LOAD_EXT(jit_helper_lt);
  LOAD_EXT(jit_helper_le);
  LOAD_EXT(jit_helper_gt);
  LOAD_EXT(jit_helper_ge);
  LOAD_EXT(jit_helper_call);
  LOAD_EXT(jit_helper_call_awaited);
  LOAD_EXT(jit_helper_await_park);
  LOAD_EXT(jit_helper_await_return_direct);
  LOAD_EXT(jit_helper_native_finish);
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
  LOAD_EXT(jit_helper_async_start);
  LOAD_EXT(jit_helper_call_spread);
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
  LOAD_EXT(jit_helper_upval_flagged);
  LOAD_EXT(jit_helper_elem_barrier);
  LOAD_EXT(jit_helper_disable_param_counters);
  LOAD_EXT(jit_helper_number_to_string);
  LOAD_EXT(ant_math_ceil);
  LOAD_EXT(ant_math_floor);
  LOAD_EXT(ant_math_round);
  LOAD_EXT(ant_math_sqrt);
  LOAD_EXT(ant_math_trunc);
  LOAD_EXT(jit_helper_adopt_open_upvalues);
  LOAD_EXT(jit_helper_take_open_upvalues);
  LOAD_EXT(jit_helper_take_open_upvalues_rebase);
  LOAD_EXT(jit_helper_closure);
  LOAD_EXT(jit_helper_in);
  LOAD_EXT(jit_helper_instanceof);
  LOAD_EXT(jit_helper_call_is_proto);
  LOAD_EXT(jit_helper_get_length);
  LOAD_EXT(jit_helper_get_length_at);
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
  LOAD_EXT(jit_helper_protector_store);
  LOAD_EXT(gc_remember_props);
  LOAD_EXT(jit_helper_get_elem);
  LOAD_EXT(jit_helper_realm_reenter);
  LOAD_EXT(js_get_index_fast);
  LOAD_EXT(jit_helper_get_elem2);
  LOAD_EXT(jit_helper_get_elem_inline);
  LOAD_EXT(jit_helper_put_elem);
  LOAD_EXT(jit_helper_put_elem_strict);
  LOAD_EXT(jit_helper_get_private);
  LOAD_EXT(jit_helper_put_private);
  LOAD_EXT(jit_helper_def_private);
  LOAD_EXT(jit_helper_spread);
  LOAD_EXT(js_arr_push);
  LOAD_EXT(jit_helper_put_global);
  LOAD_EXT(jit_helper_object);
  LOAD_EXT(jit_helper_object_template);
  LOAD_EXT(regexp_create_literal);
  LOAD_EXT(jit_helper_define_slot);
  LOAD_EXT(jit_helper_array);
  LOAD_EXT(jit_helper_catch_value);
  LOAD_EXT(jit_helper_for_in);
  LOAD_EXT(jit_helper_interrupt);
  LOAD_EXT(jit_helper_throw);
  LOAD_EXT(jit_helper_throw_error);
  LOAD_EXT(jit_helper_set_proto);
  LOAD_EXT(js_copy_data_props);
  LOAD_EXT(jit_helper_in_field);
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
  LOAD_EXT(jit_helper_new_this);
  LOAD_EXT(jit_helper_delete);
  LOAD_EXT(jit_helper_delete_strict);
  LOAD_EXT(jit_helper_set_name);
  LOAD_EXT(jit_helper_stack_overflow);
  LOAD_EXT(jit_helper_stack_overflow_error);
  LOAD_EXT(jit_helper_tier_up);
  LOAD_EXT(jit_helper_normalize_sloppy_this);
#undef LOAD_EXT
  jc->externals_loaded = true;
}

static void jit_gen_init(sv_jit_ctx_t *jc, MIR_context_t ctx) {
  MIR_gen_init(ctx);
  MIR_gen_set_code_info_func(ctx, jit_code_info, jc);
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
  jit_gen_init(jc, jc->ctx);

  jc->ctx_hot = MIR_init2(jit_heap_mir_alloc(jc->mir_heap), NULL);
  jit_gen_init(jc, jc->ctx_hot);

  jit_load_externals_once(jc);
  js->jit_ctx = jc;
}

void jit_release_gen_scratch(sv_jit_ctx_t *jc, MIR_context_t ctx) {
  MIR_gen_finish(ctx);
  jit_gen_init(jc, ctx);
  jit_heap_release(jc->mir_heap);
}

void sv_jit_gc_mark_roots(ant_t *js) {
  sv_jit_ctx_t *jc = js->jit_ctx;
  if (!jc) return;
  
  if (js->cstk.base) 
    jit_code_mark_stack(jc, js, gc_native_sp(), (uintptr_t)js->cstk.base);
  
  if (js->cstk.main_base && js->cstk.main_lo)
    jit_code_mark_stack(jc, js, (uintptr_t)js->cstk.main_lo, (uintptr_t)js->cstk.main_base);
  
  opt_tier_gc_mark_roots(js);
}

void sv_jit_release_modules(ant_t *js, sv_func_t *func) {
  sv_func_sidecar_t *sidecar = sv_func_sidecar(func);
  if (!sidecar || !sidecar->jit_modules) return;
  
  if (js->jit_ctx) for (uint32_t i = 0; i < sidecar->jit_module_count; i++)
    MIR_remove_module(sidecar->jit_modules[i].ctx, sidecar->jit_modules[i].module);
  
  free(sidecar->jit_modules);
  sidecar->jit_modules = NULL;
  sidecar->jit_module_count = sidecar->jit_module_cap = 0;
}

void sv_jit_destroy(ant_t *js) {
  sv_jit_ctx_t *jc = js->jit_ctx;
  if (!jc) return;

  MIR_gen_finish(jc->ctx);
  MIR_finish(jc->ctx);
  MIR_gen_finish(jc->ctx_hot);
  MIR_finish(jc->ctx_hot);
  
  jit_code_registry_destroy(jc);
  opt_tier_destroy(jc);
  jit_heap_destroy(jc->mir_heap);
  
  for (uint32_t i = 0; i < jc->pinned_watch_count; i++) 
    ant_proto_watch_unpin(jc->pinned_watches[i]);
    
  free(jc->pinned_watches);
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
    if (callee->call_count < SV_JIT_WARM_CALLS) continue;
    if (!jit_is_eligible(callee)) continue;
    
    sv_jit_func_t cjit = sv_jit_compile(js, callee, NULL);
    if (cjit) callee->jit_code = cjit;
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

  fn->jit_code = jit;
  ant_value_t result = sv_jit_invoke(js, SV_JIT_FROM_C, jit, vm, ctx, closure);

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
  func->jit_code_retier = false;
  sv_jit_func_t hot = sv_jit_compile_tier(js, func, closure, SV_JIT_TIER_HOT);
  
  if (sv_jit_warn_unlikely) fprintf(
    stderr, "jit: tier-up %s func=%s code_len=%d\n",
    hot ? "compiled" : "compile-failed",
    func->debug->name ? func->debug->name : "<anonymous>", func->code_len
  );
  
  if (hot) func->jit_code = hot;
  return hot;
}

static inline ant_value_t jit_osr_finish(sv_vm_t *vm, sv_frame_t *frame, ant_value_t result) {
  if (result != SV_JIT_RETRY_INTERP && result != SV_JIT_AWAIT_PARKED) sv_frame_reset_handlers(vm, frame);
  return result;
}

static void jit_osr_seed_invariant_locals(sv_func_t *func, sv_frame_t *frame) {
  if (!func->local_type_feedback) return;
  
  uint8_t *loop_written = jit_locals_written_in_loops(func);
  if (!loop_written) return;

  for (int i = 0; i < func->max_locals; i++) {
    if (loop_written[i] || func->local_type_feedback[i]) continue;
    if (vtype(frame->lp[i]) == kTypeNumber) sv_tfb_record_local(func, i, frame->lp[i]);
  }

  free(loop_written);
}

__attribute__((noinline)) ant_value_t sv_jit_try_osr(
  sv_vm_t *vm, ant_t *js, sv_frame_t *frame, sv_func_t *func, int bc_offset
) {
  if (func->is_async && func->jit_code && !func->jit_await_park && jit_loop_has_await(func, bc_offset)) {
    func->back_edge_count = 0;
    return SV_JIT_RETRY_INTERP;
  }

  if (func->has_try && sv_func_in_finally_body(func, bc_offset)) {
    func->back_edge_count = 0;
    return SV_JIT_RETRY_INTERP;
  }

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
    
    if (frame->upvalues && func->upvalue_count <= SV_CLOSURE_INLINE_UPVALS) {
      memcpy(closure->inline_upvals, frame->upvalues, sizeof(sv_upvalue_t *) * (size_t)func->upvalue_count);
      closure->upvalues = closure->inline_upvals;
    }
    
    closure->call_flags = SV_CALL_BORROWED_UPVALS;
    closure->js = js;
    closure->bound_this = js_mkundef();
    closure->super_val = js_mkundef();
    closure->module_ctx = js_mkundef();
    closure->gc_epoch = gc_get_epoch(js);
  }

  sv_jit_func_t jit;
  if (func->jit_code) jit = func->jit_code;
  else {
    sv_jit_tier_t tier = sv_jit_promote_pending(func) ? SV_JIT_TIER_HOT
     : func->code_len > JIT_OSR_COLD_COMPILE_MIN_BYTES 
     ? SV_JIT_TIER_COLD : SV_JIT_TIER_AUTO;

    jit_osr_seed_invariant_locals(func, frame);
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
    
    func->jit_code = jit;
    sv_jit_compile_callees(js, func);

    if (func->is_async && !func->jit_await_park && jit_loop_has_await(func, bc_offset)) {
      if (synthetic_closure) gc_pop_roots(js, root_mark);
      func->back_edge_count = 0;
      return SV_JIT_RETRY_INTERP;
    }
  }

  int nl = func->max_locals;
  ant_value_t osr_locals[nl > 0 ? nl : 1];
  for (int i = 0; i < nl; i++) osr_locals[i] = frame->lp[i];

  gc_vm_seg_t *interp_seg = js->vm_segs;
  uintptr_t interp_seg_lo = interp_seg ? interp_seg->lo : 0;
  uintptr_t osr_sp = gc_native_sp();
  
  bool cover_osr_frame =
    interp_seg && interp_seg->fp == *(const uintptr_t *)__builtin_frame_address(0) &&
    osr_sp < interp_seg->lo && interp_seg->lo < interp_seg->hi;
  
  if (cover_osr_frame) interp_seg->lo = osr_sp;
  sv_jit_scrub_stack();
  
  vm->jit.osr.active = SV_JIT_OSR_LOOP;
  vm->jit.osr.bc_offset = bc_offset;
  vm->jit.osr.locals = osr_locals;
  vm->jit.osr.n_locals = nl;
  vm->jit.osr.lp = frame->lp;
  vm->jit.osr.vstack = frame->lp + nl;
  vm->jit.osr.vstack_sp = vm->sp - (int)(vm->jit.osr.vstack - vm->stack);

  func->back_edge_count = 0;
  sv_jit_osr_mark_t osr_mark = { vm->jit.osr_marks };
  vm->jit.osr_marks = &osr_mark;
  
  ant_value_t result = sv_jit_invoke(js, SV_JIT_FROM_INTERP, jit, vm, &(sv_call_ctx_t){
    .this_val = frame->this,
    .new_target = frame->new_target,
    .super_val = frame->super_val,
    .args = frame->bp,
    .argc = frame->argc,
  }, closure);

  if (cover_osr_frame) interp_seg->lo = interp_seg_lo;
  vm->jit.osr_marks = osr_mark.prev;
  vm->jit.osr = (sv_jit_osr_t){0};
  
  if (synthetic_closure) gc_pop_roots(js, root_mark);

  if (result == SV_JIT_RETRY_INTERP && sv_jit_warn_unlikely) fprintf(
    stderr, "jit: osr entry-rejected func=%s offset=%d\n",
    func->debug->name ? func->debug->name : "<anonymous>", bc_offset
  );

  if (sv_is_jit_bailout(result)) {
    sv_jit_on_bailout(func);
    return SV_JIT_RETRY_INTERP;
  }

  return jit_osr_finish(vm, frame, result);
}

__attribute__((noinline)) ant_value_t sv_jit_try_catch_entry(
  sv_vm_t *vm, ant_t *js, sv_frame_t *frame, sv_func_t *func, int catch_off
) {
  if (func->is_async || vtype(frame->callee) != kTypeFunction) return SV_JIT_RETRY_INTERP;
  if (!opt_tier_has_catch_entry(js, func, (uint32_t)catch_off)) return SV_JIT_RETRY_INTERP;

  int nl = func->max_locals;
  ant_value_t osr_locals[nl > 0 ? nl : 1];
  for (int i = 0; i < nl; i++) osr_locals[i] = frame->lp[i];

  gc_vm_seg_t *interp_seg = js->vm_segs;
  uintptr_t interp_seg_lo = interp_seg ? interp_seg->lo : 0;
  uintptr_t osr_sp = gc_native_sp();
  
  bool cover_osr_frame =
    interp_seg && interp_seg->fp == *(const uintptr_t *)__builtin_frame_address(0) &&
    osr_sp < interp_seg->lo && interp_seg->lo < interp_seg->hi;
  
  if (cover_osr_frame) interp_seg->lo = osr_sp;

  vm->jit.osr.active = SV_JIT_OSR_LOOP;
  vm->jit.osr.bc_offset = catch_off + sv_op_size[OP_CATCH];
  vm->jit.osr.locals = osr_locals;
  vm->jit.osr.n_locals = nl;
  vm->jit.osr.lp = frame->lp;
  vm->jit.osr.vstack = frame->lp + nl;
  vm->jit.osr.vstack_sp = vm->sp - (int)(vm->jit.osr.vstack - vm->stack);

  sv_jit_osr_mark_t osr_mark = { vm->jit.osr_marks };
  vm->jit.osr_marks = &osr_mark;

  ant_value_t result = sv_jit_invoke(js, SV_JIT_FROM_INTERP, func->jit_code, vm, &(sv_call_ctx_t){
    .this_val = frame->this,
    .new_target = frame->new_target,
    .super_val = frame->super_val,
    .args = frame->bp,
    .argc = frame->argc,
  }, js_func_closure(frame->callee));

  if (cover_osr_frame) interp_seg->lo = interp_seg_lo;
  vm->jit.osr_marks = osr_mark.prev;
  vm->jit.osr = (sv_jit_osr_t){0};

  if (sv_is_jit_bailout(result)) {
    sv_jit_on_bailout(func);
    return SV_JIT_RETRY_INTERP;
  }
  
  return jit_osr_finish(vm, frame, result);
}

__attribute__((noinline)) ant_value_t sv_jit_await_resume(
  sv_vm_t *vm, ant_t *js, sv_frame_t *frame,
  sv_func_t *func, int resume_off
) {
  sv_jit_await_map_t *map = sv_func_jit_await_map(func);
  
  int site = map ? sv_jit_await_site_index(map, (uint32_t)(resume_off - sv_op_size[OP_AWAIT])) : -1;
  if (site < 0 || !map->sites[site].resumable || vtype(frame->callee) != kTypeFunction) return SV_JIT_RETRY_INTERP;

  ant_value_t *stack_base = frame->lp + func->max_locals;
  vm->jit.osr = (sv_jit_osr_t){
    .active = SV_JIT_OSR_AWAIT, .bc_offset = site,
    .lp = frame->lp, .vstack = stack_base,
    .vstack_sp = vm->sp - (int)(stack_base - vm->stack),
  };

  sv_jit_osr_mark_t osr_mark = { vm->jit.osr_marks };
  vm->jit.osr_marks = &osr_mark;
  
  ant_value_t result = sv_jit_invoke(js, SV_JIT_FROM_INTERP, func->jit_code, vm, &(sv_call_ctx_t){
    .this_val = frame->this,
    .new_target = frame->new_target,
    .super_val = frame->super_val,
    .args = frame->bp,
    .argc = frame->argc,
  }, js_func_closure(frame->callee));
  
  vm->jit.osr_marks = osr_mark.prev;
  vm->jit.osr = (sv_jit_osr_t){0};

  if (result == SV_JIT_RETRY_INTERP && sv_jit_warn_unlikely) fprintf(
    stderr, "jit: await-resume rejected func=%s offset=%d\n",
    func->debug->name ? func->debug->name : "<anonymous>", resume_off
  );

  if (sv_is_jit_bailout(result)) {
    sv_jit_on_bailout(func);
    return SV_JIT_RETRY_INTERP;
  }
  
  return jit_osr_finish(vm, frame, result);
}

void sv_jit_drop_func_code(sv_func_t *func) {
  func->jit_code = NULL;
  func->jit_code_cold = false;
  func->jit_tdz_specialized = false;
  func->jit_self_guarded = false;
  func->back_edge_count = 0;
  func->jit_compiled_tfb_ver = 0;
  func->call_count = SV_JIT_THRESHOLD - SV_JIT_RECOMPILE_DELAY;
}

void sv_jit_drop_all_code(ant_t *js) {
  jit_drop_all_code(js->jit_ctx);
}

void sv_jit_arm_interrupts(ant_t *js) {
  if (js->interrupts_armed) return;
  js->interrupts_armed = true;
  jit_drop_all_code(js->jit_ctx);
}
