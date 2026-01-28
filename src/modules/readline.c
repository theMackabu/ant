#include <compat.h> // IWYU pragma: keep

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <uthash.h>
#include <uv.h>

#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#include <io.h>
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#else
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#endif

#include "ant.h"
#include "errors.h"
#include "runtime.h"
#include "internal.h"

#include "modules/readline.h"
#include "modules/process.h"
#include "modules/symbol.h"

#define MAX_LINE_LENGTH 4096
#define MAX_HISTORY 1000
#define DEFAULT_PROMPT "> "
#define DEFAULT_HISTORY_SIZE 30
#define DEFAULT_TAB_SIZE 8
#define MAX_INTERFACES 64

typedef struct {
  jsval_t listener;
  bool once;
} RLEventListener;

#define MAX_LISTENERS_PER_EVENT 16

typedef struct {
  char *event_type;
  RLEventListener listeners[MAX_LISTENERS_PER_EVENT];
  int listener_count;
  UT_hash_handle hh;
} RLEventType;

typedef struct {
  char **lines;
  int count;
  int capacity;
  int current;
} rl_history_t;

typedef struct rl_interface {
  uint64_t id;
  jsval_t input_stream;
  jsval_t output_stream;
  jsval_t completer;
  jsval_t js_obj;
  char *prompt;
  char *line_buffer;
  int line_pos;
  int line_len;
  rl_history_t history;
  bool terminal;
  bool paused;
  bool closed;
  bool reading;
  int history_size;
  bool remove_history_duplicates;
  int crlf_delay;
  int escape_code_timeout;
  int tab_size;
  jsval_t pending_question_resolve;
  jsval_t pending_question_reject;
  RLEventType *events;
  uv_tty_t tty_in;
  uv_tty_t tty_out;
  bool tty_initialized;
  int escape_state;
  char escape_buf[16];
  int escape_len;
  int last_render_rows;
  UT_hash_handle hh;
#ifndef _WIN32
  struct termios saved_termios;
  bool raw_mode;
#endif
} rl_interface_t;

static rl_interface_t *interfaces = NULL;
static uint64_t next_interface_id = 1;

static void rl_history_init(rl_history_t *hist, int capacity) {
  hist->capacity = capacity > 0 ? capacity : DEFAULT_HISTORY_SIZE;
  hist->lines = calloc(hist->capacity, sizeof(char*));
  hist->count = 0;
  hist->current = -1;
}

static void rl_history_add(rl_history_t *hist, const char *line, bool remove_duplicates) {
  if (!line || strlen(line) == 0) return;
  
  if (remove_duplicates) {
    for (int i = 0; i < hist->count; i++) {
      if (strcmp(hist->lines[i], line) == 0) {
        free(hist->lines[i]);
        for (int j = i; j < hist->count - 1; j++) {
          hist->lines[j] = hist->lines[j + 1];
        }
        hist->count--; break;
      }
    }
  } else if (hist->count > 0 && strcmp(hist->lines[hist->count - 1], line) == 0) {
    return;
  }
  
  if (hist->count >= hist->capacity) {
    free(hist->lines[0]);
    memmove(hist->lines, hist->lines + 1, sizeof(char*) * (hist->capacity - 1));
    hist->count--;
  }
  
  hist->lines[hist->count++] = strdup(line);
  hist->current = hist->count;
}

static const char *rl_history_prev(rl_history_t *hist) {
  if (hist->count == 0) return NULL;
  if (hist->current > 0) hist->current--;
  return hist->lines[hist->current];
}

static const char *rl_history_next(rl_history_t *hist) {
  if (hist->count == 0) return NULL;
  if (hist->current < hist->count - 1) {
    hist->current++;
    return hist->lines[hist->current];
  }
  hist->current = hist->count;
  return "";
}

static void rl_history_free(rl_history_t *hist) {
  if (hist->lines) {
    for (int i = 0; i < hist->count; i++) {
      free(hist->lines[i]);
    }
    free(hist->lines);
    hist->lines = NULL;
  }
  hist->count = 0;
  hist->current = -1;
}

static RLEventType *find_or_create_event_type(rl_interface_t *iface, const char *event_type) {
  RLEventType *evt = NULL;
  HASH_FIND_STR(iface->events, event_type, evt);
  
  if (evt == NULL) {
    evt = malloc(sizeof(RLEventType));
    evt->event_type = strdup(event_type);
    evt->listener_count = 0;
    HASH_ADD_KEYPTR(hh, iface->events, evt->event_type, strlen(evt->event_type), evt);
  }
  
  return evt;
}

static void emit_event(struct js *js, rl_interface_t *iface, const char *event_type, jsval_t *args, int nargs) {
  RLEventType *evt = NULL;
  HASH_FIND_STR(iface->events, event_type, evt);
  
  if (evt == NULL || evt->listener_count == 0) return;
  
  int i = 0;
  while (i < evt->listener_count) {
    RLEventListener *listener = &evt->listeners[i];
    js_call(js, listener->listener, args, nargs);
    
    if (listener->once) {
      for (int j = i; j < evt->listener_count - 1; j++) {
        evt->listeners[j] = evt->listeners[j + 1];
      }
      evt->listener_count--;
    } else i++;
  }
}

