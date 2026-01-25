#ifndef ARENA_H
#define ARENA_H

#include <gc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <minicoro.h>

static inline void ANT_GC_INIT(void) {
  GC_INIT();
  GC_enable();
}

static inline void ANT_GC_COLLECT(void) {
  mco_coro* running = mco_running();
  struct GC_stack_base sb;
  int in_coroutine = (running != NULL && running->stack_base != NULL);
  
  if (in_coroutine) {
    memset(&sb, 0, sizeof(sb));
    sb.mem_base = running->stack_base;
    GC_set_stackbottom(NULL, &sb);
  }
  
  GC_gcollect();
  
  if (in_coroutine) {
    memset(&sb, 0, sizeof(sb));
    GC_get_stack_base(&sb);
    GC_set_stackbottom(NULL, &sb);
  }
}

static inline void *ant_gc_check_ptr(void *p, const char *func) {
  if (p && ((uintptr_t)p >> 48) != 0) {
    fprintf(stderr, 
      "FATAL: %s returned pointer %p outside 48-bit NaN-boxing range\n"
      "Please report this issue with your OS/architecture details.\n", func, p
    ); abort();
  }
  
  return p;
}

#define ANT_GC_MALLOC(size) \
  ant_gc_check_ptr(GC_MALLOC_IGNORE_OFF_PAGE(size), "GC_MALLOC")
#define ANT_GC_MALLOC_ATOMIC(size) \
  ant_gc_check_ptr(GC_MALLOC_ATOMIC_IGNORE_OFF_PAGE(size), "GC_MALLOC_ATOMIC")
#define ANT_GC_REALLOC(ptr, size) \
  ant_gc_check_ptr(GC_REALLOC(ptr, size), "GC_REALLOC")
  
#define ANT_GC_FREE(ptr) GC_FREE(ptr)

#define ANT_GC_REGISTER_ROOT(ptr)
#define ANT_GC_UNREGISTER_ROOT(ptr)

#endif
