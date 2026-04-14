#include <compat.h> // IWYU pragma: keep

#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <uv.h>

#ifdef _WIN32
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define ANT_ISATTY _isatty
#define ANT_STDIN_FD 0
#define ANT_STDOUT_FD 1
#define ANT_STDERR_FD 2
#else
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#define ANT_ISATTY isatty
#define ANT_STDIN_FD STDIN_FILENO
#define ANT_STDOUT_FD STDOUT_FILENO
#define ANT_STDERR_FD STDERR_FILENO
#endif

#include "ant.h"
#include "gc/roots.h"
#include "descriptors.h"
#include "errors.h"
#include "internal.h"
#include "runtime.h"
#include "tty_ctrl.h"
#include "silver/engine.h"

#include "modules/stream.h"
#include "modules/buffer.h"
#include "modules/events.h"
#include "modules/symbol.h"
#include "modules/tty.h"

static ant_value_t g_tty_readstream_proto = 0;
static ant_value_t g_tty_readstream_ctor = 0;
static ant_value_t g_tty_writestream_proto = 0;
static ant_value_t g_tty_writestream_ctor = 0;

typedef struct tty_read_stream_state {
  ant_t *js;
  ant_value_t stream_obj;
  uv_tty_t tty;
  
  int fd;
  bool initialized;
  bool reading;
  bool closing;
} tty_read_stream_state_t;

static void invoke_callback_if_needed(ant_t *js, ant_value_t cb, ant_value_t arg) {
  if (!is_callable(cb)) return;
  ant_value_t cb_args[1] = { arg };
  sv_vm_call(js->vm, js, cb, js_mkundef(), cb_args, 1, NULL, false);
}

static bool parse_fd(ant_value_t value, int *fd_out) {
  int fd = 0;
  if (!tty_ctrl_parse_int_value(value, &fd)) return false;
  if (fd < 0) return false;
  *fd_out = fd;
  return true;
}

static bool is_tty_fd(int fd) {
  if (fd < 0) return false;
  return uv_guess_handle(fd) == UV_TTY;
}

static int stream_fd_from_this(ant_t *js, int fallback_fd) {
  ant_value_t this_obj = js_getthis(js);
  if (!is_special_object(this_obj)) return fallback_fd;

  ant_value_t fd_val = js_get(js, this_obj, "fd");
  int fd = 0;
  if (!parse_fd(fd_val, &fd)) return fallback_fd;
  return fd;
}

static tty_read_stream_state_t *tty_read_stream_state_from_obj(ant_value_t stream_obj) {
  return (tty_read_stream_state_t *)stream_get_attached_state(stream_obj);
}

static ant_value_t make_stream_error(ant_t *js, const char *op, int fd, int uv_code) {
  if (uv_code != 0) return js_mkerr_typed(
    js, JS_ERR_GENERIC,
    "tty stream %s failed for fd %d: %s",
    op, fd, uv_strerror(uv_code)
  );

  return js_mkerr_typed(js, JS_ERR_GENERIC, "tty stream %s failed for fd %d", op, fd);
}

static void tty_read_stream_emit_error(tty_read_stream_state_t *state, const char *op, int uv_code) {
  ant_value_t err = 0;
  if (!state || !state->js || !is_object_type(state->stream_obj)) return;
  err = make_stream_error(state->js, op, state->fd, uv_code);
  eventemitter_emit_args(state->js, state->stream_obj, "error", &err, 1);
}

static void tty_read_stream_push_chunk(tty_read_stream_state_t *state, const char *data, size_t len) {
  ArrayBufferData *ab = NULL;
  ant_value_t chunk = 0;

  if (!state || !state->js || !data || len == 0) return;
  ab = create_array_buffer_data(len);
  if (ab) memcpy(ab->data, data, len);
  
  chunk = ab
    ? create_typed_array(state->js, TYPED_ARRAY_UINT8, ab, 0, len, "Buffer")
    : js_mkstr(state->js, data, len);
  (void)stream_readable_push(state->js, state->stream_obj, chunk, js_mkundef());
}

static void tty_read_stream_alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  buf->base = malloc(suggested_size);
#ifdef _WIN32
  buf->len = (ULONG)suggested_size;
#else
  buf->len = suggested_size;
