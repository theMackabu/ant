#ifndef ANT_CHILD_PROCESS_MODULE_H
#define ANT_CHILD_PROCESS_MODULE_H

#include "types.h"

int has_pending_child_processes(void);
ant_value_t child_process_library(ant_t *js);

#endif
