#ifndef ANT_CRASH_H
#define ANT_CRASH_H

#include <stdbool.h>
#include "types.h"

void ant_crash_init(int argc, char **argv);
void ant_crash_suppress_reporting(void);

int ant_crash_run_internal_report(ant_t *js);
bool ant_crash_is_internal_report(int argc, char **argv);

#endif
