#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <conio.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#define STDIN_FILENO 0
#define mkdir_p(path) _mkdir(path)
#else
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#define mkdir_p(path) mkdir(path, 0755)
#endif

#include <crprintf.h>
#include "highlight.h"
#include "readline.h"
#include "utf8.h"

#define MAX_LINE_LENGTH 4096

static volatile sig_atomic_t ctrl_c_pressed = 0;
static crprintf_compiled *hl_prog = NULL;
static highlight_state hl_line_state = HL_STATE_INIT;
static int repl_last_cursor_row = 0;

static void sigint_handler(int sig) {
  (void)sig;
  ctrl_c_pressed++;
}

void ant_readline_install_signal_handler(void) {
#ifdef _WIN32
  signal(SIGINT, sigint_handler);
#else
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = sigint_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
#endif
}

void ant_readline_shutdown(void) {
  if (hl_prog) {
    crprintf_compiled_free(hl_prog);
    hl_prog = NULL;
  }
}

void ant_history_init(ant_history_t *hist, int capacity) {
  hist->capacity = (capacity > 0) ? capacity : 512;
  hist->lines = malloc(sizeof(char *) * (size_t)hist->capacity);
  if (!hist->lines) hist->capacity = 0;
  hist->count = 0;
  hist->current = -1;
}

void ant_history_add(ant_history_t *hist, const char *line) {
  if (!hist || !hist->lines || hist->capacity <= 0 || !line || line[0] == '\0') return;
  if (hist->count > 0 && strcmp(hist->lines[hist->count - 1], line) == 0) return;

  if (hist->count >= hist->capacity) {
    free(hist->lines[0]);
    memmove(hist->lines, hist->lines + 1, sizeof(char *) * (size_t)(hist->capacity - 1));
    hist->count--;
  }

  hist->lines[hist->count++] = strdup(line);
  hist->current = hist->count;
}

const char *ant_history_prev(ant_history_t *hist) {
  if (!hist || !hist->lines || hist->count == 0) return NULL;
  if (hist->current > 0) hist->current--;
  return hist->lines[hist->current];
}

const char *ant_history_next(ant_history_t *hist) {
  if (!hist || !hist->lines || hist->count == 0) return NULL;
  if (hist->current < hist->count - 1) {
    hist->current++;
    return hist->lines[hist->current];
  }
  hist->current = hist->count;
  return "";
}

void ant_history_free(ant_history_t *hist) {
  if (!hist || !hist->lines) return;
  for (int i = 0; i < hist->count; i++) free(hist->lines[i]);
  free(hist->lines);
  hist->lines = NULL;
  hist->count = 0;
  hist->capacity = 0;
  hist->current = -1;
}

static char *get_history_path(void) {
  const char *home = getenv("HOME");
  if (!home) home = getenv("USERPROFILE");
  if (!home) return NULL;

  size_t len = strlen(home) + 32;
  char *path = malloc(len);
  snprintf(path, len, "%s/.ant", home);
  mkdir_p(path);
  snprintf(path, len, "%s/.ant/repl_history", home);
  return path;
}

void ant_history_load(ant_history_t *hist) {
  if (!hist || !hist->lines || hist->capacity <= 0) return;
  char *path = get_history_path();
  if (!path) return;

  FILE *fp = fopen(path, "r");
  free(path);
  if (!fp) return;

  char line[MAX_LINE_LENGTH];
  while (fgets(line, sizeof(line), fp)) {
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
    if (line[0]) ant_history_add(hist, line);
  }
  fclose(fp);
}

void ant_history_save(const ant_history_t *hist) {
  if (!hist || !hist->lines) return;
  char *path = get_history_path();
  if (!path) return;

  FILE *fp = fopen(path, "w");
  free(path);
  if (!fp) return;

  for (int i = 0; i < hist->count; i++) {
    fprintf(fp, "%s\n", hist->lines[i]);
  }
  fclose(fp);
}

