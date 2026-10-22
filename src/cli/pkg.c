#include <compat.h> // IWYU pragma: keep

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <yyjson.h>

#include "ant.h"
#include "value.h"
#include "utils.h"
#include "cli/pkg.h"
#include "esm/loader.h"
#include "modules/io.h"

static bool pkg_verbose = false;
static bool pkg_force = false;

static const char PKG_ARGV_LABEL[] = "[ant pkg]";
static const char PKG_ENTRY[] = "ant:internal/pkg";

bool pkg_parse_global_flag(const char *arg) {
  if (strcmp(arg, "-V") == 0 || strcmp(arg, "--verbose") == 0) {
    pkg_verbose = true;
    return true;
  }

  if (strcmp(arg, "--force") == 0) {
    pkg_force = true;
    return true;
  }

  return false;
}

static bool pkg_is_antx(const char *argv0) {
  const char *name = argv0;
  for (const char *p = argv0; *p; p++) if (*p == '/' || *p == '\\') name = p + 1;

  size_t len = strlen(name);
  if (len > 4 && strcasecmp(name + len - 4, ".exe") == 0) len -= 4;

  return len == 4 && strncasecmp(name, "antx", 4) == 0;
}

char **pkg_antx_argv(int *argc, char **argv) {
  if (!pkg_is_antx(argv[0])) return NULL;

  char **out = try_oom(sizeof(char *) * (size_t)(*argc + 2));
  out[0] = argv[0];
  out[1] = "x";

  for (int i = 1; i < *argc; i++) out[i + 1] = argv[i];
  out[*argc + 1] = NULL;
  (*argc)++;
  
  return out;
}

static pkg_argv_t pkg_build_argv(
  const char *argv0, const char *command,
  int argc, char **argv, int tail_argc, char **tail_argv
) {
  int total = 3 + (pkg_verbose ? 1 : 0) + (pkg_force ? 1 : 0) + argc + tail_argc;
  char **out = try_oom(sizeof(char *) * (size_t)(total + 1));

  int at = 0;
  out[at++] = (char *)argv0;
  out[at++] = (char *)PKG_ARGV_LABEL;
  
  if (pkg_verbose) out[at++] = "--verbose";
  if (pkg_force) out[at++] = "--force";
  out[at++] = (char *)command;

  for (int i = 0; i < argc; i++) out[at++] = argv[i];
  for (int i = 0; i < tail_argc; i++) out[at++] = tail_argv[i];
  out[at] = NULL;

  return (pkg_argv_t){ total, out };
}

pkg_argv_t pkg_command_argv(
  const char *argv0, const char *command,
  int argc, char **argv, int tail_argc, char **tail_argv
) {
  return pkg_build_argv(argv0, command, argc, argv, tail_argc, tail_argv);
}

static bool pkg_is_regular_file(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool pkg_script_exists(const char *name) {
  yyjson_doc *doc = yyjson_read_file("package.json", 0, NULL, NULL);
  if (!doc) return false;

  yyjson_val *scripts = yyjson_obj_get(yyjson_doc_get_root(doc), "scripts");
  yyjson_val *script = yyjson_obj_get(scripts, name);
  bool found = yyjson_is_str(script) && yyjson_get_len(script) > 0;

  yyjson_doc_free(doc);
  return found;
}

bool pkg_script_argv(
  pkg_argv_t *out, const char *argv0, const char *name,
  int argc, char **argv, int tail_argc, char **tail_argv
) {
  if (pkg_is_regular_file(name) || !pkg_script_exists(name)) return false;
  *out = pkg_build_argv(argv0, "run", argc, argv, tail_argc, tail_argv);
  return true;
}

void pkg_argv_free(pkg_argv_t *pkg) {
  free(pkg->argv);
  *pkg = (pkg_argv_t){ 0, NULL };
}

int pkg_run(ant_t *js) {
  if (io_no_color) {
    setenv("NO_COLOR", "1", 1);
    unsetenv("FORCE_COLOR");
  }

  ant_value_t ns = js_esm_import_sync_cstr(js, PKG_ENTRY, sizeof(PKG_ENTRY) - 1);
  if (vtype(ns) != kTypeError) return EXIT_SUCCESS;

  fprintf(stderr, "%s\n", js_str(js, ns));
  return EXIT_FAILURE;
}
