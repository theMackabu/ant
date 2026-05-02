#ifndef ANT_OBJECT_H
#define ANT_OBJECT_H

#include "types.h"
#include "sugar.h"
#include "shapes.h"

#include <utarray.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct {
  ant_value_t (*getter)(ant_t *, ant_value_t, const char *, size_t);
  bool (*setter)(ant_t *, ant_value_t, const char *, size_t, ant_value_t);
  bool (*deleter)(ant_t *, ant_value_t, const char *, size_t);
} ant_exotic_ops_t;

typedef struct promise_handler {
  ant_value_t onFulfilled;
  ant_value_t onRejected;
  ant_value_t nextPromise;
  struct coroutine *await_coro;
} promise_handler_t;

typedef struct {
  ant_value_t value;
  ant_value_t trigger_parent;
  
  struct ant_object *gc_pending_next;
  promise_handler_t inline_handler;
  
  UT_array *handlers;
  uint32_t promise_id;
  uint16_t handler_count;
  uint8_t state;
  
  bool trigger_queued;
  bool has_rejection_handler;
  bool processing;
  bool unhandled_reported;
  bool gc_pending_rooted;
} ant_promise_state_t;

typedef struct {
  ant_value_t target;
  ant_value_t handler;
  bool revoked;
} ant_proxy_state_t;

typedef struct {
  uint8_t slot;
  ant_value_t value;
} ant_extra_slot_t;

typedef struct {
  ant_value_t token;
  ant_value_t value;
  ant_value_t getter;
  ant_value_t setter;
  
  uint32_t hash;
  uint8_t kind;
  uint8_t occupied;
} ant_private_entry_t;

typedef struct {
  ant_private_entry_t *entries;
  uint32_t count;
  uint32_t cap;
} ant_private_table_t;

typedef struct {
  void *ptr;
  uint32_t tag;
} ant_native_entry_t;

typedef struct {
  ant_extra_slot_t *extra_slots;
  ant_native_entry_t *native_entries;
  ant_private_table_t private_table;
  ant_proxy_state_t *proxy_state;
  
  uint8_t native_count;
  uint8_t native_cap;
  uint8_t extra_count;
  uint8_t flags;
} ant_object_sidecar_t;

typedef struct ant_prop_ref {
  ant_object_t *obj;
  uint32_t slot;
  bool valid;
} ant_prop_ref_t;

typedef struct ant_object {
  struct ant_object *next;
  
  ant_value_t proto;
  ant_shape_t *shape;
  ant_value_t *overflow_prop;
  
  const ant_exotic_ops_t *exotic_ops;
  ant_value_t (*exotic_keys)(ant_t *, ant_value_t);
  
  ant_promise_state_t *promise_state;
  ant_extra_slot_t *extra_slots;
  
  void (*finalizer)(ant_t *, struct ant_object *);
  ant_value_t inobj[ANT_INOBJ_MAX_SLOTS];
  ant_native_entry_t native;

  union {
    struct { ant_value_t *data; uint32_t len; uint32_t cap; } array;
    struct { sv_closure_t *closure; } func;
    struct { ant_value_t value; } data;
  } u;

  uint32_t prop_count;
  uint32_t propref_count;

  uint8_t mark_epoch;
  uint8_t type_tag;
  uint8_t inobj_limit;
  uint8_t extra_count;
  uint8_t overflow_cap;

  uint8_t extensible: 1;
  uint8_t frozen: 1;
  uint8_t sealed: 1;
  uint8_t is_exotic: 1;
  uint8_t is_constructor: 1;
  uint8_t fast_array: 1;
  uint8_t may_have_holes: 1;
  uint8_t may_have_dense_elements: 1;
  uint8_t gc_permanent: 1;
  uint8_t generation: 1;
  uint8_t in_remember_set: 1;
} ant_object_t;

static inline bool ant_object_has_sidecar(const ant_object_t *obj) {
  return obj && (((uintptr_t)obj->extra_slots & ant_sidecar) != 0);
}

