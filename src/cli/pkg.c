#include <compat.h> // IWYU pragma: keep

#include <pkg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <argtable3.h>
#include <yyjson.h>

#include "cli/pkg.h"
#include "cli/version.h"

#include "utils.h"
#include "config.h"
#include "progress.h"
#include "modules/io.h"

bool pkg_verbose = false;

static void progress_callback(void *user_data, pkg_phase_t phase, uint32_t current, uint32_t total, const char *message) {
  progress_t *progress = (progress_t *)user_data;
  if (!progress || !message || !message[0]) return;
  
  const char *icon;
  switch (phase) {
    case PKG_PHASE_RESOLVING:   icon = "🔍"; break;
    case PKG_PHASE_FETCHING:    icon = "🚚"; break;
    case PKG_PHASE_EXTRACTING:  icon = "📦"; break;
    case PKG_PHASE_LINKING:     icon = "🔗"; break;
    case PKG_PHASE_CACHING:     icon = "💾"; break;
    case PKG_PHASE_POSTINSTALL: icon = "⚙️ "; break;
    default:                    icon = "📦"; break;
  }
  
  char msg[PROGRESS_MSG_SIZE];
  if (total > 0) snprintf(msg, sizeof(msg), "%s %s [%u/%u]", icon, message, current, total);
  else if (current > 0) snprintf(msg, sizeof(msg), "%s %s [%u]", icon, message, current);
  else snprintf(msg, sizeof(msg), "%s %s", icon, message);
  
  progress_update(progress, msg);
}

static void print_added_packages(pkg_context_t *ctx) {
  uint32_t count = pkg_get_added_count(ctx);
  uint32_t printed = 0;
  if (count > 0) printf("\n");
  
  for (uint32_t i = 0; i < count; i++) {
    pkg_added_package_t pkg;
    if (pkg_get_added_package(ctx, i, &pkg) == PKG_OK && pkg.direct) {
      printf("%s+%s %s%s%s@%s%s%s\n", 
        C_GREEN, C_RESET,
        C_BOLD, pkg.name, C_RESET,
        C_DIM, pkg.version, C_RESET
      ); printed++;
    }
  }
  
  if (printed > 0) printf("\n");
}

static uint64_t timespec_diff_ms(struct timespec *start, struct timespec *end) {
  int64_t sec = end->tv_sec - start->tv_sec;
  int64_t nsec = end->tv_nsec - start->tv_nsec;
  if (nsec < 0) { sec--; nsec += 1000000000; }
  return (uint64_t)sec * 1000 + (uint64_t)nsec / 1000000;
}

static void print_elapsed(uint64_t elapsed_ms) {
  fputs(C_BOLD, stdout);
  if (elapsed_ms < 1000) {
    printf("%llums", (unsigned long long)elapsed_ms);
  } else printf("%.2fs", (double)elapsed_ms / 1000.0);
  fputs(C_RESET, stdout);
}

static void print_install_header(const char *cmd) {
  const char *version = ant_semver();
  
  printf("%sant %s%s v%s %s(%s)%s\n", 
    C_BOLD, cmd, C_RESET, version, 
    C_DIM, ANT_GIT_HASH, C_RESET
  );
}

static void print_bin_callback(const char *name, void *user_data) {
  (void)user_data;
  printf(" %s-%s %s\n", C_DIM, C_RESET, name);
}

static void prompt_with_default(const char *prompt, const char *def, char *buf, size_t buf_size) {
  if (def && def[0]) {
    printf("%s%s%s %s(%s)%s: ", C_CYAN, prompt, C_RESET, C_DIM, def, C_RESET);
  } else printf("%s%s%s: ", C_CYAN, prompt, C_RESET);
  fflush(stdout);
  
  if (fgets(buf, (int)buf_size, stdin)) {
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
  }
  
  if (buf[0] == '\0' && def) {
    strncpy(buf, def, buf_size - 1);
    buf[buf_size - 1] = '\0';
  }
}

typedef struct {
  const char *target;
  int count;
} why_ctx_t;

static void print_why_callback(const char *name, const char *version, const char *constraint, pkg_dep_type_t dep_type, void *user_data) {
  why_ctx_t *ctx = (why_ctx_t *)user_data;
  
  if (strcmp(name, "package.json") == 0) {
    const char *type_str = dep_type.dev ? "devDependencies" : "dependencies";
    printf("  %s└%s %s%s%s %s(%s)%s\n",
      C_DIM, C_RESET,
      C_GREEN, name, C_RESET,
      C_DIM, type_str, C_RESET);
  } else {
    const char *type_str = dep_type.peer ? "peer" : (dep_type.dev ? "dev" : (dep_type.optional ? "optional" : ""));
    if (type_str[0]) {
      printf("  %s└%s %s %s%s%s@%s%s%s %s\"%s\"%s\n",
        C_DIM, C_RESET,
        type_str,
        C_BOLD, name, C_RESET,
        C_DIM, version, C_RESET,
        C_CYAN, constraint, C_RESET);
    } else {
      printf("  %s└%s %s%s%s@%s%s%s %s\"%s\"%s\n",
        C_DIM, C_RESET,
        C_BOLD, name, C_RESET,
        C_DIM, version, C_RESET,
        C_CYAN, constraint, C_RESET);
    }
  }
  
  ctx->count++;
}

static void print_script(const char *name, const char *command, void *ud) {
  (void)ud;
  if (strlen(command) > 50) {
    printf("  %-15s %.47s...\n", name, command);
  } else {
    printf("  %-15s %s\n", name, command);
  }
}

static void print_bin_name(const char *name, void *ud) {
  (void)ud;
  printf("  %s\n", name);
}

bool pkg_script_exists(const char *package_json_path, const char *script_name) {
  char script_cmd[4096];
  return pkg_get_script(package_json_path, script_name, script_cmd, sizeof(script_cmd)) >= 0;
}

static const char *get_global_dir(void) {
  static char global_dir[4096] = {0};
  if (global_dir[0] == '\0') {
    const char *home = getenv("HOME");
    if (home) snprintf(global_dir, sizeof(global_dir), "%s/.ant/pkg/global", home);
  }
  return global_dir;
}

static int cmd_add_global(const char *const *package_specs, int count) {
  print_install_header("add -g");
  
  char resolve_msg[64];
  snprintf(resolve_msg, sizeof(resolve_msg), "🔍 Resolving [%d/%d]", count, count);
  
  progress_t progress;
  if (!pkg_verbose) progress_start(&progress, resolve_msg);
  
  pkg_options_t opts = { 
    .progress_callback = pkg_verbose ? NULL : progress_callback,
    .user_data = pkg_verbose ? NULL : &progress,
    .verbose = pkg_verbose 
  };
  pkg_context_t *ctx = pkg_init(&opts);
  if (!ctx) {
    if (!pkg_verbose) progress_stop(&progress);
    fprintf(stderr, "Error: Failed to initialize package manager\n");
    return EXIT_FAILURE;
  }

  pkg_error_t err = pkg_add_global_many(ctx, package_specs, (uint32_t)count);
  if (!pkg_verbose) progress_stop(&progress);
  
  if (err != PKG_OK) {
    fprintf(stderr, "Error: %s\n", pkg_error_string(ctx));
    pkg_free(ctx);
    return EXIT_FAILURE;
  }

  pkg_install_result_t result;
  if (pkg_get_install_result(ctx, &result) == PKG_OK) {
    for (int i = 0; i < count; i++) {
      printf("\n%sinstalled globally%s %s%s%s\n", 
             C_GREEN, C_RESET, C_BOLD, package_specs[i], C_RESET);
    }
    printf("  %s(binaries linked to ~/.ant/bin)%s\n", C_DIM, C_RESET);
    printf("\n%s[%s", C_DIM, C_RESET);
    print_elapsed(result.elapsed_ms);
    printf("%s]%s done\n", C_DIM, C_RESET);
  }

  pkg_free(ctx);
  return EXIT_SUCCESS;
}

