#pragma once

#include "types.h"
#include "value.h"
#include "sugar.h"
#include "shapes.h"

#include <utarray.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "common.h"

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
  uint32_t plain_set_epoch;
} ant_proxy_state_t;

typedef struct {
  uint8_t slot;
  ant_value_t value;
} ant_extra_slot_t;

typedef struct {
  uint8_t count;
  uint8_t cap;
  ant_extra_slot_t entries[];
} ant_extra_slots_t;

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
  ant_extra_slots_t *extra_slots;
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
  uint8_t flags;
  uint8_t protector_holder;
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
  ANT_PROTECTOR_HOLDER_TYPED_ARRAY = 1u << 5,
} ant_protector_holder_t;

typedef struct ant_object {
  struct ant_object *next;
  
  ant_shape_t *shape;
  ant_value_t *overflow_prop;
  ant_extra_slots_t *extra_slots;
  
  void (*finalizer)(ant_t *, struct ant_object *);
  void *native_ptr;

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
  
  ant_object_flags_t flags;
  uint32_t ic_identity;
  uint32_t native_tag;
  ant_value_t inobj[];
} ant_object_t;

static constexpr size_t ANT_OBJECT_SIZE = sizeof(ant_object_t) + ANT_INOBJ_BASE_SLOTS * sizeof(ant_value_t);
static constexpr size_t ANT_OBJECT_WIDE_SIZE = sizeof(ant_object_t) + ANT_INOBJ_WIDE_SLOTS * sizeof(ant_value_t);
static constexpr size_t ANT_OBJECT_LARGE_SIZE = sizeof(ant_object_t) + ANT_INOBJ_LARGE_SLOTS * sizeof(ant_value_t);
static constexpr size_t ANT_OBJECT_FLAGS_HIGH_BYTE = offsetof(ant_object_t, flags) + 1;

static_assert(
  sizeof(void *) != 8 || ANT_INOBJ_BASE_SLOTS != 4 || ANT_OBJECT_SIZE == 112,
  "ordinary 64-bit object layout changed"
);

static_assert(
  offsetof(ant_object_t, u.promise.value) == offsetof(ant_object_t, u.data.value),
  "promise objects keep SLOT_DATA in u.data.value"
);

