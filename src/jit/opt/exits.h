#ifndef ANT_JIT_OPT_EXITS_H
#define ANT_JIT_OPT_EXITS_H

#include "arena.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint32_t slot;
  uint8_t kind;
  uint8_t repr;
  uint8_t reg;
  uint8_t unused;
  uint64_t bits;
} opt_exit_value_t;

typedef struct {
  uint32_t start;
  uint32_t len;
} opt_exit_chunk_t;

typedef struct {
  uint32_t first;
  uint32_t chunks;
  uint32_t count;
} opt_exit_list_t;

typedef struct {
  const opt_exit_value_t *values;
  const opt_exit_chunk_t *chunks;
  const uint32_t *refs;
} opt_exit_view_t;

typedef struct {
  opt_arena_t *arena;
  OPT_VEC(opt_exit_value_t) values;
  OPT_VEC(opt_exit_chunk_t) chunks;
  OPT_VEC(uint32_t) refs;
  OPT_VEC(opt_exit_list_t) lists;
  OPT_VEC(opt_exit_value_t) pending;
  OPT_VEC(uint32_t) pending_refs;
  OPT_VEC(uint32_t) chunk_next;
  OPT_VEC(uint32_t) list_next;
  uint32_t *chunk_heads;
  uint32_t *list_heads;
  uint32_t chunk_head_cap, list_head_cap;
} opt_exit_table_t;

void opt_exit_table_init(opt_exit_table_t *table, opt_arena_t *arena);
void opt_exit_table_clone(opt_exit_table_t *dst, const opt_exit_table_t *src, opt_arena_t *arena);
void opt_exit_begin(opt_exit_table_t *table);
void opt_exit_push(opt_exit_table_t *table, opt_exit_value_t value);
opt_exit_list_t opt_exit_finish(opt_exit_table_t *table);

static inline opt_exit_view_t opt_exit_table_view(const opt_exit_table_t *table) {
  return (opt_exit_view_t){.values = table->values.items, .chunks = table->chunks.items, .refs = table->refs.items};
}

typedef struct {
  opt_exit_view_t view;
  opt_exit_list_t list;
  uint32_t ref;
  uint32_t at;
  uint32_t end;
} opt_exit_cursor_t;

static inline opt_exit_cursor_t opt_exit_cursor(opt_exit_view_t view, opt_exit_list_t list) {
  return (opt_exit_cursor_t){.view = view, .list = list};
}

static inline const opt_exit_value_t *opt_exit_next(opt_exit_cursor_t *cursor) {
  while (cursor->at == cursor->end) {
    if (cursor->ref == cursor->list.chunks) return NULL;
    const opt_exit_chunk_t *chunk = &cursor->view.chunks[cursor->view.refs[cursor->list.first + cursor->ref++]];
    cursor->at = chunk->start;
    cursor->end = chunk->start + chunk->len;
  }
  return &cursor->view.values[cursor->at++];
}

#endif
