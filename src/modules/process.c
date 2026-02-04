#include <compat.h> // IWYU pragma: keep

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <uthash.h>
#include <uv.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <psapi.h>
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#else
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <grp.h>
#include <pwd.h>
#endif

#include "ant.h"
#include "errors.h"
#include "config.h"
#include "internal.h"
#include "runtime.h"
#include "utils.h"

#include "modules/process.h"
#include "modules/symbol.h"

#ifndef _WIN32
extern char **environ;
#else
#define environ _environ
#endif

#define DEFAULT_MAX_LISTENERS 10
#define INITIAL_LISTENER_CAPACITY 4

typedef struct {
  jsval_t listener;
  bool once;
} ProcessEventListener;

typedef struct {
  char *event_type;
  ProcessEventListener *listeners;
  int listener_count;
  int listener_capacity;
  UT_hash_handle hh;
} ProcessEventType;

static int max_listeners = DEFAULT_MAX_LISTENERS;
static ProcessEventType *process_events = NULL;

static ProcessEventType *stdin_events = NULL;
static ProcessEventType *stdout_events = NULL;
static ProcessEventType *stderr_events = NULL;

typedef struct {
  uv_tty_t tty;
  bool tty_initialized;
  bool reading;
  bool keypress_enabled;
  int escape_state;
  int escape_len;
  char escape_buf[16];
} stdin_state_t;

static stdin_state_t stdin_state = {0};
static uint64_t process_start_time = 0;

#ifndef _WIN32
static struct termios stdin_saved_termios;
static bool stdin_raw_mode = false;
static uv_signal_t sigwinch_handle;
static bool sigwinch_initialized = false;
#endif

typedef struct {
  const char *name;
  int signum;
  UT_hash_handle hh_name;
  UT_hash_handle hh_num;
} SignalEntry;

static SignalEntry *signals_by_name = NULL;
static SignalEntry *signals_by_num = NULL;

static void init_signal_map(void) {
  static bool initialized = false;
  if (initialized) return;
  
  static SignalEntry entries[] = {
#ifdef SIGHUP
    { "SIGHUP", SIGHUP, {0}, {0} },
#endif
#ifdef SIGINT
    { "SIGINT", SIGINT, {0}, {0} },
#endif
#ifdef SIGQUIT
    { "SIGQUIT", SIGQUIT, {0}, {0} },
#endif
#ifdef SIGILL
    { "SIGILL", SIGILL, {0}, {0} },
#endif
#ifdef SIGTRAP
    { "SIGTRAP", SIGTRAP, {0}, {0} },
#endif
#ifdef SIGABRT
    { "SIGABRT", SIGABRT, {0}, {0} },
#endif
#ifdef SIGBUS
    { "SIGBUS", SIGBUS, {0}, {0} },
#endif
#ifdef SIGFPE
    { "SIGFPE", SIGFPE, {0}, {0} },
#endif
#ifdef SIGUSR1
    { "SIGUSR1", SIGUSR1, {0}, {0} },
#endif
#ifdef SIGUSR2
    { "SIGUSR2", SIGUSR2, {0}, {0} },
#endif
#ifdef SIGSEGV
    { "SIGSEGV", SIGSEGV, {0}, {0} },
#endif
#ifdef SIGPIPE
    { "SIGPIPE", SIGPIPE, {0}, {0} },
#endif
#ifdef SIGALRM
    { "SIGALRM", SIGALRM, {0}, {0} },
#endif
#ifdef SIGTERM
    { "SIGTERM", SIGTERM, {0}, {0} },
#endif
#ifdef SIGCHLD
    { "SIGCHLD", SIGCHLD, {0}, {0} },
#endif
#ifdef SIGCONT
    { "SIGCONT", SIGCONT, {0}, {0} },
#endif
#ifdef SIGTSTP
    { "SIGTSTP", SIGTSTP, {0}, {0} },
#endif
#ifdef SIGTTIN
    { "SIGTTIN", SIGTTIN, {0}, {0} },
#endif
#ifdef SIGTTOU
    { "SIGTTOU", SIGTTOU, {0}, {0} },
#endif
#ifdef SIGURG
    { "SIGURG", SIGURG, {0}, {0} },
#endif
#ifdef SIGXCPU
    { "SIGXCPU", SIGXCPU, {0}, {0} },
#endif
#ifdef SIGXFSZ
    { "SIGXFSZ", SIGXFSZ, {0}, {0} },
#endif
#ifdef SIGVTALRM
    { "SIGVTALRM", SIGVTALRM, {0}, {0} },
#endif
#ifdef SIGPROF
    { "SIGPROF", SIGPROF, {0}, {0} },
#endif
#ifdef SIGWINCH
    { "SIGWINCH", SIGWINCH, {0}, {0} },
#endif
#ifdef SIGIO
    { "SIGIO", SIGIO, {0}, {0} },
#endif
#ifdef SIGSYS
    { "SIGSYS", SIGSYS, {0}, {0} },
#endif
  };
  
  for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
    HASH_ADD_KEYPTR(hh_name, signals_by_name, entries[i].name, strlen(entries[i].name), &entries[i]);
    HASH_ADD(hh_num, signals_by_num, signum, sizeof(int), &entries[i]);
  }
  
  initialized = true;
}

static int get_signal_number(const char *name) {
  init_signal_map();
  SignalEntry *entry = NULL;
  HASH_FIND(hh_name, signals_by_name, name, strlen(name), entry);
  return entry ? entry->signum : -1;
}

static const char *get_signal_name(int signum) {
  init_signal_map();
  SignalEntry *entry = NULL;
  HASH_FIND(hh_num, signals_by_num, &signum, sizeof(int), entry);
  return entry ? entry->name : NULL;
}

static ProcessEventType *find_or_create_event_type(const char *event_type) {
  ProcessEventType *evt = NULL;
  HASH_FIND_STR(process_events, event_type, evt);
  
  if (evt == NULL) {
    evt = malloc(sizeof(ProcessEventType));
    evt->event_type = strdup(event_type);
    evt->listener_count = 0;
    evt->listener_capacity = INITIAL_LISTENER_CAPACITY;
    evt->listeners = malloc(sizeof(ProcessEventListener) * evt->listener_capacity);
    HASH_ADD_KEYPTR(hh, process_events, evt->event_type, strlen(evt->event_type), evt);
  }
  
  return evt;
}

static bool ensure_listener_capacity(ProcessEventType *evt) {
  if (evt->listener_count >= evt->listener_capacity) {
    int new_capacity = evt->listener_capacity * 2;
    ProcessEventListener *new_listeners = realloc(evt->listeners, sizeof(ProcessEventListener) * new_capacity);
    if (!new_listeners) return false;
    evt->listeners = new_listeners;
    evt->listener_capacity = new_capacity;
  }
  return true;
}

static void check_listener_warning(const char *event) {
  ProcessEventType *evt = NULL;
  HASH_FIND_STR(process_events, event, evt);
  if (evt && evt->listener_count == max_listeners) fprintf(stderr, 
    "Warning: Possible EventEmitter memory leak detected. "
    "%d '%s' listeners added. Use process.setMaxListeners() to increase limit.\n",
    evt->listener_count, event
  );
}

static void emit_process_event(const char *event_type, jsval_t *args, int nargs) {
  if (!rt->js) return;
  
  ProcessEventType *evt = NULL;
  HASH_FIND_STR(process_events, event_type, evt);
  
  if (evt == NULL || evt->listener_count == 0) return;
  
  int i = 0;
  while (i < evt->listener_count) {
    ProcessEventListener *listener = &evt->listeners[i];
    js_call(rt->js, listener->listener, args, nargs);
    
    if (listener->once) {
      for (int j = i; j < evt->listener_count - 1; j++) {
        evt->listeners[j] = evt->listeners[j + 1];
      } evt->listener_count--;
    } else i++;
  }
}

