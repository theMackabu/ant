#include <compat.h> // IWYU pragma: keep

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define ANT_WRITE_FD _write
#else
#include <unistd.h>
#define ANT_WRITE_FD write
#endif

#include "tty_ctrl.h"
#include "internal.h"

bool tty_ctrl_write_fd(int fd, const char *data, size_t len) {
  if (fd < 0 || !data) return false;
  if (len == 0) return true;

  size_t off = 0;
  while (off < len) {
#ifdef _WIN32
    size_t rem = len - off;
    unsigned int chunk = (rem > (size_t)INT_MAX) ? (unsigned int)INT_MAX : (unsigned int)rem;
    int wrote = ANT_WRITE_FD(fd, data + off, chunk);
    if (wrote <= 0) return false;
    off += (size_t)wrote;
#else
    ssize_t wrote = ANT_WRITE_FD(fd, data + off, len - off);
    if (wrote < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (wrote == 0) return false;
    off += (size_t)wrote;
#endif
  }

  return true;
}

bool tty_ctrl_write_stream(FILE *stream, const char *data, size_t len, bool flush) {
  if (!stream || !data) return false;
  if (len > 0 && fwrite(data, 1, len, stream) != len) return false;
  if (flush && fflush(stream) != 0) return false;
  return true;
}

bool tty_ctrl_parse_int_value(ant_value_t value, int *out) {
  if (vtype(value) != T_NUM) return false;
  double d = js_getnum(value);
  if (!isfinite(d)) return false;
  if (d < (double)INT_MIN || d > (double)INT_MAX) return false;
  int i = (int)d;
  if ((double)i != d) return false;
  *out = i;
  return true;
}

int tty_ctrl_normalize_clear_line_dir(int dir) {
  if (dir < 0) return -1;
  if (dir > 0) return 1;
  return 0;
}

int tty_ctrl_normalize_coord(int value) {
  return value < 0 ? 0 : value;
}

bool tty_ctrl_parse_clear_line_dir(ant_value_t *args, int nargs, int dir_index, int *dir_out) {
  int dir = 0;
  if (nargs > dir_index && vtype(args[dir_index]) != T_UNDEF) {
    if (!tty_ctrl_parse_int_value(args[dir_index], &dir)) return false;
  }
  *dir_out = tty_ctrl_normalize_clear_line_dir(dir);
  return true;
}

bool tty_ctrl_parse_cursor_to_args(
  ant_value_t *args,
  int nargs,
  int x_index,
  int y_index,
  tty_ctrl_cursor_to_args_t *out
) {
  if (!out || nargs <= x_index) return false;

  int x = 0;
  if (!tty_ctrl_parse_int_value(args[x_index], &x)) return false;
  out->x = tty_ctrl_normalize_coord(x);
  out->has_y = false;
  out->y = 0;

  if (nargs > y_index && vtype(args[y_index]) != T_UNDEF) {
    int y = 0;
    if (!tty_ctrl_parse_int_value(args[y_index], &y)) return false;
    out->has_y = true;
    out->y = tty_ctrl_normalize_coord(y);
  }

  return true;
}

bool tty_ctrl_parse_move_cursor_args(
  ant_value_t *args,
  int nargs,
  int dx_index,
  int dy_index,
  int *dx,
  int *dy
) {
  if (!dx || !dy) return false;
  if (nargs <= dy_index) return false;
  if (!tty_ctrl_parse_int_value(args[dx_index], dx)) return false;
  if (!tty_ctrl_parse_int_value(args[dy_index], dy)) return false;
  return true;
}

const char *tty_ctrl_clear_line_seq(int dir, size_t *len_out) {
  dir = tty_ctrl_normalize_clear_line_dir(dir);
  const char *seq = "\033[2K\r";
  if (dir < 0) seq = "\033[1K";
  else if (dir > 0) seq = "\033[0K";

  if (len_out) *len_out = strlen(seq);
  return seq;
}

const char *tty_ctrl_clear_screen_down_seq(size_t *len_out) {
  static const char seq[] = "\033[0J";
  if (len_out) *len_out = sizeof(seq) - 1;
  return seq;
}

bool tty_ctrl_build_cursor_to(char *buf, size_t buf_size, int x, bool has_y, int y, size_t *len_out) {
  if (!buf || buf_size == 0) return false;

  int n = 0;
  if (has_y) n = snprintf(buf, buf_size, "\033[%d;%dH", y + 1, x + 1);
  else n = snprintf(buf, buf_size, "\033[%dG", x + 1);

  if (n < 0 || (size_t)n >= buf_size) return false;
  if (len_out) *len_out = (size_t)n;
  return true;
}

bool tty_ctrl_build_move_cursor_axis(char *buf, size_t buf_size, int delta, bool horizontal, size_t *len_out) {
  if (!buf || buf_size == 0) return false;

  if (delta == 0) {
    if (len_out) *len_out = 0;
    return true;
  }

  int amount = (delta < 0) ? -delta : delta;
  char cmd = 0;
  if (horizontal) cmd = (delta > 0) ? 'C' : 'D';
  else cmd = (delta > 0) ? 'B' : 'A';

  int n = snprintf(buf, buf_size, "\033[%d%c", amount, cmd);
  if (n < 0 || (size_t)n >= buf_size) return false;

  if (len_out) *len_out = (size_t)n;
  return true;
}

ant_value_t tty_ctrl_bool_result(ant_t *js, bool ok) {
  return js_bool(ok);
}
