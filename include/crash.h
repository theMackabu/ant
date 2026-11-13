#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "types.h"

typedef void (*ant_crash_guarded_call_t)(void *context);

void ant_crash_init(int argc, char **argv);
void ant_crash_suppress_reporting(void);

int ant_crash_guard_native_call(
  ant_crash_guarded_call_t call,
  void *context, uintptr_t *fault_address
);

int ant_crash_run_internal_report(ant_t *js);
bool ant_crash_is_internal_report(int argc, char **argv);
