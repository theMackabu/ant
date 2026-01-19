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
#include "config.h"
#include "runtime.h"
#include "modules/io.h"

#define MAX_HISTORY 512
#define MAX_LINE_LENGTH 4096

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
    if (js_type(result) == JS_ERR) {
      fprintf(stderr, "%s\n", js_str(js, result));
    } else if (js_type(result) != JS_UNDEF) {
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
    size_t name_len = strlen(cmd->name);
    
    if (cmd->has_arg) {
      if (
        strncmp(cmd_start, cmd->name, name_len) == 0 &&
        (cmd_start[name_len] == ' ' || cmd_start[name_len] == '\0')
      ) {
        const char *arg = cmd_start + name_len;
        while (*arg == ' ') arg++;
        return cmd->handler(js, history, arg);
      }
    } else {
      if (strcmp(cmd_start, cmd->name) == 0) return cmd->handler(js, history, NULL);
    }
  }
  
  return CMD_NOT_FOUND;
}

static void sigint_handler(int sig) {
  (void)sig;
  ctrl_c_pressed++;
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

typedef enum { KEY_NONE, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_BACKSPACE, KEY_ENTER, KEY_EOF, KEY_CHAR } key_type_t;
typedef struct { key_type_t type; int ch; } key_event_t;

static void line_set(char *line, int *pos, int *len, const char *str) {
  printf("\r\033[K> %s", str);
  fflush(stdout);
  strcpy(line, str);
  *len = strlen(line);
  *pos = *len;
}

static void line_backspace(char *line, int *pos, int *len) {
  if (*pos > 0) {
    memmove(line + *pos - 1, line + *pos, *len - *pos + 1);
    (*pos)--; (*len)--;
    printf("\b\033[K%s", line + *pos);
    for (int i = 0; i < *len - *pos; i++) printf("\033[D");
    fflush(stdout);
  }
}

static void line_insert(char *line, int *pos, int *len, int c) {
  if (*len < MAX_LINE_LENGTH - 1) {
    memmove(line + *pos + 1, line + *pos, *len - *pos + 1);
    line[*pos] = c;
    (*pos)++; (*len)++;
    printf("%c%s", c, line + *pos);
    for (int i = 0; i < *len - *pos; i++) printf("\033[D");
    fflush(stdout);
  }
}

static void cursor_move(int *pos, int len, int dir) {
  if (dir < 0 && *pos > 0) { printf("\033[D"); fflush(stdout); (*pos)--; }
  else if (dir > 0 && *pos < len) { printf("\033[C"); fflush(stdout); (*pos)++; }
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
  if (isprint(c)) return (key_event_t){ KEY_CHAR, c };
  return (key_event_t){ KEY_NONE, 0 };
}
#define TERM_INIT()
#define TERM_RESTORE()
#else
static struct termios saved_tio;
static key_event_t read_key(void) {
  if (ctrl_c_pressed > 0) return (key_event_t){ KEY_EOF, 0 };
  int c = getchar();
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
  if (isprint(c)) return (key_event_t){ KEY_CHAR, c };
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

static char* read_line_with_history(history_t *hist, struct js *js) {
  (void)js;
  char *line = malloc(MAX_LINE_LENGTH);
  int pos = 0, len = 0;
  line[0] = '\0';
  
  TERM_INIT();
  
  while (1) {
    key_event_t key = read_key();
    const char *hist_line;
    
    switch (key.type) {
      case KEY_UP:
        if ((hist_line = history_prev(hist))) line_set(line, &pos, &len, hist_line);
        break;
      case KEY_DOWN:
        if ((hist_line = history_next(hist))) line_set(line, &pos, &len, hist_line);
        break;
      case KEY_LEFT:      cursor_move(&pos, len, -1); break;
      case KEY_RIGHT:     cursor_move(&pos, len, 1); break;
      case KEY_BACKSPACE: line_backspace(line, &pos, &len); break;
      case KEY_CHAR:      line_insert(line, &pos, &len, key.ch); break;
      case KEY_ENTER:
        printf("\n"); fflush(stdout);
        TERM_RESTORE();
        line[len] = '\0';
        return line;
      case KEY_EOF:
        printf("\n"); fflush(stdout);
        TERM_RESTORE();
        free(line);
        return NULL;
      case KEY_NONE: break;
    }
  }
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
  
  while (1) {
    printf("> ");
    fflush(stdout);
    
    ctrl_c_pressed = 0;
    char *line = read_line_with_history(&history, js);
    
    if (ctrl_c_pressed > 0) {
      if (prev_ctrl_c_count > 0) {
        printf("\n");
        if (line) free(line);
        break;
      }
      printf("(To exit, press Ctrl+C again or type .exit)\n");
      prev_ctrl_c_count++;
      if (line) free(line);
      continue;
    }
    
    if (line == NULL) break;      
    prev_ctrl_c_count = 0;
    size_t line_len = strlen(line);
    
    if (line_len == 0) {
      free(line);
      continue;
    }
    
    history_add(&history, line);
    
    if (line[0] == '.') {
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
    
    jsval_t eval_result = js_eval(js, line, line_len);
    js_run_event_loop(js);
    
    if (js_type(eval_result) == JS_ERR) fprintf(stderr, "%s\n", js_str(js, eval_result)); else {
      const char *str = js_str(js, eval_result);
      print_value_colored(str, stdout);
      printf("\n");
    }
    
    free(line);
  }
  
  history_save(&history);
  history_free(&history);
}
