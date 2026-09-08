#ifndef SILVER_EVAL_ENV_H
#define SILVER_EVAL_ENV_H

#include "types.h"

ant_value_t sv_eval_read_import(
  ant_t *js, ant_value_t value, 
  const char *name, uint32_t len, bool is_default
);

void sv_eval_env_gc_mark(ant_t *js, ant_object_t *obj);
void sv_eval_env_gc_free(ant_object_t *obj);

#endif
