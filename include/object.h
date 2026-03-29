#ifndef ANT_OBJECT_H
#define ANT_OBJECT_H

#include "types.h"
#include "sugar.h"
#include "shapes.h"

#include <utarray.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
  promise_handler_t inline_handler;
  UT_array *handlers;
  uint32_t promise_id;
  uint16_t handler_count;
  uint8_t state;
  bool trigger_queued;
  bool has_rejection_handler;
  bool processing;
  bool unhandled_reported;
} ant_promise_state_t;

typedef struct {
  ant_value_t target;
  ant_value_t handler;
  bool revoked;
} ant_proxy_state_t;

typedef struct ant_object {
  struct ant_object *next;
  
  ant_value_t proto;
  ant_shape_t *shape;
  ant_value_t *overflow_prop;
  
  const ant_exotic_ops_t *exotic_ops;
  ant_value_t (*exotic_keys)(ant_t *, ant_value_t);
  
  ant_promise_state_t *promise_state;
  ant_proxy_state_t *proxy_state;
  ant_value_t *extra_slots;
  
  struct ant_object *gc_pending_next;
  void (*finalizer)(ant_t *, struct ant_object *);
  ant_value_t inobj[ANT_INOBJ_MAX_SLOTS];
  
  struct {
    void *ptr;
    uint32_t tag;
  } native;

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

  uint8_t extensible: 1;
  uint8_t frozen: 1;
  uint8_t sealed: 1;
  uint8_t is_exotic: 1;
  uint8_t is_constructor: 1;
  uint8_t fast_array: 1;
  uint8_t gc_permanent: 1;
  uint8_t generation: 1;
  uint8_t in_remember_set: 1;

  bool gc_pending_rooted;
  uint8_t overflow_cap;
} ant_object_t;

typedef struct {
  uint8_t slot;
  ant_value_t value;
} ant_extra_slot_t;

typedef struct ant_prop_ref {
  ant_object_t *obj;
  uint32_t slot;
  bool valid;
} ant_prop_ref_t;

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
