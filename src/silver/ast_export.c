#include "silver/ast_export.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "errors.h"
#include "gc/roots.h"
#include "internal.h"
#include "silver/ast.h"
#include "tokens.h"

typedef enum {
  SYNTAX_ROLE_EXPRESSION,
  SYNTAX_ROLE_STATEMENT,
  SYNTAX_ROLE_DECLARATION,
  SYNTAX_ROLE_PATTERN,
  SYNTAX_ROLE_CLASS_ELEMENT,
  SYNTAX_ROLE_IMPORT_SPECIFIER,
  SYNTAX_ROLE_EXPORT_SPECIFIER,
} syntax_role_t;

typedef struct {
  ant_value_t value;
  uint32_t start;
  uint32_t end;
  bool has_span;
} syntax_exported_t;

typedef struct {
  ant_t *js;
  const char *source;
  size_t source_len;
  bool locations;
  bool failed;
  uint32_t *utf16_at;
  uint32_t *line_starts;
  size_t line_count;
  gc_temp_root_scope_t roots;
} syntax_export_ctx_t;

static bool syntax_value_needs_root(ant_value_t value) {
  if (!is_tagged(value)) return false;
  uint8_t type = vtype(value);
  return 
    type == T_STR  || type == T_OBJ || type == T_ARR ||
    type == T_FUNC || type == T_BIGINT;
}

static ant_value_t syntax_pin(syntax_export_ctx_t *ctx, ant_value_t value) {
  if (is_err(value)) {
    ctx->failed = true;
    return value;
  }
  if (syntax_value_needs_root(value) &&
      !gc_temp_root_handle_valid(gc_temp_root_add(&ctx->roots, value))) {
    ctx->failed = true;
  }
  return value;
}

static ant_value_t syntax_obj(syntax_export_ctx_t *ctx) {
  return syntax_pin(ctx, js_mkobj(ctx->js));
}

static ant_value_t syntax_arr(syntax_export_ctx_t *ctx) {
  return syntax_pin(ctx, js_mkarr(ctx->js));
}

static ant_value_t syntax_strn(syntax_export_ctx_t *ctx, const char *str, size_t len) {
  if (!str) return syntax_pin(ctx, js_mkstr(ctx->js, "", 0));
  return syntax_pin(ctx, js_mkstr(ctx->js, str, len));
}

static ant_value_t syntax_str(syntax_export_ctx_t *ctx, const char *str) {
  return syntax_strn(ctx, str, strlen(str));
}

static void syntax_set(syntax_export_ctx_t *ctx, ant_value_t obj, const char *key, ant_value_t value) {
  if (!ctx->failed) js_set(ctx->js, obj, key, value);
}

static void syntax_set_str(syntax_export_ctx_t *ctx, ant_value_t obj, const char *key, const char *value) {
  syntax_set(ctx, obj, key, syntax_str(ctx, value));
}

static void syntax_set_bool(syntax_export_ctx_t *ctx, ant_value_t obj, const char *key, bool value) {
  syntax_set(ctx, obj, key, js_bool(value));
}

static syntax_exported_t syntax_none(void) {
  return (syntax_exported_t){ .value = js_mknull() };
}

static void syntax_span_merge(syntax_exported_t *dst, const syntax_exported_t *child) {
  if (!child->has_span) return;
  if (!dst->has_span) {
    dst->start = child->start;
    dst->end = child->end;
    dst->has_span = true;
    return;
  }
  if (child->start < dst->start) dst->start = child->start;
  if (child->end > dst->end) dst->end = child->end;
}

static syntax_exported_t syntax_span_from_node(const syntax_export_ctx_t *ctx, const sv_ast_t *node) {
  syntax_exported_t out = {0};
  if (!node) return out;
  uint32_t start = node->src_off;
  uint32_t end = node->src_end;
  if (start > ctx->source_len) start = (uint32_t)ctx->source_len;
  if (end > ctx->source_len) end = (uint32_t)ctx->source_len;
  uintptr_t source_addr = (uintptr_t)ctx->source;
  uintptr_t node_str_addr = (uintptr_t)node->str;
  if (end <= start && node->type == N_IDENT &&
      node_str_addr >= source_addr && node_str_addr <= source_addr + ctx->source_len) {
    start = (uint32_t)(node_str_addr - source_addr);
    end = start + node->len;
    if (end > ctx->source_len) end = (uint32_t)ctx->source_len;
  }
  out.start = start;
  out.end = end > start ? end : start;
  out.has_span = node->type != N_IMPORT_SPEC || end > start;
  return out;
}

static size_t syntax_line_index(const syntax_export_ctx_t *ctx, uint32_t byte_offset) {
  size_t lo = 0, hi = ctx->line_count;
  while (lo + 1 < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (ctx->line_starts[mid] <= byte_offset) lo = mid;
    else hi = mid;
  }
  return lo;
}

static ant_value_t syntax_position(syntax_export_ctx_t *ctx, uint32_t byte_offset) {
  ant_value_t out = syntax_obj(ctx);
  size_t line_index = syntax_line_index(ctx, byte_offset);
  uint32_t line_start = ctx->line_starts[line_index];
  syntax_set(ctx, out, "line", js_mknum((double)(line_index + 1)));
  syntax_set(ctx, out, "column", js_mknum((double)(ctx->utf16_at[byte_offset] - ctx->utf16_at[line_start])));
  return out;
}

