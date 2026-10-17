#ifndef ANT_NATIVE_TAG_H
#define ANT_NATIVE_TAG_H

#include <stdint.h>

#define Ant_NativeTag(s) ((uint32_t)(                                 \
  (uint32_t)(uint8_t)(s)[0] << 24 | (uint32_t)(uint8_t)(s)[1] << 16 | \
  (uint32_t)(uint8_t)(s)[2] << 8  | (uint32_t)(uint8_t)(s)[3]))

#endif
