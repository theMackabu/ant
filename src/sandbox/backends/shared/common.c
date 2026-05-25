#include "sandbox_backend/backend.h" // IWYU pragma: keep

uint32_t ant_bswap32(uint32_t x) {
  return 
    ((x & 0x000000ffu) << 24) |
    ((x & 0x0000ff00u) << 8)  |
    ((x & 0x00ff0000u) >> 8)  |
    ((x & 0xff000000u) >> 24);
}

uint64_t ant_bswap64(uint64_t x) {
  return ((uint64_t)ant_bswap32((uint32_t)x) << 32) | ant_bswap32((uint32_t)(x >> 32));
}

size_t ant_align4(size_t n) {
  return (n + 3u) & ~3u;
}

size_t ant_align_page(size_t n) {
  return (n + (size_t)ANT_HVF_PAGE_SIZE - 1u) & ~((size_t)ANT_HVF_PAGE_SIZE - 1u);
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

uint16_t ant_hvf_load16(const unsigned char *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t ant_hvf_load32(const unsigned char *p) {
  return
    (uint32_t)p[0]         |
    ((uint32_t)p[1] << 8)  |
    ((uint32_t)p[2] << 16) |
    ((uint32_t)p[3] << 24);
}

uint64_t ant_hvf_load64(const unsigned char *p) {
  return (uint64_t)ant_hvf_load32(p) | ((uint64_t)ant_hvf_load32(p + 4) << 32);
}

void ant_hvf_store16(unsigned char *p, uint16_t value) {
  p[0] = (unsigned char)value;
  p[1] = (unsigned char)(value >> 8);
}

void ant_hvf_store32(unsigned char *p, uint32_t value) {
  p[0] = (unsigned char)value;
  p[1] = (unsigned char)(value >> 8);
  p[2] = (unsigned char)(value >> 16);
  p[3] = (unsigned char)(value >> 24);
}

void ant_hvf_store64(unsigned char *p, uint64_t value) {
  ant_hvf_store32(p, (uint32_t)value);
  ant_hvf_store32(p + 4, (uint32_t)(value >> 32));
}

void *ant_hvf_guest_ptr(ant_hvf_vm_t *vm, uint64_t guest_addr, size_t len) {
  if (guest_addr < ANT_HVF_GUEST_BASE) return NULL;
  uint64_t off = guest_addr - ANT_HVF_GUEST_BASE;
  if (off > vm->mem_size || len > vm->mem_size - (size_t)off) return NULL;
  return (unsigned char *)vm->host_mem + off;
}

int ant_hvf_guest_read(ant_hvf_vm_t *vm, uint64_t guest_addr, void *out, size_t len) {
  void *src = ant_hvf_guest_ptr(vm, guest_addr, len);
  if (!src) return -EFAULT;
  memcpy(out, src, len);
  return 0;
}

int ant_hvf_guest_write(ant_hvf_vm_t *vm, uint64_t guest_addr, const void *src, size_t len) {
  void *dest = ant_hvf_guest_ptr(vm, guest_addr, len);
  if (!dest) return -EFAULT;
  memcpy(dest, src, len);
  return 0;
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

int ant_hvf_load_kernel(ant_hvf_vm_t *vm, const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -errno;

  ant_elf64_ehdr_t eh;
  int rc = ant_read_all(fd, &eh, sizeof(eh), 0);
  if (rc != 0) {
    close(fd);
    return rc;
  }

  if (memcmp(eh.ident, "\177ELF", 4) != 0 || eh.ident[4] != 2 || eh.machine != ANT_HVF_ELF_MACHINE) {
    close(fd);
    fprintf(stderr, "sandbox vm: sandbox kernel is not an " ANT_HVF_ELF_MACHINE_NAME " ELF image: %s\n", path);
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
    if (ph.filesz > ph.memsz || ph.memsz > SIZE_MAX || ph.filesz > SIZE_MAX) {
      close(fd);
      fprintf(stderr, "sandbox vm: invalid ELF segment sizes in %s\n", path);
      return -EINVAL;
    }
    void *dest = ant_hvf_guest_ptr(vm, ph.paddr, (size_t)ph.memsz);
    if (!dest) {
      close(fd);
      fprintf(
        stderr, "sandbox vm: kernel segment outside guest memory at 0x%llx\n",
        (unsigned long long)ph.paddr
      );
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
uint64_t ant_hvf_select_width(uint64_t value, unsigned offset, unsigned size) {
  return (value >> (offset * 8u)) & (size >= 8 ? UINT64_MAX : ((1ull << (size * 8u)) - 1ull));
}

void ant_hvf_assign_width(uint32_t *target, unsigned offset, unsigned size, uint64_t value) {
  uint32_t mask = size >= 4 ? UINT32_MAX : ((1u << (size * 8u)) - 1u);
  unsigned shift = offset * 8u;
  *target = (*target & ~(mask << shift)) | (((uint32_t)value & mask) << shift);
}

void ant_hvf_assign_width16(uint16_t *target, unsigned offset, unsigned size, uint64_t value) {
  uint16_t mask = size >= 2 ? UINT16_MAX : (uint16_t)((1u << (size * 8u)) - 1u);
  unsigned shift = offset * 8u;
  *target = (uint16_t)((*target & ~(mask << shift)) | (((uint16_t)value & mask) << shift));
}

void ant_hvf_assign_width64(uint64_t *target, unsigned offset, unsigned size, uint64_t value) {
  uint64_t mask = size >= 8 ? UINT64_MAX : ((1ull << (size * 8u)) - 1ull);
  unsigned shift = offset * 8u;
  *target = (*target & ~(mask << shift)) | ((value & mask) << shift);
}
