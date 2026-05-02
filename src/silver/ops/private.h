#ifndef SV_PRIVATE_H
#define SV_PRIVATE_H

#include "silver/engine.h"
#include <stdio.h>

enum {
  SV_PRIVATE_FIELD = 0,
  SV_PRIVATE_METHOD = 1,
  SV_PRIVATE_ACCESSOR = 2,
  SV_PRIVATE_GETTER = 3,
  SV_PRIVATE_SETTER = 4
};

static inline ant_value_t sv_private_entry_get(ant_t *js, ant_value_t entry, ant_offset_t idx) {
  return vtype(entry) == T_ARR 
    ? js_arr_get(js, entry, idx) 
    : js_mkundef();
}

static inline ant_value_t sv_private_entry_set(ant_t *js, ant_value_t entry, ant_offset_t idx, ant_value_t value) {
  char key_buf[8];
  int key_len = snprintf(key_buf, sizeof(key_buf), "%u", (unsigned)idx);
  return js_setprop(js, entry, js_mkstr(js, key_buf, (size_t)key_len), value);
}

static inline ant_value_t sv_private_table(ant_t *js, ant_value_t obj, bool create) {
  if (!is_object_type(obj)) return js_mkundef();
  ant_value_t table = js_get_slot(obj, SLOT_PRIVATE_ELEMENTS);
  
  if (vtype(table) == T_ARR) return table;
  if (!create) return js_mkundef();
  table = js_mkarr(js);
  
  if (is_err(table)) return table;
  js_set_slot_wb(js, obj, SLOT_PRIVATE_ELEMENTS, table);
  
  return table;
}

static inline ant_value_t sv_private_cached_entry(
  ant_t *js, ant_value_t table, ant_value_t token, ant_offset_t len
) {
  if (!is_object_type(token)) return js_mkundef();
  ant_value_t cached = js_get_slot(token, SLOT_DATA);
  if (vtype(cached) != T_NUM) return js_mkundef();

  double idx_num = js_getnum(cached);
  if (idx_num < 0 || idx_num >= (double)len) return js_mkundef();

  ant_offset_t idx = (ant_offset_t)idx_num;
  ant_value_t entry = js_arr_get(js, table, idx);
  if (vtype(entry) == T_ARR && sv_private_entry_get(js, entry, 0) == token)
    return entry;
  return js_mkundef();
}

static inline void sv_private_cache_entry(ant_t *js, ant_value_t token, ant_offset_t idx) {
  if (is_object_type(token))
    js_set_slot(token, SLOT_DATA, js_mknum((double)idx));
}

static inline ant_value_t sv_private_find_entry(ant_t *js, ant_value_t obj, ant_value_t token) {
  ant_value_t table = sv_private_table(js, obj, false);
  if (vtype(table) != T_ARR) return js_mkundef();
  ant_offset_t len = js_arr_len(js, table);

  ant_value_t cached = sv_private_cached_entry(js, table, token, len);
  if (vtype(cached) != T_UNDEF) return cached;

  for (ant_offset_t i = 0; i < len; i++) {
  ant_value_t entry = js_arr_get(js, table, i);
  if (vtype(entry) == T_ARR && sv_private_entry_get(js, entry, 0) == token) {
    sv_private_cache_entry(js, token, i);
    return entry;
  }}
  
  return js_mkundef();
}

static inline ant_value_t sv_private_make_entry(
  ant_t *js, ant_value_t obj, ant_value_t token,
  int kind, ant_value_t value, ant_value_t getter, ant_value_t setter
) {
  ant_value_t table = sv_private_table(js, obj, true);
  if (is_err(table)) return table;
  
  ant_value_t entry = js_mkarr(js);
  if (is_err(entry)) return entry;
  js_arr_push(js, entry, token);
  js_arr_push(js, entry, js_mknum((double)kind));
  js_arr_push(js, entry, value);
  js_arr_push(js, entry, getter);
  js_arr_push(js, entry, setter);
  
  ant_offset_t idx = js_arr_len(js, table);
  js_arr_push(js, table, entry);
  sv_private_cache_entry(js, token, idx);
  
  return entry;
}

static inline ant_value_t sv_private_missing(ant_t *js) {
  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot access private member on object whose class did not declare it");
}

