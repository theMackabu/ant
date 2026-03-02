#include "errors.h"
#include "internal.h"
#include "silver/engine.h"
#include "modules/io.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/ioctl.h>
#endif

typedef struct { char *buf; size_t size; } errbuf_t;

static void print_error_value(ant_t *js, jsval_t value, jsval_t fallback_stack, const char *prefix) {
  const char *stack = NULL;
  
  if (vtype(value) == T_OBJ)
    stack = get_str_prop(js, value, "stack", 5, NULL);
  
  if (!stack && vtype(fallback_stack) == T_STR) {
    jsoff_t slen;
    jsoff_t soff = vstr(js, fallback_stack, &slen);
    stack = (const char *)&js->mem[soff];
  }
  
  if (prefix) fputs(prefix, stderr);
  
  if (stack) {
    fputs(stack, stderr);
    size_t n = strlen(stack);
    if (n == 0 || stack[n - 1] != '\n') fputc('\n', stderr);
  } else if (vtype(value) == T_OBJ) {
    const char *name = get_str_prop(js, value, "name", 4, NULL);
    const char *msg = get_str_prop(js, value, "message", 7, NULL);
    if (name && msg) fprintf(stderr, "%s%s%s: %s%s%s\n", C_RED, name, C_RESET, C_BOLD, msg, C_RESET);
    else if (name) fprintf(stderr, "%s%s%s\n", C_RED, name, C_RESET);
    else fprintf(stderr, "%s\n", js_str(js, value));
  } else fprintf(stderr, "%s\n", js_str(js, value));
}

bool print_uncaught_throw(ant_t *js) {
  if (!js->thrown_exists) return false;
  print_error_value(js, js->thrown_value, js->thrown_stack, NULL);
  
  js->thrown_exists = false;
  js->thrown_value = js_mkundef();
  js->thrown_stack = js_mkundef();
  
  return true;
}

bool print_unhandled_promise_rejection(ant_t *js, jsval_t value) {
  print_error_value(js, value, js_mkundef(), "Uncaught (in promise) ");
  return true;
}

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

static bool ensure_errbuf_capacity(errbuf_t *eb, size_t needed) {
  if (needed <= eb->size) return true;

  size_t new_size = eb->size;
  while (new_size < needed) {
    size_t next = new_size * 2;
    if (next < new_size) return false;
    new_size = next;
  }

  char *next_buf = (char *)realloc(eb->buf, new_size);
  if (!next_buf) return false;
  eb->buf = next_buf;
  eb->size = new_size;
  return true;
}

__attribute__((format(printf, 3, 4)))
static size_t append_errbuf_fmt(errbuf_t *eb, size_t used, const char *fmt, ...) {
  int max_attempts = 3;
  int attempt = 0;

  for (;;) {
    if (!ensure_errbuf_capacity(eb, used + 1)) {
      return eb->size ? eb->size - 1 : used;
    }

    size_t remaining = eb->size - used;
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(eb->buf + used, remaining, fmt, ap);
    va_end(ap);

    if (written < 0) return used;
    if ((size_t)written < remaining) return used + (size_t)written;

    if (!ensure_errbuf_capacity(eb, used + (size_t)written + 1)) {
      if (++attempt >= max_attempts) return eb->size ? eb->size - 1 : used;
    }
  }
}

static inline size_t remaining_capacity(size_t used, size_t total) {
  return used >= total ? 0 : total - used;
}

static size_t append_error_header(errbuf_t *eb, ant_t *js, size_t used, int line, int col) {
  const char *file = (js->errsite.valid && js->errsite.filename) ? js->errsite.filename : js->filename;
  if (file) return append_errbuf_fmt(eb, used, "%s:%d:%d\n", file, line, col);
  return append_errbuf_fmt(eb, used, "<eval>:%d:%d\n", line, col);
}

static void get_line_col(const char *code, jsoff_t code_len, jsoff_t pos, int *out_line, int *out_col) {
  if (!out_line || !out_col) return;

  *out_line = 1;
  *out_col = 1;

  if (!code || code_len <= 0 || pos <= 0) return;
  if (pos > code_len) pos = code_len;

  for (jsoff_t i = 0; i < pos; i++) {
    char ch = code[i];
    if (ch == '\0') break;
    if (ch == '\n') {
      *out_line += 1;
      *out_col = 1;
    } else *out_col += 1;
  }
}

