#ifndef ANT_CHILD_PROCESS_MODULE_H
#define ANT_CHILD_PROCESS_MODULE_H

#include "gc.h"
#include "types.h"

jsval_t child_process_library(struct js *js);
int has_pending_child_processes(void);

void child_process_poll_events(void);
void child_process_gc_update_roots(GC_OP_VAL_ARGS);

#endif
