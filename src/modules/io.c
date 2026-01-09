#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <libgen.h>
#include <uv.h>

#include "runtime.h"
#include "modules/io.h"
#include "modules/symbol.h"

bool io_no_color = false;

#define ANSI_RED "\x1b[31m"
#define ANSI_YELLOW "\x1b[33m"
#define ANSI_CYAN "\x1b[36m"
#define ANSI_MAGENTA "\x1b[35m"
#define ANSI_RESET "\x1b[0m"

#define JSON_KEY "\x1b[0m"
#define JSON_STRING "\x1b[32m"
#define JSON_NUMBER "\x1b[33m"
#define JSON_BOOL "\x1b[35m"
#define JSON_NULL "\x1b[90m"
#define JSON_BRACE "\x1b[37m"
#define JSON_FUNC "\x1b[36m"
#define JSON_TAG "\x1b[34m"
#define JSON_REF "\x1b[90m"

static void io_putc(int c, FILE *stream) { fputc(c, stream); }
static void io_puts(const char *s, FILE *stream) { fputs(s, stream); }

static void io_print(const char *str, FILE *stream) {
  if (!io_no_color) {
    fputs(str, stream);
    return;
  }

  static void *states[] = {&&normal, &&esc, &&csi, &&done};
  const char *p = str;
  char c;

  goto *states[0];

normal:
  c = *p++;
  if (!c) goto *states[3];
  if (c == '\x1b') goto *states[1];
  fputc(c, stream);
  goto *states[0];

esc:
  c = *p++;
  if (!c) goto *states[3];
  if (c == '[') goto *states[2];
  fputc('\x1b', stream);
  fputc(c, stream);
  goto *states[0];

csi:
  c = *p++;
  if (!c) goto *states[3];
  if ((c >= '0' && c <= '9') || c == ';') goto *states[2];
  if (c != 'm') fputc(c, stream);
  goto *states[0];

done:
  return;
}