static void get_error_line(
  const char *code, jsoff_t clen, jsoff_t pos, char *buf, size_t bufsize,
  int *line_start_col, jsoff_t *out_line_start, jsoff_t *out_line_end
) {
  if (!code || bufsize == 0) {
    if (bufsize > 0) buf[0] = '\0';
    if (line_start_col) *line_start_col = 1;
    if (out_line_start) *out_line_start = 0;
    if (out_line_end) *out_line_end = 0;
    return;
  }

  if (pos > clen) pos = clen;

  if (clen == 0) {
    buf[0] = '\0';
    if (line_start_col) *line_start_col = 1;
    if (out_line_start) *out_line_start = 0;
    if (out_line_end) *out_line_end = 0;
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
  
  if (line_start_col) *line_start_col = (int)(pos - line_start) + 1;
  if (out_line_start) *out_line_start = line_start;
  if (out_line_end) *out_line_end = line_end;
}

static size_t append_error_value(errbuf_t *eb, ant_t *js, size_t used, jsval_t value) {
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
    return append_errbuf_fmt(eb, used,
      ERR_FMT,
      (int)name_len, name, (int)msg_len, msg
    );

  l_name_only:
    return append_errbuf_fmt(eb, used,
      ERR_NAME_ONLY,
      (int)name_len, name
    );
}

static int count_digits_int(int v) {
  int n = 1;
  while (v >= 10) { v /= 10; n++; }
  return n;
}

static void append_error_caret(errbuf_t *eb, size_t *n, int error_col, int span_cols) {
  if (span_cols < 1) span_cols = 1;
  if (!ensure_errbuf_capacity(eb, *n + (size_t)error_col + (size_t)span_cols + 2)) return;
  if (*n >= eb->size - 1) return;

  size_t remaining = eb->size - *n;
  for (int i = 1; i < error_col && remaining > 1; i++) {
    eb->buf[(*n)++] = ' ';
    remaining--;
  }
  
  for (int i = 0; i < span_cols && remaining > 1; i++) {
    eb->buf[(*n)++] = '^';
    remaining--;
  }
  
  eb->buf[*n] = '\0';
}

static int error_span_cols_for_line(jsoff_t src_pos, jsoff_t span_len, jsoff_t line_start, jsoff_t line_end) {
  if (line_end < line_start) return 1;
  if (src_pos < line_start) src_pos = line_start;
  if (src_pos > line_end) src_pos = line_end;
  if (span_len <= 0) return 1;

  jsoff_t span_end = src_pos + span_len;
  if (span_end < src_pos) span_end = src_pos;
  if (span_end > line_end) span_end = line_end;

  jsoff_t width = span_end - src_pos;
  if (width <= 0) width = 1;
  if (width > INT_MAX) width = INT_MAX;
  return (int)width;
}

static int error_terminal_columns(void) {
#ifdef _WIN32
  HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
  if (h == NULL || h == INVALID_HANDLE_VALUE) h = GetStdHandle(STD_OUTPUT_HANDLE);
  if (h && h != INVALID_HANDLE_VALUE) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(h, &csbi)) {
      int cols = (int)(csbi.srWindow.Right - csbi.srWindow.Left + 1);
      if (cols > 0) return cols;
    }
  }
#else
  struct winsize ws;
  if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
    return (int)ws.ws_col;
  }
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
    return (int)ws.ws_col;
  }
#endif

  const char *cols_env = getenv("COLUMNS");
  if (cols_env && *cols_env) {
    char *end = NULL;
    long cols = strtol(cols_env, &end, 10);
    if (end != cols_env && cols > 0 && cols <= INT_MAX) return (int)cols;
  }
  return 120;
}

static int error_context_src_cols_limit(int gutter_w) {
  int cols = error_terminal_columns();
  int half = cols / 2;
  if (half < 40) half = 40;
  int budget = half - (gutter_w + 3);
  if (budget < 20) budget = 20;
  return budget;
}

static const char *error_frame_name(ant_t *js, sv_frame_t *frame, sv_func_t *func) {
  if (func && func->name && func->name[0]) return func->name;
  if (js && frame && vtype(frame->callee) == T_FUNC) {
    jsoff_t name_len = 0;
    const char *name = get_str_prop(js, js_func_obj(frame->callee), "name", 4, &name_len);
    if (name && name_len > 0) return name;
  }
  return "<anonymous>";
}

