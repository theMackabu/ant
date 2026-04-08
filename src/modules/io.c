#include <compat.h> // IWYU pragma: keep

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <inttypes.h>
#include <uv.h>

#include "common.h"
#include "errors.h"
#include "output.h"
#include "internal.h"
#include "runtime.h"
#include "silver/engine.h"
#include "modules/io.h"
#include "modules/symbol.h"

bool io_no_color = false;

#define JSON_KEY    "\x1b[0m"
#define JSON_STRING "\x1b[32m"
#define JSON_NUMBER "\x1b[33m"
#define JSON_BOOL   "\x1b[35m"
#define JSON_NULL   "\x1b[90m"
#define JSON_BRACE  "\x1b[37m"
#define JSON_FUNC   "\x1b[36m"
#define JSON_TAG    "\x1b[34m"
#define JSON_REF    "\x1b[90m"
#define JSON_WHITE  "\x1b[97m"

static inline bool io_is_digit_ascii(char c) { 
  return c >= '0' && c <= '9'; 
}

static bool io_print_to_output(const char *str, ant_output_stream_t *out) {
  if (!io_no_color) {
    return ant_output_stream_append_cstr(out, str);
  }

  static void *states[] = {&&normal, &&esc, &&csi, &&done};
  const char *p = str; char c;

  goto *states[0];

  normal: {
    c = *p++;
    if (!c) goto *states[3];
    if (c == '\x1b') goto *states[1];
    if (!ant_output_stream_putc(out, c)) return false;
    goto *states[0];
  }

  esc: {
    c = *p++;
    if (!c) goto *states[3];
    if (c == '[') goto *states[2];
    if (!ant_output_stream_putc(out, '\x1b')) return false;
    if (!ant_output_stream_putc(out, c)) return false;
    goto *states[0];
  }

  csi: {
    c = *p++;
    if (!c) goto *states[3];
    if ((c >= '0' && c <= '9') || c == ';') goto *states[2];
    if (c != 'm' && !ant_output_stream_putc(out, c)) return false;
    goto *states[0];
  }
  
  done: return true;
}

enum char_class {
  CC_OTHER,  CC_NUL,    CC_QUOTE,  CC_ESCAPE, CC_LBRACE, CC_RBRACE,
  CC_LBRACK, CC_RBRACK, CC_COLON,  CC_COMMA,  CC_NEWLINE,
  CC_DIGIT,  CC_MINUS,  CC_ALPHA,  CC_IDENT,  CC_LT,     CC_GT
};

