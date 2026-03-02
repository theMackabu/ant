#ifndef SV_EXCEPTIONS_H
#define SV_EXCEPTIONS_H

#include "silver/engine.h"
#include "errors.h"

typedef enum {
  SV_FINALLY_RET_JUMP = 0,
  SV_FINALLY_RET_THROW = 1,
  SV_FINALLY_RET_RETURN = 2,
  SV_FINALLY_RET_ERROR = 3,
} sv_finally_ret_t;

static inline bool sv_is_catch_only_handler(uint8_t *ip) {
  return ip && *ip == OP_CATCH;
}

static inline void sv_clear_completion(sv_vm_t *vm) {
  if (vm->fp < 0) return;
  sv_frame_t *frame = &vm->frames[vm->fp];
  frame->completion.kind = SV_COMPLETION_NONE;
  frame->completion.value = js_mkundef();
}

static inline void sv_close_upvalues_from_slot(sv_vm_t *vm, jsval_t *slot) {
  sv_upvalue_t **pp = &vm->open_upvalues;
  while (*pp) {
  sv_upvalue_t *uv = *pp;
  if (uv->location >= slot) {
    uv->closed = *uv->location;
    uv->location = &uv->closed;
    *pp = uv->next;
  } else pp = &uv->next;}
}

static inline jsval_t sv_op_throw(sv_vm_t *vm) {
  return vm->stack[--vm->sp];
}

static inline jsval_t sv_op_throw_error(
  sv_vm_t *vm, ant_t *js,
  sv_func_t *func, uint8_t *ip
) {
  uint32_t atom_idx = sv_get_u32(ip + 1);
  uint8_t err_type = sv_get_u8(ip + 5);
  sv_atom_t *a = &func->atoms[atom_idx];
  return js_mkerr_typed(js, (js_err_type_t)err_type, "%.*s", (int)a->len, a->str);
}

static inline void sv_op_try_push(sv_vm_t *vm, uint8_t *ip) {
  sv_frame_t *frame = &vm->frames[vm->fp];
  if (vm->handler_depth < SV_HANDLER_MAX) {
    int32_t off = sv_get_i32(ip + 1);
    sv_handler_t *h = &vm->handler_stack[vm->handler_depth++];
    h->kind = SV_HANDLER_TRY;
    h->ip = ip + sv_op_size[OP_TRY_PUSH] + off;
    h->saved_sp = vm->sp;
    frame->handler_top = vm->handler_depth;
  }
}

static inline void sv_op_try_pop(sv_vm_t *vm) {
  sv_frame_t *frame = &vm->frames[vm->fp];
  for (int i = vm->handler_depth - 1; i >= frame->handler_base; i--) {
    if (vm->handler_stack[i].kind != SV_HANDLER_TRY) continue;
    if (i + 1 < vm->handler_depth) {
      memmove(
        &vm->handler_stack[i],
        &vm->handler_stack[i + 1],
        (size_t)(vm->handler_depth - i - 1) * sizeof(vm->handler_stack[0]));
    }
    vm->handler_depth--;
    frame->handler_top = vm->handler_depth;
    return;
  }
}

static inline void sv_op_catch(sv_vm_t *vm, jsval_t caught, uint8_t *ip) {
  if (vm->sp == 0) vm->stack[vm->sp++] = caught;
  sv_clear_completion(vm);
}

static inline jsval_t sv_op_finally(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  sv_frame_t *frame = &vm->frames[vm->fp];
  if (vm->handler_depth >= SV_HANDLER_MAX)
    return js_mkerr(js, "handler stack overflow");

  int32_t off = sv_get_i32(ip + 1);
  sv_handler_t *h = &vm->handler_stack[vm->handler_depth++];
  h->kind = SV_HANDLER_FINALLY;
  h->ip = ip + sv_op_size[OP_FINALLY] + off;
  h->saved_sp = 0;
  frame->handler_top = vm->handler_depth;
  
  return js_mkundef();
}

static inline sv_finally_ret_t sv_op_finally_ret(
  sv_vm_t *vm, ant_t *js,
  uint8_t **resume_ip,
  jsval_t *completion_val
) {
  sv_frame_t *frame = &vm->frames[vm->fp];
  if (vm->handler_depth <= frame->handler_base ||
      vm->handler_stack[vm->handler_depth - 1].kind != SV_HANDLER_FINALLY) {
    *completion_val = js_mkerr(js, "finally handler underflow");
    return SV_FINALLY_RET_ERROR;
  }

  sv_handler_t h = vm->handler_stack[--vm->handler_depth];
  frame->handler_top = vm->handler_depth;

  if (frame->completion.kind == SV_COMPLETION_THROW) {
    *completion_val = frame->completion.value;
    sv_clear_completion(vm);
    return SV_FINALLY_RET_THROW;
  }

  if (frame->completion.kind == SV_COMPLETION_RETURN) {
    *completion_val = frame->completion.value;
    sv_clear_completion(vm);
    return SV_FINALLY_RET_RETURN;
  }

  *resume_ip = h.ip;
  return SV_FINALLY_RET_JUMP;
}

static inline void sv_op_nip_catch(sv_vm_t *vm) {
  jsval_t a = vm->stack[vm->sp - 1];
  vm->stack[vm->sp - 2] = a;
  vm->sp--;
}

static inline uint8_t *sv_vm_throw(sv_vm_t *vm, jsval_t err, int min_fp) {
  ant_t *js = vm->js;
  if (min_fp < 0) min_fp = 0;

  for (int f = vm->fp; f >= min_fp; f--) {
    sv_frame_t *frame = &vm->frames[f];
    int base = frame->handler_base;
    int top = (f == vm->fp) ? vm->handler_depth : frame->handler_top;
    for (int i = top - 1; i >= base; i--) {
      sv_handler_t *h = &vm->handler_stack[i];
      if (h->kind != SV_HANDLER_TRY) continue;

      for (int drop = vm->fp; drop > f; drop--) {
        jsval_t *bp = vm->frames[drop].bp;
        if (bp) sv_close_upvalues_from_slot(vm, bp);
      }

      jsval_t caught = err;
      if (vtype(err) == T_ERR && js->thrown_exists &&
          vtype(js->thrown_value) != T_UNDEF) {
        caught = js->thrown_value;
        js->thrown_value = js_mkundef();
        js->thrown_exists = false;
      }

      vm->sp = h->saved_sp;
      vm->fp = f;
      vm->frames[f].completion.kind = SV_COMPLETION_THROW;
      vm->frames[f].completion.value = caught;
      vm->stack[vm->sp++] = caught;
      vm->handler_depth = i;
      vm->frames[f].handler_top = i;
      return h->ip;
    }

    frame->handler_top = base;
    if (f == vm->fp) vm->handler_depth = base;
  }
  
  sv_clear_completion(vm);
  return NULL;
}

static inline uint8_t *sv_vm_unwind_for_return(sv_vm_t *vm, jsval_t ret) {
  sv_frame_t *frame = &vm->frames[vm->fp];
  for (int i = vm->handler_depth - 1; i >= frame->handler_base; i--) {
    sv_handler_t *h = &vm->handler_stack[i];
    if (h->kind != SV_HANDLER_TRY) continue;
    if (sv_is_catch_only_handler(h->ip)) continue;

    vm->handler_depth = i;
    frame->handler_top = i;
    vm->sp = h->saved_sp;
    frame->completion.kind = SV_COMPLETION_RETURN;
    frame->completion.value = ret;
    vm->stack[vm->sp++] = ret;
    return h->ip;
  }
  
  return NULL;
}

#endif
