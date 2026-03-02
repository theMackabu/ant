#ifndef ROOTS_H
#define ROOTS_H

#include "types.h"

jshdl_t js_root(ant_t *js, jsval_t val);
jsval_t js_deref(ant_t *js, jshdl_t h);

void js_unroot(ant_t *js, jshdl_t h);
void js_root_update(ant_t *js, jshdl_t h, jsval_t val);

#endif