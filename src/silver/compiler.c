#include "silver/ast.h"
#include "silver/engine.h"
#include "silver/compiler.h"
#include "silver/directives.h"

#include "internal.h"
#include "tokens.h"
#include "runtime.h"
#include "ops/coercion.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
  const char *name;
  uint32_t name_len;
  int  depth;
  bool is_const;
  bool captured;
  bool is_tdz;
} sv_local_t;

typedef struct {
  int *offsets;
  int  count;
  int  cap;
} sv_patch_list_t;

typedef struct {
  int loop_start;
  sv_patch_list_t breaks;
  sv_patch_list_t continues;
  int scope_depth;
  const char *label;
  uint32_t label_len;
} sv_loop_t;

typedef struct {
  const char *name;
  uint32_t len;
} sv_deferred_export_t;

typedef struct sv_compiler {
  ant_t *js;
  const char *source;
  jsoff_t source_len;

  uint8_t *code;
  int code_len;
  int code_cap;

  jsval_t *constants;
  int const_count;
  int const_cap;

  sv_atom_t *atoms;
  int atom_count;
  int atom_cap;

  sv_local_t *locals;
  int local_count;
  int local_cap;
  int max_local_count;
  int scope_depth;
  int param_locals;

  sv_upval_desc_t *upval_descs;
  int upvalue_count;
  int upvalue_cap;

  sv_loop_t *loops;
  int loop_count;
  int loop_cap;

  struct sv_compiler *enclosing;
  sv_ast_t **field_inits;
  
  int field_init_count;
  int *computed_key_locals;
  int param_count;
  
  bool is_arrow;
  bool is_strict;
  sv_compile_mode_t mode;
  
  bool is_tla;
  int try_depth;
  int with_depth;
  int strict_args_local;
  int new_target_local;
  int super_local;

  const char *pending_label;
  uint32_t pending_label_len;

  const char *inferred_name;
  uint32_t inferred_name_len;

  sv_srcpos_t *srcpos;
  int srcpos_count;
  int srcpos_cap;
  uint32_t last_srcpos_off;
  uint32_t last_srcpos_end;

  sv_deferred_export_t *deferred_exports;
  int deferred_export_count;
  int deferred_export_cap;
} sv_compiler_t;

static sv_func_t *compile_function_body(
  sv_compiler_t *enclosing,
  sv_ast_t *node, sv_compile_mode_t mode
);

static void compile_expr(sv_compiler_t *c, sv_ast_t *node);
static void compile_stmt(sv_compiler_t *c, sv_ast_t *node);
static void compile_stmts(sv_compiler_t *c, sv_ast_list_t *list);
static void compile_binary(sv_compiler_t *c, sv_ast_t *node);
static void compile_unary(sv_compiler_t *c, sv_ast_t *node);
static void compile_update(sv_compiler_t *c, sv_ast_t *node);
static void compile_assign(sv_compiler_t *c, sv_ast_t *node);
static void compile_lhs_set(sv_compiler_t *c, sv_ast_t *target, bool keep);
static void compile_ternary(sv_compiler_t *c, sv_ast_t *node);
static void compile_tail_return_expr(sv_compiler_t *c, sv_ast_t *expr);
static void compile_typeof(sv_compiler_t *c, sv_ast_t *node);
static void compile_delete(sv_compiler_t *c, sv_ast_t *node);
static void compile_template(sv_compiler_t *c, sv_ast_t *node);
static void compile_call(sv_compiler_t *c, sv_ast_t *node);
static void compile_new(sv_compiler_t *c, sv_ast_t *node);
static void compile_member(sv_compiler_t *c, sv_ast_t *node);
static void compile_optional(sv_compiler_t *c, sv_ast_t *node);
static void compile_optional_get(sv_compiler_t *c, sv_ast_t *node);
static void compile_array(sv_compiler_t *c, sv_ast_t *node);
static void compile_object(sv_compiler_t *c, sv_ast_t *node);
static void compile_func_expr(sv_compiler_t *c, sv_ast_t *node);
static void compile_array_destructure(sv_compiler_t *c, sv_ast_t *pat, bool keep);
static void compile_object_destructure(sv_compiler_t *c, sv_ast_t *pat, bool keep);
static void compile_var_decl(sv_compiler_t *c, sv_ast_t *node);
static void compile_import_decl(sv_compiler_t *c, sv_ast_t *node);
static void compile_export_decl(sv_compiler_t *c, sv_ast_t *node);
static void compile_destructure_binding(sv_compiler_t *c, sv_ast_t *pat, sv_var_kind_t kind);
static void compile_if(sv_compiler_t *c, sv_ast_t *node);
static void compile_while(sv_compiler_t *c, sv_ast_t *node);
static void compile_do_while(sv_compiler_t *c, sv_ast_t *node);
static void compile_for(sv_compiler_t *c, sv_ast_t *node);
static void compile_for_in(sv_compiler_t *c, sv_ast_t *node);
static void compile_for_of(sv_compiler_t *c, sv_ast_t *node);
static void compile_break(sv_compiler_t *c, sv_ast_t *node);
static void compile_continue(sv_compiler_t *c, sv_ast_t *node);
static void compile_try(sv_compiler_t *c, sv_ast_t *node);
static void compile_switch(sv_compiler_t *c, sv_ast_t *node);
static void compile_label(sv_compiler_t *c, sv_ast_t *node);
static void compile_class(sv_compiler_t *c, sv_ast_t *node);

static const char *pin_source_text(const char *source, jsoff_t source_len) {
  if (!source || source_len <= 0) return source;
  const char *pinned = code_arena_alloc(source, (size_t)source_len);
  return pinned ? pinned : source;
}

static void emit(sv_compiler_t *c, uint8_t byte) {
  if (c->code_len >= c->code_cap) {
    c->code_cap = c->code_cap ? c->code_cap * 2 : 256;
    c->code = realloc(c->code, (size_t)c->code_cap);
  }
  c->code[c->code_len++] = byte;
}

static void emit_u16(sv_compiler_t *c, uint16_t val) {
  emit(c, (uint8_t)(val & 0xFF));
  emit(c, (uint8_t)(val >> 8));
}

static void emit_u32(sv_compiler_t *c, uint32_t val) {
  emit(c, (uint8_t)(val & 0xFF));
  emit(c, (uint8_t)((val >> 8) & 0xFF));
  emit(c, (uint8_t)((val >> 16) & 0xFF));
  emit(c, (uint8_t)((val >> 24) & 0xFF));
}

static void emit_i32(sv_compiler_t *c, int32_t val) {
  uint32_t u;
  memcpy(&u, &val, 4);
  emit_u32(c, u);
}

static void emit_op(sv_compiler_t *c, sv_op_t op) {
  emit(c, (uint8_t)op);
}

static void emit_srcpos(sv_compiler_t *c, sv_ast_t *node) {
  if (!node) return;
  const char *code = c->source;
  jsoff_t clen = c->source_len;
  if (!code || clen <= 0) return;

  uint32_t off = node->src_off;
  if (off > clen) off = (uint32_t)clen;
  uint32_t end = node->src_end;
  if (end > clen) end = (uint32_t)clen;
  if (end < off) end = off;
  if (end == off && off < (uint32_t)clen) end = off + 1;
  if (end == off) return;

  uint32_t line = 1, col = 1;
  for (uint32_t i = 0; i < off; i++) {
    if (code[i] == '\n') { line++; col = 1; }
    else col++;
  }
  if (c->srcpos_count > 0 && c->last_srcpos_off == off && c->last_srcpos_end == end) return;
  if (c->srcpos_count >= c->srcpos_cap) {
    c->srcpos_cap = c->srcpos_cap ? c->srcpos_cap * 2 : 32;
    c->srcpos = realloc(c->srcpos, (size_t)c->srcpos_cap * sizeof(sv_srcpos_t));
  }
  c->srcpos[c->srcpos_count++] = (sv_srcpos_t){
    .bc_offset = (uint32_t)c->code_len,
    .line = line,
    .col = col,
    .src_off = off,
    .src_end = end,
  };
  c->last_srcpos_off = off;
  c->last_srcpos_end = end;
}

static void patch_u32(sv_compiler_t *c, int offset, uint32_t val) {
  c->code[offset]     = (uint8_t)(val & 0xFF);
  c->code[offset + 1] = (uint8_t)((val >> 8) & 0xFF);
  c->code[offset + 2] = (uint8_t)((val >> 16) & 0xFF);
  c->code[offset + 3] = (uint8_t)((val >> 24) & 0xFF);
}

static int add_constant(sv_compiler_t *c, jsval_t val) {
  if (c->const_count >= c->const_cap) {
    c->const_cap = c->const_cap ? c->const_cap * 2 : 16;
    c->constants = realloc(c->constants, (size_t)c->const_cap * sizeof(jsval_t));
  }
  c->constants[c->const_count] = val;
  return c->const_count++;
}

static void emit_constant(sv_compiler_t *c, jsval_t val) {
  int idx = add_constant(c, val);
  if (idx <= 255) {
    emit_op(c, OP_CONST8);
    emit(c, (uint8_t)idx);
  } else {
    emit_op(c, OP_CONST);
    emit_u32(c, (uint32_t)idx);
  }
}

static void emit_number(sv_compiler_t *c, double num) {
  if (num >= -128 && num <= 127 && num == (int)num && num != -0.0) {
    emit_op(c, OP_CONST_I8);
    emit(c, (uint8_t)(int8_t)num);
  } else emit_constant(c, tov(num));
}


static inline bool is_quoted_ident_key(const sv_ast_t *node) {
  if (!node || node->type != N_IDENT || !node->str || node->len < 2) return false;
  char open = node->str[0];
  char close = node->str[node->len - 1];
  return ((open == '\'' && close == '\'') || (open == '"' && close == '"'));
}

static inline bool is_invalid_cooked_string(const sv_ast_t *node) {
  return node && (node->flags & FN_INVALID_COOKED);
}

static inline jsval_t ast_string_const(sv_compiler_t *c, const sv_ast_t *node) {
  if (!node || !node->str) return js_mkstr(c->js, "", 0);
  return js_mkstr(c->js, node->str, node->len);
}

static inline void compile_static_property_key(sv_compiler_t *c, sv_ast_t *key) {
  if (!key) {
    emit_op(c, OP_UNDEF);
    return;
  }

  if (key->type == N_STRING) {
    emit_constant(c, ast_string_const(c, key));
    return;
  }

  if (key->type == N_NUMBER) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%g", key->num);
    emit_constant(c, js_mkstr(c->js, buf, (size_t)n));
    return;
  }

  if (key->type == N_IDENT) {
    if (is_quoted_ident_key(key))
      emit_constant(c, js_mkstr(c->js, key->str + 1, key->len - 2));
    else
      emit_constant(c, js_mkstr(c->js, key->str, key->len));
    return;
  }

  compile_expr(c, key);
}

static int add_atom(sv_compiler_t *c, const char *str, uint32_t len) {
  for (int i = 0; i < c->atom_count; i++) {
    if (c->atoms[i].len == len && memcmp(c->atoms[i].str, str, len) == 0)
      return i;
  }
  if (c->atom_count >= c->atom_cap) {
    c->atom_cap = c->atom_cap ? c->atom_cap * 2 : 16;
    c->atoms = realloc(c->atoms, (size_t)c->atom_cap * sizeof(sv_atom_t));
  }
  char *copy = code_arena_bump(len);
  memcpy(copy, str, len);
  c->atoms[c->atom_count] = (sv_atom_t){ .str = copy, .len = len };
  return c->atom_count++;
}

static void emit_atom_op(sv_compiler_t *c, sv_op_t op, const char *str, uint32_t len) {
  int idx = add_atom(c, str, len);
  emit_op(c, op);
  emit_u32(c, (uint32_t)idx);
}

static inline void emit_set_function_name(
  sv_compiler_t *c,
  const char *name, uint32_t len
) {
  if (!name) { name = ""; len = 0; }
  emit_atom_op(c, OP_SET_NAME, name, len);
}

static void emit_const_assign_error(sv_compiler_t *c, const char *name, uint32_t len) {
  static const char prefix[] = "Assignment to constant variable '";
  static const char suffix[] = "'";
  
  uint32_t mlen = (uint32_t)(sizeof(prefix) - 1 + len + sizeof(suffix) - 1);
  char *buf = code_arena_bump(mlen);
  
  memcpy(buf, prefix, sizeof(prefix) - 1);
  memcpy(buf + sizeof(prefix) - 1, name, len);
  memcpy(buf + sizeof(prefix) - 1 + len, suffix, sizeof(suffix) - 1);
  
  int atom = add_atom(c, buf, mlen);
  emit_op(c, OP_THROW_ERROR);
  emit_u32(c, (uint32_t)atom);
  emit(c, (uint8_t)JS_ERR_TYPE);
}

static inline bool is_ident_str(
  const char *name, uint32_t len,
  const char *lit, uint32_t lit_len
) {
  return len == lit_len && memcmp(name, lit, lit_len) == 0;
}

static inline bool is_strict_restricted_ident(const char *name, uint32_t len) {
  return 
    is_ident_str(name, len, "eval", 4) ||
    is_ident_str(name, len, "arguments", 9);
}

static inline bool is_repl_top_level(const sv_compiler_t *c) {
  return 
    c->mode == SV_COMPILE_REPL && c->scope_depth == 0 &&
    c->enclosing && !c->enclosing->enclosing &&
    !c->is_strict;
}

static inline bool has_completion_value(const sv_compiler_t *c) {
  return c && (c->mode == SV_COMPILE_EVAL || c->mode == SV_COMPILE_REPL);
}

static inline bool has_implicit_arguments_obj(const sv_compiler_t *c) {
  return c && !c->is_arrow && c->enclosing;
}

static int resolve_local(sv_compiler_t *c, const char *name, uint32_t len) {
  for (int i = c->local_count - 1; i >= 0; i--) {
    sv_local_t *loc = &c->locals[i];
    if (loc->name_len == len && memcmp(loc->name, name, len) == 0)
      return i;
  }
  return -1;
}

static int add_local(
  sv_compiler_t *c, const char *name, uint32_t len,
  bool is_const, int depth
) {
  if (c->local_count >= c->local_cap) {
    c->local_cap = c->local_cap ? c->local_cap * 2 : 16;
    c->locals = realloc(c->locals, (size_t)c->local_cap * sizeof(sv_local_t));
  }
  int idx = c->local_count++;
  if (c->local_count > c->max_local_count)
    c->max_local_count = c->local_count;
  c->locals[idx] = (sv_local_t){
    .name = name, .name_len = len,
    .depth = depth, .is_const = is_const, .captured = false,
  };
  return idx;
}

static int ensure_local_at_depth(
  sv_compiler_t *c, const char *name, uint32_t len,
  bool is_const, int depth
) {
  int local = resolve_local(c, name, len);
  if (local != -1 && c->locals[local].depth == depth)
    return local;
  return add_local(c, name, len, is_const, depth);
}

static int local_to_frame_slot(sv_compiler_t *c, int local_idx) {
  if (c->locals[local_idx].depth == -1) return local_idx;
  return c->param_count + (local_idx - c->param_locals);
}

static int resolve_local_slot(sv_compiler_t *c, const char *name, uint32_t len) {
  int local = resolve_local(c, name, len);
  if (local == -1 || c->locals[local].depth == -1) return -1;
  int slot = local - c->param_locals;
  return (slot >= 0 && slot <= 255) ? slot : -1;
}

static int add_upvalue(sv_compiler_t *c, uint16_t index, bool is_local, bool is_const) {
  for (int i = 0; i < c->upvalue_count; i++) {
    if (c->upval_descs[i].index == index &&
        c->upval_descs[i].is_local == is_local)
      return i;
  }
  if (c->upvalue_count >= c->upvalue_cap) {
    c->upvalue_cap = c->upvalue_cap ? c->upvalue_cap * 2 : 8;
    c->upval_descs = realloc(
      c->upval_descs,
      (size_t)c->upvalue_cap * sizeof(sv_upval_desc_t));
  }
  int idx = c->upvalue_count++;
  c->upval_descs[idx] = (sv_upval_desc_t){
    .index = index, .is_local = is_local, .is_const = is_const,
  };
  return idx;
}

static int resolve_super_upvalue(sv_compiler_t *c) {
  if (!c->enclosing) return -1;
  sv_compiler_t *enc = c->enclosing;

  if (!enc->is_arrow) {
    if (enc->super_local < 0) return -1;
    enc->locals[enc->super_local].captured = true;
    uint16_t slot = (uint16_t)local_to_frame_slot(enc, enc->super_local);
    return add_upvalue(c, slot, true, false);
  }

  int upvalue = resolve_super_upvalue(enc);
  if (upvalue == -1) return -1;
  return add_upvalue(c, (uint16_t)upvalue, false, false);
}

static int resolve_arguments_upvalue(sv_compiler_t *c) {
  if (!c->enclosing) return -1;
  sv_compiler_t *enc = c->enclosing;

  if (!enc->is_arrow) {
    if (enc->strict_args_local < 0) return -1;
    enc->locals[enc->strict_args_local].captured = true;
    uint16_t slot = (uint16_t)local_to_frame_slot(enc, enc->strict_args_local);
    return add_upvalue(c, slot, true, false);
  }

  int upvalue = resolve_arguments_upvalue(enc);
  if (upvalue == -1) return -1;
  
  return add_upvalue(c, (uint16_t)upvalue, false, false);
}

static int resolve_upvalue(sv_compiler_t *c, const char *name, uint32_t len) {
  if (!c->enclosing) return -1;

  int local = resolve_local(c->enclosing, name, len);
  if (local != -1) {
    c->enclosing->locals[local].captured = true;
    uint16_t slot = (uint16_t)local_to_frame_slot(c->enclosing, local);
    return add_upvalue(c, slot, true, c->enclosing->locals[local].is_const);
  }

  int upvalue = resolve_upvalue(c->enclosing, name, len);
  if (upvalue != -1) {
    bool uv_const = c->enclosing->upval_descs[upvalue].is_const;
    return add_upvalue(c, (uint16_t)upvalue, false, uv_const);
  }

  return -1;
}

static int emit_jump(sv_compiler_t *c, sv_op_t op) {
  emit_op(c, op);
  int offset = c->code_len;
  emit_i32(c, 0);
  return offset;
}

static void patch_jump(sv_compiler_t *c, int offset) {
  int32_t delta = (int32_t)(c->code_len - offset - 4);
  patch_u32(c, offset, (uint32_t)delta);
}

static void emit_loop(sv_compiler_t *c, int loop_start) {
  emit_op(c, OP_JMP);
  int32_t delta = (int32_t)(loop_start - c->code_len - 4);
  emit_i32(c, delta);
}

static void patch_list_add(sv_patch_list_t *pl, int offset) {
  if (pl->count >= pl->cap) {
    pl->cap = pl->cap ? pl->cap * 2 : 4;
    pl->offsets = realloc(pl->offsets, (size_t)pl->cap * sizeof(int));
  }
  pl->offsets[pl->count++] = offset;
}

static void patch_list_resolve(sv_compiler_t *c, sv_patch_list_t *pl) {
  for (int i = 0; i < pl->count; i++)
    patch_jump(c, pl->offsets[i]);
  free(pl->offsets);
  *pl = (sv_patch_list_t){0};
}