static jsval_t get_history_array(struct js *js, rl_interface_t *iface) {
  jsval_t arr = js_mkarr(js);
  for (int i = 0; i < iface->history.count; i++) {
    js_arr_push(js, arr, js_mkstr(js, iface->history.lines[i], strlen(iface->history.lines[i])));
  }
  return arr;
}

static void emit_history_event(struct js *js, rl_interface_t *iface) {
  jsval_t history_arr = get_history_array(js, iface);
  emit_event(js, iface, "history", &history_arr, 1);
}

static void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf);
static void on_stdin_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);

#ifndef _WIN32
static void enter_raw_mode(rl_interface_t *iface) {
  if (iface->raw_mode) return;
  
  struct termios raw;
  if (tcgetattr(STDIN_FILENO, &iface->saved_termios) == -1) return;
  
  raw = iface->saved_termios;
  raw.c_lflag &= ~(ICANON | ECHO | ISIG);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  
  if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == -1) return;
  iface->raw_mode = true;
}

static void exit_raw_mode(rl_interface_t *iface) {
  if (!iface->raw_mode) return;
  tcsetattr(STDIN_FILENO, TCSANOW, &iface->saved_termios);
  iface->raw_mode = false;
}
#endif

static void write_output(rl_interface_t *iface, const char *str) {
  (void)iface;
  printf("%s", str);
  fflush(stdout);
}

static int get_terminal_cols(void) {
  int cols = 80;
#ifndef _WIN32
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
    cols = ws.ws_col;
  }
#else
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
    cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
  }
#endif
  return cols > 0 ? cols : 80;
}

static void move_cursor_to_line_start(rl_interface_t *iface, int cols) {
  int prompt_len = (int)strlen(iface->prompt);
  int cursor_cols = prompt_len + iface->line_pos;
  int cursor_row = cursor_cols / cols;
  if (cursor_cols > 0 && cursor_cols % cols == 0) cursor_row--;

  if (cursor_row > 0) {
    char move_buf[32];
    snprintf(move_buf, sizeof(move_buf), "\033[%dA", cursor_row);
    write_output(iface, move_buf);
  }
  write_output(iface, "\r");
}

static void clear_line_display(rl_interface_t *iface) {
  int cols = get_terminal_cols();
  int prompt_len = (int)strlen(iface->prompt);
  int line_cols = prompt_len + iface->line_len;
  int current_rows = line_cols > 0 ? (line_cols - 1) / cols + 1 : 1;
  int rows = iface->last_render_rows > current_rows ? iface->last_render_rows : current_rows;

  move_cursor_to_line_start(iface, cols);
  for (int i = 0; i < rows; i++) {
    write_output(iface, "\033[K");
    if (i < rows - 1) {
      write_output(iface, "\033[B\r");
    }
  }
  for (int i = 0; i < rows - 1; i++) {
    write_output(iface, "\033[A");
  }
  write_output(iface, "\r");
}

static void refresh_line(rl_interface_t *iface) {
  char buf[MAX_LINE_LENGTH + 256];
  int cols = get_terminal_cols();

  clear_line_display(iface);
  snprintf(buf, sizeof(buf), "%s%s", iface->prompt, iface->line_buffer);
  write_output(iface, buf);

  int prompt_len = (int)strlen(iface->prompt);
  int end_cols = prompt_len + iface->line_len;
  int end_row = end_cols > 0 ? end_cols / cols : 0;
  int cursor_cols = prompt_len + iface->line_pos;
  int cursor_row = cursor_cols > 0 ? cursor_cols / cols : 0;
  int cursor_col = cursor_cols > 0 ? cursor_cols % cols : 0;
  int up_rows = end_row - cursor_row;

  if (up_rows > 0) {
    char move_buf[32];
    snprintf(move_buf, sizeof(move_buf), "\033[%dA", up_rows);
    write_output(iface, move_buf);
  }
  write_output(iface, "\r");
  if (cursor_col > 0) {
    char move_buf[32];
    snprintf(move_buf, sizeof(move_buf), "\033[%dC", cursor_col);
    write_output(iface, move_buf);
  }

  iface->last_render_rows = end_cols > 0 ? end_cols / cols + 1 : 1;
}

static rl_interface_t *find_interface_by_id(uint64_t id) {
  rl_interface_t *iface = NULL;
  HASH_FIND(hh, interfaces, &id, sizeof(uint64_t), iface);
  return iface;
}

static void handle_history_up(rl_interface_t *iface) {
  const char *hist_line = rl_history_prev(&iface->history);
  if (hist_line) {
    strcpy(iface->line_buffer, hist_line);
    iface->line_len = (int)strlen(iface->line_buffer);
    iface->line_pos = iface->line_len;
    refresh_line(iface);
  }
}

static void handle_history_down(rl_interface_t *iface) {
  const char *hist_line = rl_history_next(&iface->history);
  if (hist_line) {
    strcpy(iface->line_buffer, hist_line);
    iface->line_len = (int)strlen(iface->line_buffer);
    iface->line_pos = iface->line_len;
    refresh_line(iface);
  }
}

