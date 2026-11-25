#include "arena.h"

#if !defined(_WIN32) && !defined(ANT_WASM_EMBED)
#include <pthread.h>
#endif

static constexpr size_t ARENA_INITIAL_COMMIT = 2ULL * 1024 * 1024;
static constexpr size_t ARENA_MIN_GROW = 64ULL * 1024;
static constexpr unsigned ARENA_CAGE_KEEP_SHIFT = 5;

static _Atomic(uint32_t) *arena_owners = NULL;
static _Atomic(uint16_t) *arena_ordinals = NULL;
static _Atomic(uint8_t) *arena_finalizers = NULL;
static uintptr_t arena_table_base = 0;
static size_t arena_table_slots = 0;
static _Atomic(uint32_t) arena_next_id = 1;

#ifdef ANT_WASM_EMBED

static constexpr size_t ARENA_WASM_SLOTS = ((uint64_t)UINT32_MAX + 1) >> ANT_ARENA_CHUNK_SHIFT;
static _Atomic(uint32_t) wasm_arena_owners[ARENA_WASM_SLOTS];
static _Atomic(uint16_t) wasm_arena_ordinals[ARENA_WASM_SLOTS];
static _Atomic(uint8_t) wasm_arena_finalizers[ARENA_WASM_SLOTS];

static bool arena_table_ready(void) {
  if (!arena_table_slots) {
    arena_owners = wasm_arena_owners;
    arena_ordinals = wasm_arena_ordinals;
    arena_finalizers = wasm_arena_finalizers;
    arena_table_slots = ARENA_WASM_SLOTS;
  }
  return true;
}

#else

#ifdef _WIN32
static SRWLOCK arena_table_lock = SRWLOCK_INIT;
static void arena_table_lock_acquire(void) { AcquireSRWLockExclusive(&arena_table_lock); }
static void arena_table_lock_release(void) { ReleaseSRWLockExclusive(&arena_table_lock); }
#else
static pthread_mutex_t arena_table_lock = PTHREAD_MUTEX_INITIALIZER;
static void arena_table_lock_acquire(void) { pthread_mutex_lock(&arena_table_lock); }
static void arena_table_lock_release(void) { pthread_mutex_unlock(&arena_table_lock); }
#endif

static bool arena_table_ready(void) {
  arena_table_lock_acquire();
  if (!arena_table_slots && ant_cage_base_address) {
    size_t slots = (ant_cage_reserved_size + ANT_ARENA_CHUNK_SIZE - 1) >> ANT_ARENA_CHUNK_SHIFT;
    _Atomic(uint32_t) *owners = calloc(slots, sizeof(*owners));
    _Atomic(uint16_t) *ordinals = calloc(slots, sizeof(*ordinals));
    _Atomic(uint8_t) *finalizers = calloc(slots, sizeof(*finalizers));
    if (owners && ordinals && finalizers) {
      arena_owners = owners;
      arena_ordinals = ordinals;
      arena_finalizers = finalizers;
      arena_table_base = ant_cage_base_address;
      arena_table_slots = slots;
    } else {
      free(owners);
      free(ordinals);
      free(finalizers);
    }
  }
  bool ready = arena_table_slots != 0;
  arena_table_lock_release();
  return ready;
}

#endif

static uint32_t arena_take_id(void) {
  uint32_t id;
  do id = atomic_fetch_add_explicit(&arena_next_id, 1, memory_order_relaxed) & ~ANT_ARENA_PARTIAL_CHUNK;
  while (id == 0);
  return id;
}

static inline size_t arena_chunk_slot(const uint8_t *base) {
  return (size_t)(((uintptr_t)base - arena_table_base) >> ANT_ARENA_CHUNK_SHIFT);
}

void fixed_arena_note_finalizer(const void *ptr) {
  uintptr_t addr = (uintptr_t)ptr;
  if (!arena_finalizers || addr < arena_table_base) return;
  size_t slot = (addr - arena_table_base) >> ANT_ARENA_CHUNK_SHIFT;
  if (slot < arena_table_slots) atomic_store_explicit(&arena_finalizers[slot], 1, memory_order_relaxed);
}

static void arena_chunk_publish(ant_fixed_arena_t *a, uint32_t index, bool partial) {
  size_t slot = arena_chunk_slot(a->chunks[index].base);
  atomic_store_explicit(&arena_ordinals[slot], (uint16_t)index, memory_order_relaxed);
  atomic_store_explicit(&arena_owners[slot], partial ? a->id | ANT_ARENA_PARTIAL_CHUNK : a->id, memory_order_relaxed);
}

static void arena_chunk_release(ant_fixed_arena_t *a, uint32_t index) {
  ant_arena_chunk_t *chunk = &a->chunks[index];
  atomic_store_explicit(&arena_owners[arena_chunk_slot(chunk->base)], 0, memory_order_relaxed);
  atomic_store_explicit(&arena_finalizers[arena_chunk_slot(chunk->base)], 0, memory_order_relaxed);
  a->committed -= chunk->committed;
  ant_cage_free(chunk->base, ANT_ARENA_CHUNK_SIZE);
  *chunk = (ant_arena_chunk_t){0};
}

