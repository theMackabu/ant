#include <stdlib.h>
#include <string.h>

#include "esm/exports.h"
#include "internal.h"
#include "shapes.h"
#include "silver/engine.h"

typedef struct {
  const char *str;
  uint32_t len;
} export_name_t;

typedef struct {
  export_name_t *items;
  int count;
  int cap;
  bool failed;
} export_names_t;

static void export_names_add(export_names_t *names, const char *str, uint32_t len) {
  if (names->count == names->cap) {
    int cap = names->cap ? names->cap * 2 : 16;
    export_name_t *items = realloc(names->items, (size_t)cap * sizeof(*items));
    if (!items) { names->failed = true; return; }
    names->items = items;
    names->cap = cap;
  }
  names->items[names->count++] = (export_name_t){ str, len };
}

static void collect_binding_names(export_names_t *names, sv_ast_t *pat) {
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
    export_names_add(names, pat->str, pat->len);
    return;
  l_left:
    collect_binding_names(names, pat->left);
    return;
  l_right:
    collect_binding_names(names, pat->right);
    return;
  l_list:
    for (int i = 0; i < pat->args.count; i++)
      collect_binding_names(names, pat->args.items[i]);
    return;
  l_props:
    for (int i = 0; i < pat->args.count; i++) {
      sv_ast_t *p = pat->args.items[i];
      if (!p) continue;
      collect_binding_names(names, p->type == N_PROPERTY ? p->right : p);
    }
    return;
}

static void collect_spec_names(export_names_t *names, sv_ast_list_t *specs) {
for (int i = 0; i < specs->count; i++) {
  sv_ast_t *spec = specs->items[i];
  if (spec && spec->type == N_IMPORT_SPEC && spec->right && spec->right->type == N_IDENT)
    export_names_add(names, spec->right->str, spec->right->len);
}}

static void predeclare_decl(export_names_t *names, sv_ast_t *decl) {
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
        collect_binding_names(names, var->left);
    }
    return;
  l_named:
    if (decl->str && decl->len > 0)
      export_names_add(names, decl->str, decl->len);
    return;
}

void esm_predeclare_exports(ant_t *js, sv_ast_t *program, ant_value_t ns) {
  if (!program || !is_object_type(ns)) return;
  export_names_t names = {0};

  for (int i = 0; i < program->args.count; i++) {
    sv_ast_t *stmt = program->args.items[i];
    if (!stmt || stmt->type != N_EXPORT) continue;
    uint32_t f = stmt->flags;

    if (f & EX_DEFAULT) export_names_add(&names, "default", 7);
    else if ((f & EX_DECL) && stmt->left) predeclare_decl(&names, stmt->left);
    else if ((f & EX_NAMED) || ((f & EX_STAR) && (f & EX_NAMESPACE))) collect_spec_names(&names, &stmt->args);
  }

  for (int i = 0; i < names.count; i++)
    setprop_cstr_attrs(js, ns, names.items[i].str, names.items[i].len, Ant_Value_Undefined, ANT_PROP_ATTR_ENUMERABLE);
  free(names.items);

  ant_object_t *ptr = js_obj_ptr(js_as_obj(ns));
  if (ptr && !names.failed && js_check_brand(ns, BRAND_MODULE_NAMESPACE)) ptr->flags.extensible = 0;
}