static void push_loop(
  sv_compiler_t *c, int loop_start,
  const char *label, uint32_t label_len
) {
  if (c->loop_count >= c->loop_cap) {
    c->loop_cap = c->loop_cap ? c->loop_cap * 2 : 4;
    c->loops = realloc(c->loops, (size_t)c->loop_cap * sizeof(sv_loop_t));
  }
  if (!label && c->pending_label) {
    label = c->pending_label;
    label_len = c->pending_label_len;
    c->pending_label = NULL;
    c->pending_label_len = 0;
  }
  c->loops[c->loop_count++] = (sv_loop_t){
    .loop_start = loop_start,
    .scope_depth = c->scope_depth,
    .label = label, .label_len = label_len,
  };
}

static void pop_loop(sv_compiler_t *c) {
  sv_loop_t *loop = &c->loops[--c->loop_count];
  patch_list_resolve(c, &loop->breaks);
  free(loop->continues.offsets);
}

static void begin_scope(sv_compiler_t *c) {
  c->scope_depth++;
}

static void end_scope(sv_compiler_t *c) {
  while (
    c->local_count > 0 &&
    c->locals[c->local_count - 1].depth >= c->scope_depth
  ) {
    sv_local_t *loc = &c->locals[c->local_count - 1];
    if (loc->captured) {
      int frame_slot = local_to_frame_slot(c, c->local_count - 1);
      emit_op(c, OP_CLOSE_UPVAL);
      emit_u16(c, (uint16_t)frame_slot);
    }
    c->local_count--;
  }
  c->scope_depth--;
}

static void emit_close_upvals(sv_compiler_t *c) {
  for (int i = 0; i < c->local_count; i++) {
  if (c->locals[i].captured) {
    int frame_slot = local_to_frame_slot(c, i);
    emit_op(c, OP_CLOSE_UPVAL);
    emit_u16(c, (uint16_t)frame_slot);
    return;
  }}
}

static void emit_with_get(
  sv_compiler_t *c, const char *name, uint32_t len,
  uint8_t fb_kind, uint16_t fb_idx
) {
  int atom = add_atom(c, name, len);
  emit_op(c, OP_WITH_GET_VAR);
  emit_u32(c, (uint32_t)atom);
  emit(c, fb_kind);
  emit_u16(c, fb_idx);
}

static void emit_with_put(
  sv_compiler_t *c, const char *name, uint32_t len,
  uint8_t fb_kind, uint16_t fb_idx
) {
  int atom = add_atom(c, name, len);
  emit_op(c, OP_WITH_PUT_VAR);
  emit_u32(c, (uint32_t)atom);
  emit(c, fb_kind);
  emit_u16(c, fb_idx);
}

static void emit_get_local(sv_compiler_t *c, int local_idx);
static void emit_put_local(sv_compiler_t *c, int local_idx);

static void emit_get_var(sv_compiler_t *c, const char *name, uint32_t len) {
  int local = resolve_local(c, name, len);
  if (local != -1) {
    if (c->with_depth > 0) {
      uint8_t kind = c->locals[local].depth == -1 ? WITH_FB_ARG : WITH_FB_LOCAL;
      uint16_t idx = kind == WITH_FB_ARG 
        ? (uint16_t)local
        : (uint16_t)(local - c->param_locals);
      emit_with_get(c, name, len, kind, idx);
      return;
    }
    if (c->locals[local].depth == -1) {
      emit_op(c, OP_GET_ARG);
      emit_u16(c, (uint16_t)local);
    } else {
      int slot = local - c->param_locals;
      if (c->locals[local].is_tdz) {
        int ai = add_atom(c, name, len);
        emit_op(c, OP_GET_LOCAL_CHK);
        emit_u16(c, (uint16_t)slot);
        emit_u32(c, (uint32_t)ai);
      } else if (slot <= 255) {
        emit_op(c, OP_GET_LOCAL8);
        emit(c, (uint8_t)slot);
      } else {
        emit_op(c, OP_GET_LOCAL);
        emit_u16(c, (uint16_t)slot);
      }
    }
    return;
  }
  int upval = resolve_upvalue(c, name, len);
  if (upval != -1) {
    if (c->with_depth > 0) {
      emit_with_get(c, name, len, WITH_FB_UPVAL, (uint16_t)upval);
      return;
    }
    emit_op(c, OP_GET_UPVAL);
    emit_u16(c, (uint16_t)upval);
    return;
  }
  if (is_ident_str(name, len, "arguments", 9)) {
    if (has_implicit_arguments_obj(c)) {
      if (c->strict_args_local >= 0) {
        emit_get_local(c, c->strict_args_local);
      } else {
        emit_op(c, OP_SPECIAL_OBJ);
        emit(c, 0);
      }
      return;
    }
    if (c->is_arrow) {
      int args_upval = resolve_arguments_upvalue(c);
      if (args_upval != -1) {
        emit_op(c, OP_GET_UPVAL);
        emit_u16(c, (uint16_t)args_upval);
        return;
      }
    }
  }
  if (c->is_arrow && is_ident_str(name, len, "super", 5)) {
    int super_upval = resolve_super_upvalue(c);
    if (super_upval != -1) {
      emit_op(c, OP_GET_UPVAL);
      emit_u16(c, (uint16_t)super_upval);
      return;
    }
  }
  if (c->with_depth > 0)
    emit_with_get(c, name, len, WITH_FB_GLOBAL, 0);
  else
    emit_atom_op(c, OP_GET_GLOBAL, name, len);
}

static void emit_set_var(sv_compiler_t *c, const char *name, uint32_t len, bool keep) {
  int local = resolve_local(c, name, len);
  if (local != -1) {
    if (c->locals[local].is_const) {
      emit_const_assign_error(c, name, len);
      return;
    }

    if (c->with_depth > 0) {
      uint8_t kind = c->locals[local].depth == -1 ? WITH_FB_ARG : WITH_FB_LOCAL;
      uint16_t idx = kind == WITH_FB_ARG 
        ? (uint16_t)local
        : (uint16_t)(local - c->param_locals);
      if (keep) emit_op(c, OP_DUP);
      emit_with_put(c, name, len, kind, idx);
      return;
    }
    if (c->locals[local].depth == -1) {
      emit_op(c, keep ? OP_SET_ARG : OP_PUT_ARG);
      emit_u16(c, (uint16_t)local);
    } else {
      int slot = local - c->param_locals;
      sv_op_t op = keep 
        ? (slot <= 255 ? OP_SET_LOCAL8 : OP_SET_LOCAL)
        : (slot <= 255 ? OP_PUT_LOCAL8 : OP_PUT_LOCAL);
      emit_op(c, op);
      if (slot <= 255) emit(c, (uint8_t)slot);
      else emit_u16(c, (uint16_t)slot);
    }
    return;
  }
  int upval = resolve_upvalue(c, name, len);
  if (upval != -1) {
    if (c->upval_descs[upval].is_const) {
      emit_const_assign_error(c, name, len);
      return;
    }
    if (c->with_depth > 0) {
      if (keep) emit_op(c, OP_DUP);
      emit_with_put(c, name, len, WITH_FB_UPVAL, (uint16_t)upval);
      return;
    }
    emit_op(c, keep ? OP_SET_UPVAL : OP_PUT_UPVAL);
    emit_u16(c, (uint16_t)upval);
    return;
  }
  if (c->with_depth > 0) {
    if (keep) emit_op(c, OP_DUP);
    emit_with_put(c, name, len, WITH_FB_GLOBAL, 0);
  } else {
    if (keep) {
      emit_op(c, OP_DUP);
      emit_atom_op(c, OP_PUT_GLOBAL, name, len);
    } else emit_atom_op(c, OP_PUT_GLOBAL, name, len);
  }
}

static void emit_put_local(sv_compiler_t *c, int local_idx) {
  int slot = local_idx - c->param_locals;
  if (slot <= 255) { emit_op(c, OP_PUT_LOCAL8); emit(c, (uint8_t)slot); }
  else { emit_op(c, OP_PUT_LOCAL); emit_u16(c, (uint16_t)slot); }
}

static void emit_get_local(sv_compiler_t *c, int local_idx) {
  int slot = local_idx - c->param_locals;
  if (slot <= 255) { emit_op(c, OP_GET_LOCAL8); emit(c, (uint8_t)slot); }
  else { emit_op(c, OP_GET_LOCAL); emit_u16(c, (uint16_t)slot); }
}


static inline bool is_ident_name(sv_ast_t *node, const char *name) {
  size_t n = strlen(name);
  return node 
    && node->type == N_IDENT && node->len == (uint32_t)n 
    && memcmp(node->str, name, n) == 0;
}

static void hoist_var_pattern(sv_compiler_t *c, sv_ast_t *pat) {
  if (!pat) return;
  switch (pat->type) {
    case N_IDENT:
      if (resolve_local(c, pat->str, pat->len) == -1)
        add_local(c, pat->str, pat->len, false, 0);
      break;
    case N_ARRAY: case N_ARRAY_PAT:
      for (int i = 0; i < pat->args.count; i++)
        hoist_var_pattern(c, pat->args.items[i]);
      break;
    case N_OBJECT: case N_OBJECT_PAT:
      for (int i = 0; i < pat->args.count; i++) {
        sv_ast_t *prop = pat->args.items[i];
        if (!prop) continue;
        if (prop->type == N_PROPERTY)
          hoist_var_pattern(c, prop->right);
        else if (prop->type == N_REST || prop->type == N_SPREAD)
          hoist_var_pattern(c, prop->right);
      }
      break;
    case N_ASSIGN_PAT:
      hoist_var_pattern(c, pat->left);
      break;
    case N_ASSIGN:
      hoist_var_pattern(c, pat->left);
      break;
    case N_REST: case N_SPREAD:
      hoist_var_pattern(c, pat->right);
      break;
    default:
      break;
  }
}

static void hoist_var_decls(sv_compiler_t *c, sv_ast_t *node) {
  if (!node) return;
  switch (node->type) {
    case N_VAR:
      if (node->var_kind == SV_VAR_VAR) {
        for (int i = 0; i < node->args.count; i++) {
          sv_ast_t *decl = node->args.items[i];
          if (decl->type == N_VARDECL && decl->left)
            hoist_var_pattern(c, decl->left);
        }
      }
      break;
    case N_BLOCK:
      for (int i = 0; i < node->args.count; i++)
        hoist_var_decls(c, node->args.items[i]);
      break;
    case N_IF:
      hoist_var_decls(c, node->left);
      hoist_var_decls(c, node->right);
      break;
    case N_WHILE: case N_DO_WHILE: case N_LABEL:
      hoist_var_decls(c, node->body);
      break;
    case N_FOR:
      hoist_var_decls(c, node->init);
      hoist_var_decls(c, node->body);
      break;
    case N_FOR_IN: case N_FOR_OF: case N_FOR_AWAIT_OF:
      hoist_var_decls(c, node->left);
      hoist_var_decls(c, node->body);
      break;
    case N_SWITCH:
      for (int i = 0; i < node->args.count; i++) {
        sv_ast_t *cas = node->args.items[i];
        for (int j = 0; j < cas->args.count; j++)
          hoist_var_decls(c, cas->args.items[j]);
      }
      break;
    case N_TRY:
      hoist_var_decls(c, node->body);
      hoist_var_decls(c, node->catch_body);
      hoist_var_decls(c, node->finally_body);
      break;
    case N_EXPORT:
      hoist_var_decls(c, node->left);
      break;
    default: break;
  }
}

static void hoist_lexical_pattern(sv_compiler_t *c, sv_ast_t *pat,
                                  bool is_const) {
  if (!pat) return;

  switch (pat->type) {
    case N_IDENT:
      ensure_local_at_depth(c, pat->str, pat->len, is_const, c->scope_depth);
      break;
    case N_ASSIGN_PAT:
      hoist_lexical_pattern(c, pat->left, is_const);
      break;
    case N_REST:
    case N_SPREAD:
      hoist_lexical_pattern(c, pat->right, is_const);
      break;
    case N_ARRAY:
    case N_ARRAY_PAT:
      for (int i = 0; i < pat->args.count; i++)
        hoist_lexical_pattern(c, pat->args.items[i], is_const);
      break;
    case N_OBJECT:
    case N_OBJECT_PAT:
      for (int i = 0; i < pat->args.count; i++) {
        sv_ast_t *prop = pat->args.items[i];
        if (!prop) continue;
        if (prop->type == N_PROPERTY)
          hoist_lexical_pattern(c, prop->right, is_const);
      }
      break;
    default:
      break;
  }
}

static void annex_b_collect_funcs(sv_ast_t *node, sv_ast_list_t *out) {
  if (!node) return;
  if (node->type == N_FUNC && node->str && !(node->flags & FN_ARROW)) {
    sv_ast_list_push(out, node);
    return;
  }
  if (node->type == N_IF) {
    annex_b_collect_funcs(node->left, out);
    annex_b_collect_funcs(node->right, out);
  } else if (node->type == N_LABEL) annex_b_collect_funcs(node->body, out);
}

static void hoist_lexical_decls(sv_compiler_t *c, sv_ast_list_t *stmts) {
  for (int i = 0; i < stmts->count; i++) {
    sv_ast_t *node = stmts->items[i];
    if (!node) continue;
    sv_ast_t *decl_node = (node->type == N_EXPORT) ? node->left : node;
    if (!decl_node) continue;

    if (decl_node->type == N_VAR && decl_node->var_kind != SV_VAR_VAR) {
      bool is_const = (decl_node->var_kind == SV_VAR_CONST);
      int lb = c->local_count;
      for (int j = 0; j < decl_node->args.count; j++) {
        sv_ast_t *decl = decl_node->args.items[j];
        if (!decl || decl->type != N_VARDECL || !decl->left) continue;
        hoist_lexical_pattern(c, decl->left, is_const);
      }
      for (int j = lb; j < c->local_count; j++) {
        c->locals[j].is_tdz = true;
        int slot = j - c->param_locals;
        emit_op(c, OP_SET_LOCAL_UNDEF);
        emit_u16(c, (uint16_t)slot);
      }
    } else if (decl_node->type == N_IMPORT_DECL) {
      for (int j = 0; j < decl_node->args.count; j++) {
        sv_ast_t *spec = decl_node->args.items[j];
        if (!spec || spec->type != N_IMPORT_SPEC ||
            !spec->right || spec->right->type != N_IDENT)
          continue;
        ensure_local_at_depth(c, spec->right->str, spec->right->len, true, c->scope_depth);
      }
    } else if (decl_node->type == N_CLASS && decl_node->str) {
      int lb = c->local_count;
      ensure_local_at_depth(c, decl_node->str, decl_node->len, false, c->scope_depth);
      if (c->local_count > lb) {
        c->locals[c->local_count - 1].is_tdz = true;
        int slot = (c->local_count - 1) - c->param_locals;
        emit_op(c, OP_SET_LOCAL_UNDEF);
        emit_u16(c, (uint16_t)slot);
      }
    } else if (decl_node->type == N_FUNC && decl_node->str && !(decl_node->flags & FN_ARROW)) {
      ensure_local_at_depth(c, decl_node->str, decl_node->len, false, c->scope_depth);
    }
    if (!c->is_strict && (decl_node->type == N_IF || decl_node->type == N_LABEL)) {
      sv_ast_list_t funcs = {0};
      annex_b_collect_funcs(decl_node, &funcs);
      for (int j = 0; j < funcs.count; j++) {
        sv_ast_t *fn = funcs.items[j];
        if (resolve_local(c, fn->str, fn->len) == -1)
          add_local(c, fn->str, fn->len, false, c->scope_depth);
      }
    }
  }
}

static void hoist_one_func(sv_compiler_t *c, sv_ast_t *node) {
  sv_func_t *fn = compile_function_body(c, node, SV_COMPILE_SCRIPT);
  if (!fn) return;
  int idx = add_constant(c, mkval(T_CFUNC, (uintptr_t)fn));
  emit_op(c, OP_CLOSURE);
  emit_u32(c, (uint32_t)idx);
  emit_set_function_name(c, node->str, node->len);
  if (is_repl_top_level(c)) {
    emit_atom_op(c, OP_PUT_GLOBAL, node->str, node->len);
  } else {
    int local = resolve_local(c, node->str, node->len);
    emit_put_local(c, local);
  }
}

static void hoist_func_decls(sv_compiler_t *c, sv_ast_list_t *stmts) {
  for (int i = 0; i < stmts->count; i++) {
    sv_ast_t *node = stmts->items[i];
    if (node && node->type == N_EXPORT && node->left)
      node = node->left;
    if (!node) continue;
    if (node->type == N_FUNC && node->str && !(node->flags & FN_ARROW)) {
      hoist_one_func(c, node);
    }
    if (!c->is_strict && (node->type == N_IF || node->type == N_LABEL)) {
      sv_ast_list_t funcs = {0};
      annex_b_collect_funcs(node, &funcs);
      for (int j = 0; j < funcs.count; j++)
        hoist_one_func(c, funcs.items[j]);
    }
  }
}

