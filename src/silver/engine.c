#include "gc.h"
#include "realm.h"
#include "errors.h"
#include "interrupt.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif !defined(ANT_WASM_EMBED)
#include <sys/mman.h>
#endif

#include "silver/call.h"
#include "silver/feedback.h"
#include "silver/jit.h"
#include "modules/promise.h"
#include "modules/regex.h"
#include "silver/glue.h"
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
#include "ops/generators.h"
#include "ops/using.h"
#include "ops/objects.h"
#include "ops/coercion.h"

#ifdef ANT_WASM_EMBED
#include "wasm_embed.h"
#endif

static constexpr size_t SV_VM_GUARD_SIZE = 65536;
static constexpr size_t SV_STACK_RESERVE = (size_t)SV_STACK_HARD_MAX * sizeof(ant_value_t);
static constexpr size_t SV_FRAMES_RESERVE = (size_t)SV_FRAMES_HARD_MAX * sizeof(sv_frame_t);
static constexpr size_t SV_VM_RESERVE = SV_STACK_RESERVE + SV_VM_GUARD_SIZE + SV_FRAMES_RESERVE;

static sv_gf_mega_cache_t *sv_gf_mega_new(ant_t *js) {
  if (!sv_ic_shape_ref_reserve(js, SV_GF_MEGA_PRIMARY + SV_GF_MEGA_SECONDARY)) return NULL;
  
  sv_gf_mega_cache_t *cache = calloc(1, sizeof(*cache));
  if (!cache) return NULL;
  
  for (unsigned i = 0; i < SV_GF_MEGA_PRIMARY; i++)
    sv_ic_shape_ref_register(js, &cache->primary[i].shape);
  
  for (unsigned i = 0; i < SV_GF_MEGA_SECONDARY; i++)
    sv_ic_shape_ref_register(js, &cache->secondary[i].shape);
  
  return cache;
}

sv_gf_mega_cache_t *sv_gf_mega_ensure(ant_t *js) {
  if (!js->ic.gf_mega) js->ic.gf_mega = sv_gf_mega_new(js);
  return js->ic.gf_mega;
}

sv_gf_mega_cache_t *sv_gf_mega_keyed_ensure(ant_t *js) {
  if (!js->ic.gf_mega_keyed) js->ic.gf_mega_keyed = sv_gf_mega_new(js);
  return js->ic.gf_mega_keyed;
}

static void sv_keyed_put_reset(ant_t *js, sv_ic_entry_t *ic) {
  sv_pf_poly_free(js, ic);
  
  ant_shape_t *shapes[3] = {
    (ic->shape_ref_mask & SV_IC_SHAPE_REF_CACHED) ? ic->cached_shape : NULL,
    (ic->shape_ref_mask & SV_IC_SHAPE_REF_ADD_FROM) ? ic->guard.add.from_shape : NULL,
    (ic->shape_ref_mask & SV_IC_SHAPE_REF_ADD_TO) ? ic->guard.add.to_shape : NULL,
  };
  
  for (int i = 0; i < 3; i++) if (shapes[i]) ant_shape_release(shapes[i]);
  *ic = (sv_ic_entry_t){.shape_ref_mask = SV_IC_UNIT_OWNED};
}

sv_ic_entry_t *sv_keyed_put_entry(ant_t *js, const ant_shape_t *shape, const char *key) {
  sv_keyed_put_cache_t *cache = js->ic.keyed_put;
  if (!cache) {
    if (!(cache = calloc(1, sizeof(*cache)))) return NULL;
    for (unsigned i = 0; i < SV_KEYED_PUT_ENTRIES; i++) cache->entries[i].shape_ref_mask = SV_IC_UNIT_OWNED;
    js->ic.keyed_put = cache;
  }
  
  uintptr_t h = ((uintptr_t)shape >> 5) ^ ((uintptr_t)key >> 3);
  unsigned index = (unsigned)((h ^ (h >> 9)) & (SV_KEYED_PUT_ENTRIES - 1));
  
  sv_ic_entry_t *ic = &cache->entries[index];
  if (cache->shapes[index] == shape && cache->keys[index] == key) return ic;
  
  if (cache->seen_shapes[index] != shape || cache->seen_keys[index] != key) {
    cache->seen_shapes[index] = shape;
    cache->seen_keys[index] = key;
    return NULL;
  }
  
  sv_keyed_put_reset(js, ic);
  cache->shapes[index] = shape;
  cache->keys[index] = key;
  
  return ic;
}

void sv_keyed_put_clear(ant_t *js) {
  sv_keyed_put_cache_t *cache = js->ic.keyed_put;
  if (!cache) return;
  for (unsigned i = 0; i < SV_KEYED_PUT_ENTRIES; i++) {
    sv_keyed_put_reset(js, &cache->entries[i]);
    cache->shapes[i] = cache->seen_shapes[i] = NULL;
    cache->keys[i] = cache->seen_keys[i] = NULL;
  }
}

void sv_keyed_put_destroy(ant_t *js) {
  sv_keyed_put_cache_t *cache = js->ic.keyed_put;
  if (!cache) return;
  for (unsigned i = 0; i < SV_KEYED_PUT_ENTRIES; i++) sv_keyed_put_reset(js, &cache->entries[i]);
  free(cache);
  js->ic.keyed_put = NULL;
}

static void sv_gf_mega_clear_entries(sv_gf_mega_entry_t *entries, unsigned count) {
  for (unsigned i = 0; i < count; i++) {
    if (!entries[i].shape) continue;
    ant_shape_release(entries[i].shape);
    entries[i] = (sv_gf_mega_entry_t){0};
  }
}

void sv_gf_mega_clear(ant_t *js) {
  sv_gf_mega_cache_t *caches[] = {js->ic.gf_mega, js->ic.gf_mega_keyed};
  for (size_t i = 0; i < sizeof(caches) / sizeof(caches[0]); i++) {
    if (!caches[i]) continue;
    sv_gf_mega_clear_entries(caches[i]->primary, SV_GF_MEGA_PRIMARY);
    sv_gf_mega_clear_entries(caches[i]->secondary, SV_GF_MEGA_SECONDARY);
  }
}

static void sv_ic_identities_reset_arena(ant_fixed_arena_t *oa) {
  for (uint32_t chunk = 0; chunk < oa->chunk_count; chunk++) {
    uint8_t *base = oa->chunks[chunk].base;
    size_t used = fixed_arena_chunk_used(oa, chunk);
    
    for (size_t off = 0; off < used; off += oa->elem_size) {
      ant_object_t *obj = (ant_object_t *)(base + off);
      if (obj->mark_epoch != ANT_GC_DEAD) obj->ic_identity = 0;
    }
  }
}

__attribute__((noinline, cold)) void sv_ic_identities_reset(ant_t *js) {
  sv_ic_identities_reset_arena(&js->obj_arena);
  sv_ic_identities_reset_arena(&js->obj_wide_arena);
  sv_ic_identities_reset_arena(&js->obj_large_arena);
}

#define SV_POLY_LINK(list, block) do { \
  (block)->prev = NULL;                \
  (block)->link_next = (list);         \
  if (list) (list)->prev = (block);    \
  (list) = (block);                    \
} while (0)

#define SV_POLY_UNLINK(list, block) do {                            \
  if ((block)->prev) (block)->prev->link_next = (block)->link_next; \
  else (list) = (block)->link_next;                                 \
  if ((block)->link_next) (block)->link_next->prev = (block)->prev; \
} while (0)

sv_gf_poly_t *sv_gf_poly_new(ant_t *js, sv_ic_entry_t *ic) {
  sv_gf_poly_t *poly = calloc(1, sizeof(*poly));
  if (!poly) return NULL;
  
  poly->kept_epoch = js->ic.epoch;
  SV_POLY_LINK(js->ic.gf_polys, poly);
  
  ic->guard.get.poly = poly;
  ic->shape_ref_mask |= SV_IC_HAS_GET_POLY;
  
  return poly;
}

sv_pf_poly_t *sv_pf_poly_new(ant_t *js, sv_ic_entry_t *ic) {
  sv_pf_poly_t *poly = calloc(1, sizeof(*poly));
  if (!poly) return NULL;
  
  poly->kept_epoch = js->ic.epoch;
  SV_POLY_LINK(js->ic.pf_polys, poly);
  
  ic->put_poly = poly;
  ic->shape_ref_mask |= SV_IC_HAS_PUT_POLY;
  
  return poly;
}

static void sv_gf_poly_release(sv_gf_poly_t *poly) {
  for (unsigned i = 0; i < SV_GF_POLY_WAYS; i++) {
    sv_gf_poly_entry_t *e = &poly->entries[i];
    if (e->shape) ant_shape_release(e->shape);
    e->shape = NULL;
  }
}

static void sv_pf_poly_release(sv_pf_poly_t *poly) {
  for (unsigned i = 0; i < SV_GF_POLY_WAYS; i++) {
    sv_pf_poly_entry_t *e = &poly->entries[i];
    if (e->shape) ant_shape_release(e->shape);
    if (e->to_shape) ant_shape_release(e->to_shape);
    e->shape = e->to_shape = NULL;
  }
}

static void sv_gf_poly_destroy(sv_gf_poly_t *poly) {
  sv_gf_poly_release(poly);
  free(poly);
}

static void sv_pf_poly_destroy(sv_pf_poly_t *poly) {
  sv_pf_poly_release(poly);
  free(poly);
}

void sv_gf_poly_free(ant_t *js, sv_ic_entry_t *ic) {
  if (!(ic->shape_ref_mask & SV_IC_HAS_GET_POLY)) return;
  sv_gf_poly_t *poly = ic->guard.get.poly;
  
  ic->guard.get.poly = NULL;
  ic->shape_ref_mask &= (sv_ic_flags_t)~SV_IC_HAS_GET_POLY;
  if (!poly) return;
  
  SV_POLY_UNLINK(js->ic.gf_polys, poly);
  sv_gf_poly_destroy(poly);
}

void sv_pf_poly_free(ant_t *js, sv_ic_entry_t *ic) {
  if (!(ic->shape_ref_mask & SV_IC_HAS_PUT_POLY)) return;
  sv_pf_poly_t *poly = ic->put_poly;
  
  ic->put_poly = NULL;
  ic->shape_ref_mask &= (sv_ic_flags_t)~SV_IC_HAS_PUT_POLY;
  if (!poly) return;
  
  SV_POLY_UNLINK(js->ic.pf_polys, poly);
  sv_pf_poly_destroy(poly);
}