typedef struct {
  const char *name;
  const char *file;
  int line;
  int col;
  int index;
  int depth;
} js_vm_frame_view_t;

typedef bool 
  (*js_vm_frame_visitor_fn)
  (ant_t *js, const js_vm_frame_view_t *view, void *ctx);

static bool error_skip_bottom_wrapper_frame(const js_vm_frame_view_t *view) {
  return 
    view &&
    view->index == 0 &&
    view->depth > 0 &&
    strcmp(view->name, "<anonymous>") == 0 &&
    view->line == 1 &&
    view->col == 1;
}

static bool error_fill_vm_frame_view(
  ant_t *js, sv_vm_t *vm, int depth, int i, 
  const char *fallback_file, js_vm_frame_view_t *out
) {
  if (!js || !vm || i < 0 || i > depth || !out) return false;

  sv_frame_t *frame = &vm->frames[i];
  sv_func_t *func = frame->func;

  out->name = error_frame_name(js, frame, func);
  out->file = (func && func->filename) ? func->filename : fallback_file;
  out->line = (func && func->source_line > 0) ? func->source_line : 1;
  out->col = 1;
  out->index = i;
  out->depth = depth;

  if (func && func->srcpos && frame->ip) {
    uint32_t l, c;
    if (sv_lookup_srcpos(func, (int)(frame->ip - func->code), &l, &c)) {
      out->line = (int)l; out->col = (int)c;
    }
  }

  return true;
}

static void error_visit_vm_stack_frames(
  ant_t *js, const char *fallback_file, js_vm_frame_visitor_fn visitor, void *ctx
) {
  if (!js || !visitor) return;
  sv_vm_t *vm = js->vm;
  if (!vm) return;

  int depth = vm->fp;
  for (int i = depth; i >= 0; i--) {
    js_vm_frame_view_t view;
    if (!error_fill_vm_frame_view(js, vm, depth, i, fallback_file, &view)) continue;
    if (error_skip_bottom_wrapper_frame(&view)) continue;
    if (!visitor(js, &view, ctx)) break;
  }
}

typedef struct {
  errbuf_t *eb;
  size_t *n;
  size_t remaining;
  const char *dim;
  const char *reset;
} error_frame_errbuf_ctx_t;

static bool error_visit_frame_append_errbuf(ant_t *js, const js_vm_frame_view_t *view, void *ctx) {
  (void)js;
  error_frame_errbuf_ctx_t *c = (error_frame_errbuf_ctx_t *)ctx;
  *c->n = append_errbuf_fmt(
    c->eb, *c->n,
    "\n    at %s %s(%s:%d:%d)%s",
    view->name, c->dim, view->file, view->line, view->col, c->reset
  );
  c->remaining = remaining_capacity(*c->n, c->eb->size);
  return c->remaining > 20;
}

static bool error_visit_frame_print_file(ant_t *js, const js_vm_frame_view_t *view, void *ctx) {
  FILE *out = (FILE *)ctx;
  fprintf(
    out, "  at %s (%s%s:%d:%d%s)\n",
    view->name, C_GRAY, view->file, view->line, view->col, C_RESET
  );
  return true;
}

