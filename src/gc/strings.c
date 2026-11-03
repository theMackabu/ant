#include "gc.h"
#include "gc/verify.h"
#include "gc/strings.h"
#include "utils.h"
#include "utf8.h"
#include "numbers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

typedef struct {
  uint8_t *bits;
  size_t n_slots;
} gc_bitmap_t;

typedef struct gc_string_block_mark {
  uintptr_t base;
  uintptr_t end;
  ant_pool_block_t *block;
  ant_pool_bucket_t *bucket;
  size_t stride;
  gc_bitmap_t bitmap;
} gc_block_mark_t;

typedef struct {
  uintptr_t page;
  uint32_t index;
} gc_string_page_t;

static constexpr size_t GC_STRING_PAGE_SHIFT = 12;
static constexpr size_t GC_STRING_PAGE_CACHE = 256;

static _Thread_local gc_string_page_t gc_string_pages[GC_STRING_PAGE_CACHE];

typedef struct gc_large_string_mark {
  uintptr_t ptr;
  ant_large_string_alloc_t *alloc;
} gc_large_string_mark_t;

// Bumped by every string sweep on this thread, in any isolate
// the thread-local string caches remember a string by address
// and a sweep may free that address for reuse.
static _Thread_local uint64_t g_strings_sweep_epoch = 1;
uint64_t gc_strings_sweep_epoch(void) { return g_strings_sweep_epoch; }

static gc_bitmap_t bitmap_alloc(size_t n_slots) {
  size_t nbytes = (n_slots + 7u) / 8u;
  uint8_t *bits = calloc(1, nbytes);
  return (gc_bitmap_t){ .bits = bits, .n_slots = n_slots };
}

static inline void bitmap_set(gc_bitmap_t *bm, size_t idx) {
  if (idx < bm->n_slots && bm->bits) bm->bits[idx / 8u] |= (uint8_t)(1u << (idx % 8u));
}

