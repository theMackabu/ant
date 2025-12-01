#if defined(__GNUC__) && !defined(__clang__)
  #pragma GCC optimize("O3,inline")
#endif

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "config.h"
#include "modules/timer.h"

typedef uint32_t jsoff_t;

typedef struct {
  jsval_t *stack;
  int depth;
  int capacity;
} this_stack_t;

static this_stack_t global_this_stack = {NULL, 0, 0};

struct js {
  jsoff_t css;            // max observed C stack size
  jsoff_t lwm;            // JS ram low watermark: min free ram observed
  const char *code;       // currently parsed code snippet
  char errmsg[256];       // error message placeholder (increased from 33)
  const char *filename;   // current filename for error reporting
  uint8_t tok;            // last parsed token value
  uint8_t consumed;       // indicator that last parsed token was consumed
  uint8_t flags;          // execution flags, see F_* constants below
  #define F_NOEXEC 1U     // parse code, but not execute
  #define F_LOOP 2U       // we are inside the loop
  #define F_CALL 4U       // we are inside a function call
  #define F_BREAK 8U      // exit the loop
  #define F_RETURN 16U    // return has been executed
  jsoff_t clen;           // code snippet length
  jsoff_t pos;            // current parsing position
  jsoff_t toff;           // offset of the last parsed token
  jsoff_t tlen;           // length of the last parsed token
  jsoff_t nogc;           // entity offset to exclude from GC
  jsval_t tval;           // holds last parsed numeric or string literal value
  jsval_t scope;          // current scope
  jsval_t this_val;       // 'this' value for currently executing function
  uint8_t *mem;           // available JS memory
  jsoff_t size;           // memory size
  jsoff_t brk;            // current mem usage boundary
  jsoff_t gct;            // GC threshold. if brk > gct, trigger GC
  jsoff_t maxcss;         // maximum allowed C stack size usage
  void *cstk;             // C stack pointer at the beginning of js_eval()
  jsval_t current_func;   // currently executing function (for native closures)
};

enum {
  TOK_ERR, TOK_EOF, TOK_IDENTIFIER, TOK_NUMBER, TOK_STRING, TOK_SEMICOLON,
  TOK_LPAREN, TOK_RPAREN, TOK_LBRACE, TOK_RBRACE, TOK_LBRACKET, TOK_RBRACKET,
  TOK_ASYNC = 50, TOK_AWAIT, TOK_BREAK, TOK_CASE, TOK_CATCH, TOK_CLASS, TOK_CONST, TOK_CONTINUE,
  TOK_DEFAULT, TOK_DELETE, TOK_DO, TOK_ELSE, TOK_FINALLY, TOK_FOR, TOK_FUNC,
  TOK_IF, TOK_IN, TOK_INSTANCEOF, TOK_LET, TOK_NEW, TOK_RETURN, TOK_SWITCH,
  TOK_THIS, TOK_THROW, TOK_TRY, TOK_VAR, TOK_VOID, TOK_WHILE, TOK_WITH,
  TOK_YIELD, TOK_UNDEF, TOK_NULL, TOK_TRUE, TOK_FALSE,
  TOK_DOT = 100, TOK_CALL, TOK_BRACKET, TOK_POSTINC, TOK_POSTDEC, TOK_NOT, TOK_TILDA,
  TOK_TYPEOF, TOK_UPLUS, TOK_UMINUS, TOK_EXP, TOK_MUL, TOK_DIV, TOK_REM,
  TOK_OPTIONAL_CHAIN, TOK_REST,
  TOK_PLUS, TOK_MINUS, TOK_SHL, TOK_SHR, TOK_ZSHR, TOK_LT, TOK_LE, TOK_GT,
  TOK_GE, TOK_EQ, TOK_NE, TOK_AND, TOK_XOR, TOK_OR, TOK_LAND, TOK_LOR,
  TOK_COLON, TOK_Q,  TOK_ASSIGN, TOK_PLUS_ASSIGN, TOK_MINUS_ASSIGN,
  TOK_MUL_ASSIGN, TOK_DIV_ASSIGN, TOK_REM_ASSIGN, TOK_SHL_ASSIGN,
  TOK_SHR_ASSIGN, TOK_ZSHR_ASSIGN, TOK_AND_ASSIGN, TOK_XOR_ASSIGN,
  TOK_OR_ASSIGN, TOK_COMMA, TOK_TEMPLATE, TOK_ARROW,
};

enum {
  T_OBJ, T_PROP, T_STR, T_UNDEF, T_NULL, T_NUM,
  T_BOOL, T_FUNC, T_CODEREF, T_CFUNC, T_ERR, T_ARR, T_PROMISE
};

static const char *typestr(uint8_t t) {
  const char *names[] = { 
    "object", "prop", "string", "undefined", "null", "number",
    "boolean", "function", "coderef", "cfunc", "err", "array", "promise"
  };
  
  return (t < sizeof(names) / sizeof(names[0])) ? names[t] : "??";
}

static jsval_t tov(double d) { union { double d; jsval_t v; } u = {d}; return u.v; }
static double tod(jsval_t v) { union { jsval_t v; double d; } u = {v}; return u.d; }
static jsval_t mkval(uint8_t type, uint64_t data) { return ((jsval_t) 0x7ff0U << 48U) | ((jsval_t) (type) << 48) | (data & 0xffffffffffffUL); }
static bool is_nan(jsval_t v) { return (v >> 52U) == 0x7ffU; }
static uint8_t vtype(jsval_t v) { return is_nan(v) ? ((v >> 48U) & 15U) : (uint8_t) T_NUM; }
static size_t vdata(jsval_t v) { return (size_t) (v & ~((jsval_t) 0x7fffUL << 48U)); }
static jsval_t mkcoderef(jsval_t off, jsoff_t len) { return mkval(T_CODEREF, (off & 0xffffffU) | ((jsval_t)(len & 0xffffffU) << 24U)); }
static jsoff_t coderefoff(jsval_t v) { return v & 0xffffffU; }
static jsoff_t codereflen(jsval_t v) { return (v >> 24U) & 0xffffffU; }

