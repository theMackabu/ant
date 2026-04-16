#ifndef SV_PROPERTY_H
#define SV_PROPERTY_H

#include "silver/engine.h"
#include "shapes.h"
#include "gc.h"
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

static inline ant_value_t sv_key_to_property_key(ant_t *js, ant_value_t key) {
  if (vtype(key) == T_SYMBOL) return key;

  ant_value_t prim = is_object_type(key) ? js_to_primitive(js, key, 1) : key;
  if (is_err(prim)) return prim;
  if (vtype(prim) == T_SYMBOL) return prim;

  return js_tostring_val(js, prim);
}

static inline ant_value_t sv_key_to_propstr(ant_t *js, ant_value_t key) {
  return coerce_to_str(js, key);
}

static inline ant_value_t sv_mk_nullish_read_error_by_key(
  ant_t *js, ant_value_t obj, ant_value_t key
) {
  uint8_t ot = vtype(obj);
  ant_value_t key_str = sv_key_to_propstr(js, key);

  if (!is_err(key_str) && vtype(key_str) == T_STR) {
    ant_offset_t klen = 0;
    ant_offset_t koff = vstr(js, key_str, &klen);
    const char *kptr = (const char *)(uintptr_t)(koff);
    
    return js_mkerr_typed(js, JS_ERR_TYPE,
      "Cannot read properties of %s (reading '%.*s')",
      ot == T_NULL ? "null" : "undefined", (int)klen, kptr
    );
  }

  return js_mkerr_typed(js, JS_ERR_TYPE,
    "Cannot read properties of %s",
    ot == T_NULL ? "null" : "undefined"
  );
}

static inline ant_object_t *sv_array_obj_ptr(ant_value_t obj) {
  if (!is_object_type(obj)) return NULL;
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  return (ptr && ptr->type_tag == T_ARR) ? ptr : NULL;
}

static inline bool sv_try_get_shape_data_prop(
  ant_t *js,
  ant_object_t *ptr,
  const char *interned,
  ant_value_t *out,
  bool *should_fallback
) {
  if (!ptr) {
    *should_fallback = true;
    return false;
  }
  if (ptr->is_exotic) {
    *should_fallback = true;
    return false;
  }
  if (!ptr->shape) return false;

  int32_t slot = ant_shape_lookup_interned(ptr->shape, interned);
  if (slot < 0) return false;

  uint32_t idx = (uint32_t)slot;
  const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, idx);
  if (!prop) {
    *out = js_mkundef();
    return true;
  }
  if (prop->has_getter || prop->has_setter) {
    *should_fallback = true;
    return false;
  }

  *out = (idx < ptr->prop_count) ? ant_object_prop_get_unchecked(ptr, idx) : js_mkundef();
  
  return true;
}

static inline bool sv_same_obj_identity(ant_value_t a, ant_value_t b) {
  if (!is_object_type(a) || !is_object_type(b)) return false;
  return vdata(js_as_obj(a)) == vdata(js_as_obj(b));
}

static inline ant_value_t sv_obj_proto_or_null(ant_value_t v) {
  if (!is_object_type(v)) return js_mknull();
  ant_object_t *ptr = js_obj_ptr(js_as_obj(v));
  if (!ptr) return js_mknull();
  return is_object_type(ptr->proto) ? ptr->proto : js_mknull();
}

typedef struct {
  int depth;
  bool overflow;
  ant_value_t fast;
} sv_proto_guard_t;

static inline void sv_proto_guard_init(sv_proto_guard_t *g) {
  g->depth = 0;
  g->overflow = false;
  g->fast = js_mknull();
}

static inline bool sv_proto_guard_hit_cycle(sv_proto_guard_t *g, ant_value_t cur) {
  if (!g->overflow) {
    if (++g->depth < MAX_PROTO_CHAIN_DEPTH) return false;
    g->overflow = true;
    g->fast = cur;
  }

  g->fast = sv_obj_proto_or_null(g->fast);
  g->fast = sv_obj_proto_or_null(g->fast);
  return sv_same_obj_identity(cur, g->fast);
}

