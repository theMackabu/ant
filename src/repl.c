#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#include <io.h>
#include <direct.h>
#define STDIN_FILENO 0
#define mkdir_p(path) _mkdir(path)
#else
#include <termios.h>
#include <unistd.h>
#define mkdir_p(path) mkdir(path, 0755)
#endif

#include "ant.h"
#include "repl.h"
#include "reactor.h"
#include "config.h"
#include "runtime.h"
#include "internal.h"
#include "modules/io.h"

#define MAX_HISTORY 512
#define MAX_LINE_LENGTH 4096
#define MAX_MULTILINE_LENGTH 65536
#define INPUT char *line, int *pos, int *len, key_event_t *key, history_t *hist, const char *prompt

static volatile sig_atomic_t ctrl_c_pressed = 0;

typedef struct {
  char **lines;
  int count;
  int capacity;
  int current;
} history_t;

typedef enum {
  CMD_OK,
  CMD_EXIT,
  CMD_NOT_FOUND
} cmd_result_t;

typedef struct {
  const char *name;
  const char *description;
  bool has_arg;
  cmd_result_t (*handler)(struct js *js, history_t *history, const char *arg);
} repl_command_t;

static void sigint_handler(int sig) {
  (void)sig;
  ctrl_c_pressed++;
}

static cmd_result_t cmd_help(struct js *js, history_t *history, const char *arg);
static cmd_result_t cmd_exit(struct js *js, history_t *history, const char *arg);
static cmd_result_t cmd_clear(struct js *js, history_t *history, const char *arg);
static cmd_result_t cmd_load(struct js *js, history_t *history, const char *arg);
static cmd_result_t cmd_save(struct js *js, history_t *history, const char *arg);
static cmd_result_t cmd_gc(struct js *js, history_t *history, const char *arg);
static cmd_result_t cmd_stats(struct js *js, history_t *history, const char *arg);

static const repl_command_t commands[] = {
  { "help",  "Show this help message", false, cmd_help },
  { "exit",  "Exit the REPL", false, cmd_exit },
  { "clear", "Clear the current context", false, cmd_clear },
  { "load",  "Load JS from a file into the REPL session", true, cmd_load },
  { "save",  "Save all evaluated commands in this REPL session to a file", true, cmd_save },
  { "gc",    "Run garbage collector", false, cmd_gc },
  { "stats", "Show memory statistics", false, cmd_stats },
  { NULL, NULL, false, NULL }
};

static cmd_result_t cmd_help(struct js *js, history_t *history, const char *arg) {
  (void)js; (void)history; (void)arg;
  for (const repl_command_t *cmd = commands; cmd->name; cmd++) {
    printf("  .%-7s - %s\n", cmd->name, cmd->description);
  }
  printf("\nPress Ctrl+C to abort current expression.\n");
  return CMD_OK;
}

static cmd_result_t cmd_exit(struct js *js, history_t *history, const char *arg) {
  (void)js; (void)history; (void)arg;
  return CMD_EXIT;
}

static cmd_result_t cmd_clear(struct js *js, history_t *history, const char *arg) {
  (void)js; (void)history; (void)arg;
  printf("Clearing context...\n");
  return CMD_OK;
}

static cmd_result_t cmd_load(struct js *js, history_t *history, const char *arg) {
  (void)history;
  if (!arg || *arg == '\0') {
    fprintf(stderr, "Usage: .load <filename>\n");
    return CMD_OK;
  }
  
  FILE *fp = fopen(arg, "r");
  if (fp == NULL) {
    fprintf(stderr, "Failed to open file: %s\n", arg);
    return CMD_OK;
  }
  
  fseek(fp, 0, SEEK_END);
  long file_size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  
  char *file_buffer = malloc(file_size + 1);
  if (file_buffer) {
    size_t len = fread(file_buffer, 1, file_size, fp);
    file_buffer[len] = '\0';
    
    jsval_t result = js_eval(js, file_buffer, len);
    js_run_event_loop(js);
    if (vtype(result) == T_ERR) {
      fprintf(stderr, "%s\n", js_str(js, result));
    } else if (vtype(result) != T_UNDEF) {
      printf("%s\n", js_str(js, result));
    }
    
    free(file_buffer);
  }
  fclose(fp);
  return CMD_OK;
}

