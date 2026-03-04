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

static inline void sv_op_object(sv_vm_t *vm, ant_t *js) {
  ant_value_t obj = mkobj(js, 0);
  ant_value_t proto = js_get_ctor_proto(js, "Object", 6);
  if (vtype(proto) == T_OBJ) js_set_proto(js, obj, proto);
  vm->stack[vm->sp++] = obj;
}

static inline void sv_op_array(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  uint16_t n = sv_get_u16(ip + 1);
  ant_value_t arr = js_mkarr(js);
  ant_handle_t h = js_root(js, arr);
  for (uint16_t i = 0; i < n; i++) {
    ant_value_t val = vm->stack[vm->sp - n + i];
    js_arr_push(js, js_deref(js, h), val);
  }
  vm->sp -= n;
  vm->stack[vm->sp++] = js_deref(js, h);
  js_unroot(js, h);
}

static inline void sv_op_regexp(sv_vm_t *vm, ant_t *js) {
  ant_value_t pattern = vm->stack[vm->sp - 2];
  ant_value_t flags   = vm->stack[vm->sp - 1];
  vm->sp -= 2;

  ant_value_t regexp_obj = mkobj(js, 0);
  ant_handle_t h = js_root(js, regexp_obj);

  ant_value_t regexp_proto = js_get_ctor_proto(js, "RegExp", 6);
  if (vtype(regexp_proto) == T_OBJ) js_set_proto(js, js_deref(js, h), regexp_proto);

  js_mkprop_fast(js, js_deref(js, h), "source", 6, pattern);

  ant_offset_t flen = 0;
  const char *fstr = "";
  if (vtype(flags) == T_STR) {
    ant_offset_t foff;
    foff = vstr(js, flags, &flen);
    fstr = (const char *)&js->mem[foff];
  }

  bool g = false, i = false, m = false, s = false, y = false;
  for (ant_offset_t k = 0; k < flen; k++) {
    if (fstr[k] == 'g') g = true;
    if (fstr[k] == 'i') i = true;
    if (fstr[k] == 'm') m = true;
    if (fstr[k] == 's') s = true;
    if (fstr[k] == 'y') y = true;
  }

  char sorted[8]; int si = 0;
  if (g) sorted[si++] = 'g';
  if (i) sorted[si++] = 'i';
  if (m) sorted[si++] = 'm';
  if (s) sorted[si++] = 's';
  if (y) sorted[si++] = 'y';

  ant_value_t obj = js_deref(js, h);
  js_mkprop_fast(js, obj, "flags", 5, js_mkstr(js, sorted, si));
  js_mkprop_fast(js, obj, "global", 6, mkval(T_BOOL, g ? 1 : 0));
  js_mkprop_fast(js, obj, "ignoreCase", 10, mkval(T_BOOL, i ? 1 : 0));
  js_mkprop_fast(js, obj, "multiline", 9, mkval(T_BOOL, m ? 1 : 0));
  js_mkprop_fast(js, obj, "dotAll", 6, mkval(T_BOOL, s ? 1 : 0));
  js_mkprop_fast(js, obj, "sticky", 6, mkval(T_BOOL, y ? 1 : 0));
  js_mkprop_fast(js, obj, "lastIndex", 9, tov(0));

  vm->stack[vm->sp++] = js_deref(js, h);
  js_unroot(js, h);
}

#endif