static inline sv_ic_entry_t *sv_ic_slot_for_ip(sv_func_t *func, uint8_t *ip) {
  if (!func || !func->ic_slots || !ip) return NULL;
  uint16_t ic_idx = sv_get_u16(ip + 5);
  if (ic_idx == UINT16_MAX || ic_idx >= func->ic_count) return NULL;
  return &func->ic_slots[ic_idx];
}

static inline bool sv_ic_try_get_hit(
  sv_ic_entry_t *ic,
  ant_object_t *receiver,
  sv_atom_t *a,
  ant_value_t *out
) {
  if (!ic || !receiver) return false;
  if (ic->epoch != ant_ic_epoch_counter) return false;
  if (ic->cached_shape != receiver->shape) return false;

  ant_object_t *source;
  ant_shape_t *prop_shape;
  
  if (ic->cached_is_own) {
    source = receiver;
    prop_shape = receiver->shape;
  } else {
    ant_object_t *holder = ic->cached_holder;
    if (!holder || holder->is_exotic || !holder->shape) return false;
    source = holder;
    prop_shape = holder->shape;
  }
  if (ic->cached_index >= source->prop_count) return false;

  const ant_shape_prop_t *prop = ant_shape_prop_at(prop_shape, ic->cached_index);
  if (!prop) return false;
  if (prop->type != ANT_SHAPE_KEY_STRING || prop->key.interned != a->str) return false;
  if (prop->has_getter || prop->has_setter) return false;

  *out = ant_object_prop_get_unchecked(source, ic->cached_index);
  return true;
}

static inline bool sv_ic_probe_get_chain(
  ant_value_t obj,
  const char *interned,
  ant_object_t **out_holder,
  uint32_t *out_index,
  ant_value_t *out_value
) {
  ant_value_t cur = obj;
  sv_proto_guard_t guard;
  sv_proto_guard_init(&guard);
  while (is_object_type(cur)) {
    ant_object_t *ptr = js_obj_ptr(js_as_obj(cur));
    if (!ptr || ptr->is_exotic) return false;
    if (!ptr->shape) {
      ant_value_t next = ptr->proto;
      if (!is_object_type(next)) break;
      cur = next;
      if (sv_proto_guard_hit_cycle(&guard, cur)) break;
      continue;
    }

    int32_t slot = ant_shape_lookup_interned(ptr->shape, interned);
    if (slot < 0) {
      ant_value_t next = ptr->proto;
      if (!is_object_type(next)) break;
      cur = next;
      if (sv_proto_guard_hit_cycle(&guard, cur)) break;
      continue;
    }

    uint32_t idx = (uint32_t)slot;
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, idx);
    if (!prop) return false;
    if (prop->has_getter || prop->has_setter) return false;

    *out_holder = ptr;
    *out_index = idx;
    *out_value = (idx < ptr->prop_count) ? ant_object_prop_get_unchecked(ptr, idx) : js_mkundef();
    return true;
  }

  return false;
}

static inline void sv_gf_ic_note_success(sv_ic_entry_t *ic) {
  if (!ic) return;
  uintptr_t aux = ic->cached_aux;
  uint8_t warmup = sv_gf_ic_warmup(aux);
  if (warmup < 0xFFu) warmup++;
  bool active = sv_gf_ic_active(aux) || warmup >= SV_GF_IC_WARMUP_ENABLE;
  ic->cached_aux = sv_gf_ic_pack_aux(warmup, 0u, active);
}

static inline void sv_gf_ic_note_miss(sv_ic_entry_t *ic) {
  if (!ic) return;
  uintptr_t aux = ic->cached_aux;
  uint8_t warmup = sv_gf_ic_warmup(aux);
  uint8_t miss = sv_gf_ic_miss_streak(aux);
  bool active = sv_gf_ic_active(aux);

  if (miss < 0xFFu) miss++;
  if (miss >= SV_GF_IC_MISS_DISABLE) {
    warmup = 0;
    miss = 0;
    active = false;
  }

  ic->cached_aux = sv_gf_ic_pack_aux(warmup, miss, active);
}

