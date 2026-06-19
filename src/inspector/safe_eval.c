#include "bind.h"
#include "internal.h"
#include "runtime.h"
#include "silver/ast.h"
#include "tokens.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool inspector_ident_char(char c, bool first) {
  unsigned char uc = (unsigned char)c;
  return c == '_' || c == '$' || isalpha(uc) || (!first && isdigit(uc));
}

bool inspector_eval_safe_member_expr(ant_t *js, const char *expr, size_t expr_len, ant_value_t *out) {
  if (!js || !expr || !out) return false;
  while (expr_len > 0 && isspace((unsigned char)*expr)) {
    expr++;
    expr_len--;
  }
  while (expr_len > 0 && isspace((unsigned char)expr[expr_len - 1])) expr_len--;
  while (expr_len > 0 && expr[expr_len - 1] == '.') expr_len--;
  if (expr_len == 0) return false;

  ant_value_t cur = js_glob(js);
  size_t pos = 0;
  bool first_part = true;
  while (pos < expr_len) {
    if (!inspector_ident_char(expr[pos], true)) return false;
    size_t start = pos++;
    while (pos < expr_len && inspector_ident_char(expr[pos], false)) pos++;
    size_t len = pos - start;
    if (len == 0 || len >= 128) return false;

    char key[128];
    memcpy(key, expr + start, len);
    key[len] = '\0';

    if (
      first_part &&
      (strcmp(key, "globalThis") == 0 || strcmp(key, "global") == 0 || strcmp(key, "this") == 0)
    ) {
      cur = js_glob(js);
    } else {
      if (first_part) cur = js_get(js, js_glob(js), key);
      else cur = is_object_type(cur) || vtype(cur) == T_CFUNC ? js_get(js, cur, key) : js_mkundef();
    }

    first_part = false;
    if (pos == expr_len) {
      *out = cur;
      return true;
    }
    if (expr[pos] != '.') return false;
    pos++;
    if (pos == expr_len) {
      *out = cur;
      return true;
    }
  }

  *out = cur;
  return true;
}

static bool inspector_value_is_primitive_key(ant_value_t value) {
  switch (vtype(value)) {
    case T_STR:
    case T_NUM:
    case T_BOOL:
    case T_NULL:
    case T_UNDEF:
      return true;
    default:
      return false;
  }
}

static bool inspector_expr_delimiters_balanced(const char *expr, size_t expr_len) {
  char stack[256];
  size_t depth = 0;
  char quote = '\0';
  bool escape = false;

  for (size_t i = 0; i < expr_len; i++) {
    char c = expr[i];
    if (quote) {
      if (escape) {
        escape = false;
        continue;
      }
      if (c == '\\') {
        escape = true;
        continue;
      }
      if (c == quote) quote = '\0';
      continue;
    }

    if (c == '\'' || c == '"' || c == '`') {
      quote = c;
      continue;
    }
    if (c == '(' || c == '[' || c == '{') {
      if (depth >= sizeof(stack)) return false;
      stack[depth++] = c;
      continue;
    }
    if (c == ')' || c == ']' || c == '}') {
      if (depth == 0) return false;
      char open = stack[--depth];
      if (
        (c == ')' && open != '(') ||
        (c == ']' && open != '[') ||
        (c == '}' && open != '{')
      ) return false;
    }
  }

  return depth == 0 && quote == '\0' && !escape;
}

static bool inspector_value_to_key(
  ant_t *js,
  ant_value_t value,
  char *buf,
  size_t buf_len,
  const char **out_key,
  size_t *out_key_len
) {
  if (!js || !buf || buf_len == 0 || !out_key || !out_key_len) return false;
  if (!inspector_value_is_primitive_key(value)) return false;

  if (vtype(value) == T_STR) {
    size_t len = 0;
    const char *str = js_getstr(js, value, &len);
    if (!str || memchr(str, '\0', len)) return false;
    *out_key = str;
    *out_key_len = len;
    return true;
  }

  js_cstr_t cstr = js_to_cstr(js, value, buf, buf_len);
  size_t len = strlen(cstr.ptr);
  if (len >= buf_len) {
    if (cstr.needs_free) free((void *)cstr.ptr);
    return false;
  }
  if (cstr.ptr != buf) memcpy(buf, cstr.ptr, len + 1);
  if (cstr.needs_free) free((void *)cstr.ptr);
  *out_key = buf;
  *out_key_len = len;
  return true;
}

