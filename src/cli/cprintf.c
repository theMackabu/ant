/*
 * cprintf - printf with inline color tags, powered by a register-based VM
 *
 * usage:
 *   cprintf("<red>error:</red> something went wrong\n");
 *   cprintf("<bold><cyan>info:</cyan></bold> hello %s\n", name);
 *   cprintf("<#ff8800>orange text</#ff8800>\n");
 *   cprintf("  <pad=18><green>%s</green></pad> %s\n", cmd->name, cmd->desc);
 *
 * supported tags:
 *   <red> <green> <yellow> <blue> <magenta> <cyan> <white> <black>
 *   <gray/grey> <bright_red> <bright_green> ... etc
 *   <bg_red> <bg_green> ... <bg_#RGB> <bg_#RRGGBB>
 *   <bold> <dim> <ul> (underline)
 *   <bold_red> <dim_cyan> etc - combine styles with underscores
 *   <bold+red> <dim+cyan+bg_blue> etc - combine styles with +
 *   <#RRGGBB> or <#RGB> for arbitrary 24-bit foreground colors
 *   <pad=N> ... </pad>  - right-pad contents to N visible columns
 *   <br/> - emit a newline, <br=N/> - emit N newlines
 *   <rpad=N> ... </rpad> - left-pad (right-align) contents to N visible columns
 *   <space=N/> - emit N spaces
 *   <gap=N/> - emit N spaces (alias for space)
 *   <let name=style1+style2+...> or <let name=style1, name2=style2...> - define a named style variable
 *   {let name=style1+style2+...} or {let name=style1, name2=style2...} - alternative syntax
 *   quoted values: {let label='hello'} or <let label="world"/> - quotes are stripped from value
 *   <$name> to apply a variable as a style, {name} to emit its value as literal text
 *   {~name} for lowercase, {^name} for uppercase
 *   </tagname> or </> to reset (pops one level)
 *   <reset/> to reset all styles (clears entire stack)
 *   << and >> to emit literal < and >
 *   %% to emit a literal %
 *
 */

#include "utils.h"
#include "cli/cprintf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <wchar.h>

bool cprintf_debug = false;
bool cprintf_debug_hex = false;

typedef enum {
  OP_NOP = 0,
  OP_EMIT_LIT,
  OP_EMIT_FMT,
  OP_SET_FG,
  OP_SET_BG,
  OP_SET_FG_RGB,
  OP_SET_BG_RGB,
  OP_SET_BOLD,
  OP_SET_DIM,
  OP_SET_UL,
  OP_STYLE_PUSH,
  OP_STYLE_FLUSH,
  OP_STYLE_RESET,
  OP_STYLE_RESET_ALL,
  OP_PAD_BEGIN,
  OP_RPAD_BEGIN,
  OP_PAD_END,
  OP_EMIT_SPACES,
  OP_EMIT_NEWLINES,
  OP_HALT,
  OP_MAX
} opcode_t;

typedef struct {
  uint32_t op;
  uint32_t operand;
} instruction_t;

typedef enum {
  COL_NONE = 0,
  COL_BLACK = 30, COL_RED, COL_GREEN, COL_YELLOW,
  COL_BLUE, COL_MAGENTA, COL_CYAN, COL_WHITE,
  COL_GRAY = 90, COL_BRIGHT_RED, COL_BRIGHT_GREEN, COL_BRIGHT_YELLOW,
  COL_BRIGHT_BLUE, COL_BRIGHT_MAGENTA, COL_BRIGHT_CYAN, COL_BRIGHT_WHITE,
  COL_RGB = 0xFF
} color_t;

#define UNPACK_R(c) (((c)>>16)&0xFF)
#define UNPACK_G(c) (((c)>>8)&0xFF)
#define UNPACK_B(c) ((c)&0xFF)