typedef enum {
  KEY_NONE, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
  KEY_HOME, KEY_END, KEY_DELETE, KEY_BACKSPACE, KEY_ENTER, KEY_EOF, KEY_CHAR
} key_type_t;

typedef struct {
  key_type_t type;
  int ch;
} key_event_t;

static int repl_terminal_cols(void) {
  int cols = 80;
#ifdef _WIN32
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
    cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
  }
#else
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
    cols = ws.ws_col;
  }
#endif
  return (cols > 0) ? cols : 80;
}

static void repl_jump_cursor(int x_pos, int y_offset) {
#ifdef _WIN32
  if (y_offset != 0) {
    char seq[32];
    snprintf(seq, sizeof(seq), "\033[%d%c", abs(y_offset), (y_offset > 0) ? 'B' : 'A');
    fputs(seq, stdout);
  }
  char seq[32];
  snprintf(seq, sizeof(seq), "\033[%dG", x_pos + 1);
  fputs(seq, stdout);
#else
  if (y_offset != 0) {
    char seq[32];
    snprintf(seq, sizeof(seq), "\033[%d%c", abs(y_offset), (y_offset > 0) ? 'B' : 'A');
    fputs(seq, stdout);
  }
  char seq[32];
  snprintf(seq, sizeof(seq), "\033[%dG", x_pos + 1);
  fputs(seq, stdout);
#endif
}

static void repl_clear_to_end(void) {
  fputs("\033[J", stdout);
}

static void repl_set_cursor_visible(bool visible) {
#ifdef _WIN32
  (void)visible;
#else
  fputs(visible ? "\033[?25h" : "\033[?25l", stdout);
#endif
}

static size_t repl_skip_ansi_escape(const char *s, size_t len, size_t i) {
  if (i >= len || (unsigned char)s[i] != 0x1B) return i;
  if (i + 1 >= len) return i + 1;

  unsigned char next = (unsigned char)s[i + 1];

  if (next == '[') {
    i += 2;
    while (i < len) {
      unsigned char ch = (unsigned char)s[i++];
      if (ch >= 0x40 && ch <= 0x7E) break;
    }
    return i;
  }

  if (next == ']') {
    i += 2;
    while (i < len) {
      unsigned char ch = (unsigned char)s[i];
      if (ch == '\a') return i + 1;
      if (ch == 0x1B && i + 1 < len && s[i + 1] == '\\') return i + 2;
      i++;
    }
    return i;
  }

  return i + 2;
}

static void repl_virtual_render(
  const char *s, size_t n,
  int screen_cols, int prompt_len,
  int *x, int *y
) {
  bool wrapped = false;
  size_t i = 0;
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    if (c == '\n' || c == '\r') {
      if (c == '\n' && !wrapped) (*y)++;
      *x = prompt_len;
      i++;
      wrapped = false;
      continue;
    }

    if (c == 0x1B) {
      i = repl_skip_ansi_escape(s, n, i);
      continue;
    }

    utf8proc_int32_t cp = 0;
    utf8proc_ssize_t clen =
      utf8_next((const utf8proc_uint8_t *)(s + i), (utf8proc_ssize_t)(n - i), &cp);
    if (clen <= 0) {
      clen = 1;
      cp = c;
    }

    int w = utf8proc_charwidth(cp);
    if (w < 0) w = 1;

    if (w > 0) {
      *x += w;
      wrapped = false;
      if (*x >= screen_cols) {
        *x = 0;
        (*y)++;
        wrapped = true;
      }
    }

    i += (size_t)clen;
  }
}

static int repl_prompt_width(const char *prompt, int cols) {
  int x = 0;
  int y = 0;
  repl_virtual_render(prompt, strlen(prompt), cols, 0, &x, &y);
  return x;
}