static inline ant_value_t sv_getprop_by_key(ant_t *js, ant_value_t obj, ant_value_t key) {
  ant_value_t prop_key = sv_key_to_property_key(js, key);
  if (is_err(prop_key)) return prop_key;
  if (vtype(prop_key) == T_SYMBOL) return js_get_sym(js, obj, prop_key);

  ant_value_t key_str = prop_key;
  if (is_err(key_str) || vtype(key_str) != T_STR) return js_mkundef();

  ant_offset_t klen = 0;
  ant_offset_t koff = vstr(js, key_str, &klen);
  
  const char *kptr = (const char *)(uintptr_t)(koff);
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

  if (t == T_STR) str_prim = obj;
  else if (t == T_SYMBOL) sym_prim = obj;
  else if (t == T_OBJ) {
    ant_value_t prim = js_get_slot(obj, SLOT_PRIMITIVE);
    if (vtype(prim) == T_STR) str_prim = prim;
    else if (vtype(prim) == T_SYMBOL) sym_prim = prim;
  }

  if (vtype(str_prim) == T_STR && is_length_key(str, len)) {
    ant_offset_t byte_len = 0;
    ant_offset_t str_off = vstr(js, str_prim, &byte_len);
    const char *str_data = (const char *)(uintptr_t)(str_off);
    return tov((double)utf16_strlen(str_data, byte_len));
  }

  if (is_length_key(str, len)) {
    ant_object_t *arr_ptr = sv_array_obj_ptr(obj);
    if (arr_ptr) return tov((double)js_arr_len(js, js_as_obj(obj)));
  }

  if (vtype(sym_prim) == T_SYMBOL && len == 11 &&
      memcmp(str, "description", 11) == 0) {
    const char *desc = js_sym_desc(js, sym_prim);
    if (desc) return js_mkstr(js, desc, strlen(desc));
    return js_mkundef();
  }

  if (t == T_OBJ || t == T_ARR || t == T_FUNC || t == T_PROMISE) {
    ant_value_t cur = obj;
    sv_proto_guard_t guard;
    sv_proto_guard_init(&guard);
    while (is_object_type(cur)) {
      ant_object_t *ptr = js_obj_ptr(js_as_obj(cur));
      bool should_fallback = false;
      ant_value_t fast_out = js_mkundef();
      if (sv_try_get_shape_data_prop(js, ptr, str, &fast_out, &should_fallback))
        return fast_out;
      if (should_fallback) break;
      cur = ptr->proto;
      if (sv_proto_guard_hit_cycle(&guard, cur)) break;
    }
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
  const char *k = (const char *)(uintptr_t)(koff);
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
    ant_value_t prim = js_get_slot(obj, SLOT_PRIMITIVE);
    if (vtype(prim) == T_STR) str = prim;
  }
  if (vtype(str) != T_STR) return false;

  size_t idx = 0;
  if (!sv_parse_string_index_key(js, key, &idx)) return false;

  ant_offset_t byte_len = 0;
  ant_offset_t str_off = vstr(js, str, &byte_len);
  const char *str_data = (const char *)(uintptr_t)(str_off);
  
  uint32_t code_unit = utf16_code_unit_at(str_data, byte_len, idx);
  if (code_unit == 0xFFFFFFFF) {
    *out = js_mkundef();
    return true;
  }

  char buf[4];
  size_t out_len = 0;
  if (code_unit >= 0xD800 && code_unit <= 0xDFFF) {
    buf[0] = (char)(0xE0 | (code_unit >> 12));
    buf[1] = (char)(0x80 | ((code_unit >> 6) & 0x3F));
    buf[2] = (char)(0x80 | (code_unit & 0x3F));
    out_len = 3;
  } else out_len = (size_t)utf8_encode(code_unit, buf);
  *out = js_mkstr(js, buf, out_len);
  
  return true;
}

