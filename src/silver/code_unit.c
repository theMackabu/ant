#include "silver/code_unit.h"
#include "silver/engine.h"

#include "cage.h"
#include "runtime.h"
#include "gc/roots.h"
#include "shapes.h"

#include <stdlib.h>
#include <string.h>

static constexpr size_t SV_CODE_BLOCK_SIZE = 16 * 1024;
static constexpr uint32_t SV_CODE_SPARE_MAX = 64;

struct sv_code_block {
  sv_code_block_t *next;
  size_t used;
  size_t capacity;
  size_t alloc_size;
  uint32_t live_units;
  alignas(CODE_ARENA_ALIGNMENT) char data[];
};

static bool vec_grow(void **items, uint32_t *cap, uint32_t need, size_t elem) {
  if (need <= *cap) return true;
  uint32_t next = *cap ? *cap * 2 : 8;
  
  while (next < need) next *= 2;
  void *grown = realloc(*items, (size_t)next * elem);
  if (!grown) return false;
  
  *items = grown;
  *cap = next;
  
  return true;
}

static sv_code_block_t *block_take(sv_code_units_t *u, size_t size) {
  const size_t block_size = SV_CODE_BLOCK_SIZE;
  
  if (size <= block_size - offsetof(sv_code_block_t, data) && u->spare) {
    sv_code_block_t *block = u->spare;
    u->spare = block->next;
    u->spare_count--;
    block->next = NULL;
    u->held_bytes += block->alloc_size;
    return block;
  }

  size_t alloc_size = block_size;
  size_t need = offsetof(sv_code_block_t, data) + size;
  
  if (need > block_size) alloc_size = (need + block_size - 1) / block_size * block_size;
  sv_code_block_t *block = ant_cage_alloc(alloc_size, _Alignof(sv_code_block_t));
  if (!block) return NULL;

  block->next = NULL;
  block->used = 0;
  block->capacity = alloc_size - offsetof(sv_code_block_t, data);
  block->alloc_size = alloc_size;
  block->live_units = 0;
  u->held_bytes += alloc_size;
  
  return block;
}

static void block_release(sv_code_units_t *u, sv_code_block_t *block) {
  u->held_bytes -= block->alloc_size;
  
  if (block->alloc_size == SV_CODE_BLOCK_SIZE && u->spare_count < SV_CODE_SPARE_MAX) {
    memset(block->data, 0, block->used);
    block->used = 0;
    block->next = u->spare;
    u->spare = block;
    u->spare_count++;
    return;
  }
  
  ant_cage_free(block, block->alloc_size);
}

void *sv_code_unit_bump(sv_code_unit_t *unit, size_t size) {
  if (!unit) return NULL;
  if (size == 0) size = 1;

  sv_code_units_t *u = &unit->js->code_units;
  const size_t mask = (size_t)CODE_ARENA_ALIGNMENT - 1u;
  size = (size + mask) & ~mask;

  sv_code_block_t *block = u->current;
  if (!block || block->used + size > block->capacity) {
    sv_code_block_t *fresh = block_take(u, size);
    if (!fresh) return NULL;
    if (block && block->live_units == 0) block_release(u, block);
    u->current = block = fresh;
  }

  if (unit->block_count == 0 || unit->blocks[unit->block_count - 1] != block) {
    if (!vec_grow((void **)&unit->blocks, &unit->block_cap, unit->block_count + 1, sizeof(*unit->blocks))) return NULL;
    unit->blocks[unit->block_count++] = block;
    block->live_units++;
  }

  void *ptr = block->data + block->used;
  block->used += size;
  unit->js->gc.code_alloc += size;
  
  return ptr;
}

void *sv_code_bump(ant_t *js, size_t size) {
  sv_code_unit_t *unit = js->code_units.active;
  return unit ? sv_code_unit_bump(unit, size) : code_arena_bump(size);
}

const char *sv_code_text(ant_t *js, const char *code, size_t len) {
  sv_code_unit_t *unit = js->code_units.active;
  
  if (!unit) return code_arena_alloc(code, len);
  if (!code || len == 0) return NULL;
  
  char *dest = sv_code_unit_bump(unit, len + 1);
  if (!dest) return NULL;
  
  memcpy(dest, code, len);
  dest[len] = '\0';
  
  return dest;
}

void sv_code_unit_pin(sv_code_unit_t *unit) {
  if (!unit || unit->pins++) return;
  sv_code_units_t *u = &unit->js->code_units;
  unit->pinned_prev = NULL;
  unit->pinned_next = u->pinned;
  if (u->pinned) u->pinned->pinned_prev = unit;
  u->pinned = unit;
}

