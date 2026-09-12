#include "jit_internal.h"

#if !defined(_WIN32)
#include <sys/mman.h>
#include <string.h>

#define JIT_HEAP_CHUNK_SIZE (1u << 20)
#define JIT_HEAP_ALIGN 16
#define JIT_HEAP_FINE_MAX 4096
#define JIT_HEAP_FINE_CLASSES (JIT_HEAP_FINE_MAX / JIT_HEAP_ALIGN)

static const uint32_t jit_heap_coarse[] = {8192, 16384, 32768, 65536, 131072};
#define JIT_HEAP_COARSE_CLASSES (sizeof(jit_heap_coarse) / sizeof(jit_heap_coarse[0]))

#define JIT_HEAP_CLASSES (JIT_HEAP_FINE_CLASSES + JIT_HEAP_COARSE_CLASSES)
#define JIT_HEAP_LARGE_RESERVE 4
#define JIT_HEAP_MAGIC 0x4d49525f48454150ull

typedef struct jit_heap_chunk {
  uint64_t magic;
  uint32_t cls, block, live, bump;
  void *free_list;
  struct jit_heap_chunk *prev, *next;
  bool full;
} jit_heap_chunk_t;
#define JIT_HEAP_CHUNK_HEADER 64

typedef struct {
  char *base;
  size_t size;
} jit_heap_map_t;

struct MIR_heap {
  struct MIR_alloc alloc;
  jit_heap_chunk_t *avail[JIT_HEAP_CLASSES];
  jit_heap_chunk_t *full[JIT_HEAP_CLASSES];
  jit_heap_map_t *maps;
  size_t map_count, map_cap;
};

static void jit_heap_unlink(jit_heap_chunk_t **head, jit_heap_chunk_t *c) {
  if (c->prev) c->prev->next = c->next; else *head = c->next;
  if (c->next) c->next->prev = c->prev;
  c->prev = c->next = NULL;
}

static void jit_heap_push(jit_heap_chunk_t **head, jit_heap_chunk_t *c) {
  c->prev = NULL;
  c->next = *head;
  if (*head) (*head)->prev = c;
  *head = c;
}

static int jit_heap_class(size_t size, uint32_t *block) {
  if (size <= JIT_HEAP_FINE_MAX) {
    uint32_t b = (uint32_t)((size + JIT_HEAP_ALIGN - 1) & ~(size_t)(JIT_HEAP_ALIGN - 1));
    if (b == 0) b = JIT_HEAP_ALIGN;
    *block = b;
    return (int)(b / JIT_HEAP_ALIGN) - 1;
  }
  
  for (size_t i = 0; i < JIT_HEAP_COARSE_CLASSES; i++)
    if (size <= jit_heap_coarse[i]) {
      *block = jit_heap_coarse[i];
      return (int)(JIT_HEAP_FINE_CLASSES + i);
    }
  
  return -1;
}

static size_t jit_heap_map_find(MIR_heap_t h, const void *p) {
  size_t lo = 0, hi = h->map_count;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (h->maps[mid].base <= (const char *)p) lo = mid + 1; else hi = mid;
  }
  return lo;
}

static jit_heap_map_t *jit_heap_map_of(MIR_heap_t h, const void *p) {
  size_t i = jit_heap_map_find(h, p);
  if (i == 0) return NULL;
  jit_heap_map_t *m = &h->maps[i - 1];
  return (const char *)p < m->base + m->size ? m : NULL;
}

static void jit_heap_map_add(MIR_heap_t h, char *base, size_t size) {
  if (h->map_count == h->map_cap) {
    size_t cap = h->map_cap ? h->map_cap * 2 : 64;
    jit_heap_map_t *maps = realloc(h->maps, cap * sizeof(*maps));
    if (!maps) abort();
    h->maps = maps;
    h->map_cap = cap;
  }
  
  size_t i = jit_heap_map_find(h, base);
  memmove(&h->maps[i + 1], &h->maps[i], (h->map_count - i) * sizeof(*h->maps));
  
  h->maps[i] = (jit_heap_map_t){base, size};
  h->map_count++;
}

