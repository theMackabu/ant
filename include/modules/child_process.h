#ifndef ANT_CHILD_PROCESS_MODULE_H
#define ANT_CHILD_PROCESS_MODULE_H

#include "types.h"

int has_pending_child_processes(void);

ant_value_t child_process_library(ant_t *js);
ant_value_t child_process_pipeline_result(ant_t *js, ant_value_t commands, ant_value_t options);

ant_value_t child_process_exec_file_result(
  ant_t *js, ant_value_t file,
  ant_value_t argv, ant_value_t options
);

#endif
