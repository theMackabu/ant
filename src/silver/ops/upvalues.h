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

  ant_value_t object_proto = js->object;
  if (vtype(object_proto) == T_OBJ) js_set_proto_init(proto_obj, object_proto);

  ant_value_t ctor_key = js_mkstr(js, "constructor", 11);
  if (is_err(ctor_key)) return ctor_key;
  ant_value_t set_ctor = js_setprop(js, proto_obj, ctor_key, func_val);
  if (is_err(set_ctor)) return set_ctor;
  js_set_descriptor(js, proto_obj, "constructor", 11, JS_DESC_W | JS_DESC_C);

  ant_value_t proto_key = js_mkstr(js, "prototype", 9);
  if (is_err(proto_key)) return proto_key;
  ant_value_t set_proto = js_setprop(js, func_obj, proto_key, proto_obj);
  if (is_err(set_proto)) return set_proto;
  js_set_descriptor(js, func_obj, "prototype", 9, JS_DESC_W);

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
  ant_value_t *loc = uv->location;
  if (sv_slot_in_vm_stack(vm, loc) && loc >= slot) {
    uv->closed = *loc;
    uv->location = &uv->closed;
    *pp = uv->next;
  }
  else pp = &uv->next;
}}

static inline sv_upvalue_t *sv_capture_upvalue(sv_vm_t *vm, ant_value_t *slot) {
  sv_upvalue_t **pp = &vm->open_upvalues;
  
  while (*pp && (*pp)->location > slot) pp = &(*pp)->next;
  if (*pp && (*pp)->location == slot) return *pp;

  sv_upvalue_t *uv = js_upvalue_alloc();
  uv->location = slot; uv->next = *pp; *pp = uv;
  
  return uv;
}

static inline void sv_op_closure(
  sv_vm_t *vm, ant_t *js, sv_frame_t *frame,
  sv_func_t *func, uint8_t *ip
) {
  uint32_t idx = sv_get_u32(ip + 1);
  sv_func_t *child = (sv_func_t *)(uintptr_t)vdata(func->constants[idx]);

  sv_closure_t *closure = js_closure_alloc(js);
  closure->func = child;
  closure->bound_this = child->is_arrow ? frame->this : js_mkundef();
  closure->bound_args = js_mkundef();
  closure->super_val = js_mkundef();
  closure->call_flags = child->is_arrow ? SV_CALL_IS_ARROW : 0;

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

  ant_value_t func_val = mkval(T_FUNC, (uintptr_t)closure);
  vm->stack[vm->sp++] = func_val;

  ant_value_t func_obj = mkobj(js, 0);
  closure->func_obj = func_obj;
  ant_value_t module_ctx = js_module_eval_active_ctx(js);
  
  js_mark_constructor(func_obj, !child->is_arrow && !child->is_method);
  js_setprop(js, func_obj, js->length_str, tov((double)child->param_count));
  js_set_descriptor(js, func_obj, "length", 6, JS_DESC_C);
  
  if (is_object_type(module_ctx)) js_set_slot_wb(js, func_obj, SLOT_MODULE_CTX, module_ctx);
  if (!child->is_arrow && !child->is_method) sv_setup_function_prototype(js, func_obj, func_val);
  
  if (child->is_async) {
    js_set_slot(func_obj, SLOT_ASYNC, js_true);
    ant_value_t async_proto = js_get_slot(js->global, SLOT_ASYNC_PROTO);
    if (vtype(async_proto) == T_FUNC) js_set_proto_init(func_obj, async_proto);
  } else {
    ant_value_t func_proto = js_get_slot(js->global, SLOT_FUNC_PROTO);
    if (vtype(func_proto) == T_FUNC) js_set_proto_init(func_obj, func_proto);
  }
}

#endif
