#ifndef ANT_CLI_COMMANDS_H
#define ANT_CLI_COMMANDS_H

#include "cli/options.h"
#include "cli/pkg.h"

typedef enum {
  ANT_ROUTE_NONE,
  ANT_ROUTE_PKG,
  ANT_ROUTE_EXIT,
} ant_route_t;

void ant_cli_print_subcommands(void);
ant_route_t ant_cli_route(const ant_cli_t *cli, pkg_argv_t *pkg, int *exit_code);

#endif
