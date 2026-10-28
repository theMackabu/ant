#ifndef ANT_JIT_OPT_BITSET_H
#define ANT_JIT_OPT_BITSET_H

#include "arena.h"
#include <assert.h>

typedef struct {
  uint64_t *words;
  uint32_t len;
} opt_bitset_t;

static inline uint32_t opt_bitset_word_count(uint32_t len) {
  return (len + 63) / 64;
}

static inline opt_bitset_t opt_bitset_new(opt_arena_t *arena, uint32_t len) {
  return (opt_bitset_t){
    .words = opt_arena_new(arena, uint64_t, opt_bitset_word_count(len)),
    .len = len,
  };
}

static inline opt_bitset_t opt_bitset_clone(opt_arena_t *arena, const opt_bitset_t *set) {
  opt_bitset_t copy = opt_bitset_new(arena, set->len);
  memcpy(copy.words, set->words, opt_bitset_word_count(set->len) * sizeof(uint64_t));
  return copy;
}

static inline void opt_bitset_copy(opt_bitset_t *dst, const opt_bitset_t *src) {
  assert(dst->len == src->len);
  memcpy(dst->words, src->words, opt_bitset_word_count(src->len) * sizeof(uint64_t));
}

static inline bool opt_bitset_contains(const opt_bitset_t *set, uint32_t index) {
  assert(index < set->len);
  return (set->words[index / 64] >> (index % 64)) & 1;
}

static inline bool opt_bitset_has(const opt_bitset_t *set, uint32_t index) {
  return index < set->len && opt_bitset_contains(set, index);
}

static inline void opt_bitset_insert(opt_bitset_t *set, uint32_t index) {
  assert(index < set->len);
  set->words[index / 64] |= UINT64_C(1) << (index % 64);
}

static inline void opt_bitset_remove(opt_bitset_t *set, uint32_t index) {
  assert(index < set->len);
  set->words[index / 64] &= ~(UINT64_C(1) << (index % 64));
}

static inline void opt_bitset_clear(opt_bitset_t *set) {
  memset(set->words, 0, opt_bitset_word_count(set->len) * sizeof(uint64_t));
}

bool opt_bitset_union_with(opt_bitset_t *set, const opt_bitset_t *other);
void opt_bitset_subtract(opt_bitset_t *set, const opt_bitset_t *other);
bool opt_bitset_is_empty(const opt_bitset_t *set);
bool opt_bitset_equal(const opt_bitset_t *set, const opt_bitset_t *other);
uint32_t opt_bitset_count(const opt_bitset_t *set);

uint32_t opt_bitset_next(const opt_bitset_t *set, uint32_t from);

#define opt_bitset_for_each(set, index) \
  for (uint32_t index = opt_bitset_next((set), 0); index < (set)->len; index = opt_bitset_next((set), index + 1))

#endif
