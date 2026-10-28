#ifndef ANT_OBJECT_H
#define ANT_OBJECT_H

#include "types.h"
#include "value.h"
#include "sugar.h"
#include "shapes.h"

#include <utarray.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct ant_proto_watch ant_proto_watch_t;
void ant_proto_watch_break(ant_object_t *obj);

typedef enum: uint8_t {
  ANT_EXOTIC_KEYS_ANY,
  ANT_EXOTIC_KEYS_NUMERIC,
} ant_exotic_keys_t;

typedef struct {
  ant_value_t (*getter)(ant_t *, ant_value_t, const char *, size_t);
  bool (*setter)(ant_t *, ant_value_t, const char *, size_t, ant_value_t);
  bool (*deleter)(ant_t *, ant_value_t, const char *, size_t);
  ant_value_t (*definer)(ant_t *, ant_value_t, const char *, size_t, ant_value_t value, bool has_value, bool plain_data);
  ant_exotic_keys_t keys;
} ant_exotic_ops_t;

typedef struct {
  const char *name;
  ant_value_t (*get)(ant_t *, ant_value_t target, const char *key, size_t key_len);
  ant_value_t (*set)(ant_t *, ant_value_t target, const char *key, size_t key_len, ant_value_t value);
  ant_value_t (*del)(ant_t *, ant_value_t target, const char *key, size_t key_len);
  ant_value_t (*keys)(ant_t *, ant_value_t target);
  ant_value_t (*flat_entries)(ant_t *, ant_value_t target);
} ant_host_proxy_ops_t;

typedef struct promise_handler {
  ant_value_t onFulfilled;
  ant_value_t onRejected;
  ant_value_t nextPromise;
  struct coroutine *await_coro;
} promise_handler_t;

typedef struct ant_promise_state {
  ant_value_t value;
  ant_value_t trigger_parent;
  
  struct ant_object *gc_pending_next;
  struct ant_object *gc_pending_prev;
  promise_handler_t inline_handler;
  
  UT_array *handlers;
  uint32_t promise_id;
  uint8_t state;
  
  bool has_inline_handler: 1;
  bool trigger_queued: 1;
  bool has_rejection_handler: 1;
  bool processing: 1;
  bool unhandled_reported: 1;
  bool gc_pending_rooted: 1;
} ant_promise_state_t;

static_assert(
  sizeof(void *) != 8 || sizeof(ant_promise_state_t) == 80,
  "promise state no longer fits an 80-byte allocation"
);

typedef struct {
  ant_value_t target;
  ant_value_t handler;
  const ant_host_proxy_ops_t *host;
  bool revoked;
  uint32_t plain_handler_epoch;
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
  sv_eval_env_state_t *eval_env_state;
  gc_card_table_t *gc_cards;
  ant_exotic_ops_t *owned_exotic_ops;
  ant_proto_watch_t *proto_watch;
  ant_shape_roots_t *shape_roots;
  
  uint8_t native_count;
  uint8_t native_cap;
  uint8_t extra_count;
  uint8_t extra_cap;
  uint8_t flags;
} ant_object_sidecar_t;

static_assert(
  _Alignof(ant_object_sidecar_t) > ant_sidecar, 
  "object sidecar pointer uses low-bit tag"
);

typedef union ant_object_flags {
  struct {
  #define ANT_OBJECT_FLAG(name, field) uint8_t field: 1;
  #include "object/flags.h"
  };
  uint16_t raw;
  uint8_t bytes[2];
} ant_object_flags_t;

typedef enum: uint8_t {
  #define ANT_OBJECT_FLAG(name, field) ANT_OBJECT_FLAG_BIT_##name,
  #include "object/flags.h"
  ANT_OBJECT_FLAG_COUNT
} ant_object_flag_bit_t;

typedef enum: uint16_t {
  #define ANT_OBJECT_FLAG(name, field) ANT_OBJECT_FLAG_##name = 1u << ANT_OBJECT_FLAG_BIT_##name,
  #include "object/flags.h"
} ant_object_flag_mask_t;

static_assert(
  sizeof(ant_object_flags_t) == 2 && ANT_OBJECT_FLAG_COUNT == 16,
  "ant_object_flags_t must cover the packed object bitfields"
);

