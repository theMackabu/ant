#ifndef ANT_JIT_OPT_TARGET_H
#define ANT_JIT_OPT_TARGET_H

#include "code.h"

static constexpr uint8_t OPT_FPR_BASE = 32;
static constexpr uint32_t OPT_REGISTER_COUNT = 64;

static constexpr uint8_t OPT_DIRECT_CALL_TEMPS = 3;
static constexpr uint32_t OPT_DIRECT_CALL_MAX_INPUTS = 6;
static constexpr uint32_t OPT_LOWERING_FPRS = 3;

typedef uint64_t opt_register_mask_t;

static inline bool opt_mask_contains(opt_register_mask_t mask, uint8_t reg) {
  return (mask >> reg) & 1;
}

static inline opt_register_mask_t opt_mask_with(opt_register_mask_t mask, uint8_t reg) {
  return mask | (UINT64_C(1) << reg);
}

typedef enum : uint8_t {
  OPT_CLASS_GENERAL,
  OPT_CLASS_FLOAT,
} opt_register_class_t;

static inline opt_register_class_t opt_register_class_of(opt_repr_t repr) {
  return repr == OPT_REPR_FLOAT64 ? OPT_CLASS_FLOAT : OPT_CLASS_GENERAL;
}

static inline opt_register_class_t opt_register_class_of_register(uint8_t reg) {
  return reg >= OPT_FPR_BASE ? OPT_CLASS_FLOAT : OPT_CLASS_GENERAL;
}

typedef struct {
  opt_register_mask_t allocatable_gprs;
  opt_register_mask_t allocatable_fprs;
  opt_register_mask_t caller_saved_gprs;
  opt_register_mask_t caller_saved_fprs;
  uint8_t argument_gprs[8];
  uint8_t argument_count;
  uint8_t return_gpr;
  uint8_t scratch_gpr;
} opt_register_set_t;

typedef struct {
  uint8_t vm;
  uint8_t frame;
  uint8_t scratch;
} opt_pinned_registers_t;

typedef enum : uint8_t {
  OPT_TARGET_AARCH64,
  OPT_TARGET_X86_64,
} opt_target_t;

#if defined(__aarch64__) || defined(_M_ARM64)
static constexpr opt_target_t OPT_TARGET_HOST = OPT_TARGET_AARCH64;
#else
static constexpr opt_target_t OPT_TARGET_HOST = OPT_TARGET_X86_64;
#endif

static inline opt_register_mask_t opt_allocatable(const opt_register_set_t *set, opt_register_class_t cls) {
  return cls == OPT_CLASS_GENERAL ? set->allocatable_gprs : set->allocatable_fprs << OPT_FPR_BASE;
}

static inline opt_register_mask_t opt_caller_saved(const opt_register_set_t *set) {
  return set->caller_saved_gprs | (set->caller_saved_fprs << OPT_FPR_BASE);
}

void opt_target_registers(opt_target_t target, opt_register_set_t *set, opt_pinned_registers_t *pinned);
opt_register_mask_t opt_few_registers(const opt_register_set_t *set, uint32_t extra);
opt_register_set_t opt_test_registers(void);

#endif
