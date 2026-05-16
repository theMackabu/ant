// TODO: cleanup module, make cleaner

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
#define WIN32_LEAN_AND_MEAN
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
#include "descriptors.h"
#include "tty_ctrl.h"
#include "silver/engine.h"

#include "gc/modules.h"
#include "modules/events.h"
#include "modules/readline.h"
#include "modules/process.h"
#include "modules/symbol.h"

#define MAX_LINE_LENGTH 4096
#define MAX_HISTORY 1000
#define DEFAULT_PROMPT "> "
#define DEFAULT_HISTORY_SIZE 30
#define DEFAULT_TAB_SIZE 8

typedef struct {
  char **lines;
  int count;
  int capacity;
  int current;
} rl_history_t;

typedef struct rl_interface {
  uint64_t id;
  ant_value_t input_stream;
  ant_value_t output_stream;
  ant_value_t completer;
  ant_value_t js_obj;
  char *prompt;
  char *active_prompt;
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
  ant_value_t pending_question_resolve;
  ant_value_t pending_question_reject;
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
  uv_signal_t sigint_watcher;
  bool sigint_watcher_active;
#endif
} rl_interface_t;

static uint64_t next_interface_id = 1;
static rl_interface_t *interfaces = NULL;

static const char *rl_render_prompt(const rl_interface_t *iface) {
  if (!iface) return "";
  return iface->active_prompt ? iface->active_prompt : iface->prompt;
}

static void rl_set_active_prompt(rl_interface_t *iface, const char *prompt) {
  if (!iface) return;
  free(iface->active_prompt);
  iface->active_prompt = prompt ? strdup(prompt) : NULL;
}

static void rl_clear_active_prompt(rl_interface_t *iface) {
  if (!iface) return;
  free(iface->active_prompt);
  iface->active_prompt = NULL;
}

static void rl_history_init(rl_history_t *hist, int capacity) {
  hist->capacity = capacity > 0 ? capacity : DEFAULT_HISTORY_SIZE;
  hist->lines = calloc(hist->capacity, sizeof(char*));
  hist->count = 0;
  hist->current = -1;
}

static void rl_history_remove_at(rl_history_t *hist, int index) {
  if (!hist || index < 0 || index >= hist->count) return;

  free(hist->lines[index]);
  if (index < hist->count - 1) memmove(
    hist->lines + index,
    hist->lines + index + 1,
    sizeof(char *) * (size_t)(hist->count - index - 1)
  );
  
  hist->count--;
}

static void rl_history_add(rl_history_t *hist, const char *line, bool remove_duplicates) {
  int duplicate_index = -1;

  if (!line || line[0] == '\0') return;
  if (!remove_duplicates && hist->count > 0 && strcmp(hist->lines[hist->count - 1], line) == 0) return;

  if (remove_duplicates) {
  for (int i = 0; i < hist->count; i++) {
    if (strcmp(hist->lines[i], line) != 0) continue;
    duplicate_index = i;
    break;
  }}

  if (duplicate_index >= 0) rl_history_remove_at(hist, duplicate_index);
  if (hist->count >= hist->capacity) rl_history_remove_at(hist, 0);

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

#ifndef _WIN32
static void enter_raw_mode(rl_interface_t *iface) {
  if (iface->raw_mode) return;
  
  struct termios raw;
  if (tcgetattr(STDIN_FILENO, &iface->saved_termios) == -1) return;
  
  raw = iface->saved_termios;
  cfmakeraw(&raw);
  raw.c_lflag |= ISIG;
  raw.c_oflag |= OPOST | ONLCR;

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
  fputs(str, stdout);
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
  int prompt_len = (int)strlen(rl_render_prompt(iface));
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
  int prompt_len = (int)strlen(rl_render_prompt(iface));
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
  const char *prompt = rl_render_prompt(iface);

  clear_line_display(iface);
  snprintf(buf, sizeof(buf), "%s%s", prompt, iface->line_buffer);
  write_output(iface, buf);

  int prompt_len = (int)strlen(prompt);
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
    memmove(
      iface->line_buffer + iface->line_pos + 1,
      iface->line_buffer + iface->line_pos,
      iface->line_len - iface->line_pos + 1
    );
    
    iface->line_buffer[iface->line_pos] = c;
    iface->line_pos++;
    iface->line_len++;
    
    if (iface->line_pos == iface->line_len) {
      int cols = get_terminal_cols();
      int prompt_len = (int)strlen(rl_render_prompt(iface));
      int total_cols = prompt_len + iface->line_len;
      int rows = total_cols > 0 ? total_cols / cols + 1 : 1;
      if (rows > iface->last_render_rows) iface->last_render_rows = rows;
      printf("%c", c);
      fflush(stdout);
    } else refresh_line(iface);
  }
}