#endif
}

static void tty_read_stream_stop(tty_read_stream_state_t *state) {
  if (!state || !state->initialized || !state->reading) return;
  uv_read_stop((uv_stream_t *)&state->tty);
  state->reading = false;
}

static void tty_read_stream_close_cb(uv_handle_t *handle) {
  tty_read_stream_state_t *state = (tty_read_stream_state_t *)handle->data;
  free(state);
}

static void tty_read_stream_finalize(ant_t *js, ant_value_t stream_obj, void *data) {
  tty_read_stream_state_t *state = (tty_read_stream_state_t *)data;

  if (!state) return;
  tty_read_stream_stop(state);

  if (state->initialized && !state->closing) {
    state->closing = true;
    uv_close((uv_handle_t *)&state->tty, tty_read_stream_close_cb);
    return;
  }

  if (!state->initialized) free(state);
}

static void tty_read_stream_on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  tty_read_stream_state_t *state = (tty_read_stream_state_t *)stream->data;

  if (!state || !state->js) goto cleanup;

  if (nread > 0) {
    tty_read_stream_push_chunk(state, buf->base, (size_t)nread);
    goto cleanup;
  }

  if (nread == UV_EOF) {
    tty_read_stream_stop(state);
    (void)stream_readable_push(state->js, state->stream_obj, js_mknull(), js_mkundef());
    goto cleanup;
  }

  if (nread < 0) {
    tty_read_stream_stop(state);
    tty_read_stream_emit_error(state, "read", (int)nread);
  }

cleanup:
  if (buf->base) free(buf->base);
}

static ant_value_t tty_readstream__read(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = js_getthis(js);
  tty_read_stream_state_t *state = tty_read_stream_state_from_obj(stream_obj);
  int rc = 0;

  if (!state) return js_mkundef();
  if (state->closing || js_truthy(js, js_get(js, stream_obj, "destroyed"))) return js_mkundef();

  if (!state->initialized) {
    rc = uv_tty_init(uv_default_loop(), &state->tty, state->fd, 0);
    if (rc != 0) {
      tty_read_stream_emit_error(state, "open", rc);
      return js_mkundef();
    }
  #ifndef _WIN32
    uv_tty_set_mode(&state->tty, tty_is_raw_mode(state->fd) ? UV_TTY_MODE_RAW : UV_TTY_MODE_NORMAL);
  #endif
    state->tty.data = state;
    state->initialized = true;
  }

  if (state->reading) return js_mkundef();
  rc = uv_read_start((uv_stream_t *)&state->tty, tty_read_stream_alloc_buffer, tty_read_stream_on_read);
  
  if (rc != 0) {
    tty_read_stream_emit_error(state, "read", rc);
    return js_mkundef();
  }

  state->reading = true;
  return js_mkundef();
}

static ant_value_t tty_readstream__destroy(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t stream_obj = js_getthis(js);
  tty_read_stream_state_t *state = tty_read_stream_state_from_obj(stream_obj);
  ant_value_t cb = nargs > 1 ? args[1] : js_mkundef();

  if (!state) {
    invoke_callback_if_needed(js, cb, js_mknull());
    return js_mkundef();
  }

  tty_read_stream_stop(state);
  stream_clear_attached_state(stream_obj);

  if (state->initialized && !state->closing) {
    state->closing = true;
    uv_close((uv_handle_t *)&state->tty, tty_read_stream_close_cb);
  } else if (!state->initialized) free(state);

  invoke_callback_if_needed(js, cb, js_mknull());
  return js_mkundef();
}

static void get_tty_size(int fd, int *rows, int *cols) {
  int out_rows = 24;
  int out_cols = 80;

#ifdef _WIN32
  HANDLE handle = INVALID_HANDLE_VALUE;
  if (fd == ANT_STDOUT_FD) {
    handle = GetStdHandle(STD_OUTPUT_HANDLE);
  } else if (fd == ANT_STDERR_FD) {
    handle = GetStdHandle(STD_ERROR_HANDLE);
  } else {
    intptr_t os_handle = _get_osfhandle(fd);
    if (os_handle != -1) handle = (HANDLE)os_handle;
  }

  if (handle != INVALID_HANDLE_VALUE) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(handle, &csbi)) {
      int width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
      int height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
      if (height > 0) out_rows = height;
      if (width > 0) out_cols = width;
    }
  }