static inline ant_value_t sv_prop_get_field_ic(
  ant_t *js,
  ant_value_t obj,
  sv_atom_t *a,
  sv_func_t *func,
  uint8_t *ip
) {
  ant_object_t *ptr = is_object_type(obj) ? js_obj_ptr(js_as_obj(obj)) : NULL;
  sv_ic_entry_t *ic = sv_ic_slot_for_ip(func, ip);
  bool track_ic = ic && !is_length_key(a->str, a->len);

  ant_value_t hit = js_mkundef();
  if (ic && ptr && !ptr->is_exotic && track_ic &&
      sv_ic_try_get_hit(ic, ptr, a, &hit)) {
    sv_gf_ic_note_success(ic);
    return hit;
  }

  if (ic && ptr && !ptr->is_exotic && track_ic) {
    ant_object_t *holder = NULL;
    uint32_t prop_idx = 0;
    ant_value_t out = js_mkundef();
    if (sv_ic_probe_get_chain(obj, a->str, &holder, &prop_idx, &out)) {
      ic->cached_shape = ptr->shape;
      ic->cached_holder = holder;
      ic->cached_index = prop_idx;
      ic->cached_is_own = (holder == ptr);
      ic->epoch = ant_ic_epoch_counter;
      sv_gf_ic_note_success(ic);
      return out;
    }
  }

  if (track_ic) sv_gf_ic_note_miss(ic);
  return sv_prop_get_at(js, obj, a->str, a->len, func, ip);
}

static inline ant_value_t sv_op_get_field(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  uint32_t idx = sv_get_u32(ip + 1);
  sv_atom_t *a = &func->atoms[idx];
  ant_value_t obj = vm->stack[--vm->sp];
  ant_value_t res = sv_prop_get_field_ic(js, obj, a, func, ip);
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
  ant_value_t res = sv_prop_get_field_ic(js, obj, a, func, ip);
  if (is_err(res)) return res;
  vm->stack[vm->sp++] = res;
  return js_mkundef();
}

static inline bool sv_try_put_field_fast(
  ant_t *js,
  ant_value_t obj,
  sv_atom_t *a,
  ant_value_t val,
  uint32_t *out_index
) {
  if (!is_object_type(obj)) return false;

  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  if (!ptr || ptr->is_exotic || !ptr->shape) return false;
  if (ptr->type_tag == T_ARR && is_length_key(a->str, a->len)) return false;

  int32_t slot = ant_shape_lookup_interned(ptr->shape, a->str);
  if (slot < 0) return false;

  uint32_t prop_idx = (uint32_t)slot;
  if (prop_idx >= ptr->prop_count) return false;

  const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, prop_idx);
  if (!prop) return false;
  if (prop->has_getter || prop->has_setter) return false;
  if ((prop->attrs & ANT_PROP_ATTR_WRITABLE) == 0) return false;

  ant_object_prop_set_unchecked(ptr, prop_idx, val);
  gc_write_barrier(js, ptr, val);
  if (out_index) *out_index = prop_idx;
  return true;
}

static inline bool sv_is_proto_atom(const sv_atom_t *a) {
  return a && a->len == STR_PROTO_LEN && memcmp(a->str, STR_PROTO, STR_PROTO_LEN) == 0;
}

static inline void sv_ic_set_add_transition(
  sv_ic_entry_t *ic,
  ant_shape_t *from,
  ant_shape_t *to,
  uint32_t slot,
  uint32_t epoch
) {
  if (!ic) return;

  if (ic->add_from_shape != from) {
    if (ic->add_from_shape) ant_shape_release(ic->add_from_shape);
    ic->add_from_shape = NULL;
    if (from) {
      ant_shape_retain(from);
      ic->add_from_shape = from;
    }
  }

  if (ic->add_to_shape != to) {
    if (ic->add_to_shape) ant_shape_release(ic->add_to_shape);
    ic->add_to_shape = NULL;
    if (to) {
      ant_shape_retain(to);
      ic->add_to_shape = to;
    }
  }

  ic->add_slot = slot;
  ic->add_epoch = epoch;
}