static int cmd_remove_global(const char *package_name) {
  print_install_header("remove -g");
  
  progress_t progress;
  if (!pkg_verbose) progress_start(&progress, "🔍 Resolving");
  
  pkg_options_t opts = { 
    .progress_callback = pkg_verbose ? NULL : progress_callback,
    .user_data = pkg_verbose ? NULL : &progress,
    .verbose = pkg_verbose 
  };
  pkg_context_t *ctx = pkg_init(&opts);
  if (!ctx) {
    if (!pkg_verbose) progress_stop(&progress);
    fprintf(stderr, "Error: Failed to initialize package manager\n");
    return EXIT_FAILURE;
  }

  pkg_error_t err = pkg_remove_global(ctx, package_name);
  if (!pkg_verbose) progress_stop(&progress);
  
  if (err == PKG_NOT_FOUND) {
    printf("\nPackage '%s' not found in global dependencies\n", package_name);
    pkg_free(ctx);
    return EXIT_SUCCESS;
  }
  
  if (err != PKG_OK) {
    fprintf(stderr, "Error: %s\n", pkg_error_string(ctx));
    pkg_free(ctx);
    return EXIT_FAILURE;
  }

  printf("\n%s-%s Removed globally: %s%s%s\n", C_RED, C_RESET, C_BOLD, package_name, C_RESET);

  pkg_free(ctx);
  return EXIT_SUCCESS;
}

static int cmd_install(void) {
  print_install_header("install");
  
  progress_t progress;
  
  if (!pkg_verbose) {
    progress_start(&progress, "🔍 Resolving [1/1]");
  }
  
  pkg_options_t opts = { 
    .progress_callback = pkg_verbose ? NULL : progress_callback,
    .user_data = pkg_verbose ? NULL : &progress,
    .verbose = pkg_verbose 
  };
  pkg_context_t *ctx = pkg_init(&opts);
  if (!ctx) {
    fprintf(stderr, "Error: Failed to initialize package manager\n");
    return EXIT_FAILURE;
  }

  struct stat st;
  bool needs_resolve = (stat("ant.lockb", &st) != 0);
  
  if (needs_resolve) {
    if (stat("package.json", &st) != 0) {
      if (!pkg_verbose) { progress_stop(&progress);  }
      fprintf(stderr, "Error: No package.json found\n");
      pkg_free(ctx);
      return EXIT_FAILURE;
    }
    
    pkg_error_t err = pkg_resolve_and_install(ctx, "package.json", "ant.lockb", "node_modules");
    if (err != PKG_OK) {
      if (!pkg_verbose) { progress_stop(&progress);  }
      fprintf(stderr, "Error: %s\n", pkg_error_string(ctx));
      pkg_free(ctx);
      return EXIT_FAILURE;
    }
  } else {
    pkg_error_t err = pkg_install(ctx, "package.json", "ant.lockb", "node_modules");
    if (err != PKG_OK) {
      if (!pkg_verbose) { progress_stop(&progress);  }
      fprintf(stderr, "Error: %s\n", pkg_error_string(ctx));
      pkg_free(ctx);
      return EXIT_FAILURE;
    }
  }
  
  if (!pkg_verbose) {
    progress_stop(&progress);
    
  }

  pkg_install_result_t result;
  if (pkg_get_install_result(ctx, &result) == PKG_OK) {
    if (result.packages_installed > 0) {
      print_added_packages(ctx);
      printf("%s%u%s package%s installed", 
        C_GREEN, result.packages_installed, C_RESET,
        result.packages_installed == 1 ? "" : "s");
      if (result.cache_hits > 0) {
        printf(" %s(%u cached)%s", C_DIM, result.cache_hits, C_RESET);
      }
      printf(" %s[%s", C_DIM, C_RESET);
      print_elapsed(result.elapsed_ms);
      printf("%s]%s\n", C_DIM, C_RESET);
    } else {
      printf("\n%sChecked%s %s%u%s installs across %s%u%s packages %s(no changes)%s %s[%s",
        C_DIM, C_RESET,
        C_GREEN, result.packages_installed + result.packages_skipped, C_RESET,
        C_GREEN, result.package_count, C_RESET,
        C_DIM, C_RESET,
        C_DIM, C_RESET);
      print_elapsed(result.elapsed_ms);
      printf("%s]%s\n", C_DIM, C_RESET);
    }
  }

  if (pkg_discover_lifecycle_scripts(ctx, "node_modules") == PKG_OK) {
    uint32_t script_count = pkg_get_lifecycle_script_count(ctx);
    if (script_count > 0) {
      printf("\n%s%u%s package%s need%s to run lifecycle scripts:\n",
        C_YELLOW, script_count, C_RESET,
        script_count == 1 ? "" : "s",
        script_count == 1 ? "s" : "");
      
      for (uint32_t i = 0; i < script_count; i++) {
        pkg_lifecycle_script_t script;
        if (pkg_get_lifecycle_script(ctx, i, &script) == PKG_OK) {
          printf("  %s•%s %s%s%s %s(%s)%s\n", 
            C_DIM, C_RESET,
            C_CYAN, script.name, C_RESET,
            C_DIM, script.script, C_RESET);
        }
      }
      
      printf("\nRun: %sant trust <pkg>%s or %sant trust --all%s\n", C_DIM, C_RESET, C_DIM, C_RESET);
    }
  }

  pkg_free(ctx);
  return EXIT_SUCCESS;
}

