#ifndef ANT_DOWNLOAD_H
#define ANT_DOWNLOAD_H

#include <stdio.h>
#include <stddef.h>
#include "progress.h"

#define ANT_UNPUBLISHED_BUILD_HINT \
  "this build is no longer published; run 'ant upgrade --canary' or 'ant upgrade --stable'"

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

int ant_manifest_fetch(
  const char *revision,
  char **body_out, size_t *body_len_out,
  char *err, size_t err_len
);

int ant_remove_tree(const char *path);
int ant_manifest_channel_url(const char *channel, char *out, size_t out_len);
int ant_manifest_revision_url(const char *revision, char *out, size_t out_len);
int ant_http_download_file(const char *url, FILE *file, const char *label, char *err, size_t err_len);

void ant_cache_prune_revisions(const char *kind, const char *keep_dirname);

#endif