#define PACK_RGB(r,g,b) \
  (((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(uint32_t)(b))

typedef struct {
  size_t mark;
  int width;
  int right_align;
} pad_entry_t;

typedef struct {
  uint32_t fg;
  uint32_t bg;
  uint32_t fg_rgb;
  uint32_t bg_rgb;
  int bold;
  int dim;
  int ul;
} style_entry_t;
  
typedef struct {
  uint32_t fg;
  uint32_t bg;
  uint32_t fg_rgb;
  uint32_t bg_rgb;

  int bold;
  int dim;
  int ul;

  style_entry_t style_stack[8];
  int style_depth;

  pad_entry_t pad_stack[8];
  int pad_depth;
} vm_regs_t;

#define MAX_VARS 16
#define MAX_VAR_NAME 32
#define MAX_VAR_VALUE 128

typedef struct {
  char name[MAX_VAR_NAME];
  char value[MAX_VAR_VALUE];
  int nlen; int vlen; int is_fmt;
} cprintf_var_t;

typedef struct {
  cprintf_var_t vars[MAX_VARS];
  int count;
} var_table_t;

static var_table_t global_vars = {0};
static const char *scan_var_brace(program_t *p, const char *ptr, const char **lit, var_table_t *vars);

struct program_t {
  instruction_t *code;
  size_t code_len;
  size_t code_cap;
  char *literals;
  size_t lit_len;
  size_t lit_cap;
};

static program_t *program_new(void) {
  program_t *p = calloc(1, sizeof(*p));
  *p = (program_t){
    .code_cap = 32,
    .code = malloc(32 * sizeof(instruction_t)),
    .lit_cap = 256,
    .literals = malloc(256),
  };
  return p;
}

static void emit_op(program_t *p, uint32_t op, uint32_t operand) {
  if (p->code_len >= p->code_cap) {
    p->code_cap *= 2;
    p->code = realloc(p->code, p->code_cap * sizeof(instruction_t));
  }
  p->code[p->code_len++] = (instruction_t){ op, operand };
}

static uint32_t add_literal(program_t *p, const char *s, size_t len) {
  while (p->lit_len + len + 1 > p->lit_cap) {
    p->lit_cap *= 2;
    p->literals = realloc(p->literals, p->lit_cap);
  }
  
  uint32_t off = (uint32_t)p->lit_len;
  memcpy(p->literals + p->lit_len, s, len);
  
  p->literals[p->lit_len + len] = '\0';
  p->lit_len += len + 1;
  
  return off;
}

typedef struct { 
  const char *name; 
  int nlen; 
  color_t col; 
} color_entry_t;

static const color_entry_t fg_colors[] = {
  {"black",5,COL_BLACK}, {"red",3,COL_RED}, {"green",5,COL_GREEN},
  {"yellow",6,COL_YELLOW}, {"blue",4,COL_BLUE}, {"magenta",7,COL_MAGENTA},
  {"cyan",4,COL_CYAN}, {"white",5,COL_WHITE},
  {"gray",4,COL_GRAY}, {"grey",4,COL_GRAY},
  {"bright_red",10,COL_BRIGHT_RED}, {"bright_green",12,COL_BRIGHT_GREEN},
  {"bright_yellow",13,COL_BRIGHT_YELLOW}, {"bright_blue",11,COL_BRIGHT_BLUE},
  {"bright_magenta",14,COL_BRIGHT_MAGENTA}, {"bright_cyan",11,COL_BRIGHT_CYAN},
  {"bright_white",12,COL_BRIGHT_WHITE},
}; 

static const color_entry_t bg_colors[] = {
  {"bg_black",8,COL_BLACK}, {"bg_red",6,COL_RED}, {"bg_green",8,COL_GREEN},
  {"bg_yellow",9,COL_YELLOW}, {"bg_blue",7,COL_BLUE}, {"bg_magenta",10,COL_MAGENTA},
  {"bg_cyan",7,COL_CYAN}, {"bg_white",8,COL_WHITE},
};

static const color_entry_t seg_bg_colors[] = {
  {"black",5,COL_BLACK}, {"red",3,COL_RED}, {"green",5,COL_GREEN},
  {"yellow",6,COL_YELLOW}, {"blue",4,COL_BLUE}, {"magenta",7,COL_MAGENTA},
  {"cyan",4,COL_CYAN}, {"white",5,COL_WHITE},
};

#define FG_COUNT (sizeof(fg_colors)/sizeof(fg_colors[0]))
#define BG_COUNT (sizeof(bg_colors)/sizeof(bg_colors[0]))
#define SEG_BG_COUNT (sizeof(seg_bg_colors)/sizeof(seg_bg_colors[0]))

static int parse_hex_rgb(const char *hex, int len, uint32_t *rgb) {
  int r, g, b;
  if (len == 4) {
    int r1=hex_digit(hex[1]), g1=hex_digit(hex[2]), b1=hex_digit(hex[3]);
    if (r1<0||g1<0||b1<0) return 0;
    r=r1*17; g=g1*17; b=b1*17;
  } else if (len == 7) {
    int r1=hex_digit(hex[1]),r2=hex_digit(hex[2]);
    int g1=hex_digit(hex[3]),g2=hex_digit(hex[4]);
    int b1=hex_digit(hex[5]),b2=hex_digit(hex[6]);
    if (r1<0||r2<0||g1<0||g2<0||b1<0||b2<0) return 0;
    r=r1*16+r2; g=g1*16+g2; b=b1*16+b2;
  } else return 0;
  *rgb = PACK_RGB(r, g, b);
  return 1;
}

static int compile_hex_fg(program_t *p, const char *tag, int len) {
  uint32_t rgb;
  if (!parse_hex_rgb(tag, len, &rgb)) return 0;
  emit_op(p, OP_SET_FG_RGB, rgb);
  return 1;
}

static int compile_hex_bg(program_t *p, const char *hex, int hlen) {
  uint32_t rgb;
  if (!parse_hex_rgb(hex, hlen, &rgb)) return 0;
  emit_op(p, OP_SET_BG_RGB, rgb);
  return 1;
}

static int tag_eq(const char *tag, int len, const char *lit) {
  int ll = (int)strlen(lit);
  return len == ll && memcmp(tag, lit, ll) == 0;
}

static int tag_prefix(const char *tag, int len, const char *pfx) {
  int pl = (int)strlen(pfx);
  return len > pl && memcmp(tag, pfx, pl) == 0;
}

static int match_fg(program_t *p, const char *s, int len) {
  for (int i = 0; i < (int)FG_COUNT; i++) if (
    len == fg_colors[i].nlen && memcmp(s, fg_colors[i].name, len) == 0) {
    emit_op(p, OP_SET_FG, fg_colors[i].col); return 1;
  }
  return 0;
}

static int match_bg(program_t *p, const char *s, int len) {
  for (int i = 0; i < (int)BG_COUNT; i++) if (
    len == bg_colors[i].nlen && memcmp(s, bg_colors[i].name, len) == 0) {
    emit_op(p, OP_SET_BG, bg_colors[i].col); return 1; 
  }
  return 0;
}

static int match_seg_bg(program_t *p, const char *s, int len) {
  for (int i = 0; i < (int)SEG_BG_COUNT; i++) if (
    len == seg_bg_colors[i].nlen && memcmp(s, seg_bg_colors[i].name, len) == 0) { 
    emit_op(p, OP_SET_BG, seg_bg_colors[i].col); return 1; 
  }
  return 0;
}

typedef struct { 
  const char *name;
  int nlen;
  int op; 
} attr_entry_t;

static const attr_entry_t attrs[] = {
  { "bold", 4, OP_SET_BOLD },
  { "dim",  3, OP_SET_DIM  },
  { "ul",   2, OP_SET_UL   },
};

#define ATTR_COUNT (sizeof(attrs) / sizeof(attrs[0]))

static int match_attr(program_t *p, const char *s, int len) {
  for (int i = 0; i < (int)ATTR_COUNT; i++) if (
    len == attrs[i].nlen && memcmp(s, attrs[i].name, len) == 0) { 
    emit_op(p, attrs[i].op, 1); return 1; 
  }
  return 0;
}

static const char *next_seg(const char *cur, const char *end, int *out_len) {
  const char *sep = cur;
  while (sep < end && *sep != '_') sep++;
  *out_len = (int)(sep - cur);
  return sep;
}

static int match_plus_seg(program_t *p, const char *seg, int slen) {
  if (match_attr(p, seg, slen))      return 1;
  if (match_fg(p, seg, slen))        return 1;
  if (match_bg(p, seg, slen))        return 1;
  if (slen > 0 && seg[0] == '#')     return compile_hex_fg(p, seg, slen);
  if (tag_prefix(seg, slen, "bg_#")) return compile_hex_bg(p, seg + 3, slen - 3);
  if (tag_prefix(seg, slen, "bg_"))  return match_seg_bg(p, seg + 3, slen - 3);
  return 0;
}

static int compile_plus_segs(program_t *p, const char *s, int len) {
  const char *seg = s;
  const char *end = s + len;
  int emitted = 0;

  while (seg < end) {
    const char *next = seg;
    while (next < end && *next != '+') next++;
    int slen = (int)(next - seg);
    if (!match_plus_seg(p, seg, slen)) return 0;
    emitted++;  seg = (next < end) ? next + 1 : end;
  }

  return emitted;
}

static int compile_let(var_table_t *vars, const char *body, int len) {
  if (len > 0 && body[len - 1] == '/') len--;

  const char *p = body;
  const char *end = body + len;

  while (p < end) {
    while (p < end && (*p == ' ' || *p == ',')) p++;
    if (p >= end) break;

    const char *eq = memchr(p, '=', end - p);
    if (!eq) return 0;

    int nlen = (int)(eq - p);

    const char *vstart = eq + 1;
    const char *vend = vstart;

    if (vstart < end && (*vstart == '\'' || *vstart == '"')) {
      char quote = *vstart;
      vstart++;
      vend = vstart;
      
      while (vend < end && *vend != quote) vend++;
      if (vend >= end) return 0;
      const char *after = vend + 1;
      
      while (after < end && (*after == ' ' || *after == ',')) after++;
      int vlen = (int)(vend - vstart);
      
      if (nlen <= 0 || nlen >= MAX_VAR_NAME || vlen < 0 || vlen >= MAX_VAR_VALUE) return 0;
      if (vars->count >= MAX_VARS) return 0;

      cprintf_var_t *v = &vars->vars[vars->count++];
      memcpy(v->name, p, nlen);
      
      v->name[nlen] = '\0'; v->nlen = nlen;
      memcpy(v->value, vstart, vlen);
      
      v->value[vlen] = '\0'; v->vlen = vlen;
      v->is_fmt = 0;
      
      for (int j = 0; j < vlen; j++) {
        if (vstart[j] == '%' && j + 1 < vlen && vstart[j + 1] != '%') 
        { v->is_fmt = 1; break; }
      }
      
      p = after;
      continue;
    }

    while (vend < end && *vend != ',') vend++;

    int vlen = (int)(vend - vstart);
    if (nlen <= 0 || nlen >= MAX_VAR_NAME || vlen <= 0 || vlen >= MAX_VAR_VALUE) return 0;
    if (vars->count >= MAX_VARS) return 0;

    cprintf_var_t *v = &vars->vars[vars->count++];
    memcpy(v->name, p, nlen);
    v->name[nlen] = '\0'; v->nlen = nlen;
    
    memcpy(v->value, vstart, vlen);
    v->value[vlen] = '\0'; v->vlen = vlen;
    p = vend;
  }

  return 1;
}

void cprintf_var(const char *name, const char *value) {
  if (global_vars.count >= MAX_VARS) return;
  int nlen = (int)strlen(name);
  int vlen = (int)strlen(value);
  if (nlen <= 0 || nlen >= MAX_VAR_NAME || vlen <= 0 || vlen >= MAX_VAR_VALUE) return;

  for (int i = 0; i < global_vars.count; i++) {
    cprintf_var_t *v = &global_vars.vars[i];
    if (v->nlen == nlen && memcmp(v->name, name, nlen) == 0) {
      memcpy(v->value, value, vlen);
      v->value[vlen] = '\0'; v->vlen = vlen; return;
    }
  }

  cprintf_var_t *v = &global_vars.vars[global_vars.count++];
  memcpy(v->name, name, nlen);
  v->name[nlen] = '\0'; v->nlen = nlen;
  
  memcpy(v->value, value, vlen);
  v->value[vlen] = '\0'; v->vlen = vlen;
}

static int compile_var_ref(program_t *p, var_table_t *vars, const char *tag, int len) {
  const char *name = tag + 1;
  int nlen = len - 1;

  const char *plus = memchr(name, '+', nlen);
  int var_nlen = plus ? (int)(plus - name) : nlen;

  for (int i = 0; i < vars->count; i++) {
    cprintf_var_t *v = &vars->vars[i];
    if (var_nlen != v->nlen || memcmp(name, v->name, var_nlen) != 0) continue;
    
    emit_op(p, OP_STYLE_PUSH, 0);
    if (!compile_plus_segs(p, v->value, v->vlen)) return 0;

    if (plus) {
      const char *rest = plus + 1;
      int rlen = nlen - var_nlen - 1;
      if (rlen > 0 && !compile_plus_segs(p, rest, rlen)) return 0;
    }

    emit_op(p, OP_STYLE_FLUSH, 0);
    return 1;
  }

  return 0;
}

static int match_attr_off(program_t *p, const char *s, int len) {
  for (int i = 0; i < (int)ATTR_COUNT; i++) if (
    len == attrs[i].nlen && memcmp(s, attrs[i].name, len) == 0) {
    emit_op(p, attrs[i].op, 0); return 1;
  }
  return 0;
}

static int match_fg_off(program_t *p, const char *s, int len) {
  for (int i = 0; i < (int)FG_COUNT; i++) if (
    len == fg_colors[i].nlen && memcmp(s, fg_colors[i].name, len) == 0) {
    emit_op(p, OP_SET_FG, COL_NONE); return 1;
  }
  if (len > 0 && s[0] == '#') { emit_op(p, OP_SET_FG, COL_NONE); return 1; }
  return 0;
}

static int match_bg_off(program_t *p, const char *s, int len) {
  for (int i = 0; i < (int)BG_COUNT; i++) if (
    len == bg_colors[i].nlen && memcmp(s, bg_colors[i].name, len) == 0) {
    emit_op(p, OP_SET_BG, COL_NONE); return 1;
  }
  if (tag_prefix(s, len, "bg_#")) { emit_op(p, OP_SET_BG, COL_NONE); return 1; }
  return 0;
}

static int compile_tag(program_t *p, const char *tag, int len, int closing, var_table_t *vars) {
  if (closing) {
    if (tag_eq(tag, len, "pad") || tag_eq(tag, len, "rpad")) { 
      emit_op(p, OP_PAD_END, 0); return 1; 
    }
    
    if (match_attr_off(p, tag, len) || match_fg_off(p, tag, len) || match_bg_off(p, tag, len)) {
      emit_op(p, OP_STYLE_FLUSH, 0); return 1;
    }
    
    emit_op(p, OP_STYLE_RESET, 0);
    return 1;
  }

  if (tag_prefix(tag, len, "let ")) return compile_let(vars, tag + 4, len - 4);
  if (tag[0] == '$' && len > 1)     return compile_var_ref(p, vars, tag, len);

  if (tag_prefix(tag, len, "pad="))  { emit_op(p, OP_PAD_BEGIN,  (uint32_t)atoi(tag + 4)); return 1; }
  if (tag_prefix(tag, len, "rpad=")) { emit_op(p, OP_RPAD_BEGIN, (uint32_t)atoi(tag + 5)); return 1; }
  
  if (tag_prefix(tag, len, "space=") && tag[len-1] == '/') {
    emit_op(p, OP_EMIT_SPACES, (uint32_t)atoi(tag + 6));
    return 1;
  }
  
  if (tag_prefix(tag, len, "gap=") && tag[len-1] == '/') {
    emit_op(p, OP_EMIT_SPACES, (uint32_t)atoi(tag + 4));
    return 1;
  }

  if (tag_eq(tag, len, "reset/")) {
    emit_op(p, OP_STYLE_RESET_ALL, 0);
    return 1;
  }
  
  if (tag_eq(tag, len, "br/")) {
    emit_op(p, OP_EMIT_NEWLINES, 1);
    return 1;
  }
  
  if (tag_prefix(tag, len, "br=") && tag[len-1] == '/') {
    emit_op(p, OP_EMIT_NEWLINES, (uint32_t)atoi(tag + 3));
    return 1;
  }

  emit_op(p, OP_STYLE_PUSH, 0);

  if (match_attr(p, tag, len)) { emit_op(p, OP_STYLE_FLUSH, 0); return 1; }
  if (match_fg(p, tag, len))   { emit_op(p, OP_STYLE_FLUSH, 0); return 1; }
  if (match_bg(p, tag, len))   { emit_op(p, OP_STYLE_FLUSH, 0); return 1; }

  if (tag[0] == '#') {
    if (!compile_hex_fg(p, tag, len)) return 0;
    emit_op(p, OP_STYLE_FLUSH, 0); return 1;
  }

  if (tag_prefix(tag, len, "bg_#")) {
    if (!compile_hex_bg(p, tag + 3, len - 3)) return 0;
    emit_op(p, OP_STYLE_FLUSH, 0); return 1;
  }

  if (memchr(tag, '+', len)) {
    if (compile_plus_segs(p, tag, len)) { emit_op(p, OP_STYLE_FLUSH, 0); return 1; }
  }

  const char *seg = tag;
  const char *end = tag + len;
  int emitted = 0;

  while (seg < end) {
    int slen;
    const char *sep = next_seg(seg, end, &slen);

    if (match_attr(p, seg, slen)) {
    } else if (tag_eq(seg, slen, "bg") && sep < end) {
      seg = sep + 1;
      sep = next_seg(seg, end, &slen);
      if (!match_seg_bg(p, seg, slen)) return 0;
    } else if (!match_fg(p, seg, slen)) return 0;

    emitted++;
    seg = (sep < end) ? sep + 1 : end;
  }

  if (emitted) { emit_op(p, OP_STYLE_FLUSH, 0); return 1; }
  return 0;
}

static void flush_lit(program_t *p, const char *lit, const char *end) {
  if (end <= lit) return;
  uint32_t off = add_literal(p, lit, end - lit);
  emit_op(p, OP_EMIT_LIT, off);
}

static const char *scan_tag(program_t *p, const char *ptr, const char **lit, var_table_t *vars) {
  flush_lit(p, *lit, ptr);

  const char *start = ptr + 1;
  int closing = 0;
  if (*start == '/') { closing = 1; start++; }

  if (closing && *start == '>') {
    emit_op(p, OP_STYLE_RESET, 0);
    *lit = start + 1;
    return *lit;
  }

  const char *end = start;
  while (*end && *end != '>') end++;

  if (*end == '>' && compile_tag(p, start, (int)(end - start), closing, vars)) {
    *lit = end + 1;
    return *lit;
  }

  uint32_t off = add_literal(p, "<", 1);
  emit_op(p, OP_EMIT_LIT, off);
  *lit = ptr + 1;
  return *lit;
}

typedef enum {
  ARG_NONE = 0,
  ARG_INT,
  ARG_LONG,
  ARG_LLONG,
  ARG_SIZE,
  ARG_DOUBLE,
  ARG_CSTR,
  ARG_PTR,
  ARG_WINT,
  ARG_WSTR,
} arg_class_t;

static arg_class_t classify_arg(const char *spec, int len) {
  char conv = spec[len - 1];
  if (conv == '%' || conv == 'n') return ARG_NONE;
  if (conv == 's')               return ARG_CSTR;
  if (conv == 'p')               return ARG_PTR;
  if (conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E' ||
      conv == 'g' || conv == 'G' || conv == 'a' || conv == 'A')
    return ARG_DOUBLE;

  const char *p = spec + 1;
  while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') p++;
  if (*p == '*') p++; else while (*p >= '0' && *p <= '9') p++;
  if (*p == '.') { p++; if (*p == '*') p++; else while (*p >= '0' && *p <= '9') p++; }

  if (p[0] == 'z')                return ARG_SIZE;
  if (p[0] == 'l' && p[1] == 'l') return ARG_LLONG;
  if (p[0] == 'l' && conv == 'c') return ARG_WINT;
  if (p[0] == 'l' && conv == 's') return ARG_WSTR;
  if (p[0] == 'l')                return ARG_LONG;
  if (p[0] == 'j')                return ARG_LLONG;

  return ARG_INT;
}

static const char *scan_fmt(program_t *p, const char *ptr, const char **lit) {
  flush_lit(p, *lit, ptr);

  const char *fs = ptr + 1;
  while (*fs=='-'||*fs=='+'||*fs==' '||*fs=='#'||*fs=='0') fs++;
  if (*fs == '*') fs++; else while (*fs >= '0' && *fs <= '9') fs++;
  if (*fs == '.') { fs++; if (*fs == '*') fs++; else while (*fs >= '0' && *fs <= '9') fs++; }
  while (*fs=='h'||*fs=='l'||*fs=='L'||*fs=='z'||*fs=='j'||*fs=='t') fs++;
  if (*fs) fs++;

  uint32_t off = add_literal(p, ptr, fs - ptr);
  arg_class_t cls = classify_arg(ptr, (int)(fs - ptr));
  emit_op(p, OP_EMIT_FMT, off | ((uint32_t)cls << 28));
  *lit = fs;
  return fs;
}

static const char *scan_escape(
  program_t *p, const char *ptr, 
  const char **lit, const char *emit, size_t elen
) {
  flush_lit(p, *lit, ptr);
  uint32_t off = add_literal(p, emit, elen);
  emit_op(p, OP_EMIT_LIT, off);
  *lit = ptr + 2;
  return *lit;
}

static void transform_case(char *dst, const char *src, int len, int lower) {
  for (int i = 0; i < len; i++) dst[i] = (char)(lower 
    ? tolower((unsigned char)src[i]) 
    : toupper((unsigned char)src[i])
  );
}

static const char *scan_let_brace(program_t *p, const char *ptr, const char **lit, var_table_t *vars) {
  flush_lit(p, *lit, ptr);

  const char *body = ptr + 5;
  const char *end = body;
  while (*end && *end != '}') end++;

  if (*end == '}' && compile_let(vars, body, (int)(end - body))) {
    *lit = end + 1;
    return *lit;
  }

  uint32_t off = add_literal(p, "{", 1);
  emit_op(p, OP_EMIT_LIT, off);
  *lit = ptr + 1;
  return *lit;
}

static void compile_fragment(program_t *p, const char *fmt, var_table_t *vars) {
  const char *ptr = fmt;
  const char *lit = ptr;

  while (*ptr) {
    if      (*ptr == '<' && ptr[1] == '<')                ptr = scan_escape(p, ptr, &lit, "<", 1);
    else if (*ptr == '>' && ptr[1] == '>')                ptr = scan_escape(p, ptr, &lit, ">", 1);
    else if (*ptr == '%' && ptr[1] == '%')                ptr = scan_escape(p, ptr, &lit, "%%", 2);
    else if (*ptr == '{' && memcmp(ptr, "{let ", 5) == 0) ptr = scan_let_brace(p, ptr, &lit, vars);
    else if (*ptr == '{')                                 ptr = scan_var_brace(p, ptr, &lit, vars);
    else if (*ptr == '<')                                 ptr = scan_tag(p, ptr, &lit, vars);
    else if (*ptr == '%' && ptr[1] && ptr[1] != '%')      ptr = scan_fmt(p, ptr, &lit);
    else ptr++;
  }

  flush_lit(p, lit, ptr);
}

static const char *scan_var_brace(program_t *p, const char *ptr, const char **lit, var_table_t *vars) {
  flush_lit(p, *lit, ptr);

  const char *name = ptr + 1;
  const char *end = name;
  while (*end && *end != '}') end++;

  if (*end != '}') goto emit_brace;

  int lower = 0, upper = 0;
  if (*name == '~') { lower = 1; name++; }
  else if (*name == '^') { upper = 1; name++; }
  int nlen = (int)(end - name);

  for (int i = 0; i < vars->count; i++) {
    cprintf_var_t *v = &vars->vars[i];
    if (nlen != v->nlen || memcmp(name, v->name, nlen) != 0) continue;

    const char *val = v->value;
    int vlen = v->vlen;
    char buf[MAX_VAR_VALUE];
    
    if (lower || upper) {
      transform_case(buf, val, vlen, lower);
      val = buf;
    }
    
    if (memchr(val, '<', vlen)) {
      char tmp[MAX_VAR_VALUE + 1];
      memcpy(tmp, val, vlen);
      tmp[vlen] = '\0';
      compile_fragment(p, tmp, vars);
    } else if (v->is_fmt) {
      arg_class_t cls = classify_arg(val, vlen);
      uint32_t off = add_literal(p, val, vlen);
      emit_op(p, OP_EMIT_FMT, off | ((uint32_t)cls << 28));
    } else {
      uint32_t off = add_literal(p, val, vlen);
      emit_op(p, OP_EMIT_LIT, off);
    }
    
    *lit = end + 1;
    return *lit;
  }

emit_brace:;
  uint32_t off = add_literal(p, "{", 1);
  emit_op(p, OP_EMIT_LIT, off);
  *lit = ptr + 1;
  return *lit;
}

program_t *cprintf_compile(const char *fmt) {
  program_t *p = program_new();
  var_table_t vars = global_vars;

  compile_fragment(p, fmt, &vars);
  emit_op(p, OP_HALT, 0);
  return p;
}

static size_t visible_len(const char *s, size_t n) {
  size_t vis = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '\x1b') while (++i < n && !isalpha(s[i]));
    else vis++;
  }
  return vis;
}

