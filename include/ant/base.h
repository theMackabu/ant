#pragma once

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>

#include "types.h"
#include "common.h"
#include "value.h"

static constexpr uint8_t CFUNC_HAS_PROTOTYPE  = 0x01u;
static constexpr uint8_t CFUNC_IS_CONSTRUCTOR = 0x02u;
static constexpr uint8_t CFUNC_NATIVE_GETTER  = 0x04u;
static constexpr uint8_t CFUNC_REALM_FREE     = 0x08u;

static constexpr double ANT_NAN = (double)NAN;
static constexpr double ANT_INFINITY = (double)INFINITY;
static constexpr double ANT_NEG_INFINITY = -(double)INFINITY;

#define Ant_Assert(cond, msg) ({                                    \
if (!(cond)) {                                                      \
  fprintf(stderr, "FATAL: %s\n  %s:%d\n", msg, __FILE__, __LINE__); \
  abort();                                                          \
}})