static void refresh_line(const char *line, int len, int pos, const char *prompt) {
  int cols = repl_terminal_cols();
  int prompt_len = repl_prompt_width(prompt, cols);
  int x_cursor = prompt_len;
  int y_cursor = 0;
  int x_end = prompt_len;
  int y_end = 0;

  repl_virtual_render(line, (size_t)pos, cols, prompt_len, &x_cursor, &y_cursor);
  repl_virtual_render(line, (size_t)len, cols, prompt_len, &x_end, &y_end);

  repl_set_cursor_visible(false);
  repl_jump_cursor(prompt_len, -repl_last_cursor_row);
  repl_clear_to_end();
  if (crprintf_get_color() && len > 0) {
    if (len <= 2048) {
      char tagged[8192];
      char rendered[8192];

      highlight_state state = hl_line_state;
      ant_highlight_stateful(line, (size_t)len, tagged, sizeof(tagged), &state);

      hl_prog = crprintf_recompile(hl_prog, tagged);
      crprintf_state *rs = crprintf_state_new();
      crsprintf_compiled(rendered, sizeof(rendered), rs, hl_prog);
      crprintf_state_free(rs);

      fputs(rendered, stdout);
    } else {
      fwrite(line, 1, (size_t)len, stdout);
    }
  } else if (len > 0) {
    fwrite(line, 1, (size_t)len, stdout);
  }
  
#ifndef _WIN32
  if ((x_end == 0) && (y_end > 0) && (len > 0) && (line[len - 1] != '\n')) {
    fputs("\n", stdout);
  }
#endif

  repl_jump_cursor(x_cursor, -(y_end - y_cursor));
  repl_set_cursor_visible(true);
  repl_last_cursor_row = y_cursor;
  fflush(stdout);
}

static void move_cursor_only(const char *line, int pos, const char *prompt) {
  int cols = repl_terminal_cols();
  int prompt_len = repl_prompt_width(prompt, cols);
  int x_cursor = prompt_len;
  int y_cursor = 0;
  repl_virtual_render(line, (size_t)pos, cols, prompt_len, &x_cursor, &y_cursor);
  repl_jump_cursor(x_cursor, -(repl_last_cursor_row - y_cursor));
  repl_last_cursor_row = y_cursor;
  fflush(stdout);
}

static int utf8_prev_pos(const char *line, int pos) {
  if (pos <= 0) return 0;
  int prev = 0;
  int i = 0;
  while (i < pos) {
    prev = i;
    utf8proc_int32_t cp = 0;
    utf8proc_ssize_t n = utf8_next(
      (const utf8proc_uint8_t *)(line + i),
      (utf8proc_ssize_t)(pos - i),
      &cp
    );
    i += (int)((n > 0) ? n : 1);
  }
  return prev;
}

static int utf8_next_pos(const char *line, int len, int pos) {
  if (pos >= len) return len;
  utf8proc_int32_t cp = 0;
  utf8proc_ssize_t n = utf8_next(
    (const utf8proc_uint8_t *)(line + pos),
    (utf8proc_ssize_t)(len - pos),
    &cp
  );
  int next = pos + (int)((n > 0) ? n : 1);
  return (next > len) ? len : next;
}

static void line_set(char *line, int *pos, int *len, const char *str, const char *prompt) {
  size_t n = strlen(str);
  if (n >= (size_t)MAX_LINE_LENGTH) n = (size_t)MAX_LINE_LENGTH - 1;
  memcpy(line, str, n);
  line[n] = '\0';
  *len = (int)strlen(line);
  *pos = *len;
  refresh_line(line, *len, *pos, prompt);
}

static void line_backspace(char *line, int *pos, int *len, const char *prompt) {
  if (*pos <= 0) return;

  int prev = utf8_prev_pos(line, *pos);
  memmove(line + prev, line + *pos, (size_t)(*len - *pos + 1));
  *len -= (*pos - prev);
  *pos = prev;
  refresh_line(line, *len, *pos, prompt);
}