typedef struct {
  char *data;
  size_t len;
} vm_output_t;

static vm_output_t cprintf_vm_run(program_t *prog, va_list ap) {
  vm_regs_t regs = {0};

  size_t cap = 512, pos = 0;
  char *out = malloc(cap);
  if (!out) return (vm_output_t){ NULL, 0 };

  #define ENSURE(n) ({ \
  while (pos + (n) + 1 > cap) { \
    cap *= 2; out = realloc(out, cap); \
    if (!out) return (vm_output_t){ NULL, 0 }; \
  }})
  
  #define OUT_STR(s, l) ({ ENSURE(l); memcpy(out+pos, s, l); pos += (l); })
  #define OUT_CSTR(s) ({ size_t _l = strlen(s); OUT_STR(s, _l); })

  const instruction_t *ip = prog->code;
  static const void *dispatch[OP_MAX] = {
    [OP_NOP]             = &&op_nop,
    [OP_EMIT_LIT]        = &&op_emit_lit,
    [OP_EMIT_FMT]        = &&op_emit_fmt,
    [OP_SET_FG]          = &&op_set_fg,
    [OP_SET_BG]          = &&op_set_bg,
    [OP_SET_FG_RGB]      = &&op_set_fg_rgb,
    [OP_SET_BG_RGB]      = &&op_set_bg_rgb,
    [OP_SET_BOLD]        = &&op_set_bold,
    [OP_SET_DIM]         = &&op_set_dim,
    [OP_SET_UL]          = &&op_set_ul,
    [OP_STYLE_PUSH]      = &&op_style_push,
    [OP_STYLE_FLUSH]     = &&op_style_flush,
    [OP_STYLE_RESET]     = &&op_style_reset,
    [OP_STYLE_RESET_ALL] = &&op_style_reset_all,
    [OP_PAD_BEGIN]       = &&op_pad_begin,
    [OP_RPAD_BEGIN]      = &&op_rpad_begin,
    [OP_PAD_END]         = &&op_pad_end,
    [OP_EMIT_SPACES]     = &&op_emit_spaces,
    [OP_EMIT_NEWLINES]   = &&op_emit_newlines,
    [OP_HALT]            = &&op_halt,
  };

  #define DISPATCH() goto *dispatch[ip->op]
  #define NEXT()     do { ip++; DISPATCH(); } while(0)

  DISPATCH();

  op_nop: NEXT();

  op_emit_lit: {
    OUT_CSTR(prog->literals + ip->operand);
    NEXT();
  }

  op_emit_fmt: {
    uint32_t lit_off = ip->operand & 0x0FFFFFFF;
    arg_class_t cls  = (arg_class_t)(ip->operand >> 28);
    const char *spec = prog->literals + lit_off;
    
    char tmp[256];
    va_list ap_copy;
    va_copy(ap_copy, ap);
    
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wformat-nonliteral"
    int n = vsnprintf(tmp, sizeof(tmp), spec, ap_copy);
    va_end(ap_copy);
    
    if (n > 0 && (size_t)n < sizeof(tmp)) {
      OUT_STR(tmp, (size_t)n);
    } else if (n > 0) {
      ENSURE((size_t)n);
      va_copy(ap_copy, ap);
      vsnprintf(out + pos, (size_t)n + 1, spec, ap_copy);
      va_end(ap_copy);
      pos += (size_t)n;
    }
    #pragma GCC diagnostic pop
    
    switch (cls) {
      case ARG_INT:    (void)va_arg(ap, int); break;
      case ARG_LONG:   (void)va_arg(ap, long); break;
      case ARG_LLONG:  (void)va_arg(ap, long long); break;
      case ARG_SIZE:   (void)va_arg(ap, size_t); break;
      case ARG_DOUBLE: (void)va_arg(ap, double); break;
      case ARG_CSTR:   (void)va_arg(ap, const char *); break;
      case ARG_PTR:    (void)va_arg(ap, void *); break;
      case ARG_WINT:   (void)va_arg(ap, wint_t); break;
      case ARG_WSTR:   (void)va_arg(ap, wchar_t *); break;
      case ARG_NONE:   break;
    }
    NEXT();
  }

  op_set_fg: { 
    regs.fg = ip->operand;
    NEXT(); 
  }
  
  op_set_bg: { 
    regs.bg = ip->operand;
    NEXT(); 
  }
  
  op_set_bold: { 
    regs.bold = ip->operand;
    NEXT(); 
  }
  
  op_set_dim: { 
    regs.dim = ip->operand;
    NEXT(); 
  }
  
  op_set_ul: { 
    regs.ul = ip->operand;
    NEXT(); 
  }
  
  op_set_fg_rgb: { 
    regs.fg = COL_RGB;
    regs.fg_rgb = ip->operand; 
    NEXT(); 
  }
  
  op_set_bg_rgb: { 
    regs.bg = COL_RGB;
    regs.bg_rgb = ip->operand;
    NEXT();
  }
  
  op_style_push: {
    if (regs.style_depth < 8) regs.style_stack[regs.style_depth++] = (style_entry_t){
      regs.fg, regs.bg, regs.fg_rgb, regs.bg_rgb, regs.bold, regs.dim, regs.ul,
    };
    NEXT();
  }
  
  op_pad_begin: {
    if (regs.pad_depth < 8) regs.pad_stack[regs.pad_depth++] 
      = (pad_entry_t){ pos, (int)ip->operand, 0 };
    NEXT();
  }
  
  op_rpad_begin: {
    if (regs.pad_depth < 8) regs.pad_stack[regs.pad_depth++] 
      = (pad_entry_t){ pos, (int)ip->operand, 1 };
    NEXT();
  }
  
  op_pad_end: {
    if (regs.pad_depth <= 0) NEXT();
    regs.pad_depth--;
    pad_entry_t pe = regs.pad_stack[regs.pad_depth];
    size_t vis = visible_len(out + pe.mark, pos - pe.mark);
    if ((size_t)pe.width <= vis) NEXT();
  
    size_t pad_n = pe.width - vis;
    ENSURE(pad_n);
    if (pe.right_align) {
      memmove(out + pe.mark + pad_n, out + pe.mark, pos - pe.mark);
      memset(out + pe.mark, ' ', pad_n);
    } else memset(out + pos, ' ', pad_n);
    
    pos += pad_n;
    NEXT();
  }
  
  op_emit_spaces: {
    size_t n = (size_t)ip->operand;
    ENSURE(n);
    memset(out + pos, ' ', n);
    pos += n;
    NEXT();
  }
  
  op_emit_newlines: {
    size_t n = (size_t)ip->operand;
    ENSURE(n);
    memset(out + pos, '\n', n);
    pos += n;
    NEXT();
  }

  op_style_reset: {
    if (regs.style_depth > 0) {
      style_entry_t se = regs.style_stack[--regs.style_depth];
      regs.fg = se.fg; regs.bg = se.bg;
      regs.fg_rgb = se.fg_rgb; regs.bg_rgb = se.bg_rgb;
      regs.bold = se.bold; regs.dim = se.dim; regs.ul = se.ul;
    } else {
      regs.fg = COL_NONE; regs.bg = COL_NONE;
      regs.fg_rgb = 0; regs.bg_rgb = 0;
      regs.bold = 0; regs.dim = 0; regs.ul = 0;
    }
    
    if (!io_no_color) {
      char esc[128];
      int n = snprintf(esc, sizeof(esc), "\x1b[0m");
      if (regs.bold) n += snprintf(esc+n, sizeof(esc)-n, "\x1b[1m");
      if (regs.dim)  n += snprintf(esc+n, sizeof(esc)-n, "\x1b[2m");
      if (regs.ul)   n += snprintf(esc+n, sizeof(esc)-n, "\x1b[4m");
      if (regs.fg == COL_RGB)
        n += snprintf(esc+n, sizeof(esc)-n, "\x1b[38;2;%d;%d;%dm",
          UNPACK_R(regs.fg_rgb), UNPACK_G(regs.fg_rgb), UNPACK_B(regs.fg_rgb));
      else if (regs.fg)
        n += snprintf(esc+n, sizeof(esc)-n, "\x1b[%dm", regs.fg);
      if (regs.bg == COL_RGB)
        n += snprintf(esc+n, sizeof(esc)-n, "\x1b[48;2;%d;%d;%dm",
          UNPACK_R(regs.bg_rgb), UNPACK_G(regs.bg_rgb), UNPACK_B(regs.bg_rgb));
      else if (regs.bg)
        n += snprintf(esc+n, sizeof(esc)-n, "\x1b[%dm", regs.bg + 10);
      OUT_STR(esc, (size_t)n);
    }
    
    NEXT();
  }
  
  op_style_reset_all: {
    regs.fg = COL_NONE; regs.bg = COL_NONE;
    regs.fg_rgb = 0; regs.bg_rgb = 0;
    regs.bold = 0; regs.dim = 0; regs.ul = 0;
    regs.style_depth = 0;
    if (!io_no_color) { OUT_CSTR("\x1b[0m"); }
    NEXT();
  }

  op_style_flush: {
    if (!io_no_color) {
      char esc[128];
      int n = snprintf(esc, sizeof(esc), "\x1b[0m");
      if (regs.bold) n += snprintf(esc+n, sizeof(esc)-n, "\x1b[1m");
      if (regs.dim)  n += snprintf(esc+n, sizeof(esc)-n, "\x1b[2m");
      if (regs.ul)   n += snprintf(esc+n, sizeof(esc)-n, "\x1b[4m");
      if (regs.fg == COL_RGB)
        n += snprintf(esc+n, sizeof(esc)-n, "\x1b[38;2;%d;%d;%dm",
          UNPACK_R(regs.fg_rgb), UNPACK_G(regs.fg_rgb), UNPACK_B(regs.fg_rgb));
      else if (regs.fg)
        n += snprintf(esc+n, sizeof(esc)-n, "\x1b[%dm", regs.fg);
      if (regs.bg == COL_RGB)
        n += snprintf(esc+n, sizeof(esc)-n, "\x1b[48;2;%d;%d;%dm",
          UNPACK_R(regs.bg_rgb), UNPACK_G(regs.bg_rgb), UNPACK_B(regs.bg_rgb));
      else if (regs.bg)
        n += snprintf(esc+n, sizeof(esc)-n, "\x1b[%dm", regs.bg + 10);
      OUT_STR(esc, (size_t)n);
    }
    NEXT();
  }
  
  op_halt: {
    out[pos] = '\0';
    return (vm_output_t){ out, pos };
  }

  #undef ENSURE
  #undef OUT_STR
  #undef OUT_CSTR
}

