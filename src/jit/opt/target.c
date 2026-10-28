#include "target.h"
#include "asm/a64.h"
#include "asm/x64.h"

typedef struct {
  opt_register_mask_t allocatable_gprs;
  opt_register_mask_t allocatable_fprs;
  opt_register_mask_t callee_saved_gprs;
  opt_register_mask_t caller_saved_gprs;
  opt_register_mask_t caller_saved_fprs;
  uint8_t argument_gprs[8];
  uint8_t argument_count;
  uint8_t return_gpr;
} opt_target_description_t;

static opt_target_description_t opt_describe_target(opt_target_t target) {
  opt_target_description_t d;
  const asm_gpr_t *args;
  uint32_t nargs;
  
  if (target == OPT_TARGET_AARCH64) {
    d = (opt_target_description_t){
      .allocatable_gprs = MASM_A64_ALLOCATABLE_GPRS,
      .allocatable_fprs = MASM_A64_ALLOCATABLE_FPRS,
      .callee_saved_gprs = MASM_A64_CALLEE_SAVED_GPRS,
      .caller_saved_gprs = MASM_A64_CALLER_SAVED_GPRS,
      .caller_saved_fprs = MASM_A64_CALLER_SAVED_FPRS,
      .return_gpr = MASM_A64_RETURN_GPRS[0],
    };
    args = MASM_A64_ARGUMENT_GPRS;
    nargs = sizeof(MASM_A64_ARGUMENT_GPRS) / sizeof(MASM_A64_ARGUMENT_GPRS[0]);
  } else {
    d = (opt_target_description_t){
      .allocatable_gprs = MASM_X64_ALLOCATABLE_GPRS,
      .allocatable_fprs = MASM_X64_ALLOCATABLE_FPRS,
      .callee_saved_gprs = MASM_X64_CALLEE_SAVED_GPRS,
      .caller_saved_gprs = MASM_X64_CALLER_SAVED_GPRS,
      .caller_saved_fprs = MASM_X64_CALLER_SAVED_FPRS,
      .return_gpr = MASM_X64_RETURN_GPRS[0],
    };
    args = MASM_X64_ARGUMENT_GPRS;
    nargs = sizeof(MASM_X64_ARGUMENT_GPRS) / sizeof(MASM_X64_ARGUMENT_GPRS[0]);
  }
  
  for (uint32_t i = 0; i < nargs && i < sizeof(d.argument_gprs); i++) d.argument_gprs[i] = (uint8_t)args[i];
  d.argument_count = (uint8_t)nargs;
  return d;
}

void opt_target_registers(opt_target_t target, opt_register_set_t *set, opt_pinned_registers_t *pinned) {
  opt_target_description_t description = opt_describe_target(target);
  uint8_t reserved[3];
  uint32_t count = 0;
  opt_register_mask_t callee_saved = description.callee_saved_gprs & description.allocatable_gprs;
  for (uint8_t reg = 0; reg < 32 && count < 3; reg++)
    if (opt_mask_contains(callee_saved, reg)) reserved[count++] = reg;
  if (pinned) *pinned = (opt_pinned_registers_t){.vm = reserved[0], .frame = reserved[1], .scratch = reserved[2]};
  opt_register_mask_t allocatable = description.allocatable_gprs;
  for (uint32_t i = 0; i < 3; i++) allocatable &= ~(UINT64_C(1) << reserved[i]);
  opt_register_mask_t fprs = 0;
  uint32_t skipped = 0;
  for (uint8_t reg = 0; reg < 32; reg++) {
    if (!opt_mask_contains(description.allocatable_fprs, reg)) continue;
    if (skipped++ < OPT_LOWERING_FPRS) continue;
    fprs = opt_mask_with(fprs, reg);
  }
  *set = (opt_register_set_t){
    .allocatable_gprs = allocatable,
    .allocatable_fprs = fprs,
    .caller_saved_gprs = description.caller_saved_gprs,
    .caller_saved_fprs = description.caller_saved_fprs,
    .argument_count = description.argument_count,
    .return_gpr = description.return_gpr,
    .scratch_gpr = reserved[2],
  };
  for (uint32_t i = 0; i < description.argument_count; i++) set->argument_gprs[i] = description.argument_gprs[i];
}

opt_register_mask_t opt_few_registers(const opt_register_set_t *set, uint32_t extra) {
  opt_register_mask_t mask = opt_mask_with(0, set->return_gpr);
  for (uint32_t i = 0; i < set->argument_count; i++) mask = opt_mask_with(mask, set->argument_gprs[i]);
  mask &= set->allocatable_gprs;
  opt_register_mask_t result = mask;
  for (uint8_t reg = 0; reg < 64 && extra; reg++) {
    if (!opt_mask_contains(set->allocatable_gprs, reg) || opt_mask_contains(mask, reg)) continue;
    result = opt_mask_with(result, reg);
    extra--;
  }
  return result;
}

opt_register_set_t opt_test_registers(void) {
  return (opt_register_set_t){
    .allocatable_gprs = 0x3f,
    .allocatable_fprs = 0xff,
    .caller_saved_gprs = 0xff,
    .caller_saved_fprs = 0xff,
    .argument_gprs = {1, 2, 3},
    .argument_count = 3,
    .return_gpr = 0,
    .scratch_gpr = 7,
  };
}
