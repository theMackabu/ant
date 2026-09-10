#ifndef VERSION_H
#define VERSION_H

#include <stdbool.h>
#include <stdio.h>

const char *ant_release_platform_target(void);
const char *ant_version_channel(void);

int ant_version_print(void);
int ant_version(void *argtable[]);
int ant_upgrade(int argc, char **argv);

bool ant_version_print_update_hint(FILE *out);

#endif
