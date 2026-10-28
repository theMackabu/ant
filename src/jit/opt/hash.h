#ifndef ANT_JIT_OPT_HASH_H
#define ANT_JIT_OPT_HASH_H

#include "arena.h"

static constexpr uint64_t OPT_HASH_SEED = UINT64_C(0x517cc1b727220a95);
static constexpr uint32_t OPT_MAP_MAX_KEY_WORDS = 4;

static inline uint64_t opt_hash_add(uint64_t hash, uint64_t word) {
  return (((hash << 5) | (hash >> 59)) ^ word) * OPT_HASH_SEED;
}

static inline uint64_t opt_hash_words(const uint64_t *words, uint32_t count) {
  uint64_t hash = 0;
  for (uint32_t i = 0; i < count; i++) hash = opt_hash_add(hash, words[i]);
  return hash;
}

typedef struct {
  opt_arena_t *arena;
  uint64_t *keys;
  uint64_t *values;
  uint8_t *used;
  uint32_t key_words;
  uint32_t len;
  uint32_t cap;
} opt_map_t;

void opt_map_init(opt_map_t *map, opt_arena_t *arena, uint32_t key_words);
void opt_map_clone(opt_map_t *dst, const opt_map_t *src, opt_arena_t *arena);
void opt_map_clear(opt_map_t *map);

uint64_t *opt_map_get(const opt_map_t *map, const uint64_t *key);
uint64_t *opt_map_entry(opt_map_t *map, const uint64_t *key, bool *inserted);
bool opt_map_remove(opt_map_t *map, const uint64_t *key);

static inline void opt_map_set(opt_map_t *map, const uint64_t *key, uint64_t value) {
  *opt_map_entry(map, key, NULL) = value;
}

static inline uint64_t *opt_map_get1(const opt_map_t *map, uint64_t a) {
  return opt_map_get(map, &a);
}

static inline uint64_t *opt_map_get2(const opt_map_t *map, uint64_t a, uint64_t b) {
  return opt_map_get(map, (uint64_t[]){a, b});
}

static inline void opt_map_set1(opt_map_t *map, uint64_t a, uint64_t value) {
  opt_map_set(map, &a, value);
}

static inline void opt_map_set2(opt_map_t *map, uint64_t a, uint64_t b, uint64_t value) {
  opt_map_set(map, (uint64_t[]){a, b}, value);
}

static inline const uint64_t *opt_map_key_at(const opt_map_t *map, uint32_t slot) {
  return &map->keys[(size_t)slot * map->key_words];
}

#define opt_map_for_each(map, slot) \
  for (uint32_t slot = 0; slot < (map)->cap; slot++) if ((map)->used[slot])

#endif
