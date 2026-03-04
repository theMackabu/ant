#ifndef SV_PROPERTY_H
#define SV_PROPERTY_H

#include "silver/engine.h"
#include "utf8.h"
#include <math.h>
#include <stdlib.h>

static inline ant_value_t sv_getprop_fallback_len(
  ant_t *js, ant_value_t obj,
  const char *key, ant_offset_t key_len
) {
  char small[64];
  char *tmp = small;

  if (key_len + 1 > (ant_offset_t)sizeof(small)) {
    tmp = malloc((size_t)key_len + 1);
    if (!tmp) return js_mkerr(js, "out of memory");
  }

  memcpy(tmp, key, (size_t)key_len);
  tmp[key_len] = '\0';
  ant_value_t out = js_getprop_fallback(js, obj, tmp);

  if (tmp != small) free(tmp);
  return out;
}

static inline ant_value_t sv_key_to_propstr(ant_t *js, ant_value_t key) {
  return coerce_to_str(js, key);
}

static inline ant_value_t sv_getprop_by_key(ant_t *js, ant_value_t obj, ant_value_t key) {
  if (vtype(key) == T_SYMBOL) return js_get_sym(js, obj, key);
  ant_value_t key_str = sv_key_to_propstr(js, key);
  if (is_err(key_str) || vtype(key_str) != T_STR) return js_mkundef();

  ant_offset_t klen = 0;
  ant_offset_t koff = vstr(js, key_str, &klen);
  const char *kptr = (const char *)&js->mem[koff];
  return sv_getprop_fallback_len(js, obj, kptr, klen);
}

static inline ant_value_t sv_prop_get_at(
  ant_t *js, ant_value_t obj, const char *str, uint32_t len,
  sv_func_t *func, uint8_t *ip
) {
  uint8_t t = vtype(obj);

  if (t == T_NULL || t == T_UNDEF) {
    if (func && ip) js_set_error_site_from_bc(js, func, (int)(ip - func->code), func->filename);
    return js_mkerr_typed(js, JS_ERR_TYPE,
      "Cannot read properties of %s (reading '%.*s')",
      t == T_NULL ? "null" : "undefined", (int)len, str);
  }

  ant_value_t str_prim = js_mkundef();
  ant_value_t sym_prim = js_mkundef();

  if (t == T_STR) {
    str_prim = obj;
  } else if (t == T_SYMBOL) {
    sym_prim = obj;
  } else if (t == T_OBJ) {
    ant_value_t prim = js_get_slot(js, obj, SLOT_PRIMITIVE);
    if (vtype(prim) == T_STR) str_prim = prim;
    else if (vtype(prim) == T_SYMBOL) sym_prim = prim;
  }

  if (vtype(str_prim) == T_STR && len == 6 && memcmp(str, "length", 6) == 0) {
    ant_offset_t byte_len = 0;
    ant_offset_t str_off = vstr(js, str_prim, &byte_len);
    const char *str_data = (const char *)&js->mem[str_off];
    return tov((double)utf16_strlen(str_data, byte_len));
  }

  if (vtype(sym_prim) == T_SYMBOL && len == 11 &&
      memcmp(str, "description", 11) == 0) {
    const char *desc = js_sym_desc(js, sym_prim);
    if (desc) return js_mkstr(js, desc, strlen(desc));
    return js_mkundef();
  }
  
  return sv_getprop_fallback_len(js, obj, str, (ant_offset_t)len);
}

static inline ant_value_t sv_prop_get(ant_t *js, ant_value_t obj, const char *str, uint32_t len) {
  return sv_prop_get_at(js, obj, str, len, NULL, NULL);
}

static inline bool sv_parse_string_index_key(ant_t *js, ant_value_t key, size_t *out_idx) {
  if (vtype(key) == T_NUM) {
    double d = tod(key);
    if (!isfinite(d) || d < 0.0) return false;
    double di = floor(d);
    if (di != d || di > (double)SIZE_MAX) return false;
    *out_idx = (size_t)di;
    return true;
  }

  if (vtype(key) != T_STR) return false;

  ant_offset_t klen = 0;
  ant_offset_t koff = vstr(js, key, &klen);
  const char *k = (const char *)&js->mem[koff];
  if (klen == 0) return false;
  if (klen > 1 && k[0] == '0') return false;

  size_t idx = 0;
  for (ant_offset_t i = 0; i < klen; i++) {
    if (k[i] < '0' || k[i] > '9') return false;
    size_t digit = (size_t)(k[i] - '0');
    if (idx > (SIZE_MAX - digit) / 10) return false;
    idx = idx * 10 + digit;
  }

  *out_idx = idx;
  return true;
}

static inline bool sv_try_string_index_get(ant_t *js, ant_value_t obj, ant_value_t key, ant_value_t *out) {
  ant_value_t str = obj;
  if (vtype(obj) == T_OBJ) {
    ant_value_t prim = js_get_slot(js, obj, SLOT_PRIMITIVE);
    if (vtype(prim) == T_STR) str = prim;
  }
  if (vtype(str) != T_STR) return false;

  size_t idx = 0;
  if (!sv_parse_string_index_key(js, key, &idx)) return false;

  ant_offset_t byte_len = 0;
  ant_offset_t str_off = vstr(js, str, &byte_len);
  const char *str_data = (const char *)&js->mem[str_off];
  size_t char_bytes = 0;
  int byte_offset = utf16_index_to_byte_offset(str_data, byte_len, idx, &char_bytes);
  if (byte_offset < 0 || char_bytes == 0) {
    *out = js_mkundef();
    return true;
  }

  *out = js_mkstr(js, str_data + byte_offset, char_bytes);
  return true;
}

