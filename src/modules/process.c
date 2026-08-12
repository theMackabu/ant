#include <compat.h> // IWYU pragma: keep

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <uthash.h>
#include <uv.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
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
#include "output.h"
#include "utils.h"
#include "tty_ctrl.h"
#include "internal.h"
#include "descriptors.h"
#include "silver/engine.h"
#include "gc/modules.h"

#include "modules/events.h"
#include "modules/process.h"
#include "sandbox/sandbox.h"
#include "modules/tty.h"
#include "modules/symbol.h"
#include "modules/buffer.h"
#include "modules/napi.h"
#include "modules/timer.h"
#include "modules/string_decoder.h"
#include "modules/fs.h"

#ifndef _WIN32
extern char **environ;
#else
#define environ _environ
#endif

typedef struct {
  uv_signal_t handle;
  int signum;
  UT_hash_handle hh;
} SignalHandle;

typedef struct {
  uv_tty_t tty;
  bool tty_initialized;
  bool reading;
  bool paused;
  bool keypress_enabled;
  ant_value_t decoder;
  int escape_state;
  int escape_len;
  char escape_buf[16];
} stdin_state_t;

struct ant_process_state {
  ant_value_t process_obj;
  ant_value_t stdin_obj;
  ant_value_t stdout_obj;
  ant_value_t stderr_obj;
  SignalHandle *signal_handles;

  stdin_state_t stdin_state;
  uint64_t process_start_time;

  stdin_byte_consumer_fn stdin_byte_consumer;
  stdin_eof_fn stdin_byte_eof;

  uint32_t sandbox_terminal_capabilities;
  uint16_t sandbox_tty_rows;
  uint16_t sandbox_tty_cols;
  bool sandbox_terminal_enabled;

  #ifndef _WIN32
  uv_signal_t sigwinch_handle;
  bool sigwinch_initialized;
  bool sigwinch_closing;
  bool sigwinch_requested;
  #endif
};

static ant_process_state_t *process_state(ant_t *js) {
  if (js->process_state) return js->process_state;

  ant_process_state_t *ps = calloc(1, sizeof(*ps));
  if (!ps) return NULL;

  ps->sandbox_tty_rows = 24;
  ps->sandbox_tty_cols = 80;
  ps->stdin_state.decoder = js_mkundef();

  js->process_state = ps;
  return ps;
}

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
  
  #define S(sig) { #sig, sig, {0}, {0} }
  static SignalEntry entries[] = {
    S(SIGINT),    S(SIGILL),    S(SIGABRT),   S(SIGFPE),
    S(SIGSEGV),   S(SIGTERM),
    #ifdef SIGHUP
      S(SIGHUP),
    #endif
    #ifdef SIGQUIT
      S(SIGQUIT),
    #endif
    #ifdef SIGTRAP
      S(SIGTRAP),
    #endif
    #ifdef SIGKILL
      S(SIGKILL),
    #endif
    #ifdef SIGBUS
      S(SIGBUS),
    #endif
    #ifdef SIGSYS
      S(SIGSYS),
    #endif
    #ifdef SIGPIPE
      S(SIGPIPE),
    #endif
    #ifdef SIGALRM
      S(SIGALRM),
    #endif
    #ifdef SIGURG
      S(SIGURG),
    #endif
    #ifdef SIGSTOP
      S(SIGSTOP),
    #endif
    #ifdef SIGTSTP
      S(SIGTSTP),
    #endif
    #ifdef SIGCONT
      S(SIGCONT),
    #endif
    #ifdef SIGCHLD
      S(SIGCHLD),
    #endif
    #ifdef SIGTTIN
      S(SIGTTIN),
    #endif
    #ifdef SIGTTOU
      S(SIGTTOU),
    #endif
    #ifdef SIGXCPU
      S(SIGXCPU),
    #endif
    #ifdef SIGXFSZ
      S(SIGXFSZ),
    #endif
    #ifdef SIGVTALRM
      S(SIGVTALRM),
    #endif
    #ifdef SIGPROF
      S(SIGPROF),
    #endif
    #ifdef SIGUSR1
      S(SIGUSR1),
    #endif
    #ifdef SIGUSR2
      S(SIGUSR2),
    #endif
    #ifdef SIGEMT
      S(SIGEMT),
    #endif
    #ifdef SIGIO
      S(SIGIO),
    #endif
    #ifdef SIGWINCH
      S(SIGWINCH),
    #endif
    #ifdef SIGINFO
      S(SIGINFO),
    #endif
    #ifdef SIGPWR
      S(SIGPWR),
    #endif
    #ifdef SIGSTKFLT
      S(SIGSTKFLT),
    #endif
    #ifdef SIGPOLL
      S(SIGPOLL),
    #endif
  };
  #undef S
  
  size_t count = sizeof(entries) / sizeof(entries[0]);
  for (size_t i = 0; i < count; i++) {
    SignalEntry *existing = NULL;
    HASH_ADD_KEYPTR(hh_name, signals_by_name, entries[i].name, strlen(entries[i].name), &entries[i]);
    HASH_FIND(hh_num, signals_by_num, &entries[i].signum, sizeof(int), existing);
    if (!existing) HASH_ADD(hh_num, signals_by_num, signum, sizeof(int), &entries[i]);
  }
  
  initialized = true;
}

int process_signal_number(const char *name) {
  if (!name || name[0] != 'S') return -1;
  init_signal_map();
  SignalEntry *entry = NULL;
  HASH_FIND(hh_name, signals_by_name, name, strlen(name), entry);
  return entry ? entry->signum : -1;
}

const char *process_signal_name(int signum) {
  init_signal_map();
  SignalEntry *entry = NULL;
  HASH_FIND(hh_num, signals_by_num, &signum, sizeof(int), entry);
  return entry ? entry->name : NULL;
}

static void uv_signal_handler(uv_signal_t *handle, int signum) {
  ant_t *js = handle->data;
  const char *name = process_signal_name(signum);
  if (name) {
    ant_value_t sig_arg = js_mkstr(js, name, strlen(name));
    emit_process_event(js, name, &sig_arg, 1);
  }
}

static void start_signal_watch(ant_t *js, int signum) {
  ant_process_state_t *ps = process_state(js);
  SignalHandle *sh = NULL;
  if (!ps) return;
  HASH_FIND_INT(ps->signal_handles, &signum, sh);
  if (sh) return;

  sh = malloc(sizeof(SignalHandle));
  sh->signum = signum;
  uv_signal_init(uv_default_loop(), &sh->handle);
  sh->handle.data = js;
  uv_signal_start(&sh->handle, uv_signal_handler, signum);
  uv_unref((uv_handle_t *)&sh->handle);
  HASH_ADD_INT(ps->signal_handles, signum, sh);
}

