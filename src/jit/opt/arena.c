#include "arena.h"

#include <stdio.h>
#include <stdlib.h>

struct opt_arena_chunk {
  opt_arena_chunk_t *next;
  size_t size;
  alignas(16) uint8_t data[];
};

static constexpr size_t OPT_ARENA_CHUNK_SIZE = 64 * 1024;
static constexpr size_t OPT_ARENA_ALIGN = 16;

void opt_out_of_memory(void) {
  fputs("ant: out of memory in the optimizing JIT\n", stderr);
  abort();
}

void opt_arena_init(opt_arena_t *arena) {
  *arena = (opt_arena_t){0};
}

void opt_arena_free(opt_arena_t *arena) {
  opt_arena_chunk_t *chunk = arena->chunk;
  while (chunk) {
    opt_arena_chunk_t *next = chunk->next;
    free(chunk);
    chunk = next;
  }
  *arena = (opt_arena_t){0};
}

static inline size_t opt_align(size_t size) {
  return (size + OPT_ARENA_ALIGN - 1) & ~(OPT_ARENA_ALIGN - 1);
}

void *opt_arena_alloc(opt_arena_t *arena, size_t size) {
  size = opt_align(size ? size : 1);
  if (arena->cursor && (size_t)(arena->limit - arena->cursor) >= size) {
    void *result = arena->cursor;
    arena->cursor += size;
    return result;
  }
  size_t chunk_size = size > OPT_ARENA_CHUNK_SIZE / 4 ? size : OPT_ARENA_CHUNK_SIZE;
  opt_arena_chunk_t *chunk = malloc(sizeof(opt_arena_chunk_t) + chunk_size);
  if (!chunk) opt_out_of_memory();
  chunk->size = chunk_size;
  if (chunk_size != OPT_ARENA_CHUNK_SIZE && arena->chunk) {
    chunk->next = arena->chunk->next;
    arena->chunk->next = chunk;
    return chunk->data;
  }
  chunk->next = arena->chunk;
  arena->chunk = chunk;
  arena->cursor = chunk->data + size;
  arena->limit = chunk->data + chunk_size;
  return chunk->data;
}

void *opt_arena_zalloc(opt_arena_t *arena, size_t size) {
  void *result = opt_arena_alloc(arena, size);
  memset(result, 0, size);
  return result;
}

void *opt_arena_dup(opt_arena_t *arena, const void *data, size_t size) {
  void *result = opt_arena_alloc(arena, size);
  if (size) memcpy(result, data, size);
  return result;
}

void *opt_arena_grow(opt_arena_t *arena, void *items, uint32_t *cap, uint32_t need, size_t elem) {
  if (need <= *cap) return items;
  uint32_t next = *cap ? *cap : 4;
  while (next < need) {
    if (next > UINT32_MAX / 2) opt_out_of_memory();
    next *= 2;
  }
  size_t old_size = opt_align((size_t)*cap * elem);
  size_t new_size = opt_align((size_t)next * elem);
  uint8_t *end = (uint8_t *)items + old_size;
  if (items && end == arena->cursor && (size_t)(arena->limit - (uint8_t *)items) >= new_size) {
    arena->cursor = (uint8_t *)items + new_size;
    *cap = next;
    return items;
  }
  void *grown = opt_arena_alloc(arena, new_size);
  if (items && *cap) memcpy(grown, items, (size_t)*cap * elem);
  *cap = next;
  return grown;
}

static void opt_text_reserve(opt_text_t *text, size_t extra) {
  size_t need = text->len + extra + 1;
  if (need <= text->cap) return;
  size_t cap = text->cap ? text->cap : 256;
  while (cap < need) cap *= 2;
  char *data = realloc(text->data, cap);
  if (!data) opt_out_of_memory();
  text->data = data;
  text->cap = cap;
}

void opt_text_append(opt_text_t *text, const char *string) {
  size_t len = strlen(string);
  opt_text_reserve(text, len);
  memcpy(text->data + text->len, string, len + 1);
  text->len += len;
}

void opt_text_appendf(opt_text_t *text, const char *format, ...) {
  va_list args;
  va_start(args, format);
  va_list copy;
  va_copy(copy, args);
  int len = vsnprintf(NULL, 0, format, copy);
  va_end(copy);
  if (len < 0) {
    va_end(args);
    return;
  }
  opt_text_reserve(text, (size_t)len);
  vsnprintf(text->data + text->len, (size_t)len + 1, format, args);
  va_end(args);
  text->len += (size_t)len;
}

void opt_text_free(opt_text_t *text) {
  free(text->data);
  *text = (opt_text_t){0};
}
