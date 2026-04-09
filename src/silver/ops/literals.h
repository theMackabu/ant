#ifndef SV_LITERALS_H
#define SV_LITERALS_H

#include "ant.h"
#include "silver/engine.h"

static inline void sv_op_const(sv_vm_t *vm, sv_func_t *func, uint8_t *ip) {
  uint32_t idx = sv_get_u32(ip + 1);
  vm->stack[vm->sp++] = func->constants[idx];
}

static inline void sv_op_const_i8(sv_vm_t *vm, uint8_t *ip) {
  int8_t val = sv_get_i8(ip + 1);
  vm->stack[vm->sp++] = tov((double)val);
}

static inline void sv_op_const8(sv_vm_t *vm, sv_func_t *func, uint8_t *ip) {
  uint8_t idx = sv_get_u8(ip + 1);
  vm->stack[vm->sp++] = func->constants[idx];
}

static inline void sv_op_undef(sv_vm_t *vm) {
  vm->stack[vm->sp++] = mkval(T_UNDEF, 0);
}

static inline void sv_op_null(sv_vm_t *vm) {
  vm->stack[vm->sp++] = mkval(T_NULL, 0);
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
  if (!func || !ip || !func->obj_sites || func->obj_site_count == 0) return NULL;
  uint32_t off = (uint32_t)(ip - func->code);
  for (uint16_t i = 0; i < func->obj_site_count; i++) {
    if (func->obj_sites[i].bc_off == off) return &func->obj_sites[i];
  }
  return NULL;
}

static inline void sv_op_object(sv_vm_t *vm, ant_t *js, sv_func_t *func, uint8_t *ip) {
  ant_value_t obj = mkobj(js, 0);
  ant_object_t *ptr = js_obj_ptr(js_as_obj(obj));
  sv_obj_site_cache_t *site = sv_obj_site_for_ip(func, ip);
  if (ptr && ptr->shape && site) {
  if (site->shared_shape) {
    if (site->shared_shape != ptr->shape) {
      ant_shape_retain(site->shared_shape);
      ant_shape_release(ptr->shape);
      ptr->shape = site->shared_shape;
    }
    uint32_t count = ant_shape_count(ptr->shape);
    if (count > ptr->prop_count) (void)js_obj_ensure_prop_capacity(ptr, count);
  } else {
    site->shared_shape = ptr->shape;
    ant_shape_retain(site->shared_shape);
  }}
  
  ant_value_t proto = js->sym.object_proto;
  if (vtype(proto) == T_OBJ) js_set_proto_init(obj, proto);
  vm->stack[vm->sp++] = obj;
}

static inline void sv_op_array(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  uint16_t n = sv_get_u16(ip + 1);
  ant_value_t arr = js_mkarr(js);
  for (uint16_t i = 0; i < n; i++) {
    ant_value_t val = vm->stack[vm->sp - n + i];
    js_arr_push(js, arr, val);
  }
  vm->sp -= n;
  vm->stack[vm->sp++] = arr;
}

// TODO: reduce duplication with regex.c
static inline void sv_op_regexp(sv_vm_t *vm, ant_t *js) {
  ant_value_t pattern = vm->stack[vm->sp - 2];
  ant_value_t flags = vm->stack[vm->sp - 1];
  vm->sp -= 2;

  ant_value_t regexp_obj = mkobj(js, 0);
  ant_value_t regexp_proto = js_get_ctor_proto(js, "RegExp", 6);
  if (vtype(regexp_proto) == T_OBJ) js_set_proto_init(regexp_obj, regexp_proto);

  js_mkprop_fast(js, regexp_obj, "source", 6, pattern);

  ant_offset_t flen = 0;
  const char *fstr = "";
  if (vtype(flags) == T_STR) {
    ant_offset_t foff;
    foff = vstr(js, flags, &flen);
    fstr = (const char *)(uintptr_t)(foff);
  }

  bool d = false, g = false, i = false, m = false;
  bool s = false, u = false, v = false, y = false;
  
  for (ant_offset_t k = 0; k < flen; k++) {
    if (fstr[k] == 'd') d = true;
    if (fstr[k] == 'g') g = true;
    if (fstr[k] == 'i') i = true;
    if (fstr[k] == 'm') m = true;
    if (fstr[k] == 's') s = true;
    if (fstr[k] == 'u') u = true;
    if (fstr[k] == 'v') v = true;
    if (fstr[k] == 'y') y = true;
  }

  char sorted[10]; int si = 0;
  if (d) sorted[si++] = 'd';
  if (g) sorted[si++] = 'g';
  if (i) sorted[si++] = 'i';
  if (m) sorted[si++] = 'm';
  if (s) sorted[si++] = 's';
  if (u) sorted[si++] = 'u';
  if (v) sorted[si++] = 'v';
  if (y) sorted[si++] = 'y';

  js_mkprop_fast(js, regexp_obj, "flags", 5, js_mkstr(js, sorted, si));
  js_mkprop_fast(js, regexp_obj, "hasIndices", 10, mkval(T_BOOL, d ? 1 : 0));
  js_mkprop_fast(js, regexp_obj, "global", 6, mkval(T_BOOL, g ? 1 : 0));
  js_mkprop_fast(js, regexp_obj, "ignoreCase", 10, mkval(T_BOOL, i ? 1 : 0));
  js_mkprop_fast(js, regexp_obj, "multiline", 9, mkval(T_BOOL, m ? 1 : 0));
  js_mkprop_fast(js, regexp_obj, "dotAll", 6, mkval(T_BOOL, s ? 1 : 0));
  js_mkprop_fast(js, regexp_obj, "unicode", 7, mkval(T_BOOL, u ? 1 : 0));
  js_mkprop_fast(js, regexp_obj, "unicodeSets", 11, mkval(T_BOOL, v ? 1 : 0));
  js_mkprop_fast(js, regexp_obj, "sticky", 6, mkval(T_BOOL, y ? 1 : 0));
  js_mkprop_fast(js, regexp_obj, "lastIndex", 9, tov(0));

  vm->stack[vm->sp++] = regexp_obj;
}

#endif
