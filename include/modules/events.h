#ifndef EVENTS_H
#define EVENTS_H

#include <stdbool.h>
#include "types.h"

ant_value_t events_library(ant_t *js);

void init_events_module(void);
void js_dispatch_global_event(ant_t *js, ant_value_t event_obj);

bool eventemitter_add_listener(
  ant_t *js,
  ant_value_t target, const char *event_type,
  ant_value_t listener, bool once
);

bool eventemitter_emit_args(
  ant_t *js,
  ant_value_t target, const char *event_type,
  ant_value_t *args, int nargs
);

#endif