static void compile_expr(sv_compiler_t *c, sv_ast_t *node) {
  if (!node) { emit_op(c, OP_UNDEF); return; }
  emit_srcpos(c, node);

  switch (node->type) {
    case N_NUMBER:
      emit_number(c, node->num);
      break;

    case N_STRING: {
      emit_constant(c, ast_string_const(c, node));
      break;
    }

    case N_BIGINT: {
      bool neg = false;
      const char *digits = node->str;
      uint32_t dlen = node->len;
      if (dlen > 0 && digits[0] == '-') {
        neg = true; digits++; dlen--;
      }
      if (dlen > 0 && digits[dlen - 1] == 'n') dlen--;
      jsval_t bi = js_mkbigint(c->js, digits, dlen, neg);
      emit_constant(c, bi);
      break;
    }

    case N_BOOL:
      emit_op(c, node->num != 0.0 ? OP_TRUE : OP_FALSE);
      break;

    case N_NULL:
      emit_op(c, OP_NULL);
      break;

    case N_UNDEF:
      emit_op(c, OP_UNDEF);
      break;

    case N_THIS:
      emit_op(c, OP_THIS);
      break;

    case N_GLOBAL_THIS:
      emit_op(c, OP_GLOBAL);
      break;

    case N_NEW_TARGET: {
      static const char nt_name[] = "\x01new.target";
      int local = resolve_local(c, nt_name, sizeof(nt_name) - 1);
      if (local >= 0) {
        emit_get_local(c, local);
      } else {
        int upval = resolve_upvalue(c, nt_name, sizeof(nt_name) - 1);
        if (upval >= 0) {
          emit_op(c, OP_GET_UPVAL);
          emit_u16(c, (uint16_t)upval);
        } else {
          emit_op(c, OP_UNDEF);
        }
      }
      break;
    }

    case N_IDENT:
      emit_get_var(c, node->str, node->len);
      break;

    case N_BINARY:
      compile_binary(c, node);
      break;

    case N_UNARY:
      compile_unary(c, node);
      break;

    case N_UPDATE:
      compile_update(c, node);
      break;

    case N_ASSIGN:
      compile_assign(c, node);
      break;

    case N_TERNARY:
      compile_ternary(c, node);
      break;

    case N_CALL:
      compile_call(c, node);
      break;

    case N_NEW:
      compile_new(c, node);
      break;

    case N_MEMBER:
      compile_member(c, node);
      break;

    case N_OPTIONAL:
      compile_optional(c, node);
      break;

    case N_ARRAY:
      compile_array(c, node);
      break;

    case N_OBJECT:
      compile_object(c, node);
      break;

    case N_FUNC:
      compile_func_expr(c, node);
      break;

    case N_CLASS:
      compile_class(c, node);
      break;

    case N_SEQUENCE:
      compile_expr(c, node->left);
      emit_op(c, OP_POP);
      compile_expr(c, node->right);
      break;

    case N_TYPEOF:
      compile_typeof(c, node);
      break;

    case N_VOID:
      compile_expr(c, node->right);
      emit_op(c, OP_VOID);
      break;

    case N_DELETE:
      compile_delete(c, node);
      break;

    case N_SPREAD:
      compile_expr(c, node->right);
      break;

    case N_TEMPLATE:
      compile_template(c, node);
      break;

    case N_AWAIT:
      compile_expr(c, node->right);
      emit_op(c, OP_AWAIT);
      if (c->enclosing && !c->enclosing->enclosing)
        c->is_tla = true;
      break;

    case N_YIELD:
      if (node->right) compile_expr(c, node->right);
      else emit_op(c, OP_UNDEF);
      emit_op(c, node->flags ? OP_YIELD_STAR : OP_YIELD);
      break;

    case N_TAGGED_TEMPLATE: {
      compile_expr(c, node->left);
      sv_ast_t *tpl = node->right;
      int n = tpl->args.count;
      int n_strings = 0, n_exprs = 0;
      for (int i = 0; i < n; i++) {
        if (tpl->args.items[i]->type == N_STRING) n_strings++;
        else n_exprs++;
      }
      int cache_idx = add_constant(c, js_mkundef());
      emit_op(c, OP_CONST);
      emit_u32(c, (uint32_t)cache_idx);
      int skip_jump = emit_jump(c, OP_JMP_TRUE_PEEK);
      emit_op(c, OP_POP);
      for (int i = 0; i < n; i++) {
        sv_ast_t *item = tpl->args.items[i];
        if (item->type != N_STRING) continue;
        if (is_invalid_cooked_string(item))
          emit_op(c, OP_UNDEF);
        else emit_constant(c, ast_string_const(c, item));
      }
      emit_op(c, OP_ARRAY);
      emit_u16(c, (uint16_t)n_strings);
      for (int i = 0; i < n; i++) {
        sv_ast_t *item = tpl->args.items[i];
        if (item->type != N_STRING) continue;
        const char *raw = item->aux ? item->aux : item->str;
        uint32_t raw_len = item->aux ? item->aux_len : item->len;
        emit_constant(c, js_mkstr(c->js, raw ? raw : "", raw_len));
      }
      emit_op(c, OP_ARRAY);
      emit_u16(c, (uint16_t)n_strings);
      emit_atom_op(c, OP_GET_GLOBAL, "Object", 6);
      emit_atom_op(c, OP_GET_FIELD2, "freeze", 6);
      emit_op(c, OP_ROT3L);
      emit_op(c, OP_CALL_METHOD);
      emit_u16(c, 1);
      emit_atom_op(c, OP_DEFINE_FIELD, "raw", 3);
      emit_atom_op(c, OP_GET_GLOBAL, "Object", 6);
      emit_atom_op(c, OP_GET_FIELD2, "freeze", 6);
      emit_op(c, OP_ROT3L);
      emit_op(c, OP_CALL_METHOD);
      emit_u16(c, 1);
      emit_op(c, OP_DUP);
      emit_op(c, OP_PUT_CONST);
      emit_u32(c, (uint32_t)cache_idx);
      patch_jump(c, skip_jump);
      for (int i = 0; i < n; i++) {
        sv_ast_t *item = tpl->args.items[i];
        if (item->type == N_STRING) continue;
        compile_expr(c, item);
      }
      emit_op(c, OP_CALL);
      emit_u16(c, (uint16_t)(1 + n_exprs));
      break;
    }

    case N_IMPORT:
      compile_expr(c, node->right);
      emit_op(c, OP_IMPORT);
      break;

    case N_REGEXP:
      emit_constant(c, js_mkstr(c->js, node->str ? node->str : "", node->len));
      emit_constant(c, js_mkstr(c->js, node->aux ? node->aux : "", node->aux_len));
      emit_op(c, OP_REGEXP);
      break;

    default:
      emit_op(c, OP_UNDEF);
      break;
  }
}

static void compile_binary(sv_compiler_t *c, sv_ast_t *node) {
  uint8_t op = node->op;

  if (op == TOK_LAND) {
    compile_expr(c, node->left);
    int jump = emit_jump(c, OP_JMP_FALSE_PEEK);
    emit_op(c, OP_POP);
    compile_expr(c, node->right);
    patch_jump(c, jump);
    return;
  }
  
  if (op == TOK_LOR) {
    compile_expr(c, node->left);
    int jump = emit_jump(c, OP_JMP_TRUE_PEEK);
    emit_op(c, OP_POP);
    compile_expr(c, node->right);
    patch_jump(c, jump);
    return;
  }
  
  if (op == TOK_NULLISH) {
    compile_expr(c, node->left);
    int jump = emit_jump(c, OP_JMP_NOT_NULLISH);
    emit_op(c, OP_POP);
    compile_expr(c, node->right);
    patch_jump(c, jump);
    return;
  }

  if (op == TOK_IN && node->left->type == N_IDENT &&
      node->left->len > 0 && node->left->str[0] == '#') {
    emit_constant(c, js_mkstr(c->js, node->left->str, node->left->len));
    compile_expr(c, node->right);
    emit_op(c, OP_IN);
    return;
  }

  compile_expr(c, node->left);
  compile_expr(c, node->right);

  switch (op) {
    case TOK_PLUS:       emit_op(c, OP_ADD); break;
    case TOK_MINUS:      emit_op(c, OP_SUB); break;
    case TOK_MUL:        emit_op(c, OP_MUL); break;
    case TOK_DIV:        emit_op(c, OP_DIV); break;
    case TOK_REM:        emit_op(c, OP_MOD); break;
    case TOK_EXP:        emit_op(c, OP_EXP); break;
    case TOK_LT:         emit_op(c, OP_LT);  break;
    case TOK_LE:         emit_op(c, OP_LE);  break;
    case TOK_GT:         emit_op(c, OP_GT);  break;
    case TOK_GE:         emit_op(c, OP_GE);  break;
    case TOK_EQ:         emit_op(c, OP_EQ);  break;
    case TOK_NE:         emit_op(c, OP_NE);  break;
    case TOK_SEQ:        emit_op(c, OP_SEQ); break;
    case TOK_SNE:        emit_op(c, OP_SNE); break;
    case TOK_AND:        emit_op(c, OP_BAND); break;
    case TOK_OR:         emit_op(c, OP_BOR);  break;
    case TOK_XOR:        emit_op(c, OP_BXOR); break;
    case TOK_SHL:        emit_op(c, OP_SHL);  break;
    case TOK_SHR:        emit_op(c, OP_SHR);  break;
    case TOK_ZSHR:       emit_op(c, OP_USHR); break;
    case TOK_INSTANCEOF: emit_op(c, OP_INSTANCEOF); break;
    case TOK_IN:         emit_op(c, OP_IN);  break;
    default:             emit_op(c, OP_UNDEF); break;
  }
}

static void compile_unary(sv_compiler_t *c, sv_ast_t *node) {
  compile_expr(c, node->right);
  switch (node->op) {
    case TOK_NOT:    emit_op(c, OP_NOT);   break;
    case TOK_TILDA:  emit_op(c, OP_BNOT);  break;
    case TOK_UPLUS:  emit_op(c, OP_UPLUS); break;
    case TOK_UMINUS: emit_op(c, OP_NEG);   break;
    default: break;
  }
}


static void compile_update(sv_compiler_t *c, sv_ast_t *node) {
  bool prefix = (node->flags & 1);
  bool is_inc = (node->op == TOK_POSTINC);
  sv_ast_t *target = node->right;

  if (target->type == N_IDENT) {
    if (prefix) {
      emit_get_var(c, target->str, target->len);
      emit_op(c, is_inc ? OP_INC : OP_DEC);
      emit_set_var(c, target->str, target->len, true);
    } else {
      emit_get_var(c, target->str, target->len);
      emit_op(c, is_inc ? OP_POST_INC : OP_POST_DEC);
      emit_set_var(c, target->str, target->len, false);
    }
  } else if (target->type == N_MEMBER && !(target->flags & 1)) {
    compile_expr(c, target->left);
    emit_op(c, OP_DUP);
    int atom = add_atom(c, target->right->str, target->right->len);
    emit_op(c, OP_GET_FIELD);
    emit_u32(c, (uint32_t)atom);
    if (prefix) {
      emit_op(c, is_inc ? OP_INC : OP_DEC);
      emit_op(c, OP_INSERT2);
      emit_op(c, OP_PUT_FIELD);
      emit_u32(c, (uint32_t)atom);
    } else {
      emit_op(c, is_inc ? OP_POST_INC : OP_POST_DEC);
      emit_op(c, OP_SWAP_UNDER);
      emit_op(c, OP_PUT_FIELD);
      emit_u32(c, (uint32_t)atom);
    }
  } else if (target->type == N_MEMBER && (target->flags & 1)) {
    compile_expr(c, target->left);
    compile_expr(c, target->right);
    emit_op(c, OP_DUP2);
    emit_op(c, OP_GET_ELEM);
    if (prefix) {
      emit_op(c, is_inc ? OP_INC : OP_DEC);
      emit_op(c, OP_INSERT3);
      emit_op(c, OP_PUT_ELEM);
    } else {
      emit_op(c, is_inc ? OP_POST_INC : OP_POST_DEC);
      emit_op(c, OP_ROT4_UNDER);
      emit_op(c, OP_PUT_ELEM);
    }
  } else {
    emit_op(c, OP_UNDEF);
  }
}

static void compile_assign(sv_compiler_t *c, sv_ast_t *node) {
  sv_ast_t *target = node->left;
  uint8_t op = node->op;

  if (op == TOK_ASSIGN) {
    if (target->type == N_MEMBER && !(target->flags & 1)) {
      int atom = add_atom(c, target->right->str, target->right->len);
      compile_expr(c, target->left);
      compile_expr(c, node->right);
      emit_op(c, OP_INSERT2);
      emit_op(c, OP_PUT_FIELD);
      emit_u32(c, (uint32_t)atom);
      return;
    }

    if (target->type == N_MEMBER && (target->flags & 1)) {
      compile_expr(c, target->left);
      compile_expr(c, target->right);
      compile_expr(c, node->right);
      emit_op(c, OP_INSERT3);
      emit_op(c, OP_PUT_ELEM);
      return;
    }

    compile_expr(c, node->right);
    compile_lhs_set(c, target, true);
    return;
  }

  if (target->type == N_IDENT) {
    if (op == TOK_PLUS_ASSIGN) {
      int slot = resolve_local_slot(c, target->str, target->len);
      if (slot >= 0 && !c->locals[slot + c->param_locals].is_const) {
        sv_ast_t *rhs = node->right;
        bool rhs_pure = rhs && (
          rhs->type == N_NUMBER || rhs->type == N_STRING ||
          rhs->type == N_BOOL || rhs->type == N_NULL ||
          rhs->type == N_UNDEF || rhs->type == N_IDENT
        );
        if (rhs_pure) {
          compile_expr(c, rhs);
          emit_op(c, OP_ADD_LOCAL);
          emit(c, (uint8_t)slot);
          emit_get_local(c, c->param_locals + slot);
          return;
        }
      }
    }

    if (op == TOK_LOR_ASSIGN || op == TOK_LAND_ASSIGN ||
        op == TOK_NULLISH_ASSIGN) {
      emit_get_var(c, target->str, target->len);
      int skip = emit_jump(c,
        op == TOK_LOR_ASSIGN ? OP_JMP_TRUE_PEEK :
        op == TOK_LAND_ASSIGN ? OP_JMP_FALSE_PEEK : OP_JMP_NOT_NULLISH);
      emit_op(c, OP_POP);
      compile_expr(c, node->right);
      emit_set_var(c, target->str, target->len, true);
      patch_jump(c, skip);
      return;
    }

    emit_get_var(c, target->str, target->len);
    compile_expr(c, node->right);
    switch (op) {
      case TOK_PLUS_ASSIGN:    emit_op(c, OP_ADD); break;
      case TOK_MINUS_ASSIGN:   emit_op(c, OP_SUB); break;
      case TOK_MUL_ASSIGN:     emit_op(c, OP_MUL); break;
      case TOK_DIV_ASSIGN:     emit_op(c, OP_DIV); break;
      case TOK_REM_ASSIGN:     emit_op(c, OP_MOD); break;
      case TOK_SHL_ASSIGN:     emit_op(c, OP_SHL); break;
      case TOK_SHR_ASSIGN:     emit_op(c, OP_SHR); break;
      case TOK_ZSHR_ASSIGN:    emit_op(c, OP_USHR); break;
      case TOK_AND_ASSIGN:     emit_op(c, OP_BAND); break;
      case TOK_XOR_ASSIGN:     emit_op(c, OP_BXOR); break;
      case TOK_OR_ASSIGN:      emit_op(c, OP_BOR); break;
      case TOK_EXP_ASSIGN:    emit_op(c, OP_EXP); break;
      default: break;
    }
    emit_set_var(c, target->str, target->len, true);
  } else if (target->type == N_MEMBER && !(target->flags & 1)) {
    int atom = add_atom(c, target->right->str, target->right->len);

    if (op == TOK_LOR_ASSIGN || op == TOK_LAND_ASSIGN ||
        op == TOK_NULLISH_ASSIGN) {
      compile_expr(c, target->left);
      emit_op(c, OP_DUP);
      emit_op(c, OP_GET_FIELD);
      emit_u32(c, (uint32_t)atom);
      int skip = emit_jump(c,
        op == TOK_LOR_ASSIGN ? OP_JMP_TRUE_PEEK :
        op == TOK_LAND_ASSIGN ? OP_JMP_FALSE_PEEK : OP_JMP_NOT_NULLISH);
      emit_op(c, OP_POP);
      compile_expr(c, node->right);
      emit_op(c, OP_INSERT2);
      emit_op(c, OP_PUT_FIELD);
      emit_u32(c, (uint32_t)atom);
      int end = emit_jump(c, OP_JMP);
      patch_jump(c, skip);
      emit_op(c, OP_NIP);
      patch_jump(c, end);
      return;
    }

    compile_expr(c, target->left);
    emit_op(c, OP_DUP);
    emit_op(c, OP_GET_FIELD);
    emit_u32(c, (uint32_t)atom);
    compile_expr(c, node->right);
    switch (op) {
      case TOK_PLUS_ASSIGN:  emit_op(c, OP_ADD); break;
      case TOK_MINUS_ASSIGN: emit_op(c, OP_SUB); break;
      case TOK_MUL_ASSIGN:   emit_op(c, OP_MUL); break;
      case TOK_DIV_ASSIGN:   emit_op(c, OP_DIV); break;
      case TOK_REM_ASSIGN:   emit_op(c, OP_MOD); break;
      case TOK_SHL_ASSIGN:   emit_op(c, OP_SHL); break;
      case TOK_SHR_ASSIGN:   emit_op(c, OP_SHR); break;
      case TOK_ZSHR_ASSIGN:  emit_op(c, OP_USHR); break;
      case TOK_AND_ASSIGN:   emit_op(c, OP_BAND); break;
      case TOK_XOR_ASSIGN:   emit_op(c, OP_BXOR); break;
      case TOK_OR_ASSIGN:    emit_op(c, OP_BOR); break;
      case TOK_EXP_ASSIGN:  emit_op(c, OP_EXP); break;
      default: break;
    }
    emit_op(c, OP_INSERT2);
    emit_op(c, OP_PUT_FIELD);
    emit_u32(c, (uint32_t)atom);
  } else if (target->type == N_MEMBER && (target->flags & 1)) {

    if (op == TOK_LOR_ASSIGN || op == TOK_LAND_ASSIGN ||
        op == TOK_NULLISH_ASSIGN) {
      compile_expr(c, target->left);
      compile_expr(c, target->right);
      emit_op(c, OP_DUP2);
      emit_op(c, OP_GET_ELEM);
      int skip = emit_jump(c,
        op == TOK_LOR_ASSIGN ? OP_JMP_TRUE_PEEK :
        op == TOK_LAND_ASSIGN ? OP_JMP_FALSE_PEEK : OP_JMP_NOT_NULLISH);
      emit_op(c, OP_POP);
      compile_expr(c, node->right);
      emit_op(c, OP_INSERT3);
      emit_op(c, OP_PUT_ELEM);
      int end = emit_jump(c, OP_JMP);
      patch_jump(c, skip);
      emit_op(c, OP_NIP2);
      patch_jump(c, end);
      return;
    }

    compile_expr(c, target->left);
    compile_expr(c, target->right);
    emit_op(c, OP_DUP2);
    emit_op(c, OP_GET_ELEM);
    compile_expr(c, node->right);
    switch (op) {
      case TOK_PLUS_ASSIGN:  emit_op(c, OP_ADD); break;
      case TOK_MINUS_ASSIGN: emit_op(c, OP_SUB); break;
      case TOK_MUL_ASSIGN:   emit_op(c, OP_MUL); break;
      case TOK_DIV_ASSIGN:   emit_op(c, OP_DIV); break;
      case TOK_REM_ASSIGN:   emit_op(c, OP_MOD); break;
      case TOK_SHL_ASSIGN:   emit_op(c, OP_SHL); break;
      case TOK_SHR_ASSIGN:   emit_op(c, OP_SHR); break;
      case TOK_ZSHR_ASSIGN:  emit_op(c, OP_USHR); break;
      case TOK_AND_ASSIGN:   emit_op(c, OP_BAND); break;
      case TOK_XOR_ASSIGN:   emit_op(c, OP_BXOR); break;
      case TOK_OR_ASSIGN:    emit_op(c, OP_BOR); break;
      case TOK_EXP_ASSIGN:  emit_op(c, OP_EXP); break;
      default: break;
    }
    emit_op(c, OP_INSERT3);
    emit_op(c, OP_PUT_ELEM);
  } else {
    compile_expr(c, node->right);
  }
}

static void compile_lhs_set(sv_compiler_t *c, sv_ast_t *target, bool keep) {
  if (target->type == N_IDENT) {
    emit_set_var(c, target->str, target->len, keep);
  } else if (target->type == N_MEMBER && !(target->flags & 1)) {
    if (keep) emit_op(c, OP_DUP);
    compile_expr(c, target->left);
    emit_op(c, OP_SWAP);
    emit_atom_op(c, OP_PUT_FIELD, target->right->str, target->right->len);
  } else if (target->type == N_MEMBER && (target->flags & 1)) {
    if (keep) emit_op(c, OP_DUP);
    compile_expr(c, target->left);
    compile_expr(c, target->right);
    emit_op(c, OP_ROT3L);
    emit_op(c, OP_PUT_ELEM);
  } else if (target->type == N_ARRAY_PAT || target->type == N_ARRAY) {
    compile_array_destructure(c, target, keep);
  } else if (target->type == N_OBJECT_PAT || target->type == N_OBJECT) {
    compile_object_destructure(c, target, keep);
  }
}