static void process_signal_handler(int signum) {
  const char *name = get_signal_name(signum);
  if (name) {
    jsval_t sig_arg = js_mkstr(rt->js, name, strlen(name));
    emit_process_event(name, &sig_arg, 1);
  }
}

static ProcessEventType *find_or_create_stdin_event(const char *event_type) {
  ProcessEventType *evt = NULL;
  HASH_FIND_STR(stdin_events, event_type, evt);
  if (evt == NULL) {
    evt = malloc(sizeof(ProcessEventType));
    evt->event_type = strdup(event_type);
    evt->listener_count = 0;
    evt->listener_capacity = INITIAL_LISTENER_CAPACITY;
    evt->listeners = malloc(sizeof(ProcessEventListener) * evt->listener_capacity);
    HASH_ADD_KEYPTR(hh, stdin_events, evt->event_type, strlen(evt->event_type), evt);
  }
  return evt;
}

static ProcessEventType *find_or_create_stdout_event(const char *event_type) {
  ProcessEventType *evt = NULL;
  HASH_FIND_STR(stdout_events, event_type, evt);
  if (evt == NULL) {
    evt = malloc(sizeof(ProcessEventType));
    evt->event_type = strdup(event_type);
    evt->listener_count = 0;
    evt->listener_capacity = INITIAL_LISTENER_CAPACITY;
    evt->listeners = malloc(sizeof(ProcessEventListener) * evt->listener_capacity);
    HASH_ADD_KEYPTR(hh, stdout_events, evt->event_type, strlen(evt->event_type), evt);
  }
  return evt;
}

static void emit_stdio_event(ProcessEventType *events, const char *event_type, jsval_t *args, int nargs) {
  if (!rt->js) return;
  ProcessEventType *evt = NULL;
  HASH_FIND_STR(events, event_type, evt);
  if (evt == NULL || evt->listener_count == 0) return;
  
  int i = 0;
  while (i < evt->listener_count) {
    ProcessEventListener *listener = &evt->listeners[i];
    js_call(rt->js, listener->listener, args, nargs);
    if (listener->once) {
      for (int j = i; j < evt->listener_count - 1; j++) {
        evt->listeners[j] = evt->listeners[j + 1];
      }
      evt->listener_count--;
    } else i++;
  }
}

static const char *stdin_escape_name(const char *seq, int len) {
  if (len < 2) return NULL;

  if (seq[0] == '[') {
    if (seq[1] >= '0' && seq[1] <= '9') {
      int num = 0;
      int idx = 1;

      while (idx < len && seq[idx] >= '0' && seq[idx] <= '9') {
        num = num * 10 + (seq[idx] - '0');
        idx++;
      }

      if (idx < len && seq[idx] == '~') {
        typedef struct {
          int code;
          const char *name;
        } esc_num_map_t;

        static const esc_num_map_t esc_num_map[] = {
          { 1, "home" },
          { 2, "insert" },
          { 3, "delete" },
          { 4, "end" },
          { 5, "pageup" },
          { 6, "pagedown" },
          { 7, "home" },
          { 8, "end" },
          { 15, "f5" },
          { 17, "f6" },
          { 18, "f7" },
          { 19, "f8" },
          { 20, "f9" },
          { 21, "f10" },
          { 23, "f11" },
          { 24, "f12" },
        };

        for (size_t i = 0; i < sizeof(esc_num_map) / sizeof(esc_num_map[0]); i++) {
          if (esc_num_map[i].code == num) return esc_num_map[i].name;
        }
      }
      return NULL;
    }

    typedef struct {
      char code;
      bool needs_tilde;
      const char *name;
    } esc_map_t;

    static const esc_map_t esc_map[] = {
      { 'A', false, "up" },
      { 'B', false, "down" },
      { 'C', false, "right" },
      { 'D', false, "left" },
      { 'H', false, "home" },
      { 'F', false, "end" },
      { 'Z', false, "tab" },
      { '2', true, "insert" },
      { '3', true, "delete" },
      { '5', true, "pageup" },
      { '6', true, "pagedown" },
    };

    for (size_t i = 0; i < sizeof(esc_map) / sizeof(esc_map[0]); i++) {
      if (seq[1] != esc_map[i].code) continue;
      if (esc_map[i].needs_tilde) {
        return (len >= 3 && seq[2] == '~') ? esc_map[i].name : NULL;
      }
      return esc_map[i].name;
    }
    return NULL;
  }

  if (seq[0] == 'O') {
    switch (seq[1]) {
      case 'P': return "f1";
      case 'Q': return "f2";
      case 'R': return "f3";
      case 'S': return "f4";
      default: return NULL;
    }
  }

  return NULL;
}

static void emit_keypress_event(
  struct js *js,
  const char *str,
  size_t str_len,
  const char *name,
  bool ctrl,
  bool meta,
  bool shift,
  const char *sequence,
  size_t sequence_len
) {
  jsval_t str_val = js_mkstr(js, str ? str : "", str ? str_len : 0);
  jsval_t key_obj = js_mkobj(js);

  if (name) {
    js_set(js, key_obj, "name", js_mkstr(js, name, strlen(name)));
  } else {
    js_set(js, key_obj, "name", js_mkundef());
  }

  js_set(js, key_obj, "ctrl", js_bool(ctrl));
  js_set(js, key_obj, "meta", js_bool(meta));
  js_set(js, key_obj, "shift", js_bool(shift));

  if (sequence) {
    js_set(js, key_obj, "sequence", js_mkstr(js, sequence, sequence_len));
  }

  jsval_t args[2] = { str_val, key_obj };
  emit_stdio_event(stdin_events, "keypress", args, 2);
}

static void process_keypress_data(struct js *js, const char *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)data[i];

    if (stdin_state.escape_state == 1) {
      stdin_state.escape_buf[stdin_state.escape_len++] = (char)c;
      if (c == '[' || c == 'O') {
        stdin_state.escape_state = 2;
        continue;
      }

      emit_keypress_event(js, "\x1b", 1, "escape", false, false, false, "\x1b", 1);
      stdin_state.escape_state = 0;
      stdin_state.escape_len = 0;
    }

    if (stdin_state.escape_state == 2) {
      stdin_state.escape_buf[stdin_state.escape_len++] = (char)c;
      if ((c >= 'A' && c <= 'Z') || c == '~' || stdin_state.escape_len >= 15) {
        char sequence[18];
        size_t seq_len = 0;
        sequence[seq_len++] = '\x1b';
        memcpy(sequence + seq_len, stdin_state.escape_buf, (size_t)stdin_state.escape_len);
        seq_len += (size_t)stdin_state.escape_len;

        const char *name = stdin_escape_name(stdin_state.escape_buf, stdin_state.escape_len);
        if (!name) name = "escape";

        emit_keypress_event(js, "", 0, name, false, false, false, sequence, seq_len);
        stdin_state.escape_state = 0;
        stdin_state.escape_len = 0;
      }
      continue;
    }

    if (c == 27) {
      stdin_state.escape_state = 1;
      stdin_state.escape_len = 0;
      continue;
    }

    if (c == '\r' || c == '\n') {
      emit_keypress_event(js, "\n", 1, "return", false, false, false, "\n", 1);
      continue;
    }

    if (c == 127 || c == 8) {
      emit_keypress_event(js, "", 0, "backspace", false, false, false, NULL, 0);
      continue;
    }

    if (c == '\t') {
      emit_keypress_event(js, "\t", 1, "tab", false, false, false, "\t", 1);
      continue;
    }

    if (c < 32) {
      char name_buf[2] = { (char)('a' + c - 1), '\0' };
      char seq = (char)c;
      emit_keypress_event(js, &seq, 1, name_buf, true, false, false, &seq, 1);
      continue;
    }

    char ch = (char)c;
    char name_buf[2] = { ch, '\0' };
    emit_keypress_event(js, &ch, 1, name_buf, false, false, false, &ch, 1);
  }

  if (stdin_state.escape_state == 1) {
    emit_keypress_event(js, "\x1b", 1, "escape", false, false, false, "\x1b", 1);
    stdin_state.escape_state = 0;
    stdin_state.escape_len = 0;
  }
}