static void handle_char_input(rl_interface_t *iface, char c) {
  if (iface->line_len < MAX_LINE_LENGTH - 1) {
    memmove(iface->line_buffer + iface->line_pos + 1,
            iface->line_buffer + iface->line_pos,
            iface->line_len - iface->line_pos + 1);
    iface->line_buffer[iface->line_pos] = c;
    iface->line_pos++;
    iface->line_len++;
    
    if (iface->line_pos == iface->line_len) {
      printf("%c", c);
      fflush(stdout);
    } else {
      refresh_line(iface);
    }
  }
}

static void handle_backspace(rl_interface_t *iface) {
  if (iface->line_pos > 0) {
    memmove(
      iface->line_buffer + iface->line_pos - 1,
      iface->line_buffer + iface->line_pos,
      iface->line_len - iface->line_pos + 1
    );
    iface->line_pos--;
    iface->line_len--;
    refresh_line(iface);
  }
}

static void handle_delete(rl_interface_t *iface) {
  if (iface->line_pos < iface->line_len) {
    memmove(
      iface->line_buffer + iface->line_pos,
      iface->line_buffer + iface->line_pos + 1,
      iface->line_len - iface->line_pos
    );
    iface->line_len--;
    refresh_line(iface);
  }
}

static void handle_escape_sequence(rl_interface_t *iface, const char *seq, int len) {
  if (len >= 2 && seq[0] == '[') {
    switch (seq[1]) {
      case 'A': handle_history_up(iface); break;
      case 'B': handle_history_down(iface); break;
      case 'C': 
        if (iface->line_pos < iface->line_len) {
          iface->line_pos++;
          printf("\033[C");
          fflush(stdout);
        }
        break;
      case 'D':
        if (iface->line_pos > 0) {
          iface->line_pos--;
          printf("\033[D");
          fflush(stdout);
        }
        break;
      case 'H':
        iface->line_pos = 0;
        refresh_line(iface);
        break;
      case 'F':
        iface->line_pos = iface->line_len;
        refresh_line(iface);
        break;
      case '3':
        if (len >= 3 && seq[2] == '~') {
          handle_delete(iface);
        }
        break;
    }
  }
}

static void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  (void)handle;
  buf->base = malloc(suggested_size);
  buf->len = suggested_size;
}

static void on_stdin_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  rl_interface_t *iface = (rl_interface_t *)stream->data;
  struct js *js = rt->js;

  if (!iface) {
    if (buf->base) free(buf->base);
    return;
  }

  if (iface->closed || iface->paused) {
    if (buf->base) free(buf->base);
    return;
  }
  
  if (nread < 0) {
    if (nread == UV_EOF) {
      emit_event(js, iface, "close", NULL, 0);
      iface->closed = true;
    }
    if (buf->base) free(buf->base);
    return;
  }
  
  for (ssize_t i = 0; i < nread; i++) {
    char c = buf->base[i];
    
    if (iface->escape_state > 0) {
      iface->escape_buf[iface->escape_len++] = c;
      
      if (iface->escape_state == 1) {
        if (c == '[' || c == 'O') {
          iface->escape_state = 2;
        } else {
          iface->escape_state = 0;
          iface->escape_len = 0;
        }
      } else if (iface->escape_state == 2) {
        if ((c >= 'A' && c <= 'Z') || c == '~') {
          handle_escape_sequence(iface, iface->escape_buf, iface->escape_len);
          iface->escape_state = 0;
          iface->escape_len = 0;
        } else if (iface->escape_len >= 15) {
          iface->escape_state = 0;
          iface->escape_len = 0;
        }
      }
      continue;
    }
    
    if (c == 27) {
      iface->escape_state = 1;
      iface->escape_len = 0;
      continue;
    }
    
    if (c == '\r' || c == '\n') {
      printf("\n");
      fflush(stdout);
      
      char *line = strdup(iface->line_buffer);
      rl_history_add(&iface->history, line, iface->remove_history_duplicates);
      emit_history_event(js, iface);
      
      jsval_t line_val = js_mkstr(js, line, strlen(line));
      emit_event(js, iface, "line", &line_val, 1);
      
      if (vtype(iface->pending_question_resolve) == T_FUNC) {
        js_call(js, iface->pending_question_resolve, &line_val, 1);
        iface->pending_question_resolve = js_mkundef();
        iface->pending_question_reject = js_mkundef();
      }
      
      iface->line_buffer[0] = '\0';
      iface->line_pos = 0;
      iface->line_len = 0;
      iface->history.current = iface->history.count;
      
      free(line);
    } else if (c == 127 || c == 8) {
      handle_backspace(iface);
    } else if (c == 3) {
      emit_event(js, iface, "SIGINT", NULL, 0);
    } else if (c == 4) {
      if (iface->line_len == 0) {
        emit_event(js, iface, "close", NULL, 0);
        iface->closed = true;
        uv_read_stop(stream);
      } else {
        handle_delete(iface);
      }
    } else if (c == 1) {
      iface->line_pos = 0;
      refresh_line(iface);
    } else if (c == 5) {
      iface->line_pos = iface->line_len;
      refresh_line(iface);
    } else if (c == 11) {
      iface->line_buffer[iface->line_pos] = '\0';
      iface->line_len = iface->line_pos;
      refresh_line(iface);
    } else if (c == 21) {
      iface->line_buffer[0] = '\0';
      iface->line_pos = 0;
      iface->line_len = 0;
      refresh_line(iface);
    } else if (c == 12) {
      printf("\033[2J\033[H");
      refresh_line(iface);
    } else if (c >= 32 && c < 127) {
      handle_char_input(iface, c);
    }
  }
  
  if (buf->base) free(buf->base);
}

