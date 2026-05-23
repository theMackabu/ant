#include <compat.h> // IWYU pragma: keep

#include "cli/pkg.h"
#include "sandbox/host.h"
#include "sandbox/sandbox.h"
#include "sandbox/vm.h"
#include "sandbox/cli.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

typedef struct {
  ant_sandbox_launch_options_t launch;
  bool verbose;
  int script_index;
} ant_sandbox_cli_options_t;

static int sandbox_parse_options(int argc, char **argv, ant_sandbox_cli_options_t *opts) {
  memset(opts, 0, sizeof(*opts));
  ant_sandbox_launch_options_init(&opts->launch);
  opts->script_index = -1;

  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    if (strcmp(arg, "--") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "sandbox: missing script after --\n");
        return -EINVAL;
      }
      opts->script_index = i + 1;
      return 0;
    }

    if (strcmp(arg, "--verbose") == 0) {
      opts->verbose = true;
      continue;
    }

    if (strcmp(arg, "--forward") == 0 || strncmp(arg, "--forward=", 10) == 0) {
      const char *value = NULL;
      if (strcmp(arg, "--forward") == 0) {
        if (i + 1 >= argc) {
          fprintf(stderr, "sandbox: --forward needs a port or host:guest pair\n");
          return -EINVAL;
        }
        value = argv[++i];
      } else {
        value = arg + 10;
      }

      char err[512] = { 0 };
      int rc = ant_sandbox_launch_add_forward(&opts->launch, value, err, sizeof(err));
      if (rc != 0) {
        fprintf(stderr, "sandbox: %s\n", err[0] ? err : "invalid forward");
        return rc;
      }
      continue;
    }

    if (strcmp(arg, "--mount") == 0 || strncmp(arg, "--mount=", 8) == 0 ||
        strcmp(arg, "--write") == 0 || strncmp(arg, "--write=", 8) == 0) {
      bool readonly = arg[2] == 'm';
      const char *value = NULL;
      if (strcmp(arg, "--mount") == 0 || strcmp(arg, "--write") == 0) {
        if (i + 1 >= argc) {
          fprintf(stderr, "sandbox: %s needs host:guest\n", arg);
          return -EINVAL;
        }
        value = argv[++i];
      } else {
        value = arg + 8;
      }
      char err[512] = { 0 };
      int rc = ant_sandbox_launch_add_mount(&opts->launch, value, readonly, err, sizeof(err));
      if (rc != 0) {
        fprintf(stderr, "sandbox: %s\n", err[0] ? err : "invalid mount");
        return rc;
      }
      continue;
    }

    if (arg[0] == '-') {
      fprintf(stderr, "sandbox: unknown option '%s'\n", arg);
      return -EINVAL;
    }

    opts->script_index = i;
    return 0;
  }

  return -EINVAL;
}

static void sandbox_print_usage(void) {
  fprintf(stderr, "Usage: ant sandbox [--verbose] [--mount host:guest] [--write host:guest] [--forward <port|host:guest>] <script.js> [args...]\n");
}

int ant_sandbox_cmd(int argc, char **argv) {
  if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
    sandbox_print_usage();
    return argc < 2 ? EXIT_FAILURE : EXIT_SUCCESS;
  }

  ant_sandbox_cli_options_t opts;
  int rc = sandbox_parse_options(argc, argv, &opts);
  if (rc != 0 || opts.script_index < 0) {
    sandbox_print_usage();
    return EXIT_FAILURE;
  }

  ant_sandbox_assets_t assets;
  char err[512] = { 0 };
  rc = ant_sandbox_assets_resolve(&assets, err, sizeof(err));
  if (rc != 0) {
    fprintf(stderr, "sandbox: %s\n", err[0] ? err : "failed to resolve sandbox assets");
    return EXIT_FAILURE;
  }

  char cwd[4096];
  if (!getcwd(cwd, sizeof(cwd))) {
    fprintf(stderr, "sandbox: failed to read current directory: %s\n", strerror(errno));
    return EXIT_FAILURE;
  }

  if (!opts.launch.explicit_mounts) {
    rc = ant_sandbox_launch_add_default_mount(&opts.launch, cwd, err, sizeof(err));
    if (rc != 0) {
      fprintf(stderr, "sandbox: %s\n", err[0] ? err : "failed to add default mount");
      return EXIT_FAILURE;
    }
  }

  int script_argc = argc - opts.script_index - 1;
  char **script_argv = argv + opts.script_index + 1;
  uint16_t tty_rows = 24;
  uint16_t tty_cols = 80;
  uint32_t capabilities = ant_sandbox_terminal_capabilities(&tty_rows, &tty_cols);
  uint16_t forward_ports[ANT_SANDBOX_MAX_FORWARDS];
  for (size_t i = 0; i < opts.launch.forward_count; i++) forward_ports[i] = opts.launch.forwards[i].guest_port;
  size_t request_len = 0;
  uint8_t *request = ant_sandbox_build_run_request_frame(opts.launch.guest_cwd,
                                                         argv[opts.script_index],
                                                         script_argc,
                                                         script_argv,
                                                         capabilities,
                                                         tty_rows,
                                                         tty_cols,
                                                         forward_ports,
                                                         (uint32_t)opts.launch.forward_count,
                                                         &request_len);
  if (!request) {
    fprintf(stderr, "sandbox: failed to build request frame\n");
    return EXIT_FAILURE;
  }

  ant_sandbox_vm_config_t config = {
    .image_path = assets.image,
    .kernel_path = assets.kernel,
    .request_data = request,
    .request_len = request_len,
    .capabilities = capabilities,
    .mounts = opts.launch.mounts,
    .mount_count = opts.launch.mount_count,
    .network_enabled = true,
    .forwards = opts.launch.forwards,
    .forward_count = opts.launch.forward_count,
    .cpu_count = 1,
    .memory_size = 1024ull * 1024ull * 1024ull,
    .timeout_ms = 0,
    .verbose = opts.verbose || pkg_verbose,
  };

  rc = ant_sandbox_vm_start(&config);
  free(request);

  if (rc == 0) return EXIT_SUCCESS;
  if (rc == -ENOSYS) {
    fprintf(stderr, "sandbox: VM backend is not ready to run the cached image yet\n");
  }
  return EXIT_FAILURE;
}
