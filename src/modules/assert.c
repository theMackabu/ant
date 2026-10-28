#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "cfunc.h"
#include "ascii.h"
#include "errors.h"
#include "error_render.h"
#include "internal.h"
#include "gc/roots.h"

#include "esm/library.h"
#include "modules/assert.h"
#include "modules/buffer.h"
#include "modules/collections.h"
#include "modules/date.h"
#include "silver/engine.h"
#include "silver/call.h"

typedef struct {
  const char *op;
  ant_value_t actual;
  ant_value_t expected;
} assertion_info_t;

static bool assert_is_error_instance(ant_t *js, ant_value_t value) {
  if (!is_object_type(value)) return false;
  ant_value_t is = do_instanceof(js, value, js->builtins.error_ctor);
  return !is_err(is) && js_truthy(js, is);
}

static ant_value_t assertion_error_new(ant_t *js, ant_value_t message, bool generated, assertion_info_t info) {
  if (is_err(message)) return message;
  
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, message);
  GC_ROOT_PIN(js, info.actual);
  GC_ROOT_PIN(js, info.expected);

  ant_value_t props = Ant_Object_CreateBare(js);
  GC_ROOT_PIN(js, props);
  js_set(js, props, "generatedMessage", js_bool(generated));
  js_set(js, props, "code", js_mkstr(js, "ERR_ASSERTION", 13));
  js_set(js, props, "actual", info.actual);
  js_set(js, props, "expected", info.expected);
  js_set(js, props, "operator", info.op ? js_mkstr(js, info.op, strlen(info.op)) : js_mkundef());

  ant_value_t err = Ant_Error_CreateWithProps(js, JS_ERR_GENERIC, props, js_getstr(js, message, NULL));
  if (vtype(err) == kTypeObject && is_object_type(js->builtins.assertion_error_proto))
    js_set_proto_init(err, js->builtins.assertion_error_proto);

  GC_ROOT_RESTORE(js, root_mark);
  return err;
}

static ant_value_t assertion_throw(ant_t *js, ant_value_t message, bool generated, assertion_info_t info) {
  ant_value_t err = assertion_error_new(js, message, generated, info);
  return is_err(err) ? err : js_throw(js, err);
}

static ant_value_t assertion_throw_text(ant_t *js, const char *text, bool generated, assertion_info_t info) {
  return assertion_throw(js, js_mkstr(js, text, strlen(text)), generated, info);
}

typedef struct {
  const char *ptr;
  size_t len;
} assert_line_t;

typedef struct {
  char *text;
  size_t len;
  assert_line_t *lines;
  int count;
} assert_inspected_t;

typedef enum {
  ASSERT_DIFF_DELETE = -1,
  ASSERT_DIFF_NOP    = 0,
  ASSERT_DIFF_INSERT = 1,
} assert_diff_op_t;

typedef struct {
  assert_diff_op_t op;
  assert_line_t line;
} assert_diff_t;

static constexpr int ASSERT_NOP_LINES_TO_COLLAPSE = 5;
static constexpr size_t ASSERT_MAX_SHORT_STRING = 12;
static constexpr size_t ASSERT_MAX_LONG_STRING = 512;
static constexpr size_t ASSERT_MYERS_TRACE_LIMIT = (size_t)64 << 20;

static bool assert_inspect(ant_t *js, ant_value_t value, bool expand, assert_inspected_t *out) {
  char stack[256];
  uint8_t flags = expand ? ANT_INSPECT_EXPAND | ANT_INSPECT_SORTED : 0;
  js_cstr_t cstr = js_inspect_cstr_opts(js, value, stack, sizeof(stack), MAX_STRINGIFY_DEPTH, flags);
  
  *out = (assert_inspected_t){ 0 };
  out->len = strlen(cstr.ptr);
  out->text = cstr.needs_free ? (char *)cstr.ptr : strndup(cstr.ptr, out->len);
  if (!out->text) return false;

  int count = 1;
  for (size_t i = 0; i < out->len; i++) count += out->text[i] == '\n';
  out->lines = malloc((size_t)count * sizeof(*out->lines));
  if (!out->lines) return false;

  const char *line = out->text;
  for (size_t i = 0; i <= out->len; i++) {
    if (i < out->len && out->text[i] != '\n') continue;
    out->lines[out->count++] = (assert_line_t){ line, (size_t)(out->text + i - line) };
    line = out->text + i + 1;
  }
  
  return true;
}

static void assert_inspected_free(assert_inspected_t *value) {
  free(value->text);
  free(value->lines);
}

static size_t assert_text_length(const char *text, size_t len) {
  size_t n = 0;
  for (size_t i = 0; i < len; i++) n += ((unsigned char)text[i] & 0xC0) != 0x80;
  return n;
}

static size_t assert_text_cut(const char *text, size_t len, size_t limit) {
  if (len <= limit) return len;
  while (limit > 0 && ((unsigned char)text[limit] & 0xC0) == 0x80) limit--;
  return limit;
}

static bool assert_is_object_value(ant_value_t value) {
  return is_object_type(value) && !is_callable(value);
}

static bool assert_is_zero(ant_value_t value) {
  return vtype(value) == kTypeNumber && js_getnum(value) == 0;
}

static const char *assert_readable_operator(const char *op) {
  static const struct { const char *op; const char *text; } table[] = {
    { "deepStrictEqual",      "Expected values to be strictly deep-equal:" },
    { "strictEqual",          "Expected values to be strictly equal:" },
    { "strictEqualObject",    "Expected \"actual\" to be reference-equal to \"expected\":" },
    { "deepEqual",            "Expected values to be loosely deep-equal:" },
    { "notDeepStrictEqual",   "Expected \"actual\" not to be strictly deep-equal to:" },
    { "notStrictEqual",       "Expected \"actual\" to be strictly unequal to:" },
    { "notStrictEqualObject", "Expected \"actual\" not to be reference-equal to \"expected\":" },
    { "notDeepEqual",         "Expected \"actual\" not to be loosely deep-equal to:" },
    { "notIdentical",         "Values have same structure but are not reference-equal:" },
    { "notDeepEqualUnequal",  "Expected values not to be loosely deep-equal:" },
  };
  
  for (size_t i = 0; op && i < sizeof(table) / sizeof(table[0]); i++)
    if (strcmp(table[i].op, op) == 0) return table[i].text;
  return NULL;
}

static bool assert_op_is(const char *op, const char *name) {
  return op && strcmp(op, name) == 0;
}

static bool assert_lines_equal(assert_line_t a, assert_line_t b, bool comma) {
  if (a.len == b.len && memcmp(a.ptr, b.ptr, a.len) == 0) return true;
  if (!comma) return false;
  if (a.len + 1 == b.len) return b.ptr[a.len] == ',' && memcmp(a.ptr, b.ptr, a.len) == 0;
  if (b.len + 1 == a.len) return a.ptr[b.len] == ',' && memcmp(a.ptr, b.ptr, b.len) == 0;
  return false;
}

static int assert_myers_diag(const int32_t *v, int k, int d, int max) {
  return k == -d || (k != d && v[k + max - 1] < v[k + max + 1]) ? k + 1 : k - 1;
}

