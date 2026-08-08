#ifndef ANT_GC_H
#define ANT_GC_H

#include "internal.h"
#include <stdbool.h>
#include <stdint.h>

static constexpr size_t GC_MAJOR_SCALE = 2048;
static constexpr size_t GC_MIN_TICK    = 1024;

static constexpr uint64_t GC_FORCE_INTERVAL_MS   = 50;
/* The periodic backstop runs minors at GC_FORCE_INTERVAL_MS; a full major
   only this often (sub-threshold steady allocation, e.g. servers). */
static constexpr uint64_t GC_FORCE_MAJOR_INTERVAL_MS = 1000;
static constexpr uint32_t GC_MAJOR_EVERY_N_MINOR = 8;

static constexpr size_t GC_NURSERY_THRESHOLD         = 32768;
static constexpr size_t GC_CLOSURE_NURSERY_THRESHOLD = 131072;
/* Promotions since the last major that force one (~36MB of arena slots);
   see the major_due clause in gc_maybe. */
static constexpr size_t GC_CLOSURE_PROMOTED_MAJOR    = 262144;
/* Bytes of closure-arena watermark growth since the last major; the
   watermark only rises when the free list is empty, i.e. when young
   reclaim is not keeping up. */
static constexpr size_t GC_CLOSURE_MAJOR_GROWTH      = 16u * 1024u * 1024u;

static constexpr size_t GC_POOL_PRESSURE_FLOOR = 8u * 1024u * 1024u;
static constexpr size_t GC_ROPE_NURSERY_THRESHOLD = 8u * 1024u * 1024u;

#define GC_OBJ_TYPE_MASK (T_FLAG_FIND(T_OBJ) \
  | T_FLAG_FIND(T_ARR)                       \
  | T_FLAG_FIND(T_PROMISE)                   \
  | T_FLAG_FIND(T_GENERATOR))

typedef struct gc_func_mark_profile {
  bool enabled;
  uint64_t collections;
  uint64_t func_visits;
  uint64_t child_edges;
  uint64_t const_slots;
  uint64_t time_ns;
} gc_func_mark_profile_t;

void gc_run(ant_t *js);
void gc_run_minor(ant_t *js);
void gc_maybe(ant_t *js);
void gc_pressure(ant_t *js);

void gc_remember_add(ant_t *js, ant_object_t *obj);
void gc_remember_func_const(ant_t *js, sv_func_t *func, uint32_t slot, ant_value_t value);
void gc_remember_upvalue(ant_t *js, struct sv_upvalue *uv);
void gc_remember_closure(ant_t *js, struct sv_closure *c);
void gc_remember_builder(ant_t *js, ant_string_builder_t *builder);
void gc_track_young_closure_slow(ant_t *js, struct sv_closure *c);
void gc_track_young_upvalue_slow(ant_t *js, struct sv_upvalue *uv);

size_t gc_live_major_threshold(ant_t *js);
size_t gc_pool_major_threshold(ant_t *js);

void gc_func_mark_profile_enable(bool enabled);
void gc_func_mark_profile_reset(void);

extern bool gc_disabled;
gc_func_mark_profile_t gc_func_mark_profile_get(void);

static inline bool gc_value_is_heap_ref(ant_value_t v) {
  if (v <= NANBOX_PREFIX) return false;
  uint8_t type = (v >> NANBOX_TYPE_SHIFT) & NANBOX_TYPE_MASK;
  return type == T_FUNC || type == T_STR || (((1u << type) & GC_OBJ_TYPE_MASK) != 0);
}

static inline bool gc_value_ref_is_young(ant_value_t v) {
  uint8_t type = (v >> NANBOX_TYPE_SHIFT) & NANBOX_TYPE_MASK;
  if (type == T_FUNC) return true;
  if (type == T_STR)
    return str_is_heap_rope(v) &&
      (ant_str_rope_ptr(v)->flags & ANT_ROPE_FLAG_YOUNG) != 0;
  ant_object_t *ref = (ant_object_t *)(uintptr_t)(v & NANBOX_DATA_MASK);
  return ref && ref->flags.generation == 0;
}

static inline void gc_write_barrier(ant_t *js, ant_object_t *writer_obj, ant_value_t new_val) {
  if (writer_obj->flags.generation != 1) return;
  if (gc_value_is_heap_ref(new_val) && gc_value_ref_is_young(new_val)) gc_remember_add(js, writer_obj);
}

#endif
