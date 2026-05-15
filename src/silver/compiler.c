#include "silver/ast.h"
#include "silver/compile_ctx.h"
#include "silver/engine.h"
#include "silver/compiler.h"
#include "silver/directives.h"

#include "internal.h"
#include "debug.h"
#include "tokens.h"
#include "runtime.h"
#include "utils.h"
#include "ops/coercion.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum {
  SV_ITER_HINT_GENERIC = 0,
  SV_ITER_HINT_ARRAY = 1,
  SV_ITER_HINT_STRING = 4,
};

static const char *pin_source_text(const char *source, ant_offset_t source_len) {
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

static void compile_receiver_property_get(sv_compiler_t *c, sv_ast_t *node);
static void compile_truthy_test_expr(sv_compiler_t *c, sv_ast_t *node);

static void emit_srcpos(sv_compiler_t *c, sv_ast_t *node) {
  if (!node) return;
  const char *code = c->source;
  ant_offset_t clen = c->source_len;
  if (!code || clen <= 0) return;

  uint32_t off = node->src_off;
  if (off > clen) off = (uint32_t)clen;
  uint32_t end = node->src_end;
  if (end > clen) end = (uint32_t)clen;
  if (end < off) end = off;
  if (end == off && off < (uint32_t)clen) end = off + 1;
  if (end == off) return;

  if (c->srcpos_count > 0 && c->last_srcpos_off == off && c->last_srcpos_end == end) return;

  uint32_t line, col;
  if (c->line_table) sv_compile_ctx_line_table_lookup(c->line_table, off, &line, &col);
  else if (c->srcpos_count > 0 && off >= c->last_srcpos_off) {
    line = c->srcpos[c->srcpos_count - 1].line;
    col = c->srcpos[c->srcpos_count - 1].col;
    for (uint32_t i = c->last_srcpos_off; i < off; i++) {
      if (code[i] == '\n') { line++; col = 1; }
      else col++;
    }
  } else {
    line = 1; col = 1;
    for (uint32_t i = 0; i < off; i++) {
      if (code[i] == '\n') { line++; col = 1; }
      else col++;
    }
  }

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

static int add_constant(sv_compiler_t *c, ant_value_t val) {
  if (vtype(val) == T_STR) {
    ant_offset_t slen;
    ant_offset_t off = vstr(c->js, val, &slen);
    const char *sptr = (const char *)(uintptr_t)off;

    const_dedup_entry_t *found = NULL;
    HASH_FIND(hh, c->const_dedup, sptr, (size_t)slen, found);
    if (found) return found->index;

    int idx = c->const_count;
    if (c->const_count >= c->const_cap) {
      c->const_cap = c->const_cap ? c->const_cap * 2 : 16;
      c->constants = realloc(c->constants, (size_t)c->const_cap * sizeof(ant_value_t));
    }
    c->constants[c->const_count++] = val;

    const_dedup_entry_t *entry = malloc(sizeof(const_dedup_entry_t));
    if (entry) {
      entry->str = sptr;
      entry->len = (size_t)slen;
      entry->index = idx;
      HASH_ADD_KEYPTR(hh, c->const_dedup, entry->str, entry->len, entry);
    }
    return idx;
  }

  if (c->const_count >= c->const_cap) {
    c->const_cap = c->const_cap ? c->const_cap * 2 : 16;
    c->constants = realloc(c->constants, (size_t)c->const_cap * sizeof(ant_value_t));
  }
  c->constants[c->const_count] = val;
  return c->const_count++;
}

static void build_gc_const_tables(sv_func_t *func) {
  if (!func || func->const_count <= 0 || !func->constants) return;

  int child_count = 0;
  for (int i = 0; i < func->const_count; i++) {
    if (vtype(func->constants[i]) == T_NTARG) child_count++;
  }

  if (child_count > 0) {
    func->child_funcs = code_arena_bump((size_t)child_count * sizeof(sv_func_t *));
    if (func->child_funcs) {
      int out = 0;
      for (int i = 0; i < func->const_count; i++) {
        if (vtype(func->constants[i]) != T_NTARG) continue;
        func->child_funcs[out++] = (sv_func_t *)(uintptr_t)vdata(func->constants[i]);
      } func->child_func_count = child_count;
    }
  }

  uint8_t *marked_slots = calloc((size_t)func->const_count, sizeof(uint8_t));
  if (!marked_slots) return;

  int slot_count = 0;
  for (int pc = 0; pc < func->code_len;) {
    sv_op_t op = (sv_op_t)func->code[pc];
    int size = (op < OP__COUNT) ? sv_op_size[op] : 0;
    if (size <= 0) break;

    if (op == OP_PUT_CONST) {
      uint32_t idx = sv_get_u32(func->code + pc + 1);
      if (idx < (uint32_t)func->const_count && !marked_slots[idx]) {
        marked_slots[idx] = 1; slot_count++;
      }
    }

    pc += size;
  }

  if (slot_count > 0) {
    func->gc_const_slots = code_arena_bump((size_t)slot_count * sizeof(uint32_t));
    if (func->gc_const_slots) {
      int out = 0;
      for (int i = 0; i < func->const_count; i++) {
        if (!marked_slots[i]) continue;
        func->gc_const_slots[out++] = (uint32_t)i;
      }
      func->gc_const_slot_count = slot_count;
    }
  }

  free(marked_slots);
}

static void emit_constant(sv_compiler_t *c, ant_value_t val) {
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

static inline bool is_template_segment(const sv_ast_t *node) {
  return node && node->type == N_STRING && (node->flags & FN_TEMPLATE_SEGMENT);
}

static inline bool is_invalid_cooked_string(const sv_ast_t *node) {
  return is_template_segment(node) && (node->flags & FN_INVALID_COOKED);
}

static inline ant_value_t ast_string_const(sv_compiler_t *c, const sv_ast_t *node) {
  if (!node || !node->str) return js_mkstr_permanent(c->js, "", 0);
  return js_mkstr_permanent(c->js, node->str, node->len);
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
    emit_constant(c, js_mkstr_permanent(c->js, buf, (size_t)n));
    return;
  }

  if (key->type == N_IDENT) {
    if (is_quoted_ident_key(key))
      emit_constant(c, js_mkstr_permanent(c->js, key->str + 1, key->len - 2));
    else
      emit_constant(c, js_mkstr_permanent(c->js, key->str, key->len));
    return;
  }

  compile_expr(c, key);
}

enum {
  SV_COMP_PRIVATE_FIELD = 0,
  SV_COMP_PRIVATE_METHOD = 1,
  SV_COMP_PRIVATE_GETTER = 3,
  SV_COMP_PRIVATE_SETTER = 4
};

static int local_to_frame_slot(sv_compiler_t *c, int local_idx);
static int add_upvalue(sv_compiler_t *c, uint16_t index, bool is_local, bool is_const);
static void emit_get_local(sv_compiler_t *c, int local_idx);

static inline bool is_private_name_node(const sv_ast_t *node) {
  return node && node->type == N_IDENT && node->str && node->len > 0 && node->str[0] == '#';
}

static sv_private_name_t *private_scope_find_current(
  sv_private_scope_t *scope, const char *name, uint32_t len
) {
  if (!scope || !name) return NULL;
  for (int i = 0; i < scope->count; i++) {
    sv_private_name_t *p = &scope->names[i];
    if (p->len == len && memcmp(p->name, name, len) == 0) return p;
  }
  return NULL;
}

static sv_private_name_t *private_scope_resolve(
  sv_compiler_t *c, const char *name, uint32_t len
) {
  for (sv_private_scope_t *scope = c->private_scope; scope; scope = scope->parent) {
    sv_private_name_t *p = private_scope_find_current(scope, name, len);
    if (p) return p;
  }
  return NULL;
}

static bool private_scope_add(
  sv_compiler_t *c, sv_private_scope_t *scope,
  sv_ast_t *name, uint8_t kind, bool is_static
) {
  if (!is_private_name_node(name)) return true;

  if (name->len == 12 && memcmp(name->str, "#constructor", 12) == 0) {
    js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Classes may not declare private constructor names");
    return false;
  }

  sv_private_name_t *existing = private_scope_find_current(scope, name->str, name->len);
  bool is_accessor = kind == SV_COMP_PRIVATE_GETTER || kind == SV_COMP_PRIVATE_SETTER;
  if (existing) {
    bool existing_accessor = existing->kind == SV_COMP_PRIVATE_GETTER ||
      existing->kind == SV_COMP_PRIVATE_SETTER;
    if (!is_accessor || !existing_accessor || existing->is_static != is_static) {
      js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Duplicate private name '%.*s'", (int)name->len, name->str);
      return false;
    }
    if ((kind == SV_COMP_PRIVATE_GETTER && existing->has_getter) ||
        (kind == SV_COMP_PRIVATE_SETTER && existing->has_setter)) {
      js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Duplicate private accessor '%.*s'", (int)name->len, name->str);
      return false;
    }
    if (kind == SV_COMP_PRIVATE_GETTER) existing->has_getter = true;
    if (kind == SV_COMP_PRIVATE_SETTER) existing->has_setter = true;
    return true;
  }

  if (scope->count >= scope->cap) {
    int new_cap = scope->cap ? scope->cap * 2 : 8;
    sv_private_name_t *new_names = realloc(scope->names, (size_t)new_cap * sizeof(*scope->names));
    if (!new_names) return false;
    scope->names = new_names;
    scope->cap = new_cap;
  }

  sv_private_name_t *p = &scope->names[scope->count++];
  
  uint64_t hash64 = hash_key(name->str, (size_t)name->len);
  uint32_t hash = (uint32_t)(hash64 ^ (hash64 >> 32));
  
  *p = (sv_private_name_t){
    .name = name->str,
    .len = name->len,
    .kind = kind,
    .hash = hash,
    .is_static = is_static,
    .has_getter = kind == SV_COMP_PRIVATE_GETTER,
    .has_setter = kind == SV_COMP_PRIVATE_SETTER,
    .owner = NULL,
    .local = -1
  };
  
  return true;
}

static int resolve_private_upvalue(sv_compiler_t *c, sv_private_name_t *p) {
  if (!c->enclosing || !p || !p->owner || p->local < 0) return -1;

  if (c->enclosing == p->owner) {
    p->owner->locals[p->local].captured = true;
    uint16_t slot = (uint16_t)local_to_frame_slot(p->owner, p->local);
    return add_upvalue(c, slot, true, false);
  }

  int upvalue = resolve_private_upvalue(c->enclosing, p);
  if (upvalue == -1) return -1;
  return add_upvalue(c, (uint16_t)upvalue, false, false);
}

static bool emit_private_token(sv_compiler_t *c, sv_ast_t *name) {
  sv_private_name_t *p = private_scope_resolve(c, name->str, name->len);
  if (!p) {
    js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Private name '%.*s' is not declared", (int)name->len, name->str);
    emit_op(c, OP_UNDEF);
    return false;
  }
  if (!p->owner || p->local < 0) {
    js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Private name '%.*s' is not initialized", (int)name->len, name->str);
    emit_op(c, OP_UNDEF);
    return false;
  }
  if (p->owner == c) {
    emit_get_local(c, p->local);
    return true;
  }
  int upvalue = resolve_private_upvalue(c, p);
  if (upvalue == -1) {
    js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Private name '%.*s' is not in scope", (int)name->len, name->str);
    emit_op(c, OP_UNDEF);
    return false;
  }
  emit_op(c, OP_GET_UPVAL);
  emit_u16(c, (uint16_t)upvalue);
  return true;
}

static int add_atom(sv_compiler_t *c, const char *str, uint32_t len) {
  const char *interned = intern_string(str, (size_t)len);
  const char *stored = interned;
  if (!stored) {
    char *copy = code_arena_bump(len);
    memcpy(copy, str, len);
    stored = copy;
  }

  for (int i = 0; i < c->atom_count; i++) {
    if (c->atoms[i].len == len && c->atoms[i].str == stored)
      return i;
  }
  if (c->atom_count >= c->atom_cap) {
    c->atom_cap = c->atom_cap ? c->atom_cap * 2 : 16;
    c->atoms = realloc(c->atoms, (size_t)c->atom_cap * sizeof(sv_atom_t));
  }
  c->atoms[c->atom_count] = (sv_atom_t){ .str = stored, .len = len };
  return c->atom_count++;
}

static inline bool sv_op_has_ic_slot(sv_op_t op) {
  return op == OP_GET_GLOBAL || op == OP_GET_GLOBAL_UNDEF ||
         op == OP_GET_FIELD || op == OP_GET_FIELD2 || op == OP_PUT_FIELD;
}

static uint16_t alloc_ic_idx(sv_compiler_t *c) {
  if (!c || c->ic_count >= (int)UINT16_MAX) return UINT16_MAX;
  return (uint16_t)c->ic_count++;
}

static void sv_func_init_obj_sites(sv_func_t *func) {
  if (!func || !func->code || func->code_len <= 0) return;

  uint32_t count = 0;
  for (int pc = 0; pc < func->code_len; ) {
    sv_op_t op = (sv_op_t)func->code[pc];
    if (op == OP_OBJECT) count++;
    uint8_t size = (op < OP__COUNT) ? sv_op_size[op] : 1;
    pc += (size > 0) ? size : 1;
  }
  if (count == 0) return;

  func->obj_sites = code_arena_bump((size_t)count * sizeof(sv_obj_site_cache_t));
  memset(func->obj_sites, 0, (size_t)count * sizeof(sv_obj_site_cache_t));
  func->obj_site_count = (uint16_t)count;

  uint32_t idx = 0;
  for (int pc = 0; pc < func->code_len && idx < count; ) {
    sv_op_t op = (sv_op_t)func->code[pc];
    if (op == OP_OBJECT) func->obj_sites[idx++].bc_off = (uint32_t)pc;
    uint8_t size = (op < OP__COUNT) ? sv_op_size[op] : 1;
    pc += (size > 0) ? size : 1;
  }
}

static void emit_atom_idx_op(sv_compiler_t *c, sv_op_t op, uint32_t atom_idx) {
  emit_op(c, op);
  emit_u32(c, atom_idx);
  if (sv_op_has_ic_slot(op))
    emit_u16(c, alloc_ic_idx(c));
}

static void emit_atom_op(sv_compiler_t *c, sv_op_t op, const char *str, uint32_t len) {
  int idx = add_atom(c, str, len);
  emit_atom_idx_op(c, op, (uint32_t)idx);
}

static inline void emit_set_function_name(
  sv_compiler_t *c,
  const char *name, uint32_t len
) {
  if (!name) { name = ""; len = 0; }
  emit_atom_op(c, OP_SET_NAME, name, len);
}

static inline bool node_needs_inferred_function_name(const sv_ast_t *node) {
  return node && 
    (node->type == N_FUNC || node->type == N_CLASS) &&
    (!node->str || node->len == 0);
}

static void compile_expr_with_inferred_name(
  sv_compiler_t *c, sv_ast_t *node,
  const char *name, uint32_t len
) {
  if (!node_needs_inferred_function_name(node) || !name) {
    compile_expr(c, node);
    return;
  }

  const char *saved_name = c->inferred_name;
  uint32_t saved_len = c->inferred_name_len;
  
  c->inferred_name = name;
  c->inferred_name_len = len;
  compile_expr(c, node);
  
  c->inferred_name = saved_name;
  c->inferred_name_len = saved_len;
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

static inline bool is_completion_top_level(const sv_compiler_t *c) {
  return has_completion_value(c) && c->enclosing && !c->enclosing->enclosing;
}

static inline bool has_completion_accumulator(const sv_compiler_t *c) {
  return c && c->completion_local >= 0;
}

static inline bool has_module_import_binding(const sv_compiler_t *c) {
  for (const sv_compiler_t *cur = c; cur; cur = cur->enclosing) {
    if (cur->mode == SV_COMPILE_MODULE) return true;
  }
  return false;
}

static inline bool has_implicit_arguments_obj(const sv_compiler_t *c) {
  return c && !c->is_arrow && c->enclosing;
}

static int resolve_local(sv_compiler_t *c, const char *name, uint32_t len) {
  if (c->local_lookup_heads && c->local_lookup_cap > 0) {
    uint32_t hash = sv_compile_ctx_hash_local_name(name, len);
    int bucket = (int)(hash & (uint32_t)(c->local_lookup_cap - 1));
    for (int i = c->local_lookup_heads[bucket]; i != -1; i = c->locals[i].lookup_next) {
      sv_local_t *loc = &c->locals[i];
      if (
        loc->name_hash == hash &&
        loc->name_len == len &&
        memcmp(loc->name, name, len) == 0
      ) return i;
    }
    return -1;
  }

  for (int i = c->local_count - 1; i >= 0; i--) {
    sv_local_t *loc = &c->locals[i];
    if (loc->name_len == len && memcmp(loc->name, name, len) == 0) return i;
  }
  
  return -1;
}

static int resolve_local_at_depth(sv_compiler_t *c, const char *name, uint32_t len, int depth) {
  for (int i = c->local_count - 1; i >= 0; i--) {
    sv_local_t *loc = &c->locals[i];
    if (
      loc->depth == depth &&
      loc->name_len == len &&
      memcmp(loc->name, name, len) == 0
    ) return i;
  }
  return -1;
}

static int add_local(
  sv_compiler_t *c, const char *name, uint32_t len,
  bool is_const, int depth
) {
  sv_compile_ctx_ensure_local_lookup_capacity(c, c->local_count + 1);
  if (c->local_count >= c->local_cap) {
    c->local_cap = c->local_cap ? c->local_cap * 2 : 16;
    c->locals = realloc(c->locals, (size_t)c->local_cap * sizeof(sv_local_t));
  }
  int idx = c->local_count++;
  if (c->local_count > c->max_local_count)
    c->max_local_count = c->local_count;
  c->locals[idx] = (sv_local_t){
    .name = name, .name_len = len,
    .name_hash = sv_compile_ctx_hash_local_name(name, len),
    .lookup_next = -1,
    .depth = depth, .is_const = is_const, .captured = false,
    .inferred_type = SV_TI_UNKNOWN,
  };
  sv_compile_ctx_local_lookup_insert(c, idx);
  return idx;
}

static int reserve_hidden_locals(sv_compiler_t *c, int count) {
  int base = c->local_count;
  for (int i = 0; i < count; i++)
    add_local(c, "", 0, false, c->scope_depth);
  return base;
}

static inline bool sv_type_is_known(uint8_t t) {
  return t != SV_TI_UNKNOWN;
}

static inline bool sv_type_is_num(uint8_t t) {
  return t == SV_TI_NUM;
}

static void ensure_slot_type_cap(sv_compiler_t *c, int slot) {
  if (slot < 0) return;
  if (slot < c->slot_type_cap) return;
  int new_cap = c->slot_type_cap ? c->slot_type_cap : 16;
  while (new_cap <= slot) new_cap *= 2;
  sv_type_info_t *next = realloc(c->slot_types, (size_t)new_cap * sizeof(sv_type_info_t));
  if (!next) return;
  memset(next + c->slot_type_cap, 0, (size_t)(new_cap - c->slot_type_cap) * sizeof(sv_type_info_t));
  c->slot_types = next;
  c->slot_type_cap = new_cap;
}

static void mark_slot_type(sv_compiler_t *c, int slot, uint8_t type) {
  if (slot < 0) return;
  ensure_slot_type_cap(c, slot);
  if (!c->slot_types || slot >= c->slot_type_cap) return;
  uint8_t old = c->slot_types[slot].type;
  if (!sv_type_is_known(type)) {
    c->slot_types[slot].type = SV_TI_UNKNOWN;
    return;
  }
  if (!sv_type_is_known(old))
    c->slot_types[slot].type = type;
  else if (old != type)
    c->slot_types[slot].type = SV_TI_UNKNOWN;
}

static void set_local_inferred_type(sv_compiler_t *c, int local_idx, uint8_t type) {
  if (local_idx < 0 || local_idx >= c->local_count) return;
  c->locals[local_idx].inferred_type = type;
  if (c->locals[local_idx].depth == -1) return;
  int slot = local_idx - c->param_locals;
  mark_slot_type(c, slot, type);
}

static inline uint8_t get_local_inferred_type(sv_compiler_t *c, int local_idx) {
  if (local_idx < 0 || local_idx >= c->local_count) return SV_TI_UNKNOWN;
  if (c->locals[local_idx].depth == -1) return SV_TI_UNKNOWN;
  if (c->locals[local_idx].is_tdz) return SV_TI_UNKNOWN;
  return c->locals[local_idx].inferred_type;
}

static const char *typeof_name_for_type(uint8_t type) {
  switch (type) {
    case SV_TI_NUM:   return "number";
    case SV_TI_STR:   return "string";
    case SV_TI_BOOL:  return "boolean";
    case SV_TI_UNDEF: return "undefined";
    case SV_TI_ARR:
    case SV_TI_OBJ:
    case SV_TI_NULL:  return "object";
    default:          return NULL;
  }
}

static uint8_t iter_hint_for_type(uint8_t type) {
  switch (type) {
    case SV_TI_ARR: return SV_ITER_HINT_ARRAY;
    case SV_TI_STR: return SV_ITER_HINT_STRING;
    default:        return SV_ITER_HINT_GENERIC;
  }
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
  const char *label, uint32_t label_len,
  bool is_switch
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
    .is_switch = is_switch,
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
    sv_compile_ctx_local_lookup_remove(c, c->local_count - 1);
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
static void emit_put_local_typed(sv_compiler_t *c, int local_idx, uint8_t type);

static inline void emit_get_module_import_binding(sv_compiler_t *c) {
  emit_op(c, OP_SPECIAL_OBJ);
  emit(c, 3);
}

static void emit_get_var(sv_compiler_t *c, const char *name, uint32_t len) {
  bool is_super = is_ident_str(name, len, "super", 5);

  if (is_super && c->super_local >= 0) {
    emit_get_local(c, c->super_local);
    return;
  }
  
  if (is_super && c->is_arrow) {
  int super_upval = resolve_super_upvalue(c);
  if (super_upval != -1) {
    emit_op(c, OP_GET_UPVAL);
    emit_u16(c, (uint16_t)super_upval);
    return;
  }}

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
  
  if (has_module_import_binding(c) && is_ident_str(name, len, "import", 6)) {
    emit_get_module_import_binding(c);
    return;
  }
  
  if (c->with_depth > 0) emit_with_get(c, name, len, WITH_FB_GLOBAL, 0);
  else emit_atom_op(c, OP_GET_GLOBAL, name, len);
}

static void emit_set_var(sv_compiler_t *c, const char *name, uint32_t len, bool keep) {
  int local = resolve_local(c, name, len);
  if (local != -1) {
    if (c->locals[local].is_const) {
      emit_const_assign_error(c, name, len);
      return;
    }
    set_local_inferred_type(c, local, SV_TI_UNKNOWN);

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
  if (has_module_import_binding(c) && is_ident_str(name, len, "import", 6)) {
    emit_const_assign_error(c, name, len);
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

static void emit_put_local_typed(sv_compiler_t *c, int local_idx, uint8_t type) {
  int slot = local_idx - c->param_locals;
  if (slot <= 255) { emit_op(c, OP_PUT_LOCAL8); emit(c, (uint8_t)slot); }
  else { emit_op(c, OP_PUT_LOCAL); emit_u16(c, (uint16_t)slot); }
  set_local_inferred_type(c, local_idx, type);
}

static inline void emit_slot_op(sv_compiler_t *c, sv_op_t op, uint16_t slot) {
  emit_op(c, op);
  emit_u16(c, slot);
}

static void emit_put_local(sv_compiler_t *c, int local_idx) {
  emit_put_local_typed(c, local_idx, SV_TI_UNKNOWN);
}

static void emit_set_completion_from_stack(sv_compiler_t *c) {
  if (has_completion_accumulator(c)) emit_put_local(c, c->completion_local);
  else emit_op(c, OP_POP);
}

static void emit_set_completion_undefined(sv_compiler_t *c) {
  if (!has_completion_accumulator(c)) return;
  emit_op(c, OP_UNDEF);
  emit_put_local(c, c->completion_local);
}

static uint8_t infer_expr_type(sv_compiler_t *c, sv_ast_t *node);

static void emit_get_local(sv_compiler_t *c, int local_idx) {
  int slot = local_idx - c->param_locals;
  if (slot <= 255) { emit_op(c, OP_GET_LOCAL8); emit(c, (uint8_t)slot); }
  else { emit_op(c, OP_GET_LOCAL); emit_u16(c, (uint16_t)slot); }
}

static bool match_self_append_local(
  sv_compiler_t *c, sv_ast_t *node,
  int *out_local_idx, uint16_t *out_slot, sv_ast_t **out_rhs
) {
  if (!c || !node || node->type != N_ASSIGN || !node->left || node->left->type != N_IDENT)
    return false;
  if (c->with_depth > 0) return false;

  int local = resolve_local(c, node->left->str, node->left->len);
  if (local < 0 || c->locals[local].is_const) return false;
  if (c->locals[local].is_tdz) return false;
  if (c->locals[local].depth == -1 && c->strict_args_local >= 0) return false;

  sv_ast_t *rhs = NULL;
  if (node->op == TOK_PLUS_ASSIGN) rhs = node->right;
  else if (
    node->op == TOK_ASSIGN &&
    node->right && node->right->type == N_BINARY && node->right->op == TOK_PLUS &&
    node->right->left && node->right->left->type == N_IDENT &&
    node->right->left->len == node->left->len &&
    memcmp(node->right->left->str, node->left->str, node->left->len) == 0
  ) rhs = node->right->right;

  if (!rhs) return false;
  uint8_t local_type = get_local_inferred_type(c, local);
  uint8_t rhs_type = infer_expr_type(c, rhs);
  if (local_type != SV_TI_STR && rhs_type != SV_TI_STR)
    return false;
    
  if (out_local_idx) *out_local_idx = local;
  if (out_slot) *out_slot = (uint16_t)local_to_frame_slot(c, local);
  if (out_rhs) *out_rhs = rhs;
  
  return true;
}

static bool is_self_append_inplace_safe_ident(sv_compiler_t *c, sv_ast_t *node) {
  if (!c || !node || node->type != N_IDENT) return false;

  if (resolve_local(c, node->str, node->len) != -1) return true;
  if (resolve_upvalue(c, node->str, node->len) != -1) return true;

  if (is_ident_str(node->str, node->len, "arguments", 9)) {
    if (has_implicit_arguments_obj(c)) return true;
    if (c->is_arrow && resolve_arguments_upvalue(c) != -1) return true;
  }

  if (c->is_arrow && is_ident_str(node->str, node->len, "super", 5))
    return resolve_super_upvalue(c) != -1;

  if (has_module_import_binding(c) && is_ident_str(node->str, node->len, "import", 6))
    return true;

  return false;
}

static bool is_self_append_inplace_safe_expr(sv_compiler_t *c, sv_ast_t *node) {
  if (!node) return false;

  switch (node->type) {
    case N_NUMBER:
    case N_STRING:
    case N_BIGINT:
    case N_BOOL:
    case N_NULL:
    case N_UNDEF:
      return true;

    case N_IDENT:
      return is_self_append_inplace_safe_ident(c, node);

    case N_BINARY: return 
      is_self_append_inplace_safe_expr(c, node->left) &&
      is_self_append_inplace_safe_expr(c, node->right);
      
    case N_UNARY:
    case N_TYPEOF:
    case N_VOID:
      return is_self_append_inplace_safe_expr(c, node->left);
      
    default:
      return false;
  }
}

static bool compile_self_append_stmt(sv_compiler_t *c, sv_ast_t *node) {
  int local = -1;
  uint16_t slot = 0;
  sv_ast_t *rhs = NULL;
  if (!match_self_append_local(c, node, &local, &slot, &rhs)) return false;
  if (is_self_append_inplace_safe_expr(c, rhs)) {
    compile_expr(c, rhs);
    emit_slot_op(c, OP_STR_APPEND_LOCAL, slot);
  } else {
    emit_slot_op(c, OP_GET_SLOT_RAW, slot);
    compile_expr(c, rhs);
    emit_slot_op(c, OP_STR_ALC_SNAPSHOT, slot);
  }
  set_local_inferred_type(c, local, SV_TI_UNKNOWN);
  return true;
}


static inline bool is_ident_name(sv_ast_t *node, const char *name) {
  size_t n = strlen(name);
  return node 
    && node->type == N_IDENT && node->len == (uint32_t)n 
    && memcmp(node->str, name, n) == 0;
}

static sv_compiler_t *root_compiler(sv_compiler_t *c) {
  while (c && c->enclosing) c = c->enclosing;
  return c;
}

static bool is_free_name(sv_compiler_t *c, const char *name, uint32_t len) {
  for (sv_compiler_t *cur = c; cur; cur = cur->enclosing) {
    if (resolve_local(cur, name, len) != -1) return false;
  }
  return true;
}

static void mark_module_syntax_seen(sv_compiler_t *c) {
  sv_compiler_t *root = root_compiler(c);
  if (root) root->module_syntax_seen = true;
}

static void request_commonjs_retry(sv_compiler_t *c) {
  sv_compiler_t *root = root_compiler(c);
  if (root && root->commonjs_retry_allowed && !root->module_syntax_seen)
    root->commonjs_retry_requested = true;
}

static bool is_module_exports_member(sv_compiler_t *c, sv_ast_t *node) {
  return node &&
    node->type == N_MEMBER &&
    !(node->flags & 1) &&
    is_ident_name(node->left, "module") &&
    node->right &&
    is_ident_name(node->right, "exports") &&
    is_free_name(c, "module", 6);
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
    case N_ASSIGN:
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
        else if (prop->type == N_REST || prop->type == N_SPREAD)
          hoist_lexical_pattern(c, prop->right, is_const);
      }
      break;
    default:
      break;
  }
}

static void annex_b_collect_funcs(sv_ast_t *node, sv_ast_list_t *out) {
  if (!node) return;
  if (node->type == N_FUNC && node->str && !(node->flags & (FN_ARROW | FN_PAREN))) {
    sv_ast_list_push(out, node);
    return;
  }
  if (node->type == N_IF) {
    annex_b_collect_funcs(node->left, out);
    annex_b_collect_funcs(node->right, out);
  } else if (node->type == N_LABEL) annex_b_collect_funcs(node->body, out);
}

static void annex_b_collect_block_var_funcs(sv_ast_t *node, sv_ast_list_t *out) {
  if (!node || node->type == N_FUNC || node->type == N_CLASS) return;
  if (node->type == N_BLOCK) {
    for (int i = 0; i < node->args.count; i++) {
      sv_ast_t *stmt = node->args.items[i];
      if (!stmt) continue;
      if (stmt->type == N_FUNC && stmt->str && !(stmt->flags & (FN_ARROW | FN_PAREN))) {
        sv_ast_list_push(out, stmt);
        continue;
      }
      annex_b_collect_block_var_funcs(stmt, out);
    }
    return;
  }
  if (node->type == N_IF) {
    annex_b_collect_block_var_funcs(node->left, out);
    annex_b_collect_block_var_funcs(node->right, out);
  } else if (node->type == N_LABEL) {
    annex_b_collect_block_var_funcs(node->body, out);
  } else if (node->type == N_WHILE || node->type == N_DO_WHILE) {
    annex_b_collect_block_var_funcs(node->body, out);
  } else if (node->type == N_FOR || node->type == N_FOR_IN || node->type == N_FOR_OF || node->type == N_FOR_AWAIT_OF) {
    annex_b_collect_block_var_funcs(node->body, out);
  } else if (node->type == N_SWITCH) {
    for (int i = 0; i < node->args.count; i++) {
      sv_ast_t *cas = node->args.items[i];
      for (int j = 0; cas && j < cas->args.count; j++)
        annex_b_collect_block_var_funcs(cas->args.items[j], out);
    }
  } else if (node->type == N_TRY) {
    annex_b_collect_block_var_funcs(node->body, out);
    annex_b_collect_block_var_funcs(node->catch_body, out);
    annex_b_collect_block_var_funcs(node->finally_body, out);
  }
}

static void hoist_lexical_decls(sv_compiler_t *c, sv_ast_list_t *stmts) {
  for (int i = 0; i < stmts->count; i++) {
    sv_ast_t *node = stmts->items[i];
    if (!node) continue;
    sv_ast_t *decl_node = (node->type == N_EXPORT) ? node->left : node;
    if (!decl_node) continue;

    if (decl_node->type == N_VAR && decl_node->var_kind != SV_VAR_VAR) {
      bool is_const = 
        (decl_node->var_kind == SV_VAR_CONST ||
        decl_node->var_kind == SV_VAR_USING ||
        decl_node->var_kind == SV_VAR_AWAIT_USING);
      int lb = c->local_count;
      for (int j = 0; j < decl_node->args.count; j++) {
        sv_ast_t *decl = decl_node->args.items[j];
        if (!decl || decl->type != N_VARDECL || !decl->left) continue;
        hoist_lexical_pattern(c, decl->left, is_const);
      }
      for (int j = lb; j < c->local_count; j++) {
        c->locals[j].is_tdz = true;
        set_local_inferred_type(c, j, SV_TI_UNKNOWN);
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
        set_local_inferred_type(c, c->local_count - 1, SV_TI_UNKNOWN);
        int slot = (c->local_count - 1) - c->param_locals;
        emit_op(c, OP_SET_LOCAL_UNDEF);
        emit_u16(c, (uint16_t)slot);
      }
    } else if (decl_node->type == N_FUNC && decl_node->str && !(decl_node->flags & (FN_ARROW | FN_PAREN))) {
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
    if (!c->is_strict && decl_node->type == N_BLOCK) {
      sv_ast_list_t funcs = {0};
      annex_b_collect_block_var_funcs(decl_node, &funcs);
      for (int j = 0; j < funcs.count; j++) {
        sv_ast_t *fn = funcs.items[j];
        if (resolve_local_at_depth(c, fn->str, fn->len, 0) == -1)
          add_local(c, fn->str, fn->len, false, 0);
      }
    }
  }
}

static void hoist_one_func(sv_compiler_t *c, sv_ast_t *node, bool annex_b_update_var) {
  sv_func_t *fn = compile_function_body(c, node, c->mode);
  if (!fn) return;
  int idx = add_constant(c, mkval(T_NTARG, (uintptr_t)fn));
  emit_op(c, OP_CLOSURE);
  emit_u32(c, (uint32_t)idx);
  emit_set_function_name(c, node->str, node->len);
  int annex_var = annex_b_update_var ? resolve_local_at_depth(c, node->str, node->len, 0) : -1;
  if (annex_var >= 0) emit_op(c, OP_DUP);
  if (is_repl_top_level(c)) {
    emit_atom_op(c, OP_PUT_GLOBAL, node->str, node->len);
  } else {
    int local = resolve_local(c, node->str, node->len);
    emit_put_local(c, local);
  }
  if (annex_var >= 0) emit_put_local(c, annex_var);
}

static void hoist_func_decls(sv_compiler_t *c, sv_ast_list_t *stmts) {
  for (int i = 0; i < stmts->count; i++) {
    sv_ast_t *node = stmts->items[i];
    if (node && node->type == N_EXPORT && node->left)
      node = node->left;
    if (!node) continue;
    if (node->type == N_FUNC && node->str && !(node->flags & (FN_ARROW | FN_PAREN))) {
      hoist_one_func(c, node, !c->is_strict && c->scope_depth > 0);
    }
    if (!c->is_strict && (node->type == N_IF || node->type == N_LABEL)) {
      sv_ast_list_t funcs = {0};
      annex_b_collect_funcs(node, &funcs);
      for (int j = 0; j < funcs.count; j++)
        hoist_one_func(c, funcs.items[j], false);
    }
  }
}

static uint8_t infer_expr_type(sv_compiler_t *c, sv_ast_t *node) {
  if (!node) return SV_TI_UNDEF;

  switch (node->type) {
    case N_NUMBER:   return SV_TI_NUM;
    case N_STRING:   return SV_TI_STR;
    case N_BOOL:     return SV_TI_BOOL;
    case N_NULL:     return SV_TI_NULL;
    case N_UNDEF:    return SV_TI_UNDEF;
    case N_ARRAY:    return SV_TI_ARR;
    case N_OBJECT:   return SV_TI_OBJ;
    case N_TEMPLATE: return SV_TI_STR;
    case N_TYPEOF:   return SV_TI_STR;
    case N_VOID:     return SV_TI_UNDEF;
    case N_NEW:      return SV_TI_OBJ;

    case N_IDENT: {
      int local = resolve_local(c, node->str, node->len);
      if (local >= 0) return get_local_inferred_type(c, local);
      return SV_TI_UNKNOWN;
    }

    case N_SEQUENCE:
      return infer_expr_type(c, node->right);

    case N_TERNARY: {
      uint8_t lt = infer_expr_type(c, node->left);
      uint8_t rhs_type = infer_expr_type(c, node->right);
      if (lt == rhs_type && sv_type_is_known(lt)) return lt;
      return SV_TI_UNKNOWN;
    }

    case N_UNARY: {
      uint8_t rhs_type = infer_expr_type(c, node->right);
      switch (node->op) {
        case TOK_UPLUS:
        case TOK_UMINUS:
          return sv_type_is_num(rhs_type) ? SV_TI_NUM : SV_TI_UNKNOWN;
        case TOK_NOT:
          return SV_TI_BOOL;
        default:
          return SV_TI_UNKNOWN;
      }
    }

    case N_BINARY: {
      uint8_t lt = infer_expr_type(c, node->left);
      uint8_t rhs_type = infer_expr_type(c, node->right);
      switch (node->op) {
        case TOK_PLUS:
          if (lt == SV_TI_NUM && rhs_type == SV_TI_NUM) return SV_TI_NUM;
          if (lt == SV_TI_STR && rhs_type == SV_TI_STR) return SV_TI_STR;
          return SV_TI_UNKNOWN;
        case TOK_MINUS:
        case TOK_MUL:
        case TOK_DIV:
          return (lt == SV_TI_NUM && rhs_type == SV_TI_NUM) ? SV_TI_NUM : SV_TI_UNKNOWN;
        case TOK_LT:
        case TOK_LE:
        case TOK_GT:
        case TOK_GE:
        case TOK_EQ:
        case TOK_NE:
        case TOK_SEQ:
        case TOK_SNE:
        case TOK_INSTANCEOF:
        case TOK_IN:
          return SV_TI_BOOL;
        case TOK_LAND:
        case TOK_LOR:
        case TOK_NULLISH:
          if (lt == rhs_type && sv_type_is_known(lt)) return lt;
          return SV_TI_UNKNOWN;
        default:
          return SV_TI_UNKNOWN;
      }
    }

    default:
      return SV_TI_UNKNOWN;
  }
}

static void compile_yield_star_expr(sv_compiler_t *c, sv_ast_t *node) {
  if (node->right) compile_expr(c, node->right);
  else emit_op(c, OP_UNDEF);

  int base_local = reserve_hidden_locals(c, 4);
  uint16_t base_slot = (uint16_t)(base_local - c->param_locals);

  emit_op(c, OP_YIELD_STAR_INIT);
  emit_u16(c, base_slot);

  emit_op(c, OP_UNDEF);
  emit_op(c, OP_YIELD_STAR_NEXT);
  emit_u16(c, base_slot);
}

void compile_expr(sv_compiler_t *c, sv_ast_t *node) {
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
      ant_value_t bi = js_mkbigint(c->js, digits, dlen, neg);
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
      if (is_private_name_node(node)) {
        js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Private names may only be used as class member names");
        emit_op(c, OP_UNDEF);
        break;
      }
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
      if (node->flags) compile_yield_star_expr(c, node);
      else {
        if (node->right) compile_expr(c, node->right);
        else emit_op(c, OP_UNDEF);
        emit_op(c, OP_YIELD);
      }
      break;

    case N_THROW:
      compile_expr(c, node->right);
      emit_op(c, OP_THROW);
      break;

    case N_TAGGED_TEMPLATE: {
      compile_expr(c, node->left);
      sv_ast_t *tpl = node->right;
      int n = tpl->args.count;
      int n_strings = 0, n_exprs = 0;
      for (int i = 0; i < n; i++) {
        if (is_template_segment(tpl->args.items[i])) n_strings++;
        else n_exprs++;
      }
      int cache_idx = add_constant(c, js_mkundef());
      emit_op(c, OP_CONST);
      emit_u32(c, (uint32_t)cache_idx);
      int skip_jump = emit_jump(c, OP_JMP_TRUE_PEEK);
      emit_op(c, OP_POP);
      for (int i = 0; i < n; i++) {
        sv_ast_t *item = tpl->args.items[i];
        if (!is_template_segment(item)) continue;
        if (is_invalid_cooked_string(item))
          emit_op(c, OP_UNDEF);
        else emit_constant(c, ast_string_const(c, item));
      }
      emit_op(c, OP_ARRAY);
      emit_u16(c, (uint16_t)n_strings);
      for (int i = 0; i < n; i++) {
        sv_ast_t *item = tpl->args.items[i];
        if (!is_template_segment(item)) continue;
        const char *raw = item->aux ? item->aux : item->str;
        uint32_t raw_len = item->aux ? item->aux_len : item->len;
        emit_constant(c, js_mkstr_permanent(c->js, raw ? raw : "", raw_len));
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
      emit_op(c, OP_SET_BRAND);
      emit(c, BRAND_TEMPLATE_OBJECT);
      emit_op(c, OP_DUP);
      emit_op(c, OP_PUT_CONST);
      emit_u32(c, (uint32_t)cache_idx);
      patch_jump(c, skip_jump);
      for (int i = 0; i < n; i++) {
        sv_ast_t *item = tpl->args.items[i];
        if (is_template_segment(item)) continue;
        compile_expr(c, item);
      }
      emit_op(c, OP_CALL);
      emit_u16(c, (uint16_t)(1 + n_exprs));
      break;
    }

    case N_IMPORT:
      compile_expr(c, node->right);
      if (has_module_import_binding(c)) {
        emit_get_module_import_binding(c);
        emit_op(c, OP_SWAP);
        emit_op(c, OP_CALL);
        emit_u16(c, 1);
      } else emit_op(c, OP_IMPORT);
      break;

    case N_REGEXP:
      emit_constant(c, js_mkstr_permanent(c->js, node->str ? node->str : "", node->len));
      emit_constant(c, js_mkstr_permanent(c->js, node->aux ? node->aux : "", node->aux_len));
      emit_op(c, OP_REGEXP);
      break;

    default:
      emit_op(c, OP_UNDEF);
      break;
  }
}

void compile_binary(sv_compiler_t *c, sv_ast_t *node) {
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
    compile_expr(c, node->right);
    emit_private_token(c, node->left);
    emit_op(c, OP_HAS_PRIVATE);
    return;
  }

  uint8_t left_type = SV_TI_UNKNOWN;
  uint8_t right_type = SV_TI_UNKNOWN;
  if (op == TOK_PLUS || op == TOK_MINUS || op == TOK_MUL || op == TOK_DIV) {
    left_type = infer_expr_type(c, node->left);
    right_type = infer_expr_type(c, node->right);
  }

  compile_expr(c, node->left);
  compile_expr(c, node->right);

  switch (op) {
    case TOK_PLUS:
      emit_op(c, (left_type == SV_TI_NUM && right_type == SV_TI_NUM) ? OP_ADD_NUM : OP_ADD);
      break;
    case TOK_MINUS:
      emit_op(c, (left_type == SV_TI_NUM && right_type == SV_TI_NUM) ? OP_SUB_NUM : OP_SUB);
      break;
    case TOK_MUL:
      emit_op(c, (left_type == SV_TI_NUM && right_type == SV_TI_NUM) ? OP_MUL_NUM : OP_MUL);
      break;
    case TOK_DIV:
      emit_op(c, (left_type == SV_TI_NUM && right_type == SV_TI_NUM) ? OP_DIV_NUM : OP_DIV);
      break;
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
    case TOK_INSTANCEOF:
      emit_op(c, OP_INSTANCEOF);
      emit_u16(c, alloc_ic_idx(c));
      break;
    case TOK_IN:         emit_op(c, OP_IN);  break;
    default:             emit_op(c, OP_UNDEF); break;
  }
}

void compile_unary(sv_compiler_t *c, sv_ast_t *node) {
  compile_expr(c, node->right);
  switch (node->op) {
    case TOK_NOT:    emit_op(c, OP_NOT);   break;
    case TOK_TILDA:  emit_op(c, OP_BNOT);  break;
    case TOK_UPLUS:  emit_op(c, OP_UPLUS); break;
    case TOK_UMINUS: emit_op(c, OP_NEG);   break;
    default: break;
  }
}


void compile_update(sv_compiler_t *c, sv_ast_t *node) {
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
  } else if (target->type == N_MEMBER && !(target->flags & 1) && is_private_name_node(target->right)) {
    compile_expr(c, target->left);
    emit_op(c, OP_DUP);
    emit_private_token(c, target->right);
    emit_op(c, OP_GET_PRIVATE);
    if (prefix) {
      emit_op(c, is_inc ? OP_INC : OP_DEC);
      emit_private_token(c, target->right);
      emit_op(c, OP_PUT_PRIVATE);
    } else {
      emit_op(c, is_inc ? OP_POST_INC : OP_POST_DEC);
      emit_op(c, OP_SWAP_UNDER);
      emit_private_token(c, target->right);
      emit_op(c, OP_PUT_PRIVATE);
      emit_op(c, OP_POP);
    }
  } else if (target->type == N_MEMBER && !(target->flags & 1)) {
    compile_expr(c, target->left);
    emit_op(c, OP_DUP);
    int atom = add_atom(c, target->right->str, target->right->len);
    emit_atom_idx_op(c, OP_GET_FIELD, (uint32_t)atom);
    if (prefix) {
      emit_op(c, is_inc ? OP_INC : OP_DEC);
      emit_op(c, OP_INSERT2);
      emit_atom_idx_op(c, OP_PUT_FIELD, (uint32_t)atom);
    } else {
      emit_op(c, is_inc ? OP_POST_INC : OP_POST_DEC);
      emit_op(c, OP_SWAP_UNDER);
      emit_atom_idx_op(c, OP_PUT_FIELD, (uint32_t)atom);
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

void compile_assign(sv_compiler_t *c, sv_ast_t *node) {
  sv_ast_t *target = node->left;
  uint8_t op = node->op;
  int append_local = -1;
  uint16_t append_slot = 0;
  sv_ast_t *append_rhs = NULL;

  if (is_module_exports_member(c, target))
    request_commonjs_retry(c);
  
  bool can_append_builder = match_self_append_local(
    c, node, &append_local, 
    &append_slot, &append_rhs
  );

  if (op == TOK_ASSIGN) {
    if (
      target->type == N_MEMBER && !(target->flags & 1) &&
      target->right && target->right->str &&
      is_ident_str(target->right->str, target->right->len, "exec", 4)
    ) c->regexp_exec_write_seen = true;
    if (
      target->type == N_MEMBER && !(target->flags & 1) &&
      target->right && target->right->str &&
      is_ident_str(target->right->str, target->right->len, "replace", 7)
    ) c->regexp_replace_write_seen = true;

    if (target->type == N_MEMBER && !(target->flags & 1) && is_private_name_node(target->right)) {
      compile_expr(c, target->left);
      compile_expr(c, node->right);
      emit_private_token(c, target->right);
      emit_op(c, OP_PUT_PRIVATE);
      return;
    }

    if (target->type == N_MEMBER && !(target->flags & 1)) {
      int atom = add_atom(c, target->right->str, target->right->len);
      compile_expr(c, target->left);
      compile_expr(c, node->right);
      emit_op(c, OP_INSERT2);
      emit_atom_idx_op(c, OP_PUT_FIELD, (uint32_t)atom);
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
    
    if (can_append_builder) {
      if (is_self_append_inplace_safe_expr(c, append_rhs)) {
        compile_expr(c, append_rhs);
        emit_slot_op(c, OP_STR_APPEND_LOCAL, append_slot);
      } else {
        emit_slot_op(c, OP_GET_SLOT_RAW, append_slot);
        compile_expr(c, append_rhs);
        emit_slot_op(c, OP_STR_ALC_SNAPSHOT, append_slot);
      }
      emit_get_var(c, target->str, target->len);
      set_local_inferred_type(c, append_local, SV_TI_UNKNOWN);
      return;
    }

    compile_expr(c, node->right);
    compile_lhs_set(c, target, true);
    return;
  }

  if (target->type == N_IDENT) {
    int lhs_local = resolve_local(c, target->str, target->len);
    uint8_t lhs_type = (lhs_local >= 0) ? get_local_inferred_type(c, lhs_local) : SV_TI_UNKNOWN;
    uint8_t rhs_type = infer_expr_type(c, node->right);

    if (can_append_builder) {
      if (is_self_append_inplace_safe_expr(c, append_rhs)) {
        compile_expr(c, append_rhs);
        emit_slot_op(c, OP_STR_APPEND_LOCAL, append_slot);
      } else {
        emit_slot_op(c, OP_GET_SLOT_RAW, append_slot);
        compile_expr(c, append_rhs);
        emit_slot_op(c, OP_STR_ALC_SNAPSHOT, append_slot);
      }
      emit_get_var(c, target->str, target->len);
      set_local_inferred_type(c, append_local, SV_TI_UNKNOWN);
      return;
    }

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
          set_local_inferred_type(c, c->param_locals + slot, SV_TI_UNKNOWN);
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
      case TOK_PLUS_ASSIGN:
        emit_op(c, (lhs_type == SV_TI_NUM && rhs_type == SV_TI_NUM) ? OP_ADD_NUM : OP_ADD);
        break;
      case TOK_MINUS_ASSIGN:
        emit_op(c, (lhs_type == SV_TI_NUM && rhs_type == SV_TI_NUM) ? OP_SUB_NUM : OP_SUB);
        break;
      case TOK_MUL_ASSIGN:
        emit_op(c, (lhs_type == SV_TI_NUM && rhs_type == SV_TI_NUM) ? OP_MUL_NUM : OP_MUL);
        break;
      case TOK_DIV_ASSIGN:
        emit_op(c, (lhs_type == SV_TI_NUM && rhs_type == SV_TI_NUM) ? OP_DIV_NUM : OP_DIV);
        break;
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
  } else if (target->type == N_MEMBER && !(target->flags & 1) && is_private_name_node(target->right)) {
    if (op == TOK_LOR_ASSIGN || op == TOK_LAND_ASSIGN ||
        op == TOK_NULLISH_ASSIGN) {
      compile_expr(c, target->left);
      emit_op(c, OP_DUP);
      emit_private_token(c, target->right);
      emit_op(c, OP_GET_PRIVATE);
      int skip = emit_jump(c,
        op == TOK_LOR_ASSIGN ? OP_JMP_TRUE_PEEK :
        op == TOK_LAND_ASSIGN ? OP_JMP_FALSE_PEEK : OP_JMP_NOT_NULLISH);
      emit_op(c, OP_POP);
      compile_expr(c, node->right);
      emit_private_token(c, target->right);
      emit_op(c, OP_PUT_PRIVATE);
      int end = emit_jump(c, OP_JMP);
      patch_jump(c, skip);
      emit_op(c, OP_NIP);
      patch_jump(c, end);
      return;
    }

    compile_expr(c, target->left);
    emit_op(c, OP_DUP);
    emit_private_token(c, target->right);
    emit_op(c, OP_GET_PRIVATE);
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
    emit_private_token(c, target->right);
    emit_op(c, OP_PUT_PRIVATE);
  } else if (target->type == N_MEMBER && !(target->flags & 1)) {
    int atom = add_atom(c, target->right->str, target->right->len);

    if (op == TOK_LOR_ASSIGN || op == TOK_LAND_ASSIGN ||
        op == TOK_NULLISH_ASSIGN) {
      compile_expr(c, target->left);
      emit_op(c, OP_DUP);
      emit_atom_idx_op(c, OP_GET_FIELD, (uint32_t)atom);
      int skip = emit_jump(c,
        op == TOK_LOR_ASSIGN ? OP_JMP_TRUE_PEEK :
        op == TOK_LAND_ASSIGN ? OP_JMP_FALSE_PEEK : OP_JMP_NOT_NULLISH);
      emit_op(c, OP_POP);
      compile_expr(c, node->right);
      emit_op(c, OP_INSERT2);
      emit_atom_idx_op(c, OP_PUT_FIELD, (uint32_t)atom);
      int end = emit_jump(c, OP_JMP);
      patch_jump(c, skip);
      emit_op(c, OP_NIP);
      patch_jump(c, end);
      return;
    }

    compile_expr(c, target->left);
    emit_op(c, OP_DUP);
    emit_atom_idx_op(c, OP_GET_FIELD, (uint32_t)atom);
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
    emit_atom_idx_op(c, OP_PUT_FIELD, (uint32_t)atom);
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

void compile_lhs_set(sv_compiler_t *c, sv_ast_t *target, bool keep) {
  if (target->type == N_IDENT) {
    emit_set_var(c, target->str, target->len, keep);
  } else if (target->type == N_MEMBER && !(target->flags & 1) && is_private_name_node(target->right)) {
    (void)keep;
    compile_expr(c, target->left);
    emit_op(c, OP_SWAP);
    emit_private_token(c, target->right);
    emit_op(c, OP_PUT_PRIVATE);
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

void compile_ternary(sv_compiler_t *c, sv_ast_t *node) {
  compile_truthy_test_expr(c, node->cond);
  int else_jump = emit_jump(c, OP_JMP_FALSE);
  compile_expr(c, node->left);
  int end_jump = emit_jump(c, OP_JMP);
  patch_jump(c, else_jump);
  compile_expr(c, node->right);
  patch_jump(c, end_jump);
}

void compile_typeof(sv_compiler_t *c, sv_ast_t *node) {
  sv_ast_t *arg = node->right;
  if (arg->type == N_IDENT) {
    int local = resolve_local(c, arg->str, arg->len);
    if (local != -1) {
      uint8_t inferred = get_local_inferred_type(c, local);
      const char *known = typeof_name_for_type(inferred);
      if (known && c->with_depth == 0) {
        emit_constant(c, js_mkstr_permanent(c->js, known, strlen(known)));
        return;
      }
      emit_get_var(c, arg->str, arg->len);
    } else {
      int upval = resolve_upvalue(c, arg->str, arg->len);
      if (upval != -1) {
        if (c->with_depth > 0) {
          emit_with_get(c, arg->str, arg->len, WITH_FB_UPVAL, (uint16_t)upval);
        } else {
          emit_op(c, OP_GET_UPVAL);
          emit_u16(c, (uint16_t)upval);
        }
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
      } else if (c->with_depth > 0) emit_with_get(c, arg->str, arg->len, WITH_FB_GLOBAL_UNDEF, 0);
      else emit_atom_op(c, OP_GET_GLOBAL_UNDEF, arg->str, arg->len);
    }
  } else compile_expr(c, arg);
  emit_op(c, OP_TYPEOF);
}

static bool sv_node_has_optional_base(sv_ast_t *n) {
  while (n) {
    if (n->type == N_OPTIONAL) return true;
    if (n->type == N_MEMBER || n->type == N_CALL) n = n->left;
    else break;
  }
  return false;
}

static void compile_delete_optional(sv_compiler_t *c, sv_ast_t *arg) {
  compile_expr(c, arg->left);
  int ok_jump = emit_jump(c, OP_JMP_NOT_NULLISH);
  emit_op(c, OP_POP);
  emit_op(c, OP_TRUE);
  int end_jump = emit_jump(c, OP_JMP);
  patch_jump(c, ok_jump);
  if (arg->flags & 1) {
    compile_expr(c, arg->right);
  } else {
    ant_value_t key = js_mkstr_permanent(c->js, arg->right->str, arg->right->len);
    emit_constant(c, key);
  }
  emit_op(c, OP_DELETE);
  patch_jump(c, end_jump);
}

void compile_delete(sv_compiler_t *c, sv_ast_t *node) {
  sv_ast_t *arg = node->right;
  if ((arg->type == N_MEMBER || arg->type == N_OPTIONAL) &&
      arg->right && is_private_name_node(arg->right)) {
    js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Cannot delete private fields");
    emit_op(c, OP_TRUE);
    return;
  }
  if (arg->type == N_OPTIONAL) {
    compile_delete_optional(c, arg);
  } else if (arg->type == N_MEMBER && sv_node_has_optional_base(arg->left)) {
    compile_delete_optional(c, arg);
  } else if (arg->type == N_MEMBER && !(arg->flags & 1)) {
    compile_expr(c, arg->left);
    ant_value_t key = js_mkstr_permanent(c->js, arg->right->str, arg->right->len);
    emit_constant(c, key);
    emit_op(c, OP_DELETE);
  } else if (arg->type == N_MEMBER && (arg->flags & 1)) {
    compile_expr(c, arg->left);
    compile_expr(c, arg->right);
    emit_op(c, OP_DELETE);
  } else if (arg->type == N_IDENT) {
    emit_atom_op(c, c->with_depth > 0 ? OP_WITH_DEL_VAR : OP_DELETE_VAR, arg->str, arg->len);
  } else {
    compile_expr(c, arg);
    emit_op(c, OP_POP);
    emit_op(c, OP_TRUE);
  }
}

void compile_template(sv_compiler_t *c, sv_ast_t *node) {
  int n = node->args.count;
  if (n == 0) {
    emit_constant(c, js_mkstr_permanent(c->js, "", 0));
    return;
  }
  for (int i = 0; i < n; i++) {
    sv_ast_t *item = node->args.items[i];
    if (is_invalid_cooked_string(item)) {
      static const char msg[] = "Invalid or unexpected token";
      int atom = add_atom(c, msg, sizeof(msg) - 1);
      emit_op(c, OP_THROW_ERROR);
      emit_u32(c, (uint32_t)atom);
      emit(c, (uint8_t)JS_ERR_SYNTAX);
      return;
    }
  }
  compile_expr(c, node->args.items[0]);
  if (!is_template_segment(node->args.items[0]))
    emit_op(c, OP_TO_PROPKEY);
  for (int i = 1; i < n; i++) {
    compile_expr(c, node->args.items[i]);
    if (!is_template_segment(node->args.items[i]))
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
  SV_CALL_SUPER = 2,
} sv_call_kind_t;

static inline bool sv_call_kind_has_receiver(sv_call_kind_t kind) {
  return kind == SV_CALL_METHOD || kind == SV_CALL_SUPER;
}

static void compile_receiver_property_get(sv_compiler_t *c, sv_ast_t *node) {
  emit_op(c, OP_DUP);
  if (node->flags & 1) {
    compile_expr(c, node->right);
    emit_op(c, OP_GET_ELEM);
  } else if (is_private_name_node(node->right)) {
    emit_private_token(c, node->right);
    emit_op(c, OP_GET_PRIVATE);
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
    if (sv_call_kind_has_receiver(kind)) emit_op(c, OP_SWAP);
    else emit_op(c, OP_GLOBAL);
    compile_call_args_array(c, node);
    emit_op(c, kind == SV_CALL_SUPER ? OP_SUPER_APPLY : OP_APPLY);
    emit_u16(c, 1);
    return;
  }

  for (int i = 0; i < argc; i++)
    compile_expr(c, node->args.items[i]);
  emit_op(c, sv_call_kind_has_receiver(kind) ? OP_CALL_METHOD : OP_CALL);
  emit_u16(c, (uint16_t)argc);
}

static sv_call_kind_t compile_call_setup_non_optional(sv_compiler_t *c, sv_ast_t *callee) {
  if (is_ident_name(callee, "super")) {
    emit_op(c, OP_THIS);
    emit_get_var(c, "super", 5);
    return SV_CALL_SUPER;
  }

  if (callee->type == N_MEMBER && is_ident_name(callee->left, "super")) {
    if (!(callee->flags & 1) && is_private_name_node(callee->right)) {
      js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Cannot access private member through super");
      emit_op(c, OP_UNDEF);
      return SV_CALL_DIRECT;
    }
    emit_op(c, OP_THIS);
    emit_op(c, OP_THIS);
    emit_get_var(c, "super", 5);
    if (callee->flags & 1)
      compile_expr(c, callee->right);
    else
      emit_constant(c, js_mkstr_permanent(c->js, callee->right->str, callee->right->len));
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

static bool compile_call_is_proto_intrinsic(
  sv_compiler_t *c, sv_ast_t *node, bool has_spread
) {
  if (!node || has_spread || node->args.count != 1) return false;
  sv_ast_t *callee = node->left;
  if (!callee || callee->type != N_MEMBER) return false;
  if ((callee->flags & 1) || !callee->right || !callee->right->str) return false;
  if (is_ident_name(callee->left, "super")) return false;
  if (!is_ident_str(callee->right->str, callee->right->len, "isPrototypeOf", 13))
    return false;

  compile_expr(c, callee->left);
  compile_receiver_property_get(c, callee);
  compile_expr(c, node->args.items[0]);
  emit_op(c, OP_CALL_IS_PROTO);
  emit_u16(c, alloc_ic_idx(c));
  return true;
}

static bool compile_call_array_includes_intrinsic(
  sv_compiler_t *c, sv_ast_t *node, bool has_spread
) {
  if (!node || has_spread || node->args.count > UINT16_MAX) return false;
  sv_ast_t *callee = node->left;
  
  if (!callee || callee->type != N_MEMBER) return false;
  if ((callee->flags & 1) || !callee->right || !callee->right->str) return false;
  if (is_ident_name(callee->left, "super")) return false;
  if (!is_ident_str(callee->right->str, callee->right->len, "includes", 8))
    return false;

  compile_expr(c, callee->left);
  compile_receiver_property_get(c, callee);
  for (int i = 0; i < node->args.count; i++)
    compile_expr(c, node->args.items[i]);
  emit_op(c, OP_CALL_ARRAY_INCLUDES);
  emit_u16(c, (uint16_t)node->args.count);
  
  return true;
}

static bool regexp_literal_exec_arg_is_simple(sv_compiler_t *c, sv_ast_t *node) {
  if (!node) return false;
  switch (node->type) {
    case N_STRING:
    case N_NUMBER:
    case N_BOOL:
    case N_NULL:
    case N_UNDEF:
    case N_THIS:
      return true;
    case N_IDENT:
      return resolve_local(c, node->str, node->len) >= 0 ||
             resolve_upvalue(c, node->str, node->len) >= 0;
    default:
      return false;
  }
}

static bool compile_regexp_literal_exec_intrinsic(
  sv_compiler_t *c, sv_ast_t *node, bool has_spread
) {
  if (!node || has_spread || node->args.count != 1) return false;
  if (c->regexp_exec_write_seen) return false;

  sv_ast_t *callee = node->left;
  if (!callee || callee->type != N_MEMBER) return false;
  if ((callee->flags & 1) || !callee->right || !callee->right->str) return false;
  if (!is_ident_str(callee->right->str, callee->right->len, "exec", 4)) return false;
  if (!callee->left || callee->left->type != N_REGEXP) return false;
  if (!regexp_literal_exec_arg_is_simple(c, node->args.items[0])) return false;

  sv_ast_t *rx = callee->left;
  emit_constant(c, js_mkstr_permanent(c->js, rx->str ? rx->str : "", rx->len));
  emit_constant(c, js_mkstr_permanent(c->js, rx->aux ? rx->aux : "", rx->aux_len));
  compile_expr(c, node->args.items[0]);
  emit_op(c, OP_RE_LITERAL_EXEC);
  return true;
}

static bool compile_string_regexp_literal_replace_intrinsic(
  sv_compiler_t *c, sv_ast_t *node, bool has_spread
) {
  if (!node || has_spread || node->args.count != 2) return false;
  if (c->regexp_replace_write_seen || c->regexp_exec_write_seen) return false;

  sv_ast_t *callee = node->left;
  if (!callee || callee->type != N_MEMBER) return false;
  if ((callee->flags & 1) || !callee->right || !callee->right->str) return false;
  if (!is_ident_str(callee->right->str, callee->right->len, "replace", 7)) return false;
  if (!regexp_literal_exec_arg_is_simple(c, callee->left)) return false;

  sv_ast_t *rx = node->args.items[0];
  sv_ast_t *replacement = node->args.items[1];
  if (!rx || rx->type != N_REGEXP || !replacement || replacement->type != N_STRING) return false;
  if (replacement->str && memchr(replacement->str, '$', replacement->len)) return false;

  compile_expr(c, callee->left);
  emit_constant(c, js_mkstr_permanent(c->js, rx->str ? rx->str : "", rx->len));
  emit_constant(c, js_mkstr_permanent(c->js, rx->aux ? rx->aux : "", rx->aux_len));
  compile_expr(c, replacement);
  emit_op(c, OP_STR_RE_LITERAL_REPLACE);
  return true;
}

static bool compile_regexp_exec_truthy_intrinsic(
  sv_compiler_t *c, sv_ast_t *node
) {
  if (!node || node->type != N_CALL || call_has_spread_arg(node) || node->args.count != 1)
    return false;
    
  sv_ast_t *callee = node->left;
  if (!callee || callee->type != N_MEMBER) return false;
  if ((callee->flags & 1) || !callee->right || !callee->right->str) return false;
  if (is_ident_name(callee->left, "super")) return false;
  if (!is_ident_str(callee->right->str, callee->right->len, "exec", 4))
    return false;

  compile_expr(c, callee->left);
  compile_receiver_property_get(c, callee);
  compile_expr(c, node->args.items[0]);
  emit_op(c, OP_RE_EXEC_TRUTHY);
  
  return true;
}

static void compile_truthy_test_expr(sv_compiler_t *c, sv_ast_t *node) {
  if (compile_regexp_exec_truthy_intrinsic(c, node)) return;
  compile_expr(c, node);
}

static void compile_optional_call_after_setup(
  sv_compiler_t *c, sv_ast_t *call_node,
  sv_call_kind_t kind, bool has_spread
) {
  emit_op(c, OP_DUP);
  emit_op(c, OP_IS_UNDEF_OR_NULL);
  int j_do_call = emit_jump(c, OP_JMP_FALSE);
  emit_op(c, OP_POP);
  if (sv_call_kind_has_receiver(kind))
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

void compile_call(sv_compiler_t *c, sv_ast_t *node) {
  sv_ast_t *callee = node->left;
  bool has_spread = call_has_spread_arg(node);

  if (is_ident_name(callee, "require") && is_free_name(c, "require", 7))
    request_commonjs_retry(c);

  if (callee->type == N_OPTIONAL) {
    compile_call_optional(c, node, callee, has_spread);
    return;
  }

  if (compile_call_is_proto_intrinsic(c, node, has_spread))
    return;

  if (compile_call_array_includes_intrinsic(c, node, has_spread))
    return;

  if (compile_regexp_literal_exec_intrinsic(c, node, has_spread))
    return;

  if (compile_string_regexp_literal_replace_intrinsic(c, node, has_spread))
    return;

  if (
    !has_spread && node->args.count >= 2 &&
    callee->type == N_MEMBER &&
    is_ident_name(callee->left, "Ant") &&
    resolve_local(c, "Ant", 3) == -1 &&
    callee->right && callee->right->type == N_IDENT &&
    callee->right->len == 5 && memcmp(callee->right->str, "match", 5) == 0 &&
    node->args.items[1]->type == N_OBJECT
  ) {
    sv_ast_t *obj = node->args.items[1];
    sv_ast_t *param = sv_ast_new(N_IDENT);
    
    param->str = "$"; param->len = 1;
    sv_ast_t *arrow = sv_ast_new(N_FUNC);
    
    arrow->flags = FN_ARROW;
    arrow->body = obj;
    arrow->line = obj->line; arrow->col = obj->col;
    arrow->src_off = obj->src_off; arrow->src_end = obj->src_end;
    
    sv_ast_list_push(&arrow->args, param);
    node->args.items[1] = arrow;
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

  if (callee->type == N_MEMBER && sv_node_has_optional_base(callee->left)) {
    compile_expr(c, callee->left);
    int ok_jump  = emit_jump(c, OP_JMP_NOT_NULLISH);
    emit_op(c, OP_POP);
    emit_op(c, OP_UNDEF);
    int end_jump = emit_jump(c, OP_JMP);
    patch_jump(c, ok_jump);
    compile_receiver_property_get(c, callee);
    compile_call_emit_invoke(c, node, SV_CALL_METHOD, has_spread);
    patch_jump(c, end_jump);
    return;
  }

  sv_call_kind_t kind = compile_call_setup_non_optional(c, callee);
  compile_call_emit_invoke(c, node, kind, has_spread);
}

void compile_new(sv_compiler_t *c, sv_ast_t *node) {
  compile_expr(c, node->left);
  emit_op(c, OP_DUP);
  if (call_has_spread_arg(node)) {
    compile_call_args_array(c, node);
    emit_op(c, OP_NEW_APPLY);
    emit_u16(c, 1);
  } else {
    int argc = node->args.count;
    for (int i = 0; i < argc; i++)
      compile_expr(c, node->args.items[i]);
    emit_op(c, OP_NEW);
    emit_u16(c, (uint16_t)argc);
  }
}

void compile_member(sv_compiler_t *c, sv_ast_t *node) {
  if (is_module_exports_member(c, node))
    request_commonjs_retry(c);

  if (is_ident_name(node->left, "super")) {
    if (!(node->flags & 1) && is_private_name_node(node->right)) {
      js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Cannot access private member through super");
      emit_op(c, OP_UNDEF);
      return;
    }
    emit_op(c, OP_THIS);
    emit_get_var(c, "super", 5);
    if (node->flags & 1)
      compile_expr(c, node->right);
    else
      emit_constant(c, js_mkstr_permanent(c->js, node->right->str, node->right->len));
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
  } else if (is_private_name_node(node->right)) {
    emit_private_token(c, node->right);
    emit_op(c, OP_GET_PRIVATE);
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

void compile_optional_get(sv_compiler_t *c, sv_ast_t *node) {
  if (node->flags & 1) {
    compile_expr(c, node->right);
    emit_op(c, OP_GET_ELEM_OPT);
  } else if (is_private_name_node(node->right)) {
    emit_private_token(c, node->right);
    emit_op(c, OP_GET_PRIVATE_OPT);
  } else {
    emit_srcpos(c, node->right);
    emit_atom_op(c, OP_GET_FIELD_OPT, node->right->str, node->right->len);
  }
}

void compile_optional(sv_compiler_t *c, sv_ast_t *node) {
  compile_expr(c, node->left);
  compile_optional_get(c, node);
}

void compile_array(sv_compiler_t *c, sv_ast_t *node) {
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

void compile_object(sv_compiler_t *c, sv_ast_t *node) {
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
      if (prop->flags & FN_GETTER) flags |= SV_DEFINE_METHOD_GETTER;
      if (prop->flags & FN_SETTER) flags |= SV_DEFINE_METHOD_SETTER;
      flags |= SV_DEFINE_METHOD_SET_NAME;
      if (prop->flags & FN_COMPUTED) compile_expr(c, prop->left);
      else compile_static_property_key(c, prop->left);
      emit_op(c, OP_SWAP);
      emit_op(c, OP_DEFINE_METHOD_COMP);
      emit(c, flags);
    } else if (prop->flags & FN_COMPUTED) {
      compile_expr(c, prop->left);
      compile_expr(c, prop->right);
      emit_op(c, OP_DEFINE_METHOD_COMP);
      emit(c, node_needs_inferred_function_name(prop->right) ? SV_DEFINE_METHOD_SET_NAME : 0);
    } else {
      if (prop->left && prop->left->type == N_IDENT && !is_quoted_ident_key(prop->left))
        compile_expr_with_inferred_name(c, prop->right, prop->left->str, prop->left->len);
      else compile_expr(c, prop->right);
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

void compile_func_expr(sv_compiler_t *c, sv_ast_t *node) {
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
  
  int idx = add_constant(c, mkval(T_NTARG, (uintptr_t)fn));
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
    emit_constant(c, js_mkstr_permanent(c->js, key->str, key->len));
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

static void compile_destructure_pattern(
  sv_compiler_t *c, sv_ast_t *pat,
  bool keep, bool consume_source,
  sv_destructure_mode_t mode,
  sv_var_kind_t kind
) {
  if (!pat) return;
  if (keep) emit_op(c, OP_DUP);

  if (pat->type == N_ARRAY_PAT || pat->type == N_ARRAY) {
    if (!consume_source && !keep) emit_op(c, OP_DUP);
    emit_op(c, OP_DESTRUCTURE_INIT);
    
    int err_local = add_local(c, "", 0, false, c->scope_depth);
    int try_jump = emit_jump(c, OP_TRY_PUSH);
    c->try_depth++;
    
    for (int i = 0; i < pat->args.count; i++) {
      sv_ast_t *elem = pat->args.items[i];
      if (!elem || elem->type == N_EMPTY) {
        emit_op(c, OP_DESTRUCTURE_NEXT);
        emit_op(c, OP_POP);
        continue;
      }
      
      if (elem->type == N_REST || elem->type == N_SPREAD) {
        emit_op(c, OP_DESTRUCTURE_REST);
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
      
      emit_op(c, OP_DESTRUCTURE_NEXT);
      
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

    emit_op(c, OP_TRY_POP);
    emit_op(c, OP_DESTRUCTURE_CLOSE);
    int end_jump = emit_jump(c, OP_JMP);

    patch_jump(c, try_jump);
    int catch_tag = emit_jump(c, OP_CATCH);
    
    emit_put_local(c, err_local);
    emit_op(c, OP_DESTRUCTURE_CLOSE);
    emit_get_local(c, err_local);
    emit_op(c, OP_THROW);
    patch_jump(c, catch_tag);
    patch_jump(c, end_jump);
    c->try_depth--;
    
    return;
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

void compile_array_destructure(sv_compiler_t *c, sv_ast_t *pat, bool keep) {
  compile_destructure_pattern(c, pat, keep, true, DESTRUCTURE_ASSIGN, SV_VAR_LET);
}

void compile_object_destructure(sv_compiler_t *c, sv_ast_t *pat, bool keep) {
  compile_destructure_pattern(c, pat, keep, true, DESTRUCTURE_ASSIGN, SV_VAR_LET);
}

static bool is_tail_callable(sv_compiler_t *c, sv_ast_t *node) {
  if (c->try_depth > 0) return false;
  if (c->using_cleanup_count > 0) return false;
  if (node->type != N_CALL) return false;
  if (call_has_spread_arg(node)) return false;
  
  sv_ast_t *callee = node->left;
  if (
    callee->type == N_IDENT && 
    callee->len == 5 && memcmp(callee->str, "super", 5) == 0
  ) return false;
  
  return true;
}

static void emit_using_dispose_call(
  sv_compiler_t *c,
  int stack_local,
  int completion_local,
  bool is_async,
  bool suppressed_completion
) {
  emit_get_local(c, stack_local);
  if (suppressed_completion) {
    emit_get_local(c, completion_local);
  }
  if (is_async && suppressed_completion) emit_op(c, OP_USING_DISPOSE_ASYNC_SUPPRESSED);
  else if (is_async) emit_op(c, OP_USING_DISPOSE_ASYNC);
  else if (suppressed_completion) emit_op(c, OP_USING_DISPOSE_SUPPRESSED);
  else emit_op(c, OP_USING_DISPOSE);
  if (is_async) emit_op(c, OP_AWAIT);
}

static void emit_using_cleanups_to_depth(sv_compiler_t *c, int target_depth) {
for (int i = c->using_cleanup_count - 1; i >= 0; i--) {
  sv_using_cleanup_t *cleanup = &c->using_cleanups[i];
  if (cleanup->scope_depth <= target_depth) break;
  emit_using_dispose_call(c, cleanup->stack_local, -1, cleanup->is_async, false);
  emit_op(c, OP_POP);
}}

static void emit_return_from_stack(sv_compiler_t *c) {
  emit_using_cleanups_to_depth(c, -1);
  emit_close_upvals(c);
  emit_op(c, OP_RETURN);
}

static void compile_tail_call(sv_compiler_t *c, sv_ast_t *node) {
  sv_ast_t *callee = node->left;

  if (callee->type == N_OPTIONAL) {
    compile_call(c, node);
    emit_return_from_stack(c);
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

void compile_tail_return_expr(sv_compiler_t *c, sv_ast_t *expr) {
  if (expr->type == N_TERNARY) {
    compile_truthy_test_expr(c, expr->cond);
    int else_jump = emit_jump(c, OP_JMP_FALSE);
    compile_tail_return_expr(c, expr->left);
    patch_jump(c, else_jump);
    compile_tail_return_expr(c, expr->right);
    return;
  }

  if (expr->type == N_AWAIT && c->is_async && c->try_depth == 0 && !c->is_tla &&
      c->using_cleanup_count == 0) {
    compile_expr(c, expr->right);
    emit_return_from_stack(c);
    return;
  }

  if (is_tail_callable(c, expr)) {
    compile_tail_call(c, expr);
    return;
  }

  compile_expr(c, expr);
  emit_return_from_stack(c);
}

void compile_stmts(sv_compiler_t *c, sv_ast_list_t *list) {
  for (int i = 0; i < list->count; i++) compile_stmt(c, list->items[i]);
}

static bool stmt_list_has_using_decl(sv_ast_list_t *list, bool *has_await_using) {
  bool found = false;
  for (int i = 0; i < list->count; i++) {
    sv_ast_t *node = list->items[i];
    if (!node) continue;
    
    sv_ast_t *decl = (node->type == N_EXPORT) ? node->left : node;
    if (!decl || decl->type != N_VAR) continue;
    
    if (decl->var_kind == SV_VAR_USING || decl->var_kind == SV_VAR_AWAIT_USING) {
      if (decl->var_kind == SV_VAR_AWAIT_USING && has_await_using) *has_await_using = true;
      found = true;
    }
  }
  
  return found;
}

static void emit_empty_disposal_stack(sv_compiler_t *c) {
  emit_op(c, OP_ARRAY);
  emit_u16(c, 0);
}

static void emit_using_push(sv_compiler_t *c, bool is_async) {
  emit_op(c, is_async ? OP_USING_PUSH_ASYNC : OP_USING_PUSH);
}

static void pop_using_cleanup(sv_compiler_t *c) {
  if (c->using_cleanup_count > 0) c->using_cleanup_count--;
}

static void emit_dispose_resource(sv_compiler_t *c, bool is_async) {
  emit_op(c, is_async ? OP_DISPOSE_RESOURCE_ASYNC : OP_DISPOSE_RESOURCE);
  if (is_async) emit_op(c, OP_AWAIT);
}

static void push_using_cleanup(
  sv_compiler_t *c,
  int stack_local,
  int scope_depth,
  bool is_async
) {
  if (c->using_cleanup_count >= c->using_cleanup_cap) {
    int cap = c->using_cleanup_cap ? c->using_cleanup_cap * 2 : 4;
    c->using_cleanups = realloc(c->using_cleanups, (size_t)cap * sizeof(sv_using_cleanup_t));
    c->using_cleanup_cap = cap;
  }

  c->using_cleanups[c->using_cleanup_count++] = (sv_using_cleanup_t){
    .stack_local = stack_local,
    .scope_depth = scope_depth,
    .is_async = is_async,
  };
}

static void compile_block_with_using(sv_compiler_t *c, sv_ast_t *node) {
  bool has_await_using = false;
  bool has_using = stmt_list_has_using_decl(&node->args, &has_await_using);

  begin_scope(c);
  hoist_lexical_decls(c, &node->args);
  hoist_func_decls(c, &node->args);

  if (!has_using) {
    compile_stmts(c, &node->args);
    end_scope(c);
    return;
  }

  emit_empty_disposal_stack(c);
  int stack_local = add_local(c, "", 0, false, c->scope_depth);
  
  emit_put_local(c, stack_local);
  int err_local = add_local(c, "", 0, false, c->scope_depth);

  int old_using_stack = c->using_stack_local;
  bool old_using_async = c->using_stack_async;
  
  c->using_stack_local = stack_local;
  c->using_stack_async = has_await_using;
  push_using_cleanup(c, stack_local, c->scope_depth, has_await_using);

  c->try_depth++;
  int try_jump = emit_jump(c, OP_TRY_PUSH);
  compile_stmts(c, &node->args);
  emit_op(c, OP_TRY_POP);
  c->try_depth--;

  emit_using_dispose_call(c, stack_local, -1, has_await_using, false);
  emit_op(c, OP_POP);
  int end_jump = emit_jump(c, OP_JMP);

  patch_jump(c, try_jump);
  int catch_tag = emit_jump(c, OP_CATCH);
  
  emit_put_local(c, err_local);
  emit_using_dispose_call(c, stack_local, err_local, has_await_using, true);
  
  if (!has_await_using) emit_op(c, OP_THROW);
  patch_jump(c, catch_tag);
  patch_jump(c, end_jump);

  c->using_stack_local = old_using_stack;
  c->using_stack_async = old_using_async;
  
  pop_using_cleanup(c);
  end_scope(c);
}

void compile_stmt(sv_compiler_t *c, sv_ast_t *node) {
  if (!node) return;
  emit_srcpos(c, node);

  switch (node->type) {
    case N_EMPTY:
    case N_DEBUGGER:
      break;

    case N_BLOCK:
      compile_block_with_using(c, node);
      break;

    case N_VAR:
      compile_var_decl(c, node);
      break;

    case N_IMPORT_DECL:
      mark_module_syntax_seen(c);
      compile_import_decl(c, node);
      break;

    case N_EXPORT:
      mark_module_syntax_seen(c);
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
      compile_for_of(c, node);
      break;

    case N_FOR_AWAIT_OF:
      if (c->enclosing && !c->enclosing->enclosing) c->is_tla = true;
      compile_for_of(c, node);
      break;

    case N_RETURN:
      if (node->right) {
        compile_tail_return_expr(c, node->right);
      } else {
        emit_op(c, OP_UNDEF);
        emit_return_from_stack(c);
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

    case N_ASSIGN:
      if (!has_completion_accumulator(c) && compile_self_append_stmt(c, node)) break;
      compile_expr(c, node);
      emit_set_completion_from_stack(c);
      break;

    case N_FUNC:
      if (node->str && !(node->flags & (FN_ARROW | FN_PAREN))) break;
      compile_expr(c, node);
      emit_set_completion_from_stack(c);
      break;

    case N_CLASS:
      compile_class(c, node);
      if (node->flags & FN_PAREN) emit_set_completion_from_stack(c);
      else emit_op(c, OP_POP);
      break;

    case N_WITH:
      emit_set_completion_undefined(c);
      compile_expr(c, node->left);
      emit_op(c, OP_ENTER_WITH);
      c->with_depth++;
      compile_stmt(c, node->body);
      c->with_depth--;
      emit_op(c, OP_EXIT_WITH);
      break;

    default:
      compile_expr(c, node);
      emit_set_completion_from_stack(c);
      break;
  }
}

enum {
  IMPORT_BIND_DEFAULT   = 1 << 0,
  IMPORT_BIND_NAMESPACE = 1 << 1,
};

void compile_import_decl(sv_compiler_t *c, sv_ast_t *node) {
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
        emit_atom_op(c, OP_IMPORT_NAMED, spec->left->str, spec->left->len);
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

void compile_export_decl(sv_compiler_t *c, sv_ast_t *node) {
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
        emit_atom_op(c, OP_IMPORT_NAMED, spec->left->str, spec->left->len);
      emit_atom_op(c, OP_EXPORT, spec->right->str, spec->right->len);
    } else {
      emit_get_var(c, spec->left->str, spec->left->len);
      emit_atom_op(c, OP_EXPORT, spec->right->str, spec->right->len);
    }
  }
}

void compile_var_decl(sv_compiler_t *c, sv_ast_t *node) {
  sv_var_kind_t kind = node->var_kind;
  
  bool is_using = (kind == SV_VAR_USING || kind == SV_VAR_AWAIT_USING);
  bool is_await_using = (kind == SV_VAR_AWAIT_USING);
  bool is_const = (kind == SV_VAR_CONST || is_using);
  bool repl_top = is_repl_top_level(c);

  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *decl = node->args.items[i];
    if (decl->type != N_VARDECL) continue;
    sv_ast_t *target = decl->left;

    if (repl_top) {
      if (!decl->right && kind == SV_VAR_VAR) continue;
      if (decl->right) {
        if (target->type == N_IDENT)
          compile_expr_with_inferred_name(c, decl->right, target->str, target->len);
        else compile_expr(c, decl->right);
      } else emit_op(c, OP_UNDEF);
      if (target->type == N_IDENT) {
        emit_atom_op(c, OP_PUT_GLOBAL, target->str, target->len);
      } else compile_destructure_pattern(c, target, false, true, DESTRUCTURE_ASSIGN, kind);
    } else if (kind == SV_VAR_VAR) {
      if (decl->right) {
        uint8_t init_type = infer_expr_type(c, decl->right);
        if (target->type == N_IDENT)
          compile_expr_with_inferred_name(c, decl->right, target->str, target->len);
        else compile_expr(c, decl->right);
        if (target->type == N_IDENT) {
          int idx = resolve_local(c, target->str, target->len);
          if (idx >= 0 && c->locals[idx].depth != -1)
            emit_put_local_typed(c, idx, init_type);
          else compile_lhs_set(c, target, false);
        } else compile_lhs_set(c, target, false);
      }
    } else {
      if (target->type == N_IDENT) {
        int idx = ensure_local_at_depth(c, target->str, target->len, is_const, c->scope_depth);
        uint8_t init_type = SV_TI_UNKNOWN;
        if (decl->right) {
          init_type = infer_expr_type(c, decl->right);
          compile_expr_with_inferred_name(c, decl->right, target->str, target->len);
        } else if (!is_const) {
          emit_op(c, OP_UNDEF);
          init_type = SV_TI_UNDEF;
        }
        if (decl->right || !is_const) {
          emit_put_local_typed(c, idx, init_type);
          c->locals[idx].is_tdz = false;
          if (is_using) {
            if (c->using_stack_local >= 0) {
              emit_get_local(c, c->using_stack_local);
              emit_get_local(c, idx);
              emit_using_push(c, is_await_using);
            } else {
              emit_get_local(c, idx);
              emit_dispose_resource(c, is_await_using);
            }
            emit_op(c, OP_POP);
          }
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

void compile_destructure_binding(sv_compiler_t *c, sv_ast_t *pat, sv_var_kind_t kind) {
  compile_destructure_pattern(c, pat, false, false, DESTRUCTURE_BIND, kind);
}

static bool fold_static_typeof_compare(
  sv_compiler_t *c, sv_ast_t *cond, bool *out_truth
) {
  if (!cond || cond->type != N_BINARY) return false;
  if (!(cond->op == TOK_SEQ || cond->op == TOK_SNE ||
        cond->op == TOK_EQ  || cond->op == TOK_NE))
    return false;

  sv_ast_t *typeof_node = NULL;
  sv_ast_t *str_node = NULL;
  if (cond->left && cond->left->type == N_TYPEOF &&
      cond->right && cond->right->type == N_STRING) {
    typeof_node = cond->left;
    str_node = cond->right;
  } else if (cond->right && cond->right->type == N_TYPEOF &&
             cond->left && cond->left->type == N_STRING) {
    typeof_node = cond->right;
    str_node = cond->left;
  } else return false;

  if (!typeof_node->right || typeof_node->right->type != N_IDENT) return false;
  sv_ast_t *ident = typeof_node->right;
  int local = resolve_local(c, ident->str, ident->len);
  if (local < 0) return false;
  const char *known = typeof_name_for_type(get_local_inferred_type(c, local));
  if (!known) return false;

  bool is_equal = (strlen(known) == str_node->len &&
                   memcmp(known, str_node->str, str_node->len) == 0);
  bool truth = (cond->op == TOK_SEQ || cond->op == TOK_EQ) ? is_equal : !is_equal;
  *out_truth = truth;
  return true;
}

void compile_if(sv_compiler_t *c, sv_ast_t *node) {
  emit_set_completion_undefined(c);

  bool folded_truth = false;
  if (fold_static_typeof_compare(c, node->cond, &folded_truth)) {
    if (folded_truth) compile_stmt(c, node->left);
    else if (node->right) compile_stmt(c, node->right);
    return;
  }

  compile_truthy_test_expr(c, node->cond);
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

void compile_while(sv_compiler_t *c, sv_ast_t *node) {
  emit_set_completion_undefined(c);

  int loop_start = c->code_len;
  push_loop(c, loop_start, NULL, 0, false);

  compile_truthy_test_expr(c, node->cond);
  int exit_jump = emit_jump(c, OP_JMP_FALSE);
  compile_stmt(c, node->body);

  sv_loop_t *loop = &c->loops[c->loop_count - 1];
  for (int i = 0; i < loop->continues.count; i++)
    patch_jump(c, loop->continues.offsets[i]);

  emit_loop(c, loop_start);
  patch_jump(c, exit_jump);
  pop_loop(c);
}

void compile_do_while(sv_compiler_t *c, sv_ast_t *node) {
  emit_set_completion_undefined(c);

  int loop_start = c->code_len;
  push_loop(c, loop_start, NULL, 0, false);
  compile_stmt(c, node->body);

  sv_loop_t *loop = &c->loops[c->loop_count - 1];
  for (int i = 0; i < loop->continues.count; i++)
    patch_jump(c, loop->continues.offsets[i]);

  compile_truthy_test_expr(c, node->cond);
  int exit_jump = emit_jump(c, OP_JMP_FALSE);
  emit_loop(c, loop_start);
  patch_jump(c, exit_jump);
  pop_loop(c);
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

static void for_collect_pattern_slots(sv_compiler_t *c, sv_ast_t *pat, int **slots, int *count, int *cap) {
  if (!pat) return;
  switch (pat->type) {
    case N_IDENT: {
      int slot = resolve_local(c, pat->str, pat->len);
      for_add_slot_unique(slots, count, cap, slot);
      break;
    }
    case N_ASSIGN_PAT:
    case N_ASSIGN:
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

static void for_collect_var_decl_slots(sv_compiler_t *c, sv_ast_t *init_var, int **slots, int *count, int *cap) {
  if (!init_var || init_var->type != N_VAR) return;
  for (int i = 0; i < init_var->args.count; i++) {
    sv_ast_t *decl = init_var->args.items[i];
    if (!decl || decl->type != N_VARDECL || !decl->left) continue;
    for_collect_pattern_slots(c, decl->left, slots, count, cap);
  }
}

void compile_for(sv_compiler_t *c, sv_ast_t *node) {
  emit_set_completion_undefined(c);
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
  push_loop(c, loop_start, NULL, 0, false);

  int exit_jump = -1;
  if (node->cond) {
    compile_truthy_test_expr(c, node->cond);
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
    int inner_idx = add_local(c, outer.name, outer.name_len, outer.is_const, c->scope_depth);
    emit_put_local(c, inner_idx);
  }}

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
      set_local_inferred_type(c, c->param_locals + slot, SV_TI_UNKNOWN);
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

void compile_for_in(sv_compiler_t *c, sv_ast_t *node) {
  compile_for_each(c, node, false);
}


void compile_for_of(sv_compiler_t *c, sv_ast_t *node) {
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
        bool is_const = (lhs->var_kind == SV_VAR_CONST ||
                         lhs->var_kind == SV_VAR_USING ||
                         lhs->var_kind == SV_VAR_AWAIT_USING);
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

static void compile_using_dispose_target(sv_compiler_t *c, sv_ast_t *lhs) {
  if (!lhs || lhs->type != N_VAR) return;
  bool is_await_using = lhs->var_kind == SV_VAR_AWAIT_USING;
  if (lhs->var_kind != SV_VAR_USING && !is_await_using) return;
  if (lhs->args.count == 0) return;

  sv_ast_t *decl = lhs->args.items[0];
  if (!decl || decl->type != N_VARDECL || !decl->left || decl->left->type != N_IDENT) return;

  emit_get_var(c, decl->left->str, decl->left->len);
  emit_dispose_resource(c, is_await_using);
  emit_op(c, OP_POP);
}

static bool compile_using_push_target(sv_compiler_t *c, sv_ast_t *lhs, int stack_local) {
  if (!lhs || lhs->type != N_VAR) return false;
  bool is_await_using = lhs->var_kind == SV_VAR_AWAIT_USING;
  if (lhs->var_kind != SV_VAR_USING && !is_await_using) return false;
  if (lhs->args.count == 0) return false;

  sv_ast_t *decl = lhs->args.items[0];
  if (!decl || decl->type != N_VARDECL || !decl->left || decl->left->type != N_IDENT) return false;

  emit_get_local(c, stack_local);
  emit_get_var(c, decl->left->str, decl->left->len);
  emit_using_push(c, is_await_using);
  emit_op(c, OP_POP);
  
  return true;
}

static void compile_for_each(sv_compiler_t *c, sv_ast_t *node, bool is_for_of) {
  emit_set_completion_undefined(c);
  begin_scope(c);

  int *iter_slots = NULL;
  int iter_count = 0;
  int iter_cap = 0;

  if (node->left && node->left->type == N_VAR &&
      node->left->var_kind != SV_VAR_VAR) {
    bool is_const = 
      (node->left->var_kind == SV_VAR_CONST ||
       node->left->var_kind == SV_VAR_USING ||
       node->left->var_kind == SV_VAR_AWAIT_USING);
    int lb = c->local_count;
    for (int i = 0; i < node->left->args.count; i++) {
      sv_ast_t *decl = node->left->args.items[i];
      if (!decl || decl->type != N_VARDECL || !decl->left) continue;
      hoist_lexical_pattern(c, decl->left, is_const);
    }
    for (int i = lb; i < c->local_count; i++) {
      c->locals[i].is_tdz = true;
      set_local_inferred_type(c, i, SV_TI_UNKNOWN);
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
  int using_stack_local = -1;

  bool is_for_await = (node->type == N_FOR_AWAIT_OF);
  bool is_using_loop = node->left && node->left->type == N_VAR &&
    (node->left->var_kind == SV_VAR_USING || node->left->var_kind == SV_VAR_AWAIT_USING);
  bool is_await_using_loop = is_using_loop && node->left->var_kind == SV_VAR_AWAIT_USING;
  int old_using_stack = c->using_stack_local;
  bool old_using_async = c->using_stack_async;
  uint8_t iter_hint = 0;
  if (is_for_of && !is_for_await)
    iter_hint = iter_hint_for_type(infer_expr_type(c, node->right));

  if (is_using_loop) {
    emit_empty_disposal_stack(c);
    using_stack_local = add_local(c, "", 0, false, c->scope_depth);
    emit_put_local(c, using_stack_local);
  }

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
  push_loop(c, loop_start, NULL, 0, false);

  if (is_using_loop) {
    emit_empty_disposal_stack(c);
    emit_put_local(c, using_stack_local);
  }

  if (is_for_of) {
    if (is_for_await) {
      emit_op(c, OP_AWAIT_ITER_NEXT);
    } else {
      emit_op(c, OP_ITER_NEXT);
      emit(c, iter_hint);
    }
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

  if (is_using_loop) {
    c->using_stack_local = using_stack_local;
    c->using_stack_async = is_await_using_loop;
    push_using_cleanup(c, using_stack_local, c->scope_depth, is_await_using_loop);
    compile_using_push_target(c, node->left, using_stack_local);
  }

  compile_stmt(c, node->body);
  sv_loop_t *loop = &c->loops[c->loop_count - 1];
  for (int i = 0; i < loop->continues.count; i++)
    patch_jump(c, loop->continues.offsets[i]);

  if (is_using_loop) {
    emit_using_dispose_call(c, using_stack_local, -1, is_await_using_loop, false);
    emit_op(c, OP_POP);

    c->using_stack_local = old_using_stack;
    c->using_stack_async = old_using_async;
    pop_using_cleanup(c);
  } else compile_using_dispose_target(c, node->left);

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

  if (is_for_of) {
    emit_op(c, OP_POP);
    emit_op(c, OP_TRY_POP);

    int skip_break_cleanup = -1;
    if (break_close_slot >= 0)
      skip_break_cleanup = emit_jump(c, OP_JMP);

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
    
    if (is_using_loop) {
      emit_using_dispose_call(c, using_stack_local, iter_err_local, is_await_using_loop, true);
      emit_put_local(c, iter_err_local);
    }
    
    emit_op(c, OP_ITER_CLOSE);          
    emit_get_local(c, iter_err_local);  
    emit_op(c, OP_THROW);              
    patch_jump(c, catch_tag);

    patch_jump(c, end_jump);
    c->try_depth--;
  } else {
    int skip_break_cleanup = -1;
    if (break_close_slot >= 0)
      skip_break_cleanup = emit_jump(c, OP_JMP);

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


static void emit_close_upvals_to_depth(sv_compiler_t *c, int target_depth) {
for (int i = c->local_count - 1; i >= 0; i--) {
  if (c->locals[i].depth <= target_depth) break;
  if (c->locals[i].captured) {
    int frame_slot = local_to_frame_slot(c, i);
    emit_op(c, OP_CLOSE_UPVAL);
    emit_u16(c, (uint16_t)frame_slot);
  }
}}

void compile_break(sv_compiler_t *c, sv_ast_t *node) {
  if (c->loop_count == 0) return;

  int target = c->loop_count - 1;
  if (node->str) for (int i = c->loop_count - 1; i >= 0; i--) if (
    c->loops[i].label &&
    c->loops[i].label_len == node->len &&
    memcmp(c->loops[i].label, node->str, node->len) == 0
  ) { target = i; break;  }

  emit_close_upvals_to_depth(c, c->loops[target].scope_depth);
  emit_using_cleanups_to_depth(c, c->loops[target].scope_depth);
  
  int offset = emit_jump(c, OP_JMP);
  patch_list_add(&c->loops[target].breaks, offset);
}


void compile_continue(sv_compiler_t *c, sv_ast_t *node) {
  for (int i = c->loop_count - 1; i >= 0; i--) if (node->str) {
    if (
      c->loops[i].label &&
      c->loops[i].label_len == node->len &&
      memcmp(c->loops[i].label, node->str, node->len) == 0
    ) {
      emit_close_upvals_to_depth(c, c->loops[i].scope_depth);
      emit_using_cleanups_to_depth(c, c->loops[i].scope_depth);
      patch_list_add(&c->loops[i].continues, emit_jump(c, OP_JMP));
      return;
    }
  } else if (!c->loops[i].is_switch) {
    emit_close_upvals_to_depth(c, c->loops[i].scope_depth);
    emit_using_cleanups_to_depth(c, c->loops[i].scope_depth);
    patch_list_add(&c->loops[i].continues, emit_jump(c, OP_JMP));
    return;
  }
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
    int loc = add_local(c, node->catch_param->str, node->catch_param->len, false, c->scope_depth);
    emit_put_local(c, loc);
  } else if (node->catch_param && is_destructure_pattern_node(node->catch_param)) {
    compile_destructure_binding(c, node->catch_param, SV_VAR_LET);
    emit_op(c, OP_POP);
  } else emit_op(c, OP_POP);  
  
  compile_stmt(c, node->catch_body);
  end_scope(c);
}

void compile_try(sv_compiler_t *c, sv_ast_t *node) {
  emit_set_completion_undefined(c);

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


void compile_switch(sv_compiler_t *c, sv_ast_t *node) {
  emit_set_completion_undefined(c);

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
  push_loop(c, c->code_len, NULL, 0, true);

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
  return n && 
    (n->type == N_WHILE || n->type == N_DO_WHILE ||
    n->type == N_FOR    || n->type == N_FOR_IN   ||
    n->type == N_FOR_OF || n->type == N_FOR_AWAIT_OF);
}

void compile_label(sv_compiler_t *c, sv_ast_t *node) {
  if (is_loop_node(node->body)) {
    c->pending_label = node->str;
    c->pending_label_len = node->len;
    compile_stmt(c, node->body);
    c->pending_label = NULL;
    c->pending_label_len = 0;
  } else {
    push_loop(c, c->code_len, node->str, node->len, false);
    compile_stmt(c, node->body);
    pop_loop(c);
  }
}

static inline bool is_class_method_def(const sv_ast_t *m);
static void emit_field_inits(sv_compiler_t *c, sv_ast_t **fields, int count) {
  sv_compiler_t *enc = c->enclosing;
  for (int i = 0; i < count; i++) {
    sv_ast_t *m = fields[i];
    bool is_fn = is_class_method_def(m);
    if (is_private_name_node(m->left)) {
      emit_op(c, OP_THIS);
      emit_private_token(c, m->left);
      if (is_fn) compile_func_expr(c, m->right);
      else if (m->right) compile_expr(c, m->right);
      else emit_op(c, OP_UNDEF);
      emit_op(c, OP_DEF_PRIVATE);
      if (m->flags & FN_GETTER) emit(c, SV_COMP_PRIVATE_GETTER);
      else if (m->flags & FN_SETTER) emit(c, SV_COMP_PRIVATE_SETTER);
      else emit(c, is_fn ? SV_COMP_PRIVATE_METHOD : SV_COMP_PRIVATE_FIELD);
      emit_op(c, OP_POP);
      continue;
    }

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

static inline bool is_class_method_def(const sv_ast_t *m) {
  return 
    m && m->right && m->right->type == N_FUNC &&
    (m->right->flags & FN_METHOD);
}

static void compile_static_initializer_value(sv_compiler_t *c, sv_ast_t *expr, int ctor_local);

static void compile_class_method(
  sv_compiler_t *c, sv_ast_t *m,
  int ctor_local, int proto_local,
  int preeval_key
) {
  bool is_static = !!(m->flags & FN_STATIC);
  int home_local = is_static ? ctor_local : proto_local;
  bool is_fn = is_class_method_def(m);

  if (is_fn) {
    if (is_static) m->right->flags |= FN_STATIC;
    compile_func_expr(c, m->right);   
    emit_get_local(c, home_local);    
    emit_op(c, OP_SET_HOME_OBJ);      
    emit_op(c, OP_SWAP);              
  } else {
    emit_get_local(c, home_local);    
    if (is_static) compile_static_initializer_value(c, m->right, ctor_local);
    else if (m->right) compile_expr(c, m->right);
    else emit_op(c, OP_UNDEF);        
  }

  uint8_t method_flags = 0;
  if (m->flags & FN_GETTER) method_flags |= SV_DEFINE_METHOD_GETTER;
  if (m->flags & FN_SETTER) method_flags |= SV_DEFINE_METHOD_SETTER;
  if (is_fn) method_flags |= SV_DEFINE_METHOD_SET_NAME;
  if (is_fn || (m->flags & (FN_GETTER | FN_SETTER))) method_flags |= SV_DEFINE_METHOD_NON_ENUM;

  if (m->flags & FN_COMPUTED) {
    if (preeval_key >= 0) emit_get_local(c, preeval_key);
    else compile_expr(c, m->left);
  } else compile_static_property_key(c, m->left); 
  
  emit_op(c, OP_SWAP);                          
  emit_op(c, OP_DEFINE_METHOD_COMP);
  emit(c, method_flags);                        
  emit_op(c, OP_POP);
}

static void compile_private_static_element(sv_compiler_t *c, sv_ast_t *m, int ctor_local) {
  bool is_fn = is_class_method_def(m);
  emit_get_local(c, ctor_local);
  emit_private_token(c, m->left);
  if (is_fn) {
    if (m->flags & FN_STATIC) m->right->flags |= FN_STATIC;
    compile_func_expr(c, m->right);
  } else if ((m->flags & FN_STATIC)) compile_static_initializer_value(c, m->right, ctor_local);
  else if (m->right) compile_expr(c, m->right);
  else emit_op(c, OP_UNDEF);

  emit_op(c, OP_DEF_PRIVATE);
  if (m->flags & FN_GETTER) emit(c, SV_COMP_PRIVATE_GETTER);
  else if (m->flags & FN_SETTER) emit(c, SV_COMP_PRIVATE_SETTER);
  else emit(c, is_fn ? SV_COMP_PRIVATE_METHOD : SV_COMP_PRIVATE_FIELD);
  emit_op(c, OP_POP);
}

static inline int compile_class_precompute_key(sv_compiler_t *c, sv_ast_t *key_expr) {
  compile_expr(c, key_expr);
  int loc = add_local(c, "", 0, false, c->scope_depth);
  emit_put_local(c, loc);
  return loc;
}

static int compile_static_child_function(sv_compiler_t *c, sv_ast_t *node, bool returns_expr) {
  sv_compiler_t comp;
  sv_compile_ctx_init_child(&comp, c, NULL, c->mode);
  comp.is_strict = true;

  static const char sv_name[] = "\x01super";
  comp.super_local = add_local(&comp, sv_name, sizeof(sv_name) - 1, false, comp.scope_depth);
  emit_op(&comp, OP_SPECIAL_OBJ);
  emit(&comp, 2);
  emit_put_local(&comp, comp.super_local);

  if (returns_expr) {
    if (node) compile_expr(&comp, node);
    else emit_op(&comp, OP_UNDEF);
    emit_return_from_stack(&comp);
  } else {
    compile_stmts(&comp, &node->args);
    emit_close_upvals(&comp);
    emit_op(&comp, OP_RETURN_UNDEF);
  }

  sv_func_t *fn = code_arena_bump(sizeof(sv_func_t));
  memset(fn, 0, sizeof(sv_func_t));
  fn->code = code_arena_bump((size_t)comp.code_len);
  memcpy(fn->code, comp.code, (size_t)comp.code_len);
  fn->code_len = comp.code_len;
  sv_func_init_obj_sites(fn);

  if (comp.const_count > 0) {
    fn->constants = code_arena_bump((size_t)comp.const_count * sizeof(ant_value_t));
    memcpy(fn->constants, comp.constants, (size_t)comp.const_count * sizeof(ant_value_t));
    fn->const_count = comp.const_count;
    build_gc_const_tables(fn);
  }
  if (comp.atom_count > 0) {
    fn->atoms = code_arena_bump((size_t)comp.atom_count * sizeof(sv_atom_t));
    memcpy(fn->atoms, comp.atoms, (size_t)comp.atom_count * sizeof(sv_atom_t));
    fn->atom_count = comp.atom_count;
  }
  fn->ic_count = (uint16_t)comp.ic_count;
  if (fn->ic_count > 0) {
    fn->ic_slots = code_arena_bump((size_t)fn->ic_count * sizeof(sv_ic_entry_t));
    memset(fn->ic_slots, 0, (size_t)fn->ic_count * sizeof(sv_ic_entry_t));
  }
  if (comp.upvalue_count > 0) {
    fn->upval_descs = code_arena_bump((size_t)comp.upvalue_count * sizeof(sv_upval_desc_t));
    memcpy(fn->upval_descs, comp.upval_descs, (size_t)comp.upvalue_count * sizeof(sv_upval_desc_t));
    fn->upvalue_count = comp.upvalue_count;
  }
  if (comp.srcpos_count > 0) {
    fn->srcpos = code_arena_bump((size_t)comp.srcpos_count * sizeof(sv_srcpos_t));
    memcpy(fn->srcpos, comp.srcpos, (size_t)comp.srcpos_count * sizeof(sv_srcpos_t));
    fn->srcpos_count = comp.srcpos_count;
  }

  fn->max_locals = comp.max_local_count;
  fn->max_stack = fn->max_locals + 64;
  fn->local_type_count = fn->max_locals;
  if (fn->max_locals > 0) {
    fn->local_types = code_arena_bump((size_t)fn->max_locals * sizeof(sv_type_info_t));
    memset(fn->local_types, 0, (size_t)fn->max_locals * sizeof(sv_type_info_t));
    if (comp.slot_types) {
      int ncopy = fn->max_locals < comp.slot_type_cap ? fn->max_locals : comp.slot_type_cap;
      memcpy(fn->local_types, comp.slot_types, (size_t)ncopy * sizeof(sv_type_info_t));
    }
  }
  fn->param_count = 0;
  fn->function_length = 0;
  fn->is_strict = true;
  fn->is_static = true;
  fn->filename = c->filename ? c->filename : c->js->filename;
  fn->source_line = node ? (int)node->line : 0;

  sv_compile_ctx_cleanup(&comp);

  return add_constant(c, mkval(T_NTARG, (uintptr_t)fn));
}

static void emit_static_child_call(sv_compiler_t *c, int func_idx, int ctor_local) {
  emit_op(c, OP_CLOSURE);
  emit_u32(c, (uint32_t)func_idx);
  emit_get_local(c, ctor_local);
  emit_op(c, OP_SET_HOME_OBJ);
  emit_op(c, OP_SWAP);
  emit_op(c, OP_CALL_METHOD);
  emit_u16(c, 0);
}

static void compile_static_initializer_value(sv_compiler_t *c, sv_ast_t *expr, int ctor_local) {
  int idx = compile_static_child_function(c, expr, true);
  emit_static_child_call(c, idx, ctor_local);
}

static void compile_static_block(sv_compiler_t *c, sv_ast_t *block, int ctor_local) {
  int idx = compile_static_child_function(c, block, false);
  emit_static_child_call(c, idx, ctor_local);
  emit_op(c, OP_POP);
}

void compile_class(sv_compiler_t *c, sv_ast_t *node) {
  int outer_name_local = -1;
  bool class_repl_top = is_repl_top_level(c);

  sv_ast_t *ctor_method = NULL;
  bool has_static_name = false;

  int field_count = 0;
  int computed_method_count = 0;

  if (node->str) outer_name_local = resolve_local(c, node->str, node->len);
  if (node->left) compile_expr(c, node->left);
  else emit_op(c, OP_UNDEF);

  sv_private_scope_t private_scope = { .parent = c->private_scope };
  sv_private_scope_t *saved_private_scope = c->private_scope;
  c->private_scope = &private_scope;

  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *m = node->args.items[i];
    if (m->type != N_METHOD) continue;
    bool is_fn = is_class_method_def(m);
    bool is_private = is_private_name_node(m->left);

    if (is_private) {
      uint8_t private_kind = (m->flags & FN_GETTER) ? SV_COMP_PRIVATE_GETTER :
        (m->flags & FN_SETTER) ? SV_COMP_PRIVATE_SETTER :
        is_fn ? SV_COMP_PRIVATE_METHOD : SV_COMP_PRIVATE_FIELD;
      if (!private_scope_add(c, &private_scope, m->left, private_kind, !!(m->flags & FN_STATIC))) {
        c->private_scope = saved_private_scope;
        free(private_scope.names);
        emit_op(c, OP_UNDEF);
        return;
      }
    }
    
    if (
      !(m->flags & FN_STATIC) &&
      !(m->flags & FN_COMPUTED) &&
      m->left && m->left->type == N_IDENT &&
      m->left->len == 11 &&
      memcmp(m->left->str, "constructor", 11) == 0
    ) { ctor_method = m; continue; }
    
    if (
      (m->flags & FN_STATIC) &&
      !(m->flags & FN_COMPUTED) &&
      m->left && m->left->str &&
      m->left->len == 4 &&
      memcmp(m->left->str, "name", 4) == 0
    ) has_static_name = true;
    
    if (!(m->flags & FN_STATIC) && (is_private || !is_fn)) field_count++;
    if (!is_private && node->str && (m->flags & FN_COMPUTED) && (is_fn || (m->flags & FN_STATIC))) computed_method_count++;
  }

  sv_ast_t **field_inits = NULL;
  int *computed_key_locals = NULL;
  int *method_comp_keys = NULL;
  if (field_count > 0) {
    field_inits = malloc(sizeof(sv_ast_t *) * field_count);
    computed_key_locals = malloc(sizeof(int) * field_count);
  }
  if (computed_method_count > 0) {
    method_comp_keys = malloc(sizeof(int) * node->args.count);
    for (int i = 0; i < node->args.count; i++) method_comp_keys[i] = -1;
  }

  if (field_count > 0 || method_comp_keys) {
  int fi = 0;
  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *m = node->args.items[i];
    if (m->type != N_METHOD || m == ctor_method) continue;
    
    bool is_fn = is_class_method_def(m);
    bool is_private = is_private_name_node(m->left);
    bool needs_instance_init = !(m->flags & FN_STATIC) && (is_private || !is_fn);
    
    if (needs_instance_init) {
      if (field_inits) field_inits[fi] = m;
      if (computed_key_locals) computed_key_locals[fi] = (!is_private && (m->flags & FN_COMPUTED))
        ? compile_class_precompute_key(c, m->left) : -1;
      fi++;
      continue;
    }
    
    if (is_private || !method_comp_keys || !(m->flags & FN_COMPUTED)) continue;
    method_comp_keys[i] = compile_class_precompute_key(c, m->left);
  }}

  int inner_name_local = -1;
  bool has_class_scope = node->str || private_scope.count > 0;
  if (has_class_scope) {
    begin_scope(c);
    if (node->str)
      inner_name_local = add_local(c, node->str, node->len, true, c->scope_depth);
    for (int i = 0; i < private_scope.count; i++) {
      sv_private_name_t *p = &private_scope.names[i];
      p->owner = c;
      p->local = add_local(c, "", 0, true, c->scope_depth);
      emit_op(c, OP_PRIVATE_TOKEN);
      emit_u32(c, p->hash);
      emit_put_local(c, p->local);
    }
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
    sv_compiler_t comp;
    sv_compile_ctx_init_child(&comp, c, NULL, c->mode);
    comp.is_strict = true;

    if (node->left) {
      emit_op(&comp, OP_THIS);
      emit_op(&comp, OP_SPECIAL_OBJ);
      emit(&comp, 2);
      emit_op(&comp, OP_SWAP);
      emit_op(&comp, OP_SPECIAL_OBJ);
      emit(&comp, 0);
      emit_op(&comp, OP_SUPER_APPLY);
      emit_u16(&comp, 1);
      emit_op(&comp, OP_POP);
    }

    emit_field_inits(&comp, field_inits, field_count);
    emit_op(&comp, OP_RETURN_UNDEF);

    sv_func_t *fn = code_arena_bump(sizeof(sv_func_t));
    memset(fn, 0, sizeof(sv_func_t));
    fn->code = code_arena_bump((size_t)comp.code_len);
    memcpy(fn->code, comp.code, (size_t)comp.code_len);
    fn->code_len = comp.code_len;
    sv_func_init_obj_sites(fn);
    if (comp.const_count > 0) {
      fn->constants = code_arena_bump((size_t)comp.const_count * sizeof(ant_value_t));
      memcpy(fn->constants, comp.constants, (size_t)comp.const_count * sizeof(ant_value_t));
      fn->const_count = comp.const_count;
      build_gc_const_tables(fn);
    }
    if (comp.atom_count > 0) {
      fn->atoms = code_arena_bump((size_t)comp.atom_count * sizeof(sv_atom_t));
      memcpy(fn->atoms, comp.atoms, (size_t)comp.atom_count * sizeof(sv_atom_t));
      fn->atom_count = comp.atom_count;
    }
    fn->ic_count = (uint16_t)comp.ic_count;
    if (fn->ic_count > 0) {
      fn->ic_slots = code_arena_bump((size_t)fn->ic_count * sizeof(sv_ic_entry_t));
      memset(fn->ic_slots, 0, (size_t)fn->ic_count * sizeof(sv_ic_entry_t));
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
    fn->local_type_count = fn->max_locals;
    if (fn->max_locals > 0) {
      fn->local_types = code_arena_bump((size_t)fn->max_locals * sizeof(sv_type_info_t));
      memset(fn->local_types, 0, (size_t)fn->max_locals * sizeof(sv_type_info_t));
      if (comp.slot_types) {
        int ncopy = fn->max_locals < comp.slot_type_cap ? fn->max_locals : comp.slot_type_cap;
        memcpy(fn->local_types, comp.slot_types, (size_t)ncopy * sizeof(sv_type_info_t));
      }
    }
    
    fn->param_count = (uint16_t)comp.param_count;
    fn->function_length = (uint16_t)comp.param_count;
    fn->is_strict = comp.is_strict;
    fn->filename = c->js->filename;
    fn->source_line = (int)node->line;
    
    if (node->str && node->len > 0) {
      char *name = code_arena_bump(node->len + 1);
      memcpy(name, node->str, node->len);
      name[node->len] = '\0';
      fn->name = name;
    }
    
    sv_compile_ctx_cleanup(&comp);
    int idx = add_constant(c, mkval(T_NTARG, (uintptr_t)fn));
    emit_op(c, OP_CLOSURE);
    emit_u32(c, (uint32_t)idx);
  } else emit_op(c, OP_UNDEF);
  
  free(field_inits);
  free(computed_key_locals);
  c->computed_key_locals = NULL;

  const char *class_name = node->str;
  uint32_t class_name_len = node->len;
  bool consumed_inferred_name = false;
  
  if (!class_name && c->inferred_name) {
    class_name = c->inferred_name;
    class_name_len = c->inferred_name_len;
    consumed_inferred_name = true;
  }

  if (!has_static_name) {
    int atom = class_name
      ? add_atom(c, class_name, class_name_len)
      : add_atom(c, "", 0);
    emit_op(c, OP_DEFINE_CLASS);
    emit_u32(c, (uint32_t)atom);
    emit(c, 1);
  } else {
    emit_op(c, OP_DEFINE_CLASS);
    emit_u32(c, 0);
    emit(c, 0);
  }
  
  if (consumed_inferred_name) {
    c->inferred_name = NULL;
    c->inferred_name_len = 0;
  }
  
  emit_u32(c, node->src_off);
  emit_u32(c, node->src_end);

  int proto_local = add_local(c, "", 0, false, c->scope_depth);
  int ctor_local = add_local(c, "", 0, false, c->scope_depth);
  emit_put_local(c, proto_local);
  emit_put_local(c, ctor_local);

  if (inner_name_local >= 0) {
    emit_get_local(c, ctor_local);
    emit_put_local(c, inner_name_local);
  }

  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *m = node->args.items[i];
    if (m->type == N_STATIC_BLOCK) {
      compile_static_block(c, m, ctor_local);
      continue;
    }
    
    if (m->type != N_METHOD) continue;
    if (m == ctor_method) continue;
    if (is_private_name_node(m->left)) {
      if (m->flags & FN_STATIC) {
        bool saved_strict = c->is_strict;
        c->is_strict = true;
        compile_private_static_element(c, m, ctor_local);
        c->is_strict = saved_strict;
      }
      continue;
    }
    
    bool is_fn = is_class_method_def(m);
    if (!is_fn && !(m->flags & FN_STATIC)) continue;
    
    bool saved_strict = c->is_strict;
    c->is_strict = true;
    compile_class_method(
      c, m, ctor_local, proto_local, 
      method_comp_keys ? method_comp_keys[i] : -1
    );
    c->is_strict = saved_strict;
  }

  free(method_comp_keys);
  emit_get_local(c, ctor_local);

  if (class_repl_top && node->str) {
    emit_op(c, OP_DUP);
    emit_atom_op(c, OP_PUT_GLOBAL, node->str, node->len);
  } else if (outer_name_local >= 0) {
    emit_op(c, OP_DUP);
    emit_put_local(c, outer_name_local);
    c->locals[outer_name_local].is_tdz = false;
  }

  if (has_class_scope) end_scope(c);
  c->private_scope = saved_private_scope;
  free(private_scope.names);
}

static bool ast_contains_await_expr(const sv_ast_t *node) {
  if (!node) return false;
  
  if (node->type == N_AWAIT) return true;
  if (node->type == N_FUNC) return false;

  if (ast_contains_await_expr(node->left))         return true;
  if (ast_contains_await_expr(node->right))        return true;
  if (ast_contains_await_expr(node->cond))         return true;
  if (ast_contains_await_expr(node->body))         return true;
  if (ast_contains_await_expr(node->catch_body))   return true;
  if (ast_contains_await_expr(node->finally_body)) return true;
  if (ast_contains_await_expr(node->catch_param))  return true;
  if (ast_contains_await_expr(node->init))         return true;
  if (ast_contains_await_expr(node->update))       return true;

  for (int i = 0; i < node->args.count; i++) {
    if (ast_contains_await_expr(node->args.items[i])) return true;
  }

  return false;
}

static bool func_params_contain_await(const sv_ast_t *node) {
  if (!node) return false;
  for (int i = 0; i < node->args.count; i++) {
    if (ast_contains_await_expr(node->args.items[i])) return true;
  }
  return false;
}

static uint16_t function_length_from_params(const sv_ast_t *node) {
  if (!node) return 0;
  uint16_t length = 0;
  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *param = node->args.items[i];
    if (!param || param->type == N_REST || param->type == N_ASSIGN_PAT) break;
    length++;
  }
  return length;
}

sv_func_t *compile_function_body(
  sv_compiler_t *enclosing,
  sv_ast_t *node,
  sv_compile_mode_t mode
) {
  if (node->args.count > UINT16_MAX) {
    js_mkerr_typed(
      enclosing->js, JS_ERR_SYNTAX,
      "too many function parameters");
    return NULL;
  }

  if ((node->flags & FN_ASYNC) && func_params_contain_await(node)) {
    js_mkerr_typed(
      enclosing->js, JS_ERR_SYNTAX,
      "await is not allowed in async function parameters");
    return NULL;
  }

  sv_compiler_t comp;
  sv_compile_ctx_init_child(&comp, enclosing, node, mode);

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

  bool has_own_use_strict = false;
  if (node->body && node->body->type == N_BLOCK) for (int i = 0; i < node->body->args.count; i++) {
    sv_ast_t *stmt = node->body->args.items[i];
    if (!stmt || stmt->type == N_EMPTY) continue;
    if (stmt->type != N_STRING) break;
    if (sv_ast_is_use_strict(comp.js, stmt)) {
      has_own_use_strict = true;
      comp.is_strict = true;
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

  if (has_own_use_strict && has_non_simple_params) {
    js_mkerr_typed(
      comp.js, JS_ERR_SYNTAX,
      "Illegal 'use strict' directive in function with non-simple parameter list");
    return NULL;
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
        set_local_inferred_type(&comp, loc, SV_TI_UNKNOWN);
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
          set_local_inferred_type(&comp, bind_lb, SV_TI_UNKNOWN);
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
          set_local_inferred_type(&comp, bind_lb, SV_TI_UNKNOWN);
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
        set_local_inferred_type(&comp, bind_lb, SV_TI_UNKNOWN);
      } else if (p->type == N_REST && p->right) {
        emit_op(&comp, OP_REST);
        emit_u16(&comp, (uint16_t)i);
        compile_destructure_binding(&comp, p->right, SV_VAR_LET);
        emit_op(&comp, OP_POP);
      }
    }

    free(param_bind_locals);
    sv_ast_t *body = node->body;

    if (body && body->type != N_BLOCK) {
      if (!repl_top) hoist_var_decls(&comp, body);
    } else if (body) {
      if (!repl_top) {
        for (int i = 0; i < body->args.count; i++)
          hoist_var_decls(&comp, body->args.items[i]);
        hoist_lexical_decls(&comp, &body->args);
      }
      hoist_func_decls(&comp, &body->args);
    }
  }

  if (!comp.is_arrow && has_implicit_arguments_obj(&comp) && (node->flags & FN_USES_ARGS)) {
    static const char args_name[] = "\x01arguments";
    comp.strict_args_local = add_local(&comp, args_name, sizeof(args_name) - 1, false, comp.scope_depth);
    emit_op(&comp, OP_SPECIAL_OBJ);
    emit(&comp, 0);
    emit_put_local(&comp, comp.strict_args_local);
  }

  if (!comp.is_arrow && comp.enclosing && (node->flags & FN_USES_NEW_TARGET)) {
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

  bool body_has_await_using = false;
  bool body_has_using = node->body && node->body->type == N_BLOCK &&
    stmt_list_has_using_decl(&node->body->args, &body_has_await_using);
  int body_using_try_jump = -1;
  int body_using_err_local = -1;
  int old_using_stack = comp.using_stack_local;
  bool old_using_async = comp.using_stack_async;

  if (body_has_using) {
    emit_empty_disposal_stack(&comp);
    int body_using_stack = add_local(&comp, "", 0, false, comp.scope_depth);
    emit_put_local(&comp, body_using_stack);
    body_using_err_local = add_local(&comp, "", 0, false, comp.scope_depth);

    comp.using_stack_local = body_using_stack;
    comp.using_stack_async = body_has_await_using;
    push_using_cleanup(&comp, body_using_stack, comp.scope_depth, body_has_await_using);

    comp.try_depth++;
    body_using_try_jump = emit_jump(&comp, OP_TRY_PUSH);
  }

  bool completion_top = is_completion_top_level(&comp);
  if (completion_top) {
    emit_op(&comp, OP_UNDEF);
    comp.completion_local = add_local(&comp, "", 0, false, comp.scope_depth);
    emit_put_local(&comp, comp.completion_local);
  }

  if (node->body) {
    if (node->body->type == N_BLOCK) {
      for (int i = 0; i < node->body->args.count; i++)
        compile_stmt(&comp, node->body->args.items[i]);
    } else compile_tail_return_expr(&comp, node->body);
  }

  for (int i = 0; i < comp.deferred_export_count; i++) {
    sv_deferred_export_t *e = &comp.deferred_exports[i];
    compile_export_emit(&comp, e->name, e->len);
  }

  if (body_has_using) {
    emit_op(&comp, OP_TRY_POP);
    comp.try_depth--;

    emit_using_dispose_call(&comp, comp.using_stack_local, -1, body_has_await_using, false);
    emit_op(&comp, OP_POP);
    int end_jump = emit_jump(&comp, OP_JMP);

    patch_jump(&comp, body_using_try_jump);
    int catch_tag = emit_jump(&comp, OP_CATCH);
    emit_put_local(&comp, body_using_err_local);
    emit_using_dispose_call(&comp, comp.using_stack_local, body_using_err_local, body_has_await_using, true);
    if (!body_has_await_using) emit_op(&comp, OP_THROW);
    patch_jump(&comp, catch_tag);
    patch_jump(&comp, end_jump);

    pop_using_cleanup(&comp);
    comp.using_stack_local = old_using_stack;
    comp.using_stack_async = old_using_async;
  }
  
  if (completion_top) {
    emit_get_local(&comp, comp.completion_local);
    emit_return_from_stack(&comp);
  }

  emit_using_cleanups_to_depth(&comp, -1);
  emit_close_upvals(&comp);
  emit_op(&comp, OP_RETURN_UNDEF);

  int max_locals = comp.max_local_count - comp.param_locals;
  sv_func_t *func = code_arena_bump(sizeof(sv_func_t));
  memset(func, 0, sizeof(sv_func_t));

  func->code = code_arena_bump((size_t)comp.code_len);
  memcpy(func->code, comp.code, (size_t)comp.code_len);
  func->code_len = comp.code_len;
  sv_func_init_obj_sites(func);

  if (comp.const_count > 0) {
    func->constants = code_arena_bump((size_t)comp.const_count * sizeof(ant_value_t));
    memcpy(func->constants, comp.constants, (size_t)comp.const_count * sizeof(ant_value_t));
    func->const_count = comp.const_count;
    build_gc_const_tables(func);
  }

  if (comp.atom_count > 0) {
    func->atoms = code_arena_bump((size_t)comp.atom_count * sizeof(sv_atom_t));
    memcpy(func->atoms, comp.atoms, (size_t)comp.atom_count * sizeof(sv_atom_t));
    func->atom_count = comp.atom_count;
  }

  func->ic_count = (uint16_t)comp.ic_count;
  if (func->ic_count > 0) {
    func->ic_slots = code_arena_bump((size_t)func->ic_count * sizeof(sv_ic_entry_t));
    memset(func->ic_slots, 0, (size_t)func->ic_count * sizeof(sv_ic_entry_t));
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
  func->local_type_count = max_locals;
  if (max_locals > 0) {
    func->local_types = code_arena_bump((size_t)max_locals * sizeof(sv_type_info_t));
    memset(func->local_types, 0, (size_t)max_locals * sizeof(sv_type_info_t));
    if (comp.slot_types) {
      int ncopy = max_locals < comp.slot_type_cap ? max_locals : comp.slot_type_cap;
      memcpy(func->local_types, comp.slot_types, (size_t)ncopy * sizeof(sv_type_info_t));
    }
  }
  
  func->param_count = (uint16_t)comp.param_count;
  func->function_length = function_length_from_params(node);
  func->is_strict = comp.is_strict;
  func->is_arrow = comp.is_arrow;
  
  func->is_async = !!(node->flags & FN_ASYNC);
  func->has_await = false;
  func->is_generator = !!(node->flags & FN_GENERATOR);
  func->is_method = !!(node->flags & FN_METHOD);
  func->is_static = !!(node->flags & FN_STATIC);
  func->is_tla = comp.is_tla;
  func->filename = enclosing->filename ? enclosing->filename : enclosing->js->filename;
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

  if (func->is_async || func->is_tla) {
  const uint8_t *ip = func->code;
  const uint8_t *end = func->code + func->code_len;
  while (ip < end) {
    sv_op_t op = (sv_op_t)*ip;
    if (op == OP_AWAIT || op == OP_FOR_AWAIT_OF || op == OP_AWAIT_ITER_NEXT) {
      func->has_await = true;
      break;
    }
    int sz = sv_op_size[op];
    if (sz <= 0) break;
    ip += sz;
  }}

  sv_compile_ctx_cleanup(&comp);
  return func;
}

const char *const sv_op_names[OP__COUNT] = {
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
  fprintf(stderr, "Frame size %d\n", func->max_locals * (int)sizeof(ant_value_t));

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
      if ((op == OP_GET_GLOBAL || op == OP_GET_GLOBAL_UNDEF ||
           op == OP_GET_FIELD || op == OP_GET_FIELD2 || op == OP_PUT_FIELD) && size >= 7)
        fprintf(stderr, " ic[%u]", sv_get_u16(func->code + pc + 5));
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
    ant_value_t v = func->constants[i];
    uint8_t t = vtype(v);
    if (t == T_STR) {
      ant_offset_t slen;
      ant_offset_t soff = vstr(js, v, &slen);
      fprintf(stderr, "           %d: <String[%d]: #%.*s>\n", i, (int)slen, (int)slen, (const char *)(uintptr_t)soff);
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
  if (vtype(func->constants[i]) == T_NTARG) {
    sv_func_t *child = (sv_func_t *)(uintptr_t)vdata(func->constants[i]);
    char child_label[256];
    snprintf(child_label, sizeof(child_label), "%s/closure[%d]", label, i);
    sv_disasm(js, child, child_label);
  }}
}

sv_func_t *sv_compile_with_commonjs_retry(
  ant_t *js, sv_ast_t *program,
  sv_compile_mode_t mode,
  const char *source, ant_offset_t source_len,
  bool allow_commonjs_retry,
  bool *out_retry_commonjs
) {
  if (out_retry_commonjs) *out_retry_commonjs = false;
  if (!program || program->type != N_PROGRAM) return NULL;
  if (sv_compile_trace_unlikely) fprintf(
    stderr, "[compile] start kind=program mode=%d len=%u body=%d strict=%d\n",
    (int)mode, (unsigned)source_len,
    program->args.count, (program->flags & FN_PARSE_STRICT) != 0 ? 1 : 0
  );
  
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

  sv_compiler_t root;
  sv_compile_ctx_init_root(
    &root, js, js->filename,
    pin_source_text(source, source_len),
    source_len, mode,
    (program->flags & FN_PARSE_STRICT) != 0,  NULL
  );
  root.commonjs_retry_allowed = allow_commonjs_retry && mode == SV_COMPILE_MODULE;
  
  root.line_table = sv_compile_ctx_build_line_table(root.source, source_len);
  sv_func_t *func = compile_function_body(&root, &top_fn, mode);
  sv_compile_ctx_free_line_table(root.line_table);

  if (!js->thrown_exists && func &&
      root.commonjs_retry_requested && !root.module_syntax_seen) {
    if (out_retry_commonjs) *out_retry_commonjs = true;
    return NULL;
  }
  
  if (sv_compile_trace_unlikely) fprintf(
    stderr, "[compile] end kind=program mode=%d thrown=%d func=%p\n",
    (int)mode, js->thrown_exists ? 1 : 0, (void *)func
  );
  
  if (js->thrown_exists || !func) return NULL;
  return func;
}

sv_func_t *sv_compile(ant_t *js, sv_ast_t *program, sv_compile_mode_t mode, const char *source, ant_offset_t source_len) {
  return sv_compile_with_commonjs_retry(
    js, program, mode, source, source_len, false, NULL
  );
}

sv_func_t *sv_compile_function(ant_t *js, const char *source, size_t len, bool is_async, bool is_generator) {
  if (sv_compile_trace_unlikely) fprintf(
    stderr, "[compile] start kind=function len=%u async=%d generator=%d\n",
    (unsigned)len, is_async ? 1 : 0, is_generator ? 1 : 0
  );
  
  const char *prefix = is_async
    ? (is_generator ? "(async function*" : "(async function")
    : (is_generator ? "(function*" : "(function");
    
  size_t prefix_len = strlen(prefix);
  size_t wrapped_len = prefix_len + len + 1;

  char *wrapped = malloc(wrapped_len + 1);
  if (!wrapped) return NULL;

  memcpy(wrapped, prefix, prefix_len);
  memcpy(wrapped + prefix_len, source, len);
  wrapped[prefix_len + len] = ')';
  wrapped[wrapped_len] = '\0';
  
  bool parse_strict = sv_vm_is_strict(js->vm);
  code_arena_mark_t parse_mark = parse_arena_mark();
  sv_ast_t *program = sv_parse(js, wrapped, (ant_offset_t)wrapped_len, parse_strict);
  
  if (!program) {
    parse_arena_rewind(parse_mark);
    free(wrapped);
    return NULL;
  }

  sv_ast_t *func_node = NULL;
  if (program->args.count > 0) {
    sv_ast_t *stmt = program->args.items[0];
    if (stmt && stmt->type == N_FUNC)  func_node = stmt;
    else if (stmt && stmt->left && stmt->left->type == N_FUNC) func_node = stmt->left;
  }

  if (!func_node) {
    parse_arena_rewind(parse_mark);
    free(wrapped);
    return NULL;
  }

  sv_compiler_t root;
  sv_compile_ctx_init_root(
    &root, js, js->filename,
    pin_source_text(wrapped, (ant_offset_t)wrapped_len),
    (ant_offset_t)wrapped_len, SV_COMPILE_SCRIPT,
    (program->flags & FN_PARSE_STRICT) != 0, NULL
  );
  
  root.line_table = sv_compile_ctx_build_line_table(root.source, (ant_offset_t)wrapped_len);
  sv_func_t *func = compile_function_body(&root, func_node, SV_COMPILE_SCRIPT);
  
  sv_compile_ctx_free_line_table(root.line_table);
  parse_arena_rewind(parse_mark);
  free(wrapped);

  if (sv_compile_trace_unlikely) fprintf(
    stderr, "[compile] end kind=function thrown=%d func=%p\n",
    js->thrown_exists ? 1 : 0, (void *)func
  );
    
  if (js->thrown_exists || !func) return NULL;
  return func;
}

sv_func_t *sv_compile_function_with_params(
  ant_t *js,
  const sv_param_t *params,
  int param_count,
  const char *body,
  size_t body_len,
  bool is_async
) {
  if (sv_compile_trace_unlikely) fprintf(
    stderr, "[compile] start kind=function-with-params len=%u params=%d async=%d\n",
    (unsigned)body_len, param_count, is_async ? 1 : 0
  );

  if (!body) {
    body = "";
    body_len = 0;
  }

  bool parse_strict = sv_vm_is_strict(js->vm);
  code_arena_mark_t parse_mark = parse_arena_mark();
  sv_ast_t *program = sv_parse(js, body, (ant_offset_t)body_len, parse_strict);
  
  if (!program) {
    parse_arena_rewind(parse_mark);
    return NULL;
  }

  static const char *k_top_name_function = "<function>";
  static const char *k_top_name_async_function = "<async function>";
  const char *top_name = is_async ? k_top_name_async_function : k_top_name_function;

  sv_ast_t top_fn;
  memset(&top_fn, 0, sizeof(top_fn));

  top_fn.type = N_FUNC;
  top_fn.line = 1;
  top_fn.str = top_name;
  top_fn.len = (uint32_t)strlen(top_name);
  top_fn.src_off = 0;
  top_fn.src_end = (body_len > 0) ? (uint32_t)body_len : 0;
  if (is_async) top_fn.flags |= FN_ASYNC;

  for (int i = 0; i < param_count; i++) {
    const char *name = (params && params[i].name) ? params[i].name : "";
    size_t name_len = 0;
    if (params && params[i].name) {
      name_len = params[i].len ? params[i].len : strlen(name);
    }

    sv_ast_t *ident = sv_ast_new(N_IDENT);
    if (!ident) {
      parse_arena_rewind(parse_mark);
      return NULL;
    }

    ident->str = name;
    ident->len = (uint32_t)name_len;
    ident->line = 1;
    ident->col = 1;
    sv_ast_list_push(&top_fn.args, ident);
  }

  top_fn.body = sv_ast_new(N_BLOCK);
  if (!top_fn.body) {
    parse_arena_rewind(parse_mark);
    return NULL;
  }
  
  top_fn.body->args = program->args;
  sv_compiler_t root;
  
  sv_compile_ctx_init_root(
    &root, js, js->filename,
    pin_source_text(body, (ant_offset_t)body_len),
    (ant_offset_t)body_len, SV_COMPILE_SCRIPT,
    (program->flags & FN_PARSE_STRICT) != 0, NULL
  );
  
  root.line_table = sv_compile_ctx_build_line_table(root.source, (ant_offset_t)body_len);
  sv_func_t *func = compile_function_body(&root, &top_fn, SV_COMPILE_SCRIPT);
  
  sv_compile_ctx_free_line_table(root.line_table);
  parse_arena_rewind(parse_mark);
  
  if (sv_compile_trace_unlikely) fprintf(
    stderr, "[compile] end kind=function-with-params thrown=%d func=%p\n",
    js->thrown_exists ? 1 : 0, (void *)func
  );
  
  if (js->thrown_exists || !func) return NULL;
  return func;
}
