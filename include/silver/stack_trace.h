#ifndef SV_STACK_TRACE_H
#define SV_STACK_TRACE_H

#include "silver/engine.h"

static constexpr int SV_JIT_INLINE_FRAMES_MAX = 4;
static constexpr int SV_STACK_INLINE_ACTIVATIONS = 16;

typedef struct {
  sv_func_t *func;
  uintptr_t pc;
  int vm_fp;
  bool osr_host;
} sv_jit_activation_t;

typedef struct {
  sv_func_t *func;
  int bc_off;
} sv_jit_frame_t;

typedef struct {
  sv_func_t *func;
  ant_value_t callee;
  uintptr_t pc;
  int bc_off;
  int frame_index;
} sv_stack_entry_t;

typedef struct {
  sv_vm_t *vm;
  gc_vm_seg_t *seg;
  sv_jit_activation_t inline_acts[SV_STACK_INLINE_ACTIVATIONS];
  sv_jit_activation_t *acts;
  int n_acts, ai;
  int i, skip;
  sv_stack_entry_t pending[SV_JIT_INLINE_FRAMES_MAX + 1];
  int n_pending;
  bool resolve;
} sv_stack_iter_t;

bool js_is_error_record(ant_value_t value);

void sv_stack_iter_init(sv_vm_t *vm, sv_stack_iter_t *it, int limit, bool resolve);
bool sv_stack_iter_next(sv_stack_iter_t *it, sv_stack_entry_t *out);
void sv_stack_iter_finish(sv_stack_iter_t *it);

int sv_jit_collect_activations(sv_vm_t *vm, sv_jit_activation_t **acts, int cap, int limit);
int sv_jit_frames_at(ant_t *js, uintptr_t pc, sv_jit_frame_t *out, int cap);

ant_value_t js_error_record_capture(ant_t *js, const js_error_site_t *site);
ant_value_t js_error_record_stack_text(ant_t *js, ant_value_t header, ant_value_t record);
ant_value_t js_error_record_throw_text(ant_t *js, ant_value_t value, ant_value_t record);

#endif