void print_value_colored(const char *str, FILE *stream) {
  if (io_no_color) {
    io_print(str, stream);
    return;
  }

  bool in_string = false;
  bool escape_next = false;
  bool is_key = true;
  int brace_depth = 0;
  char string_char = 0;
  
  for (const char *p = str; *p; p++) {
    if (escape_next) {
      io_putc(*p, stream);
      escape_next = false;
      continue;
    }
    
    if (*p == '\\' && in_string) {
      io_putc(*p, stream);
      escape_next = true;
      continue;
    }
    
    if (*p == '\'' || *p == '"') {
      if (!in_string) {
        bool use_key_color = is_key && brace_depth > 0;
        io_puts(use_key_color ? JSON_KEY : JSON_STRING, stream);
        io_putc(*p, stream);
        in_string = true;
        string_char = *p;
      } else if (*p == string_char) {
        io_putc(*p, stream);
        io_puts(ANSI_RESET, stream);
        in_string = false;
        string_char = 0;
      } else {
        io_putc(*p, stream);
      }
      continue;
    }
    
    if (in_string) {
      io_putc(*p, stream);
      continue;
    }
    
    if (*p == '[' && strncmp(p, "[Function", 9) == 0) {
      io_puts(JSON_FUNC, stream);
      while (*p && *p != ']') io_putc(*p++, stream);
      if (*p == ']') io_putc(*p, stream);
      io_puts(ANSI_RESET, stream);
      continue;
    }
    
    if (*p == '[' && strncmp(p, "[Circular", 9) == 0) {
      io_puts(JSON_REF, stream);
      while (*p && *p != ']') io_putc(*p++, stream);
      if (*p == ']') io_putc(*p, stream);
      io_puts(ANSI_RESET, stream);
      continue;
    }
    
    if (*p == '<' && strncmp(p, "<ref", 4) == 0) {
      io_puts(JSON_REF, stream);
      while (*p && *p != '>') io_putc(*p++, stream);
      if (*p == '>') io_putc(*p, stream);
      io_puts(ANSI_RESET, stream);
      continue;
    }
    
    if (strncmp(p, "Object [", 8) == 0) {
      io_puts(JSON_TAG, stream);
      io_puts("Object [", stream);
      p += 7;
      while (*p && *p != ']') io_putc(*++p, stream);
      io_puts(ANSI_RESET, stream);
      continue;
    }
    
    if (*p == ':') {
      io_putc(*p, stream);
      is_key = false;
      continue;
    }
    
    if (*p == ',') {
      io_putc(*p, stream);
      is_key = (brace_depth > 0);
      continue;
    }
    
    if (*p == '\n') {
      io_putc(*p, stream);
      is_key = (brace_depth > 0);
      continue;
    }
    
    if (*p == '{') {
      io_puts(JSON_BRACE, stream);
      io_putc(*p, stream);
      io_puts(ANSI_RESET, stream);
      brace_depth++;
      is_key = true;
      continue;
    }
    
    if (*p == '}') {
      io_puts(JSON_BRACE, stream);
      io_putc(*p, stream);
      io_puts(ANSI_RESET, stream);
      brace_depth--;
      is_key = false;
      continue;
    }
    
    if (*p == '[') {
      io_puts(JSON_BRACE, stream);
      io_putc(*p, stream);
      io_puts(ANSI_RESET, stream);
      is_key = false;
      continue;
    }
    
    if (*p == ']') {
      io_puts(JSON_BRACE, stream);
      io_putc(*p, stream);
      io_puts(ANSI_RESET, stream);
      is_key = false;
      continue;
    }
    
    if (strncmp(p, "true", 4) == 0 && !isalnum((unsigned char)p[4]) && p[4] != '_') {
      io_puts(JSON_BOOL, stream);
      io_puts("true", stream);
      io_puts(ANSI_RESET, stream);
      p += 3;
      continue;
    }
    
    if (strncmp(p, "false", 5) == 0 && !isalnum((unsigned char)p[5]) && p[5] != '_') {
      io_puts(JSON_BOOL, stream);
      io_puts("false", stream);
      io_puts(ANSI_RESET, stream);
      p += 4;
      continue;
    }
    
    if (strncmp(p, "null", 4) == 0 && !isalnum((unsigned char)p[4]) && p[4] != '_') {
      io_puts(JSON_NULL, stream);
      io_puts("null", stream);
      io_puts(ANSI_RESET, stream);
      p += 3;
      continue;
    }
    
    if (strncmp(p, "undefined", 9) == 0 && !isalnum((unsigned char)p[9]) && p[9] != '_') {
      io_puts(JSON_NULL, stream);
      io_puts("undefined", stream);
      io_puts(ANSI_RESET, stream);
      p += 8;
      continue;
    }
    
    if (strncmp(p, "Infinity", 8) == 0 && !isalnum((unsigned char)p[8]) && p[8] != '_') {
      io_puts(JSON_NUMBER, stream);
      io_puts("Infinity", stream);
      io_puts(ANSI_RESET, stream);
      p += 7;
      continue;
    }
    
    if (strncmp(p, "NaN", 3) == 0 && !isalnum((unsigned char)p[3]) && p[3] != '_') {
      io_puts(JSON_NUMBER, stream);
      io_puts("NaN", stream);
      io_puts(ANSI_RESET, stream);
      p += 2;
      continue;
    }
    
    if ((*p >= '0' && *p <= '9') || (*p == '-' && p[1] >= '0' && p[1] <= '9')) {
      io_puts(JSON_NUMBER, stream);
      if (*p == '-') io_putc(*p++, stream);
      while ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-') {
        io_putc(*p, stream);
        if (!((p[1] >= '0' && p[1] <= '9') || p[1] == '.' || p[1] == 'e' || p[1] == 'E' || p[1] == '+' || p[1] == '-')) break;
        p++;
      }
      io_puts(ANSI_RESET, stream);
      continue;
    }
    
    if (is_key && brace_depth > 0 && (isalpha((unsigned char)*p) || *p == '_' || *p == '$')) {
      io_puts(JSON_KEY, stream);
      while (isalnum((unsigned char)*p) || *p == '_' || *p == '$') {
        io_putc(*p, stream);
        if (!(isalnum((unsigned char)p[1]) || p[1] == '_' || p[1] == '$')) break;
        p++;
      }
      io_puts(ANSI_RESET, stream);
      continue;
    }
    
    io_putc(*p, stream);
  }
}

static void console_print(struct js *js, jsval_t *args, int nargs, const char *color, FILE *stream) {
  if (color && !io_no_color) io_puts(color, stream);
  
  for (int i = 0; i < nargs; i++) {
    const char *space = i == 0 ? "" : " ";
    io_puts(space, stream);
    
    if (js_type(args[i]) == JS_STR) {
      char *str = js_getstr(js, args[i], NULL);
      io_print(str, stream);
    } else {
      const char *str = js_str(js, args[i]);
      if (color && !io_no_color) io_puts(ANSI_RESET, stream);
      print_value_colored(str, stream);
      if (color && !io_no_color) io_puts(color, stream);
    }
  }
  
  if (color && !io_no_color) io_puts(ANSI_RESET, stream);
  io_putc('\n', stream);
}

static jsval_t js_console_log(struct js *js, jsval_t *args, int nargs) {
  console_print(js, args, nargs, NULL, stdout);
  return js_mkundef();
}

static jsval_t js_console_error(struct js *js, jsval_t *args, int nargs) {
  console_print(js, args, nargs, ANSI_RED, stderr);
  return js_mkundef();
}

