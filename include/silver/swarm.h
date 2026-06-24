#ifndef SILVER_JIT_H
#define SILVER_JIT_H

#ifdef ANT_JIT
#include "silver/engine.h"

#define SV_JIT_OSR_THRESHOLD 500

void sv_jit_init(ant_t *js);
void sv_jit_destroy(ant_t *js);
void sv_jit_poll(ant_t *js);
bool sv_jit_request_compile(ant_t *js, sv_func_t *func);
void sv_jit_visit_queued_funcs(
  ant_t *js, void (*visitor)(void *ctx, sv_func_t *func), void *ctx
);

sv_jit_func_t sv_jit_compile(
  ant_t *js, sv_func_t *func, 
  sv_closure_t *hint_closure
);

ant_value_t sv_jit_try_osr(
  sv_vm_t *vm, ant_t *js,
  sv_frame_t *frame, sv_func_t *func,
  int bc_offset
);

#endif
#endif