#else
  struct winsize ws;
  if (ioctl(fd, TIOCGWINSZ, &ws) == 0) {
    if (ws.ws_row > 0) out_rows = (int)ws.ws_row;
    if (ws.ws_col > 0) out_cols = (int)ws.ws_col;
  }
#endif

  if (rows) *rows = out_rows;
  if (cols) *cols = out_cols;
}

static bool str_case_eq(const char *a, const char *b) {
  if (!a || !b) return false;
  while (*a && *b) {
    int ca = tolower((unsigned char)*a);
    int cb = tolower((unsigned char)*b);
    if (ca != cb) return false;
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

static bool str_case_contains(const char *haystack, const char *needle) {
  if (!haystack || !needle) return false;
  size_t needle_len = strlen(needle);
  if (needle_len == 0) return true;

  size_t hay_len = strlen(haystack);
  if (needle_len > hay_len) return false;

  for (size_t i = 0; i + needle_len <= hay_len; i++) {
    bool match = true;
    for (size_t j = 0; j < needle_len; j++) {
      int ca = tolower((unsigned char)haystack[i + j]);
      int cb = tolower((unsigned char)needle[j]);
      if (ca != cb) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

static bool read_env_object_value(
  ant_t *js,
  ant_value_t env_obj,
  const char *key,
  char *buf,
  size_t buf_len
) {
  if (!is_special_object(env_obj) || !key || !buf || buf_len == 0) return false;

  ant_value_t value = js_get(js, env_obj, key);
  if (vtype(value) == T_UNDEF || vtype(value) == T_NULL) return false;

  ant_value_t str_val = js_tostring_val(js, value);
  size_t len = 0;
  char *str = js_getstr(js, str_val, &len);
  if (!str) return false;

  if (len >= buf_len) len = buf_len - 1;
  memcpy(buf, str, len);
  buf[len] = '\0';
  return true;
}

static const char *get_env_value(
  ant_t *js,
  ant_value_t env_obj,
  const char *key,
  char *buf,
  size_t buf_len
) {
  if (read_env_object_value(js, env_obj, key, buf, buf_len)) return buf;
  return getenv(key);
}

static int force_color_depth(const char *force_color) {
  if (!force_color) return 0;
  if (*force_color == '\0') return 4;
  if (strcmp(force_color, "0") == 0) return 1;
  if (strcmp(force_color, "1") == 0) return 4;
  if (strcmp(force_color, "2") == 0) return 8;
  if (strcmp(force_color, "3") == 0) return 24;
  return 4;
}

static int detect_color_depth(ant_t *js, int fd, ant_value_t env_obj) {
  char scratch[128];
  const char *force_color = get_env_value(js, env_obj, "FORCE_COLOR", scratch, sizeof(scratch));
  
  int forced = force_color_depth(force_color);
  if (forced > 0) return forced;

  const char *no_color = get_env_value(js, env_obj, "NO_COLOR", scratch, sizeof(scratch));
  if (no_color && *no_color) return 1;
  if (!is_tty_fd(fd)) return 1;

  const char *colorterm = get_env_value(js, env_obj, "COLORTERM", scratch, sizeof(scratch));
  if (colorterm) {
    if (str_case_contains(colorterm, "truecolor") || str_case_contains(colorterm, "24bit")) return 24;
  }

#ifdef _WIN32
  return 24;
#else
  const char *term = get_env_value(js, env_obj, "TERM", scratch, sizeof(scratch));
  if (!term) return 4;
  if (str_case_eq(term, "dumb")) return 1;
  if (str_case_contains(term, "256color")) return 8;
  if (str_case_contains(term, "color")
    || str_case_contains(term, "xterm")
    || str_case_contains(term, "screen")
    || str_case_contains(term, "ansi")
    || str_case_contains(term, "linux")
    || str_case_contains(term, "cygwin")
    || str_case_contains(term, "vt100")
  ) return 4;
  return 4;
#endif
}

static int palette_size_for_depth(int depth) {
  switch (depth) {
    case 1: return 2;
    case 4: return 16;
    case 8: return 256;
    case 24: return 16777216;
    default: return 2;
  }
}

static ant_value_t maybe_callback_or_throw(
  ant_t *js, ant_value_t this_obj,
  ant_value_t cb, bool ok,
  const char *op, int fd
) {
  if (is_callable(cb)) {
    if (ok) invoke_callback_if_needed(js, cb, js_mknull());
    else {
      ant_value_t err = make_stream_error(js, op, fd, 0);
      invoke_callback_if_needed(js, cb, err);
    }
    return this_obj;
  }
  if (!ok) return make_stream_error(js, op, fd, 0);
  return this_obj;
}

#ifndef _WIN32
static struct {
  int fd;
  bool active;
  struct termios saved;
} raw_state = { .fd = -1, .active = false };

static int tty_tcsetattr_no_sigtou(int fd, int optional_actions, const struct termios *tio) {
#ifdef SIGTTOU
  sigset_t block_set;
  sigset_t prev_set;
  if (sigemptyset(&block_set) == 0
      && sigaddset(&block_set, SIGTTOU) == 0
      && sigprocmask(SIG_BLOCK, &block_set, &prev_set) == 0) {
    int rc = tcsetattr(fd, optional_actions, tio);
    int saved_errno = errno;
    sigprocmask(SIG_SETMASK, &prev_set, NULL);
    errno = saved_errno;
    return rc;
  }
#endif
  return tcsetattr(fd, optional_actions, tio);
}
#endif

bool tty_set_raw_mode(int fd, bool enable) {
#ifdef _WIN32
  intptr_t os_handle = _get_osfhandle(fd);
  if (os_handle == -1) return false;
  HANDLE handle = (HANDLE)os_handle;

  DWORD mode = 0;
  if (!GetConsoleMode(handle, &mode)) return false;

  if (enable) {
    mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
  } else {
    mode |= ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT;
  }

  return SetConsoleMode(handle, mode) != 0;
#else
  if (!is_tty_fd(fd)) return false;

  if (enable) {
    if (raw_state.active && raw_state.fd == fd) return true;

    struct termios saved;
    if (tcgetattr(fd, &saved) == -1) return false;

    struct termios raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
#ifdef VDISCARD
    raw.c_cc[VDISCARD] = _POSIX_VDISABLE;
#endif
#ifdef VLNEXT
    raw.c_cc[VLNEXT] = _POSIX_VDISABLE;
#endif
    if (tty_tcsetattr_no_sigtou(fd, TCSANOW, &raw) == -1) return false;
    raw_state.fd = fd;
    raw_state.saved = saved;
    raw_state.active = true;
    return true;
  }

  if (!(raw_state.active && raw_state.fd == fd)) return true;
  if (tty_tcsetattr_no_sigtou(fd, TCSANOW, &raw_state.saved) == -1) return false;
  raw_state.fd = -1;
  raw_state.active = false;
  return true;
#endif
}

bool tty_is_raw_mode(int fd) {
#ifdef _WIN32
  return false;
#else
  return raw_state.active && raw_state.fd == fd;
#endif
}

static ant_value_t get_process_stream(ant_t *js, const char *name) {
  ant_value_t process_obj = js_get(js, js_glob(js), "process");
  if (!is_special_object(process_obj)) return js_mkundef();

  ant_value_t stream = js_get(js, process_obj, name);
  if (!is_special_object(stream)) return js_mkundef();
  return stream;
}

static void ensure_stream_common_props(ant_t *js, ant_value_t stream, int fd) {
  if (!is_special_object(stream)) return;
  js_set(js, stream, "fd", js_mknum((double)fd));
  js_set(js, stream, "isTTY", js_bool(is_tty_fd(fd)));
}

static ant_value_t tty_isatty(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_false;

  int fd = 0;
  if (!parse_fd(args[0], &fd)) return js_false;
  return js_bool(ANT_ISATTY(fd) != 0);
}

static ant_value_t tty_stream_write(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_false;

  size_t len = 0;
  char *data = js_getstr(js, args[0], &len);
  if (!data) return js_false;

  ant_value_t cb = js_mkundef();
  if (nargs > 1 && is_callable(args[1])) cb = args[1];

  int fd = stream_fd_from_this(js, ANT_STDOUT_FD);
  bool ok = tty_ctrl_write_fd(fd, data, len);

  if (is_callable(cb)) {
    if (ok) invoke_callback_if_needed(js, cb, js_mknull());
    else invoke_callback_if_needed(js, cb, make_stream_error(js, "write", fd, 0));
  }

  if (!ok) return js_false;
  return js_true;
}

static ant_value_t tty_write_stream_rows_getter(ant_t *js, ant_value_t *args, int nargs) {
  int fd = stream_fd_from_this(js, ANT_STDOUT_FD);
  if (!is_tty_fd(fd)) return js_mkundef();

  int rows = 0;
  int cols = 0;
  get_tty_size(fd, &rows, &cols);
  return js_mknum((double)rows);
}

static ant_value_t tty_write_stream_columns_getter(ant_t *js, ant_value_t *args, int nargs) {
  int fd = stream_fd_from_this(js, ANT_STDOUT_FD);
  if (!is_tty_fd(fd)) return js_mkundef();

  int rows = 0;
  int cols = 0;
  get_tty_size(fd, &rows, &cols);
  return js_mknum((double)cols);
}

static ant_value_t tty_write_stream_get_window_size(ant_t *js, ant_value_t *args, int nargs) {
  int fd = stream_fd_from_this(js, ANT_STDOUT_FD);
  int rows = 0;
  int cols = 0;
  get_tty_size(fd, &rows, &cols);

  ant_value_t arr = js_mkarr(js);
  js_arr_push(js, arr, js_mknum((double)cols));
  js_arr_push(js, arr, js_mknum((double)rows));
  return arr;
}

static ant_value_t tty_write_stream_clear_line(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);

  int dir = 0;
  if (!tty_ctrl_parse_clear_line_dir(args, nargs, 0, &dir)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "clearLine(dir) requires a numeric dir");
  }

  ant_value_t cb = js_mkundef();
  if (nargs > 1 && is_callable(args[1])) cb = args[1];

  int fd = stream_fd_from_this(js, ANT_STDOUT_FD);
  size_t seq_len = 0;
  
  const char *seq = tty_ctrl_clear_line_seq(dir, &seq_len);
  bool ok = tty_ctrl_write_fd(fd, seq, seq_len);
  
  return maybe_callback_or_throw(js, this_obj, cb, ok, "clearLine", fd);
}

static ant_value_t tty_write_stream_clear_screen_down(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);

  ant_value_t cb = js_mkundef();
  if (nargs > 0 && is_callable(args[0])) cb = args[0];

  int fd = stream_fd_from_this(js, ANT_STDOUT_FD);
  size_t seq_len = 0;
  
  const char *seq = tty_ctrl_clear_screen_down_seq(&seq_len);
  bool ok = tty_ctrl_write_fd(fd, seq, seq_len);
  
  return maybe_callback_or_throw(js, this_obj, cb, ok, "clearScreenDown", fd);
}

static ant_value_t tty_write_stream_cursor_to(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  int x = 0;
  
  if (nargs < 1 || !tty_ctrl_parse_int_value(args[0], &x)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "cursorTo(x[, y][, callback]) requires numeric x");
  }

  x = tty_ctrl_normalize_coord(x);

  bool has_y = false;
  int y = 0;
  ant_value_t cb = js_mkundef();

  if (nargs > 1) {
    if (is_callable(args[1])) {
      cb = args[1];
    } else if (vtype(args[1]) == T_UNDEF) {
      // no-op
    } else if (tty_ctrl_parse_int_value(args[1], &y)) {
      has_y = true;
      y = tty_ctrl_normalize_coord(y);
      if (nargs > 2 && is_callable(args[2])) cb = args[2];
    } else {
      return js_mkerr_typed(js, JS_ERR_TYPE, "cursorTo y must be a number when provided");
    }
  }

  char seq[64];
  size_t seq_len = 0;
  if (!tty_ctrl_build_cursor_to(seq, sizeof(seq), x, has_y, y, &seq_len)) {
    return js_mkerr(js, "Failed to build cursor sequence");
  }

  int fd = stream_fd_from_this(js, ANT_STDOUT_FD);
  bool ok = tty_ctrl_write_fd(fd, seq, seq_len);
  return maybe_callback_or_throw(js, this_obj, cb, ok, "cursorTo", fd);
}

static ant_value_t tty_write_stream_move_cursor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  if (nargs < 2) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "moveCursor(dx, dy[, callback]) requires dx and dy");
  }

  int dx = 0;
  int dy = 0;
  if (!tty_ctrl_parse_move_cursor_args(args, nargs, 0, 1, &dx, &dy)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "moveCursor(dx, dy[, callback]) requires numeric dx and dy");
  }

  ant_value_t cb = js_mkundef();
  if (nargs > 2 && is_callable(args[2])) cb = args[2];

  int fd = stream_fd_from_this(js, ANT_STDOUT_FD);
  bool ok = true;

  if (dx != 0) {
    char seq_x[32];
    size_t len_x = 0;
    if (!tty_ctrl_build_move_cursor_axis(seq_x, sizeof(seq_x), dx, true, &len_x)) {
      return js_mkerr(js, "Failed to build moveCursor sequence");
    }
    if (!tty_ctrl_write_fd(fd, seq_x, len_x)) ok = false;
  }

  if (ok && dy != 0) {
    char seq_y[32];
    size_t len_y = 0;
    if (!tty_ctrl_build_move_cursor_axis(seq_y, sizeof(seq_y), dy, false, &len_y)) {
      return js_mkerr(js, "Failed to build moveCursor sequence");
    }
    if (!tty_ctrl_write_fd(fd, seq_y, len_y)) ok = false;
  }

  return maybe_callback_or_throw(js, this_obj, cb, ok, "moveCursor", fd);
}

