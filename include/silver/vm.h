#ifndef SILVER_VM_H
#define SILVER_VM_H

#include "types.h"

typedef struct sv_vm sv_vm_t;
typedef struct sv_func sv_func_t;
size_t os_thread_stack_size(void);

typedef enum {
  SV_VM_MAIN,
  SV_VM_ASYNC,
} sv_vm_kind_t;

extern int sv_user_stack_size_kb;
sv_vm_t *sv_vm_create(ant_t *js, sv_vm_kind_t kind);

void sv_vm_destroy(sv_vm_t *vm);
void sv_vm_limits(sv_vm_kind_t kind, int *out_stack_size, int *out_max_frames);

void sv_vm_gc_roots(sv_vm_t *vm, void (*op_val)(void *, jsval_t *), void *ctx);
void sv_vm_gc_roots_pending(void (*op_val)(void *, jsval_t *), void *ctx);

void sv_vm_visit_frame_funcs(sv_vm_t *vm, void (*visitor)(void *, sv_func_t *), void *ctx);
void sv_disasm(ant_t *js, sv_func_t *func, const char *label);

jsval_t sv_execute_frame(
  sv_vm_t *vm, sv_func_t *func,
  jsval_t this_val, jsval_t super_val,
  jsval_t *args, int argc
);

jsval_t sv_execute_entry(
  sv_vm_t *vm, sv_func_t *func,
  jsval_t this_val,
  jsval_t *args, int argc
);

jsval_t sv_execute_entry_tla(
  ant_t *js, sv_func_t *func, 
  jsval_t this_val
);

#endif
