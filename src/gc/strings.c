#include "internal.h"
#include "gc/strings.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  uint8_t *bits;
  size_t n_slots;
} gc_bitmap_t;

typedef struct {
  uintptr_t base;
  uintptr_t end;
  ant_pool_block_t *block;
  ant_pool_bucket_t *bucket;
  size_t stride;
  gc_bitmap_t bitmap;
} gc_block_mark_t;

typedef struct {
  uintptr_t ptr;
  ant_large_string_alloc_t *alloc;
} gc_large_string_mark_t;

static gc_block_mark_t *g_string_marks = NULL;
static int g_string_mark_count = 0;
static int g_string_mark_cap = 0;

static gc_large_string_mark_t *g_large_string_marks = NULL;
static int g_large_string_mark_count = 0;
static int g_large_string_mark_cap = 0;

static gc_bitmap_t bitmap_alloc(size_t n_slots) {
  size_t nbytes = (n_slots + 7u) / 8u;
  uint8_t *bits = calloc(1, nbytes);
  return (gc_bitmap_t){ .bits = bits, .n_slots = n_slots };
}

static inline void bitmap_set(gc_bitmap_t *bm, size_t idx) {
  if (idx < bm->n_slots && bm->bits) bm->bits[idx / 8u] |= (uint8_t)(1u << (idx % 8u));
}

static inline bool bitmap_get(const gc_bitmap_t *bm, size_t idx) {
  if (idx >= bm->n_slots || !bm->bits) return false;
  return ((bm->bits[idx / 8u] >> (idx % 8u)) & 1u) != 0;
}

static inline void bitmap_free(gc_bitmap_t *bm) {
  free(bm->bits);
  bm->bits = NULL;
  bm->n_slots = 0;
}

static int pooled_mark_cmp(const void *a, const void *b) {
  const gc_block_mark_t *ma = (const gc_block_mark_t *)a;
  const gc_block_mark_t *mb = (const gc_block_mark_t *)b;
  if (ma->base < mb->base) return -1;
  if (ma->base > mb->base) return 1;
  return 0;
}

static int large_mark_cmp(const void *a, const void *b) {
  const gc_large_string_mark_t *ma = (const gc_large_string_mark_t *)a;
  const gc_large_string_mark_t *mb = (const gc_large_string_mark_t *)b;
  if (ma->ptr < mb->ptr) return -1;
  if (ma->ptr > mb->ptr) return 1;
  return 0;
}

static void collect_bucket_blocks(ant_pool_bucket_t *bucket) {
  if (!bucket || bucket->slot_stride == 0) return;
  size_t stride = bucket->slot_stride;

  for (ant_pool_block_t *b = bucket->head; b; b = b->next) {
    size_t n_slots = b->used / stride;
    if (n_slots == 0) continue;
    
    if (g_string_mark_count >= g_string_mark_cap) {
      int cap = g_string_mark_cap ? g_string_mark_cap * 2 : 32;
      g_string_marks = realloc(g_string_marks, (size_t)cap * sizeof(gc_block_mark_t));
      g_string_mark_cap = cap;
    }
    
    gc_block_mark_t *m = &g_string_marks[g_string_mark_count++];
    m->base = (uintptr_t)b->data;
    m->end = m->base + b->used;
    m->block = b;
    m->bucket = bucket;
    m->stride = stride;
    m->bitmap = bitmap_alloc(n_slots);
  }
}

static void collect_large_strings(ant_large_string_space_t *space) {
  if (!space) return;

  for (ant_large_string_alloc_t *cur = space->live; cur; cur = cur->next) {
    cur->marked = 0;
    
    if (g_large_string_mark_count >= g_large_string_mark_cap) {
      int cap = g_large_string_mark_cap ? g_large_string_mark_cap * 2 : 32;
      g_large_string_marks = realloc(g_large_string_marks, (size_t)cap * sizeof(gc_large_string_mark_t));
      g_large_string_mark_cap = cap;
    }
    
    g_large_string_marks[g_large_string_mark_count++] = (gc_large_string_mark_t){
      .ptr = (uintptr_t)large_string_flat_ptr(cur),
      .alloc = cur,
    };
  }
}

static inline void unlink_block(ant_pool_bucket_t *bucket, ant_pool_block_t *block) {
  if (block->prev) block->prev->next = block->next;
  else bucket->head = block->next;
  if (block->next) block->next->prev = block->prev;
}

static inline void large_string_unlink(ant_large_string_alloc_t **head, ant_large_string_alloc_t *alloc) {
  if (!head || !alloc) return;
  if (alloc->prev) alloc->prev->next = alloc->next;
  else *head = alloc->next;
  if (alloc->next) alloc->next->prev = alloc->prev;
  alloc->next = NULL;
  alloc->prev = NULL;
}

static inline void large_string_push_front(ant_large_string_alloc_t **head, ant_large_string_alloc_t *alloc) {
  if (!head || !alloc) return;
  alloc->prev = NULL;
  alloc->next = *head;
  if (*head) (*head)->prev = alloc;
  *head = alloc;
}

static void large_string_promote_quarantine(ant_large_string_space_t *space) {
  if (!space) return;
  ant_large_string_alloc_t *cur = space->quarantine;
  while (cur) {
    ant_large_string_alloc_t *next = cur->next;
    if (cur->quarantine_epoch < space->gc_epoch) {
      large_string_unlink(&space->quarantine, cur);
      large_string_push_front(&space->reusable, cur);
    } cur = next;
  }
}

