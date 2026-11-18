#include "compiler_internal.h"

static bool has_global_lexical(const sv_compiler_t *c, const char *name, uint32_t len) {
  for (uint32_t i = 0; i < c->global_lexical_count; i++)
    if (c->global_lexicals[i].len == len && !memcmp(c->global_lexicals[i].str, name, len)) return true;
  return false;
}

static void add_global_lexical(sv_compiler_t *c, const char *name, uint32_t len, bool is_const) {
  if (has_global_lexical(c, name, len)) {
    js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Identifier '%.*s' has already been declared", (int)len, name);
    return;
  }
  
  sv_eval_decl_t *decls = realloc(
    c->global_lexicals, ((size_t)c->global_lexical_count + 1) * sizeof(*decls));
  if (!decls) { js_mkerr(c->js, "out of memory while declaring global lexicals"); return; }
  c->global_lexicals = decls;
  int atom = add_atom(c, name, len);
  c->global_lexicals[c->global_lexical_count++] =
    (sv_eval_decl_t){ .str = c->atoms[atom].str, .len = len, .is_const = is_const };
}

static void decl_names_add(sv_decl_names_t *names, const char *str, uint32_t len, bool is_function) {
  if (!str || !len) return;
  if (names->count == names->cap) {
    int cap = names->cap * 2;
    sv_decl_name_t *items = names->items == names->inline_items
      ? malloc((size_t)cap * sizeof(*items))
      : realloc(names->items, (size_t)cap * sizeof(*items));
    if (!items) return;
    if (names->items == names->inline_items) memcpy(items, names->inline_items, sizeof(names->inline_items));
    names->items = items;
    names->cap = cap;
  }
  names->items[names->count++] = (sv_decl_name_t){ str, len, is_function };
}

void collect_pattern_names(sv_ast_t *pat, sv_decl_names_t *out) {
  if (!pat) return;
  switch (pat->type) {
    case N_IDENT: decl_names_add(out, pat->str, pat->len, false); break;
    case N_ARRAY: case N_ARRAY_PAT:
      for (int i = 0; i < pat->args.count; i++) collect_pattern_names(pat->args.items[i], out);
      break;
    case N_OBJECT: case N_OBJECT_PAT:
      for (int i = 0; i < pat->args.count; i++) {
        sv_ast_t *prop = pat->args.items[i];
        if (prop && (prop->type == N_PROPERTY || prop->type == N_REST || prop->type == N_SPREAD))
          collect_pattern_names(prop->right, out);
      }
      break;
    case N_ASSIGN_PAT: case N_ASSIGN: collect_pattern_names(pat->left, out); break;
    case N_REST: case N_SPREAD: collect_pattern_names(pat->right, out); break;
    default: break;
  }
}

static void collect_var_names(sv_ast_t *node, sv_decl_names_t *out) {
  if (!node) return;
  switch (node->type) {
    case N_VAR:
      if (node->var_kind == SV_VAR_VAR)
        for (int i = 0; i < node->args.count; i++) {
          sv_ast_t *decl = node->args.items[i];
          if (decl && decl->type == N_VARDECL) collect_pattern_names(decl->left, out);
        }
      break;
    case N_BLOCK:
      for (int i = 0; i < node->args.count; i++) collect_var_names(node->args.items[i], out);
      break;
    case N_IF: collect_var_names(node->left, out); collect_var_names(node->right, out); break;
    case N_WHILE: case N_DO_WHILE: case N_LABEL: case N_WITH: collect_var_names(node->body, out); break;
    case N_FOR: collect_var_names(node->init, out); collect_var_names(node->body, out); break;
    case N_FOR_IN: case N_FOR_OF: case N_FOR_AWAIT_OF:
      collect_var_names(node->left, out); collect_var_names(node->body, out); break;
    case N_SWITCH:
      for (int i = 0; i < node->args.count; i++) {
        sv_ast_t *cas = node->args.items[i];
        for (int j = 0; cas && j < cas->args.count; j++) collect_var_names(cas->args.items[j], out);
      }
      break;
    case N_TRY:
      collect_var_names(node->body, out);
      collect_var_names(node->catch_body, out);
      collect_var_names(node->finally_body, out);
      break;
    case N_EXPORT: collect_var_names(node->left, out); break;
    default: break;
  }
}

