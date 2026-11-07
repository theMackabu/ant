#include "backend.h"

#if defined(__aarch64__)

int ant_hvf_advance_pc(hv_vcpu_t vcpu) {
  uint64_t pc = 0;
  int rc = ant_hvf_check(ant_hvf_api.vcpu_get_reg(vcpu, HV_REG_PC, &pc), "hv_vcpu_get_reg(PC)");
  if (rc != 0) return rc;
  return ant_hvf_check(ant_hvf_api.vcpu_set_reg(vcpu, HV_REG_PC, pc + 4), "hv_vcpu_set_reg(PC)");
}

int ant_hvf_handle_mmio(ant_hvf_vm_t *vm, hv_vcpu_exit_exception_t *ex) {
  uint32_t esr = (uint32_t)ex->syndrome;
  unsigned ec = (esr >> ESR_EC_SHIFT) & 0x3f;
  if (ec == ESR_EC_HVC64) {
    uint64_t fn = 0;
    int rc = ant_hvf_check(
      ant_hvf_api.vcpu_get_reg(vm->vcpu, HV_REG_X0, &fn),
      "hv_vcpu_get_reg(HVC X0)"
    );
    if (rc != 0) return rc;
    if (fn == 0x84000008u || fn == 0x84000009u) return ANT_HVF_GUEST_SHUTDOWN;
    rc = ant_hvf_check(
      ant_hvf_api.vcpu_set_reg(vm->vcpu, HV_REG_X0, UINT64_MAX),
      "hv_vcpu_set_reg(HVC X0)"
    );
    if (rc != 0) return rc;
    return ant_hvf_advance_pc(vm->vcpu);
  }
  
  if (ec == ESR_EC_HLT && (esr & ESR_HLT_IMM16_MASK) == 0xf000u) return ANT_HVF_GUEST_SHUTDOWN;
  if (ec != ESR_EC_DATA_ABORT || !(esr & ESR_DA_ISV)) return -ENOSYS;

  unsigned size = 1u << ((esr >> ESR_DA_SAS_SHIFT) & 0x3);
  unsigned reg = (esr >> ESR_DA_SRT_SHIFT) & 0x1f;
  bool write = (esr & ESR_DA_WNR) != 0;

  if (write) {
    uint64_t value = 0;
    int rc = ant_hvf_check(
      ant_hvf_api.vcpu_get_reg(vm->vcpu, (hv_reg_t)(HV_REG_X0 + reg), &value),
      "hv_vcpu_get_reg(mmio write)"
    );
    if (rc != 0) return rc;
    if (!ant_hvf_mmio_write(vm, ex->physical_address, size, value)) return -ENOSYS;
  } else {
    uint64_t value = 0;
    if (!ant_hvf_mmio_read(vm, ex->physical_address, size, &value)) return -ENOSYS;
    int rc = ant_hvf_check(
      ant_hvf_api.vcpu_set_reg(vm->vcpu, (hv_reg_t)(HV_REG_X0 + reg), value),
      "hv_vcpu_set_reg(mmio read)"
    );
    if (rc != 0) return rc;
  }

  return ant_hvf_advance_pc(vm->vcpu);
}

#endif