static uint8_t unhex(uint8_t c) { return (c >= '0' && c <= '9') ? (uint8_t) (c - '0') : (c >= 'a' && c <= 'f') ? (uint8_t) (c - 'W') : (c >= 'A' && c <= 'F') ? (uint8_t) (c - '7') : 0; }
static bool is_space(int c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == '\f' || c == '\v'; }
static bool is_digit(int c) { return c >= '0' && c <= '9'; }
static bool is_xdigit(int c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static bool is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool is_ident_begin(int c) { return c == '_' || c == '$' || is_alpha(c); }
static bool is_ident_continue(int c) { return c == '_' || c == '$' || is_alpha(c) || is_digit(c); }
static bool is_err(jsval_t v) { return vtype(v) == T_ERR; }
static bool is_unary(uint8_t tok) { return (tok >= TOK_POSTINC && tok <= TOK_UMINUS) || tok == TOK_NOT || tok == TOK_TILDA || tok == TOK_TYPEOF || tok == TOK_VOID; }
static bool is_assign(uint8_t tok) { return (tok >= TOK_ASSIGN && tok <= TOK_OR_ASSIGN); }
static void saveoff(struct js *js, jsoff_t off, jsoff_t val) { memcpy(&js->mem[off], &val, sizeof(val)); }
static void saveval(struct js *js, jsoff_t off, jsval_t val) { memcpy(&js->mem[off], &val, sizeof(val)); }
static jsoff_t loadoff(struct js *js, jsoff_t off) { jsoff_t v = 0; assert(js->brk <= js->size); memcpy(&v, &js->mem[off], sizeof(v)); return v; }
static jsoff_t offtolen(jsoff_t off) { return (off >> 2) - 1; }
static jsoff_t vstrlen(struct js *js, jsval_t v) { return offtolen(loadoff(js, (jsoff_t) vdata(v))); }
static jsval_t loadval(struct js *js, jsoff_t off) { jsval_t v = 0; memcpy(&v, &js->mem[off], sizeof(v)); return v; }
static jsval_t upper(struct js *js, jsval_t scope) { return mkval(T_OBJ, loadoff(js, (jsoff_t) (vdata(scope) + sizeof(jsoff_t)))); }
static jsoff_t align32(jsoff_t v) { return ((v + 3) >> 2) << 2; }

#define CHECKV(_v) do { if (is_err(_v)) { res = (_v); goto done; } } while (0)
#define EXPECT(_tok, _e) do { if (next(js) != _tok) { _e; return js_mkerr(js, "parse error"); }; js->consumed = 1; } while (0)

static bool streq(const char *buf, size_t len, const char *p, size_t n);
static size_t tostr(struct js *js, jsval_t value, char *buf, size_t len);
static size_t strpromise(struct js *js, jsval_t value, char *buf, size_t len);

static inline jsoff_t esize(jsoff_t w);
static jsval_t js_expr(struct js *js);
static jsval_t js_stmt(struct js *js);
static jsval_t js_assignment(struct js *js);
static jsval_t js_arrow_func(struct js *js, jsoff_t params_start, jsoff_t params_end, bool is_async);
static jsval_t do_op(struct js *, uint8_t op, jsval_t l, jsval_t r);
static jsval_t do_instanceof(struct js *js, jsval_t l, jsval_t r);
static jsval_t resolveprop(struct js *js, jsval_t v);
static jsoff_t lkp(struct js *js, jsval_t obj, const char *buf, size_t len);
static jsval_t builtin_array_push(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_array_pop(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_array_slice(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_array_join(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_array_includes(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_indexOf(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_substring(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_split(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_slice(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_includes(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_startsWith(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_endsWith(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_replace(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_template(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_string_charCodeAt(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Object(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Promise(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Promise_resolve(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Promise_reject(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_Promise_try(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_promise_then(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_promise_catch(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_promise_finally(struct js *js, jsval_t *args, int nargs);

static void setlwm(struct js *js) {
  jsoff_t n = 0, css = 0;
  if (js->brk < js->size) n = js->size - js->brk;
  if (js->lwm > n) js->lwm = n;
  if ((char *) js->cstk > (char *) &n)
    css = (jsoff_t) ((char *) js->cstk - (char *) &n);
  if (css > js->css) js->css = css;
}

static size_t cpy(char *dst, size_t dstlen, const char *src, size_t srclen) {
  size_t i = 0;
  for (i = 0; i < dstlen && i < srclen && src[i] != 0; i++) dst[i] = src[i];
  if (dstlen > 0) dst[i < dstlen ? i : dstlen - 1] = '\0';
  return i;
}

static size_t strarr(struct js *js, jsval_t obj, char *buf, size_t len) {
  size_t n = cpy(buf, len, "[", 1);
  jsoff_t next = loadoff(js, (jsoff_t) vdata(obj)) & ~(3U | CONSTMASK);
  jsoff_t length = 0;
  jsoff_t scan = next;
  
  while (scan < js->brk && scan != 0) {
    jsoff_t koff = loadoff(js, scan + (jsoff_t) sizeof(scan));
    jsoff_t klen = offtolen(loadoff(js, koff));
    const char *key = (char *) &js->mem[koff + sizeof(koff)];
    
    if (streq(key, klen, "length", 6)) {
      jsval_t val = loadval(js, scan + (jsoff_t) (sizeof(scan) + sizeof(koff)));
      if (vtype(val) == T_NUM) length = (jsoff_t) tod(val);
      break;
    }
    scan = loadoff(js, scan) & ~(3U | CONSTMASK);
  }
  
  for (jsoff_t i = 0; i < length; i++) {
    if (i > 0) n += cpy(buf + n, len - n, ",", 1);
    char idx[16];
    snprintf(idx, sizeof(idx), "%u", (unsigned) i);
    
    jsoff_t idxlen = (jsoff_t) strlen(idx);
    jsoff_t prop = next;
    jsval_t val = js_mkundef();
    
    bool found = false;
    
    while (prop < js->brk && prop != 0) {
      jsoff_t koff = loadoff(js, prop + (jsoff_t) sizeof(prop));
      jsoff_t klen = offtolen(loadoff(js, koff));
      const char *key = (char *) &js->mem[koff + sizeof(koff)];
      if (streq(key, klen, idx, idxlen)) {
        val = loadval(js, prop + (jsoff_t) (sizeof(prop) + sizeof(koff)));
        found = true;
        break;
      }
      prop = loadoff(js, prop) & ~(3U | CONSTMASK);
    }
    
    if (found) {
      n += tostr(js, val, buf + n, len - n);
    } else {
      n += cpy(buf + n, len - n, "undefined", 9);
    }
  }
  
  return n + cpy(buf + n, len - n, "]", 1);
}

static size_t strobj(struct js *js, jsval_t obj, char *buf, size_t len) {
  size_t n = cpy(buf, len, "{", 1);
  jsoff_t next = loadoff(js, (jsoff_t) vdata(obj)) & ~(3U | CONSTMASK);
  
  while (next < js->brk && next != 0) {
    jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
    jsval_t val = loadval(js, next + (jsoff_t) (sizeof(next) + sizeof(koff)));
    n += cpy(buf + n, len - n, ",", n == 1 ? 0 : 1);
    n += tostr(js, mkval(T_STR, koff), buf + n, len - n);
    n += cpy(buf + n, len - n, ":", 1);
    n += tostr(js, val, buf + n, len - n);
    next = loadoff(js, next) & ~(3U | CONSTMASK);
  }
  
  return n + cpy(buf + n, len - n, "}", 1);
}

static size_t strnum(jsval_t value, char *buf, size_t len) {
  double dv = tod(value), iv;
  const char *fmt = modf(dv, &iv) == 0.0 ? "%.17g" : "%g";
  return (size_t) snprintf(buf, len, fmt, dv);
}

static jsoff_t vstr(struct js *js, jsval_t value, jsoff_t *len) {
  jsoff_t off = (jsoff_t) vdata(value);
  if (len) *len = offtolen(loadoff(js, off));
  return (jsoff_t) (off + sizeof(off));
}

static size_t strstring(struct js *js, jsval_t value, char *buf, size_t len) {
  jsoff_t slen, off = vstr(js, value, &slen);
  size_t n = 0;
  n += cpy(buf + n, len - n, "\"", 1);
  n += cpy(buf + n, len - n, (char *) &js->mem[off], slen);
  n += cpy(buf + n, len - n, "\"", 1);
  
  return n;
}

static size_t strfunc(struct js *js, jsval_t value, char *buf, size_t len) {
  jsval_t func_obj = mkval(T_OBJ, vdata(value));
  jsoff_t code_off = lkp(js, func_obj, "__code", 6);
  
  if (code_off == 0) return cpy(buf, len, "function()", 10);
  jsval_t code_val = resolveprop(js, mkval(T_PROP, code_off));
  
  if (vtype(code_val) != T_STR) return cpy(buf, len, "function()", 10);
  jsoff_t sn, off = vstr(js, code_val, &sn);
  size_t n = cpy(buf, len, "function", 8);
  
  return n + cpy(buf + n, len - n, (char *) &js->mem[off], sn);
}

static void get_line_col(const char *code, jsoff_t pos, int *line, int *col) {
  *line = 1;
  *col = 1;
  for (jsoff_t i = 0; i < pos && code[i] != '\0'; i++) {
    if (code[i] == '\n') {
      (*line)++;
      *col = 1;
    } else {
      (*col)++;
    }
  }
}

static void get_error_line(const char *code, jsoff_t clen, jsoff_t pos, char *buf, size_t bufsize, int *line_start_col) {
  jsoff_t line_start = pos;
  while (line_start > 0 && code[line_start - 1] != '\n') {
    line_start--;
  }
  
  jsoff_t line_end = pos;
  while (line_end < clen && code[line_end] != '\n' && code[line_end] != '\0') {
    line_end++;
  }
  
  jsoff_t line_len = line_end - line_start;
  if (line_len >= bufsize) line_len = bufsize - 1;
  
  memcpy(buf, &code[line_start], line_len);
  buf[line_len] = '\0';
  *line_start_col = (int)(pos - line_start) + 1;
}

jsval_t js_mkerr(struct js *js, const char *xx, ...) {
  va_list ap;
  int line = 0, col = 0;
  char error_line[256] = {0};
  int error_col = 0;
  get_line_col(js->code, js->toff > 0 ? js->toff : js->pos, &line, &col);
  get_error_line(js->code, js->clen, js->toff > 0 ? js->toff : js->pos, error_line, sizeof(error_line), &error_col);
  size_t n = 0;
  if (js->filename) {
    n = (size_t) snprintf(js->errmsg, sizeof(js->errmsg), "Panic: %s:%d:%d\n", js->filename, line, col);
  } else {
    n = (size_t) snprintf(js->errmsg, sizeof(js->errmsg), "Panic: <eval>:%d:%d\n", line, col);
  }
  va_start(ap, xx);
  vsnprintf(js->errmsg + n, sizeof(js->errmsg) - n, xx, ap);
  va_end(ap);
  n = strlen(js->errmsg);
  if (n < sizeof(js->errmsg) - 1) {
    js->errmsg[n++] = '\n';
  }
  size_t remaining = sizeof(js->errmsg) - n;
  size_t added = (size_t) snprintf(js->errmsg + n, remaining, "%s\n", error_line);
  n += added;
  if (n < sizeof(js->errmsg) - 1) {
    remaining = sizeof(js->errmsg) - n;
    for (int i = 1; i < error_col && remaining > 1; i++) {
      js->errmsg[n++] = ' ';
      remaining--;
    }
    if (remaining > 3) {
      js->errmsg[n++] = '^';
      js->errmsg[n++] = '^';
      js->errmsg[n++] = '^';
    }
    js->errmsg[n] = '\0';
  }
  js->errmsg[sizeof(js->errmsg) - 1] = '\0';
  js->pos = js->clen, js->tok = TOK_EOF, js->consumed = 0;
  return mkval(T_ERR, 0);
}

static size_t tostr(struct js *js, jsval_t value, char *buf, size_t len) {
  switch (vtype(value)) {
    case T_UNDEF: return cpy(buf, len, "undefined", 9);
    case T_NULL:  return cpy(buf, len, "null", 4);
    case T_BOOL:  return cpy(buf, len, vdata(value) & 1 ? "true" : "false", vdata(value) & 1 ? 4 : 5);
    case T_ARR:   return strarr(js, value, buf, len);
    case T_OBJ:   return strobj(js, value, buf, len);
    case T_STR:   return strstring(js, value, buf, len);
    case T_NUM:   return strnum(value, buf, len);
    case T_PROMISE: return strpromise(js, value, buf, len);
    case T_FUNC:  return strfunc(js, value, buf, len);
    case T_CFUNC: return (size_t) snprintf(buf, len, "\"c_func_0x%lx\"", (unsigned long) vdata(value));
    case T_PROP:  return (size_t) snprintf(buf, len, "PROP@%lu", (unsigned long) vdata(value));
    default:      return (size_t) snprintf(buf, len, "VTYPE%d", vtype(value));
  }
}

const char *js_str(struct js *js, jsval_t value) {
  char *buf = (char *) &js->mem[js->brk + sizeof(jsoff_t)];
  size_t len, available = js->size - js->brk - sizeof(jsoff_t);
  
  if (is_err(value)) return js->errmsg;
  if (js->brk + sizeof(jsoff_t) >= js->size) return "";
  
  len = tostr(js, value, buf, available);
  js_mkstr(js, NULL, len);
  return buf;
}

bool js_truthy(struct js *js, jsval_t v) {
  uint8_t t = vtype(v);
  return (t == T_BOOL && vdata(v) != 0) || (t == T_NUM && tod(v) != 0.0) ||
         (t == T_OBJ || t == T_FUNC || t == T_ARR) || (t == T_STR && vstrlen(js, v) > 0);
}

static jsoff_t js_alloc(struct js *js, size_t size) {
  jsoff_t ofs = js->brk;
  size = align32((jsoff_t) size);
  if (js->brk + size > js->size) return ~(jsoff_t) 0;
  js->brk += (jsoff_t) size;
  return ofs;
}

static jsval_t mkentity(struct js *js, jsoff_t b, const void *buf, size_t len) {
  jsoff_t ofs = js_alloc(js, len + sizeof(b));
  if (ofs == (jsoff_t) ~0) return js_mkerr(js, "oom");
  memcpy(&js->mem[ofs], &b, sizeof(b));
  if (buf != NULL) memmove(&js->mem[ofs + sizeof(b)], buf, len);
  if ((b & 3) == T_STR) js->mem[ofs + sizeof(b) + len - 1] = 0;
  return mkval(b & 3, ofs);
}

jsval_t js_mkstr(struct js *js, const void *ptr, size_t len) {
  jsoff_t n = (jsoff_t) (len + 1);
  return mkentity(js, (jsoff_t) ((n << 2) | T_STR), ptr, n);
}

static jsval_t mkobj(struct js *js, jsoff_t parent) {
  return mkentity(js, 0 | T_OBJ, &parent, sizeof(parent));
}

static jsval_t mkarr(struct js *js) {
  return mkobj(js, 0);
}

static bool is_const_prop(struct js *js, jsoff_t propoff) {
  for (jsoff_t off = 0; off < js->brk;) {
    jsoff_t v = loadoff(js, off);
    jsoff_t n = esize(v & ~(GCMASK | CONSTMASK));
    jsoff_t cleaned = v & ~(GCMASK | CONSTMASK);
    if ((cleaned & 3) == T_OBJ) {
      jsoff_t firstprop = cleaned & ~3U;
      if (firstprop == propoff && (v & CONSTMASK)) {
        return true;
      }
    } else if ((cleaned & 3) == T_PROP) {
      jsoff_t nextprop = cleaned & ~3U;
      if (nextprop == propoff && (v & CONSTMASK)) {
        return true;
      }
    }
    off += n;
  }
  return false;
}

static jsval_t mkprop(struct js *js, jsval_t obj, jsval_t k, jsval_t v, bool is_const) {
  jsoff_t koff = (jsoff_t) vdata(k);
  jsoff_t b, head = (jsoff_t) vdata(obj);
  char buf[sizeof(koff) + sizeof(v)];
  memcpy(&b, &js->mem[head], sizeof(b));
  memcpy(buf, &koff, sizeof(koff));
  memcpy(buf + sizeof(koff), &v, sizeof(v));
  jsoff_t brk = js->brk | T_OBJ;
  if (is_const) brk |= CONSTMASK;
  memcpy(&js->mem[head], &brk, sizeof(brk));
  return mkentity(js, (b & ~(3U | CONSTMASK)) | T_PROP, buf, sizeof(buf));
}

static jsval_t setprop(struct js *js, jsval_t obj, jsval_t k, jsval_t v) {
  jsoff_t koff = (jsoff_t) vdata(k);
  jsoff_t klen = offtolen(loadoff(js, koff));
  const char *key = (char *) &js->mem[koff + sizeof(jsoff_t)];
  jsoff_t existing = lkp(js, obj, key, klen);
  if (existing > 0) {
    if (is_const_prop(js, existing)) {
      return js_mkerr(js, "assignment to constant");
    }
    saveval(js, existing + sizeof(jsoff_t) * 2, v);
    return mkval(T_PROP, existing);
  }
  return mkprop(js, obj, k, v, false);
}

static inline jsoff_t esize(jsoff_t w) {
  jsoff_t cleaned = w & ~(GCMASK | CONSTMASK);
  switch (cleaned & 3U) {
    case T_OBJ:   return (jsoff_t) (sizeof(jsoff_t) + sizeof(jsoff_t));
    case T_PROP:  return (jsoff_t) (sizeof(jsoff_t) + sizeof(jsoff_t) + sizeof(jsval_t));
    case T_STR:   return (jsoff_t) (sizeof(jsoff_t) + align32(cleaned >> 2U));
    default:      return (jsoff_t) ~0U;
  }
}

static bool is_mem_entity(uint8_t t) {
  return t == T_OBJ || t == T_PROP || t == T_STR || t == T_FUNC || t == T_ARR || t == T_PROMISE;
}

static void js_fixup_offsets(struct js *js, jsoff_t start, jsoff_t size) {
  for (jsoff_t n, v, off = 0; off < js->brk; off += n) {
    v = loadoff(js, off);
    n = esize(v & ~(GCMASK | CONSTMASK));
    if (v & GCMASK) continue;
    
    jsoff_t flags = v & (GCMASK | CONSTMASK);
    jsoff_t cleaned = v & ~(GCMASK | CONSTMASK);
    if ((cleaned & 3) != T_OBJ && (cleaned & 3) != T_PROP) continue;
    jsoff_t adjusted = cleaned > start ? cleaned - size : cleaned;
    if (cleaned != adjusted) saveoff(js, off, adjusted | flags);
    if ((cleaned & 3) == T_OBJ) {
      jsoff_t u = loadoff(js, (jsoff_t) (off + sizeof(jsoff_t)));
      if (u > start) saveoff(js, (jsoff_t) (off + sizeof(jsoff_t)), u - size);
    }
    if ((cleaned & 3) == T_PROP) {
      jsoff_t koff = loadoff(js, (jsoff_t) (off + sizeof(off)));
      if (koff > start) saveoff(js, (jsoff_t) (off + sizeof(off)), koff - size);
      jsval_t val = loadval(js, (jsoff_t) (off + sizeof(off) + sizeof(off)));
      if (is_mem_entity(vtype(val)) && vdata(val) > start) {
        saveval(js, (jsoff_t) (off + sizeof(off) + sizeof(off)), mkval(vtype(val), (unsigned long) (vdata(val) - size)));
      }
    }
  }
  
  jsoff_t off = (jsoff_t) vdata(js->scope);
  if (off > start) js->scope = mkval(T_OBJ, off - size);
  if (js->nogc >= start) js->nogc -= size;
  if (js->code > (char *) js->mem && js->code - (char *) js->mem < js->size && js->code - (char *) js->mem > start) {
    js->code -= size;
  }
}

static void js_delete_marked_entities(struct js *js) {
  for (jsoff_t n, v, off = 0; off < js->brk; off += n) {
    v = loadoff(js, off);
    n = esize(v & ~(GCMASK | CONSTMASK));
    if (v & GCMASK) {
      js_fixup_offsets(js, off, n);
      memmove(&js->mem[off], &js->mem[off + n], js->brk - off - n);
      js->brk -= n;
      n = 0;
    }
  }
}

static void js_mark_all_entities_for_deletion(struct js *js) {
  for (jsoff_t v, off = 0; off < js->brk; off += esize(v & ~(GCMASK | CONSTMASK))) {
    v = loadoff(js, off);
    saveoff(js, off, v | GCMASK);
  }
}

static jsoff_t js_unmark_entity(struct js *js, jsoff_t off) {
  jsoff_t v = loadoff(js, off);
  if (v & GCMASK) {
    saveoff(js, off, v & ~GCMASK);
    jsoff_t cleaned = v & ~(GCMASK | CONSTMASK);
    if ((cleaned & 3) == T_OBJ) js_unmark_entity(js, cleaned & ~3);
    if ((cleaned & 3) == T_PROP) {
      js_unmark_entity(js, cleaned & ~3);
      js_unmark_entity(js, loadoff(js, (jsoff_t) (off + sizeof(off))));
      jsval_t val = loadval(js, (jsoff_t) (off + sizeof(off) + sizeof(off)));
      if (is_mem_entity(vtype(val))) js_unmark_entity(js, (jsoff_t) vdata(val));
    }
  }
  return v & ~(GCMASK | CONSTMASK | 3U);
}

static void js_unmark_used_entities(struct js *js) {
  jsval_t scope = js->scope;
  do {
    js_unmark_entity(js, (jsoff_t) vdata(scope));
    scope = upper(js, scope);
  } while (vdata(scope) != 0);
  if (js->nogc) js_unmark_entity(js, js->nogc);
}

void js_gc(struct js *js) {
  setlwm(js);
  if (js->nogc == (jsoff_t) ~0) return;
  js_mark_all_entities_for_deletion(js);
  js_unmark_used_entities(js);
  js_delete_marked_entities(js);
}

static jsoff_t skiptonext(const char *code, jsoff_t len, jsoff_t n) {
  while (n < len) {
    if (is_space(code[n])) {
      n++;
    } else if (n + 1 < len && code[n] == '/' && code[n + 1] == '/') {
      for (n += 2; n < len && code[n] != '\n';) n++;
    } else if (n + 3 < len && code[n] == '/' && code[n + 1] == '*') {
      for (n += 4; n < len && (code[n - 2] != '*' || code[n - 1] != '/');) n++;
    } else {
      break;
    }
  }
  return n;
}

static bool streq(const char *buf, size_t len, const char *p, size_t n) {
  return n == len && memcmp(buf, p, len) == 0;
}

static uint8_t parsekeyword(const char *buf, size_t len) {
  switch (buf[0]) {
    case 'a': if (streq("async", 5, buf, len)) return TOK_ASYNC; if (streq("await", 5, buf, len)) return TOK_AWAIT; break;
    case 'b': if (streq("break", 5, buf, len)) return TOK_BREAK; break;
    case 'c': if (streq("class", 5, buf, len)) return TOK_CLASS; if (streq("case", 4, buf, len)) return TOK_CASE; if (streq("catch", 5, buf, len)) return TOK_CATCH; if (streq("const", 5, buf, len)) return TOK_CONST; if (streq("continue", 8, buf, len)) return TOK_CONTINUE; break;
    case 'd': if (streq("do", 2, buf, len)) return TOK_DO;  if (streq("default", 7, buf, len)) return TOK_DEFAULT; if (streq("delete", 6, buf, len)) return TOK_DELETE; break;
    case 'e': if (streq("else", 4, buf, len)) return TOK_ELSE; break;
    case 'f': if (streq("for", 3, buf, len)) return TOK_FOR; if (streq("function", 8, buf, len)) return TOK_FUNC; if (streq("finally", 7, buf, len)) return TOK_FINALLY; if (streq("false", 5, buf, len)) return TOK_FALSE; break;
    case 'i': if (streq("if", 2, buf, len)) return TOK_IF; if (streq("in", 2, buf, len)) return TOK_IN; if (streq("instanceof", 10, buf, len)) return TOK_INSTANCEOF; break;
    case 'l': if (streq("let", 3, buf, len)) return TOK_LET; break;
    case 'n': if (streq("new", 3, buf, len)) return TOK_NEW; if (streq("null", 4, buf, len)) return TOK_NULL; break;
    case 'r': if (streq("return", 6, buf, len)) return TOK_RETURN; break;
    case 's': if (streq("switch", 6, buf, len)) return TOK_SWITCH; break;
    case 't': if (streq("try", 3, buf, len)) return TOK_TRY; if (streq("this", 4, buf, len)) return TOK_THIS; if (streq("throw", 5, buf, len)) return TOK_THROW; if (streq("true", 4, buf, len)) return TOK_TRUE; if (streq("typeof", 6, buf, len)) return TOK_TYPEOF; break;
    case 'u': if (streq("undefined", 9, buf, len)) return TOK_UNDEF; break;
    case 'v': if (streq("var", 3, buf, len)) return TOK_VAR; if (streq("void", 4, buf, len)) return TOK_VOID; break;
    case 'w': if (streq("while", 5, buf, len)) return TOK_WHILE; if (streq("with", 4, buf, len)) return TOK_WITH; break;
    case 'y': if (streq("yield", 5, buf, len)) return TOK_YIELD; break;
  }
  return TOK_IDENTIFIER;
}

static uint8_t parseident(const char *buf, jsoff_t len, jsoff_t *tlen) {
  if (is_ident_begin(buf[0])) {
    while (*tlen < len && is_ident_continue(buf[*tlen])) (*tlen)++;
    return parsekeyword(buf, *tlen);
  }
  return TOK_ERR;
}

static uint8_t next(struct js *js) {
  if (js->consumed == 0) return js->tok;
  
  js->consumed = 0;
  js->tok = TOK_ERR;
  js->toff = js->pos = skiptonext(js->code, js->clen, js->pos);
  js->tlen = 0;
  
  const char *buf = js->code + js->toff;
  if (js->toff >= js->clen) { js->tok = TOK_EOF; return js->tok; }
  
  #define TOK(T, LEN) { js->tok = T; js->tlen = (LEN); break; }
  #define LOOK(OFS, CH) js->toff + OFS < js->clen && buf[OFS] == CH
  
  switch (buf[0]) {
    case '?': if (LOOK(1, '.')) TOK(TOK_OPTIONAL_CHAIN, 2); TOK(TOK_Q, 1);
    case ':': TOK(TOK_COLON, 1);
    case '(': TOK(TOK_LPAREN, 1);
    case ')': TOK(TOK_RPAREN, 1);
    case '{': TOK(TOK_LBRACE, 1);
    case '}': TOK(TOK_RBRACE, 1);
    case '[': TOK(TOK_LBRACKET, 1);
    case ']': TOK(TOK_RBRACKET, 1);
    case ';': TOK(TOK_SEMICOLON, 1);
    case ',': TOK(TOK_COMMA, 1);
    case '!': if (LOOK(1, '=') && LOOK(2, '=')) TOK(TOK_NE, 3); if (LOOK(1, '=')) TOK(TOK_NE, 2); TOK(TOK_NOT, 1);
    case '.': if (LOOK(1, '.') && LOOK(2, '.')) TOK(TOK_REST, 3); TOK(TOK_DOT, 1);
    case '~': TOK(TOK_TILDA, 1);
    case '-': if (LOOK(1, '-')) TOK(TOK_POSTDEC, 2); if (LOOK(1, '=')) TOK(TOK_MINUS_ASSIGN, 2); TOK(TOK_MINUS, 1);
    case '+': if (LOOK(1, '+')) TOK(TOK_POSTINC, 2); if (LOOK(1, '=')) TOK(TOK_PLUS_ASSIGN, 2); TOK(TOK_PLUS, 1);
    case '*': if (LOOK(1, '*')) TOK(TOK_EXP, 2); if (LOOK(1, '=')) TOK(TOK_MUL_ASSIGN, 2); TOK(TOK_MUL, 1);
    case '/': if (LOOK(1, '=')) TOK(TOK_DIV_ASSIGN, 2); TOK(TOK_DIV, 1);
    case '%': if (LOOK(1, '=')) TOK(TOK_REM_ASSIGN, 2); TOK(TOK_REM, 1);
    case '&': if (LOOK(1, '&')) TOK(TOK_LAND, 2); if (LOOK(1, '=')) TOK(TOK_AND_ASSIGN, 2); TOK(TOK_AND, 1);
    case '|': if (LOOK(1, '|')) TOK(TOK_LOR, 2); if (LOOK(1, '=')) TOK(TOK_OR_ASSIGN, 2); TOK(TOK_OR, 1);
    case '=': if (LOOK(1, '=') && LOOK(2, '=')) TOK(TOK_EQ, 3); if (LOOK(1, '=')) TOK(TOK_EQ, 2); if (LOOK(1, '>')) TOK(TOK_ARROW, 2); TOK(TOK_ASSIGN, 1);
    case '<': if (LOOK(1, '<') && LOOK(2, '=')) TOK(TOK_SHL_ASSIGN, 3); if (LOOK(1, '<')) TOK(TOK_SHL, 2); if (LOOK(1, '=')) TOK(TOK_LE, 2); TOK(TOK_LT, 1);
    case '>': if (LOOK(1, '>') && LOOK(2, '=')) TOK(TOK_SHR_ASSIGN, 3); if (LOOK(1, '>')) TOK(TOK_SHR, 2); if (LOOK(1, '=')) TOK(TOK_GE, 2); TOK(TOK_GT, 1);
    case '^': if (LOOK(1, '=')) TOK(TOK_XOR_ASSIGN, 2); TOK(TOK_XOR, 1);
    case '`':
      js->tlen++;
      while (js->toff + js->tlen < js->clen && buf[js->tlen] != '`') {
        uint8_t increment = 1;
        if (buf[js->tlen] == '\\') {
          if (js->toff + js->tlen + 2 > js->clen) break;
          increment = 2;
        }
        js->tlen += increment;
      }
      if (buf[js->tlen] == '`') js->tok = TOK_TEMPLATE, js->tlen++;
      break;
    case '"': case '\'':
      js->tlen++;
      while (js->toff + js->tlen < js->clen && buf[js->tlen] != buf[0]) {
        uint8_t increment = 1;
        if (buf[js->tlen] == '\\') {
          if (js->toff + js->tlen + 2 > js->clen) break;
          increment = 2;
          if (buf[js->tlen + 1] == 'x') {
            if (js->toff + js->tlen + 4 > js->clen) break;
            increment = 4;
          }
        }
        js->tlen += increment;
      }
      if (buf[0] == buf[js->tlen]) js->tok = TOK_STRING, js->tlen++;
      break;
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9': {
      char *end;
      js->tval = tov(strtod(buf, &end));
      TOK(TOK_NUMBER, (jsoff_t) (end - buf));
    }
    default: js->tok = parseident(buf, js->clen - js->toff, &js->tlen); break;
  }
  
  js->pos = js->toff + js->tlen;
  return js->tok;
}

static inline uint8_t lookahead(struct js *js) {
  uint8_t old = js->tok, tok = 0;
  uint8_t old_consumed = js->consumed;
  jsoff_t pos = js->pos;
  
  js->consumed = 1;
  tok = next(js);
  js->pos = pos;
  js->tok = old;
  js->consumed = old_consumed;
  
  return tok;
}

void js_mkscope(struct js *js) {
  assert((js->flags & F_NOEXEC) == 0);
  jsoff_t prev = (jsoff_t) vdata(js->scope);
  js->scope = mkobj(js, prev);
}

void js_delscope(struct js *js) {
  js->scope = upper(js, js->scope);
}

static void mkscope(struct js *js) { js_mkscope(js); }
static void delscope(struct js *js) { js_delscope(js); }

static inline bool push_this(jsval_t this_value) {
  if (global_this_stack.depth >= global_this_stack.capacity) {
    int new_capacity = global_this_stack.capacity == 0 ? 16 : global_this_stack.capacity * 2;
    jsval_t *new_stack = (jsval_t *) realloc(global_this_stack.stack, new_capacity * sizeof(jsval_t));
    if (!new_stack) return false;
    global_this_stack.stack = new_stack;
    global_this_stack.capacity = new_capacity;
  }
  global_this_stack.stack[global_this_stack.depth++] = this_value;
  return true;
}

static inline jsval_t pop_this() {
  if (global_this_stack.depth > 0) {
    return global_this_stack.stack[--global_this_stack.depth];
  }
  return js_mkundef();
}

static inline jsval_t peek_this() {
  if (global_this_stack.depth > 0) {
    return global_this_stack.stack[global_this_stack.depth - 1];
  }
  return js_mkundef();
}

static jsval_t js_block(struct js *js, bool create_scope) {
  jsval_t res = js_mkundef();
  if (create_scope) mkscope(js);
  js->consumed = 1;
  while (next(js) != TOK_EOF && next(js) != TOK_RBRACE && !is_err(res)) {
    uint8_t t = js->tok;
    res = js_stmt(js);
    if (!is_err(res) && t != TOK_LBRACE && t != TOK_IF && t != TOK_WHILE &&
        t != TOK_FUNC && js->tok != TOK_SEMICOLON) {
      res = js_mkerr(js, "; expected");
      break;
    }
    if (js->flags & F_RETURN) break;
  }
  if (create_scope) delscope(js);
  return res;
}

static jsoff_t lkp(struct js *js, jsval_t obj, const char *buf, size_t len) {
  jsoff_t off = loadoff(js, (jsoff_t) vdata(obj)) & ~(3U | CONSTMASK);
  while (off < js->brk && off != 0) {
    jsoff_t koff = loadoff(js, (jsoff_t) (off + sizeof(off)));
    jsoff_t klen = (loadoff(js, koff) >> 2) - 1;
    const char *p = (char *) &js->mem[koff + sizeof(koff)];
    if (streq(buf, len, p, klen)) return off;
    off = loadoff(js, off) & ~(3U | CONSTMASK);
  }
  return 0;
}

static jsval_t lookup(struct js *js, const char *buf, size_t len) {
  if (js->flags & F_NOEXEC) return 0;
  
  for (jsval_t scope = js->scope;;) {
    jsoff_t off = lkp(js, scope, buf, len);
    if (off != 0) return mkval(T_PROP, off);
    if (vdata(scope) == 0) break;
    scope = mkval(T_OBJ, loadoff(js, (jsoff_t) (vdata(scope) + sizeof(jsoff_t))));
  }
  
  return js_mkerr(js, "'%.*s' not found", (int) len, buf);
}

static jsval_t resolveprop(struct js *js, jsval_t v) {
  if (vtype(v) != T_PROP) return v;
  return resolveprop(js, loadval(js, (jsoff_t) (vdata(v) + sizeof(jsoff_t) * 2)));
}

static jsval_t assign(struct js *js, jsval_t lhs, jsval_t val) {
  jsoff_t propoff = (jsoff_t) vdata(lhs);
  if (is_const_prop(js, propoff)) {
    return js_mkerr(js, "assignment to constant");
  }
  saveval(js, (jsoff_t) ((vdata(lhs) & ~3U) + sizeof(jsoff_t) * 2), val);
  return lhs;
}

static jsval_t do_assign_op(struct js *js, uint8_t op, jsval_t l, jsval_t r) {
  uint8_t m[] = {
    TOK_PLUS, TOK_MINUS, TOK_MUL, TOK_DIV, TOK_REM, TOK_SHL,
    TOK_SHR,  TOK_ZSHR,  TOK_AND, TOK_XOR, TOK_OR
  };
  
  jsval_t res = do_op(js, m[op - TOK_PLUS_ASSIGN], resolveprop(js, l), r);
  return assign(js, l, res);
}

static jsval_t do_string_op(struct js *js, uint8_t op, jsval_t l, jsval_t r) {
  jsoff_t n1, off1 = vstr(js, l, &n1);
  jsoff_t n2, off2 = vstr(js, r, &n2);
  
  if (op == TOK_PLUS) {
    jsval_t res = js_mkstr(js, NULL, n1 + n2);
    if (vtype(res) == T_STR) {
      jsoff_t n, off = vstr(js, res, &n);
      memmove(&js->mem[off], &js->mem[off1], n1);
      memmove(&js->mem[off + n1], &js->mem[off2], n2);
    }
    
    return res;
  } else if (op == TOK_EQ) {
    bool eq = n1 == n2 && memcmp(&js->mem[off1], &js->mem[off2], n1) == 0;
    return mkval(T_BOOL, eq ? 1 : 0);
  } else if (op == TOK_NE) {
    bool eq = n1 == n2 && memcmp(&js->mem[off1], &js->mem[off2], n1) == 0;
    return mkval(T_BOOL, eq ? 0 : 1);
  } else if (op == TOK_LT || op == TOK_LE || op == TOK_GT || op == TOK_GE) {
    jsoff_t min_len = n1 < n2 ? n1 : n2;
    int cmp = memcmp(&js->mem[off1], &js->mem[off2], min_len);
    
    if (cmp == 0) {
      if (n1 == n2) {
        return mkval(T_BOOL, (op == TOK_LE || op == TOK_GE) ? 1 : 0);
      } else {
        cmp = (n1 < n2) ? -1 : 1;
      }
    }
    
    switch (op) {
      case TOK_LT: return mkval(T_BOOL, cmp < 0 ? 1 : 0);
      case TOK_LE: return mkval(T_BOOL, cmp <= 0 ? 1 : 0);
      case TOK_GT: return mkval(T_BOOL, cmp > 0 ? 1 : 0);
      case TOK_GE: return mkval(T_BOOL, cmp >= 0 ? 1 : 0);
      default:     return js_mkerr(js, "bad str op");
    }
  } else {
    return js_mkerr(js, "bad str op");
  }
}

static jsval_t do_bracket_op(struct js *js, jsval_t l, jsval_t r) {
  jsval_t obj = resolveprop(js, l);
  jsval_t key_val = resolveprop(js, r);
  char keybuf[32];
  const char *keystr;
  size_t keylen;
  if (vtype(key_val) == T_NUM) {
    snprintf(keybuf, sizeof(keybuf), "%.0f", tod(key_val));
    keystr = keybuf;
    keylen = strlen(keybuf);
  } else if (vtype(key_val) == T_STR) {
    jsoff_t slen;
    jsoff_t off = vstr(js, key_val, &slen);
    keystr = (char *) &js->mem[off];
    keylen = slen;
  } else {
    return js_mkerr(js, "invalid index type");
  }
  if ((vtype(obj) == T_STR || vtype(obj) == T_ARR) && streq(keystr, keylen, "length", 6)) {
    if (vtype(obj) == T_STR) {
      return tov(offtolen(loadoff(js, (jsoff_t) vdata(obj))));
    }
  }
  if (vtype(obj) == T_STR) {
    if (vtype(key_val) != T_NUM) {
      return js_mkerr(js, "string indices must be numbers");
    }
    double idx_d = tod(key_val);
    if (idx_d < 0 || idx_d != (double)(long)idx_d) {
      return js_mkundef();
    }
    jsoff_t idx = (jsoff_t) idx_d;
    jsoff_t str_len = offtolen(loadoff(js, (jsoff_t) vdata(obj)));
    if (idx >= str_len) {
      return js_mkundef();
    }
    jsoff_t str_off = (jsoff_t) vdata(obj) + sizeof(jsoff_t);
    char ch[2] = {js->mem[str_off + idx], 0};
    return js_mkstr(js, ch, 1);
  }
  if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR) {
    return js_mkerr(js, "cannot index non-object");
  }
  jsoff_t off = lkp(js, obj, keystr, keylen);
  if (off == 0) {
    jsval_t key = js_mkstr(js, keystr, keylen);
    jsval_t prop = setprop(js, obj, key, js_mkundef());
    return prop;
  }
  return mkval(T_PROP, off);
}

static jsval_t do_dot_op(struct js *js, jsval_t l, jsval_t r) {
  const char *ptr = (char *) &js->code[coderefoff(r)];
  
  if (vtype(r) != T_CODEREF) return js_mkerr(js, "ident expected");
  if (vtype(l) == T_STR) {
    if (streq(ptr, codereflen(r), "length", 6)) {
      return tov(offtolen(loadoff(js, (jsoff_t) vdata(l))));
    } else if (streq(ptr, codereflen(r), "indexOf", 7)) {
      return js_mkfun(builtin_string_indexOf);
    } else if (streq(ptr, codereflen(r), "substring", 9)) {
      return js_mkfun(builtin_string_substring);
    } else if (streq(ptr, codereflen(r), "split", 5)) {
      return js_mkfun(builtin_string_split);
    } else if (streq(ptr, codereflen(r), "slice", 5)) {
      return js_mkfun(builtin_string_slice);
    } else if (streq(ptr, codereflen(r), "includes", 8)) {
      return js_mkfun(builtin_string_includes);
    } else if (streq(ptr, codereflen(r), "startsWith", 10)) {
      return js_mkfun(builtin_string_startsWith);
    } else if (streq(ptr, codereflen(r), "endsWith", 8)) {
      return js_mkfun(builtin_string_endsWith);
    } else if (streq(ptr, codereflen(r), "replace", 7)) {
      return js_mkfun(builtin_string_replace);
    } else if (streq(ptr, codereflen(r), "template", 8)) {
      return js_mkfun(builtin_string_template);
    } else if (streq(ptr, codereflen(r), "charCodeAt", 10)) {
      return js_mkfun(builtin_string_charCodeAt);
    }
  }
  
  if (vtype(l) == T_ARR && streq(ptr, codereflen(r), "length", 6)) {
    jsoff_t off = lkp(js, l, "length", 6);
    if (off == 0) return tov(0);
    return resolveprop(js, mkval(T_PROP, off));
  }
  
  if (vtype(l) == T_ARR) {
    if (streq(ptr, codereflen(r), "push", 4)) {
      return js_mkfun(builtin_array_push);
    } else if (streq(ptr, codereflen(r), "pop", 3)) {
      return js_mkfun(builtin_array_pop);
    } else if (streq(ptr, codereflen(r), "slice", 5)) {
      return js_mkfun(builtin_array_slice);
    } else if (streq(ptr, codereflen(r), "join", 4)) {
      return js_mkfun(builtin_array_join);
    } else if (streq(ptr, codereflen(r), "includes", 8)) {
      return js_mkfun(builtin_array_includes);
    }
  }
  
  if (vtype(l) == T_PROMISE) {
    if (streq(ptr, codereflen(r), "then", 4)) {
      return js_mkfun(builtin_promise_then);
    } else if (streq(ptr, codereflen(r), "catch", 5)) {
      return js_mkfun(builtin_promise_catch);
    } else if (streq(ptr, codereflen(r), "finally", 7)) {
      return js_mkfun(builtin_promise_finally);
    }
  }
  
  if (vtype(l) == T_FUNC) {
    jsval_t func_obj = mkval(T_OBJ, vdata(l));
    jsoff_t off = lkp(js, func_obj, ptr, codereflen(r));
    if (off == 0) {
      if (streq(ptr, codereflen(r), "name", 4)) {
        return js_mkstr(js, "", 0);
      }
      jsval_t key = js_mkstr(js, ptr, codereflen(r));
      jsval_t prop = setprop(js, func_obj, key, js_mkundef());
      return prop;
    }
    return mkval(T_PROP, off);
  }
  
  if (vtype(l) != T_OBJ && vtype(l) != T_ARR) return js_mkerr(js, "lookup in non-obj");
  jsoff_t off = lkp(js, l, ptr, codereflen(r));
  if (off == 0) {
    jsval_t key = js_mkstr(js, ptr, codereflen(r));
    jsval_t prop = setprop(js, l, key, js_mkundef());
    return prop;
  }
  
  return mkval(T_PROP, off);
}

static jsval_t do_optional_chain_op(struct js *js, jsval_t l, jsval_t r) {
  if (vtype(l) == T_NULL || vtype(l) == T_UNDEF) {
    return js_mkundef();
  }
  return do_dot_op(js, l, r);
}

static jsval_t js_call_params(struct js *js) {
  jsoff_t pos = js->pos;
  uint8_t flags = js->flags;
  js->flags |= F_NOEXEC;
  js->consumed = 1;
  
  for (bool comma = false; next(js) != TOK_EOF; comma = true) {
    if (!comma && next(js) == TOK_RPAREN) break;
    js_expr(js);
    if (next(js) == TOK_RPAREN) break;
    EXPECT(TOK_COMMA, js->flags = flags);
  }
  
  EXPECT(TOK_RPAREN, js->flags = flags);
  js->flags = flags;
  return mkcoderef(pos, js->pos - pos - js->tlen);
}

static void reverse(jsval_t *args, int nargs) {
  for (int i = 0; i < nargs / 2; i++) {
    jsval_t tmp = args[i];
    args[i] = args[nargs - i - 1], args[nargs - i - 1] = tmp;
  }
}

static jsval_t call_c(struct js *js,
  jsval_t (*fn)(struct js *, jsval_t *, int)) {
  int argc = 0;
  jsval_t target_this = peek_this();
  
  while (js->pos < js->clen) {
    if (next(js) == TOK_RPAREN) break;
    jsval_t arg = resolveprop(js, js_expr(js));
    if (js->brk + sizeof(arg) > js->size) return js_mkerr(js, "call oom");
    js->size -= (jsoff_t) sizeof(arg);
    memcpy(&js->mem[js->size], &arg, sizeof(arg));
    argc++;
    if (next(js) == TOK_COMMA) js->consumed = 1;
  }
  
  jsval_t saved_this = js->this_val;
  js->this_val = target_this;
  reverse((jsval_t *) &js->mem[js->size], argc);
  jsval_t res = fn(js, (jsval_t *) &js->mem[js->size], argc);
  js->this_val = saved_this;
  setlwm(js);
  
  js->size += (jsoff_t) sizeof(jsval_t) * (jsoff_t) argc;
  return res;
}

static jsval_t call_js(struct js *js, const char *fn, jsoff_t fnlen, jsval_t closure_scope) {
  jsoff_t fnpos = 1;
  jsval_t saved_scope = js->scope;
  jsval_t target_this = peek_this();
  jsoff_t parent_scope_offset;
  
  if (vtype(closure_scope) == T_OBJ) {
    parent_scope_offset = (jsoff_t) vdata(closure_scope);
  } else {
    parent_scope_offset = (jsoff_t) vdata(js->scope);
  }
  
  jsval_t function_scope = mkobj(js, parent_scope_offset);
  bool has_rest = false;
  jsoff_t rest_param_start = 0, rest_param_len = 0;
  
  while (fnpos < fnlen) {
    fnpos = skiptonext(fn, fnlen, fnpos);
    if (fnpos < fnlen && fn[fnpos] == ')') break;
    
    bool is_rest = false;
    if (fnpos + 3 < fnlen && fn[fnpos] == '.' && fn[fnpos + 1] == '.' && fn[fnpos + 2] == '.') {
      is_rest = true;
      has_rest = true;
      fnpos += 3;
      fnpos = skiptonext(fn, fnlen, fnpos);
    }
    
    jsoff_t identlen = 0;
    uint8_t tok = parseident(&fn[fnpos], fnlen - fnpos, &identlen);
    if (tok != TOK_IDENTIFIER) break;
    
    if (is_rest) {
      rest_param_start = fnpos;
      rest_param_len = identlen;
      fnpos = skiptonext(fn, fnlen, fnpos + identlen);
      break;
    }
    
    js->pos = skiptonext(js->code, js->clen, js->pos);
    js->consumed = 1;
    jsval_t v = js->code[js->pos] == ')' ? js_mkundef() : js_expr(js);
    setprop(js, function_scope, js_mkstr(js, &fn[fnpos], identlen), v);
    js->pos = skiptonext(js->code, js->clen, js->pos);
    if (js->pos < js->clen && js->code[js->pos] == ',') js->pos++;
    fnpos = skiptonext(fn, fnlen, fnpos + identlen);
    if (fnpos < fnlen && fn[fnpos] == ',') fnpos++;
  }
  
  if (has_rest && rest_param_len > 0) {
    jsval_t rest_array = mkarr(js);
    if (!is_err(rest_array)) {
      jsoff_t idx = 0;
      js->pos = skiptonext(js->code, js->clen, js->pos);
      
      while (js->pos < js->clen && js->code[js->pos] != ')') {
        js->consumed = 1;
        jsval_t arg = js_expr(js);
        if (!is_err(arg)) {
          char idxstr[16];
          snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) idx);
          jsval_t key = js_mkstr(js, idxstr, strlen(idxstr));
          setprop(js, rest_array, key, resolveprop(js, arg));
          idx++;
        }
        js->pos = skiptonext(js->code, js->clen, js->pos);
        if (js->pos < js->clen && js->code[js->pos] == ',') js->pos++;
      }
      
      jsval_t len_key = js_mkstr(js, "length", 6);
      setprop(js, rest_array, len_key, tov((double) idx));
      rest_array = mkval(T_ARR, vdata(rest_array));
      
      setprop(js, function_scope, js_mkstr(js, &fn[rest_param_start], rest_param_len), rest_array);
    }
  }
  
  js->scope = function_scope;
  if (fnpos < fnlen && fn[fnpos] == ')') fnpos++;
  fnpos = skiptonext(fn, fnlen, fnpos);
  if (fnpos < fnlen && fn[fnpos] == '{') fnpos++;
  size_t n = fnlen - fnpos - 1U;
  js->this_val = target_this;
  js->flags = F_CALL;
  
  jsval_t res = js_eval(js, &fn[fnpos], n);
  if (!is_err(res) && !(js->flags & F_RETURN)) res = js_mkundef();
  js->scope = saved_scope;
  return res;
}

static jsval_t do_call_op(struct js *js, jsval_t func, jsval_t args) {
  if (vtype(args) != T_CODEREF) return js_mkerr(js, "bad call");
  if (vtype(func) != T_FUNC && vtype(func) != T_CFUNC)
    return js_mkerr(js, "calling non-function");
  const char *code = js->code;
  jsoff_t clen = js->clen, pos = js->pos;
  
  js->code = &js->code[coderefoff(args)];
  js->clen = codereflen(args);
  js->pos = skiptonext(js->code, js->clen, 0);
  uint8_t tok = js->tok, flags = js->flags;
  jsoff_t nogc = js->nogc;
  jsval_t res = js_mkundef();
  
  if (vtype(func) == T_FUNC) {
    jsval_t func_obj = mkval(T_OBJ, vdata(func));
    jsoff_t native_off = lkp(js, func_obj, "__native_func", 13);
    if (native_off != 0) {
      jsval_t native_val = resolveprop(js, mkval(T_PROP, native_off));
      if (vtype(native_val) == T_CFUNC) {
        jsval_t saved_func = js->current_func;
        js->current_func = func;
        res = call_c(js, (jsval_t(*)(struct js *, jsval_t *, int)) vdata(native_val));
        js->current_func = saved_func;
      }
    } else {
      jsoff_t code_off = lkp(js, func_obj, "__code", 6);
      if (code_off == 0) return js_mkerr(js, "function has no code");
      jsval_t code_val = resolveprop(js, mkval(T_PROP, code_off));
      if (vtype(code_val) != T_STR) return js_mkerr(js, "function code not string");
      jsval_t closure_scope = js_mkundef();
      jsoff_t scope_off = lkp(js, func_obj, "__scope", 7);
      if (scope_off != 0) {
        closure_scope = resolveprop(js, mkval(T_PROP, scope_off));
      }
      jsoff_t fnlen, fnoff = vstr(js, code_val, &fnlen);
      const char *code_str = (const char *) (&js->mem[fnoff]);
      bool is_async = false;
      jsoff_t async_off = lkp(js, func_obj, "__async", 7);
      if (async_off != 0) {
        jsval_t async_val = resolveprop(js, mkval(T_PROP, async_off));
        is_async = vtype(async_val) == T_BOOL && vdata(async_val) == 1;
      }
      if (fnlen == 16 && memcmp(code_str, "__builtin_Object", 16) == 0) {
        res = call_c(js, builtin_Object);
      } else {
        js->nogc = (jsoff_t) (fnoff - sizeof(jsoff_t));
        res = call_js(js, code_str, fnlen, closure_scope);
        if (is_async && !is_err(res)) {
          jsval_t promise_args[] = { res };
          res = builtin_Promise_resolve(js, promise_args, 1);
        }
      }
    }
  } else {
    res = call_c(js, (jsval_t(*)(struct js *, jsval_t *, int)) vdata(func));
  }
  
  js->code = code, js->clen = clen, js->pos = pos;
  js->flags = flags, js->tok = tok, js->nogc = nogc;
  js->consumed = 1;
  return res;
}

static jsval_t do_op(struct js *js, uint8_t op, jsval_t lhs, jsval_t rhs) {
  if (js->flags & F_NOEXEC) return 0;
  jsval_t l = resolveprop(js, lhs), r = resolveprop(js, rhs);
  setlwm(js);
  if (is_err(l)) return l;
  if (is_err(r)) return r;
  if (is_assign(op) && vtype(lhs) != T_PROP) return js_mkerr(js, "bad lhs");
  switch (op) {
    case TOK_TYPEOF:     return js_mkstr(js, typestr(vtype(r)), strlen(typestr(vtype(r))));
    case TOK_VOID:       return js_mkundef();
    case TOK_INSTANCEOF: return do_instanceof(js, l, r);
    case TOK_CALL:       return do_call_op(js, l, r);
    case TOK_BRACKET:    return do_bracket_op(js, l, rhs);
    case TOK_ASSIGN:     return assign(js, lhs, r);
    case TOK_POSTINC: {
      if (vtype(lhs) != T_PROP) return js_mkerr(js, "bad lhs for ++");
      do_assign_op(js, TOK_PLUS_ASSIGN, lhs, tov(1)); return l;
    }
    case TOK_POSTDEC: {
      if (vtype(lhs) != T_PROP) return js_mkerr(js, "bad lhs for --");
      do_assign_op(js, TOK_MINUS_ASSIGN, lhs, tov(1)); return l;
    }
    case TOK_NOT:     return mkval(T_BOOL, !js_truthy(js, r));
  }
  if (is_assign(op))    return do_assign_op(js, op, lhs, r);
  if (op == TOK_EQ || op == TOK_NE) {
    bool eq = false;
    if (vtype(l) == vtype(r)) {
      if (vtype(l) == T_STR) {
        jsoff_t n1, off1 = vstr(js, l, &n1);
        jsoff_t n2, off2 = vstr(js, r, &n2);
        eq = n1 == n2 && memcmp(&js->mem[off1], &js->mem[off2], n1) == 0;
      } else if (vtype(l) == T_NUM) {
        eq = tod(l) == tod(r);
      } else if (vtype(l) == T_BOOL) {
        eq = vdata(l) == vdata(r);
      } else {
        eq = vdata(l) == vdata(r);
      }
    }
    return mkval(T_BOOL, op == TOK_EQ ? eq : !eq);
  }
  if (op == TOK_PLUS && (vtype(l) == T_STR || vtype(r) == T_STR)) {
    jsval_t l_str = l, r_str = r;
    if (vtype(l) != T_STR) {
      const char *str = js_str(js, l);
      l_str = js_mkstr(js, str, strlen(str));
      if (is_err(l_str)) return l_str;
    }
    if (vtype(r) != T_STR) {
      const char *str = js_str(js, r);
      r_str = js_mkstr(js, str, strlen(str));
      if (is_err(r_str)) return r_str;
    }
    return do_string_op(js, op, l_str, r_str);
  }
  if (vtype(l) == T_STR && vtype(r) == T_STR) return do_string_op(js, op, l, r);
  if (is_unary(op) && vtype(r) != T_NUM) return js_mkerr(js, "type mismatch");
  if (!is_unary(op) && op != TOK_DOT && op != TOK_OPTIONAL_CHAIN && op != TOK_INSTANCEOF && (vtype(l) != T_NUM || vtype(r) != T_NUM)) return js_mkerr(js, "type mismatch");
  
  double a = tod(l), b = tod(r);
  switch (op) {
    case TOK_DIV:     return tod(r) == 0 ? js_mkerr(js, "div by zero") : tov(a / b);
    case TOK_REM:     return tov(a - b * ((double) (long) (a / b)));
    case TOK_MUL:     return tov(a * b);
    case TOK_PLUS:    return tov(a + b);
    case TOK_MINUS:   return tov(a - b);
    case TOK_XOR:     return tov((double)((long) a ^ (long) b));
    case TOK_AND:     return tov((double)((long) a & (long) b));
    case TOK_OR:      return tov((double)((long) a | (long) b));
    case TOK_UMINUS:  return tov(-b);
    case TOK_UPLUS:   return r;
    case TOK_TILDA:   return tov((double)(~(long) b));
    case TOK_SHL:     return tov((double)((long) a << (long) b));
    case TOK_SHR:     return tov((double)((long) a >> (long) b));
    case TOK_DOT:     return do_dot_op(js, l, r);
    case TOK_OPTIONAL_CHAIN: return do_optional_chain_op(js, l, r);
    case TOK_LT:      return mkval(T_BOOL, a < b);
    case TOK_LE:      return mkval(T_BOOL, a <= b);
    case TOK_GT:      return mkval(T_BOOL, a > b);
    case TOK_GE:      return mkval(T_BOOL, a >= b);
    default:          return js_mkerr(js, "unknown op %d", (int) op);
  }
}

static jsval_t js_template_literal(struct js *js) {
  uint8_t *in = (uint8_t *) &js->code[js->toff];
  size_t template_len = js->tlen;
  
  size_t n = 1; 
  jsval_t parts[64]; 
  int part_count = 0;
  
  while (n < template_len - 1 && part_count < 64) {
    size_t part_start = n;
    while (n < template_len - 1) {
      if (in[n] == '\\' && n + 1 < template_len - 1) {
        n += 2;
        continue;
      }
      if (in[n] == '$' && n + 1 < template_len - 1 && in[n + 1] == '{') {
        break;
      }
      n++;
    }
    if (n > part_start || (n == part_start && (n >= template_len - 1 || in[n] != '$'))) {
      size_t part_len = n - part_start;
      uint8_t *out = &js->mem[js->brk + sizeof(jsoff_t)];
      size_t out_len = 0;
      
      if (js->brk + sizeof(jsoff_t) + part_len > js->size)
        return js_mkerr(js, "oom");
      
      for (size_t i = part_start; i < n; i++) {
        if (in[i] == '\\' && i + 1 < n) {
          i++;
          if (in[i] == 'n') out[out_len++] = '\n';
          else if (in[i] == 't') out[out_len++] = '\t';
          else if (in[i] == 'r') out[out_len++] = '\r';
          else if (in[i] == '\\') out[out_len++] = '\\';
          else if (in[i] == '`') out[out_len++] = '`';
          else out[out_len++] = in[i];
        } else {
          out[out_len++] = in[i];
        }
      }
      
      parts[part_count++] = js_mkstr(js, NULL, out_len);
    }
    
    if (n < template_len - 1 && in[n] == '$' && in[n + 1] == '{') {
      n += 2;
      size_t expr_start = n;
      int brace_count = 1;
      
      while (n < template_len - 1 && brace_count > 0) {
        if (in[n] == '{') brace_count++;
        else if (in[n] == '}') brace_count--;
        if (brace_count > 0) n++;
      }
      
      if (brace_count != 0) return js_mkerr(js, "unclosed ${");
      
      const char *saved_code = js->code;
      jsoff_t saved_clen = js->clen;
      jsoff_t saved_pos = js->pos;
      uint8_t saved_tok = js->tok;
      uint8_t saved_consumed = js->consumed;
      
      js->code = (const char *)&in[expr_start];
      js->clen = n - expr_start;
      js->pos = 0;
      js->consumed = 1;
      
      jsval_t expr_result = js_expr(js);
      
      js->code = saved_code;
      js->clen = saved_clen;
      js->pos = saved_pos;
      js->tok = saved_tok;
      js->consumed = saved_consumed;
      
      if (is_err(expr_result)) return expr_result;
      expr_result = resolveprop(js, expr_result);

      if (vtype(expr_result) != T_STR) {
        const char *str = js_str(js, expr_result);
        expr_result = js_mkstr(js, str, strlen(str));
      }
      
      parts[part_count++] = expr_result;
      n++;
    }
  }
  
  if (part_count == 0) return js_mkstr(js, "", 0);
  if (part_count == 1) return parts[0];
  
  size_t total_len = 0;
  for (int i = 0; i < part_count; i++) {
    if (vtype(parts[i]) == T_STR) {
      total_len += vstrlen(js, parts[i]);
    }
  }
  
  jsval_t result = js_mkstr(js, NULL, total_len);
  if (is_err(result)) return result;
  
  jsoff_t result_len, result_off = vstr(js, result, &result_len);
  size_t pos = 0;
  
  for (int i = 0; i < part_count; i++) {
    if (vtype(parts[i]) == T_STR) {
      jsoff_t part_len, part_off = vstr(js, parts[i], &part_len);
      memmove(&js->mem[result_off + pos], &js->mem[part_off], part_len);
      pos += part_len;
    }
  }
  
  return result;
}

static jsval_t js_str_literal(struct js *js) {
  uint8_t *in = (uint8_t *) &js->code[js->toff];
  uint8_t *out = &js->mem[js->brk + sizeof(jsoff_t)];
  size_t n1 = 0, n2 = 0;
  if (js->brk + sizeof(jsoff_t) + js->tlen > js->size)
    return js_mkerr(js, "oom");
  while (n2++ + 2 < js->tlen) {
    if (in[n2] == '\\') {
      if (in[n2 + 1] == in[0]) {
        out[n1++] = in[0];
      } else if (in[n2 + 1] == 'n') {
        out[n1++] = '\n';
      } else if (in[n2 + 1] == 't') {
        out[n1++] = '\t';
      } else if (in[n2 + 1] == 'r') {
        out[n1++] = '\r';
      } else if (in[n2 + 1] == 'x' && is_xdigit(in[n2 + 2]) &&
                 is_xdigit(in[n2 + 3])) {
        out[n1++] = (uint8_t) ((unhex(in[n2 + 2]) << 4U) | unhex(in[n2 + 3]));
        n2 += 2;
      } else {
        return js_mkerr(js, "bad str literal");
      }
      n2++;
    } else {
      out[n1++] = ((uint8_t *) js->code)[js->toff + n2];
    }
  }
  return js_mkstr(js, NULL, n1);
}

static jsval_t js_arr_literal(struct js *js) {
  uint8_t exe = !(js->flags & F_NOEXEC);
  jsval_t arr = exe ? mkarr(js) : js_mkundef();
  if (is_err(arr)) return arr;
  
  js->consumed = 1;
  jsoff_t idx = 0;
  while (next(js) != TOK_RBRACKET) {
    jsval_t val = js_expr(js);
    if (exe) {
      if (is_err(val)) return val;
      char idxstr[16];
      snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) idx);
      jsval_t key = js_mkstr(js, idxstr, strlen(idxstr));
      jsval_t res = setprop(js, arr, key, resolveprop(js, val));
      if (is_err(res)) return res;
    }
    idx++;
    if (next(js) == TOK_RBRACKET) break;
    EXPECT(TOK_COMMA, );
  }
  
  EXPECT(TOK_RBRACKET, );
  if (exe) {
    jsval_t len_key = js_mkstr(js, "length", 6);
    jsval_t len_val = tov((double) idx);
    jsval_t res = setprop(js, arr, len_key, len_val);
    if (is_err(res)) return res;
    arr = mkval(T_ARR, vdata(arr));
  }
  return arr;
}

static jsval_t js_obj_literal(struct js *js) {
  uint8_t exe = !(js->flags & F_NOEXEC);
  jsval_t obj = exe ? mkobj(js, 0) : js_mkundef();
  if (is_err(obj)) return obj;
  js->consumed = 1;
  
  while (next(js) != TOK_RBRACE) {
    jsval_t key = 0;
    jsoff_t id_off = 0, id_len = 0;
    
    if (js->tok == TOK_IDENTIFIER) {
      id_off = js->toff;
      id_len = js->tlen;
      if (exe) key = js_mkstr(js, js->code + js->toff, js->tlen);
    } else if (js->tok == TOK_STRING) {
      if (exe) key = js_str_literal(js);
    } else {
      return js_mkerr(js, "parse error");
    }
    js->consumed = 1;
    
    if (id_len > 0 && (next(js) == TOK_COMMA || next(js) == TOK_RBRACE)) {
      jsval_t val = lookup(js, js->code + id_off, id_len);
      if (exe) {
        if (is_err(val)) return val;
        if (is_err(key)) return key;
        jsval_t res = setprop(js, obj, key, resolveprop(js, val));
        if (is_err(res)) return res;
      }
    } else {
      EXPECT(TOK_COLON, );
      jsval_t val = js_expr(js);
      if (exe) {
        if (is_err(val)) return val;
        if (is_err(key)) return key;
        jsval_t res = setprop(js, obj, key, resolveprop(js, val));
        if (is_err(res)) return res;
      }
    }
    
    if (next(js) == TOK_RBRACE) break;
    EXPECT(TOK_COMMA, );
  }
  
  EXPECT(TOK_RBRACE, );
  return obj;
}

static jsval_t js_func_literal(struct js *js, bool is_async) {
  uint8_t flags = js->flags;
  js->consumed = 1;
  jsoff_t name_off = 0, name_len = 0;
  if (next(js) == TOK_IDENTIFIER) {
    name_off = js->toff;
    name_len = js->tlen;
    js->consumed = 1;
  }
  
  EXPECT(TOK_LPAREN, js->flags = flags);
  jsoff_t pos = js->pos - 1;
  for (bool comma = false; next(js) != TOK_EOF; comma = true) {
    if (!comma && next(js) == TOK_RPAREN) break;
    
    bool is_rest = false;
    if (next(js) == TOK_REST) {
      is_rest = true;
      js->consumed = 1;
      next(js);
    }
    
    if (next(js) != TOK_IDENTIFIER) {
      js->flags = flags;
      return js_mkerr(js, "identifier expected");
    }
    js->consumed = 1;
    
    if (is_rest && next(js) != TOK_RPAREN) {
      js->flags = flags;
      return js_mkerr(js, "rest parameter must be last");
    }
    if (next(js) == TOK_RPAREN) break;
    EXPECT(TOK_COMMA, js->flags = flags);
  }
  
  EXPECT(TOK_RPAREN, js->flags = flags);
  EXPECT(TOK_LBRACE, js->flags = flags);
  js->consumed = 0;
  js->flags |= F_NOEXEC;
  
  jsval_t res = js_block(js, false);
  if (is_err(res)) {
    js->flags = flags;
    return res;
  }
  js->flags = flags;
  jsval_t str = js_mkstr(js, &js->code[pos], js->pos - pos);
  js->consumed = 1;
  jsval_t func_obj = mkobj(js, 0);
  if (is_err(func_obj)) return func_obj;
  jsval_t code_key = js_mkstr(js, "__code", 6);
  if (is_err(code_key)) return code_key;
  jsval_t res2 = setprop(js, func_obj, code_key, str);
  if (is_err(res2)) return res2;
  
  if (is_async) {
    jsval_t async_key = js_mkstr(js, "__async", 7);
    if (is_err(async_key)) return async_key;
    jsval_t res_async = setprop(js, func_obj, async_key, js_mktrue());
    if (is_err(res_async)) return res_async;
  }
  
  if (name_len > 0) {
    jsval_t name_key = js_mkstr(js, "name", 4);
    if (is_err(name_key)) return name_key;
    jsval_t name_val = js_mkstr(js, &js->code[name_off], name_len);
    if (is_err(name_val)) return name_val;
    jsval_t res3 = setprop(js, func_obj, name_key, name_val);
    if (is_err(res3)) return res3;
  }
  
  if (!(flags & F_NOEXEC)) {
    jsval_t scope_key = js_mkstr(js, "__scope", 7);
    if (is_err(scope_key)) return scope_key;
    jsval_t res4 = setprop(js, func_obj, scope_key, js->scope);
    if (is_err(res4)) return res4;
  }
  
  return mkval(T_FUNC, (unsigned long) vdata(func_obj));
}

#define RTL_BINOP(_f1, _f2, _cond)  \
  jsval_t res = _f1(js);            \
  while (!is_err(res) && (_cond)) { \
    uint8_t op = js->tok;           \
    js->consumed = 1;               \
    jsval_t rhs = _f2(js);          \
    if (is_err(rhs)) return rhs;    \
    res = do_op(js, op, res, rhs);  \
  }                                 \
  return res;

#define LTR_BINOP(_f, _cond)        \
  jsval_t res = _f(js);             \
  while (!is_err(res) && (_cond)) { \
    uint8_t op = js->tok;           \
    js->consumed = 1;               \
    jsval_t rhs = _f(js);           \
    if (is_err(rhs)) return rhs;    \
    res = do_op(js, op, res, rhs);  \
  }                                 \
  return res;

static jsval_t js_literal(struct js *js) {
  next(js);
  setlwm(js);
  if (js->maxcss > 0 && js->css > js->maxcss) return js_mkerr(js, "C stack");
  js->consumed = 1;
  
  switch (js->tok) {
    case TOK_ERR:         return js_mkerr(js, "parse error");
    case TOK_NUMBER:      return js->tval;
    case TOK_STRING:      return js_str_literal(js);
    case TOK_TEMPLATE:    return js_template_literal(js);
    case TOK_LBRACE:      return js_obj_literal(js);
    case TOK_LBRACKET:    return js_arr_literal(js);
    case TOK_FUNC:        return js_func_literal(js, false);
    case TOK_ASYNC: {
      js->consumed = 1;
      uint8_t next_tok = next(js);
      if (next_tok == TOK_FUNC) {
        return js_func_literal(js, true);
      } else if (next_tok == TOK_LPAREN) {
        jsoff_t paren_start = js->pos - 1;
        js->consumed = 1;
        jsoff_t saved_pos = js->pos;
        uint8_t saved_tok = js->tok;
        uint8_t saved_consumed = js->consumed;
        uint8_t saved_flags = js->flags;
        int paren_depth = 1;
        js->flags |= F_NOEXEC;
        while (paren_depth > 0 && next(js) != TOK_EOF) {
          if (js->tok == TOK_LPAREN) paren_depth++;
          else if (js->tok == TOK_RPAREN) paren_depth--;
          js->consumed = 1;
        }
        jsoff_t paren_end = js->pos;
        bool is_arrow = lookahead(js) == TOK_ARROW;
        js->pos = saved_pos;
        js->tok = saved_tok;
        js->consumed = saved_consumed;
        js->flags = saved_flags;
        if (is_arrow) {
          js->flags |= F_NOEXEC;
          while (next(js) != TOK_RPAREN && next(js) != TOK_EOF) {
            js->consumed = 1;
          }
          if (next(js) != TOK_RPAREN) return js_mkerr(js, ") expected");
          js->consumed = 1;
          js->flags = saved_flags;
          if (next(js) != TOK_ARROW) return js_mkerr(js, "=> expected");
          js->consumed = 1;
          return js_arrow_func(js, paren_start, paren_end, true);
        }
        return js_mkerr(js, "async ( must be arrow function");
      } else if (next_tok == TOK_IDENTIFIER) {
        jsoff_t id_start = js->toff;
        jsoff_t id_len = js->tlen;
        js->consumed = 1;
        if (next(js) == TOK_ARROW) {
          js->consumed = 1;
          char param_buf[256];
          if (id_len + 3 > sizeof(param_buf)) return js_mkerr(js, "param too long");
          param_buf[0] = '(';
          memcpy(param_buf + 1, &js->code[id_start], id_len);
          param_buf[id_len + 1] = ')';
          param_buf[id_len + 2] = '\0';
          uint8_t flags = js->flags;
          bool is_expr = next(js) != TOK_LBRACE;
          jsoff_t body_start = is_expr ? js->toff : js->pos;
          jsval_t body_result;
          if (is_expr) {
            js->flags |= F_NOEXEC;
            body_result = js_assignment(js);
            if (is_err(body_result)) { js->flags = flags; return body_result; }
          } else {
            js->flags |= F_NOEXEC;
            js->consumed = 1;
            body_result = js_block(js, false);
            if (is_err(body_result)) { js->flags = flags; return body_result; }
            if (next(js) == TOK_RBRACE) js->consumed = 1;
          }
          js->flags = flags;
          jsoff_t body_end = js->pos;
          size_t fn_size = id_len + (body_end - body_start) + 64;
          char *fn_str = (char *) malloc(fn_size);
          if (!fn_str) return js_mkerr(js, "oom");
          jsoff_t fn_pos = 0;
          memcpy(fn_str + fn_pos, param_buf, id_len + 2);
          fn_pos += id_len + 2;
          fn_str[fn_pos++] = '{';
          if (is_expr) {
            memcpy(fn_str + fn_pos, "return ", 7);
            fn_pos += 7;
            size_t body_len = body_end - body_start;
            memcpy(fn_str + fn_pos, &js->code[body_start], body_len);
            fn_pos += body_len;
          } else {
            size_t body_len = body_end - body_start - 2;
            memcpy(fn_str + fn_pos, &js->code[body_start + 1], body_len);
            fn_pos += body_len;
          }
          fn_str[fn_pos++] = '}';
          jsval_t str = js_mkstr(js, fn_str, fn_pos);
          free(fn_str);
          if (is_err(str)) return str;
          jsval_t func_obj = mkobj(js, 0);
          if (is_err(func_obj)) return func_obj;
          jsval_t code_key = js_mkstr(js, "__code", 6);
          if (is_err(code_key)) return code_key;
          jsval_t res = setprop(js, func_obj, code_key, str);
          if (is_err(res)) return res;
          jsval_t async_key = js_mkstr(js, "__async", 7);
          if (is_err(async_key)) return async_key;
          jsval_t res_async = setprop(js, func_obj, async_key, js_mktrue());
          if (is_err(res_async)) return res_async;
          if (!(flags & F_NOEXEC)) {
            jsval_t scope_key = js_mkstr(js, "__scope", 7);
            if (is_err(scope_key)) return scope_key;
            jsval_t res2 = setprop(js, func_obj, scope_key, js->scope);
            if (is_err(res2)) return res2;
          }
          return mkval(T_FUNC, (unsigned long) vdata(func_obj));
        }
        return mkcoderef((jsoff_t) id_start, (jsoff_t) id_len);
      }
      return js_mkerr(js, "unexpected token after async");
    }
    case TOK_NULL:        return js_mknull();
    case TOK_UNDEF:       return js_mkundef();
    case TOK_TRUE:        return js_mktrue();
    case TOK_FALSE:       return js_mkfalse();
    case TOK_THIS:        return js->this_val;
    case TOK_IDENTIFIER:  return mkcoderef((jsoff_t) js->toff, (jsoff_t) js->tlen);
    case TOK_CATCH:       return mkcoderef((jsoff_t) js->toff, (jsoff_t) js->tlen);
    case TOK_TRY:         return mkcoderef((jsoff_t) js->toff, (jsoff_t) js->tlen);
    case TOK_FINALLY:     return mkcoderef((jsoff_t) js->toff, (jsoff_t) js->tlen);
    default:              return js_mkerr(js, "bad expr");
  }
}

static jsval_t js_arrow_func(struct js *js, jsoff_t params_start, jsoff_t params_end, bool is_async) {
  uint8_t flags = js->flags;
  bool is_expr = next(js) != TOK_LBRACE;
  jsoff_t body_start, body_end_actual;
  jsval_t body_result;
  
  if (is_expr) {
    body_start = js->toff;
    js->flags |= F_NOEXEC;
    body_result = js_assignment(js);
    if (is_err(body_result)) {
      js->flags = flags;
      return body_result;
    }
    uint8_t tok = next(js);
    if (tok == TOK_RPAREN || tok == TOK_RBRACE || tok == TOK_SEMICOLON || 
        tok == TOK_COMMA || tok == TOK_EOF) {
      body_end_actual = js->toff;
    } else {
      body_end_actual = js->pos;
    }
  } else {
    body_start = js->toff;
    js->flags |= F_NOEXEC;
    js->consumed = 1;
    body_result = js_block(js, false);
    if (is_err(body_result)) {
      js->flags = flags;
      return body_result;
    }
    if (next(js) == TOK_RBRACE) {
      body_end_actual = js->toff;
      js->consumed = 1;
    } else {
      body_end_actual = js->pos;
    }
  }
  
  js->flags = flags;
  
  size_t fn_size = (params_end - params_start) + (body_end_actual - body_start) + 32;
  char *fn_str = (char *) malloc(fn_size);
  if (!fn_str) return js_mkerr(js, "oom");
  
  jsoff_t fn_pos = 0;
  
  size_t param_len = params_end - params_start;
  memcpy(fn_str + fn_pos, &js->code[params_start], param_len);
  fn_pos += param_len;
  
  fn_str[fn_pos++] = '{';
  
  if (is_expr) {
    memcpy(fn_str + fn_pos, "return ", 7);
    fn_pos += 7;
    size_t body_len = body_end_actual - body_start;
    memcpy(fn_str + fn_pos, &js->code[body_start], body_len);
    fn_pos += body_len;
  } else {
    size_t body_len = body_end_actual - body_start - 1;
    memcpy(fn_str + fn_pos, &js->code[body_start + 1], body_len);
    fn_pos += body_len;
  }
  
  fn_str[fn_pos++] = '}';
  
  jsval_t str = js_mkstr(js, fn_str, fn_pos);
  free(fn_str);
  if (is_err(str)) return str;
  
  jsval_t func_obj = mkobj(js, 0);
  if (is_err(func_obj)) return func_obj;
  
  jsval_t code_key = js_mkstr(js, "__code", 6);
  if (is_err(code_key)) return code_key;
  
  jsval_t res = setprop(js, func_obj, code_key, str);
  if (is_err(res)) return res;
  
  if (is_async) {
    jsval_t async_key = js_mkstr(js, "__async", 7);
    if (is_err(async_key)) return async_key;
    jsval_t res_async = setprop(js, func_obj, async_key, js_mktrue());
    if (is_err(res_async)) return res_async;
  }
  
  if (!(flags & F_NOEXEC)) {
    jsval_t scope_key = js_mkstr(js, "__scope", 7);
    if (is_err(scope_key)) return scope_key;
    jsval_t res2 = setprop(js, func_obj, scope_key, js->scope);
    if (is_err(res2)) return res2;
  }
  
  return mkval(T_FUNC, (unsigned long) vdata(func_obj));
}

static jsval_t js_group(struct js *js) {
  if (next(js) == TOK_LPAREN) {
    jsoff_t paren_start = js->pos - 1;
    js->consumed = 1;
    
    jsoff_t saved_pos = js->pos;
    uint8_t saved_tok = js->tok;
    uint8_t saved_consumed = js->consumed;
    uint8_t saved_flags = js->flags;
    
    int paren_depth = 1;
    bool could_be_arrow = true;
    js->flags |= F_NOEXEC;
    
    while (paren_depth > 0 && next(js) != TOK_EOF) {
      if (js->tok == TOK_LPAREN) paren_depth++;
      else if (js->tok == TOK_RPAREN) paren_depth--;
      
      if (paren_depth > 0) {
        if (js->tok != TOK_IDENTIFIER && js->tok != TOK_COMMA && js->tok != TOK_REST) could_be_arrow = false;
      }
      js->consumed = 1;
    }
    
    jsoff_t paren_end = js->pos;
    bool is_arrow = could_be_arrow && lookahead(js) == TOK_ARROW;
    
    js->pos = saved_pos;
    js->tok = saved_tok;
    js->consumed = saved_consumed;
    js->flags = saved_flags;
    
    if (is_arrow) {
      js->flags |= F_NOEXEC;
      while (next(js) != TOK_RPAREN && next(js) != TOK_EOF) {
        js->consumed = 1;
      }
      if (next(js) != TOK_RPAREN) return js_mkerr(js, ") expected");
      js->consumed = 1;
      js->flags = saved_flags;
      
      if (next(js) != TOK_ARROW) return js_mkerr(js, "=> expected");
      js->consumed = 1;
      
      return js_arrow_func(js, paren_start, paren_end, false);
    } else {
      jsval_t v = js_expr(js);
      if (is_err(v)) return v;
      if (next(js) != TOK_RPAREN) return js_mkerr(js, ") expected");
      js->consumed = 1;
      return v;
    }
  } else {
    return js_literal(js);
  }
}

static jsval_t js_call_dot(struct js *js) {
  jsval_t res = js_group(js);
  jsval_t obj = js_mkundef();
  if (is_err(res)) return res;
  if (vtype(res) == T_CODEREF) {
    if (lookahead(js) == TOK_ARROW) return res;
    res = lookup(js, &js->code[coderefoff(res)], codereflen(res));
  }
  while (next(js) == TOK_LPAREN || next(js) == TOK_DOT || next(js) == TOK_OPTIONAL_CHAIN || next(js) == TOK_LBRACKET) {
    if (js->tok == TOK_DOT || js->tok == TOK_OPTIONAL_CHAIN) {
      uint8_t op = js->tok;
      js->consumed = 1;
      if (vtype(res) != T_PROP) {
        obj = res;
      } else {
        obj = resolveprop(js, res);
      }
      if (op == TOK_OPTIONAL_CHAIN && (vtype(obj) == T_NULL || vtype(obj) == T_UNDEF)) {
        js_group(js);
        res = js_mkundef();
      } else {
        res = do_op(js, op, res, js_group(js));
      }
    } else if (js->tok == TOK_LBRACKET) {
      js->consumed = 1;
      if (vtype(res) != T_PROP) {
        obj = res;
      } else {
        obj = resolveprop(js, res);
      }
      jsval_t idx = js_expr(js);
      if (is_err(idx)) return idx;
      if (next(js) != TOK_RBRACKET) return js_mkerr(js, "] expected");
      js->consumed = 1;
      res = do_op(js, TOK_BRACKET, res, idx);
    } else {
      jsval_t func_this = (vtype(obj) != T_UNDEF) ? obj : js->this_val;
      push_this(func_this);
      jsval_t params = js_call_params(js);
      if (is_err(params)) {
        pop_this();
        return params;
      }
      res = do_op(js, TOK_CALL, res, params);
      pop_this();
      obj = js_mkundef();
    }
  }
  return res;
}

static jsval_t js_postfix(struct js *js) {
  jsval_t res = js_call_dot(js);
  if (is_err(res)) return res;
  next(js);
  if (js->tok == TOK_POSTINC || js->tok == TOK_POSTDEC) {
    js->consumed = 1;
    res = do_op(js, js->tok, res, 0);
  }
  return res;
}

static jsval_t js_unary(struct js *js) {
  if (next(js) == TOK_NEW) {
    js->consumed = 1;
    jsval_t obj = mkobj(js, 0);
    jsval_t saved_this = js->this_val;
    js->this_val = obj;
    jsval_t result = js_postfix(js);
    jsval_t constructed_obj = js->this_val;
    js->this_val = saved_this;
    if (vtype(result) == T_OBJ || vtype(result) == T_ARR || vtype(result) == T_PROMISE) return result;
    return constructed_obj;
  } else if (next(js) == TOK_DELETE) {
    js->consumed = 1;
    jsoff_t save_pos = js->pos;
    uint8_t save_tok = js->tok;
    jsval_t operand = js_postfix(js);
    if (js->flags & F_NOEXEC) return js_mktrue();
    if (vtype(operand) != T_PROP) {
      return js_mktrue();
    }
    jsoff_t prop_off = (jsoff_t) vdata(operand);
    if (is_const_prop(js, prop_off)) {
      return js_mkerr(js, "cannot delete constant property");
    }
    jsval_t owner_obj = js_mkundef();
    for (jsoff_t off = 0; off < js->brk;) {
      jsoff_t v = loadoff(js, off);
      jsoff_t cleaned = v & ~(GCMASK | CONSTMASK);
      jsoff_t n = esize(cleaned);
      if ((cleaned & 3) == T_OBJ) {
        jsoff_t first_prop = cleaned & ~3U;
        if (first_prop == prop_off) {
          owner_obj = mkval(T_OBJ, off);
          break;
        }
      } else if ((cleaned & 3) == T_PROP) {
        jsoff_t next_prop = cleaned & ~3U;
        if (next_prop == prop_off) {
          jsoff_t deleted_next = loadoff(js, prop_off) & ~(GCMASK | CONSTMASK);
          jsoff_t current = loadoff(js, off);
          saveoff(js, off, (deleted_next & ~3U) | (current & (GCMASK | CONSTMASK | 3U)));
          saveoff(js, prop_off, loadoff(js, prop_off) | GCMASK);
          js_gc(js);
          return js_mktrue();
        }
      }
      off += n;
    }
    if (vtype(owner_obj) == T_OBJ) {
      jsoff_t obj_off = (jsoff_t) vdata(owner_obj);
      jsoff_t deleted_next = loadoff(js, prop_off) & ~(GCMASK | CONSTMASK);
      jsoff_t current = loadoff(js, obj_off);
      saveoff(js, obj_off, (deleted_next & ~3U) | (current & (GCMASK | CONSTMASK | 3U)));
      saveoff(js, prop_off, loadoff(js, prop_off) | GCMASK);
      js_gc(js);
    }
    (void) save_pos;
    (void) save_tok;
    return js_mktrue();
  } else if (next(js) == TOK_NOT || js->tok == TOK_TILDA || js->tok == TOK_TYPEOF ||
      js->tok == TOK_VOID || js->tok == TOK_MINUS || js->tok == TOK_PLUS) {
    uint8_t t = js->tok;
    if (t == TOK_MINUS) t = TOK_UMINUS;
    if (t == TOK_PLUS) t = TOK_UPLUS;
    js->consumed = 1;
    return do_op(js, t, js_mkundef(), js_unary(js));
  } else {
    return js_postfix(js);
  }
}

static jsval_t js_mul_div_rem(struct js *js) {
  LTR_BINOP(js_unary, (next(js) == TOK_MUL || js->tok == TOK_DIV || js->tok == TOK_REM));
}

static jsval_t js_plus_minus(struct js *js) {
  LTR_BINOP(js_mul_div_rem, (next(js) == TOK_PLUS || js->tok == TOK_MINUS));
}

static jsval_t js_shifts(struct js *js) {
  LTR_BINOP(js_plus_minus, 
  (next(js) == TOK_SHR || next(js) == TOK_SHL ||
   next(js) == TOK_ZSHR));
}

static jsval_t js_comparison(struct js *js) {
  LTR_BINOP(js_shifts, 
  (next(js) == TOK_LT || next(js) == TOK_LE ||
   next(js) == TOK_GT || next(js) == TOK_GE ||
   next(js) == TOK_INSTANCEOF));
}

static jsval_t js_equality(struct js *js) {
  LTR_BINOP(js_comparison, (next(js) == TOK_EQ || next(js) == TOK_NE));
}

static jsval_t js_bitwise_and(struct js *js) {
  LTR_BINOP(js_equality, (next(js) == TOK_AND));
}

static jsval_t js_bitwise_xor(struct js *js) {
  LTR_BINOP(js_bitwise_and, (next(js) == TOK_XOR));
}

static jsval_t js_bitwise_or(struct js *js) {
  LTR_BINOP(js_bitwise_xor, (next(js) == TOK_OR));
}

static jsval_t js_logical_and(struct js *js) {
  jsval_t res = js_bitwise_or(js);
  if (is_err(res)) return res;
  uint8_t flags = js->flags;
  while (next(js) == TOK_LAND) {
    js->consumed = 1;
    res = resolveprop(js, res);
    if (!js_truthy(js, res)) js->flags |= F_NOEXEC;
    if (js->flags & F_NOEXEC) {
      js_logical_and(js);
    } else {
      res = js_logical_and(js);
    }
  }
  js->flags = flags;
  return res;
}

static jsval_t js_logical_or(struct js *js) {
  jsval_t res = js_logical_and(js);
  if (is_err(res)) return res;
  uint8_t flags = js->flags;
  while (next(js) == TOK_LOR) {
    js->consumed = 1;
    res = resolveprop(js, res);
    if (js_truthy(js, res)) js->flags |= F_NOEXEC;
    if (js->flags & F_NOEXEC) {
      js_logical_or(js);
    } else {
      res = js_logical_or(js);
    }
  }
  js->flags = flags;
  return res;
}

static jsval_t js_ternary(struct js *js) {
  jsval_t res = js_logical_or(js);
  if (next(js) == TOK_Q) {
    uint8_t flags = js->flags;
    js->consumed = 1;
    if (js_truthy(js, resolveprop(js, res))) {
      res = js_ternary(js);
      js->flags |= F_NOEXEC;
      EXPECT(TOK_COLON, js->flags = flags);
      js_ternary(js);
      js->flags = flags;
    } else {
      js->flags |= F_NOEXEC;
      js_ternary(js);
      EXPECT(TOK_COLON, js->flags = flags);
      js->flags = flags;
      res = js_ternary(js);
    }
  }
  return res;
}

static jsval_t js_assignment(struct js *js) {
  jsval_t res = js_ternary(js);
  
  if (!is_err(res) && vtype(res) == T_CODEREF && next(js) == TOK_ARROW) {
    jsoff_t param_start = coderefoff(res);
    jsoff_t param_len = codereflen(res);
    js->consumed = 1;
    
    char param_buf[256];
    if (param_len + 3 > sizeof(param_buf)) return js_mkerr(js, "param too long");
    param_buf[0] = '(';
    memcpy(param_buf + 1, &js->code[param_start], param_len);
    param_buf[param_len + 1] = ')';
    param_buf[param_len + 2] = '\0';
    
    uint8_t flags = js->flags;
    bool is_expr = next(js) != TOK_LBRACE;
    jsoff_t body_start = js->pos;
    if (is_expr && js->tok != TOK_EOF) {
      body_start = js->toff;
    }
    jsval_t body_result;
    
    if (is_expr) {
      js->flags |= F_NOEXEC;
      body_result = js_assignment(js);
      if (is_err(body_result)) {
        js->flags = flags;
        return body_result;
      }
    } else {
      js->flags |= F_NOEXEC;
      js->consumed = 1;
      body_result = js_block(js, false);
      if (is_err(body_result)) {
        js->flags = flags;
        return body_result;
      }
      if (next(js) == TOK_RBRACE) js->consumed = 1;
    }
    
    js->flags = flags;
    jsoff_t body_end = js->pos;
    
    size_t fn_size = param_len + (body_end - body_start) + 64;
    char *fn_str = (char *) malloc(fn_size);
    if (!fn_str) return js_mkerr(js, "oom");
    
    jsoff_t fn_pos = 0;
    memcpy(fn_str + fn_pos, param_buf, param_len + 2);
    fn_pos += param_len + 2;
    fn_str[fn_pos++] = '{';
    
    if (is_expr) {
      memcpy(fn_str + fn_pos, "return ", 7);
      fn_pos += 7;
      size_t body_len = body_end - body_start;
      memcpy(fn_str + fn_pos, &js->code[body_start], body_len);
      fn_pos += body_len;
    } else {
      size_t body_len = body_end - body_start - 2;
      memcpy(fn_str + fn_pos, &js->code[body_start + 1], body_len);
      fn_pos += body_len;
    }
    
    fn_str[fn_pos++] = '}';
    
    jsval_t str = js_mkstr(js, fn_str, fn_pos);
    free(fn_str);
    if (is_err(str)) return str;
    
    jsval_t func_obj = mkobj(js, 0);
    if (is_err(func_obj)) return func_obj;
    
    jsval_t code_key = js_mkstr(js, "__code", 6);
    if (is_err(code_key)) return code_key;
    
    jsval_t res2 = setprop(js, func_obj, code_key, str);
    if (is_err(res2)) return res2;
    
    if (!(flags & F_NOEXEC)) {
      jsval_t scope_key = js_mkstr(js, "__scope", 7);
      if (is_err(scope_key)) return scope_key;
      jsval_t res3 = setprop(js, func_obj, scope_key, js->scope);
      if (is_err(res3)) return res3;
    }
    
    return mkval(T_FUNC, (unsigned long) vdata(func_obj));
  }
  
  while (!is_err(res) && (next(js) == TOK_ASSIGN || js->tok == TOK_PLUS_ASSIGN ||
   js->tok == TOK_MINUS_ASSIGN || js->tok == TOK_MUL_ASSIGN ||
   js->tok == TOK_DIV_ASSIGN || js->tok == TOK_REM_ASSIGN ||
   js->tok == TOK_SHL_ASSIGN || js->tok == TOK_SHR_ASSIGN ||
   js->tok == TOK_ZSHR_ASSIGN || js->tok == TOK_AND_ASSIGN ||
   js->tok == TOK_XOR_ASSIGN || js->tok == TOK_OR_ASSIGN)) {
    uint8_t op = js->tok;
    js->consumed = 1;
    jsval_t rhs = js_assignment(js);
    if (is_err(rhs)) return rhs;
    res = do_op(js, op, res, rhs);
  }
  
  return res;
}

static jsval_t js_decl(struct js *js, bool is_const) {
  uint8_t exe = !(js->flags & F_NOEXEC);
  js->consumed = 1;
  for (;;) {
    if (next(js) == TOK_LBRACE) {
      js->consumed = 1;
      
      typedef struct { jsoff_t name_off; jsoff_t name_len; } PropName;
      PropName props[32];
      int prop_count = 0;
      
      while (next(js) != TOK_RBRACE && next(js) != TOK_EOF && prop_count < 32) {
        EXPECT(TOK_IDENTIFIER, );
        props[prop_count].name_off = js->toff;
        props[prop_count].name_len = js->tlen;
        prop_count++;
        js->consumed = 1;
        
        if (next(js) == TOK_COLON) {
          js->consumed = 1;
          EXPECT(TOK_IDENTIFIER, );
          js->consumed = 1;
        }
        
        if (next(js) == TOK_RBRACE) break;
        EXPECT(TOK_COMMA, );
      }
      
      EXPECT(TOK_RBRACE, );
      
      jsval_t v = js_mkundef();
      if (next(js) == TOK_ASSIGN) {
        js->consumed = 1;
        v = js_expr(js);
        if (is_err(v)) return v;
      } else {
        return js_mkerr(js, "destructuring requires assignment");
      }
      
      if (exe) {
        jsval_t obj = resolveprop(js, v);
        if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR) {
          return js_mkerr(js, "cannot destructure non-object");
        }
        
        for (int i = 0; i < prop_count; i++) {
          const char *prop_name = &js->code[props[i].name_off];
          jsoff_t prop_len = props[i].name_len;
          
          if (lkp(js, js->scope, prop_name, prop_len) > 0) {
            return js_mkerr(js, "'%.*s' already declared", (int) prop_len, prop_name);
          }
          
          jsoff_t prop_off = lkp(js, obj, prop_name, prop_len);
          jsval_t prop_val = js_mkundef();
          
          if (prop_off > 0) {
            prop_val = resolveprop(js, mkval(T_PROP, prop_off));
          }
          
          jsval_t x = mkprop(js, js->scope, js_mkstr(js, prop_name, prop_len), prop_val, is_const);
          if (is_err(x)) return x;
        }
      }
    } else {
      EXPECT(TOK_IDENTIFIER, );
      js->consumed = 0;
      jsoff_t noff = js->toff, nlen = js->tlen;
      char *name = (char *) &js->code[noff];
      jsval_t v = js_mkundef();
      js->consumed = 1;
      if (next(js) == TOK_ASSIGN) {
        js->consumed = 1;
        v = js_expr(js);
        if (is_err(v)) return v;
      }
      if (exe) {
        if (lkp(js, js->scope, name, nlen) > 0) return js_mkerr(js, "'%.*s' already declared", (int) nlen, name);
        jsval_t x = mkprop(js, js->scope, js_mkstr(js, name, nlen), resolveprop(js, v), is_const);
        if (is_err(x)) return x;
      }
    }
    
    if (next(js) == TOK_SEMICOLON || next(js) == TOK_EOF) break;
    EXPECT(TOK_COMMA, );
  }
  return js_mkundef();
}

static jsval_t js_expr(struct js *js) {
  return js_assignment(js);
}

static jsval_t js_let(struct js *js) {
  return js_decl(js, false);
}

static jsval_t js_const(struct js *js) {
  return js_decl(js, true);
}

static jsval_t js_func_decl(struct js *js) {
  uint8_t exe = !(js->flags & F_NOEXEC);
  js->consumed = 1;
  EXPECT(TOK_IDENTIFIER, );
  js->consumed = 0;
  jsoff_t noff = js->toff, nlen = js->tlen;
  char *name = (char *) &js->code[noff];
  js->consumed = 1;
  EXPECT(TOK_LPAREN, );
  jsoff_t pos = js->pos - 1;
  for (bool comma = false; next(js) != TOK_EOF; comma = true) {
    if (!comma && next(js) == TOK_RPAREN) break;
    
    bool is_rest = false;
    if (next(js) == TOK_REST) {
      is_rest = true;
      js->consumed = 1;
      next(js);
    }
    
    if (next(js) != TOK_IDENTIFIER) return js_mkerr(js, "identifier expected");
    js->consumed = 1;
    
    if (is_rest && next(js) != TOK_RPAREN) {
      return js_mkerr(js, "rest parameter must be last");
    }
    if (next(js) == TOK_RPAREN) break;
    EXPECT(TOK_COMMA, );
  }
  EXPECT(TOK_RPAREN, );
  EXPECT(TOK_LBRACE, );
  js->consumed = 0;
  uint8_t flags = js->flags;
  js->flags |= F_NOEXEC;
  jsval_t res = js_block(js, false);
  if (is_err(res)) {
    js->flags = flags;
    return res;
  }
  js->flags = flags;
  jsval_t str = js_mkstr(js, &js->code[pos], js->pos - pos);
  jsval_t func_obj = mkobj(js, 0);
  if (is_err(func_obj)) return func_obj;
  jsval_t code_key = js_mkstr(js, "__code", 6);
  if (is_err(code_key)) return code_key;
  jsval_t res2 = setprop(js, func_obj, code_key, str);
  if (is_err(res2)) return res2;
  jsval_t name_key = js_mkstr(js, "name", 4);
  if (is_err(name_key)) return name_key;
  jsval_t name_val = js_mkstr(js, name, nlen);
  if (is_err(name_val)) return name_val;
  jsval_t res3 = setprop(js, func_obj, name_key, name_val);
  if (is_err(res3)) return res3;
  if (exe) {
    jsval_t scope_key = js_mkstr(js, "__scope", 7);
    if (is_err(scope_key)) return scope_key;
    jsval_t res4 = setprop(js, func_obj, scope_key, js->scope);
    if (is_err(res4)) return res4;
  }
  jsval_t func = mkval(T_FUNC, (unsigned long) vdata(func_obj));
  if (exe) {
    if (lkp(js, js->scope, name, nlen) > 0)
      return js_mkerr(js, "'%.*s' already declared", (int) nlen, name);
    jsval_t x = mkprop(js, js->scope, js_mkstr(js, name, nlen), func, false);
    if (is_err(x)) return x;
  }
  
  return js_mkundef();
}

static jsval_t js_func_decl_async(struct js *js) {
  uint8_t exe = !(js->flags & F_NOEXEC);
  js->consumed = 1;
  EXPECT(TOK_IDENTIFIER, );
  js->consumed = 0;
  jsoff_t noff = js->toff, nlen = js->tlen;
  char *name = (char *) &js->code[noff];
  js->consumed = 1;
  EXPECT(TOK_LPAREN, );
  jsoff_t pos = js->pos - 1;
  for (bool comma = false; next(js) != TOK_EOF; comma = true) {
    if (!comma && next(js) == TOK_RPAREN) break;
    EXPECT(TOK_IDENTIFIER, );
    if (next(js) == TOK_RPAREN) break;
    EXPECT(TOK_COMMA, );
  }
  EXPECT(TOK_RPAREN, );
  EXPECT(TOK_LBRACE, );
  js->consumed = 0;
  uint8_t flags = js->flags;
  js->flags |= F_NOEXEC;
  jsval_t res = js_block(js, false);
  if (is_err(res)) {
    js->flags = flags;
    return res;
  }
  js->flags = flags;
  jsval_t str = js_mkstr(js, &js->code[pos], js->pos - pos);
  jsval_t func_obj = mkobj(js, 0);
  if (is_err(func_obj)) return func_obj;
  jsval_t code_key = js_mkstr(js, "__code", 6);
  if (is_err(code_key)) return code_key;
  jsval_t res2 = setprop(js, func_obj, code_key, str);
  if (is_err(res2)) return res2;
  jsval_t async_key = js_mkstr(js, "__async", 7);
  if (is_err(async_key)) return async_key;
  jsval_t res_async = setprop(js, func_obj, async_key, js_mktrue());
  if (is_err(res_async)) return res_async;
  jsval_t name_key = js_mkstr(js, "name", 4);
  if (is_err(name_key)) return name_key;
  jsval_t name_val = js_mkstr(js, name, nlen);
  if (is_err(name_val)) return name_val;
  jsval_t res3 = setprop(js, func_obj, name_key, name_val);
  if (is_err(res3)) return res3;
  if (exe) {
    jsval_t scope_key = js_mkstr(js, "__scope", 7);
    if (is_err(scope_key)) return scope_key;
    jsval_t res4 = setprop(js, func_obj, scope_key, js->scope);
    if (is_err(res4)) return res4;
  }
  jsval_t func = mkval(T_FUNC, (unsigned long) vdata(func_obj));
  if (exe) {
    if (lkp(js, js->scope, name, nlen) > 0)
      return js_mkerr(js, "'%.*s' already declared", (int) nlen, name);
    jsval_t x = mkprop(js, js->scope, js_mkstr(js, name, nlen), func, false);
    if (is_err(x)) return x;
  }
  
  return js_mkundef();
}

static jsval_t js_block_or_stmt(struct js *js) {
  if (next(js) == TOK_LBRACE) return js_block(js, !(js->flags & F_NOEXEC));
  jsval_t res = resolveprop(js, js_stmt(js));
  js->consumed = 0;
  return res;
}

static jsval_t js_if(struct js *js) {
  js->consumed = 1;
  EXPECT(TOK_LPAREN, );
  jsval_t res = js_mkundef(), cond = resolveprop(js, js_expr(js));
  EXPECT(TOK_RPAREN, );
  
  bool cond_true = js_truthy(js, cond), exe = !(js->flags & F_NOEXEC);
  if (!cond_true) js->flags |= F_NOEXEC;
  jsval_t blk = js_block_or_stmt(js);
  if (cond_true) res = blk;
  if (exe && !cond_true) js->flags &= (uint8_t) ~F_NOEXEC;
  
  if (lookahead(js) == TOK_ELSE) {
    js->consumed = 1;
    next(js);
    js->consumed = 1;
    if (cond_true) js->flags |= F_NOEXEC;
    blk = js_block_or_stmt(js);
    if (!cond_true) res = blk;
    if (cond_true && exe) js->flags &= (uint8_t) ~F_NOEXEC;
  }
  
  return res;
}

static inline bool expect(struct js *js, uint8_t tok, jsval_t *res) {
  if (next(js) != tok) {
    *res = js_mkerr(js, "parse error");
    return false;
  } else {
    js->consumed = 1;
    return true;
  }
}

static inline bool is_err2(jsval_t *v, jsval_t *res) {
  bool r = is_err(*v);
  if (r) *res = *v;
  return r;
}

static jsval_t js_for(struct js *js) {
  uint8_t flags = js->flags, exe = !(flags & F_NOEXEC);
  jsval_t v, res = js_mkundef();
  jsoff_t pos1 = 0, pos2 = 0, pos3 = 0, pos4 = 0;
  if (exe) mkscope(js);
  if (!expect(js, TOK_FOR, &res)) goto done;
  if (!expect(js, TOK_LPAREN, &res)) goto done;
  if (next(js) == TOK_SEMICOLON) {
  } else if (next(js) == TOK_LET) {
    v = js_let(js);
    if (is_err2(&v, &res)) goto done;
  } else if (next(js) == TOK_CONST) {
    v = js_const(js);
    if (is_err2(&v, &res)) goto done;
  } else {
    v = js_expr(js);
    if (is_err2(&v, &res)) goto done;
  }
  if (!expect(js, TOK_SEMICOLON, &res)) goto done;
  js->flags |= F_NOEXEC;
  pos1 = js->pos;
  if (next(js) != TOK_SEMICOLON) {
    v = js_expr(js);
    if (is_err2(&v, &res)) goto done;
  }
  if (!expect(js, TOK_SEMICOLON, &res)) goto done;
  pos2 = js->pos;
  if (next(js) != TOK_RPAREN) {
    v = js_expr(js);
    if (is_err2(&v, &res)) goto done;
  }
  if (!expect(js, TOK_RPAREN, &res)) goto done;
  pos3 = js->pos;
  v = js_block_or_stmt(js);
  if (is_err2(&v, &res)) goto done;
  pos4 = js->pos;
  while (!(flags & F_NOEXEC)) {
    js->flags = flags, js->pos = pos1, js->consumed = 1;
    if (next(js) != TOK_SEMICOLON) {
      v = resolveprop(js, js_expr(js));
      if (is_err2(&v, &res)) goto done;
      if (!js_truthy(js, v)) break;
    }
    js->pos = pos3, js->consumed = 1, js->flags |= F_LOOP;
    v = js_block_or_stmt(js);
    if (is_err2(&v, &res)) goto done;
    if (js->flags & F_BREAK) break;
    if (js->flags & F_RETURN) {
      res = v;
      break;
    }
    js->flags = flags, js->pos = pos2, js->consumed = 1;
    if (next(js) != TOK_RPAREN) {
      v = js_expr(js);
      if (is_err2(&v, &res)) goto done;
    }
  }
  js->pos = pos4, js->tok = TOK_SEMICOLON, js->consumed = 0;
done:
  if (exe) delscope(js);
  uint8_t preserve = 0;
  if (js->flags & F_RETURN) {
    preserve = js->flags & (F_RETURN | F_NOEXEC);
  }
  js->flags = flags | preserve;
  return res;
}

static jsval_t js_break(struct js *js) {
  if (js->flags & F_NOEXEC) {
  } else {
    if (!(js->flags & F_LOOP)) return js_mkerr(js, "not in loop");
    js->flags |= F_BREAK | F_NOEXEC;
  }
  js->consumed = 1;
  return js_mkundef();
}

static jsval_t js_continue(struct js *js) {
  if (js->flags & F_NOEXEC) {
  } else {
    if (!(js->flags & F_LOOP)) return js_mkerr(js, "not in loop");
    js->flags |= F_NOEXEC;
  }
  js->consumed = 1;
  return js_mkundef();
}

static jsval_t js_return(struct js *js) {
  uint8_t exe = !(js->flags & F_NOEXEC);
  js->consumed = 1;
  if (exe && !(js->flags & F_CALL)) return js_mkerr(js, "not in func");
  jsval_t res = js_mkundef();
  
  if (next(js) != TOK_SEMICOLON) {
    res = resolveprop(js, js_expr(js));
  }
  
  if (exe) {
    js->pos = js->clen;
    js->flags |= F_RETURN | F_NOEXEC;
  }
  
  return res;
}

static jsval_t js_class_decl(struct js *js) {
  uint8_t exe = !(js->flags & F_NOEXEC);
  js->consumed = 1;
  EXPECT(TOK_IDENTIFIER, );
  js->consumed = 0;
  
  jsoff_t class_name_off = js->toff, class_name_len = js->tlen;
  char *class_name = (char *) &js->code[class_name_off];
  js->consumed = 1;
  
  jsoff_t super_off = 0, super_len = 0;
  if (next(js) == TOK_IDENTIFIER && js->tlen == 7 &&
      streq(&js->code[js->toff], js->tlen, "extends", 7)) {
    js->consumed = 1;
    EXPECT(TOK_IDENTIFIER, );
    super_off = js->toff;
    super_len = js->tlen;
    js->consumed = 1;
  }
  
  EXPECT(TOK_LBRACE, );
  jsoff_t class_body_start = js->pos;
  jsoff_t constructor_params_start = 0, constructor_params_end = 0;
  jsoff_t constructor_body_start = 0, constructor_body_end = 0;
  uint8_t save_flags = js->flags;
  js->flags |= F_NOEXEC;
  
  typedef struct { jsoff_t name_off, name_len, fn_start, fn_end; bool is_async; } MethodInfo;
  MethodInfo methods[32];
  int method_count = 0;
  
  while (next(js) != TOK_RBRACE && next(js) != TOK_EOF && method_count < 32) {
    bool is_async_method = false;
    if (next(js) == TOK_ASYNC) {
      is_async_method = true;
      js->consumed = 1;
    }
    EXPECT(TOK_IDENTIFIER, js->flags = save_flags);
    jsoff_t method_name_off = js->toff, method_name_len = js->tlen;
    js->consumed = 1;
    EXPECT(TOK_LPAREN, js->flags = save_flags);
    jsoff_t method_params_start = js->pos - 1;
    for (bool comma = false; next(js) != TOK_EOF; comma = true) {
      if (!comma && next(js) == TOK_RPAREN) break;
      EXPECT(TOK_IDENTIFIER, js->flags = save_flags);
      if (next(js) == TOK_RPAREN) break;
      EXPECT(TOK_COMMA, js->flags = save_flags);
    }
    EXPECT(TOK_RPAREN, js->flags = save_flags);
    jsoff_t method_params_end = js->pos;
    EXPECT(TOK_LBRACE, js->flags = save_flags);
    jsoff_t method_body_start = js->pos - 1;
    js->consumed = 0;
    jsval_t blk = js_block(js, false);
    if (is_err(blk)) {
      js->flags = save_flags;
      return blk;
    }
    jsoff_t method_body_end = js->pos;
    if (streq(&js->code[method_name_off], method_name_len, "constructor", 11)) {
      constructor_params_start = method_params_start;
      constructor_params_end = method_params_end;
      constructor_body_start = method_body_start + 1;
      constructor_body_end = method_body_end - 1;
    } else {
      methods[method_count].name_off = method_name_off;
      methods[method_count].name_len = method_name_len;
      methods[method_count].fn_start = method_params_start;
      methods[method_count].fn_end = method_body_end;
      methods[method_count].is_async = is_async_method;
      method_count++;
    }
    js->consumed = 1;
  }
  
  EXPECT(TOK_RBRACE, js->flags = save_flags);
  js->flags = save_flags;
  js->consumed = 0;
  
  if (exe) {
    jsoff_t wrapper_size = 256;
    if (constructor_body_start > 0) {
      wrapper_size += constructor_body_end - constructor_body_start;
    }
    
    for (int i = 0; i < method_count; i++) {
      wrapper_size += methods[i].name_len + 20;
      wrapper_size += methods[i].fn_end - methods[i].fn_start;
    }
    
    char *wrapper = (char *) malloc(wrapper_size);
    if (!wrapper) return js_mkerr(js, "oom");
    jsoff_t wp = 0;
    
    if (constructor_params_start > 0) {
      jsoff_t plen = constructor_params_end - constructor_params_start;
      memcpy(wrapper + wp, &js->code[constructor_params_start], plen);
      wp += plen;
    } else {
      memcpy(wrapper + wp, "()", 2);
      wp += 2;
    }
    wrapper[wp++] = '{';
    if (constructor_body_start > 0) {
      jsoff_t blen = constructor_body_end - constructor_body_start;
      memcpy(wrapper + wp, &js->code[constructor_body_start], blen);
      wp += blen;
      while (wp > 0 && (wrapper[wp - 1] == ' ' || wrapper[wp - 1] == '\t' ||
                        wrapper[wp - 1] == '\n' || wrapper[wp - 1] == '\r')) {
        wp--;
      }
      if (wp > 0 && wrapper[wp - 1] != ';' && wrapper[wp - 1] != '}') {
        wrapper[wp++] = ';';
      }
    }
    for (int i = 0; i < method_count; i++) {
      if (wp > 0 && wrapper[wp - 1] != ';' && wrapper[wp - 1] != '{') {
        wrapper[wp++] = ';';
      }
      memcpy(wrapper + wp, "this.", 5);
      wp += 5;
      memcpy(wrapper + wp, &js->code[methods[i].name_off], methods[i].name_len);
      wp += methods[i].name_len;
      wrapper[wp++] = '=';
      if (methods[i].is_async) {
        memcpy(wrapper + wp, "async ", 6);
        wp += 6;
      }
      memcpy(wrapper + wp, "function", 8);
      wp += 8;
      jsoff_t mlen = methods[i].fn_end - methods[i].fn_start;
      memcpy(wrapper + wp, &js->code[methods[i].fn_start], mlen);
      wp += mlen;
      wrapper[wp++] = ';';
    }
    
    wrapper[wp++] = '}';
    jsval_t code_str = js_mkstr(js, wrapper, wp);
    free(wrapper);
    if (is_err(code_str)) return code_str;
    jsval_t func_obj = mkobj(js, 0);
    if (is_err(func_obj)) return func_obj;
    jsval_t code_key = js_mkstr(js, "__code", 6);
    if (is_err(code_key)) return code_key;
    jsval_t res2 = setprop(js, func_obj, code_key, code_str);
    if (is_err(res2)) return res2;
    jsval_t scope_key = js_mkstr(js, "__scope", 7);
    if (is_err(scope_key)) return scope_key;
    jsval_t res3 = setprop(js, func_obj, scope_key, js->scope);
    if (is_err(res3)) return res3;
    jsval_t constructor = mkval(T_FUNC, (unsigned long) vdata(func_obj));
    if (lkp(js, js->scope, class_name, class_name_len) > 0) {
      return js_mkerr(js, "'%.*s' already declared", (int) class_name_len, class_name);
    }
    jsval_t x = mkprop(js, js->scope, js_mkstr(js, class_name, class_name_len), constructor, false);
    if (is_err(x)) return x;
  }
  
  (void) super_off;
  (void) super_len;
  (void) class_body_start;
  return js_mkundef();
}

static jsval_t js_stmt(struct js *js) {
  jsval_t res;
  if (js->brk > js->gct) js_gc(js);
  
  switch (next(js)) {
    case TOK_CASE: case TOK_CATCH:
    case TOK_DEFAULT: case TOK_DO: case TOK_FINALLY:
    case TOK_IN: case TOK_SWITCH:
    case TOK_THROW: case TOK_TRY: case TOK_VAR:
    case TOK_WITH: case TOK_WHILE: case TOK_YIELD:
      res = js_mkerr(js, "'%.*s' not implemented", (int) js->tlen, js->code + js->toff);
      break;
    case TOK_CONTINUE:  res = js_continue(js); break;
    case TOK_BREAK:     res = js_break(js); break;
    case TOK_LET:       res = js_let(js); break;
    case TOK_CONST:     res = js_const(js); break;
    case TOK_FUNC:      res = js_func_decl(js); break;
    case TOK_ASYNC:     js->consumed = 1; if (next(js) == TOK_FUNC) res = js_func_decl_async(js); else return js_mkerr(js, "async must be followed by function"); break;
    case TOK_CLASS:     res = js_class_decl(js); break;
    case TOK_IF:        res = js_if(js); break;
    case TOK_LBRACE:    res = js_block(js, !(js->flags & F_NOEXEC)); break;
    case TOK_FOR:       res = js_for(js); break;
    case TOK_RETURN:    res = js_return(js); break;
    default:            res = resolveprop(js, js_expr(js)); break;
  }
  
  if (next(js) != TOK_SEMICOLON && next(js) != TOK_EOF && next(js) != TOK_RBRACE) return js_mkerr(js, "; expected");
  js->consumed = 1;
  return res;
}

static jsval_t builtin_String(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_mkstr(js, "", 0);
  jsval_t arg = args[0];
  if (vtype(arg) == T_STR) return arg;
  const char *str = js_str(js, arg);
  return js_mkstr(js, str, strlen(str));
}

static jsval_t builtin_Number(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return tov(0.0);
  jsval_t arg = args[0];
  
  if (vtype(arg) == T_NUM) return arg;
  if (vtype(arg) == T_BOOL) {
    return tov(vdata(arg) ? 1.0 : 0.0);
  } else if (vtype(arg) == T_STR) {
    jsoff_t len;
    jsoff_t off = vstr(js, arg, &len);
    const char *str = (char *) &js->mem[off];
    char *end;
    double val = strtod(str, &end);
    return tov(val);
  } else if (vtype(arg) == T_NULL || vtype(arg) == T_UNDEF) {
    return tov(0.0);
  }
  
  return tov(0.0);
}

static jsval_t builtin_Boolean(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkval(T_BOOL, 0);
  return mkval(T_BOOL, js_truthy(js, args[0]) ? 1 : 0);
}

static jsval_t builtin_Object(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return js_mkobj(js);
  jsval_t arg = args[0];
  if (vtype(arg) == T_NULL || vtype(arg) == T_UNDEF) {
    return js_mkobj(js);
  }
  return arg;
}

static jsval_t builtin_Function(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  return js_mkundef();
}

static jsval_t builtin_Array(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = mkarr(js);
  
  if (nargs == 1 && vtype(args[0]) == T_NUM) {
    jsoff_t len = (jsoff_t) tod(args[0]);
    jsval_t len_key = js_mkstr(js, "length", 6);
    jsval_t len_val = tov((double) len);
    setprop(js, arr, len_key, len_val);
  } else {
    for (int i = 0; i < nargs; i++) {
      char idxstr[16];
      snprintf(idxstr, sizeof(idxstr), "%d", i);
      jsval_t key = js_mkstr(js, idxstr, strlen(idxstr));
      setprop(js, arr, key, args[i]);
    }
    jsval_t len_key = js_mkstr(js, "length", 6);
    jsval_t len_val = tov((double) nargs);
    setprop(js, arr, len_key, len_val);
  }
  
  return mkval(T_ARR, vdata(arr));
}

static jsval_t builtin_object_keys(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return mkarr(js);
  jsval_t obj = args[0];
  
  if (vtype(obj) != T_OBJ && vtype(obj) != T_ARR && vtype(obj) != T_FUNC) return mkarr(js);
  if (vtype(obj) == T_FUNC) obj = mkval(T_OBJ, vdata(obj));
  
  jsval_t arr = mkarr(js);
  jsoff_t idx = 0;
  jsoff_t next = loadoff(js, (jsoff_t) vdata(obj)) & ~(3U | CONSTMASK);
  
  while (next < js->brk && next != 0) {
    jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
    jsoff_t klen = offtolen(loadoff(js, koff));
    const char *key = (char *) &js->mem[koff + sizeof(koff)];
    char idxstr[16];
    snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) idx);
    jsval_t idx_key = js_mkstr(js, idxstr, strlen(idxstr));
    jsval_t key_val = js_mkstr(js, key, klen);
    setprop(js, arr, idx_key, key_val);
    idx++;
    next = loadoff(js, next) & ~(3U | CONSTMASK);
  }
  
  jsval_t len_key = js_mkstr(js, "length", 6);
  jsval_t len_val = tov((double) idx);
  setprop(js, arr, len_key, len_val);
  return mkval(T_ARR, vdata(arr));
}

static jsval_t builtin_array_push(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  arr = resolveprop(js, arr);
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "push called on non-array");
  }
  
  jsoff_t off = lkp(js, arr, "length", 6);
  jsoff_t len = 0;
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  for (int i = 0; i < nargs; i++) {
    char idxstr[16];
    snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) len);
    jsval_t key = js_mkstr(js, idxstr, strlen(idxstr));
    setprop(js, arr, key, args[i]);
    len++;
  }
  
  jsval_t len_key = js_mkstr(js, "length", 6);
  jsval_t len_val = tov((double) len);
  
  setprop(js, arr, len_key, len_val);
  return len_val;
}

static jsval_t builtin_array_pop(struct js *js, jsval_t *args, int nargs) {
  (void) args;
  (void) nargs;
  jsval_t arr = js->this_val;
  
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "pop called on non-array");
  }
  
  jsoff_t off = lkp(js, arr, "length", 6);
  jsoff_t len = 0;
  
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  if (len == 0) return js_mkundef();
  len--;
  char idxstr[16];
  snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) len);
  
  jsoff_t elem_off = lkp(js, arr, idxstr, strlen(idxstr));
  jsval_t result = js_mkundef();
  if (elem_off != 0) {
    result = resolveprop(js, mkval(T_PROP, elem_off));
  }
  
  jsval_t len_key = js_mkstr(js, "length", 6);
  jsval_t len_val = tov((double) len);
  setprop(js, arr, len_key, len_val);
  
  return result;
}

static jsval_t builtin_array_slice(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "slice called on non-array");
  }
  
  jsoff_t off = lkp(js, arr, "length", 6);
  jsoff_t len = 0;
  
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  jsoff_t start = 0, end = len;
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    double d = tod(args[0]);
    if (d < 0) {
      start = (jsoff_t) (d + len < 0 ? 0 : d + len);
    } else {
      start = (jsoff_t) (d > len ? len : d);
    }
  }
  
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    double d = tod(args[1]);
    if (d < 0) {
      end = (jsoff_t) (d + len < 0 ? 0 : d + len);
    } else {
      end = (jsoff_t) (d > len ? len : d);
    }
  }
  
  if (start > end) start = end;
  jsval_t result = mkarr(js);
  if (is_err(result)) return result;
  jsoff_t result_idx = 0;
  
  for (jsoff_t i = start; i < end; i++) {
    char idxstr[16];
    snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) i);
    jsoff_t elem_off = lkp(js, arr, idxstr, strlen(idxstr));
    if (elem_off != 0) {
      jsval_t elem = resolveprop(js, mkval(T_PROP, elem_off));
      snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) result_idx);
      jsval_t key = js_mkstr(js, idxstr, strlen(idxstr));
      setprop(js, result, key, elem);
    }
    result_idx++;
  }
  
  jsval_t len_key = js_mkstr(js, "length", 6);
  setprop(js, result, len_key, tov((double) result_idx));
  return mkval(T_ARR, vdata(result));
}

