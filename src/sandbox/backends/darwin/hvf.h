#pragma once

#include <compat.h> // IWYU pragma: keep

#include <Hypervisor/Hypervisor.h>
#include <stdint.h>

#if defined(__aarch64__)

#define ANT_HVF_GUEST_BASE 0x40000000ull
#define ANT_HVF_DTB_BASE 0x40000000ull
#define ANT_HVF_KERNEL_BASE 0x40400000ull
#define ANT_HVF_NANOS_KAS_OFFSET_SYMBOL 0x4052c1c0ull
#define ANT_HVF_UART_BASE 0x09000000ull
#define ANT_HVF_RTC_BASE 0x09010000ull
#define ANT_HVF_PCIE_MMIO_BASE 0x10000000ull
#define ANT_HVF_PCIE_PIO_BASE 0x3eff0000ull
#define ANT_HVF_PCIE_ECAM_BASE 0x3f000000ull
#define ANT_HVF_PCIE_ECAM_SIZE 0x01000000ull
#define ANT_HVF_VIRTIO_BLK_BAR 0x10041000ull
#define ANT_HVF_VIRTIO_BLK_SLOT 1u
#define ANT_HVF_VIRTIO_NET_BAR 0x10042000ull
#define ANT_HVF_VIRTIO_NET_SLOT 2u
#define ANT_HVF_VIRTIO_9P_BAR_BASE 0x10043000ull
#define ANT_HVF_VIRTIO_9P_SLOT_BASE 3u
#define ANT_HVF_VIRTIO_9P_MAX 8u
#define ANT_HVF_VIRTIO_VSOCK_BAR 0x1004b000ull
#define ANT_HVF_VIRTIO_VSOCK_SLOT 11u
#define ANT_HVF_GIC_DIST_BASE 0x08000000ull
#define ANT_HVF_GIC_REDIST_BASE 0x080a0000ull
#define ANT_HVF_GIC_MSI_BASE 0x08020000ull
#define ANT_HVF_GIC_MSI_SIZE 0x1000ull
#define ANT_HVF_GIC_MSI_VECTOR_BASE 48u
#define ANT_HVF_GIC_MSI_VECTOR_COUNT 64u
#define ANT_HVF_PAGE_SIZE 0x1000ull

#define ANT_HVF_GICR_ISPENDR0 66048u
#define ANT_HVF_GIC_EL1_VIRTUAL_TIMER 27u
#define ANT_HVF_GICM_TYPER 0x0008u
#define ANT_HVF_GICM_SET_SPI_NSR 0x0040u
#define ANT_HVF_GICM_IIDR 0x0fccu

#define ANT_HVF_GUEST_SHUTDOWN 1

#define ESR_EC_SHIFT 26
#define ESR_EC_WFX_TRAP 0x01u
#define ESR_EC_HVC64 0x16u
#define ESR_EC_DATA_ABORT 0x24u
#define ESR_EC_HLT 0x34u
#define ESR_DA_ISV (1u << 24)
#define ESR_DA_WNR (1u << 6)
#define ESR_DA_SAS_SHIFT 22
#define ESR_DA_SRT_SHIFT 16
#define ESR_HLT_IMM16_MASK 0xffffu

#define ANT_HVF_SYS_REG_CNTFRQ_EL0 ((hv_sys_reg_t)0xdf00)
#define ANT_HVF_SYS_REG_CNTVCT_EL0 ((hv_sys_reg_t)0xdf02)

#define ANT_HVF_BYTES_U16 2u
#define ANT_HVF_BYTES_U32 4u
#define ANT_HVF_ELF_IDENT_BYTES 16u
#define ANT_HVF_MAC_BYTES 6u

#define ANT_HVF_UART_DIAGNOSTIC_DEFAULT_BYTES (16u * 1024u)

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
