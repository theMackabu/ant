#ifndef ANT_CLI_SANDBOX_H
#define ANT_CLI_SANDBOX_H

#include "types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  ANT_SANDBOX_REQUEST_NONE = 0,
  ANT_SANDBOX_REQUEST_RUN,
  ANT_SANDBOX_REQUEST_EVAL,
} ant_sandbox_request_mode_t;

typedef enum {
  ANT_SANDBOX_FRAME_RUN = 1,
  ANT_SANDBOX_FRAME_EVAL = 2,
  ANT_SANDBOX_FRAME_STDOUT = 3,
  ANT_SANDBOX_FRAME_STDERR = 4,
  ANT_SANDBOX_FRAME_RESULT = 5,
  ANT_SANDBOX_FRAME_ERROR = 6,
  ANT_SANDBOX_FRAME_EXIT = 7,
} ant_sandbox_frame_type_t;

#define ANT_SANDBOX_FRAME_MAGIC "ANTF"
#define ANT_SANDBOX_FRAME_VERSION 1u
#define ANT_SANDBOX_FRAME_HEADER_SIZE 12u
#define ANT_SANDBOX_FRAME_MAX_SIZE (16u * 1024u * 1024u)

#define ANT_SANDBOX_CAP_STDOUT_TTY  (1u << 0)
#define ANT_SANDBOX_CAP_STDERR_TTY  (1u << 1)
#define ANT_SANDBOX_CAP_COLOR_FORCE (1u << 2)
#define ANT_SANDBOX_CAP_COLOR_STRIP (1u << 3)

typedef struct {
  ant_sandbox_request_mode_t mode;
  char *cwd;
  char *entry;
  char *source;
  char **argv;
  int argc;
  uint32_t capabilities;
  uint16_t tty_rows;
  uint16_t tty_cols;
  uint16_t *forward_ports;
  uint32_t forward_count;
} ant_sandbox_request_t;

void ant_sandbox_request_free(ant_sandbox_request_t *req);
uint8_t *ant_sandbox_build_frame(
  ant_sandbox_frame_type_t type,
  const void *payload,
  size_t payload_len,
  size_t *len_out
);
uint8_t *ant_sandbox_build_run_request_frame(
  const char *cwd,
  const char *entry,
  int argc,
  char **argv,
  uint32_t capabilities,
  uint16_t tty_rows,
  uint16_t tty_cols,
  const uint16_t *forward_ports,
  uint32_t forward_count,
  size_t *len_out
);
bool ant_sandbox_parse_request_frame(const uint8_t *frame, size_t frame_len, ant_sandbox_request_t *out);
int ant_sandbox_eval_module(ant_t *js, const char *script, size_t len);

#endif