static inline ant_value_t sv_op_get_field(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  uint32_t idx = sv_get_u32(ip + 1);
  sv_atom_t *a = &func->atoms[idx];
  ant_value_t obj = vm->stack[--vm->sp];
  ant_value_t res = sv_prop_get_at(js, obj, a->str, a->len, func, ip);
  if (is_err(res)) return res;
  vm->stack[vm->sp++] = res;
  return js_mkundef();
}

static inline ant_value_t sv_op_get_field2(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  uint32_t idx = sv_get_u32(ip + 1);
  sv_atom_t *a = &func->atoms[idx];
  ant_value_t obj = vm->stack[vm->sp - 1];
  ant_value_t res = sv_prop_get_at(js, obj, a->str, a->len, func, ip);
  if (is_err(res)) return res;
  vm->stack[vm->sp++] = res;
  return js_mkundef();
}

static inline ant_value_t sv_op_put_field(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  uint32_t idx = sv_get_u32(ip + 1);
  sv_atom_t *a = &func->atoms[idx];
  ant_value_t val = vm->stack[--vm->sp];
  ant_value_t obj = vm->stack[--vm->sp];
  ant_value_t key = js_mkstr(js, a->str, a->len);
  return js_setprop(js, obj, key, val);
}

static inline ant_value_t sv_op_get_elem(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  ant_value_t key = vm->stack[--vm->sp];
  ant_value_t obj = vm->stack[--vm->sp];
  uint8_t ot = vtype(obj);

  if (ot == T_NULL || ot == T_UNDEF) {
    if (func && ip) js_set_error_site_from_bc(js, func, (int)(ip - func->code), func->filename);
    return js_mkerr_typed(js, JS_ERR_TYPE,
      "Cannot read properties of %s", ot == T_NULL ? "null" : "undefined");
  }

  if (vtype(obj) == T_ARR && vtype(key) == T_NUM) {
    double d = tod(key);
    if (d >= 0 && d == (uint32_t)d) {
      vm->stack[vm->sp++] = js_arr_get(js, obj, (uint32_t)d);
      return js_mkundef();
    }
  }

  ant_value_t str_elem = js_mkundef();
  if (sv_try_string_index_get(js, obj, key, &str_elem)) {
    vm->stack[vm->sp++] = str_elem;
    return js_mkundef();
  }

  ant_value_t res = sv_getprop_by_key(js, obj, key);
  if (is_err(res)) return res;
  vm->stack[vm->sp++] = res;
  return js_mkundef();
}

static inline ant_value_t sv_op_get_elem2(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  ant_value_t key = vm->stack[--vm->sp];
  ant_value_t obj = vm->stack[vm->sp - 1];

  uint8_t ot = vtype(obj);
  if (ot == T_NULL || ot == T_UNDEF) {
    if (func && ip) js_set_error_site_from_bc(js, func, (int)(ip - func->code), func->filename);
    return js_mkerr_typed(js, JS_ERR_TYPE,
      "Cannot read properties of %s", ot == T_NULL ? "null" : "undefined");
  }

  if (vtype(obj) == T_ARR && vtype(key) == T_NUM) {
    double d = tod(key);
    if (d >= 0 && d == (uint32_t)d) {
      vm->stack[vm->sp++] = js_arr_get(js, obj, (uint32_t)d);
      return js_mkundef();
    }
  }

  ant_value_t str_elem = js_mkundef();
  if (sv_try_string_index_get(js, obj, key, &str_elem)) {
    vm->stack[vm->sp++] = str_elem;
    return js_mkundef();
  }

  ant_value_t res = sv_getprop_by_key(js, obj, key);
  if (is_err(res)) return res;
  vm->stack[vm->sp++] = res;
  return js_mkundef();
}

static inline ant_value_t sv_op_put_elem(sv_vm_t *vm, ant_t *js) {
  ant_value_t val = vm->stack[--vm->sp];
  ant_value_t key = vm->stack[--vm->sp];
  ant_value_t obj = vm->stack[--vm->sp];
  if (vtype(key) == T_SYMBOL) return js_setprop(js, obj, key, val);
  ant_value_t key_jv = sv_key_to_propstr(js, key);
  return js_setprop(js, obj, key_jv, val);
}

static inline void sv_op_define_field(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  uint32_t idx = sv_get_u32(ip + 1);
  sv_atom_t *a = &func->atoms[idx];
  ant_value_t val = vm->stack[--vm->sp];
  ant_value_t obj = vm->stack[vm->sp - 1];
  js_define_own_prop(js, obj, a->str, a->len, val);
}

static inline ant_value_t sv_op_get_length(sv_vm_t *vm, ant_t *js) {
  ant_value_t obj = vm->stack[--vm->sp];

  if (vtype(obj) == T_ARR) {
    vm->stack[vm->sp++] = tov((double)(uint32_t)js_arr_len(js, obj));
    return js_mkundef();
  }
  
  if (vtype(obj) == T_STR) {
    ant_offset_t byte_len = 0;
    ant_offset_t off = vstr(js, obj, &byte_len);
    const char *str_data = (const char *)&js->mem[off];
    vm->stack[vm->sp++] = tov((double)(uint32_t)utf16_strlen(str_data, byte_len));
    return js_mkundef();
  }

  ant_value_t res = js_getprop_fallback(js, obj, "length");
  if (is_err(res)) return res;
  
  vm->stack[vm->sp++] = res;
  return js_mkundef();
}

#endif
