#ifndef PROCESS_H
#define PROCESS_H

#include "types.h"

void process_enable_keypress_events(void);
void emit_process_event(const char *event_type, ant_value_t *args, int nargs);

void init_process_module(void);
bool has_active_stdin(void);

ant_value_t process_library(ant_t *js);

#endif