static void compile_ternary(sv_compiler_t *c, sv_ast_t *node) {
  compile_expr(c, node->cond);
  int else_jump = emit_jump(c, OP_JMP_FALSE);
  compile_expr(c, node->left);
  int end_jump = emit_jump(c, OP_JMP);
  patch_jump(c, else_jump);
  compile_expr(c, node->right);
  patch_jump(c, end_jump);
}

static void compile_typeof(sv_compiler_t *c, sv_ast_t *node) {
  sv_ast_t *arg = node->right;
  if (arg->type == N_IDENT) {
    int local = resolve_local(c, arg->str, arg->len);
    if (local != -1) {
      emit_get_var(c, arg->str, arg->len);
    } else {
      int upval = resolve_upvalue(c, arg->str, arg->len);
      if (upval != -1) {
        emit_op(c, OP_GET_UPVAL);
        emit_u16(c, (uint16_t)upval);
      } else if (
          has_implicit_arguments_obj(c) &&
          is_ident_str(arg->str, arg->len, "arguments", 9)
        ) {
        if (c->strict_args_local >= 0) {
          emit_get_local(c, c->strict_args_local);
        } else {
          emit_op(c, OP_SPECIAL_OBJ);
          emit(c, 0);
        }
      } else emit_atom_op(c, OP_GET_GLOBAL_UNDEF, arg->str, arg->len);
    }
  } else compile_expr(c, arg);
  emit_op(c, OP_TYPEOF);
}

static void compile_delete(sv_compiler_t *c, sv_ast_t *node) {
  sv_ast_t *arg = node->right;
  if (arg->type == N_MEMBER && !(arg->flags & 1)) {
    compile_expr(c, arg->left);
    jsval_t key = js_mkstr(c->js, arg->right->str, arg->right->len);
    emit_constant(c, key);
    emit_op(c, OP_DELETE);
  } else if (arg->type == N_MEMBER && (arg->flags & 1)) {
    compile_expr(c, arg->left);
    compile_expr(c, arg->right);
    emit_op(c, OP_DELETE);
  } else if (arg->type == N_IDENT) {
    emit_atom_op(c, OP_DELETE_VAR, arg->str, arg->len);
  } else {
    compile_expr(c, arg);
    emit_op(c, OP_POP);
    emit_op(c, OP_TRUE);
  }
}

static void compile_template(sv_compiler_t *c, sv_ast_t *node) {
  int n = node->args.count;
  if (n == 0) {
    emit_constant(c, js_mkstr(c->js, "", 0));
    return;
  }
  for (int i = 0; i < n; i++) {
    sv_ast_t *item = node->args.items[i];
    if (item->type == N_STRING && is_invalid_cooked_string(item)) {
      static const char msg[] = "Invalid or unexpected token";
      int atom = add_atom(c, msg, sizeof(msg) - 1);
      emit_op(c, OP_THROW_ERROR);
      emit_u32(c, (uint32_t)atom);
      emit(c, (uint8_t)JS_ERR_SYNTAX);
      return;
    }
  }
  compile_expr(c, node->args.items[0]);
  if (node->args.items[0]->type != N_STRING)
    emit_op(c, OP_TO_PROPKEY);
  for (int i = 1; i < n; i++) {
    compile_expr(c, node->args.items[i]);
    if (node->args.items[i]->type != N_STRING)
      emit_op(c, OP_TO_PROPKEY);
    emit_op(c, OP_ADD);
  }
}

static bool call_has_spread_arg(const sv_ast_t *node) {
  if (!node) return false;
  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *arg = node->args.items[i];
    if (arg && arg->type == N_SPREAD) return true;
  }
  return false;
}

static void compile_push_arg_to_array(sv_compiler_t *c, sv_ast_t *arg) {
  emit_op(c, OP_DUP);
  emit_op(c, OP_DUP);
  emit_atom_op(c, OP_GET_FIELD, "push", 4);
  compile_expr(c, arg);
  emit_op(c, OP_CALL_METHOD);
  emit_u16(c, 1);
  emit_op(c, OP_POP);
}

static void compile_concat_spread_to_array(sv_compiler_t *c, sv_ast_t *spread_arg) {
  compile_expr(c, spread_arg->right);
  emit_op(c, OP_SPREAD);
}

static void compile_call_args_array(sv_compiler_t *c, sv_ast_t *call_node) {
  emit_op(c, OP_ARRAY);
  emit_u16(c, 0);
  for (int i = 0; i < call_node->args.count; i++) {
    sv_ast_t *arg = call_node->args.items[i];
    if (arg && arg->type == N_SPREAD) compile_concat_spread_to_array(c, arg);
    else compile_push_arg_to_array(c, arg);
  }
}

typedef enum {
  SV_CALL_DIRECT = 0,
  SV_CALL_METHOD = 1,
} sv_call_kind_t;

static void compile_receiver_property_get(sv_compiler_t *c, sv_ast_t *node) {
  emit_op(c, OP_DUP);
  if (node->flags & 1) {
    compile_expr(c, node->right);
    emit_op(c, OP_GET_ELEM);
  } else {
    emit_srcpos(c, node->right);
    emit_atom_op(c, OP_GET_FIELD, node->right->str, node->right->len);
  }
}

static void compile_call_emit_invoke(
  sv_compiler_t *c, sv_ast_t *node,
  sv_call_kind_t kind, bool has_spread
) {
  int argc = node->args.count;
  if (has_spread) {
    if (kind == SV_CALL_METHOD) emit_op(c, OP_SWAP);
    else emit_op(c, OP_GLOBAL);
    compile_call_args_array(c, node);
    emit_op(c, OP_APPLY);
    emit_u16(c, 1);
    return;
  }

  for (int i = 0; i < argc; i++)
    compile_expr(c, node->args.items[i]);
  emit_op(c, kind == SV_CALL_METHOD ? OP_CALL_METHOD : OP_CALL);
  emit_u16(c, (uint16_t)argc);
}

static sv_call_kind_t compile_call_setup_non_optional(sv_compiler_t *c, sv_ast_t *callee) {
  if (is_ident_name(callee, "super")) {
    emit_op(c, OP_THIS);
    emit_get_var(c, "super", 5);
    return SV_CALL_METHOD;
  }

  if (callee->type == N_MEMBER && is_ident_name(callee->left, "super")) {
    emit_op(c, OP_THIS);
    emit_op(c, OP_THIS);
    emit_get_var(c, "super", 5);
    if (callee->flags & 1)
      compile_expr(c, callee->right);
    else
      emit_constant(c, js_mkstr(c->js, callee->right->str, callee->right->len));
    emit_op(c, OP_GET_SUPER_VAL);
    return SV_CALL_METHOD;
  }

  if (callee->type == N_MEMBER) {
    compile_expr(c, callee->left);
    compile_receiver_property_get(c, callee);
    return SV_CALL_METHOD;
  }

  compile_expr(c, callee);
  return SV_CALL_DIRECT;
}

static void compile_optional_call_after_setup(
  sv_compiler_t *c, sv_ast_t *call_node,
  sv_call_kind_t kind, bool has_spread
) {
  emit_op(c, OP_DUP);
  emit_op(c, OP_IS_UNDEF_OR_NULL);
  int j_do_call = emit_jump(c, OP_JMP_FALSE);
  emit_op(c, OP_POP);
  if (kind == SV_CALL_METHOD)
    emit_op(c, OP_POP);
  emit_op(c, OP_UNDEF);
  int j_end = emit_jump(c, OP_JMP);
  patch_jump(c, j_do_call);
  compile_call_emit_invoke(c, call_node, kind, has_spread);
  patch_jump(c, j_end);
}

static void compile_call_optional(
  sv_compiler_t *c, sv_ast_t *node,
  sv_ast_t *opt_callee, bool has_spread
) {
  if (opt_callee->right) {
    compile_expr(c, opt_callee->left);
    emit_op(c, OP_DUP);
    emit_op(c, OP_IS_UNDEF_OR_NULL);
    int j_have_obj = emit_jump(c, OP_JMP_FALSE);
    emit_op(c, OP_POP);
    emit_op(c, OP_UNDEF);
    int j_end = emit_jump(c, OP_JMP);
    patch_jump(c, j_have_obj);

    compile_receiver_property_get(c, opt_callee);
    compile_call_emit_invoke(c, node, SV_CALL_METHOD, has_spread);
    patch_jump(c, j_end);
    
    return;
  }

  sv_ast_t *target = opt_callee->left;
  if (target && target->type == N_OPTIONAL && target->right) {
    compile_expr(c, target->left);
    emit_op(c, OP_DUP);
    emit_op(c, OP_IS_UNDEF_OR_NULL);
    int j_have_obj = emit_jump(c, OP_JMP_FALSE);
    emit_op(c, OP_POP);
    emit_op(c, OP_UNDEF);
    int j_end = emit_jump(c, OP_JMP);
    patch_jump(c, j_have_obj);

    compile_receiver_property_get(c, target);
    compile_optional_call_after_setup(c, node, SV_CALL_METHOD, has_spread);
    patch_jump(c, j_end);
    return;
  }

  sv_call_kind_t kind = compile_call_setup_non_optional(c, target);
  compile_optional_call_after_setup(c, node, kind, has_spread);
}

static void compile_call(sv_compiler_t *c, sv_ast_t *node) {
  sv_ast_t *callee = node->left;
  bool has_spread = call_has_spread_arg(node);

  if (callee->type == N_OPTIONAL) {
    compile_call_optional(c, node, callee, has_spread);
    return;
  }

  if (!has_spread && is_ident_name(callee, "eval")) {
    if (node->args.count > 0)
      compile_expr(c, node->args.items[0]);
    else
      emit_op(c, OP_UNDEF);
    for (int i = 1; i < node->args.count; i++) {
      compile_expr(c, node->args.items[i]);
      emit_op(c, OP_POP);
    }
    
    emit_op(c, OP_EVAL);
    emit_u32(c, 0);
    
    return;
  }

  sv_call_kind_t kind = compile_call_setup_non_optional(c, callee);
  compile_call_emit_invoke(c, node, kind, has_spread);
}

static void compile_new(sv_compiler_t *c, sv_ast_t *node) {
  compile_expr(c, node->left);
  emit_op(c, OP_DUP);
  int argc = node->args.count;
  for (int i = 0; i < argc; i++)
    compile_expr(c, node->args.items[i]);
  emit_op(c, OP_NEW);
  emit_u16(c, (uint16_t)argc);
}

static bool sv_node_has_optional_base(sv_ast_t *n) {
  while (n) {
    if (n->type == N_OPTIONAL) return true;
    if (n->type == N_MEMBER || n->type == N_CALL) n = n->left;
    else break;
  }
  return false;
}

static void compile_member(sv_compiler_t *c, sv_ast_t *node) {
  if (is_ident_name(node->left, "super")) {
    emit_op(c, OP_THIS);
    emit_get_var(c, "super", 5);
    if (node->flags & 1)
      compile_expr(c, node->right);
    else
      emit_constant(c, js_mkstr(c->js, node->right->str, node->right->len));
    emit_op(c, OP_GET_SUPER_VAL);
    return;
  }

  compile_expr(c, node->left);

  int ok_jump = -1, end_jump = -1;
  if (sv_node_has_optional_base(node->left)) {
    ok_jump  = emit_jump(c, OP_JMP_NOT_NULLISH);
    end_jump = emit_jump(c, OP_JMP);
    patch_jump(c, ok_jump);
  }

  if (node->flags & 1) {
    compile_expr(c, node->right);
    emit_op(c, OP_GET_ELEM);
  } else {
    if (node->right->len == 6 && memcmp(node->right->str, "length", 6) == 0)
      emit_op(c, OP_GET_LENGTH);
    else {
      emit_srcpos(c, node->right);
      emit_atom_op(c, OP_GET_FIELD, node->right->str, node->right->len);
    }
  }

  if (end_jump >= 0) patch_jump(c, end_jump);
}

static void compile_optional_get(sv_compiler_t *c, sv_ast_t *node) {
  if (node->flags & 1) {
    compile_expr(c, node->right);
    emit_op(c, OP_GET_ELEM_OPT);
  } else {
    emit_srcpos(c, node->right);
    emit_atom_op(c, OP_GET_FIELD_OPT, node->right->str, node->right->len);
  }
}

static void compile_optional(sv_compiler_t *c, sv_ast_t *node) {
  compile_expr(c, node->left);
  compile_optional_get(c, node);
}

static void compile_array(sv_compiler_t *c, sv_ast_t *node) {
  int count = node->args.count;
  bool has_spread = false;
  for (int i = 0; i < count; i++) {
    sv_ast_t *elem = node->args.items[i];
    if (elem && elem->type == N_SPREAD) {
      has_spread = true;
      break;
    }
  }

  if (!has_spread) {
    for (int i = 0; i < count; i++) {
      sv_ast_t *elem = node->args.items[i];
      if (elem && elem->type == N_EMPTY)
        emit_op(c, OP_EMPTY);
      else
        compile_expr(c, elem);
    }
    emit_op(c, OP_ARRAY);
    emit_u16(c, (uint16_t)count);
    return;
  }

  emit_op(c, OP_ARRAY);
  emit_u16(c, 0);
  for (int i = 0; i < count; i++) {
    sv_ast_t *elem = node->args.items[i];
    if (elem && elem->type == N_SPREAD) compile_concat_spread_to_array(c, elem);
    else compile_push_arg_to_array(c, elem);
  }
}

static void compile_object(sv_compiler_t *c, sv_ast_t *node) {
  emit_op(c, OP_OBJECT);
  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *prop = node->args.items[i];
    if (prop->type == N_SPREAD) {
      compile_expr(c, prop->right);
      emit_op(c, OP_COPY_DATA_PROPS);
      emit(c, 0);
      emit_op(c, OP_POP);
      continue;
    }
    
    if (prop->type != N_PROPERTY) continue;
    if (prop->flags & FN_GETTER || prop->flags & FN_SETTER) {
      compile_expr(c, prop->right);
      uint8_t flags = 0;
      if (prop->flags & FN_GETTER) flags |= 1;
      if (prop->flags & FN_SETTER) flags |= 2;
      if (prop->flags & FN_COMPUTED) compile_expr(c, prop->left);
      else compile_static_property_key(c, prop->left);
      emit_op(c, OP_SWAP);
      emit_op(c, OP_DEFINE_METHOD_COMP);
      emit(c, flags);
    } else if (prop->flags & FN_COMPUTED) {
      compile_expr(c, prop->left);
      compile_expr(c, prop->right);
      emit_op(c, OP_DEFINE_METHOD_COMP);
      emit(c, 0);
    } else {
      if (prop->right && (prop->right->type == N_FUNC || prop->right->type == N_CLASS) &&
          (!prop->right->str || prop->right->len == 0) &&
          prop->left && prop->left->type == N_IDENT && !is_quoted_ident_key(prop->left)) {
        c->inferred_name = prop->left->str;
        c->inferred_name_len = prop->left->len;
      }
      compile_expr(c, prop->right);
      if ((prop->flags & FN_COLON) &&
          prop->left->type == N_IDENT && !is_quoted_ident_key(prop->left) &&
          is_ident_str(prop->left->str, prop->left->len, "__proto__", 9)) {
        emit_op(c, OP_SET_PROTO);
        continue;
      }
      if (prop->left->type == N_IDENT && !is_quoted_ident_key(prop->left)) {
        emit_atom_op(c, OP_DEFINE_FIELD, prop->left->str, prop->left->len);
      } else if (is_quoted_ident_key(prop->left)) {
        emit_atom_op(c, OP_DEFINE_FIELD, prop->left->str + 1, prop->left->len - 2);
      } else if (prop->left->type == N_STRING) {
        emit_atom_op(c, OP_DEFINE_FIELD, prop->left->str ? prop->left->str : "", prop->left->len);
      } else if (prop->left->type == N_NUMBER) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%g", prop->left->num);
        emit_atom_op(c, OP_DEFINE_FIELD, buf, (uint32_t)n);
      } else emit_atom_op(c, OP_DEFINE_FIELD, prop->left->str, prop->left->len);
    }
  }
}

static void compile_func_expr(sv_compiler_t *c, sv_ast_t *node) {
  bool has_name = node->str && node->len > 0 && !(node->flags & FN_ARROW);
  int name_local = -1;

  if (has_name) {
    begin_scope(c);
    name_local = add_local(c, node->str, node->len, true, c->scope_depth);
    emit_op(c, OP_UNDEF);
    emit_put_local(c, name_local);
  }

  sv_func_t *fn = compile_function_body(c, node, SV_COMPILE_SCRIPT);
  if (!fn) {
    emit_op(c, OP_UNDEF);
    if (has_name) end_scope(c);
    return;
  }
  int idx = add_constant(c, mkval(T_CFUNC, (uintptr_t)fn));
  emit_op(c, OP_CLOSURE);
  emit_u32(c, (uint32_t)idx);

  if (node->str && node->len > 0) {
    emit_set_function_name(c, node->str, node->len);
  } else if (c->inferred_name) {
    emit_set_function_name(c, c->inferred_name, c->inferred_name_len);
    c->inferred_name = NULL;
    c->inferred_name_len = 0;
  } else {
    emit_set_function_name(c, NULL, 0);
  }

  if (has_name) {
    emit_op(c, OP_DUP);
    emit_put_local(c, name_local);
    end_scope(c);
  }
}

typedef enum {
  DESTRUCTURE_ASSIGN = 0,
  DESTRUCTURE_BIND   = 1,
} sv_destructure_mode_t;

static void emit_build_object_rest(sv_compiler_t *c) {
  emit_op(c, OP_DUP);
  emit_op(c, OP_OBJECT);
  emit_op(c, OP_SWAP);
  emit_op(c, OP_COPY_DATA_PROPS);
  emit(c, 0);
  emit_op(c, OP_POP);
}

static void emit_delete_rest_key(sv_compiler_t *c, sv_ast_t *key) {
  emit_op(c, OP_DUP);
  if (key->type == N_IDENT) {
    emit_constant(c, js_mkstr(c->js, key->str, key->len));
  } else if (key->type == N_STRING) {
    emit_constant(c, ast_string_const(c, key));
  } else if (key->type == N_NUMBER) {
    emit_number(c, key->num);
  } else {
    compile_expr(c, key);
  }
  emit_op(c, OP_DELETE);
  emit_op(c, OP_POP);
}

static bool is_destructure_pattern_node(sv_ast_t *node) {
  return node && (node->type == N_ARRAY_PAT || node->type == N_ARRAY ||
                  node->type == N_OBJECT_PAT || node->type == N_OBJECT);
}

static void compile_destructure_pattern(sv_compiler_t *c, sv_ast_t *pat,
                                        bool keep, bool consume_source,
                                        sv_destructure_mode_t mode,
                                        sv_var_kind_t kind);

