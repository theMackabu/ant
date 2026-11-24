#pragma once

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>

#include "types.h"  // IWYU pragma: export
#include "common.h" // IWYU pragma: export
#include "value.h"  // IWYU pragma: export

typedef enum: uint8_t {
  CFUNC_HAS_PROTOTYPE  = 0x01u,
  CFUNC_IS_CONSTRUCTOR = 0x02u,
  CFUNC_NATIVE_GETTER  = 0x04u,
  CFUNC_REALM_FREE     = 0x08u,
} ant_cfunc_flags_t;

static constexpr double ANT_NAN = (double)NAN;
static constexpr double ANT_INFINITY = (double)INFINITY;
static constexpr double ANT_NEG_INFINITY = -(double)INFINITY;

[[noreturn]] static inline __attribute__((cold, noinline))
void Ant_AssertFailed(const char *msg, const char *file, int line) {
  fprintf(stderr, "FATAL: %s\n  %s:%d\n", msg, file, line);
  abort();
}

#define Ant_Assert(cond, msg) ((cond) ? (void)0 : Ant_AssertFailed(msg, __FILE__, __LINE__))
