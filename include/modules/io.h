#ifndef IO_H
#define IO_H

#include "types.h"
#include <stdio.h>
#include <stdbool.h>

extern bool io_no_color;

typedef struct {
  jsoff_t *visited;
  int count;
  int capacity;
} inspect_visited_t;

#define C(color)  (io_no_color ? "" : (color))
#define C_RESET   C("\x1b[0m")
#define C_BOLD    C("\x1b[1m")
#define C_DIM     C("\x1b[2m")
#define C_UL      C("\x1b[4m")
#define C_UL_OFF  C("\x1b[24m")
#define C_GREEN   C("\x1b[32m")
#define C_YELLOW  C("\x1b[33m")
#define C_BLUE    C("\x1b[34m")
#define C_MAGENTA C("\x1b[35m")
#define C_CYAN    C("\x1b[36m")
#define C_WHITE   C("\x1b[37m")
#define C_RED     C("\x1b[31m")

void init_console_module(void);
void print_value_colored(const char *str, FILE *stream);
void print_repl_value(ant_t *js, jsval_t val, FILE *stream);

void inspect_object(
  ant_t *js, jsval_t obj,
  FILE *stream, int depth, 
  inspect_visited_t *visited
);

void inspect_value(
  ant_t *js, jsval_t val,
  FILE *stream, int depth,
  inspect_visited_t *visited
);

jsval_t console_print(
  ant_t *js, jsval_t *args,
  int nargs, const char *color, FILE *stream
);

#endif
