#pragma once

#include <string.h>

#include "ant.h"
#include "utils.h"
#include "internal.h"

#include "modules/bigint.h"
#include "modules/buffer.h"
#include "native_getter.h" // IWYU pragma: keep

typedef enum {
  ENC_UTF8,
  ENC_HEX,
  ENC_BASE64,
  ENC_BASE64URL,
  ENC_ASCII,
  ENC_LATIN1,
  ENC_UCS2,
  ENC_UNKNOWN
} BufferEncoding;

typedef enum {
  BUFFER_NUM_UINT,
  BUFFER_NUM_INT,
  BUFFER_NUM_FLOAT,
  BUFFER_NUM_BIGINT,
  BUFFER_NUM_BIGUINT,
} buffer_num_kind_t;

typedef enum {
  BUFFER_NUM_BE,
  BUFFER_NUM_LE,
} buffer_num_endian_t;

extern ArrayBufferData buffer_out_of_bounds;

#define BUFFER_SIZE_GETTER(name, ...) extern const ant_native_getter_t name;
#define BUFFER_SLOT_GETTER(name, ...) extern const ant_native_getter_t name;
#include "buffer/native_getters.h"

TypedArrayData *typedarray_live_slow(TypedArrayData *ta);
BufferEncoding parse_encoding(const char *enc, size_t len);

void *ta_meta_alloc(ant_t *js, size_t size);
void ta_meta_free(ant_t *js, void *ptr, size_t size);
void buffer_views_add(ant_t *js, ArrayBufferData *buffer, void *view, bool dataview);
void buffer_views_remove(ant_t *js, ArrayBufferData *buffer, void *view);
void dataview_view_refresh(DataViewData *dv);
void typedarray_view_refresh(TypedArrayData *ta);
void arraybuffer_initialize(ant_t *js);
void sharedarraybuffer_initialize(ant_t *js);
void typedarray_define_methods(ant_t *js, ant_value_t typedarray_proto);
void typedarray_define_iterators(ant_t *js, ant_value_t typedarray_proto);
void uint8array_codec_initialize(ant_t *js);
void dataview_initialize(ant_t *js);

ant_value_t typedarray_this_error(ant_t *js, ant_value_t value);
ant_value_t js_typedarray_subarray(ant_params_t);
ant_value_t typedarray_initialize(ant_t *js);
ant_value_t buffer_unwrap_bigint(ant_value_t value);
ant_value_t buffer_to_number_value(ant_t *js, ant_value_t value);

ant_value_t typedarray_coerce_value(ant_t *js, const TypedArrayData *ta, ant_value_t value);
ant_value_t typedarray_write_value(ant_t *js, TypedArrayData *ta_data, size_t index, ant_value_t value);
ant_value_t js_typedarray_of(ant_native_params_t, TypedArrayType type, ant_value_t proto);
ant_value_t buffer_unknown_encoding_error(ant_t *js, ant_value_t encoding);
ant_value_t buffer_require_encoding(ant_t *js, ant_value_t value, BufferEncoding *out);
ant_value_t buffer_bytes_to_string_enc(ant_t *js, const uint8_t *data, size_t len, BufferEncoding encoding);

ant_value_t create_typed_array_like(ant_t *js, ant_value_t this_val, TypedArrayType type, ArrayBufferData *buffer, size_t byte_offset, size_t length);
ant_value_t buffer_decode_binary(ant_t *js, const char *str, size_t len, BufferEncoding encoding, uint8_t **out, size_t *out_len);
ant_value_t buffer_encode_string(ant_t *js, ant_value_t value, BufferEncoding encoding, const uint8_t **out, size_t *out_len, uint8_t **owned);
ant_value_t buffer_encode_into(ant_t *js, const char *str, size_t len, BufferEncoding encoding, uint8_t *dst, size_t cap, size_t *written);

size_t buffer_encoded_length(ant_t *js, ant_value_t str_value, const char *str, size_t len, BufferEncoding encoding);
size_t buffer_encode_capacity(ant_t *js, ant_value_t str_value, size_t len, BufferEncoding encoding);

static inline bool buffer_is_growable_shared(const ArrayBufferData *data) {
  return data && data->is_shared && data->resizable;
}

static inline TypedArrayData *typedarray_view_live(TypedArrayData *ta) {
  if (!ta || !ta->buffer) return NULL;
  return __builtin_expect(ta->buffer->views_slow, 0) ? typedarray_live_slow(ta) : ta;
}