static void handle_backspace(rl_interface_t *iface) {
  if (iface->line_pos > 0) {
    if (iface->line_pos == iface->line_len) {
      iface->line_pos--;
      iface->line_len--;
      iface->line_buffer[iface->line_len] = '\0';
      write_output(iface, "\b \b");
      int cols = get_terminal_cols();
      int prompt_len = (int)strlen(rl_render_prompt(iface));
      int total_cols = prompt_len + iface->line_len;
      iface->last_render_rows = total_cols > 0 ? total_cols / cols + 1 : 1;
      return;
    }

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
  }}
}

static void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  buf->base = malloc(suggested_size);
  buf->len = suggested_size;
}

static void emit_event(ant_t *js, rl_interface_t *iface, const char *event_type, ant_value_t *args, int nargs) {
  if (!iface || vtype(iface->js_obj) == T_UNDEF) return;
  eventemitter_emit_args(js, iface->js_obj, event_type, args, nargs);
}

static ant_value_t get_history_array(ant_t *js, rl_interface_t *iface) {
  ant_value_t arr = js_mkarr(js);
  for (int i = 0; i < iface->history.count; i++) js_arr_push(
    js, arr, js_mkstr(js, iface->history.lines[i], strlen(iface->history.lines[i]))
  );
  return arr;
}

static void emit_history_event(ant_t *js, rl_interface_t *iface) {
  ant_value_t history_arr = get_history_array(js, iface);
  emit_event(js, iface, "history", &history_arr, 1);
}


static void stop_reading(rl_interface_t *iface) {
  if (!iface->reading) return;

  uv_read_stop((uv_stream_t *)&iface->tty_in);
  iface->reading = false;
#ifndef _WIN32
  if (iface->sigint_watcher_active) {
    uv_signal_stop(&iface->sigint_watcher);
    iface->sigint_watcher_active = false;
  }
#endif
}

static bool rl_has_event_listener(ant_t *js, rl_interface_t *iface, const char *event_type) {
  if (!iface || !event_type || vtype(iface->js_obj) == T_UNDEF) return false;
  return eventemitter_listener_count(js, iface->js_obj, event_type) > 0;
}

static bool rl_add_listener(ant_t *js, rl_interface_t *iface, const char *event_type, ant_value_t listener, bool once) {
  if (!iface || !event_type || vtype(iface->js_obj) == T_UNDEF) return false;
  return eventemitter_add_listener(js, iface->js_obj, event_type, listener, once);
}

static void rl_remove_listener(ant_t *js, rl_interface_t *iface, const char *event_type, ant_value_t listener) {
  if (!iface || !event_type || vtype(iface->js_obj) == T_UNDEF) return;
  eventemitter_remove_listener(js, iface->js_obj, event_type, listener);
}

static ant_value_t rl_async_iter_state(ant_t *js, ant_value_t iterator) {
  ant_value_t state = is_object_type(iterator) ? js_get_slot(iterator, SLOT_DATA) : js_mkundef();
  return is_object_type(state) ? state : js_mkundef();
}

static ant_value_t rl_async_iter_queue(ant_t *js, ant_value_t state, const char *queue_key) {
  ant_value_t queue = is_object_type(state) ? js_get(js, state, queue_key) : js_mkundef();
  return vtype(queue) == T_ARR ? queue : js_mkundef();
}

static ant_offset_t rl_async_iter_queue_head(ant_t *js, ant_value_t state, const char *head_key) {
  ant_value_t head = is_object_type(state) ? js_get(js, state, head_key) : js_mkundef();
  return vtype(head) == T_NUM ? (ant_offset_t)js_getnum(head) : 0;
}

static void rl_async_iter_set_queue_head(ant_t *js, ant_value_t state, const char *head_key, ant_offset_t head) {
  if (is_object_type(state)) js_set(js, state, head_key, js_mknum((double)head));
}

static void rl_close_interface(ant_t *js, rl_interface_t *iface) {
  if (!iface || iface->closed) return;
  stop_reading(iface);

  if (iface->tty_initialized) {
    uv_close((uv_handle_t *)&iface->tty_in, NULL);
#ifndef _WIN32
    if (!iface->sigint_watcher_active && uv_is_active((uv_handle_t *)&iface->sigint_watcher)) {
      uv_close((uv_handle_t *)&iface->sigint_watcher, NULL);
    }
    exit_raw_mode(iface);
#endif
    iface->tty_initialized = false;
  }

  iface->closed = true;
  emit_event(js, iface, "close", NULL, 0);
}

