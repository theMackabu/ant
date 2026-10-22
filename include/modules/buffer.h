#ifndef BUFFER_H
#define BUFFER_H

#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include "types.h"
#include "native_tag.h"

typedef enum {
  ARRAY_BUFFER_STORAGE_EXTERNAL,
  ARRAY_BUFFER_STORAGE_INLINE,
  ARRAY_BUFFER_STORAGE_BORROWED,
} array_buffer_storage_t;

typedef struct ArrayBufferData {
  uint8_t *data;
  size_t length;
  size_t capacity;
  struct ArrayBufferData *owner;
  _Atomic int ref_count;
  uint32_t registry_slot;
  uint8_t is_shared;
  uint8_t is_detached;
  uint8_t storage;
  uint8_t untransferable;
  uint8_t resizable;
  uint8_t views_slow;
} ArrayBufferData;

#if UINTPTR_MAX == UINT64_MAX
static_assert(sizeof(ArrayBufferData) == 48, "ArrayBufferData layout changed");
#elif UINTPTR_MAX == UINT32_MAX
static_assert(sizeof(ArrayBufferData) == 28, "ArrayBufferData layout changed");
#endif

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
  TYPED_ARRAY_BIGUINT64,
  TYPED_ARRAY_COUNT
} TypedArrayType;

typedef struct {
  ArrayBufferData *buffer;
  TypedArrayType type;
  bool length_tracking;
  size_t byte_offset;
  size_t byte_length;
  size_t length;
  ArrayBufferData *backing;
  size_t base_length;
} TypedArrayData;

typedef struct {
  ArrayBufferData *buffer;
  size_t byte_offset;
  size_t byte_length;
  ArrayBufferData *backing;
  size_t base_length;
  bool length_tracking;
} DataViewData;

typedef enum {
  BUFFER_TA_ACCESS_NONE,
  BUFFER_TA_ACCESS_SLOW,
  BUFFER_TA_ACCESS_INLINE,
} buffer_ta_access_t;

static constexpr size_t BUFFER_INSPECT_MAX_BYTES = 50;

static constexpr uint32_t BUFFER_ARRAYBUFFER_NATIVE_TAG = Ant_NativeTag("ABUF");
static constexpr uint32_t BUFFER_TYPEDARRAY_NATIVE_TAG  = Ant_NativeTag("TYAR");
static constexpr uint32_t BUFFER_DATAVIEW_NATIVE_TAG    = Ant_NativeTag("DVIW");

ant_value_t buffer_library(ant_t *js);
size_t buffer_get_external_memory(ant_t *js);

buffer_ta_access_t buffer_typedarray_inline_get(ant_value_t obj, ant_value_t key, TypedArrayType *type);
buffer_ta_access_t buffer_typedarray_inline_put(ant_value_t obj, ant_value_t key, ant_value_t value, TypedArrayType *type);

void init_buffer_module(ant_t *js);
void cleanup_buffer_module(ant_t *js);
void free_array_buffer_data(ant_t *js, ArrayBufferData *data);
void buffer_transfer_storage(ant_t *js, ArrayBufferData *from, ArrayBufferData *to);
void buffer_move_storage(ant_t *js, ArrayBufferData *from, ArrayBufferData *to);
void buffer_detach(ant_t *js, ArrayBufferData *data);

ant_value_t create_arraybuffer_obj(ant_t *js, ArrayBufferData *buffer);
ant_value_t create_arraybuffer_copy(ant_t *js, const void *bytes, size_t length);
ant_value_t create_resizable_arraybuffer_copy(ant_t *js, const void *bytes, size_t length, size_t max_length);
ant_value_t buffer_clone_arraybuffer(ant_t *js, const ArrayBufferData *data);
ant_value_t create_shared_arraybuffer_obj(ant_t *js, ArrayBufferData *buffer);

const void *buffer_typedarray_exotic_ops(void);
const char *buffer_typedarray_type_name(TypedArrayType type);

ArrayBufferData *create_array_buffer_data(ant_t *js, size_t length);
ArrayBufferData *buffer_get_arraybuffer_data(ant_value_t value);
TypedArrayData *buffer_get_typedarray_data(ant_value_t value);
DataViewData *buffer_get_dataview_data(ant_value_t value);
ArrayBufferData *create_external_array_buffer_data(uint8_t *data, size_t length);
ArrayBufferData *buffer_storage_holder_new(void);

bool buffer_is_dataview(ant_value_t obj);
bool buffer_is_binary_source(ant_value_t value);
bool buffer_can_transfer(const ArrayBufferData *data);

bool buffer_source_get_bytes(ant_t *js, ant_value_t value, const uint8_t **out, size_t *len);
bool buffer_typedarray_data_read_index(ant_t *js, const TypedArrayData *ta_data, size_t index, ant_value_t *out);
bool buffer_typedarray_read_index(ant_t *js, ant_value_t value, size_t index, ant_value_t *out);
bool buffer_typedarray_get_index(ant_t *js, ant_value_t obj, uint32_t index, ant_value_t *out);
bool buffer_typedarray_set_index(ant_t *js, ant_value_t obj, uint32_t index, ant_value_t value, ant_value_t *result);

bool buffer_bytes_to_string(
  ant_t *js, const uint8_t *data, size_t len,
  const char *encoding, size_t encoding_len, ant_value_t *out
);

ant_value_t create_typed_array(
  ant_t *js, TypedArrayType type, ArrayBufferData *buffer,
  size_t byte_offset, size_t length, ant_value_t proto
);

ant_value_t create_typed_array_with_buffer(
  ant_t *js, TypedArrayType type, ArrayBufferData *buffer,
  size_t byte_offset, size_t length, 
  ant_value_t proto, ant_value_t arraybuffer_obj, bool tracking
);

ant_value_t create_dataview_with_buffer(
  ant_t *js, ArrayBufferData *buffer,
  size_t byte_offset, size_t byte_length,
  ant_value_t arraybuffer_obj, bool tracking
);

static inline bool Ant_Buffer_IsLive(const ArrayBufferData *buffer) {
  return buffer && !buffer->is_detached;
}

#define Ant_Buffer_ViewIsLive(view) ({               \
  typeof(view) _ant_view = (view);                   \
  _ant_view && Ant_Buffer_IsLive(_ant_view->buffer); \
})

#define Ant_Buffer_GetViewBytes(view, ptr, len) ({                 \
  typeof(view) _ant_bytes_view = (view);                           \
  bool _ant_bytes_live = Ant_Buffer_ViewIsLive(_ant_bytes_view);   \
  *(ptr) = _ant_bytes_live                                         \
    ? _ant_bytes_view->buffer->data + _ant_bytes_view->byte_offset \
    : NULL;                                                        \
  *(len) = _ant_bytes_live ? _ant_bytes_view->byte_length : 0;     \
  _ant_bytes_live;                                                 \
})

#endif
