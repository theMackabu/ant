#ifndef SV_UPVALUES_H
#define SV_UPVALUES_H

#include "silver/engine.h"
#include "descriptors.h"

static inline ant_value_t sv_setup_function_prototype(
  ant_t *js, ant_value_t func_obj, 
  ant_value_t func_val
) {
  ant_value_t proto_obj = mkobj(js, 0);
  if (is_err(proto_obj)) return proto_obj;

  ant_handle_t hproto = js_root(js, proto_obj);
  ant_value_t object_proto = js_get_ctor_proto(js, "Object", 6);
  if (vtype(object_proto) == T_OBJ)
    js_set_proto(js, js_deref(js, hproto), object_proto);

  ant_value_t ctor_key = js_mkstr(js, "constructor", 11);
  if (is_err(ctor_key)) {
    js_unroot(js, hproto);
    return ctor_key;
  }
  ant_value_t set_ctor = js_setprop(js, js_deref(js, hproto), ctor_key, func_val);
  if (is_err(set_ctor)) {
    js_unroot(js, hproto);
    return set_ctor;
  }
  js_set_descriptor(js, js_deref(js, hproto), "constructor", 11, JS_DESC_W | JS_DESC_C);

  ant_value_t proto_key = js_mkstr(js, "prototype", 9);
  if (is_err(proto_key)) {
    js_unroot(js, hproto);
    return proto_key;
  }
  ant_value_t set_proto = js_setprop(js, func_obj, proto_key, js_deref(js, hproto));
  if (is_err(set_proto)) {
    js_unroot(js, hproto);
    return set_proto;
  }
  js_set_descriptor(js, func_obj, "prototype", 9, JS_DESC_W);

  js_unroot(js, hproto);
  return js_mkundef();
}

static inline ant_value_t sv_op_get_upval(
  sv_vm_t *vm, sv_frame_t *frame,
  ant_t *js, uint8_t *ip
) {
  uint16_t idx = sv_get_u16(ip + 1);
  sv_upvalue_t *uv = frame->upvalues[idx];
  ant_value_t val = *uv->location;
  if (val == SV_TDZ)
    return js_mkerr_typed(js, JS_ERR_REFERENCE,
      "Cannot access variable before initialization");
  vm->stack[vm->sp++] = val;
  return js_mkundef();
}

static inline void sv_op_put_upval(sv_vm_t *vm, sv_frame_t *frame, uint8_t *ip) {
  uint16_t idx = sv_get_u16(ip + 1);
  sv_upvalue_t *uv = frame->upvalues[idx];
  *uv->location = vm->stack[--vm->sp];
}

static inline void sv_op_set_upval(sv_vm_t *vm, sv_frame_t *frame, uint8_t *ip) {
  uint16_t idx = sv_get_u16(ip + 1);
  sv_upvalue_t *uv = frame->upvalues[idx];
  *uv->location = vm->stack[vm->sp - 1];
}

static inline void sv_op_close_upval(sv_vm_t *vm, sv_frame_t *frame, uint8_t *ip) {
  uint16_t idx = sv_get_u16(ip + 1);
  ant_value_t *slot = sv_frame_slot_ptr(frame, idx);
  if (!slot) return;

  sv_upvalue_t **pp = &vm->open_upvalues;
  while (*pp) {
    sv_upvalue_t *uv = *pp;
    if (uv->location >= slot) {
      uv->closed = *uv->location;
      uv->location = &uv->closed;
      *pp = uv->next;
    } else pp = &uv->next;
  }
}

static inline sv_upvalue_t *sv_capture_upvalue(sv_vm_t *vm, ant_value_t *slot) {
  sv_upvalue_t **pp = &vm->open_upvalues;
  while (*pp && (*pp)->location > slot)
    pp = &(*pp)->next;

  if (*pp && (*pp)->location == slot)
    return *pp;

  sv_upvalue_t *uv = calloc(1, sizeof(sv_upvalue_t));
  uv->location = slot;
  uv->next = *pp;
  *pp = uv;
  return uv;
}

static inline void sv_op_closure(
  sv_vm_t *vm, ant_t *js, sv_frame_t *frame,
  sv_func_t *func, uint8_t *ip
) {
  uint32_t idx = sv_get_u32(ip + 1);
  sv_func_t *child = (sv_func_t *)(uintptr_t)vdata(func->constants[idx]);

  sv_closure_t *closure = calloc(1, sizeof(sv_closure_t));
  closure->func = child;
  closure->bound_this = child->is_arrow ? frame->this : js_mkundef();
  closure->call_flags = 0;

  if (child->upvalue_count > 0) {
    closure->upvalues = calloc((size_t)child->upvalue_count, sizeof(sv_upvalue_t *));
    for (int i = 0; i < child->upvalue_count; i++) {
      sv_upval_desc_t *desc = &child->upval_descs[i];
      if (desc->is_local) {
        ant_value_t *slot = sv_frame_slot_ptr(frame, desc->index);
        if (!slot) slot = frame->bp;
        closure->upvalues[i] = sv_capture_upvalue(vm, slot);
      } else closure->upvalues[i] = frame->upvalues[desc->index];
    }
  }

  ant_value_t func_obj = mkobj(js, 0);
  closure->func_obj = func_obj;
  js_setprop(js, func_obj, js->length_str, tov((double)child->param_count));
  js_set_descriptor(js, func_obj, "length", 6, JS_DESC_C);

  ant_value_t func_val = mkval(T_FUNC, (uintptr_t)closure);
  if (!child->is_arrow && !child->is_method) {
    (void)sv_setup_function_prototype(js, func_obj, func_val);
  }

  if (child->is_strict)
    js_set_slot(js, func_obj, SLOT_STRICT, js_true);
  if (child->is_arrow) {
    js_set_slot(js, func_obj, SLOT_ARROW, js_true);
    js_set_slot(js, func_obj, SLOT_BOUND_THIS, frame->this);
  }
  if (child->is_async) {
    js_set_slot(js, func_obj, SLOT_ASYNC, js_true);
    ant_value_t async_proto = js_get_slot(js, js->global, SLOT_ASYNC_PROTO);
    if (vtype(async_proto) == T_FUNC)
      js_set_proto(js, func_obj, async_proto);
  } else {
    ant_value_t func_proto = js_get_slot(js, js->global, SLOT_FUNC_PROTO);
    if (vtype(func_proto) == T_FUNC)
      js_set_proto(js, func_obj, func_proto);
  }

  vm->stack[vm->sp++] = func_val;
}

#endif