static const uint8_t char_class_table[256] = {
  [0] = CC_NUL,
  ['\n'] = CC_NEWLINE, ['"'] = CC_QUOTE, ['\''] = CC_QUOTE, ['\\'] = CC_ESCAPE,
  ['{'] = CC_LBRACE, ['}'] = CC_RBRACE, ['['] = CC_LBRACK, [']'] = CC_RBRACK,
  [':'] = CC_COLON, [','] = CC_COMMA, ['-'] = CC_MINUS, ['<'] = CC_LT, ['>'] = CC_GT,
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

static int io_iso_utc_token_len(const char *p) {
  int i = 0;

  if (*p == '+' || *p == '-') {
    i++;
    for (int d = 0; d < 6; d++, i++) if (!io_is_digit_ascii(p[i])) return 0;
  } else for (int d = 0; d < 4; d++, i++) if (!io_is_digit_ascii(p[i])) return 0;

  if (p[i++] != '-') return 0;
  if (!io_is_digit_ascii(p[i++]) || !io_is_digit_ascii(p[i++])) return 0;
  if (p[i++] != '-') return 0;
  if (!io_is_digit_ascii(p[i++]) || !io_is_digit_ascii(p[i++])) return 0;
  if (p[i++] != 'T') return 0;
  if (!io_is_digit_ascii(p[i++]) || !io_is_digit_ascii(p[i++])) return 0;
  if (p[i++] != ':') return 0;
  if (!io_is_digit_ascii(p[i++]) || !io_is_digit_ascii(p[i++])) return 0;
  if (p[i++] != ':') return 0;
  if (!io_is_digit_ascii(p[i++]) || !io_is_digit_ascii(p[i++])) return 0;
  if (p[i++] != '.') return 0;
  if (!io_is_digit_ascii(p[i++]) || !io_is_digit_ascii(p[i++]) || !io_is_digit_ascii(p[i++])) return 0;
  if (p[i++] != 'Z') return 0;

  char boundary = p[i];
  if (
    !(boundary == '\0'
    || boundary == ' '
    || boundary == '\t'
    || boundary == '\n'
    || boundary == ','
    || boundary == ']'
    || boundary == '}'
    || boundary == ')'
    || boundary == '>')
  ) return 0;

  return i;
}

#define KEYWORD(kw, color) \
  if (memcmp(p, kw, sizeof(kw) - 1) == 0 && !isalnum((unsigned char)p[sizeof(kw) - 1]) && p[sizeof(kw) - 1] != '_') { \
    ant_output_stream_append_cstr(out, color); ant_output_stream_append_cstr(out, kw); ant_output_stream_append_cstr(out, C_RESET); \
    p += sizeof(kw) - 1; goto next; \
  }

#define EMIT_UNTIL(end_char, color) \
  ant_output_stream_append_cstr(out, color); \
  while (*p && *p != end_char) ant_output_stream_putc(out, *p++); \
  if (*p == end_char) ant_output_stream_putc(out, *p++); \
  ant_output_stream_append_cstr(out, C_RESET); goto next;
  
#define EMIT_TYPE(tag, len, color) \
  if (!(is_key && brace_depth > 0) && memcmp(p, tag, len) == 0) { \
    ant_output_stream_append_cstr(out, color); ant_output_stream_append_cstr(out, tag); ant_output_stream_append_cstr(out, C_RESET); \
    p += len; goto next; \
  }

static void print_value_colored_to_output(const char *str, ant_output_stream_t *out) {
  if (io_no_color) { io_print_to_output(str, out); return; }

  static void *dispatch[] = {
    [CC_NUL] = &&done, [CC_QUOTE] = &&quote, [CC_ESCAPE] = &&other,
    [CC_LBRACE] = &&lbrace, [CC_RBRACE] = &&rbrace,
    [CC_LBRACK] = &&lbrack, [CC_RBRACK] = &&rbrack,
    [CC_COLON] = &&colon, [CC_COMMA] = &&separator, [CC_NEWLINE] = &&separator,
    [CC_DIGIT] = &&number, [CC_MINUS] = &&minus,
    [CC_ALPHA] = &&alpha, [CC_IDENT] = &&ident, [CC_LT] = &&lt, [CC_GT] = &&gt, [CC_OTHER] = &&other
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
  ant_output_stream_append_cstr(out, (is_key && brace_depth > 0) ? JSON_KEY : JSON_STRING);
  ant_output_stream_putc(out, *p++);
  while (*p) {
    if (*p == '\\' && p[1]) { ant_output_stream_putc(out, *p++); ant_output_stream_putc(out, *p++); continue; }
    if (*p == string_char) { ant_output_stream_putc(out, *p++); break; }
    ant_output_stream_putc(out, *p++);
  }
  ant_output_stream_append_cstr(out, C_RESET);
  goto next;

lbrace:
  ant_output_stream_append_cstr(out, JSON_BRACE); ant_output_stream_putc(out, *p++); ant_output_stream_append_cstr(out, C_RESET);
  brace_depth++; is_key = true; goto next;

rbrace:
  ant_output_stream_append_cstr(out, JSON_BRACE); ant_output_stream_putc(out, *p++); ant_output_stream_append_cstr(out, C_RESET);
  brace_depth--; is_key = false; goto next;

lbrack:
  switch (p[1]) {
    case 'A': if (memcmp(p + 2, "syncFunction", 7) == 0) { EMIT_UNTIL(']', JSON_FUNC) } break;
    case 'b': if (memcmp(p + 2, "yte", 3) == 0 || memcmp(p + 2, "uffer]", 6) == 0) { EMIT_UNTIL(']', JSON_STRING) } break;
    case 'F': if (memcmp(p + 2, "unction", 7) == 0) { EMIT_UNTIL(']', JSON_FUNC) } break;
    case 'n': if (memcmp(p + 2, "ative code", 10) == 0) { EMIT_UNTIL(']', JSON_FUNC) } break;
    case 'C': if (memcmp(p + 2, "ircular", 7) == 0) { EMIT_UNTIL(']', JSON_REF) } break;
    case 'G': if (memcmp(p + 2, "etter/Setter]", 13) == 0 || memcmp(p + 2, "etter]", 6) == 0) { EMIT_UNTIL(']', JSON_FUNC) } break;
    case 'S': if (memcmp(p + 2, "etter]", 6) == 0) { EMIT_UNTIL(']', JSON_FUNC) } break;
    case 'O': if (memcmp(p + 2, "bject: null prototype]", 22) == 0) { EMIT_UNTIL(']', JSON_TAG) } break;
    case 'M': if (memcmp(p + 2, "odule]", 6) == 0) { EMIT_UNTIL(']', JSON_TAG) } break;
    case 'U': if (memcmp(p + 2, "int8Contents]", 13) == 0) { EMIT_UNTIL(']', JSON_FUNC) } break;
    case 'P': if (memcmp(p + 2, "romise]", 7) == 0) { EMIT_UNTIL(']', JSON_TAG) } break;
  }
  ant_output_stream_append_cstr(out, JSON_BRACE); ant_output_stream_putc(out, *p++); ant_output_stream_append_cstr(out, C_RESET);
  array_depth++; is_key = false; goto next;

rbrack:
  ant_output_stream_append_cstr(out, JSON_BRACE); ant_output_stream_putc(out, *p++); ant_output_stream_append_cstr(out, C_RESET);
  array_depth--; is_key = false; goto next;

colon:
  ant_output_stream_putc(out, *p++); is_key = false; goto next;

separator:
  ant_output_stream_putc(out, *p++);
  is_key = (brace_depth > 0 && array_depth == 0);
  goto next;

number: {
    int iso_len = io_iso_utc_token_len(p);
    if (iso_len > 0) {
      ant_output_stream_append_cstr(out, C_MAGENTA);
      for (int k = 0; k < iso_len; k++) ant_output_stream_putc(out, *p++);
      ant_output_stream_append_cstr(out, C_RESET);
      goto next;
    }
  }
  ant_output_stream_append_cstr(out, JSON_NUMBER);
  while ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')
    ant_output_stream_putc(out, *p++);
  ant_output_stream_append_cstr(out, C_RESET);
  goto next;

minus: {
    int iso_len = io_iso_utc_token_len(p);
    if (iso_len > 0) {
      ant_output_stream_append_cstr(out, C_MAGENTA);
      for (int k = 0; k < iso_len; k++) ant_output_stream_putc(out, *p++);
      ant_output_stream_append_cstr(out, C_RESET);
      goto next;
    }
  }
  if (memcmp(p + 1, "Infinity", 8) == 0 && !isalnum((unsigned char)p[9]) && p[9] != '_') {
    ant_output_stream_append_cstr(out, JSON_NUMBER); ant_output_stream_append_cstr(out, "-Infinity"); ant_output_stream_append_cstr(out, C_RESET);
    p += 9; goto next;
  }
  if (p[1] >= '0' && p[1] <= '9') {
    ant_output_stream_append_cstr(out, JSON_NUMBER); ant_output_stream_putc(out, *p++);
    while ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')
      ant_output_stream_putc(out, *p++);
    ant_output_stream_append_cstr(out, C_RESET);
    goto next;
  }
  ant_output_stream_putc(out, *p++); goto next;

lt:
  if (memcmp(p, "<ref", 4) == 0) { EMIT_UNTIL('>', JSON_REF) }
  if (memcmp(p, "<pen", 4) == 0) { is_key = false; EMIT_UNTIL('>', C_CYAN) }
  if (memcmp(p, "<rej", 4) == 0) { is_key = false; EMIT_UNTIL('>', C_CYAN) }
  
  if (p[1] == '>' || (isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2]))) {
    ant_output_stream_append_cstr(out, JSON_BRACE); ant_output_stream_putc(out, *p++);
    ant_output_stream_append_cstr(out, JSON_WHITE);
    while (*p && *p != '>') ant_output_stream_putc(out, *p++);
    ant_output_stream_append_cstr(out, C_RESET);
    if (*p == '>') { ant_output_stream_append_cstr(out, JSON_BRACE); ant_output_stream_putc(out, *p++); ant_output_stream_append_cstr(out, C_RESET); }
    goto next;
  }
  
  ant_output_stream_append_cstr(out, JSON_BRACE); ant_output_stream_putc(out, *p++); ant_output_stream_append_cstr(out, C_RESET);
  goto next;

gt:
  ant_output_stream_append_cstr(out, JSON_BRACE); ant_output_stream_putc(out, *p++); ant_output_stream_append_cstr(out, C_RESET);
  goto next;

alpha:
  if (memcmp(p, "Object [", 8) == 0) { EMIT_UNTIL(']', JSON_TAG) }
  if (memcmp(p, "Symbol(", 7) == 0) { EMIT_UNTIL(')', JSON_STRING) }
  
  EMIT_TYPE("Map", 3, JSON_STRING)
  EMIT_TYPE("Set", 3, JSON_STRING)

  KEYWORD("true", JSON_BOOL)
  KEYWORD("false", JSON_BOOL)
  KEYWORD("null", JSON_NULL)
  KEYWORD("undefined", JSON_NULL)
  KEYWORD("Infinity", JSON_NUMBER)
  KEYWORD("NaN", JSON_NUMBER)

ident:
  if (is_key && brace_depth > 0) {
    ant_output_stream_append_cstr(out, JSON_KEY);
    while (isalnum((unsigned char)*p) || *p == '_' || *p == '$') ant_output_stream_putc(out, *p++);
    ant_output_stream_append_cstr(out, C_RESET);
    goto next;
  }
  ant_output_stream_putc(out, *p++); goto next;

other:
  if (*p == '+') {
    int iso_len = io_iso_utc_token_len(p);
    if (iso_len > 0) {
      ant_output_stream_append_cstr(out, C_MAGENTA);
      for (int k = 0; k < iso_len; k++) ant_output_stream_putc(out, *p++);
      ant_output_stream_append_cstr(out, C_RESET);
      goto next;
    }
  }
  ant_output_stream_putc(out, *p++); goto next;
}

