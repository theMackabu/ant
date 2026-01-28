#ifndef READLINE_H
#define READLINE_H

#include "gc.h"
#include "types.h"
#include <stdbool.h>

jsval_t readline_library(struct js *js);
jsval_t readline_promises_library(struct js *js);

bool has_active_readline_interfaces(void);
void readline_gc_update_roots(GC_OP_VAL_ARGS);

#endif
