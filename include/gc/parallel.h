#pragma once

#include "types.h"
#include "object.h"

typedef void (*gc_parallel_scan_fn)(ant_t *js, ant_object_t *obj);

typedef struct {
  ant_object_t **items;
  size_t base, len, cap;
} gc_par_stack_t;

void gc_par_stack_room(gc_par_stack_t *stack);
size_t gc_par_stack_give(gc_par_stack_t *stack, ant_object_t **out, size_t limit);

void gc_parallel_set_helpers(int count);
bool gc_parallel_available(void);
bool gc_parallel_drain(ant_t *js, uint32_t other_refs, gc_parallel_scan_fn scan);
