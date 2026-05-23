#pragma once

#include <compat.h> // IWYU pragma: keep

#include "sandbox/transport.h"
#include "sandbox/vm.h"
#include <Hypervisor/Hypervisor.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

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
#define ANT_HVF_DTB_MAX 0x20000u

#define ANT_VIRTIO_PCI_VENDOR 0x1af4u
#define ANT_VIRTIO_PCI_DEVICE_MODERN_BASE 0x1040u
#define ANT_VIRTIO_PCI_SUBDEVICE_NET 1u
#define ANT_VIRTIO_PCI_SUBDEVICE_BLOCK 2u
#define ANT_VIRTIO_PCI_SUBDEVICE_9P 9u
#define ANT_VIRTIO_PCI_SUBDEVICE_VSOCK 19u
#define ANT_VIRTIO_PCI_SUBVENDOR 0x1af4u
#define ANT_VIRTIO_F_VERSION_1 (1ull << 32)

#define ANT_HVF_VIRTIO_BAR_SIZE 0x1000u
#define ANT_HVF_VIRTIO_COMMON_CFG 0x000u
#define ANT_HVF_VIRTIO_COMMON_CFG_SIZE 0x038u
#define ANT_HVF_VIRTIO_NOTIFY_CFG 0x100u
#define ANT_HVF_VIRTIO_ISR_CFG 0x200u
#define ANT_HVF_VIRTIO_DEVICE_CFG 0x300u
#define ANT_HVF_VIRTIO_MSIX_TABLE 0x800u
#define ANT_HVF_VIRTIO_MSIX_PBA 0x900u
#define ANT_HVF_VIRTIO_MSIX_ENTRY_SIZE 16u
#define ANT_HVF_VIRTIO_MSIX_VECTOR_COUNT 4u
#define ANT_VIRTIO_MSI_NO_VECTOR 0xffffu

#define ANT_PCI_STATUS_CAP_LIST 0x0010u
#define ANT_PCI_COMMAND_IO 0x0001u
#define ANT_PCI_COMMAND_MEMORY 0x0002u
#define ANT_PCI_COMMAND_BUS_MASTER 0x0004u
#define ANT_PCI_CAP_VENDOR 0x09u
#define ANT_PCI_CAP_MSIX 0x11u
#define ANT_PCI_MSIX_ENABLE 0x8000u
#define ANT_PCI_MSIX_MASK_ALL 0x4000u
#define ANT_PCI_MSIX_ENTRY_MASKED 0x00000001u

#define ANT_VIRTIO_PCI_CAP_COMMON_CFG 1u
#define ANT_VIRTIO_PCI_CAP_NOTIFY_CFG 2u
#define ANT_VIRTIO_PCI_CAP_ISR_CFG 3u
#define ANT_VIRTIO_PCI_CAP_DEVICE_CFG 4u
#define ANT_VIRTIO_PCI_CAP_COMMON_POS 0x40u
#define ANT_VIRTIO_PCI_CAP_NOTIFY_POS 0x50u
#define ANT_VIRTIO_PCI_CAP_ISR_POS 0x70u
#define ANT_VIRTIO_PCI_CAP_DEVICE_POS 0x80u
#define ANT_VIRTIO_PCI_CAP_MSIX_POS 0x90u

#define ANT_VIRTIO_BLK_T_IN 0u
#define ANT_VIRTIO_BLK_T_OUT 1u
#define ANT_VIRTIO_BLK_T_FLUSH 4u
#define ANT_VIRTIO_BLK_S_OK 0u
#define ANT_VIRTIO_BLK_S_IOERR 1u
#define ANT_VIRTIO_BLK_QUEUE_SIZE 8u

