#include <compat.h> // IWYU pragma: keep

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <argtable3.h>
#include <crprintf.h>

#include "cage.h"
#include "value.h"
#include "common.h"
#include "utils.h"
#include "messages.h"
#include "silver/vm.h"
#include "modules/io.h"
#include "cli/commands.h"
#include "cli/misc.h"
#include "cli/options.h"
#include "cli/pkg.h"
#include "cli/version.h"

typedef enum {
  OPT_KIND_STR,
  OPT_KIND_LIT,
  OPT_KIND_FILE,
} opt_kind_t;

typedef struct {
  opt_kind_t kind;
  const char *shortopts;
  const char *longopts;
  const char *datatype;
  const char *glossary;
} opt_spec_t;

typedef enum {
  #define CLI_FLAG(id, kind, shortopts, longopts, datatype, glossary) OPT_##id,
  #include "cli/flags.h"
  OPT_FILES,
  OPT_END,
  OPT_COUNT,
} opt_id_t;

static const opt_spec_t opt_specs[OPT_FILES] = {
  #define CLI_FLAG(id, kind, shortopts, longopts, datatype, glossary) [OPT_##id] = { OPT_KIND_##kind, shortopts, longopts, datatype, glossary },
  #include "cli/flags.h"
};

struct ant_cli_args {
  void *table[OPT_COUNT];
};

static void *opt_new(const opt_spec_t *spec) {
  switch (spec->kind) {
    case OPT_KIND_STR: return arg_str0(spec->shortopts, spec->longopts, spec->datatype, spec->glossary);
    case OPT_KIND_LIT: return arg_lit0(spec->shortopts, spec->longopts, spec->glossary);
    case OPT_KIND_FILE: return arg_file0(spec->shortopts, spec->longopts, spec->datatype, spec->glossary);
  }
  return NULL;
}

static ant_cli_args_t *cli_args_new(int max_files) {
  ant_cli_args_t *args = try_oom(sizeof(*args));
  for (int i = 0; i < OPT_FILES; i++) args->table[i] = opt_new(&opt_specs[i]);
  args->table[OPT_FILES] = arg_filen(NULL, NULL, NULL, 0, max_files, NULL);
  args->table[OPT_END] = arg_end(20);
  return args;
}

static bool opt_set(const ant_cli_args_t *args, opt_id_t id) {
  switch (opt_specs[id].kind) {
    case OPT_KIND_STR: return ((struct arg_str *)args->table[id])->count > 0;
    case OPT_KIND_LIT: return ((struct arg_lit *)args->table[id])->count > 0;
    case OPT_KIND_FILE: return ((struct arg_file *)args->table[id])->count > 0;
  }
  return false;
}

static const char *opt_str(const ant_cli_args_t *args, opt_id_t id) {
  struct arg_str *opt = args->table[id];
  return opt->count > 0 ? opt->sval[0] : NULL;
}

static struct arg_file *opt_files(const ant_cli_args_t *args) {
  return args->table[OPT_FILES];
}

static bool is_valued_flag(const char *arg) {
  return
    strcmp(arg, "-e") == 0 ||
    strcmp(arg, "--eval") == 0 ||
    strcmp(arg, "--repl") == 0 ||
    strcmp(arg, "--input-type") == 0 ||
    strcmp(arg, "--location") == 0 ||
    strcmp(arg, "--localstorage-file") == 0 ||
    strcmp(arg, "--cron-title") == 0 ||
    strcmp(arg, "--cron-period") == 0;
}

static bool is_positional(const char *arg) {
  return arg[0] != '-' || arg[1] == '\0';
}

static bool is_print_flag(const char *arg) {
  return strcmp(arg, "-p") == 0 || strcmp(arg, "--print") == 0;
}

static bool is_code_flag(const char *arg) {
  return
    strcmp(arg, "-e") == 0 ||
    strcmp(arg, "--eval") == 0 ||
    strcmp(arg, "--repl") == 0 ||
    is_print_flag(arg);
}

static void parse_inspector_spec(const char *spec, char *host, size_t host_len, int *port) {
  if (!spec || !*spec) return;
  bool all_digits = true;

  for (const char *p = spec; *p; p++) if (!isdigit((unsigned char)*p)) {
    all_digits = false;
    break;
  }

  if (all_digits) {
    int parsed = atoi(spec);
    if (parsed > 0) *port = parsed;
    return;
  }

  const char *colon = strrchr(spec, ':');
  if (colon && colon != spec && colon[1]) {
    size_t len = (size_t)(colon - spec);
    if (len >= host_len) len = host_len - 1;

    memcpy(host, spec, len);
    host[len] = '\0';

    int parsed = atoi(colon + 1);
    if (parsed > 0) *port = parsed;

    return;
  }

  snprintf(host, host_len, "%s", spec);
}

