// meson test -C build gc-stack-residue
#include "internal.h"
#include "gc.h"
#include "gc/objects.h"
#include "gc/roots.h"
#include <assert.h>
#include <stdio.h>

#if defined(__aarch64__) || defined(__x86_64__)
static ant_value_t target;
static bool writer_ran, target_finalized;

static void finalize_target(ant_t *js, ant_object_t *obj) {
  target_finalized = true;
}

// This finalizer runs inside the first sweep. Force a raw pointer onto the
// stack even in builds where the collector would keep it in a register.
__attribute__((noinline)) static void leave_pointers(ant_t *js, ant_object_t *obj) {
  ant_object_t *volatile words[256];
  for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) words[i] = js_obj_ptr(target);
  writer_ran = true;
}

__attribute__((noinline)) static void prepare(ant_t *js) {
  target = Ant_Object_CreateBare(js);
  js_set_finalizer(target, finalize_target);
  js_set_finalizer(Ant_Object_CreateBare(js), leave_pointers);
}

__attribute__((noinline)) static void collect_next_turn(ant_t *js) {
  // Reserve an unwritten frame over the previous sweep's locals and keep it
  // on the stack while the collector scans this turn.
  uintptr_t frame[1024];
  gc_run(js);
  __asm__ volatile("" :: "r"(frame) : "memory");
}

static bool reclaimed_after(ant_t *js, void (*first_collection)(ant_t *)) {
  writer_ran = target_finalized = false;
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, target);
  prepare(js);
  first_collection(js);
  assert(writer_ran && !target_finalized);
  target = Ant_Value_Undefined;
  GC_ROOT_RESTORE(js, mark);
  // Exclude the test driver's own locals from this turn's native roots.
  js_setstackbase(js, (void *)gc_native_sp());
  collect_next_turn(js);
  js_setstackbase(js, NULL);
  return target_finalized;
}

int main(void) {
  ant_t *js = Ant_Create(NULL);
  assert(js);
  // During setup only the explicit root keeps the target alive. The writer
  // must be unreachable even if the allocator left its address on the stack.
  js_setstackbase(js, NULL);
  assert(reclaimed_after(js, gc_run_minor) && "minor GC left a stale root");
  assert(reclaimed_after(js, gc_run) && "major GC left a stale root");
  Ant_Destroy(js);
  puts("PASS stack residue from a collection does not retain an unreachable object");
}
#else
int main(void) { puts("SKIP native stack residue test requires arm64 or x86-64"); }
#endif