void sv_ic_polys_release_shapes(ant_t *js) {
  for (sv_gf_poly_t *poly = js->ic.gf_polys; poly; poly = poly->link_next) sv_gf_poly_release(poly);
  for (sv_pf_poly_t *poly = js->ic.pf_polys; poly; poly = poly->link_next) sv_pf_poly_release(poly);
}

void sv_ic_polys_cleanup(ant_t *js) {
  for (sv_gf_poly_t *poly = js->ic.gf_polys, *next; poly; poly = next) {
    next = poly->link_next;
    sv_gf_poly_destroy(poly);
  }
  
  for (sv_pf_poly_t *poly = js->ic.pf_polys, *next; poly; poly = next) {
    next = poly->link_next;
    sv_pf_poly_destroy(poly);
  }
  
  js->ic.gf_polys = NULL;
  js->ic.pf_polys = NULL;
}

#undef SV_POLY_LINK
#undef SV_POLY_UNLINK

bool sv_ic_shape_ref_reserve(ant_t *js, size_t count) {
  if (!js) return false;
  if (js->ic.shape_ref_cap - js->ic.shape_ref_len >= count) return true;

  size_t cap = js->ic.shape_ref_cap ? js->ic.shape_ref_cap : 64u;
  while (cap - js->ic.shape_ref_len < count) cap *= 2u;
  
  ant_shape_t ***slots = realloc(js->ic.shape_ref_slots, cap * sizeof(*slots));
  if (!slots) return false;
  
  js->ic.shape_ref_slots = slots;
  js->ic.shape_ref_cap = cap;
  
  return true;
}

bool sv_ic_shape_ref_register(ant_t *js, ant_shape_t **slot) {
  if (!js || !slot) goto fail;
  if (!sv_ic_shape_ref_reserve(js, 1)) goto fail;

  js->ic.shape_ref_slots[js->ic.shape_ref_len++] = slot;
  return true;

fail:
  // unregistered slots own no reference. null shapes keeps the cache
  // unusable even if its caller writes a new index and epoch after failure.
  if (slot) *slot = NULL;
  return false;
}

void sv_ic_shape_refs_mark(ant_t *js) {
  for (size_t i = 0; i < js->ic.shape_ref_len; i++) {
    ant_shape_t **slot = js->ic.shape_ref_slots[i];
    if (slot && *slot) ant_gc_shapes_mark(js, *slot);
  }
}

void sv_ic_shape_refs_cleanup(ant_t *js) {
  if (!js) return;

  for (size_t i = 0; i < js->ic.shape_ref_len; i++) {
    ant_shape_t **slot = js->ic.shape_ref_slots[i];
    if (!slot || !*slot) continue;
    ant_shape_t *shape = *slot;
    *slot = NULL;
    ant_shape_release(shape);
  }

  free(js->ic.shape_ref_slots);
  js->ic.shape_ref_slots = NULL;
  js->ic.shape_ref_len = js->ic.shape_ref_cap = 0;
}

static void *sv_vm_reserve_storage(void) {
#ifdef ANT_WASM_EMBED
  return calloc(1, SV_VM_RESERVE);
#elif defined(_WIN32)
  return VirtualAlloc(NULL, SV_VM_RESERVE, MEM_RESERVE, PAGE_READWRITE);
#else
  int flags = MAP_PRIVATE | MAP_ANON;
#ifdef MAP_NORESERVE
  flags |= MAP_NORESERVE;
#endif
  void *p = mmap(NULL, SV_VM_RESERVE, PROT_READ | PROT_WRITE, flags, -1, 0);
  if (p == MAP_FAILED) return NULL;
  if (mprotect((char *)p + SV_STACK_RESERVE, SV_VM_GUARD_SIZE, PROT_NONE) != 0) {
    munmap(p, SV_VM_RESERVE);
    return NULL;
  }
  return p;
#endif
}

static void sv_vm_release_storage(void *base) {
  if (!base) return;
#ifdef ANT_WASM_EMBED
  free(base);
#elif defined(_WIN32)
  VirtualFree(base, 0, MEM_RELEASE);
#else
  munmap(base, SV_VM_RESERVE);
#endif
}

static bool sv_vm_commit_storage(void *addr, size_t bytes) {
#ifdef ANT_WASM_EMBED
  (void)addr;
  (void)bytes;
  return true;
#elif defined(_WIN32)
  return VirtualAlloc(addr, bytes, MEM_COMMIT, PAGE_READWRITE) != NULL;
#else
  (void)addr; (void)bytes;
  return true;
#endif
}

sv_vm_t *sv_vm_create(ant_t *js) {
  int stack_size, max_frames;
  sv_vm_limits(&stack_size, &max_frames);

  if (stack_size > SV_STACK_HARD_MAX) stack_size = SV_STACK_HARD_MAX;
  if (max_frames > SV_FRAMES_HARD_MAX) max_frames = SV_FRAMES_HARD_MAX;

  sv_vm_t *vm = calloc(1, sizeof(*vm));
  if (!vm) return NULL;

  vm->js = js;
  vm->fp = -1;

  vm->stack_size = stack_size;
  vm->max_frames = max_frames;

  vm->suspend.entry_fp = -1;
  vm->suspend.saved_fp = -1;
  vm->jit.mode_fp = INT_MIN;

  vm->handler_stack = malloc((size_t)SV_HANDLER_INITIAL * sizeof(sv_handler_t));
  vm->handler_cap = SV_HANDLER_INITIAL;
  
  void *base = vm->handler_stack ? sv_vm_reserve_storage() : NULL;
  
  if (!base) {
    free(vm->handler_stack);
    free(vm);
    return NULL;
  }

  vm->stack = (ant_value_t *)base;
  vm->frames = (sv_frame_t *)((char *)base + SV_STACK_RESERVE + SV_VM_GUARD_SIZE);

  if (
    !sv_vm_commit_storage(vm->stack, (size_t)stack_size * sizeof(ant_value_t)) ||
    !sv_vm_commit_storage(vm->frames, (size_t)max_frames * sizeof(sv_frame_t))
  ) {
    sv_vm_release_storage(base);
    free(vm->handler_stack);
    free(vm);
    return NULL;
  }

  return vm;
}

void sv_vm_destroy(sv_vm_t *vm) {
  if (!vm) return;
  sv_vm_release_storage(vm->stack);
  free(vm->handler_stack);
  free(vm->async_inline.records);
  free(vm);
}

bool sv_vm_reserve_handlers(sv_vm_t *vm, int extra) {
  int need = vm->handler_depth + extra;
  
  if (need <= vm->handler_cap) return true;
  if (need > SV_HANDLER_HARD_MAX) return false;
  
  int cap = vm->handler_cap;
  while (cap < need) cap *= 2;
  
  if (cap > SV_HANDLER_HARD_MAX) cap = SV_HANDLER_HARD_MAX;
  sv_handler_t *grown = realloc(vm->handler_stack, (size_t)cap * sizeof(*grown));
  
  if (!grown) return false;
  vm->handler_stack = grown;
  vm->handler_cap = cap;
  
  return true;
}

static bool sv_vm_grow_frames(sv_vm_t *vm) {
  int new_max = vm->max_frames * 2;
  if (new_max > SV_FRAMES_HARD_MAX) new_max = SV_FRAMES_HARD_MAX;
  if (new_max <= vm->max_frames) return false;
  if (!sv_vm_commit_storage(vm->frames, (size_t)new_max * sizeof(sv_frame_t))) return false;
  vm->max_frames = new_max;
  return true;
}

static bool sv_vm_grow_stack(sv_vm_t *vm) {
  int new_size = vm->stack_size * 2;
  if (new_size > SV_STACK_HARD_MAX) new_size = SV_STACK_HARD_MAX;
  if (new_size <= vm->stack_size) return false;
  if (!sv_vm_commit_storage(vm->stack, (size_t)new_size * sizeof(ant_value_t))) return false;
  vm->stack_size = new_size;
  return true;
}

bool sv_vm_reserve(sv_vm_t *vm, int slots) {
  if (vm->fp + 1 >= vm->max_frames && !sv_vm_grow_frames(vm)) return false;
  while (vm->sp + slots > vm->stack_size)
    if (!sv_vm_grow_stack(vm)) return false;
  return true;
}

sv_activation_t *sv_activation_capture(sv_vm_t *vm, int entry_fp, sv_activation_t *reuse) {
  if (!vm || vm->fp < 0 || entry_fp < 0 || entry_fp > vm->fp) return NULL;

  sv_frame_t *entry_frame = &vm->frames[entry_fp];
  int frame_count = vm->fp - entry_fp + 1;
  int stack_base = entry_frame->prev_sp;
  int stack_count = vm->sp - stack_base;
  int handler_base = entry_frame->handler_base;
  int handler_count = vm->handler_depth - handler_base;

  if (stack_count < 0 || handler_count < 0) return NULL;

  size_t frames_off = (sizeof(sv_activation_t) + 7) & ~(size_t)7;
  size_t slots_off = frames_off + (size_t)frame_count * sizeof(sv_frame_t);
  size_t handlers_off = slots_off + (size_t)stack_count * sizeof(ant_value_t);
  size_t need = handlers_off + (size_t)handler_count * sizeof(sv_handler_t);

  sv_activation_t *act = reuse;
  if (!act || act->capacity < need) {
    act = realloc(reuse, need);
    if (!act) return NULL;
    act->capacity = need;
  }

  act->frames = (sv_frame_t *)((char *)act + frames_off);
  act->slots = (ant_value_t *)((char *)act + slots_off);
  act->handlers = (sv_handler_t *)((char *)act + handlers_off);
  
  act->frame_count = frame_count;
  act->stack_count = stack_count;
  act->handler_count = handler_count;

  ant_value_t *src_base = &vm->stack[stack_base];
  if (stack_count > 0)
    memcpy(act->slots, src_base, sizeof(ant_value_t) * (size_t)stack_count);

  for (int i = 0; i < frame_count; i++) {
    sv_frame_t *src = &vm->frames[entry_fp + i];
    sv_frame_t *dst = &act->frames[i];
    *dst = *src;

    dst->prev_sp = src->prev_sp - stack_base;
    dst->handler_base = (uint16_t)(src->handler_base - handler_base);
    dst->handler_top = (uint16_t)(src->handler_top - handler_base);

    if (src->bp) dst->bp = act->slots + (src->bp - src_base);
    if (src->lp) dst->lp = act->slots + (src->lp - src_base);
  }

  for (int i = 0; i < frame_count; i++) if (vtype(act->frames[i].arguments_obj) != kTypeUndefined) {
    js_arguments_bind_direct(vm->js, act->frames[i].arguments_obj, &act->frames[i]);
    vm->frames[entry_fp + i].arguments_obj = Ant_Value_Undefined;
  }

  for (int i = 0; i < handler_count; i++) {
    sv_handler_t h = vm->handler_stack[handler_base + i];
    h.saved_sp -= stack_base;
    act->handlers[i] = h;
  }

  act->open_upvalues = NULL;
  sv_upvalue_t **src_pp = &vm->open_upvalues;
  sv_upvalue_t **dst_pp = &act->open_upvalues;
  
  while (*src_pp) {
    sv_upvalue_t *uv = *src_pp;
    if (stack_count <= 0 || !sv_slot_in_range(src_base, (size_t)stack_count, uv->location)) {
      src_pp = &uv->next;
      continue;
    }

    ptrdiff_t slot = uv->location - src_base;
    *src_pp = uv->next;
    uv->location = &act->slots[slot];
    gc_upvalue_capture_barrier(vm->js, uv);
    uv->next = NULL;
    *dst_pp = uv;
    dst_pp = &uv->next;
  }

  vm->fp = entry_fp - 1;
  vm->sp = stack_base;
  vm->handler_depth = handler_base;
  vm->suspend.active = false;
  vm->suspend.pending = false;
  vm->suspend.is_error = false;
  vm->suspend.kind = SV_RESUME_NEXT;
  vm->suspend.value = Ant_Value_Undefined;
  vm->suspend.entry_fp = -1;
  vm->suspend.saved_fp = -1;

  return act;
}

