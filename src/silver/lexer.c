#include "silver/lexer.h"

#include "escape.h"
#include "internal.h"
#include "tokens.h"
#include "utf8.h"
#include "errors.h"

#include <runtime.h>
#include <math.h>
#include <string.h>

void sv_lexer_init(sv_lexer_t *lx, ant_t *js, const char *code, jsoff_t clen, bool strict) {
  lx->js = js;
  lx->code = code;
  lx->clen = clen;
  lx->strict = strict;
  lx->st.pos = 0;
  lx->st.toff = 0;
  lx->st.tlen = 0;
  lx->st.tval = js_mkundef();
  lx->st.tok = TOK_ERR;
  lx->st.consumed = 1;
  lx->st.had_newline = false;
}

void sv_lexer_set_error_site(sv_lexer_t *lx) {
  ant_t *js = lx->js;
  jsoff_t off = lx->st.toff > 0 ? lx->st.toff : lx->st.pos;
  js_set_error_site(js, lx->code, lx->clen, js->filename, off, lx->st.tlen);
}

void sv_lexer_save_state(const sv_lexer_t *lx, sv_lexer_state_t *st) {
  *st = lx->st;
}

void sv_lexer_restore_state(sv_lexer_t *lx, const sv_lexer_state_t *st) {
  lx->st = *st;
}

void sv_lexer_push_source(sv_lexer_t *lx, sv_lexer_checkpoint_t *cp, const char *code, jsoff_t clen) {
  cp->code = lx->code;
  cp->clen = lx->clen;
  cp->strict = lx->strict;
  cp->st = lx->st;

  lx->code = code;
  lx->clen = clen;
  lx->st.pos = 0;
  lx->st.toff = 0;
  lx->st.tlen = 0;
  lx->st.tval = js_mkundef();
  lx->st.tok = TOK_ERR;
  lx->st.consumed = 1;
  lx->st.had_newline = false;
}

void sv_lexer_pop_source(sv_lexer_t *lx, const sv_lexer_checkpoint_t *cp) {
  lx->code = cp->code;
  lx->clen = cp->clen;
  lx->strict = cp->strict;
  lx->st = cp->st;
}

sv_lex_string_t sv_lexer_str_literal(sv_lexer_t *lx) {
  ant_t *js = lx->js;
  sv_lex_string_t outv = { .str = NULL, .len = 0, .ok = false };
  uint8_t *in = (uint8_t *)&lx->code[lx->st.toff];
  size_t n1 = 0, n2 = 0;
  size_t cap = (size_t)lx->st.tlen;
  if (cap == 0) {
    outv.str = "";
    outv.ok = true;
    return outv;
  }
  uint8_t *out = code_arena_bump(cap);
  if (!out) {
    (void)js_mkerr(js, "oom");
    return outv;
  }
  while (n2++ + 2 < (size_t)lx->st.tlen) {
    if (in[n2] == '\\') {
      if (lx->strict && is_octal_escape(in, n2)) {
        sv_lexer_set_error_site(lx);
        (void)js_mkerr_typed(js, JS_ERR_SYNTAX,
          "Octal escape sequences are not allowed in strict mode.");
        return outv;
      }
      size_t extra = decode_escape(in, n2, (size_t)lx->st.tlen, out, &n1, in[0]);
      n2 += extra + 1;
    } else {
      out[n1++] = ((uint8_t *)lx->code)[lx->st.toff + n2];
    }
  }
  outv.str = (const char *)out;
  outv.len = (uint32_t)n1;
  outv.ok = true;
  return outv;
}

static int is_unicode_space(const unsigned char *p, jsoff_t remaining, bool *is_line_term) {
  if (is_line_term) *is_line_term = false;
  if (p[0] < 0x80) return 0;

  utf8proc_int32_t cp;
  utf8proc_ssize_t n = utf8_next(p, (utf8proc_ssize_t)remaining, &cp);
  
  if (cp < 0) return 0;
  if (cp == 0xFEFF) return (int)n;

  utf8proc_category_t cat = utf8proc_category(cp);
  if (cat == UTF8PROC_CATEGORY_ZS) return (int)n;
  
  if (cat == UTF8PROC_CATEGORY_ZL || cat == UTF8PROC_CATEGORY_ZP) {
    if (is_line_term) *is_line_term = true;
    return (int)n;
  }

  return 0;
}

enum { C_0 = 0, C_SPC, C_NL, C_SL, C_HI };