static bool inspector_safe_get_prop(
  ant_t *js,
  ant_value_t obj,
  const char *key,
  size_t key_len,
  ant_value_t *out
) {
  if (!js || !key || !out || memchr(key, '\0', key_len)) return false;
  if (!is_object_type(obj) && vtype(obj) != T_CFUNC && vtype(obj) != T_STR)
    return false;

  char stack_key[128];
  char *heap_key = NULL;
  const char *key_z = stack_key;
  if (key_len + 1 <= sizeof(stack_key)) {
    memcpy(stack_key, key, key_len);
    stack_key[key_len] = '\0';
  } else {
    heap_key = malloc(key_len + 1);
    if (!heap_key) return false;
    memcpy(heap_key, key, key_len);
    heap_key[key_len] = '\0';
    key_z = heap_key;
  }

  *out = js_getprop_fallback(js, obj, key_z);
  free(heap_key);
  return !is_err(*out);
}

static bool inspector_safe_get_existing_prop(
  ant_t *js,
  ant_value_t obj,
  const char *key,
  size_t key_len,
  ant_value_t *out
) {
  if (!js || !key || !out || memchr(key, '\0', key_len)) return false;
  if (lkp_proto(js, obj, key, key_len) == 0) return false;
  return inspector_safe_get_prop(js, obj, key, key_len, out);
}

static bool inspector_static_property_key(
  ant_t *js,
  sv_ast_t *node,
  char *buf,
  size_t buf_len,
  const char **out_key,
  size_t *out_key_len
) {
  if (!node) return false;
  if (node->type == N_IDENT || node->type == N_STRING) {
    if (!node->str || memchr(node->str, '\0', node->len)) return false;
    *out_key = node->str;
    *out_key_len = node->len;
    return true;
  }
  if (node->type == N_NUMBER) {
    ant_value_t key_value = tov(node->num);
    return inspector_value_to_key(js, key_value, buf, buf_len, out_key, out_key_len);
  }
  return false;
}

static bool inspector_safe_binary_numeric(
  ant_t *js,
  uint8_t op,
  ant_value_t left,
  ant_value_t right,
  ant_value_t *out
) {
  if (is_object_type(left) || is_object_type(right)) return false;
  double l = js_to_number(js, left);
  double r = js_to_number(js, right);
  switch (op) {
    case TOK_MINUS: *out = tov(l - r); return true;
    case TOK_MUL: *out = tov(l * r); return true;
    case TOK_DIV: *out = tov(l / r); return true;
    case TOK_REM: *out = tov(fmod(l, r)); return true;
    case TOK_EXP: *out = tov(pow(l, r)); return true;
    case TOK_LT: *out = js_bool(l < r); return true;
    case TOK_LE: *out = js_bool(l <= r); return true;
    case TOK_GT: *out = js_bool(l > r); return true;
    case TOK_GE: *out = js_bool(l >= r); return true;
    case TOK_SHL:
      *out = tov((double)(js_to_int32(l) << (js_to_uint32(r) & 31)));
      return true;
    case TOK_SHR:
      *out = tov((double)(js_to_int32(l) >> (js_to_uint32(r) & 31)));
      return true;
    case TOK_ZSHR:
      *out = tov((double)(js_to_uint32(l) >> (js_to_uint32(r) & 31)));
      return true;
    case TOK_AND:
      *out = tov((double)(js_to_int32(l) & js_to_int32(r)));
      return true;
    case TOK_OR:
      *out = tov((double)(js_to_int32(l) | js_to_int32(r)));
      return true;
    case TOK_XOR:
      *out = tov((double)(js_to_int32(l) ^ js_to_int32(r)));
      return true;
    default:
      return false;
  }
}

static bool inspector_safe_abstract_eq(
  ant_t *js,
  ant_value_t left,
  ant_value_t right,
  bool *out
) {
  uint8_t lt = vtype(left);
  uint8_t rtype = vtype(right);

  if ((lt == T_NULL && rtype == T_UNDEF) || (lt == T_UNDEF && rtype == T_NULL)) {
    *out = true;
    return true;
  }
  if (lt == rtype) {
    *out = strict_eq_values(js, left, right);
    return true;
  }
  if (lt == T_BOOL) {
    left = tov(vdata(left) ? 1.0 : 0.0);
    lt = T_NUM;
  }
  if (rtype == T_BOOL) {
    right = tov(vdata(right) ? 1.0 : 0.0);
    rtype = T_NUM;
  }
  if ((lt == T_NUM && rtype == T_STR) || (lt == T_STR && rtype == T_NUM)) {
    *out = js_to_number(js, left) == js_to_number(js, right);
    return true;
  }
  if (is_object_type(left) || is_object_type(right)) return false;

  *out = false;
  return true;
}