static int cmd_add(const char *const *package_specs, int count, bool dev) {
  print_install_header(dev ? "add -D" : "add");
  
  char resolve_msg[64];
  snprintf(resolve_msg, sizeof(resolve_msg), "🔍 Resolving [%d/%d]", count, count);
  
  progress_t progress;
  if (!pkg_verbose) {
    progress_start(&progress, resolve_msg);
  }
  
  pkg_options_t opts = { 
    .progress_callback = pkg_verbose ? NULL : progress_callback,
    .user_data = pkg_verbose ? NULL : &progress,
    .verbose = pkg_verbose 
  };
  pkg_context_t *ctx = pkg_init(&opts);
  if (!ctx) {
    fprintf(stderr, "Error: Failed to initialize package manager\n");
    return EXIT_FAILURE;
  }

  pkg_error_t err = pkg_add_many(ctx, "package.json", package_specs, (uint32_t)count, dev);
  if (err != PKG_OK) {
    if (!pkg_verbose) { progress_stop(&progress);  }
    fprintf(stderr, "Error: %s\n", pkg_error_string(ctx));
    pkg_free(ctx);
    return EXIT_FAILURE;
  }

  err = pkg_resolve_and_install(ctx, "package.json", "ant.lockb", "node_modules");
  if (err != PKG_OK) {
    if (!pkg_verbose) { progress_stop(&progress);  }
    fprintf(stderr, "Error: %s\n", pkg_error_string(ctx));
    pkg_free(ctx);
    return EXIT_FAILURE;
  }
  
  if (!pkg_verbose) progress_stop(&progress);

  pkg_install_result_t result;
  if (pkg_get_install_result(ctx, &result) == PKG_OK) {
    printf("\n");
    
    uint32_t added_count = pkg_get_added_count(ctx);
    for (uint32_t i = 0; i < added_count; i++) {
      pkg_added_package_t pkg;
      if (pkg_get_added_package(ctx, i, &pkg) == PKG_OK && pkg.direct) {
        int bin_count = pkg_list_package_bins("node_modules", pkg.name, NULL, NULL);
        if (bin_count > 0) {
          printf("%sinstalled%s %s%s@%s%s with binaries:\n", 
            C_GREEN, C_RESET,
            C_BOLD, pkg.name, pkg.version, C_RESET);
          pkg_list_package_bins("node_modules", pkg.name, print_bin_callback, NULL);
        } else {
          printf("%sinstalled%s %s%s@%s%s\n", 
            C_GREEN, C_RESET,
            C_BOLD, pkg.name, pkg.version, C_RESET);
        }
      }
    }
    
    printf("\n%s%u%s package%s installed %s[%s",
      C_GREEN, result.packages_installed, C_RESET,
      result.packages_installed == 1 ? "" : "s",
      C_DIM, C_RESET);
    print_elapsed(result.elapsed_ms);
    printf("%s]%s\n", C_DIM, C_RESET);
  }

  pkg_free(ctx);
  return EXIT_SUCCESS;
}

static int cmd_remove(const char *package_name) {
  print_install_header("remove");
  progress_t progress;
  
  if (!pkg_verbose) {
    progress_start(&progress, "🔍 Resolving");
  }
  
  pkg_options_t opts = { 
    .progress_callback = pkg_verbose ? NULL : progress_callback,
    .user_data = pkg_verbose ? NULL : &progress,
    .verbose = pkg_verbose 
  };
  pkg_context_t *ctx = pkg_init(&opts);
  if (!ctx) {
    fprintf(stderr, "Error: Failed to initialize package manager\n");
    return EXIT_FAILURE;
  }

  pkg_error_t err = pkg_remove(ctx, "package.json", package_name);
  if (err != PKG_OK && err != PKG_NOT_FOUND) {
    if (!pkg_verbose) { progress_stop(&progress);  }
    fprintf(stderr, "Error: %s\n", pkg_error_string(ctx));
    pkg_free(ctx);
    return EXIT_FAILURE;
  }
  
  if (err == PKG_NOT_FOUND) {
    if (!pkg_verbose) { progress_stop(&progress);  }
    printf("\n%s[%s", C_DIM, C_RESET);
    printf("%s0ms%s", C_BOLD, C_RESET);
    printf("%s]%s done\n", C_DIM, C_RESET);
    pkg_free(ctx);
    return EXIT_SUCCESS;
  }

  err = pkg_resolve_and_install(ctx, "package.json", "ant.lockb", "node_modules");
  if (err != PKG_OK) {
    if (!pkg_verbose) { progress_stop(&progress);  }
    fprintf(stderr, "Error: %s\n", pkg_error_string(ctx));
    pkg_free(ctx);
    return EXIT_FAILURE;
  }

  if (!pkg_verbose) {
    progress_stop(&progress);
    
  }

  pkg_install_result_t result;
  if (pkg_get_install_result(ctx, &result) == PKG_OK) {
    printf("\n%s%u%s package%s installed %s[%s", 
      C_GREEN, result.packages_installed, C_RESET,
      result.packages_installed == 1 ? "" : "s",
      C_DIM, C_RESET);
    print_elapsed(result.elapsed_ms);
    printf("%s]%s\n", C_DIM, C_RESET);
  }
  
  printf("%s-%s Removed: %s%s%s\n", C_RED, C_RESET, C_BOLD, package_name, C_RESET);
  pkg_free(ctx);

  return EXIT_SUCCESS;
}

static int cmd_trust(const char **pkgs, int count, bool all) {
  print_install_header("trust");
  
  struct timespec start_time;
  clock_gettime(CLOCK_MONOTONIC, &start_time);
  
  pkg_options_t opts = { .verbose = pkg_verbose };
  pkg_context_t *ctx = pkg_init(&opts);
  if (!ctx) {
    fprintf(stderr, "Error: Failed to initialize package manager\n");
    return EXIT_FAILURE;
  }

  if (pkg_discover_lifecycle_scripts(ctx, "node_modules") != PKG_OK) {
    fprintf(stderr, "Error: Failed to scan node_modules\n");
    pkg_free(ctx);
    return EXIT_FAILURE;
  }

  uint32_t script_count = pkg_get_lifecycle_script_count(ctx);
  if (script_count == 0) {
    printf("No packages need lifecycle scripts to run.\n");
    pkg_free(ctx);
    return EXIT_SUCCESS;
  }

  const char **to_run = NULL;
  uint32_t to_run_count = 0;

  if (all) {
    to_run = try_oom(script_count * sizeof(char *));
    if (to_run) {
      for (uint32_t i = 0; i < script_count; i++) {
        pkg_lifecycle_script_t script;
        if (pkg_get_lifecycle_script(ctx, i, &script) == PKG_OK) {
          to_run[to_run_count++] = script.name;
        }
      }
    }
  } else if (count > 0) {
    to_run = try_oom(count * sizeof(char *));
    if (to_run) {
      for (int i = 0; i < count; i++) {
        bool found = false;
        for (uint32_t j = 0; j < script_count; j++) {
          pkg_lifecycle_script_t script;
          if (pkg_get_lifecycle_script(ctx, j, &script) == PKG_OK) {
            if (strcmp(pkgs[i], script.name) == 0) {
              to_run[to_run_count++] = script.name;
              found = true; break;
            }
          }
        }
        if (!found) fprintf(stderr, "Warning: %s has no pending lifecycle script\n", pkgs[i]);
      }
    }
  } else {
    printf("%s%u%s package%s with lifecycle scripts:\n",
      C_YELLOW, script_count, C_RESET,
      script_count == 1 ? "" : "s");
    
    for (uint32_t i = 0; i < script_count; i++) {
      pkg_lifecycle_script_t script;
      if (pkg_get_lifecycle_script(ctx, i, &script) == PKG_OK) {
        printf("  %s•%s %s%s%s %s(%s)%s\n", 
          C_DIM, C_RESET,
          C_CYAN, script.name, C_RESET,
          C_DIM, script.script, C_RESET);
      }
    }
    printf("\nRun: %sant trust <pkg>%s or %sant trust --all%s\n", C_DIM, C_RESET, C_DIM, C_RESET);
    pkg_free(ctx);
    return EXIT_SUCCESS;
  }

  if (to_run && to_run_count > 0) {
    if (pkg_verbose) {
      printf("[trust] adding %u packages to trustedDependencies\n", to_run_count);
      for (uint32_t i = 0; i < to_run_count; i++) {
        printf("[trust]   %s\n", to_run[i]);
      }
    }
    pkg_error_t add_err = pkg_add_trusted_dependencies("package.json", to_run, to_run_count);
    if (add_err == PKG_OK) {
      printf("Added %s%u%s package%s to %strustedDependencies%s in package.json\n",
        C_GREEN, to_run_count, C_RESET,
        to_run_count == 1 ? "" : "s",
        C_BOLD, C_RESET);
    } else {
      if (pkg_verbose) printf("[trust] failed to add trustedDependencies: error %d\n", add_err);
    }
    
    printf("Running lifecycle scripts for %s%u%s package%s...\n",
      C_GREEN, to_run_count, C_RESET,
      to_run_count == 1 ? "" : "s");
    pkg_run_postinstall(ctx, "node_modules", to_run, to_run_count);
    
    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    uint64_t elapsed_ms = timespec_diff_ms(&start_time, &end_time);
    
    printf("\n%s%u%s package%s trusted %s[%s", 
      C_GREEN, to_run_count, C_RESET,
      to_run_count == 1 ? "" : "s",
      C_DIM, C_RESET);
    print_elapsed(elapsed_ms);
    printf("%s]%s\n", C_DIM, C_RESET);
    free((void *)to_run);
  }

  pkg_free(ctx);
  return EXIT_SUCCESS;
}

