#include "gc.h"
#include "errors.h"
#include <stdlib.h>

#include "silver/engine.h"
#include "silver/swarm.h"
#include "modules/regex.h"

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
#include "ops/using.h"
#include "ops/objects.h"
#include "ops/coercion.h"

sv_vm_t *sv_vm_create(ant_t *js, sv_vm_kind_t kind) {
  int stack_size, max_frames;
  sv_vm_limits(kind, &stack_size, &max_frames);

  sv_vm_t *vm = calloc(1, sizeof(*vm));
  if (!vm) return NULL;
  
  vm->js = js;
  vm->fp = -1;
  
  vm->stack_size = stack_size;
  vm->max_frames = max_frames;
  
  vm->suspended_entry_fp = -1;
  vm->suspended_saved_fp = -1;
  
  vm->stack = calloc((size_t)stack_size, sizeof(ant_value_t));
  vm->frames = calloc((size_t)max_frames, sizeof(sv_frame_t));
  
  if (!vm->stack || !vm->frames) { 
    sv_vm_destroy(vm);
    return NULL;
  }
  
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
  
  ant_value_t *old = vm->stack;
  int old_size = vm->stack_size;
  
  ant_value_t *ns = realloc(vm->stack, (size_t)new_size * sizeof(ant_value_t));
  if (!ns) return false;
  
  ptrdiff_t delta = ns - old;
  vm->stack = ns;
  vm->stack_size = new_size;
  
  if (delta != 0) {
    for (int i = 0; i <= vm->fp; i++) {
      if (vm->frames[i].bp) vm->frames[i].bp += delta;
      if (vm->frames[i].lp) vm->frames[i].lp += delta;
    }
    for (sv_upvalue_t *uv = vm->open_upvalues; uv; uv = uv->next) if (
      uv->location != &uv->closed &&
      sv_slot_in_range(old, (size_t)old_size, uv->location)
    ) uv->location += delta;
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

static ant_offset_t sv_srcpos_to_offset_local(const char *code, ant_offset_t clen, uint32_t line, uint32_t col) {
  ant_offset_t off = 0;
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
    ant_offset_t off = (ant_offset_t)src_off;
    ant_offset_t span_len = (ant_offset_t)(src_end > src_off ? (src_end - src_off) : 0);
    if (span_len <= 0 && off < (ant_offset_t)func->source_len) span_len = 1;
    
    js_set_error_site(
      js, func->source, (ant_offset_t)func->source_len,
      filename ? filename : func->filename, off, span_len
    );
    return;
  }

  uint32_t line = 0, col = 0;
  if (sv_lookup_srcpos(func, bc_offset, &line, &col)) {
    ant_offset_t off = sv_srcpos_to_offset_local(func->source, (ant_offset_t)func->source_len, line, col);
    js_set_error_site(
      js, func->source, (ant_offset_t)func->source_len,
      filename ? filename : func->filename, off, 0
    );
  }
}

void js_set_error_site_from_vm_top(ant_t *js) {
  sv_vm_t *vm = sv_vm_get_active(js);
  if (!js || !vm || vm->fp < 0) return;
  
  sv_frame_t *frame = &vm->frames[vm->fp];
  sv_func_t *func = frame->func;
  if (!func) return;
  
  int bc_off = 0;
  if (frame->ip && func->code) bc_off = (int)(frame->ip - func->code);
  js_set_error_site_from_bc(js, func, bc_off, func->filename);
}

// TODO: move to strings.c
static inline bool sv_builder_has_cached_value(const ant_string_builder_t *builder) {
  return builder && builder->cached != js_mkundef();
}

static inline ant_flat_string_t *sv_string_builder_flat_ptr(ant_value_t value) {
  if (vtype(value) != T_STR || str_is_heap_rope(value) || str_is_heap_builder(value)) return NULL;
  return (ant_flat_string_t *)(uintptr_t)vdata(value);
}

static inline ant_string_builder_t *sv_string_builder_heap_ptr(ant_value_t value) {
  if (vtype(value) != T_STR || !str_is_heap_builder(value)) return NULL;
  return ant_str_builder_ptr(value);
}

static inline uint8_t sv_builder_chunk_ascii_state(ant_flat_string_t *flat) {
  if (!flat) return STR_ASCII_UNKNOWN;
  if (flat->is_ascii == STR_ASCII_UNKNOWN)
    flat->is_ascii = str_detect_ascii_bytes(flat->bytes, (size_t)flat->len);
  return flat->is_ascii;
}

static inline void sv_builder_note_ascii(ant_string_builder_t *builder, uint8_t state) {
  if (!builder) return;
  if (state == STR_ASCII_NO) builder->ascii_state = STR_ASCII_NO;
  else if (builder->ascii_state != STR_ASCII_NO && state == STR_ASCII_YES)
    builder->ascii_state = STR_ASCII_YES;
}

static inline void sv_builder_record_flat(ant_string_builder_t *builder, ant_flat_string_t *flat) {
  if (!builder || !flat) return;
  builder->len += flat->len;
  sv_builder_note_ascii(builder, sv_builder_chunk_ascii_state(flat));
}

static ant_value_t sv_builder_normalize_chunk(ant_t *js, ant_value_t value) {
  if (vtype(value) != T_STR) return js_mkerr(js, "string builder expects string chunk");
  return str_materialize(js, value);
}

static ant_value_t sv_builder_push_chunk_value(
  ant_t *js, ant_string_builder_t *builder, ant_value_t chunk
) {
  if (!builder) return js_mkerr(js, "string builder chunk allocation failed");
  ant_builder_chunk_t *node = (ant_builder_chunk_t *)js_type_alloc(
    js, ANT_ALLOC_ROPE, sizeof(*node), _Alignof(ant_builder_chunk_t)
  );
  if (!node) return js_mkerr(js, "string builder chunk allocation failed");
  node->next = NULL;
  node->value = chunk;
  if (builder->chunk_tail) builder->chunk_tail->next = node;
  else builder->head = node;
  builder->chunk_tail = node;
  return js_mkundef();
}

static ant_value_t sv_builder_flush_tail(
  ant_t *js, ant_string_builder_t *builder
) {
  if (!builder || builder->tail_len == 0) return js_mkundef();
  ant_value_t tail = js_mkstr(js, builder->tail, builder->tail_len);
  if (is_err(tail)) return tail;
  ant_value_t push = sv_builder_push_chunk_value(js, builder, tail);
  if (is_err(push)) return push;
  builder->tail_len = 0;
  return js_mkundef();
}

static ant_value_t sv_builder_append_flat(
  ant_t *js, ant_string_builder_t *builder, ant_value_t chunk
) {
  ant_flat_string_t *flat = sv_string_builder_flat_ptr(chunk);
  if (!flat) return js_mkerr(js, "string builder received non-flat string");
  if (flat->len == 0) return js_mkundef();

  if (
    flat->len <= STR_BUILDER_TAIL_CAP &&
    builder->tail_len + flat->len <= STR_BUILDER_TAIL_CAP
  ) {
    memcpy(builder->tail + builder->tail_len, flat->bytes, (size_t)flat->len);
    builder->tail_len = (uint16_t)(builder->tail_len + flat->len);
    sv_builder_record_flat(builder, flat);
    return js_mkundef();
  }

  ant_value_t flush = sv_builder_flush_tail(js, builder);
  if (is_err(flush)) return flush;
  ant_value_t push = sv_builder_push_chunk_value(js, builder, chunk);
  
  if (is_err(push)) return push;
  sv_builder_record_flat(builder, flat);
  
  return js_mkundef();
}

static ant_value_t sv_string_builder_new(ant_t *js) {
  ant_string_builder_t *builder = (ant_string_builder_t *)js_type_alloc(
    js, ANT_ALLOC_ROPE, sizeof(*builder), _Alignof(ant_string_builder_t)
  );
  if (!builder) return js_mkerr(js, "string builder allocation failed");
  memset(builder, 0, sizeof(*builder));
  builder->cached = js_mkundef();
  builder->ascii_state = STR_ASCII_YES;
  return ant_mkbuilder_value(builder);
}

static inline void sv_record_slot_feedback(
  sv_frame_t *frame, sv_func_t *func, uint16_t slot_idx, ant_value_t value
) {
  if (!frame || !func) return;
  if ((int)slot_idx < func->param_count) return;
  sv_tfb_record_local(func, (int)(slot_idx - func->param_count), value);
}

bool sv_slot_has_open_upvalue(sv_vm_t *vm, ant_value_t *slot) {
  if (!vm || !slot) return false;
  for (sv_upvalue_t *uv = vm->open_upvalues; uv; uv = uv->next)
    if (uv->location == slot) return true;
  return false;
}

ant_value_t sv_string_builder_read_value(ant_t *js, ant_value_t value) {
  if (vtype(value) == T_STR && str_is_heap_builder(value))
    return str_materialize(js, value);
  return value;
}

static ant_value_t sv_slot_generic_add_store(
  sv_vm_t *vm, ant_t *js, ant_value_t *slot, ant_value_t lhs, ant_value_t rhs
) {
  vm->stack[vm->sp++] = lhs;
  vm->stack[vm->sp++] = rhs;
  ant_value_t err = sv_op_add(vm, js);
  if (is_err(err)) return err;
  *slot = vm->stack[--vm->sp];
  return js_mkundef();
}

ant_value_t sv_string_builder_flush_slot(
  sv_vm_t *vm, ant_t *js, sv_frame_t *frame, uint16_t slot_idx
) {
  ant_value_t *slot = sv_frame_slot_ptr(frame, slot_idx);
  if (!slot || vtype(*slot) != T_STR || !str_is_heap_builder(*slot)) return js_mkundef();

  ant_value_t out = str_materialize(js, *slot);
  if (is_err(out)) return out;
  *slot = out;
  sv_record_slot_feedback(frame, frame->func, slot_idx, out);
  return out;
}

ant_value_t sv_string_builder_append_slot(
  sv_vm_t *vm, ant_t *js, sv_frame_t *frame,
  sv_func_t *func, uint16_t slot_idx, ant_value_t rhs
) {
  ant_value_t *slot = sv_frame_slot_ptr(frame, slot_idx);
  if (!slot) return js_mkerr(js, "invalid string builder slot");

  ant_value_t lhs = *slot;
  ant_string_builder_t *builder = sv_string_builder_heap_ptr(lhs);

  if (builder) {
    ant_value_t rhs_str = coerce_to_str_concat(js, rhs);
    if (is_err(rhs_str)) return rhs_str;
    rhs_str = sv_builder_normalize_chunk(js, rhs_str);
    if (is_err(rhs_str)) return rhs_str;
    builder->cached = js_mkundef();
    ant_value_t append_err = sv_builder_append_flat(js, builder, rhs_str);
    if (is_err(append_err)) return append_err;
    sv_record_slot_feedback(frame, func, slot_idx, lhs);
    return js_mkundef();
  }

  if (vtype(lhs) == T_NUM && vtype(rhs) == T_NUM) {
    *slot = tov(tod(lhs) + tod(rhs));
    sv_record_slot_feedback(frame, func, slot_idx, *slot);
    return js_mkundef();
  }

  ant_value_t lu = unwrap_primitive(js, lhs);
  ant_value_t ru = unwrap_primitive(js, rhs);
  bool string_concat = is_non_numeric(lu) || is_non_numeric(ru);
  if (!string_concat) {
    ant_value_t add_err = sv_slot_generic_add_store(vm, js, slot, lhs, rhs);
    if (is_err(add_err)) return add_err;
    sv_record_slot_feedback(frame, func, slot_idx, *slot);
    return js_mkundef();
  }

  ant_value_t lhs_str = coerce_to_str_concat(js, lhs);
  if (is_err(lhs_str)) return lhs_str;
  ant_value_t rhs_str = coerce_to_str_concat(js, rhs);
  if (is_err(rhs_str)) return rhs_str;
  lhs_str = sv_builder_normalize_chunk(js, lhs_str);
  if (is_err(lhs_str)) return lhs_str;
  rhs_str = sv_builder_normalize_chunk(js, rhs_str);
  if (is_err(rhs_str)) return rhs_str;

  ant_value_t builder_value = sv_string_builder_new(js);
  if (is_err(builder_value)) return builder_value;
  builder = sv_string_builder_heap_ptr(builder_value);

  ant_value_t append_lhs = sv_builder_append_flat(js, builder, lhs_str);
  if (is_err(append_lhs)) return append_lhs;
  ant_value_t append_rhs = sv_builder_append_flat(js, builder, rhs_str);
  if (is_err(append_rhs)) return append_rhs;
  *slot = builder_value;
  sv_record_slot_feedback(frame, func, slot_idx, *slot);
  
  return js_mkundef();
}

ant_value_t sv_string_builder_append_snapshot_slot(
  sv_vm_t *vm, ant_t *js, sv_frame_t *frame,
  sv_func_t *func, uint16_t slot_idx, ant_value_t lhs, ant_value_t rhs
) {
  ant_value_t *slot = sv_frame_slot_ptr(frame, slot_idx);
  if (!slot) return js_mkerr(js, "invalid string builder slot");

  // the snapshot path is only semantically required if the slot changed while
  // evaluating the RHS. when it did not, delegate to the normal append path so
  // active builders can keep appending in place instead of rebuilding.
  if (*slot == lhs)
    return sv_string_builder_append_slot(vm, js, frame, func, slot_idx, rhs);

  if (vtype(lhs) == T_NUM && vtype(rhs) == T_NUM) {
    *slot = tov(tod(lhs) + tod(rhs));
    sv_record_slot_feedback(frame, func, slot_idx, *slot);
    return js_mkundef();
  }

  ant_value_t lu = unwrap_primitive(js, lhs);
  ant_value_t ru = unwrap_primitive(js, rhs);
  
  bool string_concat = is_non_numeric(lu) || is_non_numeric(ru);
  if (!string_concat) {
    ant_value_t add_err = sv_slot_generic_add_store(vm, js, slot, lhs, rhs);
    if (is_err(add_err)) return add_err;
    sv_record_slot_feedback(frame, func, slot_idx, *slot);
    return js_mkundef();
  }

  ant_value_t lhs_str = coerce_to_str_concat(js, lhs);
  if (is_err(lhs_str)) return lhs_str;
  ant_value_t rhs_str = coerce_to_str_concat(js, rhs);
  if (is_err(rhs_str)) return rhs_str;
  lhs_str = sv_builder_normalize_chunk(js, lhs_str);
  if (is_err(lhs_str)) return lhs_str;
  rhs_str = sv_builder_normalize_chunk(js, rhs_str);
  if (is_err(rhs_str)) return rhs_str;

  ant_value_t builder_value = sv_string_builder_new(js);
  if (is_err(builder_value)) return builder_value;
  ant_string_builder_t *builder = sv_string_builder_heap_ptr(builder_value);

  ant_value_t append_lhs = sv_builder_append_flat(js, builder, lhs_str);
  if (is_err(append_lhs)) return append_lhs;
  ant_value_t append_rhs = sv_builder_append_flat(js, builder, rhs_str);
  if (is_err(append_rhs)) return append_rhs;
  *slot = builder_value;
  sv_record_slot_feedback(frame, func, slot_idx, *slot);
  
  return js_mkundef();
}

void sv_vm_visit_frame_funcs(sv_vm_t *vm, void (*visitor)(void *, sv_func_t *), void *ctx) {
  if (!vm) return;
  for (int i = 0; i <= vm->fp; i++) if (vm->frames[i].func) visitor(ctx, vm->frames[i].func);
}

ant_value_t sv_call_async_closure_dispatch(
  sv_vm_t *vm, ant_t *js, sv_closure_t *closure,
  ant_value_t callee_func, ant_value_t super_val,
  ant_value_t this_val, ant_value_t *args, int argc
) {
  return sv_start_async_closure(vm, js, closure, callee_func, super_val, this_val, args, argc);
}

ant_value_t sv_execute_entry_tla(ant_t *js, sv_func_t *func, ant_value_t this_val) {
  return sv_start_tla(js, func, this_val);
}

static inline void sv_sync_frame_locals(
  sv_vm_t *vm, sv_frame_t **frame, sv_func_t **func,
  ant_value_t **bp, ant_value_t **lp
) {
  *frame = &vm->frames[vm->fp]; *func = (*frame)->func;
  *bp = (*frame)->bp; *lp = (*frame)->lp;
}

static inline void sv_drop_frame_runtime_state(ant_t *js, sv_frame_t *frame) {
if (frame && vtype(frame->arguments_obj) != T_UNDEF) {
  js_arguments_detach(js, frame->arguments_obj);
  frame->arguments_obj = js_mkundef();
}}

static inline ant_value_t sv_stage_frame_args(
  sv_vm_t *vm, ant_t *js, sv_func_t *func, ant_value_t *args, int argc,
  ant_value_t **out_bp, ant_value_t **out_lp
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

  ant_value_t *base = &vm->stack[vm->sp];
  *out_bp = base;
  *out_lp = base + arg_slots;

  if (argc > 0 && args)
    memmove(base, args, (size_t)argc * sizeof(ant_value_t));
  for (int i = argc; i < arg_slots; i++)
    base[i] = js_mkundef();
  if (func->max_locals > 0)
    for (int i = 0; i < func->max_locals; i++)
      (*out_lp)[i] = js_mkundef();
  vm->sp += need;
  return js_mkundef();
}

static inline void sv_yield_star_store_state(sv_vm_t *vm, ant_value_t *lp, uint16_t base) {
  lp[base + 0] = vm->stack[vm->sp - 3];
  lp[base + 1] = vm->stack[vm->sp - 2];
  lp[base + 2] = vm->stack[vm->sp - 1];
}

static inline void sv_yield_star_push_state(sv_vm_t *vm, ant_value_t *lp, uint16_t base) {
  vm->stack[vm->sp++] = lp[base + 0];
  vm->stack[vm->sp++] = lp[base + 1];
  vm->stack[vm->sp++] = lp[base + 2];
}

static inline void sv_yield_star_clear_state(ant_t *js, ant_value_t *lp, uint16_t base) {
  lp[base + 0] = js_mkundef();
  lp[base + 1] = js_mkundef();
  lp[base + 2] = js_mkundef();
  lp[base + 3] = js_false;
}

static inline ant_value_t sv_yield_star_unpack_result(
  ant_t *js, ant_value_t result, ant_value_t *out_value, bool *out_done
) {
  if (!is_object_type(result))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Iterator result is not an object");

  ant_value_t done = js_mkundef();
  sv_iter_result_unpack(js, result, &done, out_value);
  
  if (is_err(done)) return done;
  if (is_err(*out_value)) return *out_value;
  *out_done = js_truthy(js, done);
  
  return js_mkundef();
}

static inline ant_value_t sv_yield_star_call_method(
  sv_vm_t *vm, ant_t *js, ant_value_t iterator,
  const char *name, ant_value_t arg, ant_value_t *out_value, bool *out_done
) {
  ant_value_t method = js_getprop_fallback(js, iterator, name);
  uint8_t mt = vtype(method);
  
  if (mt != T_FUNC && mt != T_CFUNC) {
    *out_value = js_mkundef();
    *out_done = true;
    return js_mkundef();
  }

  ant_value_t call_args[1] = { arg };
  ant_value_t result = sv_vm_call(vm, js, method, iterator, call_args, 1, NULL, false);
  if (is_err(result)) return result;
  
  return sv_yield_star_unpack_result(js, result, out_value, out_done);
}

static inline ant_value_t sv_yield_star_close_iterator(
  sv_vm_t *vm, ant_t *js, ant_value_t iterator
) {
  ant_value_t close_value = js_mkundef();
  bool close_done = true;
  
  return sv_yield_star_call_method(
    vm, js, iterator, "return", js_mkundef(), 
    &close_value, &close_done
  );
}

static inline ant_value_t sv_yield_star_next(
  sv_vm_t *vm, ant_t *js, ant_value_t *lp, uint16_t base,
  ant_value_t sent, ant_value_t *out_value, bool *out_done
) {
  int tag = (int)js_getnum(lp[base + 2]);
  bool first = js_truthy(js, lp[base + 3]);
  lp[base + 3] = js_false;

  if (tag == SV_ITER_GENERIC) {
    ant_value_t iterator = lp[base + 0];
    ant_value_t next_method = lp[base + 1];
    
    uint8_t nt = vtype(next_method);
    if (nt != T_FUNC && nt != T_CFUNC)
      return js_mkerr(js, "iterator.next is not a function");
      
    ant_value_t call_args[1] = { sent };
    ant_value_t result = sv_vm_call(
      vm, js, next_method, iterator, first ? NULL : call_args, first ? 0 : 1,
      NULL, false
    );
    
    if (is_err(result)) return result;
    return sv_yield_star_unpack_result(js, result, out_value, out_done);
  }

  sv_yield_star_push_state(vm, lp, base);
  ant_value_t status = sv_iter_advance(vm, js, 0, out_value, out_done);
  if (is_err(status)) return status;
  
  sv_yield_star_store_state(vm, lp, base);
  vm->sp -= 3;
  
  return js_mkundef();
}

static inline ant_value_t sv_yield_star_throw(
  sv_vm_t *vm, ant_t *js, ant_value_t *lp, uint16_t base,
  ant_value_t thrown, ant_value_t *out_value, bool *out_done
) {
  int tag = (int)js_getnum(lp[base + 2]);
  if (tag != SV_ITER_GENERIC) return js_throw(js, thrown);

  ant_value_t iterator = lp[base + 0];
  ant_value_t throw_method = js_getprop_fallback(js, iterator, "throw");
  uint8_t tt = vtype(throw_method);
  
  if (tt != T_FUNC && tt != T_CFUNC) {
    ant_value_t close_status = sv_yield_star_close_iterator(vm, js, iterator);
    if (is_err(close_status)) return close_status;
    return js_throw(js, thrown);
  }

  ant_value_t call_args[1] = { thrown };
  ant_value_t result = sv_vm_call(vm, js, throw_method, iterator, call_args, 1, NULL, false);
  if (is_err(result)) return result;
  
  return sv_yield_star_unpack_result(js, result, out_value, out_done);
}

static inline ant_value_t sv_yield_star_return(
  sv_vm_t *vm, ant_t *js, ant_value_t *lp, uint16_t base,
  ant_value_t value, ant_value_t *out_value, bool *out_done
) {
  int tag = (int)js_getnum(lp[base + 2]);
  
  if (tag != SV_ITER_GENERIC) {
    *out_value = value;
    *out_done = true;
    return js_mkundef();
  }

  return sv_yield_star_call_method(vm, js, lp[base + 0], "return", value, out_value, out_done);
}

static inline ant_value_t sv_execute_entry_common(
  sv_vm_t *vm, sv_func_t *func, sv_upvalue_t **upvalues, int upvalue_count,
  ant_value_t callee_func, ant_value_t super_val,
  ant_value_t this_val, ant_value_t *args, int argc, ant_value_t *out_this
) {
  if (!vm || !vm->js || !func) return mkval(T_ERR, 0);
  ant_t *js = vm->js;
  if (vm->fp + 1 >= vm->max_frames && !sv_vm_grow_frames(vm))
    return js_mkerr_typed(js, JS_ERR_RANGE | JS_ERR_NO_STACK, "Maximum call stack size exceeded");

  int saved_fp = vm->fp;
  vm->fp = saved_fp + 1;
  vm->frames[vm->fp].upvalues = upvalues;
  vm->frames[vm->fp].upvalue_count = upvalue_count;
  vm->frames[vm->fp].callee = callee_func;

  ant_value_t result = sv_execute_frame(vm, func, this_val, super_val, args, argc);
  if (out_this) *out_this = vm->frames[vm->fp].this;
  if (!vm->suspended) vm->fp = saved_fp;

  return result;
}

#ifdef ANT_JIT
static inline ant_value_t sv_try_direct_closure_jit(
  sv_vm_t *vm, ant_t *js, sv_func_t *caller_func, uint8_t *caller_ip, sv_frame_t *caller_frame,
  sv_closure_t *closure, ant_value_t jit_this, ant_value_t *call_args, int call_argc
) {
  sv_func_t *callee = closure->func;
  if (!callee) return SV_JIT_RETRY_INTERP;

  if (caller_func && caller_func->type_feedback && caller_ip)
    sv_tfb_record_call_target(caller_func, (int)(caller_ip - caller_func->code), callee);

  if (callee->jit_code) {
    if (caller_frame && caller_ip) caller_frame->ip = caller_ip + 3;
    sv_jit_enter(js);
    ant_value_t jit_result = ((sv_jit_func_t)callee->jit_code)(
      vm, jit_this, js_mkundef(), closure->super_val, 
      call_args, call_argc, closure
    );
    sv_jit_leave(js);
    if (sv_is_jit_bailout(jit_result)) {
      sv_jit_on_bailout(callee);
      return SV_JIT_RETRY_INTERP;
    }
    return jit_result;
  }

  if (callee->is_generator) return SV_JIT_RETRY_INTERP;

  uint32_t cc = ++callee->call_count;
  if (__builtin_expect(cc == SV_TFB_ALLOC_THRESHOLD, 0))
    sv_tfb_ensure(callee);
  if (cc <= SV_JIT_THRESHOLD) return SV_JIT_RETRY_INTERP;

  sv_jit_func_t jit_fn = sv_jit_compile(js, callee, closure);
  if (!jit_fn) {
    callee->call_count = 0;
    callee->back_edge_count = 0;
    return SV_JIT_RETRY_INTERP;
  }

  callee->jit_code = (void *)jit_fn;
  if (caller_frame && caller_ip) caller_frame->ip = caller_ip + 3;
  sv_jit_enter(js);
  ant_value_t jit_result = jit_fn(
    vm, jit_this, js_mkundef(), closure->super_val,
    call_args, call_argc, closure
  );
  sv_jit_leave(js);
  if (sv_is_jit_bailout(jit_result)) {
    sv_jit_on_bailout(callee);
    return SV_JIT_RETRY_INTERP;
  }
  return jit_result;
}
#endif

ant_value_t sv_execute_entry(
  sv_vm_t *vm, sv_func_t *func, ant_value_t this_val, ant_value_t *args, int argc
) {
  return sv_execute_entry_common(vm, func, NULL, 0, js_mkundef(), js_mkundef(), this_val, args, argc, NULL);
}

ant_value_t sv_execute_closure_entry(
  sv_vm_t *vm, sv_closure_t *closure, ant_value_t callee_func, ant_value_t super_val,
  ant_value_t this_val, ant_value_t *args, int argc, ant_value_t *out_this
) {
  if (!closure || !closure->func) return mkval(T_ERR, 0);
  return sv_execute_entry_common(
    vm, closure->func, closure->upvalues, closure->func->upvalue_count, callee_func,
    super_val, this_val, args, argc, out_this
  );
}

ant_value_t sv_execute_frame(sv_vm_t *vm, sv_func_t *func, ant_value_t this, ant_value_t super_val, ant_value_t *args, int argc) {
  ant_t *js = vm->js;
  
  bool resuming = vm->suspended && vm->suspended_resume_pending;
  uint8_t *ip = resuming ? NULL : func->code;
  
  int entry_fp = resuming 
    ? vm->suspended_entry_fp 
    : vm->fp;
  
  ant_value_t vm_result = js_mkundef();
  ant_value_t suspended_resume_value = js_mkundef();
  sv_resume_kind_t suspended_resume_kind = SV_RESUME_NEXT;
  
  js->vm_exec_depth++;

  // TODO: shorthand?
  sv_frame_t *frame = &vm->frames[vm->fp];
  if (!resuming) {
    sv_drop_frame_runtime_state(js, frame);
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
    frame->arguments_obj = js_mkundef();
  } else {
    func = frame->func;
    ip = frame->ip;
    suspended_resume_value = vm->suspended_resume_value;
    suspended_resume_kind = vm->suspended_resume_kind;
    vm->suspended = false;
    vm->suspended_resume_pending = false;
    vm->suspended_resume_is_error = false;
    vm->suspended_resume_kind = SV_RESUME_NEXT;
    vm->suspended_resume_value = js_mkundef();
  }
  
  ant_value_t *entry_bp = NULL;
  ant_value_t *entry_lp = NULL;
  
  if (!resuming) {
  ant_value_t stage_err = sv_stage_frame_args(vm, js, func, args, argc, &entry_bp, &entry_lp);
  if (is_err(stage_err)) {
    vm_result = stage_err;
    goto sv_leave;
  }}
  
  #ifdef ANT_JIT
  if (!resuming && vm->jit_resume.active) {
    ip = func->code + vm->jit_resume.ip_offset;
    frame->ip = ip;
    int64_t rl = 
      vm->jit_resume.n_locals < func->max_locals
      ? vm->jit_resume.n_locals : func->max_locals;
    for (int64_t i = 0; i < rl; i++)
      entry_lp[i] = vm->jit_resume.locals[i];
      
    ant_value_t *old_lp = vm->jit_resume.locals;
    for (sv_upvalue_t *uv = vm->open_upvalues; uv; uv = uv->next) {
    if (uv->location >= old_lp && uv->location < old_lp + rl) {
      ptrdiff_t slot = uv->location - old_lp;
      uv->location = &entry_lp[slot];
    }}
  }
  #endif
  
  if (!resuming) {
    frame->bp = entry_bp;
    frame->lp = entry_lp;
  }

  ant_value_t *bp = frame->bp;
  ant_value_t *lp = frame->lp;

  #ifdef ANT_JIT
  if (!resuming && vm->jit_resume.active) {
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

  static const void *dispatch[OP__COUNT] = {
    #define OP_DEF(name, size, n_pop, n_push, f) [OP_##name] = &&L_##name,
    #include "silver/opcode.h"
  };
  
  ant_value_t sv_err;
  ant_value_t  tc_this = js_mkundef();
  
  #define VM_CHECK(expr) ({            \
    frame->ip = ip;                    \
    sv_err = (expr);                   \
    if (is_err(sv_err)) goto sv_throw; \
  })

  #define DISPATCH() goto *dispatch[*ip]
  #define NEXT(n)    ({ ip += (n); DISPATCH(); })

  #ifdef ANT_JIT
  #define JIT_OSR_BACK_EDGE() do {                                          \
    if (!func->jit_compile_failed) {                                        \
      if (!func->type_feedback) sv_tfb_ensure(func);                        \
      if (++func->back_edge_count >= SV_JIT_OSR_THRESHOLD) {                \
      ant_value_t osr_r = sv_jit_try_osr(                                   \
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
  if (resuming) {
    bool yield_star_resume = ip && (
      *ip == OP_YIELD_STAR_NEXT ||
      *ip == OP_YIELD_STAR_THROW ||
      *ip == OP_YIELD_STAR_RETURN
    );
    if (suspended_resume_kind == SV_RESUME_THROW && !yield_star_resume) {
      sv_err = js_throw(js, suspended_resume_value);
      goto sv_throw;
    }
    if (suspended_resume_kind == SV_RESUME_RETURN && !yield_star_resume) {
      vm->stack[vm->sp++] = suspended_resume_value;
      goto L_RETURN;
    }
    vm->stack[vm->sp++] = suspended_resume_value;
  }
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
  L_OBJECT:    { sv_op_object(vm, js, func, ip);        NEXT(1); }
  L_ARRAY:     { sv_op_array(vm, js, ip);     NEXT(3); }
  
  L_REGEXP:   { sv_op_regexp(vm, js);                    NEXT(1); }
  L_CLOSURE:  { VM_CHECK(sv_op_closure(vm, js, frame, func, ip));  NEXT(5); }

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

  L_GET_LOCAL:        { VM_CHECK(sv_op_get_local(vm, lp, js, frame, ip));   NEXT(3); }
  L_PUT_LOCAL:        { sv_op_put_local(vm, lp, frame, func, ip);           NEXT(3); }
  L_SET_LOCAL:        { sv_op_set_local(vm, lp, frame, func, ip);           NEXT(3); }
  L_GET_LOCAL8:       { VM_CHECK(sv_op_get_local8(vm, lp, js, frame, ip));  NEXT(2); }
  L_PUT_LOCAL8:       { sv_op_put_local8(vm, lp, frame, func, ip);          NEXT(2); }
  L_SET_LOCAL8:       { sv_op_set_local8(vm, lp, frame, func, ip);          NEXT(2); }
  L_SET_LOCAL_UNDEF:  { sv_op_set_local_undef(frame, lp, ip);               NEXT(3); }
  
  L_GET_LOCAL_CHK:  { VM_CHECK(sv_op_get_local_chk(vm, lp, js, frame, func, ip));  NEXT(7); }
  L_PUT_LOCAL_CHK:  { VM_CHECK(sv_op_put_local_chk(vm, lp, js, frame, func, ip));  NEXT(7); }
  L_GET_SLOT_RAW:   { VM_CHECK(sv_op_get_slot_raw(vm, js, frame, ip));             NEXT(3); }

  L_GET_ARG:  { VM_CHECK(sv_op_get_arg(vm, js, frame, ip));  NEXT(3); }
  L_PUT_ARG:  { sv_op_put_arg(vm, js, frame, ip);            NEXT(3); }
  L_SET_ARG:  { sv_op_set_arg(vm, js, frame, ip);            NEXT(3); }
  L_REST:     { sv_op_rest(vm, frame, js, ip);               NEXT(3); }

  L_GET_UPVAL:    { VM_CHECK(sv_op_get_upval(vm, frame, js, ip));  NEXT(3); }
  L_PUT_UPVAL:    { sv_op_put_upval(vm, frame, ip);                NEXT(3); }
  L_SET_UPVAL:    { sv_op_set_upval(vm, frame, ip);                NEXT(3); }
  L_CLOSE_UPVAL:  { VM_CHECK(sv_op_close_upval(vm, frame, ip));    NEXT(3); }

  L_GET_GLOBAL:        { VM_CHECK(sv_op_get_global(vm, js, func, ip));         NEXT(7); }
  L_GET_GLOBAL_UNDEF:  { sv_op_get_global_undef(vm, js, func, ip);             NEXT(7); }
  L_PUT_GLOBAL:        { VM_CHECK(sv_op_put_global(vm, js, frame, func, ip));  NEXT(5); }

  L_GET_FIELD:     { VM_CHECK(sv_op_get_field(vm, js, func, ip));   NEXT(7); }
  L_GET_FIELD2:    { VM_CHECK(sv_op_get_field2(vm, js, func, ip));  NEXT(7); }
  L_PUT_FIELD:     { VM_CHECK(sv_op_put_field(vm, js, func, ip));   NEXT(7); }
  L_GET_ELEM:      { VM_CHECK(sv_op_get_elem(vm, js, func, ip));    NEXT(1); }
  L_GET_ELEM2:     { VM_CHECK(sv_op_get_elem2(vm, js, func, ip));   NEXT(1); }
  L_PUT_ELEM:      { VM_CHECK(sv_op_put_elem(vm, js));              NEXT(1); }
  L_DEFINE_FIELD:  { sv_op_define_field(vm, js, func, ip);          NEXT(5); }
  L_GET_LENGTH:    { VM_CHECK(sv_op_get_length(vm, js));            NEXT(1); }

  L_GET_FIELD_OPT:  { VM_CHECK(sv_op_get_field_opt(vm, js, func, ip));  NEXT(5); }
  L_GET_ELEM_OPT:   { VM_CHECK(sv_op_get_elem_opt(vm, js, func, ip));   NEXT(1); }

  L_GET_PRIVATE:      { VM_CHECK(sv_op_get_private(vm, js));       NEXT(1); }
  L_GET_PRIVATE_OPT:  { VM_CHECK(sv_op_get_private_opt(vm, js));   NEXT(1); }
  L_PUT_PRIVATE:      { VM_CHECK(sv_op_put_private(vm, js));       NEXT(1); }
  L_DEF_PRIVATE:      { VM_CHECK(sv_op_def_private(vm, js, ip));   NEXT(2); }
  L_HAS_PRIVATE:      { VM_CHECK(sv_op_has_private(vm, js));       NEXT(1); }

  L_GET_SUPER:      { sv_op_get_super(vm, js);             NEXT(1); }
  L_GET_SUPER_VAL:  { sv_op_get_super_val(vm, js, frame);  NEXT(1); }
  L_PUT_SUPER_VAL:  { sv_op_put_super_val(vm, js);         NEXT(1); }

  L_ADD: {
    ant_value_t r = vm->stack[vm->sp - 1], l = vm->stack[vm->sp - 2];
    sv_tfb_record2(func, ip, l, r);
    if (__builtin_expect(vtype(l) == T_NUM && vtype(r) == T_NUM, 1)) {
      vm->sp--; vm->stack[vm->sp - 1] = tov(tod(l) + tod(r)); NEXT(1);
    }
    VM_CHECK(sv_op_add(vm, js)); NEXT(1);
  }

  L_ADD_NUM: {
    ant_value_t r = vm->stack[--vm->sp];
    ant_value_t l = vm->stack[vm->sp - 1];
    vm->stack[vm->sp - 1] = tov(tod(l) + tod(r));
    NEXT(1);
  }
  
  L_SUB: {
    ant_value_t r = vm->stack[vm->sp - 1], l = vm->stack[vm->sp - 2];
    sv_tfb_record2(func, ip, l, r);
    if (__builtin_expect(vtype(l) == T_NUM && vtype(r) == T_NUM, 1)) {
      vm->sp--; vm->stack[vm->sp - 1] = tov(tod(l) - tod(r)); NEXT(1);
    }
    VM_CHECK(sv_op_sub(vm, js)); NEXT(1);
  }

  L_SUB_NUM: {
    ant_value_t r = vm->stack[--vm->sp];
    ant_value_t l = vm->stack[vm->sp - 1];
    vm->stack[vm->sp - 1] = tov(tod(l) - tod(r));
    NEXT(1);
  }

  L_MUL_NUM: {
    ant_value_t r = vm->stack[--vm->sp];
    ant_value_t l = vm->stack[vm->sp - 1];
    vm->stack[vm->sp - 1] = tov(tod(l) * tod(r));
    NEXT(1);
  }

  L_DIV_NUM: {
    ant_value_t r = vm->stack[--vm->sp];
    ant_value_t l = vm->stack[vm->sp - 1];
    vm->stack[vm->sp - 1] = tov(tod(l) / tod(r));
    NEXT(1);
  }
  
  L_MUL:        { sv_tfb_record2(func, ip, vm->stack[vm->sp-2], vm->stack[vm->sp-1]); VM_CHECK(sv_op_mul(vm, js));                      NEXT(1); }
  L_DIV:        { sv_tfb_record2(func, ip, vm->stack[vm->sp-2], vm->stack[vm->sp-1]); VM_CHECK(sv_op_div(vm, js));                      NEXT(1); }
  L_MOD:        { sv_tfb_record2(func, ip, vm->stack[vm->sp-2], vm->stack[vm->sp-1]); VM_CHECK(sv_op_mod(vm, js));                      NEXT(1); }
  L_NEG:        { sv_tfb_record1(func, ip, vm->stack[vm->sp-1]); VM_CHECK(sv_op_neg(vm, js));                                           NEXT(1); }
  L_ADD_LOCAL:  { sv_tfb_record2(func, ip, lp[sv_get_u8(ip+1)], vm->stack[vm->sp-1]); VM_CHECK(sv_op_add_local(vm, lp, js, func, ip));  NEXT(2); }
  
  L_STR_APPEND_LOCAL: { VM_CHECK(sv_op_str_append_local(vm, js, frame, func, ip));           NEXT(3); }
  L_STR_ALC_SNAPSHOT: { VM_CHECK(sv_op_str_append_local_snapshot(vm, js, frame, func, ip));  NEXT(3); }
  L_STR_FLUSH_LOCAL:  { VM_CHECK(sv_op_str_flush_local(vm, js, frame, ip));                  NEXT(3); }

  L_EXP:        { VM_CHECK(sv_op_exp(vm, js));    NEXT(1); }
  L_UPLUS:      { VM_CHECK(sv_op_uplus(vm, js));  NEXT(1); }
  L_INC:        { sv_op_inc(vm);                  NEXT(1); }
  L_DEC:        { sv_op_dec(vm);                  NEXT(1); }
  L_POST_INC:   { sv_op_post_inc(vm);             NEXT(1); }
  L_POST_DEC:   { sv_op_post_dec(vm);             NEXT(1); }
  
  L_INC_LOCAL:  { VM_CHECK(sv_op_inc_local(lp, js, func, ip));  NEXT(2); }
  L_DEC_LOCAL:  { VM_CHECK(sv_op_dec_local(lp, js, func, ip));  NEXT(2); }

  L_EQ:   { sv_op_eq(vm, js);   NEXT(1); }
  L_NE:   { sv_op_ne(vm, js);   NEXT(1); }
  L_SEQ:  { sv_op_seq(vm, js);  NEXT(1); }
  L_SNE:  { sv_op_sne(vm, js);  NEXT(1); }
  
  L_LT: {
    ant_value_t r = vm->stack[vm->sp - 1], l = vm->stack[vm->sp - 2];
    sv_tfb_record2(func, ip, l, r);
    if (__builtin_expect(vtype(l) == T_NUM && vtype(r) == T_NUM, 1)) {
      vm->sp--; vm->stack[vm->sp - 1] = mkval(T_BOOL, tod(l) < tod(r)); NEXT(1);
    }
    VM_CHECK(sv_op_lt(vm, js)); NEXT(1);
  }
  
  L_LE:  { sv_tfb_record2(func, ip, vm->stack[vm->sp-2], vm->stack[vm->sp-1]); VM_CHECK(sv_op_le(vm, js));  NEXT(1); }
  L_GT:  { sv_tfb_record2(func, ip, vm->stack[vm->sp-2], vm->stack[vm->sp-1]); VM_CHECK(sv_op_gt(vm, js));  NEXT(1); }
  L_GE:  { sv_tfb_record2(func, ip, vm->stack[vm->sp-2], vm->stack[vm->sp-1]); VM_CHECK(sv_op_ge(vm, js));  NEXT(1); }
  
  L_INSTANCEOF:        { VM_CHECK(sv_op_instanceof(vm, js, func, ip));  NEXT(3); }
  L_IN:                { VM_CHECK(sv_op_in(vm, js));          NEXT(1); }
  L_IS_NULLISH:        { sv_op_is_nullish(vm);                NEXT(1); }
  L_IS_UNDEF_OR_NULL:  { sv_op_is_undef_or_null(vm);          NEXT(1); }

  L_BAND:  { VM_CHECK(sv_op_band(vm, js));  NEXT(1); }
  L_BOR:   { VM_CHECK(sv_op_bor(vm, js));   NEXT(1); }
  L_BXOR:  { VM_CHECK(sv_op_bxor(vm, js));  NEXT(1); }
  L_BNOT:  { VM_CHECK(sv_op_bnot(vm, js));  NEXT(1); }
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
      gc_maybe(js);
      JIT_OSR_BACK_EDGE();
    }
    DISPATCH();
  }
  
  L_JMP_FALSE: {
    uint8_t *prev = ip;
    ant_value_t v = vm->stack[--vm->sp];
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
      js->prop_refs_len = 0;
      JIT_OSR_BACK_EDGE();
    }
    DISPATCH();
  }
  
  L_JMP_TRUE: {
    uint8_t *prev = ip;
    ant_value_t v = vm->stack[--vm->sp];
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
      js->prop_refs_len = 0;
      JIT_OSR_BACK_EDGE();
    }
    DISPATCH();
  }
  
  L_JMP_FALSE_PEEK: { 
    uint8_t *prev = ip; ip = sv_op_jmp_false_peek(vm, js, ip);
    if (ip <= prev) {
      js->prop_refs_len = 0;
      JIT_OSR_BACK_EDGE();
    }
    DISPATCH();
  }
  
  L_JMP_TRUE_PEEK: { 
    uint8_t *prev = ip; ip = sv_op_jmp_true_peek(vm, js, ip);
    if (ip <= prev) {
      js->prop_refs_len = 0;
      JIT_OSR_BACK_EDGE();
    }
    DISPATCH();
  }
  
  L_JMP_NOT_NULLISH: { 
    uint8_t *prev = ip; ip = sv_op_jmp_not_nullish(vm, ip);
    if (ip <= prev) {
      js->prop_refs_len = 0;
      JIT_OSR_BACK_EDGE();
    }
    DISPATCH();
  }
   
  L_JMP8: {
    uint8_t *prev = ip; ip = sv_op_jmp8(ip);
    if (ip <= prev) {
      js->prop_refs_len = 0;
      JIT_OSR_BACK_EDGE();
    }
    DISPATCH();
  }
  
  L_JMP_FALSE8: { 
    uint8_t *prev = ip; ip = sv_op_jmp_false8(vm, js, ip);
    if (ip <= prev) {
      JIT_OSR_BACK_EDGE();
    }
    DISPATCH();
  }
  
  L_JMP_TRUE8:  { 
    uint8_t *prev = ip; ip = sv_op_jmp_true8(vm, js, ip);
    if (ip <= prev) {
      JIT_OSR_BACK_EDGE();
    }
    DISPATCH();
  }

  L_CALL: {
    uint16_t call_argc = sv_get_u16(ip + 1);
    ant_value_t *call_args = &vm->stack[vm->sp - call_argc];
    ant_value_t call_func = vm->stack[vm->sp - call_argc - 1];
    
    bool is_super_call =
      (vtype(frame->super_val) != T_UNDEF && call_func == frame->super_val);
    ant_value_t call_this = is_super_call ? frame->this : js_mkundef();

    if (!is_super_call && vtype(frame->new_target) == T_UNDEF && vtype(call_func) == T_FUNC) {
      sv_closure_t *closure = js_func_closure(call_func);
      if (closure->func != NULL) {
        if (closure->call_flags & (SV_CALL_HAS_BOUND_ARGS | SV_CALL_HAS_SUPER))
          goto call_fallback;
        if (closure->func->is_async || closure->func->is_generator) goto call_fallback;
        #ifdef ANT_JIT
        {
          ant_value_t jit_this = (
            closure->func->is_arrow || vtype(closure->bound_this) != T_UNDEF)
            ? closure->bound_this : js_mkundef();
          ant_value_t jit_result = sv_try_direct_closure_jit(
            vm, js, func, ip, frame, closure, jit_this, call_args, (int)call_argc);
          if (jit_result != SV_JIT_RETRY_INTERP) {
            vm->sp -= call_argc + 1;
            if (is_err(jit_result)) { sv_err = jit_result; goto sv_throw; }
            vm->stack[vm->sp++] = jit_result;
            ip = frame->ip;
            DISPATCH();
          }
        }
        #endif
        if (vm->fp + 1 >= vm->max_frames && !sv_vm_grow_frames(vm)) {
          sv_err = js_mkerr_typed(js, JS_ERR_RANGE | JS_ERR_NO_STACK, "Maximum call stack size exceeded");
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
        frame->arguments_obj = js_mkundef();
        ant_value_t *call_bp = NULL;
        ant_value_t *call_lp = NULL;
        ant_value_t call_stage_err = sv_stage_frame_args(
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
    ant_value_t super_this_c = call_this;
    ant_value_t call_result = sv_vm_call(
      vm, js, call_func, call_this, call_args, call_argc,
      is_super_call ? &super_this_c : NULL, is_super_call);
    sv_sync_frame_locals(vm, &frame, &func, &bp, &lp);
    vm->sp -= call_argc + 1;
    if (is_err(call_result)) { sv_err = call_result; goto sv_throw; }
    if (is_super_call)
      frame->this = is_object_type(call_result) ? call_result : super_this_c;
    vm->stack[vm->sp++] = call_result;
    NEXT(3);
  }

  L_CALL_METHOD: {
    uint16_t call_argc = sv_get_u16(ip + 1);
    ant_value_t *call_args = &vm->stack[vm->sp - call_argc];
    ant_value_t call_func = vm->stack[vm->sp - call_argc - 1];
    ant_value_t call_this = vm->stack[vm->sp - call_argc - 2];
    
    bool is_super_call =
      (vtype(frame->super_val) != T_UNDEF && call_func == frame->super_val);

    if (!is_super_call && vtype(frame->new_target) == T_UNDEF &&
        vtype(call_func) == T_FUNC) {
      sv_closure_t *closure = js_func_closure(call_func);
      if (closure->func != NULL) {
        if (closure->call_flags & (SV_CALL_HAS_BOUND_ARGS | SV_CALL_HAS_SUPER))
          goto call_method_fallback;
        if (closure->func->is_async || closure->func->is_generator) goto call_method_fallback;
        #ifdef ANT_JIT
        {
          ant_value_t jit_this = (
            closure->func->is_arrow || vtype(closure->bound_this) != T_UNDEF)
            ? closure->bound_this : call_this;
          ant_value_t jit_result = sv_try_direct_closure_jit(
            vm, js, func, ip, frame, closure, jit_this, call_args, (int)call_argc);
          if (jit_result != SV_JIT_RETRY_INTERP) {
            vm->sp -= call_argc + 2;
            if (is_err(jit_result)) { sv_err = jit_result; goto sv_throw; }
            vm->stack[vm->sp++] = jit_result;
            ip = frame->ip;
            DISPATCH();
          }
        }
        #endif
        if (vm->fp + 1 >= vm->max_frames && !sv_vm_grow_frames(vm)) {
          sv_err = js_mkerr_typed(js, JS_ERR_RANGE | JS_ERR_NO_STACK, "Maximum call stack size exceeded");
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
        frame->arguments_obj = js_mkundef();
        ant_value_t *call_bp = NULL;
        ant_value_t *call_lp = NULL;
        ant_value_t call_stage_err = sv_stage_frame_args(
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
    ant_value_t super_this_cm = call_this;
    ant_value_t call_result = sv_vm_call(
      vm, js, call_func, call_this, call_args, call_argc,
      is_super_call ? &super_this_cm : NULL, is_super_call);
    sv_sync_frame_locals(vm, &frame, &func, &bp, &lp);
    vm->sp -= call_argc + 2;
    if (is_err(call_result)) { sv_err = call_result; goto sv_throw; }
    if (is_super_call)
      frame->this = is_object_type(call_result) ? call_result : super_this_cm;
    vm->stack[vm->sp++] = call_result;
    NEXT(3);
  }

  L_CALL_ARRAY_INCLUDES: {
    uint16_t call_argc = sv_get_u16(ip + 1);
    ant_value_t *call_args = &vm->stack[vm->sp - call_argc];
    ant_value_t call_func = vm->stack[vm->sp - call_argc - 1];
    ant_value_t call_this = vm->stack[vm->sp - call_argc - 2];
    ant_value_t call_result;

    frame->ip = ip;
    if (js_is_array_includes_builtin(call_func)) {
      call_result = js_array_includes_call(js, call_this, call_args, call_argc);
    } else call_result = sv_vm_call(vm, js, call_func, call_this, call_args, call_argc, NULL, false);
    sv_sync_frame_locals(vm, &frame, &func, &bp, &lp);
    
    vm->sp -= call_argc + 2;
    if (is_err(call_result)) { sv_err = call_result; goto sv_throw; }
    vm->stack[vm->sp++] = call_result;
    
    NEXT(3);
  }

  L_CALL_IS_PROTO: {
    ant_value_t call_arg  = vm->stack[vm->sp - 1];
    ant_value_t call_func = vm->stack[vm->sp - 2];
    ant_value_t call_this = vm->stack[vm->sp - 3];
    ant_value_t call_result;

    if (
      vtype(call_func) == T_CFUNC &&
      js_cfunc_same_entrypoint(call_func, builtin_object_isPrototypeOf)
    ) call_result = sv_isproto_ic_eval(js, call_this, call_arg, func, ip); else {
      ant_value_t call_args[1] = { call_arg };
      frame->ip = ip;
      call_result = sv_vm_call(vm, js, call_func, call_this, call_args, 1, NULL, false);
      sv_sync_frame_locals(vm, &frame, &func, &bp, &lp);
    }

    vm->sp -= 3;
    if (is_err(call_result)) { sv_err = call_result; goto sv_throw; }
    vm->stack[vm->sp++] = call_result;
    NEXT(3);
  }

  L_RE_EXEC_TRUTHY: {
    ant_value_t call_arg  = vm->stack[vm->sp - 1];
    ant_value_t call_func = vm->stack[vm->sp - 2];
    ant_value_t call_this = vm->stack[vm->sp - 3];
    ant_value_t call_result;

    if (!regexp_exec_truthy_try_fast(js, call_func, call_this, call_arg, &call_result)) {
      ant_value_t call_args[1] = { call_arg }; frame->ip = ip;
      ant_value_t raw_result = sv_vm_call(vm, js, call_func, call_this, call_args, 1, NULL, false);
      sv_sync_frame_locals(vm, &frame, &func, &bp, &lp);
      if (is_err(raw_result)) call_result = raw_result;
      else call_result = mkval(T_BOOL, js_truthy(js, raw_result) ? 1 : 0);
    }
    
    vm->sp -= 3;
    if (is_err(call_result)) { sv_err = call_result; goto sv_throw; }
    vm->stack[vm->sp++] = call_result;
    NEXT(1);
  }

  L_TAIL_CALL: {
    uint16_t call_argc = sv_get_u16(ip + 1);
    ant_value_t call_func = vm->stack[vm->sp - call_argc - 1];
    tc_this = js_mkundef();

    if (vm->handler_depth == frame->handler_base &&
        vtype(frame->new_target) == T_UNDEF &&
        vtype(call_func) == T_FUNC) {
      sv_closure_t *closure = js_func_closure(call_func);
      if (closure->func != NULL) {
        if (!closure->func->is_async && !closure->func->is_generator &&
            !(closure->call_flags & (SV_CALL_HAS_BOUND_ARGS | SV_CALL_HAS_SUPER))) {
          if (closure->func->is_arrow || vtype(closure->bound_this) != T_UNDEF)
            tc_this = closure->bound_this;
          goto tail_call_inline;
        }
      }
    }
    ant_value_t *call_args = &vm->stack[vm->sp - call_argc];
    frame->ip = ip;
    ant_value_t call_result = sv_vm_call(
      vm, js, call_func, tc_this, call_args, call_argc, NULL, false);
    sv_sync_frame_locals(vm, &frame, &func, &bp, &lp);
    vm->sp -= call_argc + 1;
    if (is_err(call_result)) { sv_err = call_result; goto sv_throw; }
    vm->stack[vm->sp++] = call_result;
    goto L_RETURN;
  }

  L_TAIL_CALL_METHOD: {
    uint16_t call_argc = sv_get_u16(ip + 1);
    ant_value_t call_func = vm->stack[vm->sp - call_argc - 1];
    tc_this = vm->stack[vm->sp - call_argc - 2];

    if (vm->handler_depth == frame->handler_base &&
        vtype(frame->new_target) == T_UNDEF &&
        vtype(call_func) == T_FUNC) {
      sv_closure_t *closure = js_func_closure(call_func);
      if (closure->func != NULL) {
        if (!closure->func->is_async && !closure->func->is_generator &&
            !(closure->call_flags & (SV_CALL_HAS_BOUND_ARGS | SV_CALL_HAS_SUPER))) {
          if (closure->func->is_arrow || vtype(closure->bound_this) != T_UNDEF)
            tc_this = closure->bound_this;
          goto tail_call_inline;
        }
      }
    }
    ant_value_t *call_args = &vm->stack[vm->sp - call_argc];
    ant_value_t call_result = sv_vm_call(
      vm, js, call_func, tc_this, call_args, call_argc, NULL, false);
    sv_sync_frame_locals(vm, &frame, &func, &bp, &lp);
    vm->sp -= call_argc + 2;
    if (is_err(call_result)) { sv_err = call_result; goto sv_throw; }
    vm->stack[vm->sp++] = call_result;
    goto L_RETURN;
  }

  tail_call_inline: {
    uint16_t call_argc = sv_get_u16(ip + 1);
    ant_value_t *call_args = &vm->stack[vm->sp - call_argc];
    ant_value_t call_func = vm->stack[vm->sp - call_argc - 1];
    sv_closure_t *closure = js_func_closure(call_func);
    if (frame->bp && vm->open_upvalues) sv_close_upvalues_from_slot(vm, frame->bp);
    sv_drop_frame_runtime_state(js, frame);
    vm->sp = frame->prev_sp;
    int arg_slots = (
      (int)call_argc > closure->func->param_count)
      ? (int)call_argc : closure->func->param_count;
    int need = arg_slots + closure->func->max_locals;
    ant_value_t *base = &vm->stack[vm->sp];
    memmove(base, call_args, (size_t)call_argc * sizeof(ant_value_t));
    for (int i = (int)call_argc; i < arg_slots; i++) base[i] = js_mkundef();
    ant_value_t *new_lp = base + arg_slots;
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
    frame->arguments_obj = js_mkundef();
    frame->bp = base;
    frame->lp = new_lp;
    frame->upvalues = closure->upvalues;
    frame->upvalue_count = closure->func->upvalue_count;
    bp = frame->bp;
    lp = frame->lp;
    ip = func->code;
    DISPATCH();
  }
  
  L_NEW:       { VM_CHECK(sv_op_new(vm, js, ip));          NEXT(3); }
  L_APPLY:       { VM_CHECK(sv_op_apply(vm, js, ip));              NEXT(3); }
  L_SUPER_APPLY: { VM_CHECK(sv_op_super_apply(vm, js, frame, ip)); NEXT(3); }
  L_NEW_APPLY:   { VM_CHECK(sv_op_new_apply(vm, js, ip));          NEXT(3); }
  L_EVAL:        { VM_CHECK(sv_op_eval(vm, js, frame, ip));        NEXT(5); }

  // TODO: make the methods below DRY
  L_RETURN: {
    ant_value_t r = vm->stack[--vm->sp];
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
    sv_drop_frame_runtime_state(js, frame);
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
    ant_value_t r = js_mkundef();
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
    sv_drop_frame_runtime_state(js, frame);
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
    ant_value_t r = vm->stack[--vm->sp];
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
    sv_drop_frame_runtime_state(js, frame);
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
  L_NIP_CATCH:   { sv_op_nip_catch(vm);                  NEXT(1); }

  L_FINALLY_RET: {
    uint8_t *resume_ip = NULL;
    ant_value_t completion = js_mkundef();
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
      sv_drop_frame_runtime_state(js, frame);
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
  
  L_USING_PUSH:                     { VM_CHECK(sv_op_using_push(vm, js, false));          NEXT(1); }
  L_USING_PUSH_ASYNC:               { VM_CHECK(sv_op_using_push(vm, js, true));           NEXT(1); }
  L_DISPOSE_RESOURCE:               { VM_CHECK(sv_op_dispose_resource(vm, js, false));    NEXT(1); }
  L_DISPOSE_RESOURCE_ASYNC:         { VM_CHECK(sv_op_dispose_resource(vm, js, true));     NEXT(1); }
  L_USING_DISPOSE:                  { VM_CHECK(sv_op_using_dispose(vm, js, false, false)); NEXT(1); }
  L_USING_DISPOSE_ASYNC:            { VM_CHECK(sv_op_using_dispose(vm, js, true, false));  NEXT(1); }
  L_USING_DISPOSE_SUPPRESSED:       { VM_CHECK(sv_op_using_dispose(vm, js, false, true));  NEXT(1); }
  L_USING_DISPOSE_ASYNC_SUPPRESSED: { VM_CHECK(sv_op_using_dispose(vm, js, true, true));   NEXT(1); }

  L_FOR_IN:           { VM_CHECK(sv_op_for_in(vm, js));           NEXT(1); }
  L_FOR_OF:           { VM_CHECK(sv_op_for_of(vm, js));           NEXT(1); }
  L_FOR_AWAIT_OF:     { VM_CHECK(sv_op_for_await_of(vm, js));     NEXT(1); }
  L_ITER_NEXT:        { VM_CHECK(sv_op_iter_next(vm, js, ip));    NEXT(2); }
  L_ITER_GET_VALUE:   { sv_op_iter_get_value(vm, js);             NEXT(1); }
  L_ITER_CLOSE:       { sv_op_iter_close(vm, js);                 NEXT(1); }
  L_ITER_CALL:        { VM_CHECK(sv_op_iter_call(vm, js, ip));    NEXT(2); }
  
  L_AWAIT_ITER_NEXT:  {
    sv_await_result_t await_result = sv_op_await_iter_next(vm, js);
    if (await_result.state == SV_AWAIT_ERROR) {
      sv_err = await_result.value;
      goto sv_throw;
    }
    if (await_result.state == SV_AWAIT_SUSPENDED) {
      if (await_result.handoff) vm_result = js_mkundef();
      goto sv_leave;
    }
    NEXT(1);
  }
  
  L_DESTRUCTURE_INIT: { VM_CHECK(sv_op_destructure_init(vm, js)); NEXT(1); }
  L_DESTRUCTURE_NEXT: { VM_CHECK(sv_op_destructure_next(vm, js)); NEXT(1); }
  L_DESTRUCTURE_REST: { VM_CHECK(sv_op_destructure_rest(vm, js)); NEXT(1); }
  L_DESTRUCTURE_CLOSE:{ sv_op_destructure_close(vm, js);          NEXT(1); }

  L_AWAIT: {
    ant_value_t await_val = vm->stack[--vm->sp];
    frame->ip = ip + 1;
    vm->suspended_entry_fp = entry_fp;
    vm->suspended_saved_fp = entry_fp - 1;
    
    sv_await_result_t await_result = sv_await_value(vm, js, await_val);
    if (await_result.state == SV_AWAIT_SUSPENDED && await_result.handoff) {
      vm->suspended_entry_fp = -1;
      vm->suspended_saved_fp = -1;
      vm_result = js_mkundef();
      goto sv_leave;
    }
    
    if (await_result.state == SV_AWAIT_SUSPENDED) goto sv_leave;
    vm->suspended_entry_fp = -1;
    vm->suspended_saved_fp = -1;
    
    if (await_result.state == SV_AWAIT_ERROR) {
      sv_err = await_result.value;
      goto sv_throw;
    }
    
    vm->stack[vm->sp++] = await_result.value;
    NEXT(1);
  }
  
  L_YIELD: {
    ant_value_t yielded = vm->stack[--vm->sp];
    coroutine_t *coro = js->active_async_coro;
    if (!coro || coro->type != CORO_GENERATOR) {
      sv_err = js_mkerr(js, "yield can only be used inside generator functions");
      goto sv_throw;
    }
    coro->yield_value = yielded;
    coro->did_suspend = true;
    vm->suspended = true;
    vm->suspended_entry_fp = entry_fp;
    vm->suspended_saved_fp = entry_fp - 1;
    frame->ip = ip + 1;
    vm_result = yielded;
    goto sv_leave;
  }
  
  L_YIELD_STAR_INIT: {
    uint16_t base = sv_get_u16(ip + 1);
    ant_value_t iterable = vm->stack[--vm->sp];
    vm->stack[vm->sp++] = iterable;
    VM_CHECK(sv_op_for_of(vm, js));
    sv_yield_star_store_state(vm, lp, base);
    vm->sp -= 3;
    lp[base + 3] = js_true;
    NEXT(3);
  }

  L_YIELD_STAR_NEXT:
  L_YIELD_STAR_THROW:
  L_YIELD_STAR_RETURN: {
    coroutine_t *coro = js->active_async_coro;
    if (!coro || coro->type != CORO_GENERATOR) {
      sv_err = js_mkerr(js, "yield can only be used inside generator functions");
      goto sv_throw;
    }

    uint16_t base = sv_get_u16(ip + 1);
    ant_value_t resume_value = vm->stack[--vm->sp];
    ant_value_t yielded = js_mkundef();
    bool done = false;

    if (*ip == OP_YIELD_STAR_THROW || suspended_resume_kind == SV_RESUME_THROW) {
      VM_CHECK(sv_yield_star_throw(vm, js, lp, base, resume_value, &yielded, &done));
    } else if (*ip == OP_YIELD_STAR_RETURN || suspended_resume_kind == SV_RESUME_RETURN) {
      VM_CHECK(sv_yield_star_return(vm, js, lp, base, resume_value, &yielded, &done));
      if (done) {
        sv_yield_star_clear_state(js, lp, base);
        vm->stack[vm->sp++] = yielded;
        goto L_RETURN;
      }
    } else VM_CHECK(sv_yield_star_next(vm, js, lp, base, resume_value, &yielded, &done));

    if (done) {
      sv_yield_star_clear_state(js, lp, base);
      vm->stack[vm->sp++] = yielded;
      NEXT(3);
    }

    coro->yield_value = yielded;
    coro->did_suspend = true;
    vm->suspended = true;
    vm->suspended_entry_fp = entry_fp;
    vm->suspended_saved_fp = entry_fp - 1;
    frame->ip = ip;
    vm_result = yielded;
    goto sv_leave;
  }

  L_SPREAD:              { VM_CHECK(sv_op_spread(vm, js));         NEXT(1); }
  L_DEFINE_METHOD:       { sv_op_define_method(vm, js, func, ip);  NEXT(6); }
  L_DEFINE_METHOD_COMP:  { sv_op_define_method_comp(vm, js, ip);   NEXT(2); }
  L_SET_NAME:            { sv_op_set_name(vm, js, func, ip);       NEXT(5); }
  L_SET_NAME_COMP:       { sv_op_set_name_comp(vm, js);            NEXT(1); }
  L_SET_PROTO:           { sv_op_set_proto(vm, js);                NEXT(1); }
  L_SET_HOME_OBJ:        { sv_op_set_home_obj(vm, js);             NEXT(1); }
  L_APPEND:              { sv_op_append(vm, js);                   NEXT(1); }
  L_COPY_DATA_PROPS:     { sv_op_copy_data_props(vm, js, ip);      NEXT(2); }

  L_DEFINE_CLASS:       { sv_op_define_class(vm, js, func, ip);       NEXT(14); }
  L_DEFINE_CLASS_COMP:  { sv_op_define_class_comp(vm, js, func, ip);  NEXT(14); }

  L_TO_OBJECT:   { VM_CHECK(sv_op_to_object(vm, js));  NEXT(1); }
  L_TO_PROPKEY:  { sv_op_to_propkey(vm, js);           NEXT(1); }
  L_IS_UNDEF:    { sv_op_is_undef(vm);                 NEXT(1); }
  L_IS_NULL:     { sv_op_is_null(vm);                  NEXT(1); }

  L_IMPORT:          { VM_CHECK(sv_op_import(vm, js));                 NEXT(1); }
  L_IMPORT_SYNC:     { VM_CHECK(sv_op_import_sync(vm, js));            NEXT(1); }
  L_IMPORT_DEFAULT:  { sv_op_import_default(vm, js);                   NEXT(1); }
  L_IMPORT_NAMED:    { VM_CHECK(sv_op_import_named(vm, js, func, ip)); NEXT(5); }
  L_EXPORT:          { VM_CHECK(sv_op_export(vm, js, func, ip));       NEXT(5); }
  L_EXPORT_ALL:      { VM_CHECK(sv_op_export_all(vm, js));             NEXT(1); }

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
  if (vm->suspended) {
    if (js->vm_exec_depth > 0) js->vm_exec_depth--;
    return vm_result;
  }
  if (vm->open_upvalues) {
  for (int f = vm->fp; f >= entry_fp; f--) {
    ant_value_t *drop_bp = vm->frames[f].bp;
    if (drop_bp) sv_close_upvalues_from_slot(vm, drop_bp);
  }}
  for (int f = vm->fp; f >= entry_fp; f--)
    sv_drop_frame_runtime_state(js, &vm->frames[f]);
  vm->fp = entry_fp;
  vm->sp = vm->frames[entry_fp].prev_sp;
  vm->handler_depth = vm->frames[entry_fp].handler_base;
  if (js->vm_exec_depth > 0) js->vm_exec_depth--;

  return vm_result;

  #undef DISPATCH
  #undef NEXT
  #undef VM_CHECK

  // TODO: use entry_bp/frame->bp
  //       and sv_op_size values
  (void)bp;
  (void)sv_op_size;
}

ant_value_t sv_resume_suspended(sv_vm_t *vm) {
  if (!vm || !vm->suspended || !vm->suspended_resume_pending || vm->fp < 0)
    return mkval(T_ERR, 0);

  // crash-resistance for missing frames 
  if (vm->suspended_entry_fp < 0 || vm->suspended_entry_fp > vm->fp) {
    vm->suspended = false;
    vm->suspended_resume_pending = false;
    vm->suspended_resume_is_error = false;
    vm->suspended_resume_kind = SV_RESUME_NEXT;
    vm->suspended_resume_value = js_mkundef();
    vm->suspended_entry_fp = -1;
    vm->suspended_saved_fp = -1;
    
    return vm->js
      ? js_mkerr(vm->js, "invalid suspended entry frame")
      : mkval(T_ERR, 0);
  }

  int saved_fp = vm->suspended_saved_fp;
  sv_frame_t *frame = &vm->frames[vm->fp];
  
  if (!vm->js || !frame->func || !frame->ip) {
    vm->suspended = false;
    vm->suspended_resume_pending = false;
    vm->suspended_resume_is_error = false;
    vm->suspended_resume_kind = SV_RESUME_NEXT;
    vm->suspended_resume_value = js_mkundef();
    vm->suspended_entry_fp = -1;
    vm->suspended_saved_fp = -1;
    
    return vm->js
      ? js_mkerr(vm->js, "invalid suspended frame state")
      : mkval(T_ERR, 0);
  }

  ant_value_t result = sv_execute_frame(
    vm, frame->func, frame->this, frame->super_val, NULL, frame->argc
  );

  if (!vm->suspended) {
    vm->fp = saved_fp;
    vm->suspended_entry_fp = -1;
    vm->suspended_saved_fp = -1;
  }

  return result;
}
