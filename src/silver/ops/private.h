#ifndef SV_PRIVATE_H
#define SV_PRIVATE_H

#include <stdlib.h>
#include "gc.h"
#include "object.h"
#include "silver/engine.h"

enum {
  SV_PRIVATE_FIELD = 0,
  SV_PRIVATE_METHOD = 1,
  SV_PRIVATE_ACCESSOR = 2,
  SV_PRIVATE_GETTER = 3,
  SV_PRIVATE_SETTER = 4
};

static inline uint32_t sv_private_hash_token(ant_value_t token) {
  if (is_object_type(token)) {
    ant_value_t cached = js_get_slot(token, SLOT_DATA);
    if (vtype(cached) == T_NUM) return (uint32_t)js_getnum(cached);
  }

  uint64_t x = token ^ (token >> 33);
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  
  return (uint32_t)x;
}

static inline ant_private_table_t *sv_private_table_for_obj(ant_value_t obj, bool create) {
  if (!is_object_type(obj)) return NULL;
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  if (!ptr) return NULL;
  ant_object_sidecar_t *sidecar = create ? ant_object_ensure_sidecar(ptr) : ant_object_sidecar(ptr);
  return sidecar ? &sidecar->private_table : NULL;
}

static inline uint32_t sv_private_find_slot(
  ant_private_table_t *table, ant_value_t token, uint32_t hash, bool *found
) {
  uint32_t mask = table->cap - 1;
  uint32_t idx = hash & mask;
  
  for (;;) {
    ant_private_entry_t *entry = &table->entries[idx];
    if (!entry->occupied) {
      *found = false;
      return idx;
    }
    
    if (entry->hash == hash && entry->token == token) {
      *found = true;
      return idx;
    }
    
    idx = (idx + 1) & mask;
  }
}

static inline bool sv_private_table_grow(ant_private_table_t *table) {
  uint32_t old_cap = table->cap;
  uint32_t new_cap = old_cap ? old_cap * 2 : 8;
  
  ant_private_entry_t *new_entries = 
    (ant_private_entry_t *)calloc(new_cap, sizeof(*new_entries));
  if (!new_entries) return false;

  ant_private_entry_t *old_entries = table->entries;
  table->entries = new_entries;
  table->cap = new_cap;
  table->count = 0;

  for (uint32_t i = 0; i < old_cap; i++) {
    ant_private_entry_t entry = old_entries[i];
    if (!entry.occupied) continue;
    
    bool found = false;
    uint32_t slot = sv_private_find_slot(
      table, entry.token, 
      entry.hash, &found
    );
    
    table->entries[slot] = entry;
    table->count++;
  }

  free(old_entries);
  return true;
}

static inline ant_private_entry_t *sv_private_find_entry(ant_value_t obj, ant_value_t token) {
  ant_private_table_t *table = sv_private_table_for_obj(obj, false);
  if (!table || !table->entries || table->cap == 0) return NULL;
  
  bool found = false;
  uint32_t hash = sv_private_hash_token(token);
  uint32_t slot = sv_private_find_slot(table, token, hash, &found);
  
  return found ? &table->entries[slot] : NULL;
}

static inline ant_value_t sv_private_entry_get(ant_private_entry_t *entry, ant_offset_t idx) {
  if (!entry) return js_mkundef();
  
  switch (idx) {
    case 0: return entry->token;
    case 1: return js_mknum((double)entry->kind);
    case 2: return entry->value;
    case 3: return entry->getter;
    case 4: return entry->setter;
    default: return js_mkundef();
  }
}

static inline ant_value_t sv_private_entry_set(
  ant_t *js, ant_value_t obj, ant_private_entry_t *entry, ant_offset_t idx, ant_value_t value
) {
  if (!entry) return js_mkundef();
  
  switch (idx) {
    case 2: entry->value = value; break;
    case 3: entry->getter = value; break;
    case 4: entry->setter = value; break;
    default: return js_mkundef();
  }

  ant_object_t *ptr = is_object_type(obj) ? js_obj_ptr(js_as_obj(obj)) : NULL;
  if (ptr) gc_write_barrier(js, ptr, value);
  
  return js_mkundef();
}

