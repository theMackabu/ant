#ifndef RUNTIME_H
#define RUNTIME_H

#include "types.h"
struct arg_file;

#define CODE_ARENA_BLOCK_SIZE (64 * 1024)
#define CODE_ARENA_ALIGNMENT  8u

typedef struct {
  void *block;
  size_t used;
} code_arena_mark_t;

void ant_runtime_init(ant_t *js, int argc, char **argv, struct arg_file *ls_p);

size_t code_arena_get_memory(void);
const char *code_arena_alloc(const char *code, size_t len);

code_arena_mark_t code_arena_mark(void);
void code_arena_rewind(code_arena_mark_t mark);

void code_arena_reset(void);
size_t parse_arena_get_memory(void);

code_arena_mark_t parse_arena_mark(void);
void parse_arena_rewind(code_arena_mark_t mark);

void parse_arena_reset(void);
void *parse_arena_bump(size_t size);

void *code_arena_bump(size_t size);
void ant_runtime_set_argv(ant_t *js, int argc, char **argv);

#endif
