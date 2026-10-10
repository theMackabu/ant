#include "errors.h"
#include "modules/process.h"
#include "internal.h"
#include "descriptors.h"
#include "output.h"
#include "silver/engine.h"
#include "silver/stack_trace.h"
#include "silver/call.h"
#include "modules/io.h"
#include "gc/roots.h"
#include "highlight.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <crprintf.h>

#ifndef ANT_WASM_EMBED
#include <uv.h>
#endif

#if defined(ANT_WASM_EMBED)
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <sys/ioctl.h>
#endif

typedef struct { char *buf; size_t size; } errbuf_t;
constexpr ant_offset_t ERROR_CONTEXT_MAX_SOURCE_BYTES = 256 * 1024;

void print_error_value(ant_t *js, ant_value_t value, ant_value_t fallback_stack, const char *prefix) {
  if (is_err(value)) {
    fallback_stack = Ant_Exception_Stack(js, value);
    value = Ant_Exception_Value(js, value);
  }

  ant_value_t obj = value;
  ant_output_stream_t *out = ant_output_stream(stderr);
  
  ant_value_t stack = js_mkundef();
  bool is_real_error = false, no_stack = false;
  ant_output_stream_begin(out);
  
  if (vtype(obj) == kTypeObject) {
    ant_value_t err_type = js_get_slot(obj, SLOT_ERR_TYPE);
    is_real_error = js_get_slot(obj, SLOT_ERROR_BRAND) == js_true || vtype(err_type) == kTypeNumber;
    no_stack = vtype(err_type) == kTypeNumber && ((int)js_getnum(err_type) & JS_ERR_NO_STACK);
    if (!no_stack) stack = js_getprop_fallback_len(js, obj, "stack", 5);
  }
  
  if (!no_stack && vtype(stack) != kTypeString) stack = fallback_stack;
  if (prefix) ant_output_stream_append_cstr(out, prefix);

  if (is_real_error && vtype(stack) == kTypeString) {
    io_print_error_stack(js, out, obj, stack);
  } else if (is_real_error) {
    io_print_error_header(js, out, obj);
    io_print_error_props(js, out, obj);
  } else ant_output_stream_append_cstr(out, js_str(js, value));

  ant_output_stream_putc(out, '\n');
  ant_output_stream_flush(out);
}

// TODO: exceptions.c
bool Ant_Exception_Pending(ant_t *js) {
  return js && js->exception != js_mkundef();
}

ant_value_t Ant_Exception_Peek(ant_t *js) {
  return Ant_Exception_Pending(js) ? js->exception : js_mkundef();
}

ant_value_t Ant_Exception_Current(ant_t *js) {
  if (Ant_Exception_Pending(js)) return js->exception;
  return js && is_err(js->exception_oom) ? js->exception_oom : mkval(kTypeError, 0);
}

void Ant_Exception_Set(ant_t *js, ant_value_t completion) {
  js->exception = is_err(completion) ? completion : js_mkundef();
}

void Ant_Exception_Clear(ant_t *js) {
  js->exception = js_mkundef();
}

static ant_object_t *exception_record(ant_t *js, ant_value_t completion) {
  if (!is_err(completion)) return NULL;
  if (!vdata(completion)) completion = js ? js->exception_oom : js_mkundef();
  if (!is_err(completion) || vdata(completion) < 2) return NULL;
  ant_object_t *record = js_obj_ptr(js_as_obj(completion));
  return record && record->type_tag == kTypeError ? record : NULL;
}

ant_value_t Ant_Exception_Value(ant_t *js, ant_value_t completion) {
  ant_object_t *record = exception_record(js, completion);
  return record ? record->u.exception.value : is_err(completion) ? js_mkundef() : completion;
}

ant_value_t Ant_Exception_Raise(ant_t *js, ant_value_t value, ant_value_t stack) {
  ant_value_t completion = Ant_Exception_CreateRecord(js, value, stack);
  Ant_Exception_Set(js, completion);
  return completion;
}

ant_value_t js_take_thrown(ant_t *js, ant_value_t fallback) {
  ant_value_t completion = is_err(fallback) ? fallback : Ant_Exception_Peek(js);
  if (!is_err(completion)) return fallback;
  ant_value_t value = Ant_Exception_Value(js, completion);
  if (js->exception == completion) Ant_Exception_Clear(js);
  return value;
}

ant_value_t Ant_Error_ConsumeMarker(ant_t *js, ant_value_t value) {
  return is_err(value) ? js_take_thrown(js, value) : value;
}

ant_value_t Ant_Error_CallCallback(
  ant_t *js, ant_value_t callback, ant_value_t this_value,
  ant_value_t *args, int nargs
) {
  if (nargs < 1 || !is_err(args[0]))
    return sv_vm_call(js->vm, js, callback, this_value, args, nargs, NULL, js_mkundef());

  ant_value_t inline_args[8];
  ant_value_t *call_args = nargs <= 8 ? inline_args : malloc((size_t)nargs * sizeof(*call_args));

  if (!call_args) return js_mkerr(js, "out of memory invoking error callback");
  memcpy(call_args, args, (size_t)nargs * sizeof(*call_args));

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, callback);
  GC_ROOT_PIN(js, this_value);
  for (int i = 0; i < nargs; i++) GC_ROOT_PIN(js, call_args[i]);

  call_args[0] = Ant_Error_ConsumeMarker(js, call_args[0]);
  ant_value_t result = sv_vm_call(js->vm, js, callback, this_value, call_args, nargs, NULL, js_mkundef());

  GC_ROOT_RESTORE(js, root_mark);
  if (call_args != inline_args) free(call_args);

  return result;
}

bool print_uncaught_throw(ant_t *js) {
  if (!Ant_Exception_Pending(js)) return false;
  print_error_value(js, Ant_Exception_Value(js, Ant_Exception_Peek(js)), Ant_Exception_Stack(js, Ant_Exception_Peek(js)), NULL);
  js_take_thrown(js, js_mkundef());
  return true;
}

bool print_unhandled_promise_rejection(ant_t *js, ant_value_t value) {
  print_error_value(js, value, js_mkundef(), "Uncaught (in promise) ");
  return true;
}

bool js_mark_errorlike_no_stack(ant_t *js, ant_value_t value) {
  if (vtype(value) != kTypeObject) return false;
  if (js_get_slot(value, SLOT_ERROR_BRAND) == js_true) return false;

  const char *name = get_str_prop(js, value, "name", 4, NULL);
  const char *message = get_str_prop(js, value, "message", 7, NULL);
  if ((!name || !*name) && (!message || !*message)) return false;

  ant_value_t err_type = js_get_slot(value, SLOT_ERR_TYPE);
  int base_type = vtype(err_type) == kTypeNumber ? (int)js_getnum(err_type) : JS_ERR_GENERIC;

  js_set(js, value, "stack", js_mkundef());
  js_set_slot(value, SLOT_ERR_TYPE, js_mknum((double)(base_type | JS_ERR_NO_STACK)));
  
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
  for (;;) {
    if (!ensure_errbuf_capacity(eb, used + 1)) return used;

    size_t remaining = eb->size - used;
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(eb->buf + used, remaining, fmt, ap);
    va_end(ap);

    if (written >= 0 && (size_t)written < remaining) return used + (size_t)written;
    eb->buf[used] = '\0';
    if (written < 0 || (size_t)written > SIZE_MAX - used - 1) return used;
    if (!ensure_errbuf_capacity(eb, used + (size_t)written + 1)) return used;
  }
}