static bool parse_cage_size(const char *value) {
  char *end = NULL;
  unsigned long long gib = strtoull(value, &end, 10);
  unsigned long long max_gib = (NANBOX_DATA_MASK + 1) >> 30;
  if (end && !*end && end != value && gib <= max_gib && ant_cage_set_size((size_t)gib << 30)) return true;

  fprintf(
    stderr, "ant: --cage-size must be a whole number of GiB between %zu and %llu\n",
    ANT_CAGE_MIN_SIZE >> 30, max_gib
  );
  
  return false;
}

static bool parse_global_flag(ant_cli_t *cli, const char *arg, bool *keep) {
  *keep = false;
  if (pkg_parse_global_flag(arg)) return true;

  if (strcmp(arg, "--no-color") == 0) {
    crprintf_set_color(false);
    io_no_color = true;
  }

  else if (strncmp(arg, "--stack-size=", 13) == 0) sv_user_stack_size_kb = atoi(arg + 13);
  else if (strcmp(arg, "--sandbox-daemon") == 0) cli->sandbox_daemon = true;
  else if (strcmp(arg, "--inspect") == 0) cli->inspector.enabled = true;

  else if (strncmp(arg, "--inspect=", 10) == 0) {
    cli->inspector.enabled = true;
    parse_inspector_spec(arg + 10, cli->inspector.host, sizeof(cli->inspector.host), &cli->inspector.port);
  }

  else if (strcmp(arg, "--inspect-wait") == 0) {
    cli->inspector.enabled = true;
    cli->inspector.wait_for_session = true;
  }

  else if (strncmp(arg, "--inspect-wait=", 15) == 0) {
    cli->inspector.enabled = true;
    cli->inspector.wait_for_session = true;
    parse_inspector_spec(arg + 15, cli->inspector.host, sizeof(cli->inspector.host), &cli->inspector.port);
  }

  else if (strncmp(arg, "--cage-size=", 12) == 0) return parse_cage_size(arg + 12);
  else *keep = true;

  return true;
}

static int push_print(char **out, int count, int argc, char **argv, int *i) {
  static char print_flag[] = "-p";
  static char eval_flag[] = "-e";
  const char *arg = argv[*i];

  out[count++] = print_flag;
  if (strncmp(arg, "--print=", 8) == 0) {
    out[count++] = eval_flag;
    out[count++] = argv[*i] + 8;
  } else if (*i + 1 < argc && is_positional(argv[*i + 1]) && strcmp(argv[*i + 1], "-") != 0) {
    out[count++] = eval_flag;
    out[count++] = argv[++*i];
  }
  
  return count;
}

static bool filter_global_flags(ant_cli_t *cli, int argc, char **argv) {
  char **out = cli->filtered_argv = try_oom(sizeof(char *) * (size_t)(2 * argc + 1));
  int count = 0;
  bool parse = true;

  for (int i = 0; i < argc; i++) {
    const char *arg = argv[i];

    if (i == 0 || !parse || is_positional(arg)) {
      if (i > 0) parse = false;
      out[count++] = argv[i];
      continue;
    }

    if (strcmp(arg, "--") == 0) {
      parse = false;
      out[count++] = argv[i];
      continue;
    }

    if (is_print_flag(arg) || strncmp(arg, "--print=", 8) == 0) {
      count = push_print(out, count, argc, argv, &i);
      continue;
    }

    if (is_valued_flag(arg)) {
      out[count++] = argv[i];
      if (i + 1 < argc) out[count++] = argv[++i];
      continue;
    }

    bool keep = false;
    if (!parse_global_flag(cli, arg, &keep)) return false;
    if (keep) out[count++] = argv[i];
  }

  out[count] = NULL;
  cli->argc = count;
  cli->argv = out;
  
  return true;
}

static ant_argv_t split_script_args(int *argc, char **argv) {
  bool has_code = false;

  for (int i = 1; i < *argc; i++) {
    if (strcmp(argv[i], "--") == 0) {
      int script = has_code || i + 1 >= *argc ? 0 : 1;
      ant_argv_t tail = { *argc - i - 1 - script, argv + i + 1 + script };
      *argc = script ? i + 2 : i;
      return tail;
    }

    if (!is_positional(argv[i])) {
      if (is_code_flag(argv[i])) has_code = true;
      if (is_valued_flag(argv[i]) && i + 1 < *argc) i++;
      continue;
    }

    ant_argv_t tail = { *argc - i - 1, argv + i + 1 };
    *argc = i + 1;
    return tail;
  }

  return (ant_argv_t){ 0, NULL };
}

