#pragma once

#include <stdbool.h>

typedef struct {
  bool this_binding;
  bool super_binding;
  bool is_static;
} sv_eval_scope_flags_t;