static void get_line_col(const char *code, ant_offset_t code_len, ant_offset_t pos, int *out_line, int *out_col) {
  if (!out_line || !out_col) return;

  *out_line = 1;
  *out_col = 1;

  if (!code || code_len <= 0 || pos <= 0) return;
  if (pos > code_len) pos = code_len;

  for (ant_offset_t i = 0; i < pos; i++) {
    char ch = code[i];
    if (ch == '\0') break;
    if (ch == '\n') {
      *out_line += 1;
      *out_col = 1;
    } else *out_col += 1;
  }
}

static void get_error_line(
  const char *code, ant_offset_t clen, ant_offset_t pos, char *buf, size_t bufsize,
  int *line_start_col, ant_offset_t *out_line_start, ant_offset_t *out_line_end
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

  ant_offset_t line_start = pos;
  while (line_start > 0 && code[line_start - 1] != '\n') {
    line_start--;
  }

  ant_offset_t line_end = pos;
  while (line_end < clen && code[line_end] != '\n' && code[line_end] != '\0') {
    line_end++;
  }

  ant_offset_t line_len = line_end - line_start;
  if (line_len >= bufsize) line_len = (ant_offset_t)(bufsize - 1);

  memcpy(buf, &code[line_start], line_len);
  buf[line_len] = '\0';
  
  if (line_start_col) *line_start_col = (int)(pos - line_start) + 1;
  if (out_line_start) *out_line_start = line_start;
  if (out_line_end) *out_line_end = line_end;
}

static size_t append_error_value(errbuf_t *eb, ant_t *js, size_t used, ant_value_t value) {
  const char *name = "Error";
  const char *msg = NULL;

  ant_offset_t name_len = 5;
  ant_offset_t msg_len = 0;

  static const void *type_dispatch[] = {
    [kTypeString] = &&l_type_str,
    [kTypeObject] = &&l_type_obj,
    [kTypeFunction] = &&l_type_default,
    [kTypeArray] = &&l_type_default,
    [kTypePromise] = &&l_type_default,
    [kTypeGenerator] = &&l_type_default,
    [kTypeBigInt] = &&l_type_default,
    [kTypeNumber] = &&l_type_default,
    [kTypeBool] = &&l_type_default,
    [kTypeSymbol] = &&l_type_default,
    [kTypeBuiltin] = &&l_type_default,
    [kTypeTypedArray] = &&l_type_default,
    [kTypeError] = &&l_type_default,
    [kTypeUndefined] = &&l_type_default,
    [kTypeNull] = &&l_type_default,
  };

  uint8_t t = vtype(value);
  if (t < sizeof(type_dispatch) / sizeof(type_dispatch[0]) && type_dispatch[t]) {
    goto *type_dispatch[t];
  }
  goto l_type_default;

  l_type_str:
    msg = (const char *)(uintptr_t)(vstr(js, value, &msg_len));
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
    msg_len = msg ? (ant_offset_t)strlen(msg) : 0;
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
  const char *red = C_RED, *reset = C_RESET;
  
  if (!ensure_errbuf_capacity(eb, *n + (size_t)error_col + (size_t)span_cols + strlen(red) + strlen(reset) + 2)) return;
  if (*n >= eb->size - 1) return;

  size_t remaining = eb->size - *n;
  for (int i = 1; i < error_col && remaining > 1; i++) {
    eb->buf[(*n)++] = ' ';
    remaining--;
  }
  
  *n = append_errbuf_fmt(eb, *n, "%s", red);
  remaining = eb->size - *n;
  for (int i = 0; i < span_cols && remaining > 1; i++) {
    eb->buf[(*n)++] = '^';
    remaining--;
  }
  
  *n = append_errbuf_fmt(eb, *n, "%s", reset);
}

static int error_span_cols_for_line(ant_offset_t src_pos, ant_offset_t span_len, ant_offset_t line_start, ant_offset_t line_end) {
  if (line_end < line_start) return 1;
  if (src_pos < line_start) src_pos = line_start;
  if (src_pos > line_end) src_pos = line_end;
  if (span_len <= 0) return 1;

  ant_offset_t span_end = src_pos + span_len;
  if (span_end < src_pos) span_end = src_pos;
  if (span_end > line_end) span_end = line_end;

  ant_offset_t width = span_end - src_pos;
  if (width <= 0) width = 1;
  if (width > INT_MAX) width = INT_MAX;
  return (int)width;
}

