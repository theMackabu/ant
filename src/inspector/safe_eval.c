#include "gc/roots.h"
#include "bind.h"
#include "arrays.h"
#include "internal.h"
#include "cfunc.h"
#include "runtime.h"
#include "silver/ast.h"
#include "silver/eval_env.h"
#include "tokens.h"
#include "modules/math.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool inspector_safe_get_prop(
  ant_t *js,
  ant_value_t obj,
  const char *key,
  size_t key_len,
  ant_value_t *out
) {
  if (!js || !key || !out || memchr(key, '\0', key_len)) return false;

  uint8_t type = vtype(obj);
  if (type == kTypeString) {
    if (key_len == 6 && memcmp(key, "length", 6) == 0) {
      *out = Ant_Value_FromNumber((double)str_utf16_len(js, obj));
      return true;
    }
    obj = js_primitive_prototype(js, type);
  } else if (type == kTypeBuiltin) {
    ant_value_t promoted = js_cfunc_lookup_promoted(js, obj);
    if (vtype(promoted) == kTypeFunction) {
      obj = promoted;
    } else {
      if (
        (key_len == 4 && memcmp(key, "name", 4) == 0) ||
        (key_len == 6 && memcmp(key, "length", 6) == 0)
      ) {
        *out = js_getprop_fallback_len(js, obj, key, key_len);
        return !is_err(*out);
      }
      obj = js_primitive_prototype(js, type);
    }
  } else if (!is_object_type(obj)) return false;

  for (int depth = 0; is_object_type(obj) && depth < MAX_PROTO_CHAIN_DEPTH; depth++) {
    ant_object_t *ptr = js_obj_ptr(obj);
    ant_value_t current = js_obj_from_ptr(ptr);
    if (!ptr) return false;
    if (is_proxy(current)) return false;

    unsigned long index = 0;
    if (vtype(obj) == kTypeArray && parse_array_index(key, key_len, js_arr_len(js, obj), &index)) {
      ant_value_t element = js_arr_get(js, obj, (ant_offset_t)index);
      *out = is_empty_slot(element) ? Ant_Value_Undefined : element;
      return true;
    }

    ant_value_t value = Ant_Value_Undefined;
    if (js_try_get_own_data_prop(js, current, key, key_len, &value)) {
      *out = value;
      return true;
    }

    prop_meta_t meta;
    if (lookup_string_prop_meta(js, current, key, key_len, &meta))
      return false;

    if (ptr && ant_object_exotic_ops(ptr) && ant_object_exotic_ops(ptr)->getter) return false;
    obj = js_get_proto(js, current);
  }

  return false;
}

static bool inspector_safe_get_binding(
  ant_t *js, const char *name, size_t len, ant_value_t *out
) {
  if (sv_global_lexical_lookup(js, name, len, out))
    return !is_empty_slot(*out);
  return inspector_safe_get_prop(js, js_glob(js), name, len, out);
}

static bool inspector_safe_eval_ast(ant_t *js, sv_ast_t *node, ant_value_t *out);

static bool inspector_safe_key(
  ant_t *js,
  sv_ast_t *node,
  bool computed,
  char *buf,
  size_t buf_len,
  const char **key,
  size_t *key_len
) {
  ant_value_t value = Ant_Value_Undefined;
  if (!node) return false;

  if (!computed && (node->type == N_IDENT || node->type == N_STRING)) {
    *key = node->str;
    *key_len = node->len;
  } else {
    if (!computed && node->type != N_NUMBER) return false;
    if (!inspector_safe_eval_ast(js, node, &value)) return false;

    switch (vtype(value)) {
      case kTypeString:
        *key = js_getstr(js, value, key_len);
        break;
      case kTypeNumber:
      case kTypeBool:
      case kTypeNull:
      case kTypeUndefined: {
        js_cstr_t cstr = js_to_cstr(js, value, buf, buf_len);
        size_t len = strlen(cstr.ptr);
        bool fits = len < buf_len;
        if (fits && cstr.ptr != buf) memcpy(buf, cstr.ptr, len + 1);
        if (cstr.needs_free) free((void *)cstr.ptr);
        if (!fits) return false;
        *key = buf;
        *key_len = len;
        break;
      }
      default:
        return false;
    }
  }

  return *key && !memchr(*key, '\0', *key_len);
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
    case TOK_MINUS: *out = Ant_Value_FromNumber(l - r); return true;
    case TOK_MUL: *out = Ant_Value_FromNumber(l * r); return true;
    case TOK_DIV: *out = Ant_Value_FromNumber(l / r); return true;
    case TOK_REM: *out = Ant_Value_FromNumber(fmod(l, r)); return true;
    case TOK_EXP: *out = Ant_Value_FromNumber(ant_math_js_pow(l, r)); return true;
    case TOK_LT: *out = Ant_Value_FromBool(l < r); return true;
    case TOK_LE: *out = Ant_Value_FromBool(l <= r); return true;
    case TOK_GT: *out = Ant_Value_FromBool(l > r); return true;
    case TOK_GE: *out = Ant_Value_FromBool(l >= r); return true;
    case TOK_SHL:
      *out = Ant_Value_FromNumber((double)(js_to_int32(l) << (js_to_uint32(r) & 31)));
      return true;
    case TOK_SHR:
      *out = Ant_Value_FromNumber((double)(js_to_int32(l) >> (js_to_uint32(r) & 31)));
      return true;
    case TOK_ZSHR:
      *out = Ant_Value_FromNumber((double)(js_to_uint32(l) >> (js_to_uint32(r) & 31)));
      return true;
    case TOK_AND:
      *out = Ant_Value_FromNumber((double)(js_to_int32(l) & js_to_int32(r)));
      return true;
    case TOK_OR:
      *out = Ant_Value_FromNumber((double)(js_to_int32(l) | js_to_int32(r)));
      return true;
    case TOK_XOR:
      *out = Ant_Value_FromNumber((double)(js_to_int32(l) ^ js_to_int32(r)));
      return true;
    default:
      return false;
  }
}

