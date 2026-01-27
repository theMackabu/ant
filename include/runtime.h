#ifndef RUNTIME_H
#define RUNTIME_H

#include "ant.h"
#include <argtable3.h>

#define ANT_RUNTIME_CRYPTO_INIT      (1u << 0)
#define ANT_RUNTIME_EXT_EVENT_LOOP   (1u << 1)
#define CODE_ARENA_BLOCK_SIZE        (64 * 1024)

struct ant_runtime {
  struct js *js;
  char **argv;
  jsval_t ant_obj;
  int argc;
  unsigned int flags;
  const char *ls_fp;
};

extern struct ant_runtime *const rt;
struct ant_runtime *ant_runtime_init(struct js *js, int argc, char **argv, struct arg_file *ls_p);

const char *code_arena_alloc(const char *code, size_t len);
void code_arena_reset(void);

#endif