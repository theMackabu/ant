#ifndef DEBUG_H
#define DEBUG_H

#include <stdbool.h>

typedef enum {
  SV_DEBUG_DUMP_BYTECODE = 1u << 0,
  SV_DEBUG_DUMP_JIT      = 1u << 1,
  SV_DEBUG_JIT_WARN      = 1u << 2,
} sv_debug_flag_t;

bool sv_debug_enabled(sv_debug_flag_t flag);
void sv_debug_enable(sv_debug_flag_t flag);
void sv_debug_disable(sv_debug_flag_t flag);
void sv_debug_set(sv_debug_flag_t flag, bool enabled);

#define sv_debug_unlikely(flag)    __builtin_expect(sv_debug_enabled((flag)), 0)
#define sv_dump_bytecode_unlikely  sv_debug_unlikely(SV_DEBUG_DUMP_BYTECODE)
#define sv_dump_jit_unlikely       sv_debug_unlikely(SV_DEBUG_DUMP_JIT)
#define sv_jit_warn_unlikely       sv_debug_unlikely(SV_DEBUG_JIT_WARN)

#endif
