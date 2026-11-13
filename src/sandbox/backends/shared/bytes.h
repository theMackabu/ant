#pragma once

#include "shared/backend.h" // IWYU pragma: keep

static inline uint32_t ant_bswap32(uint32_t x) {
  return ((x & 0x000000ffu) << 24) | ((x & 0x0000ff00u) << 8) | ((x & 0x00ff0000u) >> 8) | ((x & 0xff000000u) >> 24);
}

static inline uint64_t ant_bswap64(uint64_t x) {
  return ((uint64_t)ant_bswap32((uint32_t)x) << 32) | ant_bswap32((uint32_t)(x >> 32));
}

static inline size_t ant_align4(size_t n) {
  return (n + 3u) & ~3u;
}

static inline size_t ant_align_page(size_t n) {
  return (n + (size_t)ANT_HVF_PAGE_SIZE - 1u) & ~((size_t)ANT_HVF_PAGE_SIZE - 1u);
}

static inline uint16_t ant_hvf_load16(const unsigned char *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t ant_hvf_load32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t ant_hvf_load64(const unsigned char *p) {
  return (uint64_t)ant_hvf_load32(p) | ((uint64_t)ant_hvf_load32(p + 4) << 32);
}

static inline void ant_hvf_store16(unsigned char *p, uint16_t value) {
  p[0] = (unsigned char)value;
  p[1] = (unsigned char)(value >> 8);
}

static inline void ant_hvf_store32(unsigned char *p, uint32_t value) {
  p[0] = (unsigned char)value;
  p[1] = (unsigned char)(value >> 8);
  p[2] = (unsigned char)(value >> 16);
  p[3] = (unsigned char)(value >> 24);
}

static inline void ant_hvf_store64(unsigned char *p, uint64_t value) {
  ant_hvf_store32(p, (uint32_t)value);
  ant_hvf_store32(p + 4, (uint32_t)(value >> 32));
}

static inline uint64_t ant_hvf_select_width(uint64_t value, unsigned offset, unsigned size) {
  return (value >> (offset * 8u)) & (size >= 8 ? UINT64_MAX : ((1ull << (size * 8u)) - 1ull));
}

static inline int ant_hvf_guest_read(ant_hvf_vm_t *vm, uint64_t guest_addr, void *out, size_t len) {
  void *src = ant_hvf_guest_ptr(vm, guest_addr, len);
  if (!src) return -EFAULT;
  memcpy(out, src, len);
  return 0;
}

static inline int ant_hvf_guest_write(ant_hvf_vm_t *vm, uint64_t guest_addr, const void *src, size_t len) {
  void *dest = ant_hvf_guest_ptr(vm, guest_addr, len);
  if (!dest) return -EFAULT;
  memcpy(dest, src, len);
  return 0;
}
