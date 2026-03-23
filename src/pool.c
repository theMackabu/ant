#include "internal.h"
#include "gc.h"

#include <stddef.h>
#include <stdlib.h>

static const size_t g_size_classes[ANT_POOL_SIZE_CLASS_COUNT] = {
  16u,   24u,   32u,   40u,   48u,   56u,   64u,   80u,
  96u,   112u,  128u,  160u,  192u,  224u,  256u,  320u,
  384u,  448u,  512u,  640u,  768u,  896u,  1024u, 1280u,
  1536u, 1792u, 2048u, 2560u, 3072u, 4096u, 8192u, 16384u
};

static inline bool pool_kind_uses_size_classes(ant_alloc_kind_t kind) {
  return kind == ANT_ALLOC_STRING || kind == ANT_ALLOC_BIGINT;
}

static inline size_t pool_default_block_size(ant_alloc_kind_t kind) {
switch (kind) {
  case ANT_ALLOC_SYMBOL: return ANT_POOL_SYMBOL_BLOCK_SIZE;
  case ANT_ALLOC_BIGINT: return ANT_POOL_BIGINT_BLOCK_SIZE;
  case ANT_ALLOC_STRING: return ANT_POOL_STRING_BLOCK_SIZE;
  case ANT_ALLOC_ROPE:
  default: return ANT_POOL_ROPE_BLOCK_SIZE;
}}

static inline ant_pool_t *pool_for_kind(ant_t *js, ant_alloc_kind_t kind) {
switch (kind) {
  case ANT_ALLOC_SYMBOL: return &js->pool.symbol;
  case ANT_ALLOC_BIGINT: return &js->pool.bigint.base;
  case ANT_ALLOC_STRING: return &js->pool.string.base;
  case ANT_ALLOC_ROPE:
  default: return &js->pool.rope;
}}

static inline ant_class_pool_t *class_pool_for_kind(ant_t *js, ant_alloc_kind_t kind) {
switch (kind) {
  case ANT_ALLOC_BIGINT: return &js->pool.bigint;
  case ANT_ALLOC_STRING: return &js->pool.string;
  default: return NULL;
}}

static inline size_t align_up_size(size_t value, size_t align) {
  if (align == 0) return value;
  size_t mask = align - 1;
  return (value + mask) & ~mask;
}

static inline int pool_size_class_index(size_t need) {
  for (int i = 0; i < ANT_POOL_SIZE_CLASS_COUNT; i++) {
    if (need <= g_size_classes[i]) return i;
  }
  return -1;
}

static inline size_t pool_class_block_items(size_t class_size) {
  if (class_size <= 256u) return 256u;
  if (class_size <= 1024u) return 128u;
  if (class_size <= 4096u) return 64u;
  return 32u;
}

static void *pool_bucket_alloc_fast(
  ant_pool_bucket_t *bucket,
  size_t class_size,
  size_t align
) {
  if (!bucket) return NULL;

  const size_t cache_align = _Alignof(max_align_t);
  if (align > cache_align) return NULL;

  if (bucket->slot_stride == 0) {
    bucket->slot_stride = align_up_size(class_size, cache_align);
  }

  if (bucket->slot_free) {
    void *ptr = bucket->slot_free;
    void *next;
    
    memcpy(&next, ptr, sizeof(void *));
    bucket->slot_free = next;
    
    return ptr;
  }

  if (!bucket->current || !bucket->cursor || bucket->cursor + bucket->slot_stride > bucket->end) {
  if (bucket->free_head) {
    ant_pool_block_t *reuse = bucket->free_head;
    bucket->free_head = pool_free_next(reuse);
    reuse->used = 0;
    reuse->prev = NULL;
    reuse->next = bucket->head;
    if (bucket->head) bucket->head->prev = reuse;
    bucket->head = reuse;
    bucket->current = reuse;
    bucket->cursor = reuse->data;
    bucket->end = reuse->data + reuse->cap;
  } else {
    size_t cap = bucket->block_size;
    if (cap < bucket->slot_stride) cap = bucket->slot_stride;
    ant_pool_block_t *block = pool_block_alloc(cap);
    if (!block) return NULL;
    block->next = bucket->head;
    if (bucket->head) bucket->head->prev = block;
    bucket->head = block;
    bucket->current = block;
    bucket->cursor = block->data;
    bucket->end = block->data + cap;
  }}

  uint8_t *ptr = bucket->cursor;
  bucket->cursor += bucket->slot_stride;
  if (bucket->current) bucket->current->used = (size_t)(bucket->cursor - bucket->current->data);
  
  return ptr;
}

