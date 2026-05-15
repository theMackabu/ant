#ifndef ANT_GC_H
#define ANT_GC_H

#include "internal.h"
#include <stdbool.h>
#include <stdint.h>

#define GC_MIN_TICK            1024
#define GC_NURSERY_THRESHOLD   32768
#define GC_FORCE_INTERVAL_MS   50
#define GC_MAJOR_EVERY_N_MINOR 8
#define GC_POOL_PRESSURE_FLOOR (8u * 1024u * 1024u)

#define GC_OBJ_TYPE_MASK (JS_TPFLG(T_OBJ) \
  | JS_TPFLG(T_ARR)                       \
  | JS_TPFLG(T_PROMISE)                   \
  | JS_TPFLG(T_GENERATOR))

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

void gc_remember_add(ant_t *js, ant_object_t *obj);
void gc_remember_func_const(ant_t *js, sv_func_t *func, uint32_t slot, ant_value_t value);

size_t gc_live_major_threshold(ant_t *js);
size_t gc_pool_major_threshold(ant_t *js);

void gc_func_mark_profile_enable(bool enabled);
void gc_func_mark_profile_reset(void);

extern bool gc_disabled;
gc_func_mark_profile_t gc_func_mark_profile_get(void);

static inline void gc_write_barrier(ant_t *js, ant_object_t *writer_obj, ant_value_t new_val) {
  if (writer_obj->flags.generation != 1 || new_val <= NANBOX_PREFIX) return;
  uint8_t type = (new_val >> NANBOX_TYPE_SHIFT) & NANBOX_TYPE_MASK;
  if (type == T_FUNC) gc_remember_add(js, writer_obj);
  else if ((1u << type) & GC_OBJ_TYPE_MASK) {
    ant_object_t *ref = (ant_object_t *)(uintptr_t)(new_val & NANBOX_DATA_MASK);
    if (ref && ref->flags.generation == 0) gc_remember_add(js, writer_obj);
  }
}

#endif