static jsval_t builtin_array_join(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "join called on non-array");
  }
  const char *sep = ",";
  jsoff_t sep_len = 1;
  
  if (nargs >= 1 && vtype(args[0]) == T_STR) {
    sep_len = 0;
    jsoff_t sep_off = vstr(js, args[0], &sep_len);
    sep = (const char *) &js->mem[sep_off];
  }
  
  jsoff_t off = lkp(js, arr, "length", 6);
  jsoff_t len = 0;
  
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  if (len == 0) return js_mkstr(js, "", 0);
  char result[1024] = "";
  size_t result_len = 0;
  
  for (jsoff_t i = 0; i < len; i++) {
    char idxstr[16];
    snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) i);
    jsoff_t elem_off = lkp(js, arr, idxstr, strlen(idxstr));
    
    if (i > 0 && result_len + sep_len < sizeof(result)) {
      memcpy(result + result_len, sep, sep_len);
      result_len += sep_len;
    }
    
    if (elem_off != 0) {
      jsval_t elem = resolveprop(js, mkval(T_PROP, elem_off));
      if (vtype(elem) == T_STR) {
        jsoff_t elem_len, elem_off_str = vstr(js, elem, &elem_len);
        if (result_len + elem_len < sizeof(result)) {
          memcpy(result + result_len, &js->mem[elem_off_str], elem_len);
          result_len += elem_len;
        }
      } else if (vtype(elem) == T_NUM) {
        char numstr[32];
        snprintf(numstr, sizeof(numstr), "%g", tod(elem));
        size_t num_len = strlen(numstr);
        if (result_len + num_len < sizeof(result)) {
          memcpy(result + result_len, numstr, num_len);
          result_len += num_len;
        }
      } else if (vtype(elem) == T_BOOL) {
        const char *boolstr = vdata(elem) ? "true" : "false";
        size_t bool_len = strlen(boolstr);
        if (result_len + bool_len < sizeof(result)) {
          memcpy(result + result_len, boolstr, bool_len);
          result_len += bool_len;
        }
      }
    }
  }
  return js_mkstr(js, result, result_len);
}

