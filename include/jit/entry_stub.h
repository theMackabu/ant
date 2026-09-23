#ifndef ANT_JIT_ENTRY_STUB_H
#define ANT_JIT_ENTRY_STUB_H

#include "silver/engine.h"

#if (defined(__aarch64__) || defined(__x86_64__)) && !defined(_WIN32) && !defined(ANT_WASM_EMBED)
#define ANT_JIT_ENTER_STUB 1

ant_value_t ant_jit_enter_clean(
  sv_jit_func_t fn, sv_vm_t *vm, ant_value_t this_val, ant_value_t new_target,
  ant_value_t super_val, ant_value_t *args, int argc, sv_closure_t *closure
);

extern const char ant_jit_enter_clean_ret[];

#if defined(__aarch64__)
static constexpr intptr_t ANT_JIT_ENTER_SAVED_LO = 16;  // x19-x28 at [fp+16, fp+96)
static constexpr intptr_t ANT_JIT_ENTER_SAVED_HI = 96;
#else
static constexpr intptr_t ANT_JIT_ENTER_SAVED_LO = -40; // rbx, r12-r15 at [fp-40, fp)
static constexpr intptr_t ANT_JIT_ENTER_SAVED_HI = 0;
#endif

#else
#define ANT_JIT_ENTER_STUB 0
#endif

#endif