static int error_terminal_columns(void) {
#ifdef ANT_WASM_EMBED
  return 120;
#elif defined(_WIN32)
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

#ifndef ANT_WASM_EMBED
  const char *cols_env = getenv("COLUMNS");
  if (cols_env && *cols_env) {
    char *end = NULL;
    long cols = strtol(cols_env, &end, 10);
    if (end != cols_env && cols > 0 && cols <= INT_MAX) return (int)cols;
  }
#endif
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

static const char *error_frame_name_of(ant_t *js, ant_value_t callee, sv_func_t *func) {
  if (func && func->debug->name && func->debug->name[0]) return func->debug->name;
  
  ant_value_t name;
  if (vtype(callee) == kTypeFunction && js_try_get_own_data_prop(js, callee, "name", 4, &name) && vtype(name) == kTypeString) {
    size_t len = 0;
    const char *text = js_getstr(js, name, &len);
    if (text && len > 0) return text;
  }
  
  if (func && func->debug->display_name && !func->debug->display_name_varies) 
    return func->debug->display_name;
  
  return "<anonymous>";
}

typedef struct {
  const char *name;
  const char *file;
  int line;
  int col;
} js_vm_frame_view_t;

typedef bool 
  (*js_vm_frame_visitor_fn)
  (ant_t *js, const js_vm_frame_view_t *view, void *ctx);

static void error_frame_view(
  ant_t *js, sv_func_t *func, ant_value_t callee, int bc_off,
  const char *fallback_file, js_vm_frame_view_t *out
) {
  out->name = error_frame_name_of(js, callee, func);
  out->file = (func && func->debug->filename) ? func->debug->filename : fallback_file;
  out->line = (func && func->debug->source_line > 0) ? func->debug->source_line : 1;
  out->col = 1;

  uint32_t l, c;
  if (bc_off >= 0 && sv_lookup_srcpos(func, bc_off, &l, &c)) {
    out->line = (int)l; 
    out->col = (int)c;
  }
}

static bool error_frame_is_wrapper(const sv_stack_entry_t *e, const js_vm_frame_view_t *view, int newer) {
  return 
    e->frame_index == 0 && newer > 0 &&
    view->line == 1 && view->col == 1 &&
    strcmp(view->name, "<anonymous>") == 0;
}

static bool error_visit_live_frames(
  ant_t *js, int limit, const char *fallback_file, js_vm_frame_visitor_fn visitor, void *ctx
) {
  if (!js || !js->vm) return true;
  
  sv_stack_iter_t it;
  sv_stack_entry_t e;
  
  bool ok = true;
  int count = 0;
  
  sv_stack_iter_init(js->vm, &it, limit, true);
  while (ok && (limit < 0 || count < limit) && sv_stack_iter_next(&it, &e)) {
    js_vm_frame_view_t view;
    error_frame_view(js, e.func, e.callee, e.bc_off, fallback_file, &view);
    
    if (error_frame_is_wrapper(&e, &view, count)) continue;
    count++;
    ok = visitor(js, &view, ctx);
  }
  
  sv_stack_iter_finish(&it);
  return ok;
}

static int error_stack_trace_limit(ant_t *js) {
  ant_value_t ctor = js->builtins.error_ctor, limit;
  
  if (vtype(ctor) != kTypeFunction) return 10;
  if (!js_try_get_own_data_prop(js, ctor, "stackTraceLimit", 15, &limit) || vtype(limit) != kTypeNumber) return -1;
  
  double d = js_getnum(limit);
  if (!(d > 0)) return 0;
  
  return d >= (double)INT_MAX ? INT_MAX : (int)d;
}

#ifndef ANT_WASM_EMBED
static _Thread_local struct {
  bool fetched, ok;
  uint64_t epoch;
  size_t len;
  char buf[4096];
} error_cwd_cache;

static const char *error_cwd(size_t *len) {
  uint64_t epoch = process_cwd_epoch_get();
  if (!error_cwd_cache.fetched || error_cwd_cache.epoch != epoch) {
    size_t n = sizeof(error_cwd_cache.buf);
    error_cwd_cache.ok = uv_cwd(error_cwd_cache.buf, &n) == 0;
    error_cwd_cache.len = error_cwd_cache.ok ? n : 0;
    error_cwd_cache.epoch = epoch;
    error_cwd_cache.fetched = true;
  }
  *len = error_cwd_cache.len;
  return error_cwd_cache.ok ? error_cwd_cache.buf : NULL;
}
#endif

static size_t error_frame_path_prefix(const char *file) {
#ifndef ANT_WASM_EMBED
  size_t len = 0;
  const char *cwd = error_cwd(&len);
  if (cwd && len > 0 && strncmp(file, cwd, len) == 0) {
    if (cwd[len - 1] == '/' || cwd[len - 1] == '\\') return len;
    if (file[len] == '/' || file[len] == '\\') return len + 1;
  }
#endif
  const char *slash = strrchr(file, '/');
#ifdef _WIN32
  const char *backslash = strrchr(file, '\\');
  if (backslash && (!slash || backslash > slash)) slash = backslash;
#endif
  return slash ? (size_t)(slash + 1 - file) : 0;
}

static bool append_error_frame(
  errbuf_t *eb, size_t *n, const char *name, const char *file, int line, int col, bool color
) {
  const char *dim = color ? C_DIM : "";
  const char *cyan = color ? C_CYAN : "";
  const char *yellow = color ? C_YELLOW : "";
  const char *reset = color ? C_RESET : "";
  
  size_t prefix = color && !io_no_color ? error_frame_path_prefix(file) : 0;
  size_t start = *n;

  *n = append_errbuf_fmt(eb, *n,
    "\n  %sat%s %s%s%s%s"
    "%s%s%.*s%s%s%s%s"
    "%s:%s%s%d%s%s:%s%d%s"
    "%s%s%s",
    dim, reset, name ? name : "", name ? " " : "", dim, name ? "(" : "",
    cyan, dim, (int)prefix, file, reset, cyan, file + prefix, reset,
    dim, reset, yellow, line, reset, dim, yellow, col, reset,
    dim, name ? ")" : "", reset
  );
  
  return *n > start;
}

typedef struct {
  errbuf_t *eb;
  size_t *n;
  bool color;
} error_frame_errbuf_ctx_t;

static bool error_visit_frame_append_errbuf(ant_t *js, const js_vm_frame_view_t *view, void *ctx) {
  error_frame_errbuf_ctx_t *c = (error_frame_errbuf_ctx_t *)ctx;
  return append_error_frame(c->eb, c->n, view->name, view->file, view->line, view->col, c->color);
}

ant_value_t js_capture_raw_stack(ant_t *js) {
  errbuf_t eb = { malloc(4096), 4096 };
  if (!eb.buf) return js_mkundef();
  eb.buf[0] = '\0';

  size_t n = 0;
  const char *file = js->filename ? js->filename : "<eval>";
  
  error_frame_errbuf_ctx_t ctx = { &eb, &n, false };
  error_visit_live_frames(js, error_stack_trace_limit(js), file, error_visit_frame_append_errbuf, &ctx);

  ant_value_t stack_str = js_mkstr(js, eb.buf, n);
  free(eb.buf);
  
  return stack_str;
}

static bool append_error_context(
  errbuf_t *eb, size_t *n,
  const char *src, ant_offset_t src_len,
  ant_offset_t src_pos,
  int error_line_no, int error_col, int error_span_cols
) {
  if (!src || src_len <= 0 || !n) return false;
  if (src_len > ERROR_CONTEXT_MAX_SOURCE_BYTES) return false;
  if (src_pos > src_len) src_pos = src_len;

  ant_offset_t err_line_start = src_pos;
  while (err_line_start > 0 && src[err_line_start - 1] != '\n') err_line_start--;
  
  ant_offset_t err_line_end = src_pos;
  while (err_line_end < src_len && src[err_line_end] != '\n' && src[err_line_end] != '\0') err_line_end++;

  ant_offset_t ctx_start = err_line_start;
  int first_line_no = error_line_no;
  
  for (int i = 0; i < 5 && ctx_start > 0; i++) {
    ant_offset_t prev = ctx_start - 1;
    if (src[prev] == '\n') {
      if (prev == 0) { ctx_start = 0; break; }
      prev--;
    }
    
    while (prev > 0 && src[prev] != '\n') prev--;
    ctx_start = (src[prev] == '\n') ? prev + 1 : 0;
    
    first_line_no--;
    if (first_line_no < 1) { first_line_no = 1; break; }
  }

  int gutter_w = count_digits_int(error_line_no);
  int src_cols_limit = error_context_src_cols_limit(gutter_w);

  char tagged[4096]; char rendered[8192];
  highlight_state hl_state = HL_STATE_INIT;

  ant_offset_t cur = ctx_start;
  int line_no = first_line_no;

  while (cur <= err_line_end && cur < src_len) {
    ant_offset_t ls = cur, le = cur;
    while (le < src_len && src[le] != '\n' && src[le] != '\0') le++;
    
    int line_len = (int)(le - ls);
    bool was_clipped = (src_cols_limit > 0 && line_len > src_cols_limit);

    if (!io_no_color) {
      highlight_js_line_clipped(
        src + ls, (size_t)line_len, (size_t)src_cols_limit,
        tagged, sizeof(tagged), &hl_state
      );
      crsprintf_stateful(rendered, sizeof(rendered), NULL, tagged);
      *n = append_errbuf_fmt(eb, *n, "\n%*d | %s", gutter_w, line_no, rendered);
    } else {
      int shown = was_clipped ? src_cols_limit : line_len;
      *n = append_errbuf_fmt(eb, *n, "\n%*d | %.*s", gutter_w, line_no, shown, src + ls);
    }

    if (was_clipped) *n = append_errbuf_fmt(eb, *n, "...");
    if (ls == err_line_start) {
      *n = append_errbuf_fmt(eb, *n, "\n%*s   ", gutter_w, "");
      int caret_col = error_col;
      int caret_span = error_span_cols;
      if (was_clipped) {
        int max_col = src_cols_limit > 0 ? src_cols_limit : 1;
        if (caret_col > max_col) caret_col = max_col;
        if (caret_span > max_col - caret_col + 1) caret_span = max_col - caret_col + 1;
        if (caret_span < 1) caret_span = 1;
      }
      append_error_caret(eb, n, caret_col, caret_span);
    }

    if (le >= src_len || src[le] == '\0') break;
    cur = le + 1; line_no++;
  }

  return true;
}

js_error_site_t js_error_site_from_source(
  const char *src, ant_offset_t src_len, const char *filename,
  ant_offset_t off, ant_offset_t span_len
) {
  return (js_error_site_t){
    .src = src,
    .src_len = src_len,
    .filename = filename,
    .off = off < 0 ? 0 : off,
    .span_len = span_len < 0 ? 0 : span_len,
    .valid = src != NULL && src_len >= 0,
  };
}

void js_get_call_location(ant_t *js, const char **out_filename, int *out_line, int *out_col) {
  if (!js) return;
  js_error_site_t site = js_error_site_from_vm_top(js);
  
  if (out_filename) *out_filename = (site.valid && site.filename) ? site.filename : js->filename;
  if (out_line) *out_line = 1;
  if (out_col)  *out_col  = 1;
  
  if (site.valid && site.line > 0) {
    if (out_line) *out_line = (int)site.line;
    if (out_col)  *out_col  = (int)site.col;
  } else if (site.valid && site.src) get_line_col(
    site.src, site.src_len, site.off, out_line, out_col
  );
}

typedef struct {
  const char *src;
  ant_offset_t src_len;
  ant_offset_t src_pos;
  ant_offset_t src_span_len;
  int error_col;
  int error_span_cols;
  int line;
  int col;
  char error_line[256];
} js_error_render_site_t;

static void error_render_site_init(const js_error_site_t *es, js_error_render_site_t *site) {
  memset(site, 0, sizeof(*site));
  site->line = 1;
  site->col = 1;
  site->error_col = 1;
  site->error_span_cols = 1;
  if (!es->valid) return;

  site->src = es->src;
  site->src_len = es->src_len;
  site->src_pos = es->off;
  site->src_span_len = es->span_len;
  
  if (es->line > 0) {
    site->line = (int)es->line;
    site->col = (int)es->col;
  } else get_line_col(site->src, site->src_len, site->src_pos, &site->line, &site->col);
}

static void error_render_site_finish(js_error_render_site_t *site) {
  if (site->src_len > ERROR_CONTEXT_MAX_SOURCE_BYTES) {
    site->src = NULL;
    site->src_len = 0;
    return;
  }

  ant_offset_t line_start = 0, line_end = 0;
  get_error_line(
    site->src, site->src_len, site->src_pos,
    site->error_line, sizeof(site->error_line),
    &site->error_col, &line_start, &line_end
  );
  site->error_span_cols = error_span_cols_for_line(site->src_pos, site->src_span_len, line_start, line_end);
}

static const char *error_site_file(ant_t *js, const js_error_site_t *es) {
  if (es->valid && es->filename) return es->filename;
  return js->filename ? js->filename : "<eval>";
}

static void append_site_prefix(errbuf_t *eb, size_t *n, const char *file, const js_error_render_site_t *site) {
  *n = append_errbuf_fmt(eb, *n, "%s:%d:%d", file, site->line, site->col);

  bool rendered_context = append_error_context(
    eb, n, site->src, site->src_len, site->src_pos,
    site->line, site->error_col, site->error_span_cols
  );

  if (!rendered_context && site->error_line[0]) {
    *n = append_errbuf_fmt(eb, *n, "\n%s\n", site->error_line);
    append_error_caret(eb, n, site->error_col, site->error_span_cols);
  }

  *n = append_errbuf_fmt(eb, *n, "\n");
}

enum {
  ERR_REC_SITE_FUNC,
  ERR_REC_SITE_POS,
  ERR_REC_SITE_SPAN,
  ERR_REC_LINE,
  ERR_REC_COL,
  ERR_REC_PREFIX,
  ERR_REC_FILE,
  ERR_REC_LIMIT,
  ERR_REC_FRAMES
};

enum {
  ERR_FRAME_FUNC,
  ERR_FRAME_CALLEE,
  ERR_FRAME_POS,
  ERR_FRAME_FIELDS
};

static constexpr int ERR_REC_INLINE_FRAMES = 16;

static sv_func_t *error_funcinfo_ptr(ant_value_t v) {
  return vtype(v) == kTypeFunctionInfo ? (sv_func_t *)vptr(v) : NULL;
}

static ant_value_t error_frame_pos(const sv_stack_entry_t *e) {
  return js_mknum(e->pc ? -(double)e->pc : (double)e->bc_off);
}

static bool error_capture_frame(
  ant_t *js, const sv_stack_entry_t *e, int count, ant_value_t **fields, ant_value_t *local, int *cap
) {
  if (count == *cap) {
    int grown_cap = *cap * 2;
    size_t bytes = (ERR_REC_FRAMES + (size_t)grown_cap * ERR_FRAME_FIELDS) * sizeof(ant_value_t);
    ant_value_t *grown = *fields == local ? malloc(bytes) : realloc(*fields, bytes);
    
    if (!grown) return false;
    if (*fields == local) memcpy(grown, local, (ERR_REC_FRAMES + (size_t)count * ERR_FRAME_FIELDS) * sizeof(ant_value_t));
    
    *fields = grown;
    *cap = grown_cap;
  }

  ant_value_t *slot = &(*fields)[ERR_REC_FRAMES + (size_t)count * ERR_FRAME_FIELDS];
  bool named = e->func && e->func->debug->name && e->func->debug->name[0];
  
  slot[ERR_FRAME_FUNC] = e->func ? mkref(kTypeFunctionInfo, e->func) : js_mkundef();
  slot[ERR_FRAME_CALLEE] = named ? js_mkundef() : e->callee;
  slot[ERR_FRAME_POS] = error_frame_pos(e);
  
  return true;
}

static ant_value_t error_capture_record(ant_t *js, const js_error_site_t *at) {
  int limit = error_stack_trace_limit(js);
  if (limit < 0) return js_mkundef();

  ant_value_t local[ERR_REC_FRAMES + ERR_REC_INLINE_FRAMES * ERR_FRAME_FIELDS];
  ant_value_t *fields = local;
  int cap = ERR_REC_INLINE_FRAMES, count = 0;
  bool need_file = false;

  sv_stack_iter_t it;
  sv_stack_entry_t e;
  sv_stack_iter_init(js->vm, &it, limit, false);
  
  while (count < limit && sv_stack_iter_next(&it, &e)) {
    if (e.frame_index == 0 && count > 0) {
      js_vm_frame_view_t view;
      error_frame_view(js, e.func, e.callee, e.bc_off, "", &view);
      if (error_frame_is_wrapper(&e, &view, count)) continue;
    }
    if (!error_capture_frame(js, &e, count, &fields, local, &cap)) break;
    if (!e.func || !e.func->debug->filename) need_file = true;
    count++;
  }
  sv_stack_iter_finish(&it);

  js_error_site_t es = at && at->valid ? *at : (js_error_site_t){0};
  js_error_render_site_t site;
  error_render_site_init(&es, &site);

  const char *file = es.valid ? error_site_file(js, &es) : js->filename ? js->filename : "<eval>";
  bool lazy_site = es.func && site.src == es.func->debug->source && site.src_len <= ERROR_CONTEXT_MAX_SOURCE_BYTES;
  if (es.valid) need_file |= !lazy_site || file != es.func->debug->filename;
  else need_file |= count == 0;

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t prefix = js_mkundef();
  
  if (es.valid && !lazy_site) {
    errbuf_t eb = { malloc(1024), 1024 };
    if (eb.buf) {
      size_t n = 0;
      error_render_site_finish(&site);
      append_site_prefix(&eb, &n, file, &site);
      prefix = js_mkstr(js, eb.buf, n);
      free(eb.buf);
    }
    GC_ROOT_PIN(js, prefix);
  }

  ant_value_t file_str = need_file ? js_mkstr(js, file, strlen(file)) : js_mkundef();
  GC_ROOT_PIN(js, file_str);

  fields[ERR_REC_SITE_FUNC] = lazy_site ? mkref(kTypeFunctionInfo, es.func) : js_mkundef();
  fields[ERR_REC_SITE_POS] = js_mknum((double)site.src_pos);
  fields[ERR_REC_SITE_SPAN] = js_mknum((double)site.src_span_len);
  fields[ERR_REC_LINE] = es.valid ? js_mknum(site.line) : js_mkundef();
  fields[ERR_REC_COL] = js_mknum(site.col);
  fields[ERR_REC_PREFIX] = prefix;
  fields[ERR_REC_FILE] = file_str;
  fields[ERR_REC_LIMIT] = js_mknum(limit);

  ant_value_t record = js_mkarr_dense_literal(js, fields, (uint32_t)(ERR_REC_FRAMES + (size_t)count * ERR_FRAME_FIELDS));
  if (fields != local) free(fields);

  GC_ROOT_RESTORE(js, root_mark);
  return record;
}

static int error_record_int(ant_t *js, ant_value_t rec, int field) {
  ant_value_t v = js_arr_get(js, rec, field);
  return vtype(v) == kTypeNumber ? (int)js_getnum(v) : 0;
}

static int error_record_frame_count(ant_t *js, ant_value_t rec) {
  return (int)((js_arr_len(js, rec) - ERR_REC_FRAMES) / ERR_FRAME_FIELDS);
}

typedef struct {
  sv_func_t *func;
  ant_value_t callee;
  int bc_off;
} error_record_frame_t;

static int error_record_frame(ant_t *js, ant_value_t rec, int k, error_record_frame_t *out) {
  ant_offset_t base = ERR_REC_FRAMES + (ant_offset_t)k * ERR_FRAME_FIELDS;
  sv_func_t *func = error_funcinfo_ptr(js_arr_get(js, rec, base + ERR_FRAME_FUNC));
  ant_value_t callee = js_arr_get(js, rec, base + ERR_FRAME_CALLEE);
  ant_value_t pos = js_arr_get(js, rec, base + ERR_FRAME_POS);
  double p = vtype(pos) == kTypeNumber ? js_getnum(pos) : -1;

  if (p >= -1) {
    out[0] = (error_record_frame_t){ func, callee, (int)p };
    return 1;
  }

  sv_jit_frame_t frames[SV_JIT_INLINE_FRAMES_MAX + 1];
  int n = sv_jit_frames_at(js, (uintptr_t)-p, frames, SV_JIT_INLINE_FRAMES_MAX + 1);
  if (n <= 0) frames[n++] = (sv_jit_frame_t){ func, -1 };
  for (int i = 0; i < n; i++)
    out[i] = (error_record_frame_t){ frames[i].func, i == n - 1 ? callee : js_mkundef(), frames[i].bc_off };
  return n;
}

static const char *error_record_file(ant_t *js, ant_value_t rec) {
  ant_value_t file = js_arr_get(js, rec, ERR_REC_FILE);
  if (vtype(file) == kTypeString) return js_getstr(js, file, NULL);
  
  sv_func_t *func = error_funcinfo_ptr(js_arr_get(js, rec, ERR_REC_SITE_FUNC));
  error_record_frame_t top[SV_JIT_INLINE_FRAMES_MAX + 1];
  if (!func && error_record_frame_count(js, rec) > 0 && error_record_frame(js, rec, 0, top) > 0) func = top[0].func;
  
  return func && func->debug->filename ? func->debug->filename : "<eval>";
}

static js_error_site_t error_record_site(ant_t *js, ant_value_t rec) {
  sv_func_t *func = error_funcinfo_ptr(js_arr_get(js, rec, ERR_REC_SITE_FUNC));
  if (func) {
    js_error_site_t es = js_error_site_from_source(
      func->debug->source, (ant_offset_t)func->debug->source_len, func->debug->filename,
      (ant_offset_t)error_record_int(js, rec, ERR_REC_SITE_POS),
      (ant_offset_t)error_record_int(js, rec, ERR_REC_SITE_SPAN)
    );
    es.func = func;
    es.line = (uint32_t)error_record_int(js, rec, ERR_REC_LINE);
    es.col = (uint32_t)error_record_int(js, rec, ERR_REC_COL);
    return es;
  }

  error_record_frame_t top[SV_JIT_INLINE_FRAMES_MAX + 1];
  bool derived = vtype(js_arr_get(js, rec, ERR_REC_LINE)) == kTypeUndefined;
  if (derived && error_record_frame_count(js, rec) > 0 && error_record_frame(js, rec, 0, top) > 0)
    return js_error_site_from_frame(top[0].func, top[0].bc_off);
  return (js_error_site_t){0};
}

typedef struct {
  const char *file;
  int line, col;
} error_record_tail_t;

static error_record_tail_t error_render_record_prefix(ant_t *js, ant_value_t rec, errbuf_t *eb, size_t *n) {
  error_record_tail_t tail = { error_record_file(js, rec), 1, 1 };
  ant_value_t prefix = js_arr_get(js, rec, ERR_REC_PREFIX);

  if (vtype(prefix) == kTypeString) {
    size_t len = 0;
    const char *text = js_getstr(js, prefix, &len);
    *n = append_errbuf_fmt(eb, *n, "%.*s", (int)len, text);
    tail.line = error_record_int(js, rec, ERR_REC_LINE);
    tail.col = error_record_int(js, rec, ERR_REC_COL);
    return tail;
  }

  js_error_site_t es = error_record_site(js, rec);
  js_error_render_site_t site;
  error_render_site_init(&es, &site);
  error_render_site_finish(&site);
  append_site_prefix(eb, n, tail.file, &site);
  tail.line = site.line;
  tail.col = site.col;
  return tail;
}

static int error_visit_record_frames(
  ant_t *js, ant_value_t rec, const char *fallback_file,
  js_vm_frame_visitor_fn visitor, void *ctx
) {
  int limit = error_record_int(js, rec, ERR_REC_LIMIT);
  int count = error_record_frame_count(js, rec), printed = 0;
  
  for (int k = 0; k < count && printed < limit; k++) {
    error_record_frame_t frames[SV_JIT_INLINE_FRAMES_MAX + 1];
    int n = error_record_frame(js, rec, k, frames);
    for (int i = 0; i < n && printed < limit; i++) {
      js_vm_frame_view_t view;
      error_frame_view(js, frames[i].func, frames[i].callee, frames[i].bc_off, fallback_file, &view);
      printed++;
      if (!visitor(js, &view, ctx)) return printed;
    }
  }
  
  return printed;
}

static void error_render_record_frames(ant_t *js, ant_value_t rec, errbuf_t *eb, size_t *n, error_record_tail_t tail) {
  error_frame_errbuf_ctx_t ctx = { eb, n, true };
  if (error_visit_record_frames(js, rec, tail.file, error_visit_frame_append_errbuf, &ctx) == 0 && error_record_int(js, rec, ERR_REC_LIMIT) > 0)
    append_error_frame(eb, n, NULL, tail.file, tail.line, tail.col, true);
}

char *js_error_render_pretty(ant_t *js, ant_value_t err, ant_value_t stack, size_t *out_len, size_t *header_at) {
  if (!is_object_type(err) || vtype(stack) != kTypeString) return NULL;
  ant_value_t rec = js_get_slot(err, SLOT_ERROR_STACK);
  if (vtype(rec) != kTypeArray || js_get_slot(err, SLOT_ERROR_STACK_TEXT) != stack) return NULL;

  errbuf_t eb = { malloc(4096), 4096 };
  if (!eb.buf) return NULL;
  eb.buf[0] = '\0';

  size_t n = 0;
  error_record_tail_t tail = error_render_record_prefix(js, rec, &eb, &n);
  *header_at = n;
  error_render_record_frames(js, rec, &eb, &n, tail);

  *out_len = n;
  return eb.buf;
}

static ant_value_t error_render_throw_text(ant_t *js, ant_value_t value, ant_value_t rec) {
  errbuf_t eb = { malloc(4096), 4096 };
  if (!eb.buf) return js_mkundef();
  eb.buf[0] = '\0';

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, value);
  GC_ROOT_PIN(js, rec);

  size_t n = 0;
  error_record_tail_t tail = error_render_record_prefix(js, rec, &eb, &n);
  n = append_error_value(&eb, js, n, value);
  error_render_record_frames(js, rec, &eb, &n, tail);

  ant_value_t text = js_mkstr(js, eb.buf, n);
  free(eb.buf);

  GC_ROOT_RESTORE(js, root_mark);
  return text;
}

