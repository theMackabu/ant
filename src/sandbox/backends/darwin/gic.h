#pragma once

#include <compat.h> // IWYU pragma: keep

#include "hvf.h"
#include "sandbox_backend/forward.h"

#include <stdbool.h>
#include <stdint.h>

#if defined(__aarch64__)

typedef void *ant_hvf_gic_config_t;
typedef uint32_t ant_hvf_gic_redistributor_reg_t;
typedef hv_return_t (*ant_hvf_gic_create_fn)(ant_hvf_gic_config_t);
typedef ant_hvf_gic_config_t (*ant_hvf_gic_config_create_fn)(void);
typedef hv_return_t (*ant_hvf_gic_config_set_base_fn)(ant_hvf_gic_config_t, hv_ipa_t);
typedef hv_return_t (*ant_hvf_gic_config_set_range_fn)(ant_hvf_gic_config_t, uint32_t, uint32_t);
typedef hv_return_t (*ant_hvf_gic_set_spi_fn)(uint32_t, bool);
typedef hv_return_t (*ant_hvf_gic_set_redistributor_reg_fn)(hv_vcpu_t, ant_hvf_gic_redistributor_reg_t, uint64_t);
typedef hv_return_t (*ant_hvf_gic_send_msi_fn)(hv_ipa_t, uint32_t);

typedef struct {
  ant_hvf_gic_create_fn create;
  ant_hvf_gic_config_create_fn config_create;
  ant_hvf_gic_config_set_base_fn set_distributor_base;
  ant_hvf_gic_config_set_base_fn set_redistributor_base;
  ant_hvf_gic_config_set_base_fn set_msi_region_base;
  ant_hvf_gic_config_set_range_fn set_msi_interrupt_range;
  ant_hvf_gic_set_spi_fn set_spi;
  ant_hvf_gic_set_redistributor_reg_fn set_redistributor_reg;
  ant_hvf_gic_send_msi_fn send_msi;
} ant_hvf_gic_api_t;

extern ant_hvf_gic_api_t ant_hvf_gic;

int ant_hvf_load_gic_api(void);
int ant_hvf_create_gic(ant_hvf_vm_t *vm);
bool ant_hvf_gic_msi_read(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t *value);
bool ant_hvf_gic_msi_write(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t value);

#endif