static void line_delete(char *line, int *pos, int *len, const char *prompt) {
  if (*pos >= *len) return;
  int next = utf8_next_pos(line, *len, *pos);
  memmove(line + *pos, line + next, (size_t)(*len - next + 1));
  *len -= (next - *pos);
  refresh_line(line, *len, *pos, prompt);
}

static void line_insert(char *line, int *pos, int *len, int c, const char *prompt) {
  if (*len >= MAX_LINE_LENGTH - 1) return;

  memmove(line + *pos + 1, line + *pos, (size_t)(*len - *pos + 1));
  line[*pos] = (char)c;
  (*pos)++;
  (*len)++;
  refresh_line(line, *len, *pos, prompt);
}

#ifdef _WIN32
static key_event_t read_key(void) {
  if (ctrl_c_pressed > 0) return (key_event_t){ KEY_EOF, 0 };
  int c = _getch();
  if (c == 0 || c == 0xE0) {
    int ext = _getch();
    switch (ext) {
      case 72: return (key_event_t){ KEY_UP, 0 };
      case 80: return (key_event_t){ KEY_DOWN, 0 };
      case 77: return (key_event_t){ KEY_RIGHT, 0 };
      case 75: return (key_event_t){ KEY_LEFT, 0 };
      case 71: return (key_event_t){ KEY_HOME, 0 };
      case 79: return (key_event_t){ KEY_END, 0 };
      case 83: return (key_event_t){ KEY_DELETE, 0 };
      default: return (key_event_t){ KEY_NONE, 0 };
    }
  }
  if (c == 8) return (key_event_t){ KEY_BACKSPACE, 0 };
  if (c == '\r' || c == '\n') return (key_event_t){ KEY_ENTER, 0 };
  if (c == 3) {
    ctrl_c_pressed++;
    return (key_event_t){ KEY_EOF, 0 };
  }
  if (c == 4 || c == 26) return (key_event_t){ KEY_EOF, 0 };
  if (isprint(c) || (unsigned char)c >= 0x80) return (key_event_t){ KEY_CHAR, c };
  return (key_event_t){ KEY_NONE, 0 };
}
#else
static struct termios saved_tio;

static key_event_t read_key(void) {
  if (ctrl_c_pressed > 0) return (key_event_t){ KEY_EOF, 0 };
  int c = getchar();
  if (c == EOF && !feof(stdin)) {
    clearerr(stdin);
    return (key_event_t){ KEY_EOF, 0 };
  }
  if (c == EOF) return (key_event_t){ KEY_EOF, 0 };

  if (c == 27) {
    int seq1 = getchar();
    if (seq1 == EOF) return (key_event_t){ KEY_NONE, 0 };
    if (seq1 == 'O') {
      int seq2 = getchar();
      if (seq2 == 'H') return (key_event_t){ KEY_HOME, 0 };
      if (seq2 == 'F') return (key_event_t){ KEY_END, 0 };
      return (key_event_t){ KEY_NONE, 0 };
    }
    if (seq1 != '[') return (key_event_t){ KEY_NONE, 0 };
    int seq2 = getchar();
    if (seq2 == EOF) return (key_event_t){ KEY_NONE, 0 };
    switch (seq2) {
      case 'A': return (key_event_t){ KEY_UP, 0 };
      case 'B': return (key_event_t){ KEY_DOWN, 0 };
      case 'C': return (key_event_t){ KEY_RIGHT, 0 };
      case 'D': return (key_event_t){ KEY_LEFT, 0 };
      case 'H': return (key_event_t){ KEY_HOME, 0 };
      case 'F': return (key_event_t){ KEY_END, 0 };
      default: {
        if (seq2 >= '0' && seq2 <= '9') {
          int seq3 = getchar();
          if (seq3 == '~') {
            if (seq2 == '1' || seq2 == '7') return (key_event_t){ KEY_HOME, 0 };
            if (seq2 == '4' || seq2 == '8') return (key_event_t){ KEY_END, 0 };
            if (seq2 == '3') return (key_event_t){ KEY_DELETE, 0 };
          }
        }
        return (key_event_t){ KEY_NONE, 0 };
      }
    }
  }

  if (c == 127 || c == 8) return (key_event_t){ KEY_BACKSPACE, 0 };
  if (c == '\n' || c == '\r') return (key_event_t){ KEY_ENTER, 0 };
  if (isprint(c) || (unsigned char)c >= 0x80) return (key_event_t){ KEY_CHAR, c };
  return (key_event_t){ KEY_NONE, 0 };
}
#endif