static const uint8_t cc[128] = {
  0,0,0,0,0,0,0,0,0,C_SPC,C_NL,C_SPC,C_SPC,C_SPC,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  C_SPC,0,0,0,0,0,0,0,0,0,0,0,0,0,0,C_SL,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

static jsoff_t sv_skiptonext(const char *code, jsoff_t len, jsoff_t n, bool *nl) {
  static const void *D[] = { &&L0, &&LS, &&LN, &&LSL, &&LH };
  bool saw_nl = false;
  unsigned char c;

  const char *p = code + n;
  const char *end = code + len;

  if (__builtin_expect(p == code && end - p >= 2 && p[0] == '#' && p[1] == '!', 0)) {
    for (p += 2; p < end && *p != '\n'; p++);
    if (p < end) { saw_nl = true; p++; }
  }

  if (__builtin_expect(p >= end, 0)) goto L0;
  c = (unsigned char)*p;
  goto *D[c & 0x80 ? C_HI : cc[c]];

LS:
  p++;
  if (__builtin_expect(p >= end, 0)) goto L0;
  c = (unsigned char)*p;
  goto *D[c & 0x80 ? C_HI : cc[c]];

LN:
  saw_nl = true;
  p++;
  if (__builtin_expect(p >= end, 0)) goto L0;
  c = (unsigned char)*p;
  goto *D[c & 0x80 ? C_HI : cc[c]];

LSL:
  if (p + 1 >= end) goto L0;
  if (p[1] == '/') {
    for (p += 2; p < end && *p != '\n'; p++);
    if (p < end) { saw_nl = true; p++; }
    if (__builtin_expect(p >= end, 0)) goto L0;
    c = (unsigned char)*p;
    goto *D[c & 0x80 ? C_HI : cc[c]];
  }
  if (p[1] == '*') {
    for (p += 2; p + 1 < end; p++) {
      if (*p == '*' && p[1] == '/') { p += 2; break; }
      if (*p == '\n') saw_nl = true;
    }
    if (__builtin_expect(p >= end, 0)) goto L0;
    c = (unsigned char)*p;
    goto *D[c & 0x80 ? C_HI : cc[c]];
  }
  goto L0;

LH: {
  bool lt;
  int u = is_unicode_space((const unsigned char *)p, (jsoff_t)(end - p), &lt);
  if (u > 0) {
    if (lt) saw_nl = true;
    p += u;
    if (__builtin_expect(p >= end, 0)) goto L0;
    c = (unsigned char)*p;
    goto *D[c & 0x80 ? C_HI : cc[c]];
  }
}

L0:
  if (nl) *nl = saw_nl;
  return (jsoff_t)(p - code);
}

#define K(s, t) if (len == sizeof(s)-1 && !memcmp(buf, s, sizeof(s)-1)) return t
#define M(s) (len == sizeof(s)-1 && !memcmp(buf, s, sizeof(s)-1))

bool is_eval_or_arguments_name(const char *buf, size_t len) {
  switch (buf[0]) {
    case 'a': if M("arguments") return true; break;
    case 'e': if M("eval") return true; break;
  }
  return false;
}

bool is_strict_reserved_name(const char *buf, size_t len) {
  switch (buf[0]) {
    case 'i':
      if M("interface") return true;
      if M("implements") return true;
      break;
    case 'l':
      if M("let") return true;
      break;
    case 'p':
      if M("private") return true;
      if M("package") return true;
      if M("public") return true;
      if M("protected") return true;
      break;
    case 's':
      if M("static") return true;
      break;
    case 'y':
      if M("yield") return true;
      break;
  }
  return false;
}

static uint8_t parsekeyword(const char *buf, size_t len) {
  switch (buf[0]) {
    case 'a':
      K("as", TOK_AS);
      K("async", TOK_ASYNC);
      K("await", TOK_AWAIT);
      break;
    case 'b':
      K("break", TOK_BREAK);
      break;
    case 'c':
      K("case", TOK_CASE);
      K("catch", TOK_CATCH);
      K("class", TOK_CLASS);
      K("const", TOK_CONST);
      K("continue", TOK_CONTINUE);
      break;
    case 'd':
      K("do", TOK_DO);
      K("default", TOK_DEFAULT);
      K("delete", TOK_DELETE);
      K("debugger", TOK_DEBUGGER);
      break;
    case 'e':
      K("else", TOK_ELSE);
      K("export", TOK_EXPORT);
      break;
    case 'f':
      K("for", TOK_FOR);
      K("from", TOK_FROM);
      K("false", TOK_FALSE);
      K("finally", TOK_FINALLY);
      K("function", TOK_FUNC);
      break;
    case 'g':
      K("globalThis", TOK_GLOBAL_THIS);
      break;
    case 'i':
      K("if", TOK_IF);
      K("in", TOK_IN);
      K("import", TOK_IMPORT);
      K("instanceof", TOK_INSTANCEOF);
      break;
    case 'l':
      K("let", TOK_LET);
      break;
    case 'n':
      K("new", TOK_NEW);
      K("null", TOK_NULL);
      break;
    case 'o':
      K("of", TOK_OF);
      break;
    case 'r':
      K("return", TOK_RETURN);
      break;
    case 's':
      K("super", TOK_SUPER);
      K("static", TOK_STATIC);
      K("switch", TOK_SWITCH);
      break;
    case 't':
      K("try", TOK_TRY);
      K("this", TOK_THIS);
      K("true", TOK_TRUE);
      K("throw", TOK_THROW);
      K("typeof", TOK_TYPEOF);
      break;
    case 'u':
      K("undefined", TOK_UNDEF);
      break;
    case 'v':
      K("var", TOK_VAR);
      K("void", TOK_VOID);
      break;
    case 'w':
      K("while", TOK_WHILE);
      K("with", TOK_WITH);
      K("window", TOK_WINDOW);
      break;
    case 'y':
      K("yield", TOK_YIELD);
      break;
  }
  return TOK_IDENTIFIER;
}

#undef K
#undef M

#define CHAR_DIGIT  0x01
#define CHAR_XDIGIT 0x02
#define CHAR_ALPHA  0x04
#define CHAR_IDENT  0x08
#define CHAR_IDENT1 0x10
#define CHAR_WS     0x20
#define CHAR_OCTAL  0x40

static const uint8_t char_type[256] = {
  ['\t'] = CHAR_WS, ['\n'] = CHAR_WS, ['\r'] = CHAR_WS, [' '] = CHAR_WS,
  ['0'] = CHAR_DIGIT | CHAR_XDIGIT | CHAR_IDENT | CHAR_OCTAL,
  ['1'] = CHAR_DIGIT | CHAR_XDIGIT | CHAR_IDENT | CHAR_OCTAL,
  ['2'] = CHAR_DIGIT | CHAR_XDIGIT | CHAR_IDENT | CHAR_OCTAL,
  ['3'] = CHAR_DIGIT | CHAR_XDIGIT | CHAR_IDENT | CHAR_OCTAL,
  ['4'] = CHAR_DIGIT | CHAR_XDIGIT | CHAR_IDENT | CHAR_OCTAL,
  ['5'] = CHAR_DIGIT | CHAR_XDIGIT | CHAR_IDENT | CHAR_OCTAL,
  ['6'] = CHAR_DIGIT | CHAR_XDIGIT | CHAR_IDENT | CHAR_OCTAL,
  ['7'] = CHAR_DIGIT | CHAR_XDIGIT | CHAR_IDENT | CHAR_OCTAL,
  ['8'] = CHAR_DIGIT | CHAR_XDIGIT | CHAR_IDENT,
  ['9'] = CHAR_DIGIT | CHAR_XDIGIT | CHAR_IDENT,
  ['A'] = CHAR_XDIGIT | CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['B'] = CHAR_XDIGIT | CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['C'] = CHAR_XDIGIT | CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['D'] = CHAR_XDIGIT | CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['E'] = CHAR_XDIGIT | CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['F'] = CHAR_XDIGIT | CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['a'] = CHAR_XDIGIT | CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['b'] = CHAR_XDIGIT | CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['c'] = CHAR_XDIGIT | CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['d'] = CHAR_XDIGIT | CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['e'] = CHAR_XDIGIT | CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['f'] = CHAR_XDIGIT | CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['G'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['H'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['I'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['J'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['K'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['L'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['M'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['N'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['O'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['P'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['Q'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['R'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['S'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['T'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['U'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['V'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['W'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['X'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['Y'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['Z'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['g'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['h'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['i'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['j'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['k'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['l'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['m'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['n'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['o'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['p'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['q'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['r'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['s'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['t'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['u'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['v'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['w'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['x'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['y'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1, ['z'] = CHAR_ALPHA | CHAR_IDENT | CHAR_IDENT1,
  ['_'] = CHAR_IDENT | CHAR_IDENT1,
  ['$'] = CHAR_IDENT | CHAR_IDENT1,
};

#define IS_DIGIT(c)  (char_type[(uint8_t)(c)] & CHAR_DIGIT)
#define IS_XDIGIT(c) (char_type[(uint8_t)(c)] & CHAR_XDIGIT)
#define IS_IDENT(c)  (char_type[(uint8_t)(c)] & CHAR_IDENT)
#define IS_IDENT1(c) (char_type[(uint8_t)(c)] & CHAR_IDENT1)
#define IS_OCTAL(c)  (char_type[(uint8_t)(c)] & CHAR_OCTAL)

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

static const uint8_t single_char_tok[128] = {
  ['('] = TOK_LPAREN,
  [')'] = TOK_RPAREN,
  ['{'] = TOK_LBRACE,
  ['}'] = TOK_RBRACE,
  ['['] = TOK_LBRACKET,
  [']'] = TOK_RBRACKET,
  [';'] = TOK_SEMICOLON,
  [','] = TOK_COMMA,
  [':'] = TOK_COLON,
  ['~'] = TOK_TILDA,
  ['#'] = TOK_HASH,
};

bool is_space(int c) { 
  if (c < 0 || c >= 256) return false;
  return (char_type[(uint8_t)c] & CHAR_WS) != 0;
}

bool is_digit(int c) { 
  if (c < 0 || c >= 256) return false;
  return (char_type[(uint8_t)c] & CHAR_DIGIT) != 0;
}

static bool is_ident_begin(int c) { 
  if (c < 0) return false;
  if (c < 128) return (char_type[(uint8_t)c] & CHAR_IDENT1) != 0;
  return (c & 0x80) != 0;
}

static bool is_ident_continue(int c) { 
  if (c < 0) return false;
  if (c < 128) return (char_type[(uint8_t)c] & (CHAR_IDENT | CHAR_IDENT1)) != 0;
  return (c & 0x80) != 0;
}

static int parse_unicode_escape(const char *buf, jsoff_t len, jsoff_t pos, uint32_t *codepoint) {
  if (pos + 3 >= len) return 0;
  if (buf[pos] != '\\' || buf[pos + 1] != 'u') return 0;

  if (buf[pos + 2] == '{') {
    jsoff_t i = pos + 3;
    uint32_t cp = 0;
    int ndigits = 0;
    while (i < len && is_xdigit((unsigned char)buf[i])) {
      cp = (cp << 4) | ((unsigned char)buf[i] <= '9' ? buf[i] - '0' : ((unsigned char)buf[i] | 0x20) - 'a' + 10);
      i++;
      ndigits++;
    }
    if (ndigits == 0 || i >= len || buf[i] != '}' || cp > 0x10FFFF) return 0;
    *codepoint = cp;
    return (int)(i - pos + 1);
  }

  if (pos + 5 >= len) return 0;
  uint32_t cp = 0;
  for (int i = 0; i < 4; i++) {
    int c = (unsigned char)buf[pos + 2 + i];
    if (!is_xdigit(c)) return 0;
    cp <<= 4;
    cp |= (c <= '9') ? (c - '0') : ((c | 0x20) - 'a' + 10);
  }
  *codepoint = cp;
  return 6;
}

static bool is_unicode_id_start(utf8proc_int32_t cp) {
  utf8proc_category_t cat = utf8proc_category(cp);
  return 
    cat == UTF8PROC_CATEGORY_LU || cat == UTF8PROC_CATEGORY_LL ||
    cat == UTF8PROC_CATEGORY_LT || cat == UTF8PROC_CATEGORY_LM ||
    cat == UTF8PROC_CATEGORY_LO || cat == UTF8PROC_CATEGORY_NL;
}

static bool is_unicode_id_continue(utf8proc_int32_t cp) {
  if (is_unicode_id_start(cp)) return true;
  utf8proc_category_t cat = utf8proc_category(cp);
  return 
    cat == UTF8PROC_CATEGORY_MN || cat == UTF8PROC_CATEGORY_MC ||
    cat == UTF8PROC_CATEGORY_ND || cat == UTF8PROC_CATEGORY_PC ||
    cp == 0x200C || cp == 0x200D;
}

static bool is_unicode_ident_begin(uint32_t cp) {
  if (cp < 128) return (char_type[(uint8_t)cp] & CHAR_IDENT1) != 0;
  return is_unicode_id_start((utf8proc_int32_t)cp);
}

static bool is_unicode_ident_continue(uint32_t cp) {
  if (cp < 128) return (char_type[(uint8_t)cp] & (CHAR_IDENT | CHAR_IDENT1)) != 0;
  return is_unicode_id_continue((utf8proc_int32_t)cp);
}

static size_t decode_ident_escapes(const char *src, size_t srclen, char *dst, size_t dstlen) {
  size_t si = 0, di = 0;
  while (si < srclen && di + 4 < dstlen) {
    uint32_t cp;
    int el = parse_unicode_escape(src, (jsoff_t)srclen, (jsoff_t)si, &cp);
    if (el > 0) {
      di += utf8_encode(cp, dst + di);
      si += el;
    } else dst[di++] = src[si++];
  }
  dst[di] = '\0';
  return di;
}

static uint8_t parseident(const char *buf, jsoff_t len, jsoff_t *tlen) {
  if (len == 0) return TOK_ERR;
  
  unsigned char c = (unsigned char)buf[0];
  jsoff_t i = 0;
  
  if (c < 128 && c != '\\' && is_ident_begin(c)) {
    i = 1;
    while (i < len) {
      c = (unsigned char)buf[i];
      if (c >= 128 || c == '\\') goto slow_path_continue;
      if (!is_ident_continue(c)) break;
      i++;
    }
    *tlen = i;
    return parsekeyword(buf, i);
  }
  
  if (c == '\\') {
    uint32_t first_cp;
    int esc_len = parse_unicode_escape(buf, len, 0, &first_cp);
    if (esc_len <= 0 || !is_unicode_ident_begin(first_cp)) return TOK_ERR;
    *tlen = esc_len;
    goto slow_path_loop;
  }
  
  if (c >= 128) {
    utf8proc_int32_t cp;
    utf8proc_ssize_t n = utf8_next((const utf8proc_uint8_t *)buf, (utf8proc_ssize_t)len, &cp);
    if (cp < 0 || !is_unicode_id_start(cp)) return TOK_ERR;
    i = (jsoff_t)n;
    *tlen = i;
    goto slow_path_loop;
  }
  
  return TOK_ERR;

slow_path_continue:
  *tlen = i;
  
slow_path_loop:;
  int has_escapes = (buf[0] == '\\');
  
  while (*tlen < len) {
    c = (unsigned char)buf[*tlen];
    
    if (c == '\\') {
      uint32_t cp;
      int el = parse_unicode_escape(buf, len, *tlen, &cp);
      if (el <= 0 || !is_unicode_ident_continue(cp)) break;
      *tlen += el;
      has_escapes = 1;
    } else if (c < 128) {
      if (!is_ident_continue(c)) break;
      (*tlen)++;
    } else {
      utf8proc_int32_t cp;
      utf8proc_ssize_t n = utf8_next(
        (const utf8proc_uint8_t *)&buf[*tlen],
        (utf8proc_ssize_t)(len - *tlen), &cp
      );
      if (cp < 0 || !is_unicode_id_continue(cp)) break;
      *tlen += (jsoff_t)n;
    }
  }
  
  if (has_escapes) {
    char decoded[256];
    size_t decoded_len = decode_ident_escapes(buf, *tlen, decoded, sizeof(decoded));
    uint8_t kw = parsekeyword(decoded, decoded_len);
    if (kw != TOK_IDENTIFIER) return TOK_ERR;
    return TOK_IDENTIFIER;
  }
  
  return parsekeyword(buf, *tlen);
}

static inline jsoff_t parse_decimal(const char *buf, jsoff_t maxlen, double *out) {
  uint64_t int_part = 0, frac_part = 0;
  int frac_digits = 0;
  jsoff_t i = 0;

  while (i < maxlen && (IS_DIGIT(buf[i]) || buf[i] == '_')) {
    if (buf[i] != '_') int_part = int_part * 10 + (buf[i] - '0');
    i++;
  }

  if (i < maxlen && buf[i] == '.') {
    i++;
    while (i < maxlen && (IS_DIGIT(buf[i]) || buf[i] == '_')) {
      if (buf[i] != '_') { frac_part = frac_part * 10 + (buf[i] - '0'); frac_digits++; }
      i++;
    }
  }

  static const double neg_pow10[] = {
    1e0,1e-1,1e-2,1e-3,1e-4,1e-5,1e-6,1e-7,1e-8,1e-9,1e-10,
    1e-11,1e-12,1e-13,1e-14,1e-15,1e-16,1e-17,1e-18,1e-19,1e-20
  };
  
  static const double pos_pow10[] = {
    1e0,1e1,1e2,1e3,1e4,1e5,1e6,1e7,1e8,1e9,1e10,
    1e11,1e12,1e13,1e14,1e15,1e16,1e17,1e18,1e19,1e20
  };

  double val = (double)int_part;
  if (frac_digits > 0) {
    val += (frac_digits <= 20) 
      ? (double)frac_part * neg_pow10[frac_digits] 
      : (double)frac_part * pow(10.0, -frac_digits);
  }

  if (i < maxlen && (buf[i] == 'e' || buf[i] == 'E')) {
    i++;
    int exp_sign = 1, exp_val = 0;
    if (i < maxlen && (buf[i] == '+' || buf[i] == '-')) {
      exp_sign = (buf[i] == '-') ? -1 : 1;
      i++;
    }
    while (i < maxlen && (IS_DIGIT(buf[i]) || buf[i] == '_')) {
      if (buf[i] != '_') exp_val = exp_val * 10 + (buf[i] - '0');
      i++;
    }
    if (exp_val <= 20) {
      val = (exp_sign > 0) ? val * pos_pow10[exp_val] : val * neg_pow10[exp_val];
    } else val *= pow(10.0, exp_sign * exp_val);
  }

  *out = val;
  return i;
}

static inline jsoff_t parse_binary(const char *buf, jsoff_t maxlen, double *out) {
  double val = 0;
  jsoff_t i = 2;
  while (i < maxlen && (buf[i] == '0' || buf[i] == '1' || buf[i] == '_')) {
    if (buf[i] != '_') val = val * 2 + (buf[i] - '0');
    i++;
  }
  *out = val;
  return i;
}

static inline jsoff_t parse_octal(const char *buf, jsoff_t maxlen, double *out) {
  double val = 0;
  jsoff_t i = 2;
  while (i < maxlen && (IS_OCTAL(buf[i]) || buf[i] == '_')) {
    if (buf[i] != '_') val = val * 8 + (buf[i] - '0');
    i++;
  }
  *out = val;
  return i;
}

static inline jsoff_t parse_legacy_octal(const char *buf, jsoff_t maxlen, double *out) {
  double val = 0;
  jsoff_t i = 1;
  while (i < maxlen && IS_OCTAL(buf[i])) {
      val = val * 8 + (buf[i] - '0');
      i++;
  }
  *out = val;
  return i;
}

static inline jsoff_t parse_hex(const char *buf, jsoff_t maxlen, double *out) {
  double val = 0;
  jsoff_t i = 2;
  while (i < maxlen && (IS_XDIGIT(buf[i]) || buf[i] == '_')) {
    if (buf[i] != '_') {
      int d = 
        (buf[i] >= 'a') ? (buf[i] - 'a' + 10) :
        (buf[i] >= 'A') ? (buf[i] - 'A' + 10) : (buf[i] - '0');
      val = val * 16 + d;
    } i++;
  }
  *out = val;
  return i;
}

static inline uint8_t parse_number(sv_lexer_t *lx, const char *buf, jsoff_t remaining) {
  double value = 0;
  jsoff_t numlen = 0;
  
  if (buf[0] == '0' && remaining > 1) {
    char c1 = buf[1] | 0x20; 
    if (c1 == 'b') {
      numlen = parse_binary(buf, remaining, &value);
    } else if (c1 == 'o') {
      numlen = parse_octal(buf, remaining, &value);
    } else if (c1 == 'x') {
      numlen = parse_hex(buf, remaining, &value);
    } else if (IS_OCTAL(buf[1])) {
        if (lx->strict) {
          lx->st.tok = TOK_ERR;
          lx->st.tlen = 1;
          return TOK_ERR;
        }
        numlen = parse_legacy_octal(buf, remaining, &value);
    } else if (is_digit(buf[1]) && lx->strict) {
        lx->st.tok = TOK_ERR;
        lx->st.tlen = 1;
        return TOK_ERR;
    } else numlen = parse_decimal(buf, remaining, &value);
  } else numlen = parse_decimal(buf, remaining, &value);
  
  lx->st.tval = tov(value);
  if (numlen < remaining && buf[numlen] == 'n') {
    lx->st.tok = TOK_BIGINT;
    lx->st.tlen = numlen + 1;
  } else {
    lx->st.tok = TOK_NUMBER;
    lx->st.tlen = numlen;
  }
  
  return lx->st.tok;
}

static inline uint8_t scan_string(sv_lexer_t *lx, const char *buf, jsoff_t rem, char quote) {
  jsoff_t i = 1;

  while (i < rem) {
    const char *p = buf + i;
    jsoff_t search_len = rem - i;

    const char *q = memchr(p, quote, search_len);
    const char *b = memchr(p, '\\', search_len);

    if (q == NULL) {
      lx->st.tok = TOK_ERR;
      lx->st.tlen = rem;
      return TOK_ERR;
    }

    if (b == NULL || q < b) {
      i = (jsoff_t)((q - buf) + 1);
      lx->st.tok = TOK_STRING;
      lx->st.tlen = i;
      return TOK_STRING;
    }

    jsoff_t esc_pos = (jsoff_t)(b - buf);
    if (esc_pos + 1 >= rem) {
      lx->st.tok = TOK_ERR;
      lx->st.tlen = rem;
      return TOK_ERR;
    }

    char esc_char = buf[esc_pos + 1];
    jsoff_t skip = 2;

    if (esc_char == 'x') { skip = 4; } else if (esc_char == 'u') {
      skip = (esc_pos + 2 < rem && buf[esc_pos + 2] == '{') ? 0 : 6;
      if (skip == 0) {
        jsoff_t j = esc_pos + 3;
        while (j < rem && buf[j] != '}') j++;
        skip = (j < rem) ? (j - esc_pos + 1) : (rem - esc_pos);
      }
    }

    if (esc_pos + skip > rem) {
      lx->st.tok = TOK_ERR;
      lx->st.tlen = rem;
      return TOK_ERR;
    }

    i = esc_pos + skip;
  }

  lx->st.tok = TOK_ERR;
  lx->st.tlen = rem;
  return TOK_ERR;
}

static inline jsoff_t skip_string_literal(const char *buf, jsoff_t rem, jsoff_t start, char quote) {
  jsoff_t i = start + 1;
  while (i < rem) {
    if (buf[i] == '\\') { i += 2; continue; }
    if (buf[i] == quote) { return i + 1; } i++;
  }
  return rem;
}

static inline jsoff_t skip_line_comment(const char *buf, jsoff_t rem, jsoff_t start) {
  jsoff_t i = start + 2;
  while (i < rem && buf[i] != '\n') i++;
  return i;
}

static inline jsoff_t skip_block_comment(const char *buf, jsoff_t rem, jsoff_t start) {
  jsoff_t i = start + 2;
  while (i + 1 < rem && !(buf[i] == '*' && buf[i + 1] == '/')) i++;
  return (i + 1 < rem) ? (i + 2) : rem;
}

static inline bool is_expr_ws(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static inline bool is_ident_ascii_start(char c) {
  return 
    (c == '_' || c == '$' ||
    (c >= 'a' && c <= 'z') ||
    (c >= 'A' && c <= 'Z'));
}

static inline bool is_ident_ascii_continue(char c) {
  return is_ident_ascii_start(c) || (c >= '0' && c <= '9');
}

static inline jsoff_t skip_regex_literal(const char *buf, jsoff_t rem, jsoff_t start) {
  jsoff_t i = start + 1;
  bool in_class = false;

  while (i < rem) {
    char c = buf[i];
    if (c == '\\' && i + 1 < rem) { i += 2; continue; }
    if (c == '[') in_class = true;
    else if (c == ']' && in_class) in_class = false;
    else if (c == '/' && !in_class) { i++; break; }
    i++;
  }

  while (i < rem && is_ident_ascii_start(buf[i])) i++;
  return i;
}

// todo: modularize
static inline bool regex_allowed_after_ident(const char *buf, jsoff_t start, jsoff_t end) {
  size_t n = (size_t)(end - start);
  if (n == 2 && !memcmp(buf + start, "if", 2)) return true;
  if (n == 2 && !memcmp(buf + start, "in", 2)) return true;
  if (n == 2 && !memcmp(buf + start, "do", 2)) return true;
  if (n == 2 && !memcmp(buf + start, "of", 2)) return true;
  if (n == 3 && !memcmp(buf + start, "for", 3)) return true;
  if (n == 4 && !memcmp(buf + start, "else", 4)) return true;
  if (n == 4 && !memcmp(buf + start, "case", 4)) return true;
  if (n == 5 && !memcmp(buf + start, "throw", 5)) return true;
  if (n == 6 && !memcmp(buf + start, "return", 6)) return true;
  if (n == 6 && !memcmp(buf + start, "typeof", 6)) return true;
  if (n == 6 && !memcmp(buf + start, "delete", 6)) return true;
  if (n == 4 && !memcmp(buf + start, "void", 4)) return true;
  if (n == 3 && !memcmp(buf + start, "new", 3)) return true;
  if (n == 10 && !memcmp(buf + start, "instanceof", 10)) return true;
  return false;
}

static jsoff_t skip_template_literal(const char *buf, jsoff_t rem, jsoff_t start) {
  jsoff_t i = start + 1;
  int expr_depth = 0;
  bool can_start_regex = true;

  while (i < rem) {
    char c = buf[i];

    if (c == '\\') {
      i += 2;
      continue;
    }

    if (expr_depth == 0) {
      if (c == '`') return i + 1;
      if (c == '$' && i + 1 < rem && buf[i + 1] == '{') {
        expr_depth = 1;
        can_start_regex = true;
        i += 2;
        continue;
      } i++; continue;
    }

    if (c == '\'' || c == '"') {
      i = skip_string_literal(buf, rem, i, c);
      continue;
    }

    if (c == '`') {
      jsoff_t next = skip_template_literal(buf, rem, i);
      if (next <= i) return rem;
      i = next;
      can_start_regex = false;
      continue;
    }

    if (c == '/' && i + 1 < rem) {
      if (buf[i + 1] == '/') { i = skip_line_comment(buf, rem, i); continue; }
      if (buf[i + 1] == '*') { i = skip_block_comment(buf, rem, i); continue; }
      if (can_start_regex) {
        i = skip_regex_literal(buf, rem, i);
        can_start_regex = false;
        continue;
      }
      i++;
      can_start_regex = true;
      continue;
    }

    if (is_expr_ws(c)) { i++; continue; }

    if (is_ident_ascii_start(c)) {
      jsoff_t id_start = i;
      i++;
      while (i < rem && is_ident_ascii_continue(buf[i])) i++;
      can_start_regex = regex_allowed_after_ident(buf, id_start, i);
      continue;
    }

    if ((c >= '0' && c <= '9') || (c == '.' && i + 1 < rem && (buf[i + 1] >= '0' && buf[i + 1] <= '9'))) {
      i++;
      while (i < rem) {
        char d = buf[i];
        if ((d >= '0' && d <= '9') || d == '_' || d == '.') i++;
        else break;
      }
      can_start_regex = false;
      continue;
    }

    if (c == '{') { expr_depth++; i++; can_start_regex = true; continue; }
    if (c == '}') { expr_depth--; i++; can_start_regex = false; continue; }

    if (c == '(' || c == '[' || c == ',' || c == ';' || c == ':' || c == '?' ||
        c == '!' || c == '~' || c == '+' || c == '-' || c == '*' || c == '%' ||
        c == '&' || c == '|' || c == '^' || c == '=' || c == '<' || c == '>') {
      i++;
      can_start_regex = true;
      continue;
    }

    if (c == ')' || c == ']') {
      i++;
      can_start_regex = false;
      continue;
    }

    i++;
    can_start_regex = false;
  }

  return rem;
}

static inline uint8_t scan_template(sv_lexer_t *lx, const char *buf, jsoff_t rem) {
  jsoff_t end = skip_template_literal(buf, rem, 0);
  if (end <= 1 || end > rem) {
    lx->st.tok = TOK_ERR;
    lx->st.tlen = rem;
    return TOK_ERR;
  }

  lx->st.tok = TOK_TEMPLATE;
  lx->st.tlen = end;
  return TOK_TEMPLATE;
}

static inline uint8_t parse_operator(sv_lexer_t *lx, const char *buf, jsoff_t rem) {
  #define MATCH2(c1,c2)       (rem >= 2 && buf[1] == (c2))
  #define MATCH3(c1,c2,c3)    (rem >= 3 && buf[1] == (c2) && buf[2] == (c3))
  #define MATCH4(c1,c2,c3,c4) (rem >= 4 && buf[1]==(c2) && buf[2]==(c3) && buf[3]==(c4))

  switch (buf[0]) {
  case '?':
    if (MATCH3('?','?','=')) { lx->st.tok = TOK_NULLISH_ASSIGN; lx->st.tlen = 3; }
    else if (MATCH2('?','?')) { lx->st.tok = TOK_NULLISH; lx->st.tlen = 2; }
    else if (MATCH2('?','.')) { lx->st.tok = TOK_OPTIONAL_CHAIN; lx->st.tlen = 2; }
    else { lx->st.tok = TOK_Q; lx->st.tlen = 1; }
    break;

  case '!':
    if (MATCH3('!','=','=')) { lx->st.tok = TOK_SNE; lx->st.tlen = 3; }
    else if (MATCH2('!','=')) { lx->st.tok = TOK_NE; lx->st.tlen = 2; }
    else { lx->st.tok = TOK_NOT; lx->st.tlen = 1; }
    break;

  case '=':
    if (MATCH3('=','=','=')) { lx->st.tok = TOK_SEQ; lx->st.tlen = 3; }
    else if (MATCH2('=','=')) { lx->st.tok = TOK_EQ; lx->st.tlen = 2; }
    else if (MATCH2('=','>')) { lx->st.tok = TOK_ARROW; lx->st.tlen = 2; }
    else { lx->st.tok = TOK_ASSIGN; lx->st.tlen = 1; }
    break;

  case '<':
    if (MATCH3('<','<','=')) { lx->st.tok = TOK_SHL_ASSIGN; lx->st.tlen = 3; }
    else if (MATCH2('<','<')) { lx->st.tok = TOK_SHL; lx->st.tlen = 2; }
    else if (MATCH2('<','=')) { lx->st.tok = TOK_LE; lx->st.tlen = 2; }
    else { lx->st.tok = TOK_LT; lx->st.tlen = 1; }
    break;

  case '>':
    if (MATCH4('>','>','>','=')) { lx->st.tok = TOK_ZSHR_ASSIGN; lx->st.tlen = 4; }
    else if (MATCH3('>','>','>')) { lx->st.tok = TOK_ZSHR; lx->st.tlen = 3; }
    else if (MATCH3('>','>','=')) { lx->st.tok = TOK_SHR_ASSIGN; lx->st.tlen = 3; }
    else if (MATCH2('>','>')) { lx->st.tok = TOK_SHR; lx->st.tlen = 2; }
    else if (MATCH2('>','=')) { lx->st.tok = TOK_GE; lx->st.tlen = 2; }
    else { lx->st.tok = TOK_GT; lx->st.tlen = 1; }
    break;

  case '&':
    if (MATCH3('&','&','=')) { lx->st.tok = TOK_LAND_ASSIGN; lx->st.tlen = 3; }
    else if (MATCH2('&','&')) { lx->st.tok = TOK_LAND; lx->st.tlen = 2; }
    else if (MATCH2('&','=')) { lx->st.tok = TOK_AND_ASSIGN; lx->st.tlen = 2; }
    else { lx->st.tok = TOK_AND; lx->st.tlen = 1; }
    break;

  case '|':
    if (MATCH3('|','|','=')) { lx->st.tok = TOK_LOR_ASSIGN; lx->st.tlen = 3; }
    else if (MATCH2('|','|')) { lx->st.tok = TOK_LOR; lx->st.tlen = 2; }
    else if (MATCH2('|','=')) { lx->st.tok = TOK_OR_ASSIGN; lx->st.tlen = 2; }
    else { lx->st.tok = TOK_OR; lx->st.tlen = 1; }
    break;

  case '+':
    if (MATCH2('+','+')) { lx->st.tok = TOK_POSTINC; lx->st.tlen = 2; }
    else if (MATCH2('+','=')) { lx->st.tok = TOK_PLUS_ASSIGN; lx->st.tlen = 2; }
    else { lx->st.tok = TOK_PLUS; lx->st.tlen = 1; }
    break;

  case '-':
    if (MATCH2('-','-')) { lx->st.tok = TOK_POSTDEC; lx->st.tlen = 2; }
    else if (MATCH2('-','=')) { lx->st.tok = TOK_MINUS_ASSIGN; lx->st.tlen = 2; }
    else { lx->st.tok = TOK_MINUS; lx->st.tlen = 1; }
    break;

  case '*':
    if (MATCH3('*','*','=')) { lx->st.tok = TOK_EXP_ASSIGN; lx->st.tlen = 3; }
    else if (MATCH2('*','*')) { lx->st.tok = TOK_EXP; lx->st.tlen = 2; }
    else if (MATCH2('*','=')) { lx->st.tok = TOK_MUL_ASSIGN; lx->st.tlen = 2; }
    else { lx->st.tok = TOK_MUL; lx->st.tlen = 1; }
    break;

  case '/':
    if (MATCH2('/','=')) { lx->st.tok = TOK_DIV_ASSIGN; lx->st.tlen = 2; }
    else { lx->st.tok = TOK_DIV; lx->st.tlen = 1; }
    break;

  case '%':
    if (MATCH2('%','=')) { lx->st.tok = TOK_REM_ASSIGN; lx->st.tlen = 2; }
    else { lx->st.tok = TOK_REM; lx->st.tlen = 1; }
    break;

  case '^':
    if (MATCH2('^','=')) { lx->st.tok = TOK_XOR_ASSIGN; lx->st.tlen = 2; }
    else { lx->st.tok = TOK_XOR; lx->st.tlen = 1; }
    break;

  case '.':
    if (MATCH3('.','.', '.')) { lx->st.tok = TOK_REST; lx->st.tlen = 3; }
    else if (rem > 1 && IS_DIGIT(buf[1])) {
      double val;
      lx->st.tlen = parse_decimal(buf, rem, &val);
      lx->st.tval = tov(val);
      lx->st.tok = TOK_NUMBER;
    }
    else { lx->st.tok = TOK_DOT; lx->st.tlen = 1; }
    break;

  default:
    return 0;
  }

  #undef MATCH2
  #undef MATCH3
  #undef MATCH4

  return lx->st.tok;
}

static uint8_t sv_next_raw(sv_lexer_t *lx) {
  if (likely(lx->st.consumed == 0)) return lx->st.tok;

  lx->st.consumed = 0;
  lx->st.tok = TOK_ERR;
  lx->st.toff = lx->st.pos = sv_skiptonext(lx->code, lx->clen, lx->st.pos, &lx->st.had_newline);
  lx->st.tlen = 0;

  if (unlikely(lx->st.toff >= lx->clen)) {
    lx->st.tok = TOK_EOF;
    return TOK_EOF;
  }

  const char *buf = lx->code + lx->st.toff;
  jsoff_t rem = lx->clen - lx->st.toff;
  uint8_t c = (uint8_t)buf[0];

  if (likely(c < 128)) {
    uint8_t simple_tok = single_char_tok[c];
    if (simple_tok != 0) {
      lx->st.tok = simple_tok;
      lx->st.tlen = 1;
      lx->st.pos = lx->st.toff + 1;
      return simple_tok;
    }
  }

  if (likely(IS_IDENT1(c))) {
    lx->st.tok = parseident(buf, rem, &lx->st.tlen);
    lx->st.pos = lx->st.toff + lx->st.tlen;
    return lx->st.tok;
  }

  if (IS_DIGIT(c)) {
    parse_number(lx, buf, rem);
    if (lx->st.tlen == 0) lx->st.tlen = 1;
    lx->st.pos = lx->st.toff + lx->st.tlen;
    return lx->st.tok;
  }

  if (c == '"' || c == '\'') {
    scan_string(lx, buf, rem, c);
    if (lx->st.tlen == 0) lx->st.tlen = 1;
    lx->st.pos = lx->st.toff + lx->st.tlen;
    return lx->st.tok;
  }

  if (c == '`') {
    scan_template(lx, buf, rem);
    if (lx->st.tlen == 0) lx->st.tlen = 1;
    lx->st.pos = lx->st.toff + lx->st.tlen;
    return lx->st.tok;
  }

  if (parse_operator(lx, buf, rem)) {
    if (lx->st.tlen == 0) lx->st.tlen = 1;
    lx->st.pos = lx->st.toff + lx->st.tlen;
    return lx->st.tok;
  }

  lx->st.tok = parseident(buf, rem, &lx->st.tlen);
  if (lx->st.tlen == 0) lx->st.tlen = 1;
  lx->st.pos = lx->st.toff + lx->st.tlen;
  
  return lx->st.tok;
}

uint8_t sv_lexer_next(sv_lexer_t *lx) {
  if (likely(lx->st.consumed == 0)) return lx->st.tok;
  return sv_next_raw(lx);
}

uint8_t sv_lexer_lookahead(sv_lexer_t *lx) {
  uint8_t old = lx->st.tok, tok = 0;
  uint8_t old_consumed = lx->st.consumed;
  
  jsoff_t pos = lx->st.pos;
  jsoff_t toff = lx->st.toff;
  jsoff_t tlen = lx->st.tlen;
  
  bool had_newline = lx->st.had_newline;

  lx->st.consumed = 1;
  tok = sv_lexer_next(lx);
  
  lx->st.pos = pos;
  lx->st.tok = old;
  lx->st.toff = toff;
  lx->st.tlen = tlen;
  
  lx->st.had_newline = had_newline;
  lx->st.consumed = old_consumed;
  
  return tok;
}