static size_t large_string_reusable_budget(const ant_large_string_space_t *space) {
  size_t live_capacity = 0;
  for (const ant_large_string_alloc_t *cur = space ? space->live : NULL; cur; cur = cur->next) {
    live_capacity += cur->capacity;
  }

  size_t budget = live_capacity / 4u;
  size_t min_budget = 4u * 1024u * 1024u;
  size_t max_budget = 64u * 1024u * 1024u;

  if (budget < min_budget) budget = min_budget;
  if (budget > max_budget) budget = max_budget;
  
  return budget;
}

static void large_string_trim_reusable(ant_large_string_space_t *space) {
  if (!space) return;
  size_t reusable_capacity = 0;
  
  for (ant_large_string_alloc_t *cur = space->reusable; cur; cur = cur->next) {
    reusable_capacity += cur->capacity;
  }

  size_t budget = large_string_reusable_budget(space);
  ant_large_string_alloc_t *cur = space->reusable;
  
  while (cur && reusable_capacity > budget) {
    ant_large_string_alloc_t *next = cur->next;
    reusable_capacity -= cur->capacity;
    large_string_unlink(&space->reusable, cur);
    ant_arena_free(cur, cur->alloc_size);
    cur = next;
  }
}

void gc_strings_begin(ant_t *js) {
  g_string_mark_count = 0;
  g_large_string_mark_count = 0;

  ant_string_pool_t *pool = &js->pool.string;
  for (int i = 0; i < ANT_POOL_SIZE_CLASS_COUNT; i++) {
    pool->classes[i].slot_free = NULL;
    collect_bucket_blocks(&pool->classes[i]);
  }

  pool->large.gc_epoch++;
  if (pool->large.gc_epoch == 0) pool->large.gc_epoch = 1;
  
  large_string_promote_quarantine(&pool->large);
  collect_large_strings(&pool->large);

  if (g_string_mark_count > 1) qsort(g_string_marks, 
    (size_t)g_string_mark_count, sizeof(gc_block_mark_t), pooled_mark_cmp
  );
  
  if (g_large_string_mark_count > 1) qsort(g_large_string_marks, 
    (size_t)g_large_string_mark_count, sizeof(gc_large_string_mark_t), large_mark_cmp
  );
}

void gc_strings_mark(ant_t *js, const void *ptr) {
  if (!ptr) return;
  uintptr_t p = (uintptr_t)ptr;

  if (g_string_mark_count > 0) {
    int lo = 0;
    int hi = g_string_mark_count - 1;
    
    while (lo <= hi) {
    int mid = lo + (hi - lo) / 2;
    gc_block_mark_t *m = &g_string_marks[mid];
    
    if (p < m->base) hi = mid - 1;
    else if (p >= m->end) lo = mid + 1;
    else {
      size_t offset = p - m->base;
      if (offset % m->stride == 0) bitmap_set(&m->bitmap, offset / m->stride);
      return;
    }}
  }

  if (g_large_string_mark_count == 0) return;
  int lo = 0; int hi = g_large_string_mark_count - 1;
  
  while (lo <= hi) {
  int mid = lo + (hi - lo) / 2;
  gc_large_string_mark_t *m = &g_large_string_marks[mid];
  
  if (p < m->ptr) hi = mid - 1;
  else if (p > m->ptr) lo = mid + 1;
  else {
    m->alloc->marked = 1;
    return;
  }}
}

void gc_strings_sweep(ant_t *js) {
  for (int i = 0; i < g_string_mark_count; i++) {
    gc_block_mark_t *m = &g_string_marks[i];
    ant_pool_bucket_t *bucket = m->bucket;

    bool any_live = false;
    size_t n_slots = m->bitmap.n_slots;
    
    for (size_t j = 0; j < n_slots; j++) {
    if (bitmap_get(&m->bitmap, j)) {
      any_live = true;
      break;
    }}

    if (!any_live && bucket) {
      unlink_block(bucket, m->block);
      m->block->used = 0;
      m->block->next = NULL;
      if (bucket->current == m->block) bucket->current = NULL;
      pool_free_set_next(m->block, bucket->free_head);
      pool_block_madvise_free(m->block);
      bucket->free_head = m->block;
    } else if (any_live && bucket && m->stride >= sizeof(void *)) {
      uintptr_t base = m->base;
      for (size_t j = 0; j < n_slots; j++) {
        if (bitmap_get(&m->bitmap, j)) continue;
        void *slot = (void *)(base + j * m->stride);
        void *old_head = bucket->slot_free;
        memcpy(slot, &old_head, sizeof(void *));
        bucket->slot_free = slot;
      }
    }

    bitmap_free(&m->bitmap);
  }

  ant_large_string_space_t *space = &js->pool.string.large;
  ant_large_string_alloc_t *cur = space->live;
  while (cur) {
    ant_large_string_alloc_t *next = cur->next;
    if (cur->marked) cur->marked = 0; else {
      large_string_unlink(&space->live, cur);
      cur->quarantine_epoch = space->gc_epoch;
      large_string_push_front(&space->quarantine, cur);
    } cur = next;
  }

  large_string_trim_reusable(space);

  for (int i = 0; i < ANT_POOL_SIZE_CLASS_COUNT; i++) {
    ant_pool_bucket_t *bucket = &js->pool.string.classes[i];
    ant_pool_block_t *f = bucket->free_head;
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
    
    f = bucket->free_head;
    if (kept > 0) {
      for (int k = 1; k < kept && f; k++) f = pool_free_next(f);
      if (f) pool_free_set_next(f, NULL);
    } else bucket->free_head = NULL;
  }

  g_string_mark_count = 0;
  g_large_string_mark_count = 0;
}
