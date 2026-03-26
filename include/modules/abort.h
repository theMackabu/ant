#ifndef ANT_ABORT_MODULE_H
#define ANT_ABORT_MODULE_H

#include "types.h"
#include "gc/modules.h"

void init_abort_module(void);

void gc_mark_abort(ant_t *js, gc_mark_fn mark);
void signal_do_abort(ant_t *js, ant_value_t signal, ant_value_t reason);
void abort_signal_add_listener(ant_t *js, ant_value_t signal, ant_value_t callback);

bool abort_signal_is_signal(ant_value_t signal);
bool abort_signal_is_aborted(ant_value_t signal);

ant_value_t abort_signal_get_reason(ant_value_t signal);
ant_value_t abort_signal_create_dependent(ant_t *js, ant_value_t source);

#endif