static void start_reading(rl_interface_t *iface) {
  if (iface->reading || iface->closed) return;
  
  if (!iface->tty_initialized) {
    uv_loop_t *loop = uv_default_loop();
    
    int is_tty = uv_guess_handle(STDIN_FILENO) == UV_TTY;
    
    if (is_tty) {
      if (uv_tty_init(loop, &iface->tty_in, STDIN_FILENO, 1) != 0) {
        return;
      }
      uv_tty_set_mode(&iface->tty_in, UV_TTY_MODE_RAW);
    } else {
      if (uv_tty_init(loop, &iface->tty_in, STDIN_FILENO, 1) != 0) {
        return;
      }
    }
    
    iface->tty_in.data = iface;
    iface->tty_initialized = true;
  }
  
  iface->reading = true;
  uv_read_start((uv_stream_t *)&iface->tty_in, alloc_buffer, on_stdin_read);
}

static void stop_reading(rl_interface_t *iface) {
  if (!iface->reading) return;
  
  uv_read_stop((uv_stream_t *)&iface->tty_in);
  iface->reading = false;
}

static void process_line(struct js *js, rl_interface_t *iface) {
  char *line = strdup(iface->line_buffer);
  
  rl_history_add(&iface->history, line, iface->remove_history_duplicates);
  emit_history_event(js, iface);
  
  jsval_t line_val = js_mkstr(js, line, strlen(line));
  emit_event(js, iface, "line", &line_val, 1);
  
  if (vtype(iface->pending_question_resolve) == T_FUNC) {
    js_call(js, iface->pending_question_resolve, &line_val, 1);
    iface->pending_question_resolve = js_mkundef();
    iface->pending_question_reject = js_mkundef();
  }
  
  iface->line_buffer[0] = '\0';
  iface->line_pos = 0;
  iface->line_len = 0;
  
  free(line);
}

static rl_interface_t *get_interface(struct js *js, jsval_t this_obj) {
  jsval_t id_val = js_get(js, this_obj, "_rl_id");
  if (vtype(id_val) != T_NUM) return NULL;
  
  uint64_t id = (uint64_t)js_getnum(id_val);
  rl_interface_t *iface = NULL;
  HASH_FIND(hh, interfaces, &id, sizeof(uint64_t), iface);
  return iface;
}

static jsval_t rl_interface_on(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  
  if (!iface) return js_mkerr(js, "Invalid Interface");
  if (nargs < 2) return js_mkerr(js, "on requires 2 arguments");
  
  char *event = js_getstr(js, args[0], NULL);
  if (!event) return js_mkerr(js, "event must be a string");
  int t = vtype(args[1]);
  if (t != T_FUNC && t != T_CFUNC) return js_mkerr(js, "listener must be a function");
  
  RLEventType *evt = find_or_create_event_type(iface, event);
  if (evt->listener_count >= MAX_LISTENERS_PER_EVENT) {
    return js_mkerr(js, "maximum listeners reached");
  }
  
  evt->listeners[evt->listener_count].listener = args[1];
  evt->listeners[evt->listener_count].once = false;
  evt->listener_count++;
  
  return this_obj;
}

static jsval_t rl_interface_once(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  
  if (!iface) return js_mkerr(js, "Invalid Interface");
  if (nargs < 2) return js_mkerr(js, "once requires 2 arguments");
  
  char *event = js_getstr(js, args[0], NULL);
  if (!event) return js_mkerr(js, "event must be a string");
  int t = vtype(args[1]);
  if (t != T_FUNC && t != T_CFUNC) return js_mkerr(js, "listener must be a function");
  
  RLEventType *evt = find_or_create_event_type(iface, event);
  if (evt->listener_count >= MAX_LISTENERS_PER_EVENT) {
    return js_mkerr(js, "maximum listeners reached");
  }
  
  evt->listeners[evt->listener_count].listener = args[1];
  evt->listeners[evt->listener_count].once = true;
  evt->listener_count++;
  
  return this_obj;
}

static jsval_t rl_interface_off(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  
  if (!iface) return js_mkerr(js, "Invalid Interface");
  if (nargs < 2) return this_obj;
  
  char *event = js_getstr(js, args[0], NULL);
  if (!event) return this_obj;
  
  RLEventType *evt = NULL;
  HASH_FIND_STR(iface->events, event, evt);
  if (!evt) return this_obj;
  
  for (int i = 0; i < evt->listener_count; i++) {
    if (evt->listeners[i].listener == args[1]) {
      for (int j = i; j < evt->listener_count - 1; j++) {
        evt->listeners[j] = evt->listeners[j + 1];
      }
      evt->listener_count--;
      break;
    }
  }
  
  return this_obj;
}