static bool append_error_context(
  errbuf_t *eb, size_t *n,
  const char *src, jsoff_t src_len,
  jsoff_t src_pos,
  int error_line_no, int error_col, int error_span_cols
) {
  if (!src || src_len <= 0 || !n) return false;
  if (src_pos < 0) src_pos = 0;
  if (src_pos > src_len) src_pos = src_len;

  jsoff_t err_line_start = src_pos;
  while (err_line_start > 0 && src[err_line_start - 1] != '\n') err_line_start--;
  
  jsoff_t err_line_end = src_pos;
  while (err_line_end < src_len && src[err_line_end] != '\n' && src[err_line_end] != '\0') err_line_end++;

  jsoff_t ctx_start = err_line_start;
  int first_line_no = error_line_no;
  for (int i = 0; i < 5 && ctx_start > 0; i++) {
    jsoff_t prev = ctx_start - 1;
    if (src[prev] == '\n') prev--;
    while (prev >= 0 && src[prev] != '\n') prev--;
    
    ctx_start = prev + 1;
    first_line_no--;
    if (first_line_no < 1) { first_line_no = 1; break; }
  }

  int gutter_w = count_digits_int(error_line_no);
  int src_cols_limit = error_context_src_cols_limit(gutter_w);
  int line_no = first_line_no;
  jsoff_t cur = ctx_start;
  
  while (cur <= err_line_end && cur < src_len) {
    jsoff_t line_start = cur;
    jsoff_t line_end = cur;
    
    while (line_end < src_len && src[line_end] != '\n' && src[line_end] != '\0') line_end++;
    int line_len = (int)(line_end - line_start);
    bool clipped = (src_cols_limit > 0 && line_len > src_cols_limit);
    int shown_len = clipped ? src_cols_limit : line_len;
    
    *n = append_errbuf_fmt(eb, *n, "\n%*d | %.*s", gutter_w, line_no, shown_len, src + line_start);
    if (clipped) *n = append_errbuf_fmt(eb, *n, "...");

    if (line_start == err_line_start) {
      *n = append_errbuf_fmt(eb, *n, "\n%*s   ", gutter_w, "");
      int caret_col = error_col;
      int caret_span = error_span_cols;
      
      if (clipped) {
        int max_col = shown_len > 0 ? shown_len : 1;
        if (caret_col > max_col) caret_col = max_col;
        if (caret_span > max_col - caret_col + 1) caret_span = max_col - caret_col + 1;
        if (caret_span < 1) caret_span = 1;
      } append_error_caret(eb, n, caret_col, caret_span);
    }

    if (line_end >= src_len || src[line_end] == '\0') break;
    cur = line_end + 1; line_no++;
  }

  return true;
}

static void format_error_stack(errbuf_t *eb, ant_t *js, size_t *n, int line, int col, bool include_source_line, const char *error_line, int error_col, int error_span_cols) {
  if (!ensure_errbuf_capacity(eb, *n + 1)) return;

  const char *dim = C_GRAY;
  const char *reset = C_RESET;

  if (include_source_line && error_line && error_line[0] && *n < eb->size) {
    *n = append_errbuf_fmt(eb, *n, "\n%s\n", error_line);
    append_error_caret(eb, n, error_col, error_span_cols);
  }

  size_t remaining = remaining_capacity(*n, eb->size);
  if (remaining > 20) {
    const char *file = (js->errsite.valid && js->errsite.filename)
      ? js->errsite.filename
      : (js->filename ? js->filename : "<eval>");
      
    sv_vm_t *vm = js->vm;
    int depth = vm ? vm->fp : -1;
    
    if (depth >= 0) {
      error_frame_errbuf_ctx_t ctx = { eb, n, remaining, dim, reset };
      error_visit_vm_stack_frames(js, file, error_visit_frame_append_errbuf, &ctx);
      remaining = ctx.remaining;
    }

    if (depth <= 0 && remaining > 20) {
      *n = append_errbuf_fmt(eb, *n,
        "\n    at %s%s:%d:%d%s",
        dim, file, line, col, reset
      );
      remaining = remaining_capacity(*n, eb->size);
    }

    if (remaining > 60 && js->filename && strcmp(js->filename, "[eval]") != 0) {
      *n = append_errbuf_fmt(eb, *n,
        "\n    at silver.sv_execute_frame %s(ant:internal/silver/engine:323:5)%s",
        dim, reset
      );
      remaining = remaining_capacity(*n, eb->size);
    }

    if (remaining > 40 && js->filename && strcmp(js->filename, "[eval]") != 0) {
      *n = append_errbuf_fmt(eb, *n, "\n    at %sant:internal/call:13635:14%s", dim, reset);
    }
  }

  eb->buf[eb->size - 1] = '\0';
}

void js_set_error_site(ant_t *js, const char *src, jsoff_t src_len, const char *filename, jsoff_t off, jsoff_t span_len) {
  if (!js) return;
  
  js->errsite.src = src;
  js->errsite.src_len = src_len;
  js->errsite.filename = filename;
  js->errsite.off = off < 0 ? 0 : off;
  js->errsite.span_len = span_len < 0 ? 0 : span_len;
  js->errsite.valid = (src != NULL && src_len >= 0);
}

