#include "bitset.h"

bool opt_bitset_union_with(opt_bitset_t *set, const opt_bitset_t *other) {
  assert(set->len == other->len);
  bool changed = false;
  for (uint32_t i = 0, count = opt_bitset_word_count(set->len); i < count; i++) {
    uint64_t word = set->words[i] | other->words[i];
    changed |= word != set->words[i];
    set->words[i] = word;
  }
  return changed;
}

void opt_bitset_subtract(opt_bitset_t *set, const opt_bitset_t *other) {
  assert(set->len == other->len);
  for (uint32_t i = 0, count = opt_bitset_word_count(set->len); i < count; i++)
    set->words[i] &= ~other->words[i];
}

bool opt_bitset_is_empty(const opt_bitset_t *set) {
  for (uint32_t i = 0, count = opt_bitset_word_count(set->len); i < count; i++)
    if (set->words[i]) return false;
  return true;
}

bool opt_bitset_equal(const opt_bitset_t *set, const opt_bitset_t *other) {
  return set->len == other->len
    && memcmp(set->words, other->words, opt_bitset_word_count(set->len) * sizeof(uint64_t)) == 0;
}

uint32_t opt_bitset_count(const opt_bitset_t *set) {
  uint32_t count = 0;
  for (uint32_t i = 0, words = opt_bitset_word_count(set->len); i < words; i++)
    count += (uint32_t)__builtin_popcountll(set->words[i]);
  return count;
}

uint32_t opt_bitset_next(const opt_bitset_t *set, uint32_t from) {
  if (from >= set->len) return set->len;
  uint32_t word_index = from / 64;
  uint64_t word = set->words[word_index] & (~UINT64_C(0) << (from % 64));
  uint32_t count = opt_bitset_word_count(set->len);
  for (;;) {
    if (word) {
      uint32_t index = word_index * 64 + (uint32_t)__builtin_ctzll(word);
      return index < set->len ? index : set->len;
    }
    if (++word_index >= count) return set->len;
    word = set->words[word_index];
  }
}
