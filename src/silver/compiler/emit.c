#include "compiler_internal.h"

const char *copy_source_text(ant_t *js, const char *source, ant_offset_t source_len) {
  if (!source || source_len <= 0) return source;
  const char *copy = sv_code_text(js, source, (size_t)source_len);
  if (!copy) js_mkerr(js, "out of memory copying source text");
  return copy;
}

void emit(sv_compiler_t *c, uint8_t byte) {
  if (c->code_len >= c->code_cap) {
    c->code_cap = c->code_cap ? c->code_cap * 2 : 256;
    c->code = realloc(c->code, (size_t)c->code_cap);
  }
  c->code[c->code_len++] = byte;
}

void emit_u16(sv_compiler_t *c, uint16_t val) {
  emit(c, (uint8_t)(val & 0xFF));
  emit(c, (uint8_t)(val >> 8));
}

void emit_u32(sv_compiler_t *c, uint32_t val) {
  emit(c, (uint8_t)(val & 0xFF));
  emit(c, (uint8_t)((val >> 8) & 0xFF));
  emit(c, (uint8_t)((val >> 16) & 0xFF));
  emit(c, (uint8_t)((val >> 24) & 0xFF));
}

void emit_i32(sv_compiler_t *c, int32_t val) {
  uint32_t u;
  memcpy(&u, &val, 4);
  emit_u32(c, u);
}

void emit_op(sv_compiler_t *c, sv_op_t op) {
  emit(c, (uint8_t)op);
}

uint32_t source_line_of(const sv_compiler_t *c, const sv_ast_t *node) {
  if (!node || !c->line_table || c->source_len <= 0) return 0;
  uint32_t off = node->src_off < (uint32_t)c->source_len ? node->src_off : (uint32_t)c->source_len;
  uint32_t line, col;
  sv_compile_ctx_line_table_lookup(c->line_table, off, &line, &col);
  return line;
}