int cprintf_exec(program_t *prog, FILE *stream, ...) {
  va_list ap; va_start(ap, stream);
  vm_output_t o = cprintf_vm_run(prog, ap);
  va_end(ap); if (!o.data) return -1;

  int ret = (int)fwrite(o.data, 1, o.len, stream);
  free(o.data);
  
  return ret;
}

int csprintf_inner(program_t *prog, char *buf, size_t size, ...) {
  va_list ap; va_start(ap, size);
  vm_output_t o = cprintf_vm_run(prog, ap);
  va_end(ap); if (!o.data) return -1;

  size_t copy = (o.len < size) ? o.len : size - 1;
  memcpy(buf, o.data, copy);
  buf[copy] = '\0';
  free(o.data);
  
  return (int)o.len;
}

static const char *op_names[OP_MAX] = {
  [OP_NOP]             = "NOP",
  [OP_EMIT_LIT]        = "EMIT_LIT",
  [OP_EMIT_FMT]        = "EMIT_FMT",
  [OP_SET_FG]          = "SET_FG",
  [OP_SET_BG]          = "SET_BG",
  [OP_SET_FG_RGB]      = "SET_FG_RGB",
  [OP_SET_BG_RGB]      = "SET_BG_RGB",
  [OP_SET_BOLD]        = "SET_BOLD",
  [OP_SET_DIM]         = "SET_DIM",
  [OP_SET_UL]          = "SET_UL",
  [OP_STYLE_PUSH]      = "STYLE_PUSH",
  [OP_STYLE_FLUSH]     = "STYLE_FLUSH",
  [OP_STYLE_RESET]     = "STYLE_RESET",
  [OP_STYLE_RESET_ALL] = "STYLE_RESET_ALL",
  [OP_PAD_BEGIN]       = "PAD_BEGIN",
  [OP_RPAD_BEGIN]      = "RPAD_BEGIN",
  [OP_PAD_END]         = "PAD_END",
  [OP_EMIT_SPACES]     = "EMIT_SPACES",
  [OP_EMIT_NEWLINES]   = "EMIT_NEWLINES",
  [OP_HALT]            = "HALT",
};