static jsval_t rl_interface_emit(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  
  if (!iface) return js_mkerr(js, "Invalid Interface");
  if (nargs < 1) return js_mkfalse();
  
  char *event = js_getstr(js, args[0], NULL);
  if (!event) return js_mkfalse();
  
  emit_event(js, iface, event, nargs > 1 ? &args[1] : NULL, nargs - 1);
  return js_mktrue();
}

static jsval_t rl_interface_close(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  
  if (!iface || iface->closed) return js_mkundef();
  
  stop_reading(iface);
  
  if (iface->tty_initialized) {
    uv_tty_reset_mode();
    uv_close((uv_handle_t *)&iface->tty_in, NULL);
    iface->tty_initialized = false;
  }
  
#ifndef _WIN32
  exit_raw_mode(iface);
#endif
  
  iface->closed = true;
  emit_event(js, iface, "close", NULL, 0);
  
  return js_mkundef();
}

static jsval_t rl_interface_pause(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  
  if (!iface) return js_mkerr(js, "Invalid Interface");
  
  if (!iface->paused) {
    iface->paused = true;
    stop_reading(iface);
    emit_event(js, iface, "pause", NULL, 0);
  }
  
  return this_obj;
}

static jsval_t rl_interface_resume(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  
  if (!iface) return js_mkerr(js, "Invalid Interface");
  
  if (iface->paused) {
    iface->paused = false;
    start_reading(iface);
    emit_event(js, iface, "resume", NULL, 0);
  }
  
  return this_obj;
}

static jsval_t rl_interface_prompt(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  
  if (!iface || iface->closed) return js_mkundef();
  
  if (iface->paused) {
    iface->paused = false;
    emit_event(js, iface, "resume", NULL, 0);
  }
  
  bool preserve_cursor = false;
  if (nargs > 0) preserve_cursor = js_truthy(js, args[0]);
  
  if (!preserve_cursor) {
    iface->line_buffer[0] = '\0';
    iface->line_pos = 0;
    iface->line_len = 0;
  }
  
  write_output(iface, iface->prompt);
  if (iface->line_len > 0) {
    write_output(iface, iface->line_buffer);
  }
  
  start_reading(iface);
  
  return js_mkundef();
}

static jsval_t rl_interface_set_prompt(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  
  if (!iface) return js_mkerr(js, "Invalid Interface");
  if (nargs < 1) return js_mkundef();
  
  char *new_prompt = js_getstr(js, args[0], NULL);
  if (new_prompt) {
    free(iface->prompt);
    iface->prompt = strdup(new_prompt);
  }
  
  return js_mkundef();
}

static jsval_t rl_interface_get_prompt(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  
  if (!iface) return js_mkerr(js, "Invalid Interface");
  
  return js_mkstr(js, iface->prompt, strlen(iface->prompt));
}

static void process_key_sequence(rl_interface_t *iface, const char *name, bool ctrl, bool meta, bool shift) {
  (void)meta; (void)shift;
  
  if (!name) return;
  
  if (strcmp(name, "return") == 0 || strcmp(name, "enter") == 0) {
    printf("\n");
    fflush(stdout);
  } else if (strcmp(name, "backspace") == 0) {
    if (iface->line_pos > 0) {
      memmove(iface->line_buffer + iface->line_pos - 1,
              iface->line_buffer + iface->line_pos,
              iface->line_len - iface->line_pos + 1);
      iface->line_pos--;
      iface->line_len--;
    }
  } else if (strcmp(name, "delete") == 0) {
    if (iface->line_pos < iface->line_len) {
      memmove(iface->line_buffer + iface->line_pos,
              iface->line_buffer + iface->line_pos + 1,
              iface->line_len - iface->line_pos);
      iface->line_len--;
    }
  } else if (strcmp(name, "left") == 0) {
    if (iface->line_pos > 0) iface->line_pos--;
  } else if (strcmp(name, "right") == 0) {
    if (iface->line_pos < iface->line_len) iface->line_pos++;
  } else if (strcmp(name, "home") == 0 || (ctrl && strcmp(name, "a") == 0)) {
    iface->line_pos = 0;
  } else if (strcmp(name, "end") == 0 || (ctrl && strcmp(name, "e") == 0)) {
    iface->line_pos = iface->line_len;
  } else if (ctrl && strcmp(name, "u") == 0) {
    iface->line_buffer[0] = '\0';
    iface->line_pos = 0;
    iface->line_len = 0;
  } else if (ctrl && strcmp(name, "k") == 0) {
    iface->line_buffer[iface->line_pos] = '\0';
    iface->line_len = iface->line_pos;
  }
}

