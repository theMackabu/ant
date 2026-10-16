#ifndef ANT_VM_MODULE_H
#define ANT_VM_MODULE_H

#include "types.h"
#include "silver/engine.h"

ant_value_t vm_library(ant_t *js);
ant_value_t js_vm_dynamic_import(ant_t *js, sv_func_debug_t *debug, ant_value_t specifier, ant_value_t options);

#endif
