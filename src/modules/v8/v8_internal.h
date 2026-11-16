#pragma once

#include "errors.h"
#include "types.h"
#include "modules/buffer.h"

static constexpr uint8_t V8_WIRE_VERSION = 15;
static constexpr uint8_t V8_WIRE_MIN_VERSION = 13;
static constexpr char V8_VIEW_DATAVIEW = '?';

static constexpr uint32_t SERIALIZER_NATIVE_TAG = Ant_NativeTag("V8SR");
static constexpr uint32_t DESERIALIZER_NATIVE_TAG = Ant_NativeTag("V8DS");

typedef enum: uint8_t {
  V8_TAG_VERSION = 0xff,
  V8_TAG_PADDING = 0,

  V8_TAG_UNDEFINED = '_',
  V8_TAG_NULL      = '0',
  V8_TAG_TRUE      = 'T',
  V8_TAG_FALSE     = 'F',
  V8_TAG_INT32     = 'I',
  V8_TAG_UINT32    = 'U',
  V8_TAG_DOUBLE    = 'N',
  V8_TAG_BIGINT    = 'Z',

  V8_TAG_UTF8_STRING     = 'S',
  V8_TAG_ONE_BYTE_STRING = '"',
  V8_TAG_TWO_BYTE_STRING = 'c',

  V8_TAG_REFERENCE          = '^',
  V8_TAG_BEGIN_OBJECT       = 'o',
  V8_TAG_END_OBJECT         = '{',
  V8_TAG_BEGIN_DENSE_ARRAY  = 'A',
  V8_TAG_END_DENSE_ARRAY    = '$',
  V8_TAG_BEGIN_SPARSE_ARRAY = 'a',
  V8_TAG_END_SPARSE_ARRAY   = '@',
  V8_TAG_HOLE               = '-',
  V8_TAG_BEGIN_MAP          = ';',
  V8_TAG_END_MAP            = ':',
  V8_TAG_BEGIN_SET          = '\'',
  V8_TAG_END_SET            = ',',

  V8_TAG_DATE          = 'D',
  V8_TAG_REGEXP        = 'R',
  V8_TAG_ERROR         = 'r',
  V8_TAG_TRUE_OBJECT   = 'y',
  V8_TAG_FALSE_OBJECT  = 'x',
  V8_TAG_NUMBER_OBJECT = 'n',
  V8_TAG_BIGINT_OBJECT = 'z',
  V8_TAG_STRING_OBJECT = 's',

  V8_TAG_ARRAY_BUFFER           = 'B',
  V8_TAG_RESIZABLE_ARRAY_BUFFER = '~',
  V8_TAG_ARRAY_BUFFER_VIEW      = 'V',
  V8_TAG_ARRAY_BUFFER_TRANSFER  = 't',
  V8_TAG_SHARED_ARRAY_BUFFER    = 'u',
  V8_TAG_HOST_OBJECT            = '\\',
} v8_tag_t;

typedef enum: uint8_t {
  V8_ERROR_MESSAGE = 'm',
  V8_ERROR_STACK   = 's',
  V8_ERROR_CAUSE   = 'c',
  V8_ERROR_END     = '.',
} v8_error_tag_t;

static const char v8_view_tags[TYPED_ARRAY_COUNT] = {
  [TYPED_ARRAY_INT8]          = 'b',
  [TYPED_ARRAY_UINT8]         = 'B',
  [TYPED_ARRAY_UINT8_CLAMPED] = 'C',
  [TYPED_ARRAY_INT16]         = 'w',
  [TYPED_ARRAY_UINT16]        = 'W',
  [TYPED_ARRAY_INT32]         = 'd',
  [TYPED_ARRAY_UINT32]        = 'D',
  [TYPED_ARRAY_FLOAT16]       = 'h',
  [TYPED_ARRAY_FLOAT32]       = 'f',
  [TYPED_ARRAY_FLOAT64]       = 'F',
  [TYPED_ARRAY_BIGINT64]      = 'q',
  [TYPED_ARRAY_BIGUINT64]     = 'Q',
};

static const char v8_error_prototype[JS_ERR_TYPE_COUNT] = {
  [JS_ERR_EVAL]      = 'E',
  [JS_ERR_RANGE]     = 'R',
  [JS_ERR_REFERENCE] = 'F',
  [JS_ERR_SYNTAX]    = 'S',
  [JS_ERR_TYPE]      = 'T',
  [JS_ERR_URI]       = 'U',
};

static const struct {
  char flag;
  uint16_t bit;
} v8_regexp_flags[] = {
  { 'd', 1 << 7 },
  { 'g', 1 << 0 },
  { 'i', 1 << 1 },
  { 'm', 1 << 2 },
  { 's', 1 << 5 },
  { 'u', 1 << 4 },
  { 'v', 1 << 8 },
  { 'y', 1 << 3 },
};

ant_value_t v8_serializer_constructor(ant_t *js);
ant_value_t v8_deserializer_constructor(ant_t *js);
