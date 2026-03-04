#ifndef SV_STACK_H
#define SV_STACK_H

#include "silver/engine.h"

static inline void sv_op_pop(sv_vm_t *vm) {
  vm->sp--;
}

static inline void sv_op_dup(sv_vm_t *vm) {
  ant_value_t a = vm->stack[vm->sp - 1];
  vm->stack[vm->sp++] = a;
}

static inline void sv_op_dup2(sv_vm_t *vm) {
  ant_value_t a = vm->stack[vm->sp - 2];
  ant_value_t b = vm->stack[vm->sp - 1];
  vm->stack[vm->sp++] = a;
  vm->stack[vm->sp++] = b;
}

static inline void sv_op_swap(sv_vm_t *vm) {
  ant_value_t a = vm->stack[vm->sp - 2];
  vm->stack[vm->sp - 2] = vm->stack[vm->sp - 1];
  vm->stack[vm->sp - 1] = a;
}

static inline void sv_op_rot3l(sv_vm_t *vm) {
  ant_value_t x = vm->stack[vm->sp - 3];
  vm->stack[vm->sp - 3] = vm->stack[vm->sp - 2];
  vm->stack[vm->sp - 2] = vm->stack[vm->sp - 1];
  vm->stack[vm->sp - 1] = x;
}

static inline void sv_op_rot3r(sv_vm_t *vm) {
  ant_value_t x = vm->stack[vm->sp - 1];
  vm->stack[vm->sp - 1] = vm->stack[vm->sp - 2];
  vm->stack[vm->sp - 2] = vm->stack[vm->sp - 3];
  vm->stack[vm->sp - 3] = x;
}

static inline void sv_op_nip(sv_vm_t *vm) {
  vm->stack[vm->sp - 2] = vm->stack[vm->sp - 1];
  vm->sp--;
}

static inline void sv_op_nip2(sv_vm_t *vm) {
  vm->stack[vm->sp - 3] = vm->stack[vm->sp - 1];
  vm->sp -= 2;
}

static inline void sv_op_insert2(sv_vm_t *vm) {
  ant_value_t a = vm->stack[vm->sp - 1];
  ant_value_t obj = vm->stack[vm->sp - 2];
  vm->stack[vm->sp - 2] = a;
  vm->stack[vm->sp - 1] = obj;
  vm->stack[vm->sp++] = a;
}

static inline void sv_op_insert3(sv_vm_t *vm) {
  ant_value_t a = vm->stack[vm->sp - 1];
  ant_value_t prop = vm->stack[vm->sp - 2];
  ant_value_t obj = vm->stack[vm->sp - 3];
  vm->stack[vm->sp - 3] = a;
  vm->stack[vm->sp - 2] = obj;
  vm->stack[vm->sp - 1] = prop;
  vm->stack[vm->sp++] = a;
}

static inline void sv_op_swap_under(sv_vm_t *vm) {
  ant_value_t tmp = vm->stack[vm->sp - 3];
  vm->stack[vm->sp - 3] = vm->stack[vm->sp - 2];
  vm->stack[vm->sp - 2] = tmp;
}

static inline void sv_op_rot4_under(sv_vm_t *vm) {
  ant_value_t a = vm->stack[vm->sp - 4];
  ant_value_t b = vm->stack[vm->sp - 3];
  ant_value_t c = vm->stack[vm->sp - 2];
  vm->stack[vm->sp - 4] = c;
  vm->stack[vm->sp - 3] = a;
  vm->stack[vm->sp - 2] = b;
}

#endif
