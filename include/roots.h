#ifndef ROOTS_H
#define ROOTS_H

#include "types.h"

ant_handle_t js_root(ant_t *js, ant_value_t val);
ant_value_t js_deref(ant_t *js, ant_handle_t h);

void js_unroot(ant_t *js, ant_handle_t h);
void js_root_update(ant_t *js, ant_handle_t h, ant_value_t val);

#endif