bool sv_activation_install(sv_vm_t *vm, sv_activation_t *act) {
  if (!vm || !act || act->frame_count < 1) return false;

  while (vm->sp + act->stack_count > vm->stack_size)
    if (!sv_vm_grow_stack(vm)) return false;
  while (vm->fp + act->frame_count >= vm->max_frames)
    if (!sv_vm_grow_frames(vm)) return false;
  
  if (!sv_vm_reserve_handlers(vm, act->handler_count)) return false;

  int entry_fp = vm->fp + 1;
  int stack_base = vm->sp;
  int handler_base = vm->handler_depth;
  ant_value_t *dst_base = &vm->stack[stack_base];

  if (act->stack_count > 0)
    memcpy(dst_base, act->slots, sizeof(ant_value_t) * (size_t)act->stack_count);

  for (int i = 0; i < act->frame_count; i++) {
    sv_frame_t *src = &act->frames[i];
    sv_frame_t *dst = &vm->frames[entry_fp + i];
    *dst = *src;

    dst->prev_sp = src->prev_sp + stack_base;
    dst->handler_base = (uint16_t)(src->handler_base + handler_base);
    dst->handler_top = (uint16_t)(src->handler_top + handler_base);

    if (src->bp) dst->bp = dst_base + (src->bp - act->slots);
    if (src->lp) dst->lp = dst_base + (src->lp - act->slots);

    if (vtype(dst->arguments_obj) != kTypeUndefined)
      js_arguments_rebind_frame(vm->js, dst->arguments_obj, entry_fp + i);
  }

  for (int i = 0; i < act->handler_count; i++) {
    sv_handler_t h = act->handlers[i];
    h.saved_sp += stack_base;
    vm->handler_stack[handler_base + i] = h;
  }

  if (act->open_upvalues) {
    for (sv_upvalue_t *uv = act->open_upvalues; uv; uv = uv->next)
      uv->location = dst_base + (uv->location - act->slots);
    sv_merge_open_upvalues(&vm->open_upvalues, act->open_upvalues);
    act->open_upvalues = NULL;
  }

  vm->sp = stack_base + act->stack_count;
  vm->fp = entry_fp + act->frame_count - 1;
  vm->handler_depth = handler_base + act->handler_count;

  vm->suspend.active = true;
  vm->suspend.entry_fp = entry_fp;
  vm->suspend.saved_fp = entry_fp - 1;

  act->frame_count = 0;
  return true;
}

static inline void sv_drop_frame_runtime_state(ant_t *js, sv_frame_t *frame) {
  if (frame && vtype(frame->arguments_obj) != kTypeUndefined) {
    js_arguments_detach(js, frame->arguments_obj);
    frame->arguments_obj = Ant_Value_Undefined;
  }
}

static inline void sv_drop_frame_runtime_state_keep(ant_t *js, sv_frame_t *frame, ant_value_t keep) {
  if (vtype(frame->arguments_obj) == kTypeUndefined) return;
  GC_ROOT_SAVE(keep_mark, js);
  GC_ROOT_PIN(js, keep);
  sv_drop_frame_runtime_state(js, frame);
  GC_ROOT_RESTORE(js, keep_mark);
}

void sv_activation_discard(sv_vm_t *vm, int entry_fp) {
  if (!vm || entry_fp < 0 || entry_fp > vm->fp) return;

  if (vm->open_upvalues) {
  for (int f = vm->fp; f >= entry_fp; f--) {
    ant_value_t *drop_bp = vm->frames[f].bp;
    if (drop_bp) sv_close_upvalues_from_slot(vm, drop_bp);
  }}

  for (int f = vm->fp; f >= entry_fp; f--)
    sv_drop_frame_runtime_state(vm->js, &vm->frames[f]);

  vm->fp = entry_fp - 1;
  vm->sp = vm->frames[entry_fp].prev_sp;
  vm->handler_depth = vm->frames[entry_fp].handler_base;

  vm->suspend.active = false;
  vm->suspend.pending = false;
  vm->suspend.entry_fp = -1;
  vm->suspend.saved_fp = -1;
}

void sv_activation_seal(ant_t *js, sv_activation_t *act) {
  if (!js || !act || act->frame_count <= 0) return;

  for (sv_upvalue_t *uv = act->open_upvalues; uv;) {
    sv_upvalue_t *next = uv->next;
    uv->next = NULL;
    
    if (!gc_upvalue_is_live(js, uv)) { 
      uv = next;
      continue;
    }
    
    uv->closed = *uv->location;
    uv->location = &uv->closed;
    
    if (!js->gc_running) gc_upvalue_write_barrier(js, uv, uv->closed);
    uv = next;
  }
  
  act->open_upvalues = NULL;

  for (int i = 0; i < act->frame_count; i++) {
    ant_value_t args_obj = act->frames[i].arguments_obj;
    if (vtype(args_obj) == kTypeUndefined) continue;
    if (js_obj_ptr(args_obj)->mark_epoch == ANT_GC_DEAD) continue;
    js_arguments_detach(js, args_obj);
  }
}

__attribute__((noinline, cold))
static void sv_pin_entry_values(ant_t *js, sv_vm_t *vm, ant_value_t *args, int argc) {
  if (args) for (int i = 0; i < argc; i++) GC_ROOT_PIN(js, args[i]);
  if (!vm->jit.resume.active) return;
  for (int64_t i = 0; i < vm->jit.resume.n_params; i++) GC_ROOT_PIN(js, vm->jit.resume.params[i]);
  for (int64_t i = 0; i < vm->jit.resume.n_locals; i++) GC_ROOT_PIN(js, vm->jit.resume.locals[i]);
  for (int64_t i = 0; i < vm->jit.resume.vstack_sp; i++) GC_ROOT_PIN(js, vm->jit.resume.vstack[i]);
  GC_ROOT_PIN(js, vm->jit.resume.arguments_obj);
}

static inline void sv_clear_jit_resume(sv_vm_t *vm) {
  vm->jit.resume.active = false;
  vm->jit.resume.ip_offset = 0;
  vm->jit.resume.params = NULL;
  vm->jit.resume.n_params = 0;
  vm->jit.resume.locals = NULL;
  vm->jit.resume.n_locals = 0;
  vm->jit.resume.vstack = NULL;
  vm->jit.resume.vstack_sp = 0;
  vm->jit.resume.arguments_obj = Ant_Value_Undefined;
  vm->jit.resume.throwing = false;
  vm->jit.resume.finally = NULL;
}

const sv_srcpos_t *sv_srcpos_at(sv_func_t *func, int bc_offset) {
  if (!func || !func->debug->srcpos || func->debug->srcpos_count <= 0) return NULL;
  
  const sv_srcpos_t *pos = func->debug->srcpos;
  int lo = 0, hi = func->debug->srcpos_count;
  
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if ((int)pos[mid].bc_offset <= bc_offset) lo = mid + 1;
    else hi = mid;
  }
  
  return lo > 0 ? &pos[lo - 1] : NULL;
}

ant_value_t Ant_Value_LooseEquals(ant_t *js, ant_value_t l, ant_value_t r) {
  return sv_abstract_eq(js, l, r);
}

bool sv_lookup_srcpos(sv_func_t *func, int bc_offset, uint32_t *line, uint32_t *col) {
  const sv_srcpos_t *pos = sv_srcpos_at(func, bc_offset);
  if (!pos) return false;
  if (line) *line = pos->line;
  if (col) *col = pos->col;
  return true;
}

bool sv_lookup_srcspan(sv_func_t *func, int bc_offset, uint32_t *src_off, uint32_t *src_end) {
  const sv_srcpos_t *pos = sv_srcpos_at(func, bc_offset);
  if (!pos) return false;
  if (src_off) *src_off = pos->src_off;
  if (src_end) *src_end = pos->src_end < pos->src_off ? pos->src_off : pos->src_end;
  return true;
}

static inline ant_flat_string_t *sv_string_builder_flat_ptr(ant_value_t value) {
  if (vtype(value) != kTypeString || str_is_heap_rope(value) || str_is_heap_builder(value)) return NULL;
  return (ant_flat_string_t *)vptr(value);
}

static inline ant_string_builder_t *sv_string_builder_heap_ptr(ant_value_t value) {
  if (vtype(value) != kTypeString || !str_is_heap_builder(value)) return NULL;
  return ant_str_builder_ptr(value);
}

static inline uint8_t sv_builder_chunk_ascii_state(ant_flat_string_t *flat) {
  if (!flat) return STR_ASCII_UNKNOWN;
  uint8_t state = str_flat_ascii_state(flat);
  if (state == STR_ASCII_UNKNOWN) {
    state = str_detect_ascii_bytes(flat->bytes, (size_t)flat->len);
    str_flat_set_ascii_state(flat, state);
  }
  return state;
}

static bool sv_builder_append_number(ant_string_builder_t *builder, ant_value_t num) {
  char digits[32];
  size_t n = js_number_to_chars(num, digits, sizeof(digits));
  if (n == 0 || builder->tail_len + n > STR_BUILDER_TAIL_CAP) return false;

  str_copy_small(builder->tail + builder->tail_len, digits, n);
  builder->tail_len = (uint16_t)(builder->tail_len + n);
  builder->len += (ant_offset_t)n;
  
  if (vtype(builder->cached) == kTypeNumber) builder->cached = Ant_Value_FromNumber(Ant_Value_AsNumberUnchecked(builder->cached) + (double)n);
  if (builder->ascii_state != STR_ASCII_NO) builder->ascii_state = STR_ASCII_YES;
  
  return true;
}

