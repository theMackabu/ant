#ifndef ANT_TTY_CTRL_H
#define ANT_TTY_CTRL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "types.h"

ant_value_t tty_ctrl_bool_result(ant_t *js, bool ok);

const char *tty_ctrl_clear_line_seq(int dir, size_t *len_out);
const char *tty_ctrl_clear_screen_down_seq(size_t *len_out);

int tty_ctrl_normalize_clear_line_dir(int dir);
int tty_ctrl_normalize_coord(int value);

bool tty_ctrl_parse_int_value(ant_value_t value, int *out);
bool tty_ctrl_write_fd(int fd, const char *data, size_t len);
bool tty_ctrl_write_stream(FILE *stream, const char *data, size_t len, bool flush);

bool tty_ctrl_parse_clear_line_dir(ant_value_t *args, int nargs, int dir_index, int *dir_out);
bool tty_ctrl_build_cursor_to(char *buf, size_t buf_size, int x, bool has_y, int y, size_t *len_out);
bool tty_ctrl_build_move_cursor_axis(char *buf, size_t buf_size, int delta, bool horizontal, size_t *len_out);

typedef struct {
  int x;
  bool has_y;
  int y;
} tty_ctrl_cursor_to_args_t;

bool tty_ctrl_parse_cursor_to_args(
  ant_value_t *args, int nargs,
  int x_index, int y_index,
  tty_ctrl_cursor_to_args_t *out
);

bool tty_ctrl_parse_move_cursor_args(
  ant_value_t *args, int nargs,
  int dx_index, int dy_index,
  int *dx, int *dy
);

#endif
