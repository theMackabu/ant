#ifndef URI_H
#define URI_H

#include "types.h"

void init_uri_module(ant_t *js);

ant_value_t js_encodeURI(ant_params_t);
ant_value_t js_decodeURI(ant_params_t);

ant_value_t js_encodeURIComponent(ant_params_t);
ant_value_t js_decodeURIComponent(ant_params_t);

#endif