static void syntax_add_common(
  syntax_export_ctx_t *ctx,
  syntax_exported_t *out,
  const char *type
) {
  syntax_set_str(ctx, out->value, "type", type);
  if (!out->has_span) return;
  uint32_t start = out->start > ctx->source_len ? (uint32_t)ctx->source_len : out->start;
  uint32_t end = out->end > ctx->source_len ? (uint32_t)ctx->source_len : out->end;
  if (end < start) end = start;
  syntax_set(ctx, out->value, "start", js_mknum((double)ctx->utf16_at[start]));
  syntax_set(ctx, out->value, "end", js_mknum((double)ctx->utf16_at[end]));
  if (ctx->locations) {
    ant_value_t loc = syntax_obj(ctx);
    syntax_set(ctx, loc, "start", syntax_position(ctx, start));
    syntax_set(ctx, loc, "end", syntax_position(ctx, end));
    syntax_set(ctx, out->value, "loc", loc);
  }
}

static const char *syntax_operator(uint8_t op) {
  switch (op) {
  case TOK_POSTINC: return "++";
  case TOK_POSTDEC: return "--";
  case TOK_NOT: return "!";
  case TOK_TILDA: return "~";
  case TOK_UPLUS: return "+";
  case TOK_UMINUS: return "-";
  case TOK_EXP: return "**";
  case TOK_MUL: return "*";
  case TOK_DIV: return "/";
  case TOK_REM: return "%";
  case TOK_PLUS: return "+";
  case TOK_MINUS: return "-";
  case TOK_SHL: return "<<";
  case TOK_SHR: return ">>";
  case TOK_ZSHR: return ">>>";
  case TOK_LT: return "<";
  case TOK_LE: return "<=";
  case TOK_GT: return ">";
  case TOK_GE: return ">=";
  case TOK_EQ: return "==";
  case TOK_NE: return "!=";
  case TOK_SEQ: return "===";
  case TOK_SNE: return "!==";
  case TOK_AND: return "&";
  case TOK_XOR: return "^";
  case TOK_OR: return "|";
  case TOK_LAND: return "&&";
  case TOK_LOR: return "||";
  case TOK_NULLISH: return "??";
  case TOK_IN: return "in";
  case TOK_INSTANCEOF: return "instanceof";
  case TOK_ASSIGN: return "=";
  case TOK_PLUS_ASSIGN: return "+=";
  case TOK_MINUS_ASSIGN: return "-=";
  case TOK_MUL_ASSIGN: return "*=";
  case TOK_DIV_ASSIGN: return "/=";
  case TOK_REM_ASSIGN: return "%=";
  case TOK_SHL_ASSIGN: return "<<=";
  case TOK_SHR_ASSIGN: return ">>=";
  case TOK_ZSHR_ASSIGN: return ">>>=";
  case TOK_AND_ASSIGN: return "&=";
  case TOK_XOR_ASSIGN: return "^=";
  case TOK_OR_ASSIGN: return "|=";
  case TOK_EXP_ASSIGN: return "**=";
  case TOK_LOR_ASSIGN: return "||=";
  case TOK_LAND_ASSIGN: return "&&=";
  case TOK_NULLISH_ASSIGN: return "?" "?=";
  default: return "";
  }
}

static syntax_exported_t syntax_export_node(
  syntax_export_ctx_t *ctx,
  const sv_ast_t *node,
  syntax_role_t role
);

static ant_value_t syntax_export_list(
  syntax_export_ctx_t *ctx,
  const sv_ast_list_t *list,
  syntax_role_t role,
  syntax_exported_t *parent
) {
  ant_value_t out = syntax_arr(ctx);
  for (int i = 0; i < list->count && !ctx->failed; i++) {
    const sv_ast_t *item = list->items[i];
    if (item && item->type == N_EMPTY && role == SYNTAX_ROLE_EXPRESSION) {
      js_arr_push(ctx->js, out, js_mknull());
      continue;
    }
    syntax_exported_t child = syntax_export_node(ctx, item, role);
    js_arr_push(ctx->js, out, child.value);
    syntax_span_merge(parent, &child);
  }
  return out;
}

static syntax_exported_t syntax_named_identifier(
  syntax_export_ctx_t *ctx,
  const char *name,
  size_t name_len,
  const syntax_exported_t *span
) {
  syntax_exported_t out = span ? *span : (syntax_exported_t){0};
  out.value = syntax_obj(ctx);
  bool private_name = name_len > 0 && name[0] == '#';
  syntax_set(ctx, out.value, "name", syntax_strn(
    ctx,
    private_name ? name + 1 : name,
    private_name ? name_len - 1 : name_len
  ));
  syntax_add_common(ctx, &out, private_name ? "PrivateIdentifier" : "Identifier");
  return out;
}

static void syntax_set_child(
  syntax_export_ctx_t *ctx,
  syntax_exported_t *parent,
  const char *key,
  const sv_ast_t *node,
  syntax_role_t role
) {
  syntax_exported_t child = syntax_export_node(ctx, node, role);
  syntax_set(ctx, parent->value, key, child.value);
  syntax_span_merge(parent, &child);
}

