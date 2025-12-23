#ifndef RUNTIME_H
#define RUNTIME_H

#include "ant.h"
#include <argtable3.h>

#define ANT_RUNTIME_CRYPTO_INIT      (1u << 0)
#define ANT_RUNTIME_EXT_EVENT_LOOP   (1u << 1)

struct ant_runtime {
  struct js *js;           // offset 0
  char **argv;             // offset 8
  jsval_t ant_obj;         // offset 16
  int argc;                // offset 24
  unsigned int flags;      // offset 28
  const char *ls_fp;       // offset 32
};

extern struct ant_runtime *const rt;
struct ant_runtime *ant_runtime_init(struct js *js, int argc, char **argv, struct arg_file *ls_p);

#endif