static_assert(
  offsetof(ant_object_t, u.promise.state) >= offsetof(ant_object_t, u.array.len) &&
  offsetof(ant_object_t, u.promise.state) + sizeof(ant_promise_state_t *) <=
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

static inline ant_value_t ant_object_proto(const ant_object_t *obj) {
  return obj->shape ? ant_shape_proto(obj->shape) : Ant_Value_Undefined;
}

static inline const ant_exotic_ops_t *ant_object_exotic_ops(const ant_object_t *obj) {
  return obj->flags.is_exotic ? (const ant_exotic_ops_t *)ant_shape_exotic_ops(obj->shape) : NULL;
}

static inline bool ant_object_has_sidecar(const ant_object_t *obj) {
  return obj && (((uintptr_t)obj->extra_slots & ant_sidecar) != 0);
}

static inline ant_object_sidecar_t *ant_object_sidecar(const ant_object_t *obj) {
  if (!ant_object_has_sidecar(obj)) return NULL;
  return (ant_object_sidecar_t *)((uintptr_t)obj->extra_slots & ~ant_sidecar);
}

static inline ant_extra_slots_t *ant_object_extra_block(const ant_object_t *obj) {
  if (!obj) return NULL;
  uintptr_t raw = (uintptr_t)obj->extra_slots;
  if ((raw & ant_sidecar) == 0) return obj->extra_slots;
  return ((ant_object_sidecar_t *)(raw & ~ant_sidecar))->extra_slots;
}

static inline ant_extra_slot_t *ant_object_extra_slots_ptr(const ant_object_t *obj) {
  ant_extra_slots_t *block = ant_object_extra_block(obj);
  return block ? block->entries : NULL;
}

static inline uint8_t ant_object_extra_count(const ant_object_t *obj) {
  ant_extra_slots_t *block = ant_object_extra_block(obj);
  return block ? block->count : 0;
}

static inline uint8_t ant_object_extra_capacity(const ant_object_t *obj) {
  ant_extra_slots_t *block = ant_object_extra_block(obj);
  return block ? block->cap : 0;
}

static inline ant_extra_slot_t *ant_object_extra_slots(const ant_object_t *obj, uint8_t *count) {
  ant_extra_slots_t *block = ant_object_extra_block(obj);
  if (count) *count = block ? block->count : 0;
  return block ? block->entries : NULL;
}

static inline ant_extra_slot_t *ant_object_extra_slot(const ant_object_t *obj, uint8_t slot) {
  ant_extra_slots_t *block = ant_object_extra_block(obj);
  if (!block) return NULL;
  for (uint8_t i = 0; i < block->count; i++)
    if (block->entries[i].slot == slot) return &block->entries[i];
  return NULL;
}

static inline ant_object_sidecar_t *ant_object_ensure_sidecar(ant_object_t *obj) {
  if (!obj) return NULL;
  
  ant_object_sidecar_t *sidecar = ant_object_sidecar(obj);
  if (sidecar) return sidecar;

  sidecar = (ant_object_sidecar_t *)calloc(1, sizeof(*sidecar));
  if (!sidecar) return NULL;
  
  sidecar->extra_slots = obj->extra_slots;
  obj->extra_slots = (ant_extra_slots_t *)((uintptr_t)sidecar | ant_sidecar);
  
  return sidecar;
}

static inline ant_exotic_ops_t *ant_object_owned_exotic_ops(const ant_object_t *obj) {
  ant_object_sidecar_t *sidecar = ant_object_sidecar(obj);
  return sidecar && sidecar->owned_exotic_ops == ant_object_exotic_ops(obj) ? sidecar->owned_exotic_ops : NULL;
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

static inline uint32_t ant_object_overflow_cap_for(uint32_t used) {
  if (!used) return 0;
  return used <= 4 ? 4u : 1u << (32 - __builtin_clz(used - 1));
}

static inline uint32_t ant_object_overflow_cap(const ant_object_t *obj) {
  uint32_t limit = ant_shape_get_inobj_limit(obj->shape);
  return obj->overflow_prop && obj->prop_count > limit ? ant_object_overflow_cap_for(obj->prop_count - limit) : 0;
}

static inline void ant_object_overflow_free(ant_object_t *obj) {
  free(obj->overflow_prop);
  obj->overflow_prop = NULL;
}

static inline size_t ant_object_cell_size(uint32_t inobj_limit) {
  if (inobj_limit <= ANT_INOBJ_BASE_SLOTS) return ANT_OBJECT_SIZE;
  return inobj_limit <= ANT_INOBJ_WIDE_SLOTS ? ANT_OBJECT_WIDE_SIZE : ANT_OBJECT_LARGE_SIZE;
}

static inline uint32_t ant_object_inobj_limit(const ant_object_t *obj) {
  return obj ? ant_shape_get_inobj_limit(obj->shape) : ANT_INOBJ_BASE_SLOTS;
}

static inline ant_value_t ant_object_prop_get_unchecked(const ant_object_t *obj, uint32_t slot) {
  if (slot < ANT_INOBJ_BASE_SLOTS) return obj->inobj[slot];
  uint32_t inobj_limit = ant_object_inobj_limit(obj);
  
  return (slot < inobj_limit)
    ? obj->inobj[slot]
    : obj->overflow_prop[slot - inobj_limit];
}

static inline uint8_t ant_object_protector_holder(const ant_object_t *obj) {
  const ant_object_sidecar_t *sidecar = ant_object_sidecar(obj);
  return sidecar ? sidecar->protector_holder : 0;
}

static inline void ant_object_protector_holder_add(ant_object_t *obj, uint8_t bits) {
  ant_object_sidecar_t *sidecar = ant_object_ensure_sidecar(obj);
  if (sidecar) sidecar->protector_holder |= bits;
}

static inline void ant_proto_watch_note_change(ant_object_t *obj) {
  if (__builtin_expect(ant_object_protector_holder(obj) & ANT_PROTECTOR_HOLDER_WATCHED, 0)) ant_proto_watch_break(obj);
}

static inline void ant_object_prop_set_unchecked(ant_object_t *obj, uint32_t slot, ant_value_t value) {
  ant_value_t *dst = obj->inobj + slot;
  if (slot >= ANT_INOBJ_BASE_SLOTS) {
    uint32_t inobj_limit = ant_object_inobj_limit(obj);
    if (slot >= inobj_limit) dst = obj->overflow_prop + (slot - inobj_limit);
  }
  
  ant_proto_watch_note_change(obj);
  *dst = value;
}

ant_value_t get_proto(ant_t *js, ant_value_t obj);
ant_value_t obj_extra_get(ant_object_t *obj, internal_slot_t slot);
ant_object_t *obj_alloc_with_proto(ant_t *js, uint8_t type_tag, ant_value_t proto);

bool obj_extra_reserve(ant_object_t *obj, uint8_t capacity);
void obj_delete_prop_slot(ant_object_t *obj, uint32_t slot);
void set_proto(ant_t *js, ant_value_t obj, ant_value_t proto);
