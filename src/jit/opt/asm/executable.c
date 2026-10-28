#include "executable.h"
#include <stdlib.h>
#include <string.h>

static constexpr size_t ASM_CODE_ALIGN = 64;
static constexpr size_t ASM_CODE_CHUNK = 256 * 1024;

#if defined(__aarch64__)
static constexpr int ASM_CODE_TRAP_BYTE = 0x00;
#else
static constexpr int ASM_CODE_TRAP_BYTE = 0xcc;
#endif

typedef struct {
  uint8_t *start;
  size_t size;
} asm_code_span_t;

typedef struct {
  uint8_t *base;
  size_t size;
  size_t live;
} asm_code_chunk_t;

struct asm_code_heap {
  asm_code_chunk_t *chunks;
  uint32_t chunk_count, chunk_cap;
  asm_code_span_t *spans;
  uint32_t span_count, span_cap;
  uint32_t blocks;
};

static inline size_t executable_size(size_t len) {
  size_t size = len ? len : 1;
  return (size + 16383) & ~(size_t)16383;
}

#if defined(_WIN32)

#include <windows.h>

static void *code_map(size_t size) {
  return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READ);
}

static void code_unmap(void *address, size_t size) {
  (void)size;
  VirtualFree(address, 0, MEM_RELEASE);
}

static bool code_open(uint8_t *at, size_t len) {
  DWORD old;
  return VirtualProtect(at, len, PAGE_READWRITE, &old) != 0;
}

static bool code_close(uint8_t *at, size_t len) {
  DWORD old;
  bool ok = VirtualProtect(at, len, PAGE_EXECUTE_READ, &old) != 0;
  FlushInstructionCache(GetCurrentProcess(), at, len);
  return ok;
}

#else

#include <sys/mman.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#endif