static ant_value_t tty_write_stream_get_color_depth(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t env_obj = js_mkundef();
  if (nargs > 0 && is_special_object(args[0])) env_obj = args[0];

  int fd = stream_fd_from_this(js, ANT_STDOUT_FD);
  int depth = detect_color_depth(js, fd, env_obj);
  return js_mknum((double)depth);
}

static ant_value_t tty_write_stream_has_colors(ant_t *js, ant_value_t *args, int nargs) {
  int count = 16;
  ant_value_t env_obj = js_mkundef();

  if (nargs > 0) {
    if (vtype(args[0]) == T_NUM) {
      int parsed_count = 16;
      if (!tty_ctrl_parse_int_value(args[0], &parsed_count)) {
        return js_mkerr_typed(js, JS_ERR_TYPE, "hasColors(count[, env]) count must be an integer");
      }
      count = parsed_count;
    } else if (is_special_object(args[0])) {
      env_obj = args[0];
    } else if (vtype(args[0]) != T_UNDEF) {
      return js_mkerr_typed(js, JS_ERR_TYPE, "hasColors(count[, env]) invalid first argument");
    }
  }

  if (nargs > 1 && is_special_object(args[1])) env_obj = args[1];
  if (count < 1) count = 1;

  int fd = stream_fd_from_this(js, ANT_STDOUT_FD);
  int depth = detect_color_depth(js, fd, env_obj);
  int max_colors = palette_size_for_depth(depth);
  return js_bool(max_colors >= count);
}

