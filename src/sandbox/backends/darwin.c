#include <compat.h> // IWYU pragma: keep

#include "sandbox/transport.h"
#include "sandbox/vm.h"

#include <Hypervisor/Hypervisor.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
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
#define ANT_HVF_VIRTIO_BLK_BAR_SIZE 0x1000u
#define ANT_HVF_VIRTIO_BLK_SLOT 1u
#define ANT_HVF_VIRTIO_BLK_IRQ 36u
#define ANT_HVF_VIRTIO_NET_BAR 0x10042000ull
#define ANT_HVF_VIRTIO_NET_BAR_SIZE 0x1000u
#define ANT_HVF_VIRTIO_NET_SLOT 2u
#define ANT_HVF_VIRTIO_NET_IRQ 37u
#define ANT_HVF_VIRTIO_9P0_BAR 0x10043000ull
#define ANT_HVF_VIRTIO_9P_BAR_SIZE 0x1000u
#define ANT_HVF_VIRTIO_9P0_SLOT 3u
#define ANT_HVF_VIRTIO_9P0_IRQ 38u
#define ANT_HVF_VIRTIO_VSOCK_BAR 0x10045000ull
#define ANT_HVF_VIRTIO_VSOCK_BAR_SIZE 0x1000u
#define ANT_HVF_VIRTIO_VSOCK_SLOT 6u
#define ANT_HVF_VIRTIO_VSOCK_IRQ 37u
#define ANT_HVF_GIC_DIST_BASE 0x08000000ull
#define ANT_HVF_GIC_REDIST_BASE 0x080a0000ull
#define ANT_HVF_GIC_MSI_BASE 0x08020000ull
#define ANT_HVF_PAGE_SIZE 0x1000ull
#define ANT_HVF_DTB_MAX 0x20000u

#define ANT_VIRTIO_PCI_VENDOR 0x1af4u
#define ANT_VIRTIO_PCI_DEVICE_LEGACY_BLOCK 0x1001u
#define ANT_VIRTIO_PCI_DEVICE_LEGACY_NET 0x1000u
#define ANT_VIRTIO_PCI_DEVICE_LEGACY_9P 0x1008u
#define ANT_VIRTIO_PCI_DEVICE_LEGACY_VSOCK 0x1013u
#define ANT_VIRTIO_PCI_SUBDEVICE_NET 1u
#define ANT_VIRTIO_PCI_SUBDEVICE_BLOCK 2u
#define ANT_VIRTIO_PCI_SUBDEVICE_9P 9u
#define ANT_VIRTIO_PCI_SUBDEVICE_VSOCK 19u
#define ANT_VIRTIO_PCI_SUBVENDOR 0x1af4u
#define ANT_VIRTIO_PCI_ABI_VERSION 0u

#define ANT_VIRTIO_PCI_HOST_FEATURES 0u
#define ANT_VIRTIO_PCI_GUEST_FEATURES 4u
#define ANT_VIRTIO_PCI_QUEUE_PFN 8u
#define ANT_VIRTIO_PCI_QUEUE_NUM 12u
#define ANT_VIRTIO_PCI_QUEUE_SEL 14u
#define ANT_VIRTIO_PCI_QUEUE_NOTIFY 16u
#define ANT_VIRTIO_PCI_STATUS 18u
#define ANT_VIRTIO_PCI_ISR 19u
#define ANT_VIRTIO_PCI_CONFIG 20u

#define ANT_VIRTIO_BLK_T_IN 0u
#define ANT_VIRTIO_BLK_T_OUT 1u
#define ANT_VIRTIO_BLK_T_FLUSH 4u
#define ANT_VIRTIO_BLK_S_OK 0u
#define ANT_VIRTIO_BLK_S_IOERR 1u
#define ANT_VIRTIO_BLK_QUEUE_SIZE 8u

#define ANT_VIRTIO_NET_F_MAC 0x20u
#define ANT_VIRTIO_NET_RX_QUEUE 0u
#define ANT_VIRTIO_NET_TX_QUEUE 1u
#define ANT_VIRTIO_NET_QUEUE_COUNT 2u
#define ANT_VIRTIO_NET_QUEUE_SIZE 256u
#define ANT_VIRTIO_9P_F_MOUNT_TAG 0x1u
#define ANT_VIRTIO_9P_QUEUE_SIZE 32u
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
#define ANT_HVF_9P_FID_COUNT 256u
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
  char path[ANT_HVF_9P_PATH_MAX];
} ant_hvf_9p_fid_t;

typedef struct {
  const char *root;
  const char *tag;
  uint32_t guest_features;
  uint16_t queue_sel;
  uint32_t queue_pfn;
  uint16_t last_avail;
  uint8_t status;
  uint8_t isr;
  uint32_t bar0;
  uint8_t irq;
  ant_hvf_9p_fid_t fids[ANT_HVF_9P_FID_COUNT];
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
  const char *request_json;
  uint32_t guest_features;
  uint16_t queue_sel;
  uint32_t queue_pfn[ANT_VIRTIO_VSOCK_QUEUE_COUNT];
  uint16_t last_avail[ANT_VIRTIO_VSOCK_QUEUE_COUNT];
  uint8_t status;
  uint8_t isr;
  uint32_t bar0;
  bool connected;
  bool request_sent;
  uint32_t peer_port;
  uint32_t fwd_cnt;
} ant_hvf_vsock_device_t;

typedef struct {
  void *host_mem;
  size_t mem_size;
  uint64_t kernel_entry;
  int image_fd;
  uint64_t image_sectors;
  hv_vcpu_t vcpu;
  hv_vcpu_exit_t *vcpu_exit;
  uint32_t blk_guest_features;
  uint16_t blk_queue_sel;
  uint32_t blk_queue_pfn;
  uint16_t blk_last_avail;
  uint8_t blk_status;
  uint8_t blk_isr;
  uint32_t blk_bar0;
  uint32_t net_guest_features;
  uint16_t net_queue_sel;
  uint32_t net_queue_pfn[ANT_VIRTIO_NET_QUEUE_COUNT];
  uint16_t net_last_avail[ANT_VIRTIO_NET_QUEUE_COUNT];
  uint8_t net_status;
  uint8_t net_isr;
  uint32_t net_bar0;
  bool net_enabled;
  ant_hvf_9p_device_t p9[1];
  ant_hvf_vsock_device_t vsock;
  bool trace;
  bool timed_out;
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
  ant_hvf_vm_t *vm;
  unsigned int interval_ms;
  volatile bool stop;
} ant_hvf_wake_t;

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
typedef uint16_t ant_hvf_gic_redistributor_reg_t;
typedef hv_return_t (*ant_hvf_gic_create_fn)(ant_hvf_gic_config_t);
typedef ant_hvf_gic_config_t (*ant_hvf_gic_config_create_fn)(void);
typedef hv_return_t (*ant_hvf_gic_config_set_base_fn)(ant_hvf_gic_config_t, hv_ipa_t);
typedef hv_return_t (*ant_hvf_gic_config_set_range_fn)(ant_hvf_gic_config_t, uint32_t, uint32_t);
typedef hv_return_t (*ant_hvf_gic_set_spi_fn)(uint32_t, bool);
typedef hv_return_t (*ant_hvf_gic_set_redistributor_reg_fn)(hv_vcpu_t, ant_hvf_gic_redistributor_reg_t, uint64_t);

typedef struct {
  ant_hvf_gic_create_fn create;
  ant_hvf_gic_config_create_fn config_create;
  ant_hvf_gic_config_set_base_fn set_distributor_base;
  ant_hvf_gic_config_set_base_fn set_redistributor_base;
  ant_hvf_gic_config_set_base_fn set_msi_region_base;
  ant_hvf_gic_config_set_range_fn set_msi_interrupt_range;
  ant_hvf_gic_set_spi_fn set_spi;
  ant_hvf_gic_set_redistributor_reg_fn set_redistributor_reg;
} ant_hvf_gic_api_t;

static ant_hvf_gic_api_t ant_hvf_gic;

static int ant_hvf_advance_pc(hv_vcpu_t vcpu);
static int ant_hvf_sync_vtimer(ant_hvf_vm_t *vm);
static int ant_hvf_raise_vtimer(ant_hvf_vm_t *vm, const char *where);

static uint32_t ant_bswap32(uint32_t x) {
  return ((x & 0x000000ffu) << 24) |
         ((x & 0x0000ff00u) << 8) |
         ((x & 0x00ff0000u) >> 8) |
         ((x & 0xff000000u) >> 24);
}

static uint64_t ant_bswap64(uint64_t x) {
  return ((uint64_t)ant_bswap32((uint32_t)x) << 32) | ant_bswap32((uint32_t)(x >> 32));
}

static size_t ant_align4(size_t n) {
  return (n + 3u) & ~3u;
}

static size_t ant_align_page(size_t n) {
  return (n + (size_t)ANT_HVF_PAGE_SIZE - 1u) & ~((size_t)ANT_HVF_PAGE_SIZE - 1u);
}

static int ant_hvf_check(hv_return_t ret, const char *op) {
  if (ret == HV_SUCCESS) return 0;

  if (ret == HV_DENIED) {
    fprintf(stderr,
            "sandbox vm: %s denied; sign the binary with com.apple.security.hypervisor\n",
            op);
    return -EACCES;
  }

  fprintf(stderr, "sandbox vm: %s failed with Hypervisor.framework error %d\n", op, ret);
  return -EIO;
}

static int ant_hvf_check_file(const char *kind, const char *path, off_t *size_out) {
  if (!path || !path[0]) {
    fprintf(stderr, "sandbox vm: missing %s path\n", kind);
    return -EINVAL;
  }

  struct stat st;
  if (stat(path, &st) != 0) {
    fprintf(stderr, "sandbox vm: failed to read %s %s: %s\n", kind, path, strerror(errno));
    return -errno;
  }

  if (!S_ISREG(st.st_mode)) {
    fprintf(stderr, "sandbox vm: %s is not a regular file: %s\n", kind, path);
    return -EINVAL;
  }

  if (size_out) *size_out = st.st_size;
  return 0;
}

