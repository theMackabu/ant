#pragma once

#include "sandbox/vm.h"
#include "native_tag.h"

#include "sandbox/sandbox.h" // IWYU pragma: export
#include "sandbox/host.h"    // IWYU pragma: export
#include "../vm_internal.h"  // IWYU pragma: export

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static constexpr uint32_t ANT_SANDBOX_VM_HELPER_MAGIC = Ant_NativeTag("AMVH");

enum {
  ANT_SANDBOX_VM_HELPER_STATS_FD = 197,
  ANT_SANDBOX_VM_HELPER_CMD_FD   = 198,
  ANT_SANDBOX_VM_HELPER_MSG_FD   = 199,
};

typedef enum: uint8_t {
  ANT_SANDBOX_VM_HELPER_CMD_EXECUTE,
  ANT_SANDBOX_VM_HELPER_CMD_DESTROY,
  ANT_SANDBOX_VM_HELPER_CMD_SEND,
  ANT_SANDBOX_VM_HELPER_CMD_CREATE,
  ANT_SANDBOX_VM_HELPER_CMD_STATS,
} ant_sandbox_vm_helper_cmd_t;

typedef enum: uint8_t {
  ANT_SANDBOX_VM_HELPER_MSG_CREATE_RESULT,
  ANT_SANDBOX_VM_HELPER_MSG_FRAME,
  ANT_SANDBOX_VM_HELPER_MSG_EXECUTE_RESULT,
} ant_sandbox_vm_helper_msg_t;

typedef struct {
  uint32_t magic;
  uint8_t type;
  uint8_t frame_type;
  uint16_t reserved;
  uint32_t length;
} ant_sandbox_vm_helper_header_t;

typedef struct {
  int32_t rc;
  int32_t kind;
  int32_t code;
} ant_sandbox_vm_helper_result_t;

typedef struct {
  ant_sandbox_vm_config_t config;
  ant_sandbox_mount_t *mounts;
  ant_sandbox_port_forward_t *forwards;
  uint16_t *allow_host_ports;
  char *image_path;
  char *kernel_path;
} sandbox_helper_owned_config_t;

int sandbox_vm_read_full(int fd, void *buf, size_t len);
int sandbox_vm_write_full(int fd, const void *buf, size_t len);
int sandbox_vm_send_header(int fd, uint8_t type, uint8_t frame_type, uint32_t length);
int sandbox_vm_send_result(int fd, uint8_t type, int rc, const ant_sandbox_vm_result_t *result);

uint64_t sandbox_vm_cpu_time_ns(void);
uint8_t *sandbox_helper_encode_config(const ant_sandbox_vm_config_t *config, size_t *len_out);

void sandbox_helper_owned_config_free(sandbox_helper_owned_config_t *owned);
bool sandbox_helper_decode_config(const uint8_t *data, size_t len, sandbox_helper_owned_config_t *owned);
