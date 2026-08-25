#ifndef ANT_VALUE_H
#define ANT_VALUE_H

#include "cage.h"
#include "types.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

// An IEEE 754 double-precision float is a 64-bit value with bits laid out like:
//
// 1 Sign bit
// | 11 Exponent bits
// | |          52 Mantissa (i.e. fraction) bits
// | |          |
// S[Exponent-][Mantissa------------------------------------------]
//
// A NaN is any value where all exponent bits are set and the mantissa is
// non-zero. That means there are a *lot* of bit patterns that all represent
// NaN. NaN tagging takes advantage of this by repurposing those unused
// patterns to encode non-numeric values.
//
// We define a NANBOX_PREFIX as the upper 12 bits all set (0xFFF0...):
//
// 1111 1111 1111 0000 0000 0000 ... 0000
// [sign+exp all 1s  ] [mantissa all 0s  ]
//
// This corresponds to -Infinity in IEEE 754. Any 64-bit value strictly
// greater than this prefix is a tagged (non-numeric) value. Any value less
// than or equal to it is an unmodified double — so numeric math is free.
//
// For tagged values, we carve the remaining 52 mantissa bits into two fields:
//
// 1111 1111 1111 TTTTT DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD
// [-- prefix ---][type][--------------- 47-bit data --------------------]
//
// The 5-bit type tag (bits 51–47) encodes up to 31 distinct types: objects,
// strings, booleans, undefined, null, functions, closures, errors, etc.
//
// Managed references are not stored as raw pointers. Ant reserves a contiguous
// virtual-address cage of up to 2^44 bytes and allocates managed data, including
// native-function metadata, inside it. The payload stores the object's offset
// from the cage base. Decoding adds the process-local cage base back, so the
// representation does not depend on pointers fitting in 47 bits. Offset zero
// represents null; the cage allocator leaves its first page unused.
//
// Payloads that are not managed references hold immediate data, such as 1 for
// true and 0 for false. Full pointers that must remain outside the cage, such as
// external typed-array data, are represented by IDs in a separate handle table.
//
// Encoding and decoding are simple:
//
//   mkval(type, data) = PREFIX | (type << 47) | (data & 0x7FFFFFFFFFFF)
//   mkref(type, ptr)   = mkval(type, ptr - cage_base)
//   vtype(v)          = is_tagged(v) ? (v >> 47) & 0x1F : kTypeNumber
//   vdata(v)          = v & 0x7FFFFFFFFFFF
//   vptr(v)           = cage_base + vdata(v)
//   is_tagged(v)      = v > PREFIX

static constexpr uint64_t NANBOX_TYPE_MASK  = 0x1F;
static constexpr uint64_t NANBOX_TYPE_SHIFT = 47;
static constexpr uint64_t NANBOX_PREFIX     = UINT64_C(0xFFF0000000000000);
static constexpr uint64_t NANBOX_DATA_MASK  = UINT64_C(0x00007FFFFFFFFFFF);

typedef enum: uint8_t {
  kTypeObject = 0,
  kTypeString,
  kTypeArray,
  kTypeFunction,
  kTypeBuiltin,
  kTypePromise,
  kTypeGenerator,
  kTypeUndefined,
  kTypeNull,
  kTypeBool,
  kTypeNumber,
  kTypeBigInt,
  kTypeSymbol,
  kTypeError,
  kTypeTypedArray,
  kTypeFunctionInfo,
  kTypeMap,
  kTypeSet,
  kTypeWeakMap,
  kTypeWeakSet,
  kTypeSourceCode,
  kTypeSentinel = NANBOX_TYPE_MASK
} ant_value_type_t;

static constexpr ant_value_t ANT_BOOL_TAG = NANBOX_PREFIX | ((ant_value_t)kTypeBool << NANBOX_TYPE_SHIFT);
static constexpr ant_value_t ANT_SENTINEL_TAG = NANBOX_PREFIX | ((ant_value_t)kTypeSentinel << NANBOX_TYPE_SHIFT);

static constexpr ant_value_t js_false = ANT_BOOL_TAG;
static constexpr ant_value_t js_true  = ANT_BOOL_TAG | 1;

static inline ant_value_t js_bool(bool value) {
  return ANT_BOOL_TAG | (ant_value_t)value;
}

static inline bool is_tagged(ant_value_t value) {
  return value > NANBOX_PREFIX;
}

static inline ant_value_type_t vtype_tagged(ant_value_t value) {
  return (ant_value_type_t)((value >> NANBOX_TYPE_SHIFT) & NANBOX_TYPE_MASK);
}

static inline ant_value_type_t vtype(ant_value_t value) {
  return is_tagged(value) ? vtype_tagged(value) : kTypeNumber;
}

static inline uint64_t vdata(ant_value_t value) {
  return value & NANBOX_DATA_MASK;
}

static inline ant_value_t mkval(ant_value_type_t type, uint64_t payload) {
  return NANBOX_PREFIX
    | ((ant_value_t)(type & NANBOX_TYPE_MASK) << NANBOX_TYPE_SHIFT)
    | (payload & NANBOX_DATA_MASK);
}

static inline ant_value_t mkref(ant_value_type_t type, const void *ptr) {
  return mkval(type, ant_cage_encode(ptr));
}

static inline ant_value_t mkref_tagged(
  ant_value_type_t type, const void *ptr, uint64_t tag
) {
  return mkval(type, ant_cage_encode(ptr) | tag);
}

static inline void *vptr(ant_value_t value) {
  return ant_cage_decode_nonnull(vdata(value));
}

static inline void *vptr_masked(ant_value_t value, uint64_t tag_mask) {
  return ant_cage_decode_nonnull(vdata(value) & ~tag_mask);
}

static inline void *vptr_tagged(ant_value_t value) {
  return ant_cage_decode_nonnull(value & NANBOX_DATA_MASK);
}

static inline double tod(ant_value_t value) {
  union { ant_value_t value; double number; } bits = {value};
  return bits.number;
}

static inline ant_value_t tov(double number) {
  union { double number; ant_value_t value; } bits = {number};
  if (__builtin_expect(isnan(number), 0) && bits.value > NANBOX_PREFIX)
    return UINT64_C(0x7FF8000000000000); // canonical NaN
  return bits.value;
}

#endif
