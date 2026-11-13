#include "shared/backend.h" // IWYU pragma: keep

ant_devices_t *ant_hvf_vm_devices(const ant_hvf_vm_t *vm) {
  return vm->devices;
}

bool ant_hvf_vm_verbose(const ant_hvf_vm_t *vm) {
  return vm->verbose;
}

void ant_hvf_vm_set_vsock_wake(ant_hvf_vm_t *vm, bool pending) {
  atomic_store_explicit(&vm->vsock_wake_pending, pending, memory_order_release);
}

void ant_hvf_vm_kick_if_running(ant_hvf_vm_t *vm) {
  if (atomic_load_explicit(&vm->vcpu_running, memory_order_acquire)) ant_hvf_wake_vcpu(vm);
}

bool ant_hvf_vm_frame_handler(ant_hvf_vm_t *vm, uint8_t type, const unsigned char *payload, uint32_t len) {
  return vm->frame_handler && vm->frame_handler(type, payload, len, vm->frame_handler_user);
}

int ant_hvf_vm_image_fd(const ant_hvf_vm_t *vm) {
  return vm->image_fd;
}

uint64_t ant_hvf_vm_image_sectors(const ant_hvf_vm_t *vm) {
  return vm->image_sectors;
}

int ant_hvf_check_file(const char *kind, const char *path, off_t *size_out) {
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

void *ant_hvf_guest_ptr(ant_hvf_vm_t *vm, uint64_t guest_addr, size_t len) {
  if (guest_addr < ANT_HVF_GUEST_BASE) return NULL;
  uint64_t off = guest_addr - ANT_HVF_GUEST_BASE;
  if (off > vm->mem_size || len > vm->mem_size - (size_t)off) return NULL;
  return (unsigned char *)vm->host_mem + off;
}

int ant_read_all(int fd, void *buf, size_t len, off_t off) {
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

static int kernel_load_segment(ant_hvf_vm_t *vm, int fd, const char *path, const ant_elf64_phdr_t *ph) {
  if (ph->type != 1) return 0;

  if (ph->filesz > ph->memsz || ph->memsz > SIZE_MAX) {
    fprintf(stderr, "sandbox vm: invalid ELF segment sizes in %s\n", path);
    return -EINVAL;
  }

  void *dest = ant_hvf_guest_ptr(vm, ph->paddr, (size_t)ph->memsz);
  if (!dest) {
    fprintf(stderr, "sandbox vm: kernel segment outside guest memory at 0x%llx\n", (unsigned long long)ph->paddr);
    return -EINVAL;
  }

  memset(dest, 0, (size_t)ph->memsz);
  return ant_read_all(fd, dest, (size_t)ph->filesz, (off_t)ph->offset);
}

static int kernel_load(ant_hvf_vm_t *vm, int fd, const char *path) {
  ant_elf64_ehdr_t eh;
  
  int rc = ant_read_all(fd, &eh, sizeof(eh), 0);
  if (rc != 0) return rc;

  if (memcmp(eh.ident, "\177ELF", 4) != 0 || eh.ident[4] != 2 || eh.machine != ANT_HVF_ELF_MACHINE) {
    fprintf(stderr, "sandbox vm: sandbox kernel is not an " ANT_HVF_ELF_MACHINE_NAME " ELF image: %s\n", path);
    return -EINVAL;
  }

  vm->kernel_entry = eh.entry;

  for (uint16_t i = 0; i < eh.phnum; i++) {
    ant_elf64_phdr_t ph;
    rc = ant_read_all(fd, &ph, sizeof(ph), (off_t)(eh.phoff + (uint64_t)i * eh.phentsize));
    if (rc == 0) rc = kernel_load_segment(vm, fd, path, &ph);
    if (rc != 0) return rc;
  }

  return 0;
}

int ant_hvf_load_kernel(ant_hvf_vm_t *vm, const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -errno;

  int rc = kernel_load(vm, fd, path);
  close(fd);
  
  return rc;
}
