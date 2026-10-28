#ifndef ANT_JIT_OPT_ARENA_H
#define ANT_JIT_OPT_ARENA_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct opt_arena_chunk opt_arena_chunk_t;

typedef struct {
  opt_arena_chunk_t *chunk;
  uint8_t *cursor;
  uint8_t *limit;
} opt_arena_t;

void opt_arena_init(opt_arena_t *arena);
void opt_arena_free(opt_arena_t *arena);

void *opt_arena_alloc(opt_arena_t *arena, size_t size);
void *opt_arena_zalloc(opt_arena_t *arena, size_t size);
void *opt_arena_dup(opt_arena_t *arena, const void *data, size_t size);
void *opt_arena_grow(opt_arena_t *arena, void *items, uint32_t *cap, uint32_t need, size_t elem);

[[noreturn]] void opt_out_of_memory(void);

#define opt_arena_new(arena, T, count) ((T *)opt_arena_zalloc((arena), sizeof(T) * (size_t)(count)))

#define OPT_VEC(T) struct { T *items; uint32_t len, cap; }

#define opt_vec_reserve(arena, vec, need) \
  ((vec)->items = opt_arena_grow((arena), (vec)->items, &(vec)->cap, (need), sizeof *(vec)->items))

#define opt_vec_push(arena, vec, value) \
  (opt_vec_reserve((arena), (vec), (vec)->len + 1), (vec)->items[(vec)->len++] = (value))

#define opt_vec_insert(arena, vec, index, value) do { \
  uint32_t opt_at_ = (index); \
  opt_vec_reserve((arena), (vec), (vec)->len + 1); \
  memmove(&(vec)->items[opt_at_ + 1], &(vec)->items[opt_at_], ((vec)->len - opt_at_) * sizeof *(vec)->items); \
  (vec)->items[opt_at_] = (value); \
  (vec)->len++; \
} while (0)

#define opt_vec_remove(vec, index) do { \
  uint32_t opt_at_ = (index); \
  memmove(&(vec)->items[opt_at_], &(vec)->items[opt_at_ + 1], ((vec)->len - opt_at_ - 1) * sizeof *(vec)->items); \
  (vec)->len--; \
} while (0)

#define opt_vec_copy(arena, dst, src) do { \
  (dst)->len = 0; \
  opt_vec_reserve((arena), (dst), (src)->len); \
  if ((src)->len) memcpy((dst)->items, (src)->items, (src)->len * sizeof *(src)->items); \
  (dst)->len = (src)->len; \
} while (0)

#define opt_vec_clone(arena, dst, src) do { \
  (dst)->items = NULL; (dst)->cap = 0; \
  opt_vec_copy((arena), (dst), (src)); \
} while (0)

typedef struct {
  char *data;
  size_t len, cap;
} opt_text_t;

void opt_text_append(opt_text_t *text, const char *string);
[[gnu::format(printf, 2, 3)]] void opt_text_appendf(opt_text_t *text, const char *format, ...);
void opt_text_free(opt_text_t *text);

#endif