static ant_value_t tty_read_stream_set_raw_mode(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  tty_read_stream_state_t *state = tty_read_stream_state_from_obj(this_obj);
  if (!is_special_object(this_obj)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "setRawMode() requires a ReadStream receiver");
  }

  bool enable = nargs > 0 ? js_truthy(js, args[0]) : true;
  int fd = stream_fd_from_this(js, ANT_STDIN_FD);
  if (!tty_set_raw_mode(fd, enable)) {
    return js_mkerr_typed(js, JS_ERR_GENERIC, "Failed to set raw mode for fd %d", fd);
  }
#ifndef _WIN32
  if (state && state->initialized && !state->closing) {
    uv_tty_set_mode(&state->tty, enable ? UV_TTY_MODE_RAW : UV_TTY_MODE_NORMAL);
  }
#endif
  js_set(js, this_obj, "isRaw", js_bool(enable));
  return this_obj;
}

static ant_value_t tty_read_stream_constructor(ant_t *js, ant_value_t *args, int nargs) {
  tty_read_stream_state_t *state = NULL;
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "ReadStream(fd) requires a file descriptor");

  int fd = 0;
  if (!parse_fd(args[0], &fd)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "ReadStream(fd) requires an integer file descriptor");
  }
  if (!is_tty_fd(fd)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "ReadStream fd %d is not a TTY", fd);
  }

  if (fd == ANT_STDIN_FD) {
  ant_value_t stdin_obj = get_process_stream(js, "stdin");
  if (is_special_object(stdin_obj)) {
    ensure_stream_common_props(js, stdin_obj, fd);
    if (vtype(js_get(js, stdin_obj, "isRaw")) == T_UNDEF) js_set(js, stdin_obj, "isRaw", js_false);
    return stdin_obj;
  }}

  ant_value_t obj = stream_construct_readable(js, g_tty_readstream_proto, js_mkundef());
  if (is_err(obj)) return obj;

  state = calloc(1, sizeof(*state));
  if (!state) return js_mkerr(js, "Out of memory");
  state->js = js;
  state->stream_obj = obj;
  state->fd = fd;

  ensure_stream_common_props(js, obj, fd);
  stream_set_attached_state(obj, state, tty_read_stream_finalize);
  
  js_set(js, obj, "isRaw", js_false);
  js_set(js, obj, "_read", js_mkfun(tty_readstream__read));
  js_set(js, obj, "_destroy", js_mkfun(tty_readstream__destroy));
  
  return obj;
}

