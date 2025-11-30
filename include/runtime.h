#ifndef RUNTIME_H
#define RUNTIME_H

#include "ant.h"

struct ant_runtime {
  struct js *js;
  jsval_t ant_obj;
  int crypto_initialized;
};

struct ant_runtime *ant_runtime(void);
struct ant_runtime *ant_runtime_init(struct js *js);

#endif