static void on_signal_handle_close(uv_handle_t *handle) {
  SignalHandle *sh = (SignalHandle *)handle;
  free(sh);
}

static void stop_signal_watch(ant_t *js, int signum) {
  ant_process_state_t *ps = process_state(js);
  SignalHandle *sh = NULL;
  if (!ps) return;
  HASH_FIND_INT(ps->signal_handles, &signum, sh);
  if (!sh) return;
  
  HASH_DEL(ps->signal_handles, sh);
  uv_signal_stop(&sh->handle);
  uv_close((uv_handle_t *)&sh->handle, on_signal_handle_close);
}


void emit_process_event(ant_t *js, const char *event_type, ant_value_t *args, int nargs) {
  ant_process_state_t *ps = js->process_state;
  if (!ps || !is_object_type(ps->process_obj)) return;
  eventemitter_emit_args(js, ps->process_obj, event_type, args, nargs);
}

bool process_has_event_listeners(ant_t *js, const char *event_type) {
  ant_process_state_t *ps = js->process_state;
  if (!ps || !event_type || !is_object_type(ps->process_obj)) return false;
  return eventemitter_listener_count(js, ps->process_obj, event_type) > 0;
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
  ant_t *js,
  const char *str,
  size_t str_len,
  const char *name,
  bool ctrl,
  bool meta,
  bool shift,
  const char *sequence,
  size_t sequence_len
) {
  ant_process_state_t *ps = process_state(js);
  if (!ps) return;
  ant_value_t str_val = js_mkstr(js, str ? str : "", str ? str_len : 0);
  ant_value_t key_obj = js_mkobj(js);

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

  ant_value_t args[2] = { str_val, key_obj };
  eventemitter_emit_args(js, ps->stdin_obj, "keypress", args, 2);
}

static void process_keypress_data(ant_t *js, const char *data, size_t len) {
  ant_process_state_t *ps = process_state(js);
  if (!ps) return;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)data[i];

    if (ps->stdin_state.escape_state == 1) {
      ps->stdin_state.escape_buf[ps->stdin_state.escape_len++] = (char)c;
      if (c == '[' || c == 'O') {
        ps->stdin_state.escape_state = 2;
        continue;
      }

      emit_keypress_event(js, "\x1b", 1, "escape", false, false, false, "\x1b", 1);
      ps->stdin_state.escape_state = 0;
      ps->stdin_state.escape_len = 0;
    }

    if (ps->stdin_state.escape_state == 2) {
      ps->stdin_state.escape_buf[ps->stdin_state.escape_len++] = (char)c;
      if ((c >= 'A' && c <= 'Z') || c == '~' || ps->stdin_state.escape_len >= 15) {
        char sequence[18];
        size_t seq_len = 0;
        sequence[seq_len++] = '\x1b';
        memcpy(sequence + seq_len, ps->stdin_state.escape_buf, (size_t)ps->stdin_state.escape_len);
        seq_len += (size_t)ps->stdin_state.escape_len;

        const char *name = stdin_escape_name(ps->stdin_state.escape_buf, ps->stdin_state.escape_len);
        if (!name) name = "escape";

        emit_keypress_event(js, "", 0, name, false, false, false, sequence, seq_len);
        ps->stdin_state.escape_state = 0;
        ps->stdin_state.escape_len = 0;
      }
      continue;
    }

    if (c == 27) {
      ps->stdin_state.escape_state = 1;
      ps->stdin_state.escape_len = 0;
      continue;
    }

    if (c == '\r' || c == '\n') {
      emit_keypress_event(js, "\r", 1, "return", false, false, false, "\r", 1);
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
}

static void process_keypress_flush(ant_t *js) {
  ant_process_state_t *ps = process_state(js);
  if (!ps) return;
  if (ps->stdin_state.escape_state == 1) {
    emit_keypress_event(js, "\x1b", 1, "escape", false, false, false, "\x1b", 1);
    ps->stdin_state.escape_state = 0;
    ps->stdin_state.escape_len = 0;
  }
}

static bool stdin_is_tty(void) {
  return uv_guess_handle(STDIN_FILENO) == UV_TTY;
}

static bool stdout_is_tty(ant_t *js) {
  ant_process_state_t *ps = process_state(js);
  if (!ps) return false;
  if (ps->sandbox_terminal_enabled) return (ps->sandbox_terminal_capabilities & ANT_SANDBOX_CAP_STDOUT_TTY) != 0;
  return uv_guess_handle(STDOUT_FILENO) == UV_TTY;
}

static bool stderr_is_tty(ant_t *js) {
  ant_process_state_t *ps = process_state(js);
  if (!ps) return false;
  if (ps->sandbox_terminal_enabled) return (ps->sandbox_terminal_capabilities & ANT_SANDBOX_CAP_STDERR_TTY) != 0;
  return uv_guess_handle(STDERR_FILENO) == UV_TTY;
}

static ant_value_t process_binding(ant_t *js, ant_value_t *args, int nargs) {
  const char *name;
  ant_value_t constants;

  if (nargs < 1 || vtype(args[0]) != T_STR)
    return js_mkerr_typed(js, JS_ERR_TYPE, "process.binding() requires a module name");

  name = js_getstr(js, args[0], NULL);
  if (!name || strcmp(name, "constants") != 0)
    return js_mkerr_typed(js, JS_ERR_TYPE, "No such module: %s", name ? name : "");

  constants = js_mkobj(js);
  js_set(js, constants, "fs", fs_make_constants(js));
  
  return constants;
}

