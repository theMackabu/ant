#ifndef SLAB_H
#define SLAB_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// Slab allocator for fixed-size JS objects
// Pages are allocated inline from the bump allocator (like strings)
// Pages are tracked by offset in arrays - can be scattered in memory
// This approach uses existing memory infrastructure with no special commit logic

#define SLAB_CLASS_OBJ   0
#define SLAB_CLASS_PROP  1
#define SLAB_CLASS_COUNT 2

#define SLAB_OBJ_SIZE    24
#define SLAB_PROP_SIZE   24
#define SLAB_PAGE_SLOTS  512

typedef uint64_t slab_off_t;

// Callback to allocate a page from the bump allocator
// Returns offset of allocated memory, or 0 on failure
typedef slab_off_t (*slab_alloc_page_fn)(void *ctx, size_t size);

typedef struct slab_page {
  uint32_t free_head;      // Index of first free slot (0xFFFFFFFF if full)
  uint32_t slot_size;
  uint32_t slot_count;
  uint32_t used_count;
} slab_page_t;

#define SLAB_INITIAL_PAGES 64

typedef struct slab_class {
  slab_off_t *pages;         // Dynamic array of page offsets
  uint32_t page_count;
  uint32_t page_capacity;
  uint32_t hint_page;        // Hint: last page with free slots
  uint32_t slot_size;
  size_t used_slots;
} slab_class_t;

typedef struct slab_state {
  slab_class_t classes[SLAB_CLASS_COUNT];
  slab_alloc_page_fn alloc_fn;
  void *alloc_ctx;
  bool initialized;
} slab_state_t;

static inline size_t slab_page_total_size(uint32_t slot_size, uint32_t slot_count) {
  size_t bitmap_bytes = (slot_count + 7) / 8;
  size_t header = sizeof(slab_page_t);
  size_t bitmaps = bitmap_bytes * 2;  // alloc + mark bits
  size_t slots = (size_t)slot_size * slot_count;
  return (header + bitmaps + slots + 7) & ~(size_t)7;
}

static inline size_t slab_header_size(uint32_t slot_count) {
  size_t bitmap_bytes = (slot_count + 7) / 8;
  return sizeof(slab_page_t) + bitmap_bytes * 2;
}

static inline uint8_t *slab_alloc_bits(uint8_t *mem, slab_off_t page_off) {
  return mem + page_off + sizeof(slab_page_t);
}

static inline uint8_t *slab_mark_bits(uint8_t *mem, slab_off_t page_off, uint32_t slot_count) {
  size_t bitmap_bytes = (slot_count + 7) / 8;
  return mem + page_off + sizeof(slab_page_t) + bitmap_bytes;
}

static inline uint8_t *slab_slots(uint8_t *mem, slab_off_t page_off, uint32_t slot_count) {
  return mem + page_off + slab_header_size(slot_count);
}

static inline void slab_init(slab_state_t *state, slab_alloc_page_fn alloc_fn, void *ctx) {
  memset(state, 0, sizeof(*state));
  state->classes[SLAB_CLASS_OBJ].slot_size = SLAB_OBJ_SIZE;
  state->classes[SLAB_CLASS_PROP].slot_size = SLAB_PROP_SIZE;
  state->alloc_fn = alloc_fn;
  state->alloc_ctx = ctx;
  state->initialized = true;
  
  for (int c = 0; c < SLAB_CLASS_COUNT; c++) {
    state->classes[c].pages = (slab_off_t *)calloc(SLAB_INITIAL_PAGES, sizeof(slab_off_t));
    state->classes[c].page_capacity = SLAB_INITIAL_PAGES;
  }
}

