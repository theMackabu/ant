#ifndef NAVIGATOR_H
#define NAVIGATOR_H

#include "gc.h"

void init_navigator_module(void);
void navigator_gc_update_roots(GC_OP_VAL_ARGS);

#endif
