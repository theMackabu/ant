#ifndef ANT_MODULES_CRON_H
#define ANT_MODULES_CRON_H

#include "types.h"
#include "gc/modules.h"

void init_cron_module(ant_t *js);
void gc_mark_cron(ant_t *js, gc_mark_fn mark);
void cleanup_cron_module(ant_t *js);

int cron_run_scheduled_export(
  ant_t *js,
  ant_value_t default_export,
  const char *expression
);

#endif