static uint16_t ant_hvf_load16(const unsigned char *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t ant_hvf_load32(const unsigned char *p) {
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint64_t ant_hvf_load64(const unsigned char *p) {
  return (uint64_t)ant_hvf_load32(p) | ((uint64_t)ant_hvf_load32(p + 4) << 32);
}

static void ant_hvf_store16(unsigned char *p, uint16_t value) {
  p[0] = (unsigned char)value;
  p[1] = (unsigned char)(value >> 8);
}

static void ant_hvf_store32(unsigned char *p, uint32_t value) {
  p[0] = (unsigned char)value;
  p[1] = (unsigned char)(value >> 8);
  p[2] = (unsigned char)(value >> 16);
  p[3] = (unsigned char)(value >> 24);
}

static void ant_hvf_store64(unsigned char *p, uint64_t value) {
  ant_hvf_store32(p, (uint32_t)value);
  ant_hvf_store32(p + 4, (uint32_t)(value >> 32));
}

static void ant_hvf_vsock_store_hdr(unsigned char *out, const ant_virtio_vsock_hdr_t *hdr) {
  ant_hvf_store64(out, hdr->src_cid);
  ant_hvf_store64(out + 8, hdr->dst_cid);
  ant_hvf_store32(out + 16, hdr->src_port);
  ant_hvf_store32(out + 20, hdr->dst_port);
  ant_hvf_store32(out + 24, hdr->len);
  ant_hvf_store16(out + 28, hdr->type);
  ant_hvf_store16(out + 30, hdr->op);
  ant_hvf_store32(out + 32, hdr->flags);
  ant_hvf_store32(out + 36, hdr->buf_alloc);
  ant_hvf_store32(out + 40, hdr->fwd_cnt);
}

static ant_virtio_vsock_hdr_t ant_hvf_vsock_load_hdr(const unsigned char *raw) {
  ant_virtio_vsock_hdr_t hdr = {
    .src_cid = ant_hvf_load64(raw),
    .dst_cid = ant_hvf_load64(raw + 8),
    .src_port = ant_hvf_load32(raw + 16),
    .dst_port = ant_hvf_load32(raw + 20),
    .len = ant_hvf_load32(raw + 24),
    .type = ant_hvf_load16(raw + 28),
    .op = ant_hvf_load16(raw + 30),
    .flags = ant_hvf_load32(raw + 32),
    .buf_alloc = ant_hvf_load32(raw + 36),
    .fwd_cnt = ant_hvf_load32(raw + 40),
  };
  return hdr;
}

static void *ant_hvf_guest_ptr(ant_hvf_vm_t *vm, uint64_t guest_addr, size_t len) {
  if (guest_addr < ANT_HVF_GUEST_BASE) return NULL;
  uint64_t off = guest_addr - ANT_HVF_GUEST_BASE;
  if (off > vm->mem_size || len > vm->mem_size - (size_t)off) return NULL;
  return (unsigned char *)vm->host_mem + off;
}

static int ant_hvf_guest_read(ant_hvf_vm_t *vm, uint64_t guest_addr, void *out, size_t len) {
  void *src = ant_hvf_guest_ptr(vm, guest_addr, len);
  if (!src) return -EFAULT;
  memcpy(out, src, len);
  return 0;
}

static int ant_hvf_guest_write(ant_hvf_vm_t *vm, uint64_t guest_addr, const void *src, size_t len) {
  void *dest = ant_hvf_guest_ptr(vm, guest_addr, len);
  if (!dest) return -EFAULT;
  memcpy(dest, src, len);
  return 0;
}

static int ant_read_all(int fd, void *buf, size_t len, off_t off) {
  unsigned char *p = buf;
  size_t got = 0;
  while (got < len) {
    ssize_t n = pread(fd, p + got, len - got, off + (off_t)got);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -errno;
    }
    if (n == 0) return -EIO;
    got += (size_t)n;
  }
  return 0;
}

static int ant_hvf_load_kernel(ant_hvf_vm_t *vm, const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -errno;

  ant_elf64_ehdr_t eh;
  int rc = ant_read_all(fd, &eh, sizeof(eh), 0);
  if (rc != 0) {
    close(fd);
    return rc;
  }

  if (memcmp(eh.ident, "\177ELF", 4) != 0 || eh.ident[4] != 2 || eh.machine != 183) {
    close(fd);
    fprintf(stderr, "sandbox vm: Nanos kernel is not an aarch64 ELF image: %s\n", path);
    return -EINVAL;
  }

  vm->kernel_entry = eh.entry;

  for (uint16_t i = 0; i < eh.phnum; i++) {
    ant_elf64_phdr_t ph;
    rc = ant_read_all(fd, &ph, sizeof(ph), (off_t)(eh.phoff + (uint64_t)i * eh.phentsize));
    if (rc != 0) {
      close(fd);
      return rc;
    }

    if (ph.type != 1) continue;
    void *dest = ant_hvf_guest_ptr(vm, ph.paddr, (size_t)ph.memsz);
    if (!dest) {
      close(fd);
      fprintf(stderr, "sandbox vm: kernel segment outside guest memory at 0x%llx\n",
              (unsigned long long)ph.paddr);
      return -EINVAL;
    }

    memset(dest, 0, (size_t)ph.memsz);
    rc = ant_read_all(fd, dest, (size_t)ph.filesz, (off_t)ph.offset);
    if (rc != 0) {
      close(fd);
      return rc;
    }
  }

  close(fd);
  return 0;
}

static int ant_fdt_reserve(ant_fdt_t *fdt, size_t len, unsigned char **out) {
  size_t aligned = ant_align4(len);
  if (aligned > sizeof(fdt->structure) - fdt->structure_len) return -ENOSPC;
  *out = fdt->structure + fdt->structure_len;
  memset(*out, 0, aligned);
  fdt->structure_len += aligned;
  return 0;
}

static int ant_fdt_u32(ant_fdt_t *fdt, uint32_t val) {
  unsigned char *out;
  int rc = ant_fdt_reserve(fdt, sizeof(uint32_t), &out);
  if (rc != 0) return rc;
  *(uint32_t *)out = ant_bswap32(val);
  return 0;
}

static int ant_fdt_string_offset(ant_fdt_t *fdt, const char *name) {
  size_t len = strlen(name) + 1;
  if (len > sizeof(fdt->strings) - fdt->strings_len) return -ENOSPC;
  int off = (int)fdt->strings_len;
  memcpy(fdt->strings + fdt->strings_len, name, len);
  fdt->strings_len += len;
  return off;
}

static int ant_fdt_begin(ant_fdt_t *fdt, const char *name) {
  int rc = ant_fdt_u32(fdt, FDT_BEGIN_NODE);
  if (rc != 0) return rc;
  unsigned char *out;
  size_t len = strlen(name) + 1;
  rc = ant_fdt_reserve(fdt, len, &out);
  if (rc != 0) return rc;
  memcpy(out, name, len);
  return 0;
}

static int ant_fdt_end(ant_fdt_t *fdt) {
  return ant_fdt_u32(fdt, FDT_END_NODE);
}

static int ant_fdt_prop(ant_fdt_t *fdt, const char *name, const void *data, size_t len) {
  int nameoff = ant_fdt_string_offset(fdt, name);
  if (nameoff < 0) return nameoff;
  int rc = ant_fdt_u32(fdt, FDT_PROP);
  if (rc != 0) return rc;
  rc = ant_fdt_u32(fdt, (uint32_t)len);
  if (rc != 0) return rc;
  rc = ant_fdt_u32(fdt, (uint32_t)nameoff);
  if (rc != 0) return rc;
  unsigned char *out;
  rc = ant_fdt_reserve(fdt, len, &out);
  if (rc != 0) return rc;
  memcpy(out, data, len);
  return 0;
}

static int ant_fdt_prop_null(ant_fdt_t *fdt, const char *name) {
  return ant_fdt_prop(fdt, name, "", 0);
}

static int ant_fdt_prop_string(ant_fdt_t *fdt, const char *name, const char *value) {
  return ant_fdt_prop(fdt, name, value, strlen(value) + 1);
}

static int ant_fdt_prop_u32(ant_fdt_t *fdt, const char *name, uint32_t value) {
  uint32_t be = ant_bswap32(value);
  return ant_fdt_prop(fdt, name, &be, sizeof(be));
}

static int ant_fdt_prop_cells(ant_fdt_t *fdt, const char *name, const uint32_t *cells, size_t count) {
  uint32_t be[32];
  if (count > sizeof(be) / sizeof(be[0])) return -EINVAL;
  for (size_t i = 0; i < count; i++) be[i] = ant_bswap32(cells[i]);
  return ant_fdt_prop(fdt, name, be, count * sizeof(be[0]));
}

static int ant_fdt_prop_reg64(ant_fdt_t *fdt, const char *name, const uint64_t *cells, size_t count) {
  uint64_t be[16];
  if (count > sizeof(be) / sizeof(be[0])) return -EINVAL;
  for (size_t i = 0; i < count; i++) be[i] = ant_bswap64(cells[i]);
  return ant_fdt_prop(fdt, name, be, count * sizeof(be[0]));
}

static int ant_hvf_build_dtb(ant_hvf_vm_t *vm) {
  ant_fdt_t fdt;
  memset(&fdt, 0, sizeof(fdt));

  int rc = ant_fdt_begin(&fdt, "");
  if (rc != 0) return rc;
  if ((rc = ant_fdt_prop_string(&fdt, "compatible", "linux,dummy-virt")) != 0) return rc;
  if ((rc = ant_fdt_prop_u32(&fdt, "#address-cells", 2)) != 0) return rc;
  if ((rc = ant_fdt_prop_u32(&fdt, "#size-cells", 2)) != 0) return rc;
  if ((rc = ant_fdt_prop_u32(&fdt, "interrupt-parent", 1)) != 0) return rc;

  if ((rc = ant_fdt_begin(&fdt, "cpus")) != 0) return rc;
  if ((rc = ant_fdt_prop_u32(&fdt, "#address-cells", 2)) != 0) return rc;
  if ((rc = ant_fdt_prop_u32(&fdt, "#size-cells", 0)) != 0) return rc;
  if ((rc = ant_fdt_begin(&fdt, "cpu@0")) != 0) return rc;
  if ((rc = ant_fdt_prop_string(&fdt, "device_type", "cpu")) != 0) return rc;
  if ((rc = ant_fdt_prop_string(&fdt, "compatible", "arm,arm-v8")) != 0) return rc;
  uint64_t cpu_reg[] = { 0 };
  if ((rc = ant_fdt_prop_reg64(&fdt, "reg", cpu_reg, 1)) != 0) return rc;
  if ((rc = ant_fdt_end(&fdt)) != 0) return rc;
  if ((rc = ant_fdt_end(&fdt)) != 0) return rc;

  if ((rc = ant_fdt_begin(&fdt, "memory@40000000")) != 0) return rc;
  if ((rc = ant_fdt_prop_string(&fdt, "device_type", "memory")) != 0) return rc;
  uint64_t mem_reg[] = { ANT_HVF_GUEST_BASE, vm->mem_size };
  if ((rc = ant_fdt_prop_reg64(&fdt, "reg", mem_reg, 2)) != 0) return rc;
  if ((rc = ant_fdt_end(&fdt)) != 0) return rc;

  if ((rc = ant_fdt_begin(&fdt, "chosen")) != 0) return rc;
  if ((rc = ant_fdt_prop_string(&fdt, "bootargs", "")) != 0) return rc;
  if ((rc = ant_fdt_prop_string(&fdt, "stdout-path", "/uart@9000000")) != 0) return rc;
  if ((rc = ant_fdt_end(&fdt)) != 0) return rc;

  if ((rc = ant_fdt_begin(&fdt, "intc")) != 0) return rc;
  if ((rc = ant_fdt_prop_string(&fdt, "compatible", "arm,gic-v3")) != 0) return rc;
  if ((rc = ant_fdt_prop_null(&fdt, "interrupt-controller")) != 0) return rc;
  if ((rc = ant_fdt_prop_u32(&fdt, "#interrupt-cells", 3)) != 0) return rc;
  if ((rc = ant_fdt_prop_u32(&fdt, "phandle", 1)) != 0) return rc;
  uint64_t gic_reg[] = { ANT_HVF_GIC_DIST_BASE, 0x10000, ANT_HVF_GIC_REDIST_BASE, 0x200000 };
  if ((rc = ant_fdt_prop_reg64(&fdt, "reg", gic_reg, 4)) != 0) return rc;
  if ((rc = ant_fdt_end(&fdt)) != 0) return rc;

  if ((rc = ant_fdt_begin(&fdt, "uart@9000000")) != 0) return rc;
  if ((rc = ant_fdt_prop_string(&fdt, "compatible", "arm,pl011")) != 0) return rc;
  uint64_t uart_reg[] = { ANT_HVF_UART_BASE, 0x1000 };
  if ((rc = ant_fdt_prop_reg64(&fdt, "reg", uart_reg, 2)) != 0) return rc;
  uint32_t uart_irq[] = { 0, 1, 4 };
  if ((rc = ant_fdt_prop_cells(&fdt, "interrupts", uart_irq, 3)) != 0) return rc;
  if ((rc = ant_fdt_end(&fdt)) != 0) return rc;

  if ((rc = ant_fdt_begin(&fdt, "pcie@3f000000")) != 0) return rc;
  if ((rc = ant_fdt_prop_string(&fdt, "compatible", "pci-host-ecam-generic")) != 0) return rc;
  if ((rc = ant_fdt_prop_string(&fdt, "device_type", "pci")) != 0) return rc;
  if ((rc = ant_fdt_prop_u32(&fdt, "#address-cells", 3)) != 0) return rc;
  if ((rc = ant_fdt_prop_u32(&fdt, "#size-cells", 2)) != 0) return rc;
  uint64_t pcie_reg[] = { ANT_HVF_PCIE_ECAM_BASE, ANT_HVF_PCIE_ECAM_SIZE };
  if ((rc = ant_fdt_prop_reg64(&fdt, "reg", pcie_reg, 2)) != 0) return rc;
  uint32_t bus_range[] = { 0, 0 };
  if ((rc = ant_fdt_prop_cells(&fdt, "bus-range", bus_range, 2)) != 0) return rc;
  if ((rc = ant_fdt_end(&fdt)) != 0) return rc;

  if ((rc = ant_fdt_end(&fdt)) != 0) return rc;
  if ((rc = ant_fdt_u32(&fdt, FDT_END)) != 0) return rc;

  size_t off_mem = sizeof(ant_fdt_header_t);
  size_t off_struct = ant_align4(off_mem + 16);
  size_t off_strings = off_struct + fdt.structure_len;
  size_t total = off_strings + fdt.strings_len;
  if (total > ANT_HVF_DTB_MAX) return -ENOSPC;

  unsigned char *dtb = ant_hvf_guest_ptr(vm, ANT_HVF_DTB_BASE, total);
  if (!dtb) return -EINVAL;
  memset(dtb, 0, ANT_HVF_DTB_MAX);

  ant_fdt_header_t hdr = {
    .magic = ant_bswap32(FDT_MAGIC),
    .totalsize = ant_bswap32((uint32_t)total),
    .off_dt_struct = ant_bswap32((uint32_t)off_struct),
    .off_dt_strings = ant_bswap32((uint32_t)off_strings),
    .off_mem_rsvmap = ant_bswap32((uint32_t)off_mem),
    .version = ant_bswap32(17),
    .last_comp_version = ant_bswap32(16),
    .boot_cpuid_phys = 0,
    .size_dt_strings = ant_bswap32((uint32_t)fdt.strings_len),
    .size_dt_struct = ant_bswap32((uint32_t)fdt.structure_len),
  };
  memcpy(dtb, &hdr, sizeof(hdr));
  memcpy(dtb + off_struct, fdt.structure, fdt.structure_len);
  memcpy(dtb + off_strings, fdt.strings, fdt.strings_len);
  return 0;
}

static void *ant_hvf_sym(const char *name) {
  return dlsym(RTLD_DEFAULT, name);
}

static int ant_hvf_load_gic_api(void) {
  ant_hvf_gic.create = (ant_hvf_gic_create_fn)ant_hvf_sym("hv_gic_create");
  ant_hvf_gic.config_create = (ant_hvf_gic_config_create_fn)ant_hvf_sym("hv_gic_config_create");
  ant_hvf_gic.set_distributor_base =
    (ant_hvf_gic_config_set_base_fn)ant_hvf_sym("hv_gic_config_set_distributor_base");
  ant_hvf_gic.set_redistributor_base =
    (ant_hvf_gic_config_set_base_fn)ant_hvf_sym("hv_gic_config_set_redistributor_base");
  ant_hvf_gic.set_msi_region_base =
    (ant_hvf_gic_config_set_base_fn)ant_hvf_sym("hv_gic_config_set_msi_region_base");
  ant_hvf_gic.set_msi_interrupt_range =
    (ant_hvf_gic_config_set_range_fn)ant_hvf_sym("hv_gic_config_set_msi_interrupt_range");
  ant_hvf_gic.set_spi = (ant_hvf_gic_set_spi_fn)ant_hvf_sym("hv_gic_set_spi");
  ant_hvf_gic.set_redistributor_reg =
    (ant_hvf_gic_set_redistributor_reg_fn)ant_hvf_sym("hv_gic_set_redistributor_reg");

  if (!ant_hvf_gic.create ||
      !ant_hvf_gic.config_create ||
      !ant_hvf_gic.set_distributor_base ||
      !ant_hvf_gic.set_redistributor_base ||
      !ant_hvf_gic.set_spi ||
      !ant_hvf_gic.set_redistributor_reg) {
    fprintf(stderr, "sandbox vm: Hypervisor.framework GIC symbols are unavailable on this SDK/runtime\n");
    return -ENOSYS;
  }

  return 0;
}

static int ant_hvf_create_gic(void) {
  int rc = ant_hvf_load_gic_api();
  if (rc != 0) return rc;

  ant_hvf_gic_config_t gic_config = ant_hvf_gic.config_create();
  if (!gic_config) return -EIO;

  rc = ant_hvf_check(ant_hvf_gic.set_distributor_base(gic_config, ANT_HVF_GIC_DIST_BASE),
                     "hv_gic_config_set_distributor_base");
  if (rc == 0) rc = ant_hvf_check(
    ant_hvf_gic.set_redistributor_base(gic_config, ANT_HVF_GIC_REDIST_BASE),
    "hv_gic_config_set_redistributor_base");
  if (rc == 0 && ant_hvf_gic.set_msi_region_base && ant_hvf_gic.set_msi_interrupt_range) {
    int msi_rc = ant_hvf_check(ant_hvf_gic.set_msi_region_base(gic_config, ANT_HVF_GIC_MSI_BASE),
                               "hv_gic_config_set_msi_region_base");
    if (msi_rc == 0) {
      msi_rc = ant_hvf_check(
        ant_hvf_gic.set_msi_interrupt_range(gic_config, 8192, 256),
        "hv_gic_config_set_msi_interrupt_range");
    }
    if (msi_rc != 0) {
      fprintf(stderr, "sandbox vm: continuing without GIC MSI support\n");
    }
  }
  if (rc == 0) rc = ant_hvf_check(ant_hvf_gic.create(gic_config), "hv_gic_create");

  os_release(gic_config);
  return rc;
}

static int ant_hvf_set_reg(hv_vcpu_t vcpu, hv_reg_t reg, uint64_t value, const char *name) {
  int rc = ant_hvf_check(hv_vcpu_set_reg(vcpu, reg, value), name);
  return rc;
}

static void *ant_hvf_timeout_thread(void *arg) {
  ant_hvf_timeout_t *timeout = arg;
  usleep(timeout->timeout_ms * 1000u);
  timeout->vm->timed_out = true;
  hv_vcpus_exit(&timeout->vm->vcpu, 1);
  return NULL;
}

static void *ant_hvf_wake_thread(void *arg) {
  ant_hvf_wake_t *wake = arg;
  while (!wake->stop) {
    usleep(wake->interval_ms * 1000u);
    if (!wake->stop) hv_vcpus_exit(&wake->vm->vcpu, 1);
  }
  return NULL;
}

static uint64_t ant_hvf_host_cntfrq(void) {
  uint64_t cntfrq = 0;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(cntfrq));
  return cntfrq;
}

