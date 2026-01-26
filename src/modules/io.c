#include <compat.h> // IWYU pragma: keep

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <inttypes.h>
#include <uv.h>

#include "common.h"
#include "internal.h"
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
#define JSON_WHITE "\x1b[97m"

typedef struct {
  jsoff_t *visited;
  int count;
  int capacity;
} inspect_visited_t;

static void io_putc(int c, FILE *stream) { fputc(c, stream); }
static void io_puts(const char *s, FILE *stream) { fputs(s, stream); }
static void inspect_object_full(struct js *js, jsval_t obj, FILE *stream, int depth, inspect_visited_t *visited);

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
  
#define EMIT_TYPE(tag, len, color) \
  if (!(is_key && brace_depth > 0) && memcmp(p, tag, len) == 0) { \
    io_puts(color, stream); io_puts(tag, stream); io_puts(ANSI_RESET, stream); \
    p += len; goto next; \
  }

void print_value_colored(const char *str, FILE *stream) {
  if (io_no_color) { io_print(str, stream); return; }

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
  switch (p[1]) {
    case 'A': if (memcmp(p + 2, "syncFunction", 7) == 0) { EMIT_UNTIL(']', JSON_FUNC) } break;
    case 'b': if (memcmp(p + 2, "yte", 3) == 0 || memcmp(p + 2, "uffer]", 6) == 0) { EMIT_UNTIL(']', JSON_STRING) } break;
    case 'F': if (memcmp(p + 2, "unction", 7) == 0) { EMIT_UNTIL(']', JSON_FUNC) } break;
    case 'n': if (memcmp(p + 2, "ative code]", 11) == 0) { EMIT_UNTIL(']', JSON_FUNC) } break;
    case 'C': if (memcmp(p + 2, "ircular", 7) == 0) { EMIT_UNTIL(']', JSON_REF) } break;
    case 'G': if (memcmp(p + 2, "etter/Setter]", 13) == 0 || memcmp(p + 2, "etter]", 6) == 0) { EMIT_UNTIL(']', JSON_FUNC) } break;
    case 'S': if (memcmp(p + 2, "etter]", 6) == 0) { EMIT_UNTIL(']', JSON_FUNC) } break;
    case 'O': if (memcmp(p + 2, "bject: null prototype]", 22) == 0) { EMIT_UNTIL(']', JSON_TAG) } break;
    case 'M': if (memcmp(p + 2, "odule]", 6) == 0) { EMIT_UNTIL(']', JSON_TAG) } break;
    case 'U': if (memcmp(p + 2, "int8Contents]", 13) == 0) { EMIT_UNTIL(']', JSON_FUNC) } break;
    case 'P': if (memcmp(p + 2, "romise]", 7) == 0) { EMIT_UNTIL(']', JSON_TAG) } break;
  }
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
  if (memcmp(p, "<pen", 4) == 0) { is_key = false; EMIT_UNTIL('>', ANSI_CYAN) }
  if (memcmp(p, "<rej", 4) == 0) { is_key = false; EMIT_UNTIL('>', ANSI_CYAN) }
  
  if (p[1] == '>' || (isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2]))) {
    io_puts(JSON_BRACE, stream); io_putc(*p++, stream);
    io_puts(JSON_WHITE, stream);
    while (*p && *p != '>') io_putc(*p++, stream);
    io_puts(ANSI_RESET, stream);
    if (*p == '>') { io_puts(JSON_BRACE, stream); io_putc(*p++, stream); io_puts(ANSI_RESET, stream); }
    goto next;
  }
  
  io_puts(JSON_BRACE, stream); io_putc(*p++, stream); io_puts(ANSI_RESET, stream);
  goto next;

gt:
  io_puts(JSON_BRACE, stream); io_putc(*p++, stream); io_puts(ANSI_RESET, stream);
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