static inline void sv_builder_note_ascii(ant_string_builder_t *builder, uint8_t state) {
  if (!builder) return;
  if (state == STR_ASCII_NO) builder->ascii_state = STR_ASCII_NO;
  else if (builder->ascii_state != STR_ASCII_NO && state == STR_ASCII_YES)
    builder->ascii_state = STR_ASCII_YES;
}

static inline void sv_builder_record_flat(
  ant_t *js, ant_string_builder_t *builder, 
  ant_value_t value, ant_flat_string_t *flat
) {
  if (!builder || !flat) return;
  
  ant_value_t cached = builder->cached;
  uint8_t ascii = sv_builder_chunk_ascii_state(flat);
  
  builder->len += flat->len;
  if (vtype(cached) == kTypeNumber) {
    ant_offset_t units = ascii == STR_ASCII_YES ? flat->len : str_utf16_len(js, value);
    builder->cached = Ant_Value_FromNumber(Ant_Value_AsNumberUnchecked(cached) + (double)units);
  } else builder->cached = Ant_Value_Undefined;
  
  sv_builder_note_ascii(builder, ascii);
}

static ant_value_t sv_builder_normalize_chunk(ant_t *js, ant_value_t value) {
  if (vtype(value) != kTypeString) return js_mkerr(js, "string builder expects string chunk");
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
  return Ant_Value_Undefined;
}

static ant_value_t sv_builder_flush_tail(
  ant_t *js, ant_string_builder_t *builder
) {
  if (!builder || builder->tail_len == 0) return Ant_Value_Undefined;
  
  ant_value_t tail = builder->ascii_state == STR_ASCII_YES
    ? js_mkstr_ascii(js, builder->tail, builder->tail_len)
    : js_mkstr(js, builder->tail, builder->tail_len);
    
  if (is_err(tail)) return tail;
  ant_value_t push = sv_builder_push_chunk_value(js, builder, tail);
  
  if (is_err(push)) return push;
  builder->tail_len = 0;
  
  return Ant_Value_Undefined;
}

static ant_value_t sv_builder_append_flat(
  ant_t *js, ant_string_builder_t *builder, ant_value_t chunk
) {
  ant_flat_string_t *flat = sv_string_builder_flat_ptr(chunk);
  if (!flat) return js_mkerr(js, "string builder received non-flat string");
  if (flat->len == 0) return Ant_Value_Undefined;

  if (
    flat->len <= STR_BUILDER_TAIL_CAP &&
    builder->tail_len + flat->len <= STR_BUILDER_TAIL_CAP
  ) {
    str_copy_small(builder->tail + builder->tail_len, flat->bytes, (size_t)flat->len);
    builder->tail_len = (uint16_t)(builder->tail_len + flat->len);
    sv_builder_record_flat(js, builder, chunk, flat);
    return Ant_Value_Undefined;
  }

  ant_value_t flush = sv_builder_flush_tail(js, builder);
  if (is_err(flush)) return flush;
  ant_value_t push = sv_builder_push_chunk_value(js, builder, chunk);
  
  if (is_err(push)) return push;
  sv_builder_record_flat(js, builder, chunk, flat);
  
  return Ant_Value_Undefined;
}

static ant_value_t sv_string_builder_new(ant_t *js) {
  ant_string_builder_t *builder = (ant_string_builder_t *)js_type_alloc(
    js, ANT_ALLOC_ROPE, sizeof(*builder), _Alignof(ant_string_builder_t)
  );
  if (!builder) return js_mkerr(js, "string builder allocation failed");
  memset(builder, 0, sizeof(*builder));
  builder->snapshot = Ant_Value_Undefined;
  builder->cached = Ant_Value_Undefined;
  builder->ascii_state = STR_ASCII_YES;
  return ant_mkbuilder_value(builder);
}

static ant_value_t sv_string_builder_snapshot(ant_t *js, ant_value_t value) {
  ant_string_builder_t *builder = sv_string_builder_heap_ptr(value);
  if (!builder) return value;

  if (!builder->head && builder->tail_len == 0) {
    ant_value_t snapshot = builder->snapshot;
    if (vtype(snapshot) == kTypeString) return snapshot;

    ant_value_t cached = builder->cached;
    if (vtype(cached) == kTypeString &&
        !str_is_heap_rope(cached) && !str_is_heap_builder(cached))
      return cached;
  }

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, value);
  ant_value_t result = builder->snapshot;
  GC_ROOT_PIN(js, result);
  ant_value_t tail = Ant_Value_Undefined;
  GC_ROOT_PIN(js, tail);

  for (ant_builder_chunk_t *chunk = builder->head; chunk; chunk = chunk->next) {
    ant_value_t piece = chunk->value;
    if (vtype(result) != kTypeString) result = piece;
    else {
      result = do_string_op(js, TOK_PLUS, result, piece);
      if (is_err(result)) goto fail;
    }
  }

  if (builder->tail_len > 0) {
    tail = js_mkstr(js, builder->tail, builder->tail_len);
    if (is_err(tail)) {
      result = tail;
      goto fail;
    }

    if (vtype(result) != kTypeString) result = tail;
    else {
      result = do_string_op(js, TOK_PLUS, result, tail);
      if (is_err(result)) goto fail;
    }
  }

  if (vtype(result) != kTypeString) {
    result = js_mkstr(js, "", 0);
    if (is_err(result)) goto fail;
  }

  builder->snapshot = result;
  if (str_is_heap_rope(result) &&
      (ant_str_rope_ptr(result)->flags & ANT_ROPE_FLAG_YOUNG) != 0)
    gc_remember_builder(js, builder);
  builder->head = NULL;
  builder->chunk_tail = NULL;
  builder->tail_len = 0;
  if (!str_is_heap_rope(result)) builder->cached = result;

  GC_ROOT_RESTORE(js, root_mark);
  return result;

fail:
  GC_ROOT_RESTORE(js, root_mark);
  return result;
}

static inline void sv_record_slot_feedback(
  sv_frame_t *frame, sv_func_t *func, uint16_t slot_idx, ant_value_t value
) {
  if (!frame || !func) return;
  if ((int)slot_idx < func->param_count) return;
  sv_tfb_record_local(func, (int)(slot_idx - func->param_count), value);
}

ant_value_t sv_string_builder_read_value(ant_t *js, ant_value_t value) {
  if (vtype(value) == kTypeString && str_is_heap_builder(value))
    return sv_string_builder_snapshot(js, value);
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
  return Ant_Value_Undefined;
}

ant_value_t sv_string_builder_flush_slot(
  sv_vm_t *vm, ant_t *js, sv_frame_t *frame, uint16_t slot_idx
) {
  ant_value_t *slot = sv_frame_slot_ptr(frame, slot_idx);
  if (!slot || vtype(*slot) != kTypeString || !str_is_heap_builder(*slot)) return Ant_Value_Undefined;

  ant_value_t out = str_materialize(js, *slot);
  if (is_err(out)) return out;
  *slot = out;
  sv_record_slot_feedback(frame, frame->func, slot_idx, out);
  return out;
}

static bool sv_try_short_string_append(
  ant_t *js, ant_value_t lhs, ant_value_t rhs, ant_value_t *result
) {
  ant_flat_string_t *left = sv_string_builder_flat_ptr(lhs);
  ant_flat_string_t *right = sv_string_builder_flat_ptr(rhs);
  
  if (!left || !right) return false;
  if (left->len > STR_BUILDER_TAIL_CAP || right->len > STR_BUILDER_TAIL_CAP - left->len) return false;
  
  if (left->len == 0) { *result = rhs; return true; }
  if (right->len == 0) { *result = lhs; return true; }

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, lhs);
  GC_ROOT_PIN(js, rhs);
  size_t len = (size_t)(left->len + right->len);
  *result = js_mkstr(js, NULL, len);
  
  if (!is_err(*result)) str_flat_fill_concat(
    ant_str_flat_ptr(*result),
    left->bytes, (size_t)left->len, str_flat_ascii_state(left),
    right->bytes, (size_t)right->len, str_flat_ascii_state(right)
  ); 
  
  GC_ROOT_RESTORE(js, mark);
  return true;
}

__attribute__((noinline))
ant_value_t sv_string_builder_append_at(
  sv_vm_t *vm, ant_t *js, ant_value_t *slot, sv_frame_t *frame,
  sv_func_t *func, uint16_t slot_idx, ant_value_t rhs
) {
  ant_value_t lhs = *slot;
  ant_string_builder_t *builder = sv_string_builder_heap_ptr(lhs);

  if (builder) {
    if (vtype(rhs) == kTypeNumber && sv_builder_append_number(builder, rhs)) {
      sv_record_slot_feedback(frame, func, slot_idx, lhs);
      return Ant_Value_Undefined;
    }

    ant_value_t rhs_str = rhs;
    if (!sv_string_builder_flat_ptr(rhs)) {
      rhs_str = coerce_to_str_concat(js, rhs);
      if (is_err(rhs_str)) return rhs_str;
      
      rhs_str = sv_builder_normalize_chunk(js, rhs_str);
      if (is_err(rhs_str)) return rhs_str;
    }
    
    ant_value_t append_err = sv_builder_append_flat(js, builder, rhs_str);
    if (is_err(append_err)) return append_err;
    
    sv_record_slot_feedback(frame, func, slot_idx, lhs);
    return Ant_Value_Undefined;
  }

  if (vtype(lhs) == kTypeNumber && vtype(rhs) == kTypeNumber) {
    *slot = Ant_Value_FromNumber(Ant_Value_AsNumberUnchecked(lhs) + Ant_Value_AsNumberUnchecked(rhs));
    sv_record_slot_feedback(frame, func, slot_idx, *slot);
    return Ant_Value_Undefined;
  }

  ant_value_t short_result;
  if (sv_try_short_string_append(js, lhs, rhs, &short_result)) {
    if (is_err(short_result)) return short_result;
    *slot = short_result;
    sv_record_slot_feedback(frame, func, slot_idx, *slot);
    return Ant_Value_Undefined;
  }

  ant_value_t lu = sv_add_to_primitive(js, lhs);
  if (is_err(lu)) return lu;
  
  ant_value_t ru = sv_add_to_primitive(js, rhs);
  if (is_err(ru)) return ru;
  
  bool string_concat = is_non_numeric(lu) || is_non_numeric(ru);
  if (!string_concat) {
    ant_value_t add_err = sv_slot_generic_add_store(vm, js, slot, lu, ru);
    if (is_err(add_err)) return add_err;
    sv_record_slot_feedback(frame, func, slot_idx, *slot);
    return Ant_Value_Undefined;
  }

  ant_value_t lhs_str = coerce_to_str_concat(js, lu);
  if (is_err(lhs_str)) return lhs_str;
  
  ant_value_t rhs_str = coerce_to_str_concat(js, ru);
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
  
  return Ant_Value_Undefined;
}

