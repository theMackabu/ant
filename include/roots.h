#ifndef ROOTS_H
#define ROOTS_H

#include "ant.h"

jshdl_t js_root(struct js *js, jsval_t val);
jsval_t js_deref(struct js *js, jshdl_t h);

void js_unroot(struct js *js, jshdl_t h);
void js_root_update(struct js *js, jshdl_t h, jsval_t val);

#endif