#ifndef ANT_DOMEXCEPTION_MODULE_H
#define ANT_DOMEXCEPTION_MODULE_H

#include "types.h"
#include "gc/modules.h"

void init_domexception_module(void);
void gc_mark_domexception(ant_t *js, gc_mark_fn mark);
ant_value_t make_dom_exception(ant_t *js, const char *message, const char *name);

#endif
