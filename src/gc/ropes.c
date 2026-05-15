#include "internal.h"
#include "gc/ropes.h"
#include "pool.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  uintptr_t base;
  uintptr_t end;
  ant_pool_block_t *block;
  bool has_live;
} gc_rope_mark_t;

static gc_rope_mark_t *g_rope_marks = NULL;
static int g_rope_mark_count = 0;
static int g_rope_mark_cap = 0;

static int rope_mark_cmp(const void *a, const void *b) {
  const gc_rope_mark_t *ma = (const gc_rope_mark_t *)a;
  const gc_rope_mark_t *mb = (const gc_rope_mark_t *)b;
  if (ma->base < mb->base) return -1;
  if (ma->base > mb->base) return 1;
  return 0;
}

void gc_ropes_begin(ant_t *js) {
  g_rope_mark_count = 0;

  for (ant_pool_block_t *b = js->pool.rope.head; b; b = b->next) {
    if (b->used == 0) continue;

    if (g_rope_mark_count >= g_rope_mark_cap) {
      int cap = g_rope_mark_cap ? g_rope_mark_cap * 2 : 32;
      g_rope_marks = realloc(g_rope_marks, (size_t)cap * sizeof(gc_rope_mark_t));
      g_rope_mark_cap = cap;
    }

    gc_rope_mark_t *m = &g_rope_marks[g_rope_mark_count++];
    m->base = (uintptr_t)b->data;
    m->end = m->base + b->used;
    m->block = b;
    m->has_live = false;
  }

  if (g_rope_mark_count > 1)
    qsort(g_rope_marks, (size_t)g_rope_mark_count, sizeof(gc_rope_mark_t), rope_mark_cmp);
}

bool gc_ropes_mark(const void *ptr) {
  if (!ptr || g_rope_mark_count == 0) return false;
  uintptr_t p = (uintptr_t)ptr;

  int lo = 0, hi = g_rope_mark_count - 1;
  while (lo <= hi) {
    int mid = lo + (hi - lo) / 2;
    gc_rope_mark_t *m = &g_rope_marks[mid];
    if (p < m->base) hi = mid - 1;
    else if (p >= m->end) lo = mid + 1;
    else {
      m->has_live = true;
      return true;
    }
  }
  
  return false;
}

bool gc_ropes_contains(const void *ptr, size_t size, size_t align) {
  if (!ptr || size == 0 || g_rope_mark_count == 0) return false;
  uintptr_t p = (uintptr_t)ptr;
  if (align > 1 && (p & (align - 1u)) != 0) return false;

  int lo = 0, hi = g_rope_mark_count - 1;
  while (lo <= hi) {
    int mid = lo + (hi - lo) / 2;
    gc_rope_mark_t *m = &g_rope_marks[mid];
    if (p < m->base) hi = mid - 1;
    else if (p >= m->end) lo = mid + 1;
    else return size <= m->end - p;
  }

  return false;
}

static void unlink_rope_block(ant_pool_t *pool, ant_pool_block_t *block) {
  if (block->prev) block->prev->next = block->next;
  else pool->head = block->next;
  if (block->next) block->next->prev = block->prev;
}

void gc_ropes_sweep(ant_t *js) {
  for (int i = 0; i < g_rope_mark_count; i++) {
  gc_rope_mark_t *m = &g_rope_marks[i];
  if (!m->has_live) {
    unlink_rope_block(&js->pool.rope, m->block);
    m->block->used = 0;
    m->block->next = NULL;
    pool_free_set_next(m->block, js->pool.rope.free_head);
    pool_block_madvise_free(m->block);
    js->pool.rope.free_head = m->block;
  }}

  ant_pool_block_t *f = js->pool.rope.free_head;
  int kept = 0;
  
  while (f && kept < 2) {
    f = pool_free_next(f);
    kept++;
  }
  
  while (f) {
    ant_pool_block_t *next = pool_free_next(f);
    pool_block_free(f);
    f = next;
  }
  
  if (kept > 0) {
    f = js->pool.rope.free_head;
    for (int k = 1; k < kept && f; k++) f = pool_free_next(f);
    if (f) pool_free_set_next(f, NULL);
  } else js->pool.rope.free_head = NULL;

  g_rope_mark_count = 0;
}