static int ant_hvf_init_vcpu(ant_hvf_vm_t *vm) {
  int rc = ant_hvf_set_reg(vm->vcpu, HV_REG_PC, vm->kernel_entry, "hv_vcpu_set_reg(PC)");
  if (rc != 0) return rc;
  rc = ant_hvf_set_reg(vm->vcpu, HV_REG_X0, ANT_HVF_DTB_BASE, "hv_vcpu_set_reg(X0)");
  if (rc != 0) return rc;
  rc = ant_hvf_set_reg(vm->vcpu, HV_REG_X1, 0, "hv_vcpu_set_reg(X1)");
  if (rc != 0) return rc;
  rc = ant_hvf_set_reg(vm->vcpu, HV_REG_CPSR, 0x3c5, "hv_vcpu_set_reg(CPSR)");
  if (rc != 0) return rc;
  rc = ant_hvf_check(hv_vcpu_set_sys_reg(vm->vcpu, HV_SYS_REG_MPIDR_EL1, 0),
                     "hv_vcpu_set_sys_reg(MPIDR_EL1)");
  if (rc != 0) return rc;
  hv_return_t cntfrq_rc = hv_vcpu_set_sys_reg(vm->vcpu, ANT_HVF_SYS_REG_CNTFRQ_EL0, vm->cntfrq);
  if (cntfrq_rc != HV_SUCCESS && vm->trace) {
    fprintf(stderr, "sandbox vm: HVF does not allow setting CNTFRQ_EL0 (%d)\n", cntfrq_rc);
  }
  return ant_hvf_check(hv_vcpu_set_vtimer_mask(vm->vcpu, false),
                       "hv_vcpu_set_vtimer_mask(init)");
}

static int ant_hvf_handle_wfx(ant_hvf_vm_t *vm) {
  uint64_t ctl = 0;
  uint64_t cval = 0;
  int rc = ant_hvf_check(hv_vcpu_get_sys_reg(vm->vcpu, HV_SYS_REG_CNTV_CTL_EL0, &ctl),
                         "hv_vcpu_get_sys_reg(CNTV_CTL_EL0)");
  if (rc != 0) return rc;

  rc = ant_hvf_advance_pc(vm->vcpu);
  if (rc != 0) return rc;

  if ((ctl & 1u) == 0 || (ctl & 2u) != 0) {
    if (vm->trace) fprintf(stderr, "sandbox vm: wfx wait without active vtimer\n");
    return 0;
  }

  rc = ant_hvf_check(hv_vcpu_get_sys_reg(vm->vcpu, HV_SYS_REG_CNTV_CVAL_EL0, &cval),
                     "hv_vcpu_get_sys_reg(CNTV_CVAL_EL0)");
  if (rc != 0) return rc;

  uint64_t now = mach_absolute_time();
  if (cval > now && vm->cntfrq > 0) {
    uint64_t ticks = cval - now;
    uint64_t ns = (ticks * 1000000000ull) / vm->cntfrq;
    if (ns > 100000000ull) ns = 100000000ull;
    if (vm->trace) {
      fprintf(stderr,
              "sandbox vm: wfx timer wait ticks=%llu ns=%llu\n",
              (unsigned long long)ticks,
              (unsigned long long)ns);
    }
    if (ns > 0) {
      struct timespec ts = {
        .tv_sec = (time_t)(ns / 1000000000ull),
        .tv_nsec = (long)(ns % 1000000000ull),
      };
      while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
    }
  } else if (vm->trace) {
    fprintf(stderr, "sandbox vm: wfx timer already expired\n");
  }

  return ant_hvf_sync_vtimer(vm);
}

static int ant_hvf_sync_vtimer(ant_hvf_vm_t *vm) {
  uint64_t ctl = 0;
  uint64_t cval = 0;
  if (hv_vcpu_get_sys_reg(vm->vcpu, HV_SYS_REG_CNTV_CTL_EL0, &ctl) != HV_SUCCESS ||
      hv_vcpu_get_sys_reg(vm->vcpu, HV_SYS_REG_CNTV_CVAL_EL0, &cval) != HV_SUCCESS) {
    return 0;
  }
  if ((ctl & 1u) == 0 || (ctl & 2u) != 0 || mach_absolute_time() < cval) return 0;
  return ant_hvf_raise_vtimer(vm, "vtimer sync");
}

static int ant_hvf_raise_vtimer(ant_hvf_vm_t *vm, const char *where) {
  hv_return_t gic_rc = ant_hvf_gic.set_redistributor_reg(
    vm->vcpu,
    (ant_hvf_gic_redistributor_reg_t)ANT_HVF_GICR_ISPENDR0,
    1ull << ANT_HVF_GIC_EL1_VIRTUAL_TIMER);
  if (gic_rc != HV_SUCCESS && vm->trace) {
    fprintf(stderr, "sandbox vm: HVF denied direct vtimer PPI pending at %s (%d)\n",
            where,
            gic_rc);
  }
  return ant_hvf_check(hv_vcpu_set_vtimer_mask(vm->vcpu, false),
                       "hv_vcpu_set_vtimer_mask(vtimer)");
}

static uint64_t ant_hvf_select_width(uint64_t value, unsigned offset, unsigned size) {
  return (value >> (offset * 8u)) & (size >= 8 ? UINT64_MAX : ((1ull << (size * 8u)) - 1ull));
}

static void ant_hvf_assign_width(uint32_t *target, unsigned offset, unsigned size, uint64_t value) {
  uint32_t mask = size >= 4 ? UINT32_MAX : ((1u << (size * 8u)) - 1u);
  unsigned shift = offset * 8u;
  *target = (*target & ~(mask << shift)) | (((uint32_t)value & mask) << shift);
}

static bool ant_hvf_pci_addr(uint64_t addr, unsigned *bus, unsigned *slot, unsigned *fn, unsigned *reg) {
  if (addr < ANT_HVF_PCIE_ECAM_BASE || addr >= ANT_HVF_PCIE_ECAM_BASE + ANT_HVF_PCIE_ECAM_SIZE) {
    return false;
  }

  uint64_t off = addr - ANT_HVF_PCIE_ECAM_BASE;
  *bus = (unsigned)((off >> 20) & 0xff);
  *slot = (unsigned)((off >> 15) & 0x1f);
  *fn = (unsigned)((off >> 12) & 0x7);
  *reg = (unsigned)(off & 0xfff);
  return true;
}

static bool ant_hvf_is_virtio_slot(unsigned bus, unsigned slot, unsigned fn) {
  return bus == 0 && fn == 0 &&
         (slot == ANT_HVF_VIRTIO_BLK_SLOT || slot == ANT_HVF_VIRTIO_NET_SLOT ||
          slot == ANT_HVF_VIRTIO_9P0_SLOT || slot == ANT_HVF_VIRTIO_VSOCK_SLOT);
}

static ant_hvf_9p_device_t *ant_hvf_p9_for_slot(ant_hvf_vm_t *vm, unsigned slot) {
  if (slot == ANT_HVF_VIRTIO_9P0_SLOT) return &vm->p9[0];
  return NULL;
}

static uint32_t ant_hvf_pci_config_read32(ant_hvf_vm_t *vm, unsigned bus, unsigned slot, unsigned fn, unsigned reg) {
  if (bus == 0 && slot == 0 && fn == 0 && (reg & ~3u) == 0x0c) {
    return 0;
  }

  if (!ant_hvf_is_virtio_slot(bus, slot, fn)) return UINT32_MAX;
  if (slot == ANT_HVF_VIRTIO_NET_SLOT && !vm->net_enabled) return UINT32_MAX;

  bool is_net = slot == ANT_HVF_VIRTIO_NET_SLOT;
  bool is_vsock = slot == ANT_HVF_VIRTIO_VSOCK_SLOT;
  ant_hvf_9p_device_t *p9 = ant_hvf_p9_for_slot(vm, slot);

  switch (reg & ~3u) {
    case 0x00:
      return ((p9 ? ANT_VIRTIO_PCI_DEVICE_LEGACY_9P :
               is_vsock ? ANT_VIRTIO_PCI_DEVICE_LEGACY_VSOCK :
               is_net ? ANT_VIRTIO_PCI_DEVICE_LEGACY_NET : ANT_VIRTIO_PCI_DEVICE_LEGACY_BLOCK) << 16) |
             ANT_VIRTIO_PCI_VENDOR;
    case 0x04: return 0x00100000u | ANT_VIRTIO_PCI_ABI_VERSION;
    case 0x08: return is_net ? 0x02000000u : 0x01000000u;
    case 0x0c: return 0;
    case 0x10:
      if (p9) {
        if (p9->bar0 == UINT32_MAX) return ~(ANT_HVF_VIRTIO_9P_BAR_SIZE - 1u);
        return p9->bar0;
      }
      if (is_vsock) {
        if (vm->vsock.bar0 == UINT32_MAX) return ~(ANT_HVF_VIRTIO_VSOCK_BAR_SIZE - 1u);
        return vm->vsock.bar0;
      }
      if (is_net) {
        if (vm->net_bar0 == UINT32_MAX) return ~(ANT_HVF_VIRTIO_NET_BAR_SIZE - 1u);
        return vm->net_bar0;
      }
      if (vm->blk_bar0 == UINT32_MAX) return ~(ANT_HVF_VIRTIO_BLK_BAR_SIZE - 1u);
      return vm->blk_bar0;
    case 0x2c:
      return ((p9 ? ANT_VIRTIO_PCI_SUBDEVICE_9P :
               is_vsock ? ANT_VIRTIO_PCI_SUBDEVICE_VSOCK :
               is_net ? ANT_VIRTIO_PCI_SUBDEVICE_NET : ANT_VIRTIO_PCI_SUBDEVICE_BLOCK) << 16) |
             ANT_VIRTIO_PCI_SUBVENDOR;
    case 0x34: return 0;
    case 0x3c: return 0;
    default: return 0;
  }
}

static void ant_hvf_pci_config_write(ant_hvf_vm_t *vm,
                                     unsigned bus,
                                     unsigned slot,
                                     unsigned fn,
                                     unsigned reg,
                                     unsigned size,
                                     uint64_t value) {
  if (!ant_hvf_is_virtio_slot(bus, slot, fn)) return;
  if (slot == ANT_HVF_VIRTIO_NET_SLOT && !vm->net_enabled) return;
  if ((reg & ~3u) == 0x10) {
    ant_hvf_9p_device_t *p9 = ant_hvf_p9_for_slot(vm, slot);
    uint32_t *bar = p9 ? &p9->bar0 :
                    slot == ANT_HVF_VIRTIO_VSOCK_SLOT ? &vm->vsock.bar0 :
                    slot == ANT_HVF_VIRTIO_NET_SLOT ? &vm->net_bar0 : &vm->blk_bar0;
    ant_hvf_assign_width(bar, reg & 3u, size, value);
  }
}

static int ant_hvf_vring_read_desc(ant_hvf_vm_t *vm, uint64_t desc_base, uint16_t index, ant_vring_desc_t *out) {
  unsigned char raw[16];
  int rc = ant_hvf_guest_read(vm, desc_base + (uint64_t)index * sizeof(raw), raw, sizeof(raw));
  if (rc != 0) return rc;
  out->addr = ant_hvf_load64(raw);
  out->len = ant_hvf_load32(raw + 8);
  out->flags = ant_hvf_load16(raw + 12);
  out->next = ant_hvf_load16(raw + 14);
  return 0;
}

static int ant_hvf_disk_read(ant_hvf_vm_t *vm, uint64_t sector, uint64_t guest_addr, uint32_t len) {
  void *dest = ant_hvf_guest_ptr(vm, guest_addr, len);
  if (!dest) return -EFAULT;
  off_t off = (off_t)(sector * 512ull);
  size_t done = 0;
  while (done < len) {
    ssize_t n = pread(vm->image_fd, (unsigned char *)dest + done, len - done, off + (off_t)done);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -errno;
    }
    if (n == 0) {
      memset((unsigned char *)dest + done, 0, len - done);
      return 0;
    }
    done += (size_t)n;
  }
  return 0;
}

static int ant_hvf_disk_write(ant_hvf_vm_t *vm, uint64_t sector, uint64_t guest_addr, uint32_t len) {
  void *src = ant_hvf_guest_ptr(vm, guest_addr, len);
  if (!src) return -EFAULT;
  off_t off = (off_t)(sector * 512ull);
  size_t done = 0;
  while (done < len) {
    ssize_t n = pwrite(vm->image_fd, (unsigned char *)src + done, len - done, off + (off_t)done);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -errno;
    }
    done += (size_t)n;
  }
  return 0;
}

static int ant_hvf_virtio_blk_request(ant_hvf_vm_t *vm,
                                      uint64_t desc_base,
                                      uint16_t head,
                                      uint32_t *used_len) {
  ant_vring_desc_t desc;
  int rc = ant_hvf_vring_read_desc(vm, desc_base, head, &desc);
  if (rc != 0) return rc;
  if (!(desc.flags & ANT_VRING_DESC_F_NEXT)) return -EINVAL;

  unsigned char req_raw[16];
  if (desc.len < sizeof(req_raw)) return -EINVAL;
  rc = ant_hvf_guest_read(vm, desc.addr, req_raw, sizeof(req_raw));
  if (rc != 0) return rc;

  ant_virtio_blk_req_t req = {
    .type = ant_hvf_load32(req_raw),
    .reserved = ant_hvf_load32(req_raw + 4),
    .sector = ant_hvf_load64(req_raw + 8),
  };
  (void)req.reserved;

  if (vm->trace) {
    fprintf(stderr, "sandbox vm: blk req head=%u type=%u sector=%llu\n",
            head, req.type, (unsigned long long)req.sector);
  }

  uint8_t status = ANT_VIRTIO_BLK_S_OK;
  uint32_t total = 0;
  uint16_t next = desc.next;
  ant_vring_desc_t status_desc;
  memset(&status_desc, 0, sizeof(status_desc));

  for (unsigned chain = 0; chain < ANT_VIRTIO_BLK_QUEUE_SIZE; chain++) {
    rc = ant_hvf_vring_read_desc(vm, desc_base, next, &desc);
    if (rc != 0) return rc;

    if (!(desc.flags & ANT_VRING_DESC_F_NEXT)) {
      status_desc = desc;
      break;
    }

    if (req.type == ANT_VIRTIO_BLK_T_IN) {
      rc = ant_hvf_disk_read(vm, req.sector + total / 512u, desc.addr, desc.len);
    } else if (req.type == ANT_VIRTIO_BLK_T_OUT) {
      rc = ant_hvf_disk_write(vm, req.sector + total / 512u, desc.addr, desc.len);
    } else if (req.type == ANT_VIRTIO_BLK_T_FLUSH) {
      rc = fsync(vm->image_fd) == 0 ? 0 : -errno;
    } else {
      rc = -ENOTSUP;
    }

    if (rc != 0) {
      status = rc == -ENOTSUP ? 2u : ANT_VIRTIO_BLK_S_IOERR;
    }
    total += desc.len;
    next = desc.next;
  }

  if (!status_desc.addr || status_desc.len < 1) return -EINVAL;
  rc = ant_hvf_guest_write(vm, status_desc.addr, &status, 1);
  if (rc != 0) return rc;

  *used_len = (req.type == ANT_VIRTIO_BLK_T_IN ? total : 0) + 1u;
  if (vm->trace) {
    fprintf(stderr, "sandbox vm: blk complete head=%u status=%u used_len=%u\n",
            head, status, *used_len);
  }
  return 0;
}