static void syntax_append_sequence(
  syntax_export_ctx_t *ctx,
  ant_value_t array,
  const sv_ast_t *node,
  syntax_exported_t *parent
) {
  if (node && node->type == N_SEQUENCE) {
    syntax_append_sequence(ctx, array, node->left, parent);
    syntax_append_sequence(ctx, array, node->right, parent);
    return;
  }
  syntax_exported_t item = syntax_export_node(ctx, node, SYNTAX_ROLE_EXPRESSION);
  js_arr_push(ctx->js, array, item.value);
  syntax_span_merge(parent, &item);
}

static const char *syntax_var_kind(sv_var_kind_t kind) {
  switch (kind) {
  case SV_VAR_LET: return "let";
  case SV_VAR_CONST: return "const";
  case SV_VAR_USING: return "using";
  case SV_VAR_AWAIT_USING: return "await using";
  case SV_VAR_VAR: default: return "var";
  }
}

static syntax_exported_t syntax_export_function(
  syntax_export_ctx_t *ctx,
  const sv_ast_t *node,
  syntax_role_t role,
  syntax_exported_t out
) {
  bool arrow = (node->flags & FN_ARROW) != 0;
  const char *type = arrow ? "ArrowFunctionExpression" :
    (role == SYNTAX_ROLE_STATEMENT || role == SYNTAX_ROLE_DECLARATION
      ? "FunctionDeclaration" : "FunctionExpression");

  if (!arrow && node->str && node->len) {
    syntax_exported_t id = syntax_named_identifier(ctx, node->str, node->len, NULL);
    syntax_set(ctx, out.value, "id", id.value);
  } else if (!arrow) {
    syntax_set(ctx, out.value, "id", js_mknull());
  }
  syntax_set(ctx, out.value, "params", syntax_export_list(ctx, &node->args, SYNTAX_ROLE_PATTERN, &out));
  syntax_set_child(ctx, &out, "body", node->body,
    node->body && node->body->type == N_BLOCK ? SYNTAX_ROLE_STATEMENT : SYNTAX_ROLE_EXPRESSION);
  syntax_set_bool(ctx, out.value, "async", (node->flags & FN_ASYNC) != 0);
  syntax_set_bool(ctx, out.value, "generator", (node->flags & FN_GENERATOR) != 0);
  if (arrow) syntax_set_bool(ctx, out.value, "expression", node->body && node->body->type != N_BLOCK);
  syntax_add_common(ctx, &out, type);
  return out;
}

static syntax_exported_t syntax_export_property(
  syntax_export_ctx_t *ctx,
  const sv_ast_t *node,
  syntax_role_t role,
  syntax_exported_t out
) {
  bool pattern = role == SYNTAX_ROLE_PATTERN;
  syntax_set_child(ctx, &out, "key", node->left, SYNTAX_ROLE_EXPRESSION);
  syntax_set_child(ctx, &out, "value", node->right,
    pattern ? SYNTAX_ROLE_PATTERN : SYNTAX_ROLE_EXPRESSION);
  const char *kind = (node->flags & FN_GETTER) ? "get" :
                     (node->flags & FN_SETTER) ? "set" : "init";
  syntax_set_str(ctx, out.value, "kind", kind);
  syntax_set_bool(ctx, out.value, "computed", (node->flags & FN_COMPUTED) != 0);
  syntax_set_bool(ctx, out.value, "method", node->right && node->right->type == N_FUNC);
  syntax_set_bool(ctx, out.value, "shorthand", !(node->flags & FN_COLON) &&
    node->right && node->right->type != N_FUNC);
  syntax_add_common(ctx, &out, "Property");
  return out;
}

static syntax_exported_t syntax_export_class_element(
  syntax_export_ctx_t *ctx,
  const sv_ast_t *node,
  syntax_exported_t out
) {
  if (node->type == N_STATIC_BLOCK) {
    syntax_set(ctx, out.value, "body", syntax_export_list(ctx, &node->args, SYNTAX_ROLE_STATEMENT, &out));
    syntax_add_common(ctx, &out, "StaticBlock");
    return out;
  }

  bool method = node->right && node->right->type == N_FUNC;
  syntax_set_child(ctx, &out, "key", node->left, SYNTAX_ROLE_EXPRESSION);
  syntax_set_bool(ctx, out.value, "computed", (node->flags & FN_COMPUTED) != 0);
  syntax_set_bool(ctx, out.value, "static", (node->flags & FN_STATIC) != 0);
  if (method) {
    syntax_set_child(ctx, &out, "value", node->right, SYNTAX_ROLE_EXPRESSION);
    const char *kind = (node->flags & FN_GETTER) ? "get" :
                       (node->flags & FN_SETTER) ? "set" :
      (node->left && node->left->type == N_IDENT && node->left->len == 11 &&
       memcmp(node->left->str, "constructor", 11) == 0 ? "constructor" : "method");
    syntax_set_str(ctx, out.value, "kind", kind);
    syntax_add_common(ctx, &out, "MethodDefinition");
  } else {
    if (!node->right || node->right->type == N_UNDEF)
      syntax_set(ctx, out.value, "value", js_mknull());
    else syntax_set_child(ctx, &out, "value", node->right, SYNTAX_ROLE_EXPRESSION);
    syntax_add_common(ctx, &out, "PropertyDefinition");
  }
  return out;
}

