#pragma once

#include "types.h"
#include "gc/modules.h"

ant_value_t sandbox_library(ant_t *js);
void gc_mark_sandbox(ant_t *js, gc_mark_fn mark);