void sv_code_unit_unpin(sv_code_unit_t *unit) {
  if (!unit || !unit->pins || --unit->pins) return;
  sv_code_units_t *u = &unit->js->code_units;
  if (unit->pinned_prev) unit->pinned_prev->pinned_next = unit->pinned_next;
  else u->pinned = unit->pinned_next;
  if (unit->pinned_next) unit->pinned_next->pinned_prev = unit->pinned_prev;
  unit->pinned_prev = unit->pinned_next = NULL;
}

sv_code_unit_t *sv_code_unit_begin(ant_t *js) {
  sv_code_unit_t *unit = calloc(1, sizeof(*unit));
  if (!unit) return NULL;

  unit->js = js;
  sv_code_unit_pin(unit);
  
  unit->compiling = true;
  js->code_units.held_bytes += SV_CODE_UNIT_OVERHEAD;
  js->gc.code_alloc += SV_CODE_UNIT_OVERHEAD;
  unit->outer = js->code_units.active;
  unit->next = js->code_units.units;
  js->code_units.units = unit;
  js->code_units.unit_count++;
  js->code_units.active = unit;
  
  return unit;
}

void sv_code_unit_finish(sv_code_unit_t *unit) {
  if (!unit) return;
  
  unit->js->code_units.active = unit->outer;
  unit->outer = NULL;
  unit->compiling = false;
  free(unit->compile_roots);
  unit->compile_roots = NULL;
  unit->compile_root_count = unit->compile_root_cap = 0;

  ant_t *js = unit->js;
  if (js->gc.code_alloc >= gc_code_major_threshold(js)) gc_run(js);
}

void sv_code_unit_add_func(ant_t *js, sv_func_t *func) {
  sv_code_unit_t *unit = js->code_units.active;
  if (!unit || !func) return;
  func->unit = unit;
  func->unit_next = unit->funcs;
  unit->funcs = func;
  unit->func_count++;
}

bool sv_code_unit_root(ant_t *js, ant_value_t value) {
  sv_code_unit_t *unit = js->code_units.active;
  if (!unit) return true;
  
  if (!vec_grow(
    (void **)&unit->compile_roots, &unit->compile_root_cap,
    unit->compile_root_count + 1, sizeof(*unit->compile_roots)
  )) return false;
  
  unit->compile_roots[unit->compile_root_count++] = value;
  return true;
}

bool sv_code_unit_retain_template(ant_t *js, sv_func_t *func, ant_value_t value) {
  if (!func || !func->unit) return gc_pin_permanent(js, value);

  sv_code_units_t *u = &js->code_units;
  if (u->young_len >= u->young_cap) {
    size_t cap = u->young_cap ? u->young_cap * 2 : 64;
    ant_value_t *grown = realloc(u->young_values, cap * sizeof(*grown));
    if (!grown) return false;
    
    u->young_values = grown;
    u->young_cap = cap;
  }
  
  u->young_values[u->young_len++] = value;
  return true;
}

bool sv_code_units_watch_feedback(sv_func_t *func, sv_func_t *callee) {
  if (func->fb_unit_watched) return true;
  sv_code_units_t *u = &callee->unit->js->code_units;
  
  if (u->fb_watch_len >= u->fb_watch_cap) {
    size_t cap = u->fb_watch_cap ? u->fb_watch_cap * 2 : 64;
    sv_func_t **grown = realloc(u->fb_watch, cap * sizeof(*grown));
    if (!grown) return false;
    
    u->fb_watch = grown;
    u->fb_watch_cap = cap;
  }
  
  u->fb_watch[u->fb_watch_len++] = func;
  func->fb_unit_watched = true;
  
  return true;
}

static void func_free_sidecar(sv_func_t *func) {
  uintptr_t raw = (uintptr_t)func->type_feedback;
  if (!(raw & ant_sidecar)) return;
  sv_func_sidecar_t *sidecar = (sv_func_sidecar_t *)(raw & ~ant_sidecar);
  free(sidecar->type_feedback);
  free(sidecar);
  func->type_feedback = NULL;
}