static void compile_destructure_store(sv_compiler_t *c, sv_ast_t *target,
                                      sv_destructure_mode_t mode,
                                      sv_var_kind_t kind) {
  if (!target) return;

  if (mode == DESTRUCTURE_ASSIGN) {
    compile_lhs_set(c, target, false);
    return;
  }

  if (target->type == N_IDENT) {
    bool is_const = (kind == SV_VAR_CONST);
    int idx = ensure_local_at_depth(c, target->str, target->len,
                                    is_const, c->scope_depth);
    emit_put_local(c, idx);
    return;
  }

  if (is_destructure_pattern_node(target)) {
    compile_destructure_pattern(c, target, false, true, mode, kind);
    return;
  }

  compile_lhs_set(c, target, false);
}

static void compile_destructure_pattern(sv_compiler_t *c, sv_ast_t *pat,
                                        bool keep, bool consume_source,
                                        sv_destructure_mode_t mode,
                                        sv_var_kind_t kind) {
  if (!pat) return;
  if (keep) emit_op(c, OP_DUP);

  if (pat->type == N_ARRAY_PAT || pat->type == N_ARRAY) {
    for (int i = 0; i < pat->args.count; i++) {
      sv_ast_t *elem = pat->args.items[i];
      if (!elem || elem->type == N_EMPTY) continue;

      if (elem->type == N_REST || elem->type == N_SPREAD) {
        emit_op(c, OP_DUP);
        emit_op(c, OP_DUP);
        emit_atom_op(c, OP_GET_FIELD, "slice", 5);
        emit_number(c, (double)i);
        emit_op(c, OP_CALL_METHOD);
        emit_u16(c, 1);
        compile_destructure_store(c, elem->right, mode, kind);
        continue;
      }

      sv_ast_t *target = elem;
      sv_ast_t *default_val = NULL;
      if (elem->type == N_ASSIGN_PAT ||
          (elem->type == N_ASSIGN && elem->op == TOK_ASSIGN)) {
        target = elem->left;
        default_val = elem->right;
      }

      emit_op(c, OP_DUP);
      emit_number(c, (double)i);
      emit_op(c, OP_GET_ELEM);

      if (default_val) {
        emit_op(c, OP_DUP);
        emit_op(c, OP_IS_UNDEF);
        int skip = emit_jump(c, OP_JMP_FALSE);
        emit_op(c, OP_POP);
        compile_expr(c, default_val);
        patch_jump(c, skip);
      }

      compile_destructure_store(c, target, mode, kind);
    }
  } else if (pat->type == N_OBJECT_PAT || pat->type == N_OBJECT) {
    for (int i = 0; i < pat->args.count; i++) {
      sv_ast_t *prop = pat->args.items[i];
      if (!prop) continue;

      if (prop->type == N_REST || prop->type == N_SPREAD) {
        emit_build_object_rest(c);
        for (int j = 0; j < i; j++) {
          sv_ast_t *prev = pat->args.items[j];
          if (!prev || prev->type != N_PROPERTY || !prev->left) continue;
          emit_delete_rest_key(c, prev->left);
        }
        compile_destructure_store(c, prop->right, mode, kind);
        continue;
      }
      if (prop->type != N_PROPERTY) continue;

      sv_ast_t *key = prop->left;
      sv_ast_t *value = prop->right;
      sv_ast_t *default_val = NULL;
      if (value && (value->type == N_ASSIGN_PAT ||
                    (value->type == N_ASSIGN && value->op == TOK_ASSIGN))) {
        default_val = value->right;
        value = value->left;
      }

      emit_op(c, OP_DUP);
      if ((prop->flags & FN_COMPUTED)) {
        compile_expr(c, key);
        emit_op(c, OP_GET_ELEM);
      } else if (key->type == N_IDENT) {
        emit_atom_op(c, OP_GET_FIELD, key->str, key->len);
      } else {
        compile_expr(c, key);
        emit_op(c, OP_GET_ELEM);
      }

      if (default_val) {
        emit_op(c, OP_DUP);
        emit_op(c, OP_IS_UNDEF);
        int skip = emit_jump(c, OP_JMP_FALSE);
        emit_op(c, OP_POP);
        compile_expr(c, default_val);
        patch_jump(c, skip);
      }

      compile_destructure_store(c, value, mode, kind);
    }
  }

  if (consume_source) emit_op(c, OP_POP);
}

static void compile_array_destructure(sv_compiler_t *c, sv_ast_t *pat,
                                      bool keep) {
  compile_destructure_pattern(c, pat, keep, true, DESTRUCTURE_ASSIGN, SV_VAR_LET);
}

static void compile_object_destructure(sv_compiler_t *c, sv_ast_t *pat,
                                       bool keep) {
  compile_destructure_pattern(c, pat, keep, true, DESTRUCTURE_ASSIGN, SV_VAR_LET);
}

static bool is_tail_callable(sv_compiler_t *c, sv_ast_t *node) {
  if (c->try_depth > 0) return false;
  if (node->type != N_CALL) return false;
  if (call_has_spread_arg(node)) return false;
  sv_ast_t *callee = node->left;
  if (callee->type == N_IDENT && callee->len == 5 && memcmp(callee->str, "super", 5) == 0) return false;
  return true;
}

static void compile_tail_call(sv_compiler_t *c, sv_ast_t *node) {
  sv_ast_t *callee = node->left;

  if (callee->type == N_OPTIONAL) {
    compile_call(c, node);
    emit_close_upvals(c);
    emit_op(c, OP_RETURN);
    return;
  }

  sv_call_kind_t kind = compile_call_setup_non_optional(c, callee);
  int argc = node->args.count;
  for (int i = 0; i < argc; i++)
    compile_expr(c, node->args.items[i]);

  emit_close_upvals(c);
  emit_op(c, kind == SV_CALL_METHOD ? OP_TAIL_CALL_METHOD : OP_TAIL_CALL);
  emit_u16(c, (uint16_t)argc);
}

static void compile_tail_return_expr(sv_compiler_t *c, sv_ast_t *expr) {
  if (expr->type == N_TERNARY) {
    compile_expr(c, expr->cond);
    int else_jump = emit_jump(c, OP_JMP_FALSE);
    compile_tail_return_expr(c, expr->left);
    patch_jump(c, else_jump);
    compile_tail_return_expr(c, expr->right);
    return;
  }

  if (is_tail_callable(c, expr)) {
    compile_tail_call(c, expr);
    return;
  }

  compile_expr(c, expr);
  emit_close_upvals(c);
  emit_op(c, OP_RETURN);
}

static void compile_stmts(sv_compiler_t *c, sv_ast_list_t *list) {
  for (int i = 0; i < list->count; i++)
    compile_stmt(c, list->items[i]);
}

static void compile_stmt(sv_compiler_t *c, sv_ast_t *node) {
  if (!node) return;
  emit_srcpos(c, node);

  switch (node->type) {
    case N_EMPTY:
    case N_DEBUGGER:
      break;

    case N_BLOCK:
      begin_scope(c);
      hoist_lexical_decls(c, &node->args);
      hoist_func_decls(c, &node->args);
      compile_stmts(c, &node->args);
      end_scope(c);
      break;

    case N_VAR:
      compile_var_decl(c, node);
      break;

    case N_IMPORT_DECL:
      compile_import_decl(c, node);
      break;

    case N_EXPORT:
      compile_export_decl(c, node);
      break;

    case N_IF:
      compile_if(c, node);
      break;

    case N_WHILE:
      compile_while(c, node);
      break;

    case N_DO_WHILE:
      compile_do_while(c, node);
      break;

    case N_FOR:
      compile_for(c, node);
      break;

    case N_FOR_IN:
      compile_for_in(c, node);
      break;

    case N_FOR_OF:
    case N_FOR_AWAIT_OF:
      compile_for_of(c, node);
      break;

    case N_RETURN:
      if (node->right) {
        compile_tail_return_expr(c, node->right);
      } else {
        emit_close_upvals(c);
        emit_op(c, OP_RETURN_UNDEF);
      }
      break;

    case N_THROW:
      compile_expr(c, node->right);
      emit_op(c, OP_THROW);
      break;

    case N_BREAK:
      compile_break(c, node);
      break;

    case N_CONTINUE:
      compile_continue(c, node);
      break;

    case N_TRY:
      compile_try(c, node);
      break;

    case N_SWITCH:
      compile_switch(c, node);
      break;

    case N_LABEL:
      compile_label(c, node);
      break;

    case N_FUNC:
      if (node->str && !(node->flags & FN_ARROW)) break;
      compile_expr(c, node);
      emit_op(c, OP_POP);
      break;

    case N_CLASS:
      compile_class(c, node);
      emit_op(c, OP_POP);
      break;

    case N_WITH:
      compile_expr(c, node->left);
      emit_op(c, OP_ENTER_WITH);
      c->with_depth++;
      compile_stmt(c, node->body);
      c->with_depth--;
      emit_op(c, OP_EXIT_WITH);
      break;

    default:
      compile_expr(c, node);
      emit_op(c, OP_POP);
      break;
  }
}

enum {
  IMPORT_BIND_DEFAULT   = 1 << 0,
  IMPORT_BIND_NAMESPACE = 1 << 1,
};

static void compile_import_decl(sv_compiler_t *c, sv_ast_t *node) {
  if (!node->right) return;
  bool repl_top = is_repl_top_level(c);

  compile_expr(c, node->right);
  emit_op(c, OP_IMPORT_SYNC);

  if (node->args.count == 0) {
    emit_op(c, OP_POP);
    return;
  }

  int ns_local = add_local(c, "", 0, false, c->scope_depth);
  emit_put_local(c, ns_local);

  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *spec = node->args.items[i];
    if (!spec || spec->type != N_IMPORT_SPEC ||
        !spec->right || spec->right->type != N_IDENT)
      continue;

    emit_get_local(c, ns_local);

    if (!(spec->flags & IMPORT_BIND_NAMESPACE)) {
      if ((spec->flags & IMPORT_BIND_DEFAULT) ||
          !spec->left || spec->left->type != N_IDENT) {
        emit_op(c, OP_IMPORT_DEFAULT);
      } else {
        emit_atom_op(c, OP_GET_FIELD, spec->left->str, spec->left->len);
      }
    }

    if (repl_top) {
      emit_atom_op(c, OP_PUT_GLOBAL, spec->right->str, spec->right->len);
    } else {
      int idx = ensure_local_at_depth(c, spec->right->str, spec->right->len,
                                      true, c->scope_depth);
      emit_put_local(c, idx);
    }
  }
}

static void compile_export_emit(sv_compiler_t *c, const char *name, uint32_t len) {
  emit_get_var(c, name, len);
  emit_atom_op(c, OP_EXPORT, name, len);
}

static void defer_export(sv_compiler_t *c, const char *name, uint32_t len) {
  if (c->deferred_export_count >= c->deferred_export_cap) {
    int cap = c->deferred_export_cap ? c->deferred_export_cap * 2 : 8;
    c->deferred_exports = realloc(c->deferred_exports, cap * sizeof(sv_deferred_export_t));
    c->deferred_export_cap = cap;
  }
  c->deferred_exports[c->deferred_export_count++] =
    (sv_deferred_export_t){ .name = name, .len = len };
}

static void defer_export_pattern(sv_compiler_t *c, sv_ast_t *pat) {
  if (!pat) return;
  switch (pat->type) {
    case N_IDENT:
      defer_export(c, pat->str, pat->len);
      break;
    case N_ASSIGN_PAT:
      defer_export_pattern(c, pat->left);
      break;
    case N_REST:
    case N_SPREAD:
      defer_export_pattern(c, pat->right);
      break;
    case N_ARRAY:
    case N_ARRAY_PAT:
      for (int i = 0; i < pat->args.count; i++)
        defer_export_pattern(c, pat->args.items[i]);
      break;
    case N_OBJECT:
    case N_OBJECT_PAT:
      for (int i = 0; i < pat->args.count; i++) {
        sv_ast_t *prop = pat->args.items[i];
        if (!prop) continue;
        if (prop->type == N_PROPERTY) defer_export_pattern(c, prop->right);
        else defer_export_pattern(c, prop);
      }
      break;
    default:
      break;
  }
}

static void compile_export_pattern(sv_compiler_t *c, sv_ast_t *pat) {
  if (!pat) return;
  switch (pat->type) {
    case N_IDENT:
      compile_export_emit(c, pat->str, pat->len);
      break;
    case N_ASSIGN_PAT:
      compile_export_pattern(c, pat->left);
      break;
    case N_REST:
    case N_SPREAD:
      compile_export_pattern(c, pat->right);
      break;
    case N_ARRAY:
    case N_ARRAY_PAT:
      for (int i = 0; i < pat->args.count; i++)
        compile_export_pattern(c, pat->args.items[i]);
      break;
    case N_OBJECT:
    case N_OBJECT_PAT:
      for (int i = 0; i < pat->args.count; i++) {
        sv_ast_t *prop = pat->args.items[i];
        if (!prop) continue;
        if (prop->type == N_PROPERTY) compile_export_pattern(c, prop->right);
        else compile_export_pattern(c, prop);
      }
      break;
    default:
      break;
  }
}

static void compile_export_decl(sv_compiler_t *c, sv_ast_t *node) {
  if (!node) return;

  if (node->flags & EX_DEFAULT) {
    if (node->left) compile_expr(c, node->left);
    else emit_op(c, OP_UNDEF);
    emit_atom_op(c, OP_EXPORT, "default", 7);
    return;
  }

  if ((node->flags & EX_DECL) && node->left) {
    sv_ast_t *decl = node->left;
    if (decl->type == N_VAR) {
      compile_var_decl(c, decl);
      bool is_var = decl->var_kind == SV_VAR_VAR;
      for (int i = 0; i < decl->args.count; i++) {
        sv_ast_t *var = decl->args.items[i];
        if (!var || var->type != N_VARDECL) continue;
        if (is_var && c->mode == SV_COMPILE_MODULE)
          defer_export_pattern(c, var->left);
        else
          compile_export_pattern(c, var->left);
      }
      return;
    }

    compile_stmt(c, decl);
    if ((decl->type == N_FUNC || decl->type == N_CLASS) &&
        decl->str && decl->len > 0) {
      compile_export_emit(c, decl->str, decl->len);
    }
    return;
  }

  if ((node->flags & EX_STAR) && (node->flags & EX_FROM) && node->right) {
    compile_expr(c, node->right);
    emit_op(c, OP_IMPORT_SYNC);

    if (node->flags & EX_NAMESPACE) {
      if (node->args.count > 0) {
        sv_ast_t *spec = node->args.items[0];
        if (spec && spec->type == N_IMPORT_SPEC &&
            spec->right && spec->right->type == N_IDENT) {
          emit_atom_op(c, OP_EXPORT, spec->right->str, spec->right->len);
          return;
        }
      }
      emit_op(c, OP_POP);
      return;
    }

    emit_op(c, OP_EXPORT_ALL);
    return;
  }

  if (!(node->flags & EX_NAMED) || node->args.count == 0) return;

  int ns_local = -1;
  if (node->flags & EX_FROM) {
    if (!node->right) return;
    compile_expr(c, node->right);
    emit_op(c, OP_IMPORT_SYNC);
    ns_local = add_local(c, "", 0, false, c->scope_depth);
    emit_put_local(c, ns_local);
  }

  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *spec = node->args.items[i];
    if (!spec || spec->type != N_IMPORT_SPEC || !spec->left || !spec->right ||
        spec->left->type != N_IDENT || spec->right->type != N_IDENT)
      continue;

    if (node->flags & EX_FROM) {
      emit_get_local(c, ns_local);
      if (spec->left->len == 7 && memcmp(spec->left->str, "default", 7) == 0)
        emit_op(c, OP_IMPORT_DEFAULT);
      else
        emit_atom_op(c, OP_GET_FIELD, spec->left->str, spec->left->len);
      emit_atom_op(c, OP_EXPORT, spec->right->str, spec->right->len);
    } else {
      emit_get_var(c, spec->left->str, spec->left->len);
      emit_atom_op(c, OP_EXPORT, spec->right->str, spec->right->len);
    }
  }
}

static void compile_var_decl(sv_compiler_t *c, sv_ast_t *node) {
  sv_var_kind_t kind = node->var_kind;
  bool is_const = (kind == SV_VAR_CONST);
  bool repl_top = is_repl_top_level(c);

  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *decl = node->args.items[i];
    if (decl->type != N_VARDECL) continue;
    sv_ast_t *target = decl->left;

    if (repl_top) {
      if (!decl->right && kind == SV_VAR_VAR) continue;
      if (decl->right) {
        compile_expr(c, decl->right);
      } else {
        emit_op(c, OP_UNDEF);
      }
      if (target->type == N_IDENT) {
        emit_atom_op(c, OP_PUT_GLOBAL, target->str, target->len);
      } else {
        compile_destructure_pattern(c, target, false, true,
                                    DESTRUCTURE_ASSIGN, kind);
      }
    } else if (kind == SV_VAR_VAR) {
      if (decl->right) {
        compile_expr(c, decl->right);
        compile_lhs_set(c, target, false);
      }
    } else {
      if (target->type == N_IDENT) {
        int idx = ensure_local_at_depth(c, target->str, target->len,
                                        is_const, c->scope_depth);
        if (decl->right) {
          compile_expr(c, decl->right);
        } else if (!is_const) {
          emit_op(c, OP_UNDEF);
        }
        if (decl->right || !is_const) {
          emit_put_local(c, idx);
          c->locals[idx].is_tdz = false;
        }
      } else {
        if (decl->right) {
          compile_expr(c, decl->right);
          compile_destructure_binding(c, target, kind);
          emit_op(c, OP_POP);
        }
      }
    }
  }
}

static void compile_destructure_binding(sv_compiler_t *c, sv_ast_t *pat,
                                         sv_var_kind_t kind) {
  compile_destructure_pattern(c, pat, false, false, DESTRUCTURE_BIND, kind);
}


static void compile_if(sv_compiler_t *c, sv_ast_t *node) {
  compile_expr(c, node->cond);
  int else_jump = emit_jump(c, OP_JMP_FALSE);
  compile_stmt(c, node->left);
  if (node->right) {
    int end_jump = emit_jump(c, OP_JMP);
    patch_jump(c, else_jump);
    compile_stmt(c, node->right);
    patch_jump(c, end_jump);
  } else {
    patch_jump(c, else_jump);
  }
}


static void compile_while(sv_compiler_t *c, sv_ast_t *node) {
  int loop_start = c->code_len;
  push_loop(c, loop_start, NULL, 0);

  compile_expr(c, node->cond);
  int exit_jump = emit_jump(c, OP_JMP_FALSE);
  compile_stmt(c, node->body);

  sv_loop_t *loop = &c->loops[c->loop_count - 1];
  for (int i = 0; i < loop->continues.count; i++)
    patch_jump(c, loop->continues.offsets[i]);

  emit_loop(c, loop_start);
  patch_jump(c, exit_jump);
  pop_loop(c);
}


static void compile_do_while(sv_compiler_t *c, sv_ast_t *node) {
  int loop_start = c->code_len;
  push_loop(c, loop_start, NULL, 0);

  compile_stmt(c, node->body);

  sv_loop_t *loop = &c->loops[c->loop_count - 1];
  int cond_start = c->code_len;
  for (int i = 0; i < loop->continues.count; i++)
    patch_jump(c, loop->continues.offsets[i]);

  compile_expr(c, node->cond);
  int exit_jump = emit_jump(c, OP_JMP_FALSE);
  emit_loop(c, loop_start);
  patch_jump(c, exit_jump);
  pop_loop(c);
  (void)cond_start;
}