static ant_value_t tty_write_stream_constructor(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "WriteStream(fd) requires a file descriptor");

  int fd = 0;
  if (!parse_fd(args[0], &fd)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "WriteStream(fd) requires an integer file descriptor");
  }
  if (!is_tty_fd(fd)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "WriteStream fd %d is not a TTY", fd);
  }

  if (fd == ANT_STDOUT_FD || fd == ANT_STDERR_FD) {
    ant_value_t stream = get_process_stream(js, fd == ANT_STDOUT_FD ? "stdout" : "stderr");
    if (is_special_object(stream)) {
      ensure_stream_common_props(js, stream, fd);
      return stream;
    }
  }

  ant_value_t obj = stream_construct_writable(js, g_tty_writestream_proto, js_mkundef());
  if (is_err(obj)) return obj;

  ensure_stream_common_props(js, obj, fd);
  return obj;
}

static void setup_readstream_proto(ant_t *js, ant_value_t proto) {
  if (!is_special_object(proto)) return;
  js_set(js, proto, "setRawMode", js_mkfun(tty_read_stream_set_raw_mode));
  js_set_sym(js, proto, get_toStringTag_sym(), js_mkstr(js, "ReadStream", 10));
}

static void setup_writestream_proto(ant_t *js, ant_value_t proto) {
  if (!is_special_object(proto)) return;

  js_set(js, proto, "write", js_mkfun(tty_stream_write));
  js_set(js, proto, "clearLine", js_mkfun(tty_write_stream_clear_line));
  js_set(js, proto, "clearScreenDown", js_mkfun(tty_write_stream_clear_screen_down));
  js_set(js, proto, "cursorTo", js_mkfun(tty_write_stream_cursor_to));
  js_set(js, proto, "moveCursor", js_mkfun(tty_write_stream_move_cursor));
  js_set(js, proto, "getWindowSize", js_mkfun(tty_write_stream_get_window_size));
  js_set(js, proto, "getColorDepth", js_mkfun(tty_write_stream_get_color_depth));
  js_set(js, proto, "hasColors", js_mkfun(tty_write_stream_has_colors));
  js_set_getter_desc(js, proto, "rows", 4, js_mkfun(tty_write_stream_rows_getter), JS_DESC_E | JS_DESC_C);
  js_set_getter_desc(js, proto, "columns", 7, js_mkfun(tty_write_stream_columns_getter), JS_DESC_E | JS_DESC_C);
  js_set_sym(js, proto, get_toStringTag_sym(), js_mkstr(js, "WriteStream", 11));
}

