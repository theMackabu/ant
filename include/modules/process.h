#ifndef PROCESS_H
#define PROCESS_H

#include "ant.h"

void init_process_module(void);
void process_gc_update_roots(GC_FWD_ARGS);

bool has_active_stdin(void);

#endif
