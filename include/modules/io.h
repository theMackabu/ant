#ifndef IO_H
#define IO_H

#include "ant.h"
#include <stdio.h>
#include <stdbool.h>

extern bool io_no_color;

void init_console_module(void);
void print_value_colored(const char *str, FILE *stream);
void console_print(struct js *js, jsval_t *args, int nargs, const char *color, FILE *stream);

#endif