void js_clear_error_site(ant_t *js) {
  if (!js) return;
  memset(&js->errsite, 0, sizeof(js->errsite));
}

static void resolve_error_site(
  ant_t *js, const char **out_src, jsoff_t *out_src_len,
  jsoff_t *out_src_pos, jsoff_t *out_span_len
) {
  if (js && !js->errsite.valid) js_set_error_site_from_vm_top(js);

  const char *src = NULL;
  jsoff_t src_len = 0;
  jsoff_t src_pos = 0;
  jsoff_t span_len = 0;

  if (js->errsite.valid) {
    src = js->errsite.src;
    src_len = js->errsite.src_len;
    src_pos = js->errsite.off;
    span_len = js->errsite.span_len;
  }

  if (out_src) *out_src = src;
  if (out_src_len) *out_src_len = src_len;
  if (out_src_pos) *out_src_pos = src_pos;
  if (out_span_len) *out_span_len = span_len;
}

typedef struct {
  const char *src;
  jsoff_t src_len;
  jsoff_t src_pos;
  int error_col;
  int error_span_cols;
  int line;
  int col;
  char error_line[256];
} js_error_render_site_t;

static void js_prepare_error_render_site(ant_t *js, js_error_render_site_t *site) {
  if (!site) return;
  memset(site, 0, sizeof(*site));
  site->line = 1;
  site->col = 1;
  site->error_col = 1;
  site->error_span_cols = 1;

  jsoff_t src_span_len = 0;
  jsoff_t line_start = 0, line_end = 0;
  resolve_error_site(js, &site->src, &site->src_len, &site->src_pos, &src_span_len);

  get_line_col(site->src, site->src_len, site->src_pos, &site->line, &site->col);
  get_error_line(
    site->src, site->src_len, site->src_pos,
    site->error_line, sizeof(site->error_line),
    &site->error_col, &line_start, &line_end
  );
  site->error_span_cols = error_span_cols_for_line(site->src_pos, src_span_len, line_start, line_end);
}

typedef enum {
  JS_STACK_TEXT_FROM_ERROR_OBJECT = 0,
  JS_STACK_TEXT_FROM_THROW_VALUE = 1,
} js_stack_text_kind_t;

static jsval_t js_build_stack_text(ant_t *js, js_stack_text_kind_t kind, jsval_t value) {
  js_error_render_site_t site;
  js_prepare_error_render_site(js, &site);

  errbuf_t eb = { malloc(4096), 4096 };
  if (!eb.buf) return js_mkundef();
  eb.buf[0] = '\0';

  size_t n = 0;
  n = append_error_header(&eb, js, 0, site.line, site.col);

  if (n > 0 && eb.buf[n - 1] == '\n') {
    n--;
    eb.buf[n] = '\0';
  }

  bool rendered_context = append_error_context(
    &eb, &n, site.src, site.src_len, site.src_pos,
    site.line, site.error_col, site.error_span_cols
  );

  if (!rendered_context && site.error_line[0]) {
    n = append_errbuf_fmt(&eb, n, "\n%s\n", site.error_line);
    append_error_caret(&eb, &n, site.error_col, site.error_span_cols);
  }

  n = append_errbuf_fmt(&eb, n, "\n");

  if (kind == JS_STACK_TEXT_FROM_ERROR_OBJECT) {
    const char *err_name = "Error";
    const char *err_msg = NULL;
    
    jsoff_t name_len = 5, msg_len = 0; 
    const char *n_str = get_str_prop(js, value, "name", 4, &name_len);
    
    if (n_str) err_name = n_str;
    err_msg = get_str_prop(js, value, "message", 7, &msg_len);

    if (err_msg) {
      n = append_errbuf_fmt(&eb, n,
        "%s%.*s%s: %s%.*s%s",
        C_RED, (int)name_len, err_name, C_RESET,
        C_BOLD, (int)msg_len, err_msg, C_RESET);
    } else {
      n = append_errbuf_fmt(&eb, n,
        "%s%.*s%s",
        C_RED, (int)name_len, err_name, C_RESET);
    }
  } else n = append_error_value(&eb, js, n, value);

  format_error_stack(
    &eb, js, &n, site.line, site.col, false,
    site.error_line, site.error_col, site.error_span_cols
  );

  jsval_t stack_str = js_mkstr(js, eb.buf, n);
  free(eb.buf);
  return stack_str;
}