static void jit_heap_map_remove(MIR_heap_t h, jit_heap_map_t *m) {
  size_t i = (size_t)(m - h->maps);
  munmap(m->base, m->size);
  memmove(&h->maps[i], &h->maps[i + 1], (h->map_count - i - 1) * sizeof(*h->maps));
  h->map_count--;
}

static void *jit_heap_mmap(size_t size) {
  void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  return p == MAP_FAILED ? NULL : p;
}

static jit_heap_chunk_t *jit_heap_chunk_mmap(void) {
  char *p = jit_heap_mmap(2 * JIT_HEAP_CHUNK_SIZE);
  if (!p) return NULL;
  
  char *base = 
    (char *)(((uintptr_t)p + JIT_HEAP_CHUNK_SIZE - 1) & 
    ~(uintptr_t)(JIT_HEAP_CHUNK_SIZE - 1));
  
  if (base > p) munmap(p, (size_t)(base - p));
  char *end = p + 2 * JIT_HEAP_CHUNK_SIZE;
  
  if (end > base + JIT_HEAP_CHUNK_SIZE) 
    munmap(base + JIT_HEAP_CHUNK_SIZE, (size_t)(end - base - JIT_HEAP_CHUNK_SIZE));
  
  return (jit_heap_chunk_t *)base;
}

static jit_heap_chunk_t *jit_heap_chunk_of(const void *p) {
  jit_heap_chunk_t *c = (jit_heap_chunk_t *)((uintptr_t)p & ~(uintptr_t)(JIT_HEAP_CHUNK_SIZE - 1));
  ANT_ASSERT(c->magic == JIT_HEAP_MAGIC, "JIT heap: pointer is not from a live chunk");
  return c;
}

static void *jit_heap_alloc(MIR_heap_t h, size_t size) {
  uint32_t block;
  int cls = jit_heap_class(size, &block);
  
  if (cls < 0) {
    size_t reserved = (size * JIT_HEAP_LARGE_RESERVE + 16383) & ~(size_t)16383;
    char *p = jit_heap_mmap(reserved);
    if (!p) return NULL;
    jit_heap_map_add(h, p, reserved);
    return p;
  }
  
  jit_heap_chunk_t *c = h->avail[cls];
  if (c == NULL) {
    c = jit_heap_chunk_mmap();
    if (!c) return NULL;
    c->magic = JIT_HEAP_MAGIC;
    c->cls = (uint32_t)cls;
    c->block = block;
    c->live = 0;
    c->bump = JIT_HEAP_CHUNK_HEADER;
    c->free_list = NULL;
    c->full = false;
    jit_heap_push(&h->avail[cls], c);
  }
  
  void *p;
  if (c->free_list != NULL) {
    p = c->free_list;
    c->free_list = *(void **)p;
  } else {
    p = (char *)c + c->bump;
    c->bump += block;
  }
  
  c->live++;
  if (c->free_list == NULL && c->bump + block > JIT_HEAP_CHUNK_SIZE) {
    jit_heap_unlink(&h->avail[cls], c);
    jit_heap_push(&h->full[cls], c);
    c->full = true;
  }
  
  return p;
}

static void jit_heap_free(MIR_heap_t h, void *p) {
  if (!p) return;
  jit_heap_map_t *m = jit_heap_map_of(h, p);
  
  if (m) {
    jit_heap_map_remove(h, m);
    return;
  }
  
  jit_heap_chunk_t *c = jit_heap_chunk_of(p);
  ANT_ASSERT(c->live > 0, "MIR heap: free into a chunk with no live block");
  
  *(void **)p = c->free_list;
  c->free_list = p;
  c->live--;
  
  if (c->full) {
    jit_heap_unlink(&h->full[c->cls], c);
    jit_heap_push(&h->avail[c->cls], c);
    c->full = false;
  }
}