void *pool_alloc_chain(
  ant_pool_block_t **head,
  ant_pool_block_t **free_head,
  size_t block_size,
  size_t size,
  size_t align
) {
  if (!head || size == 0) return NULL;
  if (align == 0) align = sizeof(void *);

  ant_pool_block_t *block = *head;
  if (!block) {
    size_t cap = block_size;
    if (cap < size + align) cap = size + align;
    block = pool_block_alloc(cap);
    if (!block) return NULL;
    *head = block;
  }

  uintptr_t base = (uintptr_t)block->data;
  size_t start = align_up_size(block->used, align);
  if (start + size > block->cap) {
    if (free_head && *free_head) {
      block = *free_head;
      *free_head = pool_free_next(block);
      block->used = 0;
      block->prev = NULL;
      block->next = *head;
      if (*head) (*head)->prev = block;
      *head = block;
    } else {
      size_t cap = block_size;
      if (cap < size + align) cap = size + align;
      ant_pool_block_t *next = pool_block_alloc(cap);
      if (!next) return NULL;
      next->next = block;
      if (block) block->prev = next;
      *head = next;
      block = next;
    }
    base = (uintptr_t)block->data;
    start = align_up_size(block->used, align);
  }

  void *ptr = (void *)(base + start);
  block->used = start + size;
  return ptr;
}

void *js_type_alloc(ant_t *js, ant_alloc_kind_t kind, size_t size, size_t align) {
  if (!js || size == 0) return NULL;
  if (align == 0) align = sizeof(void *);

  js->gc_pool_alloc += size;
  size_t pool_threshold = GC_HEAP_GROWTH(js->gc_pool_last_live);
  
  if (pool_threshold < (4u * 1024u * 1024u)) pool_threshold = 4u * 1024u * 1024u;
#ifdef ANT_JIT
  if (__builtin_expect(js->jit_active_depth > 0, 0)) { /* suppress GC during JIT */ }
  else
#endif
  { if (js->gc_pool_alloc >= pool_threshold) gc_run(js); } 

  ant_pool_t *pool = pool_for_kind(js, kind);
  if (pool->block_size == 0) pool->block_size = pool_default_block_size(kind);

  if (pool_kind_uses_size_classes(kind)) {
    ant_class_pool_t *class_pool = class_pool_for_kind(js, kind);
    if (!class_pool) return pool_alloc_chain(&pool->head, &pool->free_head, pool->block_size, size, align);

    size_t needed = size + (align - 1u);
    int class_idx = pool_size_class_index(needed);
    if (class_idx >= 0) {
      size_t class_size = g_size_classes[class_idx];
      ant_pool_bucket_t *bucket = &class_pool->classes[class_idx];
      if (bucket->block_size == 0) {
        size_t bucket_block_size = pool->block_size;
        size_t min_block = class_size * pool_class_block_items(class_size);
        if (bucket_block_size < min_block) bucket_block_size = min_block;
        bucket->block_size = bucket_block_size;
      }
      void *fast = pool_bucket_alloc_fast(bucket, class_size, align);
      if (fast) return fast;
      return pool_alloc_chain(&bucket->head, &bucket->free_head, bucket->block_size, size, align);
    }
  }

  return pool_alloc_chain(&pool->head, &pool->free_head, pool->block_size, size, align);
}

void js_pool_destroy(ant_pool_t *pool) {
  if (!pool) return;

  ant_pool_block_t *block = pool->head;
  while (block) {
    ant_pool_block_t *next = block->next;
    pool_block_free(block);
    block = next;
  }
  pool->head = NULL;
  pool->block_size = 0;
}

ant_pool_stats_t js_pool_stats(ant_pool_t *pool) {
  ant_pool_stats_t s = {0};
  if (!pool) return s;
  for (ant_pool_block_t *b = pool->head; b; b = b->next) {
    s.used += b->used;
    s.capacity += b->cap;
    s.blocks++;
  }
  return s;
}

static inline void pool_stats_add(ant_pool_stats_t *dst, ant_pool_stats_t src) {
  dst->used += src.used;
  dst->capacity += src.capacity;
  dst->blocks += src.blocks;
}

ant_pool_stats_t js_class_pool_stats(ant_class_pool_t *pool) {
  ant_pool_stats_t s = {0};
  if (!pool) return s;
  for (int i = 0; i < ANT_POOL_SIZE_CLASS_COUNT; i++) {
    ant_pool_t bucket = { .head = pool->classes[i].head };
    pool_stats_add(&s, js_pool_stats(&bucket));
  }
  pool_stats_add(&s, js_pool_stats(&pool->base));
  return s;
}

void js_class_pool_destroy(ant_class_pool_t *pool) {
  if (!pool) return;

  for (int i = 0; i < ANT_POOL_SIZE_CLASS_COUNT; i++) {
    ant_pool_block_t *block = pool->classes[i].head;
    while (block) {
      ant_pool_block_t *next = block->next;
      pool_block_free(block);
      block = next;
    }
    pool->classes[i].head = NULL;
    pool->classes[i].block_size = 0;
    pool->classes[i].cursor = NULL;
    pool->classes[i].end = NULL;
    pool->classes[i].slot_stride = 0;
  }

  js_pool_destroy(&pool->base);
}
