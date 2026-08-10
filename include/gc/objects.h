#ifndef ANT_GC_OBJECTS_H
#define ANT_GC_OBJECTS_H

#include "types.h"
#include "object.h"

static constexpr uint8_t ANT_GC_DEAD = 0xFF;

typedef void (*gc_extra_roots_fn)(ant_t *js);
typedef void (*gc_str_mark_fn)(ant_t *js, ant_value_t v);

uint64_t gc_get_epoch(void);
bool gc_obj_is_marked(const ant_object_t *obj);

void gc_mark_value(ant_t *js, ant_value_t v);
void gc_mark_upvalue_cells(ant_t *js, sv_upvalue_t *const *cells, uint32_t count);
void gc_mark_conservative_range(ant_t *js, const void *ptr, size_t size);

void gc_objects_run_minor(ant_t *js, gc_str_mark_fn str_mark);
void gc_objects_run(ant_t *js, gc_str_mark_fn str_mark, gc_extra_roots_fn extra_roots);

void gc_object_free(ant_t *js, ant_object_t *obj);
void gc_pin_existing_objects(ant_t *js);

void gc_root_pending_promise(ant_t *js, ant_object_t *obj);
void gc_unroot_pending_promise(ant_t *js, ant_object_t *obj);

#endif