#undef KEYWORD
#undef EMIT_UNTIL
#undef EMIT_TYPE

void print_value_colored(const char *str, FILE *stream) {
  ant_output_stream_t *out = ant_output_stream(stream);
  ant_output_stream_begin(out);
  print_value_colored_to_output(str, out);
  ant_output_stream_flush(out);
}

void print_repl_value(ant_t *js, ant_value_t val, FILE *stream) {
  ant_output_stream_t *out = ant_output_stream(stream);

  if (vtype(val) == T_STR) {
    char *str = js_getstr(js, val, NULL);
    ant_output_stream_begin(out);
    ant_output_stream_append_cstr(out, C(JSON_STRING));
    ant_output_stream_putc(out, '\'');
    ant_output_stream_append_cstr(out, str ? str : "");
    ant_output_stream_putc(out, '\'');
    ant_output_stream_append_cstr(out, C(C_RESET));
    ant_output_stream_putc(out, '\n');
    ant_output_stream_flush(out);
    return;
  }

  if (vtype(val) == T_OBJ && vtype(js_get_slot(val, SLOT_ERR_TYPE)) != T_UNDEF) {
  const char *stack = get_str_prop(js, val, "stack", 5, NULL);
  
  if (stack) {
    ant_output_stream_begin(out);
    io_print_to_output(stack, out);
    ant_output_stream_putc(out, '\n');
    ant_output_stream_flush(out);
    return;
  }}

  char cbuf[512];
  js_cstr_t cstr = js_to_cstr(js, val, cbuf, sizeof(cbuf));

  if (vtype(val) == T_ERR) {
    ant_output_stream_t *err_out = ant_output_stream(stderr);
    ant_output_stream_begin(err_out);
    ant_output_stream_append_cstr(err_out, cstr.ptr);
    ant_output_stream_putc(err_out, '\n');
    ant_output_stream_flush(err_out);
  } else {
    ant_output_stream_begin(out);
    print_value_colored_to_output(cstr.ptr, out);
    ant_output_stream_putc(out, '\n');
    ant_output_stream_flush(out);
  }

  if (cstr.needs_free) free((void *)cstr.ptr);
}

