#ifndef DEBUG_H
#define DEBUG_H

#include <stdbool.h>

// TODO: make into bitflag
typedef enum {
  SV_DEBUG_DUMP_BC        = 1u << 0,
  SV_DEBUG_DUMP_JIT       = 1u << 1,
  SV_DEBUG_JIT_WARN       = 1u << 2,
  SV_DEBUG_PARSE          = 1u << 3,
  SV_DEBUG_COMPILE        = 1u << 4,
  SV_DEBUG_DUMP_SHELL     = 1u << 5,
  SV_DEBUG_JITLESS        = 1u << 6,
  SV_DEBUG_OPT_OFF        = 1u << 7,
  SV_DEBUG_OPT_FORCE      = 1u << 8,
  SV_DEBUG_OPT_VERIFY     = 1u << 9,
  SV_DEBUG_DUMP_OPT_GRAPH = 1u << 10,
  SV_DEBUG_DUMP_OPT_STATS = 1u << 11,
  SV_DEBUG_PERF_MAP       = 1u << 12,
  SV_DEBUG_SHAPE_VERIFY   = 1u << 13,
} sv_debug_flag_t;

bool sv_debug_enabled(sv_debug_flag_t flag);
void sv_debug_enable(sv_debug_flag_t flag);
void sv_debug_disable(sv_debug_flag_t flag);

#define sv_debug_unlikely(flag)    __builtin_expect(sv_debug_enabled((flag)), 0)
#define sv_dump_bytecode_unlikely  sv_debug_unlikely(SV_DEBUG_DUMP_BC)
#define sv_dump_jit_unlikely       sv_debug_unlikely(SV_DEBUG_DUMP_JIT)
#define sv_jit_warn_unlikely       sv_debug_unlikely(SV_DEBUG_JIT_WARN)
#define sv_parse_trace_unlikely    sv_debug_unlikely(SV_DEBUG_PARSE)
#define sv_compile_trace_unlikely  sv_debug_unlikely(SV_DEBUG_COMPILE)
#define sv_dump_shell_unlikely     sv_debug_unlikely(SV_DEBUG_DUMP_SHELL)
#define sv_jitless_unlikely        sv_debug_unlikely(SV_DEBUG_JITLESS)

#endif
