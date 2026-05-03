#ifndef RUNTIME_H
#define RUNTIME_H

#include "types.h"
struct arg_file;

#define ANT_RUNTIME_CRYPTO_INIT (1u << 0)
#define CODE_ARENA_BLOCK_SIZE   (64 * 1024)

struct ant_runtime {
  ant_t *js;
  char **argv;
  ant_value_t ant_obj;
  int argc;
  int pid;
  unsigned int flags;
  const char *ls_fp;
};

typedef struct {
  void *block;
  size_t used;
} code_arena_mark_t;

extern struct ant_runtime *const rt;
struct ant_runtime *ant_runtime_init(ant_t *js, int argc, char **argv, struct arg_file *ls_p);

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

void destroy_runtime(ant_t *js);
void *code_arena_bump(size_t size);

#endif