ant_value_t console_print(ant_t *js, ant_value_t *args, int nargs, const char *color, FILE *stream) {
  ant_output_stream_t *out = ant_output_stream(stream);
  ant_output_stream_begin(out);
  if (color && !io_no_color) ant_output_stream_append_cstr(out, color);
  
  for (int i = 0; i < nargs; i++) {
    if (i) ant_output_stream_putc(out, ' ');
    if (vtype(args[i]) == T_OBJ) {
      const char *stack = get_str_prop(js, args[i], "stack", 5, NULL);
      if (stack) { io_print_to_output(stack, out); continue; }
    }
    
    char cbuf[512];
    js_cstr_t cstr = js_to_cstr(js, args[i], cbuf, sizeof(cbuf));
    
    if (vtype(args[i]) == T_STR) io_print_to_output(cstr.ptr, out); else {
      if (color && !io_no_color) ant_output_stream_append_cstr(out, C_RESET);
      print_value_colored_to_output(cstr.ptr, out);
      if (color && !io_no_color) ant_output_stream_append_cstr(out, color);
    }
    
    if (cstr.needs_free) free((void *)cstr.ptr);
  }
  
  if (color && !io_no_color) ant_output_stream_append_cstr(out, C_RESET);
  ant_output_stream_putc(out, '\n');
  ant_output_stream_flush(out);
  
  return js_mkundef();
}