static inline ant_object_sidecar_t *ant_object_sidecar(const ant_object_t *obj) {
  if (!ant_object_has_sidecar(obj)) return NULL;
  return (ant_object_sidecar_t *)((uintptr_t)obj->extra_slots & ~ant_sidecar);
}

static inline ant_extra_slot_t *ant_object_extra_slots_ptr(const ant_object_t *obj) {
  if (!obj) return NULL;
  ant_object_sidecar_t *sidecar = ant_object_sidecar(obj);
  return sidecar ? sidecar->extra_slots : obj->extra_slots;
}

static inline uint8_t ant_object_extra_count(const ant_object_t *obj) {
  if (!obj) return 0;
  ant_object_sidecar_t *sidecar = ant_object_sidecar(obj);
  return sidecar ? sidecar->extra_count : obj->extra_count;
}

static inline ant_extra_slot_t *ant_object_extra_slots(const ant_object_t *obj, uint8_t *count) {
  if (!obj) {
    if (count) *count = 0;
    return NULL;
  }
  
  ant_object_sidecar_t *sidecar = ant_object_sidecar(obj);
  if (sidecar) {
    if (count) *count = sidecar->extra_count;
    return sidecar->extra_slots;
  }
  
  if (count) *count = obj->extra_count;
  return obj->extra_slots;
}

static inline ant_extra_slot_t *ant_object_extra_slot(const ant_object_t *obj, uint8_t slot) {
  uint8_t count = 0;
  ant_extra_slot_t *entries = ant_object_extra_slots(obj, &count);
  
  for (uint8_t i = 0; i < count; i++) 
    if (entries[i].slot == slot) return &entries[i];
  
  return NULL;
}

static inline ant_object_sidecar_t *ant_object_ensure_sidecar(ant_object_t *obj) {
  if (!obj) return NULL;
  
  ant_object_sidecar_t *sidecar = ant_object_sidecar(obj);
  if (sidecar) return sidecar;

  sidecar = (ant_object_sidecar_t *)calloc(1, sizeof(*sidecar));
  if (!sidecar) return NULL;
  
  sidecar->extra_slots = obj->extra_slots;
  sidecar->extra_count = obj->extra_count;
  
  obj->extra_slots = (ant_extra_slot_t *)((uintptr_t)sidecar | ant_sidecar);
  obj->extra_count = 0;
  
  return sidecar;
}

static inline ant_proxy_state_t *ant_object_proxy_state(const ant_object_t *obj) {
  ant_object_sidecar_t *sidecar = ant_object_sidecar(obj);
  return sidecar ? sidecar->proxy_state : NULL;
}

static inline ant_private_table_t *ant_object_private_table(const ant_object_t *obj) {
  ant_object_sidecar_t *sidecar = ant_object_sidecar(obj);
  return sidecar ? (ant_private_table_t *)&sidecar->private_table : NULL;
}

static inline uint32_t ant_object_inobj_limit(const ant_object_t *obj) {
  if (!obj) return ANT_INOBJ_MAX_SLOTS;
  uint32_t limit = obj->inobj_limit;
  return (limit > ANT_INOBJ_MAX_SLOTS) ? ANT_INOBJ_MAX_SLOTS : limit;
}

static inline ant_value_t ant_object_prop_get_unchecked(const ant_object_t *obj, uint32_t slot) {
  uint32_t inobj_limit = ant_object_inobj_limit(obj);
  return (slot < inobj_limit)
    ? obj->inobj[slot]
    : obj->overflow_prop[slot - inobj_limit];
}

static inline void ant_object_prop_set_unchecked(ant_object_t *obj, uint32_t slot, ant_value_t value) {
  uint32_t inobj_limit = ant_object_inobj_limit(obj);
  if (slot < inobj_limit) obj->inobj[slot] = value;
  else obj->overflow_prop[slot - inobj_limit] = value;
}

#endif