void emit_srcpos(sv_compiler_t *c, sv_ast_t *node) {
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
      else col += utf16_units_for_utf8_byte((unsigned char)code[i]);
    }
  } else {
    line = 1; col = 1;
    for (uint32_t i = 0; i < off; i++) {
      if (code[i] == '\n') { line++; col = 1; }
      else col += utf16_units_for_utf8_byte((unsigned char)code[i]);
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

int add_constant(sv_compiler_t *c, ant_value_t val) {
  if (vtype(val) == kTypeString) {
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

int add_constant_run(sv_compiler_t *c, const ant_value_t *vals, int count) {
  if (c->const_count + count > c->const_cap) {
    int cap = c->const_cap ? c->const_cap : 16;
    while (cap < c->const_count + count) cap *= 2;
    
    ant_value_t *grown = realloc(c->constants, (size_t)cap * sizeof(ant_value_t));
    if (!grown) return -1;
    
    c->constants = grown;
    c->const_cap = cap;
  }
  
  int start = c->const_count;
  memcpy(c->constants + start, vals, (size_t)count * sizeof(ant_value_t));
  c->const_count += count;
  
  return start;
}

bool build_gc_const_tables(ant_t *js, sv_func_t *func) {
  if (!func || func->const_count <= 0 || !func->constants) return true;

  int child_count = 0;
  for (int i = 0; i < func->const_count; i++) {
    if (vtype(func->constants[i]) == kTypeFunctionInfo) child_count++;
  }

  if (child_count > 0) {
    func->child_funcs = sv_code_bump(js, (size_t)child_count * sizeof(sv_func_t *));
    if (!func->child_funcs) return false;
    
    int out = 0;
    for (int i = 0; i < func->const_count; i++) {
      if (vtype(func->constants[i]) != kTypeFunctionInfo) continue;
      sv_func_t *child = (sv_func_t *)vptr(func->constants[i]);
      child->parent = func;
      func->child_funcs[out++] = child;
    }
    
    func->child_func_count = child_count;
  }

  uint8_t *marked_slots = calloc((size_t)func->const_count, sizeof(uint8_t));
  if (!marked_slots) return false;
  int slot_count = 0;

  for (int pc = 0; pc < func->code_len;) {
    sv_op_t op = (sv_op_t)func->code[pc];
    int size = (op < OP__COUNT) ? sv_op_size[op] : 0;
    if (size <= 0) break;

    if (op == OP_PUT_CONST) {
      uint32_t idx = sv_get_u32(func->code + pc + 1);
      if (idx < (uint32_t)func->const_count && !marked_slots[idx]) {
        marked_slots[idx] = 1; 
        slot_count++;
      }
    }

    pc += size;
  }

  if (func->unit) for (int i = 0; i < func->const_count; i++) {
    uint8_t type = vtype(func->constants[i]);
    if ((type == kTypeString || type == kTypeBigInt) && !marked_slots[i]) {
      marked_slots[i] = 1;
      slot_count++;
    }
  }

  if (slot_count > 0) {
    func->gc_const_slots = sv_code_bump(js, (size_t)slot_count * sizeof(uint32_t));
    if (!func->gc_const_slots) {
      free(marked_slots);
      return false;
    }
    
    int out = 0;
    for (int i = 0; i < func->const_count; i++) {
      if (!marked_slots[i]) continue;
      func->gc_const_slots[out++] = (uint32_t)i;
    }
    
    func->gc_const_slot_count = slot_count;
  }

  free(marked_slots);
  return true;
}

sv_ic_entry_t *alloc_ic_slots(ant_t *js, uint16_t count) {
  if (count == 0) return NULL;
  sv_ic_entry_t *slots = sv_code_bump(js, (size_t)count * sizeof(*slots));
  if (!slots) return NULL;
  
  memset(slots, 0, (size_t)count * sizeof(*slots));
  if (js->code_units.active) for (uint16_t i = 0; i < count; i++)
    slots[i].shape_ref_mask = SV_IC_UNIT_OWNED;
  
  return slots;
}

void emit_constant(sv_compiler_t *c, ant_value_t val) {
  int idx = add_constant(c, val);
  if (idx <= 255) {
    emit_op(c, OP_CONST8);
    emit(c, (uint8_t)idx);
  } else {
    emit_op(c, OP_CONST);
    emit_u32(c, (uint32_t)idx);
  }
}

void emit_number(sv_compiler_t *c, double num) {
  if (num >= -128 && num <= 127 && num == (int)num && num != -0.0) {
    emit_op(c, OP_CONST_I8);
    emit(c, (uint8_t)(int8_t)num);
  } else emit_constant(c, Ant_Value_FromNumber(num));
}

const char *copy_name(ant_t *js, const char *name, uint32_t len) {
  if (!name || len == 0) return name;
  char *copy = sv_code_bump(js, len);
  
  if (!copy) {
    js_mkerr(js, "out of memory copying a binding name");
    return NULL;
  }
  
  memcpy(copy, name, len);
  return copy;
}

sv_func_t *sv_func_new(ant_t *js) {
  sv_func_t *func = sv_code_bump(js, sizeof(*func));
  Ant_Assert(func != NULL, "failed to allocate a function");
  
  memset(func, 0, sizeof(*func));
  func->realm = js->realm;
  sv_code_unit_add_func(js, func);
  
  func->debug = sv_code_bump(js, sizeof(*func->debug));
  Ant_Assert(func->debug != NULL, "failed to allocate function debug info");
  memset(func->debug, 0, sizeof(*func->debug));
  
  return func;
}

ant_value_t sv_const_str(sv_compiler_t *c, const char *ptr, size_t len) {
  if (!c->js->code_units.active) return js_mkstr_permanent(c->js, ptr, len);
  ant_value_t str = js_mkstr(c->js, ptr, len);
  if (!is_err(str) && !sv_code_unit_root(c->js, str)) return js_mkerr(c->js, "oom");
  return str;
}

ant_value_t sv_const_bigint(sv_compiler_t *c, const char *digits, size_t len, bool negative) {
  if (!c->js->code_units.active) return js_mkbigint_permanent(c->js, digits, len, negative);
  ant_value_t big = js_mkbigint(c->js, digits, len, negative);
  if (!is_err(big) && !sv_code_unit_root(c->js, big)) return js_mkerr(c->js, "oom");
  return big;
}

static constexpr int ATOM_SCAN_MAX = 16;

static uint32_t atom_slot(const sv_compiler_t *c, const char *str) {
  uint64_t h = ant_hash_mix((uint64_t)(uintptr_t)str ^ ant_hash_secret[0], ant_hash_secret[1]);
  return (uint32_t)h & (c->atom_index_cap - 1);
}

static void atom_index_insert(sv_compiler_t *c, int atom) {
  uint32_t slot = atom_slot(c, c->atoms[atom].str);
  while (c->atom_index[slot]) slot = (slot + 1) & (c->atom_index_cap - 1);
  c->atom_index[slot] = (uint32_t)atom + 1;
}

static bool atom_index_reserve(sv_compiler_t *c) {
  if ((uint32_t)(c->atom_count + 1) * 2 <= c->atom_index_cap) return true;
  uint32_t cap = ATOM_SCAN_MAX * 4;
  
  while ((uint32_t)(c->atom_count + 1) * 2 > cap) cap *= 2;
  uint32_t *index = calloc(cap, sizeof(*index));
  
  free(c->atom_index);
  c->atom_index = NULL;
  c->atom_index_cap = 0;
  
  if (!index) return false;
  c->atom_index = index;
  c->atom_index_cap = cap;
  
  for (int i = 0; i < c->atom_count; i++) atom_index_insert(c, i);
  
  return true;
}

int add_atom(sv_compiler_t *c, const char *str, uint32_t len) {
  const char *interned = intern_string(c->js, str, (size_t)len);
  const char *stored = interned;
  
  if (!stored) {
    char *copy = code_arena_bump(c->js, len);
    memcpy(copy, str, len);
    stored = copy;
  }

  bool indexed = c->atom_count >= ATOM_SCAN_MAX && atom_index_reserve(c);
  if (indexed) {
    for (uint32_t slot = atom_slot(c, stored); c->atom_index[slot]; slot = (slot + 1) & (c->atom_index_cap - 1)) {
      const sv_atom_t *atom = &c->atoms[c->atom_index[slot] - 1];
      if (atom->str == stored && atom->len == len) return (int)c->atom_index[slot] - 1;
    }
  } else {
    for (int i = 0; i < c->atom_count; i++) 
      if (c->atoms[i].len == len && c->atoms[i].str == stored) return i;
  }

  if (c->atom_count >= c->atom_cap) {
    c->atom_cap = c->atom_cap ? c->atom_cap * 2 : 16;
    c->atoms = realloc(c->atoms, (size_t)c->atom_cap * sizeof(sv_atom_t));
  }
  
  c->atoms[c->atom_count] = (sv_atom_t){ 
    .str = stored, 
    .len = len,
    .maybe_numeric = ant_key_may_be_numeric(stored, len)
  };
  
  if (indexed) atom_index_insert(c, c->atom_count);
  
  return c->atom_count++;
}

uint32_t add_map_template_desc(
  sv_compiler_t *c, sv_map_template_operation_t operation,
  const sv_ast_t *tpl, uint8_t substitution_count
) {
  if (c->map_template_count >= c->map_template_cap) {
    c->map_template_cap = c->map_template_cap ? c->map_template_cap * 2 : 8;
    c->map_templates = realloc(
    c->map_templates,
    (size_t)c->map_template_cap * sizeof(sv_map_template_desc_t));
  }

  uint32_t index = (uint32_t)c->map_template_count++;
  sv_map_template_desc_t *desc = &c->map_templates[index];
  
  memset(desc, 0, sizeof(*desc));
  desc->operation = (uint8_t)operation;
  desc->substitution_count = substitution_count;
  
  for (uint8_t i = 0; i <= substitution_count; i++) {
    const sv_ast_t *segment = tpl->args.items[i * 2];
    int atom_idx = add_atom(
      c, segment->str ? segment->str : "", segment->len);
    desc->segments[i] = c->atoms[atom_idx];
  }
  
  return index;
}

void emit_atom_idx_op(sv_compiler_t *c, sv_op_t op, uint32_t atom_idx) {
  emit_op(c, op);
  emit_u32(c, atom_idx);
  if (sv_op_has_ic_slot(op))
    emit_u16(c, alloc_ic_idx(c));
}

void emit_get_length(sv_compiler_t *c) {
  emit_op(c, OP_GET_LENGTH);
  emit_u16(c, alloc_ic_idx(c));
}

void emit_atom_op(sv_compiler_t *c, sv_op_t op, const char *str, uint32_t len) {
  int idx = add_atom(c, str, len);
  emit_atom_idx_op(c, op, (uint32_t)idx);
}

void compile_expr_with_inferred_name(
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

void emit_throw_type_error(sv_compiler_t *c, const char *msg, uint32_t len) {
  int atom = add_atom(c, msg, len);
  emit_op(c, OP_THROW_ERROR);
  emit_u32(c, (uint32_t)atom);
  emit(c, (uint8_t)JS_ERR_TYPE);
}

void emit_const_assign_error(sv_compiler_t *c, const char *name, uint32_t len) {
  static const char prefix[] = "Assignment to constant variable '";
  static const char suffix[] = "'";
  
  uint32_t mlen = (uint32_t)(sizeof(prefix) - 1 + len + sizeof(suffix) - 1);
  char *buf = sv_code_bump(c->js, mlen);
  
  memcpy(buf, prefix, sizeof(prefix) - 1);
  memcpy(buf + sizeof(prefix) - 1, name, len);
  memcpy(buf + sizeof(prefix) - 1 + len, suffix, sizeof(suffix) - 1);
  
  int atom = add_atom(c, buf, mlen);
  emit_op(c, OP_THROW_ERROR);
  emit_u32(c, (uint32_t)atom);
  emit(c, (uint8_t)JS_ERR_TYPE);
}

int emit_jump(sv_compiler_t *c, sv_op_t op) {
  emit_op(c, op);
  int offset = c->code_len;
  emit_i32(c, 0);
  return offset;
}

void patch_jump(sv_compiler_t *c, int offset) {
  int32_t delta = (int32_t)(c->code_len - offset - 4);
  patch_u32(c, offset, (uint32_t)delta);
}

void emit_loop(sv_compiler_t *c, int loop_start) {
  emit_op(c, OP_JMP);
  int32_t delta = (int32_t)(loop_start - c->code_len - 4);
  emit_i32(c, delta);
}

void patch_list_add(sv_patch_list_t *pl, int offset) {
  if (pl->count >= pl->cap) {
    pl->cap = pl->cap ? pl->cap * 2 : 4;
    pl->offsets = realloc(pl->offsets, (size_t)pl->cap * sizeof(int));
  }
  pl->offsets[pl->count++] = offset;
}

void patch_list_resolve(sv_compiler_t *c, sv_patch_list_t *pl) {
  for (int i = 0; i < pl->count; i++)
    patch_jump(c, pl->offsets[i]);
  free(pl->offsets);
  *pl = (sv_patch_list_t){0};
}
