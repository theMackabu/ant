#pragma once

#include "types.h"

__attribute__((cold)) 
ant_value_t child_process_library(ant_t *js);
void cleanup_child_process_module(ant_t *js);
int has_pending_child_processes(ant_t *js);
