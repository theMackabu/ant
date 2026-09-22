#ifndef ANT_GC_OBJECTS_H
#define ANT_GC_OBJECTS_H

#include "types.h"
#include "object.h"

static constexpr uint8_t ANT_GC_DEAD = 0xFF;
typedef void (*gc_extra_roots_fn)(ant_t *js);

typedef struct gc_vm_seg {
  struct gc_vm_seg *prev;
  uintptr_t lo, hi;
  uint32_t jit_depth;
} gc_vm_seg_t;

#if defined(__aarch64__)
#define GC_VM_SEG_SAVED_REGS_BYTES 144u
#elif defined(__x86_64__)
#define GC_VM_SEG_SAVED_REGS_BYTES 40u
#else
#define GC_VM_SEG_SAVED_REGS_BYTES 0u
#endif

static inline __attribute__((always_inline)) uintptr_t gc_native_sp(void) {
  uintptr_t sp;
#if defined(__aarch64__)
  __asm__ volatile("mov %0, sp" : "=r"(sp));
#elif defined(__x86_64__)
  __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
#else
  volatile char marker; sp = (uintptr_t)&marker;
#endif
  return sp;
}

bool gc_obj_is_marked(const ant_object_t *obj);

uint64_t gc_get_epoch(void);
uint64_t gc_objects_run(ant_t *js, gc_extra_roots_fn extra_roots);

void gc_mark_str(ant_t *js, ant_value_t v);
void gc_mark_value(ant_t *js, ant_value_t v);
void gc_mark_closure(ant_t *js, sv_closure_t *c);
void gc_mark_coroutine(ant_t *js, coroutine_t *coro);
void gc_mark_upvalue_cells(ant_t *js, sv_upvalue_t *const *cells, uint32_t count);
void gc_mark_conservative_range(ant_t *js, const void *ptr, size_t size);

void gc_objects_run_minor(ant_t *js);
void gc_object_free(ant_t *js, ant_object_t *obj);
void gc_pin_existing_objects(ant_t *js);

void gc_root_pending_promise(ant_t *js, ant_object_t *obj);
void gc_unroot_pending_promise(ant_t *js, ant_object_t *obj);

#endif