static const char *color_name(uint32_t col) {
switch (col) {
  case COL_NONE:           return "none";
  case COL_BLACK:          return "black";
  case COL_RED:            return "red";
  case COL_GREEN:          return "green";
  case COL_YELLOW:         return "yellow";
  case COL_BLUE:           return "blue";
  case COL_MAGENTA:        return "magenta";
  case COL_CYAN:           return "cyan";
  case COL_WHITE:          return "white";
  case COL_GRAY:           return "gray";
  case COL_BRIGHT_RED:     return "bright_red";
  case COL_BRIGHT_GREEN:   return "bright_green";
  case COL_BRIGHT_YELLOW:  return "bright_yellow";
  case COL_BRIGHT_BLUE:    return "bright_blue";
  case COL_BRIGHT_MAGENTA: return "bright_magenta";
  case COL_BRIGHT_CYAN:    return "bright_cyan";
  case COL_BRIGHT_WHITE:   return "bright_white";
  default:                 return "?";
}}

static const char *arg_class_name(arg_class_t cls) {
switch (cls) {
  case ARG_NONE:   return "none";
  case ARG_INT:    return "int";
  case ARG_LONG:   return "long";
  case ARG_LLONG:  return "llong";
  case ARG_SIZE:   return "size_t";
  case ARG_DOUBLE: return "double";
  case ARG_CSTR:   return "char*";
  case ARG_PTR:    return "void*";
  case ARG_WINT:   return "wint_t";
  case ARG_WSTR:   return "wchar_t*";
  default:         return "?";
}}

