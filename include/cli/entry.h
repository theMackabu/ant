#ifndef ANT_CLI_ENTRY_H
#define ANT_CLI_ENTRY_H

#include <stdbool.h>
#include <stddef.h>

#include "types.h"
#include "cli/options.h"

typedef enum {
  ANT_INPUT_COMMONJS,
  ANT_INPUT_MODULE,
} ant_input_mode_t;

int ant_entry_eval(
  ant_t *js, const char *source, size_t len,
  const char *tag, bool print, ant_input_mode_t mode
);

int ant_entry_stdin(ant_t *js, bool print, ant_input_mode_t mode, bool inspect);
int ant_entry_module(ant_t *js, const char *filename, const char *cron_period);
int ant_entry_files(ant_t *js, const char **files, int count, const char *cron_period);

ant_argv_t ant_entry_process_argv(const char *argv0, const char *first, bool resolve, ant_argv_t tail);

#endif