static void console_write_args_to_output(ant_t *js, ant_output_stream_t *out, ant_value_t *args, int nargs) {
for (int i = 0; i < nargs; i++) {
  if (i) ant_output_stream_putc(out, ' ');
  
  if (vtype(args[i]) == T_OBJ) {
  const char *stack = get_str_prop(js, args[i], "stack", 5, NULL);
  if (stack) {
    io_print_to_output(stack, out);
    continue;
  }}
  
  char cbuf[512];
  js_cstr_t cstr = js_to_cstr(js, args[i], cbuf, sizeof(cbuf));
  
  if (vtype(args[i]) == T_STR) io_print_to_output(cstr.ptr, out);
  else print_value_colored_to_output(cstr.ptr, out);
  
  if (cstr.needs_free) free((void *)cstr.ptr);
}}

static ant_value_t js_console_log(ant_t *js, ant_value_t *args, int nargs) {
  return console_print(js, args, nargs, NULL, stdout);
}

static ant_value_t js_console_error(ant_t *js, ant_value_t *args, int nargs) {
  return console_print(js, args, nargs, C_RED, stderr);
}

static ant_value_t js_console_warn(ant_t *js, ant_value_t *args, int nargs) {
  return console_print(js, args, nargs, C_YELLOW, stderr);
}

static ant_value_t js_console_assert(ant_t *js, ant_value_t *args, int nargs) {
  ant_output_stream_t *out = ant_output_stream(stderr);

  if (nargs < 1) return js_mkundef();
  
  bool is_truthy = js_truthy(js, args[0]);
  if (is_truthy) return js_mkundef();
  
  ant_output_stream_begin(out);
  ant_output_stream_append_cstr(out, "Assertion failed");
  
  if (nargs > 1) {
    ant_output_stream_append_cstr(out, ": ");
    console_write_args_to_output(js, out, args + 1, nargs - 1);
    ant_output_stream_putc(out, '\n');
    ant_output_stream_flush(out);
    return js_mkundef();
  }
  
  ant_output_stream_putc(out, '\n');
  ant_output_stream_flush(out);
  
  return js_mkundef();
}