static void fprint_escaped(FILE *out, const char *s, int max_chars) {
  for (int c = 0; *s && (max_chars < 0 || c < max_chars); s++, c++) {
    if      (*s == '\n')  fprintf(out, "\\n");
    else if (*s == '\t')  fprintf(out, "\\t");
    else if (*s == '"')   fprintf(out, "\\\"");
    else if (*s < 0x20)   fprintf(out, "\\x%02x", (unsigned char)*s);
    else                  fputc(*s, out);
  }
  if (max_chars >= 0 && *s) fprintf(out, "...");
}

static void fprint_quoted(FILE *out, const char *s, int max_chars) {
  fputc('"', out);
  fprint_escaped(out, s, max_chars);
  fputc('"', out);
}

static void fprint_operand(FILE *out, program_t *prog, instruction_t *ins, bool compact) {
  switch (ins->op) {
    case OP_EMIT_LIT: {
      const char *s = prog->literals + ins->operand;
      fprint_quoted(out, s, compact ? 24 : -1);
      break;
    }

    case OP_EMIT_FMT: {
      uint32_t    lit_off = ins->operand & 0x0FFFFFFF;
      arg_class_t cls     = (arg_class_t)(ins->operand >> 28);
      const char *s = prog->literals + lit_off;
      fprint_quoted(out, s, compact ? 24 : -1);
      fprintf(out, " (%s)", arg_class_name(cls));
      break;
    }

    case OP_SET_FG:
    case OP_SET_BG:
      if (compact) fprintf(out, "%s", color_name(ins->operand));
      else fprintf(out, "%s (ANSI %u)", color_name(ins->operand), ins->operand);
      break;

    case OP_SET_FG_RGB:
    case OP_SET_BG_RGB:
      fprintf(out, "#%02x%02x%02x",
        UNPACK_R(ins->operand), UNPACK_G(ins->operand), UNPACK_B(ins->operand));
      break;

    case OP_SET_BOLD:
    case OP_SET_DIM:
    case OP_SET_UL:
      fprintf(out, "%s", ins->operand ? "ON" : "OFF");
      break;

    case OP_PAD_BEGIN:
    case OP_RPAD_BEGIN:
      fprintf(out, "width=%u", ins->operand);
      break;

    case OP_EMIT_SPACES:
      fprintf(out, "%u", ins->operand);
      break;
      
    case OP_EMIT_NEWLINES:
      fprintf(out, "%u", ins->operand);
      break;

    case OP_NOP:
    case OP_STYLE_PUSH:
    case OP_STYLE_FLUSH:
    case OP_STYLE_RESET:
    case OP_STYLE_RESET_ALL:
    case OP_PAD_END:
    case OP_HALT: break;

    default:
      if (compact) {
        if (ins->operand) fprintf(out, "0x%x", ins->operand);
      } else fprintf(out, "0x%08x", ins->operand);
      break;
  }
}

