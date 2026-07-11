#ifndef ANT_MODULES_SANDBOX_H
#define ANT_MODULES_SANDBOX_H

#include "types.h"
#include "gc/modules.h"

ant_value_t sandbox_library(ant_t *js);
void gc_mark_sandbox(ant_t *js, gc_mark_fn mark);

#endif
