#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum: uint8_t {
#define SV_DEBUG(name, key, val, all) SV_DEBUG_BIT_##name,
#include "debug/list.h"
  SV_DEBUG_COUNT
} sv_debug_bit_t;

typedef enum: uint32_t {
#define SV_DEBUG(name, key, val, all) SV_DEBUG_##name = 1u << SV_DEBUG_BIT_##name,
#include "debug/list.h"
} sv_debug_flag_t;

static_assert(
  SV_DEBUG_COUNT <= 32,
  "sv_debug_flags holds at most 32 debug flags"
);

extern uint32_t sv_debug_flags;

static inline bool sv_debug_enabled(uint32_t flags) { return (sv_debug_flags & flags) != 0; }
static inline void sv_debug_enable(uint32_t flags) { sv_debug_flags |= flags; }
static inline void sv_debug_disable(uint32_t flags) { sv_debug_flags &= ~flags; }

#define sv_debug_unlikely(flag) __builtin_expect(sv_debug_enabled((flag)), 0)