ant_value_t sv_string_builder_append_slot(
  sv_vm_t *vm, ant_t *js, sv_frame_t *frame,
  sv_func_t *func, uint16_t slot_idx, ant_value_t rhs
) {
  ant_value_t *slot = sv_frame_slot_ptr(frame, slot_idx);
  if (!slot) return js_mkerr(js, "invalid string builder slot");
  return sv_string_builder_append_at(vm, js, slot, frame, func, slot_idx, rhs);
}

__attribute__((noinline))
ant_value_t sv_string_builder_append_snapshot_at(
  sv_vm_t *vm, ant_t *js, ant_value_t *slot, sv_frame_t *frame,
  sv_func_t *func, uint16_t slot_idx, ant_value_t lhs, ant_value_t rhs
) {
  ant_value_t short_result;
  if (sv_try_short_string_append(js, lhs, rhs, &short_result)) {
    if (is_err(short_result)) return short_result;
    *slot = short_result;
    sv_record_slot_feedback(frame, func, slot_idx, *slot);
    return Ant_Value_Undefined;
  }

  if (*slot == lhs)
    return sv_string_builder_append_at(vm, js, slot, frame, func, slot_idx, rhs);

  if (vtype(lhs) == kTypeNumber && vtype(rhs) == kTypeNumber) {
    *slot = Ant_Value_FromNumber(Ant_Value_AsNumberUnchecked(lhs) + Ant_Value_AsNumberUnchecked(rhs));
    sv_record_slot_feedback(frame, func, slot_idx, *slot);
    return Ant_Value_Undefined;
  }

  ant_value_t lu = sv_add_to_primitive(js, lhs);
  if (is_err(lu)) return lu;
  ant_value_t ru = sv_add_to_primitive(js, rhs);
  if (is_err(ru)) return ru;

  bool string_concat = is_non_numeric(lu) || is_non_numeric(ru);
  if (!string_concat) {
    ant_value_t add_err = sv_slot_generic_add_store(vm, js, slot, lu, ru);
    if (is_err(add_err)) return add_err;
    sv_record_slot_feedback(frame, func, slot_idx, *slot);
    return Ant_Value_Undefined;
  }

  ant_value_t lhs_str = coerce_to_str_concat(js, lu);
  if (is_err(lhs_str)) return lhs_str;
  ant_value_t rhs_str = coerce_to_str_concat(js, ru);
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
  
  return Ant_Value_Undefined;
}

ant_value_t sv_string_builder_append_snapshot_slot(
  sv_vm_t *vm, ant_t *js, sv_frame_t *frame,
  sv_func_t *func, uint16_t slot_idx, ant_value_t lhs, ant_value_t rhs
) {
  ant_value_t *slot = sv_frame_slot_ptr(frame, slot_idx);
  if (!slot) return js_mkerr(js, "invalid string builder slot");
  return sv_string_builder_append_snapshot_at(vm, js, slot, frame, func, slot_idx, lhs, rhs);
}

static inline ant_value_t sv_stage_frame_args(
  sv_vm_t *vm, ant_t *js, sv_func_t *func, ant_value_t *args, int argc,
  ant_value_t **out_bp, ant_value_t **out_lp
) {
  int arg_slots = (argc > func->param_count) ? argc : func->param_count;
  int need = arg_slots + func->max_locals;
  int reserve = need + func->max_stack;
  
  if (vm->sp + reserve > vm->stack_size) {
    int args_idx = (
      args && args >= vm->stack && 
      args < vm->stack + vm->stack_size
    ) ? (int)(args - vm->stack) : -1;
    
    while (vm->sp + reserve > vm->stack_size) 
      if (!sv_vm_grow_stack(vm)) return Ant_Error_CreateStackOverflow(js);
    
    if (args_idx >= 0) args = &vm->stack[args_idx];
  }

  ant_value_t *base = &vm->stack[vm->sp];
  *out_bp = base;
  *out_lp = base + arg_slots;

  if (argc > 0 && args)
    memmove(base, args, (size_t)argc * sizeof(ant_value_t));
  for (int i = argc; i < arg_slots; i++)
    base[i] = Ant_Value_Undefined;
  if (func->max_locals > 0)
    for (int i = 0; i < func->max_locals; i++)
      (*out_lp)[i] = Ant_Value_Undefined;
  vm->sp += need;
  return Ant_Value_Undefined;
}

ant_value_t sv_vm_stage_frame(
  sv_vm_t *vm, sv_func_t *func, ant_value_t *args, int argc,
  ant_value_t **out_bp, ant_value_t **out_lp
) {
  return sv_stage_frame_args(vm, vm->js, func, args, argc, out_bp, out_lp);
}

static inline ant_value_t sv_execute_entry_common(
  sv_vm_t *vm, sv_func_t *func, sv_upvalue_t **upvalues, int upvalue_count,
  ant_value_t callee_func, ant_value_t super_val, ant_value_t new_target,
  ant_value_t this_val, ant_value_t *args, int argc,
  ant_value_t eval_env, ant_value_t *out_this
) {
  if (!vm || !vm->js) return mkval(kTypeError, 0);
  ant_t *js = vm->js;

  if (!func)
    return js_mkerr_typed(js, JS_ERR_INTERNAL | JS_ERR_NO_STACK, "invalid function entry");

  if (vm->fp + 1 >= vm->max_frames && !sv_vm_grow_frames(vm))
    return Ant_Error_CreateStackOverflow(js);

  int saved_fp = vm->fp;
  vm->fp = saved_fp + 1;
  vm->frames[vm->fp].upvalues = upvalues;
  vm->frames[vm->fp].upvalue_count = upvalue_count;
  vm->frames[vm->fp].callee = callee_func;
  vm->frames[vm->fp].eval_env = eval_env;
  vm->frames[vm->fp].new_target = new_target;

  ant_realm_t *prev_realm = ant_realm_switch(js, func->realm);
  ant_value_t result = sv_execute_frame(vm, func, this_val, super_val, args, argc);
  
  if (out_this) *out_this = vm->frames[vm->fp].this;
  if (func->is_derived_ctor && !is_err(result) && vtype(result) != kTypeUndefined &&
      !is_object_type(result) && vtype(result) != kTypeBuiltin)
    result = js_mkerr_typed(js, JS_ERR_TYPE, SV_DERIVED_CTOR_RETURN_ERROR);
  if (!vm->suspend.active) vm->fp = saved_fp;
  
  ant_realm_restore(js, prev_realm);

  return result;
}

__attribute__((noinline))
static sv_jit_func_t sv_direct_closure_jit_prepare(
  ant_t *js, sv_func_t *callee, sv_closure_t *closure
) {
  if (callee->is_generator) return NULL;
  if (callee->jit_compile_failed) return NULL;

  uint32_t cc = ++callee->call_count;
  if (__builtin_expect(cc == SV_TFB_ALLOC_THRESHOLD, 0))
    sv_tfb_ensure(callee);
  if (cc <= SV_JIT_THRESHOLD) return NULL;

  sv_jit_func_t jit_fn = sv_jit_compile(js, callee, closure);
  if (!jit_fn) {
    callee->call_count = 0;
    callee->back_edge_count = 0;
    return NULL;
  }

  callee->jit_code = jit_fn;
  return jit_fn;
}

static inline __attribute__((always_inline)) ant_value_t sv_try_direct_closure_jit(
  sv_vm_t *vm, ant_t *js, sv_func_t *caller_func, uint8_t *caller_ip, sv_frame_t *caller_frame,
  sv_closure_t *closure, ant_value_t jit_this, ant_value_t *call_args, int call_argc
) {
  sv_func_t *callee = closure->func;
  if (!callee) return SV_JIT_RETRY_INTERP;

  if (caller_func && sv_func_type_feedback(caller_func) && caller_ip)
    sv_tfb_record_call_target(caller_func, (int)(caller_ip - caller_func->code), callee);

  sv_jit_func_t jit_fn = callee->jit_code;
  if (!jit_fn) {
    jit_fn = sv_direct_closure_jit_prepare(js, callee, closure);
    if (!jit_fn) return SV_JIT_RETRY_INTERP;
  }

  if (caller_frame && caller_ip) caller_frame->ip = caller_ip;
  ant_value_t jit_result = sv_jit_invoke(js, SV_JIT_FROM_INTERP, jit_fn, vm, &(sv_call_ctx_t){
    .this_val = jit_this,
    .new_target = Ant_Value_Undefined,
    .super_val = closure->super_val,
    .args = call_args,
    .argc = call_argc,
  }, closure);
  
  if (sv_is_jit_bailout(jit_result)) {
    sv_jit_on_bailout(callee);
    return SV_JIT_RETRY_INTERP;
  }
  
  return jit_result;
}

__attribute__((noinline, cold))
ant_value_t sv_jit_invoke_in_realm(
  ant_t *js, sv_jit_entry_t from, sv_jit_func_t fn, sv_vm_t *vm,
  const sv_call_ctx_t *ctx, sv_closure_t *closure
) {
  ant_realm_t *prev_realm = ant_realm_enter(js, closure->func->realm);
  ant_value_t result = sv_jit_invoke_same_realm(js, from, fn, vm, ctx, closure);
  ant_realm_enter(js, prev_realm);
  return result;
}

ant_value_t sv_closure_materialize_func_obj(ant_t *js, sv_closure_t *c, ant_value_t func_val) {
  if (c->func_obj) return c->func_obj;
  ant_realm_t *prev_realm = ant_realm_switch(js, c->func ? c->func->realm : NULL);
  sv_init_closure_function_object(js, c, func_val, c->module_ctx);
  ant_realm_restore(js, prev_realm);
  return c->func_obj;
}

ant_value_t sv_execute_entry(
  sv_vm_t *vm, sv_func_t *func, ant_value_t this_val, ant_value_t *args, int argc
) {
  return sv_execute_entry_common(
    vm, func, NULL, 0, Ant_Value_Undefined, Ant_Value_Undefined, Ant_Value_Undefined,
    this_val, args, argc, Ant_Value_Undefined, NULL
  );
}

