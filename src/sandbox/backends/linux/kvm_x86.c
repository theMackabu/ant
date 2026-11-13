#include "kvm_internal.h" // IWYU pragma: keep
#if defined(__linux__) && defined(__x86_64__)

static constexpr uint32_t ANT_KVM_ELF_PT_NOTE = 4u;
static constexpr uint32_t ANT_KVM_XEN_ELFNOTE_PHYS32_ENTRY = 18u;
static constexpr uint64_t ANT_KVM_ELF_NOTE_MAX = 64ull * 1024ull;

static int ant_kvm_write_pvh_start(ant_hvf_vm_t *vm) {
  ant_kvm_hvm_start_info_t start = {
    .magic = ANT_KVM_HVM_START_MAGIC,
    .version = 1,
    .memmap_paddr = ANT_HVF_PVH_MEMMAP,
    .memmap_entries = 1,
  };
  
  ant_kvm_hvm_memmap_entry_t mem = {
    .addr = ANT_HVF_PVH_START_INFO,
    .size = vm->mem_size - ANT_HVF_PVH_START_INFO,
    .type = ANT_KVM_HVM_MEMMAP_TYPE_RAM,
  };
  
  int rc = ant_hvf_guest_write(vm, ANT_HVF_PVH_START_INFO, &start, sizeof(start));
  if (rc != 0) return rc;
  
  return ant_hvf_guest_write(vm, ANT_HVF_PVH_MEMMAP, &mem, sizeof(mem));
}

static void ant_kvm_segment(
  struct kvm_segment *seg,
  uint16_t selector,
  uint8_t type, bool code
) {
  memset(seg, 0, sizeof(*seg));
  seg->base = 0;
  seg->limit = 0xffffffffu;
  seg->selector = selector;
  seg->type = type;
  seg->present = 1;
  seg->dpl = 0;
  seg->db = 1;
  seg->s = 1;
  seg->l = 0;
  seg->g = 1;
}

static int ant_kvm_init_vcpu(ant_hvf_vm_t *vm) {
  struct kvm_sregs sregs;
  if (ioctl(vm->vcpu_fd, KVM_GET_SREGS, &sregs) < 0) return -errno;

  ant_kvm_segment(&sregs.cs, 0x08, 0x0b, true);
  ant_kvm_segment(&sregs.ds, 0x10, 0x03, false);
  sregs.es = sregs.ds;
  sregs.fs = sregs.ds;
  sregs.gs = sregs.ds;
  sregs.ss = sregs.ds;
  sregs.cr0 |= 1u;
  if (ioctl(vm->vcpu_fd, KVM_SET_SREGS, &sregs) < 0) return -errno;

  struct kvm_regs regs = {
    .rip = vm->pvh_entry32,
    .rbx = ANT_HVF_PVH_START_INFO,
    .rsp = 0x90000,
    .rflags = 0x2,
  };
  if (ioctl(vm->vcpu_fd, KVM_SET_REGS, &regs) < 0) return -errno;
  return 0;
}

static int ant_kvm_set_cpuid(ant_hvf_vm_t *vm) {
  int nent = 128;
  for (;;) {
    size_t bytes = sizeof(struct kvm_cpuid2) + (size_t)nent * sizeof(struct kvm_cpuid_entry2);
    struct kvm_cpuid2 *cpuid = calloc(1, bytes);
    if (!cpuid) return -ENOMEM;
    cpuid->nent = (uint32_t)nent;
    if (ioctl(vm->kvm_fd, KVM_GET_SUPPORTED_CPUID, cpuid) == 0) {
      int rc = ioctl(vm->vcpu_fd, KVM_SET_CPUID2, cpuid) == 0 ? 0 : -errno;
      free(cpuid);
      return rc;
    }
    int err = errno;
    free(cpuid);
    if (err != E2BIG || nent > 4096) return -err;
    nent *= 2;
  }
}

static uint32_t ant_kvm_pci_cfg_read(ant_hvf_vm_t *vm, uint16_t port, unsigned size) {
  if ((vm->pci_addr & 0x80000000u) == 0 || port < ANT_KVM_PCI_DATA_PORT ||
      port + size > ANT_KVM_PCI_DATA_PORT + 4u) {
    return UINT32_MAX;
  }
  unsigned bus = (vm->pci_addr >> 16) & 0xffu;
  unsigned slot = (vm->pci_addr >> 11) & 0x1fu;
  unsigned fn = (vm->pci_addr >> 8) & 0x7u;
  unsigned reg = (vm->pci_addr & 0xfcu) + (unsigned)(port - ANT_KVM_PCI_DATA_PORT);
  uint32_t word = ant_hvf_pci_config_read32(vm, bus, slot, fn, reg);
  return (uint32_t)ant_hvf_select_width(word, reg & 3u, size);
}