void console_print(struct js *js, jsval_t *args, int nargs, const char *color, FILE *stream) {
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
    console_timers[console_timer_count].start_time = (double)uv_hrtime() / 1e6;
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
      double elapsed = ((double)uv_hrtime() / 1e6) - console_timers[i].start_time;
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

static const char *get_slot_name(internal_slot_t slot) {
  static const char *slot_names[] = {
    [SLOT_NONE] = "NONE",
    [SLOT_PID] = "PID",
    [SLOT_ASYNC] = "ASYNC",
    [SLOT_WITH] = "WITH",
    [SLOT_SCOPE] = "SCOPE",
    [SLOT_THIS] = "THIS",
    [SLOT_BOUND_THIS] = "BOUND_THIS",
    [SLOT_BOUND_ARGS] = "BOUND_ARGS",
    [SLOT_FIELD_COUNT] = "FIELD_COUNT",
    [SLOT_SOURCE] = "SOURCE",
    [SLOT_FIELDS] = "FIELDS",
    [SLOT_STRICT] = "STRICT",
    [SLOT_CODE] = "CODE",
    [SLOT_CODE_LEN] = "CODE_LEN",
    [SLOT_CFUNC] = "CFUNC",
    [SLOT_CORO] = "CORO",
    [SLOT_ARROW] = "ARROW",
    [SLOT_PROTO] = "PROTO",
    [SLOT_FUNC_PROTO] = "FUNC_PROTO",
    [SLOT_ASYNC_PROTO] = "ASYNC_PROTO",
    [SLOT_FROZEN] = "FROZEN",
    [SLOT_SEALED] = "SEALED",
    [SLOT_EXTENSIBLE] = "EXTENSIBLE",
    [SLOT_BUFFER] = "BUFFER",
    [SLOT_TARGET_FUNC] = "TARGET_FUNC",
    [SLOT_NAME] = "NAME",
    [SLOT_MAP] = "MAP",
    [SLOT_SET] = "SET",
    [SLOT_PRIMITIVE] = "PRIMITIVE",
    [SLOT_PROXY_REF] = "PROXY_REF",
    [SLOT_BUILTIN] = "BUILTIN",
    [SLOT_DATA] = "DATA",
    [SLOT_CTOR] = "CTOR",
    [SLOT_SUPER] = "SUPER",
    [SLOT_DEFAULT_CTOR] = "DEFAULT_CTOR",
    [SLOT_DEFAULT] = "DEFAULT",
    [SLOT_ERR_TYPE] = "ERR_TYPE",
    [SLOT_OBSERVABLE_SUBSCRIBER] = "OBSERVABLE_SUBSCRIBER",
    [SLOT_SUBSCRIPTION_OBSERVER] = "SUBSCRIPTION_OBSERVER",
    [SLOT_SUBSCRIPTION_CLEANUP] = "SUBSCRIPTION_CLEANUP",
    [SLOT_HOISTED_VARS] = "HOISTED_VARS",
    [SLOT_HOISTED_VARS_LEN] = "HOISTED_VARS_LEN",
    [SLOT_STRICT_EVAL_SCOPE] = "STRICT_EVAL_SCOPE",
    [SLOT_MODULE_SCOPE] = "MODULE_SCOPE",
    [SLOT_STRICT_ARGS] = "STRICT_ARGS",
    [SLOT_NO_FUNC_DECLS] = "NO_FUNC_DECLS",
  };
  
  if (slot < sizeof(slot_names) / sizeof(slot_names[0]) && slot_names[slot]) {
    return slot_names[slot];
  }
  return "UNKNOWN";
}

static const char *get_type_name(int type) {
  static const char *type_names[] = {
    [T_OBJ]        = "object",
    [T_PROP]       = "property",
    [T_STR]        = "string",
    [T_UNDEF]      = "undefined",
    [T_NULL]       = "null",
    [T_NUM]        = "number",
    [T_BOOL]       = "boolean",
    [T_FUNC]       = "function",
    [T_CODEREF]    = "coderef",
    [T_CFUNC]      = "function",
    [T_ERR]        = "error",
    [T_ARR]        = "array",
    [T_PROMISE]    = "Promise",
    [T_TYPEDARRAY] = "TypedArray",
    [T_BIGINT]     = "bigint",
    [T_PROPREF]    = "propref",
    [T_SYMBOL]     = "symbol",
    [T_GENERATOR]  = "Generator",
    [T_FFI]        = "ffi"
  };
  
  size_t num_types = sizeof(type_names) / sizeof(type_names[0]);
  if (type < 0 || (size_t)type >= num_types) return "unknown";
  
  return type_names[type] ? type_names[type] : "unknown";
}

static bool inspect_was_visited(inspect_visited_t *v, jsoff_t off) {
  for (int i = 0; i < v->count; i++) if (v->visited[i] == off) return true;
  return false;
}

static void inspect_print_indent(FILE *stream, int depth) {
  for (int i = 0; i < depth; i++) fprintf(stream, "  ");
}

static void inspect_mark_visited(inspect_visited_t *v, jsoff_t off) {
  if (v->count >= v->capacity) {
    v->capacity = v->capacity ? v->capacity * 2 : 32;
    v->visited = realloc(v->visited, v->capacity * sizeof(jsoff_t));
  }
  v->visited[v->count++] = off;
}

static void inspect_value(struct js *js, jsval_t val, FILE *stream, int depth, inspect_visited_t *visited) {
  int t = js_type(val);
  
  if (t == T_UNDEF) { fprintf(stream, "undefined"); return; }
  if (t == T_NULL)  { fprintf(stream, "null"); return; }
  if (t == T_BOOL)  { fprintf(stream, js_getbool(val) ? "true" : "false"); return; }
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
    if (depth > 10) fprintf(stream, "<%s @%" PRIu64 " ...>", get_type_name(t), (uint64_t)vdata(val));
    else inspect_object_full(js, val, stream, depth, visited);
    return;
  }
  
  fprintf(stream, "<%s rawtype=%d data=%" PRIu64 ">", get_type_name(t), vtype(val), (uint64_t)vdata(val));
}

