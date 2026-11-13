#pragma once

#include <stdbool.h>
#include "inspector.h"

struct arg_file;
typedef struct ant_cli_args ant_cli_args_t;

typedef struct {
  int argc;
  char **argv;
} ant_argv_t;

typedef struct {
  int argc;
  char **argv;
  int original_argc;
  char **original_argv;
  ant_argv_t script_tail;

  ant_inspector_options_t inspector;
  bool sandbox_daemon;

  const char *eval;
  const char *repl_script;
  const char *input_type;
  const char *location;
  const char *cron_period;
  
  const char **preload;
  int preload_count;
  
  struct arg_file *localstorage_file;
  const char **files;
  int file_count;

  bool repl;
  bool print;
  bool watch;
  bool hot;
  bool web_compat;
  bool no_clear_screen;
  bool jitless;
  bool pending_deprecation;
  int gc_threads;

  char **antx_argv;
  char **env_options;
  char **env_argv;
  char **filtered_argv;
  ant_cli_args_t *args;
} ant_cli_t;

bool ant_cli_parse(ant_cli_t *cli, int argc, char **argv, const char *env_options, int *exit_code);
void ant_cli_free(ant_cli_t *cli);