static void ant_kvm_pci_cfg_write(ant_hvf_vm_t *vm, uint16_t port, unsigned size, uint32_t value) {
  if ((vm->pci_addr & 0x80000000u) == 0 || port < ANT_KVM_PCI_DATA_PORT ||
      port + size > ANT_KVM_PCI_DATA_PORT + 4u) {
    return;
  }
  unsigned bus = (vm->pci_addr >> 16) & 0xffu;
  unsigned slot = (vm->pci_addr >> 11) & 0x1fu;
  unsigned fn = (vm->pci_addr >> 8) & 0x7u;
  unsigned reg = (vm->pci_addr & 0xfcu) + (unsigned)(port - ANT_KVM_PCI_DATA_PORT);
  ant_hvf_pci_config_write(vm, bus, slot, fn, reg, size, value);
}

static uint32_t ant_kvm_io_read(ant_hvf_vm_t *vm, uint16_t port, unsigned size) {
  if (port == ANT_KVM_PCI_ADDR_PORT && size == 4) return vm->pci_addr;
  if (port >= ANT_KVM_PCI_DATA_PORT && port < ANT_KVM_PCI_DATA_PORT + 4u) {
    return ant_kvm_pci_cfg_read(vm, port, size);
  }
  if (port >= ANT_KVM_COM1_PORT && port < ANT_KVM_COM1_PORT + 8u) {
    unsigned off = port - ANT_KVM_COM1_PORT;
    if (off == 5) return 0x60u;
    if (off == 6) return 0xb0u;
    return 0;
  }
  if (port == 0x70 || port == 0x71 || port == 0x80) return 0;
  return UINT32_MAX;
}

static void ant_kvm_io_write(ant_hvf_vm_t *vm, uint16_t port, unsigned size, uint32_t value) {
  if (port == ANT_KVM_PCI_ADDR_PORT && size == 4) {
    vm->pci_addr = value;
    return;
  }
  if (port >= ANT_KVM_PCI_DATA_PORT && port < ANT_KVM_PCI_DATA_PORT + 4u) {
    ant_kvm_pci_cfg_write(vm, port, size, value);
    return;
  }
  if (port == ANT_KVM_COM1_PORT) {
    ant_hvf_uart_put(vm, (uint8_t)value);
    return;
  }
}

static int ant_kvm_handle_io(ant_hvf_vm_t *vm) {
  struct kvm_run *run = vm->run;
  unsigned char *data = (unsigned char *)run + run->io.data_offset;
  for (uint32_t i = 0; i < run->io.count; i++) {
    unsigned char *p = data + (size_t)i * run->io.size;
    if (run->io.direction == ANT_KVM_PIO_OUT) {
      uint32_t value = 0;
      memcpy(&value, p, run->io.size);
      ant_kvm_io_write(vm, run->io.port, run->io.size, value);
    } else {
      uint32_t value = ant_kvm_io_read(vm, run->io.port, run->io.size);
      memcpy(p, &value, run->io.size);
    }
  }
  return 0;
}

int ant_kvm_arch_handle_mmio(ant_hvf_vm_t *vm) {
  struct kvm_run *run = vm->run;
  ant_hvf_virtio_device_t *dev = ant_hvf_virtio_for_bar(vm, run->mmio.phys_addr);
  
  if (!dev) {
    if (vm->verbose) ant_hvf_verbosef(vm,
      "unsupported MMIO %s addr=0x%llx len=%u",
      run->mmio.is_write ? "write" : "read",
      (unsigned long long)run->mmio.phys_addr,
      run->mmio.len
    );
    if (!run->mmio.is_write) memset(run->mmio.data, 0xff, run->mmio.len);
    return 0;
  }
  
  uint64_t off = run->mmio.phys_addr - ant_hvf_virtio_bar0(dev);
  if (run->mmio.is_write) {
    uint64_t value = 0;
    memcpy(&value, run->mmio.data, run->mmio.len);
    return ant_hvf_virtio_common_write(vm, dev, off, run->mmio.len, value) ? 0 : -EIO;
  }
  
  uint64_t value = 0;
  if (!ant_hvf_virtio_common_read(vm, dev, off, run->mmio.len, &value)) return -EIO;
  memcpy(run->mmio.data, &value, run->mmio.len);
  
  return 0;
}