static cmd_result_t cmd_save(struct js *js, history_t *history, const char *arg) {
  (void)js;
  if (!arg || *arg == '\0') {
    fprintf(stderr, "Usage: .save <filename>\n");
    return CMD_OK;
  }
  
  FILE *fp = fopen(arg, "w");
  if (fp == NULL) {
    fprintf(stderr, "Failed to open file for writing: %s\n", arg);
    return CMD_OK;
  }
  
  for (int i = 0; i < history->count; i++) {
    fprintf(fp, "%s\n", history->lines[i]);
  }
  fclose(fp);
  printf("Session saved to %s\n", arg);
  return CMD_OK;
}

static cmd_result_t cmd_gc(struct js *js, history_t *history, const char *arg) {
  jsval_t gc_fn = js_get(js, rt->ant_obj, "gc");
  jsval_t result = js_call(js, gc_fn, NULL, 0);
  console_print(js, &result, 1, NULL, stdout);
  return CMD_OK;
}

static cmd_result_t cmd_stats(struct js *js, history_t *history, const char *arg) {
  jsval_t stats_fn = js_get(js, rt->ant_obj, "stats");
  jsval_t result = js_call(js, stats_fn, NULL, 0);
  console_print(js, &result, 1, NULL, stdout);
  return CMD_OK;
}

static cmd_result_t execute_command(struct js *js, history_t *history, const char *line) {
  const char *cmd_start = line + 1;
  
  for (const repl_command_t *cmd = commands; cmd->name; cmd++) {
    size_t n = strlen(cmd->name);
    if (strncmp(cmd_start, cmd->name, n) != 0) continue;
    
    char next = cmd_start[n];
    if (cmd->has_arg && (next == ' ' || next == '\0')) {
      const char *arg = cmd_start + n;
      while (*arg == ' ') arg++;
      return cmd->handler(js, history, arg);
    }
    if (!cmd->has_arg && next == '\0') return cmd->handler(js, history, NULL);
  }
  
  return CMD_NOT_FOUND;
}

static void history_init(history_t *hist) {
  hist->capacity = MAX_HISTORY;
  hist->lines = malloc(sizeof(char*) * hist->capacity);
  hist->count = 0;
  hist->current = -1;
}

static void history_add(history_t *hist, const char *line) {
  if (strlen(line) == 0) return;
  if (hist->count > 0 && strcmp(hist->lines[hist->count - 1], line) == 0) return;
  
  if (hist->count >= hist->capacity) {
    free(hist->lines[0]);
    memmove(hist->lines, hist->lines + 1, sizeof(char*) * (hist->capacity - 1));
    hist->count--;
  }
  
  hist->lines[hist->count++] = strdup(line);
  hist->current = hist->count;
}

static const char* history_prev(history_t *hist) {
  if (hist->count == 0) return NULL;
  if (hist->current > 0) hist->current--;
  return hist->lines[hist->current];
}

static const char* history_next(history_t *hist) {
  if (hist->count == 0) return NULL;
  if (hist->current < hist->count - 1) {
    hist->current++;
    return hist->lines[hist->current];
  }
  hist->current = hist->count;
  return "";
}

static void history_free(history_t *hist) {
  for (int i = 0; i < hist->count; i++) free(hist->lines[i]);
  free(hist->lines);
}

