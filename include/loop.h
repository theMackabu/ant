#pragma once

#include <stdbool.h>
#include <uv.h>
#include "types.h"

bool Ant_Loop_Init(ant_t *js);
void Ant_Loop_Drain(ant_t *js);
void Ant_Loop_Destroy(ant_t *js);

static inline ant_t *Ant_Loop_Isolate(const uv_loop_t *loop) {
  return loop ? (ant_t *)loop->data : NULL;
}