static inline slab_off_t slab_add_page(uint8_t *mem, slab_state_t *state, int class_idx) {
  slab_class_t *sc = &state->classes[class_idx];
  
  if (!state->alloc_fn) return (slab_off_t)~0;
  
  // Grow pages array if needed
  if (sc->page_count >= sc->page_capacity) {
    uint32_t new_cap = sc->page_capacity * 2;
    slab_off_t *new_pages = (slab_off_t *)realloc(sc->pages, new_cap * sizeof(slab_off_t));
    if (!new_pages) return (slab_off_t)~0;
    sc->pages = new_pages;
    sc->page_capacity = new_cap;
  }
  
  uint32_t slot_size = sc->slot_size;
  uint32_t slot_count = SLAB_PAGE_SLOTS;
  size_t page_bytes = slab_page_total_size(slot_size, slot_count);
  
  slab_off_t page_off = state->alloc_fn(state->alloc_ctx, page_bytes);
  if (page_off == (slab_off_t)~0) return (slab_off_t)~0;
  
  // Initialize page
  memset(mem + page_off, 0, page_bytes);
  slab_page_t *page = (slab_page_t *)(mem + page_off);
  page->free_head = 0;
  page->slot_size = slot_size;
  page->slot_count = slot_count;
  page->used_count = 0;
  
  // Initialize free list
  uint8_t *slots = slab_slots(mem, page_off, slot_count);
  for (uint32_t i = 0; i < slot_count - 1; i++) {
    uint32_t *slot = (uint32_t *)(slots + i * slot_size);
    *slot = i + 1;
  }
  uint32_t *last = (uint32_t *)(slots + (slot_count - 1) * slot_size);
  *last = 0xFFFFFFFF;
  
  sc->pages[sc->page_count++] = page_off;
  return page_off;
}

static inline slab_off_t slab_alloc(uint8_t *mem, slab_state_t *state, int class_idx) {
  slab_class_t *sc = &state->classes[class_idx];
  
  // Fast path: check hint page first (usually the last page with free slots)
  if (sc->page_count > 0) {
    uint32_t hint = sc->hint_page;
    if (hint < sc->page_count) {
      slab_off_t page_off = sc->pages[hint];
      slab_page_t *page = (slab_page_t *)(mem + page_off);
      
      if (page->free_head != 0xFFFFFFFF) {
        uint32_t idx = page->free_head;
        uint8_t *slots = slab_slots(mem, page_off, page->slot_count);
        uint8_t *slot = slots + idx * page->slot_size;
        
        page->free_head = *(uint32_t *)slot;
        
        uint8_t *alloc_bits = slab_alloc_bits(mem, page_off);
        alloc_bits[idx / 8] |= (uint8_t)(1 << (idx & 7));
        
        page->used_count++;
        sc->used_slots++;
        
        return (slab_off_t)(slot - mem);
      }
    }
    
    // Hint failed - try newest page (common case: sequential allocation)
    uint32_t last = sc->page_count - 1;
    if (last != hint) {
      slab_off_t page_off = sc->pages[last];
      slab_page_t *page = (slab_page_t *)(mem + page_off);
      
      if (page->free_head != 0xFFFFFFFF) {
        sc->hint_page = last;
        uint32_t idx = page->free_head;
        uint8_t *slots = slab_slots(mem, page_off, page->slot_count);
        uint8_t *slot = slots + idx * page->slot_size;
        
        page->free_head = *(uint32_t *)slot;
        
        uint8_t *alloc_bits = slab_alloc_bits(mem, page_off);
        alloc_bits[idx / 8] |= (uint8_t)(1 << (idx & 7));
        
        page->used_count++;
        sc->used_slots++;
        
        return (slab_off_t)(slot - mem);
      }
    }
  }
  
  // Need new page
  slab_off_t page_off = slab_add_page(mem, state, class_idx);
  if (page_off == (slab_off_t)~0) return 0;
  
  sc->hint_page = sc->page_count - 1;
  
  slab_page_t *page = (slab_page_t *)(mem + page_off);
  uint32_t idx = page->free_head;
  uint8_t *slots = slab_slots(mem, page_off, page->slot_count);
  uint8_t *slot = slots + idx * page->slot_size;
  
  page->free_head = *(uint32_t *)slot;
  
  uint8_t *alloc_bits = slab_alloc_bits(mem, page_off);
  alloc_bits[idx / 8] |= (uint8_t)(1 << (idx & 7));
  
  page->used_count++;
  sc->used_slots++;
  
  return (slab_off_t)(slot - mem);
}

static inline bool slab_is_slab_offset(slab_state_t *state, uint8_t *mem, slab_off_t off) {
  for (int c = 0; c < SLAB_CLASS_COUNT; c++) {
    slab_class_t *sc = &state->classes[c];
    for (uint32_t p = 0; p < sc->page_count; p++) {
      slab_off_t page_off = sc->pages[p];
      slab_page_t *page = (slab_page_t *)(mem + page_off);
      uint8_t *slots = slab_slots(mem, page_off, page->slot_count);
      slab_off_t slots_start = (slab_off_t)(slots - mem);
      slab_off_t slots_end = slots_start + page->slot_count * page->slot_size;
      if (off >= slots_start && off < slots_end) return true;
    }
  }
  return false;
}