ant_value_t sv_execute_eval_entry(
  sv_vm_t *vm, sv_func_t *func,
  ant_value_t this_val, ant_value_t eval_env, ant_value_t new_target
) {
  return sv_execute_entry_common(
    vm, func, NULL, 0, Ant_Value_Undefined, Ant_Value_Undefined, new_target,
    this_val, NULL, 0, eval_env, NULL
  );
}

ant_value_t sv_execute_closure_entry(
  sv_vm_t *vm, sv_closure_t *closure, ant_value_t callee_func, ant_value_t super_val, ant_value_t new_target,
  ant_value_t this_val, ant_value_t *args, int argc, ant_value_t *out_this
) {
  if (!closure || !closure->func) return vm && vm->js
    ? js_mkerr_typed(vm->js, JS_ERR_INTERNAL | JS_ERR_NO_STACK, "invalid closure entry")
    : mkval(kTypeError, 0);
  return sv_execute_entry_common(
    vm, closure->func, closure->upvalues, closure->func->upvalue_count, callee_func,
    super_val, new_target, this_val, args, argc, sv_closure_eval_env(closure), out_this
  );
}

static inline ant_value_t sv_frame_return_value(const sv_frame_t *frame, ant_value_t value) {
  return vtype(frame->new_target) != kTypeUndefined && !is_object_type(value) ? frame->this : value;
}

static const sv_try_table_t sv_no_try_regions = { .count = 0 };

static const sv_try_table_t *sv_func_try_table(sv_func_t *func) {
  if (!func->has_try) return &sv_no_try_regions;
  sv_func_sidecar_t *sidecar = sv_func_sidecar(func);
  if (sidecar && sidecar->try_table) return sidecar->try_table;

  int n = 0;
  for (int off = 0; off < func->code_len; off += sv_op_size[func->code[off]]) {
    if (sv_op_size[func->code[off]] == 0) break;
    if (func->code[off] == OP_TRY_PUSH || func->code[off] == OP_TRY_PUSH_FINALLY) n++;
  }
  
  if (n == 0) return &sv_no_try_regions;
  sv_try_table_t *table = malloc(sizeof(*table) + (size_t)n * sizeof(sv_try_region_t));
  
  int *open = malloc((size_t)n * sizeof(int));
  int *depth = sv_func_stack_depth_map(func);

  sidecar = table && open ? sv_func_ensure_sidecar(func) : NULL;
  if (!sidecar) {
    free(table); free(open); free(depth);
    return &sv_no_try_regions;
  }

  int k = 0, top = 0;
  for (int off = 0; off < func->code_len;) {
    uint8_t op = func->code[off];
    int sz = sv_op_size[op];
  
    if (sz == 0) break;  
    if (op == OP_TRY_PUSH) {
      table->regions[k] = (sv_try_region_t){
        .try_off = (uint32_t)off, .pop_off = (uint32_t)func->code_len,
        .catch_off = (uint32_t)(off + sz + sv_get_i32(func->code + off + 1)),
        .depth = depth ? depth[off] : -1,
      };
      
      open[top++] = k++;
    } 
    else if (op == OP_TRY_POP && top > 0) table->regions[open[--top]].pop_off = (uint32_t)off;
    off += sz;
  }
  
  table->count = k;
  table->map = sv_func_try_map(func);
  table->depth = depth;

  free(open);
  sidecar->try_table = table;
  
  return table;
}

void sv_try_table_free(const sv_try_table_t *table) {
  if (!table || table == &sv_no_try_regions) return;
  free(table->map);
  free(table->depth);
  free((void *)table);
}

static void sv_frame_push_try_chain(sv_vm_t *vm, const sv_try_table_t *table, sv_func_t *func, int off, int base_sp) {
  int chain[SV_TRY_MAX];
  int count = 0;
  
  for (
    int state = table->map[off]; state >= 0 && 
    count < SV_TRY_MAX; state = table->map[func->code_len + state]
  ) chain[count++] = state;

  for (int i = count - 1; i >= 0; i--) {
    if (!sv_vm_reserve_handlers(vm, 1)) break;

    uint8_t *push = func->code + chain[i];
    uint8_t *target = push + sv_op_size[*push] + sv_get_i32(push + 1);

    if (*push == OP_FINALLY) {
      sv_handler_t h = { .kind = SV_HANDLER_FINALLY, .ip = target, .completion.value = Ant_Value_Undefined };
      sv_jit_finally_t *saved = vm->jit.resume.finally;
      if (saved) {
        saved += sv_func_finally_index(func, chain[i]);
        h.completion.kind = (sv_completion_kind_t)saved->kind;
        h.completion.value = saved->value;
        if (saved->kind == SV_COMPLETION_JUMP) h.completion.jump_ip = func->code + saved->target;
      }
      vm->handler_stack[vm->handler_depth++] = h;
      continue;
    }

    int try_depth = table->depth ? table->depth[chain[i]] : -1;
    vm->handler_stack[vm->handler_depth++] = (sv_handler_t){
      .kind = *push == OP_TRY_PUSH_FINALLY ? SV_HANDLER_TRY_FINALLY : SV_HANDLER_TRY,
      .ip = target,
      .saved_sp = try_depth >= 0 ? base_sp + try_depth : vm->sp,
    };
  }
}

bool sv_func_in_finally_body(sv_func_t *func, int off) {
  const sv_try_table_t *table = sv_func_try_table(func);
  if (!table->map || off < 0 || off >= func->code_len) return false;
  for (int state = table->map[off]; state >= 0; state = table->map[func->code_len + state])
    if (func->code[state] == OP_FINALLY) return true;
  return false;
}

bool sv_func_catch_at(sv_func_t *func, uint32_t off, uint32_t *resume_off, uint32_t *depth, bool *finally) {
  const sv_try_table_t *table = sv_func_try_table(func);
  if (!table->map || !table->depth || off >= (uint32_t)func->code_len) return false;
  
  int state = table->map[off];
  while (state >= 0 && func->code[state] == OP_FINALLY) state = table->map[func->code_len + state];
  
  if (state < 0 || table->depth[state] < 0) return false;
  const uint8_t *push = func->code + state;
  
  *finally = *push == OP_TRY_PUSH_FINALLY;
  *resume_off = (uint32_t)(state + sv_op_size[*push] + sv_get_i32(push + 1) + (*finally ? 0 : sv_op_size[OP_CATCH]));
  *depth = (uint32_t)table->depth[state];
  
  return true;
}

static void sv_resume_finally_entry(sv_vm_t *vm, sv_frame_t *frame, sv_func_t *func) {
  int off = vm->jit.resume.ip_offset;
  if (off < 0 || off >= func->code_len || func->code[off] != OP_FINALLY) return;
  const sv_jit_finally_t *saved = vm->jit.resume.finally + sv_func_finally_index(func, off);
  frame->completion = (sv_completion_t){.kind = (sv_completion_kind_t)saved->kind, .value = saved->value};
  if (saved->kind == SV_COMPLETION_JUMP) frame->completion.jump_ip = func->code + saved->target;
}

void sv_frame_push_try_handlers(sv_vm_t *vm, sv_frame_t *frame, sv_func_t *func, int off, int base_sp) {
  const sv_try_table_t *table = sv_func_try_table(func);
  
  if (table->map && off >= 0 && off < func->code_len) {
    sv_frame_push_try_chain(vm, table, func, off, base_sp);
    frame->handler_top = (uint16_t)vm->handler_depth;
    return;
  }
  
  for (int i = 0; i < table->count; i++) {
    const sv_try_region_t *r = &table->regions[i];
    if (r->try_off >= (uint32_t)off || r->pop_off < (uint32_t)off || r->catch_off <= (uint32_t)off) continue;
    if (!sv_vm_reserve_handlers(vm, 1)) break;
    
    vm->handler_stack[vm->handler_depth++] = (sv_handler_t){
      .kind = SV_HANDLER_TRY,
      .ip = func->code + r->catch_off,
      .saved_sp = r->depth >= 0 ? base_sp + r->depth : vm->sp,
    };
  }
  
  frame->handler_top = (uint16_t)vm->handler_depth;
}

static inline bool sv_async_inline_at(const sv_vm_t *vm, int fp) {
  return vm->async_inline.len != 0 && vm->async_inline.records[vm->async_inline.len - 1].fp == fp;
}

static __attribute__((noinline)) ant_value_t sv_async_inline_settle(
  sv_vm_t *vm, ant_t *js, ant_value_t value, bool is_error, bool pending
) {
  coroutine_t *coro = vm->async_inline.records[--vm->async_inline.len].coro;
  
  if (!coro) {
    GC_ROOT_SAVE(root_mark, js);
    GC_ROOT_PIN(js, value);
    
    ant_value_t promise = js_mkpromise(js);
    if (!is_err(promise)) {
      if (is_error) js_reject_promise(js, promise, value);
      else js_resolve_promise(js, promise, value);
    }
    
    GC_ROOT_RESTORE(js, root_mark);
    
    return promise;
  }

  ant_value_t promise = coro->async_promise;
  if (!pending) {
    if (is_error) js_reject_promise(js, promise, value);
    else js_resolve_promise(js, promise, value);
  }

  sv_async_unlink_activation(js, coro);
  coroutine_release(coro);
  
  return promise;
}

static inline uint8_t sv_async_inline_next_op(const sv_vm_t *vm, const sv_async_inline_t *rec) {
  return rec->entry 
    ? rec->next_op 
    : *vm->frames[rec->fp - 1].ip;
}

static inline bool sv_async_inline_elides_promise(const sv_vm_t *vm, ant_value_t value) {
  const sv_async_inline_t *rec = &vm->async_inline.records[vm->async_inline.len - 1];
  
  if (rec->coro) return false;
  if (!sv_await_type_is_primitive(vtype(value))) return false;
  
  uint8_t next = sv_async_inline_next_op(vm, rec);
  return next == OP_AWAIT || next == OP_AWAIT_RETURN;
}

static __attribute__((noinline)) ant_value_t sv_async_inline_ensure_coro(sv_vm_t *vm, ant_t *js) {
  sv_async_inline_t *entry = &vm->async_inline.records[vm->async_inline.len - 1];
  if (entry->coro) return Ant_Value_Undefined;

  ant_value_t promise = js_mkpromise(js);
  if (is_err(promise)) return promise;

  coroutine_t *coro = coroutine_alloc(js);
  if (!coro) return js_mkerr(js, "out of memory for async coroutine");

  sv_frame_t *frame = &vm->frames[entry->fp];
  sv_async_init_activation(
    coro, js, promise, frame->this,
    frame->super_val, Ant_Value_Undefined, frame->callee, frame->argc
  );
  
  sv_async_link_activation(js, coro);
  entry->coro = coro;
  
  return Ant_Value_Undefined;
}

