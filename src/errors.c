#include "tokens.h"
#include "stack.h"
#include "errors.h"
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static const char *get_error_type_name(js_err_type_t err_type) {
  static const char *names[] = {
    [JS_ERR_GENERIC]   = "Error",
    [JS_ERR_TYPE]      = "TypeError",
    [JS_ERR_SYNTAX]    = "SyntaxError",
    [JS_ERR_REFERENCE] = "ReferenceError",
    [JS_ERR_RANGE]     = "RangeError",
    [JS_ERR_EVAL]      = "EvalError",
    [JS_ERR_URI]       = "URIError",
    [JS_ERR_INTERNAL]  = "InternalError",
    [JS_ERR_AGGREGATE] = "AggregateError",
  };
  
  return names[err_type] ?: "Error";
}

static bool ensure_errmsg_capacity(struct js *js, size_t needed) {
  if (js->errmsg_size == 0) js->errmsg_size = 4096;

  if (!js->errmsg) {
    js->errmsg = (char *)malloc(js->errmsg_size);
    if (!js->errmsg) return false;
    js->errmsg[0] = '\0';
  }

  if (needed <= js->errmsg_size) return true;

  size_t new_size = js->errmsg_size;
  while (new_size < needed) {
    size_t next = new_size * 2;
    if (next < new_size) return false;
    new_size = next;
  }

  char *next_buf = (char *)realloc(js->errmsg, new_size);
  if (!next_buf) return false;
  js->errmsg = next_buf;
  js->errmsg_size = new_size;
  return true;
}

__attribute__((format(printf, 3, 4)))
static size_t append_errmsg_fmt(struct js *js, size_t used, const char *fmt, ...) {
  int max_attempts = 3;
  int attempt = 0;

  for (;;) {
    if (!ensure_errmsg_capacity(js, used + 1)) {
      return js->errmsg_size ? js->errmsg_size - 1 : used;
    }

    size_t remaining = js->errmsg_size - used;
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(js->errmsg + used, remaining, fmt, ap);
    va_end(ap);

    if (written < 0) return used;
    if ((size_t)written < remaining) return used + (size_t)written;

    if (!ensure_errmsg_capacity(js, used + (size_t)written + 1)) {
      if (++attempt >= max_attempts) return js->errmsg_size ? js->errmsg_size - 1 : used;
    }
  }
}

static inline size_t remaining_capacity(size_t used, size_t total) {
  return used >= total ? 0 : total - used;
}

static size_t append_error_header(struct js *js, size_t used, int line) {
  if (js->filename) return append_errmsg_fmt(js, used, "%s:%d\n", js->filename, line);
  return append_errmsg_fmt(js, used, "<eval>:%d\n", line);
}

static void get_line_col(const char *code, jsoff_t pos, int *line, int *col) {
  int l = 1, c = 1;
  for (jsoff_t i = 0; i < pos && code[i]; i++) code[i] == '\n' ? (l++, c = 1) : c++;
  *line = l; *col = c;
}

static void get_error_line(const char *code, jsoff_t clen, jsoff_t pos, char *buf, size_t bufsize, int *line_start_col) {
  if (!code || bufsize == 0) {
    if (bufsize > 0) buf[0] = '\0';
    if (line_start_col) *line_start_col = 1;
    return;
  }

  if (pos > clen) pos = clen;

  if (clen == 0) {
    buf[0] = '\0';
    if (line_start_col) *line_start_col = 1;
    return;
  }

  jsoff_t line_start = pos;
  while (line_start > 0 && code[line_start - 1] != '\n') {
    line_start--;
  }
  
  jsoff_t line_end = pos;
  while (line_end < clen && code[line_end] != '\n' && code[line_end] != '\0') {
    line_end++;
  }
  
  jsoff_t line_len = line_end - line_start;
  if (line_len >= bufsize) line_len = (jsoff_t)(bufsize - 1);
  
  memcpy(buf, &code[line_start], line_len);
  buf[line_len] = '\0';
  *line_start_col = (int)(pos - line_start) + 1;
}