ant_value_t Ant_Exception_Stack(ant_t *js, ant_value_t completion) {
  ant_object_t *record = exception_record(js, completion);
  if (!record) return js_mkundef();
  
  ant_value_t stack = record->u.exception.stack;
  ant_value_t value = record->u.exception.value;
  
  if (vtype(stack) == kTypeArray) {
    stack = error_render_throw_text(js, value, stack);
    record->u.exception.stack = vtype(stack) == kTypeString ? stack : js_mkundef();
    gc_write_barrier(js, record, record->u.exception.stack);
    return record->u.exception.stack;
  }
  
  if (vtype(stack) == kTypeString || !is_object_type(value)) return stack;
  if (vtype(js_get_slot(value, SLOT_ERROR_STACK)) != kTypeArray) return stack;
  
  stack = js_error_stack_value(js, value);
  return vtype(stack) == kTypeString ? stack : js_mkundef();
}

typedef struct {
  errbuf_t *eb;
  size_t *n;
} error_plain_frame_ctx_t;

static bool error_visit_frame_append_plain(ant_t *js, const js_vm_frame_view_t *view, void *ctx) {
  error_plain_frame_ctx_t *c = (error_plain_frame_ctx_t *)ctx;
  size_t start = *c->n;
  *c->n = append_errbuf_fmt(c->eb, *c->n, "\n    at %s (%s:%d:%d)", view->name, view->file, view->line, view->col);
  return *c->n > start;
}

