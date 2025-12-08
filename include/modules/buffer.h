#ifndef BUFFER_H
#define BUFFER_H

#include <stdint.h>
#include <stddef.h>

void init_buffer_module(void);

typedef struct {
  uint8_t *data;
  size_t length;
  size_t capacity;
  int ref_count;
} ArrayBufferData;

typedef enum {
  TYPED_ARRAY_INT8,
  TYPED_ARRAY_UINT8,
  TYPED_ARRAY_UINT8_CLAMPED,
  TYPED_ARRAY_INT16,
  TYPED_ARRAY_UINT16,
  TYPED_ARRAY_INT32,
  TYPED_ARRAY_UINT32,
  TYPED_ARRAY_FLOAT32,
  TYPED_ARRAY_FLOAT64,
  TYPED_ARRAY_BIGINT64,
  TYPED_ARRAY_BIGUINT64
} TypedArrayType;

typedef struct {
  ArrayBufferData *buffer;
  TypedArrayType type;
  size_t byte_offset;
  size_t byte_length;
  size_t length;
} TypedArrayData;

typedef struct {
  ArrayBufferData *buffer;
  size_t byte_offset;
  size_t byte_length;
} DataViewData;

#endif
