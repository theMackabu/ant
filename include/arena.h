#ifndef ARENA_H
#define ARENA_H

#include <gc.h>

static inline void ant_gc_init(void) {
  GC_INIT();
  GC_set_all_interior_pointers(0);
  GC_disable();
}

#define ANT_GC_INIT() ant_gc_init()
#define ANT_GC_MALLOC(size) GC_MALLOC_IGNORE_OFF_PAGE(size)
#define ANT_GC_MALLOC_ATOMIC(size) GC_MALLOC_ATOMIC_IGNORE_OFF_PAGE(size)
#define ANT_GC_REALLOC(ptr, size) GC_REALLOC(ptr, size)
#define ANT_GC_FREE(ptr) GC_FREE(ptr)
#define ANT_GC_COLLECT() do { GC_enable(); GC_gcollect(); GC_disable(); } while(0)

#define ANT_GC_REGISTER_ROOT(ptr)
#define ANT_GC_UNREGISTER_ROOT(ptr)

#endif
