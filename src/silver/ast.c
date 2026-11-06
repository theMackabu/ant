#include "silver/ast.h"
#include "runtime.h"

sv_ast_t *sv_ast_new(ant_t *js, sv_node_type_t type) {
  sv_ast_t *n = parse_arena_bump(js, sizeof(sv_ast_t));
  if (!n) return NULL;
  memset(n, 0, sizeof(sv_ast_t));
  n->type = type;
  return n;
}

bool sv_ast_list_push(ant_t *js, sv_ast_list_t *list, sv_ast_t *node) {
  if (list->count >= list->cap) {
    int new_cap = list->cap ? list->cap * 2 : 4;
    sv_ast_t **new_items = parse_arena_bump(js, (size_t)new_cap * sizeof(sv_ast_t *));
    if (!new_items) return false;
    if (list->items)
      memcpy(new_items, list->items, (size_t)list->count * sizeof(sv_ast_t *));
    list->items = new_items;
    list->cap = new_cap;
  }
  list->items[list->count++] = node;
  return true;
}

bool sv_ast_can_be_expression_statement(const sv_ast_t *node) {
  if (!node) return false;
  if ((node->type == N_FUNC || node->type == N_CLASS) && (node->flags & FN_PAREN)) return true;

  static const uint8_t expr_stmt_nodes[N__COUNT] = {
    [N_NUMBER] = 1, [N_STRING] = 1, [N_BIGINT] = 1, [N_BOOL] = 1,
    [N_NULL] = 1, [N_UNDEF] = 1, [N_THIS] = 1, [N_GLOBAL_THIS] = 1,
    [N_TEMPLATE] = 1, [N_REGEXP] = 1, [N_IDENT] = 1, [N_BINARY] = 1,
    [N_UNARY] = 1, [N_UPDATE] = 1, [N_ASSIGN] = 1, [N_TERNARY] = 1,
    [N_CALL] = 1, [N_NEW] = 1, [N_MEMBER] = 1, [N_OPTIONAL] = 1,
    [N_ARRAY] = 1, [N_OBJECT] = 1, [N_PROPERTY] = 1, [N_SPREAD] = 1,
    [N_SEQUENCE] = 1, [N_ARROW] = 1, [N_YIELD] = 1, [N_AWAIT] = 1,
    [N_TYPEOF] = 1, [N_DELETE] = 1, [N_VOID] = 1, [N_TAGGED_TEMPLATE] = 1,
    [N_IMPORT] = 1, [N_NEW_TARGET] = 1,
  };

  if (node->type == N_FUNC)
    return !(node->str && !(node->flags & FN_ARROW));
  if ((unsigned)node->type >= N__COUNT) return false;
  return expr_stmt_nodes[node->type] != 0;
}

bool ast_pattern_binds(const sv_ast_t *node, const char *name) {
  if (!node) return false;
  switch (node->type) {
    case N_IDENT:
      return node->len == strlen(name) && memcmp(node->str, name, node->len) == 0;
    case N_ASSIGN: case N_ASSIGN_PAT:
      return ast_pattern_binds(node->left, name);
    case N_REST: case N_SPREAD: case N_PROPERTY:
      return ast_pattern_binds(node->right, name);
    case N_ARRAY: case N_ARRAY_PAT: case N_OBJECT: case N_OBJECT_PAT:
      for (int i = 0; i < node->args.count; i++)
        if (ast_pattern_binds(node->args.items[i], name)) return true;
      return false;
    default: return false;
  }
}

bool ast_is_arguments_length(const sv_ast_t *node) {
  return
    node && node->type == N_MEMBER && !(node->flags & 1) &&
    ast_is_ident(node->left, "arguments", 9) &&
    ast_is_ident(node->right, "length", 6);
}

bool ast_arguments_length_only(const sv_ast_t *node, bool write, bool arrow) {
  if (!node) return true;

  switch (node->type) {
    case N_IDENT: return !ast_is_ident(node, "arguments", 9);
    case N_WITH:  return false;
    
    case N_ARROW:
      arrow = true;
      break;
      
    case N_FUNC:
    case N_CLASS:
      if (node->str && node->len == 9 && memcmp(node->str, "arguments", 9) == 0) return false;
      if (node->type == N_FUNC && !(node->flags & FN_ARROW)) return true;
      if (node->flags & FN_ARROW) arrow = true;
      break;
      
    case N_CALL:
      if (ast_is_ident(node->left, "eval", 4)) return false;
      break;
      
    case N_MEMBER:
      if (ast_is_arguments_length(node)) return !write && !arrow;
      return
        ast_arguments_length_only(node->left, false, arrow) &&
        (!(node->flags & 1) || ast_arguments_length_only(node->right, false, arrow));
      
    case N_ASSIGN:
      return
        ast_arguments_length_only(node->left, true, arrow) &&
        ast_arguments_length_only(node->right, false, arrow);
      
    case N_FOR_IN:
    case N_FOR_OF:
    case N_FOR_AWAIT_OF:
      return
        ast_arguments_length_only(node->left, true, arrow) &&
        ast_arguments_length_only(node->right, write, arrow) &&
        ast_arguments_length_only(node->body, write, arrow);
      
    case N_UPDATE:
    case N_DELETE:
      write = true;
      break;
      
    default: break;
  }

  const sv_ast_t *child;
  SV_AST_FOR_EACH_CHILD(node, child)
    if (!ast_arguments_length_only(child, write, arrow)) return false;

  return true;
}

