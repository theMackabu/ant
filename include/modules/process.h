#ifndef PROCESS_H
#define PROCESS_H

#include "gc.h"

void process_gc_update_roots(GC_OP_VAL_ARGS);
void process_enable_keypress_events(void);

void init_process_module(void);
bool has_active_stdin(void);

#endif
