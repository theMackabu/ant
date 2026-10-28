#include "exits.h"
#include "hash.h"

static constexpr uint32_t OPT_EXIT_CHUNK_VALUES = 16;
static constexpr uint32_t OPT_EXIT_SLOT_GROUP_SHIFT = 4;
static constexpr uint32_t OPT_EXIT_NONE = UINT32_MAX;

static inline bool opt_exit_value_equal(const opt_exit_value_t *a, const opt_exit_value_t *b) {
  return a->slot == b->slot && a->kind == b->kind && a->repr == b->repr && a->reg == b->reg && a->bits == b->bits;
}

static uint64_t opt_exit_values_hash(const opt_exit_value_t *values, uint32_t len) {
  uint64_t hash = len;
  for (uint32_t i = 0; i < len; i++) {
    const opt_exit_value_t *value = &values[i];
    hash = opt_hash_add(hash, (uint64_t)value->slot << 32 | (uint64_t)value->kind << 16 | (uint64_t)value->repr << 8 | value->reg);
    hash = opt_hash_add(hash, value->bits);
  }
  return hash;
}

static uint64_t opt_exit_refs_hash(const uint32_t *refs, uint32_t len) {
  uint64_t hash = len;
  for (uint32_t i = 0; i < len; i++) hash = opt_hash_add(hash, refs[i]);
  return hash;
}

void opt_exit_table_init(opt_exit_table_t *table, opt_arena_t *arena) {
  *table = (opt_exit_table_t){.arena = arena};
}

void opt_exit_table_clone(opt_exit_table_t *dst, const opt_exit_table_t *src, opt_arena_t *arena) {
  *dst = (opt_exit_table_t){.arena = arena};
  opt_vec_clone(arena, &dst->values, &src->values);
  opt_vec_clone(arena, &dst->chunks, &src->chunks);
  opt_vec_clone(arena, &dst->refs, &src->refs);
  opt_vec_clone(arena, &dst->lists, &src->lists);
  opt_vec_clone(arena, &dst->chunk_next, &src->chunk_next);
  opt_vec_clone(arena, &dst->list_next, &src->list_next);
  dst->chunk_head_cap = src->chunk_head_cap;
  dst->list_head_cap = src->list_head_cap;
  if (src->chunk_heads) dst->chunk_heads = opt_arena_dup(arena, src->chunk_heads, src->chunk_head_cap * sizeof *src->chunk_heads);
  if (src->list_heads) dst->list_heads = opt_arena_dup(arena, src->list_heads, src->list_head_cap * sizeof *src->list_heads);
}

void opt_exit_begin(opt_exit_table_t *table) {
  table->pending.len = 0;
  table->pending_refs.len = 0;
}

void opt_exit_push(opt_exit_table_t *table, opt_exit_value_t value) {
  value.unused = 0;
  opt_vec_push(table->arena, &table->pending, value);
}

static uint32_t *opt_exit_new_heads(opt_arena_t *arena, uint32_t cap) {
  uint32_t *heads = opt_arena_alloc(arena, cap * sizeof *heads);
  memset(heads, 0xff, cap * sizeof *heads);
  return heads;
}

static void opt_exit_rehash_chunks(opt_exit_table_t *table) {
  uint32_t cap = table->chunk_head_cap ? table->chunk_head_cap * 2 : 256;
  table->chunk_heads = opt_exit_new_heads(table->arena, cap);
  table->chunk_head_cap = cap;
  for (uint32_t id = 0; id < table->chunks.len; id++) {
    const opt_exit_chunk_t *chunk = &table->chunks.items[id];
    uint32_t bucket = (uint32_t)opt_exit_values_hash(&table->values.items[chunk->start], chunk->len) & (cap - 1);
    table->chunk_next.items[id] = table->chunk_heads[bucket];
    table->chunk_heads[bucket] = id;
  }
}