static int cmd_init(void) {
  FILE *fp = fopen("package.json", "r");
  if (fp) {
    fclose(fp);
    fprintf(stderr, "Error: package.json already exists\n");
    return EXIT_FAILURE;
  }

  char cwd[PATH_MAX];
  const char *default_name = "my-project";
  if (getcwd(cwd, sizeof(cwd))) {
    char *base = strrchr(cwd, '/');
    if (base && base[1]) default_name = base + 1;
  }

  bool interactive = isatty(fileno(stdin));
  
  char name[256] = {0};
  char version[64] = {0};
  char entry[256] = {0};
  
  if (interactive) {
    printf("%sant init%s\n\n", C_BOLD, C_RESET);
    
    prompt_with_default("package name", default_name, name, sizeof(name));
    prompt_with_default("version", "1.0.0", version, sizeof(version));
    prompt_with_default("entry point", "index.js", entry, sizeof(entry));
    
    printf("\n");
  } else {
    strncpy(name, default_name, sizeof(name) - 1);
    strncpy(version, "1.0.0", sizeof(version) - 1);
    strncpy(entry, "index.js", sizeof(entry) - 1);
  }

  fp = fopen("package.json", "w");
  if (!fp) {
    fprintf(stderr, "Error: Could not create package.json\n");
    return EXIT_FAILURE;
  }

  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  
  yyjson_mut_obj_add_str(doc, root, "name", name);
  yyjson_mut_obj_add_str(doc, root, "version", version);
  yyjson_mut_obj_add_str(doc, root, "type", "module");
  yyjson_mut_obj_add_str(doc, root, "main", entry);
  
  yyjson_mut_val *scripts = yyjson_mut_obj_add_obj(doc, root, "scripts");
  char start_cmd[300];
  snprintf(start_cmd, sizeof(start_cmd), "ant %s", entry);
  yyjson_mut_obj_add_str(doc, scripts, "start", start_cmd);
  
  yyjson_mut_obj_add_obj(doc, root, "dependencies");
  yyjson_mut_obj_add_obj(doc, root, "devDependencies");
  
  size_t len; char *json_str = yyjson_mut_write(
    doc, YYJSON_WRITE_PRETTY_TWO_SPACES 
    | YYJSON_WRITE_ESCAPE_UNICODE, &len
  );
  
  if (json_str) {
    fwrite(json_str, 1, len, fp);
    free(json_str);
  }
  
  yyjson_mut_doc_free(doc);
  fclose(fp);
  
  printf("%s+%s Created %spackage.json%s\n", C_GREEN, C_RESET, C_BOLD, C_RESET);
  return EXIT_SUCCESS;
}

static int cmd_why(const char *package_name) {
  struct stat st;
  if (stat("ant.lockb", &st) != 0) {
    fprintf(stderr, "Error: No lockfile found. Run 'ant install' first.\n");
    return EXIT_FAILURE;
  }
  
  pkg_why_info_t info;
  if (pkg_why_info("ant.lockb", package_name, &info) < 0) {
    fprintf(stderr, "Error: Failed to read lockfile\n");
    return EXIT_FAILURE;
  }
  
  if (!info.found) {
    printf("\n%s%s%s is not installed\n\n", C_BOLD, package_name, C_RESET);
    return EXIT_SUCCESS;
  }
  
  const char *type_label = info.is_peer ? " peer" : (info.is_dev ? " dev" : "");
  printf("\n%s%s%s@%s%s%s%s%s%s\n", C_BOLD, package_name, C_RESET, C_DIM, info.target_version, C_RESET, C_YELLOW, type_label, C_RESET);
  
  why_ctx_t ctx = { .target = package_name, .count = 0 };
  int result = pkg_why("ant.lockb", package_name, print_why_callback, &ctx);
  
  if (result < 0) {
    fprintf(stderr, "Error: Failed to read lockfile\n");
    return EXIT_FAILURE;
  }
  
  if (ctx.count == 0) {
    printf("  %s(no dependents)%s\n", C_DIM, C_RESET);
  }
  
  printf("\n");
  return EXIT_SUCCESS;
}

int pkg_cmd_init(int argc, char **argv) {
  struct arg_lit *help = arg_lit0("h", "help", "display help");
  struct arg_end *end = arg_end(5);
  
  void *argtable[] = { help, end };
  int nerrors = arg_parse(argc, argv, argtable);
  
  int exitcode = EXIT_SUCCESS;
  if (help->count > 0) {
    printf("Usage: ant init\n\n");
    printf("Create a new package.json\n");
  } else if (nerrors > 0) {
    arg_print_errors(stdout, end, "ant init");
    exitcode = EXIT_FAILURE;
  } else {
    exitcode = cmd_init();
  }
  
  arg_freetable(argtable, sizeof(argtable)/sizeof(argtable[0]));
  return exitcode;
}

int pkg_cmd_install(int argc, char **argv) {
  struct arg_str *pkgs = arg_strn(NULL, NULL, "<package[@version]>", 0, 100, NULL);
  struct arg_lit *global = arg_lit0("g", "global", "install globally");
  struct arg_lit *dev = arg_lit0("D", "save-dev", "add as devDependency");
  struct arg_lit *help = arg_lit0("h", "help", "display help");
  struct arg_end *end = arg_end(5);
  
  void *argtable[] = { pkgs, global, dev, help, end };
  int nerrors = arg_parse(argc, argv, argtable);
  
  int exitcode = EXIT_SUCCESS;
  if (help->count > 0) {
    printf("Usage: ant install [packages...] [-g] [-D] [--verbose]\n\n");
    printf("Install from lockfile, or add packages if specified.\n");
    printf("\nOptions:\n  -g, --global      Install globally to ~/.ant/pkg/global\n");
    printf("  -D, --save-dev    Add as devDependency\n");
  } else if (nerrors > 0) {
    arg_print_errors(stdout, end, "ant install");
    exitcode = EXIT_FAILURE;
  } else if (pkgs->count == 0) {
    exitcode = cmd_install();
  } else {
    bool is_dev = dev->count > 0;
    exitcode = global->count > 0 
      ? cmd_add_global(pkgs->sval, pkgs->count) 
      : cmd_add(pkgs->sval, pkgs->count, is_dev);
  }
  
  arg_freetable(argtable, sizeof(argtable)/sizeof(argtable[0]));
  return exitcode;
}