static jsval_t builtin_array_includes(struct js *js, jsval_t *args, int nargs) {
  jsval_t arr = js->this_val;
  if (vtype(arr) != T_ARR && vtype(arr) != T_OBJ) {
    return js_mkerr(js, "includes called on non-array");
  }
  
  if (nargs == 0) return mkval(T_BOOL, 0);
  jsval_t search = args[0];
  jsoff_t off = lkp(js, arr, "length", 6);
  jsoff_t len = 0;
  
  if (off != 0) {
    jsval_t len_val = resolveprop(js, mkval(T_PROP, off));
    if (vtype(len_val) == T_NUM) len = (jsoff_t) tod(len_val);
  }
  
  for (jsoff_t i = 0; i < len; i++) {
    char idxstr[16];
    snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) i);
    jsoff_t elem_off = lkp(js, arr, idxstr, strlen(idxstr));
    if (elem_off != 0) {
      jsval_t elem = resolveprop(js, mkval(T_PROP, elem_off));
      if (vtype(elem) == vtype(search)) {
        if (vtype(elem) == T_NUM && tod(elem) == tod(search)) {
          return mkval(T_BOOL, 1);
        } else if (vtype(elem) == T_BOOL && vdata(elem) == vdata(search)) {
          return mkval(T_BOOL, 1);
        } else if (vtype(elem) == T_STR) {
          jsoff_t elem_len, elem_off_str = vstr(js, elem, &elem_len);
          jsoff_t search_len, search_off = vstr(js, search, &search_len);
          if (elem_len == search_len && memcmp(&js->mem[elem_off_str], &js->mem[search_off], elem_len) == 0) {
            return mkval(T_BOOL, 1);
          }
        } else if ((vtype(elem) == T_OBJ || vtype(elem) == T_ARR || vtype(elem) == T_FUNC) && vdata(elem) == vdata(search)) {
          return mkval(T_BOOL, 1);
        }
      }
    }
  }
  return mkval(T_BOOL, 0);
}

