#ifndef PROCESS_H
#define PROCESS_H

#include "types.h"

void init_process_module(void);
ant_value_t process_library(ant_t *js);

void process_enable_keypress_events(void);
void emit_process_event(const char *event_type, ant_value_t *args, int nargs);

bool has_active_stdin(void);
bool process_has_event_listeners(const char *event_type);

#endif
