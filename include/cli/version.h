#ifndef VERSION_H
#define VERSION_H

#include <stdbool.h>
#include <stdio.h>

bool ant_version_print_update_hint(FILE *out);
const char *ant_release_platform_target(void);

int ant_version_print(void);
int ant_version(void *argtable[]);
int ant_upgrade(int argc, char **argv);

#endif