static bool remove_listener_from_events(ProcessEventType *events, const char *event, jsval_t listener) {
  ProcessEventType *evt = NULL;
  HASH_FIND_STR(events, event, evt);
  if (!evt) return false;
  
  for (int i = 0; i < evt->listener_count; i++) {
    if (evt->listeners[i].listener != listener) continue;
    memmove(
      &evt->listeners[i], &evt->listeners[i + 1], 
      (size_t)(evt->listener_count - i - 1) * sizeof(ProcessEventListener)
    );
    return --evt->listener_count == 0;
  }
  
  return false;
}

static bool stdin_is_tty(void) {
  return uv_guess_handle(STDIN_FILENO) == UV_TTY;
}

static bool stdout_is_tty(void) {
  return uv_guess_handle(STDOUT_FILENO) == UV_TTY;
}

static bool stderr_is_tty(void) {
  return uv_guess_handle(STDERR_FILENO) == UV_TTY;
}

static void get_tty_size(int fd, int *rows, int *cols) {
  int out_rows = 24, out_cols = 80;
#ifndef _WIN32
  struct winsize ws;
  if (ioctl(fd, TIOCGWINSZ, &ws) == 0) {
    if (ws.ws_row > 0) out_rows = ws.ws_row;
    if (ws.ws_col > 0) out_cols = ws.ws_col;
  }
#else
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
    int width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    int height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    if (height > 0) out_rows = height;
    if (width > 0) out_cols = width;
  }
#endif
  if (rows) *rows = out_rows;
  if (cols) *cols = out_cols;
}

#ifndef _WIN32
static bool stdin_set_raw_mode(bool enable) {
  if (!stdin_is_tty()) return false;
  if (enable) {
    if (stdin_raw_mode) return true;
    if (tcgetattr(STDIN_FILENO, &stdin_saved_termios) == -1) return false;
    struct termios raw = stdin_saved_termios;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == -1) return false;
    stdin_raw_mode = true;
    return true;
  }
  if (!stdin_raw_mode) return true;
  if (tcsetattr(STDIN_FILENO, TCSANOW, &stdin_saved_termios) == -1) return false;
  stdin_raw_mode = false;
  return true;
}
#else
static bool stdin_set_raw_mode(bool enable) {
  (void)enable;
  return false;
}
#endif

static void stdin_alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  (void)handle;
  buf->base = malloc(suggested_size);
  buf->len = suggested_size;
}

static void on_stdin_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  (void)stream;
  if (nread > 0 && rt->js) {
    jsval_t data_val = js_mkstr(rt->js, buf->base, (size_t)nread);
    emit_stdio_event(stdin_events, "data", &data_val, 1);
    if (stdin_state.keypress_enabled) process_keypress_data(rt->js, buf->base, (size_t)nread);
  }
  if (buf->base) free(buf->base);
}

static void stdin_start_reading(void) {
  if (stdin_state.reading) return;
  if (!stdin_state.tty_initialized) {
    uv_loop_t *loop = uv_default_loop();
    if (uv_tty_init(loop, &stdin_state.tty, STDIN_FILENO, 1) != 0) return;
#ifndef _WIN32
    uv_tty_set_mode(&stdin_state.tty, stdin_raw_mode ? UV_TTY_MODE_RAW : UV_TTY_MODE_NORMAL);
#endif
    stdin_state.tty.data = NULL;
    stdin_state.tty_initialized = true;
  } else {
#ifndef _WIN32
    uv_tty_set_mode(&stdin_state.tty, stdin_raw_mode ? UV_TTY_MODE_RAW : UV_TTY_MODE_NORMAL);
#endif
  }
  stdin_state.reading = true;
  uv_read_start((uv_stream_t *)&stdin_state.tty, stdin_alloc_buffer, on_stdin_read);
}

static void stdin_stop_reading(void) {
  if (!stdin_state.reading) return;
  uv_read_stop((uv_stream_t *)&stdin_state.tty);
  stdin_state.reading = false;
}

#ifndef _WIN32
static void on_sigwinch(uv_signal_t *handle, int signum) {
  (void)handle; (void)signum;
  if (!rt->js) return;
  
  jsval_t process_obj = js_get(rt->js, js_glob(rt->js), "process");
  if (!is_special_object(process_obj)) return;
  
  jsval_t stdout_obj = js_get(rt->js, process_obj, "stdout");
  if (!is_special_object(stdout_obj)) return;
  
  int rows = 0, cols = 0;
  get_tty_size(STDOUT_FILENO, &rows, &cols);
  js_set(rt->js, stdout_obj, "rows", js_mknum(rows));
  js_set(rt->js, stdout_obj, "columns", js_mknum(cols));
  
  emit_stdio_event(stdout_events, "resize", NULL, 0);
}
#endif

static void start_sigwinch_handler(void) {
#ifndef _WIN32
  if (sigwinch_initialized) return;
  uv_loop_t *loop = uv_default_loop();
  if (uv_signal_init(loop, &sigwinch_handle) != 0) return;
  if (uv_signal_start(&sigwinch_handle, on_sigwinch, SIGWINCH) != 0) {
    uv_close((uv_handle_t *)&sigwinch_handle, NULL);
    return;
  }
  uv_unref((uv_handle_t *)&sigwinch_handle);
  sigwinch_initialized = true;
#endif
}

static jsval_t js_stdin_set_raw_mode(ant_t *js, jsval_t *args, int nargs) {
  bool enable = nargs > 0 ? js_truthy(js, args[0]) : true;
  return js_bool(stdin_set_raw_mode(enable));
}

static jsval_t js_stdin_resume(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  stdin_start_reading();
  return js_getthis(js);
}

static jsval_t js_stdin_pause(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  stdin_stop_reading();
  return js_getthis(js);
}

static jsval_t js_stdin_on(ant_t *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  if (nargs < 2) return this_obj;
  
  char *event = js_getstr(js, args[0], NULL);
  if (!event || vtype(args[1]) != T_FUNC) return this_obj;
  
  ProcessEventType *evt = find_or_create_stdin_event(event);
  if (!ensure_listener_capacity(evt)) return this_obj;
  
  evt->listeners[evt->listener_count].listener = args[1];
  evt->listeners[evt->listener_count].once = false;
  evt->listener_count++;
  
  if (strcmp(event, "data") == 0) stdin_start_reading();
  
  return this_obj;
}

static jsval_t js_stdin_remove_all_listeners(ant_t *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  
  if (nargs < 1) {
    ProcessEventType *evt, *tmp;
    HASH_ITER(hh, stdin_events, evt, tmp) {
      evt->listener_count = 0;
    }
    stdin_stop_reading();
    return this_obj;
  }
  
  char *event = js_getstr(js, args[0], NULL);
  if (!event) return this_obj;
  
  ProcessEventType *evt = NULL;
  HASH_FIND_STR(stdin_events, event, evt);
  if (evt) evt->listener_count = 0;
  if (strcmp(event, "data") == 0) stdin_stop_reading();
  
  return this_obj;
}

static jsval_t js_stdin_remove_listener(ant_t *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  if (nargs < 2) return this_obj;
  
  char *event = js_getstr(js, args[0], NULL);
  if (!event) return this_obj;
  
  bool now_empty = remove_listener_from_events(stdin_events, event, args[1]);
  if (now_empty && strcmp(event, "data") == 0) stdin_stop_reading();
  
  return this_obj;
}

static jsval_t js_stdout_write(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_false;
  size_t len = 0;
  char *data = js_getstr(js, args[0], &len);
  if (!data) return js_false;
  fwrite(data, 1, len, stdout);
  fflush(stdout);
  return js_true;
}

