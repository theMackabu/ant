#include "internal.h"
#include "gc/bigints.h"
#include "pool.h"
#include "utils.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct gc_bigint_block {
  uintptr_t base;
  uintptr_t end;
  ant_pool_block_t *block;
  ant_pool_bucket_t *bucket;
  size_t stride;
  size_t slot_count;
  uint8_t *marks;
  bool block_live;
} gc_bigint_block_t;

static int bigint_block_cmp(const void *a, const void *b) {
  const gc_bigint_block_t *left = (const gc_bigint_block_t *)a;
  const gc_bigint_block_t *right = (const gc_bigint_block_t *)b;
  if (left->base < right->base) return -1;
  if (left->base > right->base) return 1;
  return 0;
}

static void bigint_block_add(
  ant_t *js, ant_pool_block_t *block,
  ant_pool_bucket_t *bucket,
  size_t stride
) {
  if (!block || block->used == 0) return;
  gc_bigint_block_t *blocks = vec_grow(
    js->gc.bigint_blocks, &js->gc.bigint_block_cap,
    js->gc.bigint_block_len + 1, sizeof(*blocks), 32
  );
  
  if (!blocks) return;
  js->gc.bigint_blocks = blocks;

  size_t slot_count = stride ? block->used / stride : 0;
  size_t mark_bytes = (slot_count + 7u) / 8u;
  uint8_t *marks = mark_bytes ? calloc(1, mark_bytes) : NULL;
  if (mark_bytes && !marks) return;

  gc_bigint_block_t *entry = &js->gc.bigint_blocks[js->gc.bigint_block_len++];
  entry->base = (uintptr_t)block->data;
  entry->end = entry->base + block->used;
  entry->block = block;
  entry->bucket = bucket;
  entry->stride = stride;
  entry->slot_count = slot_count;
  entry->marks = marks;
  entry->block_live = false;
}

static void bigint_block_unlink(
  ant_pool_block_t **head,
  ant_pool_block_t *block
) {
  if (block->prev) block->prev->next = block->next;
  else *head = block->next;
  if (block->next) block->next->prev = block->prev;
}

static void bigint_block_recycle(
  ant_pool_block_t *block,
  ant_pool_block_t **free_head
) {
  block->used = 0;
  block->prev = NULL;
  block->next = NULL;
  pool_free_set_next(block, *free_head);
  *free_head = block;
}

static void bigint_free_list_trim(ant_pool_block_t **free_head) {
  ant_pool_block_t *block = *free_head;
  size_t kept = 0;
  while (block && kept < 2u) {
    block = pool_free_next(block);
    kept++;
  }

  while (block) {
    ant_pool_block_t *next = pool_free_next(block);
    pool_block_free(block);
    block = next;
  }

  block = *free_head;
  if (kept == 0) {
    *free_head = NULL;
    return;
  }
  
  for (size_t i = 1; i < kept && block; i++) block = pool_free_next(block);
  if (block) pool_free_set_next(block, NULL);
}

void gc_bigints_begin(ant_t *js) {
  js->gc.bigint_block_len = 0;

  ant_class_pool_t *pool = &js->pool.bigint;
  for (int i = 0; i < ANT_POOL_SIZE_CLASS_COUNT; i++) {
    ant_pool_bucket_t *bucket = &pool->classes[i];
    bucket->slot_free = NULL;
    if (bucket->slot_stride == 0) continue;
    for (ant_pool_block_t *block = bucket->head; block; block = block->next)
      bigint_block_add(js, block, bucket, bucket->slot_stride);
  }

  for (ant_pool_block_t *block = pool->base.head; block; block = block->next)
    bigint_block_add(js, block, NULL, 0);

  if (js->gc.bigint_block_len > 1)
    qsort(js->gc.bigint_blocks, js->gc.bigint_block_len, sizeof(*js->gc.bigint_blocks), bigint_block_cmp);
}

void gc_bigints_mark(ant_t *js, const void *ptr) {
  if (!ptr || js->gc.bigint_block_len == 0) return;
  uintptr_t value = (uintptr_t)ptr;
  size_t lo = 0;
  size_t hi = js->gc.bigint_block_len;

  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    gc_bigint_block_t *entry = &js->gc.bigint_blocks[mid];
    if (value < entry->base) hi = mid;
    else if (value >= entry->end) lo = mid + 1u;
    else {
      if (entry->stride == 0) {
        entry->block_live = true;
        return;
      }

      size_t offset = value - entry->base;
      if (offset % entry->stride != 0) return;
      size_t slot = offset / entry->stride;
      if (slot >= entry->slot_count) return;
      entry->marks[slot / 8u] |= (uint8_t)(1u << (slot % 8u));
      return;
    }
  }
}

void gc_bigints_sweep(ant_t *js) {
  ant_class_pool_t *pool = &js->pool.bigint;

  for (size_t i = 0; i < js->gc.bigint_block_len; i++) {
    gc_bigint_block_t *entry = &js->gc.bigint_blocks[i];
    ant_pool_block_t *block = entry->block;

    if (entry->stride == 0) {
      if (!entry->block_live) {
        bigint_block_unlink(&pool->base.head, block);
        bigint_block_recycle(block, &pool->base.free_head);
      }
      continue;
    }

    ant_pool_bucket_t *bucket = entry->bucket;
    bool any_live = false;
    for (size_t slot = 0; slot < entry->slot_count; slot++) {
      bool live = ((entry->marks[slot / 8u] >> (slot % 8u)) & 1u) != 0;
      if (live) {
        any_live = true;
        break;
      }
    }

    if (!any_live) {
      bigint_block_unlink(&bucket->head, block);
      if (bucket->current == block) bucket->current = NULL;
      bigint_block_recycle(block, &bucket->free_head);
    } else {
      for (size_t slot = 0; slot < entry->slot_count; slot++) {
        bool live = ((entry->marks[slot / 8u] >> (slot % 8u)) & 1u) != 0;
        if (live) continue;
        void *free_slot = (void *)(entry->base + slot * entry->stride);
        memcpy(free_slot, &bucket->slot_free, sizeof(void *));
        bucket->slot_free = free_slot;
      }
    }

    free(entry->marks);
    entry->marks = NULL;
  }

  for (int i = 0; i < ANT_POOL_SIZE_CLASS_COUNT; i++)
    bigint_free_list_trim(&pool->classes[i].free_head);
  bigint_free_list_trim(&pool->base.free_head);

  js->gc.bigint_block_len = 0;
}

void gc_bigints_destroy(ant_t *js) {
  free(js->gc.bigint_blocks);
  js->gc.bigint_blocks = NULL;
  js->gc.bigint_block_len = js->gc.bigint_block_cap = 0;
}