static ant_value_t js_console_trace(ant_t *js, ant_value_t *args, int nargs) {
  ant_output_stream_t *out = ant_output_stream(stderr);

  ant_output_stream_begin(out);
  ant_output_stream_append_cstr(out, "Trace");
  
  if (nargs > 0) {
    ant_output_stream_append_cstr(out, ": ");
    console_write_args_to_output(js, out, args, nargs);
    ant_output_stream_putc(out, '\n');
  } else ant_output_stream_putc(out, '\n');
  
  ant_output_stream_flush(out);
  js_print_stack_trace_vm(js, stderr);
  
  return js_mkundef();
}

static ant_value_t js_console_info(ant_t *js, ant_value_t *args, int nargs) {
  return console_print(js, args, nargs, C_CYAN, stdout);
}

static ant_value_t js_console_debug(ant_t *js, ant_value_t *args, int nargs) {
  return console_print(js, args, nargs, C_MAGENTA, stdout);
}

static ant_value_t js_console_clear(ant_t *js, ant_value_t *args, int nargs) {
  if (!io_no_color) {
    ant_output_stream_t *out = ant_output_stream(stdout);
    ant_output_stream_begin(out);
    ant_output_stream_append_cstr(out, "\033[2J\033[H");
    ant_output_stream_flush(out);
  }
  return js_mkundef();
}

static struct { char *label; double start_time; } console_timers[64];
static int console_timer_count = 0;

static ant_value_t js_console_time(ant_t *js, ant_value_t *args, int nargs) {
  const char *label = "default";
  if (nargs > 0 && vtype(args[0]) == T_STR) {
    label = js_getstr(js, args[0], NULL);
  }
  
  for (int i = 0; i < console_timer_count; i++) {
  if (strcmp(console_timers[i].label, label) == 0) {
    ant_output_stream_t *out = ant_output_stream(stderr);
    ant_output_stream_begin(out);
    ant_output_stream_appendf(out, "Timer '%s' already exists\n", label);
    ant_output_stream_flush(out);
    return js_mkundef();
  }}
  
  if (console_timer_count < 64) {
    console_timers[console_timer_count].label = strdup(label);
    console_timers[console_timer_count].start_time = (double)uv_hrtime() / 1e6;
    console_timer_count++;
  }
  
  return js_mkundef();
}

static ant_value_t js_console_timeEnd(ant_t *js, ant_value_t *args, int nargs) {
  const char *label = "default";
  if (nargs > 0 && vtype(args[0]) == T_STR) {
    label = js_getstr(js, args[0], NULL);
  }
  
  for (int i = 0; i < console_timer_count; i++) {
  if (strcmp(console_timers[i].label, label) == 0) {
    double elapsed = ((double)uv_hrtime() / 1e6) - console_timers[i].start_time;
    ant_output_stream_t *out = ant_output_stream(stdout);
    
    ant_output_stream_begin(out);
    ant_output_stream_appendf(out, "%s: %.3fms\n", label, elapsed);
    ant_output_stream_flush(out);
    free(console_timers[i].label);
    
    for (int j = i; j < console_timer_count - 1; j++) {
      console_timers[j] = console_timers[j + 1];
    }
    
    console_timer_count--;
    return js_mkundef();
  }}
  
  {
    ant_output_stream_t *out = ant_output_stream(stderr);
    ant_output_stream_begin(out);
    ant_output_stream_appendf(out, "Timer '%s' does not exist\n", label);
    ant_output_stream_flush(out);
  }
  
  return js_mkundef();
}

static const char *get_slot_name(internal_slot_t slot) {
  #define ANT_SLOT_NAME(name) [name] = &#name[5],
  static const char *slot_names[] = {
    ANT_INTERNAL_SLOT_LIST(ANT_SLOT_NAME)
  };
  #undef ANT_SLOT_NAME

  if (slot < sizeof(slot_names) / sizeof(slot_names[0]) && slot_names[slot]) {
    return slot_names[slot];
  }
  
  return "UNKNOWN";
}