static jsval_t rl_interface_write(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  
  if (!iface || iface->closed) return js_mkundef();
  
  if (iface->paused) {
    iface->paused = false;
    emit_event(js, iface, "resume", NULL, 0);
  }
  
  if (nargs >= 2 && is_special_object(args[1])) {
    jsval_t key = args[1];
    jsval_t name_val = js_get(js, key, "name");
    jsval_t ctrl_val = js_get(js, key, "ctrl");
    jsval_t meta_val = js_get(js, key, "meta");
    jsval_t shift_val = js_get(js, key, "shift");
    
    char *name = (vtype(name_val) == T_STR) ? js_getstr(js, name_val, NULL) : NULL;
    bool ctrl = js_truthy(js, ctrl_val);
    bool meta = js_truthy(js, meta_val);
    bool shift = js_truthy(js, shift_val);
    
    if (name) {
      process_key_sequence(iface, name, ctrl, meta, shift);
      return js_mkundef();
    }
  }
  
  if (nargs < 1 || vtype(args[0]) == T_NULL || vtype(args[0]) == T_UNDEF) {
    return js_mkundef();
  }
  
  size_t len;
  char *data = js_getstr(js, args[0], &len);
  if (!data) return js_mkundef();
  
  for (size_t i = 0; i < len && iface->line_len < MAX_LINE_LENGTH - 1; i++) {
    char c = data[i];
    
    if (c == '\n' || c == '\r') {
      process_line(js, iface);
    } else {
      memmove(iface->line_buffer + iface->line_pos + 1, 
              iface->line_buffer + iface->line_pos, 
              iface->line_len - iface->line_pos + 1);
      iface->line_buffer[iface->line_pos] = c;
      iface->line_pos++;
      iface->line_len++;
    }
  }
  
  return js_mkundef();
}

static jsval_t rl_interface_line_getter(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  
  if (!iface) return js_mkundef();
  return js_mkstr(js, iface->line_buffer, strlen(iface->line_buffer));
}

static jsval_t rl_interface_cursor_getter(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  
  if (!iface) return js_mknum(0);
  return js_mknum((double)iface->line_pos);
}

static jsval_t rl_interface_question_callback(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  
  if (!iface || iface->closed) return js_mkundef();
  if (nargs < 2) return js_mkerr(js, "question requires query and callback");
  
  size_t query_len;
  char *query = js_getstr(js, args[0], &query_len);
  if (!query) return js_mkerr(js, "query must be a string");
  
  int t = vtype(args[1]);
  if (t != T_FUNC && t != T_CFUNC) {
    return js_mkerr(js, "callback must be a function");
  }
  
  write_output(iface, query);
  
  RLEventType *evt = find_or_create_event_type(iface, "line");
  if (evt->listener_count >= MAX_LISTENERS_PER_EVENT) {
    return js_mkerr(js, "maximum listeners reached");
  }
  
  evt->listeners[evt->listener_count].listener = args[1];
  evt->listeners[evt->listener_count].once = true;
  evt->listener_count++;
  
  return js_mkundef();
}

static jsval_t rl_interface_question_promise(struct js *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  
  if (!iface || iface->closed) return js_mkerr(js, "Interface is closed");
  if (nargs < 1) return js_mkerr(js, "question requires a query string");
  
  size_t query_len;
  char *query = js_getstr(js, args[0], &query_len);
  if (!query) return js_mkerr(js, "query must be a string");
  
  jsval_t promise = js_mkpromise(js);
  
  write_output(iface, query);
  
  iface->pending_question_resolve = js_get(js, promise, "_resolve");
  iface->pending_question_reject = js_get(js, promise, "_reject");
  
  return promise;
}

static jsval_t rl_interface_get_cursor_pos(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  
  if (!iface) {
    jsval_t result = js_mkobj(js);
    js_set(js, result, "rows", js_mknum(0));
    js_set(js, result, "cols", js_mknum(0));
    return result;
  }
  
  int prompt_len = (int)strlen(iface->prompt);
  int total_cols = prompt_len + iface->line_pos;
  
  int cols = 80;
#ifndef _WIN32
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
    cols = ws.ws_col;
  }
#else
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
    cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
  }
#endif
  
  int rows = total_cols / cols;
  int col_pos = total_cols % cols;
  
  jsval_t result = js_mkobj(js);
  js_set(js, result, "rows", js_mknum((double)rows));
  js_set(js, result, "cols", js_mknum((double)col_pos));
  return result;
}

static jsval_t rl_interface_closed_getter(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  
  if (!iface) return js_mktrue();
  return iface->closed ? js_mktrue() : js_mkfalse();
}

static jsval_t rl_interface_async_iterator(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  
  if (!iface) return js_mkerr(js, "Invalid Interface");
  
  jsval_t iterator = js_mkobj(js);
  js_set(js, iterator, "_rl_id", js_mknum((double)iface->id));
  js_set(js, iterator, "_lines", js_mkarr(js));
  js_set(js, iterator, "_done", js_mkfalse());
  
  return iterator;
}

static void free_interface(rl_interface_t *iface) {
  if (!iface) return;
  
  HASH_DEL(interfaces, iface);
  
  free(iface->prompt);
  free(iface->line_buffer);
  rl_history_free(&iface->history);
  
  RLEventType *evt, *tmp;
  HASH_ITER(hh, iface->events, evt, tmp) {
    HASH_DEL(iface->events, evt);
    free(evt->event_type);
    free(evt);
  }
  
  free(iface);
}

