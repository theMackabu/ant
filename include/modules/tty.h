#ifndef ANT_TTY_MODULE_H
#define ANT_TTY_MODULE_H

#include "types.h"

ant_value_t tty_library(ant_t *js);

void init_tty_module(ant_t *js);
bool tty_set_raw_mode(int fd, bool enable);
void tty_set_sandbox_terminal(uint32_t capabilities, uint16_t rows, uint16_t cols);

#endif
