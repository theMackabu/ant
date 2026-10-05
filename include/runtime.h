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

typedef struct {
  struct code_block *head;
  struct code_block *current;
} ant_code_arena_t;

void ant_runtime_init(ant_t *js, int argc, char **argv, struct arg_file *ls_p);
void ant_runtime_set_argv(ant_t *js, int argc, char **argv);

void *code_arena_bump(ant_t *js, size_t size);
const char *code_arena_alloc(ant_t *js, const char *code, size_t len);
size_t code_arena_get_memory(ant_t *js);

void *parse_arena_bump(ant_t *js, size_t size);
size_t parse_arena_get_memory(ant_t *js);
code_arena_mark_t parse_arena_mark(ant_t *js);

void parse_arena_rewind(ant_t *js, code_arena_mark_t mark);
void code_arenas_destroy(ant_t *js);

#endif