static char* get_history_path(void) {
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

static void history_load(history_t *hist) {
  char *path = get_history_path();
  if (!path) return;
  
  FILE *fp = fopen(path, "r");
  free(path);
  if (!fp) return;
  
  char line[MAX_LINE_LENGTH];
  while (fgets(line, sizeof(line), fp)) {
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
    if (line[0]) history_add(hist, line);
  }
  fclose(fp);
}

static void history_save(history_t *hist) {
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
  KEY_BACKSPACE, KEY_ENTER, KEY_EOF, KEY_CHAR 
} key_type_t;

typedef struct { key_type_t type; int ch; } key_event_t;
typedef void (*key_handler_t)(INPUT);

static void cursor_move(int *pos, int len, int dir) {
  if (dir < 0 && *pos > 0) { printf("\033[D"); fflush(stdout); (*pos)--; }
  else if (dir > 0 && *pos < len) { printf("\033[C"); fflush(stdout); (*pos)++; }
}

static void line_set(char *line, int *pos, int *len, const char *str, const char *prompt) {
  printf("\r\033[K%s%s", prompt, str);
  fflush(stdout); strcpy(line, str);
  *len = (int)strlen(line); *pos = *len;
}

static void line_backspace(char *line, int *pos, int *len) {
  if (*pos <= 0) return;
  
  memmove(line + *pos - 1, line + *pos, *len - *pos + 1);
  (*pos)--; (*len)--;
  printf("\b\033[K%s", line + *pos);
  for (int i = 0; i < *len - *pos; i++) printf("\033[D");
  fflush(stdout);
}

static void line_insert(char *line, int *pos, int *len, int c) {
  if (*len >= MAX_LINE_LENGTH - 1) return;
  
  memmove(line + *pos + 1, line + *pos, *len - *pos + 1);
  line[*pos] = (char)c;
  (*pos)++; (*len)++;
  printf("%c%s", c, line + *pos);
  for (int i = 0; i < *len - *pos; i++) printf("\033[D");
  fflush(stdout);
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
    }
    return (key_event_t){ KEY_NONE, 0 };
  }
  if (c == 8) return (key_event_t){ KEY_BACKSPACE, 0 };
  if (c == '\r' || c == '\n') return (key_event_t){ KEY_ENTER, 0 };
  if (c == 3) { ctrl_c_pressed++; return (key_event_t){ KEY_EOF, 0 }; }
  if (c == 4 || c == 26) return (key_event_t){ KEY_EOF, 0 };
  if (isprint(c) || (unsigned char)c >= 0x80) return (key_event_t){ KEY_CHAR, c };
  return (key_event_t){ KEY_NONE, 0 };
}
#define TERM_INIT()
#define TERM_RESTORE()
#else
static struct termios saved_tio;
static key_event_t read_key(void) {
  if (ctrl_c_pressed > 0) return (key_event_t){ KEY_EOF, 0 };
  int c = getchar();
  if (c == EOF && !feof(stdin)) { clearerr(stdin); return (key_event_t){ KEY_EOF, 0 }; }
  if (c == EOF) return (key_event_t){ KEY_EOF, 0 };
  if (c == 27) {
    if (getchar() == '[') {
      switch (getchar()) {
        case 'A': return (key_event_t){ KEY_UP, 0 };
        case 'B': return (key_event_t){ KEY_DOWN, 0 };
        case 'C': return (key_event_t){ KEY_RIGHT, 0 };
        case 'D': return (key_event_t){ KEY_LEFT, 0 };
      }
    }
    return (key_event_t){ KEY_NONE, 0 };
  }
  if (c == 127 || c == 8) return (key_event_t){ KEY_BACKSPACE, 0 };
  if (c == '\n' || c == '\r') return (key_event_t){ KEY_ENTER, 0 };
  if (isprint(c) || (unsigned char)c >= 0x80) return (key_event_t){ KEY_CHAR, c };
  return (key_event_t){ KEY_NONE, 0 };
}
#define TERM_INIT() do { \
  struct termios new_tio; \
  tcgetattr(STDIN_FILENO, &saved_tio); \
  new_tio = saved_tio; \
  new_tio.c_lflag &= ~(ICANON | ECHO); \
  tcsetattr(STDIN_FILENO, TCSANOW, &new_tio); \
} while(0)
#define TERM_RESTORE() tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio)
#endif