static inline ant_value_t sv_op_put_field(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  uint32_t idx = sv_get_u32(ip + 1);
  sv_atom_t *a = &func->atoms[idx];
  ant_value_t val = vm->stack[--vm->sp];
  ant_value_t obj = vm->stack[--vm->sp];
  ant_object_t *ptr = is_object_type(obj) ? js_obj_ptr(js_as_obj(obj)) : NULL;
  sv_ic_entry_t *ic = sv_ic_slot_for_ip(func, ip);

  if (ic && ptr && !ptr->is_exotic && ptr->shape && ic->epoch == ant_ic_epoch_counter &&
      ic->cached_shape == ptr->shape && ic->cached_holder == ptr &&
      ic->cached_index < ptr->prop_count) {
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, ic->cached_index);
    if (prop &&
        prop->type == ANT_SHAPE_KEY_STRING &&
        prop->key.interned == a->str &&
        !prop->has_getter &&
        !prop->has_setter &&
        (prop->attrs & ANT_PROP_ATTR_WRITABLE) != 0) {
      ant_object_prop_set_unchecked(ptr, ic->cached_index, val);
      gc_write_barrier(js, ptr, val);
      return val;
    }
  }

  if (ic && ptr && !ptr->is_exotic && ptr->shape &&
      !ptr->frozen && !ptr->sealed && ptr->extensible &&
      ptr->type_tag != T_ARR &&
      !sv_is_proto_atom(a) &&
      ic->add_epoch == ant_ic_epoch_counter &&
      ic->add_from_shape == ptr->shape &&
      ic->add_to_shape) {
    if (ant_shape_lookup_interned(ptr->shape, a->str) < 0) {
      ant_shape_t *old_shape = ptr->shape;
      ant_shape_retain(ic->add_to_shape);
      ptr->shape = ic->add_to_shape;
      ant_shape_release(old_shape);
      if (ic->add_slot >= ptr->prop_count &&
          !js_obj_ensure_prop_capacity(ptr, ic->add_slot + 1)) {
        return js_mkerr(js, "oom");
      }
      ant_object_prop_set_unchecked(ptr, ic->add_slot, val);
      gc_write_barrier(js, ptr, val);
      ic->cached_shape = ptr->shape;
      ic->cached_holder = ptr;
      ic->cached_index = ic->add_slot;
      ic->epoch = ant_ic_epoch_counter;
      return val;
    }
  }

  uint32_t fast_idx = 0;
  if (sv_try_put_field_fast(js, obj, a, val, &fast_idx)) {
    if (ic && ptr && ptr->shape) {
      ic->cached_shape = ptr->shape;
      ic->cached_holder = ptr;
      ic->cached_index = fast_idx;
      ic->epoch = ant_ic_epoch_counter;
    }
    return val;
  }

  ant_shape_t *old_shape = NULL;
  if (ptr && ptr->shape) {
    old_shape = ptr->shape;
    ant_shape_retain(old_shape);
  }

  ant_value_t out = setprop_interned(js, obj, a->str, a->len, val);
  if (is_err(out)) {
    if (old_shape) ant_shape_release(old_shape);
    return out;
  }

  if (ic && ptr && !ptr->is_exotic && ptr->shape) {
    int32_t slot = ant_shape_lookup_interned(ptr->shape, a->str);
    if (slot >= 0) {
      uint32_t prop_idx = (uint32_t)slot;
      const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, prop_idx);
      if (prop &&
          prop->type == ANT_SHAPE_KEY_STRING &&
          prop->key.interned == a->str &&
          !prop->has_getter &&
          !prop->has_setter &&
          (prop->attrs & ANT_PROP_ATTR_WRITABLE) != 0 &&
          prop_idx < ptr->prop_count) {
        ic->cached_shape = ptr->shape;
        ic->cached_holder = ptr;
        ic->cached_index = prop_idx;
        ic->epoch = ant_ic_epoch_counter;
        if (old_shape &&
            old_shape != ptr->shape &&
            !sv_is_proto_atom(a) &&
            !ptr->frozen &&
            !ptr->sealed &&
            ptr->extensible &&
            ptr->type_tag != T_ARR &&
            prop->attrs == ANT_PROP_ATTR_DEFAULT) {
          sv_ic_set_add_transition(ic, old_shape, ptr->shape, prop_idx, ant_ic_epoch_counter);
        }
      }
    }
  }

  if (old_shape) ant_shape_release(old_shape);

  return out;
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
    return sv_mk_nullish_read_error_by_key(js, obj, key);
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
    return sv_mk_nullish_read_error_by_key(js, obj, key);
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
  ant_value_t prop_key = sv_key_to_property_key(js, key);
  if (is_err(prop_key)) return prop_key;
  return js_setprop(js, obj, prop_key, val);
}

