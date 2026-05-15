#ifndef SV_UPVALUES_H
#define SV_UPVALUES_H

#include "internal.h"
#include "silver/engine.h"

static inline ant_value_t sv_mkprop_interned_exact_key(
  ant_t *js, ant_value_t obj,
  const char *interned_key, const char *fallback_key,
  size_t fallback_len, ant_value_t val, uint8_t attrs
) {
  if (!interned_key) interned_key = intern_string(fallback_key, fallback_len);
  if (!interned_key) return js_mkerr(js, "oom");
  return mkprop_interned_exact(js, obj, interned_key, val, attrs);
}

static inline ant_value_t sv_define_function_length(ant_t *js, ant_value_t func_obj, double length) {
  return sv_mkprop_interned_exact_key(
    js, func_obj,
    js->intern.length, "length", 6,
    tov(length), ANT_PROP_ATTR_CONFIGURABLE
  );
}

static inline ant_value_t sv_setup_function_prototype_with_parent(
  ant_t *js, ant_value_t func_obj,
  ant_value_t func_val, ant_value_t parent_proto
) {
  ant_value_t proto_obj = mkobj(js, 0);
  if (is_err(proto_obj)) return proto_obj;

  ant_value_t set_ctor = sv_mkprop_interned_exact_key(
    js, proto_obj,
    js->intern.constructor, "constructor", 11,
    func_val, ANT_PROP_ATTR_WRITABLE | ANT_PROP_ATTR_CONFIGURABLE
  );
  
  if (is_err(set_ctor)) return set_ctor;
  if (is_object_type(parent_proto)) js_set_proto_init(proto_obj, parent_proto);

  ant_value_t set_proto = sv_mkprop_interned_exact_key(
    js, func_obj,
    js->intern.prototype, "prototype", 9,
    proto_obj, ANT_PROP_ATTR_WRITABLE
  );
  
  if (is_err(set_proto)) return set_proto;
  return js_mkundef();
}

static inline ant_value_t sv_get_current_closure_module_ctx(ant_t *js, ant_value_t parent_func) {
  if (vtype(parent_func) == T_FUNC) {
    ant_value_t module_ctx = js_get_slot(js_func_obj(parent_func), SLOT_MODULE_CTX);
    if (is_object_type(module_ctx)) return module_ctx;
  }

  return js_module_eval_active_ctx(js);
}

static inline void sv_init_closure_function_object(
  ant_t *js,
  sv_closure_t *closure,
  ant_value_t func_val,
  ant_value_t module_ctx
) {
  sv_func_t *child = closure->func;
  ant_value_t func_obj = mkobj(js, 0);
  closure->func_obj = func_obj;
  if (is_err(func_obj) || !child) return;

  js_mark_constructor(
    func_obj,
    !child->is_arrow && !child->is_method && !child->is_generator && !child->is_async
  );
  (void)sv_define_function_length(js, func_obj, (double)child->function_length);

  if (is_object_type(module_ctx))
    js_set_slot_wb(js, func_obj, SLOT_MODULE_CTX, module_ctx);

  if (!child->is_arrow && !child->is_method && (!child->is_async || child->is_generator)) {
    ant_value_t parent_proto = child->is_async
      ? js->sym.async_generator_proto
      : (child->is_generator ? js->sym.generator_proto : js->sym.object_proto);
    (void)sv_setup_function_prototype_with_parent(js, func_obj, func_val, parent_proto);
  }

  if (child->is_async && child->is_generator) {
    js_set_slot(func_obj, SLOT_ASYNC, js_true);
    ant_value_t async_generator_proto = js_get_slot(js->global, SLOT_ASYNC_GENERATOR_PROTO);
    if (vtype(async_generator_proto) == T_FUNC) js_set_proto_init(func_obj, async_generator_proto);
  }
  
  else if (child->is_async) {
    js_set_slot(func_obj, SLOT_ASYNC, js_true);
    ant_value_t async_proto = js_get_slot(js->global, SLOT_ASYNC_PROTO);
    if (vtype(async_proto) == T_FUNC) js_set_proto_init(func_obj, async_proto);
  }
  
  else if (child->is_generator) {
    ant_value_t generator_proto = js_get_slot(js->global, SLOT_GENERATOR_PROTO);
    if (vtype(generator_proto) == T_FUNC) js_set_proto_init(func_obj, generator_proto);
  }
  
  else {
    ant_value_t func_proto = js_get_slot(js->global, SLOT_FUNC_PROTO);
    if (vtype(func_proto) == T_FUNC) js_set_proto_init(func_obj, func_proto);
  }
}

static inline ant_value_t sv_op_get_upval(
  sv_vm_t *vm, sv_frame_t *frame,
  ant_t *js, uint8_t *ip
) {
  uint16_t idx = sv_get_u16(ip + 1);
  sv_upvalue_t *uv = frame->upvalues[idx];
  ant_value_t val = *uv->location;
  if (val == SV_TDZ) return js_mkerr_typed(js, 
    JS_ERR_REFERENCE, 
    "Cannot access variable before initialization"
  );
  if (vtype(val) == T_STR && str_is_heap_builder(val)) {
    val = str_materialize(js, val);
    if (is_err(val)) return val;
  }
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

static inline ant_value_t sv_op_close_upval(sv_vm_t *vm, sv_frame_t *frame, uint8_t *ip) {
  uint16_t idx = sv_get_u16(ip + 1);
  ant_value_t *slot = sv_frame_slot_ptr(frame, idx);
  if (!slot) return js_mkundef();

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
  }
  
  return js_mkundef();
}

static inline sv_upvalue_t *sv_capture_upvalue(sv_vm_t *vm, ant_value_t *slot) {
  sv_upvalue_t **pp = &vm->open_upvalues;
  
  while (*pp && (*pp)->location > slot) pp = &(*pp)->next;
  if (*pp && (*pp)->location == slot) return *pp;

  sv_upvalue_t *uv = js_upvalue_alloc();
  uv->location = slot; uv->next = *pp; *pp = uv;
  
  return uv;
}

static inline ant_value_t sv_op_closure(
  sv_vm_t *vm, ant_t *js, sv_frame_t *frame,
  sv_func_t *func, uint8_t *ip
) {
  uint32_t idx = sv_get_u32(ip + 1);
  sv_func_t *child = (sv_func_t *)(uintptr_t)vdata(func->constants[idx]);

  sv_closure_t *closure = js_closure_alloc(js);
  closure->func = child;
  closure->bound_this = child->is_arrow ? frame->this : js_mkundef();
  closure->bound_argv = NULL;
  closure->bound_argc = 0;
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
  }}

  ant_value_t func_val = mkval(T_FUNC, (uintptr_t)closure);
  vm->stack[vm->sp++] = func_val;
  ant_value_t module_ctx = sv_get_current_closure_module_ctx(js, frame->callee);
  sv_init_closure_function_object(js, closure, func_val, module_ctx);
  
  return js_mkundef();
}

#endif