static void for_add_slot_unique(int **slots, int *count, int *cap, int slot) {
  if (slot < 0) return;
  for (int i = 0; i < *count; i++) {
    if ((*slots)[i] == slot) return;
  }
  if (*count >= *cap) {
    int new_cap = (*cap > 0) ? (*cap * 2) : 8;
    int *new_slots = realloc(*slots, (size_t)new_cap * sizeof(int));
    if (!new_slots) return;
    *slots = new_slots;
    *cap = new_cap;
  }
  (*slots)[(*count)++] = slot;
}

static void for_collect_pattern_slots(sv_compiler_t *c, sv_ast_t *pat,
                                      int **slots, int *count, int *cap) {
  if (!pat) return;
  switch (pat->type) {
    case N_IDENT: {
      int slot = resolve_local(c, pat->str, pat->len);
      for_add_slot_unique(slots, count, cap, slot);
      break;
    }
    case N_ASSIGN_PAT:
      for_collect_pattern_slots(c, pat->left, slots, count, cap);
      break;
    case N_REST:
    case N_SPREAD:
      for_collect_pattern_slots(c, pat->right, slots, count, cap);
      break;
    case N_ARRAY:
    case N_ARRAY_PAT:
      for (int i = 0; i < pat->args.count; i++)
        for_collect_pattern_slots(c, pat->args.items[i], slots, count, cap);
      break;
    case N_OBJECT:
    case N_OBJECT_PAT:
      for (int i = 0; i < pat->args.count; i++) {
        sv_ast_t *prop = pat->args.items[i];
        if (!prop) continue;
        if (prop->type == N_PROPERTY)
          for_collect_pattern_slots(c, prop->right, slots, count, cap);
        else
          for_collect_pattern_slots(c, prop, slots, count, cap);
      }
      break;
    default:
      break;
  }
}

static void for_collect_var_decl_slots(sv_compiler_t *c, sv_ast_t *init_var,
                                       int **slots, int *count, int *cap) {
  if (!init_var || init_var->type != N_VAR) return;
  for (int i = 0; i < init_var->args.count; i++) {
    sv_ast_t *decl = init_var->args.items[i];
    if (!decl || decl->type != N_VARDECL || !decl->left) continue;
    for_collect_pattern_slots(c, decl->left, slots, count, cap);
  }
}

static void compile_for(sv_compiler_t *c, sv_ast_t *node) {
  begin_scope(c);

  int *iter_slots = NULL;
  int iter_count = 0;
  int iter_cap = 0;

  if (node->init) {
    if (node->init->type == N_VAR) {
      sv_var_kind_t kind = node->init->var_kind;
      compile_var_decl(c, node->init);
      if (kind == SV_VAR_LET || kind == SV_VAR_CONST)
        for_collect_var_decl_slots(c, node->init, &iter_slots, &iter_count, &iter_cap);
    } else {
      compile_expr(c, node->init);
      emit_op(c, OP_POP);
    }
  }

  int loop_start = c->code_len;
  push_loop(c, loop_start, NULL, 0);

  int exit_jump = -1;
  if (node->cond) {
    compile_expr(c, node->cond);
    exit_jump = emit_jump(c, OP_JMP_FALSE);
  }

  int iter_inner_start = -1;
  if (iter_count > 0) {
    begin_scope(c);
    iter_inner_start = c->local_count;
    for (int i = 0; i < iter_count; i++) {
      int outer_idx = iter_slots[i];
      sv_local_t outer = c->locals[outer_idx];
      emit_get_local(c, outer_idx);
      int inner_idx = add_local(c, outer.name, outer.name_len,
                                outer.is_const, c->scope_depth);
      emit_put_local(c, inner_idx);
    }
  }

  compile_stmt(c, node->body);

  sv_loop_t *loop = &c->loops[c->loop_count - 1];
  for (int i = 0; i < loop->continues.count; i++)
    patch_jump(c, loop->continues.offsets[i]);

  int break_close_slot = -1;
  if (iter_count > 0) {
    for (int i = 0; i < iter_count; i++) {
      int inner_idx = iter_inner_start + i;
      if (!c->locals[inner_idx].captured) continue;
      int slot = local_to_frame_slot(c, inner_idx);
      if (break_close_slot < 0 || slot < break_close_slot)
        break_close_slot = slot;
    }

    for (int i = 0; i < iter_count; i++) {
      emit_get_local(c, iter_inner_start + i);
      emit_put_local(c, iter_slots[i]);
    }
    end_scope(c);
  }

  if (node->update) {
    sv_ast_t *upd = node->update;
    int slot;
    if (upd->type == N_UPDATE && upd->right && upd->right->type == N_IDENT &&
        (slot = resolve_local_slot(c, upd->right->str, upd->right->len)) >= 0) {
      emit_op(c, upd->op == TOK_POSTINC ? OP_INC_LOCAL : OP_DEC_LOCAL);
      emit(c, (uint8_t)slot);
    } else {
      compile_expr(c, upd);
      emit_op(c, OP_POP);
    }
  }

  emit_loop(c, loop_start);
  if (exit_jump >= 0) patch_jump(c, exit_jump);

  int skip_break_cleanup = -1;
  if (break_close_slot >= 0)
    skip_break_cleanup = emit_jump(c, OP_JMP);

  pop_loop(c);

  if (break_close_slot >= 0) {
    emit_op(c, OP_CLOSE_UPVAL);
    emit_u16(c, (uint16_t)break_close_slot);
    patch_jump(c, skip_break_cleanup);
  }

  free(iter_slots);
  end_scope(c);
}


static void compile_for_each(sv_compiler_t *c, sv_ast_t *node, bool is_for_of);

static void compile_for_in(sv_compiler_t *c, sv_ast_t *node) {
  compile_for_each(c, node, false);
}


static void compile_for_of(sv_compiler_t *c, sv_ast_t *node) {
  compile_for_each(c, node, true);
}

static void compile_for_each_assign_target(sv_compiler_t *c, sv_ast_t *lhs) {
  if (!lhs) return;
  if (lhs->type == N_VAR && lhs->args.count > 0) {
    sv_ast_t *decl = lhs->args.items[0];
    sv_ast_t *target = decl->left ? decl->left : decl;
    if (target->type == N_IDENT) {
      int loc = resolve_local(c, target->str, target->len);
      if (loc == -1) {
        bool is_const = (lhs->var_kind == SV_VAR_CONST);
        loc = add_local(c, target->str, target->len, is_const, c->scope_depth);
      }
      emit_put_local(c, loc);
    } else {
      if (lhs->var_kind == SV_VAR_VAR) {
        compile_lhs_set(c, target, false);
      } else {
        compile_destructure_binding(c, target, lhs->var_kind);
        emit_op(c, OP_POP);
      }
    }
    return;
  }
  if (lhs->type == N_IDENT) {
    emit_set_var(c, lhs->str, lhs->len, false);
    return;
  }
  compile_lhs_set(c, lhs, false);
}

static void compile_for_each(sv_compiler_t *c, sv_ast_t *node, bool is_for_of) {
  begin_scope(c);

  int *iter_slots = NULL;
  int iter_count = 0;
  int iter_cap = 0;

  if (node->left && node->left->type == N_VAR &&
      node->left->var_kind != SV_VAR_VAR) {
    bool is_const = (node->left->var_kind == SV_VAR_CONST);
    int lb = c->local_count;
    for (int i = 0; i < node->left->args.count; i++) {
      sv_ast_t *decl = node->left->args.items[i];
      if (!decl || decl->type != N_VARDECL || !decl->left) continue;
      hoist_lexical_pattern(c, decl->left, is_const);
    }
    for (int i = lb; i < c->local_count; i++) {
      c->locals[i].is_tdz = true;
      int slot = i - c->param_locals;
      emit_op(c, OP_SET_LOCAL_UNDEF);
      emit_u16(c, (uint16_t)slot);
    }
    for_collect_var_decl_slots(c, node->left, &iter_slots, &iter_count, &iter_cap);
  }

  if (!is_for_of && node->left && node->left->type == N_VAR &&
      node->left->var_kind == SV_VAR_VAR && node->left->args.count > 0) {
    sv_ast_t *decl = node->left->args.items[0];
    if (decl && decl->right) {
      compile_expr(c, decl->right);
      sv_ast_t *target = decl->left ? decl->left : decl;
      if (target->type == N_IDENT) {
        int loc = resolve_local(c, target->str, target->len);
        if (loc == -1)
          loc = add_local(c, target->str, target->len, false, c->scope_depth);
        emit_put_local(c, loc);
      } else {
        compile_lhs_set(c, target, false);
      }
    }
  }

  int iter_local = -1;
  int idx_local = -1;
  int exit_jump = -1;
  int try_jump_for_of = -1;
  int iter_err_local = -1;
  int break_close_slot = -1;
  int iter_inner_start = -1;

  bool is_for_await = (node->type == N_FOR_AWAIT_OF);
  compile_expr(c, node->right);
  if (is_for_of) {
    emit_op(c, is_for_await ? OP_FOR_AWAIT_OF : OP_FOR_OF);
    iter_err_local = add_local(c, "", 0, false, c->scope_depth);
    try_jump_for_of = emit_jump(c, OP_TRY_PUSH);
    c->try_depth++;
  } else {
    emit_op(c, OP_FOR_IN);  
    iter_local = add_local(c, "", 0, false, c->scope_depth);
    emit_put_local(c, iter_local);

    emit_number(c, 0);
    idx_local = add_local(c, "", 0, false, c->scope_depth);
    emit_put_local(c, idx_local);
  }

  int loop_start = c->code_len;
  push_loop(c, loop_start, NULL, 0);

  if (is_for_of) {
    emit_op(c, is_for_await ? OP_AWAIT_ITER_NEXT : OP_ITER_NEXT);
    exit_jump = emit_jump(c, OP_JMP_TRUE);  
  } else {
    emit_get_local(c, idx_local);
    emit_get_local(c, iter_local);
    emit_op(c, OP_GET_LENGTH);
    emit_op(c, OP_LT);
    exit_jump = emit_jump(c, OP_JMP_FALSE);

    emit_get_local(c, iter_local);
    emit_get_local(c, idx_local);
    emit_op(c, OP_GET_ELEM);
  }

  compile_for_each_assign_target(c, node->left);

  if (iter_count > 0) {
    begin_scope(c);
    iter_inner_start = c->local_count;
    for (int i = 0; i < iter_count; i++) {
      int outer_idx = iter_slots[i];
      sv_local_t outer = c->locals[outer_idx];
      emit_get_local(c, outer_idx);
      int inner_idx = add_local(c, outer.name, outer.name_len,
                                outer.is_const, c->scope_depth);
      emit_put_local(c, inner_idx);
    }
  }

  compile_stmt(c, node->body);

  sv_loop_t *loop = &c->loops[c->loop_count - 1];
  for (int i = 0; i < loop->continues.count; i++)
    patch_jump(c, loop->continues.offsets[i]);

  if (iter_count > 0) {
    for (int i = 0; i < iter_count; i++) {
      int inner_idx = iter_inner_start + i;
      if (!c->locals[inner_idx].captured) continue;
      int slot = local_to_frame_slot(c, inner_idx);
      if (break_close_slot < 0 || slot < break_close_slot)
        break_close_slot = slot;
    }
    end_scope(c);
  }

  if (!is_for_of) {
    emit_get_local(c, idx_local);
    emit_op(c, OP_INC);
    emit_put_local(c, idx_local);
  }

  emit_loop(c, loop_start);
  patch_jump(c, exit_jump);

  int skip_break_cleanup = -1;
  if (break_close_slot >= 0)
    skip_break_cleanup = emit_jump(c, OP_JMP);

  if (is_for_of) {
    emit_op(c, OP_POP);
    emit_op(c, OP_TRY_POP);

    pop_loop(c);
    if (break_close_slot >= 0) {
      emit_op(c, OP_CLOSE_UPVAL);
      emit_u16(c, (uint16_t)break_close_slot);
      patch_jump(c, skip_break_cleanup);
    }
    emit_op(c, OP_TRY_POP);   
    emit_op(c, OP_ITER_CLOSE);
    int end_jump = emit_jump(c, OP_JMP);

    patch_jump(c, try_jump_for_of);
    int catch_tag = emit_jump(c, OP_CATCH);
    emit_put_local(c, iter_err_local);  
    emit_op(c, OP_ITER_CLOSE);          
    emit_get_local(c, iter_err_local);  
    emit_op(c, OP_THROW);              
    patch_jump(c, catch_tag);

    patch_jump(c, end_jump);
    c->try_depth--;
  } else {
    pop_loop(c);
    if (break_close_slot >= 0) {
      emit_op(c, OP_CLOSE_UPVAL);
      emit_u16(c, (uint16_t)break_close_slot);
      patch_jump(c, skip_break_cleanup);
    }
  }

  free(iter_slots);
  end_scope(c);
}


static void compile_break(sv_compiler_t *c, sv_ast_t *node) {
  if (c->loop_count == 0) return;

  int target = c->loop_count - 1;
  if (node->str) {
    for (int i = c->loop_count - 1; i >= 0; i--) {
      if (c->loops[i].label &&
          c->loops[i].label_len == node->len &&
          memcmp(c->loops[i].label, node->str, node->len) == 0) {
        target = i;
        break;
      }
    }
  }

  int offset = emit_jump(c, OP_JMP);
  patch_list_add(&c->loops[target].breaks, offset);
}


static void compile_continue(sv_compiler_t *c, sv_ast_t *node) {
  if (c->loop_count == 0) return;

  int target = c->loop_count - 1;
  if (node->str) {
    for (int i = c->loop_count - 1; i >= 0; i--) {
      if (c->loops[i].label &&
          c->loops[i].label_len == node->len &&
          memcmp(c->loops[i].label, node->str, node->len) == 0) {
        target = i;
        break;
      }
    }
  }

  int offset = emit_jump(c, OP_JMP);
  patch_list_add(&c->loops[target].continues, offset);
}

static void compile_finally_block(sv_compiler_t *c, sv_ast_t *finally_body) {
  int finally_jump = emit_jump(c, OP_FINALLY);
  compile_stmt(c, finally_body);
  emit_op(c, OP_FINALLY_RET);
  patch_jump(c, finally_jump);
}

static void compile_catch_body(sv_compiler_t *c, sv_ast_t *node) {
  begin_scope(c);
  if (node->catch_param && node->catch_param->type == N_IDENT) {
    int loc = add_local(c, node->catch_param->str,
                        node->catch_param->len, false, c->scope_depth);
    emit_put_local(c, loc);
  } else if (node->catch_param && is_destructure_pattern_node(node->catch_param)) {
    compile_destructure_binding(c, node->catch_param, SV_VAR_LET);
    emit_op(c, OP_POP);
  } else {
    emit_op(c, OP_POP);  
  }
  compile_stmt(c, node->catch_body);
  end_scope(c);
}

static void compile_try(sv_compiler_t *c, sv_ast_t *node) {
  c->try_depth++;
  int try_jump = emit_jump(c, OP_TRY_PUSH);

  compile_stmt(c, node->body);
  emit_op(c, OP_TRY_POP);

  bool has_catch = (node->catch_body != NULL);
  bool has_finally = (node->finally_body != NULL);

  if (!has_finally) {
    int end_jump = emit_jump(c, OP_JMP);
    patch_jump(c, try_jump);
    int catch_tag = emit_jump(c, OP_CATCH);
    if (has_catch)
      compile_catch_body(c, node);
    else
      emit_op(c, OP_POP);
    patch_jump(c, catch_tag);
    patch_jump(c, end_jump);
    c->try_depth--;
    return;
  }

  int to_finally_from_try = emit_jump(c, OP_JMP);
  int to_finally_from_catch = -1;
  int to_finally_from_throw = -1;

  patch_jump(c, try_jump);

  if (has_catch) {
    int catch_tag = emit_jump(c, OP_CATCH);
    int catch_throw_jump = emit_jump(c, OP_TRY_PUSH);

    compile_catch_body(c, node);
    emit_op(c, OP_TRY_POP);
    to_finally_from_catch = emit_jump(c, OP_JMP);

    patch_jump(c, catch_throw_jump);
    emit_op(c, OP_POP);  
    to_finally_from_throw = emit_jump(c, OP_JMP);

    patch_jump(c, catch_tag);
  } else {
    emit_op(c, OP_POP);  
    to_finally_from_throw = emit_jump(c, OP_JMP);
  }

  patch_jump(c, to_finally_from_try);
  if (to_finally_from_catch >= 0) patch_jump(c, to_finally_from_catch);
  if (to_finally_from_throw >= 0) patch_jump(c, to_finally_from_throw);

  compile_finally_block(c, node->finally_body);

  c->try_depth--;
}


static void compile_switch(sv_compiler_t *c, sv_ast_t *node) {
  int case_count = node->args.count;
  int default_case = -1;
  int *match_to_stub = NULL;
  int *stub_to_body = NULL;

  if (case_count > 0) {
    match_to_stub = calloc((size_t)case_count, sizeof(int));
    stub_to_body = calloc((size_t)case_count, sizeof(int));
    for (int i = 0; i < case_count; i++) {
      match_to_stub[i] = -1;
      stub_to_body[i] = -1;
    }
  }

  compile_expr(c, node->cond);  
  begin_scope(c);
  push_loop(c, c->code_len, NULL, 0);  

  for (int i = 0; i < case_count; i++) {
    sv_ast_t *cas = node->args.items[i];
    if (!cas->left) {
      default_case = i;
      continue;
    }
    emit_op(c, OP_DUP);
    compile_expr(c, cas->left);
    emit_op(c, OP_SEQ);
    match_to_stub[i] = emit_jump(c, OP_JMP_TRUE);
  }

  int miss_jump = emit_jump(c, OP_JMP);
  int default_to_body = -1;
  int miss_to_end = -1;

  for (int i = 0; i < case_count; i++) {
    if (match_to_stub[i] < 0) continue;
    patch_jump(c, match_to_stub[i]);
    emit_op(c, OP_POP);
    stub_to_body[i] = emit_jump(c, OP_JMP);
  }

  patch_jump(c, miss_jump);
  emit_op(c, OP_POP);
  if (default_case >= 0)
    default_to_body = emit_jump(c, OP_JMP);
  else
    miss_to_end = emit_jump(c, OP_JMP);

  for (int i = 0; i < case_count; i++) {
    if (i == default_case && default_to_body >= 0)
      patch_jump(c, default_to_body);
    if (stub_to_body[i] >= 0)
      patch_jump(c, stub_to_body[i]);

    sv_ast_t *cas = node->args.items[i];
    compile_stmts(c, &cas->args);
  }

  if (miss_to_end >= 0)
    patch_jump(c, miss_to_end);

  free(match_to_stub);
  free(stub_to_body);
  pop_loop(c);
  end_scope(c);
}


static inline bool is_loop_node(sv_ast_t *n) {
  return n && (n->type == N_WHILE || n->type == N_DO_WHILE ||
               n->type == N_FOR   || n->type == N_FOR_IN  ||
               n->type == N_FOR_OF || n->type == N_FOR_AWAIT_OF);
}