static jsval_t js_stdout_on(ant_t *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  if (nargs < 2) return this_obj;
  
  char *event = js_getstr(js, args[0], NULL);
  if (!event || vtype(args[1]) != T_FUNC) return this_obj;
  
  ProcessEventType *evt = find_or_create_stdout_event(event);
  if (!ensure_listener_capacity(evt)) return this_obj;
  
  evt->listeners[evt->listener_count].listener = args[1];
  evt->listeners[evt->listener_count].once = false;
  evt->listener_count++;
  
  if (strcmp(event, "resize") == 0) start_sigwinch_handler();
  
  return this_obj;
}

static jsval_t js_stdout_once(ant_t *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  if (nargs < 2) return this_obj;
  
  char *event = js_getstr(js, args[0], NULL);
  if (!event || vtype(args[1]) != T_FUNC) return this_obj;
  
  ProcessEventType *evt = find_or_create_stdout_event(event);
  if (!ensure_listener_capacity(evt)) return this_obj;
  
  evt->listeners[evt->listener_count].listener = args[1];
  evt->listeners[evt->listener_count].once = true;
  evt->listener_count++;
  
  if (strcmp(event, "resize") == 0) start_sigwinch_handler();
  
  return this_obj;
}

static jsval_t js_stdout_remove_all_listeners(ant_t *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  
  if (nargs < 1) {
    ProcessEventType *evt, *tmp;
    HASH_ITER(hh, stdout_events, evt, tmp) {
      evt->listener_count = 0;
    }
    return this_obj;
  }
  
  char *event = js_getstr(js, args[0], NULL);
  if (!event) return this_obj;
  
  ProcessEventType *evt = NULL;
  HASH_FIND_STR(stdout_events, event, evt);
  if (evt) evt->listener_count = 0;
  
  return this_obj;
}

static jsval_t js_stdout_remove_listener(ant_t *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  if (nargs < 2) return this_obj;
  
  char *event = js_getstr(js, args[0], NULL);
  if (!event) return this_obj;
  
  remove_listener_from_events(stdout_events, event, args[1]);
  return this_obj;
}

static jsval_t js_stdout_get_window_size(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  int rows = 0, cols = 0;
  get_tty_size(STDOUT_FILENO, &rows, &cols);
  jsval_t arr = js_mkarr(js);
  js_arr_push(js, arr, js_mknum(cols));
  js_arr_push(js, arr, js_mknum(rows));
  return arr;
}

static jsval_t js_stdout_rows_getter(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  int rows = 0, cols = 0;
  get_tty_size(STDOUT_FILENO, &rows, &cols);
  return js_mknum(rows);
}

static jsval_t js_stdout_columns_getter(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  int rows = 0, cols = 0;
  get_tty_size(STDOUT_FILENO, &rows, &cols);
  return js_mknum(cols);
}

static ProcessEventType *find_or_create_stderr_event(const char *event_type) {
  ProcessEventType *evt = NULL;
  HASH_FIND_STR(stderr_events, event_type, evt);
  if (evt == NULL) {
    evt = malloc(sizeof(ProcessEventType));
    evt->event_type = strdup(event_type);
    evt->listener_count = 0;
    evt->listener_capacity = INITIAL_LISTENER_CAPACITY;
    evt->listeners = malloc(sizeof(ProcessEventListener) * evt->listener_capacity);
    HASH_ADD_KEYPTR(hh, stderr_events, evt->event_type, strlen(evt->event_type), evt);
  }
  return evt;
}

static jsval_t js_stderr_write(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_false;
  size_t len = 0;
  char *data = js_getstr(js, args[0], &len);
  if (!data) return js_false;
  fwrite(data, 1, len, stderr);
  fflush(stderr);
  return js_true;
}

static jsval_t js_stderr_on(ant_t *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  if (nargs < 2) return this_obj;
  
  char *event = js_getstr(js, args[0], NULL);
  if (!event || vtype(args[1]) != T_FUNC) return this_obj;
  
  ProcessEventType *evt = find_or_create_stderr_event(event);
  if (!ensure_listener_capacity(evt)) return this_obj;
  
  evt->listeners[evt->listener_count].listener = args[1];
  evt->listeners[evt->listener_count].once = false;
  evt->listener_count++;
  
  return this_obj;
}

static jsval_t js_stderr_once(ant_t *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  if (nargs < 2) return this_obj;
  
  char *event = js_getstr(js, args[0], NULL);
  if (!event || vtype(args[1]) != T_FUNC) return this_obj;
  
  ProcessEventType *evt = find_or_create_stderr_event(event);
  if (!ensure_listener_capacity(evt)) return this_obj;
  
  evt->listeners[evt->listener_count].listener = args[1];
  evt->listeners[evt->listener_count].once = true;
  evt->listener_count++;
  
  return this_obj;
}

static jsval_t js_stderr_remove_all_listeners(ant_t *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  
  if (nargs < 1) {
    ProcessEventType *evt, *tmp;
    HASH_ITER(hh, stderr_events, evt, tmp) {
      evt->listener_count = 0;
    }
    return this_obj;
  }
  
  char *event = js_getstr(js, args[0], NULL);
  if (!event) return this_obj;
  
  ProcessEventType *evt = NULL;
  HASH_FIND_STR(stderr_events, event, evt);
  if (evt) evt->listener_count = 0;
  
  return this_obj;
}

static jsval_t js_stderr_remove_listener(ant_t *js, jsval_t *args, int nargs) {
  jsval_t this_obj = js_getthis(js);
  if (nargs < 2) return this_obj;
  
  char *event = js_getstr(js, args[0], NULL);
  if (!event) return this_obj;
  
  remove_listener_from_events(stderr_events, event, args[1]);
  return this_obj;
}

static jsval_t process_uptime(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  uint64_t now = uv_hrtime();
  double seconds = (double)(now - process_start_time) / 1e9;
  return js_mknum(seconds);
}

static jsval_t process_hrtime(ant_t *js, jsval_t *args, int nargs) {
  uint64_t now = uv_hrtime();
  
  if (nargs > 0 && vtype(args[0]) == T_ARR) {
    jsval_t prev_sec = js_get(js, args[0], "0");
    jsval_t prev_nsec = js_get(js, args[0], "1");
    if (vtype(prev_sec) == T_NUM && vtype(prev_nsec) == T_NUM) {
      uint64_t prev = (uint64_t)js_getnum(prev_sec) * 1000000000ULL + (uint64_t)js_getnum(prev_nsec);
      now = now - prev;
    }
  }
  
  jsval_t arr = js_mkarr(js);
  
  uint64_t secs = now / 1000000000ULL;
  uint64_t nsecs = now % 1000000000ULL;
  
  js_arr_push(js, arr, js_mknum((double)secs));
  js_arr_push(js, arr, js_mknum((double)nsecs));
  
  return arr;
}

static jsval_t process_hrtime_bigint(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  uint64_t now = uv_hrtime();
  char buf[32];
  snprintf(buf, sizeof(buf), "%llu", (unsigned long long)now);
  return js_mkbigint(js, buf, strlen(buf), false);
}

static jsval_t process_memory_usage(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t obj = js_mkobj(js);
  
  size_t rss = 0;
  uv_resident_set_memory(&rss);
  js_set(js, obj, "rss", js_mknum((double)rss));
  
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX pmc;
  if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
    js_set(js, obj, "heapTotal", js_mknum((double)pmc.WorkingSetSize));
    js_set(js, obj, "heapUsed", js_mknum((double)pmc.PrivateUsage));
  } else {
    js_set(js, obj, "heapTotal", js_mknum(0));
    js_set(js, obj, "heapUsed", js_mknum(0));
  }
#else
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    js_set(js, obj, "heapTotal", js_mknum((double)rss));
    js_set(js, obj, "heapUsed", js_mknum((double)rss));
  } else {
    js_set(js, obj, "heapTotal", js_mknum(0));
    js_set(js, obj, "heapUsed", js_mknum(0));
  }
#endif
  
  js_set(js, obj, "external", js_mknum(0));
  js_set(js, obj, "arrayBuffers", js_mknum(0));
  
  return obj;
}

static jsval_t process_memory_usage_rss(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  size_t rss = 0;
  uv_resident_set_memory(&rss);
  return js_mknum((double)rss);
}

