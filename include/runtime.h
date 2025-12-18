#ifndef RUNTIME_H
#define RUNTIME_H

#include "ant.h"

#define ANT_RUNTIME_CRYPTO_INIT      (1u << 0)
#define ANT_RUNTIME_EXT_EVENT_LOOP   (1u << 1)

struct ant_runtime {
  struct js *js;
  char **argv;
  jsval_t ant_obj;
  int argc;
  unsigned int flags;
};

extern struct ant_runtime *const rt;
struct ant_runtime *ant_runtime_init(struct js *js, int argc, char **argv);

#endif