static inline bool ant_object_flag_masks_match_layout(void) {
  #define ANT_OBJECT_FLAG(name, field) \
  if (((ant_object_flags_t){.field = 1}).raw != ANT_OBJECT_FLAG_##name) return false;
  #include "object/flags.h"
  return true;
}

typedef enum: uint8_t {
  ANT_PROTECTOR_HOLDER             = 1u << 0,
  ANT_PROTECTOR_HOLDER_REGEXP      = 1u << 1,
  ANT_PROTECTOR_HOLDER_ITERATOR    = 1u << 2,
  ANT_PROTECTOR_HOLDER_WATCHED     = 1u << 3,
  ANT_PROTECTOR_HOLDER_UNWATCHABLE = 1u << 4,
} ant_protector_holder_t;

typedef struct ant_object {
  struct ant_object *next;
  
  ant_value_t proto;
  ant_shape_t *shape;
  ant_value_t *overflow_prop;
  
  const ant_exotic_ops_t *exotic_ops;
  ant_extra_slot_t *extra_slots;
  
  void (*finalizer)(ant_t *, struct ant_object *);
  ant_value_t inobj[ANT_INOBJ_MAX_SLOTS];

  void *native_ptr;
  uint32_t native_tag;
  uint8_t protector_holder;

  union {
    struct { ant_value_t *data; uint32_t len; uint32_t cap; } array;
    struct { sv_closure_t *closure; } func;
    struct { ant_value_t value; } data;
    struct { ant_value_t value; ant_value_t stack; } exception;
    struct { ant_value_t value; ant_promise_state_t *state; } promise;
  } u;

  uint32_t prop_count;
  uint8_t mark_epoch;
  uint8_t type_tag;
  uint8_t inobj_limit;
  uint8_t extra_count;
  uint8_t extra_cap;
  uint8_t overflow_cap;
  
  ant_object_flags_t flags;
  uint32_t ic_identity;
} ant_object_t;

static constexpr size_t ANT_OBJECT_FLAGS_HIGH_BYTE = offsetof(ant_object_t, flags) + 1;

static_assert(
  sizeof(void *) != 8 || ANT_INOBJ_MAX_SLOTS != 4 || sizeof(ant_object_t) == 136,
  "ordinary 64-bit object layout changed"
);

static_assert(
  offsetof(ant_object_t, u.promise.value) == offsetof(ant_object_t, u.data.value),
  "promise objects keep SLOT_DATA in u.data.value"
);

static_assert(
  offsetof(ant_object_t, u.promise.state) == offsetof(ant_object_t, u.array.len) &&
  offsetof(ant_object_t, u.promise.state) + sizeof(ant_promise_state_t *) ==
  offsetof(ant_object_t, u.array.cap) + sizeof(uint32_t),
  "obj_alloc clears the promise state pointer through u.array.len and u.array.cap"
);

static_assert(
  __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, 
  "ANT_OBJECT_FLAGS_HIGH_BYTE assumes a little-endian flags word"
);

static_assert(
  ANT_OBJECT_FLAG_ARGUMENTS >= 0x100, 
  "the arguments flag must live in the high flags byte"
);

static_assert(
  (0xffffu & ~((ANT_OBJECT_FLAG_ARGUMENTS << 1) - 1u)) == ANT_OBJECT_FLAG_REGEXP_BRAND,
  "only the RegExp brand may sit above the arguments flag"
);

enum {
  ANT_ARGUMENTS_SLOT_LENGTH = 0,
  ANT_ARGUMENTS_SLOT_CALLEE = 1,
  ANT_ARGUMENTS_SLOT_ITERATOR = 2,
};

static inline bool ant_object_is_array(const ant_object_t *obj) {
  return obj && obj->type_tag == kTypeArray && !obj->flags.arguments_object;
}

static inline bool ant_object_is_arguments(const ant_object_t *obj) {
  return obj && obj->type_tag == kTypeArray && obj->flags.arguments_object;
}

static inline bool js_is_array(ant_value_t v) {
  return vtype(v) == kTypeArray && !((const ant_object_t *)vptr(v))->flags.arguments_object;
}

static inline bool js_is_arguments(ant_value_t v) {
  return vtype(v) == kTypeArray && ((const ant_object_t *)vptr(v))->flags.arguments_object;
}

static inline void ant_object_guard_absence(ant_object_t *obj) {
  if (obj) obj->flags.guards_absence = 1;
}

static inline bool ant_object_has_sidecar(const ant_object_t *obj) {
  return obj && (((uintptr_t)obj->extra_slots & ant_sidecar) != 0);
}

static inline ant_object_sidecar_t *ant_object_sidecar(const ant_object_t *obj) {
  if (!ant_object_has_sidecar(obj)) return NULL;
  return (ant_object_sidecar_t *)((uintptr_t)obj->extra_slots & ~ant_sidecar);
}

static inline ant_extra_slot_t *ant_object_extra_slots_ptr(const ant_object_t *obj) {
  if (!obj) return NULL;
  uintptr_t raw = (uintptr_t)obj->extra_slots;
  if ((raw & ant_sidecar) == 0) return obj->extra_slots;
  return ((ant_object_sidecar_t *)(raw & ~ant_sidecar))->extra_slots;
}

static inline uint8_t ant_object_extra_count(const ant_object_t *obj) {
  if (!obj) return 0;
  uintptr_t raw = (uintptr_t)obj->extra_slots;
  if ((raw & ant_sidecar) == 0) return obj->extra_count;
  return ((ant_object_sidecar_t *)(raw & ~ant_sidecar))->extra_count;
}

static inline uint8_t ant_object_extra_capacity(const ant_object_t *obj) {
  if (!obj) return 0;
  uintptr_t raw = (uintptr_t)obj->extra_slots;
  if ((raw & ant_sidecar) == 0) return obj->extra_cap;
  return ((ant_object_sidecar_t *)(raw & ~ant_sidecar))->extra_cap;
}

static inline ant_extra_slot_t *ant_object_extra_slots(const ant_object_t *obj, uint8_t *count) {
  if (!obj) {
    if (count) *count = 0;
    return NULL;
  }

  uintptr_t raw = (uintptr_t)obj->extra_slots;
  if ((raw & ant_sidecar) == 0) {
    if (count) *count = obj->extra_count;
    return obj->extra_slots;
  }
  
  ant_object_sidecar_t *sidecar = (ant_object_sidecar_t *)(raw & ~ant_sidecar);
  if (count) *count = sidecar->extra_count;
  return sidecar->extra_slots;
}

static inline ant_extra_slot_t *ant_object_extra_slot(const ant_object_t *obj, uint8_t slot) {
  if (!obj) return NULL;

  uint8_t count = obj->extra_count;
  ant_extra_slot_t *entries = obj->extra_slots;

  uintptr_t raw = (uintptr_t)obj->extra_slots;
  if ((raw & ant_sidecar) != 0) {
    ant_object_sidecar_t *sidecar = (ant_object_sidecar_t *)(raw & ~ant_sidecar);
    count = sidecar->extra_count;
    entries = sidecar->extra_slots;
  }
  
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
  sidecar->extra_cap = obj->extra_cap;
  
  obj->extra_slots = (ant_extra_slot_t *)((uintptr_t)sidecar | ant_sidecar);
  obj->extra_count = 0;
  obj->extra_cap = 0;
  
  return sidecar;
}

static inline ant_exotic_ops_t *ant_object_owned_exotic_ops(const ant_object_t *obj) {
  ant_object_sidecar_t *sidecar = ant_object_sidecar(obj);
  return sidecar && sidecar->owned_exotic_ops == obj->exotic_ops ? sidecar->owned_exotic_ops : NULL;
}

static inline ant_proxy_state_t *ant_object_proxy_state(const ant_object_t *obj) {
  ant_object_sidecar_t *sidecar = ant_object_sidecar(obj);
  return sidecar ? sidecar->proxy_state : NULL;
}

static inline sv_eval_env_state_t *ant_object_eval_env_state(const ant_object_t *obj) {
  ant_object_sidecar_t *sidecar = ant_object_sidecar(obj);
  return sidecar ? sidecar->eval_env_state : NULL;
}

static inline ant_private_table_t *ant_object_private_table(const ant_object_t *obj) {
  ant_object_sidecar_t *sidecar = ant_object_sidecar(obj);
  return sidecar ? (ant_private_table_t *)&sidecar->private_table : NULL;
}

static inline ant_promise_state_t *ant_object_promise_state(const ant_object_t *obj) {
  return obj && obj->type_tag == kTypePromise ? obj->u.promise.state : NULL;
}

static inline void ant_object_make_promise(ant_object_t *obj, ant_promise_state_t *state) {
  obj->type_tag = kTypePromise;
  obj->u.promise.state = state;
}

static inline uint32_t ant_object_overflow_cap(const ant_object_t *obj) {
  return obj->overflow_cap ? (1u << obj->overflow_cap) : 0;
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

static inline void ant_proto_watch_note_change(ant_object_t *obj) {
  if (__builtin_expect(obj->protector_holder & ANT_PROTECTOR_HOLDER_WATCHED, 0)) ant_proto_watch_break(obj);
}

static inline void ant_object_prop_set_unchecked(ant_object_t *obj, uint32_t slot, ant_value_t value) {
  ant_proto_watch_note_change(obj);
  uint32_t inobj_limit = ant_object_inobj_limit(obj);
  if (slot < inobj_limit) obj->inobj[slot] = value;
  else obj->overflow_prop[slot - inobj_limit] = value;
}

#endif
