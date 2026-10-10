#include "error_render.h"
#include "internal.h"
#include "modules/io.h"
#include "modules/process.h"
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

bool errbuf_init(errbuf_t *eb, size_t size) {
  eb->buf = malloc(size);
  eb->size = eb->buf ? size : 0;
  if (eb->buf) eb->buf[0] = '\0';
  return eb->buf != NULL;
}

ant_value_t errbuf_take_string(ant_t *js, errbuf_t *eb, size_t len) {
  ant_value_t text = js_mkstr(js, eb->buf, len);
  free(eb->buf);
  eb->buf = NULL;
  return text;
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
size_t errbuf_appendf(errbuf_t *eb, size_t used, const char *fmt, ...) {
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

void error_line_col(const char *code, ant_offset_t code_len, ant_offset_t pos, int *out_line, int *out_col) {
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

size_t error_render_value(errbuf_t *eb, ant_t *js, size_t used, ant_value_t value) {
  if (vtype(value) != kTypeObject) {
    ant_offset_t len = 0;
    const char *msg = vtype(value) == kTypeString ? (const char *)(uintptr_t)vstr(js, value, &len) : js_str(js, value);
    if (vtype(value) != kTypeString) len = msg ? (ant_offset_t)strlen(msg) : 0;
    return len > 0
      ? errbuf_appendf(eb, used, ERR_FMT, 5, "Error", (int)len, msg)
      : errbuf_appendf(eb, used, ERR_NAME_ONLY, 5, "Error");
  }

  GC_ROOT_SAVE(root_mark, js);
  ant_value_t name, message;
  
  if (is_err(js_error_header_parts(js, value, &name, &message))) {
    js_take_thrown(js, js_mkundef());
    name = js_mkstr(js, "Error", 5);
    message = js_mkstr(js, "", 0);
  }
  
  GC_ROOT_PIN(js, name);
  GC_ROOT_PIN(js, message);

  size_t name_len = 0, msg_len = 0;
  const char *name_text = js_getstr(js, name, &name_len);
  const char *msg_text = js_getstr(js, message, &msg_len);

  if (name_len && msg_len) used = errbuf_appendf(eb, used, ERR_FMT, (int)name_len, name_text, (int)msg_len, msg_text);
  else if (name_len) used = errbuf_appendf(eb, used, ERR_NAME_ONLY, (int)name_len, name_text);
  else used = errbuf_appendf(eb, used, "\x1b[1m%.*s\x1b[0m", (int)msg_len, msg_text);

  GC_ROOT_RESTORE(js, root_mark);
  return used;
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
  
  *n = errbuf_appendf(eb, *n, "%s", red);
  remaining = eb->size - *n;
  for (int i = 0; i < span_cols && remaining > 1; i++) {
    eb->buf[(*n)++] = '^';
    remaining--;
  }
  
  *n = errbuf_appendf(eb, *n, "%s", reset);
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

bool error_render_frame(
  errbuf_t *eb, size_t *n, const char *name, const char *file, int line, int col, error_frame_style_t style
) {
  bool color = style == ERROR_FRAME_PRETTY;
  const char *indent = style == ERROR_FRAME_PLAIN ? "    " : "  ";
  const char *dim = color ? C_DIM : "";
  const char *cyan = color ? C_CYAN : "";
  const char *yellow = color ? C_YELLOW : "";
  const char *reset = color ? C_RESET : "";
  
  size_t prefix = color && !io_no_color ? error_frame_path_prefix(file) : 0;
  size_t start = *n;

  *n = errbuf_appendf(eb, *n,
    "\n%s%sat%s %s%s%s%s"
    "%s%s%.*s%s%s%s%s"
    "%s:%s%s%d%s%s:%s%d%s"
    "%s%s%s",
    indent, dim, reset, name ? name : "", name ? " " : "", dim, name ? "(" : "",
    cyan, dim, (int)prefix, file, reset, cyan, file + prefix, reset,
    dim, reset, yellow, line, reset, dim, yellow, col, reset,
    dim, name ? ")" : "", reset
  );
  
  return *n > start;
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
      *n = errbuf_appendf(eb, *n, "\n%*d | %s", gutter_w, line_no, rendered);
    } else {
      int shown = was_clipped ? src_cols_limit : line_len;
      *n = errbuf_appendf(eb, *n, "\n%*d | %.*s", gutter_w, line_no, shown, src + ls);
    }

    if (was_clipped) *n = errbuf_appendf(eb, *n, "...");
    if (ls == err_line_start) {
      *n = errbuf_appendf(eb, *n, "\n%*s   ", gutter_w, "");
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

void error_render_site_init(const js_error_site_t *es, js_error_render_site_t *site) {
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
  } else error_line_col(site->src, site->src_len, site->src_pos, &site->line, &site->col);
}

void error_render_site_finish(js_error_render_site_t *site) {
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

const char *error_site_file(ant_t *js, const js_error_site_t *es) {
  if (es->valid && es->filename) return es->filename;
  return js->filename ? js->filename : "<eval>";
}

void error_render_site_prefix(errbuf_t *eb, size_t *n, const char *file, const js_error_render_site_t *site) {
  *n = errbuf_appendf(eb, *n, "%s:%d:%d", file, site->line, site->col);

  bool rendered_context = append_error_context(
    eb, n, site->src, site->src_len, site->src_pos,
    site->line, site->error_col, site->error_span_cols
  );

  if (!rendered_context && site->error_line[0]) {
    *n = errbuf_appendf(eb, *n, "\n%s\n", site->error_line);
    append_error_caret(eb, n, site->error_col, site->error_span_cols);
  }

  *n = errbuf_appendf(eb, *n, "\n");
}