static inline bool is_function_declaration(sv_ast_t *node) {
  return node && node->type == N_FUNC && node->str && !(node->flags & (FN_ARROW | FN_PAREN));
}

static void collect_lexical_names(sv_ast_list_t *stmts, bool functions_lexical, sv_decl_names_t *lex, sv_decl_names_t *vars) {
  for (int i = 0; i < stmts->count; i++) {
    sv_ast_t *node = stmts->items[i];
    if (node && node->type == N_EXPORT) node = node->left;
    if (!node) continue;
    
    if (node->type == N_VAR && node->var_kind != SV_VAR_VAR) {
      for (int j = 0; j < node->args.count; j++) {
        sv_ast_t *decl = node->args.items[j];
        if (decl && decl->type == N_VARDECL) collect_pattern_names(decl->left, lex);
      }
    } else if (node->type == N_CLASS && node->str && (node->flags & FN_CLASS_DECL)) {
      decl_names_add(lex, node->str, node->len, false);
    } else if (is_function_declaration(node)) {
      decl_names_add(functions_lexical ? lex : vars, node->str, node->len, true);
    } else if (node->type == N_IMPORT_DECL) {
      for (int j = 0; j < node->args.count; j++) {
        sv_ast_t *spec = node->args.items[j];
        if (spec && spec->type == N_IMPORT_SPEC && spec->right && spec->right->type == N_IDENT)
          decl_names_add(lex, spec->right->str, spec->right->len, false);
      }
    }
  }
}

static uint32_t decl_name_hash(const sv_decl_name_t *name) {
  uint32_t h = 2166136261u;
  for (uint32_t i = 0; i < name->len; i++) h = (h ^ (uint8_t)name->str[i]) * 16777619u;
  return h;
}

static inline bool decl_name_eq(const sv_decl_name_t *a, const sv_decl_name_t *b) {
  return a->len == b->len && memcmp(a->str, b->str, a->len) == 0;
}

static bool report_redeclaration(sv_compiler_t *c, const sv_decl_name_t *name) {
  js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Identifier '%.*s' has already been declared", (int)name->len, name->str);
  return false;
}

static bool check_scope_declarations(
  sv_compiler_t *c, const sv_decl_names_t *lex,
  const sv_decl_names_t *vars, const sv_decl_names_t *params
) {
  if (lex->count == 0) return true;
  
  uint32_t size = 16;
  while (size < (uint32_t)lex->count * 2) size *= 2;
  int inline_table[64];
  int *table = size <= 64 ? inline_table : malloc(size * sizeof(*table));
  if (!table) return true;
  for (uint32_t i = 0; i < size; i++) table[i] = -1;
  
  bool ok = true;
  for (int i = 0; ok && i < lex->count; i++) {
    uint32_t slot = decl_name_hash(&lex->items[i]) & (size - 1);
    for (; table[slot] >= 0; slot = (slot + 1) & (size - 1)) {
      const sv_decl_name_t *prev = &lex->items[table[slot]];
      if (!decl_name_eq(prev, &lex->items[i])) continue;
      if (!(prev->is_function && lex->items[i].is_function && !c->is_strict)) ok = report_redeclaration(c, &lex->items[i]);
      break;
    }
    if (ok && table[slot] < 0) table[slot] = i;
  }
  
  const sv_decl_names_t *others[2] = { vars, params };
  for (int k = 0; ok && k < 2; k++) for (int i = 0; ok && others[k] && i < others[k]->count; i++) {
    uint32_t slot = decl_name_hash(&others[k]->items[i]) & (size - 1);
    for (; table[slot] >= 0; slot = (slot + 1) & (size - 1))
      if (decl_name_eq(&lex->items[table[slot]], &others[k]->items[i])) { ok = report_redeclaration(c, &others[k]->items[i]); break; }
  }
  
  if (table != inline_table) free(table);
  return ok;
}