static inline bool sv_async_inline_top_pending(const sv_vm_t *vm) {
  coroutine_t *coro = vm->async_inline.records[vm->async_inline.len - 1].coro;
  return coro && coroutine_pending(coro);
}

static __attribute__((noinline)) ant_value_t sv_async_osr_enter(
  sv_vm_t *vm, ant_t *js, sv_frame_t *frame, sv_func_t *func, int bc_offset
) {
  if (sv_async_inline_at(vm, vm->fp)) {
    ant_value_t ensured = sv_async_inline_ensure_coro(vm, js);
    if (is_err(ensured)) return ensured;
  }
  return sv_jit_try_osr(vm, js, frame, func, bc_offset);
}

static __attribute__((noinline)) ant_value_t sv_async_inline_reject(
  sv_vm_t *vm, ant_t *js, ant_value_t err, int boundary_fp
) {
  ant_value_t reason = is_err(err) ? js_take_thrown(js, err) : err;
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, reason);

  for (int f = vm->fp; f >= boundary_fp; f--) {
    ant_value_t *drop_bp = vm->frames[f].bp;
    if (drop_bp && vm->open_upvalues) sv_close_upvalues_from_slot(vm, drop_bp);
    sv_drop_frame_runtime_state(js, &vm->frames[f]);
  }

  vm->sp = vm->frames[boundary_fp].prev_sp;
  vm->handler_depth = vm->frames[boundary_fp].handler_base;
  vm->fp = boundary_fp - 1;

  ant_value_t promise = sv_async_inline_settle(vm, js, reason, true, false);
  GC_ROOT_RESTORE(js, root_mark);
  
  return promise;
}

static void sv_async_inline_discard(sv_vm_t *vm, ant_t *js, int min_fp) {
  while (vm->async_inline.len && vm->async_inline.records[vm->async_inline.len - 1].fp >= min_fp) {
    coroutine_t *coro = vm->async_inline.records[--vm->async_inline.len].coro;
    if (!coro) continue;
    sv_async_unlink_activation(js, coro);
    coroutine_release(coro);
  }
}

static bool sv_async_inline_reserve(sv_vm_t *vm) {
  if (vm->async_inline.len < vm->async_inline.cap) return true;
  
  int cap = vm->async_inline.cap ? vm->async_inline.cap * 2 : 16;
  sv_async_inline_t *grown = realloc(vm->async_inline.records, (size_t)cap * sizeof(*grown));
  
  if (!grown) return false;
  vm->async_inline.records = grown;
  vm->async_inline.cap = cap;
  
  return true;
}

int sv_async_entry_begin(sv_vm_t *vm, uint8_t next_op) {
  if (!sv_async_inline_reserve(vm)) return -1;
  int entry_fp = vm->fp + 1;
  
  vm->async_inline.records[vm->async_inline.len++] = (sv_async_inline_t){
    .fp = entry_fp, 
    .entry = true, 
    .next_op = next_op,
  };
  
  return entry_fp;
}

static ant_value_t sv_async_rejected(ant_t *js, ant_value_t err) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t reason = js_take_thrown(js, err);
  
  GC_ROOT_PIN(js, reason);
  ant_value_t promise = js_mkpromise(js);
  
  if (!is_err(promise)) js_reject_promise(js, promise, reason);
  GC_ROOT_RESTORE(js, root_mark);
  
  return promise;
}

ant_value_t sv_async_entry_finish(sv_vm_t *vm, int entry_fp, ant_value_t result) {
  ant_t *js = vm->js;
  if (vm->async_inline.len && vm->async_inline.records[vm->async_inline.len - 1].fp >= entry_fp)
    sv_async_inline_discard(vm, js, entry_fp);
  if (!is_err(result) || js_terminating_error(js, result)) return result;
  return sv_async_rejected(js, result);
}

static ant_value_t sv_async_settle_direct(ant_t *js, ant_value_t result, uint8_t next_op) {
  if (is_err(result)) return js_terminating_error(js, result) ? result : sv_async_rejected(js, result);
  if (sv_await_type_is_primitive(vtype(result)) && (next_op == OP_AWAIT || next_op == OP_AWAIT_RETURN)) return result;
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, result);
  
  ant_value_t promise = js_mkpromise(js);
  if (!is_err(promise)) js_resolve_promise(js, promise, result);
  GC_ROOT_RESTORE(js, root_mark);
  
  return promise;
}

ant_value_t sv_async_call_lazy(
  sv_vm_t *vm, sv_closure_t *closure, ant_value_t callee_func, ant_value_t super_val,
  ant_value_t this_val, ant_value_t *args, int argc, uint8_t next_op
) {
  ant_t *js = vm->js;
  sv_func_t *fn = closure->func;

  if (!fn->jit_async_direct_off) {
    sv_jit_func_t jit = fn->jit_code ? fn->jit_code : sv_direct_closure_jit_prepare(js, fn, closure);
    if (jit) {
      vm->jit.async_direct = true;
      vm->jit.async_next_op = next_op;
      vm->jit.async_settled = false;
      
      ant_value_t result = sv_jit_invoke(js, SV_JIT_FROM_C, jit, vm, &(sv_call_ctx_t){
        .this_val = this_val,
        .new_target = Ant_Value_Undefined,
        .super_val = super_val,
        .args = args,
        .argc = argc,
      }, closure);
      
      vm->jit.async_direct = false;
      if (vm->jit.async_settled) {
        vm->jit.async_settled = false;
        return result;
      }
      
      if (!sv_is_jit_bailout(result)) return sv_async_settle_direct(js, result, next_op);
      sv_jit_on_bailout(fn);
    }
  }

  int entry_fp = sv_async_entry_begin(vm, next_op);
  if (entry_fp < 0) return Ant_Error_CreateOutOfMemory(js);
  
  ant_value_t result = sv_execute_closure_entry(
    vm, closure, callee_func, super_val, 
    Ant_Value_Undefined, this_val, args, argc, NULL
  );
  
  return sv_async_entry_finish(vm, entry_fp, result);
}

static inline void sv_async_record_await_call(sv_func_t *caller, uint8_t *ip, int op_size, sv_func_t *callee) {
  if (!caller->is_async || !sv_func_type_feedback(caller)) return;
  uint8_t next = ip[op_size];
  if (next == OP_AWAIT || next == OP_AWAIT_RETURN)
    sv_tfb_record_call_target(caller, (int)(ip - caller->code), callee);
}

static __attribute__((noinline)) ant_value_t sv_async_inline_call(
  sv_vm_t *vm, ant_t *js, sv_closure_t *closure, ant_value_t call_func, ant_value_t call_this,
  ant_value_t *call_args, int call_argc, int pop_count, uint8_t *next_ip
) {
  if (vm->fp + 1 >= vm->max_frames && !sv_vm_grow_frames(vm))
    return Ant_Error_CreateStackOverflow(js);

  if (!sv_async_inline_reserve(vm)) return Ant_Error_CreateOutOfMemory(js);
  if (sv_closure_has_lexical_this(closure)) call_this = closure->bound_this;

  sv_func_t *func = closure->func;
  vm->frames[vm->fp].ip = next_ip;
  vm->sp -= pop_count;
  vm->fp++;

  sv_frame_t *frame = &vm->frames[vm->fp];
  frame->func = func;
  frame->callee = call_func;
  frame->this = call_this;
  frame->new_target = Ant_Value_Undefined;
  frame->super_val = closure->super_val;
  frame->prev_sp = vm->sp;
  frame->handler_base = (uint16_t)vm->handler_depth;
  frame->handler_top = (uint16_t)vm->handler_depth;
  frame->argc = call_argc;
  frame->completion.kind = SV_COMPLETION_NONE;
  frame->completion.value = Ant_Value_Undefined;
  frame->arguments_obj = Ant_Value_Undefined;
  frame->eval_env = sv_closure_eval_env(closure);

  ant_value_t *call_bp = NULL;
  ant_value_t *call_lp = NULL;
  
  ant_value_t stage_err = sv_stage_frame_args(
    vm, js, func, call_args,
    call_argc, &call_bp, &call_lp
  );
  
  if (is_err(stage_err)) {
    vm->fp--;
    vm->sp += pop_count;
    return stage_err;
  }

  frame->bp = call_bp;
  frame->lp = call_lp;
  frame->upvalues = closure->upvalues;
  frame->upvalue_count = func->upvalue_count;
  
  frame->this = sv_normalize_this_for_frame(js, func, frame->this);
  vm->async_inline.records[vm->async_inline.len++] = (sv_async_inline_t){ .fp = vm->fp };
  
  return Ant_Value_Undefined;
}

#include "silver/ops/macros.h"