static jsval_t builtin_string_indexOf(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = js->this_val;
  if (vtype(str) != T_STR) return js_mkerr(js, "indexOf called on non-string");
  if (nargs == 0) return tov(-1);
  jsval_t search = args[0];
  
  if (vtype(search) != T_STR) return tov(-1);
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  jsoff_t search_len, search_off = vstr(js, search, &search_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  const char *search_ptr = (char *) &js->mem[search_off];
  
  for (jsoff_t i = 0; i <= str_len - search_len; i++) {
    if (memcmp(str_ptr + i, search_ptr, search_len) == 0) {
      return tov((double) i);
    }
  }
  
  return tov(-1);
}

static jsval_t builtin_string_substring(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = js->this_val;
  if (vtype(str) != T_STR) return js_mkerr(js, "substring called on non-string");
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  jsoff_t start = 0, end = str_len;
  
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    double d = tod(args[0]);
    start = (jsoff_t) (d < 0 ? 0 : (d > str_len ? str_len : d));
  }
  
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    double d = tod(args[1]);
    end = (jsoff_t) (d < 0 ? 0 : (d > str_len ? str_len : d));
  }
  
  if (start > end) {
    jsoff_t tmp = start;
    start = end;
    end = tmp;
  }
  
  jsoff_t sub_len = end - start;
  return js_mkstr(js, str_ptr + start, sub_len);
}