static void inspect_object_full(struct js *js, jsval_t obj, FILE *stream, int depth, inspect_visited_t *visited) {
  int type = js_type(obj);
  jsoff_t obj_off = (jsoff_t)vdata(obj);
  
  if (inspect_was_visited(visited, obj_off)) {
    fprintf(stream, "[Circular *%u]", obj_off);
    return;
  }
  
  inspect_mark_visited(visited, obj_off);
  fprintf(stream, "<%s @%u> {\n", type == JS_FUNC ? "Function" : (type == JS_PROMISE ? "Promise" : "Object"), obj_off);
  
  int inner_depth = depth + 1;
  
  inspect_print_indent(stream, inner_depth);
  fprintf(stream, "[[Slots]]: {\n");
  
  for (int slot = SLOT_NONE + 1; slot < SLOT_MAX; slot++) {
    jsval_t slot_val = js_get_slot(js, obj, (internal_slot_t)slot);
    int t = js_type(slot_val);
    if (t == T_UNDEF) continue;
    
    inspect_print_indent(stream, inner_depth + 1);
    fprintf(stream, "[[%s]]: ", get_slot_name((internal_slot_t)slot));
    
    switch (slot) {
      case SLOT_CODE:
      case SLOT_CFUNC:
      case SLOT_HOISTED_VARS:
        fprintf(stream, "<native ptr 0x%" PRIx64 ">", (uint64_t)vdata(slot_val));
        break;
      case SLOT_CODE_LEN:
      case SLOT_HOISTED_VARS_LEN:
        fprintf(stream, "%.0f", js_getnum(slot_val));
        break;
      default:
        if ((t == T_OBJ || t == T_FUNC || t == T_PROMISE) && inspect_was_visited(visited, (jsoff_t)vdata(slot_val)))
          fprintf(stream, "[Circular *%u]", (jsoff_t)vdata(slot_val));
        else if (t == T_OBJ || t == T_FUNC || t == T_PROMISE)
          fprintf(stream, "<%s @%u>", get_type_name(t), (jsoff_t)vdata(slot_val));
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
  jsval_t value;
  
  while (js_prop_iter_next(&iter, &key, &key_len, &value)) {
    inspect_print_indent(stream, inner_depth + 1);
    fprintf(stream, "\"%.*s\": ", (int)key_len, key);
    inspect_value(js, value, stream, inner_depth + 1, visited);
    fprintf(stream, "\n");
  }
  
  js_prop_iter_end(&iter);
  inspect_print_indent(stream, inner_depth);
  fprintf(stream, "}\n");
  
  jsval_t proto = js_get_proto(js, obj);
  inspect_print_indent(stream, inner_depth);
  fprintf(stream, "[[Prototype]]: ");
  if (js_type(proto) == JS_NULL) {
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

static jsval_t js_console_inspect(struct js *js, jsval_t *args, int nargs) {
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
  js_set(js, console_obj, "inspect", js_mkfun(js_console_inspect));
  
  js_set(js, console_obj, get_toStringTag_sym_key(), js_mkstr(js, "console", 7));
  js_set(js, js_glob(js), "console", console_obj);
}