static void handle_up(INPUT) {
  const char *h = history_prev(hist);
  if (h) line_set(line, pos, len, h, prompt);
}

static void handle_down(INPUT) {
  const char *h = history_next(hist);
  if (h) line_set(line, pos, len, h, prompt);
}

static void handle_left(INPUT) { cursor_move(pos, *len, -1); }
static void handle_right(INPUT) { cursor_move(pos, *len, 1); }
static void handle_backspace(INPUT) { line_backspace(line, pos, len); }
static void handle_char(INPUT) { line_insert(line, pos, len, key->ch); }

static key_handler_t handlers[] = {
  [KEY_UP] = handle_up, [KEY_DOWN] = handle_down,
  [KEY_LEFT] = handle_left, [KEY_RIGHT] = handle_right,
  [KEY_BACKSPACE] = handle_backspace, [KEY_CHAR] = handle_char,
};

static char* read_line_with_history(history_t *hist, struct js *js, const char *prompt) {
  char *line = malloc(MAX_LINE_LENGTH);
  int pos = 0, len = 0; line[0] = '\0';
  
  TERM_INIT();
  
  do {
    key_event_t key = read_key();
    
    if (key.type == KEY_ENTER) {
      printf("\n"); fflush(stdout);
      TERM_RESTORE(); return line;
    }
    
    if (key.type == KEY_EOF) {
      printf("\n"); fflush(stdout);
      TERM_RESTORE();
      free(line); return NULL;
    }
    
    if (handlers[key.type]) {
      handlers[key.type](line, &pos, &len, &key, hist, prompt);
    }
  } while (1);
}

typedef struct {
  int paren, bracket, brace;
  int *templates;
  int template_count, template_cap;
  char string_char;
  bool in_string, escaped;
} parse_state_t;

static void push_template(parse_state_t *s) {
  if (s->template_count >= s->template_cap) {
    s->template_cap = s->template_cap ? s->template_cap * 2 : 8;
    int *new_templates = realloc(s->templates, s->template_cap * sizeof(int));
    if (!new_templates) { return; } s->templates = new_templates;
  }
  s->templates[s->template_count++] = s->brace;
}

static bool in_template_text(parse_state_t *s) {
  return s->template_count > 0 && s->brace == s->templates[s->template_count - 1];
}

static bool is_incomplete_input(const char *code, size_t len) {
  parse_state_t s = {0};
  
  for (size_t i = 0; i < len; i++) {
    char c = code[i];
    
    if (s.escaped) { s.escaped = false; continue; }
    if (c == '\\' && (s.in_string || s.template_count > 0)) { s.escaped = true; continue; }
    if (s.in_string) { if (c == s.string_char) s.in_string = false; continue; }
    
    if (in_template_text(&s)) {
      if (c == '`') s.template_count--;
      else if (c == '$' && i + 1 < len && code[i + 1] == '{') { s.brace++; i++; }
      continue;
    }
    
    if (c == '/' && i + 1 < len) {
      if (code[i + 1] == '/') { while (i < len && code[i] != '\n') i++; continue; }
      if (code[i + 1] == '*') {
        for (i += 2; i + 1 < len && !(code[i] == '*' && code[i + 1] == '/'); i++);
        if (i + 1 >= len) { free(s.templates); return true; }
        i++; continue;
      }
    }
    
    switch (c) {
      case '"': case '\'': s.in_string = true; s.string_char = c; break;
      case '`': push_template(&s); break;
      case '(': s.paren++; break;   case ')': s.paren--; break;
      case '[': s.bracket++; break; case ']': s.bracket--; break;
      case '{': s.brace++; break;   case '}': s.brace--; break;
    }
  }
  
  bool incomplete = s.in_string || s.template_count > 0 || s.paren > 0 || s.bracket > 0 || s.brace > 0;
  free(s.templates);
  return incomplete;
}

