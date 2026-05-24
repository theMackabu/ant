#include <compat.h> // IWYU pragma: keep

#include "cli/misc.h"
#include "cli/pkg.h"
#include "sandbox/host.h"
#include "sandbox/sandbox.h"
#include "sandbox/vm.h"
#include "sandbox/cli.h"
#include "messages.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <crprintf.h>

typedef struct {
  ant_sandbox_launch_options_t launch;
  unsigned int timeout_ms;
  unsigned int boot_timeout_ms;
  bool verbose;
  bool eval_mode;
  const char *eval_source;
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

    if (strcmp(arg, "-V") == 0 || strcmp(arg, "--verbose") == 0) {
      opts->verbose = true;
      continue;
    }

    if (strcmp(arg, "-e") == 0 || strcmp(arg, "--eval") == 0 ||
        strncmp(arg, "-e=", 3) == 0 || strncmp(arg, "--eval=", 7) == 0) {
      if (opts->script_index >= 0) {
        fprintf(stderr, "sandbox: --eval cannot be combined with a script\n");
        return -EINVAL;
      }
      opts->eval_mode = true;
      if (strcmp(arg, "-e") == 0 || strcmp(arg, "--eval") == 0) {
        if (i + 1 >= argc) {
          fprintf(stderr, "sandbox: %s needs source\n", arg);
          return -EINVAL;
        }
        opts->eval_source = argv[++i];
      } else if (strncmp(arg, "-e=", 3) == 0) {
        opts->eval_source = arg + 3;
      } else opts->eval_source = arg + 7;
      continue;
    }

    if (strcmp(arg, "-t") == 0 || strncmp(arg, "-t=", 3) == 0 ||
        strcmp(arg, "--timeout") == 0 || strcmp(arg, "--timeout-ms") == 0 ||
        strncmp(arg, "--timeout=", 10) == 0 || strncmp(arg, "--timeout-ms=", 13) == 0) {
      const char *value = NULL;
      const char *name = "--timeout-ms";
      if (strcmp(arg, "-t") == 0 || strcmp(arg, "--timeout") == 0 || strcmp(arg, "--timeout-ms") == 0) {
        if (i + 1 >= argc) {
          fprintf(stderr, "sandbox: %s needs milliseconds\n", arg);
          return -EINVAL;
        }
        value = argv[++i];
      } else if (strncmp(arg, "-t=", 3) == 0) {
        value = arg + 3;
      } else if (strncmp(arg, "--timeout-ms=", 13) == 0) {
        value = arg + 13;
      } else {
        value = arg + 10;
      }
      int rc = sandbox_parse_ms_option(name, value, &opts->timeout_ms);
      if (rc != 0) return rc;
      continue;
    }

    if (strcmp(arg, "-b") == 0 || strcmp(arg, "-r") == 0 ||
        strncmp(arg, "-b=", 3) == 0 || strncmp(arg, "-r=", 3) == 0 ||
        strcmp(arg, "--boot-timeout") == 0 || strcmp(arg, "--boot-timeout-ms") == 0 ||
        strcmp(arg, "--request-timeout") == 0 || strcmp(arg, "--request-timeout-ms") == 0 ||
        strncmp(arg, "--boot-timeout=", 15) == 0 || strncmp(arg, "--boot-timeout-ms=", 18) == 0 ||
        strncmp(arg, "--request-timeout=", 18) == 0 || strncmp(arg, "--request-timeout-ms=", 21) == 0) {
      const char *value = NULL;
      const char *name = (strcmp(arg, "-r") == 0 || strncmp(arg, "-r=", 3) == 0 ||
                         strncmp(arg, "--request", 9) == 0) ? "--request-timeout-ms" : "--boot-timeout-ms";
      if (strcmp(arg, "-b") == 0 || strcmp(arg, "-r") == 0 || strchr(arg, '=') == NULL) {
        if (i + 1 >= argc) {
          fprintf(stderr, "sandbox: %s needs milliseconds\n", arg);
          return -EINVAL;
        }
        value = argv[++i];
      } else if (strncmp(arg, "-b=", 3) == 0 || strncmp(arg, "-r=", 3) == 0) {
        value = arg + 3;
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

    if (strcmp(arg, "-f") == 0 || strncmp(arg, "-f=", 3) == 0 ||
        strcmp(arg, "--forward") == 0 || strncmp(arg, "--forward=", 10) == 0) {
      const char *value = NULL;
      if (strcmp(arg, "-f") == 0 || strcmp(arg, "--forward") == 0) {
        if (i + 1 >= argc) {
          fprintf(stderr, "sandbox: --forward needs a port or host:guest pair\n");
          return -EINVAL;
        }
        value = argv[++i];
      } else if (strncmp(arg, "-f=", 3) == 0) {
        value = arg + 3;
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

    if (strcmp(arg, "-m") == 0 || strncmp(arg, "-m=", 3) == 0 ||
        strcmp(arg, "-w") == 0 || strncmp(arg, "-w=", 3) == 0 ||
        strcmp(arg, "--mount") == 0 || strncmp(arg, "--mount=", 8) == 0 ||
        strcmp(arg, "--write") == 0 || strncmp(arg, "--write=", 8) == 0) {
      bool readonly = strcmp(arg, "-m") == 0 || strncmp(arg, "-m=", 3) == 0 ||
                      strcmp(arg, "--mount") == 0 || strncmp(arg, "--mount=", 8) == 0;
      const char *value = NULL;
      if (strcmp(arg, "-m") == 0 || strcmp(arg, "-w") == 0 ||
          strcmp(arg, "--mount") == 0 || strcmp(arg, "--write") == 0) {
        if (i + 1 >= argc) {
          fprintf(stderr, "sandbox: %s needs host:guest\n", arg);
          return -EINVAL;
        }
        value = argv[++i];
      } else if (strncmp(arg, "-m=", 3) == 0 || strncmp(arg, "-w=", 3) == 0) {
        value = arg + 3;
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

    if (opts->eval_mode) {
      fprintf(stderr, "sandbox: --eval cannot be combined with a script\n");
      return -EINVAL;
    }
    opts->script_index = i;
    return 0;
  }

  return opts->eval_mode ? 0 : -EINVAL;
}

static void sandbox_print_usage(FILE *fp) {
  crfprintf(fp, msg.sandbox_help_header);
  crfprintf(fp, msg.ant_help_flags);
  print_flag(fp, (flag_help_t){ .s = "e", .l = "eval", .d = "source", .g = msg.sandbox_flag_eval });
  print_flag(fp, (flag_help_t){ .s = "m", .l = "mount", .d = "host:guest", .g = msg.sandbox_flag_mount });
  print_flag(fp, (flag_help_t){ .s = "w", .l = "write", .d = "host:guest", .g = msg.sandbox_flag_write });
  print_flag(fp, (flag_help_t){ .s = "f", .l = "forward", .d = "port|host:guest", .g = msg.sandbox_flag_forward });
  print_flag(fp, (flag_help_t){ .s = "t", .l = "timeout-ms", .d = "ms", .g = msg.sandbox_flag_timeout });
  print_flag(fp, (flag_help_t){ .s = "b", .l = "boot-timeout-ms", .d = "ms", .g = msg.sandbox_flag_boot_timeout });
  print_flag(fp, (flag_help_t){ .s = "r", .l = "request-timeout-ms", .d = "ms", .g = msg.sandbox_flag_request_timeout });
  print_flag(fp, (flag_help_t){ .s = "V", .l = "verbose", .g = msg.sandbox_flag_verbose });
  print_flag(fp, (flag_help_t){ .s = "h", .l = "help", .g = msg.sandbox_flag_help });
}

static bool sandbox_option_takes_separate_value(const char *arg) {
  if (strchr(arg, '=') != NULL) return false;
  return strcmp(arg, "-e") == 0 || strcmp(arg, "--eval") == 0 ||
         strcmp(arg, "-t") == 0 || strcmp(arg, "--timeout") == 0 ||
         strcmp(arg, "--timeout-ms") == 0 ||
         strcmp(arg, "-b") == 0 || strcmp(arg, "--boot-timeout") == 0 ||
         strcmp(arg, "--boot-timeout-ms") == 0 ||
         strcmp(arg, "-r") == 0 || strcmp(arg, "--request-timeout") == 0 ||
         strcmp(arg, "--request-timeout-ms") == 0 ||
         strcmp(arg, "-f") == 0 || strcmp(arg, "--forward") == 0 ||
         strcmp(arg, "-m") == 0 || strcmp(arg, "--mount") == 0 ||
         strcmp(arg, "-w") == 0 || strcmp(arg, "--write") == 0;
}

static bool sandbox_args_request_help(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    if (strcmp(arg, "--") == 0 || arg[0] != '-') return false;
    if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) return true;
    if (sandbox_option_takes_separate_value(arg)) i++;
  }
  return false;
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
#if defined(__APPLE__) && !defined(__aarch64__)
      fprintf(stderr, "sandbox: Hypervisor.framework sandbox backend requires Apple Silicon\n");
#elif defined(_WIN32)
      fprintf(stderr, "sandbox: native Windows sandbox backend is not implemented; use WSL for sandbox support\n");
#else
      fprintf(stderr, "sandbox: VM backend is not available\n");
#endif
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
  if (argc < 2 || sandbox_args_request_help(argc, argv)) {
    sandbox_print_usage(argc < 2 ? stderr : stdout);
    return argc < 2 ? EXIT_FAILURE : EXIT_SUCCESS;
  }

  ant_sandbox_cli_options_t opts;
  int rc = sandbox_parse_options(argc, argv, &opts);
  if (rc != 0 || (!opts.eval_mode && opts.script_index < 0)) {
    sandbox_print_usage(stderr);
    ant_sandbox_launch_options_cleanup(&opts.launch);
    return EXIT_FAILURE;
  }

  ant_sandbox_assets_t assets;
  char err[512] = { 0 };
  rc = ant_sandbox_assets_resolve(&assets, err, sizeof(err));
  if (rc != 0) {
    fprintf(stderr, "sandbox: %s\n", err[0] ? err : "failed to resolve sandbox assets");
    ant_sandbox_launch_options_cleanup(&opts.launch);
    return EXIT_FAILURE;
  }

  char cwd[4096];
  if (!getcwd(cwd, sizeof(cwd))) {
    fprintf(stderr, "sandbox: failed to read current directory: %s\n", strerror(errno));
    ant_sandbox_launch_options_cleanup(&opts.launch);
    return EXIT_FAILURE;
  }

  if (!opts.launch.explicit_mounts) {
    rc = ant_sandbox_launch_add_default_mount(&opts.launch, cwd, err, sizeof(err));
    if (rc != 0) {
      fprintf(stderr, "sandbox: %s\n", err[0] ? err : "failed to add default mount");
      ant_sandbox_launch_options_cleanup(&opts.launch);
      return EXIT_FAILURE;
    }
  }

  uint16_t tty_rows = 24;
  uint16_t tty_cols = 80;
  uint32_t capabilities = ant_sandbox_terminal_capabilities(&tty_rows, &tty_cols);
  uint16_t forward_ports[ANT_SANDBOX_MAX_FORWARDS];
  for (size_t i = 0; i < opts.launch.forward_count; i++) forward_ports[i] = opts.launch.forwards[i].guest_port;
  size_t request_len = 0;
  uint8_t *request = NULL;
  if (opts.eval_mode) {
    request = ant_sandbox_build_eval_request_frame(opts.launch.guest_cwd,
                                                   opts.eval_source,
                                                   capabilities,
                                                   tty_rows,
                                                   tty_cols,
                                                   &request_len);
  } else {
    int script_argc = argc - opts.script_index - 1;
    char **script_argv = argv + opts.script_index + 1;
    request = ant_sandbox_build_run_request_frame(opts.launch.guest_cwd,
                                                  argv[opts.script_index],
                                                  script_argc,
                                                  script_argv,
                                                  capabilities,
                                                  tty_rows,
                                                  tty_cols,
                                                  forward_ports,
                                                  (uint32_t)opts.launch.forward_count,
                                                  &request_len);
  }
  if (!request) {
    fprintf(stderr, "sandbox: failed to build request frame\n");
    ant_sandbox_launch_options_cleanup(&opts.launch);
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
  ant_sandbox_launch_options_cleanup(&opts.launch);

  if (vm_result.kind == ANT_SANDBOX_VM_RESULT_GUEST_EXIT) {
    if (vm_result.code >= 0 && vm_result.code <= 255) return vm_result.code;
    return EXIT_FAILURE;
  }
  
  if (rc == 0) return EXIT_SUCCESS;
  sandbox_print_vm_failure(&vm_result, rc);
  
  return EXIT_FAILURE;
}
