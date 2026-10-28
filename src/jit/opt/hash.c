#include "hash.h"
#include <assert.h>

void opt_map_init(opt_map_t *map, opt_arena_t *arena, uint32_t key_words) {
  assert(key_words >= 1 && key_words <= OPT_MAP_MAX_KEY_WORDS);
  *map = (opt_map_t){.arena = arena, .key_words = key_words};
}

void opt_map_clear(opt_map_t *map) {
  if (map->cap) memset(map->used, 0, map->cap);
  map->len = 0;
}

static inline uint32_t opt_map_slot(const opt_map_t *map, const uint64_t *key) {
  return (uint32_t)(opt_hash_words(key, map->key_words) >> 32) & (map->cap - 1);
}

static inline bool opt_map_key_equal(const opt_map_t *map, uint32_t slot, const uint64_t *key) {
  return memcmp(opt_map_key_at(map, slot), key, map->key_words * sizeof(uint64_t)) == 0;
}

static void opt_map_resize(opt_map_t *map, uint32_t cap) {
  opt_map_t old = *map;
  map->cap = cap;
  map->len = 0;
  map->keys = opt_arena_alloc(map->arena, (size_t)cap * map->key_words * sizeof(uint64_t));
  map->values = opt_arena_alloc(map->arena, (size_t)cap * sizeof(uint64_t));
  map->used = opt_arena_zalloc(map->arena, cap);
  for (uint32_t slot = 0; slot < old.cap; slot++) {
    if (!old.used[slot]) continue;
    *opt_map_entry(map, opt_map_key_at(&old, slot), NULL) = old.values[slot];
  }
}

void opt_map_clone(opt_map_t *dst, const opt_map_t *src, opt_arena_t *arena) {
  *dst = *src;
  dst->arena = arena;
  if (!src->cap) return;
  dst->keys = opt_arena_dup(arena, src->keys, (size_t)src->cap * src->key_words * sizeof(uint64_t));
  dst->values = opt_arena_dup(arena, src->values, (size_t)src->cap * sizeof(uint64_t));
  dst->used = opt_arena_dup(arena, src->used, src->cap);
}

uint64_t *opt_map_get(const opt_map_t *map, const uint64_t *key) {
  if (!map->len) return NULL;
  for (uint32_t slot = opt_map_slot(map, key);; slot = (slot + 1) & (map->cap - 1)) {
    if (!map->used[slot]) return NULL;
    if (opt_map_key_equal(map, slot, key)) return &map->values[slot];
  }
}

uint64_t *opt_map_entry(opt_map_t *map, const uint64_t *key, bool *inserted) {
  if ((map->len + 1) * 4 > map->cap * 3) opt_map_resize(map, map->cap ? map->cap * 2 : 16);
  uint32_t slot = opt_map_slot(map, key);
  for (; map->used[slot]; slot = (slot + 1) & (map->cap - 1)) {
    if (opt_map_key_equal(map, slot, key)) {
      if (inserted) *inserted = false;
      return &map->values[slot];
    }
  }
  map->used[slot] = 1;
  memcpy(&map->keys[(size_t)slot * map->key_words], key, map->key_words * sizeof(uint64_t));
  map->values[slot] = 0;
  map->len++;
  if (inserted) *inserted = true;
  return &map->values[slot];
}

bool opt_map_remove(opt_map_t *map, const uint64_t *key) {
  if (!map->len) return false;
  uint32_t mask = map->cap - 1;
  uint32_t slot = opt_map_slot(map, key);
  for (;; slot = (slot + 1) & mask) {
    if (!map->used[slot]) return false;
    if (opt_map_key_equal(map, slot, key)) break;
  }
  uint32_t hole = slot;
  for (uint32_t next = (hole + 1) & mask; map->used[next]; next = (next + 1) & mask) {
    uint32_t home = opt_map_slot(map, opt_map_key_at(map, next));
    bool movable = hole <= next ? (home <= hole || home > next) : (home <= hole && home > next);
    if (!movable) continue;
    memcpy(&map->keys[(size_t)hole * map->key_words], opt_map_key_at(map, next), map->key_words * sizeof(uint64_t));
    map->values[hole] = map->values[next];
    hole = next;
  }
  map->used[hole] = 0;
  map->len--;
  return true;
}