static jsval_t process_cpu_usage(ant_t *js, jsval_t *args, int nargs) {
  jsval_t obj = js_mkobj(js);
  uv_rusage_t rusage;
  
  if (uv_getrusage(&rusage) == 0) {
    int64_t user_usec = rusage.ru_utime.tv_sec * 1000000LL + rusage.ru_utime.tv_usec;
    int64_t sys_usec = rusage.ru_stime.tv_sec * 1000000LL + rusage.ru_stime.tv_usec;
    
    if (nargs > 0 && is_special_object(args[0])) {
      jsval_t prev_user = js_get(js, args[0], "user");
      jsval_t prev_system = js_get(js, args[0], "system");
      if (vtype(prev_user) == T_NUM) user_usec -= (int64_t)js_getnum(prev_user);
      if (vtype(prev_system) == T_NUM) sys_usec -= (int64_t)js_getnum(prev_system);
    }
    
    js_set(js, obj, "user", js_mknum((double)user_usec));
    js_set(js, obj, "system", js_mknum((double)sys_usec));
  } else {
    js_set(js, obj, "user", js_mknum(0));
    js_set(js, obj, "system", js_mknum(0));
  }
  
  return obj;
}

static jsval_t process_kill(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "process.kill requires at least 1 argument");
  if (vtype(args[0]) != T_NUM) return js_mkerr(js, "pid must be a number");
  
  int pid = (int)js_getnum(args[0]);
  int sig = SIGTERM;
  
  if (nargs > 1) {
    if (vtype(args[1]) == T_NUM) {
      sig = (int)js_getnum(args[1]);
    } else if (vtype(args[1]) == T_STR) {
      char *sig_name = js_getstr(js, args[1], NULL);
      if (sig_name) {
        int signum = get_signal_number(sig_name);
        if (signum > 0) sig = signum;
        else return js_mkerr(js, "Unknown signal");
      }
    }
  }
  
  int result = uv_kill(pid, sig);
  if (result != 0) return js_mkerr(js, "Failed to send signal");
  return js_true;
}

static jsval_t process_abort(ant_t *js, jsval_t *args, int nargs) {
  (void)js; (void)args; (void)nargs;
  abort();
  return js_mkundef();
}

static jsval_t process_chdir(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "process.chdir requires 1 argument");
  
  char *dir = js_getstr(js, args[0], NULL);
  if (!dir) return js_mkerr(js, "directory must be a string");
  
  int result = uv_chdir(dir);
  if (result != 0) return js_mkerr(js, "ENOENT: no such file or directory, chdir");
  return js_mkundef();
}

static jsval_t process_umask(ant_t *js, jsval_t *args, int nargs) {
#ifdef _WIN32
  (void)args; (void)nargs;
  return js_mknum(0);
#else
  if (nargs > 0 && vtype(args[0]) == T_NUM) {
    int new_mask = (int)js_getnum(args[0]);
    int old_mask = umask((mode_t)new_mask);
    return js_mknum(old_mask);
  }
  int cur = umask(0);
  umask((mode_t)cur);
  return js_mknum(cur);
#endif
}

#ifndef _WIN32
static jsval_t process_getuid(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  return js_mknum((double)getuid());
}

static jsval_t process_geteuid(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  return js_mknum((double)geteuid());
}

static jsval_t process_getgid(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  return js_mknum((double)getgid());
}

static jsval_t process_getegid(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  return js_mknum((double)getegid());
}

static jsval_t process_getgroups(ant_t *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  int ngroups = getgroups(0, NULL);
  if (ngroups < 0) return js_mkarr(js);
  
  gid_t *groups = malloc(sizeof(gid_t) * (size_t)ngroups);
  if (!groups) return js_mkarr(js);
  
  ngroups = getgroups(ngroups, groups);
  jsval_t arr = js_mkarr(js);
  for (int i = 0; i < ngroups; i++) {
    js_arr_push(js, arr, js_mknum((double)groups[i]));
  }
  free(groups);
  return arr;
}

static jsval_t process_setuid(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "process.setuid requires 1 argument");
  
  uid_t uid;
  if (vtype(args[0]) == T_NUM) {
    uid = (uid_t)js_getnum(args[0]);
  } else if (vtype(args[0]) == T_STR) {
    char *name = js_getstr(js, args[0], NULL);
    struct passwd *pwd = getpwnam(name);
    if (!pwd) return js_mkerr(js, "setuid user not found");
    uid = pwd->pw_uid;
  } else {
    return js_mkerr(js, "uid must be a number or string");
  }
  
  if (setuid(uid) != 0) return js_mkerr(js, "setuid failed");
  return js_mkundef();
}

static jsval_t process_setgid(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "process.setgid requires 1 argument");
  
  gid_t gid;
  if (vtype(args[0]) == T_NUM) {
    gid = (gid_t)js_getnum(args[0]);
  } else if (vtype(args[0]) == T_STR) {
    char *name = js_getstr(js, args[0], NULL);
    struct group *grp = getgrnam(name);
    if (!grp) return js_mkerr(js, "setgid group not found");
    gid = grp->gr_gid;
  } else {
    return js_mkerr(js, "gid must be a number or string");
  }
  
  if (setgid(gid) != 0) return js_mkerr(js, "setgid failed");
  return js_mkundef();
}

static jsval_t process_seteuid(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "process.seteuid requires 1 argument");
  
  uid_t uid;
  if (vtype(args[0]) == T_NUM) {
    uid = (uid_t)js_getnum(args[0]);
  } else if (vtype(args[0]) == T_STR) {
    char *name = js_getstr(js, args[0], NULL);
    struct passwd *pwd = getpwnam(name);
    if (!pwd) return js_mkerr(js, "seteuid user not found");
    uid = pwd->pw_uid;
  } else {
    return js_mkerr(js, "uid must be a number or string");
  }
  
  if (seteuid(uid) != 0) return js_mkerr(js, "seteuid failed");
  return js_mkundef();
}

static jsval_t process_setegid(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "process.setegid requires 1 argument");
  
  gid_t gid;
  if (vtype(args[0]) == T_NUM) {
    gid = (gid_t)js_getnum(args[0]);
  } else if (vtype(args[0]) == T_STR) {
    char *name = js_getstr(js, args[0], NULL);
    struct group *grp = getgrnam(name);
    if (!grp) return js_mkerr(js, "setegid group not found");
    gid = grp->gr_gid;
  } else {
    return js_mkerr(js, "gid must be a number or string");
  }
  
  if (setegid(gid) != 0) return js_mkerr(js, "setegid failed");
  return js_mkundef();
}

static jsval_t process_setgroups(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_ARR) {
    return js_mkerr(js, "process.setgroups requires an array");
  }
  
  jsval_t len_val = js_get(js, args[0], "length");
  int len = (int)js_getnum(len_val);
  
  gid_t *groups = malloc(sizeof(gid_t) * (size_t)len);
  if (!groups) return js_mkerr(js, "allocation failed");
  
  for (int i = 0; i < len; i++) {
    char idx[16];
    snprintf(idx, sizeof(idx), "%d", i);
    jsval_t val = js_get(js, args[0], idx);
    if (vtype(val) == T_NUM) {
      groups[i] = (gid_t)js_getnum(val);
    } else if (vtype(val) == T_STR) {
      char *name = js_getstr(js, val, NULL);
      struct group *grp = getgrnam(name);
      if (!grp) { free(groups); return js_mkerr(js, "group not found"); }
      groups[i] = grp->gr_gid;
    } else {
      free(groups);
      return js_mkerr(js, "group id must be number or string");
    }
  }
  
  if (setgroups(len, groups) != 0) {
    free(groups);
    return js_mkerr(js, "setgroups failed");
  }
  free(groups);
  return js_mkundef();
}

