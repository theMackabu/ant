#include <compat.h> // IWYU pragma: keep

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <crprintf.h>

#include "common.h"
#include "utils.h"
#include "messages.h"
#include "cli/commands.h"
#include "cli/compile.h"
#include "cli/registry.h"
#include "cli/version.h"
#include "sandbox/cli.h"

typedef int (*cmd_fn)(int argc, char **argv);

typedef enum {
  COMMAND_PKG,
  COMMAND_NATIVE,
} command_kind_t;

typedef struct {
  const char *name;
  const char *alias;
  const char *desc;
  command_kind_t kind;
  cmd_fn fn;
} subcommand_t;

#define PKG_COMMAND(n, a, d) { .name = #n, .alias = a, .desc = d, .kind = COMMAND_PKG }
#define NATIVE_COMMAND(n, a, d, f) { .name = #n, .alias = a, .desc = d, .kind = COMMAND_NATIVE, .fn = f }

static const subcommand_t subcommands[] = {
  PKG_COMMAND(init,       NULL,      "Create a new package.json"),
  PKG_COMMAND(install,    "i",       "Install dependencies from lockfile"),
  PKG_COMMAND(update,     "up",      "Re-resolve dependencies and refresh lockfile"),
  PKG_COMMAND(add,        "a",       "Add a package to dependencies"),
  PKG_COMMAND(remove,     "rm",      "Remove a package from dependencies"),
  PKG_COMMAND(trust,      NULL,      "Run lifecycle scripts for packages"),
  PKG_COMMAND(run,        NULL,      "Run a script from package.json"),
  PKG_COMMAND(exec,       "x",       "Run a command from node_modules/.bin"),
  NATIVE_COMMAND(login,   NULL,      "Authenticate with ants.land", pkg_cmd_login),
  NATIVE_COMMAND(publish, NULL,      "Publish the current package to ants.land", pkg_cmd_publish),
  PKG_COMMAND(config,     NULL,      "Manage package manager settings"),
  PKG_COMMAND(why,        "explain", "Show why a package is installed"),
  PKG_COMMAND(info,       NULL,      "Show package information from registry"),
  PKG_COMMAND(ls,         "list",    "List installed packages"),
  PKG_COMMAND(cache,      NULL,      "Manage the package cache"),
  PKG_COMMAND(create,     NULL,      "Scaffold a project from a template"),
  NATIVE_COMMAND(sandbox, NULL,      "Run a script in the Ant sandbox", ant_sandbox_cmd),
  NATIVE_COMMAND(compile, NULL,      "Compile a script into a standalone executable", ant_cmd_compile),
  NATIVE_COMMAND(upgrade, NULL,      "Upgrade Ant to the latest version", ant_upgrade),
};

#undef PKG_COMMAND
#undef NATIVE_COMMAND

static const subcommand_t *find_subcommand(const char *name) {
  for (const subcommand_t *cmd = subcommands; cmd < subcommands + countof(subcommands); cmd++) {
    if (strcmp(name, cmd->name) == 0) return cmd;
    if (cmd->alias && strcmp(name, cmd->alias) == 0) return cmd;
  }
  return NULL;
}

void ant_cli_print_subcommands(void) {
  crprintf("<bold>Commands:</>\n");
  for (const subcommand_t *cmd = subcommands; cmd < subcommands + countof(subcommands); cmd++)
    crprintf("  <pad=18>%s</pad> %s\n", cmd->name, cmd->desc);
  crprintf(msg.ant_command_extra);
  printf("\n");
}

static int argv_index(const ant_cli_t *cli, const char *token) {
  for (int i = 1; i < cli->argc; i++) if (cli->argv[i] == token) return i;
  return -1;
}

static int run_native(const subcommand_t *cmd, int argc, char **argv, ant_argv_t tail) {
  if (tail.argc == 0) return cmd->fn(argc, argv);

  char **full = try_oom(sizeof(*full) * (size_t)(argc + tail.argc + 1));
  memcpy(full, argv, sizeof(*full) * (size_t)argc);
  memcpy(full + argc, tail.argv, sizeof(*full) * (size_t)tail.argc);
  full[argc + tail.argc] = NULL;

  int code = cmd->fn(argc + tail.argc, full);
  free(full);
  return code;
}

static ant_route_t refuse_watch(int *exit_code) {
  crfprintf(stderr, msg.watch_subcommand_error);
  *exit_code = EXIT_FAILURE;
  return ANT_ROUTE_EXIT;
}

ant_route_t ant_cli_route(const ant_cli_t *cli, pkg_argv_t *pkg, int *exit_code) {
  if (cli->sandbox_daemon || cli->eval || cli->repl || cli->file_count == 0 || !cli->files[0]) return ANT_ROUTE_NONE;

  const char *positional = cli->files[0];
  int at = argv_index(cli, positional);
  if (at <= 0) {
    crfprintf(stderr, msg.argument_fatal);
    *exit_code = EXIT_FAILURE;
    return ANT_ROUTE_EXIT;
  }

  int argc = cli->argc - at;
  char **argv = cli->argv + at;
  ant_argv_t tail = cli->script_tail;
  const subcommand_t *cmd = find_subcommand(positional);

  if (cmd) {
    if (cli->watch) return refuse_watch(exit_code);
    if (cmd->kind == COMMAND_NATIVE) {
      *exit_code = run_native(cmd, argc, argv, tail);
      return ANT_ROUTE_EXIT;
    }
    *pkg = pkg_command_argv(cli->argv[0], cmd->name, argc - 1, argv + 1, tail.argc, tail.argv);
    return ANT_ROUTE_PKG;
  }

  if (!pkg_script_argv(pkg, cli->argv[0], positional, argc, argv, tail.argc, tail.argv)) return ANT_ROUTE_NONE;
  return cli->watch ? refuse_watch(exit_code) : ANT_ROUTE_PKG;
}
