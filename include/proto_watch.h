#ifndef ANT_PROTO_WATCH_H
#define ANT_PROTO_WATCH_H

#include "object.h"

typedef void (*ant_proto_watch_invalidate_fn)(void *owner);
static constexpr size_t ANT_PROTO_WATCH_OBJECT_OFFSET = 0;

ant_proto_watch_t *ant_proto_watch(ant_object_t *obj);
bool ant_proto_watch_pin(ant_proto_watch_t *watch);
bool ant_proto_watch_depend(ant_proto_watch_t *watch, void *owner, ant_proto_watch_invalidate_fn invalidate);

void ant_proto_watch_unpin(ant_proto_watch_t *watch);
void ant_proto_watch_forget(ant_proto_watch_t *watch, void *owner);
void ant_proto_watch_detach(ant_object_t *obj);
#endif
