#ifndef READLINE_H
#define READLINE_H

#include "ant.h"
#include <stdbool.h>

jsval_t readline_library(struct js *js);
jsval_t readline_promises_library(struct js *js);

bool has_active_readline_interfaces(void);
void readline_gc_update_roots(GC_FWD_ARGS);

#endif