static jsval_t builtin_string_split(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = js->this_val;
  if (vtype(str) != T_STR) return js_mkerr(js, "split called on non-string");
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  jsval_t arr = mkarr(js);
  
  if (is_err(arr)) return arr;
  if (nargs == 0 || vtype(args[0]) != T_STR) {
    jsval_t key = js_mkstr(js, "0", 1);
    setprop(js, arr, key, str);
    jsval_t len_key = js_mkstr(js, "length", 6);
    setprop(js, arr, len_key, tov(1));
    return mkval(T_ARR, vdata(arr));
  }
  
  jsval_t sep = args[0];
  jsoff_t sep_len, sep_off = vstr(js, sep, &sep_len);
  const char *sep_ptr = (char *) &js->mem[sep_off];
  jsoff_t idx = 0;
  jsoff_t start = 0;
  
  if (sep_len == 0) {
    for (jsoff_t i = 0; i < str_len; i++) {
      char idxstr[16];
      snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) idx);
      jsval_t key = js_mkstr(js, idxstr, strlen(idxstr));
      jsval_t part = js_mkstr(js, str_ptr + i, 1);
      setprop(js, arr, key, part);
      idx++;
    }
  } else {
    for (jsoff_t i = 0; i <= str_len - sep_len; i++) {
      if (memcmp(str_ptr + i, sep_ptr, sep_len) == 0) {
        char idxstr[16];
        snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) idx);
        jsval_t key = js_mkstr(js, idxstr, strlen(idxstr));
        jsval_t part = js_mkstr(js, str_ptr + start, i - start);
        setprop(js, arr, key, part);
        idx++;
        start = i + sep_len;
        i += sep_len - 1;
      }
    }
    if (start <= str_len) {
      char idxstr[16];
      snprintf(idxstr, sizeof(idxstr), "%u", (unsigned) idx);
      jsval_t key = js_mkstr(js, idxstr, strlen(idxstr));
      jsval_t part = js_mkstr(js, str_ptr + start, str_len - start);
      setprop(js, arr, key, part);
      idx++;
    }
  }
  
  jsval_t len_key = js_mkstr(js, "length", 6);
  setprop(js, arr, len_key, tov((double) idx));
  return mkval(T_ARR, vdata(arr));
}

