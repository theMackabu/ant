#include "internal.h"
#include "gc/objects.h"
#include "gc/ropes.h"
#include "pool.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  GC_ROPE_POOL_MISC,
  GC_ROPE_POOL_OLD,
  GC_ROPE_POOL_YOUNG,
} gc_rope_pool_kind_t;

typedef struct gc_rope_mark {
  uintptr_t base;
  uintptr_t end;
  ant_pool_block_t *block;
  ant_pool_t *pool;
  gc_rope_pool_kind_t kind;
  bool has_live;
} gc_rope_mark_t;

static int rope_mark_cmp(const void *a, const void *b) {
  const gc_rope_mark_t *ma = (const gc_rope_mark_t *)a;
  const gc_rope_mark_t *mb = (const gc_rope_mark_t *)b;
  if (ma->base < mb->base) return -1;
  if (ma->base > mb->base) return 1;
  return 0;
}

static bool rope_marks_reserve(ant_t *js, size_t needed) {
  if (needed <= js->rope_gc.mark_cap) return true;

  size_t cap = js->rope_gc.mark_cap ? js->rope_gc.mark_cap : 32u;
  while (cap < needed) {
    if (cap > SIZE_MAX / 2u) {
      cap = needed;
      break;
    }
    cap *= 2u;
  }
  if (cap > SIZE_MAX / sizeof(gc_rope_mark_t)) return false;

  gc_rope_mark_t *marks = (gc_rope_mark_t *)realloc(
    js->rope_gc.marks, cap * sizeof(*marks)
  );
  if (!marks && cap != needed) {
    cap = needed;
    marks = (gc_rope_mark_t *)realloc(
      js->rope_gc.marks, cap * sizeof(*marks)
    );
  }
  if (!marks) return false;
  js->rope_gc.marks = marks;
  js->rope_gc.mark_cap = cap;
  return true;
}

static bool rope_marks_count_pool(ant_pool_t *pool, size_t *count) {
  for (ant_pool_block_t *b = pool->head; b; b = b->next) {
    if (b->used == 0) continue;
    if (*count == SIZE_MAX) return false;
    (*count)++;
  }
  return true;
}

static void rope_marks_add_pool(
  ant_t *js, ant_pool_t *pool, gc_rope_pool_kind_t kind
) {
  for (ant_pool_block_t *b = pool->head; b; b = b->next) {
    if (b->used == 0) continue;
    gc_rope_mark_t *marks = js->rope_gc.marks;
    gc_rope_mark_t *m = &marks[js->rope_gc.mark_count++];
    m->base = (uintptr_t)b->data;
    m->end = m->base + b->used;
    m->block = b;
    m->pool = pool;
    m->kind = kind;
    m->has_live = false;
  }
}

static void rope_nodes_clear_epochs(ant_pool_t *pool) {
  for (ant_pool_block_t *b = pool->head; b; b = b->next) {
  for (size_t off = 0; off + sizeof(ant_rope_heap_t) <= b->used; off += sizeof(ant_rope_heap_t)) {
    ant_rope_heap_t *rope = (ant_rope_heap_t *)(b->data + off);
    rope->mark_epoch = 0;
  }}
}

gc_ropes_begin_result_t gc_ropes_begin(ant_t *js, bool minor) {
  size_t needed = 0;
  if (!rope_marks_count_pool(&js->pool.rope, &needed) ||
      !rope_marks_count_pool(&js->rope_gc.old, &needed) ||
      !rope_marks_count_pool(&js->rope_gc.young, &needed) ||
      !rope_marks_reserve(js, needed)) {
    js->rope_gc.mark_count = 0;
    js->rope_gc.minor_marking = false;
    js->rope_gc.conservative_marking = !minor;
    return minor
      ? GC_ROPES_BEGIN_RETRY_MAJOR
      : GC_ROPES_BEGIN_CONSERVATIVE_MAJOR;
  }

  js->rope_gc.mark_count = 0;
  js->rope_gc.minor_marking = minor;
  js->rope_gc.conservative_marking = false;
  if (++js->rope_gc.mark_epoch == 0) {
    js->rope_gc.mark_epoch = 1;
    rope_nodes_clear_epochs(&js->rope_gc.old);
    rope_nodes_clear_epochs(&js->rope_gc.young);
  }

  rope_marks_add_pool(js, &js->pool.rope, GC_ROPE_POOL_MISC);
  rope_marks_add_pool(js, &js->rope_gc.old, GC_ROPE_POOL_OLD);
  rope_marks_add_pool(js, &js->rope_gc.young, GC_ROPE_POOL_YOUNG);

  if (js->rope_gc.mark_count > 1)
    qsort(js->rope_gc.marks, js->rope_gc.mark_count,
          sizeof(gc_rope_mark_t), rope_mark_cmp);
  return GC_ROPES_BEGIN_NORMAL;
}

static void rope_mark_conservative_pool(ant_t *js, ant_pool_t *pool) {
  for (ant_pool_block_t *b = pool->head; b; b = b->next)
    if (b->used) gc_mark_conservative_range(js, b->data, b->used);
}

void gc_ropes_mark_conservative_roots(ant_t *js) {
  if (!js || !js->rope_gc.conservative_marking) return;
  rope_mark_conservative_pool(js, &js->pool.rope);
  rope_mark_conservative_pool(js, &js->rope_gc.old);
  rope_mark_conservative_pool(js, &js->rope_gc.young);
}

