#ifdef HVF
HVF(vm_config_create)
HVF(vm_create)
HVF(vm_destroy)
HVF(vm_map)
HVF(vm_unmap)

HVF(vcpu_config_create)
HVF(vcpu_config_get_feature_reg)
HVF(vcpu_create)
HVF(vcpu_destroy)
HVF(vcpu_run)
HVF(vcpus_exit)

HVF(vcpu_get_reg)
HVF(vcpu_set_reg)
HVF(vcpu_get_sys_reg)
HVF(vcpu_set_sys_reg)
HVF(vcpu_set_vtimer_mask)
HVF(vcpu_get_vtimer_offset)
HVF(vcpu_set_pending_interrupt)

HVF(gic_config_create)
HVF(gic_config_set_distributor_base)
HVF(gic_config_set_redistributor_base)
HVF(gic_config_set_msi_region_base)
HVF(gic_config_set_msi_interrupt_range)
HVF(gic_create)
HVF(gic_set_redistributor_reg)
HVF(gic_send_msi)
#undef HVF
#endif