static void opt_exit_rehash_lists(opt_exit_table_t *table) {
  uint32_t cap = table->list_head_cap ? table->list_head_cap * 2 : 64;
  table->list_heads = opt_exit_new_heads(table->arena, cap);
  table->list_head_cap = cap;
  for (uint32_t id = 0; id < table->lists.len; id++) {
    const opt_exit_list_t *list = &table->lists.items[id];
    uint32_t bucket = (uint32_t)opt_exit_refs_hash(&table->refs.items[list->first], list->chunks) & (cap - 1);
    table->list_next.items[id] = table->list_heads[bucket];
    table->list_heads[bucket] = id;
  }
}

static uint32_t opt_exit_intern_chunk(opt_exit_table_t *table, const opt_exit_value_t *values, uint32_t len) {
  if (table->chunks.len >= table->chunk_head_cap) opt_exit_rehash_chunks(table);
  uint32_t bucket = (uint32_t)opt_exit_values_hash(values, len) & (table->chunk_head_cap - 1);
  for (uint32_t id = table->chunk_heads[bucket]; id != OPT_EXIT_NONE; id = table->chunk_next.items[id]) {
    const opt_exit_chunk_t *chunk = &table->chunks.items[id];
    if (chunk->len != len) continue;
    bool same = true;
    for (uint32_t i = 0; i < len && same; i++) same = opt_exit_value_equal(&table->values.items[chunk->start + i], &values[i]);
    if (same) return id;
  }
  uint32_t id = table->chunks.len;
  opt_exit_chunk_t chunk = {.start = table->values.len, .len = len};
  opt_vec_reserve(table->arena, &table->values, table->values.len + len);
  memcpy(&table->values.items[table->values.len], values, len * sizeof *values);
  table->values.len += len;
  opt_vec_push(table->arena, &table->chunks, chunk);
  opt_vec_push(table->arena, &table->chunk_next, table->chunk_heads[bucket]);
  table->chunk_heads[bucket] = id;
  return id;
}

static opt_exit_list_t opt_exit_intern_list(opt_exit_table_t *table, const uint32_t *refs, uint32_t len, uint32_t count) {
  if (table->lists.len >= table->list_head_cap) opt_exit_rehash_lists(table);
  uint32_t bucket = (uint32_t)opt_exit_refs_hash(refs, len) & (table->list_head_cap - 1);
  for (uint32_t id = table->list_heads[bucket]; id != OPT_EXIT_NONE; id = table->list_next.items[id]) {
    const opt_exit_list_t *list = &table->lists.items[id];
    if (list->chunks == len && !memcmp(&table->refs.items[list->first], refs, len * sizeof *refs)) return *list;
  }
  opt_exit_list_t list = {.first = table->refs.len, .chunks = len, .count = count};
  opt_vec_reserve(table->arena, &table->refs, table->refs.len + len);
  memcpy(&table->refs.items[table->refs.len], refs, len * sizeof *refs);
  table->refs.len += len;
  opt_vec_push(table->arena, &table->lists, list);
  opt_vec_push(table->arena, &table->list_next, table->list_heads[bucket]);
  table->list_heads[bucket] = table->lists.len - 1;
  return list;
}

opt_exit_list_t opt_exit_finish(opt_exit_table_t *table) {
  uint32_t count = table->pending.len;
  if (!count) return (opt_exit_list_t){0};
  const opt_exit_value_t *values = table->pending.items;
  for (uint32_t start = 0; start < count;) {
    uint32_t group = values[start].slot >> OPT_EXIT_SLOT_GROUP_SHIFT, end = start + 1;
    while (end < count && end - start < OPT_EXIT_CHUNK_VALUES && values[end].slot >> OPT_EXIT_SLOT_GROUP_SHIFT == group) end++;
    uint32_t id = opt_exit_intern_chunk(table, &values[start], end - start);
    opt_vec_push(table->arena, &table->pending_refs, id);
    start = end;
  }
  return opt_exit_intern_list(table, table->pending_refs.items, table->pending_refs.len, count);
}
