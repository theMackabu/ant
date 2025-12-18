#ifndef RUNTIME_H
#define RUNTIME_H

#include "ant.h"

struct ant_runtime {
  struct js *js;
  char **argv;
  jsval_t ant_obj;
  int argc;
  int crypto_initialized;
  int external_event_loop_active;
};

extern struct ant_runtime *const rt;
struct ant_runtime *ant_runtime_init(struct js *js, int argc, char **argv);

#endif