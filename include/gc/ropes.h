#ifndef ANT_GC_ROPES_H
#define ANT_GC_ROPES_H

#include "types.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum {
  GC_ROPES_BEGIN_NORMAL,
  GC_ROPES_BEGIN_RETRY_MAJOR,
  GC_ROPES_BEGIN_CONSERVATIVE_MAJOR,
} gc_ropes_begin_result_t;

gc_ropes_begin_result_t gc_ropes_begin(ant_t *js, bool minor);

void gc_ropes_sweep(ant_t *js, bool minor);
void gc_ropes_mark_conservative_roots(ant_t *js);

typedef enum {
  GC_ROPE_MARK_INVALID,
  GC_ROPE_MARK_SKIP,
  GC_ROPE_MARK_TRACE,
} gc_rope_mark_result_t;

gc_rope_mark_result_t gc_ropes_mark(
  ant_t *js, const void *ptr, 
  size_t size, size_t align
);

#endif