static bool inspector_safe_loose_eq(ant_t *js, ant_value_t left, ant_value_t right, bool *out) {
  uint8_t lt = vtype(left);
  uint8_t rt = vtype(right);
  bool l_nullish = lt == kTypeNull || lt == kTypeUndefined;
  bool r_nullish = rt == kTypeNull || rt == kTypeUndefined;

  if (lt == rt) *out = strict_eq_values(js, left, right);
  else if (l_nullish || r_nullish) *out = l_nullish && r_nullish;
  else if (is_object_type(left) || is_object_type(right)) return false;
  else {
    bool l_numeric = lt == kTypeBool || lt == kTypeNumber || lt == kTypeString;
    bool r_numeric = rt == kTypeBool || rt == kTypeNumber || rt == kTypeString;
    *out = l_numeric && r_numeric && js_to_number(js, left) == js_to_number(js, right);
  }
  return true;
}

static bool inspector_safe_eval_ast(ant_t *js, sv_ast_t *node, ant_value_t *out) {
  if (!js || !node || !out) return false;

  switch (node->type) {
    case N_NUMBER:
      *out = Ant_Value_FromNumber(node->num);
      return true;
    case N_STRING:
      *out = js_mkstr(js, node->str ? node->str : "", node->len);
      return !is_err(*out);
    case N_BOOL:
      *out = Ant_Value_FromBool(node->num != 0);
      return true;
    case N_NULL:
      *out = Ant_Value_Null;
      return true;
    case N_UNDEF:
      *out = Ant_Value_Undefined;
      return true;
    case N_THIS:
    case N_GLOBAL_THIS:
      *out = js_glob(js);
      return true;
    case N_IDENT:
      if (!node->str) return false;
      return inspector_safe_get_binding(js, node->str, node->len, out);
    case N_UNARY: {
      ant_value_t value = Ant_Value_Undefined;
      if (!inspector_safe_eval_ast(js, node->right, &value)) return false;
      if (is_object_type(value)) return false;
      switch (node->op) {
        case TOK_NOT: *out = Ant_Value_FromBool(!js_truthy(js, value)); return true;
        case TOK_TILDA: *out = Ant_Value_FromNumber((double)(~js_to_int32(js_to_number(js, value)))); return true;
        case TOK_UPLUS: *out = Ant_Value_FromNumber(js_to_number(js, value)); return true;
        case TOK_UMINUS: *out = Ant_Value_FromNumber(-js_to_number(js, value)); return true;
        default: return false;
      }
    }
    case N_TYPEOF: {
      ant_value_t value = Ant_Value_Undefined;
      if (!inspector_safe_eval_ast(js, node->right, &value)) return false;
      const char *type_name = typestr(vtype(value));
      *out = js_mkstr(js, type_name, strlen(type_name));
      return !is_err(*out);
    }
    case N_BINARY: {
      ant_value_t left = Ant_Value_Undefined;
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
          if (vtype(left) != kTypeNull && vtype(left) != kTypeUndefined) {
            *out = left;
            return true;
          }
          return inspector_safe_eval_ast(js, node->right, out);
        default:
          break;
      }

      ant_value_t right = Ant_Value_Undefined;
      if (!inspector_safe_eval_ast(js, node->right, &right)) return false;

      switch (node->op) {
        case TOK_PLUS:
          if (is_object_type(left) || is_object_type(right)) return false;
          if (vtype(left) == kTypeString || vtype(right) == kTypeString) {
            ant_value_t l_str = coerce_to_str_concat(js, left);
            ant_value_t r_str = coerce_to_str_concat(js, right);
            if (is_err(l_str) || is_err(r_str)) return false;
            *out = do_string_op(js, TOK_PLUS, l_str, r_str);
            return !is_err(*out);
          }
          *out = Ant_Value_FromNumber(js_to_number(js, left) + js_to_number(js, right));
          return true;
        case TOK_EQ:
        case TOK_NE: {
          bool equal = false;
          if (!inspector_safe_loose_eq(js, left, right, &equal)) return false;
          *out = Ant_Value_FromBool(node->op == TOK_EQ ? equal : !equal);
          return true;
        }
        case TOK_SEQ:
          *out = Ant_Value_FromBool(strict_eq_values(js, left, right));
          return true;
        case TOK_SNE:
          *out = Ant_Value_FromBool(!strict_eq_values(js, left, right));
          return true;
        case TOK_IN:
        case TOK_INSTANCEOF:
          return false;
        default:
          return inspector_safe_binary_numeric(js, node->op, left, right, out);
      }
    }
    case N_TERNARY: {
      ant_value_t cond = Ant_Value_Undefined;
      if (!inspector_safe_eval_ast(js, node->cond, &cond)) return false;
      return inspector_safe_eval_ast(js, js_truthy(js, cond) ? node->left : node->right, out);
    }
    case N_SEQUENCE:
      if (!inspector_safe_eval_ast(js, node->left, out)) return false;
      return inspector_safe_eval_ast(js, node->right, out);
    case N_MEMBER:
    case N_OPTIONAL: {
      ant_value_t obj = Ant_Value_Undefined;
      if (!node->left || !node->right || !inspector_safe_eval_ast(js, node->left, &obj))
        return false;
      if (node->type == N_OPTIONAL && (vtype(obj) == kTypeNull || vtype(obj) == kTypeUndefined)) {
        *out = Ant_Value_Undefined;
        return true;
      }
      if (vtype(obj) == kTypeNull || vtype(obj) == kTypeUndefined) return false;

      char key_buf[128];
      const char *key = NULL;
      size_t key_len = 0;
      if (!inspector_safe_key(js, node->right, node->flags & 1, key_buf, sizeof(key_buf), &key, &key_len))
        return false;

      return inspector_safe_get_prop(js, obj, key, key_len, out);
    }
    case N_ARRAY: {
      ant_value_t arr = Ant_Array_Create(js);
      if (is_err(arr)) return false;

      for (int i = 0; i < node->args.count; i++) {
        sv_ast_t *elem = node->args.items[i];
        ant_value_t value = Ant_Value_Undefined;
        if (elem && elem->type == N_SPREAD) return false;
        if (elem && elem->type != N_EMPTY && !inspector_safe_eval_ast(js, elem, &value))
          return false;
        js_arr_push(js, arr, value);
      }

      *out = arr;
      return true;
    }
    case N_OBJECT: {
      ant_value_t obj = Ant_Object_Create(js);
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
        if (!inspector_safe_key(js, prop->left, prop->flags & FN_COMPUTED, key_buf, sizeof(key_buf), &key, &key_len))
          return false;

        ant_value_t value = Ant_Value_Undefined;
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

  if (expr_len > SIZE_MAX - 3) return false;
  char *wrapped = malloc(expr_len + 3);
  if (!wrapped) return false;
  wrapped[0] = '(';
  memcpy(wrapped + 1, expr, expr_len);
  wrapped[expr_len + 1] = ')';
  wrapped[expr_len + 2] = '\0';

  GC_ROOT_SAVE(exception_mark, js);
  ant_value_t saved_exception = Ant_Exception_Peek(js);

  GC_ROOT_PIN(js, saved_exception);
  Ant_Exception_Clear(js);

  bool ok = false;
  code_arena_mark_t parse_mark = parse_arena_mark(js);
  sv_ast_t *program = sv_parse(js, wrapped, (ant_offset_t)(expr_len + 2), SV_PARSE_SCRIPT, false);
  if (
    program && program->type == N_PROGRAM &&
    program->args.count == 1 &&
    program->args.items[0] &&
    !Ant_Exception_Pending(js)
  ) {
    ok = inspector_safe_eval_ast(js, program->args.items[0], out);
    if (ok && is_err(*out)) ok = false;
  }

  parse_arena_rewind(js, parse_mark);
  free(wrapped);

  Ant_Exception_Set(js, saved_exception);
  GC_ROOT_RESTORE(js, exception_mark);

  return ok;
}