static jsval_t process_initgroups(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "process.initgroups requires 2 arguments");
  
  char *user = js_getstr(js, args[0], NULL);
  if (!user) return js_mkerr(js, "user must be a string");
  
  gid_t gid;
  if (vtype(args[1]) == T_NUM) {
    gid = (gid_t)js_getnum(args[1]);
  } else if (vtype(args[1]) == T_STR) {
    char *name = js_getstr(js, args[1], NULL);
    struct group *grp = getgrnam(name);
    if (!grp) return js_mkerr(js, "group not found");
    gid = grp->gr_gid;
  } else {
    return js_mkerr(js, "gid must be a number or string");
  }
  
  if (initgroups(user, gid) != 0) return js_mkerr(js, "initgroups failed");
  return js_mkundef();
}
#endif

static jsval_t env_getter(ant_t *js, jsval_t obj, const char *key, size_t key_len) {
  CSTR_BUF(buf, 256);
  char *key_str = CSTR_INIT(buf, key, key_len);
  if (!key_str) return js_mkundef();
  
  char *value = getenv(key_str);
  cstr_free(&buf);
  
  if (value == NULL) return js_mkundef();
  return js_mkstr(js, value, strlen(value));
}

static bool env_setter(ant_t *js, jsval_t obj, const char *key, size_t key_len, jsval_t value) {
  jsval_t str_val = coerce_to_str(js, value);
  if (is_err(str_val)) return false;
  setprop_cstr(js, obj, key, key_len, str_val);
  
  CSTR_BUF(buf, 256);
  char *key_str = CSTR_INIT(buf, key, key_len);
  if (key_str) {
    size_t val_len;
    char *val_str = js_getstr(js, str_val, &val_len);
    if (val_str) setenv(key_str, val_str, 1);
    cstr_free(&buf);
  }
  
  return true;
}

static bool env_deleter(ant_t *js, jsval_t obj, const char *key, size_t key_len) {
  CSTR_BUF(buf, 256);
  char *key_str = CSTR_INIT(buf, key, key_len);
  if (key_str) { unsetenv(key_str); cstr_free(&buf); }
  return true;
}

static void load_dotenv_file(ant_t *js, jsval_t env_obj) {
  FILE *fp = fopen(".env", "r");
  if (fp == NULL) return;
  
  char line[1024];
  while (fgets(line, sizeof(line), fp) != NULL) {
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') {
      line[len - 1] = '\0';
      len--;
    }
    if (len > 0 && line[len - 1] == '\r') {
      line[len - 1] = '\0';
      len--;
    }
    
    if (len == 0 || line[0] == '#') continue;
    char *equals = strchr(line, '=');
    if (equals == NULL) continue;
    
    *equals = '\0';
    char *key = line;
    char *value = equals + 1;
    
    while (*key == ' ' || *key == '\t') key++;
    char *key_end = key + strlen(key) - 1;
    while (key_end > key && (*key_end == ' ' || *key_end == '\t')) {
      *key_end = '\0';
      key_end--;
    }
    
    while (*value == ' ' || *value == '\t') value++;
    char *value_end = value + strlen(value) - 1;
    while (value_end > value && (*value_end == ' ' || *value_end == '\t')) {
      *value_end = '\0';
      value_end--;
    }
    
    if (strlen(value) >= 2 && 
        ((value[0] == '"' && value[strlen(value) - 1] == '"') 
        || (value[0] == '\'' && value[strlen(value) - 1] == '\''))) {
      value[strlen(value) - 1] = '\0';
      value++;
    }
    
    js_set(js, env_obj, key, js_mkstr(js, value, strlen(value)));
  }
  
  fclose(fp);
}

static jsval_t process_exit(ant_t *js, jsval_t *args, int nargs) {
  int code = 0;
  
  if (nargs > 0 && vtype(args[0]) == T_NUM) {
    code = (int)js_getnum(args[0]);
  }
  
  exit(code);
  return js_mkundef();
}

typedef struct {
  char *buf;
  size_t pos;
  size_t cap;
} env_str_ctx;

typedef void (*env_iter_cb)(
  ant_t *js,
  const char *key,
  size_t key_len,
  const char *value,
  size_t val_len,
  void *ctx
);

static void env_foreach(ant_t *js, jsval_t env_obj, env_iter_cb cb, void *ctx) {
  for (char **env = environ; *env != NULL; env++) {
    char *entry = *env;
    char *equals = strchr(entry, '=');
    if (equals == NULL) continue;
    
    size_t key_len = (size_t)(equals - entry);
    char *value = equals + 1;
    cb(js, entry, key_len, value, strlen(value), ctx);
  }
  
  ant_iter_t iter = js_prop_iter_begin(js, env_obj);
  const char *key; size_t key_len; jsval_t value;
  
  while (js_prop_iter_next(&iter, &key, &key_len, &value)) {
    if (key_len >= 2 && key[0] == '_' && key[1] == '_') continue;
    if (vtype(value) != T_STR) continue;
    
    CSTR_BUF(buf, 256);
    char *key_str = CSTR_INIT(buf, key, key_len);
    if (!key_str) continue;
    
    if (getenv(key_str)) {
      cstr_free(&buf);
      continue;
    } cstr_free(&buf);
    
    size_t val_len;
    char *val_str = js_getstr(js, value, &val_len);
    cb(js, key, key_len, val_str ? val_str : "", val_str ? val_len : 0, ctx);
  }
  
  js_prop_iter_end(&iter);
}

static void env_to_object_cb(ant_t *js, const char *key, size_t key_len, const char *value, size_t val_len, void *ctx) {
  jsval_t obj = *(jsval_t *)ctx;
  CSTR_BUF(buf, 256);
  char *key_str = CSTR_INIT(buf, key, key_len);
  if (!key_str) return;
  js_set(js, obj, key_str, js_mkstr(js, value, val_len));
  cstr_free(&buf);
}

static jsval_t env_to_object(ant_t *js, jsval_t *args, int nargs) {
  jsval_t obj = js_mkobj(js);
  env_foreach(js, js->this_val, env_to_object_cb, &obj);
  return obj;
}

static void env_tostring_cb(ant_t *js, const char *key, size_t key_len, const char *value, size_t val_len, void *ctx) {
  env_str_ctx *c = ctx;
  size_t entry_len = key_len + 1 + val_len;
  
  if (c->pos + entry_len + 2 >= c->cap) {
    size_t new_cap = c->cap * 2 + entry_len;
    char *new_buf = realloc(c->buf, new_cap);
    if (!new_buf) return;
    c->buf = new_buf;
    c->cap = new_cap;
  }
  
  if (c->pos > 0) c->buf[c->pos++] = '\n';
  memcpy(c->buf + c->pos, key, key_len);
  c->pos += key_len;
  c->buf[c->pos++] = '=';
  memcpy(c->buf + c->pos, value, val_len);
  c->pos += val_len;
}

static jsval_t env_toString(ant_t *js, jsval_t *args, int nargs) {
  env_str_ctx ctx = { .buf = malloc(4096), .pos = 0, .cap = 4096 };
  if (!ctx.buf) return js_mkstr(js, "", 0);
  
  env_foreach(js, js->this_val, env_tostring_cb, &ctx);
  ctx.buf[ctx.pos] = '\0';
  
  jsval_t ret = js_mkstr(js, ctx.buf, ctx.pos);
  free(ctx.buf);
  return ret;
}

static void env_keys_cb(ant_t *js, const char *key, size_t key_len, const char *value, size_t val_len, void *ctx) {
  jsval_t arr = *(jsval_t *)ctx;
  js_arr_push(js, arr, js_mkstr(js, key, key_len));
}

static jsval_t env_keys(ant_t *js, jsval_t obj) {
  jsval_t arr = js_mkarr(js);
  env_foreach(js, obj, env_keys_cb, &arr);
  return arr;
}

static jsval_t process_cwd(ant_t *js, jsval_t *args, int nargs) {
  char cwd[4096];
  if (getcwd(cwd, sizeof(cwd)) != NULL) {
    return js_mkstr(js, cwd, strlen(cwd));
  }
  return js_mkundef();
}