bool ast_contains_direct_suspend(const sv_ast_t *node, const sv_ast_t **out_offender) {
  if (!node) return false;

  if (node->type == N_AWAIT || node->type == N_YIELD || node->type == N_FOR_AWAIT_OF) {
    if (out_offender) *out_offender = node;
    return true;
  }

  if (node->type == N_FUNC || node->type == N_ARROW) {
    bool plain_arrow = (node->flags & FN_ARROW) && !(node->flags & FN_ASYNC);
    if (!plain_arrow) return false;
  }

  const sv_ast_t *child;
  SV_AST_FOR_EACH_CHILD(node, child)
    if (ast_contains_direct_suspend(child, out_offender)) return true;

  return false;
}

bool ast_contains_own_yield(const sv_ast_t *node, const sv_ast_t **out_offender) {
  if (!node) return false;

  if (node->type == N_YIELD) {
    if (out_offender) *out_offender = node;
    return true;
  }

  if (node->type == N_FUNC || node->type == N_ARROW) return false;
  const sv_ast_t *child;
  SV_AST_FOR_EACH_CHILD(node, child)
    if (ast_contains_own_yield(child, out_offender)) return true;

  return false;
}

bool ast_contains_lexical_new_target(const sv_ast_t *node) {
  if (!node) return false;
  if (node->type == N_NEW_TARGET) return true;
  if (node->type == N_FUNC && !(node->flags & FN_ARROW)) return false;
  
  if (node->type == N_CLASS) {
    if (ast_contains_lexical_new_target(node->left)) return true;
    for (int i = 0; i < node->args.count; i++) {
      const sv_ast_t *member = node->args.items[i];
      if (member->type == N_METHOD && (member->flags & FN_COMPUTED) &&
          ast_contains_lexical_new_target(member->left)) return true;
    }
    return false;
  }
  
  const sv_ast_t *child;
  SV_AST_FOR_EACH_CHILD(node, child)
    if (ast_contains_lexical_new_target(child)) return true;
  
  return false;
}

bool ast_contains_direct_eval(const sv_ast_t *node) {
  if (!node || (node->type == N_FUNC && !(node->flags & FN_ARROW))) return false;
  if (
    node->type == N_CALL && node->left && node->left->type == N_IDENT &&
    node->left->len == 4 && memcmp(node->left->str, "eval", 4) == 0
  ) return true;

  const sv_ast_t *child;
  SV_AST_FOR_EACH_CHILD(node, child)
    if (ast_contains_direct_eval(child)) return true;
  
  return false;
}

static bool ast_closure_captures_name_impl(
  const sv_ast_t *node, const char *name, uint32_t len, bool in_closure
) {
  if (!node) return false;
  switch (node->type) {
    case N_FUNC: case N_ARROW: case N_CLASS: case N_METHOD: case N_STATIC_BLOCK:
      in_closure = true;
      break;
    case N_IDENT:
      if (in_closure && node->len == len && memcmp(node->str, name, len) == 0) return true;
      break;
    default: break;
  }
  if (
    node->type == N_CALL && node->left && node->left->type == N_IDENT &&
    node->left->len == 4 && memcmp(node->left->str, "eval", 4) == 0
  ) return true;

  const sv_ast_t *child;
  SV_AST_FOR_EACH_CHILD(node, child)
    if (ast_closure_captures_name_impl(child, name, len, in_closure)) return true;

  return false;
}

bool ast_closure_captures_name(const sv_ast_t *node, const char *name, uint32_t len) {
  return ast_closure_captures_name_impl(node, name, len, false);
}

bool ast_mentions_name(const sv_ast_t *node, const char *name, uint32_t len) {
  return ast_closure_captures_name_impl(node, name, len, true);
}

static bool ast_references_new_target_impl(const sv_ast_t *node, bool in_arrow) {
  if (!node) return false;
  if (node->type == N_NEW_TARGET) return true;
  if (node->type == N_FUNC && !(node->flags & FN_ARROW)) return false;

  bool lexical_arrow = in_arrow ||
    (node->type == N_FUNC && (node->flags & FN_ARROW));
  if (
    lexical_arrow && node->type == N_CALL &&
    node->left && node->left->type == N_IDENT &&
    node->left->len == 5 && memcmp(node->left->str, "super", 5) == 0
  ) return true;

  const sv_ast_t *child;
  SV_AST_FOR_EACH_CHILD(node, child)
    if (ast_references_new_target_impl(child, lexical_arrow)) return true;
    
  return false;
}

bool ast_references_new_target(const sv_ast_t *node) {
  return ast_references_new_target_impl(node, false);
}