static assert_diff_t *assert_myers_diff(
  const assert_inspected_t *a, const assert_inspected_t *b, bool comma, int *out_count
) {
  int max = a->count + b->count;
  size_t width = (size_t)max * 2 + 1;
  int32_t *v = calloc(width, sizeof(*v));
  int32_t *trace = NULL;
  int levels = 0;
  assert_diff_t *result = malloc((size_t)max * sizeof(*result));
  bool found = false;

  for (int d = 0; v && result && !found && d <= max; d++) {
    if ((size_t)(levels + 1) * width > ASSERT_MYERS_TRACE_LIMIT) break;
    int32_t *next = realloc(trace, (size_t)(levels + 1) * width * sizeof(*trace));
    if (!next) break;
    trace = next;
    memcpy(trace + (size_t)levels++ * width, v, width * sizeof(*v));

    for (int k = -d; k <= d; k += 2) {
      int prev = assert_myers_diag(v, k, d, max);
      int x = prev == k + 1 ? v[k + max + 1] : v[k + max - 1] + 1;
      int y = x - k;
      while (x < a->count && y < b->count && assert_lines_equal(a->lines[x], b->lines[y], comma)) x++, y++;
      v[k + max] = x;
      if (x >= a->count && y >= b->count) { found = true; break; }
    }
  }

  int n = 0;
  if (found) {
    int x = a->count, y = b->count;
    for (int d = levels - 1; d >= 0; d--) {
      const int32_t *tv = trace + (size_t)d * width;
      int pk = assert_myers_diag(tv, x - y, d, max);
      int px = tv[pk + max], py = px - pk;
      
      for (; x > px && y > py; x--, y--) {
        assert_line_t line = a->lines[x - 1];
        if (comma && (line.len == 0 || line.ptr[line.len - 1] != ',')) line = b->lines[y - 1];
        result[n++] = (assert_diff_t){ ASSERT_DIFF_NOP, line };
      }
      
      if (d == 0) continue;
      if (x > px) result[n++] = (assert_diff_t){ ASSERT_DIFF_INSERT, a->lines[--x] };
      else result[n++] = (assert_diff_t){ ASSERT_DIFF_DELETE, b->lines[--y] };
    }
  } else if (result) {
    for (int i = b->count - 1; i >= 0; i--) result[n++] = (assert_diff_t){ ASSERT_DIFF_DELETE, b->lines[i] };
    for (int i = a->count - 1; i >= 0; i--) result[n++] = (assert_diff_t){ ASSERT_DIFF_INSERT, a->lines[i] };
  }

  free(v);
  free(trace);
  *out_count = n;
  return result;
}

static size_t assert_append_line(errbuf_t *eb, size_t n, const char *prefix, assert_line_t line) {
  return errbuf_appendf(eb, n, "%s%.*s\n", prefix, (int)line.len, line.ptr);
}

static size_t assert_print_myers(errbuf_t *eb, size_t n, const assert_diff_t *diff, int count, bool *skipped) {
  size_t start = n = errbuf_appendf(eb, n, "\n");
  int nops = 0;

  for (int i = count - 1; i >= 0; i--) {
    if (i < count - 1 && diff[i + 1].op == ASSERT_DIFF_NOP && diff[i].op != ASSERT_DIFF_NOP) {
      if (nops == ASSERT_NOP_LINES_TO_COLLAPSE + 1) n = assert_append_line(eb, n, "  ", diff[i + 1].line);
      else if (nops == ASSERT_NOP_LINES_TO_COLLAPSE + 2) {
        n = assert_append_line(eb, n, "  ", diff[i + 2].line);
        n = assert_append_line(eb, n, "  ", diff[i + 1].line);
      } else if (nops >= ASSERT_NOP_LINES_TO_COLLAPSE + 3) {
        n = errbuf_appendf(eb, n, "...\n");
        n = assert_append_line(eb, n, "  ", diff[i + 1].line);
        *skipped = true;
      }
      nops = 0;
    }

    if (diff[i].op == ASSERT_DIFF_INSERT) n = assert_append_line(eb, n, "+ ", diff[i].line);
    else if (diff[i].op == ASSERT_DIFF_DELETE) n = assert_append_line(eb, n, "- ", diff[i].line);
    else if (nops++ < ASSERT_NOP_LINES_TO_COLLAPSE) n = assert_append_line(eb, n, "  ", diff[i].line);
  }

  while (n > start && ant_ascii_is_space((unsigned char)eb->buf[n - 1])) n--;
  eb->buf[n] = '\0';
  return n;
}

static size_t assert_stacked_diff(errbuf_t *eb, size_t n, assert_line_t actual, assert_line_t expected, bool strings) {
  n = errbuf_appendf(eb, n, "\n+ %.*s\n- %.*s", (int)actual.len, actual.ptr, (int)expected.len, expected.ptr);
  if (!strings || assert_text_length(actual.ptr, actual.len) + assert_text_length(expected.ptr, expected.len) > 80) return n;

  size_t i = 0;
  while (i < actual.len && i < expected.len && actual.ptr[i] == expected.ptr[i]) i++;
  if (i >= actual.len || i < 3) return n;
  return errbuf_appendf(eb, n, "\n%*s^", (int)(assert_text_length(actual.ptr, i) + 2), "");
}

static size_t assert_err_diff(
  errbuf_t *eb, size_t n, ant_value_t actual, ant_value_t expected, const char *op, const char *custom,
  const assert_inspected_t *ia, const assert_inspected_t *ie
) {
  if (assert_op_is(op, "strictEqual") && (
    (assert_is_object_value(actual) && assert_is_object_value(expected)) ||
    (is_callable(actual) && is_callable(expected))
  )) op = "strictEqualObject";

  const char *header = "+ actual - expected";
  bool skipped = false;
  errbuf_t body;
  if (!errbuf_init(&body, 256)) return n;
  size_t m = 0;

  bool simple = ia->count == 1 && ie->count == 1 && (!assert_is_object_value(actual) || !assert_is_object_value(expected));
  if (simple) {
    size_t len = assert_text_length(ia->text, ia->len) + assert_text_length(ie->text, ie->len);
    if (vtype(actual) == kTypeString) len -= 2;
    if (vtype(expected) == kTypeString) len -= 2;
    
    if (len <= ASSERT_MAX_SHORT_STRING && !(assert_is_zero(actual) && assert_is_zero(expected))) {
      m = errbuf_appendf(&body, m, "%s !== %s", ia->text, ie->text);
      header = "";
    } else m = assert_stacked_diff(
      &body, m, ia->lines[0], ie->lines[0], 
      vtype(actual) == kTypeString && vtype(expected) == kTypeString
    );
  } else if (ia->len == ie->len && memcmp(ia->text, ie->text, ia->len) == 0) {
    op = "notIdentical";
    header = "";
    if (ia->count > 50) {
      m = errbuf_appendf(&body, m, "%.*s\n...}", (int)(ia->lines[49].ptr + ia->lines[49].len - ia->text), ia->text);
      skipped = true;
    } else m = errbuf_appendf(&body, m, "%s", ia->text);
  } else {
    int count = 0;
    assert_diff_t *diff = assert_myers_diff(ia, ie, assert_is_object_value(actual), &count);
    if (diff) m = assert_print_myers(&body, m, diff, count, &skipped);
    free(diff);
  }

  n = errbuf_appendf(
    eb, n, "%s\n%s%s\n%.*s\n", custom ? custom : assert_readable_operator(op), header,
    skipped ? "\n... Skipped lines" : "", (int)m, body.buf
  );
  
  free(body.buf);
  return n;
}

static size_t assert_not_equal_message(
  errbuf_t *eb, size_t n, ant_value_t actual, const char *op, const assert_inspected_t *ia
) {
  const char *base = assert_readable_operator(op);
  if (assert_op_is(op, "notStrictEqual") && is_object_type(actual))
    base = assert_readable_operator("notStrictEqualObject");

  if (ia->count == 1) return errbuf_appendf(eb, n, "%s%s%s", base, ia->len > 5 ? "\n\n" : " ", ia->text);
  if (ia->count <= 50) return errbuf_appendf(eb, n, "%s\n\n%s\n", base, ia->text);
  
  size_t head = (size_t)(ia->lines[45].ptr + ia->lines[45].len - ia->text);
  return errbuf_appendf(eb, n, "%s\n\n%.*s\n...\n", base, (int)head, ia->text);
}

static size_t assert_compare_message(
  errbuf_t *eb, size_t n, const char *op, const assert_inspected_t *ia, const assert_inspected_t *ie
) {
  const char *known = assert_readable_operator(op);
  bool same = ia->len == ie->len && memcmp(ia->text, ie->text, ia->len) == 0;
  
  if (assert_op_is(op, "notDeepEqual") && same) {
    size_t start = n;
    n = errbuf_appendf(eb, n, "%s\n\n%s", known, ia->text);
    if (n - start <= 1024) return n;
    n = start + assert_text_cut(eb->buf + start, n - start, 1021);
    return errbuf_appendf(eb, n, "...");
  }

  size_t rl = assert_text_cut(ia->text, ia->len, ASSERT_MAX_LONG_STRING);
  size_t ol = assert_text_cut(ie->text, ie->len, ASSERT_MAX_LONG_STRING);
  if (rl < ia->len) rl = assert_text_cut(ia->text, ia->len, ASSERT_MAX_LONG_STRING - 3);
  if (ol < ie->len) ol = assert_text_cut(ie->text, ie->len, ASSERT_MAX_LONG_STRING - 3);
  const char *rell = rl < ia->len ? "..." : "";
  const char *oell = ol < ie->len ? "..." : "";

  if (assert_op_is(op, "deepEqual")) return errbuf_appendf(
    eb, n, "%s\n\n%.*s%s\n\nshould loosely deep-equal\n\n%.*s%s",
    known, (int)rl, ia->text, rell, (int)ol, ie->text, oell
  );
  
  if (assert_op_is(op, "notDeepEqual")) return errbuf_appendf(
    eb, n, "%s\n\n%.*s%s\n\nshould not loosely deep-equal\n\n%.*s%s",
    assert_readable_operator("notDeepEqualUnequal"), (int)rl, ia->text, rell, (int)ol, ie->text, oell
  );
  
  return errbuf_appendf(
    eb, n, "%.*s%s %s %.*s%s", (int)rl, ia->text, rell,
    op ? op : "undefined", (int)ol, ie->text, oell
  );
}

