#ifndef ANT_TTY_MODULE_H
#define ANT_TTY_MODULE_H

#include "types.h"

void init_tty_module(void);
ant_value_t tty_library(ant_t *js);

bool tty_set_raw_mode(int fd, bool enable);
bool tty_is_raw_mode(int fd);

#endif