static syntax_exported_t syntax_export_node(
  syntax_export_ctx_t *ctx,
  const sv_ast_t *node,
  syntax_role_t role
) {
  if (!node) return syntax_none();

  if (role == SYNTAX_ROLE_STATEMENT && sv_ast_can_be_expression_statement(node)) {
    syntax_exported_t expression = syntax_export_node(ctx, node, SYNTAX_ROLE_EXPRESSION);
    syntax_exported_t statement = expression;
    statement.value = syntax_obj(ctx);
    syntax_set(ctx, statement.value, "expression", expression.value);
    syntax_add_common(ctx, &statement, "ExpressionStatement");
    return statement;
  }

  syntax_exported_t out = syntax_span_from_node(ctx, node);
  out.value = syntax_obj(ctx);
  if (ctx->failed) return out;

  switch (node->type) {
  case N_NUMBER:
    syntax_set(ctx, out.value, "value", js_mknum(node->num));
    syntax_add_common(ctx, &out, "Literal");
    return out;
  case N_STRING:
    syntax_set(ctx, out.value, "value", syntax_strn(ctx, node->str, node->len));
    syntax_add_common(ctx, &out, "Literal");
    return out;
  case N_BIGINT: {
    size_t len = node->len;
    if (len && node->str[len - 1] == 'n') len--;
    syntax_set(ctx, out.value, "value", js_mknull());
    syntax_set(ctx, out.value, "bigint", syntax_strn(ctx, node->str, len));
    syntax_add_common(ctx, &out, "Literal");
    return out;
  }
  case N_BOOL:
    syntax_set(ctx, out.value, "value", js_bool(node->num != 0));
    syntax_add_common(ctx, &out, "Literal");
    return out;
  case N_NULL:
    syntax_set(ctx, out.value, "value", js_mknull());
    syntax_add_common(ctx, &out, "Literal");
    return out;
  case N_UNDEF: {
    syntax_exported_t id = syntax_named_identifier(ctx, "undefined", 9, &out);
    return id;
  }
  case N_THIS:
    syntax_add_common(ctx, &out, "ThisExpression");
    return out;
  case N_GLOBAL_THIS: {
    syntax_exported_t id = syntax_named_identifier(ctx, "globalThis", 10, &out);
    return id;
  }
  case N_IDENT:
    return syntax_named_identifier(ctx, node->str, node->len, &out);
  case N_REGEXP: {
    ant_value_t regex = syntax_obj(ctx);
    syntax_set(ctx, regex, "pattern", syntax_strn(ctx, node->str, node->len));
    syntax_set(ctx, regex, "flags", syntax_strn(ctx, node->aux, node->aux_len));
    syntax_set(ctx, out.value, "value", js_mknull());
    syntax_set(ctx, out.value, "regex", regex);
    syntax_add_common(ctx, &out, "Literal");
    return out;
  }
  case N_TEMPLATE: {
    ant_value_t quasis = syntax_arr(ctx);
    ant_value_t expressions = syntax_arr(ctx);
    int quasi_index = 0;
    int quasi_count = (node->args.count + 1) / 2;
    for (int i = 0; i < node->args.count; i++) {
      const sv_ast_t *part = node->args.items[i];
      if ((i & 1) == 0) {
        syntax_exported_t quasi = syntax_span_from_node(ctx, part);
        quasi.value = syntax_obj(ctx);
        ant_value_t value = syntax_obj(ctx);
        syntax_set(ctx, value, "raw", syntax_strn(ctx, part->aux, part->aux_len));
        syntax_set(ctx, value, "cooked", (part->flags & FN_INVALID_COOKED)
          ? js_mknull() : syntax_strn(ctx, part->str, part->len));
        syntax_set(ctx, quasi.value, "value", value);
        syntax_set_bool(ctx, quasi.value, "tail", ++quasi_index == quasi_count);
        syntax_add_common(ctx, &quasi, "TemplateElement");
        js_arr_push(ctx->js, quasis, quasi.value);
      } else {
        syntax_exported_t expression = syntax_export_node(ctx, part, SYNTAX_ROLE_EXPRESSION);
        js_arr_push(ctx->js, expressions, expression.value);
        syntax_span_merge(&out, &expression);
      }
    }
    syntax_set(ctx, out.value, "quasis", quasis);
    syntax_set(ctx, out.value, "expressions", expressions);
    syntax_add_common(ctx, &out, "TemplateLiteral");
    return out;
  }
  case N_BINARY:
    syntax_set_str(ctx, out.value, "operator", syntax_operator(node->op));
    syntax_set_child(ctx, &out, "left", node->left, SYNTAX_ROLE_EXPRESSION);
    syntax_set_child(ctx, &out, "right", node->right, SYNTAX_ROLE_EXPRESSION);
    syntax_add_common(ctx, &out,
      node->op == TOK_LAND || node->op == TOK_LOR || node->op == TOK_NULLISH
        ? "LogicalExpression" : "BinaryExpression");
    return out;
  case N_UNARY:
  case N_TYPEOF:
  case N_DELETE:
  case N_VOID: {
    const char *op = node->type == N_TYPEOF ? "typeof" :
                     node->type == N_DELETE ? "delete" :
                     node->type == N_VOID ? "void" : syntax_operator(node->op);
    syntax_set_str(ctx, out.value, "operator", op);
    syntax_set_bool(ctx, out.value, "prefix", true);
    syntax_set_child(ctx, &out, "argument", node->right, SYNTAX_ROLE_EXPRESSION);
    syntax_add_common(ctx, &out, "UnaryExpression");
    return out;
  }
  case N_UPDATE:
    syntax_set_str(ctx, out.value, "operator", syntax_operator(node->op));
    syntax_set_bool(ctx, out.value, "prefix", node->flags != 0);
    syntax_set_child(ctx, &out, "argument", node->right, SYNTAX_ROLE_EXPRESSION);
    syntax_add_common(ctx, &out, "UpdateExpression");
    return out;
  case N_ASSIGN:
    syntax_set_str(ctx, out.value, "operator", syntax_operator(node->op));
    syntax_set_child(ctx, &out, "left", node->left, SYNTAX_ROLE_PATTERN);
    syntax_set_child(ctx, &out, "right", node->right, SYNTAX_ROLE_EXPRESSION);
    syntax_add_common(ctx, &out, "AssignmentExpression");
    return out;
  case N_TERNARY:
    syntax_set_child(ctx, &out, "test", node->cond, SYNTAX_ROLE_EXPRESSION);
    syntax_set_child(ctx, &out, "consequent", node->left, SYNTAX_ROLE_EXPRESSION);
    syntax_set_child(ctx, &out, "alternate", node->right, SYNTAX_ROLE_EXPRESSION);
    syntax_add_common(ctx, &out, "ConditionalExpression");
    return out;
  case N_SEQUENCE: {
    ant_value_t expressions = syntax_arr(ctx);
    syntax_append_sequence(ctx, expressions, node, &out);
    syntax_set(ctx, out.value, "expressions", expressions);
    syntax_add_common(ctx, &out, "SequenceExpression");
    return out;
  }
  case N_CALL:
    syntax_set_child(ctx, &out, "callee", node->left, SYNTAX_ROLE_EXPRESSION);
    syntax_set(ctx, out.value, "arguments", syntax_export_list(ctx, &node->args, SYNTAX_ROLE_EXPRESSION, &out));
    syntax_set_bool(ctx, out.value, "optional", node->left && node->left->type == N_OPTIONAL);
    syntax_add_common(ctx, &out, "CallExpression");
    return out;
  case N_NEW:
    syntax_set_child(ctx, &out, "callee", node->left, SYNTAX_ROLE_EXPRESSION);
    syntax_set(ctx, out.value, "arguments", syntax_export_list(ctx, &node->args, SYNTAX_ROLE_EXPRESSION, &out));
    syntax_add_common(ctx, &out, "NewExpression");
    return out;
  case N_MEMBER:
  case N_OPTIONAL:
    syntax_set_child(ctx, &out, "object", node->left, SYNTAX_ROLE_EXPRESSION);
    syntax_set_child(ctx, &out, "property", node->right, SYNTAX_ROLE_EXPRESSION);
    syntax_set_bool(ctx, out.value, "computed", node->flags != 0);
    syntax_set_bool(ctx, out.value, "optional", node->type == N_OPTIONAL);
    syntax_add_common(ctx, &out, "MemberExpression");
    return out;
  case N_ARRAY:
  case N_ARRAY_PAT:
    syntax_set(ctx, out.value, "elements",
      syntax_export_list(ctx, &node->args,
        node->type == N_ARRAY_PAT || role == SYNTAX_ROLE_PATTERN ? SYNTAX_ROLE_PATTERN : SYNTAX_ROLE_EXPRESSION,
        &out));
    syntax_add_common(ctx, &out,
      node->type == N_ARRAY_PAT || role == SYNTAX_ROLE_PATTERN ? "ArrayPattern" : "ArrayExpression");
    return out;
  case N_OBJECT:
  case N_OBJECT_PAT:
    syntax_set(ctx, out.value, "properties", syntax_export_list(ctx, &node->args,
      node->type == N_OBJECT_PAT || role == SYNTAX_ROLE_PATTERN ? SYNTAX_ROLE_PATTERN : SYNTAX_ROLE_EXPRESSION,
      &out));
    syntax_add_common(ctx, &out,
      node->type == N_OBJECT_PAT || role == SYNTAX_ROLE_PATTERN ? "ObjectPattern" : "ObjectExpression");
    return out;
  case N_PROPERTY:
    return syntax_export_property(ctx, node, role, out);
  case N_SPREAD:
  case N_REST:
    syntax_set_child(ctx, &out, "argument", node->right,
      role == SYNTAX_ROLE_PATTERN || node->type == N_REST ? SYNTAX_ROLE_PATTERN : SYNTAX_ROLE_EXPRESSION);
    syntax_add_common(ctx, &out,
      role == SYNTAX_ROLE_PATTERN || node->type == N_REST ? "RestElement" : "SpreadElement");
    return out;
  case N_ASSIGN_PAT:
    syntax_set_child(ctx, &out, "left", node->left, SYNTAX_ROLE_PATTERN);
    syntax_set_child(ctx, &out, "right", node->right, SYNTAX_ROLE_EXPRESSION);
    syntax_add_common(ctx, &out, "AssignmentPattern");
    return out;
  case N_ARROW:
  case N_FUNC:
    return syntax_export_function(ctx, node, role, out);
  case N_YIELD:
    syntax_set_child(ctx, &out, "argument", node->right, SYNTAX_ROLE_EXPRESSION);
    syntax_set_bool(ctx, out.value, "delegate", node->flags != 0);
    syntax_add_common(ctx, &out, "YieldExpression");
    return out;
  case N_AWAIT:
    syntax_set_child(ctx, &out, "argument", node->right, SYNTAX_ROLE_EXPRESSION);
    syntax_add_common(ctx, &out, "AwaitExpression");
    return out;
  case N_TAGGED_TEMPLATE:
    syntax_set_child(ctx, &out, "tag", node->left, SYNTAX_ROLE_EXPRESSION);
    syntax_set_child(ctx, &out, "quasi", node->right, SYNTAX_ROLE_EXPRESSION);
    syntax_add_common(ctx, &out, "TaggedTemplateExpression");
    return out;
  case N_NEW_TARGET: {
    syntax_exported_t meta = syntax_named_identifier(ctx, "new", 3, NULL);
    syntax_exported_t property = syntax_named_identifier(ctx, "target", 6, NULL);
    syntax_set(ctx, out.value, "meta", meta.value);
    syntax_set(ctx, out.value, "property", property.value);
    syntax_add_common(ctx, &out, "MetaProperty");
    return out;
  }
  case N_IMPORT:
    syntax_set_child(ctx, &out, "source", node->right, SYNTAX_ROLE_EXPRESSION);
    syntax_set_child(ctx, &out, "options", node->left, SYNTAX_ROLE_EXPRESSION);
    syntax_add_common(ctx, &out, "ImportExpression");
    return out;
  case N_BLOCK:
    syntax_set(ctx, out.value, "body", syntax_export_list(ctx, &node->args, SYNTAX_ROLE_STATEMENT, &out));
    syntax_add_common(ctx, &out, "BlockStatement");
    return out;
  case N_VAR:
    syntax_set_str(ctx, out.value, "kind", syntax_var_kind(node->var_kind));
    syntax_set(ctx, out.value, "declarations", syntax_export_list(ctx, &node->args, SYNTAX_ROLE_PATTERN, &out));
    syntax_add_common(ctx, &out, "VariableDeclaration");
    return out;
  case N_VARDECL:
    syntax_set_child(ctx, &out, "id", node->left, SYNTAX_ROLE_PATTERN);
    syntax_set_child(ctx, &out, "init", node->right, SYNTAX_ROLE_EXPRESSION);
    syntax_add_common(ctx, &out, "VariableDeclarator");
    return out;
  case N_IF:
    syntax_set_child(ctx, &out, "test", node->cond, SYNTAX_ROLE_EXPRESSION);
    syntax_set_child(ctx, &out, "consequent", node->left, SYNTAX_ROLE_STATEMENT);
    syntax_set_child(ctx, &out, "alternate", node->right, SYNTAX_ROLE_STATEMENT);
    syntax_add_common(ctx, &out, "IfStatement");
    return out;
  case N_WHILE:
  case N_DO_WHILE:
    syntax_set_child(ctx, &out, "test", node->cond, SYNTAX_ROLE_EXPRESSION);
    syntax_set_child(ctx, &out, "body", node->body, SYNTAX_ROLE_STATEMENT);
    syntax_add_common(ctx, &out, node->type == N_WHILE ? "WhileStatement" : "DoWhileStatement");
    return out;
  case N_FOR:
    syntax_set_child(ctx, &out, "init", node->init, SYNTAX_ROLE_EXPRESSION);
    syntax_set_child(ctx, &out, "test", node->cond, SYNTAX_ROLE_EXPRESSION);
    syntax_set_child(ctx, &out, "update", node->update, SYNTAX_ROLE_EXPRESSION);
    syntax_set_child(ctx, &out, "body", node->body, SYNTAX_ROLE_STATEMENT);
    syntax_add_common(ctx, &out, "ForStatement");
    return out;
  case N_FOR_IN:
  case N_FOR_OF:
  case N_FOR_AWAIT_OF:
    syntax_set_child(ctx, &out, "left", node->left, SYNTAX_ROLE_PATTERN);
    syntax_set_child(ctx, &out, "right", node->right, SYNTAX_ROLE_EXPRESSION);
    syntax_set_child(ctx, &out, "body", node->body, SYNTAX_ROLE_STATEMENT);
    if (node->type != N_FOR_IN)
      syntax_set_bool(ctx, out.value, "await", node->type == N_FOR_AWAIT_OF);
    syntax_add_common(ctx, &out, node->type == N_FOR_IN ? "ForInStatement" : "ForOfStatement");
    return out;
  case N_RETURN:
    syntax_set_child(ctx, &out, "argument", node->right, SYNTAX_ROLE_EXPRESSION);
    syntax_add_common(ctx, &out, "ReturnStatement");
    return out;
  case N_THROW:
    syntax_set_child(ctx, &out, "argument", node->right, SYNTAX_ROLE_EXPRESSION);
    syntax_add_common(ctx, &out, "ThrowStatement");
    return out;
  case N_BREAK:
  case N_CONTINUE:
    if (node->str && node->len) {
      syntax_exported_t label = syntax_named_identifier(ctx, node->str, node->len, NULL);
      syntax_set(ctx, out.value, "label", label.value);
    } else syntax_set(ctx, out.value, "label", js_mknull());
    syntax_add_common(ctx, &out, node->type == N_BREAK ? "BreakStatement" : "ContinueStatement");
    return out;
  case N_TRY: {
    syntax_set_child(ctx, &out, "block", node->body, SYNTAX_ROLE_STATEMENT);
    if (node->catch_body) {
      syntax_exported_t handler = syntax_span_from_node(ctx, node->catch_body);
      handler.value = syntax_obj(ctx);
      syntax_set_child(ctx, &handler, "param", node->catch_param, SYNTAX_ROLE_PATTERN);
      syntax_set_child(ctx, &handler, "body", node->catch_body, SYNTAX_ROLE_STATEMENT);
      syntax_add_common(ctx, &handler, "CatchClause");
      syntax_set(ctx, out.value, "handler", handler.value);
      syntax_span_merge(&out, &handler);
    } else syntax_set(ctx, out.value, "handler", js_mknull());
    syntax_set_child(ctx, &out, "finalizer", node->finally_body, SYNTAX_ROLE_STATEMENT);
    syntax_add_common(ctx, &out, "TryStatement");
    return out;
  }
  case N_SWITCH:
    syntax_set_child(ctx, &out, "discriminant", node->cond, SYNTAX_ROLE_EXPRESSION);
    syntax_set(ctx, out.value, "cases", syntax_export_list(ctx, &node->args, SYNTAX_ROLE_STATEMENT, &out));
    syntax_add_common(ctx, &out, "SwitchStatement");
    return out;
  case N_CASE:
    syntax_set_child(ctx, &out, "test", node->left, SYNTAX_ROLE_EXPRESSION);
    syntax_set(ctx, out.value, "consequent", syntax_export_list(ctx, &node->args, SYNTAX_ROLE_STATEMENT, &out));
    syntax_add_common(ctx, &out, "SwitchCase");
    return out;
  case N_LABEL: {
    syntax_exported_t label = syntax_named_identifier(ctx, node->str, node->len, NULL);
    syntax_set(ctx, out.value, "label", label.value);
    syntax_set_child(ctx, &out, "body", node->body, SYNTAX_ROLE_STATEMENT);
    syntax_add_common(ctx, &out, "LabeledStatement");
    return out;
  }
  case N_DEBUGGER:
    syntax_add_common(ctx, &out, "DebuggerStatement");
    return out;
  case N_EMPTY:
    syntax_add_common(ctx, &out, "EmptyStatement");
    return out;
  case N_WITH:
    syntax_set_child(ctx, &out, "object", node->left, SYNTAX_ROLE_EXPRESSION);
    syntax_set_child(ctx, &out, "body", node->body, SYNTAX_ROLE_STATEMENT);
    syntax_add_common(ctx, &out, "WithStatement");
    return out;
  case N_CLASS: {
    bool declaration = (node->flags & FN_CLASS_DECL) != 0 || role == SYNTAX_ROLE_STATEMENT;
    if (node->str && node->len) {
      syntax_exported_t id = syntax_named_identifier(ctx, node->str, node->len, NULL);
      syntax_set(ctx, out.value, "id", id.value);
    } else syntax_set(ctx, out.value, "id", js_mknull());
    syntax_set_child(ctx, &out, "superClass", node->left, SYNTAX_ROLE_EXPRESSION);
    syntax_exported_t body = syntax_span_from_node(ctx, node);
    body.value = syntax_obj(ctx);
    syntax_set(ctx, body.value, "body", syntax_export_list(ctx, &node->args, SYNTAX_ROLE_CLASS_ELEMENT, &body));
    syntax_add_common(ctx, &body, "ClassBody");
    syntax_set(ctx, out.value, "body", body.value);
    syntax_span_merge(&out, &body);
    syntax_add_common(ctx, &out, declaration ? "ClassDeclaration" : "ClassExpression");
    return out;
  }
  case N_METHOD:
  case N_STATIC_BLOCK:
    return syntax_export_class_element(ctx, node, out);
  case N_IMPORT_DECL:
    syntax_set(ctx, out.value, "specifiers", syntax_export_list(ctx, &node->args, SYNTAX_ROLE_IMPORT_SPECIFIER, &out));
    syntax_set_child(ctx, &out, "source", node->right, SYNTAX_ROLE_EXPRESSION);
    syntax_add_common(ctx, &out, "ImportDeclaration");
    return out;
  case N_IMPORT_SPEC: {
    if (role == SYNTAX_ROLE_EXPORT_SPECIFIER) {
      syntax_set_child(ctx, &out, "local", node->left, SYNTAX_ROLE_EXPRESSION);
      syntax_set_child(ctx, &out, "exported", node->right, SYNTAX_ROLE_EXPRESSION);
      syntax_add_common(ctx, &out, "ExportSpecifier");
    } else if (node->flags & 1) {
      syntax_set_child(ctx, &out, "local", node->right, SYNTAX_ROLE_PATTERN);
      syntax_add_common(ctx, &out, "ImportDefaultSpecifier");
    } else if (node->flags & 2) {
      syntax_set_child(ctx, &out, "local", node->right, SYNTAX_ROLE_PATTERN);
      syntax_add_common(ctx, &out, "ImportNamespaceSpecifier");
    } else {
      syntax_set_child(ctx, &out, "imported", node->left, SYNTAX_ROLE_EXPRESSION);
      syntax_set_child(ctx, &out, "local", node->right, SYNTAX_ROLE_PATTERN);
      syntax_add_common(ctx, &out, "ImportSpecifier");
    }
    return out;
  }
  case N_EXPORT:
    if (node->flags & EX_DEFAULT) {
      syntax_set_child(ctx, &out, "declaration", node->left,
        node->left && (node->left->type == N_FUNC || node->left->type == N_CLASS)
          ? SYNTAX_ROLE_DECLARATION : SYNTAX_ROLE_EXPRESSION);
      syntax_add_common(ctx, &out, "ExportDefaultDeclaration");
    } else if (node->flags & EX_STAR) {
      syntax_set_child(ctx, &out, "source", node->right, SYNTAX_ROLE_EXPRESSION);
      if ((node->flags & EX_NAMESPACE) && node->args.count > 0)
        syntax_set_child(ctx, &out, "exported", node->args.items[0]->right, SYNTAX_ROLE_EXPRESSION);
      else syntax_set(ctx, out.value, "exported", js_mknull());
      syntax_add_common(ctx, &out, "ExportAllDeclaration");
    } else {
      syntax_set_child(ctx, &out, "declaration", node->left, SYNTAX_ROLE_STATEMENT);
      syntax_set(ctx, out.value, "specifiers", syntax_export_list(ctx, &node->args, SYNTAX_ROLE_EXPORT_SPECIFIER, &out));
      syntax_set_child(ctx, &out, "source", node->right, SYNTAX_ROLE_EXPRESSION);
      syntax_add_common(ctx, &out, "ExportNamedDeclaration");
    }
    return out;
  case N_PROGRAM:
    out.start = 0;
    out.end = (uint32_t)ctx->source_len;
    out.has_span = true;
    syntax_set_str(ctx, out.value, "schema", "ant.syntax@1");
    syntax_set(ctx, out.value, "body", syntax_export_list(ctx, &node->args, SYNTAX_ROLE_STATEMENT, &out));
    syntax_add_common(ctx, &out, "Program");
    return out;
  case N__COUNT:
    break;
  }

  ctx->failed = true;
  return out;
}