static void tty_init_stream_constructors(ant_t *js) {
  if (g_tty_readstream_ctor && g_tty_writestream_ctor) return;
  stream_init_constructors(js);

  g_tty_readstream_proto = js_mkobj(js);
  js_set_proto_init(g_tty_readstream_proto, stream_readable_prototype(js));
  setup_readstream_proto(js, g_tty_readstream_proto);
  
  g_tty_readstream_ctor = js_make_ctor(js, tty_read_stream_constructor, g_tty_readstream_proto, "ReadStream", 10);
  js_set_proto_init(g_tty_readstream_ctor, stream_readable_constructor(js));
  
  gc_register_root(&g_tty_readstream_proto);
  gc_register_root(&g_tty_readstream_ctor);

  g_tty_writestream_proto = js_mkobj(js);
  js_set_proto_init(g_tty_writestream_proto, stream_writable_prototype(js));
  setup_writestream_proto(js, g_tty_writestream_proto);
  
  g_tty_writestream_ctor = js_make_ctor(js, tty_write_stream_constructor, g_tty_writestream_proto, "WriteStream", 11);
  js_set_proto_init(g_tty_writestream_ctor, stream_writable_constructor(js));
  
  gc_register_root(&g_tty_writestream_proto);
  gc_register_root(&g_tty_writestream_ctor);
}