static ant_value_t error_header_part(ant_t *js, ant_value_t obj, const char *key, size_t key_len, const char *fallback) {
  ant_value_t v = js_getprop_fallback_len(js, obj, key, key_len);
  if (is_err(v)) return v;
  if (vtype(v) == kTypeUndefined) return js_mkstr(js, fallback, strlen(fallback));
  return vtype(v) == kTypeString ? v : js_tostring_val(js, v);
}

ant_value_t js_error_header_text(ant_t *js, ant_value_t obj) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, obj);

  ant_value_t name = error_header_part(js, obj, "name", 4, "Error");
  GC_ROOT_PIN(js, name);
  ant_value_t msg = is_err(name) ? name : error_header_part(js, obj, "message", 7, "");
  GC_ROOT_PIN(js, msg);
  
  if (is_err(msg)) {
    GC_ROOT_RESTORE(js, root_mark);
    return msg;
  }

  size_t name_len = 0, msg_len = 0;
  const char *name_text = js_getstr(js, name, &name_len);
  const char *msg_text = js_getstr(js, msg, &msg_len);

  ant_value_t text = name_len == 0 ? msg : name;
  if (name_len > 0 && msg_len > 0) {
    errbuf_t eb = { malloc(name_len + msg_len + 3), name_len + msg_len + 3 };
    if (eb.buf) {
      size_t n = append_errbuf_fmt(&eb, 0, "%.*s: %.*s", (int)name_len, name_text, (int)msg_len, msg_text);
      text = js_mkstr(js, eb.buf, n);
      free(eb.buf);
    }
  }

  GC_ROOT_RESTORE(js, root_mark);
  return text;
}