#define ANT_VIRTIO_NET_F_MAC 0x20u
#define ANT_VIRTIO_NET_HDR_LEN 12u
#define ANT_HVF_NET_MAX_PACKET 2048u
#define ANT_HVF_NET_RX_BACKLOG 64u
#define ANT_HVF_NET_TCP_MAX 256u
#define ANT_HVF_NET_HOST_RING 1024u
#define ANT_VIRTIO_NET_RX_QUEUE 0u
#define ANT_VIRTIO_NET_TX_QUEUE 1u
#define ANT_VIRTIO_NET_QUEUE_COUNT 2u
#define ANT_VIRTIO_NET_QUEUE_SIZE 256u
#define ANT_VIRTIO_9P_F_MOUNT_TAG 0x1u
#define ANT_VIRTIO_9P_QUEUE_SIZE 32u
#define ANT_HVF_9P_MSIZE 8192u
#define ANT_HVF_9P_IOUNIT (ANT_HVF_9P_MSIZE - 11u)
#define ANT_VIRTIO_VSOCK_F_STREAM 0x1u
#define ANT_VIRTIO_VSOCK_QUEUE_COUNT 3u
#define ANT_VIRTIO_VSOCK_QUEUE_SIZE 32u
#define ANT_VIRTIO_VSOCK_TYPE_STREAM 1u
#define ANT_VIRTIO_VSOCK_OP_REQUEST 1u
#define ANT_VIRTIO_VSOCK_OP_RESPONSE 2u
#define ANT_VIRTIO_VSOCK_OP_RST 3u
#define ANT_VIRTIO_VSOCK_OP_SHUTDOWN 4u
#define ANT_VIRTIO_VSOCK_OP_RW 5u
#define ANT_VIRTIO_VSOCK_OP_CREDIT_UPDATE 6u
#define ANT_VIRTIO_VSOCK_OP_CREDIT_REQUEST 7u
#define ANT_HVF_VSOCK_GUEST_CID 3ull
#define ANT_HVF_VSOCK_HOST_CID ANT_SANDBOX_TRANSPORT_VSOCK_HOST_CID
#define ANT_HVF_VSOCK_HOST_PORT ANT_SANDBOX_TRANSPORT_VSOCK_PORT
#define ANT_HVF_VSOCK_BUF_ALLOC 65536u
#define ANT_HVF_UART_DIAGNOSTIC_DEFAULT_BYTES (16u * 1024u)
#define ANT_HVF_9P_INITIAL_FID_COUNT 256u
#define ANT_HVF_9P_PATH_MAX 1024u

#define P9_RLERROR 7u
#define P9_TSTATFS 8u
#define P9_RSTATFS 9u
#define P9_TLOPEN 12u
#define P9_RLOPEN 13u
#define P9_TGETATTR 24u
#define P9_RGETATTR 25u
#define P9_TREADDIR 40u
#define P9_RREADDIR 41u
#define P9_TVERSION 100u
#define P9_RVERSION 101u
#define P9_TATTACH 104u
#define P9_RATTACH 105u
#define P9_TWALK 110u
#define P9_RWALK 111u
#define P9_TREAD 116u
#define P9_RREAD 117u
#define P9_TCLUNK 120u
#define P9_RCLUNK 121u
#define P9_QTDIR 0x80u
#define P9_GETATTR_BASIC 0x7ffull
#define ANT_HVF_GICR_ISPENDR0 66048u
#define ANT_HVF_GIC_EL1_VIRTUAL_TIMER 27u
#define ANT_HVF_GICM_TYPER 0x0008u
#define ANT_HVF_GICM_SET_SPI_NSR 0x0040u
#define ANT_HVF_GICM_IIDR 0x0fccu

#define ANT_VRING_DESC_F_NEXT 1u
#define ANT_VRING_DESC_F_WRITE 2u

#define ANT_HVF_GUEST_SHUTDOWN 1

#define FDT_MAGIC 0xd00dfeedu
#define FDT_BEGIN_NODE 1u
#define FDT_END_NODE 2u
#define FDT_PROP 3u
#define FDT_END 9u

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
  unsigned char structure[8192];
  unsigned char strings[2048];
  size_t structure_len;
  size_t strings_len;
} ant_fdt_t;

