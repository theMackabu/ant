#ifndef SILVER_EVAL_ENV_H
#define SILVER_EVAL_ENV_H

#include "types.h"

void sv_eval_env_gc_mark(ant_t *js, ant_object_t *obj);
void sv_eval_env_gc_free(ant_object_t *obj);

#endif