static int ant_hvf_raise_spi(unsigned int irq) {
  if (!ant_hvf_gic.set_spi) return 0;
  return ant_hvf_check(ant_hvf_gic.set_spi(irq, true), "hv_gic_set_spi");
}

static int ant_hvf_lower_spi(unsigned int irq) {
  if (!ant_hvf_gic.set_spi) return 0;
  return ant_hvf_check(ant_hvf_gic.set_spi(irq, false), "hv_gic_set_spi");
}

static uint64_t ant_hvf_vring_desc_base(uint32_t queue_pfn) {
  return (uint64_t)queue_pfn << 12;
}

static uint64_t ant_hvf_vring_avail_base(uint64_t desc_base, unsigned queue_size) {
  return desc_base + (uint64_t)queue_size * 16u;
}

static uint64_t ant_hvf_vring_used_base(uint64_t avail_base, unsigned queue_size) {
  return (avail_base + 6u + (uint64_t)queue_size * 2u + 4095u) & ~4095ull;
}

static int ant_hvf_vring_add_used(ant_hvf_vm_t *vm,
                                  uint64_t used_base,
                                  unsigned queue_size,
                                  uint16_t head,
                                  uint32_t used_len) {
  unsigned char used_idx_raw[2];
  int rc = ant_hvf_guest_read(vm, used_base + 2, used_idx_raw, sizeof(used_idx_raw));
  if (rc != 0) return rc;
  uint16_t used_idx = ant_hvf_load16(used_idx_raw);
  uint64_t elem = used_base + 4u + (uint64_t)(used_idx % queue_size) * 8u;
  unsigned char used_elem[8];
  ant_hvf_store32(used_elem, head);
  ant_hvf_store32(used_elem + 4, used_len);
  rc = ant_hvf_guest_write(vm, elem, used_elem, sizeof(used_elem));
  if (rc != 0) return rc;
  ant_hvf_store16(used_idx_raw, (uint16_t)(used_idx + 1));
  return ant_hvf_guest_write(vm, used_base + 2, used_idx_raw, sizeof(used_idx_raw));
}

static int ant_hvf_virtio_blk_notify(ant_hvf_vm_t *vm) {
  if (vm->blk_queue_sel != 0 || !vm->blk_queue_pfn) return 0;

  uint64_t desc_base = ant_hvf_vring_desc_base(vm->blk_queue_pfn);
  uint64_t avail_base = ant_hvf_vring_avail_base(desc_base, ANT_VIRTIO_BLK_QUEUE_SIZE);
  uint64_t used_base = ant_hvf_vring_used_base(avail_base, ANT_VIRTIO_BLK_QUEUE_SIZE);

  unsigned char idx_raw[2];
  int rc = ant_hvf_guest_read(vm, avail_base + 2, idx_raw, sizeof(idx_raw));
  if (rc != 0) return rc;
  uint16_t avail_idx = ant_hvf_load16(idx_raw);
  if (vm->trace) {
    fprintf(stderr,
            "sandbox vm: blk notify pfn=0x%x avail=%u last=%u desc=0x%llx used=0x%llx\n",
            vm->blk_queue_pfn,
            avail_idx,
            vm->blk_last_avail,
            (unsigned long long)desc_base,
            (unsigned long long)used_base);
  }

  while (vm->blk_last_avail != avail_idx) {
    uint16_t ring_slot = vm->blk_last_avail % ANT_VIRTIO_BLK_QUEUE_SIZE;
    unsigned char head_raw[2];
    rc = ant_hvf_guest_read(vm, avail_base + 4u + (uint64_t)ring_slot * 2u,
                            head_raw, sizeof(head_raw));
    if (rc != 0) return rc;
    uint16_t head = ant_hvf_load16(head_raw);

    uint32_t used_len = 0;
    rc = ant_hvf_virtio_blk_request(vm, desc_base, head, &used_len);
    if (rc != 0) return rc;

    rc = ant_hvf_vring_add_used(vm, used_base, ANT_VIRTIO_BLK_QUEUE_SIZE, head, used_len);
    if (rc != 0) return rc;

    vm->blk_last_avail++;
  }

  vm->blk_isr |= 1u;
  return ant_hvf_raise_spi(ANT_HVF_VIRTIO_BLK_IRQ);
}

static bool ant_hvf_virtio_blk_read(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t *value) {
  uint64_t off = addr - ANT_HVF_VIRTIO_BLK_BAR;
  uint64_t word = 0;

  if (off < ANT_VIRTIO_PCI_CONFIG) {
    if (off < 4) {
      word = 0;
      *value = ant_hvf_select_width(word, (unsigned)off, size);
    } else if (off >= ANT_VIRTIO_PCI_QUEUE_PFN && off < ANT_VIRTIO_PCI_QUEUE_PFN + 4) {
      word = vm->blk_queue_pfn;
      *value = ant_hvf_select_width(word, (unsigned)(off - ANT_VIRTIO_PCI_QUEUE_PFN), size);
    } else if (off >= ANT_VIRTIO_PCI_QUEUE_NUM && off < ANT_VIRTIO_PCI_QUEUE_NUM + 2) {
      word = ANT_VIRTIO_BLK_QUEUE_SIZE;
      *value = ant_hvf_select_width(word, (unsigned)(off - ANT_VIRTIO_PCI_QUEUE_NUM), size);
    } else if (off >= ANT_VIRTIO_PCI_QUEUE_SEL && off < ANT_VIRTIO_PCI_QUEUE_SEL + 2) {
      word = vm->blk_queue_sel;
      *value = ant_hvf_select_width(word, (unsigned)(off - ANT_VIRTIO_PCI_QUEUE_SEL), size);
    } else if (off == ANT_VIRTIO_PCI_STATUS) {
      *value = vm->blk_status;
    } else if (off == ANT_VIRTIO_PCI_ISR) {
      *value = vm->blk_isr;
      if (vm->trace) fprintf(stderr, "sandbox vm: blk isr read value=0x%x\n", vm->blk_isr);
      vm->blk_isr = 0;
      ant_hvf_lower_spi(ANT_HVF_VIRTIO_BLK_IRQ);
    } else {
      *value = 0;
    }
    return true;
  }

  uint64_t cfg_off = off - ANT_VIRTIO_PCI_CONFIG;
  if (cfg_off < 8) {
    *value = ant_hvf_select_width(vm->image_sectors, (unsigned)cfg_off, size);
    return true;
  }
  if (cfg_off >= 20 && cfg_off < 24) {
    *value = ant_hvf_select_width(512u, (unsigned)(cfg_off - 20), size);
    return true;
  }

  *value = 0;
  return true;
}

static bool ant_hvf_virtio_blk_write(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t value) {
  uint64_t off = addr - ANT_HVF_VIRTIO_BLK_BAR;
  (void)size;

  switch (off) {
    case ANT_VIRTIO_PCI_GUEST_FEATURES:
      vm->blk_guest_features = (uint32_t)value;
      return true;
    case ANT_VIRTIO_PCI_QUEUE_PFN:
      vm->blk_queue_pfn = (uint32_t)value;
      if (vm->trace) fprintf(stderr, "sandbox vm: blk queue pfn=0x%x\n", vm->blk_queue_pfn);
      return true;
    case ANT_VIRTIO_PCI_QUEUE_SEL:
      vm->blk_queue_sel = (uint16_t)value;
      return true;
    case ANT_VIRTIO_PCI_QUEUE_NOTIFY:
      ant_hvf_virtio_blk_notify(vm);
      return true;
    case ANT_VIRTIO_PCI_STATUS:
      if ((uint8_t)value == 0) {
        vm->blk_status = 0;
        vm->blk_queue_pfn = 0;
        vm->blk_last_avail = 0;
      } else {
        vm->blk_status |= (uint8_t)value;
      }
      if (vm->trace) fprintf(stderr, "sandbox vm: blk status=0x%x\n", vm->blk_status);
      return true;
    default:
      return true;
  }
}

static int ant_hvf_virtio_net_tx(ant_hvf_vm_t *vm,
                                 uint64_t desc_base,
                                 uint16_t head,
                                 uint32_t *used_len) {
  ant_vring_desc_t desc;
  uint32_t total = 0;
  uint16_t index = head;

  for (unsigned chain = 0; chain < ANT_VIRTIO_NET_QUEUE_SIZE; chain++) {
    int rc = ant_hvf_vring_read_desc(vm, desc_base, index, &desc);
    if (rc != 0) return rc;
    total += desc.len;
    if (!(desc.flags & ANT_VRING_DESC_F_NEXT)) break;
    index = desc.next;
  }

  if (vm->trace) {
    fprintf(stderr, "sandbox vm: net tx complete head=%u used_len=%u\n", head, total);
  }

  *used_len = total;
  return 0;
}

static int ant_hvf_virtio_net_notify(ant_hvf_vm_t *vm) {
  if (vm->net_queue_sel >= ANT_VIRTIO_NET_QUEUE_COUNT) return 0;

  unsigned queue = vm->net_queue_sel;
  uint32_t queue_pfn = vm->net_queue_pfn[queue];
  if (!queue_pfn) return 0;

  uint64_t desc_base = ant_hvf_vring_desc_base(queue_pfn);
  uint64_t avail_base = ant_hvf_vring_avail_base(desc_base, ANT_VIRTIO_NET_QUEUE_SIZE);
  uint64_t used_base = ant_hvf_vring_used_base(avail_base, ANT_VIRTIO_NET_QUEUE_SIZE);

  unsigned char idx_raw[2];
  int rc = ant_hvf_guest_read(vm, avail_base + 2, idx_raw, sizeof(idx_raw));
  if (rc != 0) return rc;
  uint16_t avail_idx = ant_hvf_load16(idx_raw);

  if (vm->trace) {
    fprintf(stderr,
            "sandbox vm: net notify queue=%u pfn=0x%x avail=%u last=%u\n",
            queue,
            queue_pfn,
            avail_idx,
            vm->net_last_avail[queue]);
  }

  if (queue == ANT_VIRTIO_NET_RX_QUEUE) {
    vm->net_last_avail[queue] = avail_idx;
    return 0;
  }

  while (vm->net_last_avail[queue] != avail_idx) {
    uint16_t ring_slot = vm->net_last_avail[queue] % ANT_VIRTIO_NET_QUEUE_SIZE;
    unsigned char head_raw[2];
    rc = ant_hvf_guest_read(vm, avail_base + 4u + (uint64_t)ring_slot * 2u,
                            head_raw, sizeof(head_raw));
    if (rc != 0) return rc;
    uint16_t head = ant_hvf_load16(head_raw);

    uint32_t used_len = 0;
    rc = ant_hvf_virtio_net_tx(vm, desc_base, head, &used_len);
    if (rc != 0) return rc;

    rc = ant_hvf_vring_add_used(vm, used_base, ANT_VIRTIO_NET_QUEUE_SIZE, head, used_len);
    if (rc != 0) return rc;

    vm->net_last_avail[queue]++;
  }

  vm->net_isr |= 1u;
  return ant_hvf_raise_spi(ANT_HVF_VIRTIO_NET_IRQ);
}

static bool ant_hvf_virtio_net_read(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t *value) {
  uint64_t off = addr - ANT_HVF_VIRTIO_NET_BAR;
  uint64_t word = 0;

  if (off < ANT_VIRTIO_PCI_CONFIG) {
    if (off < 4) {
      word = ANT_VIRTIO_NET_F_MAC;
      *value = ant_hvf_select_width(word, (unsigned)off, size);
    } else if (off >= ANT_VIRTIO_PCI_QUEUE_PFN && off < ANT_VIRTIO_PCI_QUEUE_PFN + 4) {
      word = vm->net_queue_sel < ANT_VIRTIO_NET_QUEUE_COUNT ?
             vm->net_queue_pfn[vm->net_queue_sel] : 0;
      *value = ant_hvf_select_width(word, (unsigned)(off - ANT_VIRTIO_PCI_QUEUE_PFN), size);
    } else if (off >= ANT_VIRTIO_PCI_QUEUE_NUM && off < ANT_VIRTIO_PCI_QUEUE_NUM + 2) {
      word = vm->net_queue_sel < ANT_VIRTIO_NET_QUEUE_COUNT ? ANT_VIRTIO_NET_QUEUE_SIZE : 0;
      *value = ant_hvf_select_width(word, (unsigned)(off - ANT_VIRTIO_PCI_QUEUE_NUM), size);
    } else if (off >= ANT_VIRTIO_PCI_QUEUE_SEL && off < ANT_VIRTIO_PCI_QUEUE_SEL + 2) {
      word = vm->net_queue_sel;
      *value = ant_hvf_select_width(word, (unsigned)(off - ANT_VIRTIO_PCI_QUEUE_SEL), size);
    } else if (off == ANT_VIRTIO_PCI_STATUS) {
      *value = vm->net_status;
    } else if (off == ANT_VIRTIO_PCI_ISR) {
      *value = vm->net_isr;
      if (vm->trace) fprintf(stderr, "sandbox vm: net isr read value=0x%x\n", vm->net_isr);
      vm->net_isr = 0;
      ant_hvf_lower_spi(ANT_HVF_VIRTIO_NET_IRQ);
    } else {
      *value = 0;
    }
    return true;
  }

  static const uint8_t mac[6] = { 0x02, 0x41, 0x4e, 0x54, 0x00, 0x01 };
  uint64_t cfg_off = off - ANT_VIRTIO_PCI_CONFIG;
  if (cfg_off < sizeof(mac)) {
    word = 0;
    for (unsigned i = 0; i < size && cfg_off + i < sizeof(mac); i++) {
      word |= (uint64_t)mac[cfg_off + i] << (i * 8u);
    }
    *value = word;
    return true;
  }

  *value = 0;
  return true;
}

