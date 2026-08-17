#ifndef ANT_VFS_BUNDLE_H
#define ANT_VFS_BUNDLE_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define ANT_BUNDLE_MAGIC "ANTBNDL\x01"
#define ANT_BUNDLE_FORMAT_VERSION 1
#define ANT_BUNDLE_ABI_HASH_MAX 48
#define ANT_BUNDLE_KEY_PREFIX "/$ant/"

typedef enum {
  ANT_BUNDLE_OK = 0,
  ANT_BUNDLE_ERR_IO,
  ANT_BUNDLE_ERR_NO_TRAILER,
  ANT_BUNDLE_ERR_CORRUPT,
  ANT_BUNDLE_ERR_ABI,
  ANT_BUNDLE_ERR_OOM,
} ant_bundle_status_t;

typedef struct {
  const char *key;
  uint8_t format;
  uint8_t kind;
  const uint8_t *data;
  uint64_t data_len;
} ant_bundle_module_t;

typedef struct ant_bundle {
  uint8_t *payload;
  size_t payload_size;
  char abi_hash[ANT_BUNDLE_ABI_HASH_MAX];

  ant_bundle_module_t *modules;
  uint32_t module_count;
  uint32_t entry_idx;

  const struct ant_bundle_edge_rec *edges;
  uint32_t edge_count;
  const char *strtab;
  uint64_t strtab_len;
} ant_bundle_t;

typedef struct {
  const char *key;
  uint8_t format;
  uint8_t kind;
  const uint8_t *data;
  size_t data_len;
} ant_bundle_build_module_t;

typedef struct {
  uint32_t parent_idx;
  const char *spec;
  uint32_t child_idx;
  bool is_require;
} ant_bundle_build_edge_t;

typedef struct {
  const char *abi_hash;
  uint32_t entry_idx;
  const ant_bundle_build_module_t *modules;
  uint32_t module_count;
  const ant_bundle_build_edge_t *edges;
  uint32_t edge_count;
} ant_bundle_build_t;

void ant_bundle_close(ant_bundle_t *bundle);
int ant_bundle_write(FILE *f, const ant_bundle_build_t *build);

ant_bundle_status_t ant_bundle_open(
  const char *exe_path,
  const char *expected_abi, ant_bundle_t *out
);

const ant_bundle_module_t *ant_bundle_entry(const ant_bundle_t *bundle);
const ant_bundle_module_t *ant_bundle_get(const ant_bundle_t *bundle, const char *key);

bool ant_bundle_has_key(const ant_bundle_t *bundle, const char *key);
const char *ant_bundle_status_str(ant_bundle_status_t status);

const char *ant_bundle_resolve(
  const ant_bundle_t *bundle, const char *parent_key,
  const char *spec, bool is_require
);

#endif
