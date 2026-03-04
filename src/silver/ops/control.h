#ifndef SV_CONTROL_H
#define SV_CONTROL_H

#include "silver/engine.h"

static inline uint8_t *sv_op_jmp(uint8_t *ip) {
  int32_t off = sv_get_i32(ip + 1);
  return ip + sv_op_size[OP_JMP] + off;
}

static inline uint8_t *sv_op_jmp_false(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  ant_value_t v = vm->stack[--vm->sp];
  if (!js_truthy(js, v)) {
    int32_t off = sv_get_i32(ip + 1);
    return ip + sv_op_size[OP_JMP_FALSE] + off;
  }
  return ip + sv_op_size[OP_JMP_FALSE];
}

static inline uint8_t *sv_op_jmp_true(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  ant_value_t v = vm->stack[--vm->sp];
  if (js_truthy(js, v)) {
    int32_t off = sv_get_i32(ip + 1);
    return ip + sv_op_size[OP_JMP_TRUE] + off;
  }
  return ip + sv_op_size[OP_JMP_TRUE];
}

static inline uint8_t *sv_op_jmp_false_peek(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  ant_value_t v = vm->stack[vm->sp - 1];
  if (!js_truthy(js, v)) {
    int32_t off = sv_get_i32(ip + 1);
    return ip + sv_op_size[OP_JMP_FALSE_PEEK] + off;
  }
  return ip + sv_op_size[OP_JMP_FALSE_PEEK];
}

static inline uint8_t *sv_op_jmp_true_peek(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  ant_value_t v = vm->stack[vm->sp - 1];
  if (js_truthy(js, v)) {
    int32_t off = sv_get_i32(ip + 1);
    return ip + sv_op_size[OP_JMP_TRUE_PEEK] + off;
  }
  return ip + sv_op_size[OP_JMP_TRUE_PEEK];
}

static inline uint8_t *sv_op_jmp_not_nullish(sv_vm_t *vm, uint8_t *ip) {
  ant_value_t v = vm->stack[vm->sp - 1];
  uint8_t t = vtype(v);
  if (t != T_NULL && t != T_UNDEF) {
    int32_t off = sv_get_i32(ip + 1);
    return ip + sv_op_size[OP_JMP_NOT_NULLISH] + off;
  }
  return ip + sv_op_size[OP_JMP_NOT_NULLISH];
}

static inline uint8_t *sv_op_jmp8(uint8_t *ip) {
  int8_t off = sv_get_i8(ip + 1);
  return ip + sv_op_size[OP_JMP8] + off;
}

static inline uint8_t *sv_op_jmp_false8(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  ant_value_t v = vm->stack[--vm->sp];
  if (!js_truthy(js, v)) {
    int8_t off = sv_get_i8(ip + 1);
    return ip + sv_op_size[OP_JMP_FALSE8] + off;
  }
  return ip + sv_op_size[OP_JMP_FALSE8];
}

static inline uint8_t *sv_op_jmp_true8(sv_vm_t *vm, ant_t *js, uint8_t *ip) {
  ant_value_t v = vm->stack[--vm->sp];
  if (js_truthy(js, v)) {
    int8_t off = sv_get_i8(ip + 1);
    return ip + sv_op_size[OP_JMP_TRUE8] + off;
  }
  return ip + sv_op_size[OP_JMP_TRUE8];
}

#endif
