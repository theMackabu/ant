#ifndef ANT_CLI_DAEMON_H
#define ANT_CLI_DAEMON_H

#include <stdbool.h>

#include "types.h"
#include "sandbox/sandbox.h"

bool ant_cli_daemon_prepare(const ant_sandbox_request_t *request);
void ant_cli_daemon_terminal(ant_t *js, const ant_sandbox_request_t *request);
int ant_cli_daemon_run(ant_t *js, ant_sandbox_request_t *request, const char *argv0);

#endif