typedef struct {
  unsigned char ident[16];
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

typedef struct {
  bool active;
  uint32_t fid;
  char path[ANT_HVF_9P_PATH_MAX];
} ant_hvf_9p_fid_t;

typedef enum {
  ANT_HVF_VIRTIO_KIND_BLOCK,
  ANT_HVF_VIRTIO_KIND_NET,
  ANT_HVF_VIRTIO_KIND_9P,
  ANT_HVF_VIRTIO_KIND_VSOCK,
} ant_hvf_virtio_kind_t;

typedef struct {
  uint64_t desc;
  uint64_t avail;
  uint64_t used;
  uint16_t size;
  uint16_t last_avail;
  uint16_t msix_vector;
  uint16_t notify_off;
  bool enabled;
} ant_hvf_virtio_queue_t;

typedef struct {
  uint32_t msg_addr_lo;
  uint32_t msg_addr_hi;
  uint32_t msg_data;
  uint32_t vector_control;
} ant_hvf_msix_entry_t;

typedef struct {
  ant_hvf_virtio_kind_t kind;
  const char *name;
  uint16_t virtio_id;
  uint16_t subsystem_id;
  uint8_t slot;
  uint8_t class_code;
  uint8_t subclass;
  uint32_t bar0;
  uint32_t pci_command;
  uint16_t msix_control;
  uint32_t msix_pba;
  uint64_t device_features;
  uint64_t driver_features;
  uint32_t device_feature_select;
  uint32_t driver_feature_select;
  uint16_t config_msix_vector;
  uint16_t queue_sel;
  uint16_t queue_count;
  uint8_t status;
  uint8_t isr;
  uint8_t config_generation;
  uint16_t device_config_len;
  ant_hvf_virtio_queue_t queues[ANT_VIRTIO_VSOCK_QUEUE_COUNT];
  ant_hvf_msix_entry_t msix[ANT_HVF_VIRTIO_MSIX_VECTOR_COUNT];
} ant_hvf_virtio_device_t;

typedef struct {
  ant_hvf_virtio_device_t virtio;
  const char *root;
  const char *tag;
  ant_hvf_9p_fid_t *fids;
  size_t fid_count;
  size_t fid_capacity;
} ant_hvf_9p_device_t;

typedef struct {
  uint64_t src_cid;
  uint64_t dst_cid;
  uint32_t src_port;
  uint32_t dst_port;
  uint32_t len;
  uint16_t type;
  uint16_t op;
  uint32_t flags;
  uint32_t buf_alloc;
  uint32_t fwd_cnt;
} __attribute__((packed)) ant_virtio_vsock_hdr_t;

typedef struct {
  ant_hvf_virtio_device_t virtio;
  const void *request_data;
  size_t request_len;
  bool connected;
  bool request_sent;
  uint32_t peer_port;
  uint32_t fwd_cnt;
  bool exit_received;
  int exit_code;
  uint32_t capabilities;
  unsigned char *rx_stream;
  size_t rx_stream_len;
  size_t rx_stream_cap;
} ant_hvf_vsock_device_t;

typedef struct {
  uint32_t len;
  unsigned char data[ANT_HVF_NET_MAX_PACKET];
} ant_hvf_net_packet_t;

typedef struct ant_hvf_nat ant_hvf_nat_t;

typedef struct {
  uint8_t *data;
  size_t len;
  size_t cap;
  size_t limit;
  bool truncated;
} ant_hvf_uart_capture_t;

typedef struct {
  void *host_mem;
  size_t mem_size;
  uint64_t kernel_entry;
  int image_fd;
  uint64_t image_sectors;
  hv_vcpu_t vcpu;
  hv_vcpu_exit_t *vcpu_exit;
  ant_hvf_virtio_device_t blk;
  ant_hvf_virtio_device_t net;
  bool net_enabled;
  bool net_started;
  ant_hvf_nat_t *net_nat;
  pthread_mutex_t net_lock;
  bool net_lock_init;
  bool net_rx_wake;
  uint8_t net_mac[6];
  uint8_t net_guest_mac[6];
  bool net_guest_mac_seen;
  uint32_t net_max_packet_size;
  uint32_t net_rx_head;
  uint32_t net_rx_count;
  ant_hvf_net_packet_t net_rx_packets[ANT_HVF_NET_RX_BACKLOG];
  const ant_sandbox_port_forward_t *net_forwards;
  size_t net_forward_count;
  ant_hvf_9p_device_t p9[ANT_HVF_VIRTIO_9P_MAX];
  size_t p9_count;
  ant_hvf_vsock_device_t vsock;
  ant_hvf_uart_capture_t uart;
  bool trace;
  bool timed_out;
  bool gic_msi_enabled;
  uint32_t gic_msi_base;
  uint32_t gic_msi_count;
  uint64_t cntfrq;
  uint32_t last_exit_reason;
  uint64_t last_exit_pc;
  uint64_t last_exit_esr;
  uint64_t last_exit_ipa;
  uint64_t last_exit_va;
} ant_hvf_vm_t;

typedef struct {
  ant_hvf_vm_t *vm;
  unsigned int timeout_ms;
} ant_hvf_timeout_t;

typedef struct {
  uint64_t addr;
  uint32_t len;
} ant_hvf_iov_t;

typedef struct {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
} ant_vring_desc_t;

typedef struct {
  uint32_t type;
  uint32_t reserved;
  uint64_t sector;
} ant_virtio_blk_req_t;

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


void ant_hvf_uart_discard(ant_hvf_vm_t *vm);
void ant_hvf_uart_report_panic(ant_hvf_vm_t *vm);
bool ant_hvf_uart_has_panic(ant_hvf_vm_t *vm);
void ant_hvf_uart_put(ant_hvf_vm_t *vm, uint8_t byte);
uint32_t ant_bswap32(uint32_t x);
uint64_t ant_bswap64(uint64_t x);
size_t ant_align4(size_t n);
size_t ant_align_page(size_t n);
int ant_hvf_check(hv_return_t ret, const char *op);
int ant_hvf_check_file(const char *kind, const char *path, off_t *size_out);
uint16_t ant_hvf_load16(const unsigned char *p);
uint32_t ant_hvf_load32(const unsigned char *p);
uint64_t ant_hvf_load64(const unsigned char *p);
void ant_hvf_store16(unsigned char *p, uint16_t value);
void ant_hvf_store32(unsigned char *p, uint32_t value);
void ant_hvf_store64(unsigned char *p, uint64_t value);
void ant_hvf_vsock_store_hdr(unsigned char *out, const ant_virtio_vsock_hdr_t *hdr);
ant_virtio_vsock_hdr_t ant_hvf_vsock_load_hdr(const unsigned char *raw);
void *ant_hvf_guest_ptr(ant_hvf_vm_t *vm, uint64_t guest_addr, size_t len);
int ant_hvf_guest_read(ant_hvf_vm_t *vm, uint64_t guest_addr, void *out, size_t len);
int ant_hvf_guest_write(ant_hvf_vm_t *vm, uint64_t guest_addr, const void *src, size_t len);
int ant_read_all(int fd, void *buf, size_t len, off_t off);
int ant_hvf_load_kernel(ant_hvf_vm_t *vm, const char *path);
int ant_fdt_reserve(ant_fdt_t *fdt, size_t len, unsigned char **out);
int ant_fdt_u32(ant_fdt_t *fdt, uint32_t val);
int ant_fdt_string_offset(ant_fdt_t *fdt, const char *name);
int ant_fdt_begin(ant_fdt_t *fdt, const char *name);
int ant_fdt_end(ant_fdt_t *fdt);
int ant_fdt_prop(ant_fdt_t *fdt, const char *name, const void *data, size_t len);
int ant_fdt_prop_null(ant_fdt_t *fdt, const char *name);
int ant_fdt_prop_string(ant_fdt_t *fdt, const char *name, const char *value);
int ant_fdt_prop_u32(ant_fdt_t *fdt, const char *name, uint32_t value);
int ant_fdt_prop_cells(ant_fdt_t *fdt, const char *name, const uint32_t *cells, size_t count);
int ant_fdt_prop_reg64(ant_fdt_t *fdt, const char *name, const uint64_t *cells, size_t count);
int ant_hvf_build_dtb(ant_hvf_vm_t *vm);
void *ant_hvf_sym(const char *name);
int ant_hvf_load_gic_api(void);
int ant_hvf_create_gic(ant_hvf_vm_t *vm);
int ant_hvf_set_reg(hv_vcpu_t vcpu, hv_reg_t reg, uint64_t value, const char *name);
uint64_t ant_hvf_host_cntfrq(void);
uint64_t ant_hvf_host_cntvct(void);
int ant_hvf_init_vcpu(ant_hvf_vm_t *vm);
int ant_hvf_handle_wfx(ant_hvf_vm_t *vm);
int ant_hvf_sync_vtimer(ant_hvf_vm_t *vm);
int ant_hvf_raise_vtimer(ant_hvf_vm_t *vm, const char *where);
uint64_t ant_hvf_select_width(uint64_t value, unsigned offset, unsigned size);
void ant_hvf_assign_width(uint32_t *target, unsigned offset, unsigned size, uint64_t value);
void ant_hvf_assign_width16(uint16_t *target, unsigned offset, unsigned size, uint64_t value);
void ant_hvf_assign_width64(uint64_t *target, unsigned offset, unsigned size, uint64_t value);
bool ant_hvf_pci_addr(uint64_t addr, unsigned *bus, unsigned *slot, unsigned *fn, unsigned *reg);
bool ant_hvf_is_virtio_slot(unsigned bus, unsigned slot, unsigned fn);
ant_hvf_virtio_device_t *ant_hvf_virtio_for_slot(ant_hvf_vm_t *vm, unsigned slot);
ant_hvf_9p_device_t *ant_hvf_p9_for_slot(ant_hvf_vm_t *vm, unsigned slot);
ant_hvf_9p_device_t *ant_hvf_p9_for_virtio(ant_hvf_vm_t *vm, ant_hvf_virtio_device_t *dev);
void ant_hvf_cfg_store16(unsigned char *cfg, unsigned off, uint16_t value);
void ant_hvf_cfg_store32(unsigned char *cfg, unsigned off, uint32_t value);
void ant_hvf_virtio_cfg_cap(unsigned char *cfg,
                                   unsigned pos,
                                   unsigned next,
                                   uint8_t type,
                                   uint32_t offset,
                                   uint32_t length,
                                   bool notify);
void ant_hvf_virtio_build_config(ant_hvf_virtio_device_t *dev, unsigned char cfg[256]);
uint32_t ant_hvf_pci_config_read32(ant_hvf_vm_t *vm, unsigned bus, unsigned slot, unsigned fn, unsigned reg);
void ant_hvf_pci_config_write(ant_hvf_vm_t *vm,
                                     unsigned bus,
                                     unsigned slot,
                                     unsigned fn,
                                     unsigned reg,
                                     unsigned size,
                                     uint64_t value);
void ant_hvf_virtio_init(ant_hvf_virtio_device_t *dev,
                                ant_hvf_virtio_kind_t kind,
                                const char *name,
                                uint16_t virtio_id,
                                uint16_t subsystem_id,
                                uint8_t slot,
                                uint8_t class_code,
                                uint8_t subclass,
                                uint32_t bar0,
                                uint64_t features,
                                uint16_t queue_count,
                                uint16_t queue_size,
                                uint16_t device_config_len);
ant_hvf_virtio_device_t *ant_hvf_virtio_for_bar(ant_hvf_vm_t *vm, uint64_t addr);
void ant_hvf_virtio_reset(ant_hvf_virtio_device_t *dev);
ant_hvf_virtio_queue_t *ant_hvf_virtio_selected_queue(ant_hvf_virtio_device_t *dev);
void ant_hvf_virtio_common_bytes(ant_hvf_virtio_device_t *dev, unsigned char out[ANT_HVF_VIRTIO_COMMON_CFG_SIZE]);
bool ant_hvf_virtio_device_config_read(ant_hvf_vm_t *vm,
                                              ant_hvf_virtio_device_t *dev,
                                              uint64_t off,
                                              unsigned size,
                                              uint64_t *value);
bool ant_hvf_virtio_msix_enabled(ant_hvf_virtio_device_t *dev);
bool ant_hvf_virtio_msix_masked(ant_hvf_virtio_device_t *dev, unsigned vector);
int ant_hvf_virtio_msix_notify(ant_hvf_vm_t *vm, ant_hvf_virtio_device_t *dev, uint16_t vector);
int ant_hvf_virtio_interrupt(ant_hvf_vm_t *vm, ant_hvf_virtio_device_t *dev, unsigned queue);
bool ant_hvf_virtio_msix_read(ant_hvf_virtio_device_t *dev,
                                     uint64_t off,
                                     unsigned size,
                                     uint64_t *value);
bool ant_hvf_virtio_msix_write(ant_hvf_vm_t *vm,
                                      ant_hvf_virtio_device_t *dev,
                                      uint64_t off,
                                      unsigned size,
                                      uint64_t value);
bool ant_hvf_virtio_common_read(ant_hvf_vm_t *vm,
                                       ant_hvf_virtio_device_t *dev,
                                       uint64_t off,
                                       unsigned size,
                                       uint64_t *value);
bool ant_hvf_virtio_common_write(ant_hvf_vm_t *vm,
                                        ant_hvf_virtio_device_t *dev,
                                        uint64_t off,
                                        unsigned size,
                                        uint64_t value);
int ant_hvf_vring_read_desc(ant_hvf_vm_t *vm, uint64_t desc_base, uint16_t index, ant_vring_desc_t *out);
int ant_hvf_disk_read(ant_hvf_vm_t *vm, uint64_t sector, uint64_t guest_addr, uint32_t len);
int ant_hvf_disk_write(ant_hvf_vm_t *vm, uint64_t sector, uint64_t guest_addr, uint32_t len);
int ant_hvf_virtio_blk_request(ant_hvf_vm_t *vm,
                                      uint64_t desc_base,
                                      uint16_t head,
                                      uint32_t *used_len);
int ant_hvf_vring_add_used(ant_hvf_vm_t *vm,
                                  uint64_t used_base,
                                  unsigned queue_size,
                                  uint16_t head,
                                  uint32_t used_len);
int ant_hvf_vring_read_chain(ant_hvf_vm_t *vm,
                                    uint64_t desc_base,
                                    uint16_t head,
                                    unsigned queue_size,
                                    unsigned char *out,
                                    uint32_t out_cap,
                                    uint32_t *out_len);
int ant_hvf_vring_write_chain(ant_hvf_vm_t *vm,
                                     uint64_t desc_base,
                                     uint16_t head,
                                     unsigned queue_size,
                                     const unsigned char *data,
                                     uint32_t len,
                                     uint32_t *used_len);
void ant_hvf_net_note_rx(ant_hvf_vm_t *vm);
int ant_hvf_net_start(ant_hvf_vm_t *vm);
void ant_hvf_net_stop(ant_hvf_vm_t *vm);
int ant_hvf_virtio_blk_notify(ant_hvf_vm_t *vm, ant_hvf_virtio_device_t *dev);
int ant_hvf_virtio_net_tx(ant_hvf_vm_t *vm,
                                 uint64_t desc_base,
                                 uint16_t head,
                                 uint32_t *used_len);
int ant_hvf_virtio_net_drain_rx(ant_hvf_vm_t *vm);
int ant_hvf_virtio_net_notify(ant_hvf_vm_t *vm, ant_hvf_virtio_device_t *dev, unsigned queue);
int ant_hvf_vsock_write_iov(ant_hvf_vm_t *vm,
                                   uint64_t desc_base,
                                   uint16_t head,
                                   const unsigned char *data,
                                   uint32_t len,
                                   uint32_t *used_len);
int ant_hvf_vsock_send_packet(ant_hvf_vm_t *vm,
                                     uint16_t op,
                                     const void *payload,
                                     uint32_t payload_len);
int ant_hvf_vsock_maybe_send_request(ant_hvf_vm_t *vm);
int ant_hvf_vsock_read_tx_packet(ant_hvf_vm_t *vm,
                                        uint64_t desc_base,
                                        uint16_t head,
                                        ant_virtio_vsock_hdr_t *hdr,
                                        unsigned char **payload,
                                        uint32_t *used_len);
int ant_hvf_virtio_vsock_notify(ant_hvf_vm_t *vm, unsigned queue);
uint64_t ant_hvf_9p_hash(const char *path);
void ant_hvf_9p_qid(unsigned char *out, bool dir, const char *path);
void ant_hvf_9p_hdr(unsigned char *out, uint32_t size, uint8_t type, uint16_t tag);
uint32_t ant_hvf_9p_append_dirent(unsigned char *out,
                                         uint32_t off,
                                         uint32_t cap,
                                         const char *name,
                                         const char *qid_path,
                                         bool is_dir,
                                         uint64_t next_offset,
                                         uint8_t dtype);
uint32_t ant_hvf_9p_error(unsigned char *out, uint16_t tag, uint32_t ecode);
bool ant_hvf_9p_path_bad(const char *path);
int ant_hvf_9p_host_path(ant_hvf_9p_device_t *dev, const char *rel, char *out, size_t out_len);
int ant_hvf_9p_stat(ant_hvf_9p_device_t *dev, const char *rel, struct stat *st);
uint8_t ant_hvf_9p_dtype_from_mode(mode_t mode);
bool ant_hvf_9p_dtype_is_dir(uint8_t dtype);
int ant_hvf_9p_dirent_type(ant_hvf_9p_device_t *dev,
                                  const char *rel,
                                  uint8_t host_dtype,
                                  uint8_t *dtype,
                                  bool *is_dir);
int ant_hvf_9p_walk(ant_hvf_9p_device_t *dev, const char *base, const char *name, char *out, size_t out_len);
bool ant_hvf_9p_trace_paths(void);
int ant_hvf_9p_read_chain(ant_hvf_vm_t *vm,
                                 uint64_t desc_base,
                                 uint16_t head,
                                 unsigned queue_size,
                                 unsigned char *req,
                                 size_t req_cap,
                                 size_t *req_len,
                                 ant_hvf_iov_t *writes,
                                 size_t writes_cap,
                                 size_t *writes_len);
int ant_hvf_9p_write_response(ant_hvf_vm_t *vm,
                                     const ant_hvf_iov_t *writes,
                                     size_t writes_len,
                                     const unsigned char *resp,
                                     uint32_t resp_len);
ant_hvf_9p_fid_t *ant_hvf_9p_fid(ant_hvf_9p_device_t *dev, uint32_t fid, bool create);
uint32_t ant_hvf_9p_handle(ant_hvf_9p_device_t *dev,
                                  const unsigned char *req,
                                  size_t req_len,
                                  unsigned char *resp,
                                  size_t resp_cap);
int ant_hvf_virtio_9p_notify(ant_hvf_vm_t *vm, ant_hvf_9p_device_t *dev);
bool ant_hvf_pci_mmio_read(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t *value);
bool ant_hvf_pci_mmio_write(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t value);
bool ant_hvf_gic_msi_read(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t *value);
bool ant_hvf_gic_msi_write(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t value);
bool ant_hvf_mmio_read(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t *value);
bool ant_hvf_mmio_write(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t value);
int ant_hvf_advance_pc(hv_vcpu_t vcpu);
int ant_hvf_handle_mmio(ant_hvf_vm_t *vm, hv_vcpu_exit_exception_t *ex);

#endif
