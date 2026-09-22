#ifndef ANT_JIT_ENTRY_STUB_H
#define ANT_JIT_ENTRY_STUB_H

#include "silver/engine.h"

#if (defined(__aarch64__) || defined(__x86_64__)) && !defined(_WIN32) && !defined(ANT_WASM_EMBED)
#define ANT_JIT_ENTER_STUB 1
ant_value_t ant_jit_enter_clean(
  sv_jit_func_t fn, sv_vm_t *vm, ant_value_t this_val, ant_value_t new_target,
  ant_value_t super_val, ant_value_t *args, int argc, sv_closure_t *closure
);
#else
#define ANT_JIT_ENTER_STUB 0
#endif

#endif
