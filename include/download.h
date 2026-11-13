#ifndef ANT_DOWNLOAD_H
#define ANT_DOWNLOAD_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <yyjson.h>
#include "progress.h"

#define ANT_UNPUBLISHED_BUILD_HINT \
  "this build is no longer published; run 'ant upgrade --canary' or 'ant upgrade --stable'"

const char *ant_manifest_url(void);
const char *ant_release_mirror_url(void);
const char *ant_manifest_string(yyjson_val *obj, const char *key);

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

typedef struct {
  const char *label;
  const char *path;
  const char *url;
  uint64_t expected_size;
  uint64_t expected_gzip_size;
  bool gzip;
} ant_download_file_t;

uint64_t ant_manifest_uint(yyjson_val *obj, const char *key);
bool ant_manifest_bool(yyjson_val *obj, const char *key);
void ant_cache_prune_revisions(const char *kind, const char *keep_dirname);

int ant_remove_tree(const char *path);
int ant_manifest_channel_url(const char *channel, char *out, size_t out_len);
int ant_manifest_revision_url(const char *revision, char *out, size_t out_len);
int ant_http_download_file(const char *url, FILE *file, const char *label, char *err, size_t err_len);
int ant_download_files(const ant_download_file_t *files, size_t count, char *err, size_t err_len);
int ant_manifest_fetch(const char *revision, char **body_out, size_t *body_len_out, char *err, size_t err_len);

#endif
