#include <compat.h> // IWYU pragma: keep

#include "cli/misc.h"
#include "cli/pkg.h"
#include "sandbox/host.h"
#include "sandbox/sandbox.h"
#include "sandbox/vm.h"
#include "sandbox/cli.h"
#include "messages.h"
#include "utils.h"

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

typedef enum {
  SANDBOX_OPT_EVAL,
  SANDBOX_OPT_TIMEOUT,
  SANDBOX_OPT_BOOT_TIMEOUT,
  SANDBOX_OPT_FORWARD,
  SANDBOX_OPT_MOUNT,
  SANDBOX_OPT_WRITE,
  SANDBOX_OPT_VERBOSE,
} sandbox_option_id_t;

typedef struct {
  const char *short_name;
  const char *long_name;
  sandbox_option_id_t id;
  bool takes_value;
} sandbox_option_spec_t;

typedef struct {
  const sandbox_option_spec_t *spec;
  const char *value;
} sandbox_option_match_t;

static const sandbox_option_spec_t sandbox_option_specs[] = {
  { "-e", "--eval", SANDBOX_OPT_EVAL, true },
  { "-t", "--timeout", SANDBOX_OPT_TIMEOUT, true },
  { NULL, "--timeout-ms", SANDBOX_OPT_TIMEOUT, true },
  { "-b", "--boot-timeout", SANDBOX_OPT_BOOT_TIMEOUT, true },
  { NULL, "--boot-timeout-ms", SANDBOX_OPT_BOOT_TIMEOUT, true },
  { "-r", "--request-timeout", SANDBOX_OPT_BOOT_TIMEOUT, true },
  { NULL, "--request-timeout-ms", SANDBOX_OPT_BOOT_TIMEOUT, true },
  { "-f", "--forward", SANDBOX_OPT_FORWARD, true },
  { "-m", "--mount", SANDBOX_OPT_MOUNT, true },
  { "-w", "--write", SANDBOX_OPT_WRITE, true },
  { "-V", "--verbose", SANDBOX_OPT_VERBOSE, false },
  { NULL, NULL, 0, false },
};

static bool sandbox_match_name(const char *arg, const char *name, const char **value_out) {
  if (!name) return false;
  size_t name_len = strlen(name);
  if (strcmp(arg, name) == 0) {
    *value_out = NULL;
    return true;
  }
  if (strncmp(arg, name, name_len) == 0 && arg[name_len] == '=') {
    *value_out = arg + name_len + 1;
    return true;
  }
  return false;
}

static bool sandbox_match_option(const char *arg, sandbox_option_match_t *match) {
  for (const sandbox_option_spec_t *spec = sandbox_option_specs; spec->short_name || spec->long_name; spec++) {
    const char *value = NULL;
    if (sandbox_match_name(arg, spec->short_name, &value) ||
        sandbox_match_name(arg, spec->long_name, &value)) {
      match->spec = spec;
      match->value = value;
      return true;
    }
  }
  return false;
}

static int sandbox_parse_ms_option(const char *name, const char *value, unsigned int *out) {
  if (!value || !value[0]) {
    crfprintf(stderr, msg.arg_opt_needed, name);
    return -EINVAL;
  }
  
  errno = 0;
  char *end = NULL;
  unsigned long parsed = strtoul(value, &end, 10);
  if (errno != 0 || !end || *end != '\0' || parsed > UINT_MAX) {
    crfprintf(stderr, msg.sandbox_arg_ms_invalid, name);
    return -EINVAL;
  }
  
  *out = (unsigned int)parsed;
  return 0;
}

