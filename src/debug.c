#include "debug.h"

static unsigned sv_debug_flags = 0;

bool sv_debug_enabled(sv_debug_flag_t flag) {
  return (sv_debug_flags & (unsigned)flag) != 0;
}

void sv_debug_enable(sv_debug_flag_t flag) {
  sv_debug_flags |= (unsigned)flag;
}

void sv_debug_disable(sv_debug_flag_t flag) {
  sv_debug_flags &= ~(unsigned)flag;
}
