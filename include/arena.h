#ifndef ARENA_H
#define ARENA_H

#include <gc.h>
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

#define ANT_GC_MALLOC(size) GC_MALLOC_IGNORE_OFF_PAGE(size)
#define ANT_GC_MALLOC_ATOMIC(size) GC_MALLOC_ATOMIC_IGNORE_OFF_PAGE(size)
#define ANT_GC_REALLOC(ptr, size) GC_REALLOC(ptr, size)
#define ANT_GC_FREE(ptr) GC_FREE(ptr)

#define ANT_GC_REGISTER_ROOT(ptr)
#define ANT_GC_UNREGISTER_ROOT(ptr)

#endif