static int sandbox_apply_option(
  int argc,
  char **argv,
  int *index,
  ant_sandbox_cli_options_t *opts
) {
  const char *arg = argv[*index];
  sandbox_option_match_t match = { 0 };
  if (!sandbox_match_option(arg, &match)) {
    crfprintf(stderr, msg.arg_invalid, arg);
    return -EINVAL;
  }

  if (!match.spec->takes_value && match.value) {
    crfprintf(stderr, msg.arg_invalid, arg);
    return -EINVAL;
  }

  const char *value = match.value;
  if (match.spec->takes_value && !value) {
    if (*index + 1 >= argc) {
      crfprintf(stderr, msg.arg_opt_needed, arg);
      return -EINVAL;
    }
    value = argv[++(*index)];
  }

  char err[512] = { 0 };
  switch (match.spec->id) {
    case SANDBOX_OPT_EVAL:
      if (opts->script_index >= 0) {
        fprintf(stderr, "sandbox: --eval cannot be combined with a script\n");
        return -EINVAL;
      }
      opts->eval_mode = true;
      opts->eval_source = value;
      return 0;
    case SANDBOX_OPT_TIMEOUT:
      return sandbox_parse_ms_option("--timeout-ms", value, &opts->timeout_ms);
    case SANDBOX_OPT_BOOT_TIMEOUT:
      return sandbox_parse_ms_option(
        strncmp(arg, "--request", 9) == 0 || strcmp(arg, "-r") == 0 || strncmp(arg, "-r=", 3) == 0
          ? "--request-timeout-ms"
          : "--boot-timeout-ms",
        value,
        &opts->boot_timeout_ms
      );
    case SANDBOX_OPT_FORWARD: {
      int rc = ant_sandbox_launch_add_forward(&opts->launch, value, err, sizeof(err));
      if (rc != 0) fprintf(stderr, "sandbox: %s\n", err[0] ? err : "invalid forward");
      return rc;
    }
    case SANDBOX_OPT_MOUNT: {
      int rc = ant_sandbox_launch_add_mount(&opts->launch, value, true, err, sizeof(err));
      if (rc != 0) fprintf(stderr, "sandbox: %s\n", err[0] ? err : "invalid mount");
      return rc;
    }
    case SANDBOX_OPT_WRITE: {
      int rc = ant_sandbox_launch_add_mount(&opts->launch, value, false, err, sizeof(err));
      if (rc != 0) fprintf(stderr, "sandbox: %s\n", err[0] ? err : "invalid mount");
      return rc;
    }
    case SANDBOX_OPT_VERBOSE:
      opts->verbose = true;
      return 0;
  }

  crfprintf(stderr, msg.arg_invalid, arg);
  return -EINVAL;
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
        crfprintf(stderr, msg.arg_opt_needed, "--");
        return -EINVAL;
      }
      opts->script_index = i + 1;
      return 0;
    }

    if (arg[0] == '-') {
      int rc = sandbox_apply_option(argc, argv, &i, opts);
      if (rc != 0) return rc;
      continue;
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
  sandbox_option_match_t match = { 0 };
  return sandbox_match_option(arg, &match) && match.spec->takes_value && !match.value;
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
  else if (rc == ANT_SANDBOX_CPU_TIME_LIMIT_CODE) result->kind = ANT_SANDBOX_VM_RESULT_CPU_TIME_LIMIT;
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
    case ANT_SANDBOX_VM_RESULT_CPU_TIME_LIMIT:
      fprintf(stderr, "sandbox: VM exceeded its CPU time budget\n");
      break;
    case ANT_SANDBOX_VM_RESULT_KERNEL_PANIC:
      // the backend prints the panic summary and console tail
      break;
    case ANT_SANDBOX_VM_RESULT_PROTOCOL_ERROR:
      fprintf(stderr, "sandbox: daemon protocol error (%d)\n", result->code);
      break;
    case ANT_SANDBOX_VM_RESULT_TRANSPORT_ERROR:
      fprintf(stderr, "sandbox: transport error (%d)\n", result->code);
      break;
    case ANT_SANDBOX_VM_RESULT_CANCELED:
      fprintf(stderr, "sandbox: VM canceled\n");
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
    if (rc != 0) fputc('\n', stderr);
    sandbox_print_usage(stderr);
    ant_sandbox_launch_options_cleanup(&opts.launch);
    return EXIT_FAILURE;
  }

  if (!opts.eval_mode) {
    char *resolved_file = resolve_js_file(argv[opts.script_index]);
    if (!resolved_file) {
      crfprintf(stderr, msg.module_not_found, argv[opts.script_index]);
      ant_sandbox_launch_options_cleanup(&opts.launch);
      return EXIT_FAILURE;
    }
    free(resolved_file);
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
  if (opts.eval_mode) request = ant_sandbox_build_eval_request_frame(
    opts.launch.guest_cwd,
    opts.eval_source,
    capabilities,
    tty_rows,
    tty_cols,
    &request_len
  );
  else {
    int script_argc = argc - opts.script_index - 1;
    char **script_argv = argv + opts.script_index + 1;
    request = ant_sandbox_build_run_request_frame(
      opts.launch.guest_cwd,
      argv[opts.script_index],
      script_argc,
      script_argv,
      capabilities,
      tty_rows,
      tty_cols,
      forward_ports,
      (uint32_t)opts.launch.forward_count,
      &request_len
    );
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
    .memory_size = ANT_SANDBOX_DEFAULT_MEMORY_SIZE,
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
