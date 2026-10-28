#ifndef ANT_JIT_OPT_ASM_H
#define ANT_JIT_OPT_ASM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum {
  ASM_ARCH_X64,
  ASM_ARCH_A64,
} asm_arch_t;

typedef uint8_t asm_gpr_t;
typedef uint8_t asm_fpr_t;

typedef uint32_t asm_regset_t;

#define ASM_REG(r) ((asm_regset_t)1 << (r))

static inline bool asm_regset_contains(asm_regset_t set, uint8_t reg) {
  return (set & ASM_REG(reg)) != 0;
}

static inline asm_regset_t asm_regset_with(asm_regset_t set, uint8_t reg) {
  return set | ASM_REG(reg);
}

static inline asm_regset_t asm_regset_without(asm_regset_t set, uint8_t reg) {
  return set & ~ASM_REG(reg);
}

static inline uint32_t asm_regset_len(asm_regset_t set) {
  return (uint32_t)__builtin_popcount(set);
}

static inline uint8_t asm_regset_first(asm_regset_t set) {
  return (uint8_t)__builtin_ctz(set);
}

static inline uint8_t asm_regset_last(asm_regset_t set) {
  return (uint8_t)(31 - __builtin_clz(set));
}

#define ASM_REGSET_EACH(set, reg) \
  for (asm_regset_t reg##_rest = (set); reg##_rest; reg##_rest &= reg##_rest - 1) \
    for (uint8_t reg = asm_regset_first(reg##_rest), reg##_once = 1; reg##_once; reg##_once = 0)

#define ASM_REGSET_EACH_REVERSE(set, reg) \
  for (asm_regset_t reg##_rest = (set); reg##_rest; reg##_rest &= ~ASM_REG(asm_regset_last(reg##_rest))) \
    for (uint8_t reg = asm_regset_last(reg##_rest), reg##_once = 1; reg##_once; reg##_once = 0)

typedef enum {
  ASM_SCALE_1 = 0,
  ASM_SCALE_2 = 1,
  ASM_SCALE_4 = 2,
  ASM_SCALE_8 = 3,
} asm_scale_t;

typedef struct {
  asm_gpr_t base;
  asm_gpr_t index;
  bool has_index;
  asm_scale_t scale;
  int32_t disp;
} asm_address_t;

static inline asm_address_t asm_addr(asm_gpr_t base, int32_t disp) {
  return (asm_address_t){ .base = base, .disp = disp };
}

static inline asm_address_t asm_addr_indexed(asm_gpr_t base, asm_gpr_t index, asm_scale_t scale, int32_t disp) {
  return (asm_address_t){ .base = base, .index = index, .has_index = true, .scale = scale, .disp = disp };
}

typedef enum {
  ASM_W32,
  ASM_W64,
} asm_width_t;

static inline uint32_t asm_width_bits(asm_width_t width) {
  return width == ASM_W64 ? 64 : 32;
}

typedef struct {
  uint64_t lo;
  uint64_t hi;
} asm_u128_t;

typedef enum {
  ASM_COND_EQUAL,
  ASM_COND_NOT_EQUAL,
  ASM_COND_LESS_THAN,
  ASM_COND_LESS_THAN_OR_EQUAL,
  ASM_COND_GREATER_THAN,
  ASM_COND_GREATER_THAN_OR_EQUAL,
  ASM_COND_BELOW,
  ASM_COND_BELOW_OR_EQUAL,
  ASM_COND_ABOVE,
  ASM_COND_ABOVE_OR_EQUAL,
  ASM_COND_OVERFLOW,
  ASM_COND_NO_OVERFLOW,
  ASM_COND_ZERO,
  ASM_COND_NON_ZERO,
  ASM_COND_NEGATIVE,
  ASM_COND_POSITIVE_OR_ZERO,
} asm_cond_t;

static inline asm_cond_t asm_cond_invert(asm_cond_t cond) {
  switch (cond) {
    case ASM_COND_EQUAL: return ASM_COND_NOT_EQUAL;
    case ASM_COND_NOT_EQUAL: return ASM_COND_EQUAL;
    case ASM_COND_LESS_THAN: return ASM_COND_GREATER_THAN_OR_EQUAL;
    case ASM_COND_LESS_THAN_OR_EQUAL: return ASM_COND_GREATER_THAN;
    case ASM_COND_GREATER_THAN: return ASM_COND_LESS_THAN_OR_EQUAL;
    case ASM_COND_GREATER_THAN_OR_EQUAL: return ASM_COND_LESS_THAN;
    case ASM_COND_BELOW: return ASM_COND_ABOVE_OR_EQUAL;
    case ASM_COND_BELOW_OR_EQUAL: return ASM_COND_ABOVE;
    case ASM_COND_ABOVE: return ASM_COND_BELOW_OR_EQUAL;
    case ASM_COND_ABOVE_OR_EQUAL: return ASM_COND_BELOW;
    case ASM_COND_OVERFLOW: return ASM_COND_NO_OVERFLOW;
    case ASM_COND_NO_OVERFLOW: return ASM_COND_OVERFLOW;
    case ASM_COND_ZERO: return ASM_COND_NON_ZERO;
    case ASM_COND_NON_ZERO: return ASM_COND_ZERO;
    case ASM_COND_NEGATIVE: return ASM_COND_POSITIVE_OR_ZERO;
    case ASM_COND_POSITIVE_OR_ZERO: return ASM_COND_NEGATIVE;
  }
  return cond;
}

typedef enum {
  ASM_DCOND_EQUAL,
  ASM_DCOND_NOT_EQUAL,
  ASM_DCOND_LESS_THAN,
  ASM_DCOND_LESS_THAN_OR_EQUAL,
  ASM_DCOND_GREATER_THAN,
  ASM_DCOND_GREATER_THAN_OR_EQUAL,
  ASM_DCOND_EQUAL_OR_UNORDERED,
  ASM_DCOND_NOT_EQUAL_OR_UNORDERED,
  ASM_DCOND_LESS_THAN_OR_UNORDERED,
  ASM_DCOND_LESS_THAN_OR_EQUAL_OR_UNORDERED,
  ASM_DCOND_GREATER_THAN_OR_UNORDERED,
  ASM_DCOND_GREATER_THAN_OR_EQUAL_OR_UNORDERED,
  ASM_DCOND_ORDERED,
  ASM_DCOND_UNORDERED,
} asm_dcond_t;

static inline asm_dcond_t asm_dcond_invert(asm_dcond_t cond) {
  switch (cond) {
    case ASM_DCOND_EQUAL: return ASM_DCOND_NOT_EQUAL_OR_UNORDERED;
    case ASM_DCOND_NOT_EQUAL: return ASM_DCOND_EQUAL_OR_UNORDERED;
    case ASM_DCOND_LESS_THAN: return ASM_DCOND_GREATER_THAN_OR_EQUAL_OR_UNORDERED;
    case ASM_DCOND_LESS_THAN_OR_EQUAL: return ASM_DCOND_GREATER_THAN_OR_UNORDERED;
    case ASM_DCOND_GREATER_THAN: return ASM_DCOND_LESS_THAN_OR_EQUAL_OR_UNORDERED;
    case ASM_DCOND_GREATER_THAN_OR_EQUAL: return ASM_DCOND_LESS_THAN_OR_UNORDERED;
    case ASM_DCOND_EQUAL_OR_UNORDERED: return ASM_DCOND_NOT_EQUAL;
    case ASM_DCOND_NOT_EQUAL_OR_UNORDERED: return ASM_DCOND_EQUAL;
    case ASM_DCOND_LESS_THAN_OR_UNORDERED: return ASM_DCOND_GREATER_THAN_OR_EQUAL;
    case ASM_DCOND_LESS_THAN_OR_EQUAL_OR_UNORDERED: return ASM_DCOND_GREATER_THAN;
    case ASM_DCOND_GREATER_THAN_OR_UNORDERED: return ASM_DCOND_LESS_THAN_OR_EQUAL;
    case ASM_DCOND_GREATER_THAN_OR_EQUAL_OR_UNORDERED: return ASM_DCOND_LESS_THAN;
    case ASM_DCOND_ORDERED: return ASM_DCOND_UNORDERED;
    case ASM_DCOND_UNORDERED: return ASM_DCOND_ORDERED;
  }
  return cond;
}

static inline bool asm_dcond_evaluate(asm_dcond_t cond, double lhs, double rhs) {
  bool unordered = lhs != lhs || rhs != rhs;
  switch (cond) {
    case ASM_DCOND_EQUAL: return lhs == rhs;
    case ASM_DCOND_NOT_EQUAL: return !unordered && lhs != rhs;
    case ASM_DCOND_LESS_THAN: return lhs < rhs;
    case ASM_DCOND_LESS_THAN_OR_EQUAL: return lhs <= rhs;
    case ASM_DCOND_GREATER_THAN: return lhs > rhs;
    case ASM_DCOND_GREATER_THAN_OR_EQUAL: return lhs >= rhs;
    case ASM_DCOND_EQUAL_OR_UNORDERED: return unordered || lhs == rhs;
    case ASM_DCOND_NOT_EQUAL_OR_UNORDERED: return lhs != rhs;
    case ASM_DCOND_LESS_THAN_OR_UNORDERED: return unordered || lhs < rhs;
    case ASM_DCOND_LESS_THAN_OR_EQUAL_OR_UNORDERED: return unordered || lhs <= rhs;
    case ASM_DCOND_GREATER_THAN_OR_UNORDERED: return unordered || lhs > rhs;
    case ASM_DCOND_GREATER_THAN_OR_EQUAL_OR_UNORDERED: return unordered || lhs >= rhs;
    case ASM_DCOND_ORDERED: return !unordered;
    case ASM_DCOND_UNORDERED: return unordered;
  }
  return false;
}

typedef enum {
  ASM_NEGATIVE_ZERO_ALLOW,
  ASM_NEGATIVE_ZERO_FAIL,
} asm_negative_zero_t;

typedef struct {
  asm_regset_t saved_gprs;
  asm_regset_t saved_fprs;
  uint32_t locals_size;
  uint32_t caller_stack_offset;
} asm_frame_t;

[[noreturn]] void asm_panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define ASM_CHECK(cond, ...) do { if (!(cond)) asm_panic(__VA_ARGS__); } while (0)

static inline asm_address_t asm_frame_local(const asm_frame_t *frame, asm_gpr_t sp, uint32_t offset) {
  ASM_CHECK(offset < frame->locals_size, "local offset out of range");
  return asm_addr(sp, (int32_t)offset);
}

static inline uint64_t asm_f64_bits(double value) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

static inline double asm_f64_from_bits(uint64_t bits) {
  double value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

#endif
