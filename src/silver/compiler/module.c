#include "compiler_internal.h"

enum {
  IMPORT_BIND_DEFAULT   = 1 << 0,
  IMPORT_BIND_NAMESPACE = 1 << 1,
};

static uint8_t import_spec_binding(sv_ast_t *spec, const char **name, uint32_t *len) {
  *name = NULL;
  *len = 0;
  if (spec->flags & IMPORT_BIND_NAMESPACE) return SV_IMPORT_BIND_NONE;
  if ((spec->flags & IMPORT_BIND_DEFAULT) || !spec->left || spec->left->type != N_IDENT)
    return SV_IMPORT_BIND_DEFAULT;
  *name = spec->left->str;
  *len = spec->left->len;
  return SV_IMPORT_BIND_NAMED;
}

void mark_import_binding(sv_compiler_t *c, int idx, sv_ast_t *spec) {
  if (idx < 0) return;
  c->locals[idx].eval_flags |= SV_EVAL_BIND_LEXICAL;
  sv_binding_meta_t *binding = &c->locals[idx].binding;
  binding->import_kind = import_spec_binding(spec, &binding->import_name, &binding->import_name_len);
}

bool module_stmt_is_static_declaration(sv_ast_t *node) {
  if (!node) return false;
  if (node->type == N_IMPORT_DECL) return true;
  return node->type == N_EXPORT && (node->flags & EX_FROM);
}

void compile_module_static_declaration_prologue(sv_compiler_t *c, sv_ast_list_t *list) {
  for (int i = 0; i < list->count; i++) {
    sv_ast_t *node = list->items[i];
    if (!module_stmt_is_static_declaration(node)) continue;
    compile_stmt(c, node);
  }
}

bool should_emit_module_static_declarations(sv_compiler_t *c, sv_ast_t *body) {
  return c->mode == SV_COMPILE_MODULE &&
    c->enclosing && !c->enclosing->enclosing &&
    body && body->type == N_BLOCK;
}

static void emit_import_sync(sv_compiler_t *c, sv_ast_t *node) {
  if (node->aux) emit_atom_op(c, OP_IMPORT_SYNC_TYPE, node->aux, node->aux_len);
  else emit_op(c, OP_IMPORT_SYNC);
}

void compile_import_decl(sv_compiler_t *c, sv_ast_t *node) {
  if (!node->right) return;
  bool repl_top = is_repl_top_level(c);

  compile_expr(c, node->right);
  emit_import_sync(c, node);

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

    const char *import_name = NULL;
    uint32_t import_name_len = 0;
    uint8_t import_kind = import_spec_binding(spec, &import_name, &import_name_len);

    emit_get_local(c, ns_local);
    if (repl_top) {
      emit_import_binding_resolve(c, import_kind, import_name, import_name_len);
      emit_atom_op(c, OP_INIT_GLOBAL_LEX, spec->right->str, spec->right->len);
    } else {
      int idx = ensure_local_at_depth(c, spec->right->str, spec->right->len, true, c->scope_depth);
      emit_put_local(c, idx);
      mark_import_binding(c, idx, spec);
    }
  }
}

void compile_export_emit(sv_compiler_t *c, const char *name, uint32_t len) {
  mark_export_binding(c, name, len);
  int local = resolve_local(c, name, len);
  const sv_export_name_t *exports =
    (local >= 0) ? c->locals[local].binding.exports : NULL;

  emit_get_var(c, name, len);
  if (!exports) {
    emit_atom_op(c, OP_EXPORT, name, len);
    return;
  }

  emit_export_writes_from_stack(c, exports);
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

static bool export_clause_is_metadata_only(sv_compiler_t *c, sv_ast_t *spec) {
  if (!spec || !spec->left || spec->left->type != N_IDENT) return false;
  int local = resolve_local_at_depth(c, spec->left->str, spec->left->len, c->scope_depth);
  if (local < 0) return false;
  sv_local_t *loc = &c->locals[local];
  return loc->is_tdz && loc->binding.import_kind == SV_IMPORT_BIND_NONE;
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
      for (int i = 0; i < decl->args.count; i++) {
        sv_ast_t *var = decl->args.items[i];
        if (!var || var->type != N_VARDECL) continue;
        mark_export_pattern(c, var->left);
      }
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
    emit_import_sync(c, node);

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
    emit_import_sync(c, node);
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
      if (export_clause_is_metadata_only(c, spec))
        continue;
      emit_get_var(c, spec->left->str, spec->left->len);
      emit_atom_op(c, OP_EXPORT, spec->right->str, spec->right->len);
    }
  }
}