static inline void bitmap_set_shared(gc_bitmap_t *bm, size_t idx) {
  if (idx >= bm->n_slots || !bm->bits) return;
  uint8_t bit = (uint8_t)(1u << (idx % 8u));
  if (!(__atomic_load_n(&bm->bits[idx / 8u], __ATOMIC_RELAXED) & bit))
    __atomic_fetch_or(&bm->bits[idx / 8u], bit, __ATOMIC_RELAXED);
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

static void collect_bucket_blocks(ant_t *js, ant_pool_bucket_t *bucket) {
  if (!bucket || bucket->slot_stride == 0) return;
  size_t stride = bucket->slot_stride;

  for (ant_pool_block_t *b = bucket->head; b; b = b->next) {
    size_t n_slots = b->used / stride;
    if (n_slots == 0) continue;
    
    gc_bitmap_t bitmap = bitmap_alloc(n_slots);
    if (!bitmap.bits) continue;

    gc_block_mark_t *marks = vec_grow(
      js->gc.string_marks, &js->gc.string_mark_cap,
      js->gc.string_mark_len + 1, sizeof(*marks), 32
    );

    if (!marks) {
      bitmap_free(&bitmap);
      continue;
    }

    js->gc.string_marks = marks;
    gc_block_mark_t *m = &marks[js->gc.string_mark_len++];
    
    m->base = (uintptr_t)b->data;
    m->end = m->base + b->used;
    m->block = b;
    m->bucket = bucket;
    m->stride = stride;
    m->bitmap = bitmap;
  }
}

static void collect_large_strings(ant_t *js, ant_large_string_space_t *space) {
  if (!space) return;

  for (ant_large_string_alloc_t *cur = space->live; cur; cur = cur->next) {
    cur->marked = 0;
    
    gc_large_string_mark_t *marks = vec_grow(
      js->gc.large_string_marks, &js->gc.large_string_mark_cap,
      js->gc.large_string_mark_len + 1, sizeof(*marks), 32
    );
    
    if (!marks) { 
      cur->marked = 1;
      continue;
    }
    
    js->gc.large_string_marks = marks;

    marks[js->gc.large_string_mark_len++] = (gc_large_string_mark_t){
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
  size_t taken = space->cycle_taken > space->cycle_taken_prev ? space->cycle_taken : space->cycle_taken_prev;

  if (budget < min_budget) budget = min_budget;
  if (budget < 2u * taken) budget = 2u * taken;
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
    ant_cage_free(cur, cur->alloc_size);
    cur = next;
  }
}

void gc_strings_begin(ant_t *js) {
  memset(gc_string_pages, 0, sizeof(gc_string_pages));
  
  js->gc.string_mark_len = 0;
  js->gc.large_string_mark_len = 0;

  ant_string_pool_t *pool = &js->pool.string;
  for (int i = 0; i < ANT_POOL_SIZE_CLASS_COUNT; i++) {
    pool->classes[i].slot_free = NULL;
    collect_bucket_blocks(js, &pool->classes[i]);
  }

  pool->large.gc_epoch++;
  if (pool->large.gc_epoch == 0) pool->large.gc_epoch = 1;
  
  large_string_promote_quarantine(&pool->large);
  collect_large_strings(js, &pool->large);

  if (js->gc.string_mark_len > 1) qsort(js->gc.string_marks, 
    (size_t)js->gc.string_mark_len, sizeof(gc_block_mark_t), pooled_mark_cmp
  );
  
  if (js->gc.large_string_mark_len > 1) 
    qsort(js->gc.large_string_marks, (size_t)js->gc.large_string_mark_len, sizeof(gc_large_string_mark_t), large_mark_cmp);
}

static inline void gc_string_mark_slot(gc_block_mark_t *m, uintptr_t p, bool shared) {
  size_t offset = p - m->base;
  if (offset % m->stride) return;
  if (shared) bitmap_set_shared(&m->bitmap, offset / m->stride);
  else bitmap_set(&m->bitmap, offset / m->stride);
}

static inline __attribute__((always_inline)) void gc_strings_mark_impl(ant_t *js, const void *ptr, bool shared) {
  if (!ptr) return;
  uintptr_t p = (uintptr_t)ptr;

  if (js->gc.string_mark_len > 0) {
    uintptr_t page = p >> GC_STRING_PAGE_SHIFT;
    gc_string_page_t *slot = &gc_string_pages[page % GC_STRING_PAGE_CACHE];
    
    if (slot->page == page && slot->index < js->gc.string_mark_len) {
      gc_block_mark_t *m = &js->gc.string_marks[slot->index];
      if (p >= m->base && p < m->end) { gc_string_mark_slot(m, p, shared); return; }
    }
    
    int lo = 0;
    int hi = (int)js->gc.string_mark_len - 1;
    
    while (lo <= hi) {
    int mid = lo + (hi - lo) / 2;
    gc_block_mark_t *m = &js->gc.string_marks[mid];
    
    if (p < m->base) hi = mid - 1;
    else if (p >= m->end) lo = mid + 1;
    else {
      slot->page = page;
      slot->index = (uint32_t)mid;
      gc_string_mark_slot(m, p, shared);
      return;
    }}
  }

  if (js->gc.large_string_mark_len == 0) return;
  int lo = 0; int hi = (int)js->gc.large_string_mark_len - 1;
  
  while (lo <= hi) {
  int mid = lo + (hi - lo) / 2;
  gc_large_string_mark_t *m = &js->gc.large_string_marks[mid];
  
  if (p < m->ptr) hi = mid - 1;
  else if (p > m->ptr) lo = mid + 1;
  else {
    if (shared) __atomic_store_n(&m->alloc->marked, 1, __ATOMIC_RELAXED);
    else m->alloc->marked = 1;
    return;
  }}
}

void gc_strings_mark(ant_t *js, const void *ptr) {
  gc_strings_mark_impl(js, ptr, false);
}

void gc_strings_mark_shared(ant_t *js, const void *ptr) {
  gc_strings_mark_impl(js, ptr, true);
}

void gc_strings_sweep(ant_t *js) {
  for (size_t i = 0; i < js->gc.string_mark_len; i++) {
    gc_block_mark_t *m = &js->gc.string_marks[i];
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
      GC_VERIFY_POISON_BYTES(m->base, m->end - m->base);
      m->block->used = 0;
      m->block->next = NULL;
      if (bucket->current == m->block) bucket->current = NULL;
      pool_free_set_next(m->block, bucket->free_head);
      bucket->free_head = m->block;
    } else if (any_live && bucket && m->stride >= sizeof(void *)) {
      uintptr_t base = m->base;
      for (size_t j = 0; j < n_slots; j++) {
        if (bitmap_get(&m->bitmap, j)) continue;
        void *slot = (void *)(base + j * m->stride);
        GC_VERIFY_POISON_BYTES(slot, m->stride);
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
  space->cycle_taken_prev = space->cycle_taken;
  space->cycle_taken = 0;

  size_t keep_budget = gc_pool_major_threshold(js);
  for (int i = 0; i < ANT_POOL_SIZE_CLASS_COUNT; i++) {
    ant_pool_bucket_t *bucket = &js->pool.string.classes[i];
    ant_pool_block_t *f = bucket->free_head;
    ant_pool_block_t *kept_tail = NULL;
    
    uint32_t keep_blocks = 
      bucket->blocks_taken < bucket->blocks_taken_prev
      ? bucket->blocks_taken : bucket->blocks_taken_prev;
      
    bucket->blocks_taken_prev = bucket->blocks_taken;
    bucket->blocks_taken = 0;
    bucket->free_head = NULL;
    
    while (f) {
      ant_pool_block_t *next = pool_free_next(f);
      size_t bytes = sizeof(ant_pool_block_t) + f->cap;
      
      if (keep_blocks > 0 && bytes <= keep_budget) {
        keep_blocks--;
        keep_budget -= bytes;
        pool_free_set_next(f, NULL);
        
        if (kept_tail) pool_free_set_next(kept_tail, f);
        else bucket->free_head = f;
        
        kept_tail = f;
      } else pool_block_free(f);
      
      f = next;
    }
  }

  js->gc.string_mark_len = 0;
  js->gc.large_string_mark_len = 0;
  
  memset(js->intern_memo, 0, sizeof(js->intern_memo));
  gc_strings_epoch_bump();
}

void gc_strings_epoch_bump(void) {
  g_strings_sweep_epoch++;
  if (g_strings_sweep_epoch == 0) g_strings_sweep_epoch = 1;
}

void gc_strings_destroy(ant_t *js) {
  free(js->gc.string_marks);
  free(js->gc.large_string_marks);
  js->gc.string_marks = NULL;
  js->gc.large_string_marks = NULL;
  js->gc.string_mark_len = js->gc.string_mark_cap = 0;
  js->gc.large_string_mark_len = js->gc.large_string_mark_cap = 0;
}

typedef struct interned_string {
  uint64_t hash;
  ant_value_t value;
  char *str;
  size_t len;
} interned_string_t;

typedef struct intern_chunk {
  struct intern_chunk *next;
  size_t used;
  size_t cap;
  alignas(max_align_t) unsigned char data[];
} intern_chunk_t;

struct ant_intern_table {
  size_t mask;
  size_t count;
  size_t bytes;
  intern_chunk_t *chunks;
  interned_string_t *slots[];
};

static constexpr size_t INTERN_TABLE_MIN = 2048;
static constexpr size_t INTERN_CHUNK_SIZE = 64 * 1024;

static ant_intern_table_t *intern_table_alloc(size_t cap) {
  ant_intern_table_t *t = calloc(1, sizeof(*t) + cap * sizeof(t->slots[0]));
  if (t) t->mask = cap - 1;
  return t;
}

static void intern_table_place(ant_intern_table_t *t, interned_string_t *entry) {
  size_t i = (size_t)entry->hash & t->mask;
  while (t->slots[i]) i = (i + 1) & t->mask;
  t->slots[i] = entry;
}

static interned_string_t *intern_table_probe(const ant_intern_table_t *t, uint64_t h, const char *str, size_t len) {
  for (size_t i = (size_t)h & t->mask;; i = (i + 1) & t->mask) {
    interned_string_t *e = t->slots[i];
    if (!e) return NULL;
    if (e->hash == h && e->len == len && memcmp(e->str, str, len) == 0) return e;
  }
}

static ant_intern_table_t *intern_table_reserve(ant_t *js) {
  ant_intern_table_t *t = js->intern.table;
  if (t && (t->count + 1) * 2 < t->mask + 1) return t;

  ant_intern_table_t *next = intern_table_alloc(t ? (t->mask + 1) * 2 : INTERN_TABLE_MIN);
  if (!next) return t && t->count < t->mask ? t : NULL;

  if (t) {
    for (size_t i = 0; i <= t->mask; i++) if (t->slots[i]) intern_table_place(next, t->slots[i]);
    next->count = t->count;
    next->bytes = t->bytes;
    next->chunks = t->chunks;
    free(t);
  }
  js->intern.table = next;
  return next;
}

static interned_string_t *intern_entry_alloc(ant_intern_table_t *t, size_t size) {
  size = (size + alignof(interned_string_t) - 1) & ~(alignof(interned_string_t) - 1);
  intern_chunk_t *chunk = t->chunks;
  if (!chunk || chunk->cap - chunk->used < size) {
    size_t cap = size > INTERN_CHUNK_SIZE ? size : INTERN_CHUNK_SIZE;
    if (!(chunk = malloc(sizeof(*chunk) + cap))) return NULL;
    chunk->next = t->chunks;
    chunk->used = 0;
    chunk->cap = cap;
    t->chunks = chunk;
  }
  interned_string_t *entry = (interned_string_t *)(chunk->data + chunk->used);
  chunk->used += size;
  return entry;
}

void intern_table_free(ant_t *js) {
  ant_intern_table_t *t = js->intern.table;
  if (!t) return;
  for (intern_chunk_t *chunk = t->chunks, *next; chunk; chunk = next) {
    next = chunk->next;
    free(chunk);
  }
  free(t);
  js->intern.table = NULL;
}

ant_offset_t vstrlen(ant_t *js, ant_value_t v) { 
  if (str_is_heap_rope(v)) {
    ant_rope_heap_t *rope = ant_str_rope_ptr(v);
    return rope ? rope->len : 0;
  }
  if (str_is_heap_builder(v)) {
    ant_string_builder_t *builder = ant_str_builder_ptr(v);
    return builder ? builder->len : 0;
  }
  ant_flat_string_t *flat = ant_str_flat_ptr(v);
  return flat ? flat->len : 0;
}

ant_value_t str_materialize(ant_t *js, ant_value_t value) {
  if (vtype(value) != kTypeString) return value;
  if (str_is_heap_rope(value)) return rope_flatten(js, value);
  if (str_is_heap_builder(value)) return builder_flatten(js, value);
  return value;
}

ant_offset_t vstr(ant_t *js, ant_value_t value, ant_offset_t *len) {
  if (str_is_heap_rope(value) || str_is_heap_builder(value)) {
    ant_value_t flat = str_materialize(js, value);
    assert(!is_err(flat));
    value = flat;
  }
  
  const char *ptr = NULL;
  ant_offset_t slen = assert_flat_string_len(value, &ptr);
  if (len) *len = slen;
  return (ant_offset_t)(uintptr_t)ptr;
}

const char *intern_find(ant_t *js, const char *str, size_t len) {
  ant_intern_table_t *t = js->intern.table;
  if (!t) return NULL;
  interned_string_t *e = intern_table_probe(t, hash_key(str, len), str, len);
  return e ? e->str : NULL;
}

const char *intern_string(ant_t *js, const char *str, size_t len) {
  uint64_t h = hash_key(str, len);
  ant_intern_table_t *t = js->intern.table;
  interned_string_t *e = t ? intern_table_probe(t, h, str, len) : NULL;
  
  if (e) return e->str;
  if (!(t = intern_table_reserve(js))) return NULL;

  size_t alloc_size = sizeof(interned_string_t) + len + 1;
  if (!(e = intern_entry_alloc(t, alloc_size))) return NULL;

  e->str = (char *)(e + 1);
  memcpy(e->str, str, len);
  e->str[len] = '\0';
  e->len = len;
  e->hash = h;
  e->value = 0;
  intern_table_place(t, e);

  t->count++;
  t->bytes += alloc_size;
  
  return e->str;
}

const char *intern_find_flat(ant_t *js, const char *bytes, size_t len) {
  ant_intern_memo_entry_t *entry = &js->intern_memo[((uintptr_t)bytes >> 4) & (INTERN_MEMO_SIZE - 1)];
  if (entry->bytes == bytes) return entry->interned;

  const char *interned = intern_find(js, bytes, len);
  if (interned) *entry = (ant_intern_memo_entry_t){ bytes, interned };
  
  return interned;
}

size_t intern_length(const char *interned) {
  return ((const interned_string_t *)(const void *)interned)[-1].len;
}

ant_value_t intern_value(ant_t *js, const char *interned) {
  interned_string_t *entry = &((interned_string_t *)(void *)interned)[-1];
  if (entry->value) return entry->value;
  ant_value_t value = js_mkstr_permanent(js, interned, entry->len);
  if (!is_err(value)) entry->value = value;
  return value;
}

js_intern_stats_t js_intern_stats(ant_t *js) {
  const ant_intern_table_t *t = js->intern.table;
  return (js_intern_stats_t){ .count = t ? t->count : 0, .bytes = t ? t->bytes : 0 };
}

ant_value_t mkstr_with_ascii(ant_t *js, const void *ptr, size_t len, bool known_ascii) {
  ant_flat_string_t *flat = (ant_flat_string_t *)js_type_alloc(
    js, ANT_ALLOC_STRING, sizeof(*flat) + len + 1, 
    _Alignof(ant_flat_string_t)
  );
  
  if (!flat) return js_mkerr(js, "oom");
  flat->len = (ant_offset_t)len;
  
  if (ptr && len > 0) memcpy(flat->bytes, ptr, len);
  flat->bytes[len] = '\0';
  
  uint8_t ascii = known_ascii ? STR_ASCII_YES : ((ptr || len == 0)
    ? str_detect_ascii_bytes(flat->bytes, len)
    : STR_ASCII_UNKNOWN);
  
  if (ptr) str_flat_finish(flat, ascii);
  else str_flat_init_meta(flat, ascii);

  return mkref(kTypeString, flat);
}

ant_value_t js_empty_string(ant_t *js) {
  if (js->empty_str) return js->empty_str;
  ant_value_t empty = js_mkstr_permanent(js, "", 0);
  if (!is_err(empty)) js->empty_str = empty;
  return empty;
}

ant_value_t js_mkstr(ant_t *js, const void *ptr, size_t len) {
  if (len == 0 && ptr) return js_empty_string(js);
  return mkstr_with_ascii(js, ptr, len, false);
}

ant_value_t js_mkstr_ascii(ant_t *js, const void *ptr, size_t len) {
  return mkstr_with_ascii(js, ptr, len, true);
}

static ant_value_t mkstr_utf8_finish(ant_value_t str, bool ascii) {
  ant_flat_string_t *flat = ant_str_flat_ptr(str);
  str_flat_init_meta(flat, ascii ? STR_ASCII_YES : STR_ASCII_NO);
  if (!ascii) str_flat_set_utf_valid_state(flat, STR_UTF_VALID);
  return str;
}

ant_value_t js_mkstr_utf8_reserve(ant_t *js, size_t len, char **out_bytes) {
  ant_value_t str = mkstr_with_ascii(js, NULL, len, false);
  *out_bytes = is_err(str) ? NULL : ant_str_flat_ptr(str)->bytes;
  return str;
}

ant_value_t js_mkstr_utf8_valid(ant_t *js, const void *ptr, size_t len, bool ascii) {
  if (len == 0) return js_empty_string(js);

  char *bytes;
  ant_value_t str = js_mkstr_utf8_reserve(js, len, &bytes);
  if (is_err(str)) return str;
  
  memcpy(bytes, ptr, len);
  return mkstr_utf8_finish(str, ascii);
}

static ant_value_t mkstr_utf8_replaced(ant_t *js, const char *ptr, size_t len, size_t valid) {
  size_t out_len;
  char *out = utf8_replace_invalid(ptr, len, valid, &out_len);
  if (!out) return js_mkerr(js, "oom");

  ant_value_t result = js_mkstr_utf8_valid(js, out, out_len, false);
  free(out);

  return result;
}

ant_value_t js_mkstr_utf8_try(ant_t *js, const void *ptr, size_t len, size_t *out_valid) {
  *out_valid = len;
  if (len == 0) return js_empty_string(js);

  char *bytes;
  ant_value_t str = js_mkstr_utf8_reserve(js, len, &bytes);
  if (is_err(str)) return str;

  bool ascii;
  *out_valid = utf8_copy_valid_prefix(bytes, (const char *)ptr, len, &ascii);
  return *out_valid == len ? mkstr_utf8_finish(str, ascii) : Ant_Value_Undefined;
}

ant_value_t js_mkstr_utf8(ant_t *js, const void *ptr, size_t len) {
  size_t valid;
  ant_value_t str = js_mkstr_utf8_try(js, ptr, len, &valid);
  if (vtype(str) != kTypeUndefined) return str;
  return mkstr_utf8_replaced(js, (const char *)ptr, len, valid);
}

ant_value_t js_mkstr_utf8_commit(ant_t *js, ant_value_t str) {
  ant_flat_string_t *flat = ant_str_flat_ptr(str);
  size_t len = (size_t)flat->len;
  if (len == 0) return js_empty_string(js);

  bool ascii;
  size_t valid = utf8_valid_prefix(flat->bytes, len, &ascii);
  if (valid < len) return mkstr_utf8_replaced(js, flat->bytes, len, valid);
  return mkstr_utf8_finish(str, ascii);
}

ant_value_t js_mkstr_wtf8_lossy(ant_t *js, const void *ptr, size_t len) {
  size_t clean_len = 0;
  char *clean = wtf8_replace_invalid((const char *)ptr, len, &clean_len);
  if (!clean) return js_mkstr(js, ptr, len);

  ant_value_t result = js_mkstr(js, clean, clean_len);
  free(clean);
  return result;
}

ant_value_t js_mkstr_utf8_strip_bom(ant_t *js, const void *ptr, size_t len) {
  const uint8_t *bytes = (const uint8_t *)ptr;
  if (len >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf) {
    bytes += 3;
    len -= 3;
  }
  return js_mkstr_utf8(js, bytes, len);
}

ant_value_t js_mkstr_int32(ant_t *js, int32_t value) {
  size_t len = ant_int32_chars_len(value);
  ant_value_t str = mkstr_with_ascii(js, NULL, len, true);
  if (!is_err(str)) ant_int32_write(ant_str_flat_ptr(str)->bytes, len, value);
  return str;
}

ant_value_t js_mkstr_byte_range(ant_t *js, const char *parent, size_t start, size_t len) {
  bool known_ascii = str_flat_ascii_state(str_flat_from_bytes(parent)) == STR_ASCII_YES;
  return mkstr_with_ascii(js, parent + start, len, known_ascii);
}

void *js_permanent_alloc(ant_t *js, size_t size, size_t align) {
  if (js->pool.permanent.block_size == 0) js->pool.permanent.block_size = ANT_POOL_STRING_BLOCK_SIZE;
  return pool_alloc_chain(&js->pool.permanent.head, NULL, js->pool.permanent.block_size, size, align);
}

ant_value_t js_mkstr_permanent(ant_t *js, const void *ptr, size_t len) {
  size_t size = sizeof(ant_flat_string_t) + len + 1;
  ant_flat_string_t *flat = js_permanent_alloc(js, size, _Alignof(ant_flat_string_t));
  
  if (!flat) return js_mkerr(js, "oom");
  flat->len = (ant_offset_t)len;
  
  if (ptr && len > 0) memcpy(flat->bytes, ptr, len);
  flat->bytes[len] = '\0';
  
  str_flat_init_meta(flat, (ptr || len == 0)
    ? str_detect_ascii_bytes(flat->bytes, len)
    : STR_ASCII_UNKNOWN
  );
  
  flat->meta |= STR_META_PERMANENT;
  return mkref(kTypeString, flat);
}

static bool rope_part_ascii(ant_value_t part) {
  if (str_is_heap_rope(part)) return (ant_str_rope_ptr(part)->flags & ANT_ROPE_FLAG_ASCII) != 0;
  if (str_is_heap_builder(part)) return false;
  
  ant_flat_string_t *flat = ant_str_flat_ptr(part);
  return flat && str_flat_ascii_state(flat) == STR_ASCII_YES;
}

ant_value_t js_mkrope(ant_t *js, ant_value_t left, ant_value_t right, ant_offset_t total_len, uint16_t depth) {
  if (js->rope_gc.young_alloc >= GC_ROPE_NURSERY_THRESHOLD) {
    GC_ROOT_SAVE(root_mark, js);
    GC_ROOT_PIN(js, left);
    GC_ROOT_PIN(js, right);
    gc_pressure(js);
    GC_ROOT_RESTORE(js, root_mark);
  }

  ant_rope_heap_t *rope = js_rope_alloc(js);
  if (!rope) return js_mkerr(js, "oom");
  
  rope->len = total_len;
  rope->depth = depth;
  rope->flags = ANT_ROPE_FLAG_YOUNG | (rope_part_ascii(left) && rope_part_ascii(right) ? ANT_ROPE_FLAG_ASCII : 0);
  rope->mark_epoch = 0;
  rope->left = left;
  rope->right = right;
  rope->cached = Ant_Value_Undefined;
  
  return ant_mkrope_value(rope);
}

ant_offset_t str_len_fast(ant_t *js, ant_value_t str) {
  if (vtype(str) != kTypeString) return 0;
  if (str_is_heap_rope(str)) return rope_len(str);
  if (str_is_heap_builder(str)) return builder_len(str);
  return assert_flat_string_len(str, NULL);
}

static ant_offset_t flat_utf16_len(ant_flat_string_t *flat) {
  if (!flat) return 0;

  uint8_t ascii_state = str_flat_ascii_state(flat);
  if (ascii_state == STR_ASCII_UNKNOWN) {
    ascii_state = str_detect_ascii_bytes(flat->bytes, (size_t)flat->len);
    str_flat_set_ascii_state(flat, ascii_state);
  }
  
  if (ascii_state == STR_ASCII_YES) return flat->len;
  ant_offset_t cached = str_flat_cached_utf16_len(flat);
  
  if (cached == STR_UTF16_LEN_UNKNOWN) {
    cached = (ant_offset_t)(str_flat_utf_valid_state(flat) == STR_UTF_VALID
      ? utf16_strlen_valid_utf8(flat->bytes, (size_t)flat->len)
      : utf16_strlen(flat->bytes, (size_t)flat->len));
    str_flat_set_utf16_len(flat, cached);
  }
  
  return cached;
}

static ant_offset_t builder_utf16_len_lazy(ant_t *js, ant_value_t value) {
  ant_string_builder_t *builder = assert_builder_ptr(value);
  ant_value_t cached = builder->cached;

  if (vtype(cached) == kTypeNumber) return (ant_offset_t)Ant_Value_AsNumberUnchecked(cached);
  if (vtype(cached) == kTypeString) return str_utf16_len(js, cached);

  ant_offset_t total = 0;
  if (builder->ascii_state == STR_ASCII_YES) total = builder->len;
  else {
    if (vtype(builder->snapshot) == kTypeString)
      total += str_utf16_len(js, builder->snapshot);
    for (ant_builder_chunk_t *chunk = builder->head; chunk; chunk = chunk->next)
      total += str_utf16_len(js, chunk->value);
    total += (ant_offset_t)utf16_strlen(builder->tail, builder->tail_len);
  }

  builder->cached = Ant_Value_FromNumber((double)total);
  return total;
}

typedef struct {
  ant_rope_heap_t *rope;
  ant_offset_t left_len;
} rope_utf16_frame_t;

// TODO: make cleaner
static ant_offset_t rope_utf16_len_lazy(ant_t *js, ant_value_t root) {
  if (rope_depth(root) == ANT_ROPE_DEPTH_SATURATED) {
    ant_value_t flat = rope_flatten(js, root);
    return is_err(flat) ? 0 : flat_utf16_len(ant_str_flat_ptr(flat));
  }

  enum { ROPE_UTF16_LOCAL_FRAMES = 64 };
  rope_utf16_frame_t local_frames[ROPE_UTF16_LOCAL_FRAMES];
  rope_utf16_frame_t *frames = local_frames;
  
  size_t frame_cap = ROPE_UTF16_LOCAL_FRAMES;
  size_t sp = 0;
  
  ant_value_t current = root;
  ant_offset_t result = 0;

  for (;;) {
  if (!str_is_heap_rope(current)) result = str_utf16_len(js, current); else {
    ant_rope_heap_t *rope = assert_rope_ptr(current);
    ant_value_t cached = rope->cached;
    
    if (rope->flags & ANT_ROPE_FLAG_ASCII) result = rope->len;
    else if (vtype(cached) == kTypeNumber) result = (ant_offset_t)Ant_Value_AsNumberUnchecked(cached);
    else if (vtype(cached) == kTypeString) result = str_utf16_len(js, cached);
    
    else {
      if (sp == frame_cap) {
        size_t next_cap = frame_cap * 2u;
        rope_utf16_frame_t *expanded = frames == local_frames
          ? (rope_utf16_frame_t *)malloc(next_cap * sizeof(*expanded))
          : (rope_utf16_frame_t *)realloc(frames, next_cap * sizeof(*expanded));
        if (!expanded) {
          ant_value_t flat = rope_flatten(js, root);
          if (is_err(flat)) return 0;
          return flat_utf16_len(ant_str_flat_ptr(flat));
        }
        if (frames == local_frames)
          memcpy(expanded, frames, sp * sizeof(*expanded));
        frames = expanded;
        frame_cap = next_cap;
      }
      frames[sp++] = (rope_utf16_frame_t){
        .rope = rope,
        .left_len = STR_UTF16_LEN_UNKNOWN,
      };
      current = rope->left;
      continue;
    }}

    while (sp > 0) {
      rope_utf16_frame_t *frame = &frames[sp - 1u];
      if (frame->left_len == STR_UTF16_LEN_UNKNOWN) {
        frame->left_len = result;
        current = frame->rope->right;
        break;
      }

      result += frame->left_len;
      frame->rope->cached = Ant_Value_FromNumber((double)result);
      sp--;
    }

    if (sp == 0) {
      if (frames != local_frames) free(frames);
      return result;
    }
  }
}

ant_offset_t str_utf16_len(ant_t *js, ant_value_t str) {
  if (vtype(str) != kTypeString) return 0;
  
  if (str_is_heap_rope(str)) {
    ant_rope_heap_t *rope = ant_str_rope_ptr(str);
    if (rope->flags & ANT_ROPE_FLAG_ASCII) return rope->len;
    
    ant_value_t cached = rope_cached_flat(str);
    if (vtype(cached) == kTypeNumber) return (ant_offset_t)Ant_Value_AsNumberUnchecked(cached);
    if (vtype(cached) == kTypeString) return str_utf16_len(js, cached);
    
    return rope_utf16_len_lazy(js, str);
  }
  
  if (str_is_heap_builder(str)) return builder_utf16_len_lazy(js, str);
  return flat_utf16_len(ant_str_flat_ptr(str));
}

char *js_getstr(ant_t *js, ant_value_t value, size_t *len) {
  if (vtype(value) != kTypeString) return NULL;
  ant_offset_t n, off = vstr(js, value, &n);
  if (len != NULL) *len = n;
  return (char *)(uintptr_t)off;
}
