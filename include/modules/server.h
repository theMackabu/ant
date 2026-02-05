#ifndef SERVER_H
#define SERVER_H

#include "gc.h"

void init_server_module(void);
void server_gc_update_roots(GC_OP_VAL_ARGS);

#endif
