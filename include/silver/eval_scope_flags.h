#pragma once

#include <stdbool.h>

typedef struct {
  bool this_binding;
  bool super_binding;
  bool is_static;
  bool no_arguments;
  bool has_with;
} sv_eval_scope_flags_t;