static bool assert_op_has_diff(const char *op) {
  return assert_op_is(op, "deepStrictEqual") || assert_op_is(op, "strictEqual") || assert_op_is(op, "partialDeepStrictEqual");
}

static ant_value_t assert_message(ant_t *js, ant_value_t custom, assertion_info_t info) {
  bool has_custom = vtype(custom) != kTypeUndefined && vtype(custom) != kTypeNull;
  bool diff = assert_op_has_diff(info.op);
  if (has_custom && !diff) return js_tostring_val(js, custom);

  const char *custom_text = NULL;
  if (has_custom) {
    custom = js_tostring_val(js, custom);
    if (is_err(custom)) return custom;
    custom_text = js_getstr(js, custom, NULL);
  }

  errbuf_t eb;
  assert_inspected_t ia, ie;
  if (!errbuf_init(&eb, 256)) return Ant_Error_CreateOutOfMemory(js);
  
  bool ok = assert_inspect(js, info.actual, true, &ia) && assert_inspect(js, info.expected, true, &ie);
  size_t n = 0;
  if (!ok) n = 0;
  else if (diff) n = assert_err_diff(&eb, n, info.actual, info.expected, info.op, custom_text, &ia, &ie);
  else if (assert_op_is(info.op, "notDeepStrictEqual") || assert_op_is(info.op, "notStrictEqual"))
    n = assert_not_equal_message(&eb, n, info.actual, info.op, &ia);
  else n = assert_compare_message(&eb, n, info.op, &ia, &ie);

  assert_inspected_free(&ia);
  assert_inspected_free(&ie);
  return errbuf_take_string(js, &eb, n);
}

static ant_value_t assertion_fail(ant_t *js, ant_value_t custom, assertion_info_t info) {
  if (assert_is_error_instance(js, custom)) return js_throw(js, custom);
  bool generated = vtype(custom) == kTypeUndefined || vtype(custom) == kTypeNull;
  return assertion_throw(js, assert_message(js, custom, info), generated, info);
}

static ant_value_t assertion_fail_text(ant_t *js, ant_value_t custom, const char *text, assertion_info_t info) {
  if (vtype(custom) == kTypeUndefined || vtype(custom) == kTypeNull) return assertion_throw_text(js, text, true, info);
  return assertion_fail(js, custom, info);
}

static inline assertion_info_t assert_compared(ant_args_t, const char *op) {
  return (assertion_info_t){
    .op = op,
    .actual = nargs > 0 ? args[0] : js_mkundef(),
    .expected = nargs > 1 ? args[1] : js_mkundef(),
  };
}

typedef enum {
  ASSERT_TOK_NONE,
  ASSERT_TOK_NAME,
  ASSERT_TOK_STRING,
  ASSERT_TOK_NUM,
  ASSERT_TOK_MEMBER,
  ASSERT_TOK_OPEN,
  ASSERT_TOK_CLOSE,
  ASSERT_TOK_SEMI,
  ASSERT_TOK_OTHER,
} assert_tok_t;

static size_t assert_scan_token(const char *src, size_t len, size_t i, assert_tok_t *kind) {
  unsigned char c = (unsigned char)src[i];
  
  if (c == '\'' || c == '"' || c == '`') {
    size_t j = i + 1;
    while (j < len && src[j] != (char)c) j += src[j] == '\\' ? 2 : 1;
    *kind = c == '`' ? ASSERT_TOK_OTHER : ASSERT_TOK_STRING;
    return j < len ? j + 1 : len;
  }
  
  if (ant_ascii_is_alnum(c) || c == '_' || c == '$' || c >= 0x80) {
    size_t j = i + 1;
    while (j < len && (ant_ascii_is_alnum((unsigned char)src[j]) || src[j] == '_' || src[j] == '$' || (unsigned char)src[j] >= 0x80)) j++;
    *kind = ant_ascii_is_digit(c) ? ASSERT_TOK_NUM : ASSERT_TOK_NAME;
    return j;
  }
  
  if (c == '?' && i + 1 < len && src[i + 1] == '.' && !(i + 2 < len && ant_ascii_is_digit((unsigned char)src[i + 2]))) {
    *kind = ASSERT_TOK_MEMBER;
    return i + 2;
  }
  
  switch (c) {
    case '.': case '[': case ']': *kind = ASSERT_TOK_MEMBER; break;
    case '(': *kind = ASSERT_TOK_OPEN; break;
    case ')': *kind = ASSERT_TOK_CLOSE; break;
    case ';': *kind = ASSERT_TOK_SEMI; break;
    default: *kind = ASSERT_TOK_OTHER; break;
  }
  
  return i + 1;
}

static bool assert_first_expression(const char *line, size_t len, size_t col, size_t *out_start, size_t *out_end) {
  size_t member_start = SIZE_MAX, end = len;
  assert_tok_t last = ASSERT_TOK_NONE;
  size_t last_start = 0;
  int parens = 0;

  for (size_t i = 0; i < len;) {
    if (ant_ascii_is_space((unsigned char)line[i])) { i++; continue; }
    if (line[i] == '/' && i + 1 < len && line[i + 1] == '/') break;
    if (line[i] == '/' && i + 1 < len && line[i + 1] == '*') {
      const char *close = strstr(line + i + 2, "*/");
      i = close && (size_t)(close - line) < len ? (size_t)(close - line) + 2 : len;
      continue;
    }

    assert_tok_t kind;
    size_t start = i;
    i = assert_scan_token(line, len, i, &kind);

    if (start < col) {
      if (kind == ASSERT_TOK_SEMI) { member_start = SIZE_MAX; continue; }
      if (kind == ASSERT_TOK_MEMBER && last == ASSERT_TOK_NAME) {
        if (member_start == SIZE_MAX) member_start = last_start;
      } else if (kind != ASSERT_TOK_MEMBER && kind != ASSERT_TOK_NAME && kind != ASSERT_TOK_STRING && kind != ASSERT_TOK_NUM)
        member_start = SIZE_MAX;
      last = kind;
      last_start = start;
      continue;
    }

    if (kind == ASSERT_TOK_OPEN) parens++;
    else if (kind == ASSERT_TOK_CLOSE && --parens == 0) { end = i; break; }
    else if (kind == ASSERT_TOK_SEMI) { end = start; break; }
  }

  *out_start = member_start != SIZE_MAX ? member_start : col;
  *out_end = end;
  return *out_end > *out_start;
}

static ant_value_t assert_ok_source_message(ant_t *js) {
  js_error_site_t site = js_error_site_from_vm_top(js);
  if (!site.valid || !site.src || site.off >= site.src_len) return js_mkundef();

  const char *src = site.src;
  size_t off = (size_t)site.off, line_start = off, line_end = off;
  while (line_start > 0 && src[line_start - 1] != '\n') line_start--;
  while (line_end < (size_t)site.src_len && src[line_end] != '\n') line_end++;

  size_t start, end;
  if (!assert_first_expression(src + line_start, line_end - line_start, off - line_start, &start, &end)) return js_mkundef();

  errbuf_t eb;
  if (!errbuf_init(&eb, 128)) return js_mkundef();
  size_t n = errbuf_appendf(&eb, 0, "The expression evaluated to a falsy value:\n\n  ");
  
  for (size_t i = line_start + start; i < line_start + end; i++) {
    unsigned char c = (unsigned char)src[i];
    if (c >= 0x20 || c == '\t') n = errbuf_appendf(&eb, n, "%c", c);
    else if (c == '\b') n = errbuf_appendf(&eb, n, "\\b");
    else if (c == '\f') n = errbuf_appendf(&eb, n, "\\f");
    else if (c != '\r') n = errbuf_appendf(&eb, n, "\\u%04x", c);
  }
  
  n = errbuf_appendf(&eb, n, "\n");
  return errbuf_take_string(js, &eb, n);
}