static jsval_t rl_create_interface(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "createInterface requires options");
  
  jsval_t options = args[0];
  if (!is_special_object(options)) return js_mkerr(js, "options must be an object");
  
  rl_interface_t *iface = calloc(1, sizeof(rl_interface_t));
  if (!iface) return js_mkerr(js, "out of memory");
  
  iface->id = next_interface_id++;
  iface->prompt = strdup(DEFAULT_PROMPT);
  iface->line_buffer = calloc(MAX_LINE_LENGTH, 1);
  iface->line_pos = 0;
  iface->line_len = 0;
  iface->paused = false;
  iface->closed = false;
  iface->reading = false;
  iface->pending_question_resolve = js_mkundef();
  iface->pending_question_reject = js_mkundef();
  iface->events = NULL;
  iface->tty_initialized = false;
  iface->escape_state = 0;
  iface->escape_len = 0;
  iface->last_render_rows = 1;
  iface->js_obj = js_mkundef();
#ifndef _WIN32
  iface->raw_mode = false;
#endif
  
  iface->input_stream = js_get(js, options, "input");
  iface->output_stream = js_get(js, options, "output");
  
  jsval_t terminal_val = js_get(js, options, "terminal");
  iface->terminal = terminal_val == js_true || vtype(terminal_val) == T_UNDEF;
  
  jsval_t history_size_val = js_get(js, options, "historySize");
  iface->history_size = (vtype(history_size_val) == T_NUM) 
    ? (int)js_getnum(history_size_val) 
    : DEFAULT_HISTORY_SIZE;
  
  jsval_t remove_dup_val = js_get(js, options, "removeHistoryDuplicates");
  iface->remove_history_duplicates = js_truthy(js, remove_dup_val);
  
  jsval_t prompt_val = js_get(js, options, "prompt");
  if (vtype(prompt_val) == T_STR) {
    free(iface->prompt);
    iface->prompt = strdup(js_getstr(js, prompt_val, NULL));
  }
  
  jsval_t crlf_delay_val = js_get(js, options, "crlfDelay");
  iface->crlf_delay = (vtype(crlf_delay_val) == T_NUM) 
    ? (int)js_getnum(crlf_delay_val) 
    : 100;
  if (iface->crlf_delay < 100) iface->crlf_delay = 100;
  
  jsval_t tab_size_val = js_get(js, options, "tabSize");
  iface->tab_size = (vtype(tab_size_val) == T_NUM) 
    ? (int)js_getnum(tab_size_val) 
    : DEFAULT_TAB_SIZE;
  if (iface->tab_size < 1) iface->tab_size = 1;
  
  jsval_t completer_val = js_get(js, options, "completer");
  iface->completer = (vtype(completer_val) == T_FUNC) ? completer_val : js_mkundef();
  
  jsval_t history_val = js_get(js, options, "history");
  if (is_special_object(history_val)) {
    jsval_t len_val = js_get(js, history_val, "length");
    int len = (vtype(len_val) == T_NUM) ? (int)js_getnum(len_val) : 0;
    
    rl_history_init(&iface->history, iface->history_size);
    
    for (int i = 0; i < len; i++) {
      char key[16];
      snprintf(key, sizeof(key), "%d", i);
      jsval_t item = js_get(js, history_val, key);
      if (vtype(item) == T_STR) {
        char *line = js_getstr(js, item, NULL);
        if (line) rl_history_add(&iface->history, line, false);
      }
    }
  } else {
    rl_history_init(&iface->history, iface->history_size);
  }
  
  HASH_ADD(hh, interfaces, id, sizeof(uint64_t), iface);
  
  jsval_t obj = js_mkobj(js);
  js_set(js, obj, "_rl_id", js_mknum((double)iface->id));
  
  js_set(js, obj, "on", js_mkfun(rl_interface_on));
  js_set(js, obj, "once", js_mkfun(rl_interface_once));
  js_set(js, obj, "off", js_mkfun(rl_interface_off));
  js_set(js, obj, "addListener", js_mkfun(rl_interface_on));
  js_set(js, obj, "removeListener", js_mkfun(rl_interface_off));
  js_set(js, obj, "emit", js_mkfun(rl_interface_emit));
  
  js_set(js, obj, "close", js_mkfun(rl_interface_close));
  js_set(js, obj, "pause", js_mkfun(rl_interface_pause));
  js_set(js, obj, "resume", js_mkfun(rl_interface_resume));
  js_set(js, obj, "prompt", js_mkfun(rl_interface_prompt));
  js_set(js, obj, "setPrompt", js_mkfun(rl_interface_set_prompt));
  js_set(js, obj, "getPrompt", js_mkfun(rl_interface_get_prompt));
  js_set(js, obj, "write", js_mkfun(rl_interface_write));
  js_set(js, obj, "question", js_mkfun(rl_interface_question_callback));
  js_set(js, obj, "getCursorPos", js_mkfun(rl_interface_get_cursor_pos));
  
  js_set_getter_desc(js, obj, "line", 4, js_mkfun(rl_interface_line_getter), JS_DESC_E | JS_DESC_C);
  js_set_getter_desc(js, obj, "cursor", 6, js_mkfun(rl_interface_cursor_getter), JS_DESC_E | JS_DESC_C);
  js_set_getter_desc(js, obj, "closed", 6, js_mkfun(rl_interface_closed_getter), JS_DESC_E | JS_DESC_C);
  
  js_set(js, obj, "terminal", iface->terminal ? js_mktrue() : js_mkfalse());
  js_set(js, obj, get_asyncIterator_sym_key(), js_mkfun(rl_interface_async_iterator));
  js_set(js, obj, get_toStringTag_sym_key(), js_mkstr(js, "Interface", 9));
  
  return obj;
}