static void get_tty_size(ant_t *js, int fd, int *rows, int *cols) {
  ant_process_state_t *ps = process_state(js);
  int out_rows = 24, out_cols = 80;
  if (ps && ps->sandbox_terminal_enabled && (fd == STDOUT_FILENO || fd == STDERR_FILENO)) {
    *rows = ps->sandbox_tty_rows ? ps->sandbox_tty_rows : 24;
    *cols = ps->sandbox_tty_cols ? ps->sandbox_tty_cols : 80;
    return;
  }
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

static void process_update_sandbox_env(ant_t *js, ant_value_t process_obj) {
  ant_process_state_t *ps = process_state(js);
  if (!ps || !ps->sandbox_terminal_enabled || !is_special_object(process_obj)) return;

  ant_value_t env_obj = js_get(js, process_obj, "env");
  if (!is_special_object(env_obj)) return;

  if (ps->sandbox_terminal_capabilities & ANT_SANDBOX_CAP_COLOR_STRIP) {
    js_set(js, env_obj, "NO_COLOR", js_mkstr(js, "1", 1));
    js_delete_prop(js, env_obj, "FORCE_COLOR", 11);
  }
  else if (ps->sandbox_terminal_capabilities & ANT_SANDBOX_CAP_COLOR_FORCE) {
    js_delete_prop(js, env_obj, "NO_COLOR", 8);
    js_set(js, env_obj, "FORCE_COLOR", js_mkstr(js, "1", 1));
  }
}

void process_refresh_sandbox_argv(ant_t *js) {
  ant_value_t process_obj = js_get(js, js_glob(js), "process");
  if (!is_special_object(process_obj)) return;

  ant_value_t argv_arr = js_mkarr(js);
  for (int i = 0; i < js->runtime.argc; i++)
    js_arr_push(js, argv_arr, js_mkstr(js, js->runtime.argv[i], strlen(js->runtime.argv[i])));

  js_set(js, process_obj, "argv", argv_arr);
  js_set(js, process_obj, "argv0", js->runtime.argc > 0 ? js_mkstr(js, js->runtime.argv[0], strlen(js->runtime.argv[0])) : js_mkstr(js, "ant", 3));
  js_set(js, process_obj, "execPath", js->runtime.argc > 0 ? js_mkstr(js, js->runtime.argv[0], strlen(js->runtime.argv[0])) : js_mkundef());
}

void process_set_sandbox_terminal(ant_t *js, uint32_t capabilities, uint16_t rows, uint16_t cols) {
  ant_process_state_t *ps = process_state(js);
  if (!ps) return;
  ps->sandbox_terminal_enabled = true;
  ps->sandbox_terminal_capabilities = capabilities;

  ps->sandbox_tty_rows = rows ? rows : 24;
  ps->sandbox_tty_cols = cols ? cols : 80;

  ant_value_t process_obj = js_get(js, js_glob(js), "process");
  if (!is_special_object(process_obj)) return;
  process_update_sandbox_env(js, process_obj);

  ant_value_t stdout_obj = js_get(js, process_obj, "stdout");
  if (is_special_object(stdout_obj)) js_set(js, stdout_obj, "isTTY", js_bool(stdout_is_tty(js)));

  ant_value_t stderr_obj = js_get(js, process_obj, "stderr");
  if (is_special_object(stderr_obj)) js_set(js, stderr_obj, "isTTY", js_bool(stderr_is_tty(js)));
}

static bool stdin_set_raw_mode(bool enable) {
  if (!stdin_is_tty()) return false;
  return tty_set_raw_mode(STDIN_FILENO, enable);
}

static void stdin_alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  buf->base = malloc(suggested_size);
#ifdef _WIN32
  buf->len = (ULONG)suggested_size;
#else
  buf->len = suggested_size;
#endif
}

static inline void emit_stdin_data_event(ant_t *js, const uv_buf_t *buf, ssize_t nread) {
  ant_process_state_t *ps = process_state(js);
  if (!ps) return;
  ArrayBufferData *ab = create_array_buffer_data((size_t)nread);
  if (ab) memcpy(ab->data, buf->base, (size_t)nread);

  ant_value_t raw_val = ab
    ? create_typed_array(js, TYPED_ARRAY_UINT8, ab, 0, (size_t)nread, "Buffer")
    : js_mkstr(js, buf->base, (size_t)nread);

  ant_value_t data_val = is_object_type(ps->stdin_state.decoder)
    ? string_decoder_decode_value(js, ps->stdin_state.decoder, raw_val, false)
    : raw_val;

  if (is_err(data_val)) data_val = raw_val;
  eventemitter_emit_args(js, ps->stdin_obj, "data", &data_val, 1);
}

static void on_stdin_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  ant_t *js = stream->data;
  if (!js) goto cleanup;

  ant_process_state_t *ps = process_state(js);
  if (!ps) goto cleanup;
  if (nread < 0) {
    if (nread == UV_EOF && ps->stdin_byte_eof) ps->stdin_byte_eof(js);
    goto cleanup;
  }

  if (nread == 0) goto cleanup;
  if (!ps->stdin_state.paused && eventemitter_listener_count(js, ps->stdin_obj, "data") > 0)
    emit_stdin_data_event(js, buf, nread);

  bool want_keypress = ps->stdin_state.keypress_enabled
    && eventemitter_listener_count(js, ps->stdin_obj, "keypress") > 0;
  bool fed_keypress = false;

  for (ssize_t i = 0; i < nread; i++) {
    if (ps->stdin_state.paused) break;
    if (ps->stdin_byte_consumer) ps->stdin_byte_consumer(js, buf->base + i, 1);
    if (want_keypress) {
      process_keypress_data(js, buf->base + i, 1);
      fed_keypress = true;
    }
  }
  if (fed_keypress && !ps->stdin_state.paused) process_keypress_flush(js);

cleanup:
  if (buf->base) free(buf->base);
}

static void stdin_start_reading(ant_t *js) {
  ant_process_state_t *ps = process_state(js);
  if (!ps) return;
  if (ps->stdin_state.reading) return;
  if (!ps->stdin_state.tty_initialized) {
    uv_loop_t *loop = uv_default_loop();
    if (uv_tty_init(loop, &ps->stdin_state.tty, STDIN_FILENO, 1) != 0) return;
    ps->stdin_state.tty.data = js;
    ps->stdin_state.tty_initialized = true;
    uv_unref((uv_handle_t *)&ps->stdin_state.tty);
  }
  uv_ref((uv_handle_t *)&ps->stdin_state.tty);
  ps->stdin_state.reading = true;
  uv_read_start((uv_stream_t *)&ps->stdin_state.tty, stdin_alloc_buffer, on_stdin_read);
}

static void stdin_stop_reading(ant_t *js) {
  ant_process_state_t *ps = process_state(js);
  if (!ps) return;
  if (!ps->stdin_state.reading) return;
  uv_read_stop((uv_stream_t *)&ps->stdin_state.tty);
  ps->stdin_state.reading = false;
  uv_unref((uv_handle_t *)&ps->stdin_state.tty);
}

static void stdin_stop_reading_if_idle(ant_t *js) {
  ant_process_state_t *ps = process_state(js);
  if (!ps) return;
  if (ps->stdin_byte_consumer) return;
  if (eventemitter_listener_count(js, ps->stdin_obj, "data") > 0) return;
  if (eventemitter_listener_count(js, ps->stdin_obj, "keypress") > 0) return;
  stdin_stop_reading(js);
}

