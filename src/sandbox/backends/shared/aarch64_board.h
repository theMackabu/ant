#pragma once

#include <compat.h> // IWYU pragma: keep
#include "ant_sandbox_devices.h" // IWYU pragma: keep

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__aarch64__)

#define ANT_HVF_ELF_MACHINE_NAME "aarch64"

static constexpr uint64_t ANT_HVF_GUEST_BASE = 0x40000000;
static constexpr uint64_t ANT_HVF_DTB_BASE = 0x40000000;
static constexpr uint64_t ANT_HVF_KERNEL_BASE = 0x40400000;
static constexpr uint64_t ANT_HVF_UART_BASE = 0x09000000;
static constexpr uint64_t ANT_HVF_RTC_BASE = 0x09010000;
static constexpr uint64_t ANT_HVF_PCIE_PIO_BASE = 0x3eff0000;
static constexpr uint64_t ANT_HVF_PCIE_ECAM_BASE = 0x3f000000;
static constexpr uint64_t ANT_HVF_PCIE_ECAM_SIZE = 0x01000000;

static constexpr uint32_t ANT_HVF_VIRTIO_BLK_BAR = 0x10041000;
static constexpr uint32_t ANT_HVF_VIRTIO_NET_BAR = 0x10042000;
static constexpr uint32_t ANT_HVF_VIRTIO_9P_BAR_BASE = 0x10043000;
static constexpr uint32_t ANT_HVF_VIRTIO_VSOCK_BAR = 0x1004b000;
static constexpr uint32_t ANT_HVF_VIRTIO_RNG_BAR = 0x1004c000;

enum: uint8_t {
  ANT_HVF_VIRTIO_BLK_SLOT = 1,
  ANT_HVF_VIRTIO_NET_SLOT = 2,
  ANT_HVF_VIRTIO_9P_SLOT_BASE = 3,
  ANT_HVF_VIRTIO_VSOCK_SLOT = 11,
  ANT_HVF_VIRTIO_RNG_SLOT = 12,
};

static constexpr uint64_t ANT_HVF_GIC_DIST_BASE = 0x08000000;
static constexpr uint64_t ANT_HVF_GIC_DIST_SIZE = 0x10000;
static constexpr uint64_t ANT_HVF_GIC_V2_DIST_SIZE = 0x1000;
static constexpr uint64_t ANT_HVF_GIC_CPU_BASE = 0x08010000;
static constexpr uint64_t ANT_HVF_GIC_CPU_SIZE = 0x2000;
static constexpr uint64_t ANT_HVF_GIC_REDIST_BASE = 0x080a0000;
static constexpr uint64_t ANT_HVF_GIC_REDIST_SIZE = 0x200000;
static constexpr uint64_t ANT_HVF_GIC_MSI_BASE = 0x08020000;
static constexpr uint64_t ANT_HVF_GIC_MSI_SIZE = 0x1000;
static constexpr uint32_t ANT_HVF_GIC_MSI_VECTOR_BASE = 48;
static constexpr uint32_t ANT_HVF_GIC_MSI_VECTOR_COUNT = 64;

enum: uint32_t {
  ANT_HVF_GICM_TYPER = 0x0008,
  ANT_HVF_GICM_SET_SPI_NSR = 0x0040,
  ANT_HVF_GICM_IIDR = 0x0fcc,
};

static constexpr size_t ANT_HVF_DTB_MAX = 0x20000;
static constexpr size_t ANT_FDT_STRUCTURE_BYTES = 8192;
static constexpr size_t ANT_FDT_STRINGS_BYTES = 2048;
static constexpr size_t ANT_FDT_MAX_PROP_CELLS_32 = 32;
static constexpr size_t ANT_FDT_MAX_PROP_CELLS_64 = 16;

static constexpr uint32_t FDT_MAGIC = 0xd00dfeed;
static constexpr uint16_t ANT_HVF_ELF_MACHINE = 183;

enum: uint32_t {
  FDT_BEGIN_NODE = 1,
  FDT_END_NODE = 2,
  FDT_PROP = 3,
  FDT_END = 9,
};

typedef struct {
  uint32_t magic;
  uint32_t totalsize;
  uint32_t off_dt_struct;
  uint32_t off_dt_strings;
  uint32_t off_mem_rsvmap;
  uint32_t version;
  uint32_t last_comp_version;
  uint32_t boot_cpuid_phys;
  uint32_t size_dt_strings;
  uint32_t size_dt_struct;
} ant_fdt_header_t;

typedef struct {
  unsigned char *structure;
  unsigned char *strings;
  size_t structure_len;
  size_t structure_cap;
  size_t strings_len;
  size_t strings_cap;
} ant_fdt_t;

void ant_fdt_free(ant_fdt_t *fdt);
uint64_t ant_hvf_host_cntfrq(void);

int ant_fdt_init(ant_fdt_t *fdt);
int ant_fdt_end(ant_fdt_t *fdt);
int ant_fdt_reserve(ant_fdt_t *fdt, size_t len, unsigned char **out);
int ant_fdt_u32(ant_fdt_t *fdt, uint32_t val);
int ant_fdt_string_offset(ant_fdt_t *fdt, const char *name);
int ant_fdt_begin(ant_fdt_t *fdt, const char *name);
int ant_fdt_prop(ant_fdt_t *fdt, const char *name, const void *data, size_t len);
int ant_fdt_prop_null(ant_fdt_t *fdt, const char *name);
int ant_fdt_prop_string(ant_fdt_t *fdt, const char *name, const char *value);
int ant_fdt_prop_u32(ant_fdt_t *fdt, const char *name, uint32_t value);
int ant_fdt_prop_cells(ant_fdt_t *fdt, const char *name, const uint32_t *cells, size_t count);
int ant_fdt_prop_reg64(ant_fdt_t *fdt, const char *name, const uint64_t *cells, size_t count);
int ant_hvf_build_dtb(ant_hvf_vm_t *vm, int gic_version);

bool ant_hvf_gic_msi_read(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t *value);
bool ant_hvf_gic_msi_write(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t value);
bool ant_hvf_mmio_read(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t *value);
bool ant_hvf_mmio_write(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t value);

#endif