__attribute__((noinline))
ant_value_t sv_execute_frame(sv_vm_t *vm, sv_func_t *func, ant_value_t this, ant_value_t super_val, ant_value_t *args, int argc) {
  ant_t *js = vm->js;
  
  bool resuming = vm->suspend.active && vm->suspend.pending;
  uint8_t *ip = resuming ? NULL : func->code;
  
  int entry_fp = resuming 
    ? vm->suspend.entry_fp 
    : vm->fp;
  
  ant_value_t vm_result = Ant_Value_Undefined;
  ant_value_t suspended_resume_value = Ant_Value_Undefined;
  sv_resume_kind_t suspended_resume_kind = SV_RESUME_NEXT;
  
  js->vm_exec_depth++;

  /*
  * This activation's native frame holds no authoritative JS values, so the
  * collector may skip it (see gc_vm_seg_t). The exception is the callee-saved
  * registers the prologue stored: they belong to the caller, and a C caller
  * may keep live JS values in them.
  *
  * __builtin_unwind_init() makes the prologue save every callee-saved
  * register, so that save area has a fixed size and sits directly below the
  * frame record. The skipped segment ends where the save area begins.
  *
  * Some compilers (GCC on AArch64) put the saves above the frame record
  * instead, leaving less than a full save area below it. The segment is then
  * empty rather than inverted, so nothing is skipped.
  */
#if defined(__GNUC__)
  __builtin_unwind_init();
#endif
  gc_vm_seg_t interp_seg;
  uintptr_t seg_fp = (uintptr_t)__builtin_frame_address(0);
  uintptr_t seg_hi = seg_fp - GC_VM_SEG_SAVED_REGS_BYTES;
  
  interp_seg.prev = js->vm_segs;
  interp_seg.lo = gc_native_sp();
  interp_seg.hi = seg_hi > interp_seg.lo ? seg_hi : interp_seg.lo;
  interp_seg.fp = seg_fp;
  interp_seg.jit_depth = js->jit_active_depth;
  interp_seg.entry_vm_fp = entry_fp;
  js->vm_segs = &interp_seg;

  sv_frame_t *frame = &vm->frames[vm->fp];
  if (!resuming) {
    frame->this = this;
    frame->super_val = super_val;
    
    if (vtype(frame->arguments_obj) != kTypeUndefined) {
      GC_ROOT_SAVE(entry_mark, js);
      sv_pin_entry_values(js, vm, args, argc);
      sv_drop_frame_runtime_state(js, frame);
      GC_ROOT_RESTORE(js, entry_mark);
    }
    
    frame->ip = ip;
    frame->func = func;
    frame->prev_sp = vm->sp;
    frame->handler_base = (uint16_t)vm->handler_depth;
    frame->handler_top = (uint16_t)vm->handler_depth;
    frame->argc = argc;
    frame->completion.kind = SV_COMPLETION_NONE;
    frame->completion.value = Ant_Value_Undefined;
    frame->arguments_obj = Ant_Value_Undefined;
  } else {
    func = frame->func;
    ip = frame->ip;
    suspended_resume_value = vm->suspend.value;
    suspended_resume_kind = vm->suspend.kind;
    vm->suspend.active = false;
    vm->suspend.pending = false;
    vm->suspend.is_error = false;
    vm->suspend.kind = SV_RESUME_NEXT;
    vm->suspend.value = Ant_Value_Undefined;
  }
  
  ant_value_t *entry_bp = NULL;
  ant_value_t *entry_lp = NULL;
  
  if (!resuming) {
  ant_value_t stage_err = sv_stage_frame_args(vm, js, func, args, argc, &entry_bp, &entry_lp);
  if (is_err(stage_err)) {
    if (vm->jit.resume.active) sv_clear_jit_resume(vm);
    VM_LEAVE(stage_err);
  }}
  
  if (!resuming && vm->jit.resume.active) {
    ip = func->code + vm->jit.resume.ip_offset;
    frame->ip = ip;
    
    int64_t rp =
      vm->jit.resume.n_params < func->param_count
      ? vm->jit.resume.n_params : func->param_count;
    
    for (int64_t i = 0; i < rp; i++)
      entry_bp[i] = vm->jit.resume.params[i];
    
    int64_t rl = 
      vm->jit.resume.n_locals < func->max_locals
      ? vm->jit.resume.n_locals : func->max_locals;
    
    for (int64_t i = 0; i < rl; i++)
      entry_lp[i] = vm->jit.resume.locals[i];

    sv_rebase_open_upvalues(&vm->open_upvalues, vm->jit.resume.params, entry_bp, (size_t)rp);
    sv_rebase_open_upvalues(&vm->open_upvalues, vm->jit.resume.locals, entry_lp, (size_t)rl);
    if (is_object_type(vm->jit.resume.arguments_obj)) frame->arguments_obj = vm->jit.resume.arguments_obj;
  }
  
  if (!resuming) {
    frame->bp = entry_bp;
    frame->lp = entry_lp;
  }

  ant_value_t *lp = frame->lp;

  if (!resuming && vm->jit.resume.active) {
    int base_sp = vm->sp;
    for (int64_t i = 0; i < vm->jit.resume.vstack_sp; i++)
      vm->stack[vm->sp++] = vm->jit.resume.vstack[i];

    sv_frame_push_try_handlers(vm, frame, func, (int)vm->jit.resume.ip_offset, base_sp);
    if (vm->jit.resume.finally) sv_resume_finally_entry(vm, frame, func);
    
    if (vm->jit.resume.throwing && vm->sp > base_sp) {
      frame->completion.kind = SV_COMPLETION_THROW;
      frame->completion.value = vm->stack[vm->sp - 1];
    }
    
    sv_clear_jit_resume(vm);
  }

  if (!resuming) frame->this = sv_normalize_this_for_frame(js, func, frame->this);

  static const void *dispatch[OP__COUNT] = {
    #define OP_DEF(name, size, n_pop, n_push, f) [OP_##name] = &&L_##name,
    #include "silver/opcode.h"
  };
  
  ant_value_t sv_err;
  ant_value_t  tc_this = Ant_Value_Undefined;
  uint16_t tc_argc = 0;
  
  if (resuming) {
    bool yield_star_resume = ip && (
      *ip == OP_YIELD_STAR_NEXT   ||
      *ip == OP_YIELD_STAR_THROW  ||
      *ip == OP_YIELD_STAR_RETURN ||
      *ip == OP_YIELD_STAR_ASYNC_CALL
    );
    
    if (!yield_star_resume) {
      sv_resume_kind_t resume_kind = suspended_resume_kind;
      suspended_resume_kind = SV_RESUME_NEXT;
      
      if (resume_kind == SV_RESUME_THROW) VM_THROW(js_throw(js, suspended_resume_value));
      
      if (resume_kind == SV_RESUME_RETURN) {
        vm->stack[vm->sp++] = suspended_resume_value;
        goto L_RETURN;
      }
    }
    
    vm->stack[vm->sp++] = suspended_resume_value;
    if (__builtin_expect(func->jit_await_park, 0) && func->jit_code && vm->fp == entry_fp) {
      ant_value_t osr_r = sv_jit_await_resume(vm, js, frame, func, (int)(ip - func->code));
      if (osr_r == SV_JIT_AWAIT_PARKED) ip = frame->ip;
      else if (osr_r != SV_JIT_RETRY_INTERP) {
        if (is_err(osr_r)) VM_THROW(osr_r);
        VM_LEAVE(osr_r);
      }
    }
  }
  
  DISPATCH();

  #include "ops/handlers.h"

  sv_interrupt: {
    frame->ip = ip;
    VM_THROW(js_interrupt_raise(js));
  }

  sv_throw: {
    int throw_floor = entry_fp;
    
    bool async_floor =
      vm->async_inline.len != 0 && 
      vm->async_inline.records[vm->async_inline.len - 1].fp >= entry_fp && !js_terminating_error(js, sv_err);
    
    if (async_floor) throw_floor = vm->async_inline.records[vm->async_inline.len - 1].fp;
    
    uint8_t *catch_ip = sv_vm_throw(vm, sv_err, throw_floor);
    if (catch_ip) {
      VM_RELOAD_FRAME();
      ip = catch_ip;
      DISPATCH();
    }
    
    if (async_floor) {
      ant_value_t promise = sv_async_inline_reject(vm, js, sv_err, throw_floor);
      if (vm->fp < entry_fp) VM_LEAVE(promise);
      VM_RELOAD_FRAME();
      ip = frame->ip;
      vm->stack[vm->sp++] = promise;
      DISPATCH();
    }
    
    if (!is_err(sv_err)) VM_LEAVE(js_throw(js, sv_err));
    VM_LEAVE(sv_err);
  }

  sv_leave:
  js->vm_segs = interp_seg.prev;
  if (__builtin_expect(vm->async_inline.len != 0, 0)) sv_async_inline_discard(vm, js, entry_fp);
  
  if (vm->suspend.active) {
    if (js->vm_exec_depth > 0) js->vm_exec_depth--;
    return vm_result;
  }
  
  if (vm->open_upvalues) for (int f = vm->fp; f >= entry_fp; f--) {
    ant_value_t *drop_bp = vm->frames[f].bp;
    if (drop_bp) sv_close_upvalues_from_slot(vm, drop_bp);
  }
  
  GC_ROOT_SAVE(leave_mark, js);
  bool leave_pinned = false;
  
  for (int f = vm->fp; f >= entry_fp; f--) {
    if (!leave_pinned && vtype(vm->frames[f].arguments_obj) != kTypeUndefined) {
      GC_ROOT_PIN(js, vm_result);
      leave_pinned = true;
    }
    sv_drop_frame_runtime_state(js, &vm->frames[f]);
  }
  
  GC_ROOT_RESTORE(js, leave_mark);
  vm->fp = entry_fp;
  vm->sp = vm->frames[entry_fp].prev_sp;
  vm->handler_depth = vm->frames[entry_fp].handler_base;
  if (js->vm_exec_depth > 0) js->vm_exec_depth--;

  return vm_result;
}

#undef VM_THROW
#undef VM_LEAVE
#undef VM_RELOAD_FRAME
#undef VM_CHECK
#undef DISPATCH
#undef NEXT
#undef COMPLETE_FRAME
#undef SUSPEND_YIELD
#undef RETURN_VALUE
#undef JIT_OSR_FINISH
#undef JIT_OSR_BACK_EDGE

ant_value_t sv_resume_suspended(sv_vm_t *vm) {
  if (!vm || !vm->suspend.active || !vm->suspend.pending || vm->fp < 0)
    return vm && vm->js
      ? js_mkerr_typed(vm->js, JS_ERR_INTERNAL | JS_ERR_NO_STACK, "invalid suspended continuation")
      : mkval(kTypeError, 0);

  // crash-resistance for missing frames 
  if (vm->suspend.entry_fp < 0 || vm->suspend.entry_fp > vm->fp) {
    vm->suspend.active = false;
    vm->suspend.pending = false;
    vm->suspend.is_error = false;
    vm->suspend.kind = SV_RESUME_NEXT;
    vm->suspend.value = Ant_Value_Undefined;
    vm->suspend.entry_fp = -1;
    vm->suspend.saved_fp = -1;
    
    return vm->js
      ? js_mkerr(vm->js, "invalid suspended entry frame")
      : mkval(kTypeError, 0);
  }

  int saved_fp = vm->suspend.saved_fp;
  sv_frame_t *frame = &vm->frames[vm->fp];
  
  if (!vm->js || !frame->func || !frame->ip) {
    vm->suspend.active = false;
    vm->suspend.pending = false;
    vm->suspend.is_error = false;
    vm->suspend.kind = SV_RESUME_NEXT;
    vm->suspend.value = Ant_Value_Undefined;
    vm->suspend.entry_fp = -1;
    vm->suspend.saved_fp = -1;
    
    return vm->js
      ? js_mkerr(vm->js, "invalid suspended frame state")
      : mkval(kTypeError, 0);
  }

  ant_realm_t *prev_realm = ant_realm_switch(vm->js, frame->func->realm);
  ant_value_t result = sv_execute_frame(vm, frame->func, frame->this, frame->super_val, NULL, frame->argc);
  ant_realm_restore(vm->js, prev_realm);

  if (!vm->suspend.active) {
    vm->fp = saved_fp;
    vm->suspend.entry_fp = -1;
    vm->suspend.saved_fp = -1;
  }

  return result;
}