static inline ant_private_entry_t *sv_private_make_entry(
  ant_t *js, ant_value_t obj, ant_value_t token,
  int kind, ant_value_t value, ant_value_t getter, ant_value_t setter
) {
  ant_private_table_t *table = sv_private_table_for_obj(obj, true);
  if (!table) return NULL;
  
  if (table->cap == 0 || ((table->count + 1) * 4 >= table->cap * 3)) {
    if (!sv_private_table_grow(table)) return NULL;
  }

  bool found = false;
  uint32_t hash = sv_private_hash_token(token);
  uint32_t slot = sv_private_find_slot(table, token, hash, &found);
  ant_private_entry_t *entry = &table->entries[slot];
  
  if (!found) {
    *entry = (ant_private_entry_t){
      .token = token,
      .value = value,
      .getter = getter,
      .setter = setter,
      .hash = hash,
      .kind = (uint8_t)kind,
      .occupied = 1
    };
    table->count++;
  }

  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  if (ptr) {
    gc_write_barrier(js, ptr, token);
    gc_write_barrier(js, ptr, value);
    gc_write_barrier(js, ptr, getter);
    gc_write_barrier(js, ptr, setter);
  }

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

  ant_private_entry_t *entry = sv_private_find_entry(obj, token);
  if (!entry) return sv_private_missing(js);

  ant_value_t kind_val = sv_private_entry_get(entry, 1);
  int kind = vtype(kind_val) == T_NUM 
    ? (int)js_getnum(kind_val) 
    : SV_PRIVATE_FIELD;
  
  if (kind == SV_PRIVATE_ACCESSOR) {
    ant_value_t getter = sv_private_entry_get(entry, 3);
    if (vtype(getter) == T_UNDEF)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Private accessor has no getter");
    ant_value_t result = sv_vm_call_explicit_this(vm, js, getter, obj, NULL, 0);
    if (is_err(result)) return result;
    vm->stack[vm->sp++] = result;
    return js_mkundef();
  }

  vm->stack[vm->sp++] = sv_private_entry_get(entry, 2);
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
  ant_private_entry_t *entry = sv_private_find_entry(obj, token);
  if (!entry) return sv_private_missing(js);

  ant_value_t kind_val = sv_private_entry_get(entry, 1);
  int kind = vtype(kind_val) == T_NUM ? (int)js_getnum(kind_val) : SV_PRIVATE_FIELD;
  if (kind == SV_PRIVATE_FIELD) {
    ant_value_t set = sv_private_entry_set(js, obj, entry, 2, val);
    if (is_err(set)) return set;
    vm->stack[vm->sp++] = val;
    return js_mkundef();
  }

  if (kind == SV_PRIVATE_ACCESSOR) {
    ant_value_t setter = sv_private_entry_get(entry, 4);
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
  ant_private_entry_t *existing = sv_private_find_entry(obj, token);

  if (def_kind == SV_PRIVATE_GETTER || def_kind == SV_PRIVATE_SETTER) {
    ant_private_entry_t *entry = existing;
    if (!entry) {
      entry = sv_private_make_entry(
        js, obj, token, SV_PRIVATE_ACCESSOR,
        js_mkundef(),
        def_kind == SV_PRIVATE_GETTER ? val : js_mkundef(),
        def_kind == SV_PRIVATE_SETTER ? val : js_mkundef());
      return entry ? js_mkundef() : js_mkerr(js, "oom");
    }
    
    ant_value_t kind_val = sv_private_entry_get(entry, 1);
    int kind = vtype(kind_val) == T_NUM ? (int)js_getnum(kind_val) : SV_PRIVATE_FIELD;
    if (kind != SV_PRIVATE_ACCESSOR)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot redefine private member");
      
    ant_offset_t slot = def_kind == SV_PRIVATE_GETTER ? 3 : 4;
    if (vtype(sv_private_entry_get(entry, slot)) != T_UNDEF)
      return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot redefine private accessor");
    return sv_private_entry_set(js, obj, entry, slot, val);
  }

  if (existing)
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot initialize private member twice");

  ant_private_entry_t *entry = sv_private_make_entry(
    js, obj, token,
    def_kind == SV_PRIVATE_METHOD ? SV_PRIVATE_METHOD : SV_PRIVATE_FIELD,
    val, js_mkundef(), js_mkundef());
    
  return entry ? js_mkundef() : js_mkerr(js, "oom");
}

static inline ant_value_t sv_op_has_private(sv_vm_t *vm, ant_t *js) {
  ant_value_t token = vm->stack[--vm->sp];
  ant_value_t obj = vm->stack[--vm->sp];
  
  if (!is_object_type(obj))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Right operand of private brand check must be an object");
  vm->stack[vm->sp++] = js_bool(sv_private_find_entry(obj, token) != NULL);
  
  return js_mkundef();
}

#endif