static const char *get_type_name(int type) {
  static const char *type_names[] = {
    [T_OBJ]        = "object",
    [T_STR]        = "string",
    [T_ARR]        = "array",
    [T_FUNC]       = "function",
    [T_CFUNC]      = "function",
    [T_CLOSURE]    = "closure",
    [T_PROMISE]    = "Promise",
    [T_GENERATOR]  = "Generator",
    [T_UNDEF]      = "undefined",
    [T_NULL]       = "null",
    [T_BOOL]       = "boolean",
    [T_NUM]        = "number",
    [T_BIGINT]     = "bigint",
    [T_SYMBOL]     = "symbol",
    [T_ERR]        = "error",
    [T_TYPEDARRAY] = "TypedArray",
    [T_FFI]        = "ffi",
    [T_NTARG]      = "ntarg",
    [T_MAP]        = "map",
    [T_SET]        = "set",
    [T_WEAKMAP]    = "weakmap",
    [T_WEAKSET]    = "weakset"
  };
  
  size_t num_types = sizeof(type_names) / sizeof(type_names[0]);
  if (type < 0 || (size_t)type >= num_types) return "unknown";
  
  return type_names[type] ? type_names[type] : "unknown";
}

static bool inspect_was_visited(inspect_visited_t *v, uintptr_t off) {
  for (int i = 0; i < v->count; i++) if (v->visited[i] == off) return true;
  return false;
}

static void inspect_print_indent(FILE *stream, int depth) {
  for (int i = 0; i < depth; i++) fprintf(stream, "  ");
}

static void inspect_mark_visited(inspect_visited_t *v, uintptr_t off) {
  if (v->count >= v->capacity) {
    v->capacity = v->capacity ? v->capacity * 2 : 32;
    v->visited = realloc(v->visited, v->capacity * sizeof(uintptr_t));
  }
  v->visited[v->count++] = off;
}

void inspect_value(ant_t *js, ant_value_t val, FILE *stream, int depth, inspect_visited_t *visited) {
  int t = vtype(val);
  
  if (t == T_UNDEF) { fprintf(stream, "undefined"); return; }
  if (t == T_NULL)  { fprintf(stream, "null"); return; }
  if (t == T_BOOL)  { fprintf(stream, val == js_true ? "true" : "false"); return; }
  if (t == T_NUM)   { fprintf(stream, "%g", js_getnum(val)); return; }
  if (t == T_ERR)   { fprintf(stream, "[Error]"); return; }
  
  if (t == T_STR) {
    size_t len;
    char *str = js_getstr(js, val, &len);
    fprintf(stream, "\"%.*s\"", (int)len, str ? str : "");
    return;
  }
  
  if (t == T_SYMBOL) {
    const char *desc = js_sym_desc(js, val);
    fprintf(stream, "Symbol(%s)", desc ? desc : "");
    return;
  }
  
  if (t == T_OBJ || t == T_FUNC || t == T_PROMISE || t == T_ARR) {
    if (depth > 10) fprintf(stream, "<%s @%" PRIu64 " ...>", get_type_name(t), (uint64_t)vdata(js_as_obj(val)));
    else inspect_object(js, val, stream, depth, visited);
    return;
  }
  
  if (t == T_CFUNC) {
    fprintf(stream, "<native function 0x%" PRIx64 ">", (uint64_t)vdata(val));
    return;
  }
  
  fprintf(stream, "<%s rawtype=%d data=%" PRIu64 ">", get_type_name(t), vtype(val), (uint64_t)vdata(val));
}

