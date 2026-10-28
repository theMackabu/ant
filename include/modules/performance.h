#ifndef PERFORMANCE_H
#define PERFORMANCE_H

#include "types.h"

void init_performance_module(ant_t *js);
void Ant_Performance_MarkLoopStart(ant_t *js);
void cleanup_performance_module(ant_t *js);

ant_value_t perf_hooks_library(ant_t *js);
ant_value_t ant_performance_elu_zero(ant_t *js);
ant_value_t ant_performance_elu_from(ant_t *js, double idle, double active, ant_args_t);

bool ant_performance_elu_pair(ant_native_params_t, ant_value_t *out);

#endif