static bool inspector_safe_eval_ast(ant_t *js, sv_ast_t *node, ant_value_t *out) {
  if (!js || !node || !out) return false;

  switch (node->type) {
    case N_NUMBER:
      *out = tov(node->num);
      return true;
    case N_STRING:
      *out = js_mkstr(js, node->str ? node->str : "", node->len);
      return !is_err(*out);
    case N_BOOL:
      *out = js_bool(node->num != 0);
      return true;
    case N_NULL:
      *out = js_mknull();
      return true;
    case N_UNDEF:
      *out = js_mkundef();
      return true;
    case N_THIS:
    case N_GLOBAL_THIS:
      *out = js_glob(js);
      return true;
    case N_IDENT:
      if (!node->str || memchr(node->str, '\0', node->len)) return false;
      return inspector_safe_get_existing_prop(js, js_glob(js), node->str, node->len, out);
    case N_UNARY: {
      ant_value_t value = js_mkundef();
      if (!inspector_safe_eval_ast(js, node->right, &value)) return false;
      if (is_object_type(value)) return false;
      switch (node->op) {
        case TOK_NOT: *out = js_bool(!js_truthy(js, value)); return true;
        case TOK_TILDA: *out = tov((double)(~js_to_int32(js_to_number(js, value)))); return true;
        case TOK_UPLUS: *out = tov(js_to_number(js, value)); return true;
        case TOK_UMINUS: *out = tov(-js_to_number(js, value)); return true;
        default: return false;
      }
    }
    case N_TYPEOF: {
      ant_value_t value = js_mkundef();
      if (!inspector_safe_eval_ast(js, node->right, &value)) return false;
      const char *type_name = typestr(vtype(value));
      *out = js_mkstr(js, type_name, strlen(type_name));
      return !is_err(*out);
    }
    case N_BINARY: {
      ant_value_t left = js_mkundef();
      if (!inspector_safe_eval_ast(js, node->left, &left)) return false;

      switch (node->op) {
        case TOK_LAND:
          if (!js_truthy(js, left)) {
            *out = left;
            return true;
          }
          return inspector_safe_eval_ast(js, node->right, out);
        case TOK_LOR:
          if (js_truthy(js, left)) {
            *out = left;
            return true;
          }
          return inspector_safe_eval_ast(js, node->right, out);
        case TOK_NULLISH:
          if (vtype(left) != T_NULL && vtype(left) != T_UNDEF) {
            *out = left;
            return true;
          }
          return inspector_safe_eval_ast(js, node->right, out);
        default:
          break;
      }

      ant_value_t right = js_mkundef();
      if (!inspector_safe_eval_ast(js, node->right, &right)) return false;

      switch (node->op) {
        case TOK_PLUS:
          if (is_object_type(left) || is_object_type(right)) return false;
          if (vtype(left) == T_STR || vtype(right) == T_STR) {
            ant_value_t l_str = coerce_to_str_concat(js, left);
            ant_value_t r_str = coerce_to_str_concat(js, right);
            if (is_err(l_str) || is_err(r_str)) return false;
            *out = do_string_op(js, TOK_PLUS, l_str, r_str);
            return !is_err(*out);
          }
          *out = tov(js_to_number(js, left) + js_to_number(js, right));
          return true;
        case TOK_EQ:
        case TOK_NE: {
          bool equal = false;
          if (!inspector_safe_abstract_eq(js, left, right, &equal)) return false;
          *out = js_bool(node->op == TOK_EQ ? equal : !equal);
          return true;
        }
        case TOK_SEQ:
          *out = js_bool(strict_eq_values(js, left, right));
          return true;
        case TOK_SNE:
          *out = js_bool(!strict_eq_values(js, left, right));
          return true;
        case TOK_IN:
        case TOK_INSTANCEOF:
          return false;
        default:
          return inspector_safe_binary_numeric(js, node->op, left, right, out);
      }
    }
    case N_TERNARY: {
      ant_value_t cond = js_mkundef();
      if (!inspector_safe_eval_ast(js, node->cond, &cond)) return false;
      return inspector_safe_eval_ast(js, js_truthy(js, cond) ? node->left : node->right, out);
    }
    case N_SEQUENCE:
      if (!inspector_safe_eval_ast(js, node->left, out)) return false;
      return inspector_safe_eval_ast(js, node->right, out);
    case N_MEMBER:
    case N_OPTIONAL: {
      ant_value_t obj = js_mkundef();
      if (!node->left || !node->right || !inspector_safe_eval_ast(js, node->left, &obj))
        return false;
      if (node->type == N_OPTIONAL && (vtype(obj) == T_NULL || vtype(obj) == T_UNDEF)) {
        *out = js_mkundef();
        return true;
      }
      if (vtype(obj) == T_NULL || vtype(obj) == T_UNDEF) return false;

      char key_buf[128];
      const char *key = NULL;
      size_t key_len = 0;
      if (node->flags & 1) {
        ant_value_t key_value = js_mkundef();
        if (!inspector_safe_eval_ast(js, node->right, &key_value)) return false;
        if (!inspector_value_to_key(js, key_value, key_buf, sizeof(key_buf), &key, &key_len))
          return false;
      } else if (!inspector_static_property_key(js, node->right, key_buf, sizeof(key_buf), &key, &key_len)) {
        return false;
      }

      return inspector_safe_get_prop(js, obj, key, key_len, out);
    }
    case N_ARRAY: {
      ant_value_t arr = js_mkarr(js);
      if (is_err(arr)) return false;

      for (int i = 0; i < node->args.count; i++) {
        sv_ast_t *elem = node->args.items[i];
        ant_value_t value = js_mkundef();
        if (elem && elem->type == N_SPREAD) return false;
        if (elem && elem->type != N_EMPTY && !inspector_safe_eval_ast(js, elem, &value))
          return false;
        js_arr_push(js, arr, value);
      }

      *out = arr;
      return true;
    }
    case N_OBJECT: {
      ant_value_t obj = js_newobj(js);
      if (is_err(obj)) return false;

      for (int i = 0; i < node->args.count; i++) {
        sv_ast_t *prop = node->args.items[i];
        if (!prop || prop->type == N_SPREAD) return false;
        if (prop->type != N_PROPERTY || !prop->left || !prop->right) return false;
        if (prop->flags & (FN_GETTER | FN_SETTER)) return false;
        if (prop->right->type == N_FUNC) return false;

        char key_buf[128];
        const char *key = NULL;
        size_t key_len = 0;
        if (prop->flags & FN_COMPUTED) {
          ant_value_t key_value = js_mkundef();
          if (!inspector_safe_eval_ast(js, prop->left, &key_value)) return false;
          if (!inspector_value_to_key(js, key_value, key_buf, sizeof(key_buf), &key, &key_len))
            return false;
        } else if (!inspector_static_property_key(js, prop->left, key_buf, sizeof(key_buf), &key, &key_len)) {
          return false;
        }

        ant_value_t value = js_mkundef();
        if (!inspector_safe_eval_ast(js, prop->right, &value)) return false;
        if (is_err(js_mkprop_fast(js, obj, key, key_len, value))) return false;
      }

      *out = obj;
      return true;
    }
    default:
      return false;
  }
}

