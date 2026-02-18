#ifndef ANT_LMDB_MODULE_H
#define ANT_LMDB_MODULE_H

#include "types.h"

jsval_t lmdb_library(struct js *js);
void cleanup_lmdb_module(void);

#endif