static inline bool sv_try_define_field_fast(
  ant_t *js,
  ant_value_t obj,
  const char *interned_key,
  ant_value_t val
) {
  if (!interned_key || !is_object_type(obj)) return false;

  ant_value_t as_obj = js_as_obj(obj);
  ant_object_t *ptr = js_obj_ptr(as_obj);
  if (!ptr || ptr->is_exotic || !ptr->shape) return false;
  if (ptr->type_tag == T_ARR) return false;
  if (ptr->frozen || ptr->sealed || !ptr->extensible) return false;

  int32_t slot = ant_shape_lookup_interned(ptr->shape, interned_key);
  if (slot >= 0) {
    uint32_t idx = (uint32_t)slot;
    if (idx >= ptr->prop_count) return false;
    const ant_shape_prop_t *prop = ant_shape_prop_at(ptr->shape, idx);
    if (!prop) return false;
    if (prop->type != ANT_SHAPE_KEY_STRING || prop->key.interned != interned_key) return false;
    if (prop->has_getter || prop->has_setter) return false;
    if (prop->attrs != ANT_PROP_ATTR_DEFAULT) return false;
    ant_object_prop_set_unchecked(ptr, idx, val);
    gc_write_barrier(js, ptr, val);
    return true;
  }

  return !is_err(mkprop_interned(js, as_obj, interned_key, val, 0));
}

static inline void sv_op_define_field(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  uint32_t idx = sv_get_u32(ip + 1);
  sv_atom_t *a = &func->atoms[idx];
  ant_value_t val = vm->stack[--vm->sp];
  ant_value_t obj = vm->stack[vm->sp - 1];
  if (!sv_try_define_field_fast(js, obj, a->str, val))
    js_define_own_prop(js, obj, a->str, a->len, val);
}

static inline ant_value_t sv_op_get_length(sv_vm_t *vm, ant_t *js) {
  ant_value_t obj = vm->stack[--vm->sp];

  if (vtype(obj) == T_ARR) {
    vm->stack[vm->sp++] = tov((double)(uint32_t)js_arr_len(js, obj));
    return js_mkundef();
  }
  
  if (vtype(obj) == T_STR) {
    ant_flat_string_t *flat = ant_str_flat_ptr(obj);
    if (flat) {
      const char *str_data = flat->bytes;
      ant_offset_t byte_len = flat->len;
      vm->stack[vm->sp++] = tov((double)(uint32_t)(
        str_is_ascii(str_data) 
          ? byte_len 
          : utf16_strlen(str_data, byte_len)
      ));
      return js_mkundef();
    }

    ant_offset_t byte_len = 0;
    ant_offset_t off = vstr(js, obj, &byte_len);
    const char *str_data = (const char *)(uintptr_t)(off);
    vm->stack[vm->sp++] = tov((double)(uint32_t)utf16_strlen(str_data, byte_len));
    return js_mkundef();
  }

  ant_value_t res = js_getprop_fallback(js, obj, "length");
  if (is_err(res)) return res;
  
  vm->stack[vm->sp++] = res;
  return js_mkundef();
}

#endif
