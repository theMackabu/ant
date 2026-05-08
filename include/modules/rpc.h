#ifndef RPC_H
#define RPC_H

#include "types.h"
#include "gc/modules.h"

ant_value_t rpc_library(ant_t *js);

void cleanup_rpc_module(void);
void gc_mark_rpc(ant_t *js, gc_mark_fn mark);

#endif
