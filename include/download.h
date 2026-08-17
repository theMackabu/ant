#ifndef ANT_DOWNLOAD_H
#define ANT_DOWNLOAD_H

#include <stdio.h>
#include <stddef.h>
#include "progress.h"

const char *ant_manifest_url(void);

int ant_download_get(
  const char *url,
  FILE *file,
  const char *label,
  progress_t *progress,
  char **body_out,
  size_t *body_len_out,
  char *err,
  size_t err_len
);

int ant_manifest_fetch(char **body_out, size_t *body_len_out, char *err, size_t err_len);
int ant_http_download_file(const char *url, FILE *file, const char *label, char *err, size_t err_len);

int ant_remove_tree(const char *path);
void ant_cache_prune_revisions(const char *kind, const char *keep_dirname);

#endif
