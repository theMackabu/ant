#ifndef ANT_ZLIB_MODULE_H
#define ANT_ZLIB_MODULE_H

#include "types.h"

ant_value_t zlib_library(ant_t *js);
void gc_mark_zlib(ant_t *js, void (*mark)(ant_t *, ant_value_t));

#endif