static size_t append_error_value(struct js *js, size_t used, jsval_t value) {
  const char *name = "Error";
  const char *msg = NULL;
  
  jsoff_t name_len = 5;
  jsoff_t msg_len = 0;

  static const void *type_dispatch[] = {
    [T_STR] = &&l_type_str,
    [T_OBJ] = &&l_type_obj,
    [T_FUNC] = &&l_type_default,
    [T_ARR] = &&l_type_default,
    [T_PROMISE] = &&l_type_default,
    [T_GENERATOR] = &&l_type_default,
    [T_PROP] = &&l_type_default,
    [T_BIGINT] = &&l_type_default,
    [T_NUM] = &&l_type_default,
    [T_BOOL] = &&l_type_default,
    [T_SYMBOL] = &&l_type_default,
    [T_CFUNC] = &&l_type_default,
    [T_FFI] = &&l_type_default,
    [T_TYPEDARRAY] = &&l_type_default,
    [T_CODEREF] = &&l_type_default,
    [T_PROPREF] = &&l_type_default,
    [T_ERR] = &&l_type_default,
    [T_UNDEF] = &&l_type_default,
    [T_NULL] = &&l_type_default,
  };

  uint8_t t = vtype(value);
  if (t < sizeof(type_dispatch) / sizeof(type_dispatch[0]) && type_dispatch[t]) {
    goto *type_dispatch[t];
  }
  goto l_type_default;

  l_type_str:
    msg = (const char *)&js->mem[vstr(js, value, &msg_len)];
    goto l_type_done;

  l_type_obj:
    name = get_str_prop(js, value, "name", 4, &name_len);
    if (!name) {
      name = "Error";
      name_len = 5;
    }
    msg = get_str_prop(js, value, "message", 7, &msg_len);
    goto l_type_done;

  l_type_default:
    msg = js_str(js, value);
    msg_len = msg ? (jsoff_t)strlen(msg) : 0;
    goto l_type_done;

  l_type_done:

  static const void *dispatch[] = { &&l_with_msg, &&l_name_only };
  int key = msg ? 0 : 1;
  goto *dispatch[key];

  l_with_msg:
    return append_errmsg_fmt(js, used,
      ERR_FMT,
      (int)name_len, name, (int)msg_len, msg
    );

  l_name_only:
    return append_errmsg_fmt(js, used,
      ERR_NAME_ONLY,
      (int)name_len, name
    );
}

static void append_error_caret(struct js *js, size_t *n, int error_col) {
  if (!ensure_errmsg_capacity(js, *n + (size_t)error_col + 2)) return;
  if (*n >= js->errmsg_size - 1) return;

  size_t remaining = js->errmsg_size - *n;
  for (int i = 1; i < error_col && remaining > 1; i++) {
    js->errmsg[(*n)++] = ' ';
    remaining--;
  }
  if (remaining > 1) {
    js->errmsg[(*n)++] = '^';
  }
  js->errmsg[*n] = '\0';
}

static void format_error_stack(struct js *js, size_t *n, int line, int col, bool include_source_line, const char *error_line, int error_col) {
  if (!ensure_errmsg_capacity(js, *n + 1)) return;
  
  const char *dim = "\x1b[90m";
  const char *reset = "\x1b[0m";
  
  if (include_source_line && error_line && *n < js->errmsg_size) {
    *n = append_errmsg_fmt(js, *n, "\n%s\n", error_line);
    append_error_caret(js, n, error_col);
  }
  
  size_t remaining = remaining_capacity(*n, js->errmsg_size);
  if (remaining > 20) {
    const char *file = js->filename ? js->filename : "<eval>";
    
    for (int i = global_call_stack.depth - 1; i >= 0 && remaining > 20; i--) {
      call_frame_t *frame = &global_call_stack.frames[i];
      const char *fname = frame->function_name ? frame->function_name : "<anonymous>";
      const char *ffile = frame->filename ? frame->filename : "<eval>";
      
      if (frame->line < 0 && frame->code) {
        get_line_col(frame->code, frame->pos, &frame->line, &frame->col);
      }
      int fline = frame->line > 0 ? frame->line : 1;
      int fcol = frame->col > 0 ? frame->col : 1;
      
      *n = append_errmsg_fmt(js, *n,
        "\n    at %s %s(%s:%d:%d)%s",
        fname, dim, ffile, fline, fcol, reset
      );
      remaining = remaining_capacity(*n, js->errmsg_size);
    }
    
    if (global_call_stack.depth > 0 && remaining > 60) {
      *n = append_errmsg_fmt(js, *n,
        "\n    at Object.<anonymous> %s(%s:1:1)%s",
        dim, file, reset
      );
      remaining = remaining_capacity(*n, js->errmsg_size);
    }
    
    if (global_call_stack.depth == 0 && remaining > 20) {
      *n = append_errmsg_fmt(js, *n,
        "\n    at %s%s:%d:%d%s",
        dim, file, line, col, reset
      );
      remaining = remaining_capacity(*n, js->errmsg_size);
    }
    
    if (remaining > 60 && js->filename && strcmp(js->filename, "[eval]") != 0) {
      *n = append_errmsg_fmt(js, *n,
        "\n    at Module.executeUserEntryPoint [as runMain] %s(ant:internal/modules/run_main:149:5)%s",
        dim, reset
      );
      remaining = remaining_capacity(*n, js->errmsg_size);
    }
    
    if (remaining > 40 && js->filename && strcmp(js->filename, "[eval]") != 0) {
      *n = append_errmsg_fmt(js, *n,
        "\n    at %sant:internal/call:21728:23%s",
        dim, reset
      );
    }
  }
  
  js->errmsg[js->errmsg_size - 1] = '\0';
}