void ant_repl_run() {
  struct js *js = rt->js;
  
  js_set_filename(js, "[repl]");
  js_setup_import_meta(js, "[repl]");
  js_mkscope(js);
  
  printf("Welcome to Ant JavaScript v%s\n", ANT_VERSION);
  printf("Type \".help\" for more information.\n");
  
#ifdef _WIN32
  signal(SIGINT, sigint_handler);
#else
  struct sigaction sa;
  sa.sa_handler = sigint_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, NULL);
#endif
  
  history_t history;
  history_init(&history);
  history_load(&history);
  
  js_set(js, js_glob(js), "__dirname", js_mkstr(js, ".", 1));
  js_set(js, js_glob(js), "__filename", js_mkstr(js, "[repl]", 6));

  int prev_ctrl_c_count = 0;
  char *multiline_buf = NULL;
  size_t multiline_len = 0;
  size_t multiline_cap = 0;
  
  while (1) {
    const char *prompt = multiline_buf ? "| " : "> ";
    fputs(prompt, stdout);
    fflush(stdout);
    
    ctrl_c_pressed = 0;
    char *line = read_line_with_history(&history, js, prompt);
    
    if (ctrl_c_pressed > 0) {
      if (multiline_buf) {
        free(multiline_buf);
        multiline_buf = NULL;
        multiline_len = 0;
        multiline_cap = 0;
        prev_ctrl_c_count = 0;
        if (line) free(line);
        continue;
      }
      if (prev_ctrl_c_count > 0) {
        if (line) free(line);
        break;
      }
      printf("(To exit, press Ctrl+C again or type .exit)\n");
      prev_ctrl_c_count++;
      if (line) free(line);
      continue;
    }
    
    if (line == NULL) {
      if (multiline_buf) {
        free(multiline_buf);
        multiline_buf = NULL;
        multiline_len = 0;
        multiline_cap = 0;
        continue;
      }
      break;
    }
    
    prev_ctrl_c_count = 0;
    size_t line_len = strlen(line);
    
    if (line_len == 0 && multiline_buf) {
      if (multiline_len + 1 >= multiline_cap) {
        multiline_cap = multiline_cap ? multiline_cap * 2 : 256;
        multiline_buf = realloc(multiline_buf, multiline_cap);
      }
      multiline_buf[multiline_len++] = '\n';
      multiline_buf[multiline_len] = '\0';
      free(line);
      continue;
    }
    
    if (line_len == 0) {
      free(line);
      continue;
    }
    
    if (!multiline_buf && line[0] == '.') {
      cmd_result_t result = execute_command(js, &history, line);
      if (result == CMD_EXIT) {
        free(line);
        break;
      } else if (result == CMD_NOT_FOUND) {
        printf("Unknown command: %s\n", line);
        printf("Type \".help\" for more information.\n");
      }
      free(line);
      continue;
    }
    
    size_t new_len = multiline_len + line_len + 1;
    if (new_len >= multiline_cap || !multiline_buf) {
      multiline_cap = multiline_cap ? multiline_cap * 2 : 256;
      if (multiline_cap < new_len + 1) multiline_cap = new_len + 1;
      multiline_buf = realloc(multiline_buf, multiline_cap);
    }
    
    if (multiline_len > 0) {
      multiline_buf[multiline_len++] = '\n';
    }
    memcpy(multiline_buf + multiline_len, line, line_len);
    multiline_len += line_len;
    multiline_buf[multiline_len] = '\0';
    
    free(line);
    
    if (is_incomplete_input(multiline_buf, multiline_len)) continue;
    history_add(&history, multiline_buf);
    
    jsval_t eval_result = js_eval(js, multiline_buf, multiline_len);
    js_run_event_loop(js);
    
    if (vtype(eval_result) == T_ERR) {
      fprintf(stderr, "%s\n", js_str(js, eval_result));
    } else {
      const char *str = js_str(js, eval_result);
      print_value_colored(str, stdout);
      printf("\n");
    }
    
    free(multiline_buf);
    multiline_buf = NULL;
    multiline_len = 0;
    multiline_cap = 0;
  }
  
  if (multiline_buf) free(multiline_buf);
  history_save(&history);
  history_free(&history);
}
