#ifndef ANT_GC_OBJECTS_H
#define ANT_GC_OBJECTS_H

#include "types.h"
#include "object.h"

#define ANT_GC_DEAD 0xFF

typedef void (*gc_str_mark_fn)(ant_t *js, ant_value_t v);
bool gc_obj_is_marked(const ant_object_t *obj);

void gc_mark_value(ant_t *js, ant_value_t v);
void gc_objects_run(ant_t *js, gc_str_mark_fn str_mark);
void gc_objects_run_minor(ant_t *js, gc_str_mark_fn str_mark);
void gc_object_free(ant_t *js, ant_object_t *obj);
void gc_pin_existing_objects(ant_t *js);

void gc_root_pending_promise(ant_object_t *obj);
void gc_unroot_pending_promise(ant_object_t *obj);

#endif
