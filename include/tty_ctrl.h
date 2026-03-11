#ifndef ANT_TTY_CTRL_H
#define ANT_TTY_CTRL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

bool tty_ctrl_write_fd(int fd, const char *data, size_t len);
bool tty_ctrl_write_stream(FILE *stream, const char *data, size_t len, bool flush);

const char *tty_ctrl_clear_line_seq(int dir, size_t *len_out);
const char *tty_ctrl_clear_screen_down_seq(size_t *len_out);

bool tty_ctrl_build_cursor_to(char *buf, size_t buf_size, int x, bool has_y, int y, size_t *len_out);
bool tty_ctrl_build_move_cursor_axis(char *buf, size_t buf_size, int delta, bool horizontal, size_t *len_out);

#endif