static void process_line(ant_t *js, rl_interface_t *iface) {
  char *line = strdup(iface->line_buffer);
  
  rl_history_add(&iface->history, line, iface->remove_history_duplicates);
  emit_history_event(js, iface);
  rl_clear_active_prompt(iface);
  
  ant_value_t line_val = js_mkstr(js, line, strlen(line));
  emit_event(js, iface, "line", &line_val, 1);
  
  if (vtype(iface->pending_question_resolve) == T_FUNC) {
    sv_vm_call(js->vm, js, iface->pending_question_resolve, js_mkundef(), &line_val, 1, NULL, false);
    iface->pending_question_resolve = js_mkundef();
    iface->pending_question_reject = js_mkundef();
  }
  
  iface->line_buffer[0] = '\0';
  iface->line_pos = 0;
  iface->line_len = 0;
  
  free(line);
}

static void feed_escape(rl_interface_t *iface, char c) {
  iface->escape_buf[iface->escape_len++] = c;
  if (iface->escape_state == 1) {
    iface->escape_state = (c == '[' || c == 'O') ? 2 : 0;
    if (!iface->escape_state) iface->escape_len = 0;
    return;
  }
  bool done = (c >= 'A' && c <= 'Z') || c == '~';
  if (done) handle_escape_sequence(iface, iface->escape_buf, iface->escape_len);
  if (done || iface->escape_len >= 15) { iface->escape_state = 0; iface->escape_len = 0; }
}

static void process_byte(ant_t *js, rl_interface_t *iface, char c) {
  if (iface->escape_state > 0) { feed_escape(iface, c); return; }
  if (c == 27) { iface->escape_state = 1; iface->escape_len = 0; return; }

  switch (c) {
    case '\r': case '\n':
      putchar('\n'); fflush(stdout);
      process_line(js, iface);
      break;
    case 127: case 8: handle_backspace(iface); break;
    case 4:
      if (iface->line_len == 0) rl_close_interface(js, iface);
      else handle_delete(iface);
      break;
    case 1:  iface->line_pos = 0; refresh_line(iface); break;
    case 5:  iface->line_pos = iface->line_len; refresh_line(iface); break;
    case 11: iface->line_buffer[iface->line_pos] = '\0'; iface->line_len = iface->line_pos; refresh_line(iface); break;
    case 21: iface->line_buffer[0] = '\0'; iface->line_pos = 0; iface->line_len = 0; refresh_line(iface); break;
    case 12: printf("\033[2J\033[H"); refresh_line(iface); break;
    default: if (c >= 32 && c < 127) handle_char_input(iface, c); break;
  }
}

static void on_stdin_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  rl_interface_t *iface = (rl_interface_t *)stream->data;
  ant_t *js = rt->js;

  if (!iface || iface->closed || iface->paused) goto cleanup;

  if (nread < 0) {
    if (nread == UV_EOF) rl_close_interface(js, iface);
    goto cleanup;
  }

  for (ssize_t i = 0; i < nread; i++) {
    process_byte(js, iface, buf->base[i]);
    if (iface->closed) break;
  }

  if (iface->closed) stop_reading(iface);

cleanup:
  free(buf->base);
}

#ifndef _WIN32
static void on_sigint(uv_signal_t *handle, int signum) {
  rl_interface_t *iface = (rl_interface_t *)handle->data;
  ant_t *js = rt->js;

  if (rl_has_event_listener(js, iface, "SIGINT")) {
    emit_event(js, iface, "SIGINT", NULL, 0);
  } else if (process_has_event_listeners("SIGINT")) {
    ant_value_t sig_arg = js_mkstr(js, "SIGINT", 6);
    emit_process_event("SIGINT", &sig_arg, 1);
  } else {
    uv_signal_stop(handle);
    raise(SIGINT);
  }
}
#endif

static void start_reading(rl_interface_t *iface) {
  if (iface->reading || iface->closed) return;

  if (!iface->tty_initialized) {
    uv_loop_t *loop = uv_default_loop();
    int is_tty = uv_guess_handle(STDIN_FILENO) == UV_TTY;

    if (uv_tty_init(loop, &iface->tty_in, STDIN_FILENO, 1) != 0) return;

    if (is_tty) {
#ifndef _WIN32
      enter_raw_mode(iface);
      uv_signal_init(loop, &iface->sigint_watcher);
      iface->sigint_watcher.data = iface;
      uv_signal_start(&iface->sigint_watcher, on_sigint, SIGINT);
      iface->sigint_watcher_active = true;
#endif
    }

    iface->tty_in.data = iface;
    iface->tty_initialized = true;
  }

  iface->reading = true;
  uv_read_start((uv_stream_t *)&iface->tty_in, alloc_buffer, on_stdin_read);
}