static inline void var_bloom_add(uint64_t bloom[4], uint32_t h) {
  bloom[(h >> 6) & 3] |= 1ull << (h & 63);
  bloom[(h >> 14) & 3] |= 1ull << ((h >> 8) & 63);
}

static inline bool var_bloom_may_contain(const uint64_t bloom[4], uint32_t h) {
  return (bloom[(h >> 6) & 3] & (1ull << (h & 63))) && (bloom[(h >> 14) & 3] & (1ull << ((h >> 8) & 63)));
}

void check_statement_list_declarations(
  sv_compiler_t *c, sv_ast_list_t *stmts, bool functions_lexical,
  const sv_decl_names_t *params, bool function_body
) {
  sv_decl_names_t lex, vars;
  decl_names_init(&lex);
  decl_names_init(&vars);
  collect_lexical_names(stmts, functions_lexical, &lex, &vars);
  
  bool walk_vars = function_body;
  if (!function_body) for (int i = 0; i < lex.count && !walk_vars; i++)
    walk_vars = var_bloom_may_contain(c->var_name_bloom, decl_name_hash(&lex.items[i]));
  
  if (walk_vars) for (int i = 0; i < stmts->count; i++) collect_var_names(stmts->items[i], &vars);
  if (function_body) {
    memset(c->var_name_bloom, 0, sizeof(c->var_name_bloom));
    for (int i = 0; i < vars.count; i++) var_bloom_add(c->var_name_bloom, decl_name_hash(&vars.items[i]));
  }
  
  check_scope_declarations(c, &lex, &vars, params);
  decl_names_free(&lex);
  decl_names_free(&vars);
}

static bool lexical_names_may_be_vars(sv_compiler_t *c, const sv_decl_names_t *lex) {
  for (int i = 0; i < lex->count; i++)
    if (var_bloom_may_contain(c->var_name_bloom, decl_name_hash(&lex->items[i]))) return true;
  return false;
}

void check_switch_declarations(sv_compiler_t *c, sv_ast_t *node) {
  sv_decl_names_t lex, vars;
  decl_names_init(&lex);
  decl_names_init(&vars);
  for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *cas = node->args.items[i];
    if (cas) collect_lexical_names(&cas->args, true, &lex, &vars);
  }
  if (lexical_names_may_be_vars(c, &lex)) for (int i = 0; i < node->args.count; i++) {
    sv_ast_t *cas = node->args.items[i];
    for (int j = 0; cas && j < cas->args.count; j++) collect_var_names(cas->args.items[j], &vars);
  }
  check_scope_declarations(c, &lex, &vars, NULL);
  decl_names_free(&lex);
  decl_names_free(&vars);
}

void check_for_head_declarations(sv_compiler_t *c, sv_ast_t *head, sv_ast_t *body) {
  if (!head || head->type != N_VAR || head->var_kind == SV_VAR_VAR) return;
  sv_decl_names_t lex, vars;
  decl_names_init(&lex);
  decl_names_init(&vars);
  for (int i = 0; i < head->args.count; i++) {
    sv_ast_t *decl = head->args.items[i];
    if (decl && decl->type == N_VARDECL) collect_pattern_names(decl->left, &lex);
  }
  if (lexical_names_may_be_vars(c, &lex)) collect_var_names(body, &vars);
  check_scope_declarations(c, &lex, &vars, NULL);
  decl_names_free(&lex);
  decl_names_free(&vars);
}

void check_catch_declarations(sv_compiler_t *c, sv_ast_t *param, sv_ast_t *body) {
  if (!param || !body || body->type != N_BLOCK) return;
  sv_decl_names_t names, body_lex, body_vars;
  decl_names_init(&names);
  decl_names_init(&body_lex);
  decl_names_init(&body_vars);
  collect_pattern_names(param, &names);
  collect_lexical_names(&body->args, true, &body_lex, &body_vars);
  if (param->type != N_IDENT && lexical_names_may_be_vars(c, &names))
    for (int i = 0; i < body->args.count; i++) collect_var_names(body->args.items[i], &body_vars);
  check_scope_declarations(c, &names, &body_lex, param->type != N_IDENT ? &body_vars : NULL);
  decl_names_free(&names);
  decl_names_free(&body_lex);
  decl_names_free(&body_vars);
}