bool inspector_eval_safe_expr(ant_t *js, const char *expr, size_t expr_len, ant_value_t *out) {
  if (!js || !expr || !out) return false;
  while (expr_len > 0 && isspace((unsigned char)*expr)) {
    expr++;
    expr_len--;
  }
  while (expr_len > 0 && isspace((unsigned char)expr[expr_len - 1])) expr_len--;
  if (expr_len == 0) return false;
  if (!inspector_expr_delimiters_balanced(expr, expr_len)) return false;

  if (expr_len > SIZE_MAX - 3) return false;
  char *wrapped = malloc(expr_len + 3);
  if (!wrapped) return false;
  wrapped[0] = '(';
  memcpy(wrapped + 1, expr, expr_len);
  wrapped[expr_len + 1] = ')';
  wrapped[expr_len + 2] = '\0';

  bool saved_thrown = js->thrown_exists;
  ant_value_t saved_thrown_value = js->thrown_value;
  ant_value_t saved_thrown_stack = js->thrown_stack;
  inspector_clear_exception_state(js);

  bool ok = false;
  code_arena_mark_t parse_mark = parse_arena_mark();
  sv_ast_t *program = sv_parse(js, wrapped, (ant_offset_t)(expr_len + 2), false);
  if (
    program && program->type == N_PROGRAM &&
    program->args.count == 1 &&
    program->args.items[0] &&
    !js->thrown_exists
  ) {
    ok = inspector_safe_eval_ast(js, program->args.items[0], out);
    if (ok && is_err(*out)) ok = false;
  }
  parse_arena_rewind(parse_mark);
  free(wrapped);

  js->thrown_exists = saved_thrown;
  js->thrown_value = saved_thrown_value;
  js->thrown_stack = saved_thrown_stack;
  return ok;
}