static jsval_t js_console_warn(struct js *js, jsval_t *args, int nargs) {
  console_print(js, args, nargs, ANSI_YELLOW, stderr);
  return js_mkundef();
}

static jsval_t js_console_assert(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  
  bool is_truthy = js_truthy(js, args[0]);
  if (is_truthy) return js_mkundef();
  
  if (!io_no_color) io_puts(ANSI_RED, stderr);
  io_puts("Assertion failed", stderr);
  if (nargs > 1) {
    io_puts(": ", stderr);
    for (int i = 1; i < nargs; i++) {
      const char *space = i == 1 ? "" : " ";
      io_puts(space, stderr);
      
      if (js_type(args[i]) == JS_STR) {
        char *str = js_getstr(js, args[i], NULL);
        io_print(str, stderr);
      } else {
        const char *str = js_str(js, args[i]);
        io_print(str, stderr);
      }
    }
  }
  if (!io_no_color) io_puts(ANSI_RESET, stderr);
  io_putc('\n', stderr);
  
  return js_mkundef();
}

static jsval_t js_console_trace(struct js *js, jsval_t *args, int nargs) {
  fprintf(stderr, "Console Trace");
  if (nargs > 0 && js_type(args[0]) == JS_STR) {
    fprintf(stderr, ": ");
    char *str = js_getstr(js, args[0], NULL);
    fprintf(stderr, "%s", str);
  }
  
  fprintf(stderr, "\n");
  js_print_stack_trace(stderr);
  
  return js_mkundef();
}

static jsval_t js_console_info(struct js *js, jsval_t *args, int nargs) {
  console_print(js, args, nargs, ANSI_CYAN, stdout);
  return js_mkundef();
}

static jsval_t js_console_debug(struct js *js, jsval_t *args, int nargs) {
  console_print(js, args, nargs, ANSI_MAGENTA, stdout);
  return js_mkundef();
}

static jsval_t js_console_clear(struct js *js, jsval_t *args, int nargs) {
  (void)args;
  (void)nargs;
  if (!io_no_color) {
    fprintf(stdout, "\033[2J\033[H");
    fflush(stdout);
  }
  return js_mkundef();
}

static struct { char *label; double start_time; } console_timers[64];
static int console_timer_count = 0;

static jsval_t js_console_time(struct js *js, jsval_t *args, int nargs) {
  const char *label = "default";
  if (nargs > 0 && js_type(args[0]) == JS_STR) {
    label = js_getstr(js, args[0], NULL);
  }
  
  for (int i = 0; i < console_timer_count; i++) {
    if (strcmp(console_timers[i].label, label) == 0) {
      fprintf(stderr, "Timer '%s' already exists\n", label);
      return js_mkundef();
    }
  }
  
  if (console_timer_count < 64) {
    console_timers[console_timer_count].label = strdup(label);
    console_timers[console_timer_count].start_time = uv_hrtime() / 1e6;
    console_timer_count++;
  }
  
  return js_mkundef();
}

static jsval_t js_console_timeEnd(struct js *js, jsval_t *args, int nargs) {
  const char *label = "default";
  if (nargs > 0 && js_type(args[0]) == JS_STR) {
    label = js_getstr(js, args[0], NULL);
  }
  
  for (int i = 0; i < console_timer_count; i++) {
    if (strcmp(console_timers[i].label, label) == 0) {
      double elapsed = (uv_hrtime() / 1e6) - console_timers[i].start_time;
      fprintf(stdout, "%s: %.3fms\n", label, elapsed);
      free(console_timers[i].label);
      for (int j = i; j < console_timer_count - 1; j++) {
        console_timers[j] = console_timers[j + 1];
      }
      console_timer_count--;
      return js_mkundef();
    }
  }
  
  fprintf(stderr, "Timer '%s' does not exist\n", label);
  return js_mkundef();
}

void init_console_module() {
  struct js *js = rt->js;
  jsval_t console_obj = js_mkobj(js);
  
  js_set(js, console_obj, "log", js_mkfun(js_console_log));
  js_set(js, console_obj, "error", js_mkfun(js_console_error));
  js_set(js, console_obj, "warn", js_mkfun(js_console_warn));
  js_set(js, console_obj, "info", js_mkfun(js_console_info));
  js_set(js, console_obj, "debug", js_mkfun(js_console_debug));
  js_set(js, console_obj, "assert", js_mkfun(js_console_assert));
  js_set(js, console_obj, "trace", js_mkfun(js_console_trace));
  js_set(js, console_obj, "time", js_mkfun(js_console_time));
  js_set(js, console_obj, "timeEnd", js_mkfun(js_console_timeEnd));
  js_set(js, console_obj, "clear", js_mkfun(js_console_clear));
  
  js_set(js, console_obj, get_toStringTag_sym_key(), js_mkstr(js, "console", 7));
  js_set(js, js_glob(js), "console", console_obj);
}