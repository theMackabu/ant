#ifndef URI_H
#define URI_H

#include "types.h"

void init_uri_module(void);

ant_value_t js_encodeURI(ant_t *js, ant_value_t *args, int nargs);
ant_value_t js_decodeURI(ant_t *js, ant_value_t *args, int nargs);

ant_value_t js_encodeURIComponent(ant_t *js, ant_value_t *args, int nargs);
ant_value_t js_decodeURIComponent(ant_t *js, ant_value_t *args, int nargs);

#endif
