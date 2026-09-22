// meson test -C build gc-jit-entry-saves
//
// ant_jit_enter_clean saves its caller's callee-saved registers before
// clearing them. When the interpreter is the caller those registers hold only
// stale values, so the collector must skip the stub's save area; when the
// caller is C they may be live, so it must keep scanning it. Here the
// callee-saved registers hold the only reference to an object when the stub
// is entered, and a full collection inside the target must free the object
// from an interpreter caller and keep it from a C caller.
#include "internal.h"
#include "gc.h"
#include "gc/objects.h"
#include "jit/entry_stub.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#if ANT_JIT_ENTER_STUB
static constexpr uint64_t MASK = 0x5a5a5a5a5a5a5a5aull;

static ant_t *g_js;
static uint64_t g_masked; // the object's address, masked so no scan finds it here
static bool g_survived;

static ant_value_t target(
  sv_vm_t *vm, ant_value_t this_val, ant_value_t new_target, ant_value_t super_val,
  ant_value_t *args, int argc, sv_closure_t *closure
) {
  (void)vm; (void)this_val; (void)new_target; (void)super_val;
  (void)args; (void)argc; (void)closure;
  gc_run(g_js);
  ant_object_t *obj = (ant_object_t *)(uintptr_t)(g_masked ^ MASK);
  g_survived = obj->mark_epoch != ANT_GC_DEAD;
  return 0;
}

// keeps only the masked address, so the object's pointer stays off the stack
__attribute__((noinline)) static void make_object(ant_t *js) {
  ant_value_t obj = js_mkobj(js);
  g_masked = (uint64_t)(uintptr_t)js_obj_ptr(obj) ^ MASK;
}

// clears the stack below the caller, where make_object's frames were
__attribute__((noinline)) static void scrub_below(void) {
  volatile char region[64 * 1024];
  memset((char *)region, 0, sizeof(region));
}

// Enters compiled code the way the interpreter does (recording its segment)
// or the way C does (no segment), with the object's address in every
// callee-saved general register.
__attribute__((noinline)) static bool enter_with_object_in_registers(ant_t *js, bool from_interp) {
  __builtin_unwind_init();
  gc_vm_seg_t seg;
  if (from_interp) {
    uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
    uintptr_t hi = fp - GC_VM_SEG_SAVED_REGS_BYTES;
    seg.prev = js->vm_segs;
    seg.lo = gc_native_sp();
    seg.hi = hi > seg.lo ? hi : seg.lo;
    seg.fp = fp;
    seg.jit_depth = js->jit_active_depth;
    js->vm_segs = &seg;
  }

  make_object(js);
  scrub_below();

#if defined(__aarch64__)
  register uint64_t r19 asm("x19"), r20 asm("x20"), r21 asm("x21"), r22 asm("x22"), r23 asm("x23");
  register uint64_t r24 asm("x24"), r25 asm("x25"), r26 asm("x26"), r27 asm("x27"), r28 asm("x28");
  __asm__ volatile(
    "eor x19, %10, %11\n mov x20, x19\n mov x21, x19\n mov x22, x19\n mov x23, x19\n"
    "mov x24, x19\n mov x25, x19\n mov x26, x19\n mov x27, x19\n mov x28, x19\n"
    : "=r"(r19), "=r"(r20), "=r"(r21), "=r"(r22), "=r"(r23),
      "=r"(r24), "=r"(r25), "=r"(r26), "=r"(r27), "=r"(r28)
    : "r"(g_masked), "r"(MASK)
  );
  ant_jit_enter_clean(target, NULL, 0, 0, 0, NULL, 0, NULL);
  __asm__ volatile("" : "+r"(r19), "+r"(r20), "+r"(r21), "+r"(r22), "+r"(r23),
                        "+r"(r24), "+r"(r25), "+r"(r26), "+r"(r27), "+r"(r28));
#else
  register uint64_t rbx asm("rbx"), r12 asm("r12"), r13 asm("r13"), r14 asm("r14"), r15 asm("r15");
  __asm__ volatile(
    "mov %5, %%rbx\n xor %6, %%rbx\n mov %%rbx, %%r12\n mov %%rbx, %%r13\n"
    "mov %%rbx, %%r14\n mov %%rbx, %%r15\n"
    : "=r"(rbx), "=r"(r12), "=r"(r13), "=r"(r14), "=r"(r15)
    : "r"(g_masked), "r"(MASK)
  );
  ant_jit_enter_clean(target, NULL, 0, 0, 0, NULL, 0, NULL);
  __asm__ volatile("" : "+r"(rbx), "+r"(r12), "+r"(r13), "+r"(r14), "+r"(r15));
#endif

  if (from_interp) js->vm_segs = seg.prev;
  return g_survived;
}

int main(void) {
  ant_t *js = ant_create();
  assert(js);
  js_setstackbase(js, __builtin_frame_address(0));
  g_js = js;

  bool survived_from_c = enter_with_object_in_registers(js, false);
  bool survived_from_interp = enter_with_object_in_registers(js, true);

  // a C caller's saved registers may be live: the object must stay
  assert(survived_from_c);
  // an interpreter caller's saved registers are stale: the object must go
  assert(!survived_from_interp);

  puts("PASS entry-stub save area is skipped for interpreter callers and scanned for C callers");
  return 0;
}
#else
int main(void) { puts("SKIP entry stub not built on this target"); return 0; }
#endif