static void term_restore(void) {
#ifndef _WIN32
  tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio);
#endif
}

static char *read_line_with_history(ant_history_t *hist, const char *prompt) {
  char *line = malloc(MAX_LINE_LENGTH);
  if (!line) return NULL;
  int pos = 0;
  int len = 0;
  line[0] = '\0';
  repl_last_cursor_row = 0;

#ifndef _WIN32
  struct termios new_tio;
  tcgetattr(STDIN_FILENO, &saved_tio);
  new_tio = saved_tio;
  new_tio.c_lflag &= ~(ICANON | ECHO);
  tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
#endif

  for (;;) {
    key_event_t key = read_key();
    static const void *dispatch[] = {
      [KEY_NONE] = &&l_none,
      [KEY_UP] = &&l_up,
      [KEY_DOWN] = &&l_down,
      [KEY_LEFT] = &&l_left,
      [KEY_RIGHT] = &&l_right,
      [KEY_HOME] = &&l_home,
      [KEY_END] = &&l_end,
      [KEY_DELETE] = &&l_delete,
      [KEY_BACKSPACE] = &&l_backspace,
      [KEY_ENTER] = &&l_enter,
      [KEY_EOF] = &&l_eof,
      [KEY_CHAR] = &&l_char,
    };

    unsigned int t = (unsigned int)key.type;
    if (t < (sizeof(dispatch) / sizeof(*dispatch)) && dispatch[t]) goto *dispatch[t];
    goto l_none;

  l_enter:
    putchar('\n');
    term_restore();
    return line;

  l_eof:
    putchar('\n');
    term_restore();
    free(line);
    return NULL;

  l_up: {
    const char *h = ant_history_prev(hist);
    if (h) line_set(line, &pos, &len, h, prompt);
    continue;
  }

  l_down: {
    const char *h = ant_history_next(hist);
    if (h) line_set(line, &pos, &len, h, prompt);
    continue;
  }

  l_left:
    if (pos > 0) {
      pos = utf8_prev_pos(line, pos);
      move_cursor_only(line, pos, prompt);
    }
    continue;

  l_right:
    if (pos < len) {
      pos = utf8_next_pos(line, len, pos);
      move_cursor_only(line, pos, prompt);
    }
    continue;

  l_home:
    if (pos != 0) {
      pos = 0;
      move_cursor_only(line, pos, prompt);
    }
    continue;

  l_end:
    if (pos != len) {
      pos = len;
      move_cursor_only(line, pos, prompt);
    }
    continue;

  l_delete:
    line_delete(line, &pos, &len, prompt);
    continue;

  l_backspace:
    line_backspace(line, &pos, &len, prompt);
    continue;

  l_char:
    line_insert(line, &pos, &len, key.ch, prompt);
    continue;

  l_none:
    continue;
  }
}

ant_readline_result_t ant_readline(
  ant_history_t *hist,
  const char *prompt,
  highlight_state line_state,
  char **out_line
) {
  if (out_line) *out_line = NULL;

  hl_line_state = line_state;
  ctrl_c_pressed = 0;
  char *line = read_line_with_history(hist, prompt);

  if (ctrl_c_pressed > 0) {
    if (line) free(line);
    return ANT_READLINE_INTERRUPT;
  }
  if (!line) return ANT_READLINE_EOF;

  if (out_line) *out_line = line;
  return ANT_READLINE_LINE;
}