// assert(value, message) / assert.ok
static ant_value_t assert_ok(ant_params_t) {
  if (nargs >= 1 && js_truthy(js, args[0])) return js_mkundef();
  
  ant_value_t custom = nargs >= 2 ? args[1] : js_mkundef();
  assertion_info_t info = { "==", nargs > 0 ? args[0] : js_mkundef(), js_true };
  if (assert_is_error_instance(js, custom)) return js_throw(js, custom);
  
  if (nargs == 0) return assertion_throw_text(js, "No value argument passed to `assert.ok()`", true, info);
  if (vtype(custom) != kTypeUndefined && vtype(custom) != kTypeNull) return assertion_fail(js, custom, info);

  ant_value_t source = assert_ok_source_message(js);
  if (vtype(source) == kTypeString) return assertion_throw(js, source, true, info);
  return assertion_fail(js, js_mkundef(), info);
}

// assert.fail(message)
static ant_value_t assert_fail(ant_params_t) {
  assertion_info_t info = { "fail", js_mkundef(), js_mkundef() };
  ant_value_t custom = nargs >= 1 ? args[0] : js_mkundef();
  if (vtype(custom) == kTypeUndefined) return assertion_throw_text(js, "Failed", true, info);
  return assertion_fail(js, custom, info);
}

// assert.ifError(value)
static ant_value_t assert_if_error(ant_params_t) {
  if (nargs < 1) return js_mkundef();
  uint8_t t = vtype(args[0]);
  if (t == kTypeNull || t == kTypeUndefined) return js_mkundef();
  if (is_err(args[0])) return args[0];

  char buf[512];
  ant_value_t msg = is_object_type(args[0]) ? js_get(js, args[0], "message") : js_mkundef();
  if (is_err(msg)) return msg;
  if (vtype(msg) == kTypeString) snprintf(buf, sizeof(buf), "ifError got unwanted exception: %s", js_getstr(js, msg, NULL));
  else if (t == kTypeString) snprintf(buf, sizeof(buf), "ifError got unwanted exception: '%s'", js_getstr(js, args[0], NULL));
  else snprintf(buf, sizeof(buf), "ifError got unwanted exception: %s", js_str(js, args[0]));
  return assertion_throw_text(js, buf, false, (assertion_info_t){ "ifError", args[0], js_mknull() });
}

typedef enum {
  DEEP_ITER_NONE,
  DEEP_ITER_ARRAY,
  DEEP_ITER_SET,
  DEEP_ITER_MAP,
} deep_iter_t;

typedef struct {
  ant_t *js;
  bool strict;
  int depth;
  ant_value_t seen[256][2];
} deep_equal_t;

static bool deep_equal_values(deep_equal_t *ctx, ant_value_t a, ant_value_t b);

static bool deep_is_object(ant_value_t value) {
  return is_object_type(value) && !is_callable(value);
}

static bool deep_is_nan(ant_value_t value) {
  return vtype(value) == kTypeNumber && isnan(tod(value));
}

static bool deep_truthy(ant_t *js, ant_value_t value) {
  if (!is_err(value)) return js_truthy(js, value);
  Ant_Exception_Clear(js);
  return false;
}

static ant_value_t deep_get(ant_t *js, ant_value_t obj, ant_value_t key) {
  if (vtype(key) == kTypeSymbol) return js_get_sym(js, obj, key);
  size_t len = 0;
  const char *name = js_getstr(js, key, &len);
  return js_getprop_fallback_len(js, obj, name, len);
}

static bool deep_has_enumerable(ant_t *js, ant_value_t obj, ant_value_t key) {
  return deep_truthy(js, Ant_Object_PropertyIsEnumerable(js, obj, key));
}

static bool deep_is_index_key(ant_t *js, ant_value_t key) {
  if (vtype(key) != kTypeString) return false;
  size_t len = 0;
  const char *s = js_getstr(js, key, &len);
  if (len == 0 || len > 10 || (len > 1 && s[0] == '0')) return false;
  
  uint64_t value = 0;
  for (size_t i = 0; i < len; i++) {
    if (!ant_ascii_is_digit((unsigned char)s[i])) return false;
    value = value * 10 + (uint64_t)(s[i] - '0');
  }
  
  return value < UINT32_MAX;
}

static ant_offset_t deep_key_count(ant_t *js, ant_value_t keys, bool skip_indices) {
  ant_offset_t len = js_arr_len(js, keys);
  if (!skip_indices) return len;
  
  ant_offset_t count = 0;
  for (ant_offset_t i = 0; i < len; i++) count += !deep_is_index_key(js, js_arr_get(js, keys, i));
  return count;
}

static bool deep_strings_equal(ant_t *js, ant_value_t a, ant_value_t b) {
  return vtype(a) == kTypeString && vtype(b) == kTypeString && strict_eq_values(js, a, b);
}

static bool deep_props_equal(ant_t *js, ant_value_t a, ant_value_t b, const char *const *names, size_t count) {
  for (size_t i = 0; i < count; i++) {
    ant_value_t x = js_get(js, a, names[i]), y = js_get(js, b, names[i]);
    if (is_err(x) || is_err(y) || !same_value_values(js, x, y)) return false;
  }
  return true;
}

static bool deep_is_error(ant_t *js, ant_value_t value) {
  return js_get_slot(js_as_obj(value), SLOT_ERROR_BRAND) == js_true || assert_is_error_instance(js, value);
}

static bool deep_view_bytes(ant_value_t value, const uint8_t **bytes, size_t *len) {
  TypedArrayData *ta = buffer_get_typedarray_data(value);
  if (ta) {
    *bytes = ta->buffer && ta->buffer->data ? ta->buffer->data + ta->byte_offset : NULL;
    *len = ta->byte_length;
    return true;
  }
  
  DataViewData *dv = buffer_get_dataview_data(value);
  if (dv) {
    *bytes = dv->buffer && dv->buffer->data ? dv->buffer->data + dv->byte_offset : NULL;
    *len = dv->byte_length;
    return true;
  }
  
  return false;
}

static bool deep_bytes_equal(const uint8_t *a, size_t al, const uint8_t *b, size_t bl) {
  return al == bl && (al == 0 || (a && b && memcmp(a, b, al) == 0));
}

static bool deep_float_view_equal(deep_equal_t *ctx, ant_value_t a, ant_value_t b) {
  TypedArrayData *ta = buffer_get_typedarray_data(a), *tb = buffer_get_typedarray_data(b);
  if (!ta || !tb || ta->length != tb->length) return false;
  
  for (size_t i = 0; i < ta->length; i++) {
    ant_value_t x, y;
    if (!buffer_typedarray_data_read_index(ctx->js, ta, i, &x) || !buffer_typedarray_data_read_index(ctx->js, tb, i, &y)) return false;
    if (tod(x) != tod(y)) return false;
  }
  
  return true;
}

static bool deep_is_float_view(ant_value_t value) {
  TypedArrayData *ta = buffer_get_typedarray_data(value);
  return ta && (ta->type == TYPED_ARRAY_FLOAT16 || ta->type == TYPED_ARRAY_FLOAT32 || ta->type == TYPED_ARRAY_FLOAT64);
}

static bool deep_enumerable_or_identical(deep_equal_t *ctx, ant_value_t a, ant_value_t b, const char *name) {
  ant_value_t key = js_mkstr(ctx->js, name, strlen(name));
  if (deep_has_enumerable(ctx->js, b, key)) return true;
  return deep_equal_values(ctx, js_get(ctx->js, a, name), js_get(ctx->js, b, name));
}

static bool deep_has_own(ant_t *js, ant_value_t obj, const char *name) {
  return deep_truthy(js, Ant_Object_HasOwn(js, obj, js_mkstr(js, name, strlen(name))));
}