static jsval_t builtin_string_slice(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = js->this_val;
  if (vtype(str) != T_STR) return js_mkerr(js, "slice called on non-string");
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  jsoff_t start = 0, end = str_len;
  
  if (nargs >= 1 && vtype(args[0]) == T_NUM) {
    double d = tod(args[0]);
    if (d < 0) {
      start = (jsoff_t) (d + str_len < 0 ? 0 : d + str_len);
    } else {
      start = (jsoff_t) (d > str_len ? str_len : d);
    }
  }
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    double d = tod(args[1]);
    if (d < 0) {
      end = (jsoff_t) (d + str_len < 0 ? 0 : d + str_len);
    } else {
      end = (jsoff_t) (d > str_len ? str_len : d);
    }
  }
  
  if (start > end) start = end;
  jsoff_t sub_len = end - start;
  return js_mkstr(js, str_ptr + start, sub_len);
}

static jsval_t builtin_string_includes(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = js->this_val;
  if (vtype(str) != T_STR) return js_mkerr(js, "includes called on non-string");
  if (nargs == 0) return mkval(T_BOOL, 0);
  jsval_t search = args[0];
  
  if (vtype(search) != T_STR) return mkval(T_BOOL, 0);
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  jsoff_t search_len, search_off = vstr(js, search, &search_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  const char *search_ptr = (char *) &js->mem[search_off];
  
  if (search_len == 0) return mkval(T_BOOL, 1);
  for (jsoff_t i = 0; i <= str_len - search_len; i++) {
    if (memcmp(str_ptr + i, search_ptr, search_len) == 0) {
      return mkval(T_BOOL, 1);
    }
  }
  
  return mkval(T_BOOL, 0);
}

static jsval_t builtin_string_startsWith(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = js->this_val;
  if (vtype(str) != T_STR) return js_mkerr(js, "startsWith called on non-string");
  if (nargs == 0) return mkval(T_BOOL, 0);
  jsval_t search = args[0];
  
  if (vtype(search) != T_STR) return mkval(T_BOOL, 0);
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  jsoff_t search_len, search_off = vstr(js, search, &search_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  const char *search_ptr = (char *) &js->mem[search_off];
  
  if (search_len > str_len) return mkval(T_BOOL, 0);
  if (search_len == 0) return mkval(T_BOOL, 1);
  
  return mkval(T_BOOL, memcmp(str_ptr, search_ptr, search_len) == 0 ? 1 : 0);
}

static jsval_t builtin_string_endsWith(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = js->this_val;
  if (vtype(str) != T_STR) return js_mkerr(js, "endsWith called on non-string");
  if (nargs == 0) return mkval(T_BOOL, 0);
  jsval_t search = args[0];
  
  if (vtype(search) != T_STR) return mkval(T_BOOL, 0);
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  jsoff_t search_len, search_off = vstr(js, search, &search_len);
  
  const char *str_ptr = (char *) &js->mem[str_off];
  const char *search_ptr = (char *) &js->mem[search_off];
  
  if (search_len > str_len) return mkval(T_BOOL, 0);
  if (search_len == 0) return mkval(T_BOOL, 1);
  
  return mkval(T_BOOL, memcmp(str_ptr + str_len - search_len, search_ptr, search_len) == 0 ? 1 : 0);
}

static jsval_t builtin_string_replace(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = js->this_val;
  if (vtype(str) != T_STR) return js_mkerr(js, "replace called on non-string");
  if (nargs < 2) return str;
  
  jsval_t search = args[0];
  jsval_t replacement = args[1];
  
  if (vtype(search) != T_STR || vtype(replacement) != T_STR) return str;
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  jsoff_t search_len, search_off = vstr(js, search, &search_len);
  jsoff_t repl_len, repl_off = vstr(js, replacement, &repl_len);
  
  const char *str_ptr = (char *) &js->mem[str_off];
  const char *search_ptr = (char *) &js->mem[search_off];
  const char *repl_ptr = (char *) &js->mem[repl_off];
  
  if (search_len > str_len) return str;
  jsoff_t found_at = -1;
  
  for (jsoff_t i = 0; i <= str_len - search_len; i++) {
    if (memcmp(str_ptr + i, search_ptr, search_len) == 0) {
      found_at = i;
      break;
    }
  }
  
  if (found_at == -1) return str;
  char result[2048];
  jsoff_t result_len = 0;
  
  if (found_at > 0 && result_len + found_at < sizeof(result)) {
    memcpy(result + result_len, str_ptr, found_at);
    result_len += found_at;
  }
  
  if (result_len + repl_len < sizeof(result)) {
    memcpy(result + result_len, repl_ptr, repl_len);
    result_len += repl_len;
  }
  
  jsoff_t after_start = found_at + search_len;
  jsoff_t after_len = str_len - after_start;
  
  if (after_len > 0 && result_len + after_len < sizeof(result)) {
    memcpy(result + result_len, str_ptr + after_start, after_len);
    result_len += after_len;
  }
  
  return js_mkstr(js, result, result_len);
}

static jsval_t builtin_string_template(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = js->this_val;
  if (vtype(str) != T_STR) return js_mkerr(js, "template called on non-string");
  if (nargs < 1 || vtype(args[0]) != T_OBJ) return str;
  
  jsval_t data = args[0];
  jsoff_t str_len, str_off = vstr(js, str, &str_len);
  const char *str_ptr = (char *) &js->mem[str_off];
  
  char result[2048];
  jsoff_t result_len = 0;
  jsoff_t i = 0;
  
  while (i < str_len && result_len < sizeof(result) - 1) {
    if (i < str_len - 3 && str_ptr[i] == '{' && str_ptr[i + 1] == '{') {
      jsoff_t start = i + 2;
      jsoff_t end = start;
      while (end < str_len - 1 && !(str_ptr[end] == '}' && str_ptr[end + 1] == '}')) {
        end++;
      }
      if (end < str_len - 1 && str_ptr[end] == '}' && str_ptr[end + 1] == '}') {
        jsoff_t key_len = end - start;
        jsoff_t prop_off = lkp(js, data, str_ptr + start, key_len);
        
        if (prop_off != 0) {
          jsval_t value = resolveprop(js, mkval(T_PROP, prop_off));
          if (vtype(value) == T_STR) {
            jsoff_t val_len, val_off = vstr(js, value, &val_len);
            if (result_len + val_len < sizeof(result)) {
              memcpy(result + result_len, &js->mem[val_off], val_len);
              result_len += val_len;
            }
          } else if (vtype(value) == T_NUM) {
            char numstr[32];
            snprintf(numstr, sizeof(numstr), "%g", tod(value));
            size_t num_len = strlen(numstr);
            if (result_len + num_len < sizeof(result)) {
              memcpy(result + result_len, numstr, num_len);
              result_len += num_len;
            }
          } else if (vtype(value) == T_BOOL) {
            const char *boolstr = vdata(value) ? "true" : "false";
            size_t bool_len = strlen(boolstr);
            if (result_len + bool_len < sizeof(result)) {
              memcpy(result + result_len, boolstr, bool_len);
              result_len += bool_len;
            }
          }
        } else {
        }
        i = end + 2;
        continue;
      }
    }
    result[result_len++] = str_ptr[i++];
  }
  return js_mkstr(js, result, result_len);
}

static jsval_t builtin_string_charCodeAt(struct js *js, jsval_t *args, int nargs) {
  jsval_t str = js->this_val;
  if (vtype(str) != T_STR) return js_mkerr(js, "charCodeAt called on non-string");
  if (nargs < 1 || vtype(args[0]) != T_NUM) return tov(NAN);
  
  double idx_d = tod(args[0]);
  if (idx_d < 0 || idx_d != (double)(long)idx_d) return tov(NAN);
  
  jsoff_t idx = (jsoff_t) idx_d;
  jsoff_t str_len = offtolen(loadoff(js, (jsoff_t) vdata(str)));
  
  if (idx >= str_len) return tov(NAN);
  
  jsoff_t str_off = (jsoff_t) vdata(str) + sizeof(jsoff_t);
  unsigned char ch = (unsigned char) js->mem[str_off + idx];
  
  return tov((double) ch);
}

static jsval_t builtin_resolve_internal(struct js *js, jsval_t *args, int nargs);
static jsval_t builtin_reject_internal(struct js *js, jsval_t *args, int nargs);
static void resolve_promise(struct js *js, jsval_t p, jsval_t val);
static void reject_promise(struct js *js, jsval_t p, jsval_t val);

static size_t strpromise(struct js *js, jsval_t value, char *buf, size_t len) {
  jsval_t prom_obj = mkval(T_OBJ, vdata(value));
  jsoff_t state_off = lkp(js, prom_obj, "__state", 7);
  int state = 0;
  if (state_off != 0) {
    jsval_t val = resolveprop(js, mkval(T_PROP, state_off));
    if (vtype(val) == T_NUM) state = (int)tod(val);
  }
  
  if (state == 0) {
    return (size_t)snprintf(buf, len, "Promise { <pending> }");
  }
  
  jsoff_t value_off = lkp(js, prom_obj, "__value", 7);
  if (value_off == 0) {
    const char *s = (state == 1) ? "<fulfilled>" : "<rejected>";
    return (size_t)snprintf(buf, len, "Promise { %s }", s);
  }
  
  jsval_t prom_val = resolveprop(js, mkval(T_PROP, value_off));
  char value_buf[256];
  size_t value_len = tostr(js, prom_val, value_buf, sizeof(value_buf));
  if (value_len >= sizeof(value_buf)) value_len = sizeof(value_buf) - 1;
  value_buf[value_len] = '\0';
  
  if (state == 1) {
    return (size_t)snprintf(buf, len, "Promise { %s }", value_buf);
  } else {
    return (size_t)snprintf(buf, len, "Promise { <rejected> %s }", value_buf);
  }
}

static jsval_t mkpromise(struct js *js) {
  jsval_t obj = mkobj(js, 0);
  if (is_err(obj)) return obj;
  setprop(js, obj, js_mkstr(js, "__state", 7), tov(0.0));
  setprop(js, obj, js_mkstr(js, "__value", 7), js_mkundef());
  setprop(js, obj, js_mkstr(js, "__handlers", 10), mkarr(js));
  return mkval(T_PROMISE, vdata(obj));
}

static jsval_t builtin_trigger_handler_wrapper(struct js *js, jsval_t *args, int nargs);

static void trigger_handlers(struct js *js, jsval_t p) {
  jsval_t wrapper_obj = mkobj(js, 0);
  setprop(js, wrapper_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_trigger_handler_wrapper));
  setprop(js, wrapper_obj, js_mkstr(js, "promise", 7), p);
  jsval_t wrapper_fn = mkval(T_FUNC, vdata(wrapper_obj));
  queue_microtask(js, wrapper_fn);
}

static jsval_t builtin_trigger_handler_wrapper(struct js *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t p = js_get(js, me, "promise");
  if (vtype(p) != T_PROMISE) return js_mkundef();
  
  jsval_t p_obj = mkval(T_OBJ, vdata(p));
  jsoff_t state_off = lkp(js, p_obj, "__state", 7);
  int state = (int)tod(resolveprop(js, mkval(T_PROP, state_off)));
  
  jsoff_t val_off = lkp(js, p_obj, "__value", 7);
  jsval_t val = resolveprop(js, mkval(T_PROP, val_off));
  
  jsoff_t handlers_off = lkp(js, p_obj, "__handlers", 10);
  jsval_t handlers_arr = resolveprop(js, mkval(T_PROP, handlers_off));
  
  jsoff_t len_off = lkp(js, handlers_arr, "length", 6);
  int len = (int)tod(resolveprop(js, mkval(T_PROP, len_off)));
  
  for (int i = 0; i < len; i += 3) {
    char idx1[16], idx2[16], idx3[16];
    snprintf(idx1, sizeof(idx1), "%d", i);
    snprintf(idx2, sizeof(idx2), "%d", i+1);
    snprintf(idx3, sizeof(idx3), "%d", i+2);
    
    jsval_t onFulfilled = resolveprop(js, js_get(js, handlers_arr, idx1));
    jsval_t onRejected = resolveprop(js, js_get(js, handlers_arr, idx2));
    jsval_t nextPromise = resolveprop(js, js_get(js, handlers_arr, idx3));
    
    jsval_t handler = (state == 1) ? onFulfilled : onRejected;
    
    if (vtype(handler) == T_FUNC || vtype(handler) == T_CFUNC) {
       jsval_t res;
       if (vtype(handler) == T_CFUNC) {
          jsval_t (*fn)(struct js *, jsval_t *, int) = (jsval_t(*)(struct js *, jsval_t *, int)) vdata(handler);
          res = fn(js, &val, 1);
       } else {
          jsval_t args[] = { val };
          res = js_call(js, handler, args, 1);
       }
       
       if (is_err(res)) {
          jsval_t err_str = js_mkstr(js, js->errmsg, strlen(js->errmsg));
          reject_promise(js, nextPromise, err_str);
       } else {
          resolve_promise(js, nextPromise, res);
       }
    } else {
       if (state == 1) resolve_promise(js, nextPromise, val);
       else reject_promise(js, nextPromise, val);
    }
  }
  setprop(js, p_obj, js_mkstr(js, "__handlers", 10), mkarr(js));
  return js_mkundef();
}

static void resolve_promise(struct js *js, jsval_t p, jsval_t val) {
  jsval_t p_obj = mkval(T_OBJ, vdata(p));
  jsoff_t state_off = lkp(js, p_obj, "__state", 7);
  if (state_off == 0) return;
  jsval_t state_val = resolveprop(js, mkval(T_PROP, state_off));
  if ((int)tod(state_val) != 0) return;

  if (vtype(val) == T_PROMISE) {
     if (vdata(val) == vdata(p)) {
        jsval_t err = js_mkerr(js, "TypeError: Chaining cycle");
        reject_promise(js, p, err);
        return;
     }
     jsval_t res_obj = mkobj(js, 0);
     setprop(js, res_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_resolve_internal));
     setprop(js, res_obj, js_mkstr(js, "promise", 7), p);
     jsval_t res_fn = mkval(T_FUNC, vdata(res_obj));
     
     jsval_t rej_obj = mkobj(js, 0);
     setprop(js, rej_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_reject_internal));
     setprop(js, rej_obj, js_mkstr(js, "promise", 7), p);
     jsval_t rej_fn = mkval(T_FUNC, vdata(rej_obj));
     
     jsval_t args[] = { res_fn, rej_fn };
     jsval_t then_prop = js_get(js, val, "then");
     if (vtype(then_prop) == T_FUNC || vtype(then_prop) == T_CFUNC) {
         jsval_t saved_this = js->this_val;
         js->this_val = val;
         js_call(js, then_prop, args, 2);
         js->this_val = saved_this;
         return;
     }
  }

  setprop(js, p_obj, js_mkstr(js, "__state", 7), tov(1.0));
  setprop(js, p_obj, js_mkstr(js, "__value", 7), val);
  trigger_handlers(js, p);
}