static bool syntax_build_offsets(syntax_export_ctx_t *ctx) {
  size_t len = ctx->source_len;
  ctx->utf16_at = malloc((len + 1) * sizeof(*ctx->utf16_at));
  ctx->line_starts = malloc((len + 1) * sizeof(*ctx->line_starts));
  if (!ctx->utf16_at || !ctx->line_starts) return false;

  uint32_t units = 0;
  ctx->line_starts[ctx->line_count++] = 0;
  size_t i = 0;
  while (i < len) {
    unsigned char c = (unsigned char)ctx->source[i];
    size_t width = 1;
    uint32_t cp = c;
    if ((c & 0xe0) == 0xc0 && i + 1 < len) {
      width = 2;
      cp = ((uint32_t)(c & 0x1f) << 6) | ((uint32_t)ctx->source[i + 1] & 0x3f);
    } else if ((c & 0xf0) == 0xe0 && i + 2 < len) {
      width = 3;
      cp = ((uint32_t)(c & 0x0f) << 12) |
           (((uint32_t)ctx->source[i + 1] & 0x3f) << 6) |
           ((uint32_t)ctx->source[i + 2] & 0x3f);
    } else if ((c & 0xf8) == 0xf0 && i + 3 < len) {
      width = 4;
      cp = ((uint32_t)(c & 0x07) << 18) |
           (((uint32_t)ctx->source[i + 1] & 0x3f) << 12) |
           (((uint32_t)ctx->source[i + 2] & 0x3f) << 6) |
           ((uint32_t)ctx->source[i + 3] & 0x3f);
    }

    for (size_t j = 0; j < width && i + j <= len; j++)
      ctx->utf16_at[i + j] = units;
    units += cp > 0xffff ? 2 : 1;
    if (c == '\n') ctx->line_starts[ctx->line_count++] = (uint32_t)(i + 1);
    i += width;
  }
  ctx->utf16_at[len] = units;
  return true;
}

ant_value_t sv_ast_export_public(
  ant_t *js,
  const sv_ast_t *program,
  const char *source,
  size_t source_len,
  const char *source_type,
  bool locations
) {
  syntax_export_ctx_t ctx = {
    .js = js,
    .source = source,
    .source_len = source_len,
    .locations = locations,
  };

  if (!syntax_build_offsets(&ctx)) {
    free(ctx.utf16_at);
    free(ctx.line_starts);
    return js_mkerr_typed(js, JS_ERR_INTERNAL, "ant:syntax parse export ran out of memory");
  }

  gc_temp_root_scope_begin(js, &ctx.roots);
  syntax_exported_t root = syntax_export_node(&ctx, program, SYNTAX_ROLE_STATEMENT);
  if (!ctx.failed) syntax_set_str(&ctx, root.value, "sourceType", source_type);

  ant_value_t result = root.value;
  if (ctx.failed)
    result = js_mkerr_typed(js, JS_ERR_INTERNAL, "ant:syntax could not export the syntax tree");

  gc_temp_root_scope_end(&ctx.roots);
  free(ctx.utf16_at);
  free(ctx.line_starts);
  return result;
}