static bool deep_array_elements(deep_equal_t *ctx, ant_value_t a, ant_value_t b) {
  ant_t *js = ctx->js;
  ant_offset_t len = js_arr_len(js, a);
  
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t x = js_arr_get(js, a, i), y = js_arr_get(js, b, i);
    bool hx = vtype(x) != kTypeUndefined || deep_truthy(js, Ant_Object_HasOwn(js, a, tov((double)i)));
    bool hy = vtype(y) != kTypeUndefined || deep_truthy(js, Ant_Object_HasOwn(js, b, tov((double)i)));
    if (hx != hy || (hx && !deep_equal_values(ctx, x, y))) return false;
  }
  
  return true;
}

static bool deep_match_candidate(
  deep_equal_t *ctx, ant_value_t *candidates, bool *used, size_t count,
  ant_value_t key, ant_value_t item, map_entry_t **map
) {
  for (size_t i = 0; i < count; i++) {
    if (used[i] || !deep_equal_values(ctx, key, candidates[i])) continue;
    if (map) {
      map_entry_t *entry = Ant_Map_Find(ctx->js, map, candidates[i]);
      if (!entry || !deep_equal_values(ctx, item, entry->value)) continue;
    }
    used[i] = true;
    return true;
  }
  return false;
}

static bool deep_set_equal(deep_equal_t *ctx, ant_value_t a, ant_value_t b) {
  ant_t *js = ctx->js;
  set_entry_t **sa = get_set_from_obj(js_as_obj(a)), **sb = get_set_from_obj(js_as_obj(b));
  if (!sa || !sb) return false;

  size_t count = HASH_COUNT(*sb), n = 0;
  ant_value_t *candidates = malloc((count ? count : 1) * sizeof(*candidates));
  bool *used = calloc(count ? count : 1, sizeof(*used));
  bool ok = candidates && used;

  set_entry_t *entry, *tmp;
  HASH_ITER(hh, *sb, entry, tmp) {
    if (!ok) break;
    if (Ant_Set_Find(js, sa, entry->value)) continue;
    if (ctx->strict && !deep_is_object(entry->value)) ok = false;
    else candidates[n++] = entry->value;
  }

  HASH_ITER(hh, *sa, entry, tmp) {
    if (!ok || n == 0) break;
    if (Ant_Set_Find(js, sb, entry->value)) continue;
    ok = deep_match_candidate(ctx, candidates, used, n, entry->value, js_mkundef(), NULL);
  }

  free(candidates);
  free(used);
  return ok;
}

static bool deep_map_equal(deep_equal_t *ctx, ant_value_t a, ant_value_t b) {
  ant_t *js = ctx->js;
  map_entry_t **ma = get_map_from_obj(js_as_obj(a)), **mb = get_map_from_obj(js_as_obj(b));
  if (!ma || !mb) return false;

  size_t count = HASH_COUNT(*mb), n = 0;
  ant_value_t *candidates = malloc((count ? count : 1) * sizeof(*candidates));
  bool *used = calloc(count ? count : 1, sizeof(*used));
  bool ok = candidates && used;

  map_entry_t *entry, *tmp;
  HASH_ITER(hh, *mb, entry, tmp) {
    if (!ok) break;
    if (!deep_is_object(entry->key_val)) {
      map_entry_t *other = Ant_Map_Find(js, ma, entry->key_val);
      if (other && deep_equal_values(ctx, other->value, entry->value)) continue;
      if (ctx->strict) { ok = false; break; }
    }
    candidates[n++] = entry->key_val;
  }

  HASH_ITER(hh, *ma, entry, tmp) {
    if (!ok || n == 0) break;
    if (!deep_is_object(entry->key_val)) {
      map_entry_t *other = Ant_Map_Find(js, mb, entry->key_val);
      if (other && deep_equal_values(ctx, entry->value, other->value)) continue;
    }
    ok = deep_match_candidate(ctx, candidates, used, n, entry->key_val, entry->value, mb);
  }

  free(candidates);
  free(used);
  return ok;
}

static bool deep_key_check(deep_equal_t *ctx, ant_value_t a, ant_value_t b, deep_iter_t iter, bool skip_indices) {
  ant_t *js = ctx->js;
  GC_ROOT_SAVE(mark, js);
  
  ant_value_t ka = js_own_property_keys(js, a, ctx->strict, true);
  GC_ROOT_PIN(js, ka);
  ant_value_t kb = js_own_property_keys(js, b, ctx->strict, true);
  GC_ROOT_PIN(js, kb);

  bool ok = !is_err(ka) && !is_err(kb) && deep_key_count(js, ka, skip_indices) == deep_key_count(js, kb, skip_indices);
  ant_offset_t count = ok ? js_arr_len(js, kb) : 0;

  for (int i = ctx->depth - 1; ok && i >= 0; i--)
    if (ctx->seen[i][0] == a && ctx->seen[i][1] == b) { GC_ROOT_RESTORE(js, mark); return true; }
  
  if (ok && ctx->depth >= (int)(sizeof(ctx->seen) / sizeof(ctx->seen[0]))) ok = false;
  if (ok) {
    ctx->seen[ctx->depth][0] = a;
    ctx->seen[ctx->depth][1] = b;
    ctx->depth++;
  
    for (ant_offset_t i = 0; ok && i < count; i++) {
      ant_value_t key = js_arr_get(js, kb, i);
      if (skip_indices && deep_is_index_key(js, key)) continue;
      ok = deep_has_enumerable(js, a, key) && deep_equal_values(ctx, deep_get(js, a, key), deep_get(js, b, key));
    }
  
    if (ok && iter == DEEP_ITER_ARRAY) ok = deep_array_elements(ctx, a, b);
    else if (ok && iter == DEEP_ITER_SET) ok = deep_set_equal(ctx, a, b);
    else if (ok && iter == DEEP_ITER_MAP) ok = deep_map_equal(ctx, a, b);
    ctx->depth--;
  }

  if (is_err(ka) || is_err(kb)) Ant_Exception_Clear(js);
  GC_ROOT_RESTORE(js, mark);
  return ok;
}

static bool deep_boxed_equal(ant_t *js, ant_value_t a, ant_value_t b) {
  ant_value_t x = js_get_slot(js_as_obj(a), SLOT_PRIMITIVE);
  ant_value_t y = js_get_slot(js_as_obj(b), SLOT_PRIMITIVE);
  return vtype(x) == vtype(y) && same_value_values(js, x, y);
}

static bool deep_is_special(ant_t *js, ant_value_t value) {
  ant_object_t *ptr = js_obj_ptr(js_as_obj(value));
  const uint8_t *bytes; size_t len;
  return is_array_value(value) || deep_view_bytes(value, &bytes, &len) || buffer_get_arraybuffer_data(value) ||
    get_set_from_obj(js_as_obj(value)) || get_map_from_obj(js_as_obj(value)) || is_date_instance(value) ||
    (ptr && ptr->flags.regexp_brand) || vtype(js_get_slot(js_as_obj(value), SLOT_PRIMITIVE)) != kTypeUndefined ||
    deep_is_error(js, value);
}

