#ifndef ANT_CHILD_PROCESS_MODULE_H
#define ANT_CHILD_PROCESS_MODULE_H

#include "ant.h"

jsval_t child_process_library(struct js *js);
void child_process_poll_events(void);
int has_pending_child_processes(void);
void child_process_gc_update_roots(GC_FWD_ARGS);

#endif