static void *code_map(size_t size) {
#if defined(__APPLE__)
  void *address = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
#else
  void *address = mmap(NULL, size, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
  return address == MAP_FAILED ? NULL : address;
}

static void code_unmap(void *address, size_t size) {
  munmap(address, size);
}

#if !defined(__APPLE__)
static void code_pages(uint8_t *at, size_t len, uint8_t **start, size_t *size) {
  static size_t page;
  if (!page) page = (size_t)sysconf(_SC_PAGESIZE);
  uintptr_t lo = (uintptr_t)at & ~(uintptr_t)(page - 1);
  uintptr_t hi = ((uintptr_t)at + len + page - 1) & ~(uintptr_t)(page - 1);
  *start = (uint8_t *)lo;
  *size = hi - lo;
}
#endif

static bool code_open(uint8_t *at, size_t len) {
#if defined(__APPLE__)
  (void)at;
  (void)len;
  pthread_jit_write_protect_np(0);
  return true;
#else
  uint8_t *start;
  size_t size;
  code_pages(at, len, &start, &size);
  return mprotect(start, size, PROT_READ | PROT_WRITE) == 0;
#endif
}

static bool code_close(uint8_t *at, size_t len) {
#if defined(__APPLE__)
  pthread_jit_write_protect_np(1);
  sys_icache_invalidate(at, len);
  return true;
#else
  uint8_t *start;
  size_t size;
  code_pages(at, len, &start, &size);
  bool ok = mprotect(start, size, PROT_READ | PROT_EXEC) == 0;
  __builtin___clear_cache((char *)at, (char *)at + len);
  return ok;
#endif
}

#endif

static bool code_copy(uint8_t *at, const uint8_t *bytes, size_t len, size_t size) {
  if (!code_open(at, size)) return false;
  memcpy(at, bytes, len);
  if (size > len) memset(at + len, ASM_CODE_TRAP_BYTE, size - len);
  return code_close(at, size);
}

bool asm_executable_init(asm_executable_t *code, const uint8_t *bytes, size_t len) {
  size_t size = executable_size(len);
  uint8_t *address = code_map(size);
  if (!address) return false;
  if (!code_copy(address, bytes, len, len)) {
    code_unmap(address, size);
    return false;
  }
  *code = (asm_executable_t){ .address = address, .size = size };
  return true;
}

bool asm_executable_patch(asm_executable_t *code, size_t offset, const uint8_t *bytes, size_t len) {
  uint8_t *at = (uint8_t *)code->address + offset;
  if (!code->address || offset + len > code->size || !code_open(at, len)) return false;
  memcpy(at, bytes, len);
  return code_close(at, len);
}

static bool asm_code_heap_grow(void **items, uint32_t *cap, uint32_t need, size_t elem) {
  if (need <= *cap) return true;
  uint32_t next = *cap ? *cap * 2 : 16;
  while (next < need) next *= 2;
  void *grown = realloc(*items, next * elem);
  if (!grown) return false;
  *items = grown;
  *cap = next;
  return true;
}

asm_code_heap_t *asm_code_heap_new(void) {
  return calloc(1, sizeof(asm_code_heap_t));
}

void asm_code_heap_destroy(asm_code_heap_t *heap) {
  if (!heap) return;
  for (uint32_t i = 0; i < heap->chunk_count; i++) code_unmap(heap->chunks[i].base, heap->chunks[i].size);
  free(heap->chunks);
  free(heap->spans);
  free(heap);
}

static asm_code_chunk_t *asm_code_heap_chunk_of(asm_code_heap_t *heap, const uint8_t *at) {
  for (uint32_t i = 0; i < heap->chunk_count; i++) {
    asm_code_chunk_t *chunk = &heap->chunks[i];
    if (at >= chunk->base && at < chunk->base + chunk->size) return chunk;
  }
  return NULL;
}

static uint32_t asm_code_heap_span_index(const asm_code_heap_t *heap, const uint8_t *at) {
  uint32_t low = 0, high = heap->span_count;
  while (low < high) {
    uint32_t mid = (low + high) / 2;
    if (heap->spans[mid].start < at) low = mid + 1;
    else high = mid;
  }
  return low;
}

static bool asm_code_heap_insert_span(asm_code_heap_t *heap, uint32_t index, asm_code_span_t span) {
  if (!asm_code_heap_grow((void **)&heap->spans, &heap->span_cap, heap->span_count + 1, sizeof *heap->spans)) return false;
  memmove(&heap->spans[index + 1], &heap->spans[index], (heap->span_count - index) * sizeof *heap->spans);
  heap->spans[index] = span;
  heap->span_count++;
  return true;
}

static void asm_code_heap_remove_span(asm_code_heap_t *heap, uint32_t index) {
  memmove(&heap->spans[index], &heap->spans[index + 1], (heap->span_count - index - 1) * sizeof *heap->spans);
  heap->span_count--;
}

static bool asm_code_heap_add_chunk(asm_code_heap_t *heap, size_t need) {
  size_t size = need > ASM_CODE_CHUNK ? executable_size(need) : ASM_CODE_CHUNK;
  if (!asm_code_heap_grow((void **)&heap->chunks, &heap->chunk_cap, heap->chunk_count + 1, sizeof *heap->chunks)) return false;
  uint8_t *base = code_map(size);
  if (!base) return false;
  asm_code_span_t span = {.start = base, .size = size};
  if (!asm_code_heap_insert_span(heap, asm_code_heap_span_index(heap, base), span)) {
    code_unmap(base, size);
    return false;
  }
  heap->chunks[heap->chunk_count++] = (asm_code_chunk_t){.base = base, .size = size};
  return true;
}

static uint8_t *asm_code_heap_take(asm_code_heap_t *heap, size_t size) {
  for (uint32_t i = 0; i < heap->span_count; i++) {
    asm_code_span_t *span = &heap->spans[i];
    if (span->size < size) continue;
    uint8_t *at = span->start;
    span->start += size;
    span->size -= size;
    if (!span->size) asm_code_heap_remove_span(heap, i);
    return at;
  }
  return NULL;
}

bool asm_code_heap_place(asm_code_heap_t *heap, asm_executable_t *code, const uint8_t *bytes, size_t len) {
  size_t size = ((len ? len : 1) + ASM_CODE_ALIGN - 1) & ~(ASM_CODE_ALIGN - 1);
  uint8_t *at = asm_code_heap_take(heap, size);
  if (!at && asm_code_heap_add_chunk(heap, size)) at = asm_code_heap_take(heap, size);
  if (!at) return false;
  asm_code_chunk_t *chunk = asm_code_heap_chunk_of(heap, at);
  chunk->live += size;
  heap->blocks++;
  *code = (asm_executable_t){ .address = at, .size = size, .heap = heap };
  if (code_copy(at, bytes, len, size)) return true;
  asm_executable_free(code);
  return false;
}

static void asm_code_heap_release(asm_code_heap_t *heap, uint8_t *at, size_t size) {
  asm_code_chunk_t *chunk = asm_code_heap_chunk_of(heap, at);
  if (!chunk) return;
  if (code_open(at, size)) {
    memset(at, ASM_CODE_TRAP_BYTE, size);
    code_close(at, size);
  }
  chunk->live -= size;
  heap->blocks--;
  uint32_t index = asm_code_heap_span_index(heap, at);
  uint8_t *chunk_end = chunk->base + chunk->size;
  asm_code_span_t span = {.start = at, .size = size};
  if (index > 0) {
    asm_code_span_t *before = &heap->spans[index - 1];
    if (before->start >= chunk->base && before->start + before->size == at) {
      span.start = before->start;
      span.size += before->size;
      asm_code_heap_remove_span(heap, --index);
    }
  }
  if (index < heap->span_count) {
    asm_code_span_t *after = &heap->spans[index];
    if (after->start < chunk_end && span.start + span.size == after->start) {
      span.size += after->size;
      asm_code_heap_remove_span(heap, index);
    }
  }
  if (!chunk->live && heap->chunk_count > 1) {
    code_unmap(chunk->base, chunk->size);
    *chunk = heap->chunks[--heap->chunk_count];
    return;
  }
  asm_code_heap_insert_span(heap, index, span);
}

asm_code_heap_stats_t asm_code_heap_stats(const asm_code_heap_t *heap) {
  asm_code_heap_stats_t stats = {0};
  if (!heap) return stats;
  for (uint32_t i = 0; i < heap->chunk_count; i++) {
    stats.mapped += heap->chunks[i].size;
    stats.live += heap->chunks[i].live;
  }
  stats.chunks = heap->chunk_count;
  stats.blocks = heap->blocks;
  return stats;
}

void asm_executable_free(asm_executable_t *code) {
  if (code->heap) asm_code_heap_release(code->heap, code->address, code->size);
  else if (code->address) code_unmap(code->address, code->size);
  *code = (asm_executable_t){ 0 };
}
