#pragma once

#include "types.h"

size_t os_thread_stack_size(void);
int os_run_main(int (*fn)(int, char **), int argc, char **argv);

extern int sv_user_stack_size_kb;

sv_vm_t *sv_vm_create(ant_t *js);
ant_value_t sv_resume_suspended(sv_vm_t *vm);

void sv_vm_destroy(sv_vm_t *vm);
void sv_vm_limits(int *out_stack_size, int *out_max_frames);
void sv_disasm(ant_t *js, sv_func_t *func, const char *label);

ant_value_t sv_execute_entry(sv_vm_t *vm, sv_func_t *func, ant_value_t this_val, ant_value_t *args, int argc);
ant_value_t sv_execute_frame(sv_vm_t *vm, sv_func_t *func, ant_value_t this_val, ant_value_t super_val, ant_value_t *args, int argc);
ant_value_t sv_vm_call_explicit_this(sv_vm_t *vm, ant_t *js, ant_value_t func, ant_value_t this_val, ant_value_t *args, int argc);

typedef enum {
  SV_CALL_MODE_NORMAL = 0,
  SV_CALL_MODE_EXPLICIT_THIS,
  SV_CALL_MODE_CONSTRUCT,
} sv_call_mode_t;

ant_value_t sv_vm_call(
  sv_vm_t *vm, ant_t *js, ant_value_t func,
  ant_value_t this_val, ant_value_t *args, int argc,
  ant_value_t *out_this, ant_value_t new_target
);

ant_value_t sv_vm_call_mode(
  sv_vm_t *vm, ant_t *js, ant_value_t func,
  ant_value_t this_val, ant_value_t *args, int argc,
  ant_value_t *out_this, sv_call_mode_t mode, ant_value_t new_target
);

ant_value_t sv_super_constructor(ant_value_t constructor);

ant_value_t sv_construct(
  sv_vm_t *vm, ant_t *js, ant_value_t func,
  ant_value_t *args, int argc, ant_value_t new_target
);

ant_value_t sv_super_construct_error(ant_t *js, ant_value_t parent, ant_value_t new_target, ant_value_t error);

ant_value_t sv_super_construct_slow(
  sv_vm_t *vm, ant_t *js, ant_value_t constructor, ant_value_t this_val,
  ant_value_t *args, int argc, ant_value_t new_target
);