static void jit_heap_chunk_unmap(jit_heap_chunk_t *c) {
  c->magic = 0;
  munmap(c, JIT_HEAP_CHUNK_SIZE);
}

void jit_heap_release(MIR_heap_t h) {
  if (!h) return;
  for (size_t cls = 0; cls < JIT_HEAP_CLASSES; cls++) {
    jit_heap_chunk_t *c, *next;
    for (c = h->full[cls]; c != NULL; c = c->next) ANT_ASSERT(
      c->magic == JIT_HEAP_MAGIC && c->cls == cls && c->full && c->live > 0,
      "MIR heap: full chunk list is inconsistent"
    );
    
    for (c = h->avail[cls]; c != NULL; c = next) {
      next = c->next;
      ANT_ASSERT(
        c->magic == JIT_HEAP_MAGIC && c->cls == cls && !c->full
        && c->live <= (JIT_HEAP_CHUNK_SIZE - JIT_HEAP_CHUNK_HEADER) / c->block,
        "MIR heap: available chunk list is inconsistent"
      );
      if (c->live != 0) continue;
      jit_heap_unlink(&h->avail[cls], c);
      jit_heap_chunk_unmap(c);
    }
  }
}

static void *jit_heap_mir_malloc(size_t size, void *ud) { 
  return jit_heap_alloc(ud, size);
}

static void jit_heap_mir_free(void *p, void *ud) {
  jit_heap_free(ud, p);
}

static void *jit_heap_mir_calloc(size_t n, size_t size, void *ud) {
  void *p = jit_heap_alloc(ud, n * size);
  if (p) memset(p, 0, n * size);
  return p;
}

static void *jit_heap_mir_realloc(void *p, size_t old_size, size_t size, void *ud) {
  MIR_heap_t h = ud;
  if (!p) return jit_heap_alloc(h, size);
  
  jit_heap_map_t *m = jit_heap_map_of(h, p);
  if (m ? size <= m->size : size <= jit_heap_chunk_of(p)->block) return p;
  
  void *q = jit_heap_alloc(h, size);
  if (!q) return NULL;
  
  memcpy(q, p, old_size < size ? old_size : size);
  jit_heap_free(h, p);
  
  return q;
}

MIR_alloc_t jit_heap_mir_alloc(MIR_heap_t h) { 
  return h ? &h->alloc : NULL;
}

MIR_heap_t jit_heap_create(void) {
  MIR_heap_t h = calloc(1, sizeof(*h));
  if (!h) return NULL;
  h->alloc = (struct MIR_alloc){
    .malloc = jit_heap_mir_malloc, .calloc = jit_heap_mir_calloc,
    .realloc = jit_heap_mir_realloc, .free = jit_heap_mir_free, .user_data = h,
  };
  return h;
}

void jit_heap_destroy(MIR_heap_t h) {
  if (!h) return;
  
  for (size_t cls = 0; cls < JIT_HEAP_CLASSES; cls++) {
    jit_heap_chunk_t *c, *next;
    for (c = h->avail[cls]; c != NULL; c = next) { next = c->next; jit_heap_chunk_unmap(c); }
    for (c = h->full[cls]; c != NULL; c = next) { next = c->next; jit_heap_chunk_unmap(c); }
  }
  
  while (h->map_count > 0) 
    jit_heap_map_remove(h, &h->maps[h->map_count - 1]);
  
  free(h->maps);
  free(h);
}

#else

MIR_heap_t jit_heap_create(void) { return NULL; }
MIR_alloc_t jit_heap_mir_alloc(MIR_heap_t h) { (void)h; return NULL; }
void jit_heap_release(MIR_heap_t h) { (void)h; }
void jit_heap_destroy(MIR_heap_t h) { (void)h; }

#endif