static void js_capture_stack(ant_t *js, jsval_t err_obj) {
  jsval_t stack_str = js_build_stack_text(js, JS_STACK_TEXT_FROM_ERROR_OBJECT, err_obj);
  if (vtype(stack_str) != T_STR) return;

  js_set(js, err_obj, "stack", stack_str);
  js_clear_error_site(js);
}

js_err_type_t get_error_type(ant_t *js) {
  if (!js->thrown_exists) return JS_ERR_GENERIC;
  jsval_t err_type = js_get_slot(js, js->thrown_value, SLOT_ERR_TYPE);
  if (vtype(err_type) != T_NUM) return JS_ERR_GENERIC;
  return (js_err_type_t)((int)js_getnum(err_type) & ~JS_ERR_NO_STACK);
}

__attribute__((format(printf, 4, 5)))
jsval_t js_create_error(ant_t *js, js_err_type_t err_type, jsval_t props, const char *xx, ...) {
  va_list ap;
  char error_msg[256] = {0};

  bool no_stack = (err_type & JS_ERR_NO_STACK) != 0;
  js_err_type_t base_type = (js_err_type_t)(err_type & ~JS_ERR_NO_STACK);

  va_start(ap, xx);
    vsnprintf(error_msg, sizeof(error_msg), xx, ap);
  va_end(ap);

  const char *err_name = get_error_type_name(base_type);
  size_t err_name_len = strlen(err_name);
  size_t msg_len = strlen(error_msg);

  jsval_t err_obj = js_mkobj(js);
  js_set(js, err_obj, "name", js_mkstr(js, err_name, err_name_len));
  js_set(js, err_obj, "message", js_mkstr(js, error_msg, msg_len));
  js_set_slot(js, err_obj, SLOT_ERR_TYPE, js_mknum((double)err_type));

  int props_type = vtype(props);
  if ((JS_TYPE_FLAG(props_type) & T_SPECIAL_OBJECT_MASK) != 0) {
    js_merge_obj(js, err_obj, props);
  }
  jsval_t proto = js_get_ctor_proto(js, err_name, err_name_len);
  int proto_type = vtype(proto);
  if ((JS_TYPE_FLAG(proto_type) & T_SPECIAL_OBJECT_MASK) != 0) {
    js_set_proto(js, err_obj, proto);
  }

  js->thrown_exists = true;
  js->thrown_value = err_obj;

  if (!no_stack) {
    js_capture_stack(js, err_obj);
  }

  js_clear_error_site(js);
  return mkval(T_ERR, vdata(err_obj));
}

jsval_t js_throw(ant_t *js, jsval_t value) {
  if (vtype(value) == T_OBJ) {
    jsval_t existing = js_get(js, value, "stack");
    if (vtype(existing) == T_STR) {
      js->thrown_exists = true;
      js->thrown_value = value;
      js_clear_error_site(js);
      return mkval(T_ERR, vdata(value));
    }
    jsval_t slot = js_get_slot(js, value, SLOT_ERR_TYPE);
    if (vtype(slot) == T_NUM && ((int)js_getnum(slot) & JS_ERR_NO_STACK)) {
      js->thrown_exists = true;
      js->thrown_value = value;
      js_clear_error_site(js);
      return mkval(T_ERR, vdata(value));
    }
  }

  jsval_t stack_str = js_build_stack_text(js, JS_STACK_TEXT_FROM_THROW_VALUE, value);
  if (vtype(stack_str) != T_STR) return mkval(T_ERR, 0);

  if (vtype(value) == T_OBJ) {
    js_set(js, value, "stack", stack_str);
  }

  js->thrown_exists = true;
  js->thrown_value = value;
  js->thrown_stack = stack_str;
  js_clear_error_site(js);
  
  return mkval(T_ERR, 0);
}

void js_print_stack_trace_vm(ant_t *js, FILE *stream) {
  const char *fallback_file = js->filename ? js->filename : "<unknown>";
  if (!stream) return;
  
  error_visit_vm_stack_frames(
    js, fallback_file, 
    error_visit_frame_print_file, stream
  );
}