js_err_type_t get_error_type(struct js *js) {
  if (!(js->flags & F_THROW)) return JS_ERR_GENERIC;
  jsval_t err_type = js_get_slot(js, js->thrown_value, SLOT_ERR_TYPE);
  if (vtype(err_type) != T_NUM) return JS_ERR_GENERIC;
  return (js_err_type_t)(int)js_getnum(err_type);
}

__attribute__((format(printf, 4, 5)))
jsval_t js_create_error(struct js *js, js_err_type_t err_type, jsval_t props, const char *xx, ...) {
  va_list ap;
  int line = 0, col = 0;
  char error_line[256] = {0};
  int error_col = 0;
  char error_msg[256] = {0};
  
  bool no_stack = (err_type & JS_ERR_NO_STACK) != 0;
  err_type = (js_err_type_t)(err_type & ~JS_ERR_NO_STACK);
  
  if (!js->errmsg) {
    js->errmsg_size = 4096;
    js->errmsg = (char *)malloc(js->errmsg_size);
    if (!js->errmsg) return mkval(T_ERR, 0);
  }
  
  get_line_col(js->code, js->toff > 0 ? js->toff : js->pos, &line, &col);
  get_error_line(js->code, js->clen, js->toff > 0 ? js->toff : js->pos, error_line, sizeof(error_line), &error_col);
  
  va_start(ap, xx);
  vsnprintf(error_msg, sizeof(error_msg), xx, ap);
  va_end(ap);
  
  const char *err_name = get_error_type_name(err_type);
  size_t err_name_len = strlen(err_name);
  size_t msg_len = strlen(error_msg);
  
  jsval_t err_obj = js_mkobj(js);
  js_set(js, err_obj, "name", js_mkstr(js, err_name, err_name_len));
  js_set(js, err_obj, "message", js_mkstr(js, error_msg, msg_len));
  js_set_slot(js, err_obj, SLOT_ERR_TYPE, js_mknum((double)err_type));
  
  int props_type = vtype(props);
  if ((TYPE_FLAG(props_type) & T_SPECIAL_OBJECT_MASK) != 0) {
    js_merge_obj(js, err_obj, props);
  }
  jsval_t proto = js_get_ctor_proto(js, err_name, err_name_len);
  int proto_type = vtype(proto);
  if ((TYPE_FLAG(proto_type) & T_SPECIAL_OBJECT_MASK) != 0) {
    js_set_proto(js, err_obj, proto);
  }
  
  js->flags |= F_THROW;
  js->thrown_value = err_obj;
  
  size_t n = 0;
  n = append_error_header(js, 0, line);
  
  if (n < js->errmsg_size - 1) n = append_errmsg_fmt(
    js, n,
    "\x1b[31m%s\x1b[0m: \x1b[1m%s\x1b[0m",
    err_name, error_msg
  );
  
  if (!no_stack) {
    format_error_stack(js, &n, line, col, true, error_line, error_col);
  }
  
  js->pos = js->clen, js->tok = TOK_EOF, js->consumed = 0;
  return mkval(T_ERR, 0);
}

jsval_t js_throw(struct js *js, jsval_t value) {
  int line = 0, col = 0;
  char error_line[256] = {0};
  int error_col = 0;
  
  get_line_col(js->code, js->toff > 0 ? js->toff : js->pos, &line, &col);
  get_error_line(js->code, js->clen, js->toff > 0 ? js->toff : js->pos, error_line, sizeof(error_line), &error_col);
  
  if (!js->errmsg) {
    js->errmsg_size = 4096;
    js->errmsg = (char *)malloc(js->errmsg_size);
    if (!js->errmsg) return mkval(T_ERR, 0);
  }
  
  size_t n = 0;
  
  n = append_error_header(js, 0, line);
  n = append_error_value(js, n, value);
  
  format_error_stack(js, &n, line, col, true, error_line, error_col);
  
  js->flags |= F_THROW;
  js->thrown_value = value;
  js->pos = js->clen;
  js->tok = TOK_EOF;
  js->consumed = 0;
  return mkval(T_ERR, 0);
}

void js_print_stack_trace(FILE *stream) {
  int i = global_call_stack.depth;

loop:
  if (--i < 0) goto done;
  call_frame_t *frame = &global_call_stack.frames[i];
  
  if (frame->line >= 0 || !frame->code) goto print;
  get_line_col(frame->code, frame->pos, &frame->line, &frame->col);

print:
  fprintf(stream, "  at %s (\x1b[90m%s:%d:%d\x1b[0m)\n",
    frame->function_name ?: "<anonymous>",
    frame->filename ?: "<unknown>",
    frame->line > 0 ? frame->line : 1,
    frame->col > 0 ? frame->col : 1);
  goto loop;

done:
  return;
}