int pkg_cmd_add(int argc, char **argv) {
  struct arg_str *pkgs = arg_strn(NULL, NULL, "<package[@version]>", 1, 100, NULL);
  struct arg_lit *global = arg_lit0("g", "global", "install globally");
  struct arg_lit *dev = arg_lit0("D", "save-dev", "add as devDependency");
  struct arg_lit *help = arg_lit0("h", "help", "display help");
  struct arg_end *end = arg_end(5);
  
  void *argtable[] = { pkgs, global, dev, help, end };
  int nerrors = arg_parse(argc, argv, argtable);
  
  int exitcode = EXIT_SUCCESS;
  if (help->count > 0) {
    printf("Usage: ant add <package[@version]>... [options]\n\n");
    printf("Add packages to dependencies.\n");
    printf("\nOptions:\n  -g, --global      Install globally to ~/.ant/pkg/global\n");
    printf("  -D, --save-dev    Add as devDependency\n");
  } else if (nerrors > 0) {
    arg_print_errors(stdout, end, "ant add");
    exitcode = EXIT_FAILURE;
  } else {
    bool is_dev = dev->count > 0;
    exitcode = global->count > 0 
      ? cmd_add_global(pkgs->sval, pkgs->count) 
      : cmd_add(pkgs->sval, pkgs->count, is_dev);
  }
  
  arg_freetable(argtable, sizeof(argtable)/sizeof(argtable[0]));
  return exitcode;
}

int pkg_cmd_remove(int argc, char **argv) {
  struct arg_str *pkgs = arg_strn(NULL, NULL, "<package>", 1, 100, NULL);
  struct arg_lit *global = arg_lit0("g", "global", "remove from global packages");
  struct arg_lit *help = arg_lit0("h", "help", "display help");
  struct arg_end *end = arg_end(5);
  
  void *argtable[] = { pkgs, global, help, end };
  int nerrors = arg_parse(argc, argv, argtable);
  
  int exitcode = EXIT_SUCCESS;
  if (help->count > 0) {
    printf("Usage: ant remove <package>... [-g]\n\n");
    printf("Remove packages from dependencies.\n");
    printf("\nOptions:\n  -g, --global    Remove from global packages\n");
  } else if (nerrors > 0) {
    arg_print_errors(stdout, end, "ant remove");
    exitcode = EXIT_FAILURE;
  } else {
    for (int i = 0; i < pkgs->count && exitcode == EXIT_SUCCESS; i++) {
      exitcode = global->count > 0 ? cmd_remove_global(pkgs->sval[i]) : cmd_remove(pkgs->sval[i]);
    }
  }
  
  arg_freetable(argtable, sizeof(argtable)/sizeof(argtable[0]));
  return exitcode;
}

int pkg_cmd_trust(int argc, char **argv) {
  struct arg_str *pkgs = arg_strn(NULL, NULL, "<package>", 0, 100, NULL);
  struct arg_lit *all = arg_lit0("a", "all", "trust all packages with lifecycle scripts");
  struct arg_lit *help = arg_lit0("h", "help", "display help");
  struct arg_end *end = arg_end(5);
  
  void *argtable[] = { pkgs, all, help, end };
  int nerrors = arg_parse(argc, argv, argtable);
  
  int exitcode = EXIT_SUCCESS;
  if (help->count > 0) {
    printf("Usage: ant trust [packages...] [--all]\n\n");
    printf("Run lifecycle scripts for packages.\n");
    printf("  --all, -a    Trust and run all pending lifecycle scripts\n");
  } else if (nerrors > 0) {
    arg_print_errors(stdout, end, "ant trust");
    exitcode = EXIT_FAILURE;
  } else {
    exitcode = cmd_trust(pkgs->sval, pkgs->count, all->count > 0);
  }
  
  arg_freetable(argtable, sizeof(argtable)/sizeof(argtable[0]));
  return exitcode;
}

int pkg_cmd_run(int argc, char **argv) {
  if (argc < 2) {
    printf("Usage: ant run <script> [-- args...]\n\n");
    printf("Run a script from package.json\n\n");
    printf("Available scripts:\n");
    
    int count = pkg_list_scripts("package.json", NULL, NULL);
    if (count < 0) {
      printf("  (no package.json found)\n");
    } else if (count == 0) {
      printf("  (no scripts defined)\n");
    } else {
      pkg_list_scripts("package.json", print_script, NULL);
    }
    return EXIT_SUCCESS;
  }
  
  const char *script_name = argv[1];
  
  char script_cmd[4096];
  int script_len = pkg_get_script("package.json", script_name, script_cmd, sizeof(script_cmd));
  if (script_len < 0) {
    fprintf(stderr, "Error: script '%s' not found in package.json\n", script_name);
    fprintf(stderr, "Try 'ant run' to list available scripts.\n");
    return EXIT_FAILURE;
  }
  
  char extra_args[4096] = {0};
  int extra_args_len = 0;
  bool found_separator = false;
  
  for (int i = 2; i < argc; i++) {
    if (!found_separator && strcmp(argv[i], "--") == 0) {
      found_separator = true;
      continue;
    }
    if (found_separator) {
      if (extra_args_len > 0) {
        extra_args[extra_args_len++] = ' ';
      }
      size_t arg_len = strlen(argv[i]);
      if ((size_t)extra_args_len + arg_len < sizeof(extra_args) - 1) {
        memcpy(extra_args + extra_args_len, argv[i], arg_len);
        extra_args_len += (int)arg_len;
      }
    }
  }
  extra_args[extra_args_len] = '\0';
  
  printf("%s$%s %s%s%s", C_MAGENTA, C_RESET, C_BOLD, script_cmd, C_RESET);
  if (extra_args_len > 0) {
    printf(" %s", extra_args);
  }
  printf("\n");
  
  pkg_script_result_t result = {0};
  pkg_error_t err = pkg_run_script(
    "package.json", script_name, "node_modules",
    extra_args_len > 0 ? extra_args : NULL,
    &result
  );
  
  if (err != PKG_OK) {
    if (err == PKG_NOT_FOUND) {
      fprintf(stderr, "Error: script '%s' not found\n", script_name);
    } else {
      fprintf(stderr, "Error: failed to run script '%s'\n", script_name);
    }
    return EXIT_FAILURE;
  }
  
  if (result.signal != 0) {
    fprintf(stderr, "Script '%s' killed by signal %d\n", script_name, result.signal);
    return 128 + result.signal;
  }
  
  return result.exit_code;
}

