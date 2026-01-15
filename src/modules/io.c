#include <compat.h> // IWYU pragma: keep

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
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

enum char_class {
  CC_OTHER,  CC_NUL,    CC_QUOTE,  CC_ESCAPE, CC_LBRACE, CC_RBRACE,
  CC_LBRACK, CC_RBRACK, CC_COLON,  CC_COMMA,  CC_NEWLINE,
  CC_DIGIT,  CC_MINUS,  CC_ALPHA,  CC_IDENT,  CC_LT
};

static const uint8_t char_class_table[256] = {
  [0] = CC_NUL,
  ['\n'] = CC_NEWLINE, ['"'] = CC_QUOTE, ['\''] = CC_QUOTE, ['\\'] = CC_ESCAPE,
  ['{'] = CC_LBRACE, ['}'] = CC_RBRACE, ['['] = CC_LBRACK, [']'] = CC_RBRACK,
  [':'] = CC_COLON, [','] = CC_COMMA, ['-'] = CC_MINUS, ['<'] = CC_LT,
  ['_'] = CC_IDENT, ['$'] = CC_IDENT,
  ['0'] = CC_DIGIT, ['1'] = CC_DIGIT, ['2'] = CC_DIGIT, ['3'] = CC_DIGIT,
  ['4'] = CC_DIGIT, ['5'] = CC_DIGIT, ['6'] = CC_DIGIT, ['7'] = CC_DIGIT,
  ['8'] = CC_DIGIT, ['9'] = CC_DIGIT,
  ['a'] = CC_ALPHA, ['b'] = CC_ALPHA, ['c'] = CC_ALPHA, ['d'] = CC_ALPHA,
  ['e'] = CC_ALPHA, ['f'] = CC_ALPHA, ['g'] = CC_ALPHA, ['h'] = CC_ALPHA,
  ['i'] = CC_ALPHA, ['j'] = CC_ALPHA, ['k'] = CC_ALPHA, ['l'] = CC_ALPHA,
  ['m'] = CC_ALPHA, ['n'] = CC_ALPHA, ['o'] = CC_ALPHA, ['p'] = CC_ALPHA,
  ['q'] = CC_ALPHA, ['r'] = CC_ALPHA, ['s'] = CC_ALPHA, ['t'] = CC_ALPHA,
  ['u'] = CC_ALPHA, ['v'] = CC_ALPHA, ['w'] = CC_ALPHA, ['x'] = CC_ALPHA,
  ['y'] = CC_ALPHA, ['z'] = CC_ALPHA,
  ['A'] = CC_ALPHA, ['B'] = CC_ALPHA, ['C'] = CC_ALPHA, ['D'] = CC_ALPHA,
  ['E'] = CC_ALPHA, ['F'] = CC_ALPHA, ['G'] = CC_ALPHA, ['H'] = CC_ALPHA,
  ['I'] = CC_ALPHA, ['J'] = CC_ALPHA, ['K'] = CC_ALPHA, ['L'] = CC_ALPHA,
  ['M'] = CC_ALPHA, ['N'] = CC_ALPHA, ['O'] = CC_ALPHA, ['P'] = CC_ALPHA,
  ['Q'] = CC_ALPHA, ['R'] = CC_ALPHA, ['S'] = CC_ALPHA, ['T'] = CC_ALPHA,
  ['U'] = CC_ALPHA, ['V'] = CC_ALPHA, ['W'] = CC_ALPHA, ['X'] = CC_ALPHA,
  ['Y'] = CC_ALPHA, ['Z'] = CC_ALPHA,
};

#define KEYWORD(kw, color) \
  if (memcmp(p, kw, sizeof(kw) - 1) == 0 && !isalnum((unsigned char)p[sizeof(kw) - 1]) && p[sizeof(kw) - 1] != '_') { \
    io_puts(color, stream); io_puts(kw, stream); io_puts(ANSI_RESET, stream); \
    p += sizeof(kw) - 1; goto next; \
  }

#define EMIT_UNTIL(end_char, color) \
  io_puts(color, stream); \
  while (*p && *p != end_char) io_putc(*p++, stream); \
  if (*p == end_char) io_putc(*p++, stream); \
  io_puts(ANSI_RESET, stream); goto next;