static void compile_label(sv_compiler_t *c, sv_ast_t *node) {
  if (is_loop_node(node->body)) {
    c->pending_label = node->str;
    c->pending_label_len = node->len;
    compile_stmt(c, node->body);
    c->pending_label = NULL;
    c->pending_label_len = 0;
  } else {
    push_loop(c, c->code_len, node->str, node->len);
    compile_stmt(c, node->body);
    pop_loop(c);
  }
}


static void emit_field_inits(sv_compiler_t *c, sv_ast_t **fields, int count) {
  sv_compiler_t *enc = c->enclosing;
  for (int i = 0; i < count; i++) {
    sv_ast_t *m = fields[i];
    emit_op(c, OP_THIS);
    if (m->right) compile_expr(c, m->right);
    else emit_op(c, OP_UNDEF);
    if (m->flags & FN_COMPUTED) {
      int key_local = enc->computed_key_locals[i];
      enc->locals[key_local].captured = true;
      uint16_t slot = (uint16_t)local_to_frame_slot(enc, key_local);
      int uv = add_upvalue(c, slot, true, false);
      emit_op(c, OP_GET_UPVAL);
      emit_u16(c, (uint16_t)uv);
    } else {
      compile_static_property_key(c, m->left);
    }
    emit_op(c, OP_SWAP);
    emit_op(c, OP_DEFINE_METHOD_COMP);
    emit(c, 0);
    emit_op(c, OP_POP);
  }
}


static sv_ast_t *find_class_constructor(sv_ast_t *node) {
  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *m = node->args.items[i];
    if (m->type == N_METHOD && m->left && m->left->type == N_IDENT &&
        m->left->len == 11 &&
        memcmp(m->left->str, "constructor", 11) == 0) {
      return m;
    }
  }
  return NULL;
}

static void compile_class_method(sv_compiler_t *c, sv_ast_t *m,
                                 int ctor_local, int proto_local,
                                 int preeval_key) {
  bool is_static = !!(m->flags & FN_STATIC);
  int home_local = is_static ? ctor_local : proto_local;
  bool is_fn = (m->right && m->right->type == N_FUNC);

  if (is_fn) {
    compile_func_expr(c, m->right);   
    emit_get_local(c, home_local);    
    emit_op(c, OP_SET_HOME_OBJ);      
    emit_op(c, OP_SWAP);              
  } else {
    emit_get_local(c, home_local);    
    if (m->right) compile_expr(c, m->right);
    else emit_op(c, OP_UNDEF);        
  }

  uint8_t method_flags = 0;
  if (m->flags & FN_GETTER) method_flags |= 1;
  if (m->flags & FN_SETTER) method_flags |= 2;

  if (m->flags & FN_COMPUTED) {
    if (preeval_key >= 0) emit_get_local(c, preeval_key);
    else compile_expr(c, m->left);
  } else compile_static_property_key(c, m->left); 
  emit_op(c, OP_SWAP);                          
  emit_op(c, OP_DEFINE_METHOD_COMP);
  emit(c, method_flags);                        
  emit_op(c, OP_POP);
}

static void compile_class(sv_compiler_t *c, sv_ast_t *node) {
  int outer_name_local = -1;
  bool class_repl_top = is_repl_top_level(c);
  if (node->str)
    outer_name_local = resolve_local(c, node->str, node->len);

  if (node->left)
    compile_expr(c, node->left);
  else
    emit_op(c, OP_UNDEF);

  sv_ast_t *ctor_method = find_class_constructor(node);

  int field_count = 0;
  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *m = node->args.items[i];
    if (m->type != N_METHOD) continue;
    if (m == ctor_method) continue;
    if (m->flags & FN_STATIC) continue;
    bool is_fn = (m->right && m->right->type == N_FUNC);
    if (!is_fn) field_count++;
  }

  sv_ast_t **field_inits = NULL;
  int *computed_key_locals = NULL;
  if (field_count > 0) {
    field_inits = malloc(sizeof(sv_ast_t *) * field_count);
    computed_key_locals = malloc(sizeof(int) * field_count);
    int fi = 0;
    for (int i = 0; i < node->args.count; i++) {
      sv_ast_t *m = node->args.items[i];
      if (m->type != N_METHOD) continue;
      if (m == ctor_method) continue;
      if (m->flags & FN_STATIC) continue;
      bool is_fn = (m->right && m->right->type == N_FUNC);
      if (!is_fn) {
        field_inits[fi] = m;
        if (m->flags & FN_COMPUTED) {
          compile_expr(c, m->left);
          int loc = add_local(c, "", 0, false, c->scope_depth);
          emit_put_local(c, loc);
          computed_key_locals[fi] = loc;
        } else {
          computed_key_locals[fi] = -1;
        }
        fi++;
      }
    }
  }

  int *method_comp_keys = NULL;
  if (node->str) {
    for (int i = 0; i < node->args.count; i++) {
      sv_ast_t *m = node->args.items[i];
      if (m->type != N_METHOD || !(m->flags & FN_COMPUTED)) continue;
      if (m == ctor_method) continue;
      bool is_fn = (m->right && m->right->type == N_FUNC);
      if (!is_fn && !(m->flags & FN_STATIC)) continue;
      if (!method_comp_keys) {
        method_comp_keys = malloc(sizeof(int) * node->args.count);
        for (int j = 0; j < node->args.count; j++) method_comp_keys[j] = -1;
      }
      compile_expr(c, m->left);
      int loc = add_local(c, "", 0, false, c->scope_depth);
      emit_put_local(c, loc);
      method_comp_keys[i] = loc;
    }
  }

  int inner_name_local = -1;
  if (node->str) {
    begin_scope(c);
    inner_name_local = add_local(c, node->str, node->len, true, c->scope_depth);
  }

  if (ctor_method && ctor_method->right) {
    c->field_inits = field_inits;
    c->field_init_count = field_count;
    c->computed_key_locals = computed_key_locals;
    compile_func_expr(c, ctor_method->right);
    c->field_inits = NULL;
    c->field_init_count = 0;
    c->computed_key_locals = NULL;
  } else if (field_count > 0) {
    c->computed_key_locals = computed_key_locals;
    sv_compiler_t comp = {0};
    comp.js = c->js;
    comp.source = c->source;
    comp.source_len = c->source_len;
    comp.enclosing = c;
    comp.scope_depth = 0;
    comp.is_strict = c->is_strict;
    comp.param_count = 0;

    emit_field_inits(&comp, field_inits, field_count);
    emit_op(&comp, OP_RETURN_UNDEF);

    sv_func_t *fn = code_arena_bump(sizeof(sv_func_t));
    memset(fn, 0, sizeof(sv_func_t));
    fn->code = code_arena_bump((size_t)comp.code_len);
    memcpy(fn->code, comp.code, (size_t)comp.code_len);
    fn->code_len = comp.code_len;
    if (comp.const_count > 0) {
      fn->constants = code_arena_bump((size_t)comp.const_count * sizeof(jsval_t));
      memcpy(fn->constants, comp.constants, (size_t)comp.const_count * sizeof(jsval_t));
      fn->const_count = comp.const_count;
    }
    if (comp.atom_count > 0) {
      fn->atoms = code_arena_bump((size_t)comp.atom_count * sizeof(sv_atom_t));
      memcpy(fn->atoms, comp.atoms, (size_t)comp.atom_count * sizeof(sv_atom_t));
      fn->atom_count = comp.atom_count;
    }
    if (comp.upvalue_count > 0) {
      fn->upval_descs = code_arena_bump(
        (size_t)comp.upvalue_count * sizeof(sv_upval_desc_t));
      memcpy(fn->upval_descs, comp.upval_descs,
             (size_t)comp.upvalue_count * sizeof(sv_upval_desc_t));
      fn->upvalue_count = comp.upvalue_count;
    }
    fn->max_locals = comp.max_local_count;
    fn->max_stack = fn->max_locals + 64;
    fn->param_count = comp.param_count;
    fn->is_strict = comp.is_strict;
    fn->filename = c->js->filename;
    fn->source_line = (int)node->line;
    if (node->str && node->len > 0) {
      char *name = code_arena_bump(node->len + 1);
      memcpy(name, node->str, node->len);
      name[node->len] = '\0';
      fn->name = name;
    }
    free(comp.code); free(comp.constants); free(comp.atoms);
    free(comp.locals); free(comp.upval_descs); free(comp.loops);

    int idx = add_constant(c, mkval(T_CFUNC, (uintptr_t)fn));
    emit_op(c, OP_CLOSURE);
    emit_u32(c, (uint32_t)idx);
  } else emit_op(c, OP_UNDEF);
  
  free(field_inits);
  free(computed_key_locals);
  c->computed_key_locals = NULL;

  bool has_static_name = false;
  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *m = node->args.items[i];
    if (m->type != N_METHOD) continue;
    if (!(m->flags & FN_STATIC)) continue;
    if (m->flags & FN_COMPUTED) continue;
    if (m->left && m->left->str && m->left->len == 4 &&
        memcmp(m->left->str, "name", 4) == 0) {
      has_static_name = true;
      break;
    }
  }

  if (node->str && !has_static_name) {
    int atom = add_atom(c, node->str, node->len);
    emit_op(c, OP_DEFINE_CLASS);
    emit_u32(c, (uint32_t)atom);
    emit(c, 1);
  } else {
    emit_op(c, OP_DEFINE_CLASS);
    emit_u32(c, 0);
    emit(c, 0);
  }

  int proto_local = add_local(c, "", 0, false, c->scope_depth);
  int ctor_local = add_local(c, "", 0, false, c->scope_depth);
  emit_put_local(c, proto_local);
  emit_put_local(c, ctor_local);

  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *m = node->args.items[i];
    if (m->type == N_STATIC_BLOCK) {
      begin_scope(c);
      compile_stmts(c, &m->args);
      end_scope(c);
      continue;
    }
    if (m->type != N_METHOD) continue;
    if (m == ctor_method) continue;
    bool is_fn = (m->right && m->right->type == N_FUNC);
    if (!is_fn && !(m->flags & FN_STATIC)) continue;
    compile_class_method(c, m, ctor_local, proto_local,
                         method_comp_keys ? method_comp_keys[i] : -1);
  }

  free(method_comp_keys);
  emit_get_local(c, ctor_local);

  if (inner_name_local >= 0) {
    emit_op(c, OP_DUP);
    emit_put_local(c, inner_name_local);
  }

  if (class_repl_top && node->str) {
    emit_op(c, OP_DUP);
    emit_atom_op(c, OP_PUT_GLOBAL, node->str, node->len);
  } else if (outer_name_local >= 0) {
    emit_op(c, OP_DUP);
    emit_put_local(c, outer_name_local);
    c->locals[outer_name_local].is_tdz = false;
  }

  if (node->str)
    end_scope(c);
}

static sv_func_t *compile_function_body(
  sv_compiler_t *enclosing,
  sv_ast_t *node,
  sv_compile_mode_t mode
) {
  sv_compiler_t comp = {0};
  comp.js = enclosing->js;
  comp.source = enclosing->source;
  comp.source_len = enclosing->source_len;
  comp.enclosing = enclosing;
  comp.scope_depth = 0;
  comp.is_arrow = !!(node->flags & FN_ARROW);
  comp.is_strict = enclosing->is_strict;
  comp.mode = mode;
  comp.strict_args_local = -1;
  comp.new_target_local = -1;
  comp.super_local = -1;
  comp.param_count = node->args.count;

  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *p = node->args.items[i];
    if (p->type == N_IDENT) {
      add_local(&comp, p->str, p->len, false, -1);  
    } else if (p->type == N_REST && p->right && p->right->type == N_IDENT) {
      add_local(&comp, p->right->str, p->right->len, false, -1);
    } else if (p->type == N_ASSIGN_PAT && p->left && p->left->type == N_IDENT) {
      add_local(&comp, p->left->str, p->left->len, false, -1);
    } else add_local(&comp, "", 0, false, -1);
  }

  comp.param_locals = comp.local_count;

  if (!comp.is_strict && node->body && node->body->type == N_BLOCK) {
    for (int i = 0; i < node->body->args.count; i++) {
      sv_ast_t *stmt = node->body->args.items[i];
      if (!stmt || stmt->type == N_EMPTY) continue;
      if (stmt->type != N_STRING) break;
      if (sv_ast_is_use_strict(comp.js, stmt)) comp.is_strict = true;
    }
  }

  if (comp.is_strict) {
    const char *param_names[256];
    uint32_t param_lens[256];
    int param_name_count = 0;

    for (int i = 0; i < node->args.count; i++) {
      sv_ast_t *p = node->args.items[i];
      const char *pname = NULL;
      uint32_t plen = 0;

      if (p && p->type == N_IDENT) {
        pname = p->str; plen = p->len;
      } else if (p && p->type == N_REST && p->right && p->right->type == N_IDENT) {
        pname = p->right->str; plen = p->right->len;
      } else if (p && p->type == N_ASSIGN_PAT && p->left && p->left->type == N_IDENT) {
        pname = p->left->str; plen = p->left->len;
      }

      if (!pname || plen == 0) continue;

      if (is_strict_restricted_ident(pname, plen)) {
        js_mkerr_typed(
          comp.js, JS_ERR_SYNTAX,
          "strict mode forbids '%.*s' as a parameter name",
          (int)plen, pname);
        return NULL;
      }

      for (int j = 0; j < param_name_count; j++) {
        if (param_lens[j] == plen &&
            memcmp(param_names[j], pname, plen) == 0) {
          js_mkerr_typed(
            comp.js, JS_ERR_SYNTAX,
            "duplicate parameter name '%.*s' in strict mode",
            (int)plen, pname);
          return NULL;
        }
      }

      if (param_name_count < (int)(sizeof(param_names) / sizeof(param_names[0]))) {
        param_names[param_name_count] = pname;
        param_lens[param_name_count] = plen;
        param_name_count++;
      }
    }
  }

  bool has_non_simple_params = false;
  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *p = node->args.items[i];
    if (p->type != N_IDENT) { has_non_simple_params = true; break; }
  }
  
  bool repl_top = is_repl_top_level(&comp);
  if (!has_non_simple_params && node->body) {
    if (node->body->type == N_BLOCK) {
      if (!repl_top) {
        for (int i = 0; i < node->body->args.count; i++)
          hoist_var_decls(&comp, node->body->args.items[i]);
        hoist_lexical_decls(&comp, &node->body->args);
      }
      hoist_func_decls(&comp, &node->body->args);
    } else if (!repl_top) hoist_var_decls(&comp, node->body);
  }

  if (!has_non_simple_params) {
    for (int i = 0; i < node->args.count; i++) {
      sv_ast_t *p = node->args.items[i];
      if (p->type == N_ASSIGN_PAT) {
        emit_op(&comp, OP_GET_ARG);
        emit_u16(&comp, (uint16_t)i);
        emit_op(&comp, OP_DUP);
        emit_op(&comp, OP_IS_UNDEF);
        int skip = emit_jump(&comp, OP_JMP_FALSE);
        emit_op(&comp, OP_POP);
        compile_expr(&comp, p->right);
        patch_jump(&comp, skip);
        if (p->left && p->left->type == N_IDENT) {
          emit_op(&comp, OP_PUT_ARG);
          emit_u16(&comp, (uint16_t)i);
        } else if (p->left) {
          compile_destructure_binding(&comp, p->left, SV_VAR_LET);
          emit_op(&comp, OP_POP);
        } else emit_op(&comp, OP_POP);
      } else if (
        p->type == N_ARRAY_PAT || p->type == N_ARRAY ||
        p->type == N_OBJECT_PAT || p->type == N_OBJECT) {
        emit_op(&comp, OP_GET_ARG);
        emit_u16(&comp, (uint16_t)i);
        compile_destructure_binding(&comp, p, SV_VAR_LET);
        emit_op(&comp, OP_POP);
      } else if (p->type == N_REST && p->right && p->right->type == N_IDENT) {
        emit_op(&comp, OP_REST);
        emit_u16(&comp, (uint16_t)i);
        int loc = resolve_local(&comp, p->right->str, p->right->len);
        if (loc >= 0) {
          emit_op(&comp, OP_PUT_ARG);
          emit_u16(&comp, (uint16_t)loc);
        }
      } else if (p->type == N_REST && p->right) {
        emit_op(&comp, OP_REST);
        emit_u16(&comp, (uint16_t)i);
        compile_destructure_binding(&comp, p->right, SV_VAR_LET);
        emit_op(&comp, OP_POP);
      }
    }
  } else {
    int *param_bind_locals = NULL;
    int  param_bind_count  = 0;
    int  param_bind_cap    = 0;

    for (int i = 0; i < node->args.count; i++) {
      sv_ast_t *p = node->args.items[i];
      const char *pname = NULL;
      uint32_t    plen  = 0;
      if (p->type == N_IDENT) {
        pname = p->str; plen = p->len;
      } else if ((
        p->type == N_ASSIGN_PAT) && p->left &&
        p->left->type == N_IDENT) {
        pname = p->left->str; plen = p->left->len;
      } else if (
        p->type == N_REST && p->right &&
        p->right->type == N_IDENT) {
        pname = p->right->str; plen = p->right->len;
      }

      int lb = comp.local_count;
      if (pname && plen) {
        int loc = add_local(&comp, pname, plen, false, 0);
        comp.locals[loc].is_tdz = true;
        int slot = loc - comp.param_locals;
        emit_op(&comp, OP_SET_LOCAL_UNDEF);
        emit_u16(&comp, (uint16_t)slot);
      }

      if (param_bind_count >= param_bind_cap) {
        param_bind_cap = param_bind_cap ? param_bind_cap * 2 : 8;
        param_bind_locals = realloc(
          param_bind_locals,
          (size_t)param_bind_cap * sizeof(int));
      }
      param_bind_locals[param_bind_count++] = lb;
    }

    for (int i = 0; i < node->args.count; i++) {
      sv_ast_t *p = node->args.items[i];
      int bind_lb = param_bind_locals[i];

      if (p->type == N_IDENT) {
        if (bind_lb < comp.local_count) {
          emit_op(&comp, OP_GET_ARG);
          emit_u16(&comp, (uint16_t)i);
          int slot = bind_lb - comp.param_locals;
          comp.locals[bind_lb].is_tdz = false;
          if (slot <= 255) { emit_op(&comp, OP_PUT_LOCAL8); emit(&comp, (uint8_t)slot); }
          else { emit_op(&comp, OP_PUT_LOCAL); emit_u16(&comp, (uint16_t)slot); }
        }
      } else if (p->type == N_ASSIGN_PAT) {
        emit_op(&comp, OP_GET_ARG);
        emit_u16(&comp, (uint16_t)i);
        emit_op(&comp, OP_DUP);
        emit_op(&comp, OP_IS_UNDEF);
        int skip = emit_jump(&comp, OP_JMP_FALSE);
        emit_op(&comp, OP_POP);
        compile_expr(&comp, p->right);
        patch_jump(&comp, skip);
        if (p->left && p->left->type == N_IDENT && bind_lb < comp.local_count) {
          int slot = bind_lb - comp.param_locals;
          comp.locals[bind_lb].is_tdz = false;
          if (slot <= 255) { emit_op(&comp, OP_PUT_LOCAL8); emit(&comp, (uint8_t)slot); }
          else { emit_op(&comp, OP_PUT_LOCAL); emit_u16(&comp, (uint16_t)slot); }
        } else if (p->left) {
          compile_destructure_binding(&comp, p->left, SV_VAR_LET);
          emit_op(&comp, OP_POP);
        } else {
          emit_op(&comp, OP_POP);
        }
      } else if (
        p->type == N_ARRAY_PAT || p->type == N_ARRAY ||
        p->type == N_OBJECT_PAT || p->type == N_OBJECT) {
        emit_op(&comp, OP_GET_ARG);
        emit_u16(&comp, (uint16_t)i);
        compile_destructure_binding(&comp, p, SV_VAR_LET);
        emit_op(&comp, OP_POP);
      } else if (
        p->type == N_REST && p->right && p->right->type == N_IDENT &&
        bind_lb < comp.local_count) {
        emit_op(&comp, OP_REST);
        emit_u16(&comp, (uint16_t)i);
        int slot = bind_lb - comp.param_locals;
        comp.locals[bind_lb].is_tdz = false;
        if (slot <= 255) { emit_op(&comp, OP_PUT_LOCAL8); emit(&comp, (uint8_t)slot); }
        else { emit_op(&comp, OP_PUT_LOCAL); emit_u16(&comp, (uint16_t)slot); }
      } else if (p->type == N_REST && p->right) {
        emit_op(&comp, OP_REST);
        emit_u16(&comp, (uint16_t)i);
        compile_destructure_binding(&comp, p->right, SV_VAR_LET);
        emit_op(&comp, OP_POP);
      }
    }

    free(param_bind_locals);
    if (node->body) {
      if (node->body->type == N_BLOCK) {
        if (!repl_top) {
          for (int i = 0; i < node->body->args.count; i++)
            hoist_var_decls(&comp, node->body->args.items[i]);
          hoist_lexical_decls(&comp, &node->body->args);
        }
        hoist_func_decls(&comp, &node->body->args);
      } else if (!repl_top) {
        hoist_var_decls(&comp, node->body);
      }
    }
  }

  if (!comp.is_arrow && has_implicit_arguments_obj(&comp) &&
      (node->flags & FN_USES_ARGS)) {
    comp.strict_args_local = add_local(&comp, "", 0, false, comp.scope_depth);
    emit_op(&comp, OP_SPECIAL_OBJ);
    emit(&comp, 0);
    emit_put_local(&comp, comp.strict_args_local);
  }

  if (!comp.is_arrow && comp.enclosing) {
    static const char nt_name[] = "\x01new.target";
    comp.new_target_local = add_local(&comp, nt_name, sizeof(nt_name) - 1, false, comp.scope_depth);
    emit_op(&comp, OP_SPECIAL_OBJ);
    emit(&comp, 1);
    emit_put_local(&comp, comp.new_target_local);
  }

  if (!comp.is_arrow && comp.enclosing && (node->flags & (FN_METHOD | FN_GETTER | FN_SETTER | FN_STATIC))) {
    static const char sv_name[] = "\x01super";
    comp.super_local = add_local(&comp, sv_name, sizeof(sv_name) - 1, false, comp.scope_depth);
    emit_op(&comp, OP_SPECIAL_OBJ);
    emit(&comp, 2);
    emit_put_local(&comp, comp.super_local);
  }

  if (enclosing->field_init_count > 0) {
    emit_field_inits(&comp, enclosing->field_inits, enclosing->field_init_count);
  }

  if (node->body) {
    if (node->body->type == N_BLOCK) {
      int last_expr_idx = -1;
      if (has_completion_value(&comp) && node->body->args.count > 0) {
        sv_ast_t *last = node->body->args.items[node->body->args.count - 1];
        if (sv_ast_can_be_expression_statement(last))
          last_expr_idx = node->body->args.count - 1;
      }

      for (int i = 0; i < node->body->args.count; i++) {
        sv_ast_t *stmt = node->body->args.items[i];
        if (i == last_expr_idx) {
          compile_expr(&comp, stmt);
          emit_close_upvals(&comp);
          emit_op(&comp, OP_RETURN);
        } else {
          compile_stmt(&comp, stmt);
        }
      }
    } else compile_tail_return_expr(&comp, node->body);
  }

  for (int i = 0; i < comp.deferred_export_count; i++) {
    sv_deferred_export_t *e = &comp.deferred_exports[i];
    compile_export_emit(&comp, e->name, e->len);
  }
  
  free(comp.deferred_exports);
  emit_close_upvals(&comp);
  emit_op(&comp, OP_RETURN_UNDEF);

  int max_locals = comp.max_local_count - comp.param_locals;
  sv_func_t *func = code_arena_bump(sizeof(sv_func_t));
  memset(func, 0, sizeof(sv_func_t));

  func->code = code_arena_bump((size_t)comp.code_len);
  memcpy(func->code, comp.code, (size_t)comp.code_len);
  func->code_len = comp.code_len;

  if (comp.const_count > 0) {
    func->constants = code_arena_bump((size_t)comp.const_count * sizeof(jsval_t));
    memcpy(func->constants, comp.constants, (size_t)comp.const_count * sizeof(jsval_t));
    func->const_count = comp.const_count;
  }

  if (comp.atom_count > 0) {
    func->atoms = code_arena_bump((size_t)comp.atom_count * sizeof(sv_atom_t));
    memcpy(func->atoms, comp.atoms, (size_t)comp.atom_count * sizeof(sv_atom_t));
    func->atom_count = comp.atom_count;
  }

  if (comp.upvalue_count > 0) {
    func->upval_descs = code_arena_bump(
      (size_t)comp.upvalue_count * sizeof(sv_upval_desc_t));
    memcpy(func->upval_descs, comp.upval_descs,
      (size_t)comp.upvalue_count * sizeof(sv_upval_desc_t));
    func->upvalue_count = comp.upvalue_count;
  }

  if (comp.srcpos_count > 0) {
    func->srcpos = code_arena_bump((size_t)comp.srcpos_count * sizeof(sv_srcpos_t));
    memcpy(func->srcpos, comp.srcpos, (size_t)comp.srcpos_count * sizeof(sv_srcpos_t));
    func->srcpos_count = comp.srcpos_count;
  }

  if (enclosing->source && enclosing->source_len > 0) {
    func->source = enclosing->source;
    func->source_len = (int)enclosing->source_len;
    func->source_start = (int)node->src_off;
    func->source_end   = (node->src_end > node->src_off)
      ? (int)node->src_end : func->source_len;
  }

  func->max_locals = max_locals;
  func->max_stack = max_locals + 64;
  func->param_count = comp.param_count;
  func->is_strict = comp.is_strict;
  func->is_arrow = comp.is_arrow;
  
  func->is_async = !!(node->flags & FN_ASYNC);
  func->is_generator = !!(node->flags & FN_GENERATOR);
  func->is_method = !!(node->flags & FN_METHOD);
  func->is_tla = comp.is_tla;
  func->filename = enclosing->js->filename;
  func->source_line = (int)node->line;
  if (node->str && node->len > 0) {
    char *name = code_arena_bump(node->len + 1);
    memcpy(name, node->str, node->len);
    name[node->len] = '\0';
    func->name = name;
  } else if (enclosing->inferred_name && enclosing->inferred_name_len > 0) {
    char *name = code_arena_bump(enclosing->inferred_name_len + 1);
    memcpy(name, enclosing->inferred_name, enclosing->inferred_name_len);
    name[enclosing->inferred_name_len] = '\0';
    func->name = name;
  }

  free(comp.code);
  free(comp.constants);
  free(comp.atoms);
  free(comp.locals);
  free(comp.upval_descs);
  free(comp.loops);
  free(comp.srcpos);

  return func;
}