static ant_value_t error_stack_materialize(ant_t *js, ant_value_t obj, ant_value_t rec) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, rec);

  ant_value_t header = js_error_header_text(js, obj);
  GC_ROOT_PIN(js, header);
  
  errbuf_t eb = { malloc(1024), 1024 };
  if (is_err(header) || !eb.buf) {
    free(eb.buf);
    GC_ROOT_RESTORE(js, root_mark);
    return is_err(header) ? header : js_mkerr(js, "out of memory");
  }

  size_t len = 0;
  const char *text = js_getstr(js, header, &len);
  size_t n = append_errbuf_fmt(&eb, 0, "%.*s", (int)len, text);

  const char *file = error_record_file(js, rec);
  error_plain_frame_ctx_t ctx = { &eb, &n };
  if (error_visit_record_frames(js, rec, file, error_visit_frame_append_plain, &ctx) == 0 && error_record_int(js, rec, ERR_REC_LIMIT) > 0) {
    js_error_site_t es = error_record_site(js, rec);
    js_error_render_site_t site;
    error_render_site_init(&es, &site);
    if (!es.valid && vtype(js_arr_get(js, rec, ERR_REC_LINE)) == kTypeNumber) {
      site.line = error_record_int(js, rec, ERR_REC_LINE);
      site.col = error_record_int(js, rec, ERR_REC_COL);
    }
    n = append_errbuf_fmt(&eb, n, "\n    at %s:%d:%d", file, site.line, site.col);
  }

  ant_value_t stack = js_mkstr(js, eb.buf, n);
  free(eb.buf);

  GC_ROOT_RESTORE(js, root_mark);
  return stack;
}

