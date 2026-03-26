#ifndef BUFFER_H
#define BUFFER_H

#include <stdint.h>
#include <stddef.h>
#include "types.h"

typedef struct {
  uint8_t *data;
  size_t length;
  size_t capacity;
  int ref_count;
  int is_shared;
  int is_detached;
} ArrayBufferData;

typedef enum {
  TYPED_ARRAY_INT8,
  TYPED_ARRAY_UINT8,
  TYPED_ARRAY_UINT8_CLAMPED,
  TYPED_ARRAY_INT16,
  TYPED_ARRAY_UINT16,
  TYPED_ARRAY_INT32,
  TYPED_ARRAY_UINT32,
  TYPED_ARRAY_FLOAT16,
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

ant_value_t buffer_library(ant_t *js);

void init_buffer_module(void);
void cleanup_buffer_module(void);
void free_array_buffer_data(ArrayBufferData *data);

ArrayBufferData *create_array_buffer_data(size_t length);
ant_value_t create_arraybuffer_obj(ant_t *js, ArrayBufferData *buffer);
const char *buffer_typedarray_type_name(TypedArrayType type);

ant_value_t create_typed_array(
  ant_t *js, TypedArrayType type, ArrayBufferData *buffer,
  size_t byte_offset, size_t length, const char *type_name
);

ant_value_t create_typed_array_with_buffer(
  ant_t *js, TypedArrayType type, ArrayBufferData *buffer,
  size_t byte_offset, size_t length, 
  const char *type_name, ant_value_t arraybuffer_obj
);

ant_value_t create_dataview_with_buffer(
  ant_t *js, ArrayBufferData *buffer,
  size_t byte_offset, size_t byte_length,
  ant_value_t arraybuffer_obj
);

size_t buffer_get_external_memory(void);
bool buffer_is_dataview(ant_value_t obj);
bool buffer_source_get_bytes(ant_t *js, ant_value_t value, const uint8_t **out, size_t *len);

#endif