static bool ant_hvf_virtio_net_write(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t value) {
  uint64_t off = addr - ANT_HVF_VIRTIO_NET_BAR;
  (void)size;

  switch (off) {
    case ANT_VIRTIO_PCI_GUEST_FEATURES:
      vm->net_guest_features = (uint32_t)value;
      return true;
    case ANT_VIRTIO_PCI_QUEUE_PFN:
      if (vm->net_queue_sel < ANT_VIRTIO_NET_QUEUE_COUNT) {
        vm->net_queue_pfn[vm->net_queue_sel] = (uint32_t)value;
        if (vm->trace) {
          fprintf(stderr,
                  "sandbox vm: net queue %u pfn=0x%x\n",
                  vm->net_queue_sel,
                  vm->net_queue_pfn[vm->net_queue_sel]);
        }
      }
      return true;
    case ANT_VIRTIO_PCI_QUEUE_SEL:
      vm->net_queue_sel = (uint16_t)value;
      return true;
    case ANT_VIRTIO_PCI_QUEUE_NOTIFY:
      ant_hvf_virtio_net_notify(vm);
      return true;
    case ANT_VIRTIO_PCI_STATUS:
      if ((uint8_t)value == 0) {
        vm->net_status = 0;
        memset(vm->net_queue_pfn, 0, sizeof(vm->net_queue_pfn));
        memset(vm->net_last_avail, 0, sizeof(vm->net_last_avail));
      } else {
        vm->net_status |= (uint8_t)value;
      }
      if (vm->trace) fprintf(stderr, "sandbox vm: net status=0x%x\n", vm->net_status);
      return true;
    default:
      return true;
  }
}

static int ant_hvf_vsock_write_iov(ant_hvf_vm_t *vm,
                                   uint64_t desc_base,
                                   uint16_t head,
                                   const unsigned char *data,
                                   uint32_t len,
                                   uint32_t *used_len) {
  uint16_t index = head;
  uint32_t done = 0;

  for (unsigned chain = 0; chain < ANT_VIRTIO_VSOCK_QUEUE_SIZE && done < len; chain++) {
    ant_vring_desc_t desc;
    int rc = ant_hvf_vring_read_desc(vm, desc_base, index, &desc);
    if (rc != 0) return rc;

    if (desc.flags & ANT_VRING_DESC_F_WRITE) {
      uint32_t n = desc.len;
      if (n > len - done) n = len - done;
      rc = ant_hvf_guest_write(vm, desc.addr, data + done, n);
      if (rc != 0) return rc;
      done += n;
    }

    if (!(desc.flags & ANT_VRING_DESC_F_NEXT)) break;
    index = desc.next;
  }

  if (done != len) return -ENOSPC;
  *used_len = done;
  return 0;
}

static int ant_hvf_vsock_send_packet(ant_hvf_vm_t *vm,
                                     uint16_t op,
                                     const char *payload,
                                     uint32_t payload_len) {
  ant_hvf_vsock_device_t *dev = &vm->vsock;
  uint32_t queue_pfn = dev->queue_pfn[0];
  if (!queue_pfn) return -EAGAIN;

  uint64_t desc_base = ant_hvf_vring_desc_base(queue_pfn);
  uint64_t avail_base = ant_hvf_vring_avail_base(desc_base, ANT_VIRTIO_VSOCK_QUEUE_SIZE);
  uint64_t used_base = ant_hvf_vring_used_base(avail_base, ANT_VIRTIO_VSOCK_QUEUE_SIZE);

  unsigned char idx_raw[2];
  int rc = ant_hvf_guest_read(vm, avail_base + 2, idx_raw, sizeof(idx_raw));
  if (rc != 0) return rc;
  uint16_t avail_idx = ant_hvf_load16(idx_raw);
  if (dev->last_avail[0] == avail_idx) return -EAGAIN;

  uint16_t ring_slot = dev->last_avail[0] % ANT_VIRTIO_VSOCK_QUEUE_SIZE;
  unsigned char head_raw[2];
  rc = ant_hvf_guest_read(vm, avail_base + 4u + (uint64_t)ring_slot * 2u,
                          head_raw, sizeof(head_raw));
  if (rc != 0) return rc;
  uint16_t head = ant_hvf_load16(head_raw);

  unsigned char packet[4096];
  size_t hdr_len = sizeof(ant_virtio_vsock_hdr_t);
  if (hdr_len + payload_len > sizeof(packet)) return -E2BIG;

  ant_virtio_vsock_hdr_t hdr = {
    .src_cid = ANT_HVF_VSOCK_HOST_CID,
    .dst_cid = ANT_HVF_VSOCK_GUEST_CID,
    .src_port = ANT_HVF_VSOCK_HOST_PORT,
    .dst_port = dev->peer_port,
    .len = payload_len,
    .type = ANT_VIRTIO_VSOCK_TYPE_STREAM,
    .op = op,
    .flags = 0,
    .buf_alloc = ANT_HVF_VSOCK_BUF_ALLOC,
    .fwd_cnt = dev->fwd_cnt,
  };
  ant_hvf_vsock_store_hdr(packet, &hdr);
  if (payload_len > 0) memcpy(packet + hdr_len, payload, payload_len);

  uint32_t used_len = 0;
  rc = ant_hvf_vsock_write_iov(vm, desc_base, head, packet, (uint32_t)(hdr_len + payload_len), &used_len);
  if (rc != 0) return rc;
  rc = ant_hvf_vring_add_used(vm, used_base, ANT_VIRTIO_VSOCK_QUEUE_SIZE, head, used_len);
  if (rc != 0) return rc;

  dev->last_avail[0]++;
  dev->isr |= 1u;
  if (vm->trace) {
    fprintf(stderr,
            "sandbox vm: vsock rx packet op=%u len=%u peer_port=%u\n",
            op,
            payload_len,
            dev->peer_port);
  }
  return ant_hvf_raise_spi(ANT_HVF_VIRTIO_VSOCK_IRQ);
}

static int ant_hvf_vsock_maybe_send_request(ant_hvf_vm_t *vm) {
  ant_hvf_vsock_device_t *dev = &vm->vsock;
  if (!dev->connected || dev->request_sent || !dev->request_json) return 0;

  size_t len = strlen(dev->request_json);
  if (len > 3900) return -E2BIG;
  char line[4096];
  memcpy(line, dev->request_json, len);
  line[len++] = '\n';

  int rc = ant_hvf_vsock_send_packet(vm, ANT_VIRTIO_VSOCK_OP_RW, line, (uint32_t)len);
  if (rc == 0) dev->request_sent = true;
  if (rc == -EAGAIN) return 0;
  return rc;
}

static int ant_hvf_vsock_read_tx_packet(ant_hvf_vm_t *vm,
                                        uint64_t desc_base,
                                        uint16_t head,
                                        ant_virtio_vsock_hdr_t *hdr,
                                        uint32_t *used_len) {
  unsigned char raw[sizeof(ant_virtio_vsock_hdr_t)];
  uint32_t done = 0;
  uint32_t total = 0;
  uint16_t index = head;

  for (unsigned chain = 0; chain < ANT_VIRTIO_VSOCK_QUEUE_SIZE; chain++) {
    ant_vring_desc_t desc;
    int rc = ant_hvf_vring_read_desc(vm, desc_base, index, &desc);
    if (rc != 0) return rc;

    if (!(desc.flags & ANT_VRING_DESC_F_WRITE)) {
      uint32_t n = desc.len;
      if (done < sizeof(raw)) {
        uint32_t copy = n;
        if (copy > sizeof(raw) - done) copy = (uint32_t)(sizeof(raw) - done);
        rc = ant_hvf_guest_read(vm, desc.addr, raw + done, copy);
        if (rc != 0) return rc;
        done += copy;
      }
      total += n;
    }

    if (!(desc.flags & ANT_VRING_DESC_F_NEXT)) break;
    index = desc.next;
  }

  if (done < sizeof(raw)) return -EINVAL;
  *hdr = ant_hvf_vsock_load_hdr(raw);
  *used_len = total;
  return 0;
}

static int ant_hvf_virtio_vsock_notify(ant_hvf_vm_t *vm, unsigned queue) {
  ant_hvf_vsock_device_t *dev = &vm->vsock;
  if (queue >= ANT_VIRTIO_VSOCK_QUEUE_COUNT) return 0;
  uint32_t queue_pfn = dev->queue_pfn[queue];
  if (!queue_pfn) return 0;

  if (queue == 0) return ant_hvf_vsock_maybe_send_request(vm);
  if (queue == 2) return 0;

  uint64_t desc_base = ant_hvf_vring_desc_base(queue_pfn);
  uint64_t avail_base = ant_hvf_vring_avail_base(desc_base, ANT_VIRTIO_VSOCK_QUEUE_SIZE);
  uint64_t used_base = ant_hvf_vring_used_base(avail_base, ANT_VIRTIO_VSOCK_QUEUE_SIZE);

  unsigned char idx_raw[2];
  int rc = ant_hvf_guest_read(vm, avail_base + 2, idx_raw, sizeof(idx_raw));
  if (rc != 0) return rc;
  uint16_t avail_idx = ant_hvf_load16(idx_raw);

  while (dev->last_avail[queue] != avail_idx) {
    uint16_t ring_slot = dev->last_avail[queue] % ANT_VIRTIO_VSOCK_QUEUE_SIZE;
    unsigned char head_raw[2];
    rc = ant_hvf_guest_read(vm, avail_base + 4u + (uint64_t)ring_slot * 2u,
                            head_raw, sizeof(head_raw));
    if (rc != 0) return rc;
    uint16_t head = ant_hvf_load16(head_raw);

    ant_virtio_vsock_hdr_t hdr;
    uint32_t used_len = 0;
    rc = ant_hvf_vsock_read_tx_packet(vm, desc_base, head, &hdr, &used_len);
    if (rc != 0) return rc;

    if (vm->trace) {
      fprintf(stderr,
              "sandbox vm: vsock tx op=%u len=%u src=%llu:%u dst=%llu:%u\n",
              hdr.op,
              hdr.len,
              (unsigned long long)hdr.src_cid,
              hdr.src_port,
              (unsigned long long)hdr.dst_cid,
              hdr.dst_port);
    }

    if (hdr.op == ANT_VIRTIO_VSOCK_OP_REQUEST && hdr.dst_port == ANT_HVF_VSOCK_HOST_PORT) {
      dev->connected = true;
      dev->peer_port = hdr.src_port;
      ant_hvf_vsock_send_packet(vm, ANT_VIRTIO_VSOCK_OP_RESPONSE, NULL, 0);
      ant_hvf_vsock_maybe_send_request(vm);
    } else if (hdr.op == ANT_VIRTIO_VSOCK_OP_RW) {
      dev->fwd_cnt += hdr.len;
    }

    rc = ant_hvf_vring_add_used(vm, used_base, ANT_VIRTIO_VSOCK_QUEUE_SIZE, head, used_len);
    if (rc != 0) return rc;
    dev->last_avail[queue]++;
  }

  dev->isr |= 1u;
  return ant_hvf_raise_spi(ANT_HVF_VIRTIO_VSOCK_IRQ);
}

static bool ant_hvf_virtio_vsock_read(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t *value) {
  ant_hvf_vsock_device_t *dev = &vm->vsock;
  uint64_t off = addr - dev->bar0;
  uint64_t word = 0;

  if (off < ANT_VIRTIO_PCI_CONFIG) {
    if (off < 4) {
      word = ANT_VIRTIO_VSOCK_F_STREAM;
      *value = ant_hvf_select_width(word, (unsigned)off, size);
    } else if (off >= ANT_VIRTIO_PCI_QUEUE_PFN && off < ANT_VIRTIO_PCI_QUEUE_PFN + 4) {
      word = dev->queue_sel < ANT_VIRTIO_VSOCK_QUEUE_COUNT ? dev->queue_pfn[dev->queue_sel] : 0;
      *value = ant_hvf_select_width(word, (unsigned)(off - ANT_VIRTIO_PCI_QUEUE_PFN), size);
    } else if (off >= ANT_VIRTIO_PCI_QUEUE_NUM && off < ANT_VIRTIO_PCI_QUEUE_NUM + 2) {
      word = dev->queue_sel < ANT_VIRTIO_VSOCK_QUEUE_COUNT ? ANT_VIRTIO_VSOCK_QUEUE_SIZE : 0;
      *value = ant_hvf_select_width(word, (unsigned)(off - ANT_VIRTIO_PCI_QUEUE_NUM), size);
    } else if (off >= ANT_VIRTIO_PCI_QUEUE_SEL && off < ANT_VIRTIO_PCI_QUEUE_SEL + 2) {
      *value = dev->queue_sel;
    } else if (off == ANT_VIRTIO_PCI_STATUS) {
      *value = dev->status;
    } else if (off == ANT_VIRTIO_PCI_ISR) {
      *value = dev->isr;
      dev->isr = 0;
      ant_hvf_lower_spi(ANT_HVF_VIRTIO_VSOCK_IRQ);
    } else {
      *value = 0;
    }
    return true;
  }

  uint64_t cfg_off = off - ANT_VIRTIO_PCI_CONFIG;
  if (cfg_off < 8) {
    *value = ant_hvf_select_width(ANT_HVF_VSOCK_GUEST_CID, (unsigned)cfg_off, size);
    return true;
  }
  *value = 0;
  return true;
}

static bool ant_hvf_virtio_vsock_write(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t value) {
  ant_hvf_vsock_device_t *dev = &vm->vsock;
  uint64_t off = addr - dev->bar0;
  (void)size;

  switch (off) {
    case ANT_VIRTIO_PCI_GUEST_FEATURES:
      dev->guest_features = (uint32_t)value;
      return true;
    case ANT_VIRTIO_PCI_QUEUE_PFN:
      if (dev->queue_sel < ANT_VIRTIO_VSOCK_QUEUE_COUNT) {
        dev->queue_pfn[dev->queue_sel] = (uint32_t)value;
      }
      return true;
    case ANT_VIRTIO_PCI_QUEUE_SEL:
      dev->queue_sel = (uint16_t)value;
      return true;
    case ANT_VIRTIO_PCI_QUEUE_NOTIFY: {
      unsigned queue = value < ANT_VIRTIO_VSOCK_QUEUE_COUNT ? (unsigned)value : dev->queue_sel;
      ant_hvf_virtio_vsock_notify(vm, queue);
      return true;
    }
    case ANT_VIRTIO_PCI_STATUS:
      if ((uint8_t)value == 0) {
        memset(dev->queue_pfn, 0, sizeof(dev->queue_pfn));
        memset(dev->last_avail, 0, sizeof(dev->last_avail));
        dev->status = 0;
        dev->isr = 0;
        dev->connected = false;
        dev->request_sent = false;
        dev->peer_port = 0;
        dev->fwd_cnt = 0;
      } else {
        dev->status |= (uint8_t)value;
      }
      return true;
    default:
      return true;
  }
}

typedef struct {
  uint64_t addr;
  uint32_t len;
} ant_hvf_iov_t;

static uint64_t ant_hvf_9p_hash(const char *path) {
  uint64_t h = 1469598103934665603ull;
  for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
    h ^= *p;
    h *= 1099511628211ull;
  }
  return h ? h : 1;
}

static void ant_hvf_9p_qid(unsigned char *out, bool dir, const char *path) {
  out[0] = dir ? P9_QTDIR : 0;
  ant_hvf_store32(out + 1, 0);
  ant_hvf_store64(out + 5, ant_hvf_9p_hash(path));
}

