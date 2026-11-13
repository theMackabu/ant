#pragma once

#include "shared/backend.h" // IWYU pragma: keep

static inline ant_devices_t *ant_hvf_devices(ant_hvf_vm_t *vm) {
  if (!vm->devices) {
    #if defined(__aarch64__)
    vm->devices = ant_devices_new(ANT_HVF_PCIE_ECAM_BASE, ANT_HVF_PCIE_ECAM_SIZE);
    #else
    vm->devices = ant_devices_new(0, 0);
    #endif
  }
  return vm->devices;
}

static inline int ant_hvf_net_start(ant_hvf_vm_t *vm) {
  if (!ant_hvf_devices(vm)) return -ENOMEM;
  
  ant_net_builder_t *builder = ant_net_new(vm, vm->net_mac, vm->net_allow_host_all, vm->net_allow_lan);
  if (!builder) return -ENOMEM;
  
  for (size_t i = 0; i < vm->net_forward_count; i++)
    ant_net_add_forward(builder, vm->net_forwards[i].host_port, vm->net_forwards[i].guest_port);
  
  for (size_t i = 0; i < vm->net_allow_host_port_count; i++)
    ant_net_allow_host_port(builder, vm->net_allow_host_ports[i]);
  
  return ant_net_start(builder);
}
