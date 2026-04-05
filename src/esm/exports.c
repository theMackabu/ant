#include "esm/exports.h"
#include "internal.h"

static void collect_binding_names(ant_t *js, sv_ast_t *pat, ant_value_t ns) {
  if (!pat) return;

  static const void *dispatch[N__COUNT] = {
    [N_IDENT]      = &&l_ident,
    [N_ASSIGN_PAT] = &&l_left,
    [N_REST]       = &&l_right,
    [N_SPREAD]     = &&l_right,
    [N_ARRAY]      = &&l_list,
    [N_ARRAY_PAT]  = &&l_list,
    [N_OBJECT]     = &&l_props,
    [N_OBJECT_PAT] = &&l_props,
  };

  if ((unsigned)pat->type < N__COUNT && dispatch[pat->type])
    goto *dispatch[pat->type];
  return;

  l_ident:
    setprop_cstr(js, ns, pat->str, pat->len, js_mkundef());
    return;
  l_left:
    collect_binding_names(js, pat->left, ns);
    return;
  l_right:
    collect_binding_names(js, pat->right, ns);
    return;
  l_list:
    for (int i = 0; i < pat->args.count; i++)
      collect_binding_names(js, pat->args.items[i], ns);
    return;
  l_props:
    for (int i = 0; i < pat->args.count; i++) {
      sv_ast_t *p = pat->args.items[i];
      if (!p) continue;
      collect_binding_names(js, p->type == N_PROPERTY ? p->right : p, ns);
    }
    return;
}

static void collect_spec_names(ant_t *js, sv_ast_list_t *specs, ant_value_t ns) {
for (int i = 0; i < specs->count; i++) {
  sv_ast_t *spec = specs->items[i];
  if (spec && spec->type == N_IMPORT_SPEC && spec->right && spec->right->type == N_IDENT)
    setprop_cstr(js, ns, spec->right->str, spec->right->len, js_mkundef());
}}

static void predeclare_decl(ant_t *js, sv_ast_t *decl, ant_value_t ns) {
  static const void *dispatch[N__COUNT] = {
    [N_VAR]   = &&l_var,
    [N_FUNC]  = &&l_named,
    [N_CLASS] = &&l_named,
  };

  if ((unsigned)decl->type < N__COUNT && dispatch[decl->type])
    goto *dispatch[decl->type];
  return;

  l_var:
    for (int i = 0; i < decl->args.count; i++) {
      sv_ast_t *var = decl->args.items[i];
      if (var && var->type == N_VARDECL)
        collect_binding_names(js, var->left, ns);
    }
    return;
  l_named:
    if (decl->str && decl->len > 0)
      setprop_cstr(js, ns, decl->str, decl->len, js_mkundef());
    return;
}

void esm_predeclare_exports(ant_t *js, sv_ast_t *program, ant_value_t ns) {
  if (!program || !is_object_type(ns)) return;

  for (int i = 0; i < program->args.count; i++) {
  sv_ast_t *stmt = program->args.items[i];
  if (!stmt || stmt->type != N_EXPORT) continue;
  uint16_t f = stmt->flags;
  
  if (f & EX_DEFAULT) setprop_cstr(js, ns, "default", 7, js_mkundef());
  else if ((f & EX_DECL) && stmt->left) predeclare_decl(js, stmt->left, ns);
  else if ((f & EX_NAMED) || ((f & EX_STAR) && (f & EX_NAMESPACE))) collect_spec_names(js, &stmt->args, ns);
}}