static const char *sv_op_names[OP__COUNT] = {
#define OP_DEF(name, size, n_pop, n_push, f) [OP_##name] = #name,
#include "silver/opcode.h"
};

enum {
  SVF_none, SVF_u8, SVF_i8, SVF_u16, SVF_i16, SVF_u32, SVF_i32,
  SVF_atom, SVF_atom_u8, SVF_label, SVF_label8, SVF_loc, SVF_loc8,
  SVF_loc_atom, SVF_arg, SVF_const, SVF_const8, SVF_npop, SVF_var_ref,
};

static const uint8_t sv_op_fmts[OP__COUNT] = {
#define OP_DEF(name, size, n_pop, n_push, f) [OP_##name] = SVF_##f,
#include "silver/opcode.h"
};

void sv_disasm(ant_t *js, sv_func_t *func, const char *label) {
  const char *fname = func->name ? func->name : "";

  fprintf(stderr, "[generated bytecode for function: %s (%p <SharedFunctionInfo %s>)]\n",
    fname, (void *)func, fname);
  fprintf(stderr, "Bytecode length: %d\n", func->code_len);
  fprintf(stderr, "Parameter count %d\n", func->param_count);
  fprintf(stderr, "Register count %d\n", func->max_locals);
  fprintf(stderr, "Frame size %d\n", func->max_locals * (int)sizeof(jsval_t));

  int pc = 0;
  while (pc < func->code_len) {
    uint8_t op = func->code[pc];
    const char *name = (op < OP__COUNT) ? sv_op_names[op] : "???";
    uint8_t size = (op < OP__COUNT) ? sv_op_size[op] : 1;
    uint8_t fmt = (op < OP__COUNT) ? sv_op_fmts[op] : SVF_none;

    uint32_t line, col;
    if (sv_lookup_srcpos(func, pc, &line, &col))
      fprintf(stderr, "%5u S> ", line);
    else
      fprintf(stderr, "         ");

    fprintf(stderr, "%p @ %4d : ", (void *)(func->code + pc), pc);

    char hex[32];
    int hlen = 0;
    for (int i = 0; i < size && i < 8; i++)
      hlen += snprintf(hex + hlen, sizeof(hex) - hlen, "%02x ", func->code[pc + i]);
      
    fprintf(stderr, "%-18s", hex);
    fprintf(stderr, "%s", name ? name : "???");

    switch (fmt) {
    case SVF_u8:
      fprintf(stderr, " [%d]", func->code[pc + 1]);
      break;
    case SVF_i8:
      fprintf(stderr, " [%d]", (int8_t)func->code[pc + 1]);
      break;
    case SVF_u16:
      fprintf(stderr, " [%d]", sv_get_u16(func->code + pc + 1));
      break;
    case SVF_i16:
      fprintf(stderr, " [%d]", (int16_t)sv_get_u16(func->code + pc + 1));
      break;
    case SVF_u32:
      fprintf(stderr, " [%d]", (int)sv_get_u32(func->code + pc + 1));
      break;
    case SVF_i32:
      fprintf(stderr, " [%d]", (int32_t)sv_get_u32(func->code + pc + 1));
      break;
    case SVF_atom: {
      uint32_t idx = sv_get_u32(func->code + pc + 1);
      if (idx < (uint32_t)func->atom_count)
        fprintf(stderr, " [%.*s]", (int)func->atoms[idx].len, func->atoms[idx].str);
      else
        fprintf(stderr, " a%u", idx);
      break;
    }
    case SVF_atom_u8: {
      uint32_t idx = sv_get_u32(func->code + pc + 1);
      uint8_t extra = func->code[pc + 5];
      if (idx < (uint32_t)func->atom_count)
        fprintf(stderr, " [%.*s], [%d]", (int)func->atoms[idx].len, func->atoms[idx].str, extra);
      else
        fprintf(stderr, " a%u, [%d]", idx, extra);
      break;
    }
    case SVF_label: {
      int32_t off = (int32_t)sv_get_u32(func->code + pc + 1);
      fprintf(stderr, " [%d] (%d)", off, pc + size + off);
      break;
    }
    case SVF_label8: {
      int8_t off = (int8_t)func->code[pc + 1];
      fprintf(stderr, " [%d] (%d)", off, pc + size + off);
      break;
    }
    case SVF_loc:
      fprintf(stderr, " r%d", sv_get_u16(func->code + pc + 1));
      break;
    case SVF_loc8:
      fprintf(stderr, " r%d", func->code[pc + 1]);
      break;
    case SVF_loc_atom: {
      uint16_t slot = sv_get_u16(func->code + pc + 1);
      uint32_t aidx = sv_get_u32(func->code + pc + 3);
      fprintf(stderr, " r%d", slot);
      if (aidx < (uint32_t)func->atom_count)
        fprintf(stderr, ", [%.*s]", (int)func->atoms[aidx].len, func->atoms[aidx].str);
      else
        fprintf(stderr, ", a%u", aidx);
      break;
    }
    case SVF_arg:
      fprintf(stderr, " a%d", sv_get_u16(func->code + pc + 1));
      break;
    case SVF_const: {
      uint32_t idx = sv_get_u32(func->code + pc + 1);
      fprintf(stderr, " [%u]", idx);
      break;
    }
    case SVF_const8:
      fprintf(stderr, " [%d]", func->code[pc + 1]);
      break;
    case SVF_npop:
      fprintf(stderr, " %d", sv_get_u16(func->code + pc + 1));
      break;
    case SVF_var_ref:
      fprintf(stderr, " [%d]", sv_get_u16(func->code + pc + 1));
      break;
    default:
      break;
    }

    fprintf(stderr, "\n");
    pc += size;
  }

  fprintf(stderr, "Constant pool (size = %d)\n", func->const_count);
  for (int i = 0; i < func->const_count; i++) {
    jsval_t v = func->constants[i];
    uint8_t t = vtype(v);
    if (t == T_STR) {
      jsoff_t slen;
      jsoff_t soff = vstr(js, v, &slen);
      fprintf(stderr, "           %d: <String[%d]: #%.*s>\n", i, (int)slen, (int)slen, &js->mem[soff]);
    } else if (t == T_NUM) {
      fprintf(stderr, "           %d: <Number [%g]>\n", i, tod(v));
    } else if (t == T_CFUNC) {
      sv_func_t *child = (sv_func_t *)(uintptr_t)vdata(v);
      const char *cname = child->name ? child->name : "";
      fprintf(stderr, "           %d: <SharedFunctionInfo %s>\n", i, cname);
    } else fprintf(stderr, "           %d: <Unknown type=%d>\n", i, t);
  }

  if (func->atom_count > 0) {
    fprintf(stderr, "Atom table (size = %d)\n", func->atom_count);
    for (int i = 0; i < func->atom_count; i++)
      fprintf(stderr, "           %d: <String[%d]: #%.*s>\n",
        i, (int)func->atoms[i].len, (int)func->atoms[i].len, func->atoms[i].str);
  }

  fprintf(stderr, "Handler Table (size = 0)\n");
  fprintf(stderr, "Source Position Table (size = %d)\n", func->srcpos_count);
  fprintf(stderr, "\n");

  for (int i = 0; i < func->const_count; i++) {
    if (vtype(func->constants[i]) == T_CFUNC) {
      sv_func_t *child = (sv_func_t *)(uintptr_t)vdata(func->constants[i]);
      char child_label[256];
      snprintf(child_label, sizeof(child_label), "%s/closure[%d]", label, i);
      sv_disasm(js, child, child_label);
    }
  }
}

sv_func_t *sv_compile(ant_t *js, sv_ast_t *program, sv_compile_mode_t mode, const char *source, jsoff_t source_len) {
  if (!program || program->type != N_PROGRAM) return NULL;
  
  static const char *k_top_name_script = "<script>";
  static const char *k_top_name_module = "<module>";
  static const char *k_top_name_eval   = "<eval>";
  static const char *k_top_name_repl   = "<repl>";
  const        char *top_name          = k_top_name_script;
  
  switch (mode) {
    case SV_COMPILE_MODULE: top_name = k_top_name_module; break;
    case SV_COMPILE_EVAL:   top_name = k_top_name_eval; break;
    case SV_COMPILE_REPL:   top_name = k_top_name_repl; break;
    case SV_COMPILE_SCRIPT:
    default:                top_name = k_top_name_script; break;
  }

  sv_ast_t top_fn;
  memset(&top_fn, 0, sizeof(top_fn));
  
  top_fn.type = N_FUNC;
  top_fn.line = 1;
  top_fn.str = top_name;
  top_fn.len = (uint32_t)strlen(top_name);
  top_fn.src_off = 0;
  top_fn.src_end = (source_len > 0) ? (uint32_t)source_len : 0;
  top_fn.body = sv_ast_new(N_BLOCK);
  top_fn.body->args = program->args;

  sv_compiler_t root = {0};
  root.js = js;
  root.source = pin_source_text(source, source_len);
  root.source_len = source_len;
  root.mode = mode;
  root.is_strict = ((program->flags & FN_PARSE_STRICT) != 0);

  sv_func_t *func = compile_function_body(&root, &top_fn, mode);
  if (js->thrown_exists || !func) return NULL;
  return func;
}

sv_func_t *sv_compile_function(ant_t *js, const char *source, size_t len, bool is_async) {
  const char *prefix = is_async ? "(async function" : "(function";
  size_t prefix_len = strlen(prefix);
  size_t wrapped_len = prefix_len + len + 1;

  char *wrapped = malloc(wrapped_len + 1);
  if (!wrapped) return NULL;

  memcpy(wrapped, prefix, prefix_len);
  memcpy(wrapped + prefix_len, source, len);
  wrapped[prefix_len + len] = ')';
  wrapped[wrapped_len] = '\0';
  
  bool parse_strict = sv_vm_is_strict(js->vm);
  sv_ast_t *program = sv_parse(js, wrapped, (jsoff_t)wrapped_len, parse_strict);
  if (!program) { free(wrapped); return NULL; }

  sv_ast_t *func_node = NULL;
  if (program->args.count > 0) {
    sv_ast_t *stmt = program->args.items[0];
    if (stmt && stmt->type == N_FUNC)
      func_node = stmt;
    else if (stmt && stmt->left && stmt->left->type == N_FUNC)
      func_node = stmt->left;
  }

  if (!func_node) { free(wrapped); return NULL; }

  sv_compiler_t root = {0};
  root.js = js;
  root.source = pin_source_text(wrapped, (jsoff_t)wrapped_len);
  root.source_len = (jsoff_t)wrapped_len;
  root.mode = SV_COMPILE_SCRIPT;
  root.is_strict = (program->flags & FN_PARSE_STRICT) != 0;

  sv_func_t *func = compile_function_body(&root, func_node, SV_COMPILE_SCRIPT);
  free(wrapped);

  if (js->thrown_exists || !func) return NULL;
  return func;
}

sv_func_t *sv_compile_function_parts(
  ant_t *js,
  const char *params,
  size_t params_len,
  const char *body,
  size_t body_len,
  bool is_async
) {
  size_t source_len = params_len + body_len + 3;
  char *source = malloc(source_len + 1);
  if (!source) return NULL;

  size_t pos = 0;
  source[pos++] = '(';
  if (params_len > 0 && params) {
    memcpy(source + pos, params, params_len);
    pos += params_len;
  }
  source[pos++] = ')';
  source[pos++] = '{';
  if (body_len > 0 && body) {
    memcpy(source + pos, body, body_len);
    pos += body_len;
  }
  source[pos++] = '}';
  source[pos] = '\0';

  sv_func_t *func = sv_compile_function(js, source, pos, is_async);
  free(source);
  return func;
}