static void ant_hvf_9p_hdr(unsigned char *out, uint32_t size, uint8_t type, uint16_t tag) {
  ant_hvf_store32(out, size);
  out[4] = type;
  ant_hvf_store16(out + 5, tag);
}

static uint32_t ant_hvf_9p_append_dirent(unsigned char *out,
                                         uint32_t off,
                                         uint32_t cap,
                                         const char *name,
                                         const char *qid_path,
                                         bool is_dir,
                                         uint64_t next_offset,
                                         uint8_t dtype) {
  size_t name_len = strlen(name);
  uint32_t rec_len = (uint32_t)(13u + 8u + 1u + 2u + name_len);
  if (name_len > UINT16_MAX || rec_len > cap - off) return 0;

  ant_hvf_9p_qid(out + off, is_dir, qid_path);
  ant_hvf_store64(out + off + 13, next_offset);
  out[off + 21] = dtype;
  ant_hvf_store16(out + off + 22, (uint16_t)name_len);
  memcpy(out + off + 24, name, name_len);
  return rec_len;
}

static uint32_t ant_hvf_9p_error(unsigned char *out, uint16_t tag, uint32_t ecode) {
  ant_hvf_9p_hdr(out, 11, P9_RLERROR, tag);
  ant_hvf_store32(out + 7, ecode);
  return 11;
}

static bool ant_hvf_9p_path_bad(const char *path) {
  if (!path || path[0] == '/') return true;
  if (strcmp(path, "..") == 0) return true;
  if (strncmp(path, "../", 3) == 0) return true;
  return strstr(path, "/../") || strstr(path, "/..");
}

static int ant_hvf_9p_host_path(ant_hvf_9p_device_t *dev, const char *rel, char *out, size_t out_len) {
  if (!dev->root || ant_hvf_9p_path_bad(rel)) return -ENOENT;
  int n = rel[0] ? snprintf(out, out_len, "%s/%s", dev->root, rel)
                 : snprintf(out, out_len, "%s", dev->root);
  if (n < 0 || (size_t)n >= out_len) return -ENAMETOOLONG;
  return 0;
}

static int ant_hvf_9p_stat(ant_hvf_9p_device_t *dev, const char *rel, struct stat *st) {
  memset(st, 0, sizeof(*st));
  char host[4096];
  int rc = ant_hvf_9p_host_path(dev, rel, host, sizeof(host));
  if (rc != 0) return rc;
  if (lstat(host, st) != 0) return -errno;
  return 0;
}

static uint8_t ant_hvf_9p_dtype_from_mode(mode_t mode) {
  if (S_ISDIR(mode)) return DT_DIR;
  if (S_ISLNK(mode)) return DT_LNK;
  if (S_ISCHR(mode)) return DT_CHR;
  if (S_ISBLK(mode)) return DT_BLK;
  if (S_ISFIFO(mode)) return DT_FIFO;
  if (S_ISSOCK(mode)) return DT_SOCK;
  return DT_REG;
}

static bool ant_hvf_9p_dtype_is_dir(uint8_t dtype) {
  return dtype == DT_DIR;
}

static int ant_hvf_9p_dirent_type(ant_hvf_9p_device_t *dev,
                                  const char *rel,
                                  uint8_t host_dtype,
                                  uint8_t *dtype,
                                  bool *is_dir) {
  if (host_dtype != DT_UNKNOWN) {
    *dtype = host_dtype;
    *is_dir = ant_hvf_9p_dtype_is_dir(host_dtype);
    return 0;
  }

  struct stat st;
  int rc = ant_hvf_9p_stat(dev, rel, &st);
  if (rc != 0) return rc;
  *dtype = ant_hvf_9p_dtype_from_mode(st.st_mode);
  *is_dir = S_ISDIR(st.st_mode);
  return 0;
}

static int ant_hvf_9p_walk(ant_hvf_9p_device_t *dev, const char *base, const char *name, char *out, size_t out_len) {
  if (strcmp(name, ".") == 0) {
    int n = snprintf(out, out_len, "%s", base);
    return n < 0 || (size_t)n >= out_len ? -ENAMETOOLONG : 0;
  }
  if (strcmp(name, "..") == 0) {
    if (base[0] == '\0') {
      out[0] = '\0';
      return 0;
    }
    int n = snprintf(out, out_len, "%s", base);
    if (n < 0 || (size_t)n >= out_len) return -ENAMETOOLONG;
    char *slash = strrchr(out, '/');
    if (slash) *slash = '\0';
    else out[0] = '\0';
    struct stat st;
    return ant_hvf_9p_stat(dev, out, &st);
  }
  int n = base[0] ? snprintf(out, out_len, "%s/%s", base, name)
                  : snprintf(out, out_len, "%s", name);
  if (n < 0 || (size_t)n >= out_len) return -ENAMETOOLONG;
  struct stat st;
  return ant_hvf_9p_stat(dev, out, &st);
}

static int ant_hvf_9p_read_chain(ant_hvf_vm_t *vm,
                                 uint64_t desc_base,
                                 uint16_t head,
                                 unsigned queue_size,
                                 unsigned char *req,
                                 size_t req_cap,
                                 size_t *req_len,
                                 ant_hvf_iov_t *writes,
                                 size_t writes_cap,
                                 size_t *writes_len) {
  uint16_t index = head;
  *req_len = 0;
  *writes_len = 0;

  for (unsigned chain = 0; chain < queue_size; chain++) {
    ant_vring_desc_t desc;
    int rc = ant_hvf_vring_read_desc(vm, desc_base, index, &desc);
    if (rc != 0) return rc;

    if (desc.flags & ANT_VRING_DESC_F_WRITE) {
      if (*writes_len >= writes_cap) return -E2BIG;
      writes[*writes_len] = (ant_hvf_iov_t){ .addr = desc.addr, .len = desc.len };
      (*writes_len)++;
    } else {
      if (*req_len + desc.len > req_cap) return -E2BIG;
      rc = ant_hvf_guest_read(vm, desc.addr, req + *req_len, desc.len);
      if (rc != 0) return rc;
      *req_len += desc.len;
    }

    if (!(desc.flags & ANT_VRING_DESC_F_NEXT)) break;
    index = desc.next;
  }

  return 0;
}

static int ant_hvf_9p_write_response(ant_hvf_vm_t *vm,
                                     const ant_hvf_iov_t *writes,
                                     size_t writes_len,
                                     const unsigned char *resp,
                                     uint32_t resp_len) {
  uint32_t done = 0;
  for (size_t i = 0; i < writes_len && done < resp_len; i++) {
    uint32_t n = writes[i].len;
    if (n > resp_len - done) n = resp_len - done;
    int rc = ant_hvf_guest_write(vm, writes[i].addr, resp + done, n);
    if (rc != 0) return rc;
    done += n;
  }
  return done == resp_len ? 0 : -ENOSPC;
}

static ant_hvf_9p_fid_t *ant_hvf_9p_fid(ant_hvf_9p_device_t *dev, uint32_t fid) {
  if (fid >= ANT_HVF_9P_FID_COUNT) return NULL;
  return &dev->fids[fid];
}

static uint32_t ant_hvf_9p_handle(ant_hvf_9p_device_t *dev,
                                  const unsigned char *req,
                                  size_t req_len,
                                  unsigned char *resp,
                                  size_t resp_cap) {
  if (req_len < 7) return 0;
  uint8_t type = req[4];
  uint16_t tag = ant_hvf_load16(req + 5);
  uint32_t fid;
  ant_hvf_9p_fid_t *f;

  switch (type) {
    case P9_TVERSION: {
      if (req_len < 13) return ant_hvf_9p_error(resp, tag, EINVAL);
      uint32_t msize = ant_hvf_load32(req + 7);
      uint16_t vlen = ant_hvf_load16(req + 11);
      if (13u + vlen > req_len || 13u + vlen > resp_cap) return ant_hvf_9p_error(resp, tag, EINVAL);
      uint32_t size = 13u + vlen;
      ant_hvf_9p_hdr(resp, size, P9_RVERSION, tag);
      ant_hvf_store32(resp + 7, msize < 8192 ? msize : 8192);
      ant_hvf_store16(resp + 11, vlen);
      memcpy(resp + 13, req + 13, vlen);
      return size;
    }
    case P9_TATTACH:
      if (req_len < 15) return ant_hvf_9p_error(resp, tag, EINVAL);
      fid = ant_hvf_load32(req + 7);
      f = ant_hvf_9p_fid(dev, fid);
      if (!f) return ant_hvf_9p_error(resp, tag, EINVAL);
      f->active = true;
      f->path[0] = '\0';
      ant_hvf_9p_hdr(resp, 20, P9_RATTACH, tag);
      ant_hvf_9p_qid(resp + 7, true, "");
      return 20;
    case P9_TWALK: {
      if (req_len < 17) return ant_hvf_9p_error(resp, tag, EINVAL);
      fid = ant_hvf_load32(req + 7);
      uint32_t newfid = ant_hvf_load32(req + 11);
      uint16_t nwname = ant_hvf_load16(req + 15);
      f = ant_hvf_9p_fid(dev, fid);
      ant_hvf_9p_fid_t *nf = ant_hvf_9p_fid(dev, newfid);
      if (!f || !f->active || !nf) return ant_hvf_9p_error(resp, tag, ENOENT);
      char path[ANT_HVF_9P_PATH_MAX];
      snprintf(path, sizeof(path), "%s", f->path);
      size_t off = 17;
      uint32_t size = 9;
      ant_hvf_9p_hdr(resp, 0, P9_RWALK, tag);
      ant_hvf_store16(resp + 7, 0);
      for (uint16_t i = 0; i < nwname; i++) {
        if (off + 2 > req_len) return ant_hvf_9p_error(resp, tag, EINVAL);
        uint16_t nlen = ant_hvf_load16(req + off);
        off += 2;
        if (off + nlen > req_len || nlen >= ANT_HVF_9P_PATH_MAX) return ant_hvf_9p_error(resp, tag, EINVAL);
        char name[ANT_HVF_9P_PATH_MAX];
        memcpy(name, req + off, nlen);
        name[nlen] = '\0';
        off += nlen;
        char next[ANT_HVF_9P_PATH_MAX];
        int rc = ant_hvf_9p_walk(dev, path, name, next, sizeof(next));
        if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
        snprintf(path, sizeof(path), "%s", next);
        struct stat st;
        ant_hvf_9p_stat(dev, path, &st);
        if (size + 13 > resp_cap) return ant_hvf_9p_error(resp, tag, ENOSPC);
        ant_hvf_9p_qid(resp + size, S_ISDIR(st.st_mode), path);
        size += 13;
      }
      nf->active = true;
      snprintf(nf->path, sizeof(nf->path), "%s", path);
      ant_hvf_store32(resp, size);
      ant_hvf_store16(resp + 7, nwname);
      return size;
    }
    case P9_TGETATTR: {
      if (req_len < 19) return ant_hvf_9p_error(resp, tag, EINVAL);
      fid = ant_hvf_load32(req + 7);
      f = ant_hvf_9p_fid(dev, fid);
      if (!f || !f->active) return ant_hvf_9p_error(resp, tag, ENOENT);
      struct stat st;
      int rc = ant_hvf_9p_stat(dev, f->path, &st);
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      ant_hvf_9p_hdr(resp, 143, P9_RGETATTR, tag);
      ant_hvf_store64(resp + 7, P9_GETATTR_BASIC);
      ant_hvf_9p_qid(resp + 15, S_ISDIR(st.st_mode), f->path);
      ant_hvf_store32(resp + 28, (uint32_t)st.st_mode);
      ant_hvf_store32(resp + 32, 0);
      ant_hvf_store32(resp + 36, 0);
      ant_hvf_store64(resp + 40, (uint64_t)(st.st_nlink ? st.st_nlink : 1));
      ant_hvf_store64(resp + 48, 0);
      ant_hvf_store64(resp + 56, (uint64_t)st.st_size);
      ant_hvf_store64(resp + 64, 4096);
      ant_hvf_store64(resp + 72, (uint64_t)st.st_blocks);
      ant_hvf_store64(resp + 80, (uint64_t)st.st_atime);
      ant_hvf_store64(resp + 88, 0);
      ant_hvf_store64(resp + 96, (uint64_t)st.st_mtime);
      ant_hvf_store64(resp + 104, 0);
      ant_hvf_store64(resp + 112, (uint64_t)st.st_ctime);
      ant_hvf_store64(resp + 120, 0);
      ant_hvf_store64(resp + 128, 0);
      ant_hvf_store64(resp + 136, 0);
      return 143;
    }
    case P9_TLOPEN:
      if (req_len < 15) return ant_hvf_9p_error(resp, tag, EINVAL);
      fid = ant_hvf_load32(req + 7);
      f = ant_hvf_9p_fid(dev, fid);
      if (!f || !f->active) return ant_hvf_9p_error(resp, tag, ENOENT);
      struct stat st;
      if (ant_hvf_9p_stat(dev, f->path, &st) != 0) return ant_hvf_9p_error(resp, tag, ENOENT);
      ant_hvf_9p_hdr(resp, 24, P9_RLOPEN, tag);
      ant_hvf_9p_qid(resp + 7, S_ISDIR(st.st_mode), f->path);
      ant_hvf_store32(resp + 20, 8192);
      return 24;
    case P9_TREAD: {
      if (req_len < 23) return ant_hvf_9p_error(resp, tag, EINVAL);
      fid = ant_hvf_load32(req + 7);
      uint64_t offset = ant_hvf_load64(req + 11);
      uint32_t count = ant_hvf_load32(req + 19);
      f = ant_hvf_9p_fid(dev, fid);
      if (!f || !f->active) return ant_hvf_9p_error(resp, tag, ENOENT);
      if (11u + count > resp_cap) count = (uint32_t)(resp_cap - 11u);
      uint32_t got = 0;
      char host[4096];
      int rc = ant_hvf_9p_host_path(dev, f->path, host, sizeof(host));
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      int fd = open(host, O_RDONLY);
      if (fd < 0) return ant_hvf_9p_error(resp, tag, (uint32_t)errno);
      ssize_t n = pread(fd, resp + 11, count, (off_t)offset);
      if (n < 0) {
        uint32_t e = (uint32_t)errno;
        close(fd);
        return ant_hvf_9p_error(resp, tag, e);
      }
      got = (uint32_t)n;
      close(fd);
      ant_hvf_9p_hdr(resp, 11u + got, P9_RREAD, tag);
      ant_hvf_store32(resp + 7, got);
      return 11u + got;
    }
    case P9_TREADDIR: {
      if (req_len < 23) return ant_hvf_9p_error(resp, tag, EINVAL);
      fid = ant_hvf_load32(req + 7);
      uint64_t offset = ant_hvf_load64(req + 11);
      uint32_t count = ant_hvf_load32(req + 19);
      f = ant_hvf_9p_fid(dev, fid);
      if (!f || !f->active) return ant_hvf_9p_error(resp, tag, ENOENT);
      if (count > resp_cap - 11u) count = (uint32_t)(resp_cap - 11u);

      uint32_t used = 0;
      char host[4096];
      int rc = ant_hvf_9p_host_path(dev, f->path, host, sizeof(host));
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      DIR *dir = opendir(host);
      if (!dir) return ant_hvf_9p_error(resp, tag, (uint32_t)errno);

      uint64_t index = 0;
      struct dirent *ent;
      while ((ent = readdir(dir))) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (index++ < offset) continue;

        char child[ANT_HVF_9P_PATH_MAX];
        int npath = f->path[0] ? snprintf(child, sizeof(child), "%s/%s", f->path, ent->d_name)
                               : snprintf(child, sizeof(child), "%s", ent->d_name);
        if (npath < 0 || (size_t)npath >= sizeof(child)) continue;
        uint8_t dtype = DT_UNKNOWN;
        bool is_dir = false;
        if (ant_hvf_9p_dirent_type(dev, child, ent->d_type, &dtype, &is_dir) != 0) continue;

        uint32_t added = ant_hvf_9p_append_dirent(resp + 11, used, count,
                                                  ent->d_name, child, is_dir,
                                                  index, dtype);
        if (added == 0) break;
        used += added;
      }
      closedir(dir);

      if (dev->root && getenv("ANT_SANDBOX_VM_TRACE_9P_READDIR")) {
        fprintf(stderr,
                "sandbox vm: 9p readdir path=%s offset=%llu count=%u used=%u\n",
                f->path[0] ? f->path : ".",
                (unsigned long long)offset,
                count,
                used);
      }

      ant_hvf_9p_hdr(resp, 11u + used, P9_RREADDIR, tag);
      ant_hvf_store32(resp + 7, used);
      return 11u + used;
    }
    case P9_TSTATFS:
      ant_hvf_9p_hdr(resp, 61, P9_RSTATFS, tag);
      ant_hvf_store32(resp + 7, 0x01021997);
      ant_hvf_store32(resp + 11, 4096);
      for (unsigned i = 15; i < 57; i += 8) ant_hvf_store64(resp + i, 0);
      ant_hvf_store32(resp + 57, 255);
      return 61;
    case P9_TCLUNK:
      if (req_len >= 11) {
        f = ant_hvf_9p_fid(dev, ant_hvf_load32(req + 7));
        if (f) memset(f, 0, sizeof(*f));
      }
      ant_hvf_9p_hdr(resp, 7, P9_RCLUNK, tag);
      return 7;
    default:
      return ant_hvf_9p_error(resp, tag, ENOSYS);
  }
}