int pkg_cmd_exec(int argc, char **argv) {
  if (argc < 2) {
    printf("Usage: ant x [--ant] <command> [args...]\n\n");
    printf("Run a command from node_modules/.bin or download temporarily\n\n");
    printf("Options:\n");
    printf("  --ant    Run with ant instead of node\n\n");
    printf("Available commands:\n");
    
    int count = pkg_list_bins("node_modules", NULL, NULL);
    if (count < 0) {
      printf("  (no binaries found - run 'ant install' first)\n");
    } else if (count == 0) {
      printf("  (no binaries installed)\n");
    } else {
      pkg_list_bins("node_modules", print_bin_name, NULL);
    }
    return EXIT_SUCCESS;
  }
  
  bool use_ant = false;
  int cmd_idx = 1;
  
  if (strcmp(argv[1], "--ant") == 0) {
    use_ant = true;
    cmd_idx = 2;
    if (argc < 3) {
      fprintf(stderr, "Error: missing command after --ant\n");
      return EXIT_FAILURE;
    }
  }
  
  const char *cmd_name = argv[cmd_idx];
  char bin_path[4096];
  
  int path_len = pkg_get_bin_path("node_modules", cmd_name, bin_path, sizeof(bin_path));
  
  if (path_len < 0) {
    const char *global_dir = get_global_dir();
    if (global_dir[0]) {
      char global_nm[4096];
      snprintf(global_nm, sizeof(global_nm), "%s/node_modules", global_dir);
      path_len = pkg_get_bin_path(global_nm, cmd_name, bin_path, sizeof(bin_path));
    }
  }
  
  if (path_len < 0) {
    progress_t progress;
    
    if (!pkg_verbose) {
      char msg[256];
      snprintf(msg, sizeof(msg), "🔍 Resolving %s", cmd_name);
      progress_start(&progress, msg);
    }
    
    pkg_options_t opts = { 
      .progress_callback = pkg_verbose ? NULL : progress_callback,
      .user_data = pkg_verbose ? NULL : &progress,
      .verbose = pkg_verbose 
    };
    pkg_context_t *ctx = pkg_init(&opts);
    if (!ctx) {
      if (!pkg_verbose) progress_stop(&progress);
      fprintf(stderr, "Error: Failed to initialize package manager\n");
      return EXIT_FAILURE;
    }
    
    pkg_error_t err = pkg_exec_temp(ctx, cmd_name, bin_path, sizeof(bin_path));
    if (!pkg_verbose) progress_stop(&progress);
    
    if (err != PKG_OK) {
      const char *err_msg = pkg_error_string(ctx);
      if (err_msg && err_msg[0]) {
        fprintf(stderr, "Error: %s\n", err_msg);
      } else {
        fprintf(stderr, "Error: '%s' not found\n", cmd_name);
      }
      pkg_free(ctx);
      return EXIT_FAILURE;
    }
    pkg_free(ctx);
  }
  
  const char *runtime = use_ant ? "ant" : "node";
  int arg_offset = cmd_idx + 1;
  int new_argc = argc - arg_offset + 2;
  
  char **exec_argv = try_oom(sizeof(char*) * (new_argc + 1));
  if (!exec_argv) {
    fprintf(stderr, "Error: out of memory\n");
    return EXIT_FAILURE;
  }
  
  exec_argv[0] = (char *)runtime;
  exec_argv[1] = bin_path;
  for (int i = arg_offset; i < argc; i++) {
    exec_argv[i - arg_offset + 2] = argv[i];
  }
  exec_argv[new_argc] = NULL;
  
  execvp(runtime, exec_argv);
  
  free(exec_argv);
  fprintf(stderr, "Error: failed to execute '%s %s' - is %s installed?\n", runtime, bin_path, runtime);
  return EXIT_FAILURE;
}

int pkg_cmd_why(int argc, char **argv) {
  struct arg_str *pkg = arg_str1(NULL, NULL, "<package>", "package name to query");
  struct arg_lit *help = arg_lit0("h", "help", "display help");
  struct arg_end *end = arg_end(5);
  
  void *argtable[] = { pkg, help, end };
  int nerrors = arg_parse(argc, argv, argtable);
  
  int exitcode = EXIT_SUCCESS;
  if (help->count > 0) {
    printf("Usage: ant why <package>\n\n");
    printf("Show which packages depend on the given package.\n");
  } else if (nerrors > 0) {
    arg_print_errors(stdout, end, "ant why");
    exitcode = EXIT_FAILURE;
  } else {
    exitcode = cmd_why(pkg->sval[0]);
  }
  
  arg_freetable(argtable, sizeof(argtable)/sizeof(argtable[0]));
  return exitcode;
}

