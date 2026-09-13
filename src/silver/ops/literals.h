#ifndef SV_LITERALS_H
#define SV_LITERALS_H

#include "ant.h"
#include "modules/regex.h"
#include "silver/engine.h"

static inline void sv_op_const(sv_vm_t *vm, sv_func_t *func, uint8_t *ip) {
  uint32_t idx = sv_get_u32(ip + 1);
  vm->stack[vm->sp++] = func->constants[idx];
}

static inline void sv_op_const_i8(sv_vm_t *vm, uint8_t *ip) {
  int8_t val = sv_get_i8(ip + 1);
  vm->stack[vm->sp++] = tov((double)val);
}

static inline int sv_literal_constant_at(sv_func_t *func, const uint8_t *ip, ant_value_t *value) {
  if (ip >= func->code + func->code_len) return 0;
  int size = sv_op_size[*ip];
  if (!size || ip + size > func->code + func->code_len) return 0;
  
  switch (*ip) {
    case OP_CONST: case OP_CONST8: {
      uint32_t index = *ip == OP_CONST ? sv_get_u32(ip + 1) : ip[1];
      if (index >= (uint32_t)func->const_count) return 0;
      *value = func->constants[index];
      uint8_t type = vtype(*value);
      if (type != kTypeString && type != kTypeNumber && type != kTypeBool &&
          type != kTypeNull && type != kTypeUndefined && type != kTypeBigInt) return 0;
      break;
    }
    
    case OP_CONST_I8: *value = tov((double)(int8_t)ip[1]); break;
    case OP_TRUE: *value = js_true; break;
    case OP_FALSE: *value = js_false; break;
    case OP_NULL: *value = js_mknull(); break;
    case OP_UNDEF: *value = js_mkundef(); break;
    default: return 0;
  }
  
  return size;
}

static inline void sv_op_const8(sv_vm_t *vm, sv_func_t *func, uint8_t *ip) {
  uint8_t idx = sv_get_u8(ip + 1);
  vm->stack[vm->sp++] = func->constants[idx];
}

static inline void sv_op_undef(sv_vm_t *vm) {
  vm->stack[vm->sp++] = mkval(kTypeUndefined, 0);
}

static inline void sv_op_null(sv_vm_t *vm) {
  vm->stack[vm->sp++] = mkval(kTypeNull, 0);
}

static inline void sv_op_true(sv_vm_t *vm) {
  vm->stack[vm->sp++] = js_true;
}

static inline void sv_op_false(sv_vm_t *vm) {
  vm->stack[vm->sp++] = js_false;
}

static inline void sv_op_this(sv_vm_t *vm, sv_frame_t *frame) {
  vm->stack[vm->sp++] = frame->this;
}

static inline void sv_op_global(sv_vm_t *vm, ant_t *js) {
  vm->stack[vm->sp++] = js->global;
}

static inline sv_obj_site_cache_t *sv_obj_site_for_ip(sv_func_t *func, uint8_t *ip) {
  if (!func || !func->code || !ip) return NULL;
  uint32_t off = (uint32_t)(ip - func->code);
  return sv_obj_site_for_offset(func, off);
}

static inline void sv_obj_site_apply(
  ant_t *js, sv_func_t *func,
  sv_obj_site_cache_t *site, ant_object_t *ptr
) {
  if (!ptr || !ptr->shape || !site) return;

  if (
    !site->shared_shape && !site->shape_build_failed &&
    site->key_atoms && site->key_count
  ) {
    ant_shape_t *sh = ptr->shape;
    ant_shape_retain(sh);
    bool ok = true;
    
    for (uint16_t i = 0; i < site->key_count && ok; i++) {
      uint32_t ai = site->key_atoms[i];
      if (ai >= (uint32_t)func->atom_count) { ok = false; break; }
      ok = ant_shape_add_interned_tr(&sh, func->atoms[ai].str, ANT_PROP_ATTR_DEFAULT, NULL);
    }
    
    if (ok) site->shared_shape = sh;
    else {
      ant_shape_release(sh);
      site->shape_build_failed = true;
    }
  }

  if (site->shared_shape) {
    if (site->shared_shape != ptr->shape) {
      ant_shape_retain(site->shared_shape);
      ant_shape_release(ptr->shape);
      ptr->shape = site->shared_shape;
    }
    uint32_t count = ant_shape_count(ptr->shape);
    if (count > ptr->prop_count) (void)js_obj_ensure_prop_capacity(ptr, count);
  } else if (!site->key_atoms) {
    site->shared_shape = ptr->shape;
    ant_shape_retain(site->shared_shape);
  }
}

static inline void sv_op_object(sv_vm_t *vm, ant_t *js, sv_func_t *func, uint8_t *ip) {
  ant_value_t obj = mkobj(js, 0);
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  sv_obj_site_cache_t *site = sv_obj_site_for_ip(func, ip);
  sv_obj_site_apply(js, func, site, ptr);

  ant_value_t proto = js->sym.object_proto;
  if (vtype(proto) == kTypeObject) js_set_proto_init(obj, proto);
  vm->stack[vm->sp++] = obj;
}

static inline void sv_op_private_token(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  ant_value_t obj = mkobj(js, 0);
  uint32_t hash = sv_get_u32(ip + 1);
  js_set_slot(obj, SLOT_DATA, js_mknum((double)hash));
  vm->stack[vm->sp++] = obj;
}

static inline void sv_op_array(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  uint16_t n = sv_get_u16(ip + 1);
  ant_value_t arr = js_mkarr_dense_literal(js, &vm->stack[vm->sp - n], n);
  vm->sp -= n;
  vm->stack[vm->sp++] = arr;
}

static inline void sv_op_set_brand(sv_vm_t *vm, uint8_t *ip) {
  if (vm->sp <= 0) return;
  uint8_t brand = sv_get_u8(ip + 1);
  ant_value_t obj = vm->stack[vm->sp - 1];
  if (is_object_type(obj))
    js_set_slot(obj, SLOT_BRAND, js_mknum((double)brand));
}

static inline void sv_op_regexp(sv_vm_t *vm, ant_t *js) {
  ant_value_t pattern = vm->stack[vm->sp - 2];
  ant_value_t flags = vm->stack[vm->sp - 1];
  vm->sp -= 2;
  vm->stack[vm->sp++] = regexp_create_literal(js, pattern, flags);
}

#endif
