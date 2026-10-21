#ifndef PKG_CMDS_H
#define PKG_CMDS_H

#include <stdbool.h>
#include <stddef.h>
#include "types.h"

typedef struct {
  int argc;
  char **argv;
} pkg_argv_t;

extern bool pkg_verbose;

pkg_argv_t pkg_command_argv(
  const char *argv0, const char *command, 
  int argc, char **argv
);

bool pkg_script_argv(
  pkg_argv_t *out, const char *argv0, const char *name,
  int argc, char **argv, int tail_argc, char **tail_argv
);

int pkg_run(ant_t *js);
void pkg_argv_free(pkg_argv_t *pkg);

bool pkg_parse_global_flag(const char *arg);
char **pkg_antx_argv(int *argc, char **argv);

#endif
