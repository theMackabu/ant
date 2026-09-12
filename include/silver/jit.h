#ifndef SILVER_JIT_H
#define SILVER_JIT_H

#include "silver/engine.h"

// TODO: constexpr
#define SV_JIT_OSR_THRESHOLD 500
#define SV_JIT_MAX_CODE_BYTES (64 * 1024)
#define SV_JIT_OSR_THRESHOLD_SCALE_BYTES 512

typedef enum {
  SV_JIT_TIER_AUTO, // hot context if the function looped, cheap otherwise
  SV_JIT_TIER_COLD, // cheap context; marks jit_code_cold for later re-tier
  SV_JIT_TIER_HOT,  // hot context regardless of history
} sv_jit_tier_t;

void sv_jit_init(ant_t *js);
void sv_jit_destroy(ant_t *js);

sv_jit_func_t sv_jit_tier_up(
  ant_t *js, sv_func_t *func, 
  sv_closure_t *closure
);

sv_jit_func_t sv_jit_compile(
  ant_t *js, sv_func_t *func, 
  sv_closure_t *hint_closure
);

sv_jit_func_t sv_jit_compile_tier(
  ant_t *js, sv_func_t *func,
  sv_closure_t *hint_closure, sv_jit_tier_t tier
);

ant_value_t sv_jit_try_osr(
  sv_vm_t *vm, ant_t *js,
  sv_frame_t *frame, sv_func_t *func,
  int bc_offset
);

static inline bool sv_jit_promote_pending(const sv_func_t *func) {
  return func->jit_code_cold && func->jit_code == NULL;
}

static inline uint32_t sv_jit_osr_threshold_for(int code_len) {
  if (code_len <= 0) return SV_JIT_OSR_THRESHOLD;
  
  uint64_t t =
    (uint64_t)SV_JIT_OSR_THRESHOLD * (uint64_t)code_len / 
    (uint64_t)SV_JIT_OSR_THRESHOLD_SCALE_BYTES;
  
  if (t < SV_JIT_OSR_THRESHOLD) t = SV_JIT_OSR_THRESHOLD;
  if (t > UINT32_MAX / 2) t = UINT32_MAX / 2;
  
  return (uint32_t)t;
}

#endif
