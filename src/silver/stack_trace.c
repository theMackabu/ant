#include "silver/stack_trace.h"
#include "errors.h"

#include <stdlib.h>

js_error_site_t js_error_site_from_bc(sv_func_t *func, int bc_offset) {
  js_error_site_t site = {0};
  
  const sv_srcpos_t *pos = bc_offset >= 0 ? sv_srcpos_at(func, bc_offset) : NULL;
  if (!pos || !func->debug->source || func->debug->source_len <= 0) return site;

  const char *src = func->debug->source;
  ant_offset_t src_len = (ant_offset_t)func->debug->source_len;
  ant_offset_t off = (ant_offset_t)pos->src_off;
  ant_offset_t span_len = pos->src_end > pos->src_off ? (ant_offset_t)(pos->src_end - pos->src_off) : 0;
  if (span_len == 0 && off < src_len) span_len = 1;

  site = js_error_site_from_source(src, src_len, func->debug->filename, off, span_len);
  site.func = func;
  site.line = pos->line;
  site.col = pos->col;
  
  return site;
}

js_error_site_t js_error_site_from_frame(sv_func_t *func, int bc_offset) {
  return js_error_site_from_bc(func, bc_offset < 0 ? 0 : bc_offset);
}

js_error_site_t js_error_site_from_vm_top(ant_t *js) {
  sv_stack_iter_t it;
  sv_stack_entry_t top;
  
  sv_stack_iter_init(js ? js->vm : NULL, &it, 1, true);
  bool found = sv_stack_iter_next(&it, &top);
  sv_stack_iter_finish(&it);
  
  return found 
    ? js_error_site_from_frame(top.func, top.bc_off) 
    : (js_error_site_t){0};
}

void sv_stack_iter_init(sv_vm_t *vm, sv_stack_iter_t *it, int limit, bool resolve) {
  it->vm = vm;
  it->seg = vm ? vm->js->vm_segs : NULL;
  it->acts = it->inline_acts;
  it->n_acts = 0;
  it->ai = 0;
  it->i = vm ? vm->fp : -1;
  it->skip = -1;
  it->n_pending = 0;
  it->resolve = resolve;
  if (vm && vm->js->jit_active_depth)
    it->n_acts = sv_jit_collect_activations(vm, &it->acts, SV_STACK_INLINE_ACTIVATIONS, limit);
}

void sv_stack_iter_finish(sv_stack_iter_t *it) {
  if (it->acts != it->inline_acts) free(it->acts);
  it->acts = it->inline_acts;
}

static int sv_stack_frame_bc_off(sv_stack_iter_t *it, int idx, const sv_frame_t *frame) {
  if (!frame->func || !frame->ip || !frame->func->code) return -1;
  int off = (int)(frame->ip - frame->func->code);
  if (idx >= it->vm->fp || off == 0) return off;
  while (it->seg && it->seg->entry_vm_fp > idx + 1) it->seg = it->seg->prev;
  return it->seg && it->seg->entry_vm_fp == idx + 1 ? off : off - 1;
}

static void sv_stack_resolve(sv_stack_iter_t *it, const sv_jit_activation_t *a, ant_value_t callee) {
  sv_jit_frame_t frames[SV_JIT_INLINE_FRAMES_MAX + 1];
  
  int n = sv_jit_frames_at(it->vm->js, a->pc, frames, SV_JIT_INLINE_FRAMES_MAX + 1);
  if (n <= 0) frames[n++] = (sv_jit_frame_t){ a->func, -1 };
  
  for (int k = 0; k < n; k++) it->pending[n - 1 - k] = (sv_stack_entry_t){
    .func = frames[k].func,
    .callee = k == n - 1 ? callee : js_mkundef(),
    .bc_off = frames[k].bc_off,
    .frame_index = -1,
  };
  
  it->n_pending = n;
}

bool sv_stack_iter_next(sv_stack_iter_t *it, sv_stack_entry_t *out) {
  sv_vm_t *vm = it->vm;
  if (!vm) return false;
  
  if (it->n_pending > 0) {
    *out = it->pending[--it->n_pending];
    return true;
  }

  for (;;) {
    if (it->ai < it->n_acts && it->acts[it->ai].vm_fp >= it->i) {
      sv_jit_activation_t *a = &it->acts[it->ai++];
      bool host = a->osr_host && a->vm_fp >= 0 && a->vm_fp <= vm->fp;
      
      if (host) it->skip = a->vm_fp;
      if (!a->func) continue;
      
      ant_value_t callee = host ? vm->frames[a->vm_fp].callee : js_mkundef();
      if (it->resolve) {
        sv_stack_resolve(it, a, callee);
        *out = it->pending[--it->n_pending];
      } else *out = (sv_stack_entry_t){ a->func, callee, a->pc, -1, -1 };
      
      return true;
    }

    if (it->i < 0) return false;
    int idx = it->i--;
    if (idx == it->skip) continue;
    
    sv_frame_t *frame = &vm->frames[idx];
    *out = (sv_stack_entry_t){ 
      frame->func, frame->callee, 0, 
      sv_stack_frame_bc_off(it, idx, frame), idx 
    };
    
    return true;
  }
}
