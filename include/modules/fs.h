#ifndef ANT_FS_MODULE_H
#define ANT_FS_MODULE_H

#include "gc.h"
#include "types.h"

jsval_t fs_library(struct js *js);
int has_pending_fs_ops(void);

void init_fs_module(void);
void fs_poll_events(void);
void fs_gc_update_roots(GC_OP_VAL_ARGS);

#endif
