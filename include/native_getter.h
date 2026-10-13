#ifndef ANT_NATIVE_GETTER_H
#define ANT_NATIVE_GETTER_H

#include "ant.h"
#include "handles.h"

#include <stdint.h>

typedef enum : uint8_t {
  ANT_NATIVE_GETTER_SLOT_DATA,
  ANT_NATIVE_GETTER_SIZE,
} ant_native_getter_kind_t;

typedef enum : uint8_t {
  ANT_NATIVE_DETACH_NONE,
  ANT_NATIVE_DETACH_SELF,
  ANT_NATIVE_DETACH_REF,
} ant_native_detach_t;

typedef struct {
  ant_cfunc_meta_t meta;
  uint32_t native_tag;
  ant_native_getter_kind_t kind;
  ant_native_detach_t detach;
  uint16_t ref_offset;
  uint16_t flag_offset;
  uint16_t field_offset;
} ant_native_getter_t;

static inline const ant_native_getter_t *ant_native_getter_of(const ant_cfunc_meta_t *meta) {
  return meta && (meta->flags & CFUNC_NATIVE_GETTER) ? (const ant_native_getter_t *)ant_cfunc_meta_source(meta) : NULL;
}

#endif
