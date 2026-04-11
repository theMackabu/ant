#ifndef EVENTS_H
#define EVENTS_H

#include <stdbool.h>
#include "types.h"

#define EVENTS_DEFAULT_MAX_LISTENERS 10

ant_value_t events_library(ant_t *js);
ant_value_t eventemitter_prototype(ant_t *js);

void init_events_module(void);
void js_dispatch_global_event(ant_t *js, ant_value_t event_obj);

bool eventemitter_add_listener(
  ant_t *js,
  ant_value_t target, const char *event_type,
  ant_value_t listener, bool once
);

bool eventemitter_add_listener_val(
  ant_t *js,
  ant_value_t target, ant_value_t key,
  ant_value_t listener, bool once
);

bool eventemitter_emit_args(
  ant_t *js,
  ant_value_t target, const char *event_type,
  ant_value_t *args, int nargs
);

bool eventemitter_emit_args_val(
  ant_t *js,
  ant_value_t target, ant_value_t key,
  ant_value_t *args, int nargs
);

bool eventemitter_remove_listener(
  ant_t *js,
  ant_value_t target, const char *event_type,
  ant_value_t listener
);

bool eventemitter_remove_listener_val(
  ant_t *js,
  ant_value_t target, ant_value_t key,
  ant_value_t listener
);

ant_offset_t eventemitter_listener_count(
  ant_t *js,
  ant_value_t target, const char *event_type
);

ant_offset_t eventemitter_listener_count_val(
  ant_t *js,
  ant_value_t target, ant_value_t key
);

#endif
