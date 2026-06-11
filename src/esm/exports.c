#include "esm/exports.h"
#include "internal.h"

#include <stdlib.h>
#include <string.h>

/* Pre-declares every statically known export on the module namespace before
   evaluation, in sorted key order (module namespace exotic objects expose
   sorted own keys). var bindings hoist as undefined; everything else holds
   the TDZ sentinel until its declaration initializes it, so reads through
   cycles can throw ReferenceError before initialization.

   Names are appended without dedup, sorted once, then merged in a single
   adjacent pass — no quadratic membership scans. */

typedef struct {
  const char *name;
  uint32_t len;
  bool is_var;
} esm_export_name_t;

typedef struct {
  esm_export_name_t *items;
  int count;
  int cap;
  bool oom;
} esm_export_list_t;

static void export_list_add(esm_export_list_t *list, const char *name, uint32_t len, bool is_var) {
  if (!name || len == 0 || list->oom) return;
  if (list->count >= list->cap) {
    int cap = list->cap ? list->cap * 2 : 16;
    esm_export_name_t *next = realloc(list->items, (size_t)cap * sizeof(*next));
    if (!next) { list->oom = true; return; }
    list->items = next;
    list->cap = cap;
  }
  list->items[list->count++] = (esm_export_name_t){ .name = name, .len = len, .is_var = is_var };
}

static int export_name_cmp(const void *a, const void *b) {
  const esm_export_name_t *x = a, *y = b;
  uint32_t min = x->len < y->len ? x->len : y->len;
  int r = memcmp(x->name, y->name, min);
  if (r) return r;
  return (int)x->len - (int)y->len;
}

static void export_list_sort_merge(esm_export_list_t *list) {
  if (list->count > 1)
    qsort(list->items, (size_t)list->count, sizeof(*list->items), export_name_cmp);
  int out = 0;
  for (int i = 0; i < list->count; i++) {
    if (out > 0 && export_name_cmp(&list->items[out - 1], &list->items[i]) == 0) {
      list->items[out - 1].is_var = list->items[out - 1].is_var || list->items[i].is_var;
      continue;
    }
    list->items[out++] = list->items[i];
  }
  list->count = out;
}

static bool export_list_bsearch(const esm_export_list_t *list, const char *name, uint32_t len) {
  esm_export_name_t key = { .name = name, .len = len };
  return list->count > 0 &&
    bsearch(&key, list->items, (size_t)list->count, sizeof(key), export_name_cmp) != NULL;
}

