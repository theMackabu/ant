#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <ctype.h>
#include <signal.h>
#include <unistd.h>

#include "ant.h"
#include "repl.h"
#include "config.h"
#include "runtime.h"
#include "modules/io.h"

#define MAX_HISTORY 100
#define MAX_LINE_LENGTH 4096

static volatile sig_atomic_t ctrl_c_pressed = 0;

typedef struct {
  char **lines;
  int count;
  int capacity;
  int current;
} history_t;

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

static char* read_line_with_history(history_t *hist, struct js *js) {
  static struct termios old_tio, new_tio;
  char *line = malloc(MAX_LINE_LENGTH);
  
  int pos = 0;
  int len = 0;
  line[0] = '\0';
  
  tcgetattr(STDIN_FILENO, &old_tio);
  new_tio = old_tio;
  new_tio.c_lflag &= ~(ICANON | ECHO);
  tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
  
  while (1) {
    if (ctrl_c_pressed > 0) {
      printf("\n");
      fflush(stdout);
      tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
      free(line);
      return NULL;
    }
    
    int c = getchar();
    
    if (c == EOF) {
      if (ctrl_c_pressed > 0) {
        printf("\n");
        fflush(stdout);
        tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
        free(line);
        return NULL;
      }
      tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
      free(line);
      return NULL;
    }
    
    if (c == 27) {
      int next1 = getchar();
      if (next1 == '[') {
        int next2 = getchar();
        
        if (next2 == 'A') {
          const char *prev = history_prev(hist);
          if (prev) {
            printf("\r\033[K> ");
            fflush(stdout);
            
            strcpy(line, prev);
            len = strlen(line);
            pos = len;
            printf("%s", line);
            fflush(stdout);
          }
          continue;
        } else if (next2 == 'B') {
          const char *next = history_next(hist);
          if (next) {
            printf("\r\033[K> ");
            fflush(stdout);
            
            strcpy(line, next);
            len = strlen(line);
            pos = len;
            printf("%s", line);
            fflush(stdout);
          }
          continue;
        } else if (next2 == 'C') {
          if (pos < len) {
            printf("\033[C");
            fflush(stdout);
            pos++;
          }
          continue;
        } else if (next2 == 'D') {
          if (pos > 0) {
            printf("\033[D");
            fflush(stdout);
            pos--;
          }
          continue;
        }
      }
      continue;
    }
    
    if (c == 127 || c == 8) {
      if (pos > 0) {
        memmove(line + pos - 1, line + pos, len - pos + 1);
        pos--;
        len--;
        printf("\b\033[K%s", line + pos);
        for (int i = 0; i < len - pos; i++) printf("\033[D");
        fflush(stdout);
      }
      continue;
    }
    
    if (c == '\n' || c == '\r') {
      printf("\n");
      fflush(stdout);
      tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
      line[len] = '\0';
      return line;
    }
    
    if (isprint(c) && len < MAX_LINE_LENGTH - 1) {
      memmove(line + pos + 1, line + pos, len - pos + 1);
      line[pos] = c;
      pos++;
      len++;
      printf("%c%s", c, line + pos);
      for (int i = 0; i < len - pos; i++) printf("\033[D");
      fflush(stdout);
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
  
  struct sigaction sa;
  sa.sa_handler = sigint_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, NULL);
  
  history_t history;
  history_init(&history);
  
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
      if (strcmp(line, ".help") == 0) {
        printf("  .help    - Show this help message\n");
        printf("  .exit    - Exit the REPL\n");
        printf("  .clear   - Clear the current context\n");
        printf("  .load    - Load JS from a file into the REPL session\n");
        printf("  .save    - Save all evaluated commands in this REPL session to a file\n");
        printf("  .gc      - Run garbage collector\n");
        printf("  .stats   - Show memory statistics\n");
        printf("\nPress Ctrl+C to abort current expression.\n");
      } else if (strcmp(line, ".exit") == 0) {
        free(line);
        break;
      } else if (strcmp(line, ".clear") == 0) {
        printf("Clearing context...\n");
      } else if (strncmp(line, ".load", 5) == 0) {
        const char *filename = line + 5;
        while (*filename == ' ') filename++;
        
        if (*filename == '\0') {
          fprintf(stderr, "Usage: .load <filename>\n");
        } else {
          FILE *fp = fopen(filename, "r");
          if (fp == NULL) {
            fprintf(stderr, "Failed to open file: %s\n", filename);
          } else {
            fseek(fp, 0, SEEK_END);
            long file_size = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            
            char *file_buffer = malloc(file_size + 1);
            if (file_buffer) {
              size_t len = fread(file_buffer, 1, file_size, fp);
              file_buffer[len] = '\0';
              
              jsval_t result = js_eval(js, file_buffer, len);
              if (js_type(result) == JS_ERR) {
                fprintf(stderr, "%s\n", js_str(js, result));
              } else if (js_type(result) != JS_UNDEF) {
                printf("%s\n", js_str(js, result));
              }
              
              free(file_buffer);
            }
            fclose(fp);
          }
        }
      } else if (strncmp(line, ".save", 5) == 0) {
        const char *filename = line + 5;
        while (*filename == ' ') filename++;
        
        if (*filename == '\0') {
          fprintf(stderr, "Usage: .save <filename>\n");
        } else {
          FILE *fp = fopen(filename, "w");
          if (fp == NULL) {
            fprintf(stderr, "Failed to open file for writing: %s\n", filename);
          } else {
            for (int i = 0; i < history.count; i++) fprintf(fp, "%s\n", history.lines[i]);
            fclose(fp);
            printf("Session saved to %s\n", filename);
          }
        }
      } else if (strcmp(line, ".gc") == 0) {
        printf("Garbage collection complete\n");
      } else if (strcmp(line, ".stats") == 0) {
        size_t total, min, cstack;
        js_stats(js, &total, &min, &cstack);
        printf("Memory stats:\n");
        printf("  Total: %zu bytes\n", total);
        printf("  Free: %zu bytes\n", min);
        printf("  C Stack: %zu bytes\n", cstack);
      } else {
        printf("Unknown command: %s\n", line);
        printf("Type \".help\" for more information.\n");
      }
      free(line);
      continue;
    }
    
    jsval_t eval_result = js_eval(js, line, line_len);
    
    if (js_type(eval_result) == JS_ERR) fprintf(stderr, "%s\n", js_str(js, eval_result)); else {
      const char *str = js_str(js, eval_result);
      print_value_colored(str, stdout);
      printf("\n");
    }
    
    free(line);
  }
  
  history_free(&history);
}
