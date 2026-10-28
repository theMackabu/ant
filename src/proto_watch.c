#include "proto_watch.h"
#include "internal.h"

#include <stddef.h>
#include <stdlib.h>

typedef struct {
  void *owner;
  ant_proto_watch_invalidate_fn invalidate;
} ant_proto_watch_dependent_t;

struct ant_proto_watch {
  ant_object_t *object;
  ant_proto_watch_dependent_t *dependents;
  uint32_t count;
  uint32_t cap;
  uint32_t refs;
  bool pinned;
};

static_assert(
  offsetof(struct ant_proto_watch, object) == ANT_PROTO_WATCH_OBJECT_OFFSET,
  "compiled code reads the watched object at this offset"
);

static void proto_watch_release(ant_proto_watch_t *watch) {
  if (--watch->refs) return;
  free(watch->dependents);
  free(watch);
}

ant_proto_watch_t *ant_proto_watch(ant_object_t *obj) {
  if (!obj) return NULL;
  if (obj->protector_holder & ANT_PROTECTOR_HOLDER_WATCHED) return ant_object_sidecar(obj)->proto_watch;
  
  if (
    (obj->protector_holder & ANT_PROTECTOR_HOLDER_UNWATCHABLE) ||
    obj->type_tag != kTypeObject || obj->flags.is_exotic || !obj->shape
  ) return NULL;

  ant_proto_watch_t *watch = calloc(1, sizeof(*watch));
  ant_object_sidecar_t *sidecar = watch ? ant_object_ensure_sidecar(obj) : NULL;
  
  ant_shape_t *shape = sidecar ? ant_shape_clone(obj->shape) : NULL;
  if (!shape) {
    free(watch);
    return NULL;
  }

  ant_shape_release(obj->shape);
  
  obj->shape = shape;
  watch->object = obj;
  watch->refs = 1;
  sidecar->proto_watch = watch;
  obj->protector_holder |= ANT_PROTECTOR_HOLDER_WATCHED;
  
  return watch;
}

bool ant_proto_watch_depend(ant_proto_watch_t *watch, void *owner, ant_proto_watch_invalidate_fn invalidate) {
  if (!watch->object) return false;
  
  if (watch->count == watch->cap) {
    uint32_t cap = watch->cap ? watch->cap * 2 : 4;
    ant_proto_watch_dependent_t *grown = realloc(watch->dependents, cap * sizeof(*grown));
    if (!grown) return false;
    
    watch->dependents = grown;
    watch->cap = cap;
  }
  
  watch->dependents[watch->count++] = (ant_proto_watch_dependent_t){.owner = owner, .invalidate = invalidate};
  watch->refs++;
  
  return true;
}

bool ant_proto_watch_pin(ant_proto_watch_t *watch) {
  if (watch->pinned) return false;
  watch->pinned = true;
  watch->refs++;
  return true;
}

void ant_proto_watch_unpin(ant_proto_watch_t *watch) {
  watch->pinned = false;
  proto_watch_release(watch);
}

void ant_proto_watch_forget(ant_proto_watch_t *watch, void *owner) {
  for (uint32_t i = 0; i < watch->count; i++) {
    if (watch->dependents[i].owner != owner) continue;
    watch->dependents[i] = watch->dependents[--watch->count];
    break;
  }
  
  proto_watch_release(watch);
}

static ant_proto_watch_t *proto_watch_unlink(ant_object_t *obj) {
  ant_object_sidecar_t *sidecar = ant_object_sidecar(obj);
  ant_proto_watch_t *watch = sidecar->proto_watch;
  
  obj->protector_holder = (uint8_t)((obj->protector_holder & ~ANT_PROTECTOR_HOLDER_WATCHED) | ANT_PROTECTOR_HOLDER_UNWATCHABLE);
  sidecar->proto_watch = NULL;
  watch->object = NULL;
  
  return watch;
}

void ant_proto_watch_detach(ant_object_t *obj) {
  ant_proto_watch_t *watch = proto_watch_unlink(obj);
  watch->count = 0;
  proto_watch_release(watch);
}

void ant_proto_watch_break(ant_object_t *obj) {
  ant_proto_watch_t *watch = proto_watch_unlink(obj);
  uint32_t count = watch->count;
  watch->count = 0;
  for (uint32_t i = 0; i < count; i++) watch->dependents[i].invalidate(watch->dependents[i].owner);
  proto_watch_release(watch);
}