static void print_help(ant_cli_args_t *args) {
  bool colored = crprintf_get_color();
  crprintf_set_color(io_should_color_fd(STDOUT_FILENO));
  if (ant_version_print_update_hint(stdout)) printf("\n");
  crprintf(msg.ant_help_header);

  ant_cli_print_subcommands();
  crprintf(msg.ant_help_flags);

  print_flags_help(stdout, args->table);
  print_flag(stdout, (flag_help_t){ .s = "V", .l = "verbose",  .g = "enable verbose output" });
  print_flag(stdout, (flag_help_t){ .l = "no-color", .g = "disable colored output" });
  print_flag(stdout, (flag_help_t){ .l = "cage-size", .d = "<GiB>", .g = "pointer cage reservation shared by all isolates (default 32)" });
  crprintf_set_color(colored);
}

static bool handle_info_flags(ant_cli_args_t *args, int nerrors, int *exit_code) {
  *exit_code = EXIT_SUCCESS;

  if (opt_set(args, OPT_HELP)) print_help(args);
  else if (opt_set(args, OPT_VERSION_RAW)) fputs(ANT_VERSION "\n", stdout);
  else if (opt_set(args, OPT_VERSION_CHANNEL)) puts(ant_version_channel());
  else if (opt_set(args, OPT_VERSION)) *exit_code = ant_version_print();
  else if (nerrors > 0) {
    print_errors(stdout, args->table[OPT_END]);
    print_help(args);
    *exit_code = EXIT_FAILURE;
  }
  else {
    *exit_code = EXIT_FAILURE;
    return false;
  }

  return true;
}

static bool check_combinations(ant_cli_args_t *args) {
  if (opt_set(args, OPT_NO_CLEAR_SCREEN) && !opt_set(args, OPT_WATCH)) {
    crfprintf(stderr, msg.misuse_clear_screen);
    return false;
  }

  if (opt_set(args, OPT_CRON_TITLE) != opt_set(args, OPT_CRON_PERIOD)) {
    fprintf(stderr, "Error: --cron-title and --cron-period must be used together.\n");
    return false;
  }

  bool single_file = !opt_set(args, OPT_EVAL) && !opt_set(args, OPT_REPL) && opt_files(args)->count == 1;
  if (opt_set(args, OPT_CRON_PERIOD) && !single_file) {
    fprintf(stderr, "Error: cron execution requires exactly one script file.\n");
    return false;
  }

  return true;
}

static void export_args(ant_cli_t *cli, ant_cli_args_t *args) {
  cli->eval = opt_str(args, OPT_EVAL);
  cli->repl = opt_set(args, OPT_REPL);
  cli->repl_script = opt_str(args, OPT_REPL);
  cli->input_type = opt_str(args, OPT_INPUT_TYPE);
  cli->location = opt_str(args, OPT_LOCATION);
  cli->cron_period = opt_str(args, OPT_CRON_PERIOD);
  cli->localstorage_file = args->table[OPT_LOCALSTORAGE_FILE];
  cli->files = opt_files(args)->filename;
  cli->file_count = opt_files(args)->count;
  cli->print = opt_set(args, OPT_PRINT);
  cli->watch = opt_set(args, OPT_WATCH);
  cli->web_compat = opt_set(args, OPT_WEB_COMPAT);
  cli->no_clear_screen = opt_set(args, OPT_NO_CLEAR_SCREEN);
  cli->jitless = opt_set(args, OPT_JITLESS);
}

bool ant_cli_parse(ant_cli_t *cli, int argc, char **argv, int *exit_code) {
  *cli = (ant_cli_t){
    .original_argc = argc,
    .original_argv = argv,
    .inspector = { .host = "127.0.0.1", .port = 9229 },
  };
  *exit_code = EXIT_FAILURE;

  cli->antx_argv = pkg_antx_argv(&argc, argv);
  if (cli->antx_argv) argv = cli->antx_argv;
  if (!filter_global_flags(cli, argc, argv)) return false;

  cli->script_tail = split_script_args(&cli->argc, cli->argv);
  cli->args = cli_args_new(cli->argc);
  int nerrors = arg_parse(cli->argc, cli->argv, cli->args->table);

  if (handle_info_flags(cli->args, nerrors, exit_code)) return false;
  if (!check_combinations(cli->args)) return false;

  export_args(cli, cli->args);
  return true;
}

void ant_cli_free(ant_cli_t *cli) {
  if (cli->args) arg_freetable(cli->args->table, countof(cli->args->table));
  free(cli->args);
  free(cli->filtered_argv);
  free(cli->antx_argv);
  *cli = (ant_cli_t){ 0 };
}