void cprintf_disasm(program_t *prog, FILE *out) {
  fprintf(out, "; cprintf bytecode — %zu instructions, %zu bytes literal pool\n", prog->code_len, prog->lit_len);
  fprintf(out, "; %-4s  %-16s %s\n", "addr", "opcode", "operand");
  fprintf(out, "; ----  ---------------- -------\n");

  for (size_t i = 0; i < prog->code_len; i++) {
    instruction_t *ins = &prog->code[i];
    const char *name = (ins->op < OP_MAX) ? op_names[ins->op] : "???";

    fprintf(out, "  %04zu  %-16s ", i, name);
    fprint_operand(out, prog, ins, false);
    fputc('\n', out);
  }
}

void cprintf_hexdump(program_t *prog, FILE *out) {
  fprintf(out, "; cprintf hex dump — %zu instructions, %zu bytes literal pool\n", prog->code_len, prog->lit_len);
  fprintf(out, "; %-4s  %-26s %s\n", "addr", "bytes", "decoded");
  fprintf(out, "; ----  -------------------------  --------\n");

  for (size_t i = 0; i < prog->code_len; i++) {
    instruction_t *ins = &prog->code[i];
    const uint8_t *raw = (const uint8_t *)ins;
    const char *name = (ins->op < OP_MAX) ? op_names[ins->op] : "???";

    fprintf(out, "  %04zu  ", i);
    for (size_t b = 0; b < sizeof(instruction_t); b++) fprintf(out, "%02x ", raw[b]);

    fprintf(out, " ; %s ", name);
    fprint_operand(out, prog, ins, true);
    fputc('\n', out);
  }

  if (prog->lit_len > 0) {
    fprintf(out, "\n; literal pool (%zu bytes):\n", prog->lit_len);
    const uint8_t *lit = (const uint8_t *)prog->literals;
    for (size_t off = 0; off < prog->lit_len; off += 16) {
      fprintf(out, "  %04zx  ", off);

      size_t end = off + 16;
      if (end > prog->lit_len) end = prog->lit_len;
      for (size_t b = off; b < off + 16; b++) {
        if (b < end) fprintf(out, "%02x ", lit[b]);
        else         fprintf(out, "   ");
        if (b == off + 7) fputc(' ', out);
      }

      fprintf(out, " |");
      for (size_t b = off; b < end; b++) fputc((lit[b] >= 0x20 && lit[b] < 0x7f) ? lit[b] : '.', out);
      fprintf(out, "|\n");
    }
  }
}