static jsval_t process_on(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "process.on requires 2 arguments");
  
  char *event = js_getstr(js, args[0], NULL);
  if (!event) return js_mkerr(js, "event must be a string");
  if (vtype(args[1]) != T_FUNC) return js_mkerr(js, "listener must be a function");
  
  int signum = get_signal_number(event);
  if (signum > 0) {
    signal(signum, process_signal_handler);
  }
  
  ProcessEventType *evt = find_or_create_event_type(event);
  if (!ensure_listener_capacity(evt)) {
    return js_mkerr(js, "failed to allocate listener");
  }
  
  evt->listeners[evt->listener_count].listener = args[1];
  evt->listeners[evt->listener_count].once = false;
  evt->listener_count++;
  
  check_listener_warning(event);
  
  return js_get(js, js_glob(js), "process");
}

static jsval_t process_once(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "process.once requires 2 arguments");
  
  char *event = js_getstr(js, args[0], NULL);
  if (!event) return js_mkerr(js, "event must be a string");
  if (vtype(args[1]) != T_FUNC) return js_mkerr(js, "listener must be a function");
  
  int signum = get_signal_number(event);
  if (signum > 0) {
    signal(signum, process_signal_handler);
  }
  
  ProcessEventType *evt = find_or_create_event_type(event);
  if (!ensure_listener_capacity(evt)) {
    return js_mkerr(js, "failed to allocate listener");
  }
  
  evt->listeners[evt->listener_count].listener = args[1];
  evt->listeners[evt->listener_count].once = true;
  evt->listener_count++;
  
  check_listener_warning(event);
  
  return js_get(js, js_glob(js), "process");
}

static jsval_t process_off(ant_t *js, jsval_t *args, int nargs) {
  jsval_t process_obj = js_get(js, js_glob(js), "process");
  if (nargs < 2) return process_obj;
  
  char *event = js_getstr(js, args[0], NULL);
  if (!event) return process_obj;
  
  ProcessEventType *evt = NULL;
  HASH_FIND_STR(process_events, event, evt);
  if (!evt) return process_obj;
  
  for (int i = 0; i < evt->listener_count; i++) {
    if (evt->listeners[i].listener == args[1]) {
      for (int j = i; j < evt->listener_count - 1; j++) {
        evt->listeners[j] = evt->listeners[j + 1];
      } evt->listener_count--;
      break;
    }
  }
  
  if (evt->listener_count == 0) {
    int signum = get_signal_number(event);
    if (signum > 0) signal(signum, SIG_DFL);
  }
  
  return process_obj;
}

static jsval_t process_remove_all_listeners(ant_t *js, jsval_t *args, int nargs) {
  jsval_t process_obj = js_get(js, js_glob(js), "process");
  
  if (nargs > 0 && vtype(args[0]) == T_STR) {
    char *event = js_getstr(js, args[0], NULL);
    if (event) {
      ProcessEventType *evt = NULL;
      HASH_FIND_STR(process_events, event, evt);
      if (evt) {
        evt->listener_count = 0;
        int signum = get_signal_number(event);
        if (signum > 0) signal(signum, SIG_DFL);
      }
    }
  } else {
    ProcessEventType *evt, *tmp;
    HASH_ITER(hh, process_events, evt, tmp) {
      int signum = get_signal_number(evt->event_type);
      if (signum > 0) signal(signum, SIG_DFL);
      evt->listener_count = 0;
    }
  }
  
  return process_obj;
}

static jsval_t process_emit(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_false;
  
  char *event = js_getstr(js, args[0], NULL);
  if (!event) return js_false;
  
  emit_process_event(event, nargs > 1 ? &args[1] : NULL, nargs - 1);
  return js_true;
}

static jsval_t process_listener_count(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mknum(0);
  
  char *event = js_getstr(js, args[0], NULL);
  if (!event) return js_mknum(0);
  
  ProcessEventType *evt = NULL;
  HASH_FIND_STR(process_events, event, evt);
  
  return js_mknum(evt ? evt->listener_count : 0);
}

static jsval_t process_set_max_listeners(ant_t *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "setMaxListeners requires 1 argument");
  if (vtype(args[0]) != T_NUM) return js_mkerr(js, "n must be a number");
  
  int n = (int)js_getnum(args[0]);
  if (n < 0) return js_mkerr(js, "n must be non-negative");
  
  max_listeners = n;
  return js_get(js, js_glob(js), "process");
}

static jsval_t process_get_max_listeners(ant_t *js, jsval_t *args, int nargs) {
  return js_mknum(max_listeners);
}

