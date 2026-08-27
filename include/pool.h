#ifndef ANT_POOL_H
#define ANT_POOL_H

#include "cage.h"
#include "types.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(ANT_WASM_EMBED)
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

struct ant_pool_block {
  struct ant_pool_block *next;
  struct ant_pool_block *prev;
  size_t used;
  size_t cap;
  uint8_t data[];
};

static constexpr int ANT_POOL_SIZE_CLASS_COUNT = 32;

static constexpr size_t ANT_POOL_ROPE_BLOCK_SIZE   = 64u * 1024u;
static constexpr size_t ANT_POOL_SYMBOL_BLOCK_SIZE = 32u * 1024u;
static constexpr size_t ANT_POOL_BIGINT_BLOCK_SIZE = 64u * 1024u;
static constexpr size_t ANT_POOL_STRING_BLOCK_SIZE = 128u * 1024u;

static constexpr uint16_t ANT_ROPE_FLAG_YOUNG = 1u;
static constexpr uint16_t ANT_ROPE_DEPTH_SATURATED = UINT16_MAX;

typedef struct {
  ant_pool_block_t *head;
  ant_pool_block_t *free_head;
  size_t block_size;
} ant_pool_t;

typedef struct {
  ant_pool_block_t *head;
  ant_pool_block_t *current;
  ant_pool_block_t *free_head;
  void *slot_free;
  size_t block_size;
  uint8_t *cursor;
  uint8_t *end;
  size_t slot_stride;
} ant_pool_bucket_t;

typedef struct {
  ant_pool_t base;
  ant_pool_bucket_t classes[ANT_POOL_SIZE_CLASS_COUNT];
} ant_class_pool_t;

typedef struct ant_large_string_alloc {
  struct ant_large_string_alloc *next;
  struct ant_large_string_alloc *prev;
  size_t capacity;
  size_t alloc_size;
  uint32_t quarantine_epoch;
  uint8_t marked;
  ant_offset_t len;
  uint64_t meta;
  char bytes[];
} ant_large_string_alloc_t;

typedef struct {
  ant_offset_t len;
  uint16_t depth;
  uint16_t flags;
  uint32_t mark_epoch;
  ant_value_t left;
  ant_value_t right;
  ant_value_t cached;
} ant_rope_heap_t;

typedef struct {
  ant_large_string_alloc_t *live;
  ant_large_string_alloc_t *reusable;
  ant_large_string_alloc_t *quarantine;
  uint32_t gc_epoch;
} ant_large_string_space_t;

typedef struct ant_string_pool {
  size_t block_size;
  ant_pool_bucket_t classes[ANT_POOL_SIZE_CLASS_COUNT];
  ant_large_string_space_t large;
} ant_string_pool_t;

typedef enum {
  ANT_ALLOC_ROPE = 0,
  ANT_ALLOC_SYMBOL = 1,
  ANT_ALLOC_BIGINT = 2,
  ANT_ALLOC_STRING = 3,
} ant_alloc_kind_t;

typedef struct {
  size_t used;
  size_t capacity;
  size_t blocks;
} ant_pool_stats_t;

typedef struct {
  ant_pool_stats_t pooled;
  ant_pool_stats_t large_live;
  ant_pool_stats_t large_reusable;
  ant_pool_stats_t large_quarantine;
  ant_pool_stats_t total;
} ant_string_pool_stats_t;

static inline ant_pool_block_t *pool_free_next(ant_pool_block_t *b) {
  ant_pool_block_t *p; memcpy(&p, b->data, sizeof(ant_pool_block_t *)); return p;
}

static inline void pool_free_set_next(ant_pool_block_t *b, ant_pool_block_t *n) {
  memcpy(b->data, &n, sizeof(ant_pool_block_t *));
}

static inline void pool_block_free(ant_pool_block_t *block) {
  ant_cage_free(block, sizeof(ant_pool_block_t) + block->cap);
}

static inline ant_pool_block_t *pool_block_alloc(size_t cap) {
  ant_pool_block_t *b = (ant_pool_block_t *)ant_cage_alloc(
    sizeof(ant_pool_block_t) + cap, sizeof(void *)
  );
  
  if (!b) return NULL;
  
  b->next = NULL;
  b->prev = NULL;
  b->used = 0;
  b->cap  = cap;
  
  return b;
}

static inline void pool_block_madvise_free(ant_pool_block_t *block) {
#if defined(ANT_WASM_EMBED)
  (void)block;
#elif !defined(_WIN32)
  const uintptr_t page = 4096;
  uintptr_t start = (uintptr_t)block->data + sizeof(void *);
  uintptr_t aligned = (start + page - 1) & ~(page - 1);
  uintptr_t end = ((uintptr_t)block->data + block->cap) & ~(page - 1);
  if (aligned >= end) return;
#ifdef __APPLE__
  madvise((void *)aligned, end - aligned, MADV_FREE_REUSABLE);
#else
  madvise((void *)aligned, end - aligned, MADV_DONTNEED);
#endif
#endif
}

void js_pool_destroy(ant_pool_t *pool);
void js_class_pool_destroy(ant_class_pool_t *pool);
void js_string_pool_destroy(ant_string_pool_t *pool);

ant_rope_heap_t *js_rope_alloc(ant_t *js);
ant_pool_stats_t js_pool_stats(ant_pool_t *pool);
ant_pool_stats_t js_rope_pool_stats(ant_t *js);
ant_pool_stats_t js_class_pool_stats(ant_class_pool_t *pool);
ant_string_pool_stats_t js_string_pool_stats(ant_string_pool_t *pool);

void *js_type_alloc(
  ant_t *js, ant_alloc_kind_t kind,
  size_t size, size_t align
);

void *pool_alloc_chain(
  ant_pool_block_t **head, ant_pool_block_t **free_head, 
  size_t block_size, size_t size, size_t align
);

#endif
