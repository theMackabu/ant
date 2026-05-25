#ifndef ANT_SANDBOX_HOST_H
#define ANT_SANDBOX_HOST_H

#include "sandbox/vm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ANT_SANDBOX_DEFAULT_GUEST_CWD "/workspace"
#define ANT_SANDBOX_MAX_FORWARDS 32
#define ANT_SANDBOX_MAX_MOUNTS 8

typedef struct {
  char image[4096];
  char kernel[4096];
  char cache_dir[4096];
} ant_sandbox_assets_t;

typedef struct {
  ant_sandbox_mount_t mounts[ANT_SANDBOX_MAX_MOUNTS];
  char mount_hosts[ANT_SANDBOX_MAX_MOUNTS][4096];
  char mount_guests[ANT_SANDBOX_MAX_MOUNTS][1024];
  char mount_tags[ANT_SANDBOX_MAX_MOUNTS][1200];
  ant_sandbox_port_forward_t forwards[ANT_SANDBOX_MAX_FORWARDS];
  char temp_dirs[ANT_SANDBOX_MAX_MOUNTS][4096];
  size_t mount_count;
  size_t temp_dir_count;
  size_t forward_count;
  char guest_cwd[1024];
  bool explicit_mounts;
} ant_sandbox_launch_options_t;

const char *ant_sandbox_cache_arch(void);
void ant_sandbox_launch_options_init(ant_sandbox_launch_options_t *opts);
void ant_sandbox_launch_options_cleanup(ant_sandbox_launch_options_t *opts);

int ant_sandbox_launch_add_mount(
  ant_sandbox_launch_options_t *opts,
  const char *value,
  bool readonly,
  char *err,
  size_t err_len
);

int ant_sandbox_launch_add_forward(
  ant_sandbox_launch_options_t *opts,
  const char *value,
  char *err,
  size_t err_len
);

int ant_sandbox_launch_add_default_mount(
  ant_sandbox_launch_options_t *opts,
  const char *host_path,
  char *err,
  size_t err_len
);

int ant_sandbox_assets_resolve(ant_sandbox_assets_t *assets, char *err, size_t err_len);
uint32_t ant_sandbox_terminal_capabilities(uint16_t *rows_out, uint16_t *cols_out);

#endif
