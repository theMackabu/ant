// meson test -C build jit-entry-stub
//
// ant_jit_enter_clean must pass all seven arguments through unchanged, call
// the target with every callee-saved general register zeroed, and restore the
// caller's callee-saved registers afterwards (see src/jit/entry_stub.c).
#include "jit/entry_stub.h"
#include <assert.h>
#include <stdio.h>

#if ANT_JIT_ENTER_STUB
static uint64_t seen_regs[10];

static ant_value_t target(
  sv_vm_t *vm, ant_value_t this_val, ant_value_t new_target, ant_value_t super_val,
  ant_value_t *args, int argc, sv_closure_t *closure
) {
#if defined(__aarch64__)
  __asm__ volatile(
    "stp x19, x20, [%0]\n stp x21, x22, [%0, #16]\n stp x23, x24, [%0, #32]\n"
    "stp x25, x26, [%0, #48]\n stp x27, x28, [%0, #64]\n"
    :: "r"(seen_regs) : "memory");
#elif defined(__x86_64__)
  __asm__ volatile(
    "mov %%rbx, (%0)\n mov %%r12, 8(%0)\n mov %%r13, 16(%0)\n mov %%r14, 24(%0)\n mov %%r15, 32(%0)\n"
    :: "r"(seen_regs) : "memory");
#endif
  assert(vm == (sv_vm_t *)0x1111);
  assert(this_val == 0x2222 && new_target == 0x3333 && super_val == 0x4444);
  assert(args == (ant_value_t *)0x5555 && argc == -7);
  assert(closure == (sv_closure_t *)0x6666);
  return 0x7777;
}

int main(void) {
  // keep values live in callee-saved registers across the call
#if defined(__aarch64__)
  register uint64_t keep asm("x19") = 0xfeedface;
#else
  register uint64_t keep asm("rbx") = 0xfeedface;
#endif
  __asm__ volatile("" : "+r"(keep));
  for (int i = 0; i < 3; i++) {
    ant_value_t r = ant_jit_enter_clean(
      target, (sv_vm_t *)0x1111, 0x2222, 0x3333, 0x4444, (ant_value_t *)0x5555, -7, (sv_closure_t *)0x6666
    );
    assert(r == 0x7777);
  }
  __asm__ volatile("" : "+r"(keep));
  assert(keep == 0xfeedface);
#if defined(__aarch64__)
  for (int i = 0; i < 10; i++) assert(seen_regs[i] == 0);
#else
  for (int i = 0; i < 5; i++) assert(seen_regs[i] == 0);
#endif
  puts("PASS entry stub passes arguments, zeroes callee-saved registers and restores them");
  return 0;
}
#else
int main(void) { puts("SKIP entry stub not built on this target"); return 0; }
#endif