static int ant_kvm_find_pvh_entry(const char *path, uint64_t *entry_out) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -errno;
  
  ant_elf64_ehdr_t eh;
  int rc = ant_read_all(fd, &eh, sizeof(eh), 0);
  if (rc != 0) goto done;
  
  rc = -ENOENT;
  if (eh.phoff == 0 || eh.phentsize < sizeof(ant_elf64_phdr_t)) goto done;

  for (uint16_t i = 0; i < eh.phnum; i++) {
    ant_elf64_phdr_t ph;
    rc = ant_read_all(fd, &ph, sizeof(ph), (off_t)(eh.phoff + (uint64_t)i * eh.phentsize));
    if (rc != 0) goto done;
    rc = -ENOENT;
    if (ph.type != ANT_KVM_ELF_PT_NOTE || ph.filesz == 0 || ph.filesz > ANT_KVM_ELF_NOTE_MAX) continue;

    unsigned char *notes = malloc((size_t)ph.filesz);
    if (!notes) { rc = -ENOMEM; goto done; }
    rc = ant_read_all(fd, notes, (size_t)ph.filesz, (off_t)ph.offset);
    if (rc != 0) { free(notes); goto done; }

    rc = -ENOENT;
    uint64_t off = 0;
    while (ph.filesz - off >= 12u) {
      uint32_t namesz = ant_hvf_load32(notes + off);
      uint32_t descsz = ant_hvf_load32(notes + off + 4u);
      uint32_t type = ant_hvf_load32(notes + off + 8u);
      uint64_t name_off = off + 12u;
      uint64_t desc_off = name_off + ant_align4(namesz);
      uint64_t next = desc_off + ant_align4(descsz);
      if (next > ph.filesz || next <= off) break;
      if (type == ANT_KVM_XEN_ELFNOTE_PHYS32_ENTRY && namesz == 4u &&
          memcmp(notes + name_off, "Xen", 4) == 0 && (descsz == 4u || descsz == 8u)) {
        *entry_out = descsz == 4u ? ant_hvf_load32(notes + desc_off) : ant_hvf_load64(notes + desc_off);
        rc = 0;
        break;
      }
      off = next;
    }
    free(notes);
    if (rc == 0) goto done;
  }

done:
  close(fd);
  return rc;
}

int ant_kvm_arch_handle_exit(ant_hvf_vm_t *vm, bool *stop) {
  (void)stop;
  if (vm->run->exit_reason != KVM_EXIT_IO) return -ENOTSUP;
  return ant_kvm_handle_io(vm);
}

void ant_kvm_arch_report_timeout(ant_hvf_vm_t *vm) {
  (void)vm;
}

int ant_kvm_arch_setup_vm(ant_kvm_session_t *session) {
  ant_hvf_vm_t *vm = &session->vm;
  int tss = 0xfffbd000;
  (void)ioctl(vm->vm_fd, KVM_SET_TSS_ADDR, tss);
  if (ioctl(vm->vm_fd, KVM_CREATE_IRQCHIP, 0) < 0) return -errno;

  session->irqchip_created = true;
  struct kvm_pit_config pit = { .flags = KVM_PIT_SPEAKER_DUMMY };
  (void)ioctl(vm->vm_fd, KVM_CREATE_PIT2, &pit);
  return 0;
}

int ant_kvm_arch_prepare_boot(ant_hvf_vm_t *vm, const char *kernel_path) {
  int rc = ant_kvm_find_pvh_entry(kernel_path, &vm->pvh_entry32);
  if (rc != 0) {
    fprintf(stderr, "sandbox vm: sandbox kernel has no PVH entry note\n");
    return rc;
  }

  ant_hvf_verbosef(vm, "pvh_start32=0x%llx", (unsigned long long)vm->pvh_entry32);
  return ant_kvm_write_pvh_start(vm);
}

int ant_kvm_arch_setup_vcpu(ant_kvm_session_t *session) {
  int rc = ant_kvm_set_cpuid(&session->vm);
  return rc != 0 ? rc : ant_kvm_init_vcpu(&session->vm);
}

#endif