void inspect_object(ant_t *js, ant_value_t obj, FILE *stream, int depth, inspect_visited_t *visited) {
  int type = vtype(obj);
  obj = js_as_obj(obj);
  uintptr_t obj_off = (uintptr_t)vdata(obj);
  
  if (inspect_was_visited(visited, obj_off)) {
    fprintf(stream, "[Circular *%llu]", (u64)obj_off);
    return;
  }
  
  inspect_mark_visited(visited, obj_off);
  fprintf(stream, "<%s @%llu> {\n", type == T_FUNC ? "Function" : (type == T_PROMISE ? "Promise" : "Object"), (u64)obj_off);
  
  int inner_depth = depth + 1;
  
  inspect_print_indent(stream, inner_depth);
  fprintf(stream, "[[Slots]]: {\n");
  
  for (int slot = SLOT_NONE + 1; slot < SLOT_MAX; slot++) {
    ant_value_t slot_val = js_get_slot(obj, (internal_slot_t)slot);
    int t = vtype(slot_val);
    if (t == T_UNDEF) continue;
    
    inspect_print_indent(stream, inner_depth + 1);
    fprintf(stream, "[[%s]]: ", get_slot_name((internal_slot_t)slot));
    
    switch (slot) {
      case SLOT_CODE:
      case SLOT_CFUNC:
        fprintf(stream, "<native ptr 0x%" PRIx64 ">", (uint64_t)vdata(slot_val));
        break;
      case SLOT_CODE_LEN:
        fprintf(stream, "%.0f", js_getnum(slot_val));
        break;
      default:
        if ((t == T_OBJ || t == T_FUNC || t == T_PROMISE) && inspect_was_visited(visited, (uintptr_t)vdata(js_as_obj(slot_val))))
          fprintf(stream, "[Circular *%llu]", (u64)vdata(js_as_obj(slot_val)));
        else if (t == T_OBJ || t == T_FUNC || t == T_PROMISE)
          fprintf(stream, "<%s @%llu>", get_type_name(t), (u64)vdata(js_as_obj(slot_val)));
        else
          inspect_value(js, slot_val, stream, inner_depth + 1, visited);
        break;
    }
    
    fprintf(stream, "\n");
  }
  
  inspect_print_indent(stream, inner_depth);
  fprintf(stream, "}\n");
  
  inspect_print_indent(stream, inner_depth);
  fprintf(stream, "[[Properties]]: {\n");
  
  ant_iter_t iter = js_prop_iter_begin(js, obj);
  const char *key;
  size_t key_len;
  ant_value_t value;
  
  while (js_prop_iter_next(&iter, &key, &key_len, &value)) {
    inspect_print_indent(stream, inner_depth + 1);
    fprintf(stream, "\"%.*s\": ", (int)key_len, key);
    inspect_value(js, value, stream, inner_depth + 1, visited);
    fprintf(stream, "\n");
  }
  
  js_prop_iter_end(&iter);
  inspect_print_indent(stream, inner_depth);
  fprintf(stream, "}\n");
  
  ant_value_t proto = js_get_proto(js, obj);
  inspect_print_indent(stream, inner_depth);
  fprintf(stream, "[[Prototype]]: ");
  if (vtype(proto) == T_NULL) {
    fprintf(stream, "null\n");
  } else {
    fprintf(stream, "\n");
    inspect_print_indent(stream, inner_depth);
    inspect_value(js, proto, stream, inner_depth, visited);
    fprintf(stream, "\n");
  }
  
  inspect_print_indent(stream, depth);
  fprintf(stream, "}");
}

static ant_value_t js_console_inspect(ant_t *js, ant_value_t *args, int nargs) {
  FILE *stream = stdout;
  inspect_visited_t visited = {0};
  
  for (int i = 0; i < nargs; i++) {
    if (i > 0) fprintf(stream, " ");
    inspect_value(js, args[i], stream, 0, &visited);
  }
  
  fprintf(stream, "\n");
  if (visited.visited) free(visited.visited);
  
  return js_mkundef();
}

ant_value_t console_library(ant_t *js) {
  ant_value_t console_obj = js_mkobj(js);
  
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
  js_set(js, console_obj, "inspect", js_mkfun(js_console_inspect));
  
  js_set_sym(js, console_obj, get_toStringTag_sym(), js_mkstr(js, "console", 7));
  return console_obj;
}

void init_console_module() {
  ant_t *js = rt->js;
  ant_value_t console_obj = console_library(js);
  js_set(js, js_glob(js), "console", console_obj);
}
