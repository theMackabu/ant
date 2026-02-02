#ifndef PKG_CMDS_H
#define PKG_CMDS_H

#include <stdbool.h>

extern bool pkg_verbose;

int pkg_cmd_init(int argc, char **argv);
int pkg_cmd_install(int argc, char **argv);
int pkg_cmd_add(int argc, char **argv);
int pkg_cmd_remove(int argc, char **argv);
int pkg_cmd_trust(int argc, char **argv);
int pkg_cmd_run(int argc, char **argv);
int pkg_cmd_exec(int argc, char **argv);
int pkg_cmd_why(int argc, char **argv);
int pkg_cmd_info(int argc, char **argv);
int pkg_cmd_ls(int argc, char **argv);
int pkg_cmd_cache(int argc, char **argv);

bool pkg_script_exists(const char *package_json_path, const char *script_name);

#endif