void init_process_module() {
  ant_t *js = rt->js;
  jsval_t global = js_glob(js);
  
  process_start_time = uv_hrtime();
  jsval_t process_proto = js_mkobj(js);
  
  js_set(js, process_proto, "exit", js_mkfun(process_exit));
  js_set(js, process_proto, "on", js_mkfun(process_on));
  js_set(js, process_proto, "addListener", js_mkfun(process_on));
  js_set(js, process_proto, "once", js_mkfun(process_once));
  js_set(js, process_proto, "off", js_mkfun(process_off));
  js_set(js, process_proto, "removeListener", js_mkfun(process_off));
  js_set(js, process_proto, "removeAllListeners", js_mkfun(process_remove_all_listeners));
  js_set(js, process_proto, "emit", js_mkfun(process_emit));
  js_set(js, process_proto, "listenerCount", js_mkfun(process_listener_count));
  js_set(js, process_proto, "setMaxListeners", js_mkfun(process_set_max_listeners));
  js_set(js, process_proto, "getMaxListeners", js_mkfun(process_get_max_listeners));
  js_set(js, process_proto, "cwd", js_mkfun(process_cwd));
  js_set(js, process_proto, "chdir", js_mkfun(process_chdir));
  js_set(js, process_proto, "uptime", js_mkfun(process_uptime));
  js_set(js, process_proto, "cpuUsage", js_mkfun(process_cpu_usage));
  js_set(js, process_proto, "kill", js_mkfun(process_kill));
  js_set(js, process_proto, "abort", js_mkfun(process_abort));
  js_set(js, process_proto, "umask", js_mkfun(process_umask));
  
  jsval_t mem_usage_fn = js_heavy_mkfun(js, process_memory_usage, js_mkundef());
  js_set(js, mem_usage_fn, "rss", js_mkfun(process_memory_usage_rss));
  js_set(js, process_proto, "memoryUsage", mem_usage_fn);
  
  jsval_t hrtime_fn = js_heavy_mkfun(js, process_hrtime, js_mkundef());
  js_set(js, hrtime_fn, "bigint", js_mkfun(process_hrtime_bigint));
  js_set(js, process_proto, "hrtime", hrtime_fn);
  
#ifndef _WIN32
  js_set(js, process_proto, "getuid", js_mkfun(process_getuid));
  js_set(js, process_proto, "geteuid", js_mkfun(process_geteuid));
  js_set(js, process_proto, "getgid", js_mkfun(process_getgid));
  js_set(js, process_proto, "getegid", js_mkfun(process_getegid));
  js_set(js, process_proto, "getgroups", js_mkfun(process_getgroups));
  js_set(js, process_proto, "setuid", js_mkfun(process_setuid));
  js_set(js, process_proto, "setgid", js_mkfun(process_setgid));
  js_set(js, process_proto, "seteuid", js_mkfun(process_seteuid));
  js_set(js, process_proto, "setegid", js_mkfun(process_setegid));
  js_set(js, process_proto, "setgroups", js_mkfun(process_setgroups));
  js_set(js, process_proto, "initgroups", js_mkfun(process_initgroups));
#endif
  
  js_set(js, process_proto, get_toStringTag_sym_key(), js_mkstr(js, "process", 7));
  
  jsval_t process_obj = js_mkobj(js);
  jsval_t env_obj = js_mkobj(js);
  js_set_proto(js, process_obj, process_proto);

  load_dotenv_file(js, env_obj);
  js_set_keys(js, env_obj, env_keys);

  js_set_getter(js, env_obj, env_getter);
  js_set_setter(js, env_obj, env_setter);
  js_set_deleter(js, env_obj, env_deleter);
  
  js_set(js, env_obj, "toObject", js_mkfun(env_to_object));
  js_set(js, env_obj, "toString", js_mkfun(env_toString));
  js_set(js, process_obj, "env", env_obj);
  
  jsval_t argv_arr = js_mkarr(js);
  for (int i = 0; i < rt->argc; i++) {
    js_arr_push(js, argv_arr, js_mkstr(js, rt->argv[i], strlen(rt->argv[i])));
  }
  
  js_set(js, process_obj, "argv", argv_arr);
  js_set(js, process_obj, "execArgv", js_mkarr(js));
  js_set(js, process_obj, "argv0", rt->argc > 0 ? js_mkstr(js, rt->argv[0], strlen(rt->argv[0])) : js_mkstr(js, "ant", 3));
  js_set(js, process_obj, "execPath", rt->argc > 0 ? js_mkstr(js, rt->argv[0], strlen(rt->argv[0])) : js_mkundef());
  
  js_set(js, process_obj, "pid", js_mknum((double)getpid()));
  js_set(js, process_obj, "ppid", js_mknum((double)getppid()));
  
  char version_str[128];
  snprintf(version_str, sizeof(version_str), "v%s", ANT_VERSION);
  js_set(js, process_obj, "version", js_mkstr(js, version_str, strlen(version_str)));
  
  jsval_t versions_obj = js_mkobj(js);
  js_set(js, versions_obj, "ant", js_mkstr(js, ANT_VERSION, strlen(ANT_VERSION)));
  char uv_ver[32];
  snprintf(uv_ver, sizeof(uv_ver), "%d.%d.%d", UV_VERSION_MAJOR, UV_VERSION_MINOR, UV_VERSION_PATCH);
  js_set(js, versions_obj, "uv", js_mkstr(js, uv_ver, strlen(uv_ver)));
  js_set(js, process_obj, "versions", versions_obj);
  
  jsval_t release_obj = js_mkobj(js);
  js_set(js, release_obj, "name", js_mkstr(js, "ant", 3));
  js_set(js, process_obj, "release", release_obj);
  
  // process.platform
  #if defined(__APPLE__)
    js_set(js, process_obj, "platform", js_mkstr(js, "darwin", 6));
  #elif defined(__linux__)
    js_set(js, process_obj, "platform", js_mkstr(js, "linux", 5));
  #elif defined(_WIN32) || defined(_WIN64)
    js_set(js, process_obj, "platform", js_mkstr(js, "win32", 5));
  #elif defined(__FreeBSD__)
    js_set(js, process_obj, "platform", js_mkstr(js, "freebsd", 7));
  #else
    js_set(js, process_obj, "platform", js_mkstr(js, "unknown", 7));
  #endif
  
  // process.arch
  #if defined(__x86_64__) || defined(_M_X64)
    js_set(js, process_obj, "arch", js_mkstr(js, "x64", 3));
  #elif defined(__i386__) || defined(_M_IX86)
    js_set(js, process_obj, "arch", js_mkstr(js, "ia32", 4));
  #elif defined(__aarch64__) || defined(_M_ARM64)
    js_set(js, process_obj, "arch", js_mkstr(js, "arm64", 5));
  #elif defined(__arm__) || defined(_M_ARM)
    js_set(js, process_obj, "arch", js_mkstr(js, "arm", 3));
  #else
    js_set(js, process_obj, "arch", js_mkstr(js, "unknown", 7));
  #endif
  
  jsval_t stdin_proto = js_mkobj(js);
  js_set(js, stdin_proto, "setRawMode", js_mkfun(js_stdin_set_raw_mode));
  js_set(js, stdin_proto, "resume", js_mkfun(js_stdin_resume));
  js_set(js, stdin_proto, "pause", js_mkfun(js_stdin_pause));
  js_set(js, stdin_proto, "on", js_mkfun(js_stdin_on));
  js_set(js, stdin_proto, "removeListener", js_mkfun(js_stdin_remove_listener));
  js_set(js, stdin_proto, "off", js_mkfun(js_stdin_remove_listener));
  js_set(js, stdin_proto, "removeAllListeners", js_mkfun(js_stdin_remove_all_listeners));
  js_set(js, stdin_proto, get_toStringTag_sym_key(), js_mkstr(js, "ReadStream", 10));
  
  jsval_t stdin_obj = js_mkobj(js);
  js_set_proto(js, stdin_obj, stdin_proto);
  js_set(js, stdin_obj, "isTTY", js_bool(stdin_is_tty()));
  js_set(js, process_obj, "stdin", stdin_obj);
  
  jsval_t stdout_proto = js_mkobj(js);
  js_set(js, stdout_proto, "write", js_mkfun(js_stdout_write));
  js_set(js, stdout_proto, "on", js_mkfun(js_stdout_on));
  js_set(js, stdout_proto, "once", js_mkfun(js_stdout_once));
  js_set(js, stdout_proto, "removeListener", js_mkfun(js_stdout_remove_listener));
  js_set(js, stdout_proto, "off", js_mkfun(js_stdout_remove_listener));
  js_set(js, stdout_proto, "removeAllListeners", js_mkfun(js_stdout_remove_all_listeners));
  js_set(js, stdout_proto, "getWindowSize", js_mkfun(js_stdout_get_window_size));
  js_set(js, stdout_proto, get_toStringTag_sym_key(), js_mkstr(js, "WriteStream", 11));
  
  jsval_t stdout_obj = js_mkobj(js);
  js_set_proto(js, stdout_obj, stdout_proto);
  js_set(js, stdout_obj, "isTTY", js_bool(stdout_is_tty()));
  js_set_getter_desc(js, stdout_obj, "rows", 4, js_mkfun(js_stdout_rows_getter), JS_DESC_E | JS_DESC_C);
  js_set_getter_desc(js, stdout_obj, "columns", 7, js_mkfun(js_stdout_columns_getter), JS_DESC_E | JS_DESC_C);
  js_set(js, process_obj, "stdout", stdout_obj);
  
  jsval_t stderr_proto = js_mkobj(js);
  js_set(js, stderr_proto, "write", js_mkfun(js_stderr_write));
  js_set(js, stderr_proto, "on", js_mkfun(js_stderr_on));
  js_set(js, stderr_proto, "once", js_mkfun(js_stderr_once));
  js_set(js, stderr_proto, "removeListener", js_mkfun(js_stderr_remove_listener));
  js_set(js, stderr_proto, "off", js_mkfun(js_stderr_remove_listener));
  js_set(js, stderr_proto, "removeAllListeners", js_mkfun(js_stderr_remove_all_listeners));
  js_set(js, stderr_proto, get_toStringTag_sym_key(), js_mkstr(js, "WriteStream", 11));
  
  jsval_t stderr_obj = js_mkobj(js);
  js_set_proto(js, stderr_obj, stderr_proto);
  js_set(js, stderr_obj, "isTTY", js_bool(stderr_is_tty()));
  js_set(js, process_obj, "stderr", stderr_obj);
  
  js_set(js, global, "process", process_obj);
}

#define GC_OP_EVENTS(events) do { \
  ProcessEventType *evt, *tmp; \
  HASH_ITER(hh, events, evt, tmp) { \
    for (int i = 0; i < evt->listener_count; i++) \
      op_val(ctx, &evt->listeners[i].listener); \
  } \
} while(0)

void process_gc_update_roots(GC_OP_VAL_ARGS) {
  GC_OP_EVENTS(process_events);
  GC_OP_EVENTS(stdin_events);
  GC_OP_EVENTS(stdout_events);
  GC_OP_EVENTS(stderr_events);
}

#undef GC_OP_EVENTS

bool has_active_stdin(void) { 
  return stdin_state.reading; 
}

void process_enable_keypress_events(void) {
  stdin_state.keypress_enabled = true;
  stdin_state.escape_state = 0;
  stdin_state.escape_len = 0;
}
