#include "silver/swarm.h"

void sv_jit_init(ant_t *js) {
  (void)js;
}

void sv_jit_destroy(ant_t *js) {
  (void)js;
}

sv_jit_func_t sv_jit_compile(
  ant_t *js, sv_func_t *func, sv_closure_t *hint_closure
) {
  (void)js;
  (void)hint_closure;
  if (func) func->jit_compile_failed = true;
  return NULL;
}

ant_value_t sv_jit_try_compile_and_call(
  sv_vm_t *vm, ant_t *js, sv_closure_t *closure,
  ant_value_t callee_func, sv_call_ctx_t *ctx, ant_value_t *out_this
) {
  (void)vm;
  (void)js;
  (void)callee_func;
  (void)ctx;
  (void)out_this;
  if (closure && closure->func) closure->func->jit_compile_failed = true;
  return SV_JIT_RETRY_INTERP;
}

ant_value_t sv_jit_try_osr(
  sv_vm_t *vm, ant_t *js, sv_frame_t *frame, sv_func_t *func, int bc_offset
) {
  (void)vm;
  (void)js;
  (void)frame;
  (void)bc_offset;
  if (func) func->jit_compile_failed = true;
  return SV_JIT_RETRY_INTERP;
}