void print_value_colored(const char *str, FILE *stream) {
  if (io_no_color) { io_print(str, stream); return; }

  static void *dispatch[] = {
    [CC_NUL] = &&done, [CC_QUOTE] = &&quote, [CC_ESCAPE] = &&other,
    [CC_LBRACE] = &&lbrace, [CC_RBRACE] = &&rbrace,
    [CC_LBRACK] = &&lbrack, [CC_RBRACK] = &&rbrack,
    [CC_COLON] = &&colon, [CC_COMMA] = &&separator, [CC_NEWLINE] = &&separator,
    [CC_DIGIT] = &&number, [CC_MINUS] = &&minus,
    [CC_ALPHA] = &&alpha, [CC_IDENT] = &&ident, [CC_LT] = &&lt, [CC_OTHER] = &&other
  };

  const char *p = str;
  char string_char = 0;
  int brace_depth = 0, array_depth = 0;
  bool is_key = true;

  goto next;

next:
  goto *dispatch[char_class_table[(unsigned char)*p]];

done:
  return;

quote:
  string_char = *p;
  io_puts((is_key && brace_depth > 0) ? JSON_KEY : JSON_STRING, stream);
  io_putc(*p++, stream);
  while (*p) {
    if (*p == '\\' && p[1]) { io_putc(*p++, stream); io_putc(*p++, stream); continue; }
    if (*p == string_char) { io_putc(*p++, stream); break; }
    io_putc(*p++, stream);
  }
  io_puts(ANSI_RESET, stream);
  goto next;

lbrace:
  io_puts(JSON_BRACE, stream); io_putc(*p++, stream); io_puts(ANSI_RESET, stream);
  brace_depth++; is_key = true; goto next;

rbrace:
  io_puts(JSON_BRACE, stream); io_putc(*p++, stream); io_puts(ANSI_RESET, stream);
  brace_depth--; is_key = false; goto next;

lbrack:
  if (memcmp(p, "[Function", 9) == 0 || memcmp(p, "[native code]", 13) == 0) { EMIT_UNTIL(']', JSON_FUNC) }
  if (memcmp(p, "[Circular", 9) == 0) { EMIT_UNTIL(']', JSON_REF) }
  io_puts(JSON_BRACE, stream); io_putc(*p++, stream); io_puts(ANSI_RESET, stream);
  array_depth++; is_key = false; goto next;

rbrack:
  io_puts(JSON_BRACE, stream); io_putc(*p++, stream); io_puts(ANSI_RESET, stream);
  array_depth--; is_key = false; goto next;

colon:
  io_putc(*p++, stream); is_key = false; goto next;

separator:
  io_putc(*p++, stream);
  is_key = (brace_depth > 0 && array_depth == 0);
  goto next;

number:
  io_puts(JSON_NUMBER, stream);
  while ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')
    io_putc(*p++, stream);
  io_puts(ANSI_RESET, stream);
  goto next;

minus:
  if (p[1] >= '0' && p[1] <= '9') {
    io_puts(JSON_NUMBER, stream); io_putc(*p++, stream);
    while ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')
      io_putc(*p++, stream);
    io_puts(ANSI_RESET, stream);
    goto next;
  }
  io_putc(*p++, stream); goto next;

lt:
  if (memcmp(p, "<ref", 4) == 0) { EMIT_UNTIL('>', JSON_REF) }
  while (*p && *p != '>') io_putc(*p++, stream);
  if (*p == '>') io_putc(*p++, stream);
  goto next;

alpha:
  if (memcmp(p, "Object [", 8) == 0) {
    io_puts(JSON_TAG, stream); io_puts("Object [", stream);
    p += 8;
    while (*p && *p != ']') io_putc(*p++, stream);
    io_puts(ANSI_RESET, stream);
    goto next;
  }
  KEYWORD("true", JSON_BOOL)
  KEYWORD("false", JSON_BOOL)
  KEYWORD("null", JSON_NULL)
  KEYWORD("undefined", JSON_NULL)
  KEYWORD("Infinity", JSON_NUMBER)
  KEYWORD("NaN", JSON_NUMBER)

ident:
  if (is_key && brace_depth > 0) {
    io_puts(JSON_KEY, stream);
    while (isalnum((unsigned char)*p) || *p == '_' || *p == '$') io_putc(*p++, stream);
    io_puts(ANSI_RESET, stream);
    goto next;
  }
  io_putc(*p++, stream); goto next;

other:
  io_putc(*p++, stream); goto next;
}

#undef KEYWORD
#undef EMIT_UNTIL

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