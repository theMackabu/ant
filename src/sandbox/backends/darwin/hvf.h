#pragma once

#include <compat.h> // IWYU pragma: keep
#include "shared/aarch64_board.h" // IWYU pragma: keep

#include <stdint.h>
#include <Hypervisor/Hypervisor.h>

#if defined(__aarch64__)

static constexpr uint64_t ANT_HVF_NANOS_KAS_OFFSET_SYMBOL = 0x4052c1c0;
static constexpr uint32_t ANT_HVF_VIRTIO_9P_MAX = 8;
static constexpr uint64_t ANT_HVF_PAGE_SIZE = 0x1000;

static constexpr int ANT_HVF_GUEST_SHUTDOWN = 1;
static constexpr uint32_t ANT_HVF_GIC_EL1_VIRTUAL_TIMER = 27;

enum: uint32_t {
  ESR_EC_WFX_TRAP = 0x01,
  ESR_EC_HVC64 = 0x16,
  ESR_EC_DATA_ABORT = 0x24,
  ESR_EC_HLT = 0x34,
};

static constexpr unsigned ESR_EC_SHIFT = 26;
static constexpr uint32_t ESR_WFX_TI_WFE = 0x1;
static constexpr uint32_t ESR_DA_ISV = 1u << 24;
static constexpr uint32_t ESR_DA_WNR = 1u << 6;
static constexpr unsigned ESR_DA_SAS_SHIFT = 22;
static constexpr unsigned ESR_DA_SRT_SHIFT = 16;
static constexpr uint32_t ESR_HLT_IMM16_MASK = 0xffff;

static constexpr hv_sys_reg_t ANT_HVF_SYS_REG_CNTFRQ_EL0 = (hv_sys_reg_t)0xdf00;
static constexpr hv_sys_reg_t ANT_HVF_SYS_REG_CNTVCT_EL0 = (hv_sys_reg_t)0xdf02;

static constexpr size_t ANT_HVF_MAC_BYTES = 6;
static constexpr size_t ANT_HVF_ELF_IDENT_BYTES = 16;
static constexpr size_t ANT_HVF_UART_DIAGNOSTIC_DEFAULT_BYTES = 16 * 1024;

#if !__has_include(<Hypervisor/hv_gic.h>)
typedef void *hv_gic_config_t;
typedef uint32_t hv_gic_redistributor_reg_t;
static constexpr hv_gic_redistributor_reg_t HV_GIC_REDISTRIBUTOR_REG_GICR_ISPENDR0 = 0x10200;

hv_gic_config_t hv_gic_config_create(void);
hv_return_t hv_gic_create(hv_gic_config_t config);
hv_return_t hv_gic_send_msi(hv_ipa_t address, uint32_t intid);

hv_return_t hv_gic_config_set_distributor_base(hv_gic_config_t config, hv_ipa_t base);
hv_return_t hv_gic_config_set_redistributor_base(hv_gic_config_t config, hv_ipa_t base);
hv_return_t hv_gic_config_set_msi_region_base(hv_gic_config_t config, hv_ipa_t base);
hv_return_t hv_gic_config_set_msi_interrupt_range(hv_gic_config_t config, uint32_t base, uint32_t count);
hv_return_t hv_gic_set_redistributor_reg(hv_vcpu_t vcpu, hv_gic_redistributor_reg_t reg, uint64_t value);
#endif

typedef struct {
#define HVF(name) __typeof__(hv_##name) *name;
#include "hvf_symbols.h"
  hv_return_t (*vm_config_set_ipa_granule)(hv_vm_config_t, uint32_t);
} ant_hvf_api_t;

extern ant_hvf_api_t ant_hvf_api;
int ant_hvf_load_api(void);

typedef struct {
  unsigned char ident[ANT_HVF_ELF_IDENT_BYTES];
  uint16_t type;
  uint16_t machine;
  uint32_t version;
  uint64_t entry;
  uint64_t phoff;
  uint64_t shoff;
  uint32_t flags;
  uint16_t ehsize;
  uint16_t phentsize;
  uint16_t phnum;
  uint16_t shentsize;
  uint16_t shnum;
  uint16_t shstrndx;
} ant_elf64_ehdr_t;

typedef struct {
  uint32_t type;
  uint32_t flags;
  uint64_t offset;
  uint64_t vaddr;
  uint64_t paddr;
  uint64_t filesz;
  uint64_t memsz;
  uint64_t align;
} ant_elf64_phdr_t;

#endif