static inline ant_value_t sv_op_get_private_impl(sv_vm_t *vm, ant_t *js, bool optional) {
  ant_value_t token = vm->stack[--vm->sp];
  ant_value_t obj = vm->stack[--vm->sp];

  if (!is_object_type(obj)) {
    if (optional && (vtype(obj) == T_UNDEF || vtype(obj) == T_NULL)) {
      vm->stack[vm->sp++] = js_mkundef();
      return js_mkundef();
    }
    return sv_private_missing(js);
  }

  ant_value_t entry = sv_private_find_entry(js, obj, token);
  if (vtype(entry) == T_UNDEF) return sv_private_missing(js);

  ant_value_t kind_val = sv_private_entry_get(js, entry, 1);
  int kind = vtype(kind_val) == T_NUM ? (int)js_getnum(kind_val) : SV_PRIVATE_FIELD;
  if (kind == SV_PRIVATE_ACCESSOR) {
    ant_value_t getter = sv_private_entry_get(js, entry, 3);
    if (vtype(getter) == T_UNDEF)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Private accessor has no getter");
    ant_value_t result = sv_vm_call_explicit_this(vm, js, getter, obj, NULL, 0);
    if (is_err(result)) return result;
    vm->stack[vm->sp++] = result;
    return js_mkundef();
  }

  vm->stack[vm->sp++] = sv_private_entry_get(js, entry, 2);
  return js_mkundef();
}

static inline ant_value_t sv_op_get_private(sv_vm_t *vm, ant_t *js) {
  return sv_op_get_private_impl(vm, js, false);
}

static inline ant_value_t sv_op_get_private_opt(sv_vm_t *vm, ant_t *js) {
  return sv_op_get_private_impl(vm, js, true);
}

static inline ant_value_t sv_op_put_private(sv_vm_t *vm, ant_t *js) {
  ant_value_t token = vm->stack[--vm->sp];
  ant_value_t val = vm->stack[--vm->sp];
  ant_value_t obj = vm->stack[--vm->sp];

  if (!is_object_type(obj)) return sv_private_missing(js);
  ant_value_t entry = sv_private_find_entry(js, obj, token);
  if (vtype(entry) == T_UNDEF) return sv_private_missing(js);

  ant_value_t kind_val = sv_private_entry_get(js, entry, 1);
  int kind = vtype(kind_val) == T_NUM ? (int)js_getnum(kind_val) : SV_PRIVATE_FIELD;
  if (kind == SV_PRIVATE_FIELD) {
    ant_value_t set = sv_private_entry_set(js, entry, 2, val);
    if (is_err(set)) return set;
    vm->stack[vm->sp++] = val;
    return js_mkundef();
  }

  if (kind == SV_PRIVATE_ACCESSOR) {
    ant_value_t setter = sv_private_entry_get(js, entry, 4);
    if (vtype(setter) == T_UNDEF)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Private accessor has no setter");
    ant_value_t args[1] = { val };
    ant_value_t result = sv_vm_call_explicit_this(vm, js, setter, obj, args, 1);
    if (is_err(result)) return result;
    vm->stack[vm->sp++] = val;
    return js_mkundef();
  }

  return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot write to private method");
}

static inline ant_value_t sv_op_def_private(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  uint8_t def_kind = sv_get_u8(ip + 1);
  ant_value_t val = vm->stack[--vm->sp];
  ant_value_t token = vm->stack[--vm->sp];
  ant_value_t obj = vm->stack[vm->sp - 1];

  if (!is_object_type(obj)) return sv_private_missing(js);
  ant_value_t existing = sv_private_find_entry(js, obj, token);

  if (def_kind == SV_PRIVATE_GETTER || def_kind == SV_PRIVATE_SETTER) {
    ant_value_t entry = existing;
    if (vtype(entry) == T_UNDEF) {
      entry = sv_private_make_entry(
        js, obj, token, SV_PRIVATE_ACCESSOR,
        js_mkundef(),
        def_kind == SV_PRIVATE_GETTER ? val : js_mkundef(),
        def_kind == SV_PRIVATE_SETTER ? val : js_mkundef());
      return is_err(entry) ? entry : js_mkundef();
    }

    ant_value_t kind_val = sv_private_entry_get(js, entry, 1);
    int kind = vtype(kind_val) == T_NUM ? (int)js_getnum(kind_val) : SV_PRIVATE_FIELD;
    if (kind != SV_PRIVATE_ACCESSOR)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot redefine private member");

    ant_offset_t slot = def_kind == SV_PRIVATE_GETTER ? 3 : 4;
    if (vtype(sv_private_entry_get(js, entry, slot)) != T_UNDEF)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot redefine private accessor");
    return sv_private_entry_set(js, entry, slot, val);
  }

  if (vtype(existing) != T_UNDEF)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot initialize private member twice");

  ant_value_t entry = sv_private_make_entry(
    js, obj, token,
    def_kind == SV_PRIVATE_METHOD ? SV_PRIVATE_METHOD : SV_PRIVATE_FIELD,
    val, js_mkundef(), js_mkundef());
  return is_err(entry) ? entry : js_mkundef();
}

static inline ant_value_t sv_op_has_private(sv_vm_t *vm, ant_t *js) {
  ant_value_t token = vm->stack[--vm->sp];
  ant_value_t obj = vm->stack[--vm->sp];
  if (!is_object_type(obj))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Right operand of private brand check must be an object");
  vm->stack[vm->sp++] = js_bool(vtype(sv_private_find_entry(js, obj, token)) != T_UNDEF);
  return js_mkundef();
}

#endif