static rl_interface_t *get_interface(ant_t *js, ant_value_t this_obj) {
  ant_value_t id_val = js_get(js, this_obj, "_rl_id");
  if (vtype(id_val) != T_NUM) return NULL;
  
  uint64_t id = (uint64_t)js_getnum(id_val);
  rl_interface_t *iface = NULL;
  HASH_FIND(hh, interfaces, &id, sizeof(uint64_t), iface);
  return iface;
}

static ant_value_t rl_interface_close(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  
  if (!iface || iface->closed) return js_mkundef();
  rl_close_interface(js, iface);
  
  return js_mkundef();
}

static ant_value_t rl_interface_pause(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  if (!iface) return js_mkerr(js, "Invalid Interface");
  
  if (!iface->paused) {
    iface->paused = true;
    stop_reading(iface);
    emit_event(js, iface, "pause", NULL, 0);
  }
  
  return this_obj;
}

static ant_value_t rl_interface_resume(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  if (!iface) return js_mkerr(js, "Invalid Interface");
  
  if (iface->paused) {
    iface->paused = false;
    start_reading(iface);
    emit_event(js, iface, "resume", NULL, 0);
  }
  
  return this_obj;
}

static ant_value_t rl_interface_prompt(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
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
  
  write_output(iface, rl_render_prompt(iface));
  if (iface->line_len > 0) {
    write_output(iface, iface->line_buffer);
  }
  
  start_reading(iface);
  return js_mkundef();
}