void init_tty_module(void) {
  ant_t *js = rt->js;
  if (!js) return;
  tty_init_stream_constructors(js);

  ant_value_t process_obj = js_get(js, js_glob(js), "process");
  if (!is_special_object(process_obj)) return;

  ant_value_t stdin_obj = js_get(js, process_obj, "stdin");
  if (is_special_object(stdin_obj)) {
    ensure_stream_common_props(js, stdin_obj, ANT_STDIN_FD);
    if (vtype(js_get(js, stdin_obj, "isRaw")) == T_UNDEF) js_set(js, stdin_obj, "isRaw", js_false);

    ant_value_t stdin_proto = js_get_proto(js, stdin_obj);
    if (is_special_object(stdin_proto)) {
      js_set_proto_init(stdin_proto, stream_readable_prototype(js));
      setup_readstream_proto(js, stdin_proto);
    }
    stream_init_readable_object(js, stdin_obj, js_mkundef());
  }

  ant_value_t stdout_obj = js_get(js, process_obj, "stdout");
  if (is_special_object(stdout_obj)) {
    ensure_stream_common_props(js, stdout_obj, ANT_STDOUT_FD);
    js_set_getter_desc(js, stdout_obj, "rows", 4, js_mkfun(tty_write_stream_rows_getter), JS_DESC_E | JS_DESC_C);
    js_set_getter_desc(js, stdout_obj, "columns", 7, js_mkfun(tty_write_stream_columns_getter), JS_DESC_E | JS_DESC_C);

    ant_value_t stdout_proto = js_get_proto(js, stdout_obj);
    if (is_special_object(stdout_proto)) js_set_proto_init(stdout_proto, stream_duplex_prototype(js));
    setup_writestream_proto(js, stdout_proto);
    stream_init_duplex_object(js, stdout_obj, js_mkundef());
    js_set(js, stdout_obj, "readable", js_false);
  }

  ant_value_t stderr_obj = js_get(js, process_obj, "stderr");
  if (is_special_object(stderr_obj)) {
    ensure_stream_common_props(js, stderr_obj, ANT_STDERR_FD);
    js_set_getter_desc(js, stderr_obj, "rows", 4, js_mkfun(tty_write_stream_rows_getter), JS_DESC_E | JS_DESC_C);
    js_set_getter_desc(js, stderr_obj, "columns", 7, js_mkfun(tty_write_stream_columns_getter), JS_DESC_E | JS_DESC_C);
    
    ant_value_t stderr_proto = js_get_proto(js, stderr_obj);
    if (is_special_object(stderr_proto)) js_set_proto_init(stderr_proto, stream_duplex_prototype(js));
    setup_writestream_proto(js, stderr_proto);
    stream_init_duplex_object(js, stderr_obj, js_mkundef());
    js_set(js, stderr_obj, "readable", js_false);
  }
}

ant_value_t tty_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);
  tty_init_stream_constructors(js);

  js_set(js, lib, "isatty", js_mkfun(tty_isatty));
  js_set(js, lib, "ReadStream", g_tty_readstream_ctor);
  js_set(js, lib, "WriteStream", g_tty_writestream_ctor);
  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "tty", 3));

  return lib;
}