static int ant_hvf_virtio_9p_notify(ant_hvf_vm_t *vm, ant_hvf_9p_device_t *dev) {
  uint32_t queue_pfn = dev->queue_pfn;
  if (!queue_pfn) return 0;

  uint64_t desc_base = ant_hvf_vring_desc_base(queue_pfn);
  uint64_t avail_base = ant_hvf_vring_avail_base(desc_base, ANT_VIRTIO_9P_QUEUE_SIZE);
  uint64_t used_base = ant_hvf_vring_used_base(avail_base, ANT_VIRTIO_9P_QUEUE_SIZE);

  unsigned char idx_raw[2];
  int rc = ant_hvf_guest_read(vm, avail_base + 2, idx_raw, sizeof(idx_raw));
  if (rc != 0) return rc;
  uint16_t avail_idx = ant_hvf_load16(idx_raw);
  if (vm->trace) {
    fprintf(stderr,
            "sandbox vm: 9p tag=%s notify pfn=0x%x avail=%u last=%u desc=0x%llx used=0x%llx\n",
            dev->tag ? dev->tag : "?",
            queue_pfn,
            avail_idx,
            dev->last_avail,
            (unsigned long long)desc_base,
            (unsigned long long)used_base);
  }

  while (dev->last_avail != avail_idx) {
    uint16_t ring_slot = dev->last_avail % ANT_VIRTIO_9P_QUEUE_SIZE;
    unsigned char head_raw[2];
    rc = ant_hvf_guest_read(vm, avail_base + 4u + (uint64_t)ring_slot * 2u,
                            head_raw, sizeof(head_raw));
    if (rc != 0) return rc;
    uint16_t head = ant_hvf_load16(head_raw);

    unsigned char req[8192];
    unsigned char resp[8192];
    ant_hvf_iov_t writes[8];
    size_t req_len = 0;
    size_t writes_len = 0;
    rc = ant_hvf_9p_read_chain(vm, desc_base, head, ANT_VIRTIO_9P_QUEUE_SIZE,
                               req, sizeof(req), &req_len, writes, 8, &writes_len);
    if (rc != 0) {
      if (vm->trace) fprintf(stderr, "sandbox vm: 9p read chain failed %d\n", rc);
      return rc;
    }
    if (vm->trace && req_len >= 7) {
      fprintf(stderr,
              "sandbox vm: 9p tag=%s req head=%u type=%u len=%zu writes=%zu\n",
              dev->tag ? dev->tag : "?",
              head,
              req[4],
              req_len,
              writes_len);
    }
    uint32_t resp_len = ant_hvf_9p_handle(dev, req, req_len, resp, sizeof(resp));
    if (resp_len == 0) resp_len = ant_hvf_9p_error(resp, 0, EIO);
    rc = ant_hvf_9p_write_response(vm, writes, writes_len, resp, resp_len);
    if (rc != 0) {
      if (vm->trace) fprintf(stderr, "sandbox vm: 9p write response failed %d len=%u\n", rc, resp_len);
      return rc;
    }
    rc = ant_hvf_vring_add_used(vm, used_base, ANT_VIRTIO_9P_QUEUE_SIZE, head, resp_len);
    if (rc != 0) return rc;
    if (vm->trace) {
      fprintf(stderr,
              "sandbox vm: 9p tag=%s complete head=%u resp_len=%u\n",
              dev->tag ? dev->tag : "?",
              head,
              resp_len);
    }
    dev->last_avail++;
  }

  dev->isr |= 1u;
  return ant_hvf_raise_spi(dev->irq);
}

static bool ant_hvf_virtio_9p_read(ant_hvf_vm_t *vm,
                                   ant_hvf_9p_device_t *dev,
                                   uint64_t addr,
                                   unsigned size,
                                   uint64_t *value) {
  uint64_t off = addr - dev->bar0;
  uint64_t word = 0;

  if (off < ANT_VIRTIO_PCI_CONFIG) {
    if (off < 4) {
      word = ANT_VIRTIO_9P_F_MOUNT_TAG;
      *value = ant_hvf_select_width(word, (unsigned)off, size);
    } else if (off >= ANT_VIRTIO_PCI_QUEUE_PFN && off < ANT_VIRTIO_PCI_QUEUE_PFN + 4) {
      *value = ant_hvf_select_width(dev->queue_pfn, (unsigned)(off - ANT_VIRTIO_PCI_QUEUE_PFN), size);
    } else if (off >= ANT_VIRTIO_PCI_QUEUE_NUM && off < ANT_VIRTIO_PCI_QUEUE_NUM + 2) {
      *value = ant_hvf_select_width(ANT_VIRTIO_9P_QUEUE_SIZE, (unsigned)(off - ANT_VIRTIO_PCI_QUEUE_NUM), size);
    } else if (off >= ANT_VIRTIO_PCI_QUEUE_SEL && off < ANT_VIRTIO_PCI_QUEUE_SEL + 2) {
      *value = dev->queue_sel;
    } else if (off == ANT_VIRTIO_PCI_STATUS) {
      *value = dev->status;
    } else if (off == ANT_VIRTIO_PCI_ISR) {
      *value = dev->isr;
      dev->isr = 0;
      ant_hvf_lower_spi(dev->irq);
    } else {
      *value = 0;
    }
    return true;
  }

  uint64_t cfg_off = off - ANT_VIRTIO_PCI_CONFIG;
  uint16_t tag_len = (uint16_t)strlen(dev->tag);
  if (cfg_off < 2) {
    *value = ant_hvf_select_width(tag_len, (unsigned)cfg_off, size);
  } else if (cfg_off - 2 < tag_len) {
    for (unsigned i = 0; i < size && cfg_off - 2 + i < tag_len; i++) {
      word |= (uint64_t)(unsigned char)dev->tag[cfg_off - 2 + i] << (i * 8u);
    }
    *value = word;
  } else {
    *value = 0;
  }
  return true;
}

static bool ant_hvf_virtio_9p_write(ant_hvf_vm_t *vm,
                                    ant_hvf_9p_device_t *dev,
                                    uint64_t addr,
                                    unsigned size,
                                    uint64_t value) {
  uint64_t off = addr - dev->bar0;
  (void)size;

  switch (off) {
    case ANT_VIRTIO_PCI_GUEST_FEATURES:
      dev->guest_features = (uint32_t)value;
      return true;
    case ANT_VIRTIO_PCI_QUEUE_PFN:
      dev->queue_pfn = (uint32_t)value;
      return true;
    case ANT_VIRTIO_PCI_QUEUE_SEL:
      dev->queue_sel = (uint16_t)value;
      return true;
    case ANT_VIRTIO_PCI_QUEUE_NOTIFY:
      ant_hvf_virtio_9p_notify(vm, dev);
      return true;
    case ANT_VIRTIO_PCI_STATUS:
      if ((uint8_t)value == 0) {
        dev->status = 0;
        dev->queue_pfn = 0;
        dev->last_avail = 0;
        memset(dev->fids, 0, sizeof(dev->fids));
      } else {
        dev->status |= (uint8_t)value;
      }
      return true;
    default:
      return true;
  }
}

static bool ant_hvf_pci_mmio_read(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t *value) {
  unsigned bus, slot, fn, reg;
  if (!ant_hvf_pci_addr(addr, &bus, &slot, &fn, &reg)) return false;
  uint32_t word = ant_hvf_pci_config_read32(vm, bus, slot, fn, reg);
  *value = ant_hvf_select_width(word, reg & 3u, size);
  return true;
}

static bool ant_hvf_pci_mmio_write(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t value) {
  unsigned bus, slot, fn, reg;
  if (!ant_hvf_pci_addr(addr, &bus, &slot, &fn, &reg)) return false;
  ant_hvf_pci_config_write(vm, bus, slot, fn, reg, size, value);
  return true;
}

static bool ant_hvf_mmio_read(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t *value) {
  if (addr == ANT_HVF_UART_BASE + 0x18) {
    *value = 0;
    return true;
  }
  if (addr >= ANT_HVF_RTC_BASE && addr < ANT_HVF_RTC_BASE + 0x1000) {
    uint64_t off = addr - ANT_HVF_RTC_BASE;
    switch (off) {
      case 0x000: *value = (uint32_t)time(NULL); return true;
      case 0xfe0: *value = 0x31; return true;
      case 0xfe4: *value = 0x10; return true;
      case 0xfe8: *value = 0x04; return true;
      case 0xfec: *value = 0x00; return true;
      case 0xff0: *value = 0x0d; return true;
      case 0xff4: *value = 0xf0; return true;
      case 0xff8: *value = 0x05; return true;
      case 0xffc: *value = 0xb1; return true;
      default: *value = 0; return true;
    }
  }
  if (addr >= ANT_HVF_PCIE_ECAM_BASE && addr < ANT_HVF_PCIE_ECAM_BASE + ANT_HVF_PCIE_ECAM_SIZE) {
    return ant_hvf_pci_mmio_read(vm, addr, size, value);
  }
  if (addr >= ANT_HVF_VIRTIO_BLK_BAR && addr < ANT_HVF_VIRTIO_BLK_BAR + ANT_HVF_VIRTIO_BLK_BAR_SIZE) {
    return ant_hvf_virtio_blk_read(vm, addr, size, value);
  }
  if (addr >= ANT_HVF_VIRTIO_NET_BAR && addr < ANT_HVF_VIRTIO_NET_BAR + ANT_HVF_VIRTIO_NET_BAR_SIZE) {
    return ant_hvf_virtio_net_read(vm, addr, size, value);
  }
  if (addr >= vm->vsock.bar0 && addr < (uint64_t)vm->vsock.bar0 + ANT_HVF_VIRTIO_VSOCK_BAR_SIZE) {
    return ant_hvf_virtio_vsock_read(vm, addr, size, value);
  }
  for (size_t i = 0; i < sizeof(vm->p9) / sizeof(vm->p9[0]); i++) {
    if (addr >= vm->p9[i].bar0 && addr < (uint64_t)vm->p9[i].bar0 + ANT_HVF_VIRTIO_9P_BAR_SIZE) {
      return ant_hvf_virtio_9p_read(vm, &vm->p9[i], addr, size, value);
    }
  }
  if (addr >= ANT_HVF_PCIE_PIO_BASE && addr < ANT_HVF_PCIE_PIO_BASE + 0x10000) {
    *value = 0;
    return true;
  }
  return false;
}

static bool ant_hvf_mmio_write(ant_hvf_vm_t *vm, uint64_t addr, unsigned size, uint64_t value) {
  (void)size;
  if (addr == ANT_HVF_UART_BASE) {
    fputc((int)(value & 0xff), stdout);
    fflush(stdout);
    return true;
  }
  if (addr >= ANT_HVF_RTC_BASE && addr < ANT_HVF_RTC_BASE + 0x1000) return true;
  if (addr >= ANT_HVF_PCIE_ECAM_BASE && addr < ANT_HVF_PCIE_ECAM_BASE + ANT_HVF_PCIE_ECAM_SIZE) {
    return ant_hvf_pci_mmio_write(vm, addr, size, value);
  }
  if (addr >= ANT_HVF_VIRTIO_BLK_BAR && addr < ANT_HVF_VIRTIO_BLK_BAR + ANT_HVF_VIRTIO_BLK_BAR_SIZE) {
    return ant_hvf_virtio_blk_write(vm, addr, size, value);
  }
  if (addr >= ANT_HVF_VIRTIO_NET_BAR && addr < ANT_HVF_VIRTIO_NET_BAR + ANT_HVF_VIRTIO_NET_BAR_SIZE) {
    return ant_hvf_virtio_net_write(vm, addr, size, value);
  }
  if (addr >= vm->vsock.bar0 && addr < (uint64_t)vm->vsock.bar0 + ANT_HVF_VIRTIO_VSOCK_BAR_SIZE) {
    return ant_hvf_virtio_vsock_write(vm, addr, size, value);
  }
  for (size_t i = 0; i < sizeof(vm->p9) / sizeof(vm->p9[0]); i++) {
    if (addr >= vm->p9[i].bar0 && addr < (uint64_t)vm->p9[i].bar0 + ANT_HVF_VIRTIO_9P_BAR_SIZE) {
      return ant_hvf_virtio_9p_write(vm, &vm->p9[i], addr, size, value);
    }
  }
  if (addr >= ANT_HVF_PCIE_PIO_BASE && addr < ANT_HVF_PCIE_PIO_BASE + 0x10000) return true;
  return false;
}

static int ant_hvf_advance_pc(hv_vcpu_t vcpu) {
  uint64_t pc = 0;
  int rc = ant_hvf_check(hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc), "hv_vcpu_get_reg(PC)");
  if (rc != 0) return rc;
  return ant_hvf_check(hv_vcpu_set_reg(vcpu, HV_REG_PC, pc + 4), "hv_vcpu_set_reg(PC)");
}