static inline TypedArrayData *typedarray_live(ant_value_t value) {
  return typedarray_view_live(buffer_get_typedarray_data(value));
}

static inline void buffer_view_release(ant_t *js, ArrayBufferData *backing, void *view) {
  if (!backing) return;
  if (__builtin_expect(backing->resizable, 0)) buffer_views_remove(js, backing, view);
  free_array_buffer_data(js, backing);
}

static inline ssize_t normalize_index(ssize_t idx, ssize_t len) {
  if (idx < 0) idx += len;
  if (idx < 0) return 0;
  if (idx > len) return len;
  return idx;
}

static inline bool typedarray_type_is_bigint(TypedArrayType type) {
  static const bool bigint[] = {
    #define BUFFER_TA_IS_BIGINT_INT    false
    #define BUFFER_TA_IS_BIGINT_FLOAT  false
    #define BUFFER_TA_IS_BIGINT_BIGINT true
    #define BUFFER_TYPED_ARRAY(name, ctor, ctype, kind, ...) BUFFER_TA_IS_BIGINT_##kind,
    #include "buffer/typed_arrays.h"
    #undef BUFFER_TA_IS_BIGINT_INT
    #undef BUFFER_TA_IS_BIGINT_FLOAT
    #undef BUFFER_TA_IS_BIGINT_BIGINT
  };

  return type < TYPED_ARRAY_COUNT && bigint[type];
}

static inline size_t buffer_byte_length(const ArrayBufferData *data) {
  return data->is_shared ? __atomic_load_n(&data->length, __ATOMIC_SEQ_CST) : data->length;
}

static inline uint64_t buffer_load_bytes(const uint8_t *ptr, size_t width, bool little_endian) {
  uint64_t bits = 0;
  memcpy(&bits, ptr, width);
  return little_endian ? bits : __builtin_bswap64(bits) >> (64 - 8 * width);
}

static inline void buffer_store_bytes(uint8_t *ptr, size_t width, bool little_endian, uint64_t bits) {
  if (!little_endian) bits = __builtin_bswap64(bits << (64 - 8 * width));
  memcpy(ptr, &bits, width);
}

static inline __attribute__((always_inline)) ant_value_t buffer_num_decode(
  ant_t *js, uint64_t bits, buffer_num_kind_t kind, size_t width
) {
  unsigned shift = 64 - 8 * (unsigned)width;
  switch (kind) {
    case BUFFER_NUM_UINT:    return Ant_Value_FromNumber((double)bits);
    case BUFFER_NUM_INT:     return Ant_Value_FromNumber((double)((int64_t)(bits << shift) >> shift));
    case BUFFER_NUM_BIGINT:  return bigint_from_int64(js, (int64_t)bits);
    case BUFFER_NUM_BIGUINT: return bigint_from_uint64(js, bits);
    case BUFFER_NUM_FLOAT: break;
  }
  
  if (width == 2) return Ant_Value_FromNumber(half_to_double((uint16_t)bits));
  if (width == 4) {
    float f; uint32_t narrow = (uint32_t)bits;
    memcpy(&f, &narrow, sizeof(f));
    return Ant_Value_FromNumber((double)f);
  }
  
  double d;
  memcpy(&d, &bits, sizeof(d));
  return Ant_Value_FromNumber(d);
}

static inline __attribute__((always_inline)) uint64_t buffer_float_bits(double num, size_t width) {
  uint64_t bits;
  if (width == 2) return double_to_half(num);
  if (width == 4) { float f = (float)num; uint32_t narrow; memcpy(&narrow, &f, sizeof(narrow)); return narrow; }
  memcpy(&bits, &num, sizeof(bits));
  return bits;
}

static inline bool buffer_encoding_is_binary(BufferEncoding encoding) {
  return encoding == ENC_HEX || encoding == ENC_BASE64 || encoding == ENC_BASE64URL;
}

static inline uint8_t typedarray_to_uint8_clamped(double value) {
  if (isnan(value) || value <= 0.0) return 0;
  if (value >= 255.0) return 255;

  double floor_value = floor(value);
  double fraction = value - floor_value;
  
  if (fraction > 0.5) return (uint8_t)(floor_value + 1.0);
  if (fraction < 0.5) return (uint8_t)floor_value;
  
  return ((uint32_t)floor_value & 1U) ? (uint8_t)(floor_value + 1.0) : (uint8_t)floor_value;
}