static bool func_release(ant_t *js, sv_func_t *func) {
  bool dropped_slots = false;
  uintptr_t raw = (uintptr_t)func->type_feedback;
  
  if (raw & ant_sidecar) {
    sv_func_sidecar_t *sidecar = (sv_func_sidecar_t *)(raw & ~ant_sidecar);
    if (sidecar->ctor_proto_shape_registered) {
      ant_shape_t *shape = sidecar->ctor_proto_shape;
      sidecar->ctor_proto_shape = SV_IC_SHAPE_SLOT_DEAD;
      if (shape) ant_shape_release(shape);
      dropped_slots = true;
    }
  } else free(func->type_feedback);

  free(func->local_type_feedback);
  free(func->call_target_fb);
  
  if (!(raw & ant_sidecar)) func->type_feedback = NULL;
  func->local_type_feedback = NULL;
  func->call_target_fb = NULL;

  for (uint32_t i = 0; func->obj_sites && i < func->obj_site_count; i++) {
    ant_shape_t *shape = func->obj_sites[i].shared_shape;
    func->obj_sites[i].shared_shape = NULL;
    if (shape) ant_shape_release(shape);
  }

  for (uint32_t i = 0; func->ic_slots && i < func->ic_count; i++) {
    sv_ic_entry_t *ic = &func->ic_slots[i];
    sv_gf_poly_free(js, ic);
    sv_pf_poly_free(js, ic);
    
    ant_shape_t **slots[3] = {
      (ic->shape_ref_mask & SV_IC_SHAPE_REF_CACHED)   ? &ic->cached_shape         : NULL,
      (ic->shape_ref_mask & SV_IC_SHAPE_REF_ADD_FROM) ? &ic->guard.add.from_shape : NULL,
      (ic->shape_ref_mask & SV_IC_SHAPE_REF_ADD_TO)   ? &ic->guard.add.to_shape   : NULL,
    };
    
    for (int k = 0; k < 3; k++) {
      if (!slots[k]) continue;
      ant_shape_t *shape = *slots[k];
      *slots[k] = SV_IC_SHAPE_SLOT_DEAD;
      if (shape) ant_shape_release(shape);
      dropped_slots = true;
    }
    
    ic->shape_ref_mask = 0;
  }

  return dropped_slots;
}

static void unit_free_memory(sv_code_units_t *u, sv_code_unit_t *unit) {
  u->held_bytes -= SV_CODE_UNIT_OVERHEAD;
  
  for (uint32_t i = 0; i < unit->block_count; i++) {
    sv_code_block_t *block = unit->blocks[i];
    if (--block->live_units == 0 && block != u->current) block_release(u, block);
  }
  
  free(unit->blocks);
  free(unit->compile_roots);
  free(unit);
}

void sv_code_units_sweep(ant_t *js, uint64_t epoch) {
  sv_code_units_t *u = &js->code_units;
  sv_code_unit_t *dying = NULL;
  bool dropped_slots = false;

  for (sv_code_unit_t **link = &u->units; *link;) {
    sv_code_unit_t *unit = *link;
    if (!sv_code_unit_dying(unit, epoch)) {
      link = &unit->next;
      continue;
    }
    
    *link = unit->next;
    u->unit_count--;
    
    for (sv_func_t *func = unit->funcs; func; func = func->unit_next)
      dropped_slots |= func_release(js, func);  
    
    unit->next = dying;
    dying = unit;
  }

  if (!dying) return;
  if (dropped_slots) sv_ic_shape_refs_drop_dead(js);

  for (sv_code_unit_t *unit = dying, *next; unit; unit = next) {
    next = unit->next;
    for (sv_func_t *func = unit->funcs; func; func = func->unit_next) func_free_sidecar(func);
    unit_free_memory(u, unit);
  }
}

void sv_code_units_destroy(ant_t *js) {
  sv_code_units_t *u = &js->code_units;

  for (sv_code_unit_t *unit = u->units, *next; unit; unit = next) {
    next = unit->next;
    for (uint32_t i = 0; i < unit->block_count; i++) {
      sv_code_block_t *block = unit->blocks[i];
      if (--block->live_units == 0 && block != u->current) ant_cage_free(block, block->alloc_size);
    }
    
    free(unit->blocks);
    free(unit->compile_roots);
    free(unit);
  }
  
  u->units = NULL;
  u->pinned = NULL;
  u->unit_count = 0;

  if (u->current) ant_cage_free(u->current, u->current->alloc_size);
  u->current = NULL;

  for (sv_code_block_t *block = u->spare, *next; block; block = next) {
    next = block->next;
    ant_cage_free(block, block->alloc_size);
  }
  
  u->spare = NULL;
  u->spare_count = 0;

  free(u->young_values);
  u->young_values = NULL;
  u->young_len = u->young_cap = 0;

  free(u->fb_watch);
  u->fb_watch = NULL;
  u->fb_watch_len = u->fb_watch_cap = 0;
}
