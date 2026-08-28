#ifndef VERSION_H
#define VERSION_H

#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>

bool ant_version_print_update_hint(FILE *out);
const char *ant_release_platform_target(void);

int ant_version_print(void);
int ant_version(void *argtable[]);
int ant_upgrade(int argc, char **argv);

int ant_manifest_fetch(
  char **body_out, size_t *body_len_out,
  char *err, size_t err_len
);

int ant_http_download_file(
  const char *url, FILE *file, const char *label,
  char *err, size_t err_len
);

#endif