static bool deep_objects_equal(deep_equal_t *ctx, ant_value_t a, ant_value_t b) {
  ant_t *js = ctx->js;
  if (ctx->strict && js_get_proto(js, a) != js_get_proto(js, b)) return false;

  ant_value_t tag_a = Ant_Object_ToStringTag(js, a);
  ant_value_t tag_b = Ant_Object_ToStringTag(js, b);
  if (is_err(tag_a) || is_err(tag_b)) { Ant_Exception_Clear(js); return false; }
  if (!deep_strings_equal(js, tag_a, tag_b)) return false;

  ant_object_t *pa = js_obj_ptr(js_as_obj(a)), *pb = js_obj_ptr(js_as_obj(b));
  const uint8_t *ba, *bb; size_t la, lb;
  ArrayBufferData *abuf;

  if (is_array_value(a)) {
    if (!is_array_value(b) || js_arr_len(js, a) != js_arr_len(js, b)) return false;
    return deep_key_check(ctx, a, b, DEEP_ITER_ARRAY, true);
  }
  
  size_t tag_len = 0;
  const char *tag = js_getstr(js, tag_a, &tag_len);
  if (tag_len == 15 && memcmp(tag, "[object Object]", 15) == 0) return deep_key_check(ctx, a, b, DEEP_ITER_NONE, false);

  if (is_date_instance(a)) {
    if (!is_date_instance(b)) return false;
    ant_value_t ta = js_get_slot(js_as_obj(a), SLOT_DATA), tb = js_get_slot(js_as_obj(b), SLOT_DATA);
    if (vtype(ta) != kTypeNumber || vtype(tb) != kTypeNumber || tod(ta) != tod(tb)) return false;
  } else if (pa && pa->flags.regexp_brand) {
    static const char *const names[] = { "source", "flags", "lastIndex" };
    if (!pb || !pb->flags.regexp_brand || !deep_props_equal(js, a, b, names, 3)) return false;
  } else if (deep_view_bytes(a, &ba, &la)) {
    if (!deep_view_bytes(b, &bb, &lb)) return false;
    if (!ctx->strict && deep_is_float_view(a)) { if (!deep_float_view_equal(ctx, a, b)) return false; }
    else if (!deep_bytes_equal(ba, la, bb, lb)) return false;
    return deep_key_check(ctx, a, b, DEEP_ITER_NONE, true);
  } else if (get_set_from_obj(js_as_obj(a))) {
    set_entry_t **sb = get_set_from_obj(js_as_obj(b));
    if (!sb || HASH_COUNT(*get_set_from_obj(js_as_obj(a))) != HASH_COUNT(*sb)) return false;
    return deep_key_check(ctx, a, b, DEEP_ITER_SET, false);
  } else if (get_map_from_obj(js_as_obj(a))) {
    map_entry_t **mb = get_map_from_obj(js_as_obj(b));
    if (!mb || HASH_COUNT(*get_map_from_obj(js_as_obj(a))) != HASH_COUNT(*mb)) return false;
    return deep_key_check(ctx, a, b, DEEP_ITER_MAP, false);
  } else if ((abuf = buffer_get_arraybuffer_data(a))) {
    ArrayBufferData *bbuf = buffer_get_arraybuffer_data(b);
    if (!bbuf || !deep_bytes_equal(abuf->data, abuf->length, bbuf->data, bbuf->length)) return false;
  } else if (deep_is_error(js, a)) {
    if (
      !deep_is_error(js, b) ||
      !deep_enumerable_or_identical(ctx, a, b, "message") ||
      !deep_enumerable_or_identical(ctx, a, b, "name") ||
      !deep_enumerable_or_identical(ctx, a, b, "cause") ||
      !deep_enumerable_or_identical(ctx, a, b, "errors") ||
      deep_has_own(js, a, "cause") != deep_has_own(js, b, "cause")
    ) return false;
  } else if (vtype(js_get_slot(js_as_obj(a), SLOT_PRIMITIVE)) != kTypeUndefined) {
    if (!deep_boxed_equal(js, a, b)) return false;
  } else if (deep_is_special(js, b)) return false;

  return deep_key_check(ctx, a, b, DEEP_ITER_NONE, false);
}

static bool deep_equal_values(deep_equal_t *ctx, ant_value_t a, ant_value_t b) {
  ant_t *js = ctx->js;
  if (strict_eq_values(js, a, b)) return !ctx->strict || same_value_values(js, a, b);

  bool oa = deep_is_object(a), ob = deep_is_object(b);
  if (ctx->strict) {
    if (vtype(a) == kTypeNumber) return deep_is_nan(a) && deep_is_nan(b);
    if (!oa || !ob) return false;
  } else if (!oa || !ob) {
    if (oa || ob) return false;
    if (deep_is_nan(a) && deep_is_nan(b)) return true;
    return deep_truthy(js, Ant_Value_LooseEquals(js, a, b));
  }

  return deep_objects_equal(ctx, a, b);
}

bool js_deep_equal(ant_t *js, ant_value_t a, ant_value_t b, bool strict) {
  deep_equal_t ctx = { .js = js, .strict = strict };
  return deep_equal_values(&ctx, a, b);
}

static ant_value_t assert_equal(ant_params_t) {
  if (nargs < 2) return js_mkundef();
  if (!deep_truthy(js, Ant_Value_LooseEquals(js, args[0], args[1])) && !(deep_is_nan(args[0]) && deep_is_nan(args[1]))) return assertion_fail(js, nargs >= 3 ? args[2] : js_mkundef(), assert_compared(args, nargs, "=="));
  return js_mkundef();
}

static ant_value_t assert_not_equal(ant_params_t) {
  if (nargs < 2) return js_mkundef();
  if (deep_truthy(js, Ant_Value_LooseEquals(js, args[0], args[1])) || (deep_is_nan(args[0]) && deep_is_nan(args[1]))) return assertion_fail(js, nargs >= 3 ? args[2] : js_mkundef(), assert_compared(args, nargs, "!="));
  return js_mkundef();
}

static ant_value_t assert_strict_equal(ant_params_t) {
  if (nargs < 2) return js_mkundef();
  if (!same_value_values(js, args[0], args[1])) return assertion_fail(js, nargs >= 3 ? args[2] : js_mkundef(), assert_compared(args, nargs, "strictEqual"));
  return js_mkundef();
}

static ant_value_t assert_not_strict_equal(ant_params_t) {
  if (nargs < 2) return js_mkundef();
  if (same_value_values(js, args[0], args[1])) return assertion_fail(js, nargs >= 3 ? args[2] : js_mkundef(), assert_compared(args, nargs, "notStrictEqual"));
  return js_mkundef();
}

static ant_value_t assert_deep_equal(ant_params_t) {
  if (nargs < 2) return js_mkundef();
  if (!js_deep_equal(js, args[0], args[1], false)) return assertion_fail(js, nargs >= 3 ? args[2] : js_mkundef(), assert_compared(args, nargs, "deepEqual"));
  return js_mkundef();
}

static ant_value_t assert_not_deep_equal(ant_params_t) {
  if (nargs < 2) return js_mkundef();
  if (js_deep_equal(js, args[0], args[1], false)) return assertion_fail(js, nargs >= 3 ? args[2] : js_mkundef(), assert_compared(args, nargs, "notDeepEqual"));
  return js_mkundef();
}

static ant_value_t assert_deep_strict_equal(ant_params_t) {
  if (nargs < 2) return js_mkundef();
  if (!js_deep_equal(js, args[0], args[1], true)) return assertion_fail(js, nargs >= 3 ? args[2] : js_mkundef(), assert_compared(args, nargs, "deepStrictEqual"));
  return js_mkundef();
}

static ant_value_t assert_not_deep_strict_equal(ant_params_t) {
  if (nargs < 2) return js_mkundef();
  if (js_deep_equal(js, args[0], args[1], true)) return assertion_fail(js, nargs >= 3 ? args[2] : js_mkundef(), assert_compared(args, nargs, "notDeepStrictEqual"));
  return js_mkundef();
}

static ant_value_t assert_regexp(ant_native_params_t, bool expect_match, const char *op) {
  ant_value_t input = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t regexp = nargs > 1 ? args[1] : js_mkundef();
  ant_value_t custom = nargs > 2 ? args[2] : js_mkundef();
  
  ant_object_t *re = is_object_type(regexp) ? js_obj_ptr(js_as_obj(regexp)) : NULL;
  if (!re || !re->flags.regexp_brand) return Ant_Error_CreateInvalidArgType(js, "regexp", "an instance of RegExp", regexp);

  bool is_string = vtype(input) == kTypeString;
  if (is_string) {
    ant_value_t test_fn = js_getprop_fallback(js, regexp, "test");
    if (is_err(test_fn)) return test_fn;
    ant_value_t result = sv_vm_call(js->vm, js, test_fn, regexp, &input, 1, NULL, js_mkundef());
    if (is_err(result)) return result;
    if (js_truthy(js, result) == expect_match) return js_mkundef();
  }

  assertion_info_t info = { op, input, regexp };
  if (assert_is_error_instance(js, custom)) return js_throw(js, custom);
  if (js_truthy(js, custom)) return assertion_throw(js, js_tostring_val(js, custom), false, info);

  assert_inspected_t ii, ir = { 0 };
  if (!assert_inspect(js, input, false, &ii) || (is_string && !assert_inspect(js, regexp, false, &ir))) {
    assert_inspected_free(&ii);
    assert_inspected_free(&ir);
    return Ant_Error_CreateOutOfMemory(js);
  }

  errbuf_t eb;
  size_t n = 0;
  if (!errbuf_init(&eb, 128)) n = 0;
  else if (!is_string) n = errbuf_appendf(
    &eb, n, "The \"string\" argument must be of type string. Received type %s (%s)",
    typestr(vtype(input)), ii.text
  );
  
  else n = errbuf_appendf(
    &eb, n, "%s %s. Input:\n\n%s\n", expect_match 
      ? "The input did not match the regular expression" 
      : "The input was expected to not match the regular expression",
    ir.text, ii.text
  );

  assert_inspected_free(&ii);
  assert_inspected_free(&ir);
  
  return assertion_throw(js, eb.buf ? errbuf_take_string(js, &eb, n) : Ant_Error_CreateOutOfMemory(js), true, info);
}