static ant_value_t rl_interface_set_prompt(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
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

static ant_value_t rl_interface_get_prompt(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
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

static ant_value_t rl_interface_write(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  
  if (!iface || iface->closed) return js_mkundef();
  
  if (iface->paused) {
    iface->paused = false;
    emit_event(js, iface, "resume", NULL, 0);
  }
  
  if (nargs >= 2 && is_special_object(args[1])) {
    ant_value_t key = args[1];
    ant_value_t name_val = js_get(js, key, "name");
    ant_value_t ctrl_val = js_get(js, key, "ctrl");
    ant_value_t meta_val = js_get(js, key, "meta");
    ant_value_t shift_val = js_get(js, key, "shift");
    
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

static ant_value_t rl_interface_line_getter(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  
  if (!iface) return js_mkundef();
  return js_mkstr(js, iface->line_buffer, strlen(iface->line_buffer));
}

static ant_value_t rl_interface_cursor_getter(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  
  if (!iface) return js_mknum(0);
  return js_mknum((double)iface->line_pos);
}

static ant_value_t rl_interface_question_callback(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
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
  
  rl_set_active_prompt(iface, query);
  write_output(iface, rl_render_prompt(iface));
  if (!rl_add_listener(js, iface, "line", args[1], true)) return js_mkerr(js, "listener must be a function");
  
  return js_mkundef();
}

static ant_value_t rl_interface_question_promise(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  
  if (!iface || iface->closed) return js_mkerr(js, "Interface is closed");
  if (nargs < 1) return js_mkerr(js, "question requires a query string");
  
  size_t query_len;
  char *query = js_getstr(js, args[0], &query_len);
  if (!query) return js_mkerr(js, "query must be a string");
  
  ant_value_t promise = js_mkpromise(js);
  
  rl_set_active_prompt(iface, query);
  write_output(iface, rl_render_prompt(iface));
  
  iface->pending_question_resolve = js_get(js, promise, "_resolve");
  iface->pending_question_reject = js_get(js, promise, "_reject");
  
  return promise;
}

static ant_value_t rl_interface_get_cursor_pos(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  
  if (!iface) {
    ant_value_t result = js_mkobj(js);
    js_set(js, result, "rows", js_mknum(0));
    js_set(js, result, "cols", js_mknum(0));
    return result;
  }
  
  int prompt_len = (int)strlen(rl_render_prompt(iface));
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
  
  ant_value_t result = js_mkobj(js);
  js_set(js, result, "rows", js_mknum((double)rows));
  js_set(js, result, "cols", js_mknum((double)col_pos));
  return result;
}

static ant_value_t rl_interface_closed_getter(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  
  if (!iface) return js_true;
  return js_bool(iface->closed);
}

static void free_interface(rl_interface_t *iface) {
  if (!iface) return;
  
  HASH_DEL(interfaces, iface);
  
  free(iface->prompt);
  free(iface->active_prompt);
  free(iface->line_buffer);
  rl_history_free(&iface->history);
  free(iface);
}

static ant_value_t rl_clear_line(ant_t *js, ant_value_t *args, int nargs) {
  int dir = 0;
  if (!tty_ctrl_parse_clear_line_dir(args, nargs, 1, &dir)) return js_false;

  size_t seq_len = 0;
  const char *seq = tty_ctrl_clear_line_seq(dir, &seq_len);
  return tty_ctrl_bool_result(js, tty_ctrl_write_stream(stdout, seq, seq_len, true));
}

static ant_value_t rl_clear_screen_down(ant_t *js, ant_value_t *args, int nargs) {
  size_t seq_len = 0;
  const char *seq = tty_ctrl_clear_screen_down_seq(&seq_len);
  return tty_ctrl_bool_result(js, tty_ctrl_write_stream(stdout, seq, seq_len, true));
}

static ant_value_t rl_cursor_to(ant_t *js, ant_value_t *args, int nargs) {
  tty_ctrl_cursor_to_args_t parsed;
  if (!tty_ctrl_parse_cursor_to_args(args, nargs, 1, 2, &parsed)) return js_false;

  char seq[64];
  size_t seq_len = 0;
  bool ok = tty_ctrl_build_cursor_to(
    seq, sizeof(seq),
    parsed.x, parsed.has_y, parsed.y,
    &seq_len
  );
  if (!ok) return js_false;

  return tty_ctrl_bool_result(js, tty_ctrl_write_stream(stdout, seq, seq_len, true));
}

static ant_value_t rl_move_cursor(ant_t *js, ant_value_t *args, int nargs) {
  int dx = 0;
  int dy = 0;
  if (!tty_ctrl_parse_move_cursor_args(args, nargs, 1, 2, &dx, &dy)) return js_false;

  bool ok = true;
  if (dx != 0) {
    char seq_x[32];
    size_t len_x = 0;
    ok = tty_ctrl_build_move_cursor_axis(seq_x, sizeof(seq_x), dx, true, &len_x);
    if (ok) ok = tty_ctrl_write_stream(stdout, seq_x, len_x, false);
  }

  if (ok && dy != 0) {
    char seq_y[32];
    size_t len_y = 0;
    ok = tty_ctrl_build_move_cursor_axis(seq_y, sizeof(seq_y), dy, false, &len_y);
    if (ok) ok = tty_ctrl_write_stream(stdout, seq_y, len_y, false);
  }

  if (!ok) return js_false;
  return tty_ctrl_bool_result(js, fflush(stdout) == 0);
}

static ant_value_t rl_emit_keypress_events(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs > 0) {
    ant_value_t stdin_obj = js_get(js, js_get(js, js_glob(js), "process"), "stdin");
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

static void rl_async_iter_compact_queue(ant_t *js, ant_value_t state, const char *queue_key, const char *head_key) {
  ant_value_t queue = rl_async_iter_queue(js, state, queue_key);
  ant_offset_t head = rl_async_iter_queue_head(js, state, head_key);
  ant_offset_t len = vtype(queue) == T_ARR ? js_arr_len(js, queue) : 0;
  ant_value_t compact = 0;

  if (vtype(queue) != T_ARR || head == 0) return;

  if (head >= len) {
    js_set(js, state, queue_key, js_mkarr(js));
    js_set(js, state, head_key, js_mknum(0));
    return;
  }

  if (head <= 32 && head * 2 < len) return;

  compact = js_mkarr(js);
  for (ant_offset_t i = head; i < len; i++) js_arr_push(js, compact, js_arr_get(js, queue, i));
  js_set(js, state, queue_key, compact);
  js_set(js, state, head_key, js_mknum(0));
}

static void rl_async_iter_queue_push(ant_t *js, ant_value_t state, const char *queue_key, ant_value_t value) {
  ant_value_t queue = rl_async_iter_queue(js, state, queue_key);
  if (vtype(queue) == T_ARR) js_arr_push(js, queue, value);
}

static ant_value_t rl_async_iter_queue_shift(ant_t *js, ant_value_t state, const char *queue_key, const char *head_key) {
  ant_value_t queue = rl_async_iter_queue(js, state, queue_key);
  ant_offset_t head = rl_async_iter_queue_head(js, state, head_key);
  ant_offset_t len = vtype(queue) == T_ARR ? js_arr_len(js, queue) : 0;
  ant_value_t value = js_mkundef();

  if (vtype(queue) != T_ARR || head >= len) return js_mkundef();
  value = js_arr_get(js, queue, head);
  rl_async_iter_set_queue_head(js, state, head_key, head + 1);
  rl_async_iter_compact_queue(js, state, queue_key, head_key);
  
  return value;
}

static void rl_async_iter_cleanup(ant_t *js, ant_value_t state) {
  ant_value_t iface_obj = is_object_type(state) ? js_get(js, state, "iface") : js_mkundef();
  rl_interface_t *iface = get_interface(js, iface_obj);
  if (!iface) return;

  rl_remove_listener(js, iface, "line", js_get(js, state, "onLine"));
  rl_remove_listener(js, iface, "close", js_get(js, state, "onClose"));
}

static void rl_async_iter_finish(ant_t *js, ant_value_t state) {
  if (!is_object_type(state)) return;

  js_set(js, state, "done", js_true);
  rl_async_iter_cleanup(js, state);

  for (;;) {
    ant_value_t pending = rl_async_iter_queue_shift(js, state, "pending", "pendingHead");
    if (vtype(pending) != T_PROMISE) break;
    js_resolve_promise(js, pending, js_iter_result(js, false, js_mkundef()));
  }
}

static ant_value_t rl_async_iter_on_line(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = rl_async_iter_state(js, js_getcurrentfunc(js));
  ant_value_t line = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t pending = 0;

  if (!is_object_type(state)) return js_mkundef();
  if (js_truthy(js, js_get(js, state, "done"))) return js_mkundef();

  pending = rl_async_iter_queue_shift(js, state, "pending", "pendingHead");
  if (vtype(pending) == T_PROMISE) {
    js_resolve_promise(js, pending, js_iter_result(js, true, line));
  } else rl_async_iter_queue_push(js, state, "buffer", line);

  return js_mkundef();
}

static ant_value_t rl_async_iter_on_close(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = rl_async_iter_state(js, js_getcurrentfunc(js));
  rl_async_iter_finish(js, state);
  return js_mkundef();
}

static ant_value_t rl_async_iter_next(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = rl_async_iter_state(js, js_getthis(js));
  ant_value_t promise = js_mkpromise(js);
  ant_value_t value = 0;

  if (!is_object_type(state)) {
    js_resolve_promise(js, promise, js_iter_result(js, false, js_mkundef()));
    return promise;
  }

  value = rl_async_iter_queue_shift(js, state, "buffer", "bufferHead");
  if (vtype(value) != T_UNDEF) {
    js_resolve_promise(js, promise, js_iter_result(js, true, value));
    return promise;
  }

  if (js_truthy(js, js_get(js, state, "done"))) {
    js_resolve_promise(js, promise, js_iter_result(js, false, js_mkundef()));
    return promise;
  }

  rl_async_iter_queue_push(js, state, "pending", promise);
  return promise;
}

static ant_value_t rl_async_iter_return(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t state = rl_async_iter_state(js, js_getthis(js));
  ant_value_t promise = js_mkpromise(js);

  if (!is_object_type(state)) {
    js_resolve_promise(js, promise, js_iter_result(js, false, js_mkundef()));
    return promise;
  }

  if (!js_truthy(js, js_get(js, state, "done"))) {
    ant_value_t iface_obj = js_get(js, state, "iface");
    rl_interface_t *iface = get_interface(js, iface_obj);
    
    if (iface && !iface->closed) {
      ant_value_t old_this = js_getthis(js);
      js_setthis(js, iface_obj);
      rl_interface_close(js, NULL, 0);
      js_setthis(js, old_this);
    } else rl_async_iter_finish(js, state);
  }

  js_resolve_promise(js, promise, js_iter_result(js, false, js_mkundef()));
  return promise;
}

static ant_value_t rl_get_async_iter_proto(ant_t *js) {
  if (is_object_type(js->sym.rl_async_iter_proto)) return js->sym.rl_async_iter_proto;

  js->sym.rl_async_iter_proto = js_mkobj(js);
  js_set(js, js->sym.rl_async_iter_proto, "next", js_mkfun(rl_async_iter_next));
  js_set(js, js->sym.rl_async_iter_proto, "return", js_mkfun(rl_async_iter_return));
  js_set_sym(js, js->sym.rl_async_iter_proto, get_asyncIterator_sym(), js_mkfun(sym_this_cb));
  js_set_sym(js, js->sym.rl_async_iter_proto, get_toStringTag_sym(), js_mkstr(js, "AsyncIterator", 13));

  return js->sym.rl_async_iter_proto;
}

static ant_value_t rl_interface_async_iterator(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  rl_interface_t *iface = get_interface(js, this_obj);
  
  ant_value_t iterator = 0;
  ant_value_t state = 0;
  ant_value_t on_line = 0;
  ant_value_t on_close = 0;
  
  if (!iface) return js_mkerr(js, "Invalid Interface");

  iterator = js_mkobj(js);
  state = js_mkobj(js);

  js_set_proto_init(iterator, rl_get_async_iter_proto(js));
  js_set_slot_wb(js, iterator, SLOT_DATA, state);

  js_set(js, state, "iface", this_obj);
  js_set(js, state, "buffer", js_mkarr(js));
  js_set(js, state, "bufferHead", js_mknum(0));
  js_set(js, state, "pending", js_mkarr(js));
  js_set(js, state, "pendingHead", js_mknum(0));
  js_set(js, state, "done", js_bool(iface->closed));

  if (!iface->closed) {
    on_line = js_heavy_mkfun(js, rl_async_iter_on_line, state);
    on_close = js_heavy_mkfun(js, rl_async_iter_on_close, state);
    js_set(js, state, "onLine", on_line);
    js_set(js, state, "onClose", on_close);
    
  if (!rl_add_listener(js, iface, "line", on_line, false) || !rl_add_listener(js, iface, "close", on_close, false)) {
    rl_remove_listener(js, iface, "line", on_line);
    rl_remove_listener(js, iface, "close", on_close);
    return js_mkerr(js, "listener must be a function");
  }}

  return iterator;
}

static ant_value_t rl_get_interface_proto(ant_t *js) {
  if (is_object_type(js->sym.rl_interface_proto)) return js->sym.rl_interface_proto;

  js->sym.rl_interface_proto = js_mkobj(js);
  js_set_proto_init(js->sym.rl_interface_proto, eventemitter_prototype(js));

  js_set(js, js->sym.rl_interface_proto, "close", js_mkfun(rl_interface_close));
  js_set(js, js->sym.rl_interface_proto, "pause", js_mkfun(rl_interface_pause));
  js_set(js, js->sym.rl_interface_proto, "resume", js_mkfun(rl_interface_resume));
  js_set(js, js->sym.rl_interface_proto, "prompt", js_mkfun(rl_interface_prompt));
  js_set(js, js->sym.rl_interface_proto, "setPrompt", js_mkfun(rl_interface_set_prompt));
  js_set(js, js->sym.rl_interface_proto, "getPrompt", js_mkfun(rl_interface_get_prompt));
  js_set(js, js->sym.rl_interface_proto, "write", js_mkfun(rl_interface_write));
  js_set(js, js->sym.rl_interface_proto, "question", js_mkfun(rl_interface_question_callback));
  js_set(js, js->sym.rl_interface_proto, "getCursorPos", js_mkfun(rl_interface_get_cursor_pos));

  js_set_getter_desc(js, js->sym.rl_interface_proto, "line", 4, js_mkfun(rl_interface_line_getter), JS_DESC_E | JS_DESC_C);
  js_set_getter_desc(js, js->sym.rl_interface_proto, "cursor", 6, js_mkfun(rl_interface_cursor_getter), JS_DESC_E | JS_DESC_C);
  js_set_getter_desc(js, js->sym.rl_interface_proto, "closed", 6, js_mkfun(rl_interface_closed_getter), JS_DESC_E | JS_DESC_C);
  js_set_sym(js, js->sym.rl_interface_proto, get_asyncIterator_sym(), js_mkfun(rl_interface_async_iterator));
  js_set_sym(js, js->sym.rl_interface_proto, get_toStringTag_sym(), js_mkstr(js, "Interface", 9));

  return js->sym.rl_interface_proto;
}

static ant_value_t rl_create_interface(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "createInterface requires options");
  
  ant_value_t options = args[0];
  if (!is_special_object(options)) return js_mkerr(js, "options must be an object");
  
  rl_interface_t *iface = calloc(1, sizeof(rl_interface_t));
  if (!iface) return js_mkerr(js, "out of memory");
  
  iface->id = next_interface_id++;
  iface->prompt = strdup(DEFAULT_PROMPT);
  iface->active_prompt = NULL;
  iface->line_buffer = calloc(MAX_LINE_LENGTH, 1);
  iface->line_pos = 0;
  iface->line_len = 0;
  iface->paused = false;
  iface->closed = false;
  iface->reading = false;
  iface->pending_question_resolve = js_mkundef();
  iface->pending_question_reject = js_mkundef();
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
  
  ant_value_t terminal_val = js_get(js, options, "terminal");
  iface->terminal = terminal_val == js_true || vtype(terminal_val) == T_UNDEF;
  
  ant_value_t history_size_val = js_get(js, options, "historySize");
  iface->history_size = (vtype(history_size_val) == T_NUM) 
    ? (int)js_getnum(history_size_val) 
    : DEFAULT_HISTORY_SIZE;
  
  ant_value_t remove_dup_val = js_get(js, options, "removeHistoryDuplicates");
  iface->remove_history_duplicates = js_truthy(js, remove_dup_val);
  
  ant_value_t prompt_val = js_get(js, options, "prompt");
  if (vtype(prompt_val) == T_STR) {
    free(iface->prompt);
    iface->prompt = strdup(js_getstr(js, prompt_val, NULL));
  }
  
  ant_value_t crlf_delay_val = js_get(js, options, "crlfDelay");
  iface->crlf_delay = (vtype(crlf_delay_val) == T_NUM) 
    ? (int)js_getnum(crlf_delay_val) 
    : 100;
  if (iface->crlf_delay < 100) iface->crlf_delay = 100;
  
  ant_value_t tab_size_val = js_get(js, options, "tabSize");
  iface->tab_size = (vtype(tab_size_val) == T_NUM) 
    ? (int)js_getnum(tab_size_val) 
    : DEFAULT_TAB_SIZE;
  if (iface->tab_size < 1) iface->tab_size = 1;
  
  ant_value_t completer_val = js_get(js, options, "completer");
  int ctype = vtype(completer_val);
  iface->completer = (ctype == T_FUNC || ctype == T_CFUNC) ? completer_val : js_mkundef();
  
  ant_value_t history_val = js_get(js, options, "history");
  if (is_special_object(history_val)) {
    ant_value_t len_val = js_get(js, history_val, "length");
    int len = (vtype(len_val) == T_NUM) ? (int)js_getnum(len_val) : 0;
    
    rl_history_init(&iface->history, iface->history_size);
    
    for (int i = 0; i < len; i++) {
      char key[16];
      snprintf(key, sizeof(key), "%d", i);
      ant_value_t item = js_get(js, history_val, key);
      if (vtype(item) == T_STR) {
        char *line = js_getstr(js, item, NULL);
        if (line) rl_history_add(&iface->history, line, false);
      }
    }
  } else rl_history_init(&iface->history, iface->history_size);
  HASH_ADD(hh, interfaces, id, sizeof(uint64_t), iface);
  
  ant_value_t obj = js_mkobj(js);
  js_set_proto_init(obj, rl_get_interface_proto(js));
  iface->js_obj = obj;
  js_set(js, obj, "_rl_id", js_mknum((double)iface->id));
  js_set(js, obj, "terminal", js_bool(iface->terminal));

  start_reading(iface);
  
  return obj;
}

static ant_value_t rl_create_interface_promises(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t iface_obj = rl_create_interface(js, args, nargs);
  if (vtype(iface_obj) == T_ERR) return iface_obj;
  js_set(js, iface_obj, "question", js_mkfun(rl_interface_question_promise));
  
  return iface_obj;
}

ant_value_t readline_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);

  js_set(js, lib, "createInterface", js_mkfun(rl_create_interface));
  js_set(js, lib, "clearLine", js_mkfun(rl_clear_line));
  js_set(js, lib, "clearScreenDown", js_mkfun(rl_clear_screen_down));
  js_set(js, lib, "cursorTo", js_mkfun(rl_cursor_to));
  js_set(js, lib, "moveCursor", js_mkfun(rl_move_cursor));
  js_set(js, lib, "emitKeypressEvents", js_mkfun(rl_emit_keypress_events));
  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "readline", 8));
  
  return lib;
}