void process_stdin_attach_reader(ant_t *js, stdin_byte_consumer_fn on_bytes, stdin_eof_fn on_eof) {
  ant_process_state_t *ps = process_state(js);
  if (!ps) return;
  ps->stdin_byte_consumer = on_bytes;
  ps->stdin_byte_eof = on_eof;
  ps->stdin_state.paused = false;
  stdin_start_reading(js);
}

void process_stdin_detach_reader(ant_t *js) {
  ant_process_state_t *ps = process_state(js);
  if (!ps) return;
  ps->stdin_byte_consumer = NULL;
  ps->stdin_byte_eof = NULL;
  stdin_stop_reading_if_idle(js);
}

#ifndef _WIN32
static void on_sigwinch(uv_signal_t *handle, int signum) {
  ant_t *js = handle->data;
  if (!js) return;

  ant_process_state_t *ps = process_state(js);
  if (!ps) return;
  ant_value_t process_obj = js_get(js, js_glob(js), "process");
  if (!is_special_object(process_obj)) return;

  ant_value_t stdout_obj = js_get(js, process_obj, "stdout");
  if (!is_special_object(stdout_obj)) return;

  eventemitter_emit_args(js, ps->stdout_obj, "resize", NULL, 0);
}
#endif

static void start_sigwinch_handler(ant_t *js);

#ifndef _WIN32
static void on_sigwinch_close(uv_handle_t *handle) {
  ant_t *js = handle ? (ant_t *)handle->data : NULL;
  ant_process_state_t *ps = js ? js->process_state : NULL;
  if (!ps) return;

  ps->sigwinch_initialized = false;
  ps->sigwinch_closing = false;
  if (ps->sigwinch_requested) start_sigwinch_handler(js);
}
#endif

static void start_sigwinch_handler(ant_t *js) {
#ifndef _WIN32
  ant_process_state_t *ps = process_state(js);
  if (!ps) return;
  ps->sigwinch_requested = true;
  if (ps->sigwinch_initialized || ps->sigwinch_closing) return;

  uv_loop_t *loop = uv_default_loop();
  if (uv_signal_init(loop, &ps->sigwinch_handle) != 0) return;
  ps->sigwinch_initialized = true;
  ps->sigwinch_handle.data = js;
  if (uv_signal_start(&ps->sigwinch_handle, on_sigwinch, SIGWINCH) != 0) {
    ps->sigwinch_requested = false;
    ps->sigwinch_closing = true;
    uv_close((uv_handle_t *)&ps->sigwinch_handle, on_sigwinch_close);
    return;
  }
  uv_unref((uv_handle_t *)&ps->sigwinch_handle);
#endif
}

static void stop_sigwinch_handler(ant_t *js) {
#ifndef _WIN32
  ant_process_state_t *ps = js ? js->process_state : NULL;
  if (!ps) return;
  ps->sigwinch_requested = false;
  if (!ps->sigwinch_initialized || ps->sigwinch_closing) return;

  uv_signal_stop(&ps->sigwinch_handle);
  ps->sigwinch_closing = true;
  uv_close((uv_handle_t *)&ps->sigwinch_handle, on_sigwinch_close);
#endif
}

static ant_value_t js_stdin_set_raw_mode(ant_t *js, ant_value_t *args, int nargs) {
  bool enable = nargs > 0 ? js_truthy(js, args[0]) : true;
  return js_bool(stdin_set_raw_mode(enable));
}

static ant_value_t js_stdin_set_encoding(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t this_obj = js_getthis(js);
  ant_process_state_t *ps = process_state(js);
  if (!ps) return js_mkerr(js, "out of memory");
  
  ant_value_t encoding = nargs > 0 && !is_undefined(args[0]) ? args[0] : js_mkstr(js, "utf8", 4);
  ant_value_t decoder = string_decoder_create(js, encoding);
  ant_value_t encoding_str = 0;

  if (is_err(decoder)) return decoder;
  encoding_str = js_tostring_val(js, encoding);
  if (is_err(encoding_str)) return encoding_str;

  ps->stdin_state.decoder = decoder;
  js_set(js, this_obj, "encoding", encoding_str);
  
  return this_obj;
}

static ant_value_t js_stdin_resume(ant_params_t) {
  ant_process_state_t *ps = process_state(js);
  if (!ps) return js_mkerr(js, "out of memory");
  ps->stdin_state.paused = false;
  stdin_start_reading(js);
  return js_getthis(js);
}

static ant_value_t js_stdin_pause(ant_params_t) {
  ant_process_state_t *ps = process_state(js);
  if (!ps) return js_mkerr(js, "out of memory");
  ps->stdin_state.paused = true;
  stdin_stop_reading(js);
  return js_getthis(js);
}

static ant_value_t process_write_stream(ant_t *js, ant_value_t *args, int nargs, FILE *stream, int fd) {
  if (nargs < 1) return js_false;

  size_t len = 0;
  char *data = js_getstr(js, args[0], &len);
  ant_output_stream_t *out = NULL;

  if (!data) return js_false;
  if (!ant_output_has_writer() && uv_guess_handle(fd) == UV_TTY)
    return js_bool(tty_ctrl_write_fd(fd, data, len));

  out = ant_output_stream(stream);
  ant_output_stream_begin(out);

  if (!ant_output_stream_append(out, data, len)) return js_false;
  return ant_output_stream_flush(out) ? js_true : js_false;
}

static ant_value_t js_stdout_write(ant_t *js, ant_value_t *args, int nargs) {
  return process_write_stream(js, args, nargs, stdout, STDOUT_FILENO);
}

static ant_value_t js_stdout_get_window_size(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  int rows = 0, cols = 0;
  get_tty_size(js, STDOUT_FILENO, &rows, &cols);
  ant_value_t arr = js_mkarr(js);
  js_arr_push(js, arr, js_mknum(cols));
  js_arr_push(js, arr, js_mknum(rows));
  return arr;
}

static ant_value_t js_stdout_rows_getter(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  int rows = 0, cols = 0;
  get_tty_size(js, STDOUT_FILENO, &rows, &cols);
  return js_mknum(rows);
}

static ant_value_t js_stdout_columns_getter(ant_t *js, ant_value_t *args, int nargs) {
  int rows = 0, cols = 0;
  get_tty_size(js, STDOUT_FILENO, &rows, &cols);
  return js_mknum(cols);
}

static ant_value_t js_stderr_write(ant_t *js, ant_value_t *args, int nargs) {
  return process_write_stream(js, args, nargs, stderr, STDERR_FILENO);
}

