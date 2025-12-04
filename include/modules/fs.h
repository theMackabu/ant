#ifndef ANT_FS_MODULE_H
#define ANT_FS_MODULE_H

#include "ant.h"

jsval_t fs_library(struct js *js);
void fs_poll_events(void);
int has_pending_fs_ops(void);

#endif
