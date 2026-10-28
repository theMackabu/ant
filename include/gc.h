#ifndef ANT_GC_H
#define ANT_GC_H

#include "internal.h"
#include <stdbool.h>
#include <stdint.h>

static constexpr size_t GC_MAJOR_MIN_PROMOTIONS = 4;
static constexpr size_t GC_MIN_TICK = 1024;

static constexpr uint32_t GC_MAJOR_EVERY_N_MINOR   = 8;
static constexpr uint32_t GC_MAJOR_TIME_SHARE_LOW  = 41;
static constexpr uint32_t GC_MAJOR_TIME_SHARE_HIGH = 102;
static constexpr uint32_t GC_MAJOR_WORK_SHARE_HIGH = 800;
static constexpr uint32_t GC_SKIP_MINOR_SURVIVAL   = 192;

static constexpr size_t GC_NURSERY_THRESHOLD = 32768;
static constexpr size_t GC_RECLAIM_MIN_FREE_DIVISOR = 64;
static constexpr size_t GC_CLOSURE_NURSERY_THRESHOLD = 131072;
static constexpr size_t GC_CLOSURE_PROMOTED_MAJOR = 262144;
static constexpr size_t GC_SKIP_PROBE_NURSERY = GC_NURSERY_THRESHOLD / 4;

static constexpr size_t GC_CLOSURE_MAJOR_GROWTH = 16u * 1024u * 1024u;
static constexpr size_t GC_POOL_PRESSURE_FLOOR = 8u * 1024u * 1024u;
static constexpr size_t GC_ROPE_NURSERY_THRESHOLD = 8u * 1024u * 1024u;
static constexpr size_t GC_ARRAY_GROWTH_FLOOR = 16u * 1024u * 1024u;
static constexpr size_t GC_OFFHEAP_NURSERY_THRESHOLD = 8u * 1024u * 1024u;
static constexpr size_t GC_OFFHEAP_MAJOR_FLOOR = 32u * 1024u * 1024u;

#define GC_OBJ_TYPE_MASK (T_FLAG_FIND(kTypeObject) \
  | T_FLAG_FIND(kTypeError)                        \
  | T_FLAG_FIND(kTypeArray)                        \
  | T_FLAG_FIND(kTypePromise)                      \
  | T_FLAG_FIND(kTypeGenerator))

void gc_state_init(ant_t *js);
void gc_run(ant_t *js);
void gc_run_minor(ant_t *js);
bool gc_safepoint(ant_t *js);
void gc_refresh_alloc_limit(ant_t *js);
void gc_array_grew(ant_t *js);
void gc_array_limits_init(ant_t *js);
void gc_alloc_check(ant_t *js);
void gc_pressure(ant_t *js);
bool gc_alloc_due(ant_t *js);
bool gc_idle_wanted(ant_t *js);
bool gc_reclaim_arena(ant_t *js, const struct ant_fixed_arena *arena);

size_t gc_alloc_marker(ant_t *js);
void gc_idle(ant_t *js, int64_t budget_ms);

void gc_remember_add(ant_t *js, ant_object_t *obj);
void gc_remember_props(ant_t *js, ant_object_t *obj);
void gc_remember_upvalue(ant_t *js, struct sv_upvalue *uv);
bool gc_upvalue_is_live(ant_t *js, const struct sv_upvalue *uv);
void gc_remember_coroutine(ant_t *js, struct coroutine *coro);
void gc_forget_coroutine(ant_t *js, struct coroutine *coro);
void gc_remember_closure(ant_t *js, struct sv_closure *c);
void gc_remember_builder(ant_t *js, ant_string_builder_t *builder);
void gc_track_young_closure_slow(ant_t *js, struct sv_closure *c);
void gc_track_young_upvalue_slow(ant_t *js, struct sv_upvalue *uv);

size_t gc_live_major_threshold(ant_t *js);
size_t gc_pool_major_threshold(ant_t *js);
size_t gc_code_major_threshold(ant_t *js);

void gc_func_mark_profile_enable(ant_t *js, bool enabled);
void gc_func_mark_profile_reset(ant_t *js);

extern bool gc_disabled;
gc_func_mark_profile_t gc_func_mark_profile_get(ant_t *js);

static inline bool gc_value_is_heap_ref(ant_value_t v) {
  if (!is_tagged(v)) return false;
  uint8_t type = vtype_tagged(v);
  if (type == kTypeError && vdata(v) < 2) return false;
  return 
    type == kTypeFunction || 
    type == kTypeString   || 
    (((1u << type) & GC_OBJ_TYPE_MASK) != 0);
}

static inline bool gc_value_ref_is_young(ant_value_t v) {
  uint8_t type = vtype_tagged(v);
  if (type == kTypeFunction) return true;
  if (type == kTypeString) return str_is_heap_rope(v) && (ant_str_rope_ptr(v)->flags & ANT_ROPE_FLAG_YOUNG) != 0;
  ant_object_t *ref = (ant_object_t *)vptr(v);
  return ref && ref->flags.generation == 0;
}

static inline void gc_write_barrier(ant_t *js, ant_object_t *writer_obj, ant_value_t new_val) {
  if (writer_obj->flags.generation != 1) return;
  if (writer_obj->flags.in_remember_set && writer_obj->type_tag != kTypeArray) return;
  if (gc_value_is_heap_ref(new_val) && gc_value_ref_is_young(new_val)) gc_remember_add(js, writer_obj);
}

static inline void gc_write_barrier_prop(ant_t *js, ant_object_t *writer_obj, ant_value_t new_val) {
  if (writer_obj->flags.generation != 1) return;
  if (gc_value_is_heap_ref(new_val) && gc_value_ref_is_young(new_val)) gc_remember_props(js, writer_obj);
}

static inline void gc_write_barrier_elem(ant_t *js, ant_object_t *arr, uint32_t idx, ant_value_t new_val) {
  if (arr->flags.generation != 1) return;
  if (!gc_value_is_heap_ref(new_val) || !gc_value_ref_is_young(new_val)) return;
  // too small for a card table (capacity never shrinks, so it never had
  // one) and already remembered whole: nothing more to record
  if (arr->flags.in_remember_set && arr->u.array.cap < GC_CARD_MIN_CAP) return;
  gc_remember_element(js, arr, idx);
}

static inline void gc_elements_moved(ant_object_t *arr) {
  if (arr->flags.in_remember_set) gc_cards_mark_all(arr);
}

static inline void gc_note_offheap_alloc(ant_t *js, size_t bytes) {
  if (!js) return;
  js->gc.offheap_alloc += bytes;
  if (js->gc.offheap_alloc >= GC_OFFHEAP_NURSERY_THRESHOLD) js->gc.alloc_limit = 0;
}

static inline void gc_note_overflow_alloc(ant_t *js, size_t bytes) {
  js->gc.overflow_live += bytes;
  gc_note_offheap_alloc(js, bytes);
}

static inline void gc_note_overflow_free(ant_t *js, size_t bytes) {
  ANT_ASSERT(js->gc.overflow_live >= bytes, "overflow slots released more bytes than were allocated");
  js->gc.overflow_live -= bytes;
}

#endif