ant_value_t readline_promises_library(ant_t *js) {
  ant_value_t lib = js_mkobj(js);

  js_set(js, lib, "createInterface", js_mkfun(rl_create_interface_promises));
  js_set(js, lib, "clearLine", js_mkfun(rl_clear_line));
  js_set(js, lib, "clearScreenDown", js_mkfun(rl_clear_screen_down));
  js_set(js, lib, "cursorTo", js_mkfun(rl_cursor_to));
  js_set(js, lib, "moveCursor", js_mkfun(rl_move_cursor));
  js_set(js, lib, "emitKeypressEvents", js_mkfun(rl_emit_keypress_events));
  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "readline/promises", 17));
  
  return lib;
}

void gc_mark_readline(ant_t *js, gc_mark_fn mark) {
  if (js->sym.rl_async_iter_proto) mark(js, js->sym.rl_async_iter_proto);
  if (js->sym.rl_interface_proto) mark(js, js->sym.rl_interface_proto);
  
  rl_interface_t *iface, *tmp;
  HASH_ITER(hh, interfaces, iface, tmp) {
    mark(js, iface->input_stream);
    mark(js, iface->output_stream);
    mark(js, iface->completer);
    mark(js, iface->js_obj);
    mark(js, iface->pending_question_resolve);
    mark(js, iface->pending_question_reject);
  }
}
