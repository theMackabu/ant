#include "silver/call.h"
#include "silver/feedback.h"
#include "silver/jit.h"
#include "silver/stack_trace.h"

void sv_jit_init(ant_t *js) {}
void sv_jit_destroy(ant_t *js) {}

sv_jit_func_t sv_jit_compile(ant_t *js, sv_func_t *func, sv_closure_t *hint_closure) {
  if (func) func->jit_compile_failed = true;
  return NULL;
}

ant_value_t sv_jit_try_compile_and_call(
  sv_vm_t *vm, ant_t *js, sv_closure_t *closure, ant_value_t callee_func, 
  sv_call_ctx_t *ctx, ant_value_t *out_this
) {
  if (closure && closure->func) closure->func->jit_compile_failed = true;
  return SV_JIT_RETRY_INTERP;
}

ant_value_t sv_jit_try_osr(
  sv_vm_t *vm, ant_t *js, sv_frame_t *frame, sv_func_t *func, int bc_offset
) {
  if (func) func->jit_compile_failed = true;
  return SV_JIT_RETRY_INTERP;
}

int sv_jit_collect_activations(sv_vm_t *vm, sv_jit_activation_t **acts, int cap, int limit) { return 0; }
int sv_jit_frames_at(ant_t *js, uintptr_t pc, sv_jit_frame_t *out, int cap) { return 0; }

ant_value_t sv_jit_await_resume(sv_vm_t *vm, ant_t *js, sv_frame_t *frame, sv_func_t *func, int resume_off) { return SV_JIT_RETRY_INTERP; }
ant_value_t sv_jit_try_catch_entry(sv_vm_t *vm, ant_t *js, sv_frame_t *frame, sv_func_t *func, int catch_off) { return SV_JIT_RETRY_INTERP; }

void sv_jit_drop_func_code(sv_func_t *func) {}
void sv_jit_drop_closure_pinned(sv_func_t *func) {}
void sv_jit_drop_opt_code(sv_func_t *func) {}

void sv_jit_gc_mark_roots(ant_t *js) {}
void sv_jit_forget_dying(ant_t *js, uint64_t epoch) {}
void sv_jit_release_modules(ant_t *js, sv_func_t *func) {}

void opt_tier_func_free(ant_t *js, sv_func_t *func) {}
void opt_tier_func_trace(ant_t *js, sv_func_t *func) {}

void opt_tier_gc_doom(ant_t *js) {}
void opt_tier_gc_weak(ant_t *js) {}
