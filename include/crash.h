#pragma once

#include <stdbool.h>
#include <uv.h>
#include "types.h"

void ant_crash_init(int argc, char **argv);
void ant_crash_suppress_reporting(void);

bool ant_crash_handles_signal(int sig);
void ant_crash_set_signal_listener(int sig, uv_async_t *listener);
unsigned ant_crash_take_signal_deliveries(int sig);

int ant_crash_run_internal_report(ant_t *js);
bool ant_crash_is_internal_report(int argc, char **argv);