static size_t arena_grow_step(const ant_fixed_arena_t *a) {
  size_t grow = a->committed / 4;
  if (grow < ARENA_MIN_GROW) grow = ARENA_MIN_GROW;
  if (grow > ARENA_GROW_INCREMENT) grow = ARENA_GROW_INCREMENT;
  return grow;
}

static size_t arena_commit_target(const ant_fixed_arena_t *a, size_t committed, size_t want, size_t needed) {
  size_t target = ant_arena_round_up_page(committed + want);
  if (target > ANT_ARENA_CHUNK_SIZE) target = ANT_ARENA_CHUNK_SIZE;
  if (target - committed > a->limit - a->committed) target = committed + (a->limit - a->committed);
  return target >= needed ? target : 0;
}

static void arena_point_at(ant_fixed_arena_t *a, uint32_t index, size_t used) {
  ant_arena_chunk_t *chunk = &a->chunks[index];
  size_t usable = chunk->committed < a->chunk_bytes ? chunk->committed : a->chunk_bytes;
  a->cursor = chunk->base + used;
  a->end = chunk->base + usable;
  a->last_end = chunk->base + ANT_ARENA_CHUNK_SIZE;
}

static bool arena_add_chunk(ant_fixed_arena_t *a, size_t want) {
  if (a->committed >= a->limit || a->chunk_count >= ANT_ARENA_MAX_CHUNKS) return false;

  if (a->chunk_count == a->chunk_cap) {
    uint32_t cap = a->chunk_cap ? a->chunk_cap * 2 : 4;
    ant_arena_chunk_t *chunks = realloc(a->chunks, (size_t)cap * sizeof(*chunks));
    if (!chunks) return false;
    a->chunks = chunks;
    a->chunk_cap = cap;
  }

  size_t target = arena_commit_target(a, 0, want, ant_arena_round_up_page(a->elem_size));
  if (!target) return false;

  uint8_t *base = ant_cage_reserve(ANT_ARENA_CHUNK_SIZE, ANT_ARENA_CHUNK_SIZE, ant_cage_reserved_size >> ARENA_CAGE_KEEP_SHIFT);
  if (!base) return false;

  if (!arena_table_ready() || ant_arena_commit(base, 0, target) != 0) {
    ant_cage_free(base, ANT_ARENA_CHUNK_SIZE);
    return false;
  }

  a->owners = arena_owners;
  a->ordinals = arena_ordinals;
  a->finalizers = arena_finalizers;
  a->table_base = arena_table_base;
  a->table_slots = arena_table_slots;

  uint32_t index = a->chunk_count++;
  a->chunks[index] = (ant_arena_chunk_t){ .base = base, .used = 0, .committed = target };
  a->committed += target;

  if (index) arena_chunk_publish(a, index - 1, false);
  arena_chunk_publish(a, index, true);
  arena_point_at(a, index, 0);

  return true;
}

bool fixed_arena_init(ant_fixed_arena_t *a, size_t elem_size, size_t epoch_offset, size_t max_size) {
  *a = (ant_fixed_arena_t){
    .elem_size = elem_size,
    .epoch_offset = epoch_offset,
    .chunk_bytes = ANT_ARENA_CHUNK_SIZE / elem_size * elem_size,
    .limit = ant_arena_round_up_page(max_size),
  };

  if (!a->chunk_bytes) return false;
  a->id = arena_take_id();
  return arena_add_chunk(a, ARENA_INITIAL_COMMIT);
}

void fixed_arena_destroy(ant_fixed_arena_t *a) {
  while (a->chunk_count) arena_chunk_release(a, --a->chunk_count);
  free(a->chunks);
  *a = (ant_fixed_arena_t){ .elem_size = a->elem_size, .epoch_offset = a->epoch_offset, .chunk_bytes = a->chunk_bytes };
}

__attribute__((noinline)) bool fixed_arena_refill(ant_fixed_arena_t *a) {
  if (a->chunk_count) {
    uint32_t index = a->chunk_count - 1;
    ant_arena_chunk_t *chunk = &a->chunks[index];
    size_t used = (size_t)(a->cursor - chunk->base);

    if (used + a->elem_size <= a->chunk_bytes) {
      size_t target = arena_commit_target(a, chunk->committed, arena_grow_step(a), used + a->elem_size);
      if (!target || ant_arena_commit(chunk->base, chunk->committed, target) != 0) return false;
      a->committed += target - chunk->committed;
      chunk->committed = target;
      arena_point_at(a, index, used);
      return true;
    }

    chunk->used = used;
  }

  return arena_add_chunk(a, arena_grow_step(a));
}

void fixed_arena_trim(ant_fixed_arena_t *a, uint32_t index, size_t used) {
  if (index >= a->chunk_count) return;
  while (a->chunk_count > index + 1) arena_chunk_release(a, --a->chunk_count);

  ant_arena_chunk_t *chunk = &a->chunks[index];
  size_t keep = ant_arena_round_up_page(used);

  if (keep < chunk->committed && ant_arena_decommit(chunk->base, chunk->committed, used) == 0) {
    a->committed -= chunk->committed - keep;
    chunk->committed = keep;
  }

  chunk->used = used;
  arena_chunk_publish(a, index, true);
  arena_point_at(a, index, used);
}