static ant_value_t assert_exception_matches(
  ant_t *js, ant_value_t thrown, ant_value_t expected, ant_value_t message, const char *op
) {
  ant_object_t *re = is_object_type(expected) ? js_obj_ptr(js_as_obj(expected)) : NULL;
  if (re && re->flags.regexp_brand) {
    GC_ROOT_SAVE(mark, js);
    ant_value_t match_args[3] = { coerce_to_str(js, thrown), expected, message };
    GC_ROOT_PIN(js, match_args[0]);
    ant_value_t result = is_err(match_args[0]) ? match_args[0] : assert_regexp(js, match_args, 3, true, op);
    GC_ROOT_RESTORE(js, mark);
    return result;
  }

  if (!is_callable(expected)) return js_mkundef();
  ant_value_t prototype = js_get(js, expected, "prototype");
  if (is_err(prototype)) return prototype;
  
  if (!is_undefined(prototype)) {
    ant_value_t matches = do_instanceof(js, thrown, expected);
    if (is_err(matches)) return matches;
    if (js_truthy(js, matches)) return js_mkundef();
  }

  ant_value_t error_ctor = js->primordial_values[ANT_PRIMORDIAL_Error];
  if (js_is_prototype_of(js, error_ctor, expected))
    return assertion_fail_text(
      js, message, "The error is expected to be an instance of the specified constructor",
      (assertion_info_t){ op, thrown, expected }
    );

  ant_value_t receiver = Ant_Object_CreateBare(js);
  if (is_err(receiver)) return receiver;
  ant_value_t valid = sv_vm_call(js->vm, js, expected, receiver, &thrown, 1, NULL, js_mkundef());
  
  if (is_err(valid)) return valid;
  if (valid != js_true)
    return assertion_fail_text(
      js, message, "The validation function is expected to return true",
      (assertion_info_t){ op, thrown, expected }
    );
  
  return js_mkundef();
}

static ant_value_t assert_missing_exception(
  ant_t *js, ant_value_t expected, ant_value_t message, const char *what, const char *op
) {
  if (assert_is_error_instance(js, message)) return js_throw(js, message);

  char kind[160] = "";
  ant_value_t name = is_callable(expected) ? js_get(js, expected, "name") : js_mkundef();
  if (vtype(name) == kTypeString) snprintf(kind, sizeof(kind), " (%s)", js_getstr(js, name, NULL));

  char buf[512];
  if (vtype(message) == kTypeString) snprintf(buf, sizeof(buf), "Missing expected %s%s: %s", what, kind, js_getstr(js, message, NULL));
  else snprintf(buf, sizeof(buf), "Missing expected %s%s.", what, kind);

  return assertion_throw_text(js, buf, false, (assertion_info_t){ op, js_mkundef(), expected });
}

static ant_value_t assert_throws(ant_params_t) {
  if (nargs < 1 || !is_callable(args[0]))
    return js_mkerr(js, "assert.throws: first argument must be a function");

  GC_ROOT_SAVE(mark, js);
  ant_value_t fn = args[0];
  ant_value_t expected = nargs >= 2 ? args[1] : js_mkundef();
  ant_value_t message = nargs >= 3 ? args[2] : js_mkundef();
  ant_value_t thrown = js_mkundef();
  if (vtype(expected) == kTypeString && nargs < 3) {
    message = expected;
    expected = js_mkundef();
  }
  
  GC_ROOT_PIN(js, fn);
  GC_ROOT_PIN(js, expected);
  GC_ROOT_PIN(js, message);
  GC_ROOT_PIN(js, thrown);

  ant_value_t result = sv_vm_call(js->vm, js, fn, js_mkundef(), NULL, 0, NULL, js_mkundef());
  if (!is_err(result)) result = assert_missing_exception(js, expected, message, "exception", "throws");
  else {
    thrown = js_take_thrown(js, result);
    result = assert_exception_matches(js, thrown, expected, message, "throws");
  } GC_ROOT_RESTORE(js, mark);
  
  return result;
}

static ant_value_t assert_does_not_throw(ant_params_t) {
  if (nargs < 1 || vtype(args[0]) != kTypeFunction)
    return js_mkerr(js, "assert.doesNotThrow: first argument must be a function");

  ant_value_t result = sv_vm_call(js->vm, js, args[0], js_mkundef(), NULL, 0, NULL, js_mkundef());
  if (!is_err(result)) return js_mkundef();

  GC_ROOT_SAVE(mark, js);
  ant_value_t thrown = js_take_thrown(js, result);
  
  GC_ROOT_PIN(js, thrown);
  ant_value_t msg = is_object_type(thrown) ? js_get(js, thrown, "message") : js_mkundef();
  
  if (is_err(msg)) {
    GC_ROOT_RESTORE(js, mark);
    return msg;
  }
  
  ant_value_t user_msg = nargs >= 3 ? args[2] : nargs >= 2 && vtype(args[1]) == kTypeString ? args[1] : js_mkundef();
  char head[256] = "Got unwanted exception.";
  if (vtype(user_msg) == kTypeString) snprintf(head, sizeof(head), "Got unwanted exception: %s", js_getstr(js, user_msg, NULL));

  char buf[512];
  snprintf(
    buf, sizeof(buf), "%s\nActual message: \"%s\"", head,
    vtype(msg) == kTypeString ? js_getstr(js, msg, NULL) : js_str(js, thrown)
  );
  
  result = assertion_throw_text(js, buf, false, (assertion_info_t){ "doesNotThrow", thrown, js_mkundef() });
  GC_ROOT_RESTORE(js, mark);
  return result;
}

static ant_value_t assert_rejection_settle(
  ant_t *js, ant_value_t promise, ant_value_t result, ant_value_t value,
  ant_value_t expected, ant_value_t message, bool expect_rejection
) {
  bool rejected = promise_was_rejected(result);
  if (rejected) promise_mark_handled(result);

  if (expect_rejection) {
    ant_value_t check = rejected
      ? assert_exception_matches(js, value, expected, message, "rejects")
      : assert_missing_exception(js, expected, message, "rejection", "rejects");
    if (is_err(check)) js_reject_promise(js, promise, check);
    else js_resolve_promise(js, promise, js_mkundef());
  } else if (!rejected) js_resolve_promise(js, promise, js_mkundef());
  else js_reject_promise(js, promise, Ant_Error_Create(js, JS_ERR_TYPE, "Got unwanted rejection"));

  return js_mkundef();
}

static ant_value_t assert_rejection_on_settled(ant_params_t) {
  ant_value_t state = js_func_data(js->current_func);
  return assert_rejection_settle(
    js, js_get(js, state, "promise"), js_get(js, state, "result"), nargs > 0 ? args[0] : js_mkundef(),
    js_get(js, state, "expected"), js_get(js, state, "message"),
    js_truthy(js, js_get(js, state, "expect_rejection"))
  );
}