ant_value_t js_error_stack_value(ant_t *js, ant_value_t obj) {
  ant_value_t text = js_mkundef(), rec = js_mkundef();
  for (int depth = 0; is_object_type(obj) && depth < 64; depth++) {
    text = js_get_slot(obj, SLOT_ERROR_STACK_TEXT);
    rec = js_get_slot(obj, SLOT_ERROR_STACK);
    if (vtype(rec) == kTypeArray || vtype(text) != kTypeUndefined) break;
    obj = js_get_proto(js, obj);
  }

  if (vtype(rec) != kTypeArray || vtype(text) != kTypeUndefined) return text;

  js_set_slot(obj, SLOT_ERROR_STACK_TEXT, js_mkstr(js, "", 0));
  ant_value_t stack = error_stack_materialize(js, obj, rec);
  js_set_slot_wb(js, obj, SLOT_ERROR_STACK_TEXT, is_err(stack) ? js_mkundef() : stack);
  
  return stack;
}

static ant_value_t builtin_error_stack_get(ant_params_t) {
  return js_error_stack_value(js, js_getthis(js));
}

static ant_value_t builtin_error_stack_set(ant_params_t) {
  ant_value_t obj = js_getthis(js);
  if (!is_object_type(obj)) return js_mkundef();
  js_set_slot(obj, SLOT_ERROR_STACK, js_mkundef());
  js_set_slot_wb(js, obj, SLOT_ERROR_STACK_TEXT, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

void js_error_define_stack(ant_t *js, ant_value_t obj, ant_value_t record, ant_value_t text) {
  js_reserve_slots(obj, 3);
  js_set_slot_wb(js, obj, SLOT_ERROR_STACK, record);
  if (vtype(text) != kTypeUndefined || vtype(js_get_slot(obj, SLOT_ERROR_STACK_TEXT)) != kTypeUndefined)
    js_set_slot_wb(js, obj, SLOT_ERROR_STACK_TEXT, text);
  js_define_accessor_desc(
    js, js_as_obj(obj), "stack", 5,
    js_mkfun(builtin_error_stack_get),
    js_mkfun(builtin_error_stack_set), JS_DESC_C
  );
}

static bool error_has_captured_stack(ant_value_t obj) {
  return 
    vtype(js_get_slot(obj, SLOT_ERROR_STACK)) == kTypeArray ||
    vtype(js_get_slot(obj, SLOT_ERROR_STACK_TEXT)) != kTypeUndefined;
}

static void error_capture_stack_at(ant_t *js, ant_value_t err_obj, const js_error_site_t *site) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, err_obj);
  js_error_define_stack(js, err_obj, error_capture_record(js, site), js_mkundef());
  GC_ROOT_RESTORE(js, root_mark);
}

void js_capture_stack(ant_t *js, ant_value_t err_obj) {
  error_capture_stack_at(js, err_obj, NULL);
}

js_err_type_t get_error_type(ant_t *js) {
  if (!Ant_Exception_Pending(js)) return JS_ERR_GENERIC;
  ant_value_t err_type = js_get_slot(Ant_Exception_Value(js, Ant_Exception_Peek(js)), SLOT_ERR_TYPE);
  if (vtype(err_type) != kTypeNumber) return JS_ERR_GENERIC;
  return (js_err_type_t)((int)js_getnum(err_type) & ~JS_ERR_NO_STACK);
}

static ant_value_t make_error_value(
  ant_t *js, const js_error_site_t *site, js_err_type_t err_type,
  ant_value_t props, const char *error_msg
) {
  bool no_stack = (err_type & JS_ERR_NO_STACK) != 0;
  js_err_type_t base_type = (js_err_type_t)(err_type & ~JS_ERR_NO_STACK);

  const char *err_name = get_error_type_name(base_type);
  size_t err_name_len = strlen(err_name);
  size_t msg_len = strlen(error_msg);

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, props);

  ant_value_t err_obj = js_mkobj(js);
  GC_ROOT_PIN(js, err_obj);

  if (is_err(err_obj)) {
    GC_ROOT_RESTORE(js, mark);
    return err_obj;
  }

  ant_value_t message = js_mkstr(js, error_msg, msg_len);
  GC_ROOT_PIN(js, message);
  mkprop_interned(js, err_obj, js->intern.message, message, ANT_PROP_ATTR_WRITABLE | ANT_PROP_ATTR_CONFIGURABLE);
  js_set_slot(err_obj, SLOT_ERR_TYPE, js_mknum((double)err_type));

  int props_type = vtype(props);
  if ((T_FLAG_FIND(props_type) & T_SPECIAL_OBJECT_MASK) != 0)
    js_merge_obj(js, err_obj, props);

  ant_value_t proto = js_get_ctor_proto(js, err_name, err_name_len);
  int proto_type = vtype(proto);
  if ((T_FLAG_FIND(proto_type) & T_SPECIAL_OBJECT_MASK) != 0)
    js_set_proto_init(err_obj, proto);
  else {
    js_set(js, err_obj, "name", js_mkstr(js, err_name, err_name_len));
    js_set_descriptor(js, err_obj, "name", 4, JS_DESC_W | JS_DESC_C);
  }

  if (!no_stack) error_capture_stack_at(js, err_obj, site);
  GC_ROOT_RESTORE(js, mark);

  return err_obj;
}

__attribute__((format(printf, 5, 0)))
static ant_value_t CreateFormattedErrorValue(
  ant_t *js, const js_error_site_t *site, js_err_type_t err_type,
  ant_value_t props, const char *fmt, va_list args
) {
  char local[256];
  char *message = local;

  va_list copy;

  va_copy(copy, args);
  int length = vsnprintf(local, sizeof(local), fmt, copy);
  va_end(copy);

  if (length < 0) goto format_failed;

  if ((size_t)length >= sizeof(local)) {
    size_t capacity = (size_t)length + 1;
    message = malloc(capacity);

    if (!message) {
      ant_value_t failure = is_err(js->exception_oom) ? js->exception_oom : mkval(kTypeError, 0);
      Ant_Exception_Set(js, failure);
      return failure;
    }

    va_copy(copy, args);
    int written = vsnprintf(message, capacity, fmt, copy);
    va_end(copy);

    if (written < 0 || (size_t)written >= capacity) {
      free(message);
      goto format_failed;
    }
  }

  ant_value_t value = make_error_value(js, site, err_type, props, message);
  if (message != local) free(message);
  return value;

format_failed:
  return js_throw(js, Ant_Error_Create(js, JS_ERR_INTERNAL | JS_ERR_NO_STACK, "failed to format error message"));
}

static ant_value_t raise_created_error(ant_t *js, ant_value_t value) {
  if (is_err(value)) return value;
  return Ant_Exception_Raise(js, value, js_mkundef());
}

__attribute__((format(printf, 4, 5)))
ant_value_t js_create_error(ant_t *js, js_err_type_t err_type, ant_value_t props, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  ant_value_t value = CreateFormattedErrorValue(js, NULL, err_type, props, fmt, ap);
  va_end(ap);
  return raise_created_error(js, value);
}