static void collect_pattern_names(esm_export_list_t *list, sv_ast_t *pat, bool is_var) {
  if (!pat) return;

  static const void *dispatch[N__COUNT] = {
    [N_IDENT]      = &&l_ident,
    [N_ASSIGN_PAT] = &&l_left,
    [N_ASSIGN]     = &&l_left,
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
    export_list_add(list, pat->str, pat->len, is_var);
    return;
  l_left:
    collect_pattern_names(list, pat->left, is_var);
    return;
  l_right:
    collect_pattern_names(list, pat->right, is_var);
    return;
  l_list:
    for (int i = 0; i < pat->args.count; i++)
      collect_pattern_names(list, pat->args.items[i], is_var);
    return;
  l_props:
    for (int i = 0; i < pat->args.count; i++) {
      sv_ast_t *p = pat->args.items[i];
      if (!p) continue;
      collect_pattern_names(list, p->type == N_PROPERTY ? p->right : p, is_var);
    }
    return;
}

/* names bound by hoisted `var` declarations anywhere in the module body;
   mirrors the statement shapes the compiler's hoist_var_decls walks */
static void collect_var_names(esm_export_list_t *vars, sv_ast_t *node) {
  if (!node) return;

  static const void *dispatch[N__COUNT] = {
    [N_VAR]          = &&l_var,
    [N_EXPORT]       = &&l_left,
    [N_BLOCK]        = &&l_list,
    [N_PROGRAM]      = &&l_list,
    [N_IF]           = &&l_if,
    [N_WHILE]        = &&l_body,
    [N_DO_WHILE]     = &&l_body,
    [N_LABEL]        = &&l_body,
    [N_FOR]          = &&l_for,
    [N_FOR_IN]       = &&l_for_each,
    [N_FOR_OF]       = &&l_for_each,
    [N_FOR_AWAIT_OF] = &&l_for_each,
    [N_SWITCH]       = &&l_switch,
    [N_TRY]          = &&l_try,
  };

  if ((unsigned)node->type < N__COUNT && dispatch[node->type])
    goto *dispatch[node->type];
  return;

  l_var:
    if (node->var_kind != SV_VAR_VAR) return;
    for (int i = 0; i < node->args.count; i++) {
      sv_ast_t *decl = node->args.items[i];
      if (decl && decl->type == N_VARDECL)
        collect_pattern_names(vars, decl->left, true);
    }
    return;
  l_left:
    collect_var_names(vars, node->left);
    return;
  l_list:
    for (int i = 0; i < node->args.count; i++)
      collect_var_names(vars, node->args.items[i]);
    return;
  l_if:
    collect_var_names(vars, node->left);
    collect_var_names(vars, node->right);
    collect_var_names(vars, node->body);
    return;
  l_body:
    collect_var_names(vars, node->body);
    return;
  l_for:
    collect_var_names(vars, node->init);
    collect_var_names(vars, node->body);
    return;
  l_for_each:
    collect_var_names(vars, node->left);
    collect_var_names(vars, node->body);
    return;
  l_switch:
    for (int i = 0; i < node->args.count; i++) {
      sv_ast_t *cas = node->args.items[i];
      if (!cas) continue;
      for (int j = 0; j < cas->args.count; j++)
        collect_var_names(vars, cas->args.items[j]);
    }
    return;
  l_try:
    collect_var_names(vars, node->body);
    collect_var_names(vars, node->catch_body);
    collect_var_names(vars, node->finally_body);
    return;
}

static void collect_spec_names(esm_export_list_t *list, const esm_export_list_t *vars, sv_ast_list_t *specs) {
  for (int i = 0; i < specs->count; i++) {
    sv_ast_t *spec = specs->items[i];
    if (!spec || spec->type != N_IMPORT_SPEC || !spec->right || spec->right->type != N_IDENT)
      continue;
    /* a clause export of a hoisted var binding is initialized (undefined)
       from module entry; lexical bindings stay in TDZ */
    bool is_var = spec->left && spec->left->type == N_IDENT &&
                  export_list_bsearch(vars, spec->left->str, spec->left->len);
    export_list_add(list, spec->right->str, spec->right->len, is_var);
  }
}

static void collect_decl_names(esm_export_list_t *list, sv_ast_t *decl) {
  static const void *dispatch[N__COUNT] = {
    [N_VAR]   = &&l_var,
    [N_FUNC]  = &&l_named,
    [N_CLASS] = &&l_named,
  };

  if ((unsigned)decl->type < N__COUNT && dispatch[decl->type])
    goto *dispatch[decl->type];
  return;

  l_var: {
    bool is_var = decl->var_kind == SV_VAR_VAR;
    for (int i = 0; i < decl->args.count; i++) {
      sv_ast_t *var = decl->args.items[i];
      if (var && var->type == N_VARDECL)
        collect_pattern_names(list, var->left, is_var);
    }
    return;
  }
  l_named:
    if (decl->str && decl->len > 0)
      export_list_add(list, decl->str, decl->len, false);
    return;
}

static inline bool stmt_is_local_export_clause(const sv_ast_t *stmt) {
  uint16_t f = stmt->flags;
  return (f & EX_NAMED) && !(f & (EX_FROM | EX_DECL | EX_DEFAULT | EX_STAR));
}

void esm_predeclare_exports(ant_t *js, sv_ast_t *program, ant_value_t ns) {
  if (!program || !is_object_type(ns)) return;

  esm_export_list_t list = {0};
  esm_export_list_t vars = {0};

  /* local export clauses need to know which bindings are hoisted vars;
     only pay for the body walk when one exists */
  for (int i = 0; i < program->args.count; i++) {
    sv_ast_t *stmt = program->args.items[i];
    if (stmt && stmt->type == N_EXPORT && stmt_is_local_export_clause(stmt)) {
      collect_var_names(&vars, program);
      export_list_sort_merge(&vars);
      break;
    }
  }

  for (int i = 0; i < program->args.count; i++) {
    sv_ast_t *stmt = program->args.items[i];
    if (!stmt || stmt->type != N_EXPORT) continue;
    uint16_t f = stmt->flags;

    if (f & EX_DEFAULT) export_list_add(&list, "default", 7, false);
    else if ((f & EX_DECL) && stmt->left) collect_decl_names(&list, stmt->left);
    else if ((f & EX_NAMED) || ((f & EX_STAR) && (f & EX_NAMESPACE)))
      collect_spec_names(&list, &vars, &stmt->args);
  }

  if (!list.oom) {
    export_list_sort_merge(&list);
    for (int i = 0; i < list.count; i++) {
      ant_value_t init = list.items[i].is_var ? js_mkundef() : T_EMPTY;
      js_module_ns_define(js, ns, list.items[i].name, list.items[i].len, init);
      if (list.items[i].len == 7 && memcmp(list.items[i].name, "default", 7) == 0)
        js_set_slot_wb(js, ns, SLOT_DEFAULT, init);
    }
  }

  free(list.items);
  free(vars.items);
}
