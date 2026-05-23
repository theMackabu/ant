#include <compat.h> // IWYU pragma: keep

#include "cli/pkg.h"
#include "sandbox/host.h"
#include "sandbox/sandbox.h"
#include "sandbox/vm.h"
#include "sandbox/cli.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

typedef struct {
  ant_sandbox_launch_options_t launch;
  unsigned int timeout_ms;
  unsigned int boot_timeout_ms;
  bool verbose;
  int script_index;
} ant_sandbox_cli_options_t;

static int sandbox_parse_ms_option(const char *name, const char *value, unsigned int *out) {
  if (!value || !value[0]) {
    fprintf(stderr, "sandbox: %s needs milliseconds\n", name);
    return -EINVAL;
  }
  errno = 0;
  char *end = NULL;
  unsigned long parsed = strtoul(value, &end, 10);
  if (errno != 0 || !end || *end != '\0' || parsed > UINT_MAX) {
    fprintf(stderr, "sandbox: %s must be a non-negative millisecond value\n", name);
    return -EINVAL;
  }
  *out = (unsigned int)parsed;
  return 0;
}

static int sandbox_parse_options(int argc, char **argv, ant_sandbox_cli_options_t *opts) {
  memset(opts, 0, sizeof(*opts));
  ant_sandbox_launch_options_init(&opts->launch);
  opts->boot_timeout_ms = ANT_SANDBOX_DEFAULT_BOOT_TIMEOUT_MS;
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

    if (strcmp(arg, "--timeout") == 0 || strcmp(arg, "--timeout-ms") == 0 ||
        strncmp(arg, "--timeout=", 10) == 0 || strncmp(arg, "--timeout-ms=", 13) == 0) {
      const char *value = NULL;
      const char *name = strncmp(arg, "--timeout-ms", 12) == 0 ? "--timeout-ms" : "--timeout";
      if (strcmp(arg, "--timeout") == 0 || strcmp(arg, "--timeout-ms") == 0) {
        if (i + 1 >= argc) {
          fprintf(stderr, "sandbox: %s needs milliseconds\n", arg);
          return -EINVAL;
        }
        value = argv[++i];
      } else if (strncmp(arg, "--timeout-ms=", 13) == 0) {
        value = arg + 13;
      } else {
        value = arg + 10;
      }
      int rc = sandbox_parse_ms_option(name, value, &opts->timeout_ms);
      if (rc != 0) return rc;
      continue;
    }

    if (strcmp(arg, "--boot-timeout") == 0 || strcmp(arg, "--boot-timeout-ms") == 0 ||
        strcmp(arg, "--request-timeout") == 0 || strcmp(arg, "--request-timeout-ms") == 0 ||
        strncmp(arg, "--boot-timeout=", 15) == 0 || strncmp(arg, "--boot-timeout-ms=", 18) == 0 ||
        strncmp(arg, "--request-timeout=", 18) == 0 || strncmp(arg, "--request-timeout-ms=", 21) == 0) {
      const char *value = NULL;
      const char *name = strncmp(arg, "--request", 9) == 0 ? "--request-timeout-ms" : "--boot-timeout-ms";
      if (strchr(arg, '=') == NULL) {
        if (i + 1 >= argc) {
          fprintf(stderr, "sandbox: %s needs milliseconds\n", arg);
          return -EINVAL;
        }
        value = argv[++i];
      } else if (strncmp(arg, "--boot-timeout-ms=", 18) == 0) {
        value = arg + 18;
      } else if (strncmp(arg, "--boot-timeout=", 15) == 0) {
        value = arg + 15;
      } else if (strncmp(arg, "--request-timeout-ms=", 21) == 0) {
        value = arg + 21;
      } else {
        value = arg + 18;
      }
      int rc = sandbox_parse_ms_option(name, value, &opts->boot_timeout_ms);
      if (rc != 0) return rc;
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
  fprintf(stderr, "Usage: ant sandbox [--verbose] [--timeout-ms ms] [--boot-timeout-ms ms] [--mount host:guest] [--write host:guest] [--forward <port|host:guest>] <script.js> [args...]\n");
}

static void sandbox_fill_result_from_rc(ant_sandbox_vm_result_t *result, int rc) {
  if (!result || result->kind != ANT_SANDBOX_VM_RESULT_NONE) return;
  if (rc == 0) return;
  if (rc == -ENOSYS) result->kind = ANT_SANDBOX_VM_RESULT_BACKEND_UNAVAILABLE;
  else if (rc == -EINVAL) result->kind = ANT_SANDBOX_VM_RESULT_CONFIG_ERROR;
  else if (rc == -ETIMEDOUT) result->kind = ANT_SANDBOX_VM_RESULT_TIMEOUT;
  else result->kind = ANT_SANDBOX_VM_RESULT_VM_ERROR;
  result->code = rc;
}

static void sandbox_print_vm_failure(const ant_sandbox_vm_result_t *result, int rc) {
  if (!result || result->kind == ANT_SANDBOX_VM_RESULT_NONE) {
    fprintf(stderr, "sandbox: VM failed (%d)\n", rc);
    return;
  }

  switch (result->kind) {
    case ANT_SANDBOX_VM_RESULT_GUEST_EXIT:
      break;
    case ANT_SANDBOX_VM_RESULT_BACKEND_UNAVAILABLE:
      fprintf(stderr, "sandbox: VM backend is not available\n");
      break;
    case ANT_SANDBOX_VM_RESULT_CONFIG_ERROR:
      fprintf(stderr, "sandbox: VM configuration failed (%d)\n", result->code);
      break;
    case ANT_SANDBOX_VM_RESULT_TIMEOUT:
      fprintf(stderr, "sandbox: VM timed out\n");
      break;
    case ANT_SANDBOX_VM_RESULT_KERNEL_PANIC:
      /* The backend prints the panic summary and console tail. */
      break;
    case ANT_SANDBOX_VM_RESULT_PROTOCOL_ERROR:
      fprintf(stderr, "sandbox: daemon protocol error (%d)\n", result->code);
      break;
    case ANT_SANDBOX_VM_RESULT_TRANSPORT_ERROR:
      fprintf(stderr, "sandbox: transport error (%d)\n", result->code);
      break;
    case ANT_SANDBOX_VM_RESULT_VM_ERROR:
    case ANT_SANDBOX_VM_RESULT_NONE:
    default:
      fprintf(stderr, "sandbox: VM failed (%d)\n", result->code ? result->code : rc);
      break;
  }
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

  ant_sandbox_vm_result_t vm_result = {0};
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
    .timeout_ms = opts.timeout_ms,
    .boot_timeout_ms = opts.boot_timeout_ms,
    .verbose = opts.verbose || pkg_verbose,
    .result = &vm_result,
  };

  rc = ant_sandbox_vm_start(&config);
  sandbox_fill_result_from_rc(&vm_result, rc);
  free(request);

  if (rc == 0) return EXIT_SUCCESS;
  if (vm_result.kind == ANT_SANDBOX_VM_RESULT_GUEST_EXIT) {
    if (vm_result.code >= 0 && vm_result.code <= 255) return vm_result.code;
    return EXIT_FAILURE;
  }
  sandbox_print_vm_failure(&vm_result, rc);
  return EXIT_FAILURE;
}