static gc_rope_mark_t *rope_mark_find(ant_t *js, const void *ptr) {
  if (!js || !ptr) return NULL;
  uintptr_t p = (uintptr_t)ptr;
  gc_rope_mark_t *marks = js->rope_gc.marks;
  size_t lo = 0, hi = js->rope_gc.mark_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    gc_rope_mark_t *m = &marks[mid];
    if (p < m->base) hi = mid;
    else if (p >= m->end) lo = mid + 1u;
    else return m;
  }
  return NULL;
}

bool gc_ropes_mark(ant_t *js, const void *ptr) {
  gc_rope_mark_t *m = rope_mark_find(js, ptr);
  if (!m) return false;

  if (m->kind == GC_ROPE_POOL_MISC) {
    m->has_live = true;
    return true;
  }

  uintptr_t p = (uintptr_t)ptr;
  if ((p - m->base) % sizeof(ant_rope_heap_t) != 0 ||
      sizeof(ant_rope_heap_t) > m->end - p)
    return false;

  if (js->rope_gc.minor_marking && m->kind == GC_ROPE_POOL_OLD) return false;
  ant_rope_heap_t *rope = (ant_rope_heap_t *)ptr;
  if (rope->mark_epoch == js->rope_gc.mark_epoch) return false;
  rope->mark_epoch = js->rope_gc.mark_epoch;
  m->has_live = true;
  return true;
}

bool gc_ropes_contains(
  ant_t *js, const void *ptr, size_t size, size_t align
) {
  if (!js || !ptr || size == 0) return false;
  if (align > 1 && (align & (align - 1u)) != 0) return false;
  uintptr_t p = (uintptr_t)ptr;
  if (align > 1 && (p & (align - 1u)) != 0) return false;
  gc_rope_mark_t *m = rope_mark_find(js, ptr);
  return m && size <= m->end - p;
}

static void unlink_rope_block(ant_pool_t *pool, ant_pool_block_t *block) {
  if (block->prev) block->prev->next = block->next;
  else pool->head = block->next;
  if (block->next) block->next->prev = block->prev;
  block->next = NULL;
  block->prev = NULL;
}

static void recycle_rope_block(ant_pool_t *pool, ant_pool_block_t *block) {
  block->used = 0;
  pool_free_set_next(block, pool->free_head);
  pool_block_madvise_free(block);
  pool->free_head = block;
}

static void promote_rope_block(ant_t *js, ant_pool_block_t *block) {
  for (size_t off = 0; off + sizeof(ant_rope_heap_t) <= block->used; off += sizeof(ant_rope_heap_t)) {
    ant_rope_heap_t *rope = (ant_rope_heap_t *)(block->data + off);
    rope->flags &= (uint16_t)~ANT_ROPE_FLAG_YOUNG;
  }

  ant_pool_t *old = &js->rope_gc.old;
  block->next = old->head;
  
  if (old->head) old->head->prev = block;
  old->head = block;
}

static void trim_rope_free_blocks(ant_pool_t *pool, int keep) {
  ant_pool_block_t *f = pool->free_head;
  ant_pool_block_t *last = NULL;
  int kept = 0;
  while (f && kept < keep) {
    last = f;
    f = pool_free_next(f);
    kept++;
  }
  if (last) pool_free_set_next(last, NULL);
  else pool->free_head = NULL;
  while (f) {
    ant_pool_block_t *next = pool_free_next(f);
    pool_block_free(f);
    f = next;
  }
}

void gc_ropes_sweep(ant_t *js, bool minor) {
  if (js->rope_gc.conservative_marking) {
    ANT_ASSERT(!minor, "conservative rope sweep must be a major");
    for (ant_pool_block_t *b = js->rope_gc.young.head; b;) {
      ant_pool_block_t *next = b->next;
      unlink_rope_block(&js->rope_gc.young, b);
      if (b->used) promote_rope_block(js, b);
      else recycle_rope_block(&js->rope_gc.young, b);
      b = next;
    }
  }

  gc_rope_mark_t *marks = js->rope_gc.marks;
  size_t count = js->rope_gc.mark_count;
  for (size_t i = 0; i < count; i++) {
    gc_rope_mark_t *m = &marks[i];
    if (minor && m->kind != GC_ROPE_POOL_YOUNG) continue;

    if (m->kind == GC_ROPE_POOL_YOUNG) {
      unlink_rope_block(m->pool, m->block);
      if (m->has_live) promote_rope_block(js, m->block);
      else recycle_rope_block(&js->rope_gc.young, m->block);
      continue;
    }

    if (!m->has_live) {
      unlink_rope_block(m->pool, m->block);
      ant_pool_t *recycle = m->kind == GC_ROPE_POOL_OLD
        ? &js->rope_gc.young : &js->pool.rope;
      recycle_rope_block(recycle, m->block);
    }
  }

  trim_rope_free_blocks(&js->pool.rope, 2);
  trim_rope_free_blocks(&js->rope_gc.young, 2);
  js->rope_gc.young_alloc = 0;
  js->rope_gc.mark_count = 0;
  js->rope_gc.minor_marking = false;
  js->rope_gc.conservative_marking = false;
}