int split_param_binding(sv_compiler_t *c, int param_local) {
  sv_local_t *param = &c->locals[param_local];
  const char *name = param->name;
  uint32_t len = param->name_len;
  int body_local = add_local(c, name, len, false, 0);
  emit_get_local(c, param_local);
  emit_put_local(c, body_local);
  return body_local;
}

static inline bool names_parameter(sv_compiler_t *c, const char *name, uint32_t len) {
  int local = resolve_local(c, name, len);
  return local >= 0 && local < c->param_bind_end;
}

static void hoist_var_pattern(sv_compiler_t *c, sv_ast_t *pat) {
  if (!pat) return;
  switch (pat->type) {
    case N_IDENT: {
      if (is_sloppy_eval(c)) { add_eval_var(c, pat->str, pat->len, false); break; }
      int existing = resolve_local(c, pat->str, pat->len);
      if (existing == -1) add_local(c, pat->str, pat->len, false, 0);
      else if (c->param_split && existing < c->param_bind_end) split_param_binding(c, existing);
      break;
    }
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
        mark_char_code_at_binding(c, prop);
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

void hoist_var_decls(sv_compiler_t *c, sv_ast_t *node) {
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
    case N_WHILE: case N_DO_WHILE: case N_LABEL: case N_WITH:
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

void hoist_lexical_pattern(sv_compiler_t *c, sv_ast_t *pat,
                                  bool is_const) {
  if (!pat) return;

  switch (pat->type) {
    case N_IDENT: {
      if (is_repl_top_level(c)) {
        add_global_lexical(c, pat->str, pat->len, is_const);
        break;
      }
      int local = ensure_local_at_depth(c, pat->str, pat->len, is_const, c->scope_depth);
      c->locals[local].eval_flags |= SV_EVAL_BIND_LEXICAL;
      break;
    }
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
        mark_char_code_at_binding(c, prop);
      }
      break;
    default:
      break;
  }
}

void annex_b_collect_funcs(ant_t *js, sv_ast_t *node, sv_ast_list_t *out) {
  if (!node) return;
  if (node->type == N_FUNC && node->str && !(node->flags & (FN_ARROW | FN_PAREN))) {
    sv_ast_list_push(js, out, node);
    return;
  }
  if (node->type == N_IF) {
    annex_b_collect_funcs(js, node->left, out);
    annex_b_collect_funcs(js, node->right, out);
  } else if (node->type == N_LABEL) annex_b_collect_funcs(js, node->body, out);
}

static void annex_b_collect_block_var_funcs(ant_t *js, sv_ast_t *node, sv_ast_list_t *out) {
  if (!node || node->type == N_FUNC || node->type == N_CLASS) return;
  if (node->type == N_BLOCK) {
    for (int i = 0; i < node->args.count; i++) {
      sv_ast_t *stmt = node->args.items[i];
      if (!stmt) continue;
      if (stmt->type == N_FUNC && stmt->str && !(stmt->flags & (FN_ARROW | FN_PAREN))) {
        sv_ast_list_push(js, out, stmt);
        continue;
      }
      annex_b_collect_block_var_funcs(js, stmt, out);
    }
    return;
  }
  if (node->type == N_IF) {
    annex_b_collect_block_var_funcs(js, node->left, out);
    annex_b_collect_block_var_funcs(js, node->right, out);
  } else if (node->type == N_LABEL) {
    annex_b_collect_block_var_funcs(js, node->body, out);
  } else if (node->type == N_WHILE || node->type == N_DO_WHILE || node->type == N_WITH) {
    annex_b_collect_block_var_funcs(js, node->body, out);
  } else if (node->type == N_FOR || node->type == N_FOR_IN || node->type == N_FOR_OF || node->type == N_FOR_AWAIT_OF) {
    annex_b_collect_block_var_funcs(js, node->body, out);
  } else if (node->type == N_SWITCH) {
    for (int i = 0; i < node->args.count; i++) {
      sv_ast_t *cas = node->args.items[i];
      for (int j = 0; cas && j < cas->args.count; j++) {
        sv_ast_t *stmt = cas->args.items[j];
        if (stmt && stmt->type == N_FUNC && stmt->str && !(stmt->flags & (FN_ARROW | FN_PAREN)))
          sv_ast_list_push(js, out, stmt);
        else annex_b_collect_block_var_funcs(js, stmt, out);
      }
    }
  } else if (node->type == N_TRY) {
    annex_b_collect_block_var_funcs(js, node->body, out);
    annex_b_collect_block_var_funcs(js, node->catch_body, out);
    annex_b_collect_block_var_funcs(js, node->finally_body, out);
  }
}

static void mark_export_binding_as(
  sv_compiler_t *c,
  const char *name, uint32_t len,
  const char *export_name, uint32_t export_name_len
) {
  int local = resolve_local(c, name, len);
  if (local == -1) return;

  sv_export_name_t **slot = &c->locals[local].binding.exports;
  while (*slot) {
    if ((*slot)->len == export_name_len &&
        memcmp((*slot)->name, export_name, export_name_len) == 0)
      return;
    slot = &(*slot)->next;
  }

  sv_export_name_t *node = sv_code_bump(c->js, sizeof(sv_export_name_t));
  if (!node) return;
  node->name = export_name;
  node->len = export_name_len;
  node->next = NULL;
  *slot = node;
}

void mark_export_binding(sv_compiler_t *c, const char *name, uint32_t len) {
  mark_export_binding_as(c, name, len, name, len);
}

void mark_export_pattern(sv_compiler_t *c, sv_ast_t *pat) {
  if (!pat) return;
  switch (pat->type) {
    case N_IDENT:
      mark_export_binding(c, pat->str, pat->len);
      break;
    case N_ASSIGN_PAT:
      mark_export_pattern(c, pat->left);
      break;
    case N_REST:
    case N_SPREAD:
      mark_export_pattern(c, pat->right);
      break;
    case N_ARRAY:
    case N_ARRAY_PAT:
      for (int i = 0; i < pat->args.count; i++)
        mark_export_pattern(c, pat->args.items[i]);
      break;
    case N_OBJECT:
    case N_OBJECT_PAT:
      for (int i = 0; i < pat->args.count; i++) {
        sv_ast_t *prop = pat->args.items[i];
        if (!prop) continue;
        if (prop->type == N_PROPERTY) mark_export_pattern(c, prop->right);
        else mark_export_pattern(c, prop);
      }
      break;
    default:
      break;
  }
}

void hoist_lexical_decls(sv_compiler_t *c, sv_ast_list_t *stmts) {
  for (int i = 0; i < stmts->count; i++) {
    sv_ast_t *node = stmts->items[i];
    if (!node) continue;
    sv_ast_t *decl_node = (node->type == N_EXPORT) ? node->left : node;
    if (!decl_node) continue;

    if (decl_node->type == N_VAR && decl_node->var_kind != SV_VAR_VAR) {
      bool is_using =
        decl_node->var_kind == SV_VAR_USING ||
        decl_node->var_kind == SV_VAR_AWAIT_USING;
      bool is_const = decl_node->var_kind == SV_VAR_CONST || is_using;
      if (is_using && is_repl_top_level(c)) continue;
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
      if (node->type == N_EXPORT) for (int j = 0; j < decl_node->args.count; j++) {
        sv_ast_t *decl = decl_node->args.items[j];
        if (!decl || decl->type != N_VARDECL) continue;
        mark_export_pattern(c, decl->left);
      }
    } else if (decl_node->type == N_IMPORT_DECL) {
      for (int j = 0; j < decl_node->args.count; j++) {
        sv_ast_t *spec = decl_node->args.items[j];
        if (!spec || spec->type != N_IMPORT_SPEC ||
            !spec->right || spec->right->type != N_IDENT)
          continue;
        if (is_repl_top_level(c)) {
          add_global_lexical(c, spec->right->str, spec->right->len, true);
          continue;
        }
        int idx = ensure_local_at_depth(c, spec->right->str, spec->right->len, true, c->scope_depth);
        mark_import_binding(c, idx, spec);
      }
    } else if (
      decl_node->type == N_CLASS && decl_node->str &&
      (decl_node->flags & FN_CLASS_DECL) && is_repl_top_level(c)
    ) {
      add_global_lexical(c, decl_node->str, decl_node->len, false);
    } else if (
      decl_node->type == N_CLASS && decl_node->str &&
      (decl_node->flags & FN_CLASS_DECL)
    ) {
      int lb = c->local_count;
      int class_local = ensure_local_at_depth(c, decl_node->str, decl_node->len, false, c->scope_depth);
      c->locals[class_local].eval_flags |= SV_EVAL_BIND_LEXICAL;
      if (c->local_count > lb) {
        c->locals[c->local_count - 1].is_tdz = true;
        set_local_inferred_type(c, c->local_count - 1, SV_TI_UNKNOWN);
        int slot = (c->local_count - 1) - c->param_locals;
        emit_op(c, OP_SET_LOCAL_UNDEF);
        emit_u16(c, (uint16_t)slot);
      }
      if (node->type == N_EXPORT) {
        if (node->flags & EX_DEFAULT)
          mark_export_binding_as(c, decl_node->str, decl_node->len, "default", 7);
        else
          mark_export_binding(c, decl_node->str, decl_node->len);
      }
    } else if (decl_node->type == N_FUNC && decl_node->str && !(decl_node->flags & (FN_ARROW | FN_PAREN))) {
      if (is_sloppy_eval(c) && c->scope_depth == 0)
        add_eval_var(c, decl_node->str, decl_node->len, false);
      else {
        int local = resolve_local(c, decl_node->str, decl_node->len);
        bool split = c->param_split && c->scope_depth == 0 && local >= 0 && local < c->param_bind_end;
        bool declared = local != -1 && c->locals[local].depth == c->scope_depth && !split;
        if (!declared) {
          local = add_local(c, decl_node->str, decl_node->len, false, c->scope_depth);
          declared = true;
        }
        if (declared) c->locals[local].eval_flags |= c->scope_depth > 0 ? SV_EVAL_BIND_LEXICAL : 0;
      }
      if (node->type == N_EXPORT) {
        if (node->flags & EX_DEFAULT)
          mark_export_binding_as(c, decl_node->str, decl_node->len, "default", 7);
        else
          mark_export_binding(c, decl_node->str, decl_node->len);
      }
    }
    if (!c->is_strict && (decl_node->type == N_IF || decl_node->type == N_LABEL)) {
      sv_ast_list_t funcs = {0};
      annex_b_collect_funcs(c->js, decl_node, &funcs);
      for (int j = 0; j < funcs.count; j++) {
        sv_ast_t *fn = funcs.items[j];
        if (is_sloppy_eval(c) && c->scope_depth == 0) add_eval_var(c, fn->str, fn->len, true);
        else if (resolve_local(c, fn->str, fn->len) == -1)
          add_local(c, fn->str, fn->len, false, c->scope_depth);
      }
    }
    if (!c->is_strict) {
      sv_ast_list_t funcs = {0};
      annex_b_collect_block_var_funcs(c->js, decl_node, &funcs);
      for (int j = 0; j < funcs.count; j++) {
        sv_ast_t *fn = funcs.items[j];
        if (is_sloppy_eval(c)) add_eval_var(c, fn->str, fn->len, true);
        else if (resolve_local_at_depth(c, fn->str, fn->len, 0) == -1 && !names_parameter(c, fn->str, fn->len))
          add_local(c, fn->str, fn->len, false, 0);
      }
    }
  }

  if (is_sloppy_eval(c) && c->scope_depth == 0) {
    for (uint32_t i = 0; i < c->eval_var_count;) {
      sv_eval_decl_t *decl = &c->eval_vars[i];
      int local = resolve_local_at_depth(c, decl->str, decl->len, 0);
      bool lexical = has_global_lexical(c, decl->str, decl->len) ||
        (local >= 0 && (c->locals[local].eval_flags & SV_EVAL_BIND_LEXICAL));
      if (!lexical) { i++; continue; }
      if (!decl->annex_b) {
        js_mkerr_typed(c->js, JS_ERR_SYNTAX, "Identifier '%.*s' has already been declared",
          (int)decl->len, decl->str);
        return;
      }
      memmove(decl, decl + 1, (--c->eval_var_count - i) * sizeof(*decl));
    }
  }

  for (int i = 0; i < stmts->count; i++) {
    sv_ast_t *node = stmts->items[i];
    if (!node || node->type != N_EXPORT) continue;

    if ((node->flags & EX_DECL) && node->left && node->left->type == N_VAR) {
      for (int j = 0; j < node->left->args.count; j++) {
        sv_ast_t *decl = node->left->args.items[j];
        if (!decl || decl->type != N_VARDECL) continue;
        mark_export_pattern(c, decl->left);
      }
      continue;
    }

    if (!(node->flags & EX_NAMED) || (node->flags & (EX_FROM | EX_DECL | EX_DEFAULT | EX_STAR)))
      continue;
    for (int j = 0; j < node->args.count; j++) {
      sv_ast_t *spec = node->args.items[j];
      if (!spec || spec->type != N_IMPORT_SPEC || !spec->left || !spec->right ||
          spec->left->type != N_IDENT || spec->right->type != N_IDENT)
        continue;
      mark_export_binding_as(
        c, spec->left->str, spec->left->len,
        spec->right->str, spec->right->len);
    }
  }
}

void emit_closure(sv_compiler_t *c, int index) {
  sv_func_t *child = (sv_func_t *)vptr(c->constants[index]);
  uint32_t scope = 0;
  if (child->needs_eval_env && !capture_dynamic_eval_scope(c, &scope)) return;
  sv_op_t op = child->needs_eval_env ? OP_CLOSURE_EVAL
    : c->args_mapped ? OP_CLOSURE_ARGS : OP_CLOSURE;
  emit_op(c, op);
  emit_u32(c, (uint32_t)index);
  if (child->needs_eval_env) emit_u32(c, scope);
}

void hoist_one_func(sv_compiler_t *c, sv_ast_t *node, bool annex_b_update_var) {
  sv_func_t *fn = compile_function_body(c, node, SV_COMPILE_SCRIPT);
  if (!fn) return;
  int idx = add_constant(c, mkref(kTypeFunctionInfo, fn));
  emit_closure(c, idx);
  emit_set_function_name(c, node->str, node->len);
  int annex_var = annex_b_update_var && !is_sloppy_eval(c) ? resolve_local_at_depth(c, node->str, node->len, 0) : -1;
  if (annex_var >= 0 && ((c->locals[annex_var].eval_flags & SV_EVAL_BIND_LEXICAL) || annex_var < c->param_bind_end)) annex_var = -1;
  bool eval_annex_var = false;
  if (annex_b_update_var && is_sloppy_eval(c))
    for (uint32_t i = 0; i < c->eval_var_count; i++)
      if (c->eval_vars[i].len == node->len && !memcmp(c->eval_vars[i].str, node->str, node->len))
        eval_annex_var = true;
  if (annex_var >= 0 || eval_annex_var) emit_op(c, OP_DUP);
  if (is_sloppy_eval(c) && c->scope_depth == 0) {
    bool declared = false;
    for (uint32_t i = 0; i < c->eval_var_count; i++)
      if (c->eval_vars[i].len == node->len && !memcmp(c->eval_vars[i].str, node->str, node->len))
        declared = true;
    if (declared) emit_atom_op(c, OP_PUT_EVAL_FUNCTION, node->str, node->len);
    else emit_op(c, OP_POP);
  } else {
    int local = resolve_local(c, node->str, node->len);
    emit_put_local(c, local);
  }
  if (annex_var >= 0) emit_put_local(c, annex_var);
  if (eval_annex_var) emit_atom_op(c, OP_PUT_EVAL_FUNCTION, node->str, node->len);
}

void hoist_func_decls(sv_compiler_t *c, sv_ast_list_t *stmts) {
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
      annex_b_collect_funcs(c->js, node, &funcs);
      for (int j = 0; j < funcs.count; j++)
        hoist_one_func(c, funcs.items[j], false);
    }
  }
}
