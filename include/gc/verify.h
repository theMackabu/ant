#ifndef ANT_GC_VERIFY_H
#define ANT_GC_VERIFY_H

#include <string.h>

#ifdef ANT_GC_VERIFY
void gc_verify_stress(ant_t *js);
void gc_verify_poison_object(ant_object_t *obj);
#define GC_VERIFY_STRESS(js)          gc_verify_stress(js)
#define GC_VERIFY_POISON_OBJECT(obj)  gc_verify_poison_object(obj)
#define GC_VERIFY_POISON_BYTES(p, n)  memset((void *)(p), 0xdb, (size_t)(n))
#else
#define GC_VERIFY_STRESS(js)          ((void)0)
#define GC_VERIFY_POISON_OBJECT(obj)  ((void)0)
#define GC_VERIFY_POISON_BYTES(p, n)  ((void)0)
#endif

#endif