static ant_value_t process_uptime(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_process_state_t *ps = process_state(js);
  if (!ps) return js_mkerr(js, "out of memory");
  uint64_t now = uv_hrtime();
  double seconds = (double)(now - ps->process_start_time) / 1e9;
  return js_mknum(seconds);
}

static ant_value_t process_hrtime(ant_t *js, ant_value_t *args, int nargs) {
  uint64_t now = uv_hrtime();
  
  if (nargs > 0 && vtype(args[0]) == T_ARR) {
    ant_value_t prev_sec = js_get(js, args[0], "0");
    ant_value_t prev_nsec = js_get(js, args[0], "1");
    if (vtype(prev_sec) == T_NUM && vtype(prev_nsec) == T_NUM) {
      uint64_t prev = (uint64_t)js_getnum(prev_sec) * 1000000000ULL + (uint64_t)js_getnum(prev_nsec);
      now = now - prev;
    }
  }
  
  ant_value_t arr = js_mkarr(js);
  
  uint64_t secs = now / 1000000000ULL;
  uint64_t nsecs = now % 1000000000ULL;
  
  js_arr_push(js, arr, js_mknum((double)secs));
  js_arr_push(js, arr, js_mknum((double)nsecs));
  
  return arr;
}

static ant_value_t process_hrtime_bigint(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  uint64_t now = uv_hrtime();
  char buf[32];
  snprintf(buf, sizeof(buf), "%llu", (unsigned long long)now);
  return js_mkbigint(js, buf, strlen(buf), false);
}

static ant_value_t process_memory_usage(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t obj = js_mkobj(js);
  
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

static ant_value_t process_memory_usage_rss(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  size_t rss = 0;
  uv_resident_set_memory(&rss);
  return js_mknum((double)rss);
}

static ant_value_t process_cpu_usage(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t obj = js_mkobj(js);
  uv_rusage_t rusage;
  
  if (uv_getrusage(&rusage) == 0) {
    int64_t user_usec = rusage.ru_utime.tv_sec * 1000000LL + rusage.ru_utime.tv_usec;
    int64_t sys_usec = rusage.ru_stime.tv_sec * 1000000LL + rusage.ru_stime.tv_usec;
    
    if (nargs > 0 && is_special_object(args[0])) {
      ant_value_t prev_user = js_get(js, args[0], "user");
      ant_value_t prev_system = js_get(js, args[0], "system");
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

static ant_value_t process_kill(ant_t *js, ant_value_t *args, int nargs) {
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
        int signum = process_signal_number(sig_name);
        if (signum > 0) sig = signum;
        else return js_mkerr(js, "Unknown signal");
      }
    }
  }
  
  int result = uv_kill(pid, sig);
  if (result != 0) return js_mkerr(js, "Failed to send signal");
  return js_true;
}

static ant_value_t process_abort(ant_params_t) {
  abort();
  return js_mkundef();
}

static ant_value_t process_chdir(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "process.chdir requires 1 argument");
  
  char *dir = js_getstr(js, args[0], NULL);
  if (!dir) return js_mkerr(js, "directory must be a string");
  
  int result = uv_chdir(dir);
  if (result != 0) return js_mkerr(js, "ENOENT: no such file or directory, chdir");
  return js_mkundef();
}