static const char *format_size(uint64_t bytes, char *buf, size_t buf_size) {
  if (bytes >= 1024ULL * 1024 * 1024) snprintf(buf, buf_size, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
  else if (bytes >= 1024 * 1024) snprintf(buf, buf_size, "%.2f MB", (double)bytes / (1024.0 * 1024.0));
  else if (bytes >= 1024) snprintf(buf, buf_size, "%.2f KB", (double)bytes / 1024.0);
  else snprintf(buf, buf_size, "%llu B", (unsigned long long)bytes);
  return buf;
}

static int cmd_info(const char *package_spec) {
  pkg_options_t opts = { .verbose = false };
  pkg_context_t *ctx = pkg_init(&opts);
  if (!ctx) {
    fprintf(stderr, "Error: Failed to initialize package manager\n");
    return EXIT_FAILURE;
  }

  pkg_info_t info;
  pkg_error_t err = pkg_info(ctx, package_spec, &info);
  if (err != PKG_OK) {
    fprintf(stderr, "Error: %s\n", pkg_error_string(ctx));
    pkg_free(ctx);
    return EXIT_FAILURE;
  }

  char size_buf[32];
  
  printf("%s%s%s%s@%s%s%s%s%s", C_BLUE, C_UL, info.name, C_UL_OFF, C_BLUE, C_BOLD, C_UL, info.version, C_RESET);
  if (info.license[0]) printf(" | %s%s%s", C_CYAN, info.license, C_RESET);
  printf(" | deps: %u | versions: %u\n", info.dep_count, info.version_count);
  
  if (info.description[0]) printf("%s\n", info.description);
  if (info.homepage[0]) printf("%s%s%s\n", C_BLUE, info.homepage, C_RESET);
  if (info.keywords[0]) printf("keywords: %s\n", info.keywords);
  
  uint32_t dep_count = pkg_info_dependency_count(ctx);
  if (dep_count > 0) {
    printf("\n%sdependencies%s (%u):\n", C_BOLD, C_RESET, dep_count);
    for (uint32_t i = 0; i < dep_count; i++) {
      pkg_dependency_t dep;
      if (pkg_info_get_dependency(ctx, i, &dep) == PKG_OK) {
        printf("- %s%s%s: %s\n", C_CYAN, dep.name, C_RESET, dep.version);
      }
    }
  }
  
  printf("\n%sdist%s\n", C_BOLD, C_RESET);
  if (info.tarball[0]) printf(" %s.tarball:%s %s\n", C_DIM, C_RESET, info.tarball);
  if (info.shasum[0]) printf(" %s.shasum:%s %s%s%s\n", C_DIM, C_RESET, C_GREEN, info.shasum, C_RESET);
  if (info.integrity[0]) printf(" %s.integrity:%s %s%s%s\n", C_DIM, C_RESET, C_GREEN, info.integrity, C_RESET);
  if (info.unpacked_size > 0) printf(" %s.unpackedSize:%s %s%s%s\n", C_DIM, C_RESET, C_BLUE, format_size(info.unpacked_size, size_buf, sizeof(size_buf)), C_RESET);
  
  uint32_t tag_count = pkg_info_dist_tag_count(ctx);
  if (tag_count > 0) {
    printf("\n%sdist-tags:%s\n", C_BOLD, C_RESET);
    for (uint32_t i = 0; i < tag_count; i++) {
      pkg_dist_tag_t tag;
      if (pkg_info_get_dist_tag(ctx, i, &tag) == PKG_OK) {
        const char *tag_color = C_MAGENTA;
        if (strcmp(tag.tag, "beta") == 0) tag_color = C_BLUE;
        else if (strcmp(tag.tag, "latest") == 0) tag_color = C_CYAN;
        printf("%s%s%s: %s\n", tag_color, tag.tag, C_RESET, tag.version);
      }
    }
  }
  
  uint32_t maint_count = pkg_info_maintainer_count(ctx);
  if (maint_count > 0) {
    printf("\n%smaintainers:%s\n", C_BOLD, C_RESET);
    for (uint32_t i = 0; i < maint_count; i++) {
      pkg_maintainer_t maint;
      if (pkg_info_get_maintainer(ctx, i, &maint) == PKG_OK) {
        printf("- %s", maint.name);
        if (maint.email[0]) printf(" <%s>", maint.email);
        printf("\n");
      }
    }
  }
  
  // Published date
  if (info.published[0]) printf("\n%sPublished:%s %s\n", C_BOLD, C_RESET, info.published);
  
  pkg_free(ctx);
  return EXIT_SUCCESS;
}

int pkg_cmd_info(int argc, char **argv) {
  struct arg_str *pkg = arg_str1(NULL, NULL, "<package[@version]>", "package to look up");
  struct arg_lit *help = arg_lit0("h", "help", "display help");
  struct arg_end *end = arg_end(5);
  
  void *argtable[] = { pkg, help, end };
  int nerrors = arg_parse(argc, argv, argtable);
  
  int exitcode = EXIT_SUCCESS;
  if (help->count > 0) {
    printf("Usage: ant info <package[@version]>\n\n");
    printf("Show package information from the npm registry.\n");
  } else if (nerrors > 0) {
    arg_print_errors(stdout, end, "ant info");
    exitcode = EXIT_FAILURE;
  } else {
    exitcode = cmd_info(pkg->sval[0]);
  }
  
  arg_freetable(argtable, sizeof(argtable)/sizeof(argtable[0]));
  return exitcode;
}

typedef struct {
  int count;
  bool show_path;
  const char *nm_path;
} ls_ctx_t;

static void print_ls_package(const char *name, void *user_data) {
  ls_ctx_t *ctx = (ls_ctx_t *)user_data;
  
  char pkg_json_path[4096];
  snprintf(pkg_json_path, sizeof(pkg_json_path), "%s/%s/package.json", ctx->nm_path, name);
  
  FILE *f = fopen(pkg_json_path, "r");
  if (!f) {
    printf("  %s%s%s\n", C_BOLD, name, C_RESET);
    ctx->count++;
    return;
  }
  
  char buf[8192];
  size_t len = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[len] = '\0';
  
  const char *version = "?";
  char version_buf[64] = {0};
  
  char *ver_key = strstr(buf, "\"version\"");
  if (ver_key) {
    char *colon = strchr(ver_key, ':');
    if (colon) {
      char *quote1 = strchr(colon, '"');
      if (quote1) {
        char *quote2 = strchr(quote1 + 1, '"');
        if (quote2) {
          size_t vlen = (size_t)(quote2 - quote1 - 1);
          if (vlen < sizeof(version_buf)) {
            memcpy(version_buf, quote1 + 1, vlen);
            version_buf[vlen] = '\0';
            version = version_buf;
          }
        }
      }
    }
  }
  
  printf("  %s%s%s@%s%s%s\n", C_BOLD, name, C_RESET, C_DIM, version, C_RESET);
  ctx->count++;
}

typedef struct {
  int count;
  int total;
} pkg_ls_ctx_t;

static void print_pkg_cb(const char *name, const char *version, void *user_data) {
  pkg_ls_ctx_t *ctx = (pkg_ls_ctx_t *)user_data;
  ctx->count++;
  const char *prefix = (ctx->count == ctx->total) ? "└──" : "├──";
  printf("%s%s%s %s%s%s@%s\n", C_DIM, prefix, C_RESET, C_BOLD, name, C_RESET, version);
}

static int cmd_ls(bool is_global) {
  pkg_options_t opts = { .verbose = pkg_verbose };
  pkg_context_t *ctx = pkg_init(&opts);
  if (!ctx) {
    fprintf(stderr, "Error: Failed to initialize package manager\n");
    return EXIT_FAILURE;
  }
  
  char path_buf[PATH_MAX];
  const char *base_path;
  const char *nm_path;
  char nm_full_path[PATH_MAX];
  
  if (is_global) {
    base_path = get_global_dir();
    snprintf(nm_full_path, sizeof(nm_full_path), "%s/node_modules", base_path);
    nm_path = nm_full_path;
  } else {
    if (!getcwd(path_buf, sizeof(path_buf))) {
      fprintf(stderr, "Error: Could not get current directory\n");
      pkg_free(ctx);
      return EXIT_FAILURE;
    }
    base_path = path_buf;
    nm_path = "node_modules";
  }
  
  uint32_t direct = is_global ? pkg_count_global(ctx) : pkg_count_local(ctx);
  uint32_t total = pkg_count_installed(nm_path);
  
  printf("%s%s/node_modules%s", C_DIM, base_path, C_RESET);
  
  if (direct == 0) {
    printf("\n  (no package.json)\n");
    pkg_free(ctx);
    return EXIT_SUCCESS;
  }
  
  if (total == 0) {
    printf("\n  (empty)\n");
    pkg_free(ctx);
    return EXIT_SUCCESS;
  }
  
  printf(" %s(%u)%s\n", C_DIM, total, C_RESET);
  
  pkg_ls_ctx_t ls_ctx = { .count = 0, .total = (int)direct };
  if (is_global) pkg_list_global(ctx, print_pkg_cb, &ls_ctx);
  else pkg_list_local(ctx, print_pkg_cb, &ls_ctx);
  
  pkg_free(ctx);
  return EXIT_SUCCESS;
}

int pkg_cmd_ls(int argc, char **argv) {
  struct arg_lit *global = arg_lit0("g", "global", "list global packages");
  struct arg_lit *help = arg_lit0("h", "help", "display help");
  struct arg_end *end = arg_end(5);
  
  void *argtable[] = { global, help, end };
  int nerrors = arg_parse(argc, argv, argtable);
  
  int exitcode = EXIT_SUCCESS;
  if (help->count > 0) {
    printf("Usage: ant ls [-g]\n\n");
    printf("List installed packages.\n");
    printf("\nOptions:\n  -g, --global    List global packages\n");
  } else if (nerrors > 0) {
    arg_print_errors(stdout, end, "ant ls");
    exitcode = EXIT_FAILURE;
  } else exitcode = cmd_ls(global->count > 0);
  
  arg_freetable(argtable, sizeof(argtable)/sizeof(argtable[0]));
  return exitcode;
}

static int cmd_cache_info(void) {
  pkg_options_t opts = { .verbose = pkg_verbose };
  pkg_context_t *ctx = pkg_init(&opts);
  if (!ctx) {
    fprintf(stderr, "Error: Failed to initialize package manager\n");
    return EXIT_FAILURE;
  }
  
  pkg_cache_stats_t stats;
  pkg_error_t err = pkg_cache_stats(ctx, &stats);
  if (err != PKG_OK) {
    fprintf(stderr, "Error: Failed to get cache stats\n");
    pkg_free(ctx);
    return EXIT_FAILURE;
  }
  
  char size_buf[64], db_buf[64];
  printf("%sCache location:%s ~/.ant/pkg\n", C_BOLD, C_RESET);
  printf("%sPackages:%s      %u\n", C_BOLD, C_RESET, stats.package_count);
  printf("%sSize:%s          %s\n", C_BOLD, C_RESET, format_size(stats.total_size, size_buf, sizeof(size_buf)));
  printf("%sDB size:%s       %s\n", C_BOLD, C_RESET, format_size(stats.db_size, db_buf, sizeof(db_buf)));
  
  pkg_free(ctx);
  return EXIT_SUCCESS;
}

static int cmd_cache_prune(uint32_t max_age_days) {
  pkg_options_t opts = { .verbose = pkg_verbose };
  pkg_context_t *ctx = pkg_init(&opts);
  if (!ctx) {
    fprintf(stderr, "Error: Failed to initialize package manager\n");
    return EXIT_FAILURE;
  }
  
  int32_t pruned = pkg_cache_prune(ctx, max_age_days);
  if (pruned < 0) {
    fprintf(stderr, "Error: Failed to prune cache\n");
    pkg_free(ctx);
    return EXIT_FAILURE;
  }
  
  if (pruned == 0) {
    printf("No packages to prune (all packages newer than %u days)\n", max_age_days);
  } else {
    printf("%sPruned%s %d package%s older than %u days\n", 
      C_GREEN, C_RESET, pruned, pruned == 1 ? "" : "s", max_age_days);
  }
  
  pkg_free(ctx);
  return EXIT_SUCCESS;
}

static int cmd_cache_sync(void) {
  pkg_options_t opts = { .verbose = pkg_verbose };
  pkg_context_t *ctx = pkg_init(&opts);
  if (!ctx) {
    fprintf(stderr, "Error: Failed to initialize package manager\n");
    return EXIT_FAILURE;
  }
  
  pkg_cache_sync(ctx);
  printf("%sCache synced%s\n", C_GREEN, C_RESET);
  
  pkg_free(ctx);
  return EXIT_SUCCESS;
}

int pkg_cmd_cache(int argc, char **argv) {
  if (argc < 2) {
    printf("Usage: ant cache <command>\n\n");
    printf("Manage the package cache.\n\n");
    printf("Commands:\n");
    printf("  info           Show cache statistics\n");
    printf("  prune [days]   Remove packages older than N days (default: 30)\n");
    printf("  sync           Sync cache to disk\n");
    return EXIT_SUCCESS;
  }
  
  const char *subcmd = argv[1];
  
  if (strcmp(subcmd, "info") == 0) {
    return cmd_cache_info();
  } else if (strcmp(subcmd, "prune") == 0) {
    uint32_t days = 30;
    if (argc >= 3) {
      days = (uint32_t)atoi(argv[2]);
      if (days == 0) days = 30;
    }
    return cmd_cache_prune(days);
  } else if (strcmp(subcmd, "sync") == 0) {
    return cmd_cache_sync();
  } else {
    fprintf(stderr, "Unknown cache command: %s\n", subcmd);
    fprintf(stderr, "Run 'ant cache' for usage.\n");
    return EXIT_FAILURE;
  }
}

int pkg_cmd_create(int argc, char **argv) {
  if (argc < 2) {
    printf("Usage: ant create <template> [dest] [...flags]\n");
    printf("       ant create <github-org/repo> [dest] [...flags]\n\n");
    printf("Scaffold a new project from a template.\n\n");
    printf("Templates:\n");
    printf("  NPM:    Runs 'ant x create-<template>' with given arguments\n");
    printf("  GitHub: Clones repository contents as template\n\n");
    printf("Environment variables:\n");
    printf("  GITHUB_TOKEN    Supply a token for private repos or higher rate limits\n");
    return EXIT_SUCCESS;
  }

  const char *template = argv[1];
  bool is_github = (strchr(template, '/') != NULL);

  if (is_github) {
    const char *dest = NULL;
    
    for (int i = 2; i < argc; i++) {
      if (argv[i][0] != '-') { dest = argv[i]; break; }
    }

    if (!dest) {
      const char *slash = strrchr(template, '/');
      dest = slash ? slash + 1 : template;
    }

    struct stat st;
    if (stat(dest, &st) == 0) {
      fprintf(stderr, "Error: directory '%s' already exists\n", dest);
      return EXIT_FAILURE;
    }

    char url[1024];
    if (strncmp(template, "https://", 8) == 0 || strncmp(template, "git@", 4) == 0) {
      snprintf(url, sizeof(url), "%s", template);
    } else snprintf(url, sizeof(url), "https://github.com/%s.git", template);

    printf("%s+%s Creating project from %s%s%s...\n", C_GREEN, C_RESET, C_BOLD, template, C_RESET);

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "git clone --depth 1 %s %s", url, dest);
    int ret = system(cmd);
    if (ret != 0) {
      fprintf(stderr, "Error: failed to clone %s\n", url);
      return EXIT_FAILURE;
    }

    char git_dir[1024];
    snprintf(git_dir, sizeof(git_dir), "%s/.git", dest);
    
    char rm_cmd[1024];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", git_dir);
    system(rm_cmd);

    if (stat(dest, &st) == 0) {
      char pkg_json[1024];
      snprintf(pkg_json, sizeof(pkg_json), "%s/package.json", dest);
      if (stat(pkg_json, &st) == 0) {
        printf("\n%sDone!%s Created %s%s%s\n", C_GREEN, C_RESET, C_BOLD, dest, C_RESET);
        printf("\n  cd %s\n  ant install\n\n", dest);
      } else printf("\n%sDone!%s Created %s%s%s\n", C_GREEN, C_RESET, C_BOLD, dest, C_RESET);
    }

    return EXIT_SUCCESS;
  }

  char create_pkg[512];
  snprintf(create_pkg, sizeof(create_pkg), "create-%s", template);

  int new_argc = argc;
  char **new_argv = malloc(sizeof(char*) * (new_argc + 1));
  if (!new_argv) {
    fprintf(stderr, "Error: out of memory\n");
    return EXIT_FAILURE;
  }

  new_argv[0] = argv[0];
  new_argv[1] = create_pkg;
  
  for (int i = 2; i < argc; i++) new_argv[i] = argv[i];
  new_argv[new_argc] = NULL;

  int ret = pkg_cmd_exec(new_argc, new_argv);
  free(new_argv);
  
  return ret;
}
