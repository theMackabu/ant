#ifndef ANT_JIT_OPT_IR_VALUE_H
#define ANT_JIT_OPT_IR_VALUE_H

#include "../code.h"
#include "value.h"

#include <stddef.h>
#include <string.h>

static constexpr uint64_t IR_VALUE_UNDEFINED = NANBOX_PREFIX | ((uint64_t)kTypeUndefined << NANBOX_TYPE_SHIFT);
static constexpr uint64_t IR_VALUE_NULL = NANBOX_PREFIX | ((uint64_t)kTypeNull << NANBOX_TYPE_SHIFT);
static constexpr uint64_t IR_VALUE_FALSE = ANT_BOOL_TAG;
static constexpr uint64_t IR_VALUE_TRUE = ANT_BOOL_TAG | 1;
static constexpr uint64_t IR_VALUE_EMPTY = ANT_SENTINEL_TAG | 0xDEADu;
static constexpr uint64_t IR_VALUE_CANONICAL_NAN = UINT64_C(0x7FF8000000000000);

static constexpr uint64_t IR_VALUE_VIRTUAL_UNMAPPED_ARGUMENTS = ANT_SENTINEL_TAG | 0xA6A0u;
static constexpr uint64_t IR_VALUE_VIRTUAL_MAPPED_ARGUMENTS = ANT_SENTINEL_TAG | 0xA6A1u;

static constexpr uint32_t IR_VALUE_OBJECT_TYPES =
  (1u << kTypeObject) | (1u << kTypeArray) | (1u << kTypeFunction) | (1u << kTypePromise) | (1u << kTypeGenerator);

static inline double ir_value_to_double(uint64_t bits) {
  double number;
  memcpy(&number, &bits, sizeof number);
  return number;
}

static inline uint64_t ir_value_double_bits(double number) {
  uint64_t bits;
  memcpy(&bits, &number, sizeof bits);
  return bits;
}

static inline bool ir_value_is_number(uint64_t bits) {
  return bits <= NANBOX_PREFIX;
}

static inline ant_value_type_t ir_value_type(uint64_t bits) {
  return ir_value_is_number(bits) ? kTypeNumber : (ant_value_type_t)((bits >> NANBOX_TYPE_SHIFT) & NANBOX_TYPE_MASK);
}

static inline bool ir_value_is_object(uint64_t bits) {
  return !ir_value_is_number(bits) && ((1u << ir_value_type(bits)) & IR_VALUE_OBJECT_TYPES);
}

static inline bool ir_value_is_string(uint64_t bits) {
  return ir_value_type(bits) == kTypeString;
}

static inline bool ir_value_is_bool(uint64_t bits) {
  return ir_value_type(bits) == kTypeBool;
}

static inline bool ir_value_as_int32(uint64_t bits, int32_t *out) {
  if (!ir_value_is_number(bits)) return false;
  double number = ir_value_to_double(bits);
  if (!(number >= -2147483648.0 && number <= 2147483647.0)) return false;
  int32_t integer = (int32_t)number;
  if ((double)integer != number || (integer == 0 && signbit(number))) return false;
  if (out) *out = integer;
  return true;
}

static inline bool ir_value_is_double(uint64_t bits) {
  return ir_value_is_number(bits) && !ir_value_as_int32(bits, NULL);
}

static inline uint64_t ir_value_int32(int32_t value) {
  return ir_value_double_bits((double)value);
}

static inline uint64_t ir_value_number(double number) {
  if (isnan(number)) return IR_VALUE_CANONICAL_NAN;
  return ir_value_double_bits(number);
}

static inline uint64_t ir_value_boxed_constant(uint64_t bits, opt_repr_t repr) {
  switch (repr) {
    case OPT_REPR_INT32: return ir_value_int32((int32_t)(uint32_t)bits);
    case OPT_REPR_BOOL: return bits ? IR_VALUE_TRUE : IR_VALUE_FALSE;
    case OPT_REPR_FLOAT64: return ir_value_number(ir_value_to_double(bits));
    default: return bits;
  }
}

static inline int ir_value_virtual_arguments_kind(uint64_t bits) {
  if (bits == IR_VALUE_VIRTUAL_MAPPED_ARGUMENTS) return 1;
  if (bits == IR_VALUE_VIRTUAL_UNMAPPED_ARGUMENTS) return 0;
  return -1;
}

void ir_value_describe(uint64_t bits, char *buffer, size_t size);

#endif
