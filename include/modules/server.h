#ifndef SERVER_H
#define SERVER_H

#include "ant.h"

// Ant.serve(port, handler)
jsval_t js_serve(struct js *js, jsval_t *args, int nargs);

#endif
