#ifndef ARENA_H
#define ARENA_H

#define NO_EXECUTE_PERMISSION 1
#define GC_NO_THREAD_REDIRECTS 1
#define GC_THREADS 1
#include <gc.h>

#define ANT_GC_INIT() GC_INIT()
#define ANT_GC_MALLOC(size) GC_MALLOC(size)
#define ANT_GC_MALLOC_ATOMIC(size) GC_MALLOC_ATOMIC(size)
#define ANT_GC_REALLOC(ptr, size) GC_REALLOC(ptr, size)
#define ANT_GC_FREE(ptr) GC_FREE(ptr)
#define ANT_GC_COLLECT() GC_gcollect()

#define ANT_GC_REGISTER_ROOT(ptr)
#define ANT_GC_UNREGISTER_ROOT(ptr)

#endif
