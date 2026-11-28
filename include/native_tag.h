#pragma once

#include <stdint.h>

#define Ant_NativeTag(s) ((uint32_t)(                                 \
  (uint32_t)(uint8_t)(s)[0] << 24 | (uint32_t)(uint8_t)(s)[1] << 16 | \
  (uint32_t)(uint8_t)(s)[2] << 8  | (uint32_t)(uint8_t)(s)[3]))

#define Ant_SentinelTag(s) (ANT_SENTINEL_TAG | (uint64_t)Ant_NativeTag(s))