static ant_value_t assert_rejection(ant_native_params_t, bool expect_rejection) {
  const char *method = expect_rejection ? "rejects" : "doesNotReject";
  if (nargs < 1) return js_mkerr(js, "assert.%s: first argument required", method);

  GC_ROOT_SAVE(mark, js);
  ant_value_t expected = nargs >= 2 ? args[1] : js_mkundef();
  ant_value_t message = nargs >= 3 ? args[2] : js_mkundef();
  
  if (vtype(expected) == kTypeString && nargs < 3) {
    message = expected;
    expected = js_mkundef();
  }
  
  GC_ROOT_PIN(js, expected);
  GC_ROOT_PIN(js, message);

  ant_value_t promise = js_mkpromise(js);
  GC_ROOT_PIN(js, promise);
  
  ant_value_t result = vtype(args[0]) == kTypeFunction
    ? sv_vm_call(js->vm, js, args[0], js_mkundef(), NULL, 0, NULL, js_mkundef())
    : args[0];
  
  GC_ROOT_PIN(js, result);
  if (is_err(result)) {
    js_reject_promise(js, promise, result);
  } else if (vtype(result) == kTypePromise) {
    ant_value_t state = Ant_Object_CreateBare(js);
    GC_ROOT_PIN(js, state);
    
    js_set(js, state, "promise", promise);
    js_set(js, state, "result", result);
    js_set(js, state, "expected", expected);
    js_set(js, state, "message", message);
    js_set(js, state, "expect_rejection", js_bool(expect_rejection));
    
    ant_value_t on_settled = js_heavy_mkfun(js, assert_rejection_on_settled, state);
    GC_ROOT_PIN(js, on_settled);
    
    ant_value_t continuation = js_promise_then(js, result, on_settled, on_settled);
    if (is_err(continuation))
      js_reject_promise(js, promise, continuation);
    else promise_mark_handled(continuation);
  } else assert_rejection_settle(js, promise, result, result, expected, message, expect_rejection);

  GC_ROOT_RESTORE(js, mark);
  return promise;
}

static ant_value_t assert_rejects(ant_params_t) {
  return assert_rejection(js, args, nargs, true);
}

static ant_value_t assert_does_not_reject(ant_params_t) {
  return assert_rejection(js, args, nargs, false);
}

static ant_value_t assert_match(ant_params_t) {
  return assert_regexp(js, args, nargs, true, "match");
}

static ant_value_t assert_does_not_match(ant_params_t) {
  return assert_regexp(js, args, nargs, false, "doesNotMatch");
}

static ant_value_t assert_assertion_error_ctor(ant_params_t) {
  ant_value_t options = nargs >= 1 ? args[0] : js_mkundef();
  if (vtype(options) != kTypeObject) return Ant_Error_CreateInvalidArgType(js, "options", "of type object", options);
  assertion_info_t info = { NULL, js_mkundef(), js_mkundef() };
  ant_value_t fields[4] = { js_mkundef(), js_mkundef(), js_mkundef(), js_mkundef() };
  static const char *const names[] = { "message", "actual", "expected", "operator" };

  for (int i = 0; i < 4; i++) {
    fields[i] = js_get(js, options, names[i]);
    if (is_err(fields[i])) return fields[i];
  }

  GC_ROOT_SAVE(mark, js);
  for (int i = 0; i < 4; i++) GC_ROOT_PIN(js, fields[i]);
  info.actual = fields[1];
  info.expected = fields[2];
  info.op = vtype(fields[3]) == kTypeString ? js_getstr(js, fields[3], NULL) : NULL;

  ant_value_t err = assertion_error_new(js, assert_message(js, fields[0], info), !js_truthy(js, fields[0]), info);
  if (!is_err(err) && is_object_type(call_new_target)) {
    GC_ROOT_PIN(js, err);
    ant_value_t proto = js_get(js, call_new_target, "prototype");
    if (is_object_type(proto)) js_set_proto_init(err, proto);
  }

  GC_ROOT_RESTORE(js, mark);
  return err;
}

static ant_value_t assert_strict_object(ant_t *js, ant_value_t assert_fn) {
  static const struct { const char *name; const char *source; } members[] = {
    { "ok", "ok" },
    { "fail", "fail" },
    { "ifError", "ifError" },
    { "equal", "strictEqual" },
    { "notEqual", "notStrictEqual" },
    { "strictEqual", "strictEqual" },
    { "notStrictEqual", "notStrictEqual" },
    { "deepEqual", "deepStrictEqual" },
    { "notDeepEqual", "notDeepStrictEqual" },
    { "deepStrictEqual", "deepStrictEqual" },
    { "notDeepStrictEqual", "notDeepStrictEqual" },
    { "throws", "throws" },
    { "doesNotThrow", "doesNotThrow" },
    { "rejects", "rejects" },
    { "doesNotReject", "doesNotReject" },
    { "match", "match" },
    { "doesNotMatch", "doesNotMatch" },
    { "AssertionError", "AssertionError" },
  };

  GC_ROOT_SAVE(mark, js);
  ant_value_t strict_obj = Ant_Object_CreateBare(js);
  GC_ROOT_PIN(js, strict_obj);
  js_set_slot(strict_obj, SLOT_CFUNC, Ant_Function_Create(assert_ok));
  js_define_function_length(js, strict_obj, 0);
  js_set_function_name(js, strict_obj, "strict", 6);

  for (size_t i = 0; i < countof(members); i++) {
    const char *key = intern_string(js, members[i].name, strlen(members[i].name));
    if (!key) { GC_ROOT_RESTORE(js, mark); return js_mkerr(js, "oom"); }
    mkprop_interned_exact(js, strict_obj, key, js_get(js, assert_fn, members[i].source), ANT_PROP_ATTR_DEFAULT);
  }

  ant_value_t strict_fn = js_obj_to_func(js, strict_obj);
  js_set(js, strict_fn, "strict", strict_fn);
  js_set(js, strict_fn, "default", strict_fn);
  
  GC_ROOT_RESTORE(js, mark);
  return strict_fn;
}

ant_value_t assert_library(ant_t *js) {
  ant_value_t assert_obj = Ant_Object_CreateBare(js);
  js_set_slot(assert_obj, SLOT_CFUNC, Ant_Function_Create(assert_ok));
  js_define_function_length(js, assert_obj, 0);
  js_set_function_name(js, assert_obj, "assert", 6);

  js_set(js, assert_obj, "ok", Ant_Function_Create(assert_ok));
  js_set(js, assert_obj, "fail", Ant_Function_CreateWithArity(assert_fail, 1));
  js_set(js, assert_obj, "ifError", Ant_Function_CreateWithArity(assert_if_error, 1));
  js_set(js, assert_obj, "equal", Ant_Function_CreateWithArity(assert_equal, 2));
  js_set(js, assert_obj, "notEqual", Ant_Function_CreateWithArity(assert_not_equal, 2));
  js_set(js, assert_obj, "strictEqual", Ant_Function_CreateWithArity(assert_strict_equal, 2));
  js_set(js, assert_obj, "notStrictEqual", Ant_Function_CreateWithArity(assert_not_strict_equal, 2));
  js_set(js, assert_obj, "deepEqual", Ant_Function_CreateWithArity(assert_deep_equal, 2));
  js_set(js, assert_obj, "notDeepEqual", Ant_Function_CreateWithArity(assert_not_deep_equal, 2));
  js_set(js, assert_obj, "deepStrictEqual", Ant_Function_CreateWithArity(assert_deep_strict_equal, 2));
  js_set(js, assert_obj, "notDeepStrictEqual", Ant_Function_CreateWithArity(assert_not_deep_strict_equal, 2));
  js_set(js, assert_obj, "throws", Ant_Function_CreateWithArity(assert_throws, 1));
  js_set(js, assert_obj, "doesNotThrow", Ant_Function_CreateWithArity(assert_does_not_throw, 1));
  js_set(js, assert_obj, "rejects", Ant_Function_CreateWithArity(assert_rejects, 1));
  js_set(js, assert_obj, "doesNotReject", Ant_Function_CreateWithArity(assert_does_not_reject, 1));
  js_set(js, assert_obj, "match", Ant_Function_CreateWithArity(assert_match, 2));
  js_set(js, assert_obj, "doesNotMatch", Ant_Function_CreateWithArity(assert_does_not_match, 2));

  ant_value_t ae_fn = Ant_Error_CreateConstructorEx(
    js, "AssertionError", Ant_Function_CreateWithArity(assert_assertion_error_ctor, 1), 
    &js->builtins.assertion_error_proto
  );
  
  js_set(js, assert_obj, "AssertionError", ae_fn);
  ant_value_t lib = js_obj_to_func(js, assert_obj);
  js_set(js, lib, "default", lib);
  js_set(js, lib, "strict", assert_strict_object(js, lib));
  
  return lib;
}

ant_value_t assert_strict_library(ant_t *js) {
  ant_library_entry_t *entry = ant_library_find("assert", 6);
  ant_value_t lib = entry ? ant_library_load(js, entry) : assert_library(js);
  return is_err(lib) ? lib : js_get(js, lib, "strict");
}