__attribute__((format(printf, 5, 6)))
ant_value_t js_create_error_at(
  ant_t *js, const js_error_site_t *site, js_err_type_t err_type,
  ant_value_t props, const char *fmt, ...
) {
  va_list ap;
  va_start(ap, fmt);
  ant_value_t value = CreateFormattedErrorValue(js, site, err_type, props, fmt, ap);
  va_end(ap);
  return raise_created_error(js, value);
}

ant_value_t Ant_Error_Create(ant_t *js, js_err_type_t err_type, const char *message) {
  return make_error_value(js, NULL, err_type, js_mkundef(), message);
}

__attribute__((format(printf, 3, 4)))
ant_value_t Ant_Error_CreateFormatted(ant_t *js, js_err_type_t err_type, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  ant_value_t value = CreateFormattedErrorValue(js, NULL, err_type, js_mkundef(), fmt, ap);
  va_end(ap);
  return value;
}

ant_value_t js_throw(ant_t *js, ant_value_t value) {
  if (is_err(value)) {
    Ant_Exception_Set(js, value);
    return value;
  }

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, value);

  ant_value_t stack = js_mkundef();
  GC_ROOT_PIN(js, stack);

  bool no_stack = false;
  if (vtype(value) == kTypeObject) {
    ant_value_t kind = js_get_slot(value, SLOT_ERR_TYPE);
    if (error_has_captured_stack(value)) {
      no_stack = true;
      stack = js_get_slot(value, SLOT_ERROR_STACK_TEXT);
    } else if (vtype(kind) == kTypeNumber && ((int)js_getnum(kind) & JS_ERR_NO_STACK)) no_stack = true;
    else no_stack = js_try_get_own_data_prop(js, value, "stack", 5, &stack) && vtype(stack) == kTypeString;
    if (vtype(stack) != kTypeString) stack = js_mkundef();
  }

  if (!no_stack) stack = error_capture_record(js, NULL);
  ant_value_t result = Ant_Exception_Raise(js, value, stack);

  GC_ROOT_RESTORE(js, mark);
  return result;
}

enum { 
  CS_FILE = 0,
  CS_LINE,
  CS_COL,
  CS_NAME
};

static ant_value_t callsite_field(ant_t *js, int field) {
  ant_value_t data = js_get_slot(js->this_val, SLOT_DATA);
  if (vtype(data) != kTypeArray) return js_mkundef();
  return js_arr_get(js, data, field);
}

static ant_value_t callsite_getFileName(ant_params_t) { return callsite_field(js, CS_FILE); }
static ant_value_t callsite_getLineNumber(ant_params_t) { return callsite_field(js, CS_LINE); }
static ant_value_t callsite_getColumnNumber(ant_params_t) { return callsite_field(js, CS_COL); }
static ant_value_t callsite_getFunctionName(ant_params_t) { return callsite_field(js, CS_NAME); }

static ant_value_t callsite_getTypeName(ant_params_t) { return js_mknull(); }
static ant_value_t callsite_getMethodName(ant_params_t) { return callsite_field(js, CS_NAME); }

static ant_value_t callsite_isNative(ant_params_t) { return js_false; }
static ant_value_t callsite_isToplevel(ant_params_t) { return js_false; }
static ant_value_t callsite_isEval(ant_params_t) { return js_false; }
static ant_value_t callsite_isConstructor(ant_params_t) { return js_false; }
static ant_value_t callsite_getEvalOrigin(ant_params_t) { return js_mkundef(); }
static ant_value_t callsite_getThis(ant_params_t) { return js_mkundef(); }

static ant_value_t callsite_toString(ant_params_t) {
  ant_value_t name = callsite_field(js, CS_NAME);
  ant_value_t file = callsite_field(js, CS_FILE);
  ant_value_t line = callsite_field(js, CS_LINE);
  ant_value_t col  = callsite_field(js, CS_COL);

  const char *n = js_str(js, name);
  const char *f = js_str(js, file);
  int l = vtype(line) == kTypeNumber ? (int)js_getnum(line) : 0;
  int c = vtype(col)  == kTypeNumber ? (int)js_getnum(col)  : 0;

  char buf[512];
  int len = snprintf(buf, sizeof(buf), "%s (%s:%d:%d)", n, f, l, c);
  if (len < 0) len = 0;
  return js_mkstr(js, buf, (size_t)len);
}

typedef struct {
  ant_t *js;
  ant_value_t arr;
  ant_value_t proto;
} callsite_build_ctx_t;

static bool callsite_visit_frame(ant_t *js, const js_vm_frame_view_t *view, void *ctx) {
  callsite_build_ctx_t *c = (callsite_build_ctx_t *)ctx;

  ant_value_t data = js_mkarr(js);
  js_arr_push(js, data, js_mkstr(js, view->file, strlen(view->file)));
  js_arr_push(js, data, js_mknum((double)view->line));
  js_arr_push(js, data, js_mknum((double)view->col));
  js_arr_push(js, data, js_mkstr(js, view->name, strlen(view->name)));

  ant_value_t site = js_mkobj(js);
  js_set_proto_init(site, c->proto);
  js_set_slot(site, SLOT_DATA, data);

  js_arr_push(js, c->arr, site);
  return true;
}

ant_value_t js_build_callsite_array(ant_t *js) {
  ant_value_t proto = js_mkobj(js);
  
  js_set(js, proto, "getFileName",     js_mkfun(callsite_getFileName));
  js_set(js, proto, "getLineNumber",   js_mkfun(callsite_getLineNumber));
  js_set(js, proto, "getColumnNumber", js_mkfun(callsite_getColumnNumber));
  js_set(js, proto, "getFunctionName", js_mkfun(callsite_getFunctionName));
  js_set(js, proto, "getTypeName",     js_mkfun(callsite_getTypeName));
  js_set(js, proto, "getMethodName",   js_mkfun(callsite_getMethodName));
  js_set(js, proto, "isNative",        js_mkfun(callsite_isNative));
  js_set(js, proto, "isToplevel",      js_mkfun(callsite_isToplevel));
  js_set(js, proto, "isEval",          js_mkfun(callsite_isEval));
  js_set(js, proto, "isConstructor",   js_mkfun(callsite_isConstructor));
  js_set(js, proto, "getEvalOrigin",   js_mkfun(callsite_getEvalOrigin));
  js_set(js, proto, "getThis",         js_mkfun(callsite_getThis));
  js_set(js, proto, "toString",        js_mkfun(callsite_toString));

  ant_value_t arr = js_mkarr(js);
  callsite_build_ctx_t ctx = { js, arr, proto };

  const char *file = js->filename ? js->filename : "<eval>";

  error_visit_live_frames(js, error_stack_trace_limit(js), file, callsite_visit_frame, &ctx);
  return arr;
}

void js_print_stack_trace_vm(ant_t *js, FILE *stream) {
  if (!stream) return;
  
  errbuf_t eb = { malloc(4096), 4096 };
  if (!eb.buf) return;

  size_t n = 0;
  const char *file = js->filename ? js->filename : "<unknown>";
  error_frame_errbuf_ctx_t ctx = { &eb, &n, true };
  
  error_visit_live_frames(js, -1, file, error_visit_frame_append_errbuf, &ctx);
  if (n > 0) {
    fwrite(eb.buf + 1, 1, n - 1, stream);
    fputc('\n', stream);
  }
  
  free(eb.buf);
}