static int ant_hvf_handle_mmio(ant_hvf_vm_t *vm, hv_vcpu_exit_exception_t *ex) {
  uint32_t esr = (uint32_t)ex->syndrome;
  unsigned ec = (esr >> ESR_EC_SHIFT) & 0x3f;
  if (ec == ESR_EC_HVC64) {
    uint64_t fn = 0;
    int rc = ant_hvf_check(hv_vcpu_get_reg(vm->vcpu, HV_REG_X0, &fn), "hv_vcpu_get_reg(HVC X0)");
    if (rc != 0) return rc;
    if (fn == 0x84000008u || fn == 0x84000009u) return ANT_HVF_GUEST_SHUTDOWN;
    rc = ant_hvf_check(hv_vcpu_set_reg(vm->vcpu, HV_REG_X0, UINT64_MAX), "hv_vcpu_set_reg(HVC X0)");
    if (rc != 0) return rc;
    return ant_hvf_advance_pc(vm->vcpu);
  }
  if (ec == ESR_EC_HLT && (esr & ESR_HLT_IMM16_MASK) == 0xf000u) {
    if (vm->trace) {
      uint64_t op = 0;
      uint64_t arg = 0;
      uint64_t status = 0;
      hv_vcpu_get_reg(vm->vcpu, HV_REG_X0, &op);
      hv_vcpu_get_reg(vm->vcpu, HV_REG_X1, &arg);
      if (op == 0x18u) ant_hvf_guest_read(vm, arg + 8, &status, sizeof(status));
      fprintf(stderr,
              "sandbox vm: guest semihosting exit op=0x%llx status=%llu\n",
              (unsigned long long)op,
              (unsigned long long)status);
    }
    return ANT_HVF_GUEST_SHUTDOWN;
  }

  if (ec != ESR_EC_DATA_ABORT || !(esr & ESR_DA_ISV)) {
    return -ENOSYS;
  }

  unsigned size = 1u << ((esr >> ESR_DA_SAS_SHIFT) & 0x3);
  unsigned reg = (esr >> ESR_DA_SRT_SHIFT) & 0x1f;
  bool write = (esr & ESR_DA_WNR) != 0;

  if (write) {
    uint64_t value = 0;
    int rc = ant_hvf_check(hv_vcpu_get_reg(vm->vcpu, (hv_reg_t)(HV_REG_X0 + reg), &value),
                           "hv_vcpu_get_reg(mmio write)");
    if (rc != 0) return rc;
    if (vm->trace) {
      fprintf(stderr,
              "sandbox vm: mmio write ipa=0x%llx size=%u x%u=0x%llx\n",
              (unsigned long long)ex->physical_address,
              size,
              reg,
              (unsigned long long)value);
    }
    if (!ant_hvf_mmio_write(vm, ex->physical_address, size, value)) return -ENOSYS;
  } else {
    uint64_t value = 0;
    if (!ant_hvf_mmio_read(vm, ex->physical_address, size, &value)) return -ENOSYS;
    if (vm->trace) {
      fprintf(stderr,
              "sandbox vm: mmio read ipa=0x%llx size=%u x%u=0x%llx\n",
              (unsigned long long)ex->physical_address,
              size,
              reg,
              (unsigned long long)value);
    }
    int rc = ant_hvf_check(hv_vcpu_set_reg(vm->vcpu, (hv_reg_t)(HV_REG_X0 + reg), value),
                           "hv_vcpu_set_reg(mmio read)");
    if (rc != 0) return rc;
  }

  return ant_hvf_advance_pc(vm->vcpu);
}

static int ant_hvf_run(ant_hvf_vm_t *vm, unsigned int timeout_ms) {
  pthread_t timeout_thread;
  pthread_t wake_thread;
  ant_hvf_timeout_t timeout;
  ant_hvf_wake_t wake;
  bool timeout_thread_started = false;
  bool wake_thread_started = false;
  int rc = 0;
  if (timeout_ms > 0) {
    timeout.vm = vm;
    timeout.timeout_ms = timeout_ms;
    int prc = pthread_create(&timeout_thread, NULL, ant_hvf_timeout_thread, &timeout);
    if (prc == 0) {
      timeout_thread_started = true;
    }
  }

  unsigned int wake_interval_ms = 0;
  const char *wake_env = getenv("ANT_SANDBOX_VM_WAKE_MS");
  if (wake_env && wake_env[0]) wake_interval_ms = (unsigned int)strtoul(wake_env, NULL, 10);
  if (wake_interval_ms > 0) {
    wake.vm = vm;
    wake.interval_ms = wake_interval_ms;
    wake.stop = false;
    int prc = pthread_create(&wake_thread, NULL, ant_hvf_wake_thread, &wake);
    if (prc == 0) wake_thread_started = true;
  }

  for (;;) {
    rc = ant_hvf_check(hv_vcpu_run(vm->vcpu), "hv_vcpu_run");
    if (rc != 0) goto done;

    vm->last_exit_reason = vm->vcpu_exit->reason;
    hv_vcpu_get_reg(vm->vcpu, HV_REG_PC, &vm->last_exit_pc);
    if (vm->vcpu_exit->reason == HV_EXIT_REASON_EXCEPTION) {
      vm->last_exit_esr = vm->vcpu_exit->exception.syndrome;
      vm->last_exit_ipa = vm->vcpu_exit->exception.physical_address;
      vm->last_exit_va = vm->vcpu_exit->exception.virtual_address;
    }

    if (vm->vcpu_exit->reason == HV_EXIT_REASON_EXCEPTION) {
      uint32_t ec = (uint32_t)(vm->vcpu_exit->exception.syndrome >> ESR_EC_SHIFT);
      if (ec == ESR_EC_WFX_TRAP) {
        rc = ant_hvf_handle_wfx(vm);
      } else {
        rc = ant_hvf_handle_mmio(vm, &vm->vcpu_exit->exception);
      }
      if (rc == ANT_HVF_GUEST_SHUTDOWN) {
        rc = 0;
        goto done;
      }
      if (rc != 0) {
        uint64_t pc = 0;
        hv_vcpu_get_reg(vm->vcpu, HV_REG_PC, &pc);
        fprintf(stderr,
                "sandbox vm: unhandled guest exception at pc=0x%llx esr=0x%llx ipa=0x%llx va=0x%llx\n",
                (unsigned long long)pc,
                (unsigned long long)vm->vcpu_exit->exception.syndrome,
                (unsigned long long)vm->vcpu_exit->exception.physical_address,
                (unsigned long long)vm->vcpu_exit->exception.virtual_address);
        goto done;
      }
    } else if (vm->vcpu_exit->reason == HV_EXIT_REASON_VTIMER_ACTIVATED) {
      if (vm->trace) fprintf(stderr, "sandbox vm: vtimer activated\n");
      rc = ant_hvf_raise_vtimer(vm, "vtimer activated");
      if (rc != 0) goto done;
    } else if (vm->vcpu_exit->reason == HV_EXIT_REASON_CANCELED) {
      if (vm->timed_out) {
        uint64_t pc = 0;
        uint64_t cntv_ctl = 0;
        uint64_t cntv_cval = 0;
        uint64_t cntvct = 0;
        uint64_t cntfrq = 0;
        uint64_t kas_offset = 0;
        hv_vcpu_get_reg(vm->vcpu, HV_REG_PC, &pc);
        hv_vcpu_get_sys_reg(vm->vcpu, HV_SYS_REG_CNTV_CTL_EL0, &cntv_ctl);
        hv_vcpu_get_sys_reg(vm->vcpu, HV_SYS_REG_CNTV_CVAL_EL0, &cntv_cval);
        hv_vcpu_get_sys_reg(vm->vcpu, ANT_HVF_SYS_REG_CNTVCT_EL0, &cntvct);
        hv_vcpu_get_sys_reg(vm->vcpu, ANT_HVF_SYS_REG_CNTFRQ_EL0, &cntfrq);
        ant_hvf_guest_read(vm, ANT_HVF_NANOS_KAS_OFFSET_SYMBOL, &kas_offset, sizeof(kas_offset));
        fprintf(stderr,
                "sandbox vm: guest timed out at pc=0x%llx low_pc=0x%llx kas_offset=0x%llx last_exit=%u last_pc=0x%llx last_esr=0x%llx last_ipa=0x%llx last_va=0x%llx cntv_ctl=0x%llx cntv_cval=0x%llx cntvct=0x%llx cntfrq=%llu\n",
                (unsigned long long)pc,
                (unsigned long long)(kas_offset ? pc - kas_offset : 0),
                (unsigned long long)kas_offset,
                vm->last_exit_reason,
                (unsigned long long)vm->last_exit_pc,
                (unsigned long long)vm->last_exit_esr,
                (unsigned long long)vm->last_exit_ipa,
                (unsigned long long)vm->last_exit_va,
                (unsigned long long)cntv_ctl,
                (unsigned long long)cntv_cval,
                (unsigned long long)cntvct,
                (unsigned long long)cntfrq);
        rc = -ETIMEDOUT;
        goto done;
      } else {
        if (vm->trace && wake_interval_ms > 0) fprintf(stderr, "sandbox vm: vcpu wake\n");
        rc = ant_hvf_sync_vtimer(vm);
        if (rc != 0) goto done;
        continue;
      }
    } else {
      fprintf(stderr, "sandbox vm: unknown vCPU exit reason %u\n", vm->vcpu_exit->reason);
      rc = -EIO;
      goto done;
    }
  }

done:
  if (wake_thread_started) {
    wake.stop = true;
    hv_vcpus_exit(&vm->vcpu, 1);
    pthread_join(wake_thread, NULL);
  }
  if (timeout_thread_started) {
    if (!vm->timed_out) pthread_cancel(timeout_thread);
    pthread_join(timeout_thread, NULL);
  }
  return rc;
}

static int ant_hvf_start(const ant_sandbox_vm_config_t *config) {
  off_t image_size = 0;
  off_t kernel_size = 0;
  int rc = ant_hvf_check_file("image", config->image_path, &image_size);
  if (rc != 0) return rc;
  rc = ant_hvf_check_file("kernel", config->kernel_path, &kernel_size);
  if (rc != 0) return rc;

  ant_hvf_vm_t vm;
  memset(&vm, 0, sizeof(vm));
  vm.mem_size = config->memory_size ? (size_t)config->memory_size : (1024ull * 1024ull * 1024ull);
  vm.mem_size = ant_align_page(vm.mem_size);
  vm.image_fd = -1;
  vm.image_sectors = (uint64_t)image_size / 512ull;
  vm.blk_bar0 = (uint32_t)ANT_HVF_VIRTIO_BLK_BAR;
  vm.net_bar0 = (uint32_t)ANT_HVF_VIRTIO_NET_BAR;
  vm.net_enabled = getenv("ANT_SANDBOX_VM_NET") != NULL;
  vm.vsock.request_json = config->request_json;
  vm.vsock.bar0 = (uint32_t)ANT_HVF_VIRTIO_VSOCK_BAR;
  vm.p9[0].root = config->shared_dir_path;
  vm.p9[0].tag = "0";
  vm.p9[0].bar0 = (uint32_t)ANT_HVF_VIRTIO_9P0_BAR;
  vm.p9[0].irq = ANT_HVF_VIRTIO_9P0_IRQ;
  vm.cntfrq = ant_hvf_host_cntfrq();
  vm.trace = getenv("ANT_SANDBOX_VM_TRACE") != NULL;

  bool vm_created = false;
  bool mem_mapped = false;
  bool vcpu_created = false;

  if (vm.mem_size < 64ull * 1024ull * 1024ull) {
    return -EINVAL;
  }

  vm.image_fd = open(config->image_path, O_RDWR);
  if (vm.image_fd < 0) {
    rc = -errno;
    goto done;
  }

  vm.host_mem = mmap(NULL, vm.mem_size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
  if (vm.host_mem == MAP_FAILED) {
    rc = -errno;
    return rc;
  }

  rc = ant_hvf_check(hv_vm_create(NULL), "hv_vm_create");
  if (rc != 0) goto done;
  vm_created = true;

  rc = ant_hvf_create_gic();
  if (rc != 0) goto done;

  rc = ant_hvf_check(
    hv_vm_map(vm.host_mem, ANT_HVF_GUEST_BASE, vm.mem_size,
              HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC),
    "hv_vm_map");
  if (rc != 0) goto done;
  mem_mapped = true;

  rc = ant_hvf_load_kernel(&vm, config->kernel_path);
  if (rc != 0) goto done;

  rc = ant_hvf_build_dtb(&vm);
  if (rc != 0) goto done;

  rc = ant_hvf_check(hv_vcpu_create(&vm.vcpu, &vm.vcpu_exit, NULL), "hv_vcpu_create");
  if (rc != 0) goto done;
  vcpu_created = true;

  rc = ant_hvf_init_vcpu(&vm);
  if (rc != 0) goto done;

  fprintf(stderr,
          "sandbox vm: booting cached Nanos kernel via Hypervisor.framework; image=%s (%lld bytes) kernel=%s (%lld bytes)\n",
          config->image_path,
          (long long)image_size,
          config->kernel_path,
          (long long)kernel_size);

  unsigned int timeout_ms = config->timeout_ms ? config->timeout_ms : 60000;
  const char *timeout_env = getenv("ANT_SANDBOX_VM_TIMEOUT_MS");
  if (timeout_env && timeout_env[0]) timeout_ms = (unsigned int)strtoul(timeout_env, NULL, 10);

  rc = ant_hvf_run(&vm, timeout_ms);
  if (rc == -ETIMEDOUT) fprintf(stderr, "sandbox vm: guest timed out\n");

done:
  if (vcpu_created) {
    int destroy_rc = ant_hvf_check(hv_vcpu_destroy(vm.vcpu), "hv_vcpu_destroy");
    if (rc == 0) rc = destroy_rc;
  }
  if (mem_mapped) {
    int unmap_rc = ant_hvf_check(hv_vm_unmap(ANT_HVF_GUEST_BASE, vm.mem_size), "hv_vm_unmap");
    if (rc == 0) rc = unmap_rc;
  }
  if (vm_created) {
    int destroy_rc = ant_hvf_check(hv_vm_destroy(), "hv_vm_destroy");
    if (rc == 0) rc = destroy_rc;
  }
  if (vm.image_fd >= 0) close(vm.image_fd);
  if (vm.host_mem && vm.host_mem != MAP_FAILED) munmap(vm.host_mem, vm.mem_size);
  return rc;
}

#else

static int ant_hvf_start(const ant_sandbox_vm_config_t *config) {
  (void)config;
  fprintf(stderr, "sandbox vm: Hypervisor.framework backend requires Apple Silicon\n");
  return -ENOSYS;
}

#endif

const ant_sandbox_vm_backend_t ant_sandbox_vm_darwin_backend = {
  .name = "hypervisor.framework",
  .start = ant_hvf_start,
};