static ant_value_t process_umask(ant_t *js, ant_value_t *args, int nargs) {
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
static ant_value_t process_getuid(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  return js_mknum((double)getuid());
}

static ant_value_t process_geteuid(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  return js_mknum((double)geteuid());
}

static ant_value_t process_getgid(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  return js_mknum((double)getgid());
}

static ant_value_t process_getegid(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  return js_mknum((double)getegid());
}

static ant_value_t process_getgroups(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  int ngroups = getgroups(0, NULL);
  if (ngroups < 0) return js_mkarr(js);
  
  gid_t *groups = malloc(sizeof(gid_t) * (size_t)ngroups);
  if (!groups) return js_mkarr(js);
  
  ngroups = getgroups(ngroups, groups);
  ant_value_t arr = js_mkarr(js);
  for (int i = 0; i < ngroups; i++) {
    js_arr_push(js, arr, js_mknum((double)groups[i]));
  }
  free(groups);
  return arr;
}

static ant_value_t process_setuid(ant_t *js, ant_value_t *args, int nargs) {
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

static ant_value_t process_setgid(ant_t *js, ant_value_t *args, int nargs) {
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

static ant_value_t process_seteuid(ant_t *js, ant_value_t *args, int nargs) {
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

static ant_value_t process_setegid(ant_t *js, ant_value_t *args, int nargs) {
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

static ant_value_t process_setgroups(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1 || vtype(args[0]) != T_ARR) {
    return js_mkerr(js, "process.setgroups requires an array");
  }
  
  ant_value_t len_val = js_get(js, args[0], "length");
  int len = (int)js_getnum(len_val);
  
  gid_t *groups = malloc(sizeof(gid_t) * (size_t)len);
  if (!groups) return js_mkerr(js, "allocation failed");
  
  for (int i = 0; i < len; i++) {
    char idx[16];
    snprintf(idx, sizeof(idx), "%d", i);
    ant_value_t val = js_get(js, args[0], idx);
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

static ant_value_t process_initgroups(ant_t *js, ant_value_t *args, int nargs) {
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

static ant_value_t env_getter(ant_t *js, ant_value_t obj, const char *key, size_t key_len) {
  CSTR_BUF(buf, 256);
  char *key_str = CSTR_INIT(buf, key, key_len);
  if (!key_str) return js_mkundef();
  
  char *value = getenv(key_str);
  cstr_free(&buf);
  
  if (value == NULL) return js_mkundef();
  return js_mkstr(js, value, strlen(value));
}

static bool env_setter(ant_t *js, ant_value_t obj, const char *key, size_t key_len, ant_value_t value) {
  ant_value_t str_val = coerce_to_str(js, value);
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

static bool env_deleter(ant_t *js, ant_value_t obj, const char *key, size_t key_len) {
  CSTR_BUF(buf, 256);
  char *key_str = CSTR_INIT(buf, key, key_len);
  if (key_str) { unsetenv(key_str); cstr_free(&buf); }
  return true;
}

static void load_dotenv_file(ant_t *js, ant_value_t env_obj) {
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

static ant_value_t process_exit(ant_t *js, ant_value_t *args, int nargs) {
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

static void env_foreach(ant_t *js, ant_value_t env_obj, env_iter_cb cb, void *ctx) {
  for (char **env = environ; *env != NULL; env++) {
    char *entry = *env;
    char *equals = strchr(entry, '=');
    if (equals == NULL) continue;
    
    size_t key_len = (size_t)(equals - entry);
    char *value = equals + 1;
    cb(js, entry, key_len, value, strlen(value), ctx);
  }
  
  ant_iter_t iter = js_prop_iter_begin(js, env_obj);
  const char *key; size_t key_len; ant_value_t value;
  
  while (js_prop_iter_next(&iter, &key, &key_len, &value)) {
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
  ant_value_t obj = *(ant_value_t *)ctx;
  CSTR_BUF(buf, 256);
  char *key_str = CSTR_INIT(buf, key, key_len);
  if (!key_str) return;
  js_set(js, obj, key_str, js_mkstr(js, value, val_len));
  cstr_free(&buf);
}

static ant_value_t env_to_object(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t obj = js_mkobj(js);
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

static ant_value_t env_toString(ant_t *js, ant_value_t *args, int nargs) {
  env_str_ctx ctx = { .buf = malloc(4096), .pos = 0, .cap = 4096 };
  if (!ctx.buf) return js_mkstr(js, "", 0);
  
  env_foreach(js, js->this_val, env_tostring_cb, &ctx);
  ctx.buf[ctx.pos] = '\0';
  
  ant_value_t ret = js_mkstr(js, ctx.buf, ctx.pos);
  free(ctx.buf);
  return ret;
}

static void env_keys_cb(ant_t *js, const char *key, size_t key_len, const char *value, size_t val_len, void *ctx) {
  ant_value_t arr = *(ant_value_t *)ctx;
  js_arr_push(js, arr, js_mkstr(js, key, key_len));
}

static ant_value_t env_keys(ant_t *js, ant_value_t obj) {
  ant_value_t arr = js_mkarr(js);
  env_foreach(js, obj, env_keys_cb, &arr);
  return arr;
}

static ant_value_t process_cwd(ant_t *js, ant_value_t *args, int nargs) {
  char cwd[4096];
  if (getcwd(cwd, sizeof(cwd)) != NULL) {
    return js_mkstr(js, cwd, strlen(cwd));
  }
  return js_mkundef();
}

static bool process_is_error_object(ant_value_t value) {
  if (is_err(value)) return true;
  return is_object_type(value) && js_get_slot(value, SLOT_ERROR_BRAND) == js_true;
}

static void process_get_warning_options(
  ant_t *js, ant_value_t options,
  const char **type, const char **code, const char **detail,
  ant_offset_t *detail_len
) {
  if (!is_object_type(options)) return;

  ant_value_t type_val = js_get(js, options, "type");
  ant_value_t code_val = js_get(js, options, "code");
  ant_value_t detail_val = js_get(js, options, "detail");

  if (vtype(type_val) == T_STR) *type = js_getstr(js, type_val, NULL);
  if (vtype(code_val) == T_STR) *code = js_getstr(js, code_val, NULL);
  if (vtype(detail_val) == T_STR) *detail = (const char *)(uintptr_t)vstr(js, detail_val, detail_len);
}

static ant_value_t process_make_warning_object(
  ant_t *js, const char *type, js_cstr_t msg, const char *code,
  const char *detail, ant_offset_t detail_len
) {
  ant_value_t warning_obj = js_mkobj(js);
  js_set_proto_init(warning_obj, js_get_ctor_proto(js, "Error", 5));
  js_set_slot(warning_obj, SLOT_ERROR_BRAND, js_true);
  js_set(js, warning_obj, "name", js_mkstr(js, type, strlen(type)));
  js_set(js, warning_obj, "message", js_mkstr(js, msg.ptr, msg.len));

  if (code) js_set(js, warning_obj, "code", js_mkstr(js, code, strlen(code)));
  if (detail) js_set(js, warning_obj, "detail", js_mkstr(js, detail, detail_len));

  js_capture_stack(js, warning_obj);
  return warning_obj;
}

static ant_value_t process_emit_warning(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();

  ant_value_t warning = args[0];
  const char *type = "Warning";
  const char *code = NULL;
  const char *detail = NULL;
  ant_offset_t detail_len = 0;

  if (nargs >= 2) {
    if (vtype(args[1]) == T_STR) type = js_getstr(js, args[1], NULL);
    else process_get_warning_options(js, args[1], &type, &code, &detail, &detail_len);
  }

  if (nargs >= 3 && vtype(args[2]) == T_STR) code = js_getstr(js, args[2], NULL);

  char msg_buf[512];
  js_cstr_t msg = {0};
  ant_value_t warning_event_arg = warning;
  bool is_error = process_is_error_object(warning);

  if (is_error) {
    ant_value_t warning_obj = js_as_obj(warning);
    ant_offset_t prop_len = 0;
    const char *name_prop = get_str_prop(js, warning_obj, "name", 4, &prop_len);
    if (name_prop) type = name_prop;
    
    const char *message_prop = get_str_prop(js, warning_obj, "message", 7, &prop_len);
    msg.ptr = message_prop ? message_prop : "";
    msg.len = message_prop ? prop_len : 0;
    msg.needs_free = false;
    
    code = get_str_prop(js, warning_obj, "code", 4, NULL);
    detail = get_str_prop(js, warning_obj, "detail", 6, &detail_len);
    warning_event_arg = warning_obj;
  } else {
    msg = js_to_cstr(js, warning, msg_buf, sizeof(msg_buf));
    warning_event_arg = process_make_warning_object(js, type, msg, code, detail, detail_len);
  }

  fprintf(stderr, "(%s:%d) ", "ant", (int)getpid());
  if (code) fprintf(stderr, "[%s] ", code);
  
  fprintf(stderr, "%s: %.*s\n", type ? type : "Warning", (int)msg.len, msg.ptr);
  if (detail) fprintf(stderr, "%.*s\n", (int)detail_len, detail);

  emit_process_event(js, "warning", &warning_event_arg, 1);
  if (msg.needs_free) free((void *)msg.ptr);
  
  return js_mkundef();
}


static ant_value_t process_next_tick(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "process.nextTick requires a callback");
    
  ant_value_t cb = args[0];
  if (vtype(cb) != T_FUNC && vtype(cb) != T_CFUNC)
    return js_mkerr_typed(js, JS_ERR_TYPE, "process.nextTick callback is not a function");
    
  if (nargs <= 1) queue_next_tick(js, cb);
  else queue_next_tick_with_args(js, cb, args + 1, nargs - 1);
  
  return js_mkundef();
}

static bool process_event_key_is(
  ant_t *js,
  ant_value_t key,
  const char *expected
) {
  const char *value = NULL;
  size_t len = 0;
  size_t expected_len = strlen(expected);

  if (vtype(key) != T_STR) return false;
  value = js_getstr(js, key, &len);
  return value && len == expected_len && memcmp(value, expected, len) == 0;
}

static void process_listener_change(
  ant_t *js,
  ant_value_t target,
  ant_value_t key,
  ant_offset_t listener_count,
  void *context
) {
  ant_process_state_t *ps = (ant_process_state_t *)context;
  if (!ps || vtype(key) != T_STR) return;

  if (target == ps->process_obj) {
    const char *event = js_getstr(js, key, NULL);
    int signum = event ? process_signal_number(event) : -1;
    if (signum > 0) {
      if (listener_count > 0) start_signal_watch(js, signum);
      else stop_signal_watch(js, signum);
    }
    return;
  }

  if (
    target == ps->stdin_obj && (
      process_event_key_is(js, key, "data") ||
      process_event_key_is(js, key, "keypress")
    )
  ) {
    if (listener_count > 0) {
      ps->stdin_state.paused = false;
      stdin_start_reading(js);
    } else stdin_stop_reading_if_idle(js);
    return;
  }

  if (
    target == ps->stdout_obj &&
    process_event_key_is(js, key, "resize")
  ) {
    if (listener_count > 0) start_sigwinch_handler(js);
    else stop_sigwinch_handler(js);
  }
}

static void process_set_methods(ant_t *js, ant_value_t obj, bool include_event_methods) {
  js_set(js, obj, "exit", js_mkfun(process_exit));
  js_set(js, obj, "cwd", js_mkfun(process_cwd));
  js_set(js, obj, "chdir", js_mkfun(process_chdir));
  js_set(js, obj, "uptime", js_mkfun(process_uptime));
  js_set(js, obj, "cpuUsage", js_mkfun(process_cpu_usage));
  js_set(js, obj, "kill", js_mkfun(process_kill));
  js_set(js, obj, "abort", js_mkfun(process_abort));
  js_set(js, obj, "umask", js_mkfun(process_umask));
  js_set(js, obj, "nextTick", js_mkfun(process_next_tick));
  js_set(js, obj, "emitWarning", js_mkfun(process_emit_warning));
  js_set(js, obj, "dlopen", js_mkfun(napi_process_dlopen_js));

  ant_value_t mem_usage_fn = js_heavy_mkfun(js, process_memory_usage, js_mkundef());
  js_set(js, mem_usage_fn, "rss", js_mkfun(process_memory_usage_rss));
  js_set(js, obj, "memoryUsage", mem_usage_fn);

  ant_value_t hrtime_fn = js_heavy_mkfun(js, process_hrtime, js_mkundef());
  js_set(js, hrtime_fn, "bigint", js_mkfun(process_hrtime_bigint));
  js_set(js, obj, "hrtime", hrtime_fn);

  if (include_event_methods) {
    ant_value_t ee_proto = eventemitter_prototype(js);
    if (is_object_type(ee_proto)) js_set_proto_init(obj, ee_proto);
  }

#ifndef _WIN32
  js_set(js, obj, "getuid", js_mkfun(process_getuid));
  js_set(js, obj, "geteuid", js_mkfun(process_geteuid));
  js_set(js, obj, "getgid", js_mkfun(process_getgid));
  js_set(js, obj, "getegid", js_mkfun(process_getegid));
  js_set(js, obj, "getgroups", js_mkfun(process_getgroups));
  js_set(js, obj, "setuid", js_mkfun(process_setuid));
  js_set(js, obj, "setgid", js_mkfun(process_setgid));
  js_set(js, obj, "seteuid", js_mkfun(process_seteuid));
  js_set(js, obj, "setegid", js_mkfun(process_setegid));
  js_set(js, obj, "setgroups", js_mkfun(process_setgroups));
  js_set(js, obj, "initgroups", js_mkfun(process_initgroups));
#endif
}

ant_value_t process_library(ant_t *js) {
  ant_value_t process_obj = js_get(js, js_glob(js), "process");
  js_set(js, process_obj, "default", process_obj);
  js_set_slot_wb(js, process_obj, SLOT_DEFAULT, process_obj);
  return process_obj;
}

static void process_set_string(ant_t *js, ant_value_t obj, const char *key, const char *value) {
  js_set(js, obj, key, js_mkstr(js, value, strlen(value)));
}

void init_process_module(ant_t *js) {
  ant_process_state_t *ps = process_state(js);
  ant_value_t global = js_glob(js);
  ant_value_t ee_proto = eventemitter_prototype(js);

  if (!ps) return;

  ps->stdin_state.decoder = js_mkundef();
  ps->process_start_time = uv_hrtime();
  ant_value_t process_proto = js_newobj(js);

  process_set_methods(js, process_proto, true);
  js_set_sym(js, process_proto, get_toStringTag_sym(), js_mkstr(js, "process", 7));

  ant_value_t process_obj = js_mkobj(js);
  ant_value_t env_obj = js_newobj(js);
  
  js_set_proto_init(process_obj, process_proto);
  ps->process_obj = process_obj;
  eventemitter_set_listener_change_hook(js, process_obj, process_listener_change, ps);
  
  process_set_methods(js, process_obj, false);
  js_set(js, process_obj, "binding", js_mkfun(process_binding));

  load_dotenv_file(js, env_obj);
  js_set_keys(env_obj, env_keys);

  js_set_getter(env_obj, env_getter);
  js_set_setter(env_obj, env_setter);
  js_set_deleter(env_obj, env_deleter);
  
  defmethod(js, env_obj, "toObject", 8, js_mkfun(env_to_object));
  defmethod(js, env_obj, "toString", 8, js_mkfun(env_toString));
  
  js_set(js, process_obj, "env", env_obj);
  process_update_sandbox_env(js, process_obj);
  
  ant_value_t argv_arr = js_mkarr(js);
  for (int i = 0; i < js->runtime.argc; i++) js_arr_push(
    js, argv_arr, 
    js_mkstr(js, js->runtime.argv[i], strlen(js->runtime.argv[i]))
  );

  js_set(js, process_obj, "argv", argv_arr);
  js_set(js, process_obj, "execArgv", js_mkarr(js));
  
  js_set(js, process_obj, "argv0", js->runtime.argc > 0 
    ? js_mkstr(js, js->runtime.argv[0], strlen(js->runtime.argv[0])) 
    : js_mkstr(js, "ant", 3)
  );
  
  js_set(js, process_obj, "execPath", js->runtime.argc > 0 
    ? js_mkstr(js, js->runtime.argv[0], strlen(js->runtime.argv[0])) 
    : js_mkundef()
  );
  
  js_set(js, process_obj, "pid", js_mknum((double)getpid()));
  js_set(js, process_obj, "ppid", js_mknum((double)getppid()));

  ant_value_t features_obj = js_newobj(js);
  js_set(js, features_obj, "uv", js_true);
  process_set_string(js, features_obj, "tls", "BoringSSL");
  process_set_string(js, features_obj, "typescript", "transform");
  js_set(js, process_obj, "features", features_obj);
  process_set_string(js, process_obj, "version", "v25.9.0");
  
  ant_value_t versions_obj = js_newobj(js);
  process_set_string(js, versions_obj, "ant", ANT_VERSION);
  process_set_string(js, versions_obj, "silver", ANT_VERSION);
  process_set_string(js, versions_obj, "skim", SKIM_VERSION);
  
  char uv_ver[32];
  snprintf(uv_ver, sizeof(uv_ver), "%d.%d.%d", UV_VERSION_MAJOR, UV_VERSION_MINOR, UV_VERSION_PATCH);
  process_set_string(js, versions_obj, "uv", uv_ver);
  process_set_string(js, versions_obj, "node", "25.9.0");
  process_set_string(js, versions_obj, "brotli", "1.1.0");
  process_set_string(js, versions_obj, "llhttp", "9.3.1");
  process_set_string(js, versions_obj, "nghttp2", "1.68.0");
  process_set_string(js, versions_obj, "simdjson", "0.12.0");
  process_set_string(js, versions_obj, "pcre2", "10.47");
  process_set_string(js, versions_obj, "libffi", "3.5.2");
  process_set_string(js, versions_obj, "lmdb", "0.9.33");
  process_set_string(js, versions_obj, "utf8proc", "2.10.0");
  process_set_string(js, versions_obj, "zlib", "2.3.3");
  process_set_string(js, versions_obj, "v8", "14.1.146.11-node.25");
  process_set_string(js, versions_obj, "modules", "141");
  process_set_string(js, versions_obj, "napi", "10");
  process_set_string(js, versions_obj, "wamr", "26756a5c5846c262c9e5c89f3fc8e1e693ee2539");
  process_set_string(js, versions_obj, "boringssl", "297b11798a0ed6bc7736aa57328909a4afbbf67a");
  js_set(js, process_obj, "versions", versions_obj);
  
  ant_value_t release_obj = js_newobj(js);
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
  
  ant_value_t stdin_proto = js_mkobj(js);
  if (is_object_type(ee_proto)) js_set_proto_init(stdin_proto, ee_proto);
  js_set(js, stdin_proto, "setRawMode", js_mkfun(js_stdin_set_raw_mode));
  js_set(js, stdin_proto, "setEncoding", js_mkfun(js_stdin_set_encoding));
  js_set(js, stdin_proto, "resume", js_mkfun(js_stdin_resume));
  js_set(js, stdin_proto, "pause", js_mkfun(js_stdin_pause));
  js_set_sym(js, stdin_proto, get_toStringTag_sym(), js_mkstr(js, "ReadStream", 10));
  
  ant_value_t stdin_obj = js_mkobj(js);
  js_set_proto_init(stdin_obj, stdin_proto);
  ps->stdin_obj = stdin_obj;
  eventemitter_set_listener_change_hook(js, stdin_obj, process_listener_change, ps);
  js_set(js, stdin_obj, "isTTY", js_bool(stdin_is_tty()));
  js_set(js, stdin_obj, "encoding", js_mkundef());
  js_set(js, process_obj, "stdin", stdin_obj);
  
  ant_value_t stdout_proto = js_mkobj(js);
  if (is_object_type(ee_proto)) js_set_proto_init(stdout_proto, ee_proto);
  js_set(js, stdout_proto, "write", js_mkfun(js_stdout_write));
  js_set(js, stdout_proto, "getWindowSize", js_mkfun(js_stdout_get_window_size));
  js_set_sym(js, stdout_proto, get_toStringTag_sym(), js_mkstr(js, "WriteStream", 11));
  
  ant_value_t stdout_obj = js_mkobj(js);
  js_set_proto_init(stdout_obj, stdout_proto);
  ps->stdout_obj = stdout_obj;
  eventemitter_set_listener_change_hook(js, stdout_obj, process_listener_change, ps);
  js_set(js, stdout_obj, "isTTY", js_bool(stdout_is_tty(js)));
  js_set_getter_desc(js, stdout_obj, "rows", 4, js_mkfun(js_stdout_rows_getter), JS_DESC_E | JS_DESC_C);
  js_set_getter_desc(js, stdout_obj, "columns", 7, js_mkfun(js_stdout_columns_getter), JS_DESC_E | JS_DESC_C);
  js_set(js, process_obj, "stdout", stdout_obj);
  
  ant_value_t stderr_proto = js_mkobj(js);
  if (is_object_type(ee_proto)) js_set_proto_init(stderr_proto, ee_proto);
  js_set(js, stderr_proto, "write", js_mkfun(js_stderr_write));
  js_set_sym(js, stderr_proto, get_toStringTag_sym(), js_mkstr(js, "WriteStream", 11));
  
  ant_value_t stderr_obj = js_mkobj(js);
  js_set_proto_init(stderr_obj, stderr_proto);
  ps->stderr_obj = stderr_obj;
  eventemitter_set_listener_change_hook(js, stderr_obj, process_listener_change, ps);
  js_set(js, stderr_obj, "isTTY", js_bool(stderr_is_tty(js)));
  js_set(js, process_obj, "stderr", stderr_obj);
  js_set_global_builtin(js, "process", process_obj);
}


bool has_active_stdin(ant_t *js) {
  ant_process_state_t *ps = js->process_state;
  return ps && ps->stdin_state.reading;
}

void gc_mark_process(ant_t *js, gc_mark_fn mark) {
  ant_process_state_t *ps = js->process_state;
  if (!ps) return;
  if (is_object_type(ps->process_obj)) mark(js, ps->process_obj);
  if (is_object_type(ps->stdin_obj)) mark(js, ps->stdin_obj);
  if (is_object_type(ps->stdout_obj)) mark(js, ps->stdout_obj);
  if (is_object_type(ps->stderr_obj)) mark(js, ps->stderr_obj);
  if (is_object_type(ps->stdin_state.decoder)) mark(js, ps->stdin_state.decoder);
}

void process_enable_keypress_events(ant_t *js) {
  ant_process_state_t *ps = process_state(js);
  if (!ps) return;
  ps->stdin_state.keypress_enabled = true;
  ps->stdin_state.escape_state = 0;
  ps->stdin_state.escape_len = 0;
}
