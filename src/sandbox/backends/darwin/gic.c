#include "backend.h"

#if defined(__aarch64__)

int ant_hvf_create_gic(ant_hvf_vm_t *vm) {
  hv_gic_config_t config = ant_hvf_api.gic_config_create();
  if (!config) return -EIO;

  int rc = ant_hvf_check(
    ant_hvf_api.gic_config_set_distributor_base(config, ANT_HVF_GIC_DIST_BASE),
    "hv_gic_config_set_distributor_base"
  );
  
  if (rc == 0) rc = ant_hvf_check(
    ant_hvf_api.gic_config_set_redistributor_base(config, ANT_HVF_GIC_REDIST_BASE),
    "hv_gic_config_set_redistributor_base"
  );
  
  if (rc == 0) rc = ant_hvf_check(
    ant_hvf_api.gic_config_set_msi_region_base(config, ANT_HVF_GIC_MSI_BASE),
    "hv_gic_config_set_msi_region_base"
  );
  
  if (rc == 0) rc = ant_hvf_check(
    ant_hvf_api.gic_config_set_msi_interrupt_range(config, ANT_HVF_GIC_MSI_VECTOR_BASE, ANT_HVF_GIC_MSI_VECTOR_COUNT),
    "hv_gic_config_set_msi_interrupt_range"
  );
  
  if (rc == 0) rc = ant_hvf_check(ant_hvf_api.gic_create(config), "hv_gic_create");

  os_release(config);
  if (rc != 0) return rc;

  vm->gic_msi_enabled = true;
  vm->gic_msi_base = ANT_HVF_GIC_MSI_VECTOR_BASE;
  vm->gic_msi_count = ANT_HVF_GIC_MSI_VECTOR_COUNT;
  
  return 0;
}

int ant_hvf_send_msi(ant_hvf_vm_t *vm, uint64_t addr, uint32_t data) {
  if (!vm->gic_msi_enabled) return -ENOSYS;
  return ant_hvf_api.gic_send_msi((hv_ipa_t)addr, data) == HV_SUCCESS ? 0 : -EIO;
}

int ant_hvf_raise_vtimer(ant_hvf_vm_t *vm) {
  ant_hvf_api.gic_set_redistributor_reg(
    vm->vcpu, HV_GIC_REDISTRIBUTOR_REG_GICR_ISPENDR0,
    1ull << ANT_HVF_GIC_EL1_VIRTUAL_TIMER
  );
  
  ant_hvf_api.vcpu_set_pending_interrupt(vm->vcpu, HV_INTERRUPT_TYPE_IRQ, true);
  return ant_hvf_check(ant_hvf_api.vcpu_set_vtimer_mask(vm->vcpu, false), "hv_vcpu_set_vtimer_mask(vtimer)");
}

#endif
