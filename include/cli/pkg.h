#ifndef PKG_CMDS_H
#define PKG_CMDS_H

#include <stdbool.h>

extern bool pkg_verbose;
extern bool pkg_force;

bool pkg_script_exists(const char *package_json_path, const char *script_name);

#endif
