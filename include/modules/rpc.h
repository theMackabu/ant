#pragma once

#include "types.h"
#include "gc/modules.h"

__attribute__((cold)) 
ant_value_t rpc_library(ant_t *js);
void cleanup_rpc_module(ant_t *js);
void gc_mark_rpc(ant_t *js, gc_mark_fn mark);