static jsval_t rl_create_interface_promises(struct js *js, jsval_t *args, int nargs) {
  jsval_t iface_obj = rl_create_interface(js, args, nargs);
  if (vtype(iface_obj) == T_ERR) return iface_obj;
  js_set(js, iface_obj, "question", js_mkfun(rl_interface_question_promise));
  
  return iface_obj;
}

static jsval_t rl_clear_line(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkfalse();
  int dir = (int)js_getnum(args[1]);
  
  const char *seq;
  switch (dir) {
    case -1: seq = "\033[1K"; break;
    case 1:  seq = "\033[0K"; break;
    case 0:
    default: seq = "\033[2K\r"; break;
  }
  
  printf("%s", seq);
  fflush(stdout);
  
  return js_mktrue();
}

static jsval_t rl_clear_screen_down(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  
  printf("\033[J");
  fflush(stdout);
  
  return js_mktrue();
}

static jsval_t rl_cursor_to(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkfalse();
  int x = (int)js_getnum(args[1]);
  
  if (nargs >= 3 && vtype(args[2]) == T_NUM) {
    int y = (int)js_getnum(args[2]);
    printf("\033[%d;%dH", y + 1, x + 1);
  } else {
    printf("\033[%dG", x + 1);
  }
  fflush(stdout);
  
  return js_mktrue();
}

static jsval_t rl_move_cursor(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 3) return js_mkfalse();
  
  int dx = (int)js_getnum(args[1]);
  int dy = (int)js_getnum(args[2]);
  
  if (dx > 0) printf("\033[%dC", dx);
  else if (dx < 0) printf("\033[%dD", -dx);
  
  if (dy > 0) printf("\033[%dB", dy);
  else if (dy < 0) printf("\033[%dA", -dy);
  
  fflush(stdout);
  return js_mktrue();
}

static jsval_t rl_emit_keypress_events(struct js *js, jsval_t *args, int nargs) {
  if (nargs > 0) {
    jsval_t stdin_obj = js_get(js, js_get(js, js_glob(js), "process"), "stdin");
    if (stdin_obj != args[0]) {
      return js_mkerr(js, "emitKeypressEvents only supports process.stdin");
    }
  }
  process_enable_keypress_events();
  return js_mkundef();
}

bool has_active_readline_interfaces(void) {
  rl_interface_t *iface, *tmp;
  HASH_ITER(hh, interfaces, iface, tmp) {
    if (!iface->closed && iface->reading) return true;
  }
  return false;
}

void readline_gc_update_roots(GC_OP_VAL_ARGS) {
  rl_interface_t *iface, *tmp;
  HASH_ITER(hh, interfaces, iface, tmp) {
    op_val(ctx, &iface->input_stream);
    op_val(ctx, &iface->output_stream);
    op_val(ctx, &iface->completer);
    op_val(ctx, &iface->js_obj);
    op_val(ctx, &iface->pending_question_resolve);
    op_val(ctx, &iface->pending_question_reject);
    
    RLEventType *evt, *evt_tmp;
    HASH_ITER(hh, iface->events, evt, evt_tmp) {
      for (int i = 0; i < evt->listener_count; i++) op_val(ctx, &evt->listeners[i].listener);
    }
  }
}

jsval_t readline_library(struct js *js) {
  jsval_t lib = js_mkobj(js);

  js_set(js, lib, "createInterface", js_mkfun(rl_create_interface));
  js_set(js, lib, "clearLine", js_mkfun(rl_clear_line));
  js_set(js, lib, "clearScreenDown", js_mkfun(rl_clear_screen_down));
  js_set(js, lib, "cursorTo", js_mkfun(rl_cursor_to));
  js_set(js, lib, "moveCursor", js_mkfun(rl_move_cursor));
  js_set(js, lib, "emitKeypressEvents", js_mkfun(rl_emit_keypress_events));
  js_set(js, lib, get_toStringTag_sym_key(), js_mkstr(js, "readline", 8));
  
  return lib;
}

jsval_t readline_promises_library(struct js *js) {
  jsval_t lib = js_mkobj(js);

  js_set(js, lib, "createInterface", js_mkfun(rl_create_interface_promises));
  js_set(js, lib, "clearLine", js_mkfun(rl_clear_line));
  js_set(js, lib, "clearScreenDown", js_mkfun(rl_clear_screen_down));
  js_set(js, lib, "cursorTo", js_mkfun(rl_cursor_to));
  js_set(js, lib, "moveCursor", js_mkfun(rl_move_cursor));
  js_set(js, lib, "emitKeypressEvents", js_mkfun(rl_emit_keypress_events));
  js_set(js, lib, get_toStringTag_sym_key(), js_mkstr(js, "readline/promises", 17));
  
  return lib;
}