static void reject_promise(struct js *js, jsval_t p, jsval_t val) {
  jsval_t p_obj = mkval(T_OBJ, vdata(p));
  jsoff_t state_off = lkp(js, p_obj, "__state", 7);
  if (state_off == 0) return;
  jsval_t state_val = resolveprop(js, mkval(T_PROP, state_off));
  if ((int)tod(state_val) != 0) return;

  setprop(js, p_obj, js_mkstr(js, "__state", 7), tov(2.0));
  setprop(js, p_obj, js_mkstr(js, "__value", 7), val);
  trigger_handlers(js, p);
}

static jsval_t builtin_resolve_internal(struct js *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t p = js_get(js, me, "promise");
  if (vtype(p) != T_PROMISE) return js_mkundef();
  resolve_promise(js, p, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static jsval_t builtin_reject_internal(struct js *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t p = js_get(js, me, "promise");
  if (vtype(p) != T_PROMISE) return js_mkundef();
  reject_promise(js, p, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

static jsval_t builtin_promise_executor_wrapper(struct js *js, jsval_t *args, int nargs);

static jsval_t builtin_Promise(struct js *js, jsval_t *args, int nargs) {
  jsval_t p = mkpromise(js);
  jsval_t res_obj = mkobj(js, 0);
  setprop(js, res_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_resolve_internal));
  setprop(js, res_obj, js_mkstr(js, "promise", 7), p);
  jsval_t res_fn = mkval(T_FUNC, vdata(res_obj));
  jsval_t rej_obj = mkobj(js, 0);
  setprop(js, rej_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_reject_internal));
  setprop(js, rej_obj, js_mkstr(js, "promise", 7), p);
  jsval_t rej_fn = mkval(T_FUNC, vdata(rej_obj));
  if (nargs > 0) {
     jsval_t wrapper_obj = mkobj(js, 0);
     setprop(js, wrapper_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_promise_executor_wrapper));
     setprop(js, wrapper_obj, js_mkstr(js, "executor", 8), args[0]);
     setprop(js, wrapper_obj, js_mkstr(js, "resolve", 7), res_fn);
     setprop(js, wrapper_obj, js_mkstr(js, "reject", 6), rej_fn);
     jsval_t wrapper_fn = mkval(T_FUNC, vdata(wrapper_obj));
     queue_microtask(js, wrapper_fn);
  }
  return p;
}

static jsval_t builtin_promise_executor_wrapper(struct js *js, jsval_t *args, int nargs) {
  jsval_t me = js->current_func;
  jsval_t executor = js_get(js, me, "executor");
  jsval_t res_fn = js_get(js, me, "resolve");
  jsval_t rej_fn = js_get(js, me, "reject");
  jsval_t exec_args[] = { res_fn, rej_fn };
  js_call(js, executor, exec_args, 2);
  return js_mkundef();
}

static jsval_t builtin_Promise_resolve(struct js *js, jsval_t *args, int nargs) {
  jsval_t val = nargs > 0 ? args[0] : js_mkundef();
  if (vtype(val) == T_PROMISE) return val;
  jsval_t p = mkpromise(js);
  resolve_promise(js, p, val);
  return p;
}

static jsval_t builtin_Promise_reject(struct js *js, jsval_t *args, int nargs) {
  jsval_t val = nargs > 0 ? args[0] : js_mkundef();
  jsval_t p = mkpromise(js);
  reject_promise(js, p, val);
  return p;
}

static jsval_t builtin_promise_then(struct js *js, jsval_t *args, int nargs) {
  jsval_t p = js->this_val;
  if (vtype(p) != T_PROMISE) return js_mkerr(js, "not a promise");
  jsval_t nextP = mkpromise(js);
  jsval_t onFulfilled = nargs > 0 ? args[0] : js_mkundef();
  jsval_t onRejected = nargs > 1 ? args[1] : js_mkundef();
  jsval_t p_obj = mkval(T_OBJ, vdata(p));
  jsval_t handlers = resolveprop(js, js_get(js, p_obj, "__handlers"));
  jsval_t push_args[] = { onFulfilled, onRejected, nextP };
  jsval_t saved_this = js->this_val;
  js->this_val = handlers;
  builtin_array_push(js, push_args, 3);
  js->this_val = saved_this;
  jsoff_t state_off = lkp(js, p_obj, "__state", 7);
  int state = (int)tod(resolveprop(js, mkval(T_PROP, state_off)));
  if (state != 0) trigger_handlers(js, p);
  return nextP;
}

static jsval_t builtin_promise_catch(struct js *js, jsval_t *args, int nargs) {
  jsval_t args_then[] = { js_mkundef(), nargs > 0 ? args[0] : js_mkundef() };
  return builtin_promise_then(js, args_then, 2);
}

static jsval_t builtin_promise_finally(struct js *js, jsval_t *args, int nargs) {
  jsval_t fn = nargs > 0 ? args[0] : js_mkundef();
  jsval_t args_then[] = { fn, fn };
  return builtin_promise_then(js, args_then, 2);
}

static jsval_t builtin_Promise_try(struct js *js, jsval_t *args, int nargs) {
  if (nargs == 0) return builtin_Promise_resolve(js, args, 0);
  jsval_t fn = args[0];
  jsval_t res = js_call(js, fn, NULL, 0);
  if (is_err(res)) {
     jsval_t err_str = js_mkstr(js, js->errmsg, strlen(js->errmsg));
     jsval_t rej_args[] = { err_str };
     return builtin_Promise_reject(js, rej_args, 1);
  }
  jsval_t res_args[] = { res };
  return builtin_Promise_resolve(js, res_args, 1);
}

static jsval_t do_instanceof(struct js *js, jsval_t l, jsval_t r) {
  uint8_t ltype = vtype(l);
  if (vtype(r) == T_FUNC) {
    jsval_t func_obj = mkval(T_OBJ, vdata(r));
    jsoff_t native_off = lkp(js, func_obj, "__native_func", 13);
    if (native_off != 0) {
      jsval_t native_val = resolveprop(js, mkval(T_PROP, native_off));
      if (vtype(native_val) == T_CFUNC) {
        jsval_t (*fn)(struct js *, jsval_t *, int) = (jsval_t(*)(struct js *, jsval_t *, int)) vdata(native_val);
        if (fn == builtin_Promise) {
          return mkval(T_BOOL, ltype == T_PROMISE ? 1 : 0);
        }
      }
    }
    jsoff_t code_off = lkp(js, func_obj, "__code", 6);
    if (code_off != 0) {
      jsval_t code_val = resolveprop(js, mkval(T_PROP, code_off));
      if (vtype(code_val) == T_STR) {
        jsoff_t fnlen, fnoff = vstr(js, code_val, &fnlen);
        const char *code_str = (const char *) (&js->mem[fnoff]);
        if (fnlen == 16 && memcmp(code_str, "__builtin_Object", 16) == 0) {
          return mkval(T_BOOL, ltype == T_OBJ ? 1 : 0);
        }
      }
    }
  }
  if (vtype(r) == T_CFUNC) {
    jsval_t (*fn)(struct js *, jsval_t *, int) = (jsval_t(*)(struct js *, jsval_t *, int)) vdata(r);
    if (fn == builtin_Object) {
      return mkval(T_BOOL, ltype == T_OBJ ? 1 : 0);
    } else if (fn == builtin_Function) {
      return mkval(T_BOOL, (ltype == T_FUNC || ltype == T_CFUNC) ? 1 : 0);
    } else if (fn == builtin_String) {
      return mkval(T_BOOL, ltype == T_STR ? 1 : 0);
    } else if (fn == builtin_Number) {
      return mkval(T_BOOL, ltype == T_NUM ? 1 : 0);
    } else if (fn == builtin_Boolean) {
      return mkval(T_BOOL, ltype == T_BOOL ? 1 : 0);
    } else if (fn == builtin_Array) {
      return mkval(T_BOOL, ltype == T_ARR ? 1 : 0);
    } else if (fn == builtin_Promise) {
      return mkval(T_BOOL, ltype == T_PROMISE ? 1 : 0);
    }
  }
  return mkval(T_BOOL, 0);
}

struct js *js_create(void *buf, size_t len) {
  struct js *js = NULL;
  if (len < sizeof(*js) + esize(T_OBJ)) return js;
  memset(buf, 0, len);
  
  js = (struct js *) buf;
  js->mem = (uint8_t *) (js + 1);
  js->size = (jsoff_t) (len - sizeof(*js));
  js->scope = mkobj(js, 0);
  js->size = js->size / 8U * 8U;
  js->lwm = js->size;
  js->gct = js->size / 2;
  js->this_val = js->scope;
  
  jsval_t glob = js->scope;
  jsval_t obj_func_obj = mkobj(js, 0);
  jsval_t obj_code_key = js_mkstr(js, "__code", 6);
  jsval_t obj_code_val = js_mkstr(js, "__builtin_Object", 16);
  setprop(js, obj_func_obj, obj_code_key, obj_code_val);
  jsval_t obj_keys_key = js_mkstr(js, "keys", 4);
  setprop(js, obj_func_obj, obj_keys_key, js_mkfun(builtin_object_keys));
  jsval_t obj_func = mkval(T_FUNC, vdata(obj_func_obj));
  
  setprop(js, glob, js_mkstr(js, "Object", 6), obj_func);
  setprop(js, glob, js_mkstr(js, "Function", 8), js_mkfun(builtin_Function));
  setprop(js, glob, js_mkstr(js, "String", 6), js_mkfun(builtin_String));
  setprop(js, glob, js_mkstr(js, "Number", 6), js_mkfun(builtin_Number));
  setprop(js, glob, js_mkstr(js, "Boolean", 7), js_mkfun(builtin_Boolean));
  setprop(js, glob, js_mkstr(js, "Array", 5), js_mkfun(builtin_Array));
  
  jsval_t p_proto = js_mkobj(js);
  setprop(js, p_proto, js_mkstr(js, "then", 4), js_mkfun(builtin_promise_then));
  setprop(js, p_proto, js_mkstr(js, "catch", 5), js_mkfun(builtin_promise_catch));
  setprop(js, p_proto, js_mkstr(js, "finally", 7), js_mkfun(builtin_promise_finally));
  
  jsval_t p_ctor_obj = mkobj(js, 0);
  setprop(js, p_ctor_obj, js_mkstr(js, "__native_func", 13), js_mkfun(builtin_Promise));
  setprop(js, p_ctor_obj, js_mkstr(js, "resolve", 7), js_mkfun(builtin_Promise_resolve));
  setprop(js, p_ctor_obj, js_mkstr(js, "reject", 6), js_mkfun(builtin_Promise_reject));
  setprop(js, p_ctor_obj, js_mkstr(js, "try", 3), js_mkfun(builtin_Promise_try));
  setprop(js, p_ctor_obj, js_mkstr(js, "prototype", 9), p_proto);
  
  setprop(js, glob, js_mkstr(js, "Promise", 7), mkval(T_FUNC, vdata(p_ctor_obj)));
  
  return js;
}

double js_getnum(jsval_t value) { return tod(value); }
int js_getbool(jsval_t value) { return vdata(value) & 1 ? 1 : 0; }

void js_setgct(struct js *js, size_t gct) { js->gct = (jsoff_t) gct; }
void js_setmaxcss(struct js *js, size_t max) { js->maxcss = (jsoff_t) max; }
void js_set_filename(struct js *js, const char *filename) { js->filename = filename; }

jsval_t js_mktrue(void) { return mkval(T_BOOL, 1); }
jsval_t js_mkfalse(void) { return mkval(T_BOOL, 0); }
jsval_t js_mkundef(void) { return mkval(T_UNDEF, 0); }
jsval_t js_mknull(void) { return mkval(T_NULL, 0); }
jsval_t js_mknum(double value) { return tov(value); }
jsval_t js_mkobj(struct js *js) { return mkobj(js, 0); }
jsval_t js_glob(struct js *js) { (void) js; return mkval(T_OBJ, 0); }
jsval_t js_mkfun(jsval_t (*fn)(struct js *, jsval_t *, int)) { return mkval(T_CFUNC, (size_t) (void *) fn); }
jsval_t js_getthis(struct js *js) { return js->this_val; }

void js_set(struct js *js, jsval_t obj, const char *key, jsval_t val) {
  if (vtype(obj) == T_OBJ) {
    setprop(js, obj, js_mkstr(js, key, strlen(key)), val);
  } else if (vtype(obj) == T_FUNC) {
    jsval_t func_obj = mkval(T_OBJ, vdata(obj));
    setprop(js, func_obj, js_mkstr(js, key, strlen(key)), val);
  }
}

jsval_t js_get(struct js *js, jsval_t obj, const char *key) {
  if (vtype(obj) == T_FUNC) {
    jsval_t func_obj = mkval(T_OBJ, vdata(obj));
    jsoff_t off = lkp(js, func_obj, key, strlen(key));
    return off == 0 ? js_mkundef() : resolveprop(js, mkval(T_PROP, off));
  }
  
  if (vtype(obj) != T_OBJ) return js_mkundef();
  jsoff_t off = lkp(js, obj, key, strlen(key));
  return off == 0 ? js_mkundef() : resolveprop(js, mkval(T_PROP, off));
}

char *js_getstr(struct js *js, jsval_t value, size_t *len) {
  if (vtype(value) != T_STR) return NULL;
  jsoff_t n, off = vstr(js, value, &n);
  if (len != NULL) *len = n;
  return (char *) &js->mem[off];
}

void js_merge_obj(struct js *js, jsval_t dst, jsval_t src) {
  if (vtype(dst) != T_OBJ || vtype(src) != T_OBJ) return;
  jsoff_t next = loadoff(js, (jsoff_t) vdata(src)) & ~(3U | CONSTMASK);
  while (next < js->brk && next != 0) {
    jsoff_t koff = loadoff(js, next + (jsoff_t) sizeof(next));
    jsval_t val = loadval(js, next + (jsoff_t) (sizeof(next) + sizeof(koff)));
    jsoff_t klen = offtolen(loadoff(js, koff));
    
    char *key = (char *) &js->mem[koff + sizeof(koff)];
    setprop(js, dst, js_mkstr(js, key, klen), val);
    next = loadoff(js, next) & ~(3U | CONSTMASK);
  }
}

int js_type(jsval_t val) {
  switch (vtype(val)) {
    case T_UNDEF:   return JS_UNDEF;
    case T_NULL:    return JS_NULL;
    case T_BOOL:    return vdata(val) == 0 ? JS_FALSE: JS_TRUE;
    case T_STR:     return JS_STR;
    case T_NUM:     return JS_NUM;
    case T_ERR:     return JS_ERR;
    default:        return JS_PRIV;
  }
}

void js_stats(struct js *js, size_t *total, size_t *lwm, size_t *css) {
  if (total) *total = js->size;
  if (lwm) *lwm = js->lwm;
  if (css) *css = js->css;
}

bool js_chkargs(jsval_t *args, int nargs, const char *spec) {
  int i = 0, ok = 1;
  for (; ok && i < nargs && spec[i]; i++) {
    uint8_t t = vtype(args[i]), c = (uint8_t) spec[i];
    ok = (c == 'b' && t == T_BOOL) || (c == 'd' && t == T_NUM) ||
         (c == 's' && t == T_STR) || (c == 'j');
  }
  if (spec[i] != '\0' || i != nargs) ok = 0;
  return ok;
}

jsval_t js_eval(struct js *js, const char *buf, size_t len) {
  jsval_t res = js_mkundef();
  if (len == (size_t) ~0U) len = strlen(buf);
  js->consumed = 1;
  js->tok = TOK_ERR;
  js->code = buf;
  js->clen = (jsoff_t) len;
  js->pos = 0;
  js->cstk = &res;
  while (next(js) != TOK_EOF && !is_err(res)) {
    res = js_stmt(js);
    if (js->flags & F_RETURN) break;
  }
  return res;
}

jsval_t js_call(struct js *js, jsval_t func, jsval_t *args, int nargs) {
  if (vtype(func) == T_CFUNC) {
    jsval_t (*fn)(struct js *, jsval_t *, int) = (jsval_t(*)(struct js *, jsval_t *, int)) vdata(func);
    return fn(js, args, nargs);
  } else if (vtype(func) == T_FUNC) {
    jsval_t func_obj = mkval(T_OBJ, vdata(func));
    jsoff_t native_off = lkp(js, func_obj, "__native_func", 13);
    if (native_off != 0) {
      jsval_t native_val = resolveprop(js, mkval(T_PROP, native_off));
      if (vtype(native_val) == T_CFUNC) {
        jsval_t saved_func = js->current_func;
        js->current_func = func;
        jsval_t (*fn)(struct js *, jsval_t *, int) = (jsval_t(*)(struct js *, jsval_t *, int)) vdata(native_val);
        jsval_t res = fn(js, args, nargs);
        js->current_func = saved_func;
        return res;
      }
    }
    jsoff_t code_off = lkp(js, func_obj, "__code", 6);
    
    if (code_off == 0) return js_mkerr(js, "function has no code");
    jsval_t code_val = resolveprop(js, mkval(T_PROP, code_off));
    if (vtype(code_val) != T_STR) return js_mkerr(js, "function code not string");
    jsoff_t fnlen, fnoff = vstr(js, code_val, &fnlen);
    const char *fn = (const char *) (&js->mem[fnoff]);
    jsoff_t fnpos = 1;
    
    uint8_t saved_flags = js->flags;
    js->flags = 0;
    mkscope(js);
    js->flags = saved_flags;
    int arg_idx = 0;
    
    while (fnpos < fnlen) {
      fnpos = skiptonext(fn, fnlen, fnpos);
      if (fnpos < fnlen && fn[fnpos] == ')') break;
      jsoff_t identlen = 0;
      uint8_t tok = parseident(&fn[fnpos], fnlen - fnpos, &identlen);
      if (tok != TOK_IDENTIFIER) break;
      jsval_t v = arg_idx < nargs ? args[arg_idx] : js_mkundef();
      setprop(js, js->scope, js_mkstr(js, &fn[fnpos], identlen), v);
      arg_idx++;
      fnpos = skiptonext(fn, fnlen, fnpos + identlen);
      if (fnpos < fnlen && fn[fnpos] == ',') fnpos++;
    }
    
    if (fnpos < fnlen && fn[fnpos] == ')') fnpos++;
    fnpos = skiptonext(fn, fnlen, fnpos);
    
    if (fnpos < fnlen && fn[fnpos] == '{') fnpos++;
    size_t body_len = fnlen - fnpos - 1;
    
    jsval_t saved_this = js->this_val;
    js->this_val = js_glob(js);
    
    js->flags = F_CALL;
    jsval_t res = js_eval(js, &fn[fnpos], body_len);
    if (!is_err(res) && !(js->flags & F_RETURN)) res = js_mkundef();
    
    js->this_val = saved_this;
    delscope(js);
    
    return res;
  }
  return js_mkerr(js, "not a function");
}

#ifdef JS_DUMP
void js_dump(struct js *js) {
  jsoff_t off = 0, v;
  printf("JS size %u, brk %u, lwm %u, css %u, nogc %u\n", js->size, js->brk, js->lwm, (unsigned) js->css, js->nogc);
  while (off < js->brk) {
    memcpy(&v, &js->mem[off], sizeof(v));
    printf(" %5u: ", off);
    jsoff_t cleaned = v & ~(GCMASK | CONSTMASK);
    if ((cleaned & 3U) == T_OBJ) {
      printf("OBJ %u %u%s\n", cleaned & ~3U, loadoff(js, (jsoff_t) (off + sizeof(off))), (v & CONSTMASK) ? " [CONST]" : "");
    } else if ((cleaned & 3U) == T_PROP) {
      jsoff_t koff = loadoff(js, (jsoff_t) (off + sizeof(v)));
      jsval_t val = loadval(js, (jsoff_t) (off + sizeof(v) + sizeof(v)));
      printf("PROP next %u, koff %u vtype %d vdata %lu%s\n", cleaned & ~3U, koff, vtype(val), (unsigned long) vdata(val), (v & CONSTMASK) ? " [CONST]" : "");
    } else if ((cleaned & 3) == T_STR) {
      jsoff_t len = offtolen(cleaned);
      printf("STR %u [%.*s]\n", len, (int) len, js->mem + off + sizeof(v));
    } else {
      printf("???\n");
      break;
    }
    off += esize(v & ~(GCMASK | CONSTMASK));
  }
}
#endif