static inline bool slab_is_marked(uint8_t *mem, slab_state_t *state, int class_idx, slab_off_t slot_off) {
  slab_class_t *sc = &state->classes[class_idx];
  
  for (uint32_t p = 0; p < sc->page_count; p++) {
    slab_off_t page_off = sc->pages[p];
    slab_page_t *page = (slab_page_t *)(mem + page_off);
    uint8_t *slots = slab_slots(mem, page_off, page->slot_count);
    slab_off_t slots_start = (slab_off_t)(slots - mem);
    slab_off_t slots_end = slots_start + page->slot_count * page->slot_size;
    
    if (slot_off >= slots_start && slot_off < slots_end) {
      uint32_t idx = (uint32_t)((slot_off - slots_start) / page->slot_size);
      uint8_t *mark_bits = slab_mark_bits(mem, page_off, page->slot_count);
      return (mark_bits[idx / 8] & (1 << (idx % 8))) != 0;
    }
  }
  return false;
}

static inline void slab_mark(uint8_t *mem, slab_state_t *state, int class_idx, slab_off_t slot_off) {
  slab_class_t *sc = &state->classes[class_idx];
  
  for (uint32_t p = 0; p < sc->page_count; p++) {
    slab_off_t page_off = sc->pages[p];
    slab_page_t *page = (slab_page_t *)(mem + page_off);
    uint8_t *slots = slab_slots(mem, page_off, page->slot_count);
    slab_off_t slots_start = (slab_off_t)(slots - mem);
    slab_off_t slots_end = slots_start + page->slot_count * page->slot_size;
    
    if (slot_off >= slots_start && slot_off < slots_end) {
      uint32_t idx = (uint32_t)((slot_off - slots_start) / page->slot_size);
      uint8_t *mark_bits = slab_mark_bits(mem, page_off, page->slot_count);
      mark_bits[idx / 8] |= (1 << (idx % 8));
      return;
    }
  }
}

static inline size_t slab_sweep(uint8_t *mem, slab_state_t *state, int class_idx) {
  slab_class_t *sc = &state->classes[class_idx];
  size_t freed = 0;
  
  for (uint32_t p = 0; p < sc->page_count; p++) {
    slab_off_t page_off = sc->pages[p];
    slab_page_t *page = (slab_page_t *)(mem + page_off);
    uint8_t *alloc_bits = slab_alloc_bits(mem, page_off);
    uint8_t *mark_bits = slab_mark_bits(mem, page_off, page->slot_count);
    uint8_t *slots = slab_slots(mem, page_off, page->slot_count);
    size_t bitmap_bytes = (page->slot_count + 7) / 8;
    
    for (uint32_t i = 0; i < page->slot_count; i++) {
      bool allocated = alloc_bits[i / 8] & (1 << (i % 8));
      bool marked = mark_bits[i / 8] & (1 << (i % 8));
      
      if (allocated && !marked) {
        alloc_bits[i / 8] &= ~(1 << (i % 8));
        uint32_t *slot = (uint32_t *)(slots + i * page->slot_size);
        *slot = page->free_head;
        page->free_head = i;
        page->used_count--;
        sc->used_slots--;
        freed++;
      }
    }
    
    memset(mark_bits, 0, bitmap_bytes);
  }
  
  return freed;
}

static inline size_t slab_sweep_all(uint8_t *mem, slab_state_t *state) {
  size_t total = 0;
  for (int c = 0; c < SLAB_CLASS_COUNT; c++) {
    total += slab_sweep(mem, state, c);
  }
  return total;
}

static inline size_t slab_used_bytes(slab_state_t *state) {
  size_t total = 0;
  for (int c = 0; c < SLAB_CLASS_COUNT; c++) {
    total += state->classes[c].used_slots * state->classes[c].slot_size;
  }
  return total;
}

static inline size_t slab_total_pages(slab_state_t *state) {
  size_t total = 0;
  for (int c = 0; c < SLAB_CLASS_COUNT; c++) {
    total += state->classes[c].page_count;
  }
  return total;
}

#endif
