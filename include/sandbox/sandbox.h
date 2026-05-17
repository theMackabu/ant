#ifndef ANT_CLI_SANDBOX_H
#define ANT_CLI_SANDBOX_H

#include "types.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
  ANT_SANDBOX_REQUEST_NONE = 0,
  ANT_SANDBOX_REQUEST_RUN,
  ANT_SANDBOX_REQUEST_EVAL,
} ant_sandbox_request_mode_t;

typedef struct {
  ant_sandbox_request_mode_t mode;
  char *cwd;
  char *entry;
  char *source;
  char **argv;
  int argc;
} ant_sandbox_request_t;

void ant_sandbox_request_free(ant_sandbox_request_t *req);
bool ant_sandbox_parse_request_json(const char *json, size_t json_len, ant_sandbox_request_t *out);
int ant_sandbox_eval_module(ant_t *js, const char *script, size_t len